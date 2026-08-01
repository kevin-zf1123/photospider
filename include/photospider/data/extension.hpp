#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/region.hpp"

/**
 * @file extension.hpp
 * @brief Versioned dependency-neutral data-extension and identity contracts.
 */

namespace ps {

class Value;

/** @brief Maximum Facets retained by one V-14 descriptor envelope. */
constexpr std::size_t kMaximumExtensionFacets = 32U;
/** @brief Maximum buffers retained by one V-14 provider-defined Value. */
constexpr std::size_t kMaximumExtensionBuffers = 32U;
/** @brief Maximum buffer envelopes retained by one provider Layout. */
constexpr std::size_t kMaximumBufferEnvelopes = 128U;
/** @brief Maximum bytes retained by one extension payload. */
constexpr std::size_t kMaximumExtensionPayloadBytes = 1024U * 1024U;
/** @brief Maximum serialized V-14 metadata-envelope bytes. */
constexpr std::size_t kMaximumExtensionMetadataBytes = 8U * 1024U * 1024U;

/**
 * @brief Permanent process-independent 128-bit extension identity.
 *
 * @throws Nothing for ordinary value operations.
 * @note The all-zero identity is invalid. Names remain diagnostic only.
 */
struct ExtensionIdentity final {
  /** @brief Most-significant fixed identity word. */
  std::uint64_t high = 0U;
  /** @brief Least-significant fixed identity word. */
  std::uint64_t low = 0U;

  /**
   * @brief Reports whether this identity is nonzero.
   * @return True unless both words are zero.
   * @throws Nothing.
   */
  constexpr bool valid() const noexcept { return high != 0U || low != 0U; }

  /**
   * @brief Compares both permanent identity words.
   * @param other Identity to compare.
   * @return True when both words match.
   * @throws Nothing.
   */
  constexpr bool operator==(const ExtensionIdentity& other) const noexcept {
    return high == other.high && low == other.low;
  }

  /**
   * @brief Compares identities for inequality.
   * @param other Identity to compare.
   * @return True when either word differs.
   * @throws Nothing.
   */
  constexpr bool operator!=(const ExtensionIdentity& other) const noexcept {
    return !(*this == other);
  }

  /**
   * @brief Provides canonical numeric ordering.
   * @param other Identity to compare.
   * @return True when this high/low pair sorts first.
   * @throws Nothing.
   */
  constexpr bool operator<(const ExtensionIdentity& other) const noexcept {
    return high < other.high || (high == other.high && low < other.low);
  }
};

/**
 * @brief Identifies one typed definition namespace.
 * @throws Nothing for ordinary value operations.
 */
enum class ExtensionDefinitionKind : std::uint32_t {
  /** @brief Logical Representation Schema definition. */
  Schema = 1U,
  /** @brief Orthogonal logical Facet definition. */
  Facet = 2U,
  /** @brief Physical Layout definition. */
  Layout = 3U,
};

/**
 * @brief Byte-preserving versioned descriptor or Layout extension.
 *
 * @throws std::bad_alloc when copied payload storage cannot allocate.
 * @note The Host owns framing and preservation but never interprets payload.
 */
struct ExtensionRecord final {
  /** @brief Typed definition namespace selected by this record. */
  ExtensionDefinitionKind kind = ExtensionDefinitionKind::Schema;
  /** @brief Permanent definition identity. */
  ExtensionIdentity identity;
  /** @brief Nonzero provider-owned structural version. */
  std::uint32_t structural_version = 0U;
  /** @brief Exact provider payload, including versioned unknown bytes. */
  std::vector<std::byte> payload;

  /**
   * @brief Compares complete byte-preserving extension state.
   * @param other Record to compare.
   * @return True when kind, identity, version, and bytes all match.
   * @throws Nothing under byte-vector equality.
   */
  bool operator==(const ExtensionRecord& other) const noexcept {
    return kind == other.kind && identity == other.identity &&
           structural_version == other.structural_version &&
           payload == other.payload;
  }
};

/**
 * @brief Complete provider-defined logical descriptor envelope.
 *
 * @throws std::bad_alloc when copied Facet or payload storage cannot allocate.
 * @note Facet insertion order is preserved for byte-exact artifact round-trip;
 * canonical DescriptorDigest traversal applies its own deterministic sort.
 */
struct DataDescriptorEnvelope final {
  /** @brief Exactly one Schema extension. */
  ExtensionRecord schema;
  /** @brief Zero or more byte-preserving Facet extensions. */
  std::vector<ExtensionRecord> facets;

