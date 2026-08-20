#include "graph/graph_cache_service.hpp"

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
#include <atomic>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/value_region.hpp"
#include "execution/device/compute_io_executor.hpp"
#include "graph/graph_traversal_service.hpp"
#include "photospider/core/graph_error.hpp"
#include "photospider/data/image_view.hpp"
#include "photospider/data/value_artifact.hpp"
#include "photospider/plugin/data_definition_registry.hpp"
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
#include "graph/graph_cache_service_test_access.hpp"  // NOLINT(build/include_subdir)
#endif

namespace ps {

namespace {

using DiskCacheLoadStatus = GraphModel::DiskCacheLoadStatus;

/**
 * @brief Holds one disk-cache read attempt plus the loaded output, if any.
 *
 * The public diagnostic record is stored separately from the NodeOutput so
 * GraphModel can retain lightweight inspectable state without owning decoded
 * image payloads for failed or historical attempts.
 *
 * @throws std::bad_alloc when contained diagnostic or output state allocates.
 * @note Instances are request-local and moved between read helpers; they own no
 * Graph, codec, filesystem handle, or worker lifetime.
 */
struct DiskCacheReadAttempt {
  /** @brief Lightweight diagnostic published after this attempt settles. */
  GraphModel::DiskCacheLoadResult result;
  /** @brief Detached candidate populated only for successful reusable hits. */
  NodeOutput output;
  /** @brief Whether final multi-entry handling must retain this miss reason. */
  bool preserve_miss_diagnostic = false;
};

/** @brief Private graph-cache transaction manifest structural version. */
constexpr std::uint32_t kGraphCacheManifestVersion = 1U;

/** @brief Bit indicating one digest-bound parameter metadata payload. */
constexpr std::uint32_t kGraphCacheManifestHasMetadata = 1U;

/** @brief Exact private graph-cache manifest magic. */
constexpr std::array<std::byte, 8U> kGraphCacheManifestMagic{
    std::byte{'P'}, std::byte{'S'}, std::byte{'C'},
    std::byte{'A'}, std::byte{'C'}, std::byte{'H'},
    std::byte{'E'}, std::byte{'1'}};  // NOLINT(whitespace/indent_namespace)

/** @brief Frozen maximum encoded parameter metadata bytes per cache entry. */
constexpr auto kMaximumGraphCacheMetadataBytes = 16ULL * 1024ULL * 1024ULL;

/**
 * @brief Identifies one exact immutable cache transaction payload file.
 * @throws Nothing for aggregate construction and comparison.
 * @note The record contains content facts only and grants no path authority.
 */
struct CacheArtifactFileRecord final {
  /** @brief Exact positive byte size, or zero for an absent optional file. */
  std::uint64_t byte_size = 0U;
  /** @brief SHA-256 over the complete exact file bytes. */
  ArtifactPayloadDigest digest;
};

/**
 * @brief Versioned commit record for one graph-cache replay transaction.
 * @throws Nothing for ordinary aggregate construction.
 * @note The configured cache path remains the cache-key authority. This record
 * binds one complete public named-Value archive and optional parameter bytes;
 * the image-codec projection remains an auxiliary policy output and is never
 * used to reconstruct runtime Value authority.
 */
struct GraphCacheManifest final {
  /** @brief Exact private manifest version. */
  std::uint32_t structural_version = kGraphCacheManifestVersion;
  /** @brief Exact public named-Value archive version. */
  std::uint32_t archive_version = kNamedValueArtifactSetArchiveVersion;
  /** @brief Closed optional-field flags. */
  std::uint32_t flags = 0U;
  /** @brief Number of exact named Values in the archive. */
  std::uint32_t value_count = 0U;
  /** @brief Number of exact parameter outputs in the metadata payload. */
  std::uint32_t parameter_count = 0U;
  /** @brief Complete named-Value archive file identity. */
  CacheArtifactFileRecord archive;
  /** @brief Optional encoded parameter metadata file identity. */
  CacheArtifactFileRecord metadata;
};

/**
 * @brief Resolves every controlled sibling in one configured cache entry.
 * @throws std::bad_alloc when path construction cannot allocate.
 * @note Suffixes are appended rather than replacing the configured extension,
 * preserving the existing image projection and `.yml` metadata paths.
 */
struct GraphCacheArtifactPaths final {
  /** @brief Per-node directory containing the complete transaction. */
  fs::path directory;
  /** @brief Existing configured image-codec projection path. */
  fs::path image_projection;
  /** @brief Existing configured parameter metadata path. */
  fs::path metadata;
  /** @brief Canonical public named-Value archive path. */
  fs::path value_archive;
  /** @brief Versioned manifest written last. */
  fs::path manifest;
};

/**
 * @brief Derives one cache entry's controlled transaction paths.
 * @param graph Graph whose cache root anchors the entry.
 * @param node Node supplying the stable numeric cache namespace.
 * @param entry Configured nonempty image cache entry.
 * @return Complete deterministic sibling path set.
 * @throws std::bad_alloc when path construction cannot allocate.
 */
GraphCacheArtifactPaths graph_cache_artifact_paths(const GraphModel& graph,
                                                   const Node& node,
                                                   const CacheEntry& entry) {
  GraphCacheArtifactPaths paths;
  paths.directory = graph.cache_root / std::to_string(node.id);
  paths.image_projection = paths.directory / entry.location;
  paths.metadata = paths.image_projection;
  paths.metadata.replace_extension(".yml");
  paths.value_archive = paths.image_projection;
  paths.value_archive += ".values";
  paths.manifest = paths.image_projection;
  paths.manifest += ".manifest";
  return paths;
}

/**
 * @brief Appends one canonical little-endian 32-bit scalar.
 * @param output Mutable encoded byte owner.
 * @param value Scalar to append.
 * @return Nothing.
 * @throws std::bad_alloc when output growth cannot allocate.
 */
void append_cache_u32(std::vector<std::byte>* output, std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    output->push_back(std::byte{static_cast<unsigned char>(value >> shift)});
  }
}

/**
 * @brief Appends one canonical little-endian 64-bit scalar.
 * @param output Mutable encoded byte owner.
 * @param value Scalar to append.
 * @return Nothing.
 * @throws std::bad_alloc when output growth cannot allocate.
 */
void append_cache_u64(std::vector<std::byte>* output, std::uint64_t value) {
  for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
    output->push_back(std::byte{static_cast<unsigned char>(value >> shift)});
  }
}

/**
 * @brief Reads one bounded canonical 32-bit manifest scalar.
 * @param bytes Complete manifest bytes.
 * @param offset Mutable next-byte offset.
 * @return Decoded scalar.
 * @throws GraphError with `InvalidParameter` on truncation.
 */
std::uint32_t read_cache_u32(const std::vector<std::byte>& bytes,
                             std::size_t* offset) {
  if (*offset > bytes.size() || bytes.size() - *offset < 4U) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Graph cache manifest is truncated.");
  }
  std::uint32_t value = 0U;
  for (unsigned int index = 0U; index < 4U; ++index) {
    value |= std::to_integer<std::uint32_t>(bytes[*offset + index])
             << (8U * index);
  }
  *offset += 4U;
  return value;
}

/**
 * @brief Reads one bounded canonical 64-bit manifest scalar.
 * @param bytes Complete manifest bytes.
 * @param offset Mutable next-byte offset.
 * @return Decoded scalar.
 * @throws GraphError with `InvalidParameter` on truncation.
 */
std::uint64_t read_cache_u64(const std::vector<std::byte>& bytes,
                             std::size_t* offset) {
  if (*offset > bytes.size() || bytes.size() - *offset < 8U) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Graph cache manifest is truncated.");
  }
  std::uint64_t value = 0U;
  for (unsigned int index = 0U; index < 8U; ++index) {
    value |= std::to_integer<std::uint64_t>(bytes[*offset + index])
             << (8U * index);
  }
  *offset += 8U;
  return value;
}

/**
 * @brief Requires one manifest's closed versions, flags, counts, and sizes.
 * @param manifest Candidate detached record.
 * @return Nothing after complete validation.
 * @throws GraphError with `InvalidParameter` for malformed facts.
 */
