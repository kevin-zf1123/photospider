#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include "common/floating_environment.hpp"
#include "common/result_support.hpp"
#include "photospider/plugin/operation_registry.hpp"
#include "photospider/plugin/result_program.hpp"
#include "photospider/plugin/tensor_helpers.hpp"
#include "photospider/plugin/tile_helpers.hpp"

namespace ps::plugin_internal::numeric_ops {
// A window owns only a Need-authorized rectangle. Logical batch coordinates
// select its physical pieces; affine and paged storage use the same reader.
class MathTensorReader final {
 public:
  explicit MathTensorReader(const ResultTensorInput& input,
                            const CancellationToken& cancellation)
      : input_(input), cancellation_(cancellation) {}
  std::uint64_t bits(const std::vector<std::uint64_t>& at) {
    const auto& spec = input_.spec();
    if (!window_.valid()) {
      window_ = math_take(
          input_.acquire(Region::whole(spec.sample_shape()), cancellation_));
    }
    const auto axis = window_.sample_axis();
    auto& row = rows_[at.back() % rows_.size()];
    bool hit = row.valid && at[axis] >= row.at[axis] &&
               at[axis] - row.at[axis] < row.run.samples;
    for (std::size_t i = 0; hit && i < at.size(); ++i)
      if (i != axis && at[i] != row.at[i])
        hit = false;
    if (!hit) {
      row.run = math_take(window_.row_run(at));
      std::copy(at.begin(), at.end(), row.at.begin());
      row.valid = true;
    }
    const auto offset = static_cast<__int128>(at[axis] - row.at[axis]) *
                        row.run.sample_stride_bytes;
    std::uint64_t word = 0;
    std::memcpy(&word, row.run.data + static_cast<std::ptrdiff_t>(offset),
                TensorSpan::element_size(spec.descriptor.element_type));
    return word;
  }

 private:
  struct Row {
    std::array<std::uint64_t, 8> at{};
    ResultTensorRun run;
    bool valid = false;
  };
  const ResultTensorInput& input_;
  const CancellationToken& cancellation_;
  ResultTensorReadWindow window_;
  std::array<Row, 4> rows_{};
};

class MathTensorWriter final {
 public:
  explicit MathTensorWriter(const ResultTensorWriteWindow& writer)
      : writer_(writer) {}
  uint8_t* address(const std::vector<uint64_t>& at) {
    const auto axis = writer_.sample_axis();
    bool hit = run_.samples && at[axis] >= at_[axis] &&
               at[axis] - at_[axis] < run_.samples;
    for (size_t i = 0; hit && i < at.size(); ++i)
      if (i != axis && at[i] != at_[i])
        hit = false;
    if (!hit) {
      run_ = math_take(writer_.row_run(at));
      std::copy(at.begin(), at.end(), at_.begin());
    }
    return run_.data + static_cast<std::ptrdiff_t>(
                           static_cast<__int128>(at[axis] - at_[axis]) *
                           run_.sample_stride_bytes);
  }

