#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/core/device.hpp"
#include "photospider/data/value_artifact.hpp"

/**
 * @file value_result.hpp
 * @brief Owned named-Value Host results and payload-free inspection records.
 */

namespace ps {

/** @brief Frozen maximum named Values returned by one Host compute request. */
inline constexpr std::size_t kMaximumHostResultValues = 4096U;

/** @brief Frozen maximum byte length of one Host result output name. */
inline constexpr std::size_t kMaximumHostResultNameBytes = 128U;

/**
 * @brief Payload-free physical binding facts for one Value buffer.
 * @throws Nothing for ordinary value operations.
 * @note Allocation, mapping, native-handle, pointer, lease, and fence identity
 *       are deliberately absent. These copied facts grant no payload access.
 */
struct ValueBufferInspection final {
  /** @brief Dense zero-based buffer position. */
  std::size_t buffer_index = 0U;
  /** @brief Concrete local backend family. */
  DeviceBackend backend = DeviceBackend::CPU;
  /** @brief Explicit memory domain of the current local binding. */
  MemoryDomain memory_domain = MemoryDomain::Host;
  /** @brief Exact checked byte-envelope length. */
  std::size_t byte_size = 0U;
  /** @brief Whether an explicit host read lease can be acquired after Ready. */
  bool host_visible = false;
};

/**
 * @brief Owned payload-free inspection snapshot for one named Value.
 *
 * @throws std::bad_alloc when copied names, descriptors, Facets, Layouts,
 *         extension records, buffers, or diagnostics cannot allocate.
 * @note Inspection never polls by waiting, maps storage, creates a lease,
 *       invokes a provider, reads payload bytes, or changes readiness.
 */
struct NamedValueInspection final {
  /** @brief Exact bounded output name. */
  std::string name;
  /** @brief Built-in or provider-defined representation family. */
  ValueRepresentationKind representation = ValueRepresentationKind::DenseTensor;
  /** @brief Strided, Blocked, or provider-defined physical layout family. */
  StorageLayoutKind layout_kind = StorageLayoutKind::Strided;
  /** @brief DenseTensor descriptor only for a built-in representation. */
  std::optional<DenseTensorDescriptor> dense_descriptor;
  /** @brief Complete ordinary-image interpretation when present. */
  std::optional<ImageFacet> image_facet;
  /** @brief Strided Layout only for a Strided built-in representation. */
  std::optional<StridedLayout> strided_layout;
  /** @brief Blocked Layout only for a Blocked built-in representation. */
  std::optional<BlockedLayout> blocked_layout;
  /** @brief Provider descriptor only for a provider-defined representation. */
  std::optional<DataDescriptorEnvelope> provider_descriptor;
  /** @brief Provider Layout only for a provider-defined representation. */
  std::optional<ProviderDefinedLayout> provider_layout;
  /** @brief One copied record for every retained Value buffer. */
  std::vector<ValueBufferInspection> buffers;
  /** @brief Process-local immutable publication revision for diagnostics. */
  ValueRevisionId revision;
  /** @brief Process-local producer/transfer identity for diagnostics. */
  ProducerIdentity producer;
  /** @brief Available canonical descriptor identity without recomputation. */
  std::optional<DescriptorDigest> descriptor_digest;
  /** @brief Available canonical logical-content identity without recomputation.
   */
  std::optional<ContentDigest> content_digest;
  /** @brief Available canonical physical-layout identity without recomputation.
   */
  std::optional<StorageLayoutDigest> storage_layout_digest;
  /** @brief Bounded unresolved references to separate statistics artifacts. */
  std::vector<ValueArtifactStatisticsReference> statistics_references;
  /** @brief Nonblocking producer-completion observation. */
  ReadyFenceState readiness = ReadyFenceState::Pending;
  /** @brief Owned typed failure only when readiness is Failed. */
  std::optional<ReadyFenceFailure> failure;
};

/**
 * @brief Optional portable observation facts retained beside one exact Value.
 * @throws std::bad_alloc when statistics-reference ownership allocates.
 * @note These facts grant no payload, artifact, cache, mapping, or statistics
 *       resolution authority. Empty digest fields mean no prior boundary made
 *       that canonical fact available without new work.
 */
struct NamedValuePortableMetadata final {
  /** @brief Available canonical descriptor digest. */
  std::optional<DescriptorDigest> descriptor_digest;
  /** @brief Available canonical logical-content digest. */
  std::optional<ContentDigest> content_digest;
  /** @brief Available canonical storage-layout digest. */
  std::optional<StorageLayoutDigest> storage_layout_digest;
  /** @brief Bounded identity-independent statistics references. */
  std::vector<ValueArtifactStatisticsReference> statistics_references;
};

/**
 * @brief Exact owned named Value returned by a Host compute-result operation.
 * @throws std::bad_alloc when copied name or Value metadata ownership
 * allocates.
 * @note The Value is the sole payload, binding, readiness, allocation, and
 *       revision authority. The name is not inferred from descriptor metadata.
 */
struct NamedValue final {
  /**
   * @brief Creates an invalid transport sentinel for staged reconstruction.
   * @throws Nothing.
   * @note `NamedValueResult` rejects this state; it exists only for local
   *       transaction assembly before all fields are assigned.
   */
  NamedValue() noexcept = default;