void validate_graph_cache_manifest(const GraphCacheManifest& manifest) {
  const bool has_metadata =
      (manifest.flags & kGraphCacheManifestHasMetadata) != 0U;
  const ArtifactPayloadDigest empty_digest;
  if (manifest.structural_version != kGraphCacheManifestVersion ||
      manifest.archive_version != kNamedValueArtifactSetArchiveVersion ||
      (manifest.flags & ~kGraphCacheManifestHasMetadata) != 0U ||
      manifest.value_count > kMaximumNamedValueArtifacts ||
      manifest.parameter_count > kMaximumNamedValueArtifacts ||
      manifest.archive.byte_size == 0U ||
      manifest.archive.byte_size >
          kMaximumValueArtifactPayloadBytes +
              static_cast<std::uint64_t>(kMaximumValueArtifactMetadataBytes) ||
      (has_metadata &&
       (manifest.parameter_count == 0U || manifest.metadata.byte_size == 0U ||
        manifest.metadata.byte_size > kMaximumGraphCacheMetadataBytes)) ||
      (!has_metadata &&
       (manifest.parameter_count != 0U || manifest.metadata.byte_size != 0U ||
        !(manifest.metadata.digest == empty_digest)))) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Graph cache manifest facts are invalid.");
  }
}

/**
 * @brief Encodes one validated graph-cache manifest canonically.
 * @param manifest Complete detached transaction record.
 * @return Exact fixed-length version-one bytes.
 * @throws GraphError for malformed facts and std::bad_alloc for allocation.
 */
std::vector<std::byte> encode_graph_cache_manifest(
    const GraphCacheManifest& manifest) {
  validate_graph_cache_manifest(manifest);
  std::vector<std::byte> bytes;
  bytes.reserve(8U + 5U * 4U + 2U * 8U + 2U * 32U);
  bytes.insert(bytes.end(), kGraphCacheManifestMagic.begin(),
               kGraphCacheManifestMagic.end());
  append_cache_u32(&bytes, manifest.structural_version);
  append_cache_u32(&bytes, manifest.archive_version);
  append_cache_u32(&bytes, manifest.flags);
  append_cache_u32(&bytes, manifest.value_count);
  append_cache_u32(&bytes, manifest.parameter_count);
  append_cache_u64(&bytes, manifest.archive.byte_size);
  append_cache_u64(&bytes, manifest.metadata.byte_size);
  bytes.insert(bytes.end(), manifest.archive.digest.bytes.begin(),
               manifest.archive.digest.bytes.end());
  bytes.insert(bytes.end(), manifest.metadata.digest.bytes.begin(),
               manifest.metadata.digest.bytes.end());
  return bytes;
}

/**
 * @brief Decodes one exact versioned graph-cache manifest.
 * @param bytes Complete fixed-length manifest bytes.
 * @return Detached validated transaction record.
 * @throws GraphError with `InvalidParameter` for malformed framing or facts.
 */
GraphCacheManifest decode_graph_cache_manifest(
    const std::vector<std::byte>& bytes) {
  constexpr std::size_t kEncodedSize = 8U + 5U * 4U + 2U * 8U + 2U * 32U;
  if (bytes.size() != kEncodedSize ||
      !std::equal(kGraphCacheManifestMagic.begin(),
                  kGraphCacheManifestMagic.end(), bytes.begin())) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Graph cache manifest framing is invalid.");
  }
  std::size_t offset = kGraphCacheManifestMagic.size();
  GraphCacheManifest manifest;
  manifest.structural_version = read_cache_u32(bytes, &offset);
  manifest.archive_version = read_cache_u32(bytes, &offset);
  manifest.flags = read_cache_u32(bytes, &offset);
  manifest.value_count = read_cache_u32(bytes, &offset);
  manifest.parameter_count = read_cache_u32(bytes, &offset);
  manifest.archive.byte_size = read_cache_u64(bytes, &offset);
  manifest.metadata.byte_size = read_cache_u64(bytes, &offset);
  std::memcpy(manifest.archive.digest.bytes.data(), bytes.data() + offset,
              manifest.archive.digest.bytes.size());
  offset += manifest.archive.digest.bytes.size();
  std::memcpy(manifest.metadata.digest.bytes.data(), bytes.data() + offset,
              manifest.metadata.digest.bytes.size());
  offset += manifest.metadata.digest.bytes.size();
  if (offset != bytes.size()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Graph cache manifest has trailing bytes.");
  }
  validate_graph_cache_manifest(manifest);
  return manifest;
}

/**
 * @brief Reads one exact bounded regular cache payload into detached bytes.
 * @param path Controlled cache sibling path.
 * @param maximum_bytes Frozen maximum accepted byte count.
 * @param expected_bytes Optional exact manifest-declared byte count.
 * @return Complete detached file bytes.
 * @throws Filesystem, stream, length, overflow, or allocation failures.
 * @note File size is checked before allocation and again after read. A racing
 * writer can therefore cause rejection but cannot publish partial bytes.
 */
std::vector<std::byte> read_cache_file_bytes(
    const fs::path& path, std::uint64_t maximum_bytes,
    std::optional<std::uint64_t> expected_bytes = std::nullopt) {
  const std::uintmax_t physical_size = fs::file_size(path);
  if (physical_size > maximum_bytes ||
      (expected_bytes.has_value() && physical_size != *expected_bytes) ||
      physical_size > std::numeric_limits<std::size_t>::max() ||
      physical_size > static_cast<std::uintmax_t>(
                          std::numeric_limits<std::streamsize>::max())) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Graph cache payload size is invalid.");
  }
  const std::size_t size = static_cast<std::size_t>(physical_size);
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    throw fs::filesystem_error("Could not open graph cache payload", path,
                               std::make_error_code(std::errc::io_error));
  }
  std::vector<std::byte> bytes(size);
  if (size != 0U) {
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(size));
  }
  if (!stream || fs::file_size(path) != physical_size) {
    throw GraphError(GraphErrc::Io,
                     "Graph cache payload changed during detached read.");
  }
  return bytes;
}

/**
 * @brief Writes one complete cache payload to its controlled final sibling.
 * @param path Existing-parent destination path.
 * @param bytes Complete bytes to truncate and write.
 * @return Nothing after checked close.
 * @throws std::runtime_error when open, write, or close fails.
 * @note Graph cache is discardable and retains its existing non-durable
 * failure semantics; the separate manifest remains the final hit authority.
 */
void write_cache_file_bytes(const fs::path& path,
                            const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.is_open()) {
    throw std::runtime_error("Could not open graph cache payload for writing.");
  }
  if (!bytes.empty()) {
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  stream.close();
  if (!stream) {
    throw std::runtime_error("Could not write complete graph cache payload.");
  }
}

/**
 * @brief Captures one exact detached file identity for a manifest.
 * @param path Existing cache payload path.
 * @param maximum_bytes Frozen maximum accepted payload size.
 * @return Exact size and SHA-256 content digest.
 * @throws File-read, hashing, or allocation exceptions unchanged.
 */
CacheArtifactFileRecord capture_cache_file_record(const fs::path& path,
                                                  std::uint64_t maximum_bytes) {
  const std::vector<std::byte> bytes =
      read_cache_file_bytes(path, maximum_bytes);
  return {static_cast<std::uint64_t>(bytes.size()),
          compute_artifact_payload_digest(bytes)};
}

/**
 * @brief Requires detached bytes to match one manifest-bound file record.
 * @param bytes Exact candidate file bytes.
 * @param record Manifest-declared size and digest.
 * @return Nothing after complete agreement.
 * @throws GraphError with `InvalidParameter` for mismatch.
 */
void validate_cache_file_record(const std::vector<std::byte>& bytes,
                                const CacheArtifactFileRecord& record) {
  if (static_cast<std::uint64_t>(bytes.size()) != record.byte_size ||
      !(compute_artifact_payload_digest(bytes) == record.digest)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Graph cache payload size or digest disagrees.");
  }
}

/**
 * @brief Builds the exact canonical Value-name set required by one plan.
 * @param schema Frozen output schema.
 * @return Strictly increasing complete names including optional `image`.
 * @throws std::invalid_argument for duplicate, reserved, empty, NUL, or
 * oversized generic names.
 * @throws std::bad_alloc when result ownership cannot allocate.
 */
