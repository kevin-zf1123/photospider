#include "compute/dirty/dirty_region_snapshot_builder.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>

#include "compute/compute_geometry.hpp"
#include "core/ops.hpp"
#include "core/region_image_adapter.hpp"

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
  if (update.source_region && update.source_region->is_empty()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Dirty source Region is empty for node " +
                         std::to_string(update.node_id) + ".");
  }
  if (update.source_roi && update.source_region) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "Dirty source lifecycle accepts either ROI or Region, not both.");
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
    std::unordered_map<int, PixelSize> extent_cache;
    const ImageBounds data_window = extent_resolver_.resolve_output_data_window(
        graph, update.node_id, extent_cache);
    const RegionSet region = region_image_adapter::from_storage_pixel_rect(
        *update.source_roi, data_window);
    state.source_rois.push_back(*update.source_roi);
    state.source_regions.push_back(region);
    snapshot.source_roi_records[update.node_id].push_back(
        {update.node_id, update.domain, *update.source_roi,
         snapshot.graph_generation});
    snapshot.source_region_records[update.node_id].push_back(
        {update.node_id, update.domain, region, snapshot.graph_generation});
  } else if (update.source_region) {
    state.source_regions.push_back(*update.source_region);
    snapshot.source_region_records[update.node_id].push_back(
        {update.node_id, update.domain, *update.source_region,
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
  snapshot.per_node_dirty_regions.clear();
  snapshot.actual_dirty_rois.clear();
  snapshot.actual_dirty_regions.clear();
  snapshot.edge_mappings.clear();

  std::unordered_map<int, PixelSize> hp_size_cache;
  const auto append_image_source = [&](int node_id, DirtyDomain record_domain,
                                       const PixelRect& source_roi) {
    if (!graph.has_node(node_id) || record_domain != domain ||
        is_rect_empty(source_roi)) {
      return;
    }
    const PixelRect domain_roi =
        normalize_source_roi(graph, node_id, domain, source_roi, hp_size_cache);
    if (is_rect_empty(domain_roi)) {
      return;
    }
    const Node& node = graph.node(node_id);
    const ImageBounds hp_data_window =
        extent_resolver_.resolve_output_data_window(graph, node_id,
                                                    hp_size_cache);
    const PixelSize hp_size = infer_hp_size(graph, node_id, hp_size_cache);
    const PixelSize domain_size =
        domain == DirtyDomain::HighPrecision
            ? hp_size
            : scale_down_size(hp_size, kRtDownscaleFactor);
    const ImageBounds domain_data_window =
        domain == DirtyDomain::HighPrecision
            ? hp_data_window
            : ImageBounds{0, 0, domain_size.width, domain_size.height};
    const RegionSet domain_region =
        region_image_adapter::from_storage_pixel_rect(domain_roi,
                                                      domain_data_window);
    snapshot.per_node_dirty_rois[node_id].push_back(domain_roi);
    snapshot.actual_dirty_rois[node_id].push_back(domain_roi);
    snapshot.per_node_dirty_regions[node_id].push_back(domain_region);
    snapshot.actual_dirty_regions[node_id].push_back(domain_region);
    append_node_work(
        snapshot,
        DirtyNodeWorkRecord{&node, node_id, domain, domain_roi,
                            domain_data_window, tile_size_for_domain(domain)});
  };

  for (const auto& [node_id, records] : snapshot.source_region_records) {
    if (!graph.has_node(node_id)) {
      continue;
    }
    for (const auto& record : records) {
      if (record.domain != domain || record.source_region.is_empty()) {
        continue;
      }
      if (record.source_region.atoms().size() == 1U &&
          std::holds_alternative<ImageRect>(
              record.source_region.atoms().front())) {
        const ImageBounds hp_data_window =
            extent_resolver_.resolve_output_data_window(graph, node_id,
                                                        hp_size_cache);
        append_image_source(node_id, record.domain,
                            region_image_adapter::to_storage_pixel_rect(
                                record.source_region, hp_data_window));
        continue;
      }
      if (domain != DirtyDomain::HighPrecision ||
          record.source_region.atoms().size() != 1U ||
          !std::holds_alternative<TensorSlice>(
              record.source_region.atoms().front())) {
        throw GraphError(
            GraphErrc::InvalidParameter,
            "Dirty source Region cannot be materialized in this domain.");
      }
      const TensorSlice& tensor =
          std::get<TensorSlice>(record.source_region.atoms().front());
      if (!(tensor.domain == dense_tensor_region_domain())) {
        throw GraphError(
            GraphErrc::InvalidParameter,
            "Dirty source TensorSlice requires the built-in dense tensor "
            "domain.");
      }
      snapshot.per_node_dirty_regions[node_id].push_back(record.source_region);
      snapshot.actual_dirty_regions[node_id].push_back(record.source_region);
      snapshot.dirty_monolithic_nodes.push_back(
          {node_id, domain, PixelRect{}, true, record.source_region});
    }
  }

  for (const auto& [node_id, records] : snapshot.source_roi_records) {
    if (snapshot.source_region_records.count(node_id) != 0U) {
      continue;
    }
    for (const auto& record : records) {
      append_image_source(node_id, record.domain, record.source_roi);
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
    const RegionSet region = region_image_adapter::from_storage_pixel_rect(
        record.work_roi, record.data_window);
    snapshot.dirty_monolithic_nodes.push_back(
        {record.node_id, record.domain, record.work_roi, true, region});
    return;
  }
  enumerate_tiles(snapshot,
                  DirtyTileEnumeration{record.node_id, record.domain,
                                       DirtyTileLevel::Micro, record.work_roi,
                                       record.data_window, record.tile_size});
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
  const PixelRect storage_bounds = region_image_adapter::to_storage_pixel_rect(
      RegionSet::whole(), request.data_window);
  if (intersect_rect(request.roi, storage_bounds) != request.roi) {
    throw std::invalid_argument(
        "Dirty tile enumeration ROI exceeds its image data window.");
  }
  const PixelRect aligned = align_rect(request.roi, request.tile_size);
  const std::int64_t right =
      static_cast<std::int64_t>(aligned.x) + aligned.width;
  const std::int64_t bottom =
      static_cast<std::int64_t>(aligned.y) + aligned.height;
  for (std::int64_t y = aligned.y; y < bottom; y += request.tile_size) {
    for (std::int64_t x = aligned.x; x < right; x += request.tile_size) {
      const PixelRect tile_roi{static_cast<int>(x), static_cast<int>(y),
                               static_cast<int>(std::min<std::int64_t>(
                                   request.tile_size, right - x)),
                               static_cast<int>(std::min<std::int64_t>(
                                   request.tile_size, bottom - y))};
      const PixelRect bounded_tile_roi =
          intersect_rect(tile_roi, storage_bounds);
      snapshot.dirty_tiles.push_back(
          {request.node_id, request.domain, request.level,
           static_cast<int>(x / request.tile_size),
           static_cast<int>(y / request.tile_size), request.tile_size, tile_roi,
           region_image_adapter::from_storage_pixel_rect(bounded_tile_roi,
                                                         request.data_window)});
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
