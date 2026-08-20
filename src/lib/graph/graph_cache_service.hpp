#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/cache_metadata_codec.hpp"
#include "core/image_artifact_codec.hpp"
#include "graph/graph_model.hpp"
#include "graph/image_statistics_store.hpp"

namespace ps {

class DataDefinitionRegistry;

namespace execution {
class ComputeIoExecutor;
}  // namespace execution

/**
 * @brief Freezes the complete planned shape of one portable disk-cache set.
 *
 * @throws std::bad_alloc when owning or copying parameter-output names cannot
 * allocate.
 * @note Every planned Value name is validated against one canonical
 * `NamedValueArtifactSet` archive. Parameter outputs remain detached values
 * whose encoded bytes are digest-bound by the cache manifest. Instances are
 * request-owned snapshots and retain no Graph, registry, or codec state.
 */
struct ValueDiskCacheOutputSchema final {
  /** @brief Whether the frozen plan requires the canonical `image` Value. */
  bool canonical_image_planned = false;
  /** @brief Exact frozen parameter-result names in the metadata payload. */
  std::vector<std::string> parameter_output_names;
  /** @brief Exact frozen generic Value names outside the `image` slot. */
  std::vector<std::string> generic_named_value_output_names;
};

/** @brief Default maximum canonical graph-cache archive bytes per entry. */
inline constexpr std::uint64_t kDefaultGraphCacheMaximumArchiveBytes =
    512ULL * 1024ULL * 1024ULL;  // NOLINT(whitespace/indent_namespace)

/** @brief Default maximum detached parameter-metadata bytes per entry. */
inline constexpr std::uint64_t kDefaultGraphCacheMaximumMetadataBytes =
    16ULL * 1024ULL * 1024ULL;  // NOLINT(whitespace/indent_namespace)

/** @brief Default maximum auxiliary image-projection bytes per entry. */
inline constexpr std::uint64_t kDefaultGraphCacheMaximumProjectionBytes =
    512ULL * 1024ULL * 1024ULL;  // NOLINT(whitespace/indent_namespace)

/** @brief Default aggregate replay allocation quota for one transaction. */
inline constexpr std::uint64_t kDefaultGraphCacheMaximumTransactionBytes =
    kDefaultGraphCacheMaximumArchiveBytes +  // NOLINT(whitespace/indent_namespace)
    kDefaultGraphCacheMaximumMetadataBytes;  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Freezes GraphCache-specific disk and replay resource boundaries.
 *
 * @throws Nothing for aggregate construction and comparison.
 * @note These limits are independent from the wider public portable-artifact
 * framing bounds. `maximum_transaction_bytes` is the aggregate archive plus
 * detached-metadata replay quota; the auxiliary projection is separately
 * bounded because it never enters replay or Value reconstruction.
 */
struct GraphCacheResourceLimits final {
  /** @brief Positive maximum canonical archive bytes accepted or written. */
  std::uint64_t maximum_archive_bytes = kDefaultGraphCacheMaximumArchiveBytes;
  /** @brief Positive maximum detached metadata bytes accepted or written. */
  std::uint64_t maximum_metadata_bytes = kDefaultGraphCacheMaximumMetadataBytes;
  /** @brief Positive maximum auxiliary projection file size. */
  std::uint64_t maximum_projection_bytes =
      kDefaultGraphCacheMaximumProjectionBytes;
  /** @brief Positive checked archive-plus-metadata replay quota. */
  std::uint64_t maximum_transaction_bytes =
      kDefaultGraphCacheMaximumTransactionBytes;
};

/**
 * @class GraphCacheService
 * @brief Coordinates graph memory-cache cleanup and HP disk-cache persistence.
 *
 * The service owns no graph state. It reads and mutates cache-related fields on
 * GraphModel and Node, saves formal HP outputs to configured cache files, loads
 * disk cache entries back into HP outputs or temporary execution slots, and
 * synchronizes stale disk files with current HP cache authority.
 *
 * @note Disk-cache load wrappers keep their historical bool return contract:
 * true means a reusable output is available, false means the caller should
 * compute normally. Detailed miss/error diagnostics are recorded through
 * GraphModel's locked disk-cache diagnostic API. Every complete formal named
 * Value is captured through the public `NamedValueArtifactSet` archive. A
 * versioned manifest digest-binds that archive and optional detached parameter
 * metadata; an image-codec file is only an auxiliary projection and never a
 * replay authority. Replay validates the manifest, exact frozen names,
 * descriptor/layout/binding facts, payload digests, provider availability,
 * metadata names, and stable manifest generation before publishing the
 * complete candidate with fresh runtime allocation/revision identities.
 * Packed, quantized, latent, unreadable, pending, unknown-provider, or
 * otherwise unsupported Values fail with a typed invalid-parameter error
 * before executor admission, filesystem mutation, or codec invocation.
 * Partial, tampered, mixed-generation, or retired image/YAML pairs cannot
 * publish any output. Graph-document locations are restricted to one safe
 * cross-platform leaf below the numeric node directory. Root-scoped process
 * coordination serializes readers, writers, cleanup, synchronization, and
 * clear across independent service instances; a cryptographically random
 * generation joins each archive to its manifest. No-follow regular-file,
 * one-link, current-owner, non-sparse, stat-before-allocation, and frozen
 * service quota checks precede replay allocation. Disk persistence is
 * currently POSIX-only and uses descriptor-relative no-follow operations.
 * On Windows every nonempty-root disk save, load, cleanup, synchronization,
 * and clear request fails closed before codec, filesystem, executor, Graph,
 * cache, timing, or diagnostic effects. Empty-root and disabled-save no-disk
 * intent retains its historical no-op or memory/statistics behavior. Windows
 * persistence is a future capability, not a partially supported handle path.
 * This service contains no OpenCV/YAML calls or provider-library types. Staged
 * HP commit may submit the const cache-save mechanism to the
 * process compute-I/O executor while the graph-state policy owner retains path,
 * failure, timing, and publication decisions; administration and load paths
 * remain synchronous.
 */
class GraphCacheService {
 public:
#if defined(_WIN32)
  /**
   * @brief Whether this build supports GraphCache disk persistence.
   * @note False on Windows and true on the current POSIX implementation. The
   * value describes disk persistence only; memory/statistics APIs are
   * platform-independent.
   */
  inline static constexpr bool kDiskPersistenceSupported = false;
#else
  /**
   * @brief Whether this build supports GraphCache disk persistence.
   * @note False on Windows and true on the current POSIX implementation. The
   * value describes disk persistence only; memory/statistics APIs are
   * platform-independent.
   */
  inline static constexpr bool kDiskPersistenceSupported = true;
#endif

