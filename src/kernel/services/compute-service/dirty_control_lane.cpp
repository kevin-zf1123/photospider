#include "kernel/services/compute-service/dirty_control_lane.hpp"

#include "kernel/services/compute-service/dirty_region_planner.hpp"

namespace ps::compute {

namespace {

bool has_dispatchable_dirty_region(const DirtyRegionSnapshot& snapshot) {
  return !snapshot.actual_dirty_rois.empty() ||
         !snapshot.per_node_dirty_rois.empty() ||
         !snapshot.dirty_tiles.empty() ||
         !snapshot.dirty_monolithic_nodes.empty();
}

}  // namespace

DirtyControlLane::DirtyControlLane(GraphTraversalService& traversal,
                                   RoiPropagationService& roi_propagation)
    : traversal_(traversal), roi_propagation_(roi_propagation) {}

DirtyControlLaneResult DirtyControlLane::begin_dirty_source(
    GraphModel& graph, int node_id, DirtyDomain domain,
    const cv::Rect& source_roi) const {
  DirtyRegionPlanner planner(traversal_, roi_propagation_);
  return build_result(
      planner.begin_dirty_source(graph, node_id, domain, source_roi),
      DirtyControlEvent::Begin);
}

DirtyControlLaneResult DirtyControlLane::update_dirty_source(
    GraphModel& graph, int node_id, DirtyDomain domain,
    const cv::Rect& source_roi) const {
  DirtyRegionPlanner planner(traversal_, roi_propagation_);
  return build_result(
      planner.update_dirty_source(graph, node_id, domain, source_roi),
      DirtyControlEvent::Update);
}

DirtyControlLaneResult DirtyControlLane::end_dirty_source(
    GraphModel& graph, int node_id, DirtyDomain domain) const {
  DirtyRegionPlanner planner(traversal_, roi_propagation_);
  return build_result(planner.end_dirty_source(graph, node_id, domain),
                      DirtyControlEvent::End);
}

DirtyControlLaneResult DirtyControlLane::build_result(
    const DirtyRegionSnapshot& snapshot, DirtyControlEvent event) const {
  const bool has_dirty_region = has_dispatchable_dirty_region(snapshot);
  DirtyControlLaneResult result;
  result.snapshot = snapshot;
  result.event = event;
  result.generation = snapshot.graph_generation;
  result.dirty_updating_count = snapshot.dirty_updating_count;
  result.should_wake_dispatcher = has_dirty_region;
  result.cutoff_after_downstream = event == DirtyControlEvent::End &&
                                   snapshot.dirty_updating_count == 0 &&
                                   has_dirty_region;
  return result;
}

}  // namespace ps::compute
