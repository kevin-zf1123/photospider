#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal::tensor_ops {
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw result.status();
  return result.take_value();
}
inline void require(Status status) {
  if (!status.ok())
    throw status;
}
inline SchemaTemplate scalar_schema() {
  SchemaTemplate schema;
  schema.id = "photospider.tensor";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ElementType::Float64, {1}};
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
inline OperationPortConstraint port() {
  OperationPortConstraint constraint;

  constraint.result_schema_id = "photospider.tensor";
  constraint.result_schema_version = 1;
  constraint.tensor_key = "samples";
  return constraint;
}
inline Status check_tensor(const OperationMetadata& metadata) {
  if (!metadata.result_schema || metadata.result_schema->tensors.size() != 1)
    return {ErrorCode::TypeMismatch,
            "operation requires one tensor member and no scalar fields"};
  return metadata.result_schema->validate();
}
inline OperationTraits traits(std::uint32_t inputs) {
  OperationTraits traits;
  traits.input_count = inputs;
  traits.input_schema.assign(inputs, port());
  auto& output = traits.outputs[0];
  output.output_schema = port();
  output.result_schema = scalar_schema();
  return traits;
}
// Check the complete scalar entry contract once, before unchecked invocation.
inline Status scalar_signature(const KernelPreparation& query,
                               unsigned inputs) {
  auto scalar = [](const KernelView& view, unsigned port) {
    return view.port == port && view.member == 0 && view.tile_index == 0 &&
           view.domain == physical::Domain::Host &&
           view.layout.element_type == ElementType::Float64 &&
           view.layout.rank == 1 && view.layout.shape[0] == 1 &&
           view.layout.origin[0] == 0 && view.bytes >= 8 &&
           view.layout.byte_offset <= view.bytes - 8;
  };
  if (query.reads.size() != inputs || query.writes.size() != 1 ||
      !query.task_parameters.empty() ||
      query.scratch != std::array<std::uint64_t, 3>{} ||
      !scalar(query.writes[0], 0))
    return {ErrorCode::TypeMismatch, "invalid scalar kernel signature"};
  for (unsigned i = 0; i < inputs; ++i)
    if (!scalar(query.reads[i], i))
      return {ErrorCode::TypeMismatch, "invalid scalar kernel input"};
  return Status::success();
}
inline OperationDefinition identity(const char* key) {
  OperationDefinition operation;
  operation.key = key;
  operation.traits = traits(1);
  OperationPortConstraint tensor;

  tensor.element_type_mask = 127;
  operation.traits.input_schema[0] = tensor;
  operation.traits.outputs[0].output_schema = tensor;
  operation.traits.requires_metadata_specialization = true;
  operation.specialize_metadata =
      [](const auto& inputs,
         const auto&) -> Result<std::vector<OperationOutputSpecialization>> {
    auto valid = check_tensor(inputs[0]);
    if (!valid.ok())
      return Result<std::vector<OperationOutputSpecialization>>(valid);
    OperationOutputSpecialization output;
    output.metadata.result_schema = inputs[0].result_schema;
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  operation.lower =
      [](const OperationLowering& query) -> Result<OperationProgram> {
    OperationProgramBuilder builder(query);
    for (const auto& member : query.requests[0].members) {
      for (auto index : member.second) {
        auto task = builder.add_copy();
        if (!task.ok())
          return Result<OperationProgram>(task.status());
        auto status = builder.read_input(task.value(), 0, member.first, index);
        if (status.ok())
          status = builder.write_output(task.value(), 0, member.first, index);
        if (!status.ok())
          return Result<OperationProgram>(status);
      }
    }
    return builder.finish();
  };
  return operation;
}
}  // namespace ps::plugin_internal::tensor_ops
