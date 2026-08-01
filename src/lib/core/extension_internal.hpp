#pragma once

#include <cstddef>
#include <vector>

#include "photospider/data/extension.hpp"

/**
 * @file extension_internal.hpp
 * @brief Source-private canonical-content digest composition seam.
 */

namespace ps::internal {

/**
 * @brief Hashes one descriptor identity and provider-canonical logical bytes.
 *
 * @param descriptor Exact canonical descriptor digest.
 * @param canonical_content Provider-emitted logical site/sample/record bytes
 *        after callback validation and access checks.
 * @return Typed SHA-256 canonical-v1 content digest.
 * @throws std::bad_alloc only if the SHA implementation allocates, which the
 *         current fixed-size implementation does not.
 * @note Physical buffer order, offsets, padding, and allocation identities
 *       must not be appended by the provider adapter.
 */
ContentDigest compute_content_digest_from_canonical_bytes(
    const DescriptorDigest& descriptor,
    const std::vector<std::byte>& canonical_content);

}  // namespace ps::internal
