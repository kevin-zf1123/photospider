#include "00-foundation/image_native.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include "execution/result_native.hpp"
#include "rgba32f/image_gpu.h"

namespace ps::plugin_internal::image_ops {
namespace {
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw result.status();
  return result.take_value();
}
void check(Status status) {
  if (!status.ok())
    throw status;
}
}  // namespace
Result<MutableBuffer> native_image(const ResultProgramPhase& phase,
                                   std::uint32_t kind, const Region& output,
                                   const std::vector<Region>& sources,
                                   unsigned radius, const double* weights,
                                   unsigned factor) try {
  using Answer = Result<MutableBuffer>;
  if (!phase.gpu || phase.gpu->backend != PS_GPU_BACKEND_METAL_V1)
    return Answer(Status{ErrorCode::BackendUnavailable,
                         "image operation requires Metal"});
  const auto count = phase.query.inputs.size();
  if (!count || count > 8 || sources.size() != count)
    return Answer(Status{ErrorCode::Internal, "invalid native image inputs"});
  const auto& dimensions = output.dimensions();
  const auto& target = phase.query.output.result_schema->tensors[0];
  const auto samples = take(output.element_count());
  unsigned __int128 work = static_cast<unsigned __int128>(samples) * 32;
  for (const auto& source : sources)
    work += static_cast<unsigned __int128>(take(source.element_count())) * 16;
  if (kind == 2)
    work += (static_cast<unsigned __int128>(sources[0].dimensions()[2].extent) *
                 dimensions[3].extent * 4 +
             samples) *
            (2 * radius + 1) * 8;
  if (work > UINT64_MAX)
    return Answer(
        Status{ErrorCode::ResourceExhausted, "native image work overflow"});
  check(phase.consume_work(static_cast<std::uint64_t>(work)));
  std::array<Value, 8> backing;
  std::array<ps_image_buffer_view, 8> views{};
  std::array<std::array<std::uint64_t, 3>, 8> origins{}, extents{}, shapes{};
  std::array<std::array<std::int64_t, 3>, 8> strides{};
  std::array<float, 8> controls{};
  for (unsigned i = 0; i < count; ++i) {
    const auto& tensor = phase.query.inputs[i].result_schema->tensors[0];
    const auto scalar = tensor.batch_axes.empty() &&
                        tensor.descriptor.shape.size() == 1 &&
                        tensor.descriptor.shape[0] == 1;
    auto& view = views[i];
    view.rank = scalar ? 1 : tensor.descriptor.shape.size();
    view.shape = shapes[i].data();
    view.storage_origin = origins[i].data();
    view.byte_strides = strides[i].data();
    view.demand_offsets = origins[i].data();
    view.demand_extents = extents[i].data();
    if (scalar) {
      check(phase.read_tensor(i, 0, {0}, &controls[i], 4));
      view.data = reinterpret_cast<const std::uint8_t*>(&controls[i]);
      view.byte_size = 4;
      shapes[i][0] = extents[i][0] = 1;
      strides[i][0] = 4;
      continue;
    }
    const auto window = take(phase.tensors->at({i, 0}).acquire(
        sources[i], phase.query.cancellation));
    backing[i] = take(execution_internal::ResultNativeScope::input(
        window, phase.consume_work, phase.query.cancellation));
    const auto& value = backing[i];
    auto coordinate_memory =
        take(phase.resources.reserve(ResourceCapacity::host(5 * 8, 5 * 8)));
    std::vector<std::uint64_t> at;
    at.reserve(5);
    for (const auto& dimension : sources[i].dimensions())
      at.push_back(dimension.offset);
    view.byte_offset = take(value.byte_address(at));
    view.data = value.bytes().data();
    view.byte_size = value.bytes().size();
    for (unsigned axis = 0; axis < view.rank; ++axis) {
      origins[i][axis] = sources[i].dimensions()[axis + 2].offset;
      extents[i][axis] = sources[i].dimensions()[axis + 2].extent;
      shapes[i][axis] = tensor.descriptor.shape[axis];
      strides[i][axis] = value.layout().byte_strides[axis + 2];
    }
  }
  struct Output {
    const ResultProgramPhase& phase;
    std::optional<MutableBuffer> output;
    std::array<MutableBuffer, 2> scratch;
    unsigned used = 0;
    std::uint64_t bytes;
    Status failure;
    bool published = false;
  } state{phase, {}, {}, 0, samples * 4, {}, false};
  const std::array<std::uint64_t, 3> offsets{dimensions[2].offset,
                                             dimensions[3].offset, 0};
  const std::array<std::uint64_t, 3> extents_out{dimensions[2].extent,
                                                 dimensions[3].extent, 4};
  ps_image_gpu_output sink{};
  sink.context = &state;
  sink.output_rank = target.descriptor.shape.size();
  sink.output_shape = target.descriptor.shape.data();
  sink.output_offsets = offsets.data();
  sink.output_extents = extents_out.data();
  sink.output_byte_size = state.bytes;
  sink.gpu = phase.gpu;
  sink.allocate_output = [](void* raw) -> std::uint8_t* {
    auto& s = *static_cast<Output*>(raw);
    auto buffer = execution_internal::ResultNativeScope::output(s.bytes);
    if (!buffer.ok()) {
      s.failure = buffer.status();
      return nullptr;
    }
    s.output.emplace(buffer.take_value());
    return s.output->data();
  };
  sink.allocate_scratch = [](void* raw, std::uint64_t bytes) -> std::uint8_t* {
    auto& s = *static_cast<Output*>(raw);
    if (s.used == s.scratch.size()) {
      s.failure = {ErrorCode::Internal, "image scratch count"};
      return nullptr;
    }
    auto buffer = s.phase.allocator.allocate(bytes);
    if (!buffer.ok()) {
      s.failure = buffer.status();
      return nullptr;
    }
    s.scratch[s.used] = buffer.take_value();
    return s.scratch[s.used++].data();
  };
  sink.publish = [](void* raw, const std::uint8_t* bytes, std::uint64_t size) {
    auto& s = *static_cast<Output*>(raw);
    s.published = s.output && bytes == s.output->data() && size == s.bytes;
    return s.published ? 1 : 0;
  };
  const auto cancelled = [](void* raw) {
    return static_cast<const CancellationToken*>(raw)->cancelled() ? 1 : 0;
  };
  const auto result = ps_execute_gpu_image(
      kind, views.data(), count, radius, weights, factor, cancelled,
      const_cast<CancellationToken*>(&phase.query.cancellation), &sink);
  if (!state.failure.ok())
    return Answer(state.failure);
  if (result == 0 && state.published)
    return Answer(std::move(*state.output));
  return Answer(Status{result == 2   ? ErrorCode::Cancelled
                       : result == 3 ? ErrorCode::BackendUnavailable
                       : result == 4 ? ErrorCode::ResourceExhausted
                                     : ErrorCode::OperationFailed,
                       "native image operation failed"});
} catch (const Status& status) {
  return Result<MutableBuffer>(status);
}
}  // namespace ps::plugin_internal::image_ops
