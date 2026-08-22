#include "graph/graph_cache_service.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
#include <atomic>
#endif
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

#if defined(_WIN32)
/**
 * @brief Rejects GraphCache disk persistence on the unsupported Windows host.
 * @return Never returns.
 * @throws GraphError with the stable `InvalidParameter` platform boundary.
 * @throws std::bad_alloc if diagnostic ownership cannot allocate.
 * @note The helper performs no codec, filesystem, executor, Graph, cache,
 * timing, diagnostic, or platform-API work. Windows persistence is a future
 * capability rather than a partial native-filesystem implementation.
 */
[[noreturn]] void throw_graph_cache_disk_persistence_unsupported() {
  throw GraphError(GraphErrc::InvalidParameter,
                   "Graph cache disk persistence is unsupported on Windows.");
}
#endif

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
constexpr std::uint32_t kGraphCacheManifestVersion = 2U;

/** @brief Bit indicating one digest-bound parameter metadata payload. */
constexpr std::uint32_t kGraphCacheManifestHasMetadata = 1U;

/** @brief Exact private graph-cache manifest magic. */
constexpr std::array<std::byte, 8U> kGraphCacheManifestMagic{
    std::byte{'P'}, std::byte{'S'}, std::byte{'C'},
    std::byte{'A'}, std::byte{'C'}, std::byte{'H'},
    std::byte{'E'}, std::byte{'1'}};  // NOLINT(whitespace/indent_namespace)

/** @brief Frozen maximum encoded parameter metadata bytes per cache entry. */
constexpr auto kMaximumGraphCacheMetadataBytes = 16ULL * 1024ULL * 1024ULL;

/** @brief Cryptographically random cache-generation byte count. */
constexpr std::size_t kGraphCacheGenerationBytes = 16U;

/** @brief Portable configured leaf-byte bound including its original suffix. */
constexpr std::size_t kMaximumGraphCacheLocationBytes = 192U;

/** @brief Exact owner-defined cache generation joined across transaction data.
 */
using GraphCacheGeneration = std::array<std::byte, kGraphCacheGenerationBytes>;

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
  /** @brief Random writer generation repeated by every archive envelope. */
  GraphCacheGeneration generation;
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
  /** @brief Safe single-leaf image projection name. */
  std::string image_leaf;
  /** @brief Safe single-leaf detached metadata name. */
  std::string metadata_leaf;
  /** @brief Safe single-leaf portable archive name. */
  std::string archive_leaf;
  /** @brief Safe single-leaf manifest name. */
  std::string manifest_leaf;
};

/**
 * @brief Process-shared serialization state for one normalized cache root.
 * @throws Nothing for ordinary construction.
 * @note The mutex covers reads, writes, cleanup, synchronize, and clear across
 * every GraphCacheService instance in this process. `mutation_epoch`
 * invalidates an asynchronous writer prepared before any later save, partial
 * cleanup, synchronization, or drive-clear linearization point.
 */
struct GraphCacheRootCoordination final {
  /** @brief Serializes every transaction under this exact root. */
  std::mutex mutex;
  /** @brief Monotonic cache-mutation generation protected by `mutex`. */
  std::uint64_t mutation_epoch = 0U;
};

/** @brief Serializes the weak process registry of root coordinators. */
std::mutex g_graph_cache_coordination_mutex;

/**
 * @brief Weak process registry avoiding permanent cache-root retention.
 * @note Access requires `g_graph_cache_coordination_mutex`; expired entries are
 * erased opportunistically whenever another root is acquired.
 */
std::unordered_map<std::string, std::weak_ptr<GraphCacheRootCoordination>>
    g_graph_cache_coordinators;  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Active root guards on the current thread for reentry detection.
 * @note Different roots may nest; attempting the same root throws before
 * mutex acquisition instead of deadlocking through a codec or test hook.
 */
thread_local std::vector<const GraphCacheRootCoordination*>
    g_active_graph_cache_roots;  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Returns the process coordinator for one lexical cache-root identity.
 * @param root Nonempty configured graph cache root.
 * @return Shared coordinator retained through the complete caller operation.
 * @throws std::invalid_argument when `root` is empty.
 * @throws Filesystem or allocation exceptions from key normalization/registry.
 * @note Weak canonicalization coalesces lexical and intermediate-symlink
 * aliases for serialization only; it grants no filesystem authority.
 * Directory capabilities independently reject a final-root symlink and bind
 * exact filesystem identities for every operation.
 */
std::shared_ptr<GraphCacheRootCoordination> graph_cache_root_coordination(
    const fs::path& root) {
  if (root.empty()) {
    throw std::invalid_argument(
        "Graph cache coordination requires a nonempty root.");
  }
  std::string key = fs::weakly_canonical(fs::absolute(root)).generic_string();
  std::transform(key.begin(), key.end(), key.begin(), [](char character) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte >= static_cast<unsigned char>('A') &&
        byte <= static_cast<unsigned char>('Z')) {
      return static_cast<char>(byte - static_cast<unsigned char>('A') +
                               static_cast<unsigned char>('a'));
    }
    return character;
  });
  std::lock_guard<std::mutex> lock(g_graph_cache_coordination_mutex);
  for (auto iterator = g_graph_cache_coordinators.begin();
       iterator != g_graph_cache_coordinators.end();) {
    if (iterator->second.expired()) {
      iterator = g_graph_cache_coordinators.erase(iterator);
    } else {
      ++iterator;
    }
  }
  const auto found = g_graph_cache_coordinators.find(key);
  if (found != g_graph_cache_coordinators.end()) {
    if (std::shared_ptr<GraphCacheRootCoordination> retained =
            found->second.lock()) {
      return retained;
    }
  }
  auto created = std::make_shared<GraphCacheRootCoordination>();
  g_graph_cache_coordinators[key] = created;
  return created;
}

/**
 * @brief Holds one root transaction mutex and rejects same-thread reentry.
 * @throws std::logic_error when the same root is already active on this thread.
 * @throws std::bad_alloc when active-root tracking cannot grow.
 * @throws std::system_error when mutex acquisition fails.
 * @note Codec callbacks execute under this guard. In test-enabled builds the
 * root-lock observer runs immediately before the nonblocking lock attempt and
 * after that attempt classifies contention; it must not re-enter this root.
 * Reentry is a contract error, never a recursive cache transaction or hidden
 * deadlock.
 */
class GraphCacheRootGuard final {
 public:
  /**
   * @brief Acquires one retained root coordinator.
   * @param coordination Non-null process coordinator.
   * @param cache_root Nonempty root used only for test-checkpoint provenance.
   * @throws std::invalid_argument for an empty owner.
   * @throws std::logic_error, std::bad_alloc, or std::system_error as above.
   * @throws Any exception selected by the test-only observer before or after
   * the nonblocking lock attempt.
   */
  explicit GraphCacheRootGuard(
      std::shared_ptr<GraphCacheRootCoordination> coordination,
      const fs::path& cache_root)
      : coordination_(std::move(coordination)) {
    if (!coordination_) {
      throw std::invalid_argument(
          "Graph cache root guard requires retained coordination.");
    }
    if (std::find(g_active_graph_cache_roots.begin(),
                  g_active_graph_cache_roots.end(),
                  coordination_.get()) != g_active_graph_cache_roots.end()) {
      throw std::logic_error(
          "Graph cache operation cannot re-enter the same cache root.");
    }
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
    testing::notify_graph_cache_service_test_hook(
        testing::GraphCacheServiceTestEvent::RootOperationBeforeLock,
        cache_root);
    lock_ = std::unique_lock<std::mutex>(coordination_->mutex, std::defer_lock);
    if (lock_.try_lock()) {
      testing::notify_graph_cache_service_test_hook(
          testing::GraphCacheServiceTestEvent::
              RootOperationLockAcquiredWithoutContention,
          cache_root);
    } else {
      testing::notify_graph_cache_service_test_hook(
          testing::GraphCacheServiceTestEvent::RootOperationLockContended,
          cache_root);
      lock_.lock();
    }
#else
    (void)cache_root;
    lock_ = std::unique_lock<std::mutex>(coordination_->mutex);
#endif
    g_active_graph_cache_roots.push_back(coordination_.get());
    active_ = true;
  }

  /** @brief Releases current-thread tracking before the mutex. @throws Nothing.
   */
  ~GraphCacheRootGuard() noexcept {
    if (active_) {
      if (g_active_graph_cache_roots.empty() ||
          g_active_graph_cache_roots.back() != coordination_.get()) {
        std::terminate();
      }
      g_active_graph_cache_roots.pop_back();
    }
  }

  /**
   * @brief Prevents duplicate root-lock and tracking ownership.
   * @param other Unused source because construction is forbidden.
   * @throws Nothing; this operation is deleted.
   */
  GraphCacheRootGuard(const GraphCacheRootGuard& other) = delete;

  /**
   * @brief Prevents duplicate root-lock and tracking assignment.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing; this operation is deleted.
   */
  GraphCacheRootGuard& operator=(const GraphCacheRootGuard& other) = delete;

  /** @brief Returns the protected mutation epoch. @return Current epoch. */
  std::uint64_t mutation_epoch() const noexcept {
    return coordination_->mutation_epoch;
  }

  /**
   * @brief Advances the protected cache-mutation epoch.
   * @return New nonzero epoch.
   * @throws std::overflow_error when the process epoch is exhausted.
   */
  std::uint64_t advance_mutation_epoch() {
    if (coordination_->mutation_epoch ==
        std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("Graph cache mutation epoch exhausted.");
    }
    return ++coordination_->mutation_epoch;
  }

 private:
  /** @brief Retains the exact weak-registry entry through unlock. */
  std::shared_ptr<GraphCacheRootCoordination> coordination_;
  /** @brief Root mutex released after current-thread tracking is removed. */
  std::unique_lock<std::mutex> lock_;
  /** @brief True only after active-root tracking succeeds. */
  bool active_ = false;
};

