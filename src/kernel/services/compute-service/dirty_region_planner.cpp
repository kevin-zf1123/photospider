#include "kernel/services/compute-service/dirty_region_planner.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_set>

#include "kernel/param_utils.hpp"
#include "kernel/services/compute-service/compute_geometry.hpp"
#include "kernel/services/graph_traversal_service.hpp"
#include "kernel/services/roi_propagation_service.hpp"

namespace ps::compute {

bool DirtyRegionSnapshot::empty() const {
  return dirty_source_nodes.empty() && dirty_tiles.empty() &&
         dirty_monolithic_nodes.empty() && per_node_dirty_rois.empty() &&
         actual_dirty_rois.empty() && edge_mappings.empty();
}

DirtyRegionPlanner::DirtyRegionPlanner(GraphTraversalService& traversal,
                                       RoiPropagationService& roi_propagation)
    : traversal_(traversal), roi_propagation_(roi_propagation) {}

HighPrecisionDirtyPlan DirtyRegionPlanner::plan_high_precision(
    GraphModel& graph, int node_id, const cv::Rect& dirty_roi) {
  if (!graph.has_node(node_id)) {
    throw GraphError(GraphErrc::NotFound, "Cannot compute HP update: node " +
                                              std::to_string(node_id) +
                                              " not found.");
  }
  if (is_rect_empty(dirty_roi)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Cannot compute HP update: dirty ROI is empty.");
  }

  HighPrecisionDirtyPlan result;
  result.execution_order = traversal_.topo_postorder_from(graph, node_id);
  if (result.execution_order.empty())
    result.execution_order.push_back(node_id);
  result.snapshot.graph_generation = ++graph.dirty_generation_counter;

  std::unordered_map<int, cv::Size> hp_size_cache;
  auto ensure_entry = [&](int nid) -> HpPlanEntry& {
    auto [it, inserted] = result.entries.emplace(nid, HpPlanEntry{});
    if (inserted) {
      it->second.hp_size = infer_hp_size(graph, nid, hp_size_cache);
      it->second.halo_hp = infer_halo_hp(graph.node(nid));
    } else {
      if (it->second.hp_size.width <= 0 || it->second.hp_size.height <= 0)
        it->second.hp_size = infer_hp_size(graph, nid, hp_size_cache);
      if (it->second.halo_hp == 0)
        it->second.halo_hp = infer_halo_hp(graph.node(nid));
    }
    return it->second;
  };

  HpPlanEntry& target_entry = ensure_entry(node_id);
  target_entry.roi_hp =
      clip_rect(align_rect(dirty_roi, kHpMicroTileSize), target_entry.hp_size);
  if (is_rect_empty(target_entry.roi_hp)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Dirty ROI does not intersect node output.");
  }

  std::unordered_map<int, cv::Size> size_cache;
  for (auto it = result.execution_order.rbegin();
       it != result.execution_order.rend(); ++it) {
    const int current_id = *it;
    if (!result.entries.count(current_id))
      continue;
    HpPlanEntry& current_entry = result.entries.at(current_id);
    if (is_rect_empty(current_entry.roi_hp))
      continue;

    const Node& current_node = graph.node(current_id);
    current_entry.halo_hp =
        std::max(current_entry.halo_hp, infer_halo_hp(current_node));

    cv::Rect upstream_roi_hp = roi_propagation_.compute_upstream_roi(
        current_node, current_entry.roi_hp, graph, size_cache);
    upstream_roi_hp = align_rect(upstream_roi_hp, kHpMicroTileSize);

    for (const auto& edge : graph.upstream_edges(current_id)) {
      if (edge.kind != GraphTopologyEdgeKind::ImageInput ||
          edge.from_node_id < 0)
        continue;
      HpPlanEntry& parent_entry = ensure_entry(edge.from_node_id);
      cv::Rect parent_roi = clip_rect(upstream_roi_hp, parent_entry.hp_size);
      if (is_rect_empty(parent_roi))
        continue;
      parent_entry.roi_hp =
          is_rect_empty(parent_entry.roi_hp)
              ? parent_roi
              : clip_rect(merge_rect(parent_entry.roi_hp, parent_roi),
                          parent_entry.hp_size);
      result.snapshot.edge_mappings.push_back(
          {edge.from_node_id, current_id, DirtyDomain::HighPrecision,
           parent_roi, current_entry.roi_hp,
           DirtyEdgeDirection::BackwardDemand});
    }
  }

  std::vector<int> erase_ids;
  for (auto& [nid, entry] : result.entries) {
    if (entry.hp_size.width <= 0 || entry.hp_size.height <= 0) {
      erase_ids.push_back(nid);
      continue;
    }
    entry.roi_hp =
        clip_rect(align_rect(entry.roi_hp, kHpMicroTileSize), entry.hp_size);
    if (is_rect_empty(entry.roi_hp)) {
      erase_ids.push_back(nid);
      continue;
    }
    if (entry.halo_hp == 0)
      entry.halo_hp = infer_halo_hp(graph.node(nid));
    if (is_monolithic_boundary(graph.node(nid))) {
      entry.roi_hp = cv::Rect(0, 0, entry.hp_size.width, entry.hp_size.height);
      result.snapshot.dirty_monolithic_nodes.push_back(
          {nid, DirtyDomain::HighPrecision, entry.roi_hp, true});
    } else {
      enumerate_tiles(result.snapshot, nid, DirtyDomain::HighPrecision,
                      DirtyTileLevel::Micro, entry.roi_hp, kHpMicroTileSize);
    }
    result.snapshot.per_node_dirty_rois[nid].push_back(entry.roi_hp);
  }
  for (int nid : erase_ids)
    result.entries.erase(nid);

  if (result.entries.empty())
    throw GraphError(GraphErrc::InvalidParameter,
                     "HP planner produced empty execution set.");
  result.snapshot.actual_dirty_rois = result.snapshot.per_node_dirty_rois;
  populate_dirty_source_metadata(graph, result.snapshot,
                                 DirtyDomain::HighPrecision, result.entries);
  return result;
}

RealTimeDirtyPlan DirtyRegionPlanner::plan_real_time(
    GraphModel& graph, int node_id, const cv::Rect& dirty_roi) {
  if (!graph.has_node(node_id)) {
    throw GraphError(GraphErrc::NotFound, "Cannot compute RT update: node " +
                                              std::to_string(node_id) +
                                              " not found.");
  }
  if (is_rect_empty(dirty_roi)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Cannot compute RT update: dirty ROI is empty.");
  }

  RealTimeDirtyPlan result;
  result.execution_order = traversal_.topo_postorder_from(graph, node_id);
  if (result.execution_order.empty())
    result.execution_order.push_back(node_id);
  result.snapshot.graph_generation = ++graph.dirty_generation_counter;

  std::unordered_map<int, cv::Size> hp_size_cache;
  auto ensure_entry = [&](int nid) -> RtPlanEntry& {
    auto [it, inserted] = result.entries.emplace(nid, RtPlanEntry{});
    if (inserted) {
      it->second.hp_size = infer_hp_size(graph, nid, hp_size_cache);
      it->second.rt_size =
          scale_down_size(it->second.hp_size, kRtDownscaleFactor);
      it->second.halo_hp = infer_halo_hp(graph.node(nid));
      it->second.halo_rt =
          (it->second.halo_hp + kRtDownscaleFactor - 1) / kRtDownscaleFactor;
    } else {
      if (it->second.hp_size.width <= 0 || it->second.hp_size.height <= 0) {
        it->second.hp_size = infer_hp_size(graph, nid, hp_size_cache);
        it->second.rt_size =
            scale_down_size(it->second.hp_size, kRtDownscaleFactor);
      }
      if (it->second.halo_hp == 0) {
        it->second.halo_hp = infer_halo_hp(graph.node(nid));
        it->second.halo_rt =
            (it->second.halo_hp + kRtDownscaleFactor - 1) / kRtDownscaleFactor;
      }
    }
    return it->second;
  };

  RtPlanEntry& target_entry = ensure_entry(node_id);
  target_entry.roi_hp =
      clip_rect(align_rect(dirty_roi, kHpAlignment), target_entry.hp_size);
  if (is_rect_empty(target_entry.roi_hp)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Dirty ROI does not intersect node output.");
  }
  target_entry.roi_rt = clip_rect(
      align_rect(scale_down_rect(target_entry.roi_hp, kRtDownscaleFactor),
                 kRtTileSize),
      target_entry.rt_size);
  if (is_rect_empty(target_entry.roi_rt)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Dirty ROI collapses after RT scaling.");
  }

  std::unordered_map<int, cv::Size> size_cache;
  for (auto it = result.execution_order.rbegin();
       it != result.execution_order.rend(); ++it) {
    const int current_id = *it;
    auto plan_it = result.entries.find(current_id);
    if (plan_it == result.entries.end())
      continue;
    RtPlanEntry& current_entry = plan_it->second;
    if (is_rect_empty(current_entry.roi_hp))
      continue;

    const Node& current_node = graph.node(current_id);
    current_entry.halo_hp =
        std::max(current_entry.halo_hp, infer_halo_hp(current_node));
    current_entry.halo_rt =
        (current_entry.halo_hp + kRtDownscaleFactor - 1) / kRtDownscaleFactor;

    cv::Rect upstream_roi_hp = roi_propagation_.compute_upstream_roi(
        current_node, current_entry.roi_hp, graph, size_cache);
    upstream_roi_hp = clip_rect(upstream_roi_hp, current_entry.hp_size);
    if (is_rect_empty(upstream_roi_hp))
      continue;

    for (const auto& edge : graph.upstream_edges(current_id)) {
      if (edge.kind != GraphTopologyEdgeKind::ImageInput ||
          edge.from_node_id < 0)
        continue;
      RtPlanEntry& parent_entry = ensure_entry(edge.from_node_id);
      cv::Rect parent_roi = clip_rect(align_rect(upstream_roi_hp, kHpAlignment),
                                      parent_entry.hp_size);
      if (is_rect_empty(parent_roi))
        continue;
      parent_entry.roi_hp =
          is_rect_empty(parent_entry.roi_hp)
              ? parent_roi
              : clip_rect(merge_rect(parent_entry.roi_hp, parent_roi),
                          parent_entry.hp_size);
      result.snapshot.edge_mappings.push_back(
          {edge.from_node_id, current_id, DirtyDomain::RealTime, parent_roi,
           current_entry.roi_hp, DirtyEdgeDirection::BackwardDemand});
    }
  }

  std::vector<int> erase_ids;
  for (auto& [nid, entry] : result.entries) {
    if (entry.hp_size.width <= 0 || entry.hp_size.height <= 0) {
      erase_ids.push_back(nid);
      continue;
    }
    entry.roi_hp =
        clip_rect(align_rect(entry.roi_hp, kHpAlignment), entry.hp_size);
    if (is_rect_empty(entry.roi_hp)) {
      erase_ids.push_back(nid);
      continue;
    }
    entry.rt_size = scale_down_size(entry.hp_size, kRtDownscaleFactor);
    entry.roi_rt =
        clip_rect(align_rect(scale_down_rect(entry.roi_hp, kRtDownscaleFactor),
                             kRtTileSize),
                  entry.rt_size);
    if (is_rect_empty(entry.roi_rt)) {
      erase_ids.push_back(nid);
      continue;
    }
    if (entry.halo_hp == 0) {
      entry.halo_hp = infer_halo_hp(graph.node(nid));
      entry.halo_rt =
          (entry.halo_hp + kRtDownscaleFactor - 1) / kRtDownscaleFactor;
    }
    if (is_monolithic_boundary(graph.node(nid))) {
      entry.roi_hp = cv::Rect(0, 0, entry.hp_size.width, entry.hp_size.height);
      entry.roi_rt = cv::Rect(0, 0, entry.rt_size.width, entry.rt_size.height);
      result.snapshot.dirty_monolithic_nodes.push_back(
          {nid, DirtyDomain::RealTime, entry.roi_rt, true});
    } else {
      enumerate_tiles(result.snapshot, nid, DirtyDomain::RealTime,
                      DirtyTileLevel::Micro, entry.roi_rt, kRtTileSize);
    }
    result.snapshot.per_node_dirty_rois[nid].push_back(entry.roi_hp);
  }
  for (int nid : erase_ids)
    result.entries.erase(nid);

  if (result.entries.empty()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "RT planner produced empty execution set.");
  }
  result.snapshot.actual_dirty_rois = result.snapshot.per_node_dirty_rois;
  populate_dirty_source_metadata(graph, result.snapshot, DirtyDomain::RealTime,
                                 result.entries);
  return result;
}

