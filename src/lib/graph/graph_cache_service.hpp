#pragma once

#include <cstddef>
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

namespace execution {
class ComputeIoExecutor;
}  // namespace execution

/**
 * @brief Freezes the complete planned shape representable by image disk cache.
 *
 * @throws std::bad_alloc when owning or copying parameter-output names cannot
 * allocate.
 * @note The image/YAML artifact pair can represent one canonical image and an
 * exact `NodeOutput::data` parameter map. Generic named Values have no durable
 * format and therefore force pre-filesystem miss/save-skip behavior. Instances
 * are request-owned snapshots and retain no Graph, registry, or codec state.
 */
struct ImageDiskCacheOutputSchema final {
  /** @brief Whether the frozen plan requires the canonical image sibling. */
  bool canonical_image_planned = false;
  /** @brief Exact frozen parameter-result names requiring the YAML sibling. */
  std::vector<std::string> parameter_output_names;
  /** @brief Whether any unrepresentable generic named Value is planned. */
  bool contains_generic_named_values = false;
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
 * GraphModel's locked disk-cache diagnostic API. Compatible image bytes and
 * detached named values cross injected `ImageArtifactCodec` and
 * `CacheMetadataCodec` boundaries; packed, quantized, or latent formal Values
 * fail with a typed invalid-parameter error before executor admission,
 * filesystem mutation, or codec invocation. A planned schema containing any
 * generic named Value makes every existing artifact an incompatible miss
 * before filesystem or codec inspection. Otherwise, physical image/YAML
 * sibling presence must equal the frozen image/parameter shape before either
 * codec is entered, and decoded metadata keys must equal the exact frozen
 * parameter names before a Hit is returned. Exact validated formal outputs
 * containing generic Values skip image/YAML persistence before planned-byte
 * admission, filesystem work, or codec invocation. Successful representable
 * saves write every planned sibling before removing an unplanned predecessor
 * sibling; failures remain non-transactional but cannot produce a reusable
 * shape-mismatched hit. This service contains no OpenCV/YAML calls or
 * provider-library types. Staged HP commit may submit the const cache-save
 * mechanism to the
 * process compute-I/O executor while the graph-state policy owner retains path,
 * failure, timing, and publication decisions; synchronous administration and
 * load paths remain unchanged.
 */
class GraphCacheService {
 public:
  /**
   * @brief Creates a cache service with explicitly owned artifact codecs.
   *
   * @param image_codec Shared codec used for every image cache read and write.
   * @param metadata_codec Shared codec used for every named-value metadata
   * read and write.
   * @param maximum_statistics_entries Positive bounded derived statistics
   * result capacity owned by this cache service.
   * @throws std::invalid_argument when either codec owner is empty.
   * @throws std::invalid_argument when the statistics bound is zero or exceeds
   * its source-private frozen maximum.
   * @throws std::bad_alloc when statistics store ownership cannot allocate.
   * @note Both immutable codec owners are retained for the complete service
   * lifetime and may be shared by independent graph services. They own no graph
   * state, path derivation, timing, diagnostic, or cache policy.
   */
  GraphCacheService(std::shared_ptr<const ImageArtifactCodec> image_codec,
                    std::shared_ptr<const CacheMetadataCodec> metadata_codec,
                    std::size_t maximum_statistics_entries =
                        kDefaultImageStatisticsCacheEntries);

  /**
   * @brief Schedules or reuses one Value-backed derived statistics request.
   * @param value Exact sealed image Value retained by scheduled scan work.
   * @param content_digest Optional already-known canonical content identity.
   * @param query Complete normalized bounded observation request.
   * @param scheduler Trusted internal one-task ownership receiver.
   * @return Move-only request providing explicit cancellation and one future.
   * @throws Validation, scheduling, synchronization, future, or allocation
   * exceptions from the owned statistics store.
   * @note No compatibility ImageBuffer, allocation identity, graph revision,
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
   * @throws std::filesystem::filesystem_error on filesystem failures.
   * @throws std::bad_alloc when filesystem path or diagnostic storage exhausts
   *         memory.
   * @note Empty cache roots are treated as a no-op. Removal and recreation are
   *       not an atomic filesystem transaction: removal may succeed before
   *       recreation fails. The serialized facade therefore publishes its
   *       prepared successor revision before calling this method and never
   *       rolls that invalidation back.
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
   * @throws std::filesystem::filesystem_error or graph access exceptions from
   * the delegated clear operations.
   * @note Result details are discarded for compatibility with the legacy
   * command path; callers needing counts should use the split clear APIs.
   */
  void clear_cache(GraphModel& graph) const;

  /**
   * @brief Saves every node with formal HP output to configured disk cache.
   *
   * @param graph Graph whose nodes are scanned for HP cache outputs.
   * @param cache_precision Precision label used for image serialization.
   * @return Number of nodes for which a save attempt was issued.
   * @throws Codec, filesystem, graph, or allocation exceptions from saving.
   * @note RT-only state is ignored because disk cache authority is HP-only.
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
   * @throws Filesystem, codec, or graph access exceptions.
   * @note Nodes with only RT state do not protect existing disk cache files.
   */
  GraphModel::DiskSyncResult synchronize_disk_cache(
      GraphModel& graph, const std::string& cache_precision) const;