  /**
   * @brief Owns one exact name, Value, and already-available portable facts.
   * @param output_name Exact result name validated by NamedValueResult.
   * @param output_value Exact immutable Value retained by the result.
   * @param metadata Optional copied digest/statistics observations.
   * @throws Nothing for movement of already-owned fields.
   * @note Construction performs no validation, wait, payload access, digest,
   *       provider call, or statistics resolution; NamedValueResult validates
   *       the complete collection transactionally.
   */
  NamedValue(std::string output_name, Value output_value,
             NamedValuePortableMetadata metadata = {}) noexcept
      : name(std::move(output_name)),
        value(std::move(output_value)),
        portable_metadata(std::move(metadata)) {}

  /** @brief Exact bounded output name. */
  std::string name;
  /** @brief Exact immutable Value retained for the result lifetime. */
  Value value;
  /** @brief Optional already-available portable observation facts. */
  NamedValuePortableMetadata portable_metadata;
};

/**
 * @brief Owned canonically ordered result of one Host Value compute.
 *
 * Construction validates bounded strictly increasing non-NUL names, valid
 * Values, and optionally terminal Ready state before publishing the collection.
 *
 * @throws std::invalid_argument for malformed ordering, names, Values, or
 *         non-Ready values when terminal readiness is required.
 * @throws std::length_error when the frozen output-count bound is exceeded.
 * @throws std::bad_alloc when owned result storage cannot allocate.
 * @note Empty is a successful exact result for a node declaring no Value
 *       outputs. Copies retain the exact Value PImpl identities.
 */
class NamedValueResult final {
 public:
  /** @brief Creates one valid empty result. @throws Nothing. */
  NamedValueResult() noexcept = default;

  /**
   * @brief Validates and owns one exact named Value collection.
   * @param values Strictly increasing exact output records.
   * @param require_ready Whether every Value must currently be Ready.
   * @throws std::invalid_argument, std::length_error, or std::bad_alloc as
   *         described by the class contract.
   * @note Validation finishes before replacing constructor-local ownership.
   */
  explicit NamedValueResult(std::vector<NamedValue> values,
                            bool require_ready = true);

  /**
   * @brief Returns the exact immutable named Value collection.
   * @return Borrowed canonically ordered records.
   * @throws Nothing.
   */
  const std::vector<NamedValue>& values() const noexcept { return values_; }

  /**
   * @brief Finds one exact named Value without changing result ownership.
   * @param name Exact output name.
   * @return Borrowed Value pointer, or null when absent.
   * @throws Nothing.
   */
  const Value* find(const std::string& name) const noexcept;

  /**
   * @brief Creates bounded payload-free inspection for every named Value.
   * @return Owned snapshots in the same canonical name order.
   * @throws std::bad_alloc when copied metadata cannot allocate.
   * @note The method performs no wait, payload read, mapping, or provider call.
   */
  std::vector<NamedValueInspection> inspect() const;

 private:
  /** @brief Exact canonically ordered owned outputs. */
  std::vector<NamedValue> values_;
};

}  // namespace ps