  /**
   * @brief Requires current-platform GraphCache disk persistence support.
   * @return Nothing when the platform supports GraphCache disk persistence.
   * @throws GraphError with `InvalidParameter` on Windows.
   * @throws std::bad_alloc if constructing the typed diagnostic fails.
   * @note The check performs no codec, filesystem, executor, Graph, cache,
   * timing, or diagnostic work. Callers apply established empty-root or
   * disabled-save no-disk policy before invoking it.
   */
  static void require_disk_persistence_supported();

  /**
   * @brief Creates a cache service with explicitly owned artifact codecs.
   *
   * @param image_codec Shared codec used for every optional image projection
   * write.
   * @param metadata_codec Shared codec used for every named-value metadata
   * read and write.
   * @param maximum_statistics_entries Positive bounded derived statistics
   * result capacity owned by this cache service.
   * @param data_definitions Borrowed process registry used to reconstruct
   * provider-defined artifacts; null permits built-in replay only.
   * @param resource_limits Immutable GraphCache-specific archive, metadata,
   * projection, and aggregate replay bounds.
   * @throws std::invalid_argument when either codec owner is empty.
   * @throws std::invalid_argument when the statistics bound is zero or exceeds
   * its source-private frozen maximum.
   * @throws std::invalid_argument when resource limits are zero, exceed public
   * artifact bounds, or cannot admit either individually bounded replay file.
   * @throws std::bad_alloc when statistics store ownership cannot allocate.
   * @note Both immutable codec owners are retained for the complete service
   * lifetime and may be shared by independent graph services. They own no graph
   * state, path derivation, timing, diagnostic, or cache policy.
   */
  GraphCacheService(std::shared_ptr<const ImageArtifactCodec> image_codec,
                    std::shared_ptr<const CacheMetadataCodec> metadata_codec,
                    std::size_t maximum_statistics_entries =
                        kDefaultImageStatisticsCacheEntries,
                    DataDefinitionRegistry* data_definitions = nullptr,
                    GraphCacheResourceLimits resource_limits = {});

