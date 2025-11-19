#pragma once

#include <functional>
#include <ostream>
#include <optional>
#include <unordered_set>
#include <vector>

#include <opencv2/core.hpp>

#include "graph_model.hpp"

namespace ps {

class GraphTraversalService {
 public:
  using MetadataFormatter = std::function<std::string(const Node&)>;

  std::vector<int> topo_postorder_from(const GraphModel& graph,
                                       int end_node_id) const;
  std::vector<int> ending_nodes(const GraphModel& graph) const;
  bool is_ancestor(const GraphModel& graph, int potential_ancestor_id,
                   int node_id, std::unordered_set<int>& visited) const;
  std::vector<int> parents_of(const GraphModel& graph, int node_id) const;
  std::vector<int> get_trees_containing_node(const GraphModel& graph,
                                             int node_id) const;

  void print_dependency_tree(const GraphModel& graph, std::ostream& os,
                             bool show_parameters = true,
                             bool show_metadata = false,
                             MetadataFormatter formatter = nullptr) const;
  void print_dependency_tree(const GraphModel& graph, std::ostream& os,
                             int start_node_id,
                             bool show_parameters = true,
                             bool show_metadata = false,
                             MetadataFormatter formatter = nullptr) const;

  std::optional<cv::Rect> project_roi_forward(const GraphModel& graph,
                                              int start_node_id,
                                              const cv::Rect& start_roi,
                                              int target_node_id) const;

  std::optional<cv::Rect> project_roi_backward(const GraphModel& graph,
                                               int target_node_id,
                                               const cv::Rect& target_roi,
                                               int source_node_id) const;
};

}  // namespace ps