  /**
   * @brief Compares complete preserved descriptor state.
   * @param other Descriptor to compare.
   * @return True when Schema and ordered Facets match exactly.
   * @throws Nothing under supported vector equality.
   */
  bool operator==(const DataDescriptorEnvelope& other) const noexcept {
    return schema == other.schema && facets == other.facets;
  }
};

/**
 * @brief One checked provider Layout reference into a Value buffer.
 *
 * @throws Nothing for ordinary value operations.
 * @note Offset and length are relative to the checked BufferHandle range, not
 * to an allocation base or native handle.
 */
struct BufferEnvelope final {
  /** @brief Dense zero-based buffer index. */
  std::uint32_t buffer_index = 0U;
  /** @brief Provider-defined nonzero logical role. */
  std::uint32_t logical_role = 0U;
  /** @brief Byte offset inside the referenced BufferHandle range. */
  std::uint64_t offset = 0U;
  /** @brief Positive checked byte length. */
  std::uint64_t length = 0U;

  /**
   * @brief Compares all generic Layout envelope facts.
   * @param other Envelope to compare.
   * @return True when index, role, offset, and length match.
   * @throws Nothing.
   */
  constexpr bool operator==(const BufferEnvelope& other) const noexcept {
    return buffer_index == other.buffer_index &&
           logical_role == other.logical_role && offset == other.offset &&
           length == other.length;
  }
};

/**
 * @brief Complete byte-preserving provider-defined physical Layout envelope.
 *
 * @throws std::bad_alloc when copied payload or envelope storage cannot
 * allocate.
 * @note Buffer allocation identities and payload bytes are intentionally
 * absent.
 */
struct ProviderDefinedLayout final {
  /** @brief Exactly one Layout extension record. */
  ExtensionRecord definition;
  /** @brief Generic checked buffer subranges used by the Layout. */
  std::vector<BufferEnvelope> buffers;

  /**
   * @brief Compares complete preserved Layout metadata.
   * @param other Layout to compare.
   * @return True when definition and ordered envelopes match exactly.
   * @throws Nothing under supported vector equality.
   */
  bool operator==(const ProviderDefinedLayout& other) const noexcept {
    return definition == other.definition && buffers == other.buffers;
  }
};

/**
 * @brief Stable category for extension publication or callback failures.
 * @throws Nothing for ordinary enum operations.
 */
enum class ExtensionErrorCode {
  /** @brief Generic envelope framing or limit is invalid. */
  InvalidEnvelope,
  /** @brief Buffer index/range/cross-reference is invalid. */
  InvalidBinding,
  /** @brief No active provider resolves all selected definitions. */
  MissingProvider,
  /** @brief A provider exists but structural version is unsupported. */
  UnsupportedSchemaVersion,
  /** @brief Provider callback rejected descriptor or Layout semantics. */
  ProviderRejected,
  /** @brief Provider callback returned malformed output. */
  InvalidProviderOutput,
  /** @brief Ready or host-readable payload is unavailable. */
  PayloadUnavailable,
  /** @brief Serialized metadata framing is malformed or truncated. */
  InvalidSerialization,
};

/**
 * @brief Host-owned typed extension contract failure.
 *
 * @throws std::bad_alloc when diagnostic storage cannot allocate.
 * @note No provider exception identity or borrowed diagnostic pointer escapes.
 */
class ExtensionContractError final : public std::runtime_error {
 public:
  /**
   * @brief Constructs one owned typed failure.
   * @param code Stable extension failure category.
   * @param message Owned reader-facing diagnostic.
   * @throws std::bad_alloc when runtime_error storage cannot allocate.
   */
  ExtensionContractError(ExtensionErrorCode code, std::string message)
      : std::runtime_error(std::move(message)), code_(code) {}

  /**
   * @brief Returns the stable failure category.
   * @return Category supplied at construction.
   * @throws Nothing.
   */
  ExtensionErrorCode code() const noexcept { return code_; }