/**
 * @brief Produces a conservative cross-platform cache-leaf collision key.
 * @param leaf Validated single-leaf bytes.
 * @return ASCII-case-folded key used only for authority-alias rejection.
 * @throws std::bad_alloc when key ownership cannot allocate.
 * @note Windows and common macOS volumes are case-insensitive while POSIX may
 * not be. Treating ASCII case aliases as collisions everywhere keeps one graph
 * definition from gaining different transaction authority after migration.
 */
std::string portable_graph_cache_leaf_key(std::string leaf) {
  std::transform(leaf.begin(), leaf.end(), leaf.begin(), [](char character) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte >= static_cast<unsigned char>('A') &&
        byte <= static_cast<unsigned char>('Z')) {
      return static_cast<char>(byte - static_cast<unsigned char>('A') +
                               static_cast<unsigned char>('a'));
    }
    return character;
  });
  return leaf;
}

/**
 * @brief Reports whether a leaf uses a reserved Windows device basename.
 * @param location Candidate portable leaf bytes.
 * @return True for CON, PRN, AUX, NUL, COM1-9, or LPT1-9 before extension.
 * @throws std::bad_alloc when folded basename ownership cannot allocate.
 * @note The enclosing portable validator admits ASCII only, which also rejects
 * Windows' Unicode superscript COM/LPT digit spellings before filesystem use.
 */
bool graph_cache_leaf_has_reserved_device_name(const std::string& location) {
  const std::size_t dot = location.find('.');
  const std::string basename =
      portable_graph_cache_leaf_key(location.substr(0U, dot));
  if (basename == "con" || basename == "prn" || basename == "aux" ||
      basename == "nul") {
    return true;
  }
  return basename.size() == 4U &&
         (basename.compare(0U, 3U, "com") == 0 ||
          basename.compare(0U, 3U, "lpt") == 0) &&
         basename[3] >= '1' && basename[3] <= '9';
}

/**
 * @brief Requires one graph-document cache location to be a safe leaf.
 * @param location Untrusted configured location bytes.
 * @return Validated filesystem leaf.
 * @throws GraphError with `InvalidParameter` for empty, oversized, rooted,
 * traversing, multi-component, non-ASCII/non-allowlisted, Windows-root-like,
 * reserved, or ambiguous names.
 * @throws std::bad_alloc when path ownership cannot allocate.
 * @note The portable allowlist is exactly ASCII letters, digits, dot,
 * underscore, and hyphen. It therefore rejects every separator, control,
 * Windows-illegal punctuation, Unicode normalization alias, and superscript
 * device-name spelling identically on every platform.
 */
fs::path validate_graph_cache_location(const std::string& location) {
  const bool invalid_character =
      std::any_of(location.begin(), location.end(), [](char character) {
        const unsigned char byte = static_cast<unsigned char>(character);
        const bool ascii_letter = (byte >= static_cast<unsigned char>('A') &&
                                   byte <= static_cast<unsigned char>('Z')) ||
                                  (byte >= static_cast<unsigned char>('a') &&
                                   byte <= static_cast<unsigned char>('z'));
        const bool ascii_digit = byte >= static_cast<unsigned char>('0') &&
                                 byte <= static_cast<unsigned char>('9');
        return !ascii_letter && !ascii_digit && character != '.' &&
               character != '_' && character != '-';
      });
  if (location.empty() || location.size() > kMaximumGraphCacheLocationBytes ||
      invalid_character) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Graph cache location must be one safe file leaf.");
  }
  const fs::path candidate(location);
  if (candidate.is_absolute() || candidate.has_root_name() ||
      candidate.has_root_directory() || !candidate.has_filename() ||
      candidate.filename() != candidate || candidate == "." ||
      candidate == ".." || location.back() == ' ' || location.back() == '.' ||
      graph_cache_leaf_has_reserved_device_name(location)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Graph cache location must be one safe file leaf.");
  }
  return candidate;
}

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
  const fs::path location = validate_graph_cache_location(entry.location);
  GraphCacheArtifactPaths paths;
  paths.directory = graph.cache_root / std::to_string(node.id);
  paths.image_projection = paths.directory / location;
  paths.metadata = paths.image_projection;
  paths.metadata.replace_extension(".yml");
  paths.value_archive = paths.image_projection;
  paths.value_archive += ".values";
  paths.manifest = paths.image_projection;
  paths.manifest += ".manifest";
  paths.image_leaf = paths.image_projection.filename().string();
  paths.metadata_leaf = paths.metadata.filename().string();
  paths.archive_leaf = paths.value_archive.filename().string();
  paths.manifest_leaf = paths.manifest.filename().string();
  const std::array<std::string, 4U> leaves{
      paths.image_leaf, paths.metadata_leaf, paths.archive_leaf,
      paths.manifest_leaf};
  for (std::size_t left = 0U; left < leaves.size(); ++left) {
    for (std::size_t right = left + 1U; right < leaves.size(); ++right) {
      if (leaves[left] == leaves[right]) {
        throw GraphError(GraphErrc::InvalidParameter,
                         "Graph cache transaction paths alias one another.");
      }
    }
  }
  return paths;
}

/**
 * @brief Creates one unpredictable graph-cache writer generation.
 * @return Nonzero 128-bit generation owned by one prepared transaction.
 * @throws std::runtime_error when the platform cryptographic generator fails.
 * @note The generation is persistent join metadata, not a runtime Value,
 * allocation, revision, path, durability receipt, or security capability.
 */
GraphCacheGeneration make_graph_cache_generation() {
  GraphCacheGeneration generation;
  if (RAND_bytes(reinterpret_cast<unsigned char*>(generation.data()),
                 static_cast<int>(generation.size())) != 1) {
    throw std::runtime_error("Graph cache generation randomization failed.");
  }
  if (std::all_of(generation.begin(), generation.end(),
                  [](std::byte byte) { return byte == std::byte{0}; })) {
    throw std::runtime_error("Graph cache generation was invalid.");
  }
  return generation;
}

/**
 * @brief Encodes one binary cache generation as canonical lowercase hex.
 * @param generation Exact nonzero binary generation.
 * @return Fixed 32-byte owner join carried by every archive envelope.
 * @throws std::bad_alloc when string ownership cannot allocate.
 */
std::string graph_cache_generation_text(
    const GraphCacheGeneration& generation) {
  constexpr std::array<char, 16U> kHex{'0', '1', '2', '3', '4', '5', '6', '7',
                                       '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string result(generation.size() * 2U, '\0');
  for (std::size_t index = 0U; index < generation.size(); ++index) {
    const unsigned int byte = std::to_integer<unsigned int>(generation[index]);
    result[index * 2U] = kHex[byte >> 4U];
    result[index * 2U + 1U] = kHex[byte & 0x0fU];
  }
  return result;
}

/**
 * @brief Validates one immutable GraphCache-specific resource configuration.
 * @param limits Candidate constructor limits.
 * @return Nothing after individual and aggregate bounds agree.
 * @throws std::invalid_argument for zero, excessive, or incoherent limits.
 * @note The public artifact bound remains only a framing ceiling; product
 * GraphCache always applies these lower frozen allocation and disk limits.
 */
void validate_graph_cache_resource_limits(
    const GraphCacheResourceLimits& limits) {
  constexpr std::uint64_t kMaximumPublicArchiveBytes =
      kMaximumValueArtifactPayloadBytes +
      static_cast<std::uint64_t>(kMaximumValueArtifactMetadataBytes);
  if (limits.maximum_archive_bytes == 0U ||
      limits.maximum_archive_bytes > kMaximumPublicArchiveBytes ||
      limits.maximum_metadata_bytes == 0U ||
      limits.maximum_metadata_bytes > kMaximumGraphCacheMetadataBytes ||
      limits.maximum_projection_bytes == 0U ||
      limits.maximum_projection_bytes > kMaximumPublicArchiveBytes ||
      limits.maximum_transaction_bytes == 0U ||
      limits.maximum_transaction_bytes < limits.maximum_archive_bytes ||
      limits.maximum_transaction_bytes < limits.maximum_metadata_bytes ||
      limits.maximum_transaction_bytes >
          kMaximumPublicArchiveBytes + kMaximumGraphCacheMetadataBytes) {
    throw std::invalid_argument(
        "GraphCacheService resource limits are invalid.");
  }
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
 * @param limits Frozen GraphCache-specific resource boundaries.
 * @return Nothing after complete validation.
 * @throws GraphError with `InvalidParameter` for malformed facts.
 */
void validate_graph_cache_manifest(const GraphCacheManifest& manifest,
                                   const GraphCacheResourceLimits& limits) {
  const bool has_metadata =
      (manifest.flags & kGraphCacheManifestHasMetadata) != 0U;
  const ArtifactPayloadDigest empty_digest;
  const bool zero_generation =
      std::all_of(manifest.generation.begin(), manifest.generation.end(),
                  [](std::byte byte) { return byte == std::byte{0}; });
  const bool transaction_overflow =
      manifest.metadata.byte_size >
      std::numeric_limits<std::uint64_t>::max() - manifest.archive.byte_size;
  const std::uint64_t transaction_bytes =
      transaction_overflow
          ? std::numeric_limits<std::uint64_t>::max()
          : manifest.archive.byte_size + manifest.metadata.byte_size;
  if (manifest.structural_version != kGraphCacheManifestVersion ||
      manifest.archive_version != kNamedValueArtifactSetArchiveVersion ||
      (manifest.flags & ~kGraphCacheManifestHasMetadata) != 0U ||
      manifest.value_count > kMaximumNamedValueArtifacts ||
      manifest.parameter_count > kMaximumNamedValueArtifacts ||
      zero_generation || manifest.archive.byte_size == 0U ||
      manifest.archive.byte_size > limits.maximum_archive_bytes ||
      transaction_overflow ||
      transaction_bytes > limits.maximum_transaction_bytes ||
      (has_metadata &&
       (manifest.parameter_count == 0U || manifest.metadata.byte_size == 0U ||
        manifest.metadata.byte_size > limits.maximum_metadata_bytes)) ||
      (!has_metadata &&
       (manifest.parameter_count != 0U || manifest.metadata.byte_size != 0U ||
        !(manifest.metadata.digest == empty_digest)))) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Graph cache manifest facts or resource limits are "
                     "invalid.");
  }
}

