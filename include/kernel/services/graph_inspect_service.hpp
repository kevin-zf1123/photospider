#pragma once

#include <optional>
#include <string>
#include <vector>

#include "graph_model.hpp"

namespace ps {

struct NodeMetadataSummary {
  bool has_cached_output = false;
  std::string source_label;
  DebugMeta debug;
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
