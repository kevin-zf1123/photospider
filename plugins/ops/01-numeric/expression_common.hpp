#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "00-foundation/numeric_common.hpp"
#include "data/input_validation.hpp"
#include "plugin/expression.hpp"

namespace ps::plugin_internal::expression_ops {
inline Status stopped() {
  Status status;
  status.code = ErrorCode::Cancelled;
  return status;
}
inline Result<OperationMetadata> metadata(const OperationTraits& traits,
                                          const OperationInvocation& call) {
  auto resolved =
      resolve_operation_traits(traits, call.inputs.size(), call.parameters);
  if (!resolved.ok())
    return Result<OperationMetadata>(resolved.status());
  std::vector<OperationMetadata> inputs;
  for (const auto& input : call.inputs)
    inputs.push_back({input.descriptor(), input.facets()});
  return infer_operation_output(resolved.value(), inputs, call.parameters);
}

inline SemanticDescriptor semantic(const Value& value) {
  for (const auto& facet : value.facets())
    if (facet.key == "photospider.semantic")
      return decode_semantic(facet).take_value();
  return {};
}
// Retain local sampling distances before weighting. Normalizing by a power
// of two bounds products even when the declared step is near DBL_MAX.

}  // namespace ps::plugin_internal::expression_ops
