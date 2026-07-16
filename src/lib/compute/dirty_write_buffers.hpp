#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "compute/downsample_executor.hpp"
#include "compute/realtime_proxy_graph.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {

/**
 * @brief Request-local staging buffer for HP dirty writes.
 *
 * HighPrecisionDirtyWriteBuffer owns per-node HP outputs, ROI metadata,
 * version counters, and dirty-source generation updates produced by dirty
 * worker tasks. The owning executor commits the staged data into GraphModel
 * only after the RT sibling commit gate allows original-graph mutation.
 *
 * @note Existing CPU output is deep-copied before tiled ROI writes so worker
 * execution never mutates visible GraphModel image storage before commit.
 * Opaque non-CPU descriptors are shared only until tiled allocation or
 * monolithic whole-output replacement; their payload is never mutated through
 * the generic staging path.
 */
class HighPrecisionDirtyWriteBuffer {
 public:
  /**
   * @brief Constructs an HP staging buffer.
   *
   * @param seed_existing_outputs Whether ensure_output() should seed from
   * existing GraphModel HP cache. Force-recache HP dirty requests pass false
   * only after the executor has expanded planning to the full HP frame.
   * @throws Nothing.
   */
  explicit HighPrecisionDirtyWriteBuffer(bool seed_existing_outputs = true);

  /**
   * @brief Returns staged HP output for one node when available.
   *
   * @param node_id Graph node id to inspect.
   * @return Pointer to staged output, or nullptr when the node has not staged
   * output in this request.
   * @throws Nothing directly.
   * @note The pointer remains valid until buffer destruction or commit.
   */
  const NodeOutput* find_output(int node_id) const;

  /**
   * @brief Checks whether staged HP output exists for one node.
   *
   * @param node_id Graph node id to inspect.
   * @return True when find_output(node_id) returns non-null.
   * @throws Nothing directly.
   */
  bool has_output(int node_id) const;

  /**
   * @brief Ensures a writable staged HP output exists for one node.
   *
   * @param node Graph node whose current HP state seeds the staged entry.
   * @return Mutable staged output owned by this buffer.
   * @throws GraphError when existing image output cannot be cloned; may throw
   * std::bad_alloc while allocating map entries or image storage.
   * @note When constructed with seed_existing_outputs=false, the staged output
   * starts empty even if the graph already has HP cache; callers must then
   * execute a full-output HP plan before commit.
   */
  NodeOutput& ensure_output(const Node& node);

  /**
   * @brief Imports one immutable HP preflight result into request staging.
   *
   * @param node Graph node whose final HP state may be committed.
   * @param output Preflight output copied into request-local ownership.
   * @param hp_version Version to publish after complete request success.
   * @param hp_roi Full image ROI represented by output, when applicable.
   * @param dirty_source_generation Shared generation for preflight closure
   * roots, otherwise nullopt.
   * @return Nothing.
   * @throws std::bad_alloc when output ownership or map storage is copied.
   * @note Import never mutates GraphModel. A later phase may read the staged
   * output, but externally satisfied preflight nodes must not write it again.
   */
  void import_precomputed_output(
      const Node& node, const NodeOutput& output, int hp_version,
      const std::optional<cv::Rect>& hp_roi,
      std::optional<uint64_t> dirty_source_generation = std::nullopt);

  /**
   * @brief Records HP metadata for one successful dirty node update.
   *
   * @param node Graph node whose staged output was updated.
   * @param roi_hp HP-space ROI represented by this update.
   * @param hp_size Full HP output extent used to clamp merged ROI metadata.
   * @param dirty_source Whether the node is a dirty source boundary.
   * @param dirty_generation Dirty generation committed for source nodes.
   * @return New staged HP version after incrementing.
   * @throws std::bad_alloc if the staged entry must be created.
   * @note Version increments once per dirty task, preserving prior executor
   * semantics while hiding the increment until graph commit.
   */
  int mark_updated(const Node& node, const cv::Rect& roi_hp,
                   const cv::Size& hp_size, bool dirty_source,
                   uint64_t dirty_generation);

  /**
   * @brief Moves staged HP state into the original GraphModel.
   *
   * @param graph Graph receiving HP outputs and metadata.
   * @throws GraphError when a staged node no longer exists.
   * @note The caller must hold graph.graph_mutex_. This method performs no
   * locking on GraphModel so the commit boundary remains explicit.
   */
  void commit_to_graph(GraphModel& graph);

  /**
   * @brief Builds HP-to-RT downsample requests from committed staged state.
   *
   * @return Downsample requests carrying final staged HP versions.
   * @throws std::bad_alloc if result vector allocation fails.
   * @note The requests are valid after commit_to_graph() has made HP outputs
   * visible on GraphModel.
   */
  std::vector<DownsampleExecutor::Request> downsample_requests() const;

 private:
  /**
   * @brief Complete staged HP state for one graph node.
   *
   * @note `initialized` tracks whether graph metadata has been captured.
   * `has_output` controls whether commit_to_graph() writes output state.
   */
  struct Entry {
    bool initialized = false;
    bool has_output = false;
    NodeOutput output;
    std::optional<cv::Rect> hp_roi;
    int hp_version = 0;
    std::optional<uint64_t> dirty_source_generation;
  };

