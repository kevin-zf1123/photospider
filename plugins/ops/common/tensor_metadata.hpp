#pragma once

#include <vector>

#include "photospider/data/tensor_span.hpp"

namespace ps::plugin_internal {
/** @brief Local tensor facts for pure algorithm metadata inference.
 * This owned value holds no payload and is not an operation port contract.
 */
struct TensorMetadata final {
  TensorDescriptor descriptor;
};
}  // namespace ps::plugin_internal
