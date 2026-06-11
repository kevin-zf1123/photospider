#include "kernel/services/compute-service/dirty_region_planner.hpp"

#include <algorithm>
#include <sstream>

#include "kernel/param_utils.hpp"
#include "kernel/services/compute-service/compute_geometry.hpp"
#include "kernel/services/graph_traversal_service.hpp"

namespace ps::compute {

bool DirtyRegionSnapshot::empty() const {
  return dirty_tiles.empty() && dirty_monolithic_nodes.empty() &&
         per_node_dirty_rois.empty() && edge_mappings.empty();
}

DirtyRegionPlanner::DirtyRegionPlanner(GraphTraversalService& traversal)
    : traversal_(traversal) {}

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
  result.snapshot.graph_generation = ++generation_counter_;

  std::unordered_map<int, cv::Size> hp_size_cache;
  auto ensure_entry = [&](int nid) -> HpPlanEntry& {
    auto [it, inserted] = result.entries.emplace(nid, HpPlanEntry{});
    if (inserted) {
      it->second.hp_size = infer_hp_size(graph, nid, hp_size_cache);
      it->second.halo_hp = infer_halo_hp(graph.nodes.at(nid));
    } else {
      if (it->second.hp_size.width <= 0 || it->second.hp_size.height <= 0)
        it->second.hp_size = infer_hp_size(graph, nid, hp_size_cache);
      if (it->second.halo_hp == 0)
        it->second.halo_hp = infer_halo_hp(graph.nodes.at(nid));
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

    const Node& current_node = graph.nodes.at(current_id);
    current_entry.halo_hp =
        std::max(current_entry.halo_hp, infer_halo_hp(current_node));

    cv::Rect upstream_roi_hp = GraphTraversalService::compute_upstream_roi(
        current_node, current_entry.roi_hp, graph, size_cache);
    upstream_roi_hp = align_rect(upstream_roi_hp, kHpMicroTileSize);

    for (const auto& img_input : current_node.image_inputs) {
      if (img_input.from_node_id < 0)
        continue;
      HpPlanEntry& parent_entry = ensure_entry(img_input.from_node_id);
      cv::Rect parent_roi = clip_rect(upstream_roi_hp, parent_entry.hp_size);
      if (is_rect_empty(parent_roi))
        continue;
      parent_entry.roi_hp =
          is_rect_empty(parent_entry.roi_hp)
              ? parent_roi
              : clip_rect(merge_rect(parent_entry.roi_hp, parent_roi),
                          parent_entry.hp_size);
      result.snapshot.edge_mappings.push_back(
          {img_input.from_node_id, current_id, DirtyDomain::HighPrecision,
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
      entry.halo_hp = infer_halo_hp(graph.nodes.at(nid));
    if (is_monolithic_boundary(graph.nodes.at(nid))) {
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
  result.snapshot.graph_generation = ++generation_counter_;

  std::unordered_map<int, cv::Size> hp_size_cache;
  auto ensure_entry = [&](int nid) -> RtPlanEntry& {
    auto [it, inserted] = result.entries.emplace(nid, RtPlanEntry{});
    if (inserted) {
      it->second.hp_size = infer_hp_size(graph, nid, hp_size_cache);
      it->second.rt_size =
          scale_down_size(it->second.hp_size, kRtDownscaleFactor);
      it->second.halo_hp = infer_halo_hp(graph.nodes.at(nid));
      it->second.halo_rt =
          (it->second.halo_hp + kRtDownscaleFactor - 1) / kRtDownscaleFactor;
    } else {
      if (it->second.hp_size.width <= 0 || it->second.hp_size.height <= 0) {
        it->second.hp_size = infer_hp_size(graph, nid, hp_size_cache);
        it->second.rt_size =
            scale_down_size(it->second.hp_size, kRtDownscaleFactor);
      }
      if (it->second.halo_hp == 0) {
        it->second.halo_hp = infer_halo_hp(graph.nodes.at(nid));
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

    const Node& current_node = graph.nodes.at(current_id);
    current_entry.halo_hp =
        std::max(current_entry.halo_hp, infer_halo_hp(current_node));
    current_entry.halo_rt =
        (current_entry.halo_hp + kRtDownscaleFactor - 1) / kRtDownscaleFactor;

    cv::Rect upstream_roi_hp = GraphTraversalService::compute_upstream_roi(
        current_node, current_entry.roi_hp, graph, size_cache);
    upstream_roi_hp = clip_rect(upstream_roi_hp, current_entry.hp_size);
    if (is_rect_empty(upstream_roi_hp))
      continue;

    for (const auto& img_input : current_node.image_inputs) {
      if (img_input.from_node_id < 0)
        continue;
      RtPlanEntry& parent_entry = ensure_entry(img_input.from_node_id);
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
          {img_input.from_node_id, current_id, DirtyDomain::RealTime,
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
      entry.halo_hp = infer_halo_hp(graph.nodes.at(nid));
      entry.halo_rt =
          (entry.halo_hp + kRtDownscaleFactor - 1) / kRtDownscaleFactor;
    }
    if (is_monolithic_boundary(graph.nodes.at(nid))) {
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
  return result;
}

cv::Size DirtyRegionPlanner::infer_hp_size(
    GraphModel& graph, int node_id,
    std::unordered_map<int, cv::Size>& cache) const {
  if (cache.count(node_id))
    return cache.at(node_id);

  cv::Size size{0, 0};
  const Node& node = graph.nodes.at(node_id);
  auto take_from_output = [&](const std::optional<NodeOutput>& opt) -> bool {
    if (!opt)
      return false;
    const auto& img = opt->image_buffer;
    if (img.width <= 0 || img.height <= 0)
      return false;
    size = cv::Size(img.width, img.height);
    return true;
  };
  if (take_from_output(node.cached_output_high_precision) ||
      take_from_output(node.cached_output)) {
    cache[node_id] = size;
    return size;
  }

  const int width =
      as_int_flexible(node.runtime_parameters, "width",
                      as_int_flexible(node.parameters, "width", 0));
  const int height =
      as_int_flexible(node.runtime_parameters, "height",
                      as_int_flexible(node.parameters, "height", 0));
  if (width > 0 && height > 0) {
    size = cv::Size(width, height);
    cache[node_id] = size;
    return size;
  }

  for (const auto& input : node.image_inputs) {
    if (input.from_node_id < 0)
      continue;
    cv::Size parent_size = infer_hp_size(graph, input.from_node_id, cache);
    if (parent_size.width > 0 && parent_size.height > 0) {
      size = parent_size;
      break;
    }
  }
  cache[node_id] = size;
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
      << " tiles=" << snapshot.dirty_tiles.size()
      << " monolithic=" << snapshot.dirty_monolithic_nodes.size()
      << " nodes=" << snapshot.per_node_dirty_rois.size()
      << " edges=" << snapshot.edge_mappings.size();
  return out.str();
}

}  // namespace ps::compute
