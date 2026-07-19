#include "compute/dirty_region_snapshot_builder.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "compute/compute_geometry.hpp"
#include "core/ops.hpp"

namespace ps::compute {

/** @copydoc DirtyRegionSnapshotBuilder::apply_source_lifecycle_event */
void DirtyRegionSnapshotBuilder::apply_source_lifecycle_event(
    const GraphModel& graph, DirtyRegionSnapshot& snapshot,
    const DirtySourceLifecycleUpdate& update) const {
  if (!graph.has_node(update.node_id)) {
    throw GraphError(
        GraphErrc::NotFound,
        "Dirty source node " + std::to_string(update.node_id) + " not found.");
  }
  if (update.source_roi && is_rect_empty(*update.source_roi)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Dirty source ROI is empty for node " +
                         std::to_string(update.node_id) + ".");
  }

  if (std::find(snapshot.dirty_source_nodes.begin(),
                snapshot.dirty_source_nodes.end(),
                update.node_id) == snapshot.dirty_source_nodes.end()) {
    snapshot.dirty_source_nodes.push_back(update.node_id);
  }

  DirtySourceNodeState& state = snapshot.dirty_source_state[update.node_id];
  state.node_id = update.node_id;
  state.domain = update.domain;
  state.lifecycle = update.lifecycle;
  state.generation = snapshot.graph_generation;
  if (update.source_roi) {
    state.source_rois.push_back(*update.source_roi);
    snapshot.source_roi_records[update.node_id].push_back(
        {update.node_id, update.domain, *update.source_roi,
         snapshot.graph_generation});
  }

  snapshot.dirty_updating_count = 0;
  for (const auto& [_, source_state] : snapshot.dirty_source_state) {
    if (source_state.lifecycle == DirtySourceLifecycleState::Updating) {
      ++snapshot.dirty_updating_count;
    }
  }
}

/** @copydoc DirtyRegionSnapshotBuilder::refresh_actual_dirty_regions */
void DirtyRegionSnapshotBuilder::refresh_actual_dirty_regions(
    const GraphModel& graph, DirtyRegionSnapshot& snapshot,
    DirtyDomain domain) const {
  snapshot.dirty_tiles.clear();
  snapshot.dirty_monolithic_nodes.clear();
  snapshot.per_node_dirty_rois.clear();
  snapshot.actual_dirty_rois.clear();
  snapshot.edge_mappings.clear();

  std::unordered_map<int, PixelSize> hp_size_cache;
  for (const auto& [node_id, records] : snapshot.source_roi_records) {
    if (!graph.has_node(node_id)) {
      continue;
    }
    const Node& node = graph.node(node_id);
    for (const auto& record : records) {
      if (record.domain != domain || is_rect_empty(record.source_roi)) {
        continue;
      }
      const PixelRect domain_roi = normalize_source_roi(
          graph, node_id, domain, record.source_roi, hp_size_cache);
      if (is_rect_empty(domain_roi)) {
        continue;
      }
      snapshot.per_node_dirty_rois[node_id].push_back(domain_roi);
      snapshot.actual_dirty_rois[node_id].push_back(domain_roi);
      append_node_work(snapshot,
                       DirtyNodeWorkRecord{&node, node_id, domain, domain_roi,
                                           tile_size_for_domain(domain)});
    }
  }
}

/** @copydoc DirtyRegionSnapshotBuilder::is_monolithic_boundary */
bool DirtyRegionSnapshotBuilder::is_monolithic_boundary(
    const Node& node) const {
  const auto impls =
      OpRegistry::instance().get_implementations(node.type, node.subtype);
  return impls && impls->monolithic_hp && !impls->tiled_hp;
}

/** @copydoc DirtyRegionSnapshotBuilder::append_node_work */
void DirtyRegionSnapshotBuilder::append_node_work(
    DirtyRegionSnapshot& snapshot, const DirtyNodeWorkRecord& record) const {
  if (!record.node || is_rect_empty(record.work_roi)) {
    return;
  }
  if (is_monolithic_boundary(*record.node)) {
    snapshot.dirty_monolithic_nodes.push_back(
        {record.node_id, record.domain, record.work_roi, true});
    return;
  }
  enumerate_tiles(
      snapshot,
      DirtyTileEnumeration{record.node_id, record.domain, DirtyTileLevel::Micro,
                           record.work_roi, record.tile_size});
}

/**
 * @copydoc DirtyRegionSnapshotBuilder::enumerate_tiles
 *
 * @note Tile enumeration emits value-type keys rather than pointers so
 *       inspection stays stable across graph reloads or node replacement.
 */
void DirtyRegionSnapshotBuilder::enumerate_tiles(
    DirtyRegionSnapshot& snapshot, const DirtyTileEnumeration& request) const {
  if (is_rect_empty(request.roi) || request.tile_size <= 0) {
    return;
  }
  const PixelRect aligned = align_rect(request.roi, request.tile_size);
  const std::int64_t right =
      static_cast<std::int64_t>(aligned.x) + aligned.width;
  const std::int64_t bottom =
      static_cast<std::int64_t>(aligned.y) + aligned.height;
  for (std::int64_t y = aligned.y; y < bottom; y += request.tile_size) {
    for (std::int64_t x = aligned.x; x < right; x += request.tile_size) {
      PixelRect tile_roi{static_cast<int>(x), static_cast<int>(y),
                         static_cast<int>(std::min<std::int64_t>(
                             request.tile_size, right - x)),
                         static_cast<int>(std::min<std::int64_t>(
                             request.tile_size, bottom - y))};
      snapshot.dirty_tiles.push_back({request.node_id, request.domain,
                                      request.level,
                                      static_cast<int>(x / request.tile_size),
                                      static_cast<int>(y / request.tile_size),
                                      request.tile_size, tile_roi});
    }
  }
}

/** @copydoc DirtyRegionSnapshotBuilder::normalize_source_roi */
PixelRect DirtyRegionSnapshotBuilder::normalize_source_roi(
    const GraphModel& graph, int node_id, DirtyDomain domain,
    const PixelRect& source_roi,
    std::unordered_map<int, PixelSize>& hp_size_cache) const {
  const PixelSize hp_size = infer_hp_size(graph, node_id, hp_size_cache);
  PixelRect clipped = clip_rect(source_roi, hp_size);
  if (is_rect_empty(clipped)) {
    return PixelRect{};
  }
  if (domain == DirtyDomain::HighPrecision) {
    return clip_rect(align_rect(clipped, kHpMicroTileSize), hp_size);
  }
  const PixelSize rt_size = scale_down_size(hp_size, kRtDownscaleFactor);
  return clip_rect(
      align_rect(scale_down_rect(clipped, kRtDownscaleFactor), kRtTileSize),
      rt_size);
}

/** @copydoc DirtyRegionSnapshotBuilder::infer_hp_size */
PixelSize DirtyRegionSnapshotBuilder::infer_hp_size(
    const GraphModel& graph, int node_id,
    std::unordered_map<int, PixelSize>& cache) const {
  return extent_resolver_.resolve_output_extent(graph, node_id, cache);
}

/** @copydoc DirtyRegionSnapshotBuilder::tile_size_for_domain */
int DirtyRegionSnapshotBuilder::tile_size_for_domain(DirtyDomain domain) const {
  return domain == DirtyDomain::HighPrecision ? kHpMicroTileSize : kRtTileSize;
}

}  // namespace ps::compute
