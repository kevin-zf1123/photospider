/**
 * @file kernel_dirty_roi_facade.cpp
 * @brief Implements Kernel ROI projection and dirty-source lifecycle facades.
 */
#include "kernel/kernel.hpp"

namespace ps {

std::optional<cv::Rect> Kernel::project_roi_forward(const std::string& name,
                                                    int start_node_id,
                                                    const cv::Rect& start_roi,
                                                    int target_node_id) {
  return with_graph_state_last_error(
      name, "ROI projection failed: ",
      [this, start_node_id, start_roi, target_node_id](GraphModel& graph) {
        return roi_propagation_service_.project_roi_forward(
            graph, start_node_id, start_roi, target_node_id);
      },
      true);
}

std::optional<cv::Rect> Kernel::project_roi_backward(const std::string& name,
                                                     int target_node_id,
                                                     const cv::Rect& target_roi,
                                                     int source_node_id) {
  return with_graph_state_last_error(
      name, "ROI back-projection failed: ",
      [this, target_node_id, target_roi, source_node_id](GraphModel& graph) {
        return roi_propagation_service_.project_roi_backward(
            graph, target_node_id, target_roi, source_node_id);
      },
      true);
}

std::optional<compute::DirtyRegionSnapshot> Kernel::begin_dirty_source(
    const std::string& name, int node_id, compute::DirtyDomain domain,
    const cv::Rect& source_roi) {
  auto result = begin_dirty_source_control(name, node_id, domain, source_roi);
  if (!result) {
    return std::nullopt;
  }
  return result->snapshot;
}

std::optional<compute::DirtyControlLaneResult>
Kernel::begin_dirty_source_control(const std::string& name, int node_id,
                                   compute::DirtyDomain domain,
                                   const cv::Rect& source_roi) {
  return with_graph_state_last_error(
      name, "Dirty source begin failed: ",
      [this, node_id, domain, source_roi](GraphModel& graph) {
        compute::DirtyControlLane lane(traversal_service_,
                                       roi_propagation_service_);
        return lane.begin_dirty_source(graph, node_id, domain, source_roi);
      });
}

std::optional<compute::DirtyRegionSnapshot> Kernel::update_dirty_source(
    const std::string& name, int node_id, compute::DirtyDomain domain,
    const cv::Rect& source_roi) {
  auto result = update_dirty_source_control(name, node_id, domain, source_roi);
  if (!result) {
    return std::nullopt;
  }
  return result->snapshot;
}

std::optional<compute::DirtyControlLaneResult>
Kernel::update_dirty_source_control(const std::string& name, int node_id,
                                    compute::DirtyDomain domain,
                                    const cv::Rect& source_roi) {
  return with_graph_state_last_error(
      name, "Dirty source update failed: ",
      [this, node_id, domain, source_roi](GraphModel& graph) {
        compute::DirtyControlLane lane(traversal_service_,
                                       roi_propagation_service_);
        return lane.update_dirty_source(graph, node_id, domain, source_roi);
      });
}

std::optional<compute::DirtyRegionSnapshot> Kernel::end_dirty_source(
    const std::string& name, int node_id, compute::DirtyDomain domain) {
  auto result = end_dirty_source_control(name, node_id, domain);
  if (!result) {
    return std::nullopt;
  }
  return result->snapshot;
}

std::optional<compute::DirtyControlLaneResult> Kernel::end_dirty_source_control(
    const std::string& name, int node_id, compute::DirtyDomain domain) {
  return with_graph_state_last_error(
      name,
      "Dirty source end failed: ", [this, node_id, domain](GraphModel& graph) {
        compute::DirtyControlLane lane(traversal_service_,
                                       roi_propagation_service_);
        return lane.end_dirty_source(graph, node_id, domain);
      });
}

}  // namespace ps