  /**
   * @brief Schedules or reuses one Value-backed derived statistics request.
   * @param value Exact sealed image Value retained by scheduled scan work.
   * @param content_digest Optional already-known canonical content identity.
   * @param query Complete normalized bounded observation request.
   * @param scheduler Trusted internal one-task ownership receiver.
   * @return Move-only request providing explicit cancellation and one future.
   * @throws Validation, scheduling, synchronization, future, or allocation
   * exceptions from the owned statistics store.
   * @note No mutable image staging, allocation identity, graph revision,
   * HP/RT generation, descriptor digest, or disk path participates in the key.
   */
  ScheduledImageStatistics schedule_image_statistics(
      Value value, std::optional<ContentDigest> content_digest,
      ImageStatisticsQuery query,
      const ImageStatisticsStore::Scheduler& scheduler) const;

  /**
   * @brief Looks up one complete derived statistics cache identity.
   * @param key Complete validated Value-backed statistics key.
   * @return Copied result or nullopt when absent.
   * @throws Validation, synchronization, or allocation exceptions.
   * @note Lookup mutates no graph, Value, formal cache, or eviction ordering.
   */
  std::optional<ImageStatisticsResult> lookup_image_statistics(
      const ImageStatisticsCacheKey& key) const;

  /**
   * @brief Invalidates derived results for one exact Value revision.
   * @param revision Valid process-local immutable Value revision.
   * @return Number of removed derived results.
   * @throws std::invalid_argument for an invalid revision.
   * @throws std::system_error when cache synchronization fails.
   * @note Allocation identity, Graph revision, HP/RT generation, Region, and
   * the referenced Value remain unchanged.
   */
  std::size_t invalidate_image_statistics_revision(
      ValueRevisionId revision) const;

  /**
   * @brief Evicts every retained derived statistics result.
   * @return Number of removed entries.
   * @throws std::system_error when cache synchronization fails.
   * @note In-flight requests retain their Value and may publish later unless
   * explicitly cancelled; formal memory and disk cache state is untouched.
   */
  std::size_t clear_image_statistics() const;

  /**
   * @brief Returns the current number of retained derived statistics results.
   * @return Count within the configured immutable bound.
   * @throws std::system_error when cache synchronization fails.
   */
  std::size_t image_statistics_size() const;

  /**
   * @brief Builds the per-node cache directory path under a graph cache root.
   *
   * @param graph Graph whose `cache_root` anchors disk cache files.
   * @param node_id Node id used as the cache directory name.
   * @return Path `<cache_root>/<node_id>`.
   * @throws std::bad_alloc if path/string allocation fails.
   * @note The function only builds the path; it does not create directories or
   * validate whether the node exists.
   */
  std::filesystem::path node_cache_dir(const GraphModel& graph,
                                       int node_id) const;

  /**
   * @brief Removes all disk-cache files for a graph and recreates the root.
   *
   * @param graph Graph whose disk cache root should be cleared.
   * @return Number of filesystem entries removed.
   * @throws GraphError with `InvalidParameter` for an unsafe root or any
   *         nonempty-root Windows request.
   * @throws std::filesystem::filesystem_error on filesystem failures.
   * @throws std::bad_alloc when filesystem path or diagnostic storage exhausts
   *         memory.
   * @note Empty cache roots are treated as a no-op. Removal and recreation are
   *       not an atomic filesystem transaction: removal may succeed before
   *       recreation fails. The serialized facade therefore publishes its
   *       prepared successor revision before calling this method and never
   *       rolls that invalidation back. Process-root coordination also
   *       supersedes every earlier admitted asynchronous cache writer before
   *       removal begins. Windows rejects the complete disk-cache API before
   *       coordination, epoch, filesystem, or Graph/cache mutation. Windows
   *       persistence remains a future target.
   */
  GraphModel::DriveClearResult clear_drive_cache(GraphModel& graph) const;

