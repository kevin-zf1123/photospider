#pragma once

#include <vector>

#include "photospider/data/semantic.hpp"
#include "photospider/data/value.hpp"

namespace ps::plugin_internal {
/** @brief Local tensor facts for pure algorithm metadata inference.
 * This owned value holds no payload and is not an operation port contract.
 */
struct TensorMetadata final {
  ValueDescriptor descriptor;
  std::vector<ValueFacet> facets;
};
}  // namespace ps::plugin_internal
