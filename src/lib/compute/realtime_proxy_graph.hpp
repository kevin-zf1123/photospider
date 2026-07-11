#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {

/**
 * @brief Low-resolution proxy graph state owned by the RT compute path.
 *
 * RealtimeProxyGraph mirrors only graph node ids and stores transient
 * low-resolution output state for those ids. It deliberately does not copy
 * Node parameters, inputs, topology edges, caches, or HP runtime state from
 * GraphModel. The original GraphModel remains the authoritative HP graph.
 *
 * @note Callers must synchronize the proxy against GraphModel before RT
 * planning or commit when topology may have changed. The proxy owns its own
 * mutex so RT preview state can be committed without mutating GraphModel node
 * cache fields.
 */
class RealtimeProxyGraph {
 public:
  /**
   * @brief Transient RT state for one graph node id.
   *
   * @note ROI metadata is stored in HP coordinates so existing dirty-region
   * inspection and frontend mapping semantics remain consistent.
   */
  struct NodeState {
    /** @brief Low-resolution RT output for this node, when materialized. */
    std::optional<NodeOutput> output;

    /** @brief Most recent or merged HP-space ROI represented by RT output. */
    std::optional<cv::Rect> roi_hp;

    /** @brief Version counter advanced on every RT proxy update. */
    int version = 0;

    /** @brief Dirty source generation committed by the RT path, when any. */
    std::optional<uint64_t> dirty_source_generation;
  };

  /**
   * @brief Synchronizes proxy node ids with the current GraphModel topology.
   *
   * @param graph Source graph whose node ids and topology generation are used.
   * @throws std::bad_alloc if map storage grows.
   * @note Existing NodeState payloads are preserved only while the observed
   * graph topology generation is unchanged. Reload, replacement, clear, and
   * topology edits reset all live proxy entries so reused node ids cannot read
   * output from a previous graph state.
   */
  void synchronize_with_graph(const GraphModel& graph);

  /**
   * @brief Returns the topology generation last observed during sync.
   *
   * @return GraphModel topology_generation() captured by the latest
   * synchronize_with_graph() call.
   * @throws Nothing.
   */
  uint64_t topology_generation() const;

  /**
   * @brief Clears proxy state for nodes selected by an RT dirty plan.
   *
   * @param node_ids Graph node ids whose proxy output and metadata are reset.
   * @throws std::bad_alloc only if map lookup inserts missing ids.
   * @note This affects RT preview state only and never mutates GraphModel.
   */
  void reset_nodes(const std::vector<int>& node_ids);

  /**
   * @brief Returns committed RT proxy output for one node.
   *
   * @param node_id Graph node id to inspect.
   * @return Pointer to committed proxy output, or nullptr when no proxy output
   * exists for that node.
   * @throws Nothing directly.
   * @note The returned pointer remains valid until the next proxy mutation for
   * that node. Current compute requests are serialized by GraphStateExecutor,
   * so dirty worker reads are not concurrent with other request commits.
   */
  const NodeOutput* find_output(int node_id) const;

  /**
   * @brief Returns the full committed proxy state for one node.
   *
   * @param node_id Graph node id to inspect.
   * @return Pointer to proxy state, or nullptr when the proxy has no matching
   * node entry.
   * @throws Nothing directly.
   * @note Used by write buffers to seed staged output and version metadata.
   */
  const NodeState* find_state(int node_id) const;

  /**
   * @brief Looks up committed RT dirty-source generation metadata.
   *
   * @param node_id Graph node id to inspect.
   * @return Generation value when committed for this proxy node, otherwise 0.
   * @throws Nothing directly.
   * @note Zero matches the legacy "not committed" generation behavior used by
   * stale source checks.
   */
  uint64_t dirty_source_generation(int node_id) const;

  /**
   * @brief Commits one staged RT proxy node state.
   *
   * @param node_id Graph node id receiving the staged state.
   * @param state New proxy state to store.
   * @throws std::bad_alloc if the proxy node id is not present and must be
   * inserted defensively.
   * @note The value is moved into proxy storage; GraphModel is not touched.
   */
  void commit_node_state(int node_id, NodeState state);

  /**
   * @brief Validates and returns a mutable RT proxy output.
   *
   * @param node_id Graph node id whose proxy output is required.
   * @return Mutable output owned by the proxy graph.
   * @throws GraphError when no proxy output exists for the requested node.
   * @note The returned reference remains valid until the proxy node is reset or
   * overwritten by a later RT commit.
   */
  NodeOutput& require_output(int node_id);

 private:
  /** @brief Mutex protecting proxy node state and topology generation. */
  mutable std::mutex mutex_;

  /** @brief Proxy state keyed by original GraphModel node id. */
  std::map<int, NodeState> nodes_;

  /** @brief Last GraphModel topology generation synchronized into the proxy. */
  uint64_t topology_generation_ = 0;
};

}  // namespace ps::compute