std::vector<std::string> expected_cache_value_names(
    const ValueDiskCacheOutputSchema& schema) {
  std::vector<std::string> names = schema.generic_named_value_output_names;
  for (const std::string& name : names) {
    if (name.empty() || name == NodeOutput::kImageOutputName ||
        name.size() > NodeOutput::kMaximumNamedValueNameBytes ||
        name.find('\0') != std::string::npos) {
      throw std::invalid_argument(
          "Value disk cache schema contains an invalid generic name.");
    }
  }
  if (schema.canonical_image_planned) {
    names.emplace_back(NodeOutput::kImageOutputName);
  }
  std::sort(names.begin(), names.end());
  if (std::adjacent_find(names.begin(), names.end()) != names.end()) {
    throw std::invalid_argument(
        "Value disk cache schema contains duplicate Value names.");
  }
  return names;
}

/**
 * @brief Builds the explicit code-value endpoint selected by cache policy.
 * @param precision Exact `int8` or `int16` cache precision label.
 * @return Unsigned code-value endpoint with inclusive physical range.
 * @throws std::invalid_argument for every unknown label.
 */
SampleEndpoint cache_code_endpoint(const std::string& precision) {
  if (precision == "int8") {
    return SampleEndpoint{
        SampleEncoding{1U, SampleEncodingKind::CodeValue},
        SampleDomain{SampleDomainKind::CodeValue, 0.0, 255.0}};
  }
  if (precision == "int16") {
    return SampleEndpoint{
        SampleEncoding{1U, SampleEncodingKind::CodeValue},
        SampleDomain{SampleDomainKind::CodeValue, 0.0, 65535.0}};
  }
  throw std::invalid_argument("Unknown image cache precision: " + precision);
}

/**
 * @brief Builds explicit Value-to-cache code conversion from Value metadata.
 * @param value Ready ordinary image carrying one default sample endpoint.
 * @param precision Exact cache precision label.
 * @return Complete deterministic encode request.
 * @throws std::invalid_argument for missing/per-channel metadata or label.
 */
ImageArtifactEncodeRequest cache_encode_request(const Value& value,
                                                const std::string& precision) {
  if (!value.image_facet().has_value() ||
      !value.image_facet()->sample_domain.has_value() ||
      !value.image_facet()->sample_domain->per_channel.empty()) {
    throw std::invalid_argument(
        "Image cache encode requires one explicit default sample endpoint.");
  }
  const SampleDomainFacet& samples = *value.image_facet()->sample_domain;
  SampleConversion conversion;
  conversion.source = SampleEndpoint{samples.encoding, samples.default_domain};
  conversion.destination = cache_code_endpoint(precision);
  conversion.destination_element_semantics = ElementSemantics::UnsignedInteger;
  conversion.destination_storage_encoding =
      StorageEncoding{precision == "int16" ? 16U : 8U};
  conversion.out_of_domain = OutOfDomainPolicy::Reject;
  conversion.rounding = SampleRoundingMode::NearestEven;
  conversion.non_finite = NonFinitePolicy::Reject;
  conversion.precision_loss = PrecisionLossPolicy::Allow;
  return ImageArtifactEncodeRequest{conversion};
}

/**
 * @brief Returns a pointer to a node's formal HP cache when it exists.
 *
 * @param node Node whose reusable HP output should be inspected.
 * @return Pointer to cached HP output, or nullptr when no HP cache exists.
 * @throws Nothing.
 * @note RT state is intentionally ignored because disk cache authority is
 * limited to formal HP output.
 */
const NodeOutput* hp_cache_ptr(const Node& node) {
  if (node.cached_output_high_precision) {
    return &*node.cached_output_high_precision;
  }
  return nullptr;
}

/**
 * @brief Reports whether a node configures one executable image cache entry.
 *
 * @param node Node whose cache destinations are inspected without mutation.
 * @return True when at least one image entry has a nonempty location.
 * @throws Nothing for current string and vector read operations.
 * @note Unsupported or empty cache entries retain their historical no-op
 *       behavior and do not trigger Value validation.
 */
bool has_image_disk_cache_entry(const Node& node) {
  return std::any_of(
      node.caches.begin(), node.caches.end(), [](const CacheEntry& entry) {
        return entry.cache_type == "image" && !entry.location.empty();
      });
}

/**
 * @brief Fails closed when a canonical image cannot enter codec projection.
 *
 * @param output Formal HP output inspected without payload copying.
 * @throws GraphError with `InvalidParameter` for packed, quantized, latent,
 * non-host-readable, pending, or otherwise unsupported Value facts.
 * @throws std::bad_alloc when validation state or diagnostic allocation fails.
 * @note This helper performs no planned-byte admission, filesystem operation,
 * codec call, payload read, or persistent identity creation. Generic and
 * provider-defined non-image Values are validated by portable artifact capture
 * instead; metadata-only parameter results with no formal Value remain
 * supported.
 */
void validate_image_disk_cache_output(const NodeOutput& output) {
  if (!output.has_image_value()) {
    return;
  }
  try {
    (void)ImageView(output.image_value());
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        std::string("Image disk cache cannot persist formal Value: ") +
            error.what());
  }
}

/**
 * @brief Reports whether one formal HP output has complete exact validity.
 * @param node Node whose output and hp_region are inspected together.
 * @return True when both exist and hp_region covers the derived full output.
 * @throws std::logic_error, std::invalid_argument, std::overflow_error, or
 * std::bad_alloc when retained output facts cannot be validated.
 * @note Partial validity must not protect or produce a regionless disk cache
 * artifact because disk load initializes current artifacts as complete.
 */
bool has_complete_hp_cache(const Node& node) {
  const NodeOutput* output = hp_cache_ptr(node);
  return output != nullptr && node.hp_region.has_value() &&
         value_region::node_output_region_is_complete(*output, *node.hp_region);
}

/**
 * @brief Adds one value to a checked compute-I/O byte estimate.
 * @param total Mutable estimate accumulated so far.
 * @param value Additional byte count.
 * @return Nothing.
 * @throws GraphError with `ComputeError` when addition overflows.
 * @note This changes admission metadata only and allocates no payload.
 */
void add_planned_bytes(std::uint64_t& total, std::uint64_t value) {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) {
    throw GraphError(GraphErrc::ComputeError,
                     "Compute-I/O cache-save byte estimate overflowed.");
  }
  total += value;
}

/**
 * @brief Converts a native allocation size to the executor byte domain.
 * @param value Host-size byte count.
 * @return Exact unsigned 64-bit value.
 * @throws GraphError with `ComputeError` when the host size is wider.
 * @note The conversion performs no allocation or payload access.
 */
std::uint64_t planned_size(std::size_t value) {
  if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
    if (value >
        static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
      throw GraphError(GraphErrc::ComputeError,
                       "Compute-I/O cache-save size is unrepresentable.");
    }
  }
  return static_cast<std::uint64_t>(value);
}

/**
 * @brief Multiplies two checked compute-I/O estimate dimensions.
 * @param left First factor.
 * @param right Second factor.
 * @return Exact product.
 * @throws GraphError with `ComputeError` when multiplication overflows.
 * @note The result is admission metadata, not an allocation request.
 */
std::uint64_t multiply_planned_bytes(std::uint64_t left, std::uint64_t right) {
  if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
    throw GraphError(GraphErrc::ComputeError,
                     "Compute-I/O cache-save byte estimate overflowed.");
  }
  return left * right;
}

/**
 * @brief Adds recursively retained named-value content to an estimate.
 * @param total Mutable checked byte estimate.
 * @param value Named output value inspected without copying.
 * @return Nothing.
 * @throws GraphError with `ComputeError` when checked arithmetic overflows.
 * @throws ParameterTypeError if a corrupt kind/accessor contract is observed.
 * @note Fixed envelopes conservatively cover container/value bookkeeping;
 * string and key payloads are counted separately with null terminators.
 */
void add_parameter_value_planned_bytes(std::uint64_t& total,
                                       const plugin::ParameterValue& value) {
  add_planned_bytes(total, planned_size(sizeof(plugin::ParameterValue)));
  switch (value.kind()) {
    case plugin::ParameterKind::Null:
    case plugin::ParameterKind::Bool:
    case plugin::ParameterKind::Int64:
    case plugin::ParameterKind::Double:
      return;
    case plugin::ParameterKind::String:
      add_planned_bytes(total, planned_size(value.as_string().size()));
      add_planned_bytes(total, 1U);
      return;
    case plugin::ParameterKind::Array:
      for (const plugin::ParameterValue& child : value.as_array()) {
        add_parameter_value_planned_bytes(total, child);
      }
      return;
    case plugin::ParameterKind::Object:
      for (const auto& [key, child] : value.as_object()) {
        add_planned_bytes(total, planned_size(key.size()));
        add_planned_bytes(total, 1U);
        add_parameter_value_planned_bytes(total, child);
      }
      return;
  }
  throw GraphError(GraphErrc::ComputeError,
                   "Compute-I/O cache-save parameter kind is invalid.");
}

