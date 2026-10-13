#pragma once

#include <map>
#include <string>
#include <vector>

#include "common/tensor_metadata.hpp"
#include "photospider/compiler/workflow_document.hpp"

namespace ps::plugin_internal::expression_result {
/** @brief Resolve numeric.sample_expression or lut.apply_1d metadata.
 * Uses only static input descriptors/facets and parameters, without payload
 * reads. The caller validates operation arity and required parameter types.
 * Saves/restores the caller's floating environment on every return.
 */
Result<std::vector<ValueFacet>> infer_expression_facets(
    bool lut, const std::vector<TensorMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    const ValueDescriptor& output);
}  // namespace ps::plugin_internal::expression_result
