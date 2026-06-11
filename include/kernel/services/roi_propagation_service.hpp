#pragma once

#include <opencv2/core.hpp>
#include <optional>
#include <unordered_map>

#include "graph_model.hpp"
#include "kernel/services/graph_extent_resolver.hpp"

namespace ps {

class RoiPropagationService {
 public:
  cv::Rect compute_upstream_roi(
      const Node& node, const cv::Rect& downstream_roi, const GraphModel& graph,
      std::unordered_map<int, cv::Size>& size_cache) const;

  std::optional<cv::Rect> project_roi_forward(const GraphModel& graph,
                                              int start_node_id,
                                              const cv::Rect& start_roi,
                                              int target_node_id) const;

  std::optional<cv::Rect> project_roi_backward(const GraphModel& graph,
                                               int target_node_id,
                                               const cv::Rect& target_roi,
                                               int source_node_id) const;

 private:
  GraphExtentResolver extent_resolver_;
};

}  // namespace ps
