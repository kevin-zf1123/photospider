#include "plugin/port_validation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/dense_layout_validation.hpp"

namespace ps::input_internal {
namespace {
Status failure(ErrorCode code, const char* message) {
  return Status::failure(code, message);
}
Status validate_member_descriptor(const OperationPortConstraint& port,
                                  const TensorDescriptor& descriptor);
bool valid_constraint(const OperationPortConstraint& port) {
  const bool fixed = !port.result_schema_id.empty();
  if ((fixed && (!valid_input_name(port.result_schema_id) ||
                 !port.result_schema_version)) ||
      (!fixed && port.result_schema_version) ||
      (!fixed && !tensor_member_predicate(port)))
    return false;
  return (port.tensor_key.empty() || valid_input_name(port.tensor_key)) &&
         port.rank <= 8 && port.element_type <= 7 &&
         !(port.element_type_mask & ~UINT32_C(127)) &&
         !(port.element_type && port.element_type_mask);
}
}  // namespace

Status validate_declaration(WorkflowInputDeclaration* declaration) {
  if (!declaration->result_schema)
    return failure(ErrorCode::InvalidArgument,
                   "workflow input requires Result schema");
  return declaration->result_schema->validate(true);
}

Status validate_port_schema(const OperationTraits& traits) {
  if (traits.outputs.size() != 1)
    return failure(ErrorCode::InvalidArgument, "select one output schema");
  if (traits.input_count > 1024 ||
      traits.input_schema.size() !=
          traits.input_count +
              (traits.repeated_maximum && !traits.repeated_resolved ? 1U
                                                                    : 0U) ||
      !valid_constraint(traits.outputs[0].output_schema)) {
    return failure(ErrorCode::InvalidArgument,
                   "invalid port schema count or output");
  }
  for (const auto& port : traits.input_schema)
    if (!valid_constraint(port))
      return failure(ErrorCode::InvalidArgument, "invalid Result input port");
  return Status::success();
}

bool tensor_member_predicate(const OperationPortConstraint& port) noexcept {
  return !port.tensor_key.empty() || port.element_type ||
         port.element_type_mask || port.rank;
}
Result<std::uint32_t> resolve_tensor_member(const OperationPortConstraint& port,
                                            const OperationMetadata& metadata) {
  using Answer = Result<std::uint32_t>;
  if (!metadata.result_schema)
    return Answer(
        failure(ErrorCode::TypeMismatch, "Result tensor port required"));
  const auto& schema = *metadata.result_schema;
  std::uint32_t slot = 0;
  if (port.tensor_key.empty()) {
    if (schema.tensors.size() != 1)
      return Answer(
          failure(ErrorCode::TypeMismatch, "select a named tensor member"));
  } else {
    while (slot < schema.tensors.size() &&
           std::string_view(schema.tensors[slot].key) != port.tensor_key)
      ++slot;
    if (slot == schema.tensors.size())
      return Answer(failure(ErrorCode::TypeMismatch,
                            "required tensor member is missing"));
  }
  const auto& tensor = schema.tensors[slot];
  auto valid = validate_member_descriptor(port, tensor.descriptor);
  if (!valid.ok())
    return Answer(valid);
  return Answer(slot);
}
Status validate_port_metadata(const OperationPortConstraint& port,
                              const OperationMetadata& metadata) {
  if (metadata.result_schema) {
    if (!port.result_schema_id.empty() &&
        (std::string_view(metadata.result_schema->id) !=
             std::string_view(port.result_schema_id) ||
         metadata.result_schema->version != port.result_schema_version))
      return failure(ErrorCode::TypeMismatch,
                     "structured input schema mismatch");
    auto valid = metadata.result_schema->validate(true);
    if (!valid.ok() || !tensor_member_predicate(port))
      return valid;
    auto slot = resolve_tensor_member(port, metadata);
    return slot.ok() ? Status::success() : slot.status();
  }
  return failure(ErrorCode::TypeMismatch, "Result schema required");
}

namespace {
Status validate_member_descriptor(const OperationPortConstraint& port,
                                  const TensorDescriptor& descriptor) {
  auto canonical_status = validate_value_structure(descriptor);
  if (!canonical_status.ok())
    return canonical_status;
  const auto element = static_cast<std::uint32_t>(descriptor.element_type);
  if ((port.element_type &&
       port.element_type !=
           static_cast<std::uint32_t>(descriptor.element_type)) ||
      (port.rank && port.rank != descriptor.shape.size()) ||
      (port.element_type_mask &&
       !(port.element_type_mask & (1U << (element - 1)))))
    return failure(ErrorCode::TypeMismatch, "port dtype/rank mismatch");
  return Status::success();
}

}  // namespace

}  // namespace ps::input_internal
