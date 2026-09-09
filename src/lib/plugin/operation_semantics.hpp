#pragma once

#include <map>
#include <string>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"

namespace ps::contract_internal {
/** @brief Shared closed semantic transformations; independent of callback/key.
 */
Result<std::vector<ValueFacet>> infer_transformed_facets(
    const OperationTraits& traits, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    const ValueDescriptor& output);
}  // namespace ps::contract_internal