/**
 * @brief Encodes one validated graph-cache manifest canonically.
 * @param manifest Complete detached transaction record.
 * @param limits Frozen GraphCache-specific resource boundaries.
 * @return Exact fixed-length version-two bytes.
 * @throws GraphError for malformed facts and std::bad_alloc for allocation.
 */
std::vector<std::byte> encode_graph_cache_manifest(
    const GraphCacheManifest& manifest,
    const GraphCacheResourceLimits& limits) {
  validate_graph_cache_manifest(manifest, limits);
  std::vector<std::byte> bytes;
  bytes.reserve(8U + 5U * 4U + kGraphCacheGenerationBytes + 2U * 8U + 2U * 32U);
  bytes.insert(bytes.end(), kGraphCacheManifestMagic.begin(),
               kGraphCacheManifestMagic.end());
  append_cache_u32(&bytes, manifest.structural_version);
  append_cache_u32(&bytes, manifest.archive_version);
  append_cache_u32(&bytes, manifest.flags);
  append_cache_u32(&bytes, manifest.value_count);
  append_cache_u32(&bytes, manifest.parameter_count);
  bytes.insert(bytes.end(), manifest.generation.begin(),
               manifest.generation.end());
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
 * @param limits Frozen GraphCache-specific resource boundaries.
 * @return Detached validated transaction record.
 * @throws GraphError with `InvalidParameter` for malformed framing or facts.
 */
GraphCacheManifest decode_graph_cache_manifest(
    const std::vector<std::byte>& bytes,
    const GraphCacheResourceLimits& limits) {
  constexpr std::size_t kEncodedSize =
      8U + 5U * 4U + kGraphCacheGenerationBytes + 2U * 8U + 2U * 32U;
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
  std::memcpy(manifest.generation.data(), bytes.data() + offset,
              manifest.generation.size());
  offset += manifest.generation.size();
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
  validate_graph_cache_manifest(manifest, limits);
  return manifest;
}

/**
 * @brief Incremental SHA-256 owner used by bounded descriptor reads.
 * @throws std::bad_alloc when OpenSSL context allocation fails.
 * @throws std::runtime_error when digest initialization fails.
 * @note The helper retains no payload bytes and therefore allows manifest
 * capture and codec-file validation to stream with constant memory.
 */
class GraphCacheDigestBuilder final {
 public:
  /** @brief Allocates and initializes one SHA-256 context. */
  GraphCacheDigestBuilder() : context_(EVP_MD_CTX_new(), &EVP_MD_CTX_free) {
    if (!context_) {
      throw std::bad_alloc();
    }
    if (EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1) {
      throw std::runtime_error("Graph cache SHA-256 initialization failed.");
    }
  }

  /**
   * @brief Prevents copying one mutable one-use digest context.
   * @param other Unused source because construction is forbidden.
   * @throws Nothing; this operation is deleted.
   */
  GraphCacheDigestBuilder(const GraphCacheDigestBuilder& other) = delete;

  /**
   * @brief Prevents assigning one mutable one-use digest context.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing; this operation is deleted.
   */
  GraphCacheDigestBuilder& operator=(const GraphCacheDigestBuilder& other) =
      delete;

  /**
   * @brief Extends the digest with one exact byte span.
   * @param bytes Span start, null only when `size` is zero.
   * @param size Exact byte count.
   * @return Nothing.
   * @throws std::runtime_error when OpenSSL update fails.
   */
  void update(const std::byte* bytes, std::size_t size) {
    if (size == 0U) {
      return;
    }
    if (EVP_DigestUpdate(context_.get(), bytes, size) != 1) {
      throw std::runtime_error("Graph cache SHA-256 update failed.");
    }
  }

  /**
   * @brief Finalizes this one-use digest builder.
   * @return Exact SHA-256 digest.
   * @throws std::runtime_error when OpenSSL finalization fails.
   */
  ArtifactPayloadDigest finish() {
    ArtifactPayloadDigest digest;
    unsigned int size = 0U;
    if (EVP_DigestFinal_ex(
            context_.get(),
            reinterpret_cast<unsigned char*>(digest.bytes.data()),
            &size) != 1 ||
        size != digest.bytes.size()) {
      throw std::runtime_error("Graph cache SHA-256 finalization failed.");
    }
    return digest;
  }

 private:
  /** @brief Unique OpenSSL context owner. */
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
};

/**
 * @brief Detached bytes plus the identity computed during the same read.
 * @throws std::bad_alloc when byte ownership allocates.
 * @note Digesting occurs while the one final byte owner is filled; no second
 * payload-proportional buffer or post-read hash copy is created.
 */
struct GraphCacheFileRead final {
  /** @brief Exact bounded detached bytes. */
  std::vector<std::byte> bytes;
  /** @brief Size and SHA-256 computed from those exact bytes. */
  CacheArtifactFileRecord record;
};

/**
 * @brief Binds one raw file identity to an exact writer generation.
 * @param record Raw size and SHA-256 over file bytes.
 * @param generation Random generation repeated by the archive envelopes.
 * @return Same size plus SHA-256 over generation, canonical size, and raw
 * digest.
 * @throws std::runtime_error when incremental hashing fails.
 * @note Both archive and metadata manifest records cross this function. The
 * metadata codec therefore needs no reserved business key, while a mixed
 * generation cannot reuse either file's otherwise valid raw digest.
 */
CacheArtifactFileRecord bind_graph_cache_file_record(
    const CacheArtifactFileRecord& record,
    const GraphCacheGeneration& generation) {
  std::array<std::byte, 8U> encoded_size{};
  for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
    encoded_size[shift / 8U] =
        std::byte{static_cast<unsigned char>(record.byte_size >> shift)};
  }
  GraphCacheDigestBuilder digest;
  digest.update(generation.data(), generation.size());
  digest.update(encoded_size.data(), encoded_size.size());
  digest.update(record.digest.bytes.data(), record.digest.bytes.size());
  return {record.byte_size, digest.finish()};
}

#if !defined(_WIN32)
/**
 * @brief Unique POSIX descriptor used by graph-cache directory capabilities.
 * @throws Nothing for construction, movement, and destruction.
 */
class GraphCacheDescriptor final {
 public:
  /** @brief Takes one descriptor or the invalid sentinel. */
  explicit GraphCacheDescriptor(int descriptor = -1) noexcept
      : descriptor_(descriptor) {}
  /** @brief Closes the exact owned descriptor. */
  ~GraphCacheDescriptor() noexcept {
    if (descriptor_ >= 0) {
      (void)::close(descriptor_);
    }
  }
  /**
   * @brief Prevents duplicate native descriptor ownership.
   * @param other Unused source because construction is forbidden.
   * @throws Nothing; this operation is deleted.
   */
  GraphCacheDescriptor(const GraphCacheDescriptor& other) = delete;
  /**
   * @brief Prevents duplicate native descriptor assignment.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing; this operation is deleted.
   */
  GraphCacheDescriptor& operator=(const GraphCacheDescriptor& other) = delete;
  /**
   * @brief Moves exact native descriptor ownership.
   * @param other Source invalidated after transfer.
   * @throws Nothing.
   */
  GraphCacheDescriptor(GraphCacheDescriptor&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}
  /** @brief Returns the borrowed native descriptor. */
  int get() const noexcept { return descriptor_; }

 private:
  /** @brief Owned descriptor, or -1. */
  int descriptor_ = -1;
};

/**
 * @brief Throws one path-attributed filesystem error for current errno.
 * @param operation Human-readable failing operation.
 * @param path Controlled path associated with the descriptor operation.
 * @throws std::filesystem::filesystem_error unconditionally.
 */
[[noreturn]] void throw_graph_cache_errno(const char* operation,
                                          const fs::path& path) {
  throw fs::filesystem_error(operation, path,
                             std::error_code(errno, std::generic_category()));
}

/**
 * @brief Requires a no-follow descriptor to be one private regular file.
 * @param descriptor Open candidate descriptor.
 * @param path Diagnostic controlled path.
 * @return Exact stat snapshot.
 * @throws GraphError for nonregular, linked, invalid-size, or foreign-owner
 * files; filesystem_error when fstat fails.
 * @note One-link ownership prevents a graph cache write from truncating an
 * outside hard-link alias. Current effective uid owns every accepted leaf.
 */
struct stat graph_cache_regular_stat(int descriptor, const fs::path& path) {
  struct stat value{};
  if (::fstat(descriptor, &value) != 0) {
    throw_graph_cache_errno("Could not stat graph cache leaf", path);
  }
  if (!S_ISREG(value.st_mode) || value.st_nlink != 1 || value.st_size < 0 ||
      value.st_uid != ::geteuid()) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "Graph cache leaf must be one owned regular non-aliased file.");
  }
  return value;
}
#endif

/**
 * @brief Holds one verified numeric node directory below a cache root.
 *
 * @throws Filesystem, validation, and allocation exceptions from `open`.
 * @note POSIX builds retain root/node directory descriptors and perform every
 * owned archive/manifest read, write, and delete with no-follow `*at` calls.
 * Windows builds retain only a typed fail-closed stub and call no filesystem
 * API. Codec path calls on POSIX are bracketed by directory/leaf identity
 * validation because those dependency-neutral interfaces currently accept
 * paths rather than native capabilities.
 */