 private:
  /** @brief Stable failure category. */
  ExtensionErrorCode code_;
};

/**
 * @brief Stable pure property-query state.
 * @throws Nothing for ordinary enum operations.
 */
enum class PropertyQueryState {
  /** @brief One bounded scalar or byte value is present. */
  Available,
  /** @brief The property is defined but does not apply to this descriptor. */
  NotApplicable,
  /** @brief The provider cannot determine the property from current facts. */
  Unknown,
  /** @brief Explicit payload traversal or later work would be required. */
  Deferred,
  /** @brief No active provider owns the required typed definition bundle. */
  MissingProvider,
  /** @brief A typed identity exists only at another structural version. */
  UnsupportedSchemaVersion,
  /** @brief Descriptor or callback output framing is invalid. */
  InvalidDescriptor,
};

/**
 * @brief Bounded property query naming one provider-defined property.
 * @throws Nothing for ordinary value operations.
 */
struct PropertyQuery final {
  /** @brief Stable property identity. */
  ExtensionIdentity property;
};

/**
 * @brief Owned pure property-query outcome.
 *
 * @throws std::bad_alloc when copied byte or diagnostic storage cannot
 * allocate.
 * @note Available may carry either the optional scalar or byte value, never
 * both. Other states carry neither value.
 */
struct PropertyQueryResult final {
  /** @brief Exact typed query state. */
  PropertyQueryState state = PropertyQueryState::Unknown;
  /** @brief Optional bounded unsigned scalar result. */
  std::optional<std::uint64_t> unsigned_value;
  /** @brief Optional bounded opaque byte result. */
  std::vector<std::byte> bytes_value;
  /** @brief Host-owned diagnostic copied before lease release. */
  std::string diagnostic;
};

/**
 * @brief Bounded V-14 set predicate over one Schema and logical-site range.
 *
 * @throws Nothing for ordinary value operations.
 * @note Minimum/maximum endpoints are inclusive. Validation rejects zero
 * versions, inverted ranges, and an invalid Schema identity.
 */
struct DataSpec final {
  /** @brief Required Schema identity. */
  ExtensionIdentity schema_identity;
  /** @brief Inclusive structural-version lower bound. */
  std::uint32_t minimum_version = 0U;
  /** @brief Inclusive structural-version upper bound. */
  std::uint32_t maximum_version = 0U;
  /** @brief Inclusive logical-site lower bound. */
  std::uint64_t minimum_logical_sites = 0U;
  /** @brief Inclusive logical-site upper bound. */
  std::uint64_t maximum_logical_sites = 0U;
};

/**
 * @brief Typed DataSpec set relation without conversion authority.
 * @throws Nothing for ordinary enum operations.
 */
enum class DataSpecRelation {
  /** @brief Every described producer value satisfies the requested set. */
  Subset,
  /** @brief Producer and requested sets do not intersect. */
  Disjoint,
  /** @brief Sets overlap but runtime facts must guard concrete acceptance. */
  PartialOverlapWithRuntimeGuard,
  /** @brief Available metadata cannot prove a safe set relation. */
  CannotEvaluate,
};

/**
 * @brief Host-owned DataSpec compatibility outcome.
 * @throws std::bad_alloc when diagnostic storage cannot allocate.
 */
struct DataSpecResult final {
  /** @brief Exact set relation. */
  DataSpecRelation relation = DataSpecRelation::CannotEvaluate;
  /** @brief True only for PartialOverlapWithRuntimeGuard. */
  bool requires_runtime_guard = false;
  /** @brief Host-owned provider or validation diagnostic. */
  std::string diagnostic;
};

/**
 * @brief Typed provider Region-evaluation state.
 * @throws Nothing for ordinary enum operations.
 */
enum class ProviderRegionState {
  /** @brief The normalized request and selected-site count are exact. */
  Exact,
  /** @brief The provider cannot determine an exact bounded selection. */
  Unknown,
  /** @brief The provider does not support the request domain or shape. */
  Unsupported,
  /** @brief The request exceeds the explicit complexity budget. */
  TooComplex,
  /** @brief No active provider owns the required typed definition bundle. */
  MissingProvider,
  /** @brief A typed identity exists only at another structural version. */
  UnsupportedSchemaVersion,
  /** @brief Descriptor, request, or callback output framing is invalid. */
  InvalidDescriptor,
};

/**
 * @brief Owned bounded provider Region outcome.
 * @throws std::bad_alloc when Region or diagnostic storage cannot allocate.
 * @note Only Exact carries a Region, copied from the validated request.
 */
struct ProviderRegionResult final {
  /** @brief Exact typed provider outcome. */
  ProviderRegionState state = ProviderRegionState::Unknown;
  /** @brief Exact normalized request selection when available. */
  std::optional<RegionSet> region;
  /** @brief Exact selected logical-site count reported by the provider. */
  std::uint64_t selected_logical_sites = 0U;
  /** @brief Host-owned diagnostic. */
  std::string diagnostic;
};

/**
 * @brief Only canonical digest algorithm approved by V-14.
 * @throws Nothing for ordinary enum operations.
 */
enum class CanonicalDigestAlgorithm : std::uint32_t {
  /** @brief SHA-256 over canonical stream format version one. */
  Sha256CanonicalV1 = 1U,
};

/** @brief Exact SHA-256 byte width. */
constexpr std::size_t kCanonicalDigestBytes = 32U;

/**
 * @brief Strongly typed canonical descriptor identity.
 * @throws Nothing for ordinary value operations.
 */
struct DescriptorDigest final {
  /** @brief Explicit approved algorithm and stream version. */
  CanonicalDigestAlgorithm algorithm =
      CanonicalDigestAlgorithm::Sha256CanonicalV1;
  /** @brief Exact 256-bit digest bytes in conventional network order. */
  std::array<std::byte, kCanonicalDigestBytes> bytes{};

