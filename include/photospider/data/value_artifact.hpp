#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "photospider/data/extension.hpp"
#include "photospider/data/value.hpp"

/**
 * @file value_artifact.hpp
 * @brief Versioned portable Value metadata, payload, and reconstruction.
 */

namespace ps {

/** @brief Portable Value artifact envelope structural version. */
inline constexpr std::uint32_t kValueArtifactEnvelopeVersion = 1U;

/** @brief Canonical named-Value artifact-set archive structural version. */
inline constexpr std::uint32_t kNamedValueArtifactSetArchiveVersion = 1U;

/** @brief Frozen maximum serialized Value artifact metadata bytes. */
inline constexpr std::size_t kMaximumValueArtifactMetadataBytes =
    16U * 1024U * 1024U;  // NOLINT(whitespace/indent_namespace)

/** @brief Frozen maximum portable Host output-name byte length. */
inline constexpr std::size_t kMaximumValueArtifactNameBytes = 128U;

/** @brief Frozen maximum aggregate payload bytes accepted by one artifact. */
inline constexpr std::uint64_t kMaximumValueArtifactPayloadBytes =
    8ULL * 1024ULL * 1024ULL * 1024ULL;  // NOLINT

/** @brief Frozen maximum portable payload reconstruction alignment. */
inline constexpr std::uint64_t kMaximumValueArtifactAlignment = 4096U;

/** @brief Frozen maximum named Values retained by one portable artifact set. */
inline constexpr std::size_t kMaximumNamedValueArtifacts = 4096U;

/** @brief Frozen maximum statistics references carried by one named Value. */
inline constexpr std::size_t kMaximumValueArtifactStatisticsReferences = 128U;

/** @brief Frozen maximum bytes in one portable artifact diagnostic string. */
inline constexpr std::size_t kMaximumValueArtifactStringBytes = 4096U;

/**
 * @brief SHA-256 identity of one exact portable payload buffer.
 * @throws Nothing for ordinary value operations.
 */
struct ArtifactPayloadDigest final {
  /** @brief Exact digest bytes in conventional network order. */
  std::array<std::byte, 32U> bytes{};

  /** @brief Compares all digest bytes. */
  bool operator==(const ArtifactPayloadDigest& other) const noexcept {
    return bytes == other.bytes;
  }
};

/**
 * @brief Portable metadata for one ordered immutable artifact payload.
 * @throws Nothing for ordinary value operations.
 * @note No allocation, device, path, handle, mapping, or lease identity exists.
 */
struct ValueArtifactBuffer final {
  /** @brief Dense zero-based payload index. */
  std::uint32_t index = 0U;
  /** @brief Provider logical role, or zero for a built-in DenseTensor buffer.
   */
  std::uint32_t logical_role = 0U;
  /** @brief Checked absolute payload offset in the enclosing artifact set. */
  std::uint64_t artifact_offset = 0U;
  /** @brief Exact positive payload byte length. */
  std::uint64_t byte_size = 0U;
  /** @brief Positive power-of-two alignment required during reconstruction. */
  std::uint64_t required_alignment = 1U;
  /** @brief Exact SHA-256 payload identity. */
  ArtifactPayloadDigest digest;