 private:
  const ResultTensorWriteWindow& writer_;
  std::array<uint64_t, 8> at_{};
  ResultTensorMutableRun run_;
};
inline void math_next(std::vector<uint64_t>& at,
                      const std::vector<uint64_t>& shape) {
  for (size_t axis = at.size(); axis-- > 0;) {
    if (++at[axis] < shape[axis])
      break;
    at[axis] = 0;
  }
}
template <class Kernel>
using WholeTensorPublication = decltype(std::declval<Kernel&>().publish(
    std::declval<const ResultProgramPhase&>(), std::declval<ResultBuilder&>()));
template <class Kernel>
WholeTensorPublication<Kernel> publish_whole_tensor(
    Kernel& kernel, const ResultProgramPhase& phase, ResultBuilder& builder,
    int) {
  return kernel.publish(phase, builder);
}
template <class Kernel>
Status publish_whole_tensor(Kernel& kernel, const ResultProgramPhase& phase,
                            ResultBuilder& builder, int64_t) {
  const auto shape =
      phase.query.output.result_schema->tensors[0].sample_shape();
  return sdk::publish_dense_tensor(
      phase, builder, 0, Region::whole(shape),
      [&](const auto& writers) {
        return math_callback(phase,
                             [&] { return kernel.write(phase, writers); });
      },
      phase.query.cancellation);
}
inline bool whole_tensor_port_selected(const ResultProgramQuery& query,
                                       uint32_t port) {
  if (!query.prepared)
    return true;
  if (query.shared_stage != UINT32_MAX) {
    const auto& projection = query.prepared->traits()
                                 .shared_stages.at(query.shared_stage)
                                 .input_indices;
    return std::find(projection.begin(), projection.end(), port) !=
           projection.end();
  }
  const auto& selected =
      query.prepared->traits().outputs.at(query.output_index).input_indices;
  return !selected ||
         std::find(selected->begin(), selected->end(), port) != selected->end();
}
// Shared private Whole protocol. Numerical kernels receive authorized input
// capabilities and one borrowed packed writer; no TensorSpan operation is
// invoked.
template <class Kernel>
struct WholeTensorProgram final {
  Kernel kernel;
  bool started = false;
  uint32_t next_input = 0;
  std::optional<ResultTensorInputs> ready;
  Footprint output;
  template <class... Args>
  explicit WholeTensorProgram(Args&&... args)
      : kernel(std::forward<Args>(args)...) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    FloatingEnvironment environment;
    if (!environment.active())
      return Result<ResultProgramPoll>(Status{
          ErrorCode::OperationFailed, "numeric environment unavailable"});
    // Region/coordinate and small callback argument vectors are ordinary C++
    // scratch; admit their bounded capacity before either stage allocates.
    auto scratch =
        math_take(phase.resources.reserve(ResourceCapacity::host(4096, 4096)));
    const auto& schema = *phase.query.output.result_schema;
    const auto shape = schema.tensors[0].sample_shape();
    if (!ready)
      ready.emplace(
          std::less<std::pair<uint32_t, uint32_t>>{},
          ResourceAllocator<ResultTensorInputs::value_type>(phase.resources));
    if (phase.tensors && !phase.tensors->empty()) {
      math_require(phase.consume_work(phase.tensors->size()));
      for (const auto& input : *phase.tensors)
        ready->insert_or_assign(input.first, input.second);
    }
    if (!started) {
      output = phase.query.tensor_outputs ? *phase.query.tensor_outputs
                                          : math_take(Footprint::all(shape));
      started = true;
    }
    if (!output.empty() && next_input < phase.query.inputs.size()) {
      ResultProgramNeed need;
      while (next_input < phase.query.inputs.size() && need.tiles.size() < 64) {
        const auto port = next_input++;
        if (!whole_tensor_port_selected(phase.query, port))
          continue;
        {
          auto tile_need_0 = sdk::tile_need_for_samples(
              phase.resources, phase.query, port, 0,
              math_take(Footprint::all(phase.query.inputs[port]
                                           .result_schema->tensors[0]
                                           .sample_shape())));
          if (!tile_need_0.ok())
            return Result<ResultProgramPoll>(tile_need_0.status());
          need.tiles.push_back(tile_need_0.take_value());
        }
      }
      if (!need.tiles.empty())
        return Result<ResultProgramPoll>(std::move(need));
    }
    auto builder = math_take(ResultBuilder::start(
        phase.resources, schema, phase.query.semantic_key, {},
        phase.query.tile_extent));
    if (!output.empty()) {
      // The phase container expires after each poll. These owning capabilities
      // retain only captured input facts and an owning failure latch, so a
      // bounded multi-envelope gather does not retain any phase callback.
      auto supplied = phase;
      supplied.tensors = &*ready;
      math_require(publish_whole_tensor(kernel, supplied, builder, 0));
    }
    return Result<ResultProgramPoll>(
        ResultPublication{math_take(builder.seal()), true});
  } catch (const Status& status) {
    auto failure = math_whole_failure(status);
    math_record_failure(phase, failure);
    return Result<ResultProgramPoll>(failure);
  } catch (const std::bad_alloc&) {
    auto failure = Status{ErrorCode::ResourceExhausted,
                          {},
                          FailureReason::CapacityLimit,
                          {FailureOrigin::Resource, FailureScope::Run}};
    math_record_failure(phase, failure);
    return Result<ResultProgramPoll>(failure);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