/**
 * @brief Detached immutable input for one complete cache-save mechanism.
 * @throws std::bad_alloc when archive, metadata, policy, or Value ownership
 * cannot allocate.
 * @note Preparation completes all portable Value capture and typed capability
 * validation before executor admission or filesystem work. Partial outputs
 * carry no archive and authorize stale-transaction cleanup only.
 */
struct PreparedGraphCacheSave final {
  /** @brief Whether exact hp_region proves the complete formal output. */
  bool complete_output = false;
  /** @brief Exact canonical public named-Value archive bytes. */
  std::vector<std::byte> value_archive;
  /** @brief Number of Values encoded into `value_archive`. */
  std::uint32_t value_count = 0U;
  /** @brief Detached parameter outputs written through the metadata codec. */
  plugin::ParameterMap parameters;
  /** @brief Optional exact canonical image retained for codec projection. */
  std::optional<Value> image_projection;
  /** @brief Explicit conversion policy paired with the retained image. */
  std::optional<ImageArtifactEncodeRequest> image_request;
  /** @brief Positive checked executor admission estimate. */
  std::uint64_t planned_bytes = 0U;
};

/**
 * @brief Captures every formal named Value into one canonical portable set.
 * @param output Exact validated formal HP output.
 * @return Encoded public archive after every payload and digest validates.
 * @throws All capture, digest, provider, bounds, and allocation failures.
 * @note The map is already canonical name order. No archive escapes until all
 * Values have been synchronously copied through checked read leases.
 */
std::vector<std::byte> capture_graph_cache_value_archive(
    const NodeOutput& output) {
  if (output.named_values.size() > kMaximumNamedValueArtifacts) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Graph cache Value count exceeds the portable bound.");
  }
  NamedValueArtifactSet artifacts;
  artifacts.values.reserve(output.named_values.size());
  for (const auto& [name, value] : output.named_values) {
    artifacts.values.push_back(capture_value_artifact(name, value));
  }
  return encode_named_value_artifact_set(artifacts);
}

/**
 * @brief Prepares one cache save before executor or filesystem side effects.
 * @param graph Graph providing current cache policy and root.
 * @param node Node providing entries and formal HP output.
 * @param cache_precision Precision label retained by image projection policy.
 * @return Complete detached save, or nullopt when policy requires no task.
 * @throws GraphError with `InvalidParameter` when any formal Value cannot be
 * captured as a complete portable artifact or image projection policy is
 * unsupported.
 * @throws std::bad_alloc unchanged from detached preparation.
 * @throws GraphError with `ComputeError` for checked estimate overflow.
 * @note Portable capture intentionally precedes task/byte admission so a
 * configured unsupported Value never becomes a silently skipped cache task.
 * No filesystem or codec method is called here.
 */
std::optional<PreparedGraphCacheSave> prepare_graph_cache_save(
    const GraphModel& graph, const Node& node,
    const std::string& cache_precision) {
  if (graph.skip_save_cache() || graph.cache_root.empty() ||
      node.caches.empty() || hp_cache_ptr(node) == nullptr) {
    return std::nullopt;
  }
  if (!has_image_disk_cache_entry(node)) {
    return std::nullopt;
  }

  PreparedGraphCacheSave prepared;
  const NodeOutput& output = *hp_cache_ptr(node);
  try {
    validate_image_disk_cache_output(output);
    prepared.complete_output = has_complete_hp_cache(node);
    if (prepared.complete_output) {
      if (output.data.size() > kMaximumNamedValueArtifacts) {
        throw GraphError(
            GraphErrc::InvalidParameter,
            "Graph cache parameter count exceeds the manifest bound.");
      }
      prepared.value_count =
          static_cast<std::uint32_t>(output.named_values.size());
      prepared.value_archive = capture_graph_cache_value_archive(output);
      prepared.parameters = output.data;
      if (output.has_image_value()) {
        prepared.image_projection = output.image_value();
        prepared.image_request =
            cache_encode_request(output.image_value(), cache_precision);
      }
    }
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError&) {
    throw;
  } catch (const std::exception& error) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        std::string("Value disk cache cannot capture formal output: ") +
            error.what());
  } catch (...) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Value disk cache cannot capture formal output: unknown "
                     "non-standard failure.");
  }

  constexpr std::uint64_t kTaskAndPathOverhead = 1024U;
  constexpr std::uint64_t kEntryOverhead = 256U;
  std::uint64_t total = kTaskAndPathOverhead;
  const auto& native_root = graph.cache_root.native();
  add_planned_bytes(total,
                    multiply_planned_bytes(planned_size(native_root.size()),
                                           sizeof(fs::path::value_type)));
  add_planned_bytes(total, planned_size(cache_precision.size()));
  add_planned_bytes(total, 1U);

  std::uint64_t output_bytes = 0U;
  if (prepared.complete_output) {
    add_planned_bytes(output_bytes,
                      planned_size(prepared.value_archive.size()));
    if (prepared.image_projection.has_value()) {
      add_planned_bytes(
          output_bytes,
          planned_size(prepared.image_projection->storage_size()));
    }
    for (const auto& [key, value] : prepared.parameters) {
      add_planned_bytes(output_bytes, planned_size(key.size()));
      add_planned_bytes(output_bytes, 1U);
      add_parameter_value_planned_bytes(output_bytes, value);
    }
  }

  std::uint64_t supported_entries = 0U;
  for (const CacheEntry& entry : node.caches) {
    if (entry.cache_type != "image" || entry.location.empty()) {
      continue;
    }
    ++supported_entries;
    add_planned_bytes(total, kEntryOverhead);
    add_planned_bytes(total, planned_size(entry.location.size()));
    add_planned_bytes(total, 1U);
  }
  if (supported_entries == 0U) {
    return std::nullopt;
  }
  add_planned_bytes(total, output_bytes);
  prepared.planned_bytes = total;
  return prepared;
}

/**
 * @brief Tests whether a node has formal HP cache state.
 *
 * @param node Node whose memory cache fields should be inspected.
 * @return true when the HP cache field is populated.
 * @throws Nothing.
 * @note RT proxy state is owned outside GraphModel and is not cleared by this
 * node-local helper.
 */
bool has_memory_cache(const Node& node) {
  return node.cached_output_high_precision.has_value();
}

/**
 * @brief Clears formal HP cache state from a node.
 *
 * @param node Node whose memory cache fields should be reset.
 * @throws Destructors for cached payload members are expected not to throw.
 * @note Topology, cache entries, and version counters are left unchanged.
 * Matching Region validity is cleared with the output. RT proxy state is not
 * stored on Node.
 */
void reset_memory_cache(Node& node) {
  node.cached_output_high_precision.reset();
  node.hp_region.reset();
}

/**
 * @brief Adds elapsed disk IO duration to the graph's aggregate IO counter.
 *
 * @param graph Graph whose atomic timing counter should be incremented.
 * @param start_io Start timestamp captured immediately before IO work.
 * @throws Nothing.
 * @note Uses compare-exchange because std::atomic<double> has no fetch_add in
 * C++17.
 */
void add_io_duration(GraphModel& graph,
                     std::chrono::high_resolution_clock::time_point start_io) {
  auto end_io = std::chrono::high_resolution_clock::now();
  double duration_ms =
      std::chrono::duration<double, std::milli>(end_io - start_io).count();

  double expected = graph.total_io_time_ms.load();
  while (!graph.total_io_time_ms.compare_exchange_weak(
      expected, expected + duration_ms)) {
  }
}

/**
 * @brief Adds an already measured worker duration to Graph I/O diagnostics.
 * @param graph Graph whose atomic timing counter should be incremented.
 * @param duration Nonnegative independent-worker callback duration.
 * @return Nothing.
 * @throws Nothing.
 * @note The graph-state caller invokes this after terminal completion; the
 * compute-I/O callback itself never receives Graph mutation authority.
 */
void add_io_duration(GraphModel& graph, std::chrono::nanoseconds duration) {
  const double duration_ms =
      std::chrono::duration<double, std::milli>(duration).count();
  double expected = graph.total_io_time_ms.load();
  while (!graph.total_io_time_ms.compare_exchange_weak(
      expected, expected + duration_ms)) {
  }
}

