#pragma once

#include <ostream>
#include <unordered_set>
#include <vector>

#include "graph_model.hpp"

namespace ps {

class GraphTraversalService {
 public:
  std::vector<int> topo_postorder_from(const GraphModel& graph,
                                       int end_node_id) const;
  std::vector<int> ending_nodes(const GraphModel& graph) const;
  bool is_ancestor(const GraphModel& graph, int potential_ancestor_id,
                   int node_id, std::unordered_set<int>& visited) const;
  std::vector<int> parents_of(const GraphModel& graph, int node_id) const;
  std::vector<int> get_trees_containing_node(const GraphModel& graph,
                                             int node_id) const;

  void print_dependency_tree(const GraphModel& graph, std::ostream& os,
                             bool show_parameters = true) const;
  void print_dependency_tree(const GraphModel& graph, std::ostream& os,
                             int start_node_id,
                             bool show_parameters = true) const;
};

}  // namespace ps