  /**
   * @brief Clears in-memory formal HP cache state.
   *
   * @param graph Graph whose nodes should be inspected and cleared.
   * @return Number of nodes that had memory cache state removed.
   * @throws GraphError or std::exception from graph node access.
   * @note Graph topology, disk cache files, and RT proxy graph state are not
   * changed.
   */
  GraphModel::MemoryClearResult clear_memory_cache(GraphModel& graph) const;

  /**
   * @brief Clears both disk-cache files and in-memory cache state.
   *
   * @param graph Graph whose cache state should be cleared.
   * @throws GraphError with `InvalidParameter` on Windows for a nonempty disk
   *         root, before either cache is mutated.
   * @throws std::filesystem::filesystem_error or graph access exceptions from
   * the delegated clear operations.
   * @note Result details are discarded for compatibility with the legacy
   * command path; callers needing counts should use the split clear APIs.
   * Windows callers may still invoke `clear_memory_cache` directly. With an
   * empty root this combined operation clears memory normally; with a nonempty
   * root it never skips its failed disk phase to clear memory partially.
   */
  void clear_cache(GraphModel& graph) const;

  /**
   * @brief Saves every node with formal HP output to configured disk cache.
   *
   * @param graph Graph whose nodes are scanned for HP cache outputs.
   * @param cache_precision Precision label used for image serialization.
   * @return Number of nodes for which a save attempt was issued.
   * @throws GraphError with `InvalidParameter` on Windows when the cache root
   * is nonempty.
   * @throws Codec, filesystem, graph, or allocation exceptions from saving.
   * @note RT-only state is ignored because disk cache authority is HP-only.
   * One root-scoped mutation epoch supersedes earlier admitted writers before
   * the batch begins. An empty root performs only the historical HP-node count
   * and is not a disk-persistence request on any platform.
   */
  GraphModel::CacheSaveResult cache_all_nodes(
      GraphModel& graph, const std::string& cache_precision) const;

  /**
   * @brief Drops memory cache state for non-ending nodes.
   *
   * @param graph Graph whose traversal endings define retained nodes.
   * @return Number of nodes whose memory cache was cleared.
   * @throws GraphError or std::exception from traversal or graph access.
   * @note This preserves final outputs while freeing intermediate HP memory.
   * RT proxy memory is owned outside GraphModel.
   */
  GraphModel::MemoryClearResult free_transient_memory(GraphModel& graph) const;

  /**
   * @brief Makes disk cache reflect current formal HP memory cache state.
   *
   * @param graph Graph whose disk cache should be synchronized.
   * @param cache_precision Precision label used for image serialization.
   * @return Counts for saved HP nodes and removed stale files/directories.
   * @throws GraphError with `InvalidParameter` on Windows when the cache root
   * is nonempty.
   * @throws Filesystem, codec, or graph access exceptions.
   * @note Nodes with only RT state do not protect existing disk cache files.
   * The complete save/cleanup pass is root-serialized and supersedes earlier
   * admitted asynchronous writers before its first filesystem effect. An
   * empty root performs only the historical HP-node count.
   */
  GraphModel::DiskSyncResult synchronize_disk_cache(
      GraphModel& graph, const std::string& cache_precision) const;