  /**
   * @brief Ensures an entry exists and has graph metadata initialized.
   *
   * @param node Graph node used for initial HP metadata.
   * @return Mutable staged entry.
   * @throws GraphError when image cloning fails.
   * @note The caller must hold mutex_.
   */
  Entry& ensure_entry_locked(const Node& node);

  /** @brief Whether new entries seed output pixels from GraphModel HP cache. */
  bool seed_existing_outputs_ = true;

  /** @brief Mutex protecting staged entry creation and metadata updates. */
  mutable std::mutex mutex_;

  /** @brief Staged HP entries keyed by original GraphModel node id. */
  std::map<int, Entry> entries_;
};

/**
 * @brief Request-local staging buffer for RT proxy graph writes.
 *
 * RealtimeProxyWriteBuffer owns low-resolution output and RT metadata while RT
 * dirty worker tasks execute. After all RT work drains, the owning executor
 * commits staged state into RealtimeProxyGraph, not into GraphModel.
 *
 * @note Existing committed CPU proxy output is deep-copied before ROI writes.
 * Opaque non-CPU descriptors are shared only until replacement and are never
 * mutated through the generic staging path.
 */
class RealtimeProxyWriteBuffer {
 public:
  /**
   * @brief Constructs an RT proxy staging buffer.
   *
   * @param proxy_graph Committed proxy graph used to seed existing RT state.
   * @param seed_existing_outputs Whether staged entries should seed from the
   * committed proxy graph. Force-recache requests pass false.
   * @throws Nothing.
   */
  explicit RealtimeProxyWriteBuffer(RealtimeProxyGraph& proxy_graph,
                                    bool seed_existing_outputs = true);

  /**
   * @brief Returns staged RT proxy output for one node when available.
   *
   * @param node_id Graph node id to inspect.
   * @return Pointer to staged output, or nullptr when absent.
   * @throws Nothing directly.
   * @note The pointer remains valid until buffer destruction or commit.
   */
  const NodeOutput* find_output(int node_id) const;

  /**
   * @brief Checks whether staged RT output exists for one node.
   *
   * @param node_id Graph node id to inspect.
   * @return True when find_output(node_id) returns non-null.
   * @throws Nothing directly.
   */
  bool has_output(int node_id) const;

  /**
   * @brief Ensures a writable staged RT proxy output exists for one node.
   *
   * @param node_id Original GraphModel node id.
   * @return Mutable staged output owned by this buffer.
   * @throws GraphError when existing proxy image output cannot be cloned; may
   * throw std::bad_alloc while allocating map entries or image storage.
   * @note The staged output is seeded from RealtimeProxyGraph only when this
   * buffer was constructed with seed_existing_outputs=true.
   */
  NodeOutput& ensure_output(int node_id);

  /**
   * @brief Records RT proxy metadata for one successful dirty update.
   *
   * @param node_id Graph node id whose proxy output was updated.
   * @param roi_hp HP-space ROI represented by this RT update.
   * @param hp_size Full HP-space extent used to clamp merged ROI metadata.
   * @param dirty_source Whether the node is a dirty source boundary.
   * @param dirty_generation Dirty generation committed for source nodes.
   * @return New staged RT proxy version after incrementing.
   * @throws std::bad_alloc if the staged entry must be created.
   */
  int mark_updated(int node_id, const cv::Rect& roi_hp, const cv::Size& hp_size,
                   bool dirty_source, uint64_t dirty_generation);

  /**
   * @brief Moves all staged RT proxy state into RealtimeProxyGraph.
   *
   * @throws GraphError when proxy output validation fails; may throw
   * std::bad_alloc while committing defensive node entries.
   * @note GraphModel is not read or mutated during this commit.
   */
  void commit_to_proxy_graph();

 private:
  /**
   * @brief Complete staged RT proxy state for one node id.
   *
   * @note `initialized` records whether committed proxy metadata has been
   * captured. `has_output` controls whether the entry is committed.
   */
  struct Entry {
    bool initialized = false;
    bool has_output = false;
    RealtimeProxyGraph::NodeState state;
  };

  /**
   * @brief Ensures an entry exists and has proxy metadata initialized.
   *
   * @param node_id Original GraphModel node id.
   * @return Mutable staged entry.
   * @throws GraphError when image cloning fails.
   * @note The caller must hold mutex_.
   */
  Entry& ensure_entry_locked(int node_id);

  /** @brief Committed proxy graph that receives staged RT output. */
  RealtimeProxyGraph& proxy_graph_;

  /** @brief Whether entries seed output pixels from committed proxy state. */
  bool seed_existing_outputs_ = true;

  /** @brief Mutex protecting staged entry creation and metadata updates. */
  mutable std::mutex mutex_;

  /** @brief Staged RT entries keyed by original GraphModel node id. */
  std::map<int, Entry> entries_;
};

}  // namespace ps::compute