/**
 * @brief Builds a lightweight diagnostic record for a disk-cache attempt.
 *
 * @param node_id Node id whose cache entry is being inspected.
 * @param cache_entry Optional cache entry that supplied type and location.
 * @param cache_file Resolved image path, when available.
 * @param metadata_file Resolved metadata path, when available.
 * @param status Outcome status for the attempt.
 * @param code Error category when the attempt failed.
 * @param message Human-readable diagnostic text.
 * @return Populated diagnostic result.
 * @throws std::bad_alloc from string/path copies.
 * @note `code` is meaningful only for Error status; callers pass Unknown for
 * hits, misses, and skipped attempts.
 */
GraphModel::DiskCacheLoadResult make_load_result(
    int node_id, const CacheEntry* cache_entry, fs::path cache_file,
    fs::path metadata_file, DiskCacheLoadStatus status, GraphErrc code,
    std::string message) {
  GraphModel::DiskCacheLoadResult result;
  result.node_id = node_id;
  if (cache_entry) {
    result.cache_type = cache_entry->cache_type;
    result.location = cache_entry->location;
  }
  result.cache_file = std::move(cache_file);
  result.metadata_file = std::move(metadata_file);
  result.status = status;
  result.code = code;
  result.message = std::move(message);
  return result;
}

/**
 * @brief Creates a skipped-attempt result without a concrete cache entry.
 *
 * @param node_id Node id associated with the skipped attempt.
 * @param message Reason the service did not inspect disk files.
 * @return Diagnostic result with Skipped status.
 * @throws std::bad_alloc from message allocation.
 * @note This is used for disabled cache roots, empty cache lists, unsupported
 * entries, and nodes that already have HP memory cache.
 */
DiskCacheReadAttempt make_skipped_attempt(int node_id, std::string message) {
  DiskCacheReadAttempt attempt;
  attempt.result =
      make_load_result(node_id, nullptr, {}, {}, DiskCacheLoadStatus::Skipped,
                       GraphErrc::Unknown, std::move(message));
  return attempt;
}

/**
 * @brief Creates a concrete miss for an incompatible transaction/schema.
 *
 * @param node_id Node id associated with the incompatible entry.
 * @param cache_entry Entry that supplied the inspected transaction paths.
 * @param cache_file Resolved optional image-projection path.
 * @param metadata_file Resolved parameter-metadata path.
 * @param message Exact transaction or decoded-name incompatibility reason.
 * @return Miss whose specific diagnostic survives multi-entry finalization.
 * @throws std::bad_alloc from path and message copies.
 * @note The output is empty. Transaction-shape mismatches call this before
 * reconstruction; decoded-name mismatches discard the complete local candidate.
 */
DiskCacheReadAttempt make_schema_shape_miss(int node_id,
                                            const CacheEntry& cache_entry,
                                            const fs::path& cache_file,
                                            const fs::path& metadata_file,
                                            std::string message) {
  DiskCacheReadAttempt attempt;
  attempt.result = make_load_result(node_id, &cache_entry, cache_file,
                                    metadata_file, DiskCacheLoadStatus::Miss,
                                    GraphErrc::Unknown, std::move(message));
  attempt.preserve_miss_diagnostic = true;
  return attempt;
}

/**
 * @brief Compares detached metadata keys with the exact frozen plan.
 *
 * @param values Decoded parameter map owned by the current read attempt.
 * @param planned_names Exact unique parameter names frozen by planning.
 * @return True only when cardinality and every planned key match exactly.
 * @throws Nothing for current container size and lookup operations.
 * @note Values are deliberately not interpreted here; codec parsing owns value
 * validity, while output authority later validates the same admitted map.
 */
bool has_exact_parameter_output_names(
    const plugin::ParameterMap& values,
    const std::vector<std::string>& planned_names) {
  return values.size() == planned_names.size() &&
         std::all_of(planned_names.begin(), planned_names.end(),
                     [&](const std::string& name) {
                       return values.find(name) != values.end();
                     });
}

/**
 * @brief Creates an error result for a concrete cache entry.
 *
 * @param node_id Node id whose cache file failed to load.
 * @param cache_entry Cache entry that supplied the failed paths.
 * @param cache_file Resolved image cache path.
 * @param metadata_file Resolved metadata path.
 * @param code Error category for the failure.
 * @param message Human-readable failure reason.
 * @return Diagnostic result with Error status.
 * @throws std::bad_alloc from string/path copies.
 * @note The loaded output remains empty and must not be consumed by callers.
 */
DiskCacheReadAttempt make_error_attempt(int node_id,
                                        const CacheEntry& cache_entry,
                                        const fs::path& cache_file,
                                        const fs::path& metadata_file,
                                        GraphErrc code, std::string message) {
  DiskCacheReadAttempt attempt;
  attempt.result =
      make_load_result(node_id, &cache_entry, cache_file, metadata_file,
                       DiskCacheLoadStatus::Error, code, std::move(message));
  return attempt;
}

/**
 * @brief Reads one concrete disk-cache entry and converts failures to results.
 *
 * @param graph Graph whose cache root anchors the cache entry.
 * @param node Node that owns the cache entry.
 * @param cache_entry Image cache entry to inspect.
 * @param output_schema Complete frozen image/parameter output shape.
 * @param metadata_codec Injected codec used to decode named-value metadata.
 * @param data_definitions Optional provider registry used for reconstruction.
 * @return Hit, Miss, or Error attempt with diagnostic details.
 * @throws std::bad_alloc from result/message allocation.
 * @note The versioned manifest binds the archive and optional metadata bytes.
 * Exact Value/parameter names and all public artifact facts validate before a
 * candidate escapes. Shape mismatches are Miss; tamper, filesystem, provider,
 * and decode failures are Error rather than silently collapsed into miss.
 */
