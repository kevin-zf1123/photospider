#pragma once

#include <array>
#include <cstdint>

#include "photospider/data/extension.hpp"
#include "photospider/data/value.hpp"

/**
 * @file value_descriptor_metadata.hpp
 * @brief Source-private exact operation descriptor metadata retained by Value.
 */

namespace ps {

/**
 * @brief Exact representation identity carried by one operation DenseTensor.
 *
 * The ordinary C++ DenseTensor/ImageFacet/StridedLayout objects retain the
 * interpreted metadata graph. This record independently retains the publisher
 * assigned Schema, optional Facet, and Layout identities, structural versions,
 * and opaque digest words that the pure-C operation ABI must echo unchanged.
 *
 * @throws Nothing for ordinary construction, copying, and comparison.
 * @note All-zero digest words mean unavailable and remain distinct from a
 * synthesized digest. The Host never interprets or recomputes these words.
 */
struct DenseTensorValueDescriptorMetadata final {
  /** @brief Permanent representation-Schema identity. */
  ExtensionIdentity schema_identity;
  /** @brief Permanent primary Facet identity, or zero when no Facet exists. */
  ExtensionIdentity facet_identity;
  /** @brief Permanent physical Strided Layout identity. */
  ExtensionIdentity layout_identity;
  /** @brief Nonzero logical descriptor structural version. */
  std::uint64_t descriptor_version = 0U;
  /** @brief Nonzero physical Layout structural version. */
  std::uint64_t layout_version = 0U;
  /** @brief Exact opaque descriptor-digest words. */
  std::array<std::uint64_t, 4U> descriptor_digest{};
  /** @brief Exact opaque logical-content-digest words. */
  std::array<std::uint64_t, 4U> content_digest{};
  /** @brief Exact opaque physical-Layout-digest words. */
  std::array<std::uint64_t, 4U> layout_digest{};

  /**
   * @brief Compares every identity, version, and opaque digest word.
   * @param other Metadata to compare.
   * @return True only when the complete representation identity matches.
   * @throws Nothing.
   */
  bool operator==(
      const DenseTensorValueDescriptorMetadata& other) const noexcept {
    return schema_identity == other.schema_identity &&
           facet_identity == other.facet_identity &&
           layout_identity == other.layout_identity &&
           descriptor_version == other.descriptor_version &&
           layout_version == other.layout_version &&
           descriptor_digest == other.descriptor_digest &&
           content_digest == other.content_digest &&
           layout_digest == other.layout_digest;
  }

  /**
   * @brief Compares complete representation metadata for inequality.
   * @param other Metadata to compare.
   * @return True when any identity, version, or digest word differs.
   * @throws Nothing.
   */
  bool operator!=(
      const DenseTensorValueDescriptorMetadata& other) const noexcept {
    return !(*this == other);
  }
};

/**
 * @brief Source-private bridge into ValueBuilder and immutable Value metadata.
 *
 * @throws std::invalid_argument for zero identities or versions.
 * @throws std::logic_error for invalid, sealed, or conflicting builder state.
 * @note This bridge never changes descriptor/layout bytes, payload ownership,
 * readiness, revision, or producer identity. Attachment occurs before Value
 * publication and an identical repeated attachment is idempotent.
 */
class DenseTensorValueDescriptorMetadataAccess final {
 public:
  /**
   * @brief Attaches exact metadata to one unsealed DenseTensor builder.
   * @param builder Nonnull exclusive builder before publication.
   * @param metadata Complete Schema/Layout identity/version record to retain;
   *        Facet identity is zero exactly when the builder has no Facet.
   * @return Nothing.
   * @throws std::invalid_argument for null builder or malformed metadata.
   * @throws std::logic_error when the builder is moved-from, sealed, or already
   * carries different metadata.
   * @note Repeating the identical metadata is permitted for independently
   * validated tiles sharing one final output binding. A source-private Host
   * binding may attach it while its sole WriteLease is active because the
   * record changes no payload address, byte, or producer authority.
   */
  static void attach(ValueBuilder* builder,
                     DenseTensorValueDescriptorMetadata metadata);

  /**
   * @brief Observes exact operation metadata retained by one immutable Value.
   * @param value Value to inspect without payload access.
   * @return Borrowed metadata, or null when the Value is invalid,
   * provider-defined, or was published outside an operation descriptor route.
   * @throws Nothing.
   * @note The pointer remains valid while `value` or one of its copies retains
   * the shared immutable publication.
   */
  static const DenseTensorValueDescriptorMetadata* get(
      const Value& value) noexcept;
};

}  // namespace ps
