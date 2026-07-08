#pragma once

#include <optional>
#include <string>
#include <vector>

#include "graph_model.hpp"  // NOLINT(build/include_subdir)

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

struct GraphNodeInspectInfo {
  int id = -1;
  std::string name;
  std::string type;
  std::string subtype;
  YAML::Node parameters;
  std::optional<NodeMetadataSummary> metadata;
};

struct GraphInspectionSnapshot {
  std::vector<GraphNodeInspectInfo> nodes;
};

struct DependencyTreeEntry {
  int depth = 0;
  std::optional<GraphTopologyEdge> incoming_edge;
  GraphNodeInspectInfo node;
  bool cycle = false;
};

struct DependencyTree {
  enum class Scope {
    EndingNodes,
    StartNode,
  };

  Scope scope = Scope::EndingNodes;
  std::optional<int> start_node_id;
  bool graph_empty = false;
  bool start_node_found = true;
  bool no_ending_nodes = false;
  std::vector<int> root_node_ids;
  std::vector<DependencyTreeEntry> entries;
};

class GraphInspectService {
 public:
  GraphNodeInspectInfo inspect_node(const Node& node,
                                    bool include_metadata = true) const;

  GraphInspectionSnapshot inspect_graph(const GraphModel& graph,
                                        bool include_metadata = true) const;

  DependencyTree dependency_tree(const GraphModel& graph,
                                 bool include_metadata = false) const;
  DependencyTree dependency_tree(const GraphModel& graph, int start_node_id,
                                 bool include_metadata = false) const;
};

}  // namespace ps