DiskCacheReadAttempt read_cache_entry(
    const GraphModel& graph, const Node& node, const CacheEntry& cache_entry,
    const ValueDiskCacheOutputSchema& output_schema,
    const CacheMetadataCodec& metadata_codec,
    DataDefinitionRegistry* data_definitions) {
  const GraphCacheArtifactPaths paths =
      graph_cache_artifact_paths(graph, node, cache_entry);

  try {
    const bool has_image_projection = fs::exists(paths.image_projection);
    const bool has_metadata_file = fs::exists(paths.metadata);
    const bool has_archive_file = fs::exists(paths.value_archive);
    const bool has_manifest_file = fs::exists(paths.manifest);
    if (!has_image_projection && !has_metadata_file && !has_archive_file &&
        !has_manifest_file) {
      DiskCacheReadAttempt attempt;
      attempt.result = make_load_result(
          node.id, &cache_entry, paths.image_projection, paths.metadata,
          DiskCacheLoadStatus::Miss, GraphErrc::Unknown,
          "No disk cache transaction exists for configured entry.");
      return attempt;
    }
    if (!has_archive_file || !has_manifest_file) {
      return make_schema_shape_miss(
          node.id, cache_entry, paths.image_projection, paths.metadata,
          "Disk-cache transaction is partial or uses the retired image/YAML "
          "format.");
    }

    constexpr std::uint64_t kMaximumManifestBytes = 1024U;
    const std::vector<std::byte> initial_manifest_bytes =
        read_cache_file_bytes(paths.manifest, kMaximumManifestBytes);
    const GraphCacheManifest manifest =
        decode_graph_cache_manifest(initial_manifest_bytes);
    const std::vector<std::string> expected_names =
        expected_cache_value_names(output_schema);
    const bool expects_metadata_file =
        !output_schema.parameter_output_names.empty();
    const bool manifest_has_metadata =
        (manifest.flags & kGraphCacheManifestHasMetadata) != 0U;
    if (manifest.value_count != expected_names.size() ||
        manifest.parameter_count !=
            output_schema.parameter_output_names.size() ||
        manifest_has_metadata != expects_metadata_file ||
        has_metadata_file != expects_metadata_file) {
      return make_schema_shape_miss(
          node.id, cache_entry, paths.image_projection, paths.metadata,
          "Disk-cache manifest/files do not match the frozen planned output "
          "shape.");
    }

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
    testing::notify_graph_cache_service_test_hook(
        testing::GraphCacheServiceTestEvent::ManifestReadBeforePayload,
        paths.directory);
#endif

    const std::vector<std::byte> archive_bytes = read_cache_file_bytes(
        paths.value_archive,
        kMaximumValueArtifactPayloadBytes +
            static_cast<std::uint64_t>(kMaximumValueArtifactMetadataBytes),
        manifest.archive.byte_size);
    validate_cache_file_record(archive_bytes, manifest.archive);
    const NamedValueArtifactSet artifacts =
        decode_named_value_artifact_set(archive_bytes);
    if (artifacts.values.size() != manifest.value_count) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Graph cache archive count disagrees with manifest.");
    }
    std::vector<std::string> actual_names;
    actual_names.reserve(artifacts.values.size());
    for (const ValueArtifact& artifact : artifacts.values) {
      actual_names.push_back(artifact.envelope.output_name);
    }
    if (actual_names != expected_names) {
      return make_schema_shape_miss(
          node.id, cache_entry, paths.image_projection, paths.metadata,
          "Disk-cache archive names do not match the frozen planned Value "
          "schema.");
    }

    NodeOutput candidate;
    for (const ValueArtifact& artifact : artifacts.values) {
      candidate.publish_named_value(
          artifact.envelope.output_name,
          reconstruct_value_artifact(artifact, data_definitions));
    }

    if (manifest_has_metadata) {
      const std::vector<std::byte> metadata_before =
          read_cache_file_bytes(paths.metadata, kMaximumGraphCacheMetadataBytes,
                                manifest.metadata.byte_size);
      validate_cache_file_record(metadata_before, manifest.metadata);
      candidate.data = metadata_codec.read(paths.metadata);
      const std::vector<std::byte> metadata_after =
          read_cache_file_bytes(paths.metadata, kMaximumGraphCacheMetadataBytes,
                                manifest.metadata.byte_size);
      validate_cache_file_record(metadata_after, manifest.metadata);
      if (metadata_after != metadata_before) {
        throw GraphError(
            GraphErrc::InvalidParameter,
            "Graph cache metadata changed during transactional replay.");
      }
    }
    if (!has_exact_parameter_output_names(
            candidate.data, output_schema.parameter_output_names)) {
      return make_schema_shape_miss(
          node.id, cache_entry, paths.image_projection, paths.metadata,
          "Disk-cache metadata keys do not match the frozen planned "
          "parameter-output schema.");
    }

    const std::vector<std::byte> final_manifest_bytes =
        read_cache_file_bytes(paths.manifest, kMaximumManifestBytes);
    if (final_manifest_bytes != initial_manifest_bytes) {
      throw GraphError(
          GraphErrc::InvalidParameter,
          "Graph cache manifest changed during transactional replay.");
    }

    DiskCacheReadAttempt attempt;
    attempt.output = std::move(candidate);
    attempt.result = make_load_result(
        node.id, &cache_entry, paths.image_projection, paths.metadata,
        DiskCacheLoadStatus::Hit, GraphErrc::Unknown,
        "Loaded portable named-Value disk cache transaction.");
    return attempt;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const fs::filesystem_error& e) {
    return make_error_attempt(
        node.id, cache_entry, paths.image_projection, paths.metadata,
        GraphErrc::Io,
        std::string("Filesystem failed while reading disk cache: ") + e.what());
  } catch (const GraphError& e) {
    return make_error_attempt(node.id, cache_entry, paths.image_projection,
                              paths.metadata, e.code(), e.what());
  } catch (const ExtensionContractError& e) {
    return make_error_attempt(
        node.id, cache_entry, paths.image_projection, paths.metadata,
        GraphErrc::InvalidParameter,
        std::string("Portable cache provider validation failed: ") + e.what());
  } catch (const std::invalid_argument& e) {
    return make_error_attempt(
        node.id, cache_entry, paths.image_projection, paths.metadata,
        GraphErrc::InvalidParameter,
        std::string("Portable cache artifact validation failed: ") + e.what());
  } catch (const std::overflow_error& e) {
    return make_error_attempt(
        node.id, cache_entry, paths.image_projection, paths.metadata,
        GraphErrc::InvalidParameter,
        std::string("Portable cache artifact validation failed: ") + e.what());
  } catch (const std::length_error& e) {
    return make_error_attempt(
        node.id, cache_entry, paths.image_projection, paths.metadata,
        GraphErrc::InvalidParameter,
        std::string("Portable cache artifact validation failed: ") + e.what());
  } catch (const std::exception& e) {
    return make_error_attempt(
        node.id, cache_entry, paths.image_projection, paths.metadata,
        GraphErrc::Unknown,
        std::string("Unexpected exception while reading disk cache: ") +
            e.what());
  } catch (...) {
    return make_error_attempt(
        node.id, cache_entry, paths.image_projection, paths.metadata,
        GraphErrc::Unknown,
        "Unknown non-standard exception while reading disk cache.");
  }
}

/**
 * @brief Scans a node's cache entries and returns the first terminal result.
 *
 * @param graph Graph whose cache root anchors the entries.
 * @param node Node whose cache entries should be inspected.
 * @param output_schema Complete frozen image/parameter output shape.
 * @param metadata_codec Injected codec used for every existing metadata file.
 * @param data_definitions Optional provider registry used for reconstruction.
 * @return Hit/Error for the first existing or failing entry, Miss when all
 * supported entries are absent, or Skipped when no load should be attempted.
 * @throws std::bad_alloc from diagnostic construction.
 * @note Missing and incompatible entries remain cache misses and do not stop
 * scanning later entries; the last incompatibility reason is retained.
 * Read/parse errors stop immediately to preserve their diagnostics.
 */
DiskCacheReadAttempt read_first_disk_cache_entry(
    const GraphModel& graph, const Node& node,
    const ValueDiskCacheOutputSchema& output_schema,
    const CacheMetadataCodec& metadata_codec,
    DataDefinitionRegistry* data_definitions) {
  if (graph.cache_root.empty()) {
    return make_skipped_attempt(node.id, "Graph has no disk cache root.");
  }
  if (node.caches.empty()) {
    return make_skipped_attempt(node.id, "Node has no configured cache entry.");
  }

  bool saw_supported_entry = false;
  DiskCacheReadAttempt last_miss =
      make_skipped_attempt(node.id, "No supported image cache entry found.");
  std::optional<DiskCacheReadAttempt> last_incompatible_miss;
  for (const auto& cache_entry : node.caches) {
    if (cache_entry.cache_type != "image" || cache_entry.location.empty()) {
      continue;
    }

    saw_supported_entry = true;
    DiskCacheReadAttempt attempt =
        read_cache_entry(graph, node, cache_entry, output_schema,
                         metadata_codec, data_definitions);
    if (attempt.result.status != DiskCacheLoadStatus::Miss) {
      return attempt;
    }
    if (attempt.preserve_miss_diagnostic) {
      last_incompatible_miss = std::move(attempt);
      continue;
    }
    last_miss = std::move(attempt);
  }

  if (last_incompatible_miss.has_value()) {
    return std::move(*last_incompatible_miss);
  }
  if (saw_supported_entry) {
    last_miss.result.message =
        "No disk cache files exist for configured image cache entries.";
    return last_miss;
  }
  return last_miss;
}

/**
 * @brief Stores the diagnostic result from one disk-cache load attempt.
 *
 * @param graph Graph receiving the latest diagnostic record.
 * @param result Diagnostic result to move into GraphModel.
 * @throws std::bad_alloc if optional storage needs allocation.
 * @note The graph stores only the most recent attempt by design; detailed
 * histories can be added later through event services if needed.
 */
void record_disk_cache_load_result(GraphModel& graph,
                                   GraphModel::DiskCacheLoadResult result) {
  graph.record_disk_cache_load_result(std::move(result));
}

/**
 * @brief Commits a successful read attempt through a caller-supplied consumer.
 *
 * @param graph Graph whose IO timing and diagnostics should be updated.
 * @param attempt Read attempt returned by read_first_disk_cache_entry.
 * @param start_io Start timestamp captured before scanning disk cache entries.
 * @param consume_output Callable that accepts a NodeOutput rvalue on hit.
 * @return true when the attempt was a disk-cache hit; false otherwise.
 * @throws Exceptions from `consume_output` or diagnostic storage.
 * @note The template keeps assignment into node HP cache and execution temp
 * output slots unified without exposing output ownership in GraphModel.
 */
