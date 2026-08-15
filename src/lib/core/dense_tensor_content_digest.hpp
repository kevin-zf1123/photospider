/**
 * @file dense_tensor_content_digest.hpp
 * @brief Declares canonical logical content identity for built-in tensors.
 */
#pragma once

#include "photospider/data/extension.hpp"

namespace ps {
class Value;
}

namespace ps::internal {

/**
 * @brief Computes canonical-v1 logical content for a built-in DenseTensor.
 * @param value Valid built-in DenseTensor publication.
 * @return Available digest, PayloadUnavailable for a non-Ready/non-readable
 * binding, or InvalidDescriptor for a malformed/unsupported retained value.
 * @throws std::bad_alloc when bounded descriptor/traversal state cannot
 * allocate.
 * @note Canonical traversal is row-major logical-axis order. It excludes
 * strides, offsets, padding, allocation identity, device identity, and Value
 * revision. Native whole-byte scalars are emitted little-endian; FP4 values
 * emit one low-nibble code byte per logical element.
 */
ContentDigestResult compute_dense_tensor_content_digest(const Value& value);

}  // namespace ps::internal