  /**
   * @brief Saves one node's complete formal HP output as a portable
   * transaction.
   *
   * @param graph Graph providing cache root and IO timing counters.
   * @param node Node whose HP output and cache entries should be saved.
   * @param cache_precision Precision label used for image serialization.
   * @throws GraphError with `InvalidParameter` on Windows for a nonempty-root,
   * enabled-save request, before capture or any side effect.
   * @throws Codec, filesystem, graph, or allocation exceptions from saving.
   * @note The method is a no-op for disabled saving, missing cache roots,
   * unsupported cache-entry types, or nodes without HP output. An image entry
   * with an empty, absolute, parent-traversing, multi-component, aliased, or
   * otherwise unsafe location is rejected before filesystem or codec work.
   * Every complete formal Value, including provider-defined
   * multi-buffer Values, must enter one public named-Value archive; unsupported
   * facts throw `GraphError{InvalidParameter}` before executor admission,
   * filesystem work, or codec effects. Partial hp_region validity is
   * classified before ImageView, artifact capture, provider validation, or
   * executor admission and directly removes the older complete transaction so
   * a later load cannot relabel stale bytes as complete. A canonical CPU image
   * may additionally cross the selected image
   * codec as an auxiliary projection, but replay reconstructs only from the
   * archive. Named ParameterValue outputs cross the metadata codec. The archive
   * and metadata byte sizes/digests are bound by a manifest written last;
   * one random generation is repeated by every archive envelope and binds
   * both archive and metadata manifest records;
   * failures or concurrent replacement yield a non-reusable transaction and
   * never authorize partial publication. Runtime allocation and Value revision
   * identities are never persisted. Empty roots and `skip_save_cache` remain
   * no-disk no-ops on every platform.
   */
  void save_cache_if_configured(GraphModel& graph, const Node& node,
                                const std::string& cache_precision) const;

  /**
   * @brief Saves one eligible HP cache through the bounded process I/O worker.
   *
   * The caller remains on its graph-state transaction lane. This method first
   * performs read-only policy checks and checked planned-byte estimation, then
   * lazily constructs one callback only after executor task/byte admission. It
   * waits for typed terminal completion, applies worker duration to the Graph
   * timing counter, and propagates codec/filesystem failure before returning.
   *
   * @param executor Process-owned independent compute-I/O authority.
   * @param lifetime_token Non-null prepared-Graph or Run owner retained until
   * callback settlement.
   * @param graph Transaction-owned Graph providing cache policy and paths.
   * @param node Stable node inside `graph` whose formal HP output is saved.
   * @param cache_precision Precision label forwarded to the selected codec.
   * @throws GraphError with `InvalidParameter` when portable persistence
   * rejects a packed, quantized, latent, unreadable, pending, provider-missing,
   * or otherwise incompatible Value before executor admission.
   * @throws GraphError with `InvalidParameter` on Windows for a nonempty-root,
   * enabled-save request before capture or executor admission.
   * @throws GraphError with `ComputeError` for typed admission rejection or
   * cancellation.
   * @throws Codec, filesystem, graph, allocation, or synchronization
   * exceptions from estimation, construction, execution, or waiting.
   * @note The I/O callback receives only const Graph/Node access and therefore
   * cannot mutate Graph state. This preserves the current pre-publication
   * cache failure ordering; it grants no Graph-document, daemon, OutputStore,
   * retry, receipt, or durability authority. A later root mutation supersedes
   * an admitted but not-yet-authoritative callback, which then settles as a
   * successful no-op without recreating a removed transaction. Empty roots and
   * `skip_save_cache` remain no-disk no-ops on every platform.
   */
  void save_cache_if_configured_via_executor(
      execution::ComputeIoExecutor& executor,
      const std::shared_ptr<const void>& lifetime_token, GraphModel& graph,
      const Node& node, const std::string& cache_precision) const;

