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

#include "photospider/plugin/operation_registry.hpp"
#include "photospider/plugin/result_program.hpp"
#include "plugin/failure_latch.hpp"

namespace ps::plugin_internal::numeric_ops {
template <class T>
T math_take(Result<T> answer) {
  if (!answer.ok())
    throw answer.status();
  return answer.take_value();
}
inline void math_require(Status status) {
  if (!status.ok())
    throw status;
}
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
                Value::element_size(spec.descriptor.element_type));
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
inline SchemaTemplate numeric_tensor_schema(
    ElementType dtype, const std::vector<uint64_t>& shape) {
  SchemaTemplate schema;
  schema.id = "photospider.tensor";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {dtype, shape};
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
inline void set_whole_tensor_output(OperationTraits& traits, ElementType type,
                                    uint64_t continuation, uint32_t index = 0) {
  auto& output = traits.outputs[index];
  output.key = "values";
  output.output_schema.kind = OperationPortKind::Result;
  output.output_schema.result_schema_id = "photospider.tensor";
  output.output_schema.result_schema_version = 1;
  output.result_schema = numeric_tensor_schema(type, {1});
  output.region_rule = OperationRegionRule::Whole;
  output.dependency_version = 2;
  output.continuation_bytes = continuation;
  output.maximum_dependency_stages = 2;
}
inline void math_record_failure(const ResultProgramPhase& phase,
                                const Status& failure) {
  // Checked private kernels own this cause. Record its full status before the
  // ambient allocator's code-only flag is observed by the coordinator.
  if (phase.failure_latch)
    phase.failure_latch->record(failure);
  if (phase.failure_observer)
    phase.failure_observer(failure);
}
inline Status math_whole_failure(Status failure) {
  if (failure.detail.scope == FailureScope::Atom) {
    failure.detail.scope = FailureScope::Run;
    failure.detail.atom.reset();
  }
  return failure;
}
template <class Callback>
Status math_callback(const ResultProgramPhase& phase, Callback&& callback) {
  try {
    auto status = math_whole_failure(callback());
    if (!status.ok())
      math_record_failure(phase, status);
    return status;
  } catch (const Status& status) {
    auto failure = math_whole_failure(status);
    math_record_failure(phase, failure);
    return failure;
  } catch (const std::bad_alloc&) {
    auto failure = Status{ErrorCode::ResourceExhausted,
                          {},
                          FailureReason::CapacityLimit,
                          {FailureOrigin::Resource, FailureScope::Run}};
    math_record_failure(phase, failure);
    return failure;
  }
}
template <class Kernel>
using WholeTensorPublication = decltype(std::declval<Kernel&>().publish(
    std::declval<const ResultProgramPhase&>(), std::declval<ResultBuilder&>(),
    std::declval<ResultRelation>()));
template <class Kernel>
WholeTensorPublication<Kernel> publish_whole_tensor(
    Kernel& kernel, const ResultProgramPhase& phase, ResultBuilder& builder,
    ResultRelation relation, int) {
  return kernel.publish(phase, builder, std::move(relation));
}
template <class Kernel>
Status publish_whole_tensor(Kernel& kernel, const ResultProgramPhase& phase,
                            ResultBuilder& builder, ResultRelation relation,
                            int64_t) {
  const auto shape =
      phase.query.output.result_schema->tensors[0].sample_shape();
  return builder.publish_tensor_kernel(
      0, Region::whole(shape),
      [&](const auto& writers) {
        return math_callback(phase,
                             [&] { return kernel.write(phase, writers); });
      },
      std::move(relation), {true, true, true, true}, phase.query.cancellation);
}
inline bool whole_tensor_port_selected(const ResultProgramQuery& query,
                                       uint32_t port) {
  if (!query.prepared)
    return true;
  const auto& selected =
      query.prepared->traits().outputs.at(query.output_index).input_indices;
  return !selected ||
         std::find(selected->begin(), selected->end(), port) != selected->end();
}
template <class Kernel>
auto whole_tensor_support_roles(int) -> decltype(Kernel::support_roles()) {
  return Kernel::support_roles();
}
template <class Kernel>
std::uint32_t whole_tensor_support_roles(int64_t) {
  return 1;
}
// Shared private Whole protocol. Numerical kernels receive authorized input
// capabilities and one borrowed packed writer; no Value operation is invoked.
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
      while (next_input < phase.query.inputs.size() &&
             need.tensors.size() < 64) {
        const auto port = next_input++;
        if (!whole_tensor_port_selected(phase.query, port))
          continue;
        need.tensors.push_back(
            {port, 0,
             math_take(Footprint::all(phase.query.inputs[port]
                                          .result_schema->tensors[0]
                                          .sample_shape())),
             13});
      }
      if (!need.tensors.empty())
        return Result<ResultProgramPoll>(std::move(need));
    }
    auto builder = math_take(ResultBuilder::start(
        phase.resources, schema, phase.query.semantic_key, {},
        phase.association ? std::vector<uint64_t>(phase.association->begin(),
                                                  phase.association->end())
                          : std::vector<uint64_t>{},
        128, 128, phase.query.resources));
    ResultRelation descriptors, data;
    const auto count = math_take(schema.tensors[0].sample_count());
    for (uint32_t port = 0; port < phase.query.inputs.size(); ++port) {
      if (!whole_tensor_port_selected(phase.query, port))
        continue;
      auto descriptor = math_take(
          ResultRelation::cartesian(phase.resources, 1,
                                    {port, 8, 0, output.empty() ? 0U : 1U,
                                     ResultSupportTarget::Descriptor, 0}));
      descriptors = descriptors.valid()
                        ? math_take(ResultRelation::unite(
                              phase.resources, {descriptors, descriptor}))
                        : std::move(descriptor);
      if (!output.empty()) {
        const auto input_count = math_take(
            phase.query.inputs[port].result_schema->tensors[0].sample_count());
        auto input = math_take(ResultRelation::cartesian(
            phase.resources, count,
            {port, whole_tensor_support_roles<Kernel>(0), 0, input_count,
             ResultSupportTarget::Tensor, 0}));
        data = data.valid() ? math_take(ResultRelation::unite(phase.resources,
                                                              {data, input}))
                            : std::move(input);
      }
    }
    if (!descriptors.valid()) {
      // An empty runtime projection uses compiled metadata only. The empty
      // support is a known constant witness and schedules no source actor.
      descriptors = math_take(ResultRelation::cartesian(
          phase.resources, 1,
          {0, 8, 0, 0, ResultSupportTarget::Descriptor, 0}));
      data = math_take(ResultRelation::cartesian(
          phase.resources, count,
          {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
    }
    math_require(builder.bind_descriptor_relation(std::move(descriptors)));
    if (!output.empty()) {
      // The phase container expires after each poll. These owning capabilities
      // retain only captured input facts and an owning failure latch, so a
      // bounded multi-envelope gather does not retain any phase callback.
      auto supplied = phase;
      supplied.tensors = &*ready;
      math_require(
          publish_whole_tensor(kernel, supplied, builder, std::move(data), 0));
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