  /**
   * @brief Compares complete typed descriptor digest state.
   * @param other Digest to compare.
   * @return True when algorithm and all bytes match.
   * @throws Nothing.
   */
  bool operator==(const DescriptorDigest& other) const noexcept {
    return algorithm == other.algorithm && bytes == other.bytes;
  }
};

/**
 * @brief Strongly typed canonical storage-Layout identity.
 * @throws Nothing for ordinary value operations.
 */
struct StorageLayoutDigest final {
  /** @brief Explicit approved algorithm and stream version. */
  CanonicalDigestAlgorithm algorithm =
      CanonicalDigestAlgorithm::Sha256CanonicalV1;
  /** @brief Exact 256-bit digest bytes. */
  std::array<std::byte, kCanonicalDigestBytes> bytes{};

  /**
   * @brief Compares complete typed Layout digest state.
   * @param other Digest to compare.
   * @return True when algorithm and all bytes match.
   * @throws Nothing.
   */
  bool operator==(const StorageLayoutDigest& other) const noexcept {
    return algorithm == other.algorithm && bytes == other.bytes;
  }
};

/**
 * @brief Strongly typed canonical logical-content identity.
 * @throws Nothing for ordinary value operations.
 */
struct ContentDigest final {
  /** @brief Explicit approved algorithm and stream version. */
  CanonicalDigestAlgorithm algorithm =
      CanonicalDigestAlgorithm::Sha256CanonicalV1;
  /** @brief Exact 256-bit digest bytes. */
  std::array<std::byte, kCanonicalDigestBytes> bytes{};

  /**
   * @brief Compares complete typed content digest state.
   * @param other Digest to compare.
   * @return True when algorithm and all bytes match.
   * @throws Nothing.
   */
  bool operator==(const ContentDigest& other) const noexcept {
    return algorithm == other.algorithm && bytes == other.bytes;
  }
};

/**
 * @brief Typed availability state for explicit content traversal.
 * @throws Nothing for ordinary enum operations.
 */
enum class ContentDigestState {
  /** @brief Canonical logical content was traversed and hashed. */
  Available,
  /** @brief No active or retained provider can interpret the content. */
  MissingProvider,
  /** @brief A typed identity exists only at another structural version. */
  UnsupportedSchemaVersion,
  /** @brief Descriptor, Layout, or callback framing is invalid. */
  InvalidDescriptor,
  /** @brief Required Ready host-readable payload cannot be acquired. */
  PayloadUnavailable,
  /** @brief Provider validation or canonical traversal failed. */
  ProviderFailure,
};

/**
 * @brief Owned explicit content-digest outcome.
 * @throws std::bad_alloc when diagnostic storage cannot allocate.
 * @note Only Available carries a digest.
 */
struct ContentDigestResult final {
  /** @brief Exact availability or failure state. */
  ContentDigestState state = ContentDigestState::InvalidDescriptor;
  /** @brief Canonical logical digest only for Available. */
  std::optional<ContentDigest> digest;
  /** @brief Host-owned provider/readiness diagnostic. */
  std::string diagnostic;
};

/**
 * @brief Provider-independent artifact/cache metadata envelope.
 *
 * @throws std::bad_alloc when copied envelope storage cannot allocate.
 * @note Runtime bindings, allocation/revision identities, fences, leases,
 * provider objects, native handles, and device identities are absent.
 */
struct ExtensionArtifactEnvelope final {
  /** @brief Byte-preserving logical descriptor. */
  DataDescriptorEnvelope descriptor;
  /** @brief Byte-preserving provider-defined Layout metadata. */
  ProviderDefinedLayout layout;
  /** @brief Optional canonical descriptor digest. */
  std::optional<DescriptorDigest> descriptor_digest;
  /** @brief Optional canonical logical-content digest. */
  std::optional<ContentDigest> content_digest;
  /** @brief Optional canonical storage-Layout digest. */
  std::optional<StorageLayoutDigest> storage_layout_digest;

