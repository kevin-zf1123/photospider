#pragma once

#include <map>
#include <mutex>
#include <optional>

#include "graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {

/**
 * @brief Request-local staging buffer for real-time dirty output writes.
 *
 * Realtime dirty node execution writes proxy `NodeOutput` values, ROI metadata,
 * version counters, and dirty-source generation commits into this buffer first.
 * The owning `RealTimeDirtyExecutor` commits the buffered state to `GraphModel`
 * only after all scheduler or inline dirty work for the request has completed.
 *
 * @note Existing graph RT output is deep-copied into the buffer before ROI
 * writes. This keeps worker writes from mutating visible graph-owned
 * `ImageBuffer` storage through shared pointers before the commit boundary.
 */
class RealtimeDirtyWriteBuffer {
 public:
  /**
   * @brief Returns a staged output for one node when it has been materialized.
   *
   * @param node_id Graph node id to inspect.
   * @return Pointer to the staged output, or nullptr when the node has not
   * produced RT output in this request.
   * @throws Nothing directly.
   * @note The returned pointer remains valid until commit or destruction of the
   * buffer. Callers must rely on task dependencies to avoid reading a staged
   * output while another task is still writing that same node.
   */
  const NodeOutput* find_output(int node_id) const;

  /**
   * @brief Checks whether the request has staged RT output for one node.
   *
   * @param node_id Graph node id to inspect.
   * @return True when find_output(node_id) would return a non-null pointer.
   * @throws Nothing directly.
   * @note Used by dirty-source boundary validation before graph commit.
   */
  bool has_output(int node_id) const;

  /**
   * @brief Ensures one node has a writable staged RT output.
   *
   * @param node Graph node whose existing RT output and metadata seed the
   * staged entry.
   * @return Mutable staged output owned by this request buffer.
   * @throws GraphError when an existing image output cannot be copied; may
   * throw std::bad_alloc while allocating map entries or deep-copy storage.
   * @note If the graph node already has RT output, the output is deep-copied.
   * Otherwise an empty NodeOutput is created and later allocation chooses the
   * planned RT extent.
   */
  NodeOutput& ensure_output(const Node& node);

  /**
   * @brief Records RT ROI, version, and dirty-source generation metadata.
   *
   * @param node_id Graph node id whose staged output was updated.
   * @param roi_hp HP-space ROI represented by the RT update.
   * @param hp_size HP-space full extent used to clamp merged ROI metadata.
   * @param dirty_source Whether the node is a dirty source boundary.
   * @param dirty_generation Dirty generation committed for dirty source nodes.
   * @throws std::bad_alloc if the staged entry must be created.
   * @note The version counter starts from the graph node's RT version captured
   * by ensure_output() and advances once per successful node update.
   */
  void mark_updated(int node_id, const cv::Rect& roi_hp,
                    const cv::Size& hp_size, bool dirty_source,
                    uint64_t dirty_generation);

  /**
   * @brief Moves all staged RT state into the visible graph model.
   *
   * @param graph Graph receiving staged RT outputs and metadata.
   * @throws GraphError when a staged node no longer exists in the graph.
   * @note The caller must hold graph.graph_mutex_. This method deliberately
   * performs no graph locking so the commit boundary remains explicit in the
   * owning executor.
   */
  void commit_to_graph(GraphModel& graph);

 private:
  /**
   * @brief Complete staged RT state for one graph node.
   *
   * @note `initialized` distinguishes an entry seeded from graph state from an
   * empty default map entry. `has_output` indicates whether an actual staged
   * output should be committed.
   */
  struct Entry {
    bool initialized = false;
    bool has_output = false;
    NodeOutput output;
    std::optional<cv::Rect> rt_roi;
    int rt_version = 0;
    std::optional<uint64_t> dirty_source_generation;
  };

  /** @brief Mutex protecting staged entry creation and metadata updates. */
  mutable std::mutex mutex_;

  /**
   * @brief Staged RT entries keyed by graph node id.
   *
   * @note std::map keeps pointers and references to existing entries stable
   * while other worker tasks stage additional nodes.
   */
  std::map<int, Entry> entries_;
};

}  // namespace ps::compute