class GraphCacheNodeDirectory final {
 public:
  /**
   * @brief Opens or creates one verified numeric node directory.
   * @param root Configured graph cache root.
   * @param node_id Stable graph node id.
   * @param create Whether absent root/node directories may be created.
   * @return Held directory, or nullopt when absent and creation is disabled.
   * @throws GraphError for a symlink or non-directory authority boundary.
   * @throws Filesystem or allocation exceptions unchanged.
   */
  static std::optional<GraphCacheNodeDirectory> open(const fs::path& root,
                                                     int node_id, bool create) {
    if (root.empty()) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Graph cache root must be nonempty.");
    }
    const std::string node_leaf = std::to_string(node_id);
#if !defined(_WIN32)
    if (create) {
      fs::create_directories(root);
    } else if (!fs::exists(root)) {
      return std::nullopt;
    }
    const int raw_root =
        ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (raw_root < 0) {
      if (errno == ELOOP || errno == ENOTDIR) {
        throw GraphError(GraphErrc::InvalidParameter,
                         "Graph cache root must not be a symlink.");
      }
      throw_graph_cache_errno("Could not open graph cache root", root);
    }
    GraphCacheDescriptor root_descriptor(raw_root);
    struct stat root_stat{};
    if (::fstat(root_descriptor.get(), &root_stat) != 0) {
      throw_graph_cache_errno("Could not stat graph cache root", root);
    }
    if (!S_ISDIR(root_stat.st_mode)) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Graph cache root must be a directory.");
    }
    if (create &&
        ::mkdirat(root_descriptor.get(), node_leaf.c_str(), 0700) != 0 &&
        errno != EEXIST) {
      throw_graph_cache_errno("Could not create graph cache node directory",
                              root / node_leaf);
    }
    const int raw_node =
        ::openat(root_descriptor.get(), node_leaf.c_str(),
                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (raw_node < 0) {
      if (!create && errno == ENOENT) {
        return std::nullopt;
      }
      if (errno == ELOOP || errno == ENOTDIR) {
        throw GraphError(
            GraphErrc::InvalidParameter,
            "Graph cache node path must be a real child directory.");
      }
      throw_graph_cache_errno("Could not open graph cache node directory",
                              root / node_leaf);
    }
    GraphCacheDescriptor node_descriptor(raw_node);
    struct stat node_stat{};
    if (::fstat(node_descriptor.get(), &node_stat) != 0) {
      throw_graph_cache_errno("Could not stat graph cache node directory",
                              root / node_leaf);
    }
    if (!S_ISDIR(node_stat.st_mode)) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Graph cache node authority is not a directory.");
    }
    return GraphCacheNodeDirectory(root, node_leaf, std::move(root_descriptor),
                                   std::move(node_descriptor), root_stat,
                                   node_stat);
#else
    (void)node_leaf;
    (void)create;
    throw_graph_cache_disk_persistence_unsupported();
