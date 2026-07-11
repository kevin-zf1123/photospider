#include "compute/dirty_region_planner.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "compute/compute_geometry.hpp"
#include "compute/dirty_region_planning_policy.hpp"
#include "core/param_utils.hpp"
#include "graph/graph_traversal_service.hpp"
#include "graph/roi_propagation_service.hpp"

namespace ps::compute {
using detail::has_valid_size;
using detail::HighPrecisionDirtyPolicy;
using detail::RealTimeDirtyPolicy;

bool DirtyRegionSnapshot::empty() const {
  return dirty_source_nodes.empty() && dirty_tiles.empty() &&
         dirty_monolithic_nodes.empty() && per_node_dirty_rois.empty() &&
         actual_dirty_rois.empty() && edge_mappings.empty();
}

DirtyRegionPlanner::DirtyRegionPlanner(GraphTraversalService& traversal,
                                       RoiPropagationService& roi_propagation)
    : traversal_(traversal), roi_propagation_(roi_propagation) {}

template <typename Policy>
typename Policy::Entry& DirtyRegionPlanner::ensure_plan_entry(
    GraphModel& graph, typename Policy::Plan& plan, int node_id,
    std::unordered_map<int, cv::Size>& hp_size_cache) {
  auto [it, inserted] = plan.entries.emplace(node_id, typename Policy::Entry{});
  typename Policy::Entry& entry = it->second;
  if (inserted || !has_valid_size(entry.hp_size)) {
    entry.hp_size = infer_hp_size(graph, node_id, hp_size_cache);
    Policy::refresh_size_fields(entry);
  }
  if (inserted || entry.halo_hp == 0) {
    entry.halo_hp = infer_halo_hp(graph.node(node_id));
    Policy::refresh_halo_fields(entry);
  }
  return entry;
}

template <typename Policy>
void DirtyRegionPlanner::propagate_dirty_entries(
    GraphModel& graph, typename Policy::Plan& plan,
    std::unordered_map<int, cv::Size>& hp_size_cache) {
  std::unordered_map<int, cv::Size> size_cache;
  for (auto it = plan.execution_order.rbegin();
       it != plan.execution_order.rend(); ++it) {
    const int current_id = *it;
    auto plan_it = plan.entries.find(current_id);
    if (plan_it == plan.entries.end())
      continue;
    typename Policy::Entry& current_entry = plan_it->second;
    if (is_rect_empty(current_entry.roi_hp))
      continue;

    const Node& current_node = graph.node(current_id);
    current_entry.halo_hp =
        std::max(current_entry.halo_hp, infer_halo_hp(current_node));
    Policy::refresh_halo_fields(current_entry);

    cv::Rect upstream_roi_hp = roi_propagation_.compute_upstream_roi(
        current_node, current_entry.roi_hp, graph, size_cache);
    upstream_roi_hp =
        Policy::normalize_upstream_roi(upstream_roi_hp, current_entry);
    if (Policy::skip_empty_upstream_roi(upstream_roi_hp))
      continue;

    for (const auto& edge : graph.upstream_edges(current_id)) {
      if (edge.kind != GraphTopologyEdgeKind::ImageInput ||
          edge.from_node_id < 0)
        continue;
      typename Policy::Entry& parent_entry = ensure_plan_entry<Policy>(
          graph, plan, edge.from_node_id, hp_size_cache);
      cv::Rect parent_roi =
          Policy::parent_hp_roi(upstream_roi_hp, parent_entry);
      if (is_rect_empty(parent_roi))
        continue;
      parent_entry.roi_hp =
          is_rect_empty(parent_entry.roi_hp)
              ? parent_roi
              : clip_rect(merge_rect(parent_entry.roi_hp, parent_roi),
                          parent_entry.hp_size);
      plan.snapshot.edge_mappings.push_back(
          {edge.from_node_id, current_id, Policy::kDomain, parent_roi,
           current_entry.roi_hp, DirtyEdgeDirection::BackwardDemand});
    }
  }
}

template <typename Policy>
void DirtyRegionPlanner::finalize_dirty_entries(GraphModel& graph,
                                                typename Policy::Plan& plan) {
  std::vector<int> erase_ids;
  for (auto& [node_id, entry] : plan.entries) {
    if (!has_valid_size(entry.hp_size)) {
      erase_ids.push_back(node_id);
      continue;
    }
    entry.roi_hp = Policy::finalize_hp_roi(entry.roi_hp, entry.hp_size);
    if (is_rect_empty(entry.roi_hp)) {
      erase_ids.push_back(node_id);
      continue;
    }
    Policy::refresh_size_fields(entry);
    if (!Policy::refresh_domain_roi(entry)) {
      erase_ids.push_back(node_id);
      continue;
    }
    if (entry.halo_hp == 0) {
      entry.halo_hp = infer_halo_hp(graph.node(node_id));
      Policy::refresh_halo_fields(entry);
    }
    const Node& node = graph.node(node_id);
    if (snapshot_builder_.is_monolithic_boundary(node)) {
      Policy::promote_monolithic(entry);
    }
    snapshot_builder_.append_node_work(
        plan.snapshot, DirtyNodeWorkRecord{&node, node_id, Policy::kDomain,
                                           Policy::snapshot_work_roi(entry),
                                           Policy::tile_size()});
    plan.snapshot.per_node_dirty_rois[node_id].push_back(entry.roi_hp);
  }
  for (int node_id : erase_ids)
    plan.entries.erase(node_id);
}

template <typename Policy>
typename Policy::Plan DirtyRegionPlanner::plan_dirty_domain(
    GraphModel& graph, int node_id, const cv::Rect& dirty_roi) {
  if (!graph.has_node(node_id)) {
    throw GraphError(GraphErrc::NotFound,
                     std::string("Cannot compute ") + Policy::kIntentLabel +
                         " update: node " + std::to_string(node_id) +
                         " not found.");
  }
  if (is_rect_empty(dirty_roi)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     std::string("Cannot compute ") + Policy::kIntentLabel +
                         " update: dirty ROI is empty.");
  }