  /**
   * @brief Attempts to satisfy a node from disk cache into its HP memory cache.
   *
   * @param graph Graph whose cache root, timing, and diagnostics are updated.
   * @param node Node receiving the loaded HP output on cache hit.
   * @param output_schema Complete frozen Value/parameter output shape.
   * @return true when complete HP output is already present or disk cache was
   * loaded; false for partial memory validity, cache miss, skipped load, or
   * read/parse error.
   * @throws std::bad_alloc from diagnostic/output/Value storage. Disk read,
   * parse, and explicit CPU Value import failures are recorded through
   * GraphModel's locked disk-cache diagnostic API and reported as false.
   * @throws GraphError with `InvalidParameter` on Windows when the cache root
   * is nonempty, before memory-cache inspection or diagnostic publication.
   * @note This preserves the legacy try-load bool contract while making disk
   * errors distinguishable from misses through graph diagnostics. Successful
   * archive reconstruction mints fresh process-local allocation/revision
   * identities for every Value. Replay validates the versioned manifest,
   * generation-bound detached archive/metadata records, exact planned Value
   * names, complete artifact facts, and exact parameter keys, then rereads the
   * manifest before returning Hit. Unsafe paths, symlink/nonregular/sparse
   * leaves, and frozen resource-limit violations fail before archive
   * allocation. Any mismatch publishes nothing.
   * A hit publishes the output, incremented HP content version, and derived
   * full-validity Region together on the supplied Node. Empty-root calls retain
   * the existing memory/Skipped diagnostic behavior and perform no disk work.
   */
  bool try_load_from_disk_cache(GraphModel& graph, Node& node,
                                ValueDiskCacheOutputSchema output_schema) const;

  /**
   * @brief Attempts to load a node's disk cache into a caller-owned output.
   *
   * @param graph Graph whose cache root, timing, and diagnostics are updated.
   * @param node Node whose cache entries define candidate disk files.
   * @param out Receives the loaded output on cache hit.
   * @param output_schema Complete frozen Value/parameter output shape.
   * @return true on disk cache hit; false on cache miss, skipped load, or
   * read/parse error.
   * @throws std::bad_alloc from diagnostic/output/Value storage. Disk read,
   * parse, and explicit CPU Value import failures are recorded through
   * GraphModel's locked disk-cache diagnostic API and reported as false.
   * @throws GraphError with `InvalidParameter` on Windows when the cache root
   * is nonempty, before memory-cache inspection or diagnostic publication.
   * @note Used by execution worker paths that stage outputs outside the
   * formal HP cache before committing. Existing complete or partial formal
   * memory state prevents disk load so regionless artifacts cannot override
   * current runtime validity. Replay validates the complete manifest-bound
   * named-Value transaction, safe descriptor-backed files, resource limits,
   * and exact frozen Value/parameter names before one move publishes the
   * candidate; mismatches and errors leave `out` unchanged. Empty-root calls
   * retain the existing memory/Skipped diagnostic behavior and perform no disk
   * work.
   */
  bool try_load_from_disk_cache_into(
      GraphModel& graph, const Node& node, NodeOutput& out,
      ValueDiskCacheOutputSchema output_schema) const;

 private:
  /**
   * @brief Shared immutable codec used by every image projection operation.
   * @note The owner is non-null after construction. The codec may be called by
   * different graph-state lanes concurrently and therefore must provide its own
   * provider-local synchronization when needed.
   */
  std::shared_ptr<const ImageArtifactCodec> image_codec_;

  /**
   * @brief Shared immutable codec used by every metadata artifact operation.
   * @note The owner is non-null after construction. The codec may be called by
   * different graph-state lanes concurrently and owns no cache policy or
   * diagnostic state.
   */
  std::shared_ptr<const CacheMetadataCodec> metadata_codec_;

  /**
   * @brief Immutable GraphCache-specific allocation and disk-file boundaries.
   * @note Construction validates the complete aggregate. Every save/load
   * observes this frozen copy; no graph document or portable envelope may
   * widen it at runtime.
   */
  const GraphCacheResourceLimits resource_limits_;

  /**
   * @brief Borrowed provider-definition authority for executable replay.
   * @note The pointer may be null for built-in-only products. When non-null,
   * its owner must outlive this service and every in-flight cache operation.
   */
  DataDefinitionRegistry* data_definitions_ = nullptr;

  /**
   * @brief Per-service bounded owner of discardable observed-image results.
   * @note Accepted tasks retain only its opaque state through settlement; this
   * member owns no worker, Value, graph, formal cache, or persistence identity.
   */
  ImageStatisticsStore image_statistics_store_;
};

}  // namespace ps
