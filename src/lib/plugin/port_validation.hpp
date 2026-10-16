#include <vector>
#pragma once

#include "data/tensor_validation.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps::input_internal {
/** @brief Validates and canonicalizes a metadata-only declaration. */
Status validate_declaration(WorkflowInputDeclaration* declaration);

/** @brief Checks all closed port schema combinations before registration. */
Status validate_port_schema(const OperationTraits& traits);

/** @brief Checks declarative output/repeated-input records before publication.
 */
Status validate_operation_contract(const OperationTraits& traits);

/** @brief Whether this Result constraint selects a tensor member. Without a
 * key a dtype/rank/facet predicate requires one unambiguous tensor member.
 */
bool tensor_member_predicate(const OperationPortConstraint& port) noexcept;

Result<std::uint32_t> resolve_tensor_member(const OperationPortConstraint& port,
                                            const OperationMetadata& metadata);

Status validate_port_metadata(const OperationPortConstraint& port,
                              const OperationMetadata& metadata);

}  // namespace ps::input_internal
