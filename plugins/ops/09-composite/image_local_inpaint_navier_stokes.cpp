#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "00-foundation/image_program.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "09-composite/inpaint_ns.hpp"
#include "data/result_window_access.hpp"
#include "plugin/builtin_operations.hpp"
#include "plugin/port_validation.hpp"

namespace ps::plugin_internal {
namespace {
using inpaint_ns::check_stop;
using numeric_ops::math_require;
using numeric_ops::math_take;
using Poll = Result<ResultProgramPoll>;
float sample(const ResultTensorReadWindow& input,
             const std::vector<std::uint64_t>& coordinate) {
  const auto run = math_take(input.row_run(coordinate));
  float result;
  std::memcpy(&result, run.data, 4);
  return result;
}
Status profile(const std::vector<OperationMetadata>& inputs) {
  auto valid = image_ops::check_image(inputs[0], image_ops::ImageKind::Tensor);
  if (!valid.ok())
    return valid;
  valid = image_ops::check_image(inputs[1], image_ops::ImageKind::Coverage);
  if (!valid.ok())
    return valid;
  const auto& image = inputs[0].result_schema->tensors[0];
  const auto& mask = inputs[1].result_schema->tensors[0];
  const auto& shape = image.descriptor.shape;
  if (image.descriptor.element_type != ElementType::Float32 || shape[2] != 4 ||
      shape[0] < 3 || shape[1] < 3 || shape[0] > 32768 || shape[1] > 32768 ||
      shape[0] > INT32_MAX / shape[1] || image.batch_axes != mask.batch_axes ||
      mask.descriptor.shape != std::vector<std::uint64_t>{shape[0], shape[1]} ||
      image.facets.size() != 1)
    return Status{ErrorCode::TypeMismatch, "inpaint shape or facet profile"};
  auto semantic = decode_semantic(image.facets[0]);
  if (!semantic.ok())
    return semantic.status();
  auto expected = rgba_semantics();
  expected.reference = semantic.value().reference;
  auto facet = encode_semantic(expected);
  if (!facet.ok() ||
      !input_internal::same_facets(image.facets, {facet.value()}))
    return Status{ErrorCode::TypeMismatch,
                  "inpaint requires ordered linear premultiplied RGBA"};
  return Status::success();
}
void charge(const ResultProgramPhase& phase, unsigned __int128 work) {
  check_stop(phase.query.cancellation);
  if (work > UINT64_MAX)
    throw Status{ErrorCode::ResourceExhausted, "inpaint work bound overflow"};
  math_require(phase.consume_work(static_cast<std::uint64_t>(work)));
}
Result<MutableBuffer> execute(const ResultProgramPhase& phase, bool adapter) {
  using Answer = Result<MutableBuffer>;
  try {
    check_stop(phase.query.cancellation);
    input_internal::Float32Environment environment;
    if (!environment.active())
      throw inpaint_ns::NumericFailure{};
    const auto& spec = phase.query.output.result_schema->tensors[0];
    const auto& shape = spec.descriptor.shape;
    const auto h = shape[0], w = shape[1], n = h * w, p = (h + 2) * (w + 2);
    const auto planes = spec.batch_axes[0] * spec.batch_axes[1];
    auto image = math_take(phase.tensors->at({0, 0}).acquire(
        Region::whole(spec.sample_shape()), phase.query.cancellation));
    const auto& mask_spec = phase.query.inputs[1].result_schema->tensors[0];
    auto hole_mask = math_take(phase.tensors->at({1, 0}).acquire(
        Region::whole(mask_spec.sample_shape()), phase.query.cancellation));
    const auto image_work =
        math_take(execution_internal::ResultWindowAccess::read_work(image));
    const auto mask_work =
        math_take(execution_internal::ResultWindowAccess::read_work(hole_mask));
    // Validation and original-bit copying read eight image and two mask samples
    // per pixel at most. Include coordinates, classification, and byte copies.
    charge(phase, static_cast<unsigned __int128>(planes) * n *
                      (8 * static_cast<unsigned __int128>(image_work) +
                       2 * static_cast<unsigned __int128>(mask_work) + 256));
    ResourceVector<std::uint64_t> holes{
        ResourceAllocator<std::uint64_t>(phase.resources)};
    holes.resize(planes);
    std::vector<std::uint64_t> ic(5), mc(4);
    for (std::uint64_t plane = 0; plane < planes; ++plane) {
      ic[0] = mc[0] = plane / spec.batch_axes[1];
      ic[1] = mc[1] = plane % spec.batch_axes[1];
      for (std::uint64_t i = 0; i < n; ++i) {
        if (!(i & 511U))
          check_stop(phase.query.cancellation);
        ic[2] = mc[2] = i / w;
        ic[3] = mc[3] = i % w;
        const auto mask = sample(hole_mask, mc);
        if (mask != 0 && mask != 1)
          throw inpaint_ns::NumericFailure{};
        holes[plane] += mask != 0;
        for (ic[4] = 0; ic[4] < 4; ++ic[4]) {
          const auto value = sample(image, ic);
          if (!std::isfinite(value) || (ic[4] == 3 && value != 1))
            throw inpaint_ns::NumericFailure{};
        }
      }
    }
    for (const auto count : holes)
      if (count == n)
        return Answer(Status{ErrorCode::OperationFailed,
                             "inpaint mask has no known pixels"});
    const auto count = math_take(spec.sample_count());
    if (count > UINT64_MAX / 4)
      return Answer(
          Status{ErrorCode::ResourceExhausted, "inpaint output size overflow"});
    check_stop(phase.query.cancellation);
    auto output = math_take(phase.resources.allocator().allocate(count * 4));
    for (std::uint64_t plane_index = 0; plane_index < planes; ++plane_index) {
      ic[0] = mc[0] = plane_index / spec.batch_axes[1];
      ic[1] = mc[1] = plane_index % spec.batch_axes[1];
      auto* target = output.data() + plane_index * n * 16;
      for (std::uint64_t i = 0; i < n; ++i) {
        if (!(i & 1023U))
          check_stop(phase.query.cancellation);
        ic[2] = i / w;
        ic[3] = i % w;
        for (ic[4] = 0; ic[4] < 4; ++ic[4]) {
          const auto value = sample(image, ic);
          std::memcpy(target + (i * 4 + ic[4]) * 4, &value, 4);
        }
      }
      if (!holes[plane_index])
        continue;
      const auto radius = static_cast<int>(
          std::get<std::int64_t>(phase.query.parameters.at("radius")));
      std::uint64_t logarithm = 0;
      for (auto size = n; size; size >>= 1)
        ++logarithm;
      // Per channel: P initialization, N band/packing/copy visits, at most N
      // pushes and pops, and K*(2r+1)^2 candidates. 128*log bounds heap element
      // comparisons/moves; 512 bounds each candidate's branches, loads and
      // fixed arithmetic. Prepay before the unchanged sequential math kernel.
      charge(phase,
             3 * (static_cast<unsigned __int128>(p) * 8 +
                  static_cast<unsigned __int128>(n) * (256 + 128 * logarithm) +
                  static_cast<unsigned __int128>(holes[plane_index]) *
                      (2 * radius + 1) * (2 * radius + 1) * 512));
      const auto allocate = [&](std::uint64_t bytes) {
        check_stop(phase.query.cancellation);
        auto memory = math_take(phase.allocator.allocate(bytes));
        check_stop(phase.query.cancellation);
        return memory;
      };
      auto mask_buffer = allocate(n), plane_buffer = allocate(4 * n);
      auto* mask = mask_buffer.data();
      auto* plane = reinterpret_cast<float*>(plane_buffer.data());
      MutableBuffer state, time, heap, second;
      if (adapter) {
        second = allocate(4 * n);
      } else {
        state = allocate(p);
        time = allocate(4 * p);
        heap = allocate(16 * n);
      }
      for (std::uint64_t i = 0; i < n; ++i) {
        if (!(i & 4095U))
          check_stop(phase.query.cancellation);
        mc[2] = i / w;
        mc[3] = i % w;
        mask[i] = sample(hole_mask, mc) == 0 ? 0 : 255;
      }
      for (std::uint64_t c = 0; c < 3; ++c) {
        check_stop(phase.query.cancellation);
        for (std::uint64_t i = 0; i < n; ++i) {
          if (!(i & 4095U))
            check_stop(phase.query.cancellation);
          float value = 0;
          if (!mask[i])
            std::memcpy(&value, target + (i * 4 + c) * 4, 4);
          new (plane + i) float{value};
        }
        std::feclearexcept(FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW);
        float* result = plane;
        if (adapter) {
#ifdef PHOTOSPIDER_HAS_INPAINT_OPENCV
          result = reinterpret_cast<float*>(second.data());
          check_stop(phase.query.cancellation);
          inpaint_ns::opencv(plane, result, mask, static_cast<int>(h),
                             static_cast<int>(w), radius);
#else
          return Answer(
              Status{ErrorCode::BackendUnavailable, "OpenCV adapter disabled"});
#endif
        } else {
          inpaint_ns::native(plane, mask, static_cast<int>(h),
                             static_cast<int>(w), radius, state.data(),
                             reinterpret_cast<float*>(time.data()), heap.data(),
                             phase.query.cancellation);
        }
        check_stop(phase.query.cancellation);
        if (std::fetestexcept(FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW))
          throw inpaint_ns::NumericFailure{};
        for (std::uint64_t i = 0; i < n; ++i) {
          if (!(i & 4095U))
            check_stop(phase.query.cancellation);
          if (!std::isfinite(result[i]))
            throw inpaint_ns::NumericFailure{};
          if (mask[i])
            std::memcpy(target + (i * 4 + c) * 4, result + i, 4);
        }
      }
    }
    check_stop(phase.query.cancellation);
    return Answer(std::move(output));
  } catch (const inpaint_ns::Stopped&) {
    return Answer(Status{ErrorCode::Cancelled, "inpaint cancelled"});
  } catch (const inpaint_ns::NumericFailure&) {
    return Answer(Status{ErrorCode::OperationFailed,
                         "inpaint requires binary coverage, opaque finite RGBA "
                         "and finite arithmetic"});
  } catch (const Status& status) {
    return Answer(status);
  } catch (const std::bad_alloc&) {
    return Answer(
        Status{ErrorCode::ResourceExhausted, "inpaint allocation failed"});
  }
}
ResultBuilder builder(const ResultProgramPhase& phase, bool empty) {
  auto result = math_take(ResultBuilder::start(
      phase.resources, *phase.query.output.result_schema,
      phase.query.semantic_key, {}, {}, phase.query.tile_height,
      phase.query.tile_width, phase.query.resources));
  std::vector<ResultRelation> relations;
  for (std::uint32_t i = 0; i < 2; ++i)
    relations.push_back(math_take(ResultRelation::cartesian(
        phase.resources, 1,
        {i, 8, 0, empty ? 0U : 1U, ResultSupportTarget::Descriptor, 0})));
  math_require(result.bind_descriptor_relation(
      math_take(ResultRelation::unite(phase.resources, relations))));
  return result;
}
Poll empty_result(const ResultProgramPhase& phase) try {
  auto result = builder(phase, true);
  return Poll(ResultPublication{math_take(result.seal()), true});
} catch (const Status& status) {
  return Poll(status);
}
struct Program {
  bool adapter, requested = false;
  explicit Program(bool external) : adapter(external) {}
  Poll poll(const ResultProgramPhase& phase) try {
    auto metadata = math_take(
        phase.resources.reserve(ResourceCapacity::host(16384, 16384)));
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      for (std::uint32_t i = 0; i < 2; ++i)
        need.tensors.push_back(
            {i, 0,
             math_take(Footprint::all(phase.query.inputs[i]
                                          .result_schema->tensors[0]
                                          .sample_shape())),
             13});
      return Poll(std::move(need));
    }
    auto output = math_take(execute(phase, adapter));
    auto result = builder(phase, false);
    const auto& tensor = phase.query.output.result_schema->tensors[0];
    const auto count = math_take(tensor.sample_count());
    std::vector<ResultRelation> relations;
    for (std::uint32_t i = 0; i < 2; ++i)
      relations.push_back(math_take(ResultRelation::cartesian(
          phase.resources, count,
          {i, 5, 0,
           math_take(
               phase.query.inputs[i].result_schema->tensors[0].sample_count()),
           ResultSupportTarget::Tensor, 0},
          DependencyGuarantee::Conservative)));
    auto shape = tensor.sample_shape();
    StridedLayout layout;
    layout.byte_strides.resize(shape.size());
    std::uint64_t stride = 4;
    for (std::size_t axis = shape.size(); axis-- > 0;) {
      if (stride > INT64_MAX)
        throw Status{ErrorCode::ResourceExhausted,
                     "inpaint output stride overflow"};
      layout.byte_strides[axis] = stride;
      stride *= shape[axis];
    }
    math_require(result.publish_tensor(
        0, Region::whole(shape), layout, std::move(output).freeze(),
        math_take(ResultRelation::unite(phase.resources, relations)),
        {true, true, true, true}, phase.query.cancellation));
    return Poll(ResultPublication{math_take(result.seal()), true});
  } catch (const Status& status) {
    return Poll(status);
  }
};
Status add(OperationRegistry* registry, const char* key, bool adapter) {
  OperationDefinition op;
  op.key = key;
  auto& t = op.traits;
  // External OpenCV library replacement is not part of the kernel build ID.
  t.cacheable = !adapter;
  t.input_count = 2;
  OperationPortConstraint image;
  image.kind = OperationPortKind::Result;
  image.result_schema_id = "photospider.image";
  image.result_schema_version = 1;
  t.input_schema = {image, image};
  t.parameter_schema = {
      {"radius", OperationParameterType::Int64, true, true, 1, 32}};
  // Root owns output. Per-plane native scratch is 21*N+5*P < 35*N;
  // twice the authorized image+mask byte count admits 40*N per plane.
  t.workspace_input_multiplier = 2;
  t.workspace_bytes = 65536;
  t.requires_metadata_specialization = true;
  auto& out = t.outputs[0];
  out.key = "image";
  out.output_schema = image;
  out.result_schema = image_ops::image_schema();
  out.region_rule = OperationRegionRule::Whole;
  out.continuation_bytes = sizeof(Program);
  out.maximum_dependency_stages = 2;
  op.prepare_static = [](const auto& inputs,
                         const auto&) -> Result<OperationPreparation> {
    auto valid = profile(inputs);
    if (!valid.ok())
      return Result<OperationPreparation>(valid);
    OperationPreparation prepared;
    OperationOutputSpecialization output;
    auto schema = *inputs[0].result_schema;
    schema.publication = PublishPolicy::CompleteBundle;
    output.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
    prepared.outputs.push_back(std::move(output));
    return Result<OperationPreparation>(std::move(prepared));
  };
  op.start_result = [adapter](const ResultProgramQuery& query,
                              const BufferAllocator& allocator) {
    if (query.tensor_outputs && query.tensor_outputs->empty())
      return ResultContinuation::stateless<empty_result>();
    return ResultContinuation::make<Program>(allocator, adapter);
  };
  return registry->register_operation(std::move(op));
}
}  // namespace
Status register_image_local_inpaint_navier_stokes(OperationRegistry* registry) {
  auto status =
      add(registry, "image.local_inpaint_navier_stokes_native_apple_silicon",
          false);
  if (!status.ok())
    return status;
#ifdef PHOTOSPIDER_HAS_INPAINT_OPENCV
  return add(registry, "image.local_inpaint_navier_stokes_openCV", true);
#else
  return status;
#endif
}
}  // namespace ps::plugin_internal