  /**
   * @brief Saves one node's formal HP output when an image cache is configured.
   *
   * @param graph Graph providing cache root and IO timing counters.
   * @param node Node whose HP output and cache entries should be saved.
   * @param cache_precision Precision label used for image serialization.
   * @throws Codec, filesystem, graph, or allocation exceptions from saving.
   * @note The method is a no-op for disabled saving, missing cache roots,
   * unsupported cache-entry types, empty locations, or nodes without HP
   * output. A configured image entry with a packed, quantized, or latent formal
   * Value instead throws `GraphError{InvalidParameter}` before filesystem or
   * codec effects. A formal output containing generic named Values is an
   * explicit no-op before planned-byte admission, filesystem work, or codec
   * effects because the current artifact pair cannot encode that exact output
   * schema.
   * Partial hp_region validity is never serialized; any older configured
   * artifact for that node is removed so a later load cannot relabel stale
   * bytes as complete.
   * A valid sealed CPU image Value is the sole serialization authority and is
   * copied into a callback-local codec-compatible ImageBuffer. Nonempty
   * compatibility staging is rejected instead of serving as a fallback.
   * Named ParameterValue outputs cross the metadata codec; a future device
   * adapter may add explicit download. After all required writes succeed, an
   * absent image removes only the configured image sibling and empty parameter
   * data removes only its YAML sibling. Image-plus-parameter output retains
   * both, while an empty complete output removes both. If a required write
   * fails, stale siblings are retained and the exception propagates; cleanup
   * failures may leave a partially updated pair, which later shape validation
   * rejects as a miss. The current Value must be image-faceted, Strided,
   * unquantized, host-readable, and whole-byte compatible before the save
   * mechanism proceeds. Runtime allocation and Value revision identities are
   * never persisted.
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
   * @throws GraphError with `InvalidParameter` when image disk persistence
   * rejects a packed, quantized, latent, or otherwise incompatible Value
   * before executor admission.
   * @throws GraphError with `ComputeError` for typed admission rejection or
   * cancellation.
   * @throws Codec, filesystem, graph, allocation, or synchronization
   * exceptions from estimation, construction, execution, or waiting.
   * @note The I/O callback receives only const Graph/Node access and therefore
   * cannot mutate Graph state. This preserves the current pre-publication
   * cache failure ordering; it grants no Graph-document, daemon, OutputStore,
   * retry, receipt, or durability authority.
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
   * @param output_schema Complete frozen image/parameter/generic output shape.
   * @return true when complete HP output is already present or disk cache was
   * loaded; false for partial memory validity, cache miss, skipped load, or
   * read/parse error.
   * @throws std::bad_alloc from diagnostic/output/Value storage. Disk read,
   * parse, and explicit CPU Value import failures are recorded through
   * GraphModel's locked disk-cache diagnostic API and reported as false.
   * @note This preserves the legacy try-load bool contract while making disk
   * errors distinguishable from misses through graph diagnostics. Successful
   * CPU image decode mints fresh process-local allocation/revision identities.
   * A schema containing generic named Values records an incompatible Miss and
   * performs no filesystem or codec inspection. Representable plans compare
   * sibling presence before either codec and compare exact decoded parameter
   * keys before returning Hit; mismatches are misses and never reach output
   * authority validation as false hits.
   * A hit publishes the output, incremented HP content version, and derived
   * full-validity Region together on the supplied Node.
   */
  bool try_load_from_disk_cache(GraphModel& graph, Node& node,
                                ImageDiskCacheOutputSchema output_schema) const;

  /**
   * @brief Attempts to load a node's disk cache into a caller-owned output.
   *
   * @param graph Graph whose cache root, timing, and diagnostics are updated.
   * @param node Node whose cache entries define candidate disk files.
   * @param out Receives the loaded output on cache hit.
   * @param output_schema Complete frozen image/parameter/generic output shape.
   * @return true on disk cache hit; false on cache miss, skipped load, or
   * read/parse error.
   * @throws std::bad_alloc from diagnostic/output/Value storage. Disk read,
   * parse, and explicit CPU Value import failures are recorded through
   * GraphModel's locked disk-cache diagnostic API and reported as false.
   * @note Used by execution worker paths that stage outputs outside the
   * formal HP cache before committing. Existing complete or partial formal
   * memory state prevents disk load so regionless artifacts cannot override
   * current runtime validity. A schema containing generic named Values records
   * an incompatible Miss and performs no filesystem or codec inspection.
   * Representable plans compare sibling presence before either codec and
   * compare exact decoded parameter keys before returning Hit; mismatches are
   * misses and leave `out` unchanged.
   */
  bool try_load_from_disk_cache_into(
      GraphModel& graph, const Node& node, NodeOutput& out,
      ImageDiskCacheOutputSchema output_schema) const;

 private:
  /**
   * @brief Shared immutable codec used by every image artifact operation.
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
   * @brief Per-service bounded owner of discardable observed-image results.
   * @note Accepted tasks retain only its opaque state through settlement; this
   * member owns no worker, Value, graph, formal cache, or persistence identity.
   */
  ImageStatisticsStore image_statistics_store_;
};

}  // namespace ps