template <typename OutputConsumer>
bool finalize_disk_cache_load(
    GraphModel& graph, DiskCacheReadAttempt attempt,
    std::chrono::high_resolution_clock::time_point start_io,
    OutputConsumer&& consume_output) {
  const bool loaded = attempt.result.status == DiskCacheLoadStatus::Hit;
  if (loaded) {
    consume_output(std::move(attempt.output));
    add_io_duration(graph, start_io);
  }
  record_disk_cache_load_result(graph, std::move(attempt.result));
  return loaded;
}

/**
 * @brief Removes configured artifacts excluded by the retained transaction.
 *
 * @param paths Exact paths for the optional projections and transaction files.
 * @param retain_image Whether the completed output retains the image
 * projection.
 * @param retain_metadata Whether it retains detached parameter metadata.
 * @param retain_transaction Whether it retains the archive and manifest.
 * @return Nothing after optional empty-directory removal.
 * @throws std::filesystem::filesystem_error when an existence, removal, or
 * emptiness query fails.
 * @note Callers invoke this after required writes succeed, or directly for
 * partial outputs that retain no transaction. Removal is intentionally scoped
 * to the four configured paths and is not atomic: one removal can succeed
 * before a later removal fails. Replay still requires a digest-valid archive
 * and manifest pair, so residual mixed generations cannot publish output.
 */
void remove_cache_siblings_not_retained(const GraphCacheArtifactPaths& paths,
                                        bool retain_image, bool retain_metadata,
                                        bool retain_transaction) {
  if (!retain_image && fs::exists(paths.image_projection)) {
    (void)fs::remove(paths.image_projection);
  }
  if (!retain_metadata && fs::exists(paths.metadata)) {
    (void)fs::remove(paths.metadata);
  }
  if (!retain_transaction && fs::exists(paths.value_archive)) {
    (void)fs::remove(paths.value_archive);
  }
  if (!retain_transaction && fs::exists(paths.manifest)) {
    (void)fs::remove(paths.manifest);
  }
  if (fs::exists(paths.directory) && fs::is_empty(paths.directory)) {
    (void)fs::remove(paths.directory);
  }
}

/**
 * @brief Executes the service-owned cache-save filesystem and codec mechanism.
 * @param graph Read-only Graph policy, cache root, and prepared output owner.
 * @param node Read-only node whose configured cache entries are processed.
 * @param prepared Detached archive, metadata, image policy, and admission
 * estimate captured before side effects.
 * @param image_codec Codec selected and retained by GraphCacheService.
 * @param metadata_codec Named-value codec retained by GraphCacheService.
 * @param timing_graph Optional graph-state-only timing sink; null on the
 * independent I/O worker.
 * @throws Codec, filesystem, Graph, Value, or allocation exceptions unchanged.
 * @note A null timing sink guarantees that provider work mutates no Graph
 * state. Partial HP output removes the complete older transaction. Complete
 * output writes the optional image projection and parameter bytes, the exact
 * named-Value archive, and finally its versioned manifest. Any failure before
 * the manifest leaves an unusable mixed/partial generation; replay verifies
 * every file and publishes no partial NodeOutput.
 */
void save_cache_mechanism(const GraphModel& graph, const Node& node,
                          const PreparedGraphCacheSave& prepared,
                          const ImageArtifactCodec& image_codec,
                          const CacheMetadataCodec& metadata_codec,
                          GraphModel* timing_graph) {
  for (const CacheEntry& cache_entry : node.caches) {
    if (cache_entry.cache_type != "image" || cache_entry.location.empty()) {
      continue;
    }

    const GraphCacheArtifactPaths paths =
        graph_cache_artifact_paths(graph, node, cache_entry);
    if (!prepared.complete_output) {
      remove_cache_siblings_not_retained(paths, false, false, false);
      continue;
    }

    const bool retain_image = prepared.image_projection.has_value();
    const bool retain_metadata = !prepared.parameters.empty();
    fs::create_directories(paths.directory);

    const auto start_io = std::chrono::high_resolution_clock::now();
    if (retain_image) {
      image_codec.encode(paths.image_projection, *prepared.image_projection,
                         *prepared.image_request);
    }
    if (retain_metadata) {
      metadata_codec.write(paths.metadata, prepared.parameters);
    }
    write_cache_file_bytes(paths.value_archive, prepared.value_archive);
    remove_cache_siblings_not_retained(paths, retain_image, retain_metadata,
                                       true);

    GraphCacheManifest manifest;
    manifest.value_count = prepared.value_count;
    manifest.archive = capture_cache_file_record(
        paths.value_archive,
        kMaximumValueArtifactPayloadBytes +
            static_cast<std::uint64_t>(kMaximumValueArtifactMetadataBytes));
    if (retain_metadata) {
      manifest.flags |= kGraphCacheManifestHasMetadata;
      manifest.parameter_count =
          static_cast<std::uint32_t>(prepared.parameters.size());
      manifest.metadata = capture_cache_file_record(
          paths.metadata, kMaximumGraphCacheMetadataBytes);
      if (metadata_codec.read(paths.metadata) != prepared.parameters) {
        throw GraphError(
            GraphErrc::InvalidParameter,
            "Graph cache metadata codec round-trip changed parameter facts.");
      }
    }
    const std::vector<std::byte> manifest_bytes =
        encode_graph_cache_manifest(manifest);
    write_cache_file_bytes(paths.manifest, manifest_bytes);
    const std::vector<std::byte> persisted_manifest =
        read_cache_file_bytes(paths.manifest, 1024U, manifest_bytes.size());
    if (persisted_manifest != manifest_bytes) {
      throw GraphError(GraphErrc::Io,
                       "Graph cache manifest write did not round-trip.");
    }
    if (timing_graph != nullptr) {
      add_io_duration(*timing_graph, start_io);
    }
  }
}

}  // namespace

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
namespace testing {
namespace {

/** @brief Borrowed cache hook pointer stored by the test-only seam. */
using GraphCacheServiceTestHookPtr = const GraphCacheServiceTestHook*;

/**
 * @brief Process-local observer for deterministic cache-clear tests.
 * @throws Nothing for atomic initialization and pointer publication.
 * @note Tests serialize installation and clear the pointer before destroying
 * the borrowed hook or context.
 */
std::atomic<GraphCacheServiceTestHookPtr> g_graph_cache_service_test_hook{
    nullptr};  // NOLINT(whitespace/indent_namespace)

}  // namespace

/** @copydoc ps::testing::set_graph_cache_service_test_hook */
void set_graph_cache_service_test_hook(
    const GraphCacheServiceTestHook* hook) noexcept {
  g_graph_cache_service_test_hook.store(hook, std::memory_order_release);
}

/** @copydoc ps::testing::notify_graph_cache_service_test_hook */
void notify_graph_cache_service_test_hook(
    GraphCacheServiceTestEvent event, const std::filesystem::path& cache_root) {
  const GraphCacheServiceTestHook* hook =
      g_graph_cache_service_test_hook.load(std::memory_order_acquire);
  if (hook != nullptr && hook->notify != nullptr) {
    hook->notify(hook->context, event, cache_root);
  }
}

}  // namespace testing
#endif

/** @copydoc GraphCacheService::GraphCacheService */
GraphCacheService::GraphCacheService(
    std::shared_ptr<const ImageArtifactCodec> image_codec,
    std::shared_ptr<const CacheMetadataCodec> metadata_codec,
    std::size_t maximum_statistics_entries,
    DataDefinitionRegistry* data_definitions)
    : image_codec_(std::move(image_codec)),
      metadata_codec_(std::move(metadata_codec)),
      data_definitions_(data_definitions),
      image_statistics_store_(maximum_statistics_entries) {
  if (!image_codec_) {
    throw std::invalid_argument(
        "GraphCacheService requires an image artifact codec");
  }
  if (!metadata_codec_) {
    throw std::invalid_argument(
        "GraphCacheService requires a cache metadata codec");
  }
}

/** @copydoc GraphCacheService::schedule_image_statistics */
ScheduledImageStatistics GraphCacheService::schedule_image_statistics(
    Value value, std::optional<ContentDigest> content_digest,
    ImageStatisticsQuery query,
    const ImageStatisticsStore::Scheduler& scheduler) const {
  return image_statistics_store_.schedule(
      std::move(value), std::move(content_digest), std::move(query), scheduler);
}

/** @copydoc GraphCacheService::lookup_image_statistics */
std::optional<ImageStatisticsResult> GraphCacheService::lookup_image_statistics(
    const ImageStatisticsCacheKey& key) const {
  return image_statistics_store_.lookup(key);
}

