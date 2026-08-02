#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "photospider/data/extension.hpp"

/**
 * @file extension_internal.hpp
 * @brief Source-private canonical-content digest composition seam.
 */

namespace ps::internal {

/**
 * @brief Incrementally composes one length-framed canonical ContentDigest.
 *
 * Construction writes the frozen prefix, domain, DescriptorDigest field, and
 * canonical-content field header, including the already measured total byte
 * length. Each `append()` then updates the Host-owned SHA-256 state directly;
 * `finish()` accepts only the exact measured byte count.
 *
 * @throws std::bad_alloc when the fixed-size implementation owner cannot
 * allocate.
 * @note The only total-size ceiling is SHA-256 canonical-v1's intrinsic
 * 64-bit bit-length framing. No payload-proportional staging is owned here.
 */
class CanonicalContentDigestWriter final {
 public:
  /**
   * @brief Initializes canonical framing for one measured logical stream.
   * @param descriptor Exact canonical descriptor digest.
   * @param canonical_content_size Exact total provider-emitted byte count.
   * @throws ExtensionContractError for an unsupported descriptor algorithm or
   * content length outside SHA-256 canonical-v1 framing.
   * @throws std::bad_alloc when fixed implementation state cannot allocate.
   */
  CanonicalContentDigestWriter(const DescriptorDigest& descriptor,
                               std::uint64_t canonical_content_size);

  /** @brief Releases fixed Host-owned digest state without provider work. */
  ~CanonicalContentDigestWriter() noexcept;

  /** @brief Copy construction is forbidden for mutable digest state. */
  CanonicalContentDigestWriter(const CanonicalContentDigestWriter&) = delete;
  /** @brief Copy assignment is forbidden for mutable digest state. */
  CanonicalContentDigestWriter& operator=(const CanonicalContentDigestWriter&) =
      delete;
  /** @brief Move construction is forbidden while a C sink may borrow state. */
  CanonicalContentDigestWriter(CanonicalContentDigestWriter&&) = delete;
  /** @brief Move assignment is forbidden while a C sink may borrow state. */
  CanonicalContentDigestWriter& operator=(CanonicalContentDigestWriter&&) =
      delete;

  /**
   * @brief Synchronously hashes one provider-emitted logical byte segment.
   * @param data Borrowed bytes, null only when `size` is zero.
   * @param size Exact segment byte count.
   * @throws ExtensionContractError for malformed input, a byte-count mismatch,
   * or use after finalization.
   * @note Bytes are consumed before return and never retained; segment
   * boundaries do not enter canonical framing.
   */
  void append(const std::byte* data, std::size_t size);

  /**
   * @brief Finalizes one exactly measured canonical content stream.
   * @return Typed SHA-256 canonical-v1 ContentDigest.
   * @throws ExtensionContractError when fewer or more bytes than measured were
   * appended, or when finalization is repeated.
   */
  ContentDigest finish();

 private:
  /** @brief Translation-unit-private SHA state and checked counters. */
  struct Impl;

  /** @brief Fixed implementation state, never payload-proportional storage. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps::internal
