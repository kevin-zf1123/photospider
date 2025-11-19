#pragma once

#include <string>

#include "graph_model.hpp"

namespace ps {

class GraphInspectService {
 public:
  // Human-readable metadata dump for a single node.
  std::string format_node_metadata(const Node& node) const;

  // Aggregate metadata for all nodes in a graph.
  std::string inspect_all_nodes(const GraphModel& graph) const;
};

}  // namespace ps