  /**
   * @brief Compares every persisted metadata fact.
   * @param other Envelope to compare.
   * @return True when preserved records and optional digests match.
   * @throws Nothing under supported equality operations.
   */
  bool operator==(const ExtensionArtifactEnvelope& other) const noexcept {
    return descriptor == other.descriptor && layout == other.layout &&
           descriptor_digest == other.descriptor_digest &&
           content_digest == other.content_digest &&
           storage_layout_digest == other.storage_layout_digest;
  }
};

/**
 * @brief Validates one provider-independent descriptor envelope.
 * @param descriptor Candidate Schema and ordered Facets.
 * @throws ExtensionContractError for invalid kinds, identities, versions,
 * duplicate Facets, or hard-limit violations.
 * @throws std::bad_alloc only if validation storage cannot allocate.
 * @note No registry lookup, provider callback, payload access, or digest work
 * occurs.
 */
void validate_data_descriptor_envelope(
    const DataDescriptorEnvelope& descriptor);

/**
 * @brief Validates one provider-independent Layout metadata envelope.
 * @param layout Candidate Layout record and buffer references.
 * @param buffer_sizes Exact positive checked BufferHandle range sizes.
 * @throws ExtensionContractError for invalid kind, identity, version, limits,
 * references, ranges, overflow, or forbidden overlap.
 * @throws std::bad_alloc when bounded validation storage cannot allocate.
 * @note No provider callback or payload access occurs.
 */
void validate_provider_defined_layout(
    const ProviderDefinedLayout& layout,
    const std::vector<std::size_t>& buffer_sizes);

/**
 * @brief Computes canonical descriptor identity without provider access.
 * @param descriptor Valid byte-preserving descriptor envelope.
 * @return Typed SHA-256 canonical-v1 descriptor digest.
 * @throws ExtensionContractError when the envelope is invalid.
 * @throws std::bad_alloc when bounded traversal staging cannot allocate.
 * @note Unknown payload bytes participate exactly; Facet order is canonical.
 */
DescriptorDigest compute_descriptor_digest(
    const DataDescriptorEnvelope& descriptor);

/**
 * @brief Computes canonical provider-Layout identity without provider access.
 * @param layout Valid byte-preserving Layout metadata.
 * @return Typed SHA-256 canonical-v1 Layout digest.
 * @throws ExtensionContractError when Layout framing is invalid.
 * @throws std::bad_alloc when bounded traversal staging cannot allocate.
 * @note Allocation identities, native handles, devices, fences, and payload
 * are excluded.
 */
StorageLayoutDigest compute_storage_layout_digest(
    const ProviderDefinedLayout& layout);

/**
 * @brief Computes canonical logical content through the Value generation.
 * @param value Valid provider-defined Value.
 * @return Typed availability and optional ContentDigest.
 * @throws std::bad_alloc when bounded metadata or fixed digest state cannot
 * allocate.
 * @note The operation is explicit and may read Ready host-visible payload;
 * property, Region, and DataSpec query paths never call it implicitly. The
 * Host measures then incrementally hashes the deterministic provider stream
 * without retaining payload-proportional storage or imposing a cumulative
 * 64 MiB logical-content limit.
 */
ContentDigestResult compute_content_digest(const Value& value);

/**
 * @brief Serializes one dependency-neutral artifact/cache metadata envelope.
 * @param envelope Preserved descriptor/Layout and optional typed digests.
 * @return Versioned bounded binary metadata bytes.
 * @throws ExtensionContractError for invalid records or maximum-size overflow.
 * @throws std::bad_alloc when output storage cannot allocate.
 * @note Encoding requires no provider and preserves Facet order and all
 * extension bytes exactly.
 */
std::vector<std::byte> encode_extension_artifact(
    const ExtensionArtifactEnvelope& envelope);

/**
 * @brief Transactionally decodes provider-independent artifact metadata.
 * @param bytes Complete versioned bounded binary envelope.
 * @return Fully owned byte-preserving metadata.
 * @throws ExtensionContractError for malformed, truncated, noncanonical, or
 * oversized framing.
 * @throws std::bad_alloc when owned metadata cannot allocate.
 * @note Decode performs no registry lookup or provider interpretation.
 */
ExtensionArtifactEnvelope decode_extension_artifact(
    const std::vector<std::byte>& bytes);

}  // namespace ps
