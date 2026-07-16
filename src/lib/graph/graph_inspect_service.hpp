#pragma once

#include <optional>
#include <string>
#include <vector>

#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps {

/**
 * @brief Cached output metadata copied for node inspection.
 *
 * @throws Nothing for value construction except string allocation when copied.
 * @note This internal summary keeps implementation-owned cache objects out of
 *       frontend-facing inspection snapshots while preserving the output
 *       dimensions needed to describe local pixel space.
 */
struct NodeMetadataSummary {
  /** @brief Whether a high-precision cached output was available. */
  bool has_cached_output = false;

  /** @brief Human-readable label for the cache source that produced metadata.
   */
  std::string source_label;

  /** @brief Cached output width in local pixels. */
  int output_width = 0;

  /** @brief Cached output height in local pixels. */
  int output_height = 0;

  /** @brief Debug metadata copied from the cached output. */
  DebugMeta debug;

  /** @brief Spatial transform and absolute ROI copied from the cached output.
   */
  SpatialContext space;
};

/**
 * @brief Internal value snapshot for one inspected graph node.
 *
 * @throws std::bad_alloc when copied string, YAML, or metadata storage
 * exhausts memory.
 * @note The snapshot owns copied values and exposes no mutable Node reference.
 */
struct GraphNodeInspectInfo {
  /** @brief Stable graph-local node id. */
  int id = -1;

  /** @brief Human-readable node name. */
  std::string name;

  /** @brief Registered operation type. */
  std::string type;

  /** @brief Registered operation subtype. */
  std::string subtype;

  /** @brief Cloned static parameter map. */
  YAML::Node parameters;

  /** @brief Optional copied cache/spatial metadata. */
  std::optional<NodeMetadataSummary> metadata;
};

/**
 * @brief Internal value snapshot for all nodes in one graph.
 *
 * @throws std::bad_alloc when node snapshot storage grows.
 * @note Node order follows GraphModel::node_ids() for deterministic frontends.
 */
struct GraphInspectionSnapshot {
  /** @brief Copied node inspection rows. */
  std::vector<GraphNodeInspectInfo> nodes;
};

/**
 * @brief One flattened dependency-tree row.
 *
 * @throws std::bad_alloc when copied node or edge values allocate.
 * @note depth is a display indentation value, not a topology owner.
 */
struct DependencyTreeEntry {
  /** @brief Display indentation depth for this row. */
  int depth = 0;

  /** @brief Incoming topology edge, absent for a root row. */
  std::optional<GraphTopologyEdge> incoming_edge;

  /** @brief Copied node snapshot for this row. */
  GraphNodeInspectInfo node;

  /** @brief Whether this row closes a cycle in the current traversal path. */
  bool cycle = false;
};

/**
 * @brief Flattened dependency-tree inspection result.
 *
 * @throws std::bad_alloc when root or entry storage grows.
 * @note This is a copied diagnostic view and never mutates graph topology.
 */
struct DependencyTree {
  /**
   * @brief Root selection policy used for one dependency-tree traversal.
   *
   * @throws Nothing.
   */
  enum class Scope {
    /** @brief Traverse from every graph ending node. */
    EndingNodes,

    /** @brief Traverse from one requested node. */
    StartNode,
  };

  /** @brief Scope selected for this result. */
  Scope scope = Scope::EndingNodes;

  /** @brief Requested node id when scope is StartNode. */
  std::optional<int> start_node_id;

  /** @brief Whether the inspected graph contained no nodes. */
  bool graph_empty = false;

  /** @brief Whether a requested start node exists. */
  bool start_node_found = true;

  /** @brief Whether ending-node scope found no traversal roots. */
  bool no_ending_nodes = false;

  /** @brief Root node ids selected for traversal. */
  std::vector<int> root_node_ids;

  /** @brief Flattened dependency rows in traversal order. */
  std::vector<DependencyTreeEntry> entries;
};

/**
 * @brief Builds copied node, graph, and dependency-tree inspection values.
 *
 * @throws std::bad_alloc when traversal or copied snapshot storage exhausts
 * memory.
 * @note Callers own graph-state serialization. The service retains no graph,
 * node, cache, or snapshot reference after a method returns.
 */
class GraphInspectService {
 public:
  /**
   * @brief Copies one Node into an inspection-safe value.
   *
   * @param node Node to inspect during serialized graph access.
   * @param include_metadata Whether formal HP cache metadata is copied.
   * @return Owned node inspection value.
   * @throws std::bad_alloc if strings, YAML clone, or metadata copy allocates.
   * @throws YAML::Exception if parameter cloning fails for another reason.
   * @note No Node or NodeOutput reference escapes through the result.
   */
  GraphNodeInspectInfo inspect_node(const Node& node,
                                    bool include_metadata = true) const;

  /**
   * @brief Traverses graph ids and copies every node inspection value.
   *
   * @param graph Graph to inspect during serialized graph access.
   * @param include_metadata Whether formal HP cache metadata is copied.
   * @return Owned graph inspection snapshot in deterministic node-id order.
   * @throws std::bad_alloc if traversal, YAML clone, or result storage
   * exhausts memory.
   * @throws GraphError if a node id disappears during caller-unsafe mutation.
   * @throws YAML::Exception if parameter cloning fails for another reason.
   * @note BUILD_TESTING may compile an internal immutable-input failpoint in
   * the collection loop; no callable test seam is installed or exported.
   */
  GraphInspectionSnapshot inspect_graph(const GraphModel& graph,
                                        bool include_metadata = true) const;

  /**
   * @brief Builds a dependency tree rooted at every ending node.
   *
   * @param graph Graph whose upstream topology is traversed.
   * @param include_metadata Whether node cache metadata is copied.
   * @return Owned flattened dependency tree.
   * @throws std::bad_alloc if traversal path or result storage grows.
   * @throws YAML::Exception if copied parameter conversion fails.
   * @note The caller must serialize graph mutation for the full call.
   */
  DependencyTree dependency_tree(const GraphModel& graph,
                                 bool include_metadata = false) const;

  /**
   * @brief Builds a dependency tree rooted at one requested node.
   *
   * @param graph Graph whose upstream topology is traversed.
   * @param start_node_id Requested traversal root.
   * @param include_metadata Whether node cache metadata is copied.
   * @return Owned flattened dependency tree, with start_node_found=false when
   * the requested id is absent.
   * @throws std::bad_alloc if traversal path or result storage grows.
   * @throws YAML::Exception if copied parameter conversion fails.
   * @note The caller must serialize graph mutation for the full call.
   */
  DependencyTree dependency_tree(const GraphModel& graph, int start_node_id,
                                 bool include_metadata = false) const;
};

}  // namespace ps