#endif
  }

  /**
   * @brief Prevents duplicate directory-capability ownership.
   * @param other Unused source because construction is forbidden.
   * @throws Nothing; this operation is deleted.
   */
  GraphCacheNodeDirectory(const GraphCacheNodeDirectory& other) = delete;
  /**
   * @brief Prevents duplicate directory-capability assignment.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing; this operation is deleted.
   */
  GraphCacheNodeDirectory& operator=(const GraphCacheNodeDirectory& other) =
      delete;
  /**
   * @brief Moves the complete verified directory capability.
   * @param other Source invalidated after transfer.
   * @throws Nothing.
   */
  GraphCacheNodeDirectory(GraphCacheNodeDirectory&& other) noexcept = default;

  /** @brief Returns the lexical path supplied to codec interfaces. */
  const fs::path& path() const noexcept { return path_; }

  /**
   * @brief Returns a controlled codec path for one already validated leaf.
   * @param leaf Single safe sibling filename.
   * @return Lexical node-directory child path.
   * @throws std::bad_alloc when path ownership cannot allocate.
   */
  fs::path leaf_path(const std::string& leaf) const { return path_ / leaf; }

  /**
   * @brief Reports whether one leaf exists while rejecting unsafe types.
   * @param leaf Single safe sibling filename.
   * @return True only for one current owned regular one-link file.
   * @throws GraphError for symlink, directory, device, fifo, or hard-link
   * alias; filesystem errors unchanged.
   */
  bool leaf_exists(const std::string& leaf) const {
#if !defined(_WIN32)
    struct stat value{};
    if (::fstatat(node_descriptor_.get(), leaf.c_str(), &value,
                  AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) {
        return false;
      }
      throw_graph_cache_errno("Could not observe graph cache leaf",
                              leaf_path(leaf));
    }
    if (!S_ISREG(value.st_mode) || value.st_nlink != 1 ||
        value.st_uid != ::geteuid()) {
      throw GraphError(
          GraphErrc::InvalidParameter,
          "Graph cache leaf must be one owned regular non-aliased file.");
    }
    return true;
#else
    (void)leaf;
    throw_graph_cache_disk_persistence_unsupported();
#endif
  }

  /**
   * @brief Reads one bounded leaf into its only payload-proportional owner.
   * @param leaf Single safe sibling filename.
   * @param maximum_bytes Frozen per-file maximum checked before allocation.
   * @param expected_bytes Optional exact manifest-declared length.
   * @param reject_sparse Whether sparse physical storage is forbidden.
   * @param allocation_event Whether to publish the test-only archive
   * preallocation checkpoint after every stat check succeeds.
   * @return Exact bytes and digest computed during the same descriptor read.
   * @throws GraphError for type, identity, size, sparse, or digest-boundary
   * drift; filesystem, allocation, and hashing exceptions unchanged.
   */
  GraphCacheFileRead read_leaf(
      const std::string& leaf, std::uint64_t maximum_bytes,
      std::optional<std::uint64_t> expected_bytes = std::nullopt,
      bool reject_sparse = true, bool allocation_event = false) const {
#if !defined(_WIN32)
    const fs::path diagnostic_path = leaf_path(leaf);
    const int raw = ::openat(node_descriptor_.get(), leaf.c_str(),
                             O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (raw < 0) {
      if (errno == ELOOP || errno == ENOTDIR) {
        throw GraphError(GraphErrc::InvalidParameter,
                         "Graph cache read refuses a symlink leaf.");
      }
      throw_graph_cache_errno("Could not open graph cache leaf",
                              diagnostic_path);
    }
    GraphCacheDescriptor descriptor(raw);
    const struct stat before =
        graph_cache_regular_stat(descriptor.get(), diagnostic_path);
    const std::uintmax_t size = static_cast<std::uintmax_t>(before.st_size);
    if (size > maximum_bytes ||
        (expected_bytes.has_value() && size != *expected_bytes) ||
        size > std::numeric_limits<std::size_t>::max()) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Graph cache payload size exceeds its frozen limit.");
    }
    constexpr std::uintmax_t kStatBlockBytes = 512U;
    if (reject_sparse && size != 0U && before.st_blocks >= 0) {
      const std::uintmax_t blocks =
          static_cast<std::uintmax_t>(before.st_blocks);
      if (blocks <=
              std::numeric_limits<std::uintmax_t>::max() / kStatBlockBytes &&
          blocks * kStatBlockBytes < size) {
        throw GraphError(GraphErrc::InvalidParameter,
                         "Graph cache payload contains sparse storage.");
      }
    }
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
    if (allocation_event) {
      testing::notify_graph_cache_service_test_hook(
          testing::GraphCacheServiceTestEvent::ArchiveAllocationApproved,
          path_);
    }
#else
    (void)allocation_event;
#endif
    GraphCacheFileRead result;
    result.bytes.resize(static_cast<std::size_t>(size));
    GraphCacheDigestBuilder digest;
    std::size_t offset = 0U;
    while (offset < result.bytes.size()) {
      const std::size_t chunk = std::min(
          result.bytes.size() - offset,
          static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
      const ssize_t count =
          ::read(descriptor.get(), result.bytes.data() + offset, chunk);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        throw GraphError(GraphErrc::Io,
                         "Graph cache payload shortened during read.");
      }
      digest.update(result.bytes.data() + offset,
                    static_cast<std::size_t>(count));
      offset += static_cast<std::size_t>(count);
    }
    const struct stat after =
        graph_cache_regular_stat(descriptor.get(), diagnostic_path);
    struct stat named{};
    if (::fstatat(node_descriptor_.get(), leaf.c_str(), &named,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
        after.st_size != before.st_size || named.st_dev != before.st_dev ||
        named.st_ino != before.st_ino || named.st_size != before.st_size) {
      throw GraphError(GraphErrc::Io,
                       "Graph cache payload identity changed during read.");
    }
    result.record = {static_cast<std::uint64_t>(result.bytes.size()),
                     digest.finish()};
    return result;
#else
    (void)leaf;
    (void)maximum_bytes;
    (void)expected_bytes;
    (void)reject_sparse;
    (void)allocation_event;
    throw_graph_cache_disk_persistence_unsupported();
#endif
  }

  /**
   * @brief Streams one existing leaf into a size/digest record.
   * @param leaf Single safe sibling filename.
   * @param maximum_bytes Frozen maximum accepted length.
   * @param expected_bytes Optional exact expected length.
   * @param reject_sparse Whether physical holes are forbidden.
   * @return Exact constant-memory record.
   * @throws The same validation/filesystem/hash exceptions as `read_leaf`.
   */
  CacheArtifactFileRecord capture_leaf_record(
      const std::string& leaf, std::uint64_t maximum_bytes,
      std::optional<std::uint64_t> expected_bytes = std::nullopt,
      bool reject_sparse = true) const {
#if !defined(_WIN32)
    const fs::path diagnostic_path = leaf_path(leaf);
    const int raw = ::openat(node_descriptor_.get(), leaf.c_str(),
                             O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (raw < 0) {
      if (errno == ELOOP || errno == ENOTDIR) {
        throw GraphError(GraphErrc::InvalidParameter,
                         "Graph cache capture refuses a symlink leaf.");
      }
      throw_graph_cache_errno("Could not open graph cache leaf",
                              diagnostic_path);
    }
    GraphCacheDescriptor descriptor(raw);
    const struct stat before =
        graph_cache_regular_stat(descriptor.get(), diagnostic_path);
    const std::uintmax_t size = static_cast<std::uintmax_t>(before.st_size);
    if (size > maximum_bytes ||
        (expected_bytes.has_value() && size != *expected_bytes)) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Graph cache payload size exceeds its frozen limit.");
    }
    constexpr std::uintmax_t kStatBlockBytes = 512U;
    if (reject_sparse && size != 0U && before.st_blocks >= 0) {
      const std::uintmax_t blocks =
          static_cast<std::uintmax_t>(before.st_blocks);
      if (blocks <=
              std::numeric_limits<std::uintmax_t>::max() / kStatBlockBytes &&
          blocks * kStatBlockBytes < size) {
        throw GraphError(GraphErrc::InvalidParameter,
                         "Graph cache payload contains sparse storage.");
      }
    }
    GraphCacheDigestBuilder digest;
    std::array<std::byte, 64U * 1024U> buffer;
    std::uint64_t total = 0U;
    while (true) {
      const ssize_t count = ::read(descriptor.get(), buffer.data(),
                                   static_cast<std::size_t>(buffer.size()));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0) {
        throw_graph_cache_errno("Could not read graph cache leaf",
                                diagnostic_path);
      }
      if (count == 0) {
        break;
      }
      digest.update(buffer.data(), static_cast<std::size_t>(count));
      total += static_cast<std::uint64_t>(count);
    }
    const struct stat after =
        graph_cache_regular_stat(descriptor.get(), diagnostic_path);
    struct stat named{};
    if (::fstatat(node_descriptor_.get(), leaf.c_str(), &named,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        total != size || after.st_dev != before.st_dev ||
        after.st_ino != before.st_ino || after.st_size != before.st_size ||
        named.st_dev != before.st_dev || named.st_ino != before.st_ino ||
        named.st_size != before.st_size) {
      throw GraphError(GraphErrc::Io,
                       "Graph cache payload identity changed during capture.");
    }
    return {total, digest.finish()};
#else
    (void)leaf;
    (void)maximum_bytes;
    (void)expected_bytes;
    (void)reject_sparse;
    throw_graph_cache_disk_persistence_unsupported();
#endif
  }

  /**
   * @brief Writes one owned archive/manifest leaf without following aliases.
   * @param leaf Single safe sibling filename.
   * @param bytes Complete bounded bytes.
   * @return Exact persisted record after identity revalidation.
   * @throws GraphError for symlink, nonregular, hard-linked, or identity drift.
   * @throws Filesystem, allocation, or hashing exceptions unchanged.
   * @note Existing files are opened without truncation, validated by fd, then
   * truncated. The method remains non-durable and performs no fsync/rename.
   */
  CacheArtifactFileRecord write_leaf(
      const std::string& leaf, const std::vector<std::byte>& bytes) const {
#if !defined(_WIN32)
    const fs::path diagnostic_path = leaf_path(leaf);
    const int raw = ::openat(node_descriptor_.get(), leaf.c_str(),
                             O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (raw < 0) {
      if (errno == ELOOP || errno == ENOTDIR) {
        throw GraphError(GraphErrc::InvalidParameter,
                         "Graph cache write refuses a symlink leaf.");
      }
      throw_graph_cache_errno("Could not open graph cache leaf for writing",
                              diagnostic_path);
    }
    GraphCacheDescriptor descriptor(raw);
    const struct stat before =
        graph_cache_regular_stat(descriptor.get(), diagnostic_path);
    if (::ftruncate(descriptor.get(), 0) != 0) {
      throw_graph_cache_errno("Could not truncate graph cache leaf",
                              diagnostic_path);
    }
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
      const std::size_t chunk = std::min(
          bytes.size() - offset,
          static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
      const ssize_t count =
          ::write(descriptor.get(), bytes.data() + offset, chunk);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0) {
        throw_graph_cache_errno("Could not write graph cache leaf",
                                diagnostic_path);
      }
      if (count == 0) {
        throw GraphError(GraphErrc::Io,
                         "Graph cache leaf write made no progress.");
      }
      offset += static_cast<std::size_t>(count);
    }
    const struct stat after =
        graph_cache_regular_stat(descriptor.get(), diagnostic_path);
    struct stat named{};
    if (after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
        static_cast<std::uintmax_t>(after.st_size) != bytes.size() ||
        ::fstatat(node_descriptor_.get(), leaf.c_str(), &named,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        named.st_dev != after.st_dev || named.st_ino != after.st_ino ||
        named.st_size != after.st_size) {
      throw GraphError(GraphErrc::Io,
                       "Graph cache written leaf identity changed.");
    }
    return {static_cast<std::uint64_t>(bytes.size()),
            compute_artifact_payload_digest(bytes)};
#else
    (void)leaf;
    (void)bytes;
    throw_graph_cache_disk_persistence_unsupported();
#endif
  }

  /**
   * @brief Removes one existing regular one-link leaf without following it.
   * @param leaf Single safe sibling filename.
   * @return True when removed; false when absent.
   * @throws GraphError for symlink, nonregular, or hard-link alias.
   * @throws Filesystem errors unchanged.
   */
  bool remove_leaf_if_present(const std::string& leaf) const {
#if !defined(_WIN32)
    if (!leaf_exists(leaf)) {
      return false;
    }
    if (::unlinkat(node_descriptor_.get(), leaf.c_str(), 0) != 0) {
      throw_graph_cache_errno("Could not remove graph cache leaf",
                              leaf_path(leaf));
    }
    return true;
#else
    (void)leaf;
    throw_graph_cache_disk_persistence_unsupported();
#endif
  }

  /**
   * @brief Removes the held numeric node directory only when exactly empty.
   * @return True when the directory was removed; false when nonempty/already
   * absent.
   * @throws GraphError for directory identity replacement.
   * @throws Filesystem or allocation exceptions unchanged.
   */
  bool remove_if_empty() {
#if !defined(_WIN32)
    const int duplicate = ::dup(node_descriptor_.get());
    if (duplicate < 0) {
      throw_graph_cache_errno("Could not duplicate graph cache directory",
                              path_);
    }
    DIR* stream = ::fdopendir(duplicate);
    if (stream == nullptr) {
      (void)::close(duplicate);
      throw_graph_cache_errno("Could not enumerate graph cache directory",
                              path_);
    }
    bool empty = true;
    errno = 0;
    while (const dirent* entry = ::readdir(stream)) {
      const std::string_view name(entry->d_name);
      if (name != "." && name != "..") {
        empty = false;
        break;
      }
    }
    const int enumeration_error = errno;
    (void)::closedir(stream);
    if (enumeration_error != 0) {
      errno = enumeration_error;
      throw_graph_cache_errno("Could not enumerate graph cache directory",
                              path_);
    }
    if (!empty) {
      return false;
    }
    validate_binding();
    if (::unlinkat(root_descriptor_.get(), node_leaf_.c_str(), AT_REMOVEDIR) !=
        0) {
      if (errno == ENOTEMPTY || errno == EEXIST || errno == ENOENT) {
        return false;
      }
      throw_graph_cache_errno("Could not remove graph cache node directory",
                              path_);
    }
    return true;
#else
    throw_graph_cache_disk_persistence_unsupported();
#endif
  }

  /**
   * @brief Revalidates root and numeric child paths against held descriptors.
   * @return Nothing after exact identity agreement.
   * @throws GraphError when either lexical path was replaced.
   * @throws Filesystem errors unchanged.
   * @note Called immediately before and after path-only codec invocations to
   * narrow their unavoidable same-uid POSIX compare/use race.
   */
  void validate_binding() const {
#if !defined(_WIN32)
    struct stat root_named{};
    if (::lstat(root_.c_str(), &root_named) != 0) {
      throw_graph_cache_errno("Could not revalidate graph cache root", root_);
    }
    struct stat node_named{};
    if (::fstatat(root_descriptor_.get(), node_leaf_.c_str(), &node_named,
                  AT_SYMLINK_NOFOLLOW) != 0) {
      throw_graph_cache_errno("Could not revalidate graph cache node", path_);
    }
    if (!S_ISDIR(root_named.st_mode) || !S_ISDIR(node_named.st_mode) ||
        root_named.st_dev != root_stat_.st_dev ||
        root_named.st_ino != root_stat_.st_ino ||
        node_named.st_dev != node_stat_.st_dev ||
        node_named.st_ino != node_stat_.st_ino) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Graph cache directory identity was replaced.");
    }
#else
    throw_graph_cache_disk_persistence_unsupported();
#endif
  }

 private:
#if !defined(_WIN32)
  /**
   * @brief Constructs one fully validated POSIX directory capability.
   * @param root Lexical configured root.
   * @param node_leaf Numeric child name.
   * @param root_descriptor Held root descriptor.
   * @param node_descriptor Held node descriptor.
   * @param root_stat Root identity snapshot.
   * @param node_stat Node identity snapshot.
   * @throws std::bad_alloc from path/string ownership.
   */
  GraphCacheNodeDirectory(fs::path root, std::string node_leaf,
                          GraphCacheDescriptor root_descriptor,
                          GraphCacheDescriptor node_descriptor,
                          struct stat root_stat, struct stat node_stat)
      : root_(std::move(root)),
        path_(root_ / node_leaf),
        node_leaf_(std::move(node_leaf)),
        root_descriptor_(std::move(root_descriptor)),
        node_descriptor_(std::move(node_descriptor)),
        root_stat_(root_stat),
        node_stat_(node_stat) {}

#endif

  /** @brief Lexical configured cache root used by codec paths. */
  fs::path root_;
  /** @brief Lexical numeric node path used by codec paths. */
  fs::path path_;
  /** @brief Stable numeric child leaf below the held root. */
  std::string node_leaf_;
#if !defined(_WIN32)
  /** @brief Held no-follow root directory capability. */
  GraphCacheDescriptor root_descriptor_;
  /** @brief Held no-follow numeric node directory capability. */
  GraphCacheDescriptor node_descriptor_;
  /** @brief Root identity bound at open. */
  struct stat root_stat_{};
  /** @brief Numeric child identity bound at open. */
  struct stat node_stat_{};

#endif
};