DirtyRegionSnapshot DirtyRegionPlanner::begin_dirty_source(
    GraphModel& graph, int node_id, DirtyDomain domain,
    const cv::Rect& source_roi) {
  DirtyRegionSnapshot snapshot =
      graph.last_dirty_region_snapshot.value_or(DirtyRegionSnapshot{});
  if (snapshot.graph_generation == 0) {
    snapshot.graph_generation = ++graph.dirty_generation_counter;
  }
  apply_source_lifecycle_event(graph, snapshot, node_id, domain, &source_roi,
                               DirtySourceLifecycleState::Updating);
  refresh_actual_dirty_regions(graph, snapshot, domain);
  graph.last_dirty_region_snapshot = snapshot;
  graph.recent_dirty_region_snapshots.push_back(snapshot);
  if (graph.recent_dirty_region_snapshots.size() > 16) {
    graph.recent_dirty_region_snapshots.erase(
        graph.recent_dirty_region_snapshots.begin());
  }
  graph.last_dirty_region_snapshot_debug = describe_snapshot(snapshot);
  return snapshot;
}

DirtyRegionSnapshot DirtyRegionPlanner::update_dirty_source(
    GraphModel& graph, int node_id, DirtyDomain domain,
    const cv::Rect& source_roi) {
  DirtyRegionSnapshot snapshot =
      graph.last_dirty_region_snapshot.value_or(DirtyRegionSnapshot{});
  if (snapshot.graph_generation == 0) {
    snapshot.graph_generation = ++graph.dirty_generation_counter;
  }
  apply_source_lifecycle_event(graph, snapshot, node_id, domain, &source_roi,
                               DirtySourceLifecycleState::Updating);
  refresh_actual_dirty_regions(graph, snapshot, domain);
  graph.last_dirty_region_snapshot = snapshot;
  graph.recent_dirty_region_snapshots.push_back(snapshot);
  if (graph.recent_dirty_region_snapshots.size() > 16) {
    graph.recent_dirty_region_snapshots.erase(
        graph.recent_dirty_region_snapshots.begin());
  }
  graph.last_dirty_region_snapshot_debug = describe_snapshot(snapshot);
  return snapshot;
}

