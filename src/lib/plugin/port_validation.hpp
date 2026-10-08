#include <vector>
#pragma once

#include "data/value_validation.hpp"
#include "photospider/data/dependency.hpp"
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

Status validate_port_metadata(const OperationPortConstraint& port,
                              const ValueDescriptor& descriptor,
                              const std::vector<ValueFacet>& facets);

/** @brief Shared checked Whole/elementwise/halo demand rule for planning and
 * direct invocation. Halo traits must have their static parameter resolved.
 */
Result<Region> derive_input_demand(
    const OperationTraits& traits, const Region& output_demand,
    const std::vector<std::uint64_t>& output_shape,
    const std::vector<std::uint64_t>& input_shape, OperationPortKind kind);

/**
 * @brief Checks dense port metadata and numeric domain without coercion.
 * @note stop is observed periodically while scanning, and returns Ok or a
 * prioritized Cancelled/Stale code without allocating diagnostic strings.
 */
Status validate_port_value(const OperationPortConstraint& port,
                           const Value& value, ErrorCode numeric_failure,
                           const std::function<ErrorCode()>& stop);

/** @brief Validate a bounded scalar tensor using captured immutable coverage.
 * Reads its logical sample through Result access without dense-layout
 * assumptions. numeric_failure distinguishes external admission from computed
 * input failure.
 */
Status validate_port_tensor(const OperationPortConstraint& port,
                            const ResultRef& result,
                            const ResultDescriptor& descriptor,
                            const OperationMetadata& metadata,
                            ErrorCode numeric_failure,
                            const CancellationToken& cancellation,
                            const std::function<ErrorCode()>& stop);

/** @brief Copies a static need as Validation with fixed full channel interval.
 * @note Caller has validated metadata and mapping rank. Source tags and all
 * leading-axis relations remain unchanged.
 */
DependencyMappedNeed validation_map(DependencyMappedNeed support,
                                    const OperationMetadata& metadata);
}  // namespace ps::input_internal