/** @copydoc GraphCacheService::invalidate_image_statistics_revision */
std::size_t GraphCacheService::invalidate_image_statistics_revision(
    ValueRevisionId revision) const {
  return image_statistics_store_.invalidate_revision(revision);
}

/** @copydoc GraphCacheService::clear_image_statistics */
std::size_t GraphCacheService::clear_image_statistics() const {
  return image_statistics_store_.clear();
}

/** @copydoc GraphCacheService::image_statistics_size */
std::size_t GraphCacheService::image_statistics_size() const {
  return image_statistics_store_.size();
}

std::filesystem::path GraphCacheService::node_cache_dir(const GraphModel& graph,
                                                        int node_id) const {
  return graph.cache_root / std::to_string(node_id);
}

void GraphCacheService::save_cache_if_configured(
    GraphModel& graph, const Node& node,
    const std::string& cache_precision) const {
  const std::optional<PreparedGraphCacheSave> prepared =
      prepare_graph_cache_save(graph, node, cache_precision);
  if (!prepared.has_value()) {
    return;
  }
  save_cache_mechanism(graph, node, *prepared, *image_codec_, *metadata_codec_,
                       &graph);
}

/** @copydoc GraphCacheService::save_cache_if_configured_via_executor */
void GraphCacheService::save_cache_if_configured_via_executor(
    execution::ComputeIoExecutor& executor,
    const std::shared_ptr<const void>& lifetime_token, GraphModel& graph,
    const Node& node, const std::string& cache_precision) const {
  std::optional<PreparedGraphCacheSave> prepared =
      prepare_graph_cache_save(graph, node, cache_precision);
  if (!prepared.has_value()) {
    return;
  }
  const auto retained_prepared =
      std::make_shared<const PreparedGraphCacheSave>(std::move(*prepared));

  const execution::ComputeIoSubmission submission = executor.try_submit(
      retained_prepared->planned_bytes, lifetime_token,
      [&graph, &node, retained_prepared,
       this]() -> execution::ComputeIoExecutor::Task {
        const std::shared_ptr<const ImageArtifactCodec> image_codec =
            image_codec_;
        const std::shared_ptr<const CacheMetadataCodec> metadata_codec =
            metadata_codec_;
        return
            [&graph, &node, retained_prepared, image_codec, metadata_codec]() {
              save_cache_mechanism(graph, node, *retained_prepared,
                                   *image_codec, *metadata_codec, nullptr);
            };
      });
  if (!submission.accepted()) {
    throw GraphError(GraphErrc::ComputeError,
                     std::string("Compute-I/O cache save rejected: ") +
                         execution::compute_io_admission_status_name(
                             submission.admission_status()));
  }

  const execution::ComputeIoTaskResult result = submission.completion().wait();
  add_io_duration(graph, result.work_duration());
  if (result.status() == execution::ComputeIoCompletionStatus::Cancelled) {
    throw GraphError(GraphErrc::ComputeError,
                     "Compute-I/O cache save was cancelled.");
  }
  result.rethrow_if_failed();
}

bool GraphCacheService::try_load_from_disk_cache(
    GraphModel& graph, Node& node,
    ValueDiskCacheOutputSchema output_schema) const {
  if (node.cached_output_high_precision.has_value()) {
    const bool complete_output = has_complete_hp_cache(node);
    record_disk_cache_load_result(
        graph, make_skipped_attempt(
                   node.id,
                   complete_output
                       ? "Node already has complete formal HP memory cache."
                       : "Node has partial formal HP memory cache and requires "
                         "whole-output recomputation.")
                   .result);
    return complete_output;
  }
  auto start_io = std::chrono::high_resolution_clock::now();
  DiskCacheReadAttempt attempt = read_first_disk_cache_entry(
      graph, node, output_schema, *metadata_codec_, data_definitions_);
  return finalize_disk_cache_load(
      graph, std::move(attempt), start_io, [&](NodeOutput output) {
        RegionSet full_region = value_region::full_node_output_region(output);
        node.cached_output_high_precision = std::move(output);
        node.hp_region = std::move(full_region);
        node.hp_version++;
      });
}

bool GraphCacheService::try_load_from_disk_cache_into(
    GraphModel& graph, const Node& node, NodeOutput& out,
    ValueDiskCacheOutputSchema output_schema) const {
  if (node.cached_output_high_precision.has_value()) {
    record_disk_cache_load_result(
        graph, make_skipped_attempt(
                   node.id,
                   "Node already has formal HP memory state; disk cache cannot "
                   "override complete or partial runtime validity.")
                   .result);
    return false;
  }
  auto start_io = std::chrono::high_resolution_clock::now();
  DiskCacheReadAttempt attempt = read_first_disk_cache_entry(
      graph, node, output_schema, *metadata_codec_, data_definitions_);
  return finalize_disk_cache_load(
      graph, std::move(attempt), start_io,
      [&](NodeOutput output) { out = std::move(output); });
}

/** @copydoc GraphCacheService::clear_drive_cache */
GraphModel::DriveClearResult GraphCacheService::clear_drive_cache(
    GraphModel& graph) const {
  GraphModel::DriveClearResult result;
  if (!graph.cache_root.empty() && fs::exists(graph.cache_root)) {
    result.removed_entries = fs::remove_all(graph.cache_root);
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
    testing::notify_graph_cache_service_test_hook(
        testing::GraphCacheServiceTestEvent::DriveCacheRootRemoved,
        graph.cache_root);
#endif
    fs::create_directories(graph.cache_root);
  }
  return result;
}

GraphModel::MemoryClearResult GraphCacheService::clear_memory_cache(
    GraphModel& graph) const {
  GraphModel::MemoryClearResult result;
  for (int node_id : graph.node_ids()) {
    Node& node = graph.mutable_node(node_id);
    if (has_memory_cache(node)) {
      reset_memory_cache(node);
      result.cleared_nodes++;
    }
  }
  return result;
}

void GraphCacheService::clear_cache(GraphModel& graph) const {
  (void)clear_drive_cache(graph);
  (void)clear_memory_cache(graph);
}

GraphModel::CacheSaveResult GraphCacheService::cache_all_nodes(
    GraphModel& graph, const std::string& cache_precision) const {
  GraphModel::CacheSaveResult result;
  for (int node_id : graph.node_ids()) {
    const Node& node = graph.node(node_id);
    if (hp_cache_ptr(node)) {
      save_cache_if_configured(graph, node, cache_precision);
      result.saved_nodes++;
    }
  }
  return result;
}

GraphModel::MemoryClearResult GraphCacheService::free_transient_memory(
    GraphModel& graph) const {
  GraphTraversalService traversal;
  auto ends = traversal.ending_nodes(graph);
  std::unordered_set<int> endset(ends.begin(), ends.end());

  GraphModel::MemoryClearResult result;
  for (int node_id : graph.node_ids()) {
    Node& node = graph.mutable_node(node_id);
    if (has_memory_cache(node) && !endset.count(node_id)) {
      reset_memory_cache(node);
      result.cleared_nodes++;
    }
  }
  return result;
}

GraphModel::DiskSyncResult GraphCacheService::synchronize_disk_cache(
    GraphModel& graph, const std::string& cache_precision) const {
  GraphModel::DiskSyncResult result;
  result.saved_nodes = cache_all_nodes(graph, cache_precision).saved_nodes;

  for (int node_id : graph.node_ids()) {
    const Node& node = graph.node(node_id);
    if (has_complete_hp_cache(node) || node.caches.empty()) {
      continue;
    }

    auto dir_path = node_cache_dir(graph, node.id);
    if (!fs::exists(dir_path)) {
      continue;
    }

    for (const auto& cache_entry : node.caches) {
      if (cache_entry.location.empty()) {
        continue;
      }
      auto cache_file = dir_path / cache_entry.location;
      auto meta_file = cache_file;
      meta_file.replace_extension(".yml");
      auto archive_file = cache_file;
      archive_file += ".values";
      auto manifest_file = cache_file;
      manifest_file += ".manifest";

      for (const fs::path& file :
           {cache_file, meta_file, archive_file, manifest_file}) {
        if (fs::exists(file) && fs::remove(file)) {
          result.removed_files++;
        }
      }
    }

    if (fs::is_empty(dir_path)) {
      const bool removed_dir = fs::remove(dir_path);
      if (removed_dir) {
        result.removed_dirs++;
      }
    }
  }

  return result;
}

}  // namespace ps
