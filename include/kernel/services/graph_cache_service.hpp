#pragma once

#include <filesystem>
#include <string>

#include "graph_model.hpp"

namespace ps {

/**
 * @class GraphCacheService
 * @brief Coordinates graph memory-cache cleanup and HP disk-cache persistence.
 *
 * The service owns no graph state. It reads and mutates cache-related fields on
 * GraphModel and Node, saves formal HP outputs to configured cache files, loads
 * disk cache entries back into HP outputs or temporary scheduler slots, and
 * synchronizes stale disk files with current HP cache authority.
 *
 * @note Disk-cache load wrappers keep their historical bool return contract:
 * true means a reusable output is available, false means the caller should
 * compute normally. Detailed miss/error diagnostics are recorded through
 * GraphModel's locked disk-cache diagnostic API.
 */
class GraphCacheService {
 public:
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
   * @note Empty cache roots are treated as a no-op.
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
   * @throws OpenCV, YAML, filesystem, or allocation exceptions from saving.
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
   * @throws Filesystem, OpenCV, YAML, or graph access exceptions.
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
   * @throws OpenCV, YAML, filesystem, or allocation exceptions from saving.
   * @note The method is a no-op for disabled saving, missing cache roots,
   * unsupported cache entries, empty locations, or nodes without HP output.
   */
  void save_cache_if_configured(GraphModel& graph, const Node& node,
                                const std::string& cache_precision) const;

  /**
   * @brief Attempts to satisfy a node from disk cache into its HP memory cache.
   *
   * @param graph Graph whose cache root, timing, and diagnostics are updated.
   * @param node Node receiving the loaded HP output on cache hit.
   * @return true when HP output is already present or disk cache was loaded;
   * false on cache miss, skipped load, or read/parse error.
   * @throws std::bad_alloc from diagnostic/output storage. Disk read and parse
   * failures are recorded through GraphModel's locked disk-cache diagnostic API
   * and reported as false.
   * @note This preserves the legacy try-load bool contract while making disk
   * errors distinguishable from misses through graph diagnostics.
   */
  bool try_load_from_disk_cache(GraphModel& graph, Node& node) const;

  /**
   * @brief Attempts to load a node's disk cache into a caller-owned output.
   *
   * @param graph Graph whose cache root, timing, and diagnostics are updated.
   * @param node Node whose cache entries define candidate disk files.
   * @param out Receives the loaded output on cache hit.
   * @return true on disk cache hit; false on cache miss, skipped load, or
   * read/parse error.
   * @throws std::bad_alloc from diagnostic/output storage. Disk read and parse
   * failures are recorded through GraphModel's locked disk-cache diagnostic API
   * and reported as false.
   * @note Used by scheduler worker paths that stage outputs outside the
   * formal HP cache before committing.
   */
  bool try_load_from_disk_cache_into(GraphModel& graph, const Node& node,
                                     NodeOutput& out) const;
};

}  // namespace ps