  typename Policy::Plan result;
  result.execution_order = traversal_.topo_postorder_from(graph, node_id);
  if (result.execution_order.empty())
    result.execution_order.push_back(node_id);
  result.snapshot.graph_generation = ++graph.dirty_generation_counter;

  std::unordered_map<int, cv::Size> hp_size_cache;
  typename Policy::Entry& target_entry =
      ensure_plan_entry<Policy>(graph, result, node_id, hp_size_cache);
  target_entry.roi_hp = Policy::target_hp_roi(dirty_roi, target_entry);
  if (is_rect_empty(target_entry.roi_hp)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Dirty ROI does not intersect node output.");
  }
  if (!Policy::refresh_domain_roi(target_entry)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     Policy::domain_roi_empty_message());
  }

  propagate_dirty_entries<Policy>(graph, result, hp_size_cache);
  finalize_dirty_entries<Policy>(graph, result);
  if (result.entries.empty())
    throw GraphError(GraphErrc::InvalidParameter, Policy::kEmptyPlanMessage);
  result.snapshot.actual_dirty_rois = result.snapshot.per_node_dirty_rois;
  populate_dirty_source_metadata(graph, result.snapshot, Policy::kDomain,
                                 result.entries);
  return result;
}

HighPrecisionDirtyPlan DirtyRegionPlanner::plan_high_precision(
    GraphModel& graph, int node_id, const cv::Rect& dirty_roi) {
  return plan_dirty_domain<HighPrecisionDirtyPolicy>(graph, node_id, dirty_roi);
}

RealTimeDirtyPlan DirtyRegionPlanner::plan_real_time(
    GraphModel& graph, int node_id, const cv::Rect& dirty_roi) {
  return plan_dirty_domain<RealTimeDirtyPolicy>(graph, node_id, dirty_roi);
}

DirtyRegionSnapshot DirtyRegionPlanner::begin_dirty_source(
    GraphModel& graph, int node_id, DirtyDomain domain,
    const cv::Rect& source_roi) {
  return update_dirty_source_snapshot(graph, node_id, domain, &source_roi,
                                      DirtySourceLifecycleState::Updating);
}

DirtyRegionSnapshot DirtyRegionPlanner::update_dirty_source(
    GraphModel& graph, int node_id, DirtyDomain domain,
    const cv::Rect& source_roi) {
  return update_dirty_source_snapshot(graph, node_id, domain, &source_roi,
                                      DirtySourceLifecycleState::Updating);
}

DirtyRegionSnapshot DirtyRegionPlanner::end_dirty_source(GraphModel& graph,
                                                         int node_id,
                                                         DirtyDomain domain) {
  return update_dirty_source_snapshot(graph, node_id, domain, nullptr,
                                      DirtySourceLifecycleState::Settled);
}

DirtyRegionSnapshot DirtyRegionPlanner::update_dirty_source_snapshot(
    GraphModel& graph, int node_id, DirtyDomain domain,
    const cv::Rect* source_roi, DirtySourceLifecycleState lifecycle) {
  DirtyRegionSnapshot snapshot =
      graph.last_dirty_region_snapshot.value_or(DirtyRegionSnapshot{});
  if (snapshot.graph_generation == 0) {
    snapshot.graph_generation = ++graph.dirty_generation_counter;
  }
  snapshot_builder_.apply_source_lifecycle_event(
      graph, snapshot,
      DirtySourceLifecycleUpdate{node_id, domain, source_roi, lifecycle});
  snapshot_builder_.refresh_actual_dirty_regions(graph, snapshot, domain);
  graph.last_dirty_region_snapshot = snapshot;
  graph.recent_dirty_region_snapshots.push_back(snapshot);
  if (graph.recent_dirty_region_snapshots.size() > 16) {
    graph.recent_dirty_region_snapshots.erase(
        graph.recent_dirty_region_snapshots.begin());
  }
  graph.last_dirty_region_snapshot_debug = describe_snapshot(snapshot);
  return snapshot;
}

