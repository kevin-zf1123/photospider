#pragma once

#include <cstdint>
#include <new>
#include <utility>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"
#include "photospider/plugin/result_program.hpp"

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

  output.output_schema.result_schema_id = "photospider.tensor";
  output.output_schema.result_schema_version = 1;
  output.result_schema = numeric_tensor_schema(type, {1});
  output.continuation_bytes = continuation;
  output.maximum_dependency_stages = 2;
}
inline void math_record_failure(const ResultProgramPhase& phase,
                                const Status& failure) {
  // Checked private kernels own this cause. Record its full status before the
  // ambient allocator's code-only flag is observed by the coordinator.
  phase.record_failure(failure);
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
}  // namespace ps::plugin_internal::numeric_ops