DirtyRegionSnapshot DirtyRegionPlanner::end_dirty_source(GraphModel& graph,
                                                         int node_id,
                                                         DirtyDomain domain) {
  DirtyRegionSnapshot snapshot =
      graph.last_dirty_region_snapshot.value_or(DirtyRegionSnapshot{});
  if (snapshot.graph_generation == 0) {
    snapshot.graph_generation = ++graph.dirty_generation_counter;
  }
  apply_source_lifecycle_event(graph, snapshot, node_id, domain, nullptr,
                               DirtySourceLifecycleState::Settled);
  refresh_actual_dirty_regions(graph, snapshot, domain);
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
    DirtySourceNodeState& state =
        snapshot.dirty_source_state[source_node_id];
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

void DirtyRegionPlanner::refresh_actual_dirty_regions(
    GraphModel& graph, DirtyRegionSnapshot& snapshot,
    DirtyDomain domain) const {
  snapshot.dirty_tiles.clear();
  snapshot.dirty_monolithic_nodes.clear();
  snapshot.per_node_dirty_rois.clear();
  snapshot.actual_dirty_rois.clear();
  snapshot.edge_mappings.clear();

  std::unordered_map<int, cv::Size> hp_size_cache;
  for (const auto& [node_id, records] : snapshot.source_roi_records) {
    if (!graph.has_node(node_id)) {
      continue;
    }
    const Node& node = graph.node(node_id);
    const cv::Size hp_size = infer_hp_size(graph, node_id, hp_size_cache);
    for (const auto& record : records) {
      if (record.domain != domain || is_rect_empty(record.source_roi)) {
        continue;
      }
      cv::Rect clipped = clip_rect(record.source_roi, hp_size);
      if (is_rect_empty(clipped)) {
        continue;
      }
      if (domain == DirtyDomain::HighPrecision) {
        clipped = clip_rect(align_rect(clipped, kHpMicroTileSize), hp_size);
      } else {
        cv::Size rt_size = scale_down_size(hp_size, kRtDownscaleFactor);
        clipped = clip_rect(
            align_rect(scale_down_rect(clipped, kRtDownscaleFactor),
                       kRtTileSize),
            rt_size);
      }
      snapshot.per_node_dirty_rois[node_id].push_back(clipped);
      snapshot.actual_dirty_rois[node_id].push_back(clipped);
      if (is_monolithic_boundary(node)) {
        snapshot.dirty_monolithic_nodes.push_back(
            {node_id, domain, clipped, true});
      } else {
        enumerate_tiles(snapshot, node_id, domain, DirtyTileLevel::Micro,
                        clipped,
                        domain == DirtyDomain::HighPrecision
                            ? kHpMicroTileSize
                            : kRtTileSize);
      }
    }
  }
}

void DirtyRegionPlanner::apply_source_lifecycle_event(
    GraphModel& graph, DirtyRegionSnapshot& snapshot, int node_id,
    DirtyDomain domain, const cv::Rect* source_roi,
    DirtySourceLifecycleState lifecycle) {
  if (!graph.has_node(node_id)) {
    throw GraphError(GraphErrc::NotFound, "Dirty source node " +
                                              std::to_string(node_id) +
                                              " not found.");
  }
  if (source_roi && is_rect_empty(*source_roi)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Dirty source ROI is empty for node " +
                         std::to_string(node_id) + ".");
  }

  if (std::find(snapshot.dirty_source_nodes.begin(),
                snapshot.dirty_source_nodes.end(),
                node_id) == snapshot.dirty_source_nodes.end()) {
    snapshot.dirty_source_nodes.push_back(node_id);
  }

  DirtySourceNodeState& state = snapshot.dirty_source_state[node_id];
  state.node_id = node_id;
  state.domain = domain;
  state.lifecycle = lifecycle;
  state.generation = snapshot.graph_generation;
  if (source_roi) {
    state.source_rois.push_back(*source_roi);
    snapshot.source_roi_records[node_id].push_back(
        {node_id, domain, *source_roi, snapshot.graph_generation});
  }

  snapshot.dirty_updating_count = 0;
  for (const auto& [_, source_state] : snapshot.dirty_source_state) {
    if (source_state.lifecycle == DirtySourceLifecycleState::Updating) {
      ++snapshot.dirty_updating_count;
    }
  }
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

bool DirtyRegionPlanner::is_monolithic_boundary(const Node& node) const {
  const auto* impls =
      OpRegistry::instance().get_implementations(node.type, node.subtype);
  return impls && impls->monolithic_hp && !impls->tiled_hp;
}

void DirtyRegionPlanner::enumerate_tiles(DirtyRegionSnapshot& snapshot,
                                         int node_id, DirtyDomain domain,
                                         DirtyTileLevel level,
                                         const cv::Rect& roi,
                                         int tile_size) const {
  if (is_rect_empty(roi) || tile_size <= 0)
    return;
  const cv::Rect aligned = align_rect(roi, tile_size);
  for (int y = aligned.y; y < aligned.y + aligned.height; y += tile_size) {
    for (int x = aligned.x; x < aligned.x + aligned.width; x += tile_size) {
      cv::Rect tile_roi(x, y,
                        std::min(tile_size, aligned.x + aligned.width - x),
                        std::min(tile_size, aligned.y + aligned.height - y));
      snapshot.dirty_tiles.push_back({node_id, domain, level, x / tile_size,
                                      y / tile_size, tile_size, tile_roi});
    }
  }
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