template <typename EntryMap>
void DirtyRegionPlanner::populate_dirty_source_metadata(
    GraphModel& graph, DirtyRegionSnapshot& snapshot, DirtyDomain domain,
    const EntryMap& entries) const {
  std::unordered_set<int> entry_nodes;
  entry_nodes.reserve(entries.size());
  for (const auto& [node_id, _] : entries) {
    entry_nodes.insert(node_id);
  }

  std::vector<int> source_nodes;
  for (const auto& [node_id, _] : entries) {
    bool has_planned_image_parent = false;
    for (const auto& edge : graph.upstream_edges(node_id)) {
      if (edge.kind == GraphTopologyEdgeKind::ImageInput &&
          entry_nodes.count(edge.from_node_id)) {
        has_planned_image_parent = true;
        break;
      }
    }
    if (!has_planned_image_parent) {
      source_nodes.push_back(node_id);
    }
  }
  std::sort(source_nodes.begin(), source_nodes.end());

  for (int source_node_id : source_nodes) {
    if (std::find(snapshot.dirty_source_nodes.begin(),
                  snapshot.dirty_source_nodes.end(),
                  source_node_id) == snapshot.dirty_source_nodes.end()) {
      snapshot.dirty_source_nodes.push_back(source_node_id);
    }
    DirtySourceNodeState& state = snapshot.dirty_source_state[source_node_id];
    state.node_id = source_node_id;
    state.domain = domain;
    state.lifecycle = DirtySourceLifecycleState::Settled;
    state.generation = snapshot.graph_generation;

    auto roi_it = snapshot.per_node_dirty_rois.find(source_node_id);
    if (roi_it == snapshot.per_node_dirty_rois.end()) {
      continue;
    }
    for (const auto& roi : roi_it->second) {
      if (is_rect_empty(roi)) {
        continue;
      }
      state.source_rois.push_back(roi);
      snapshot.source_roi_records[source_node_id].push_back(
          {source_node_id, domain, roi, snapshot.graph_generation});
    }
  }
  snapshot.dirty_updating_count = 0;
}

cv::Size DirtyRegionPlanner::infer_hp_size(
    GraphModel& graph, int node_id,
    std::unordered_map<int, cv::Size>& cache) const {
  if (cache.count(node_id))
    return cache.at(node_id);

  cv::Size size{0, 0};
  size = extent_resolver_.resolve_output_extent(graph, node_id, cache);
  return size;
}

int DirtyRegionPlanner::infer_halo_hp(const Node& node) const {
  if (node.type != "image_process")
    return 0;
  if (node.subtype == "gaussian_blur" ||
      node.subtype == "gaussian_blur_tiled") {
    int k = as_int_flexible(node.runtime_parameters, "ksize",
                            as_int_flexible(node.parameters, "ksize", 0));
    if (k <= 0)
      k = 3;
    if (k % 2 == 0)
      ++k;
    return std::max(0, k / 2);
  }
  if (node.subtype == "convolve") {
    int radius =
        as_int_flexible(node.runtime_parameters, "kernel_radius",
                        as_int_flexible(node.parameters, "kernel_radius", 0));
    radius = std::max(
        radius, as_int_flexible(node.runtime_parameters, "radius", radius));
    radius =
        std::max(radius, as_int_flexible(node.parameters, "radius", radius));
    int ksize =
        as_int_flexible(node.runtime_parameters, "kernel_size",
                        as_int_flexible(node.parameters, "kernel_size", 0));
    if (ksize <= 0) {
      ksize = as_int_flexible(node.runtime_parameters, "ksize",
                              as_int_flexible(node.parameters, "ksize", 0));
    }
    if (ksize > 0)
      radius = std::max(radius, std::max(0, (ksize - 1) / 2));
    if (radius <= 0)
      radius = 1;
    return radius;
  }
  return 0;
}

std::string DirtyRegionPlanner::describe_snapshot(
    const DirtyRegionSnapshot& snapshot) {
  std::ostringstream out;
  out << "generation=" << snapshot.graph_generation
      << " sources=" << snapshot.dirty_source_nodes.size()
      << " updating=" << snapshot.dirty_updating_count
      << " actual=" << snapshot.actual_dirty_rois.size()
      << " tiles=" << snapshot.dirty_tiles.size()
      << " monolithic=" << snapshot.dirty_monolithic_nodes.size()
      << " nodes=" << snapshot.per_node_dirty_rois.size()
      << " edges=" << snapshot.edge_mappings.size();
  return out.str();
}

}  // namespace ps::compute