/**
 * @brief Requires one observed file record to match a manifest-bound record.
 * @param actual Exact descriptor-read size and digest.
 * @param expected Manifest-declared size and digest.
 * @return Nothing after complete agreement.
 * @throws GraphError with `InvalidParameter` for mismatch.
 */
void validate_cache_file_record(const CacheArtifactFileRecord& actual,
                                const CacheArtifactFileRecord& expected) {
  if (actual.byte_size != expected.byte_size ||
      !(actual.digest == expected.digest)) {
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
 * @param graph Graph whose root participates in deterministic path derivation.
 * @param node Node whose cache destinations are inspected without mutation.
 * @return True when at least one image entry has a nonempty location.
 * @throws GraphError with `InvalidParameter` when any image entry location is
 * empty, rooted, traversing, aliased, or otherwise unsafe.
 * @throws std::bad_alloc when path validation cannot allocate.
 * @note Unsupported cache-entry types retain their historical no-op behavior.
 * Every image entry is validated before Value capture or filesystem effects.
 */
bool has_image_disk_cache_entry(const GraphModel& graph, const Node& node) {
  bool found = false;
  std::unordered_set<std::string> controlled_leaves;
  for (const CacheEntry& entry : node.caches) {
    if (entry.cache_type != "image") {
      continue;
    }
    const GraphCacheArtifactPaths paths =
        graph_cache_artifact_paths(graph, node, entry);
    for (const std::string* leaf :
         {&paths.image_leaf, &paths.metadata_leaf, &paths.archive_leaf,
          &paths.manifest_leaf}) {
      if (!controlled_leaves.insert(portable_graph_cache_leaf_key(*leaf))
               .second) {
        throw GraphError(
            GraphErrc::InvalidParameter,
            "Graph cache entries alias one controlled transaction leaf.");
      }
    }
    found = true;
  }
  return found;
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
 * Explicit empty/whole validity is classified before interpreting the Value,
 * and finite provider-defined canonical-image validity is conservatively
 * incomplete, so provider-incompatible partial output can authorize
 * predecessor cleanup without constructing an ImageView or consulting a
 * provider. Whole provider-defined output remains complete here and therefore
 * reaches the existing unsupported-image preflight.
 */
bool has_complete_hp_cache(const Node& node) {
  const NodeOutput* output = hp_cache_ptr(node);
  if (output == nullptr || !node.hp_region.has_value() ||
      node.hp_region->is_empty()) {
    return false;
  }
  if (node.hp_region->is_whole()) {
    return true;
  }
  if (output->has_image_value() &&
      output->image_value().representation_kind() ==
          ValueRepresentationKind::ProviderDefined) {
    return false;
  }
  return value_region::node_output_region_is_complete(*output, *node.hp_region);
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
  /** @brief Random writer generation joined by archive and manifest. */
  GraphCacheGeneration generation;
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
 * @param generation Random writer generation applied to every envelope.
 * @return Encoded public archive after every payload and digest validates.
 * @throws All capture, digest, provider, bounds, and allocation failures.
 * @note The map is already canonical name order. No archive escapes until all
 * Values have been synchronously copied through checked read leases.
 */
std::vector<std::byte> capture_graph_cache_value_archive(
    const NodeOutput& output, const GraphCacheGeneration& generation) {
  if (output.named_values.size() > kMaximumNamedValueArtifacts) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Graph cache Value count exceeds the portable bound.");
  }
  NamedValueArtifactSet artifacts;
  artifacts.values.reserve(output.named_values.size());
  const std::string generation_text = graph_cache_generation_text(generation);
  for (const auto& [name, value] : output.named_values) {
    ValueArtifact artifact = capture_value_artifact(name, value);
    artifact.envelope.joins.commit_identity = generation_text;
    artifacts.values.push_back(std::move(artifact));
  }
  return encode_named_value_artifact_set(artifacts);
}

/**
 * @brief Prepares one cache save before executor or filesystem side effects.
 * @param graph Graph providing current cache policy and root.
 * @param node Node providing entries and formal HP output.
 * @param cache_precision Precision label retained by image projection policy.
 * @param limits Frozen GraphCache-specific resource boundaries.
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
    const std::string& cache_precision,
    const GraphCacheResourceLimits& limits) {
  if (graph.skip_save_cache() || graph.cache_root.empty() ||
      node.caches.empty() || hp_cache_ptr(node) == nullptr) {
    return std::nullopt;
  }
  if (!has_image_disk_cache_entry(graph, node)) {
    return std::nullopt;
  }

  PreparedGraphCacheSave prepared;
  const NodeOutput& output = *hp_cache_ptr(node);
  try {
    prepared.complete_output = has_complete_hp_cache(node);
    if (prepared.complete_output) {
      validate_image_disk_cache_output(output);
      if (output.data.size() > kMaximumNamedValueArtifacts) {
        throw GraphError(
            GraphErrc::InvalidParameter,
            "Graph cache parameter count exceeds the manifest bound.");
      }
      prepared.value_count =
          static_cast<std::uint32_t>(output.named_values.size());
      prepared.generation = make_graph_cache_generation();
      prepared.value_archive =
          capture_graph_cache_value_archive(output, prepared.generation);
      if (prepared.value_archive.empty() ||
          prepared.value_archive.size() > limits.maximum_archive_bytes ||
          prepared.value_archive.size() > limits.maximum_transaction_bytes) {
        throw GraphError(
            GraphErrc::InvalidParameter,
            "Graph cache archive exceeds its frozen resource limit.");
      }
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
    if (entry.cache_type != "image") {
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
 * @param limits Frozen GraphCache-specific resource boundaries.
 * @return Hit, Miss, or Error attempt with diagnostic details.
 * @throws GraphError with `InvalidParameter` on Windows before path, codec,
 * filesystem, allocation, or diagnostic work.
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
    DataDefinitionRegistry* data_definitions,
    const GraphCacheResourceLimits& limits) {
#if defined(_WIN32)
  throw_graph_cache_disk_persistence_unsupported();
#endif
  const GraphCacheArtifactPaths paths =
      graph_cache_artifact_paths(graph, node, cache_entry);

  try {
    std::optional<GraphCacheNodeDirectory> directory =
        GraphCacheNodeDirectory::open(graph.cache_root, node.id, false);
    if (!directory.has_value()) {
      DiskCacheReadAttempt attempt;
      attempt.result = make_load_result(
          node.id, &cache_entry, paths.image_projection, paths.metadata,
          DiskCacheLoadStatus::Miss, GraphErrc::Unknown,
          "No disk cache transaction exists for configured entry.");
      return attempt;
    }
    const bool has_image_projection = directory->leaf_exists(paths.image_leaf);
    const bool has_metadata_file = directory->leaf_exists(paths.metadata_leaf);
    const bool has_archive_file = directory->leaf_exists(paths.archive_leaf);
    const bool has_manifest_file = directory->leaf_exists(paths.manifest_leaf);
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
    const GraphCacheFileRead initial_manifest = directory->read_leaf(
        paths.manifest_leaf, kMaximumManifestBytes, std::nullopt, true, false);
    const std::vector<std::byte>& initial_manifest_bytes =
        initial_manifest.bytes;
    const GraphCacheManifest manifest =
        decode_graph_cache_manifest(initial_manifest_bytes, limits);
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

    GraphCacheFileRead archive =
        directory->read_leaf(paths.archive_leaf, limits.maximum_archive_bytes,
                             manifest.archive.byte_size, true, true);
    validate_cache_file_record(
        bind_graph_cache_file_record(archive.record, manifest.generation),
        manifest.archive);
    NamedValueArtifactSet artifacts =
        decode_named_value_artifact_set(archive.bytes);
    if (artifacts.values.size() != manifest.value_count) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Graph cache archive count disagrees with manifest.");
    }
    std::vector<std::string> actual_names;
    actual_names.reserve(artifacts.values.size());
    const std::string generation_text =
        graph_cache_generation_text(manifest.generation);
    for (const ValueArtifact& artifact : artifacts.values) {
      if (artifact.envelope.joins.commit_identity != generation_text ||
          artifact.envelope.joins.artifact_identity.has_value() ||
          artifact.envelope.joins.slot_identity.has_value()) {
        throw GraphError(
            GraphErrc::InvalidParameter,
            "Graph cache archive generation disagrees with its manifest.");
      }
      actual_names.push_back(artifact.envelope.output_name);
    }
    if (actual_names != expected_names) {
      return make_schema_shape_miss(
          node.id, cache_entry, paths.image_projection, paths.metadata,
          "Disk-cache archive names do not match the frozen planned Value "
          "schema.");
    }
    std::vector<std::byte>().swap(archive.bytes);

    NodeOutput candidate;
    for (ValueArtifact& artifact : artifacts.values) {
      Value reconstructed =
          reconstruct_value_artifact(artifact, data_definitions);
      candidate.publish_named_value(artifact.envelope.output_name,
                                    std::move(reconstructed));
      for (std::vector<std::byte>& payload : artifact.payloads) {
        std::vector<std::byte>().swap(payload);
      }
    }

    if (manifest_has_metadata) {
      const CacheArtifactFileRecord metadata_before =
          directory->capture_leaf_record(paths.metadata_leaf,
                                         limits.maximum_metadata_bytes,
                                         manifest.metadata.byte_size, true);
      validate_cache_file_record(
          bind_graph_cache_file_record(metadata_before, manifest.generation),
          manifest.metadata);
      directory->validate_binding();
      candidate.data = metadata_codec.read(paths.metadata);
      directory->validate_binding();
      const CacheArtifactFileRecord metadata_after =
          directory->capture_leaf_record(paths.metadata_leaf,
                                         limits.maximum_metadata_bytes,
                                         manifest.metadata.byte_size, true);
      validate_cache_file_record(
          bind_graph_cache_file_record(metadata_after, manifest.generation),
          manifest.metadata);
      if (metadata_after.byte_size != metadata_before.byte_size ||
          !(metadata_after.digest == metadata_before.digest)) {
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

    const GraphCacheFileRead final_manifest =
        directory->read_leaf(paths.manifest_leaf, kMaximumManifestBytes,
                             initial_manifest.record.byte_size, true, false);
    if (final_manifest.bytes != initial_manifest_bytes ||
        !(final_manifest.record.digest == initial_manifest.record.digest)) {
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
 * @param limits Frozen GraphCache-specific resource boundaries.
 * @return Hit/Error for the first existing or failing entry, Miss when all
 * supported entries are absent, or Skipped when no load should be attempted.
 * @throws GraphError with `InvalidParameter` on Windows for a nonempty-root
 * request, before entry inspection or diagnostic construction.
 * @throws std::bad_alloc from diagnostic construction.
 * @note Missing and incompatible entries remain cache misses and do not stop
 * scanning later entries; the last incompatibility reason is retained.
 * Read/parse errors stop immediately to preserve their diagnostics.
 */
DiskCacheReadAttempt read_first_disk_cache_entry(
    const GraphModel& graph, const Node& node,
    const ValueDiskCacheOutputSchema& output_schema,
    const CacheMetadataCodec& metadata_codec,
    DataDefinitionRegistry* data_definitions,
    const GraphCacheResourceLimits& limits) {
  if (graph.cache_root.empty()) {
    return make_skipped_attempt(node.id, "Graph has no disk cache root.");
  }
#if defined(_WIN32)
  throw_graph_cache_disk_persistence_unsupported();
#endif
  if (node.caches.empty()) {
    return make_skipped_attempt(node.id, "Node has no configured cache entry.");
  }

  try {
    if (!has_image_disk_cache_entry(graph, node)) {
      return make_skipped_attempt(node.id,
                                  "No supported image cache entry found.");
    }
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError& error) {
    const auto invalid_entry = std::find_if(
        node.caches.begin(), node.caches.end(),
        [](const CacheEntry& entry) { return entry.cache_type == "image"; });
    if (invalid_entry == node.caches.end()) {
      throw;
    }
    return make_error_attempt(node.id, *invalid_entry, {}, {}, error.code(),
                              error.what());
  }

  bool saw_supported_entry = false;
  DiskCacheReadAttempt last_miss =
      make_skipped_attempt(node.id, "No supported image cache entry found.");
  std::optional<DiskCacheReadAttempt> last_incompatible_miss;
  for (const auto& cache_entry : node.caches) {
    if (cache_entry.cache_type != "image") {
      continue;
    }

    saw_supported_entry = true;
    DiskCacheReadAttempt attempt =
        read_cache_entry(graph, node, cache_entry, output_schema,
                         metadata_codec, data_definitions, limits);
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
void remove_cache_siblings_not_retained(GraphCacheNodeDirectory& directory,
                                        const GraphCacheArtifactPaths& paths,
                                        bool retain_image, bool retain_metadata,
                                        bool retain_transaction) {
  if (!retain_image) {
    (void)directory.remove_leaf_if_present(paths.image_leaf);
  }
  if (!retain_metadata) {
    (void)directory.remove_leaf_if_present(paths.metadata_leaf);
  }
  if (!retain_transaction) {
    (void)directory.remove_leaf_if_present(paths.archive_leaf);
  }
  if (!retain_transaction) {
    (void)directory.remove_leaf_if_present(paths.manifest_leaf);
  }
  (void)directory.remove_if_empty();
}

/**
 * @brief Executes the service-owned cache-save filesystem and codec mechanism.
 * @param graph Read-only Graph policy, cache root, and prepared output owner.
 * @param node Read-only node whose configured cache entries are processed.
 * @param prepared Detached archive, metadata, image policy, and admission
 * estimate captured before side effects.
 * @param image_codec Codec selected and retained by GraphCacheService.
 * @param metadata_codec Named-value codec retained by GraphCacheService.
 * @param limits Frozen GraphCache-specific resource boundaries.
 * @param timing_graph Optional graph-state-only timing sink; null on the
 * independent I/O worker.
 * @throws GraphError with `InvalidParameter` on Windows before path, codec,
 * filesystem, allocation, cleanup, or timing work.
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
                          const GraphCacheResourceLimits& limits,
                          GraphModel* timing_graph) {
#if defined(_WIN32)
  throw_graph_cache_disk_persistence_unsupported();
#endif
  for (const CacheEntry& cache_entry : node.caches) {
    if (cache_entry.cache_type != "image") {
      continue;
    }

    const GraphCacheArtifactPaths paths =
        graph_cache_artifact_paths(graph, node, cache_entry);
    if (!prepared.complete_output) {
      std::optional<GraphCacheNodeDirectory> directory =
          GraphCacheNodeDirectory::open(graph.cache_root, node.id, false);
      if (directory.has_value()) {
        remove_cache_siblings_not_retained(*directory, paths, false, false,
                                           false);
      }
      continue;
    }

    const bool retain_image = prepared.image_projection.has_value();
    const bool retain_metadata = !prepared.parameters.empty();
    std::optional<GraphCacheNodeDirectory> directory =
        GraphCacheNodeDirectory::open(graph.cache_root, node.id, true);
    if (!directory.has_value()) {
      throw GraphError(GraphErrc::Io,
                       "Graph cache node directory could not be created.");
    }

    const auto start_io = std::chrono::high_resolution_clock::now();
    if (retain_image) {
      (void)directory->leaf_exists(paths.image_leaf);
      directory->validate_binding();
      image_codec.encode(paths.image_projection, *prepared.image_projection,
                         *prepared.image_request);
      directory->validate_binding();
      if (directory->leaf_exists(paths.image_leaf)) {
        (void)directory->capture_leaf_record(paths.image_leaf,
                                             limits.maximum_projection_bytes,
                                             std::nullopt, true);
      }
    }
    CacheArtifactFileRecord metadata_record;
    if (retain_metadata) {
      (void)directory->leaf_exists(paths.metadata_leaf);
      directory->validate_binding();
      metadata_codec.write(paths.metadata, prepared.parameters);
      directory->validate_binding();
      metadata_record = directory->capture_leaf_record(
          paths.metadata_leaf, limits.maximum_metadata_bytes, std::nullopt,
          true);
    }
    const CacheArtifactFileRecord archive_record =
        directory->write_leaf(paths.archive_leaf, prepared.value_archive);
    const CacheArtifactFileRecord expected_archive{
        static_cast<std::uint64_t>(prepared.value_archive.size()),
        compute_artifact_payload_digest(prepared.value_archive)};
    validate_cache_file_record(archive_record, expected_archive);
    remove_cache_siblings_not_retained(*directory, paths, retain_image,
                                       retain_metadata, true);

    GraphCacheManifest manifest;
    manifest.value_count = prepared.value_count;
    manifest.generation = prepared.generation;
    const CacheArtifactFileRecord captured_archive =
        directory->capture_leaf_record(paths.archive_leaf,
                                       limits.maximum_archive_bytes,
                                       expected_archive.byte_size, true);
    validate_cache_file_record(captured_archive, expected_archive);
    manifest.archive =
        bind_graph_cache_file_record(captured_archive, prepared.generation);
    if (retain_metadata) {
      manifest.flags |= kGraphCacheManifestHasMetadata;
      manifest.parameter_count =
          static_cast<std::uint32_t>(prepared.parameters.size());
      directory->validate_binding();
      if (metadata_codec.read(paths.metadata) != prepared.parameters) {
        throw GraphError(
            GraphErrc::InvalidParameter,
            "Graph cache metadata codec round-trip changed parameter facts.");
      }
      directory->validate_binding();
      const CacheArtifactFileRecord metadata_after =
          directory->capture_leaf_record(paths.metadata_leaf,
                                         limits.maximum_metadata_bytes,
                                         metadata_record.byte_size, true);
      validate_cache_file_record(metadata_after, metadata_record);
      manifest.metadata =
          bind_graph_cache_file_record(metadata_after, prepared.generation);
    }
    const std::vector<std::byte> manifest_bytes =
        encode_graph_cache_manifest(manifest, limits);
    const CacheArtifactFileRecord manifest_record =
        directory->write_leaf(paths.manifest_leaf, manifest_bytes);
    const GraphCacheFileRead persisted_manifest = directory->read_leaf(
        paths.manifest_leaf, 1024U, manifest_record.byte_size, true, false);
    if (persisted_manifest.bytes != manifest_bytes ||
        !(persisted_manifest.record.digest == manifest_record.digest)) {
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
    DataDefinitionRegistry* data_definitions,
    GraphCacheResourceLimits resource_limits)
    : image_codec_(std::move(image_codec)),
      metadata_codec_(std::move(metadata_codec)),
      resource_limits_(resource_limits),
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
  validate_graph_cache_resource_limits(resource_limits_);
}

/** @copydoc GraphCacheService::require_disk_persistence_supported */
void GraphCacheService::require_disk_persistence_supported() {
#if defined(_WIN32)
  throw_graph_cache_disk_persistence_unsupported();
#endif
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

/** @copydoc GraphCacheService::node_cache_dir */
std::filesystem::path GraphCacheService::node_cache_dir(const GraphModel& graph,
                                                        int node_id) const {
  return graph.cache_root / std::to_string(node_id);
}

/** @copydoc GraphCacheService::save_cache_if_configured */
void GraphCacheService::save_cache_if_configured(
    GraphModel& graph, const Node& node,
    const std::string& cache_precision) const {
  if (graph.skip_save_cache() || graph.cache_root.empty()) {
    return;
  }
  require_disk_persistence_supported();
  const std::shared_ptr<GraphCacheRootCoordination> coordination =
      graph_cache_root_coordination(graph.cache_root);
  GraphCacheRootGuard guard(coordination, graph.cache_root);
  const std::optional<PreparedGraphCacheSave> prepared =
      prepare_graph_cache_save(graph, node, cache_precision, resource_limits_);
  if (!prepared.has_value()) {
    return;
  }
  (void)guard.advance_mutation_epoch();
  save_cache_mechanism(graph, node, *prepared, *image_codec_, *metadata_codec_,
                       resource_limits_, &graph);
}

/** @copydoc GraphCacheService::save_cache_if_configured_via_executor */
void GraphCacheService::save_cache_if_configured_via_executor(
    execution::ComputeIoExecutor& executor,
    const std::shared_ptr<const void>& lifetime_token, GraphModel& graph,
    const Node& node, const std::string& cache_precision) const {
  if (graph.skip_save_cache() || graph.cache_root.empty()) {
    return;
  }
  require_disk_persistence_supported();
  const std::shared_ptr<GraphCacheRootCoordination> coordination =
      graph_cache_root_coordination(graph.cache_root);
  std::optional<PreparedGraphCacheSave> prepared;
  std::uint64_t prepared_epoch = 0U;
  {
    GraphCacheRootGuard guard(coordination, graph.cache_root);
    prepared = prepare_graph_cache_save(graph, node, cache_precision,
                                        resource_limits_);
    if (!prepared.has_value()) {
      return;
    }
    prepared_epoch = guard.advance_mutation_epoch();
    if (!prepared->complete_output) {
      save_cache_mechanism(graph, node, *prepared, *image_codec_,
                           *metadata_codec_, resource_limits_, &graph);
      return;
    }
  }
  const auto retained_prepared =
      std::make_shared<const PreparedGraphCacheSave>(std::move(*prepared));

  const execution::ComputeIoSubmission submission = executor.try_submit(
      retained_prepared->planned_bytes, lifetime_token,
      [&graph, &node, retained_prepared, coordination, prepared_epoch,
       this]() -> execution::ComputeIoExecutor::Task {
        const std::shared_ptr<const ImageArtifactCodec> image_codec =
            image_codec_;
        const std::shared_ptr<const CacheMetadataCodec> metadata_codec =
            metadata_codec_;
        const GraphCacheResourceLimits resource_limits = resource_limits_;
        return [&graph, &node, retained_prepared, image_codec, metadata_codec,
                coordination, prepared_epoch, resource_limits]() {
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
          testing::notify_graph_cache_service_test_hook(
              testing::GraphCacheServiceTestEvent::AsyncWriterBeforeRootLock,
              graph.cache_root);
#endif
          GraphCacheRootGuard guard(coordination, graph.cache_root);
          if (guard.mutation_epoch() != prepared_epoch) {
            return;
          }
          save_cache_mechanism(graph, node, *retained_prepared, *image_codec,
                               *metadata_codec, resource_limits, nullptr);
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

/** @copydoc GraphCacheService::try_load_from_disk_cache */
bool GraphCacheService::try_load_from_disk_cache(
    GraphModel& graph, Node& node,
    ValueDiskCacheOutputSchema output_schema) const {
  if (!graph.cache_root.empty()) {
    require_disk_persistence_supported();
  }
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
  const auto load = [&]() {
    auto start_io = std::chrono::high_resolution_clock::now();
    DiskCacheReadAttempt attempt = read_first_disk_cache_entry(
        graph, node, output_schema, *metadata_codec_, data_definitions_,
        resource_limits_);
    return finalize_disk_cache_load(
        graph, std::move(attempt), start_io, [&](NodeOutput output) {
          RegionSet full_region = value_region::full_node_output_region(output);
          node.cached_output_high_precision = std::move(output);
          node.hp_region = std::move(full_region);
          node.hp_version++;
        });
  };
  if (graph.cache_root.empty()) {
    return load();
  }
  const std::shared_ptr<GraphCacheRootCoordination> coordination =
      graph_cache_root_coordination(graph.cache_root);
  GraphCacheRootGuard guard(coordination, graph.cache_root);
  return load();
}

/** @copydoc GraphCacheService::try_load_from_disk_cache_into */
bool GraphCacheService::try_load_from_disk_cache_into(
    GraphModel& graph, const Node& node, NodeOutput& out,
    ValueDiskCacheOutputSchema output_schema) const {
  if (!graph.cache_root.empty()) {
    require_disk_persistence_supported();
  }
  if (node.cached_output_high_precision.has_value()) {
    record_disk_cache_load_result(
        graph, make_skipped_attempt(
                   node.id,
                   "Node already has formal HP memory state; disk cache cannot "
                   "override complete or partial runtime validity.")
                   .result);
    return false;
  }
  const auto load = [&]() {
    auto start_io = std::chrono::high_resolution_clock::now();
    DiskCacheReadAttempt attempt = read_first_disk_cache_entry(
        graph, node, output_schema, *metadata_codec_, data_definitions_,
        resource_limits_);
    return finalize_disk_cache_load(
        graph, std::move(attempt), start_io,
        [&](NodeOutput output) { out = std::move(output); });
  };
  if (graph.cache_root.empty()) {
    return load();
  }
  const std::shared_ptr<GraphCacheRootCoordination> coordination =
      graph_cache_root_coordination(graph.cache_root);
  GraphCacheRootGuard guard(coordination, graph.cache_root);
  return load();
}

/** @copydoc GraphCacheService::clear_drive_cache */
GraphModel::DriveClearResult GraphCacheService::clear_drive_cache(
    GraphModel& graph) const {
  GraphModel::DriveClearResult result;
  if (graph.cache_root.empty()) {
    return result;
  }
  require_disk_persistence_supported();
  const std::shared_ptr<GraphCacheRootCoordination> coordination =
      graph_cache_root_coordination(graph.cache_root);
  GraphCacheRootGuard guard(coordination, graph.cache_root);
  (void)guard.advance_mutation_epoch();
#if !defined(_WIN32)
  const fs::file_status status = fs::symlink_status(graph.cache_root);
  if (status.type() != fs::file_type::not_found) {
    if (fs::is_symlink(status) || !fs::is_directory(status)) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Graph cache clear refuses a symlink or non-directory "
                       "root.");
    }
    result.removed_entries = fs::remove_all(graph.cache_root);
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
    testing::notify_graph_cache_service_test_hook(
        testing::GraphCacheServiceTestEvent::DriveCacheRootRemoved,
        graph.cache_root);
#endif
    fs::create_directories(graph.cache_root);
  }
#endif
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

/** @copydoc GraphCacheService::clear_cache */
void GraphCacheService::clear_cache(GraphModel& graph) const {
  (void)clear_drive_cache(graph);
  (void)clear_memory_cache(graph);
}

/** @copydoc GraphCacheService::cache_all_nodes */
GraphModel::CacheSaveResult GraphCacheService::cache_all_nodes(
    GraphModel& graph, const std::string& cache_precision) const {
  GraphModel::CacheSaveResult result;
  if (graph.cache_root.empty()) {
    for (int node_id : graph.node_ids()) {
      if (hp_cache_ptr(graph.node(node_id))) {
        result.saved_nodes++;
      }
    }
    return result;
  }
  require_disk_persistence_supported();
  const std::shared_ptr<GraphCacheRootCoordination> coordination =
      graph_cache_root_coordination(graph.cache_root);
  GraphCacheRootGuard guard(coordination, graph.cache_root);
  (void)guard.advance_mutation_epoch();
  for (int node_id : graph.node_ids()) {
    const Node& node = graph.node(node_id);
    if (hp_cache_ptr(node)) {
      const std::optional<PreparedGraphCacheSave> prepared =
          prepare_graph_cache_save(graph, node, cache_precision,
                                   resource_limits_);
      if (prepared.has_value()) {
        save_cache_mechanism(graph, node, *prepared, *image_codec_,
                             *metadata_codec_, resource_limits_, &graph);
      }
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

/** @copydoc GraphCacheService::synchronize_disk_cache */
GraphModel::DiskSyncResult GraphCacheService::synchronize_disk_cache(
    GraphModel& graph, const std::string& cache_precision) const {
  GraphModel::DiskSyncResult result;
  if (graph.cache_root.empty()) {
    for (int node_id : graph.node_ids()) {
      if (hp_cache_ptr(graph.node(node_id))) {
        result.saved_nodes++;
      }
    }
    return result;
  }
  require_disk_persistence_supported();
  const std::shared_ptr<GraphCacheRootCoordination> coordination =
      graph_cache_root_coordination(graph.cache_root);
  GraphCacheRootGuard guard(coordination, graph.cache_root);
  (void)guard.advance_mutation_epoch();

  for (int node_id : graph.node_ids()) {
    const Node& node = graph.node(node_id);
    if (!hp_cache_ptr(node)) {
      continue;
    }
    const std::optional<PreparedGraphCacheSave> prepared =
        prepare_graph_cache_save(graph, node, cache_precision,
                                 resource_limits_);
    if (prepared.has_value()) {
      save_cache_mechanism(graph, node, *prepared, *image_codec_,
                           *metadata_codec_, resource_limits_, &graph);
    }
    result.saved_nodes++;
  }

  for (int node_id : graph.node_ids()) {
    const Node& node = graph.node(node_id);
    if (has_complete_hp_cache(node) || node.caches.empty()) {
      continue;
    }

    if (!has_image_disk_cache_entry(graph, node)) {
      continue;
    }

    std::optional<GraphCacheNodeDirectory> directory =
        GraphCacheNodeDirectory::open(graph.cache_root, node.id, false);
    if (!directory.has_value()) {
      continue;
    }

    for (const auto& cache_entry : node.caches) {
      if (cache_entry.cache_type != "image") {
        continue;
      }
      const GraphCacheArtifactPaths paths =
          graph_cache_artifact_paths(graph, node, cache_entry);
      for (const std::string& leaf :
           {paths.image_leaf, paths.metadata_leaf, paths.archive_leaf,
            paths.manifest_leaf}) {
        if (directory->remove_leaf_if_present(leaf)) {
          result.removed_files++;
        }
      }
    }

    if (directory->remove_if_empty()) {
      result.removed_dirs++;
    }
  }

  return result;
}

}  // namespace ps