  /** @brief Compares complete portable buffer metadata. */
  bool operator==(const ValueArtifactBuffer& other) const noexcept {
    return index == other.index && logical_role == other.logical_role &&
           artifact_offset == other.artifact_offset &&
           byte_size == other.byte_size &&
           required_alignment == other.required_alignment &&
           digest == other.digest;
  }
};

/**
 * @brief Optional durable-owner joins carried without path or runtime identity.
 * @throws std::bad_alloc when copied text storage cannot allocate.
 * @note Empty fields mean the portable Value is not joined to that owner.
 */
struct ValueArtifactJoin final {
  /** @brief Stable owner-defined artifact identity. */
  std::optional<std::string> artifact_identity;
  /** @brief Stable owner-defined commit identity. */
  std::optional<std::string> commit_identity;
  /** @brief Stable owner-defined output-slot identity. */
  std::optional<std::string> slot_identity;
};

/**
 * @brief Identity-independent reference to a separately committed statistic.
 * @throws std::bad_alloc when copied text storage cannot allocate.
 * @note The record never contains a ValueRevisionId or process-local cache key.
 */
struct ValueArtifactStatisticsReference final {
  /** @brief Exact logical content identity on which the statistic was run. */
  ContentDigest content_digest;
  /** @brief Canonical normalized query identity. */
  ArtifactPayloadDigest query_digest;
  /** @brief Bounded stable algorithm name. */
  std::string algorithm;
  /** @brief Nonzero algorithm contract version. */
  std::uint32_t algorithm_version = 0U;
  /** @brief Distinct owner-defined statistics artifact identity. */
  std::string artifact_identity;
};

/**
 * @brief Dependency-neutral portable metadata for one immutable Value.
 *
 * @throws std::bad_alloc when copied descriptors, Facets, Layouts, or buffer
 *         records cannot allocate.
 * @note Runtime allocation/revision/producer/fence/device/provider-generation,
 *       native-handle, mapping, lease, path, quota, and publication authority
 *       are deliberately absent.
 */
struct ValueArtifactEnvelope final {
  /** @brief Exact envelope structural version. */
  std::uint32_t structural_version = kValueArtifactEnvelopeVersion;
  /** @brief Exact bounded canonical Host output name. */
  std::string output_name;
  /** @brief Built-in or provider-defined representation family. */
  ValueRepresentationKind representation = ValueRepresentationKind::DenseTensor;
  /** @brief Strided, Blocked, or provider-defined layout family. */
  StorageLayoutKind layout_kind = StorageLayoutKind::Strided;
  /** @brief Built-in descriptor only for DenseTensor. */
  std::optional<DenseTensorDescriptor> dense_descriptor;
  /** @brief Optional complete ordinary-image interpretation. */
  std::optional<ImageFacet> image_facet;
  /** @brief Strided Layout only for Strided DenseTensor. */
  std::optional<StridedLayout> strided_layout;
  /** @brief Blocked Layout only for Blocked DenseTensor. */
  std::optional<BlockedLayout> blocked_layout;
  /** @brief Provider descriptor only for provider-defined Values. */
  std::optional<DataDescriptorEnvelope> provider_descriptor;
  /** @brief Provider Layout only for provider-defined Values. */
  std::optional<ProviderDefinedLayout> provider_layout;
  /** @brief Optional canonical logical content identity. */
  std::optional<ContentDigest> content_digest;
  /** @brief Canonical logical descriptor identity. */
  DescriptorDigest descriptor_digest;
  /** @brief Canonical physical Layout identity. */
  StorageLayoutDigest storage_layout_digest;
  /** @brief Optional durable-owner identity joins. */
  ValueArtifactJoin joins;
  /** @brief Bounded references to separately committed statistics artifacts. */
  std::vector<ValueArtifactStatisticsReference> statistics_references;
  /** @brief Exact dense ordered payload metadata. */
  std::vector<ValueArtifactBuffer> buffers;
};

/**
 * @brief Complete in-memory portable artifact with owned immutable bytes.
 * @throws std::bad_alloc when metadata or payload copying cannot allocate.
 * @note Payload vector order must exactly match envelope buffer indices.
 */
struct ValueArtifact final {
  /** @brief Complete payload-independent metadata. */
  ValueArtifactEnvelope envelope;
  /** @brief Exact owned bytes for each ordered buffer. */
  std::vector<std::vector<std::byte>> payloads;
};

/**
 * @brief Complete canonical ordered artifact set for one Host result.
 * @throws std::bad_alloc when artifact ownership cannot allocate.
 * @note Artifact output names are strictly increasing and unique.
 */
struct NamedValueArtifactSet final {
  /** @brief Exact canonically named artifacts. */
  std::vector<ValueArtifact> values;
};

/**
 * @brief Computes SHA-256 over one exact artifact or payload byte sequence.
 * @param bytes Complete immutable bytes, including an allowed empty sequence.
 * @return Exact conventional SHA-256 digest.
 * @throws std::bad_alloc or std::runtime_error when OpenSSL cannot hash.
 * @note The helper assigns no semantic meaning and reads no Value metadata.
 */
ArtifactPayloadDigest compute_artifact_payload_digest(
    const std::vector<std::byte>& bytes);

/**
 * @brief Captures one Ready host-readable Value as a portable artifact.
 * @param output_name Exact bounded canonical result name.
 * @param value Valid built-in or provider-defined Value.
 * @return Complete envelope and exact ordered payload bytes.
 * @throws std::invalid_argument for an invalid Value.
 * @throws ReadyFenceAccessError, BufferAccessError, or ExtensionContractError
 *         when payload access or provider content validation is unavailable.
 * @throws std::overflow_error or std::length_error when frozen bounds fail.
 * @throws std::bad_alloc when owned artifact storage cannot allocate.
 * @note Capture retains no runtime identity and reads bytes only through exact
 *       Ready host leases retained for each synchronous copy. Each retained
 *       range's physical alignment guarantee becomes its portable
 *       reconstruction requirement.
 */
ValueArtifact capture_value_artifact(std::string output_name,
                                     const Value& value);

/**
 * @brief Validates complete portable metadata and payloads without publishing.
 * @param artifact Candidate artifact.
 * @return Nothing after version, shape/Layout, role/index, size/digest,
 * content, and aggregate-bound validation succeeds.
 * @throws std::invalid_argument, std::overflow_error, std::length_error, or
 *         ExtensionContractError for malformed facts.
 * @throws std::bad_alloc when bounded validation cannot allocate.
 * @note Provider semantic validation is deferred to reconstruction because it
 *       requires an injected registry generation.
 */
void validate_value_artifact(const ValueArtifact& artifact);

/**
 * @brief Transactionally reconstructs one fresh local Value.
 * @param artifact Fully owned portable metadata and payloads.
 * @param registry Provider registry required only for provider-defined input;
 *        it may be null for built-in artifacts.
 * @return Fresh Ready CPU Value with new allocation, producer, fence, binding,
 *         provider-owner, and Value revision identities.
 * @throws All validation and local Value publication failures unchanged.
 * @note No Value escapes unless framing, payload digests, local built-in or
 *       provider validation, and optional canonical content identity all agree.
 */
Value reconstruct_value_artifact(const ValueArtifact& artifact,
                                 DataDefinitionRegistry* registry = nullptr);

/**
 * @brief Encodes one payload-independent envelope in canonical binary form.
 * @param envelope Valid portable metadata.
 * @return Exact version-one bounded bytes.
 * @throws std::invalid_argument, std::overflow_error, std::length_error, or
 *         ExtensionContractError for invalid metadata.
 * @throws std::bad_alloc when output storage cannot allocate.
 * @note Payload bytes and all runtime/path/authority identities are absent.
 */
std::vector<std::byte> encode_value_artifact_envelope(
    const ValueArtifactEnvelope& envelope);

/**
 * @brief Transactionally decodes one canonical portable metadata envelope.
 * @param bytes Complete encoded metadata without trailing bytes.
 * @return Fully owned version-one envelope.
 * @throws std::invalid_argument, std::overflow_error, std::length_error, or
 *         ExtensionContractError for malformed/noncanonical framing.
 * @throws std::bad_alloc when decoded ownership cannot allocate.
 * @note Decode publishes no Value and grants no payload, path, or lease access.
 */
ValueArtifactEnvelope decode_value_artifact_envelope(
    const std::vector<std::byte>& bytes);

/**
 * @brief Encodes one complete named artifact set into a bounded binary archive.
 * @param artifacts Valid canonical set with exact payload bytes.
 * @return Metadata-first archive with separately addressed aligned payloads.
 * @throws Artifact validation, overflow, length, digest, or allocation errors.
 * @note Payload spans are outside the encoded envelope records; no JSON,
 *       native handle, path, lease, or process-local identity is serialized.
 */
std::vector<std::byte> encode_named_value_artifact_set(
    const NamedValueArtifactSet& artifacts);

/**
 * @brief Decodes one complete named artifact archive transactionally.
 * @param bytes Exact archive bytes without trailing content.
 * @return Detached canonical artifacts after every span/digest validates.
 * @throws Artifact validation, overflow, length, or allocation errors.
 * @note Decoding publishes no Value and grants no durable/cache authority.
 */
NamedValueArtifactSet decode_named_value_artifact_set(
    const std::vector<std::byte>& bytes);

}  // namespace ps
