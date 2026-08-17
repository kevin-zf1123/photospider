#include "compute/dirty/dirty_region_planner.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "compute/compute_geometry.hpp"
#include "compute/dirty/dirty_region_planning_policy.hpp"
#include "compute/request/compute_cache_policy.hpp"
#include "core/ops.hpp"
#include "core/region_image_adapter.hpp"
#include "graph/graph_traversal_service.hpp"
#include "graph/roi_propagation_service.hpp"
#include "photospider/data/image_view.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Reports whether request-local parameter results can change geometry.
 *
 * @param node Node whose connected parameter inputs are inspected.
 * @return True when at least one connected parameter producer may execute in
 *         the same dirty request.
 * @throws Nothing.
 * @note DirtyRegionPlanner has no immutable-value proof for a connected
 *       producer. It therefore never treats a previously cached value as the
 *       current request's exact halo/LUT/propagator input.
 */
bool requires_conservative_parameter_geometry(const Node& node) noexcept {
  return std::any_of(
      node.parameter_inputs.begin(), node.parameter_inputs.end(),
      [](const ParameterInput& input) { return input.from_node_id >= 0; });
}

/**
 * @brief Returns an extent-bounded halo that covers every input pixel.
 *
 * @param extent Positive output or input extent bounding the request.
 * @return Maximum dimension, or zero for an invalid extent.
 * @throws Nothing.
 * @note RT conversion uses int64 arithmetic, so this bounded value cannot
 *       overflow while preserving conservative whole-input coverage.
 */
int full_extent_halo(const PixelSize& extent) noexcept {
  return std::max({0, extent.width, extent.height});
}

/**
 * @brief Finds an upstream-preferred concrete sealed dense output descriptor.
 *
 * @param graph Graph whose image dependency cone and caches are inspected.
 * @param node_id Target node id.
 * @return Borrowed-by-value concrete shape, or nullopt when unavailable.
 * @throws std::bad_alloc when shape copying cannot allocate.
 * @note The core dense identity chain preserves its input descriptor. Searching
 *       ancestors before the target avoids preferring stale downstream bytes
 *       when a current upstream cache still supplies the concrete shape.
 */
std::optional<std::vector<std::size_t>> dense_shape_for_node(
    const GraphModel& graph, int node_id) {
  std::unordered_set<int> visited;
  const auto find_shape =
      [&](const auto& self,
          int current_id) -> std::optional<std::vector<std::size_t>> {
    if (!visited.insert(current_id).second || !graph.has_node(current_id)) {
      return std::nullopt;
    }
    const Node& current = graph.node(current_id);
    for (const ImageInput& input : current.image_inputs) {
      if (input.from_node_id < 0 || !graph.has_node(input.from_node_id)) {
        continue;
      }
      if (std::optional<std::vector<std::size_t>> upstream =
              self(self, input.from_node_id)) {
        return upstream;
      }
    }
    if (current.cached_output_high_precision &&
        current.cached_output_high_precision->has_image_value()) {
      return current.cached_output_high_precision->image_value()
          .dense_tensor_descriptor()
          .shape;
    }
    return std::nullopt;
  };
  return find_shape(find_shape, node_id);
}

/**
 * @brief Reports whether one formal HP cache can satisfy whole-output reads.
 * @param node Node whose output and exact validity are inspected.
 * @return True only when ComputeCachePolicy accepts complete reusable output.
 * @throws std::logic_error, std::invalid_argument, std::overflow_error, or
 * std::bad_alloc when retained output facts cannot be validated.
 * @note Tensor dirty planning uses only complete caches as dependency
 * boundaries. Partial or missing parent validity is recomputed and staged.
 */
bool complete_tensor_dependency_available(const Node& node) {
  return ComputeCachePolicy::has_reusable_output(node);
}

/**
 * @brief Merges repeated exact TensorSlice demand for one upstream node.
 * @param existing Mutable optional demand already accumulated.
 * @param update Exact demand projected through one identity edge.
 * @throws GraphError when the bounded Region subset cannot represent the exact
 * union.
 * @throws std::bad_alloc when Region algebra storage cannot allocate.
 * @note Conservative widening is forbidden because execution and cache
 * validity require exact coordinates.
 */
void merge_tensor_demand(std::optional<RegionSet>* existing,
                         const RegionSet& update) {
  if (!existing->has_value()) {
    *existing = update;
    return;
  }
  const RegionOperationResult merged = union_regions(**existing, update);
  if (merged.status() != RegionOperationStatus::Exact ||
      !merged.region().has_value()) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "TensorSlice dependency demand exceeds the exact Region subset.");
  }
  *existing = *merged.region();
}

/**
 * @brief Reports whether one node has at least one connected image parent.
 * @param graph Graph whose topology edges are inspected.
 * @param node_id Node id to inspect.
 * @return True when a valid upstream ImageInput edge exists.
 * @throws std::bad_alloc only if graph edge snapshotting allocates.
 */
bool has_image_parent(const GraphModel& graph, int node_id) {
  for (const GraphTopologyEdge& edge : graph.upstream_edges(node_id)) {
    if (edge.kind == GraphTopologyEdgeKind::ImageInput &&
        edge.from_node_id >= 0 && graph.has_node(edge.from_node_id)) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Canonicalizes one route-visible device inventory as a value set.
 *
 * @param available_devices Caller-ordered route inventory.
 * @return Sorted, duplicate-free device labels.
 * @throws std::bad_alloc when copied vector storage cannot allocate.
 * @note Registry selection tests membership rather than caller order, so this
 * representation is the stable context compared with task population.
 */
std::vector<Device> canonicalize_route_devices(
    const std::vector<Device>& available_devices) {
  std::vector<Device> result = available_devices;
  std::sort(result.begin(), result.end(), [](Device lhs, Device rhs) {
    return static_cast<int>(lhs) < static_cast<int>(rhs);
  });
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

/**
 * @brief Selects and freezes one exact core TensorSlice execution route.
 *
 * @param propagation Request-bound route selection authority.
 * @param node Node whose selected implementation is inspected.
 * @param node_id Stable node id used in diagnostics.
 * @param target True when this is the request target rather than an upstream
 * dependency.
 * @return Canonical operation key and callback-free complete route.
 * @throws GraphError with `GraphErrc::InvalidParameter` when the selected
 * implementation is absent, tiled, or not the exact source-private core
 * dense identity.
 * @throws std::bad_alloc or callback-copy exceptions from registry selection
 * and callback-free snapshot construction.
 * @note The temporary implementation owns any callback/DSO lease only until
 * this helper copies its scalar route and returns.
 */
DirtyRegionPlannedOperationRoute select_tensor_operation_route_or_throw(
    const RoiPropagationService& propagation, const Node& node, int node_id,
    bool target) {
  const std::optional<OpImplementation> selected =
      propagation.select_route_implementation(node);
  if (!selected.has_value() || !selected->is_monolithic() ||
      !ops::find_core_region_monolithic_operation(
           node.type, node.subtype, std::get<MonolithicOpFunc>(selected->func))
           .has_value()) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        target ? "Target operation has no exact TensorSlice execution contract."
               : "TensorSlice dependency path requires the exact core identity "
                 "operation at node " +
                     std::to_string(node_id) + ".");
  }
  return DirtyRegionPlannedOperationRoute{
      make_key(node.type, node.subtype),
      make_planned_operation_route(*selected),
  };
}

/**
 * @brief Validates and clips one TensorSlice to a concrete dense shape.
 *
 * @param graph Graph supplying the target and concrete tensor shape.
 * @param node_id Target node id.
 * @param tensor_region Exact one-atom TensorSlice candidate.
 * @return Nonempty exact TensorSlice clipped to the concrete shape.
 * @throws GraphError when the node, descriptor, rank, or clipped Region is
 * invalid.
 * @throws std::bad_alloc when descriptor or Region storage cannot allocate.
 * @note Route identity is deliberately checked by the caller so Tensor
 * planning can select exactly once and retain the same callback-free route.
 */
RegionSet clip_tensor_region_to_shape_or_throw(const GraphModel& graph,
                                               int node_id,
                                               const RegionSet& tensor_region) {
  if (tensor_region.atoms().size() != 1U ||
      !std::holds_alternative<TensorSlice>(tensor_region.atoms().front())) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "HP dirty planning accepts one ImageRect or TensorSlice.");
  }
  const TensorSlice& tensor =
      std::get<TensorSlice>(tensor_region.atoms().front());
  if (!(tensor.domain == dense_tensor_region_domain())) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "TensorSlice planning requires the built-in dense tensor domain.");
  }
  if (!graph.has_node(node_id)) {
    throw GraphError(GraphErrc::NotFound,
                     "Cannot compute HP tensor update: node not found.");
  }
  const std::optional<std::vector<std::size_t>> shape =
      dense_shape_for_node(graph, node_id);
  if (!shape.has_value()) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "TensorSlice planning requires a concrete sealed DenseTensor shape.");
  }
  const RegionOperationResult clipped = clip_region_to_tensor_shape(
      tensor_region, dense_tensor_region_domain(), *shape);
  if (clipped.status() != RegionOperationStatus::Exact ||
      !clipped.region().has_value()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "TensorSlice planning could not produce an exact Region.");
  }
  if (clipped.region()->is_empty()) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "TensorSlice does not intersect the target DenseTensor bounds.");
  }
  return *clipped.region();
}

/**
 * @brief Checks current Tensor execution eligibility before shape clipping.
 *
 * @param propagation Request-bound route selection authority.
 * @param graph Graph supplying the target and concrete tensor shape.
 * @param node_id Target node id.
 * @param tensor_region Exact one-atom TensorSlice candidate.
 * @return Nonempty exact TensorSlice clipped to the concrete shape.
 * @throws GraphError when no exact core route or concrete bounded Region
 * exists.
 * @throws std::bad_alloc or callback-copy exceptions from route selection and
 * Region storage.
 * @note Dirty lifecycle entry points use this helper without retaining a plan;
 * request planning instead freezes the selected route explicitly.
 */
RegionSet validate_and_clip_tensor_region_or_throw(
    const RoiPropagationService& propagation, const GraphModel& graph,
    int node_id, const RegionSet& tensor_region) {
  if (!graph.has_node(node_id)) {
    throw GraphError(GraphErrc::NotFound,
                     "Cannot compute HP tensor update: node not found.");
  }
  (void)select_tensor_operation_route_or_throw(propagation, graph.node(node_id),
                                               node_id, true);
  return clip_tensor_region_to_shape_or_throw(graph, node_id, tensor_region);
}

}  // namespace

using detail::has_valid_size;
using detail::HighPrecisionDirtyPolicy;
using detail::RealTimeDirtyPolicy;

bool DirtyRegionSnapshot::empty() const {
  return dirty_source_nodes.empty() && dirty_source_state.empty() &&
         source_roi_records.empty() && source_region_records.empty() &&
         dirty_tiles.empty() && dirty_monolithic_nodes.empty() &&
         per_node_dirty_rois.empty() && per_node_dirty_regions.empty() &&
         actual_dirty_rois.empty() && actual_dirty_regions.empty() &&
         edge_mappings.empty();
}

// NOLINTBEGIN(whitespace/indent_namespace)
DirtyRegionPlanner::DirtyRegionPlanner(
    GraphTraversalService& traversal, RoiPropagationService& roi_propagation,
    const std::unordered_set<int>* stabilized_geometry_nodes,
    const std::unordered_set<int>* forced_parameter_producers,
    std::optional<uint64_t> fixed_generation)
    : traversal_(traversal),
      roi_propagation_(roi_propagation),
      stabilized_geometry_nodes_(stabilized_geometry_nodes),
      forced_parameter_producers_(forced_parameter_producers),
      fixed_generation_(fixed_generation) {}
// NOLINTEND

template <typename Policy>
typename Policy::Entry& DirtyRegionPlanner::ensure_plan_entry(
    GraphModel& graph, typename Policy::Plan& plan, int node_id,
    std::unordered_map<int, PixelSize>& hp_size_cache) {
  auto [it, inserted] = plan.entries.emplace(node_id, typename Policy::Entry{});
  typename Policy::Entry& entry = it->second;
  if (inserted || !has_valid_size(entry.hp_size)) {
    entry.hp_size = infer_hp_size(graph, node_id, hp_size_cache);
    Policy::refresh_size_fields(entry);
  }
  if (inserted || entry.halo_hp == 0) {
    entry.halo_hp = infer_halo_hp(graph, graph.node(node_id));
    Policy::refresh_halo_fields(entry);
  }
  return entry;
}

template <typename Policy>
void DirtyRegionPlanner::propagate_dirty_entries(
    GraphModel& graph, typename Policy::Plan& plan,
    std::unordered_map<int, PixelSize>& hp_size_cache) {
  std::unordered_map<int, PixelSize> size_cache;
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
    if (has_stabilized_geometry(current_id)) {
      current_entry.roi_hp = detail::full_extent_roi(current_entry.hp_size);
    }
    if (requires_conservative_parameter_geometry(current_node) &&
        !has_stabilized_geometry(current_id)) {
      current_entry.roi_hp = detail::full_extent_roi(current_entry.hp_size);
      current_entry.halo_hp = full_extent_halo(current_entry.hp_size);
      Policy::refresh_halo_fields(current_entry);

      for (const ParameterInput& input : current_node.parameter_inputs) {
        if (input.from_node_id < 0 || !graph.has_node(input.from_node_id)) {
          continue;
        }
        typename Policy::Entry& parameter_entry = ensure_plan_entry<Policy>(
            graph, plan, input.from_node_id, hp_size_cache);
        if (!has_valid_size(parameter_entry.hp_size)) {
          parameter_entry.hp_size = PixelSize{1, 1};
          Policy::refresh_size_fields(parameter_entry);
        }
        parameter_entry.roi_hp =
            detail::full_extent_roi(parameter_entry.hp_size);
      }

      for (const auto& edge : graph.upstream_edges(current_id)) {
        if (edge.kind != GraphTopologyEdgeKind::ImageInput ||
            edge.from_node_id < 0) {
          continue;
        }
        typename Policy::Entry& parent_entry = ensure_plan_entry<Policy>(
            graph, plan, edge.from_node_id, hp_size_cache);
        if (!has_valid_size(parent_entry.hp_size)) {
          continue;
        }
        current_entry.halo_hp = std::max(
            current_entry.halo_hp, full_extent_halo(parent_entry.hp_size));
        Policy::refresh_halo_fields(current_entry);
        const PixelRect parent_roi =
            detail::full_extent_roi(parent_entry.hp_size);
        parent_entry.roi_hp = parent_roi;
        DirtyEdgeMapping mapping{
            edge.from_node_id,    current_id,
            Policy::kDomain,      parent_roi,
            current_entry.roi_hp, DirtyEdgeDirection::BackwardDemand};
        mapping.from_region = region_image_adapter::from_pixel_rect(parent_roi);
        mapping.to_region =
            region_image_adapter::from_pixel_rect(current_entry.roi_hp);
        plan.snapshot.edge_mappings.push_back(std::move(mapping));
      }
      continue;
    }
    current_entry.halo_hp =
        std::max(current_entry.halo_hp, infer_halo_hp(graph, current_node));
    Policy::refresh_halo_fields(current_entry);

    const UpstreamRoiProjection upstream_projection =
        roi_propagation_.compute_upstream_projection(
            current_node, current_entry.roi_hp, graph, size_cache);

    for (const auto& edge : graph.upstream_edges(current_id)) {
      if (edge.kind != GraphTopologyEdgeKind::ImageInput ||
          edge.from_node_id < 0)
        continue;
      typename Policy::Entry& parent_entry = ensure_plan_entry<Policy>(
          graph, plan, edge.from_node_id, hp_size_cache);
      PixelRect upstream_roi_hp = Policy::normalize_upstream_roi(
          upstream_projection.roi_for_input(edge.input_index,
                                            parent_entry.hp_size),
          current_entry);
      if (Policy::skip_empty_upstream_roi(upstream_roi_hp))
        continue;
      PixelRect parent_roi =
          Policy::parent_hp_roi(upstream_roi_hp, parent_entry);
      if (is_rect_empty(parent_roi))
        continue;
      parent_entry.roi_hp =
          is_rect_empty(parent_entry.roi_hp)
              ? parent_roi
              : clip_rect(merge_rect(parent_entry.roi_hp, parent_roi),
                          parent_entry.hp_size);
      DirtyEdgeMapping mapping{
          edge.from_node_id,    current_id,
          Policy::kDomain,      parent_roi,
          current_entry.roi_hp, DirtyEdgeDirection::BackwardDemand};
      mapping.from_region = region_image_adapter::from_pixel_rect(parent_roi);
      mapping.to_region =
          region_image_adapter::from_pixel_rect(current_entry.roi_hp);
      plan.snapshot.edge_mappings.push_back(std::move(mapping));
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
    entry.region_hp = region_image_adapter::from_pixel_rect(entry.roi_hp);
    Policy::refresh_size_fields(entry);
    if (!Policy::refresh_domain_roi(entry)) {
      erase_ids.push_back(node_id);
      continue;
    }
    if (entry.halo_hp == 0) {
      entry.halo_hp = infer_halo_hp(graph, graph.node(node_id));
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
    plan.snapshot.per_node_dirty_regions[node_id].push_back(entry.region_hp);
  }
  for (int node_id : erase_ids)
    plan.entries.erase(node_id);
}

template <typename Policy>
typename Policy::Plan DirtyRegionPlanner::plan_dirty_domain(
    GraphModel& graph, int node_id, const PixelRect& dirty_roi) {
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
  result.snapshot.graph_generation = select_plan_generation(graph);

  std::unordered_map<int, PixelSize> hp_size_cache;
  typename Policy::Entry& target_entry =
      ensure_plan_entry<Policy>(graph, result, node_id, hp_size_cache);
  target_entry.roi_hp = has_stabilized_geometry(node_id)
                            ? detail::full_extent_roi(target_entry.hp_size)
                            : Policy::target_hp_roi(dirty_roi, target_entry);
  if (is_rect_empty(target_entry.roi_hp)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Dirty ROI does not intersect node output.");
  }
  if (!Policy::refresh_domain_roi(target_entry)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     Policy::domain_roi_empty_message());
  }

  if (forced_parameter_producers_) {
    for (int producer_id : *forced_parameter_producers_) {
      if (!graph.has_node(producer_id)) {
        throw GraphError(GraphErrc::NotFound, "Forced parameter producer " +
                                                  std::to_string(producer_id) +
                                                  " not found.");
      }
      typename Policy::Entry& producer_entry =
          ensure_plan_entry<Policy>(graph, result, producer_id, hp_size_cache);
      if (!has_valid_size(producer_entry.hp_size)) {
        producer_entry.hp_size = PixelSize{1, 1};
        Policy::refresh_size_fields(producer_entry);
      }
      producer_entry.roi_hp = detail::full_extent_roi(producer_entry.hp_size);
    }
  }

  propagate_dirty_entries<Policy>(graph, result, hp_size_cache);
  finalize_dirty_entries<Policy>(graph, result);
  if (result.entries.empty())
    throw GraphError(GraphErrc::InvalidParameter, Policy::kEmptyPlanMessage);
  result.snapshot.actual_dirty_rois = result.snapshot.per_node_dirty_rois;
  result.snapshot.actual_dirty_regions = result.snapshot.per_node_dirty_regions;
  populate_dirty_source_metadata(graph, result.snapshot, Policy::kDomain,
                                 result.entries);
  return result;
}

HighPrecisionDirtyPlan DirtyRegionPlanner::plan_high_precision(
    GraphModel& graph, int node_id, const PixelRect& dirty_roi) {
  return plan_dirty_domain<HighPrecisionDirtyPolicy>(graph, node_id, dirty_roi);
}

/** @copydoc DirtyRegionPlanner::plan_high_precision(GraphModel&,int,const
 * RegionSet&) */
HighPrecisionDirtyPlan DirtyRegionPlanner::plan_high_precision(
    GraphModel& graph, int node_id, const RegionSet& dirty_region) {
  if (dirty_region.is_empty()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Cannot compute HP update: dirty Region is empty.");
  }
  if (dirty_region.is_whole()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Whole HP Region requires explicit finite target bounds.");
  }
  if (dirty_region.atoms().size() == 1U &&
      std::holds_alternative<ImageRect>(dirty_region.atoms().front())) {
    return plan_high_precision(
        graph, node_id, region_image_adapter::to_pixel_rect(dirty_region));
  }
  const RegionSet clipped_region =
      clip_tensor_region_to_shape_or_throw(graph, node_id, dirty_region);
  const DirtyRegionPlannedOperationRoute target_route =
      select_tensor_operation_route_or_throw(
          roi_propagation_, graph.node(node_id), node_id, true);
  HighPrecisionDirtyPlan result;
  result.snapshot.graph_generation = select_plan_generation(graph);
  result.operation_routes.intent = roi_propagation_.intent();
  result.operation_routes.available_devices =
      canonicalize_route_devices(roi_propagation_.available_devices());
  std::vector<int> dependency_order =
      traversal_.topo_postorder_from(graph, node_id);
  if (dependency_order.empty()) {
    dependency_order.push_back(node_id);
  }

  std::unordered_map<int, std::optional<RegionSet>> demands;
  demands[node_id] = clipped_region;
  for (auto order_it = dependency_order.rbegin();
       order_it != dependency_order.rend(); ++order_it) {
    const int current_id = *order_it;
    auto demand_it = demands.find(current_id);
    if (demand_it == demands.end() || !demand_it->second.has_value()) {
      continue;
    }
    const RegionSet& current_region = *demand_it->second;
    const Node& current_node = graph.node(current_id);
    if (current_id != node_id &&
        complete_tensor_dependency_available(current_node)) {
      continue;
    }

    const bool image_parent = has_image_parent(graph, current_id);
    if (!image_parent && current_id != node_id) {
      throw GraphError(
          GraphErrc::MissingDependency,
          "TensorSlice dependency path reached an uncached image source at "
          "node " +
              std::to_string(current_id) + ".");
    }
    DirtyRegionPlannedOperationRoute current_route =
        current_id == node_id
            ? target_route
            : select_tensor_operation_route_or_throw(
                  roi_propagation_, current_node, current_id, false);

    HpPlanEntry entry;
    entry.region_hp = current_region;
    if (current_node.cached_output_high_precision &&
        current_node.cached_output_high_precision->has_image_value() &&
        current_node.cached_output_high_precision->image_value()
            .image_facet()
            .has_value()) {
      const ImageView view(
          current_node.cached_output_high_precision->image_value());
      entry.hp_size = PixelSize{static_cast<int>(view.width()),
                                static_cast<int>(view.height())};
    }
    result.entries.emplace(current_id, entry);
    result.operation_routes.node_routes.emplace(current_id,
                                                std::move(current_route));
    result.snapshot.per_node_dirty_regions[current_id].push_back(
        entry.region_hp);
    result.snapshot.actual_dirty_regions[current_id].push_back(entry.region_hp);
    result.snapshot.dirty_monolithic_nodes.push_back(
        {current_id, DirtyDomain::HighPrecision, PixelRect{}, true,
         entry.region_hp});

    for (const GraphTopologyEdge& edge : graph.upstream_edges(current_id)) {
      if (edge.kind != GraphTopologyEdgeKind::ImageInput ||
          edge.from_node_id < 0 || !graph.has_node(edge.from_node_id)) {
        continue;
      }
      DirtyEdgeMapping mapping;
      mapping.from_node_id = edge.from_node_id;
      mapping.to_node_id = current_id;
      mapping.domain = DirtyDomain::HighPrecision;
      mapping.direction = DirtyEdgeDirection::BackwardDemand;
      mapping.from_region = current_region;
      mapping.to_region = current_region;
      result.snapshot.edge_mappings.push_back(std::move(mapping));

      const Node& parent = graph.node(edge.from_node_id);
      if (complete_tensor_dependency_available(parent)) {
        continue;
      }
      merge_tensor_demand(&demands[edge.from_node_id], current_region);
    }
  }

  for (int planned_id : dependency_order) {
    if (result.entries.count(planned_id) != 0U) {
      result.execution_order.push_back(planned_id);
    }
  }
  if (result.entries.empty() || result.execution_order.empty()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "TensorSlice planning produced no executable work.");
  }
  populate_dirty_source_metadata(graph, result.snapshot,
                                 DirtyDomain::HighPrecision, result.entries);
  return result;
}

RealTimeDirtyPlan DirtyRegionPlanner::plan_real_time(
    GraphModel& graph, int node_id, const PixelRect& dirty_roi) {
  return plan_dirty_domain<RealTimeDirtyPolicy>(graph, node_id, dirty_roi);
}

/** @copydoc DirtyRegionPlanner::plan_real_time(GraphModel&,int,const
 * RegionSet&) */
RealTimeDirtyPlan DirtyRegionPlanner::plan_real_time(
    GraphModel& graph, int node_id, const RegionSet& dirty_region) {
  if (dirty_region.is_empty() || dirty_region.is_whole() ||
      dirty_region.atoms().size() != 1U ||
      !std::holds_alternative<ImageRect>(dirty_region.atoms().front())) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "RT proxy accepts only one finite exact ImageRect Region.");
  }
  return plan_real_time(graph, node_id,
                        region_image_adapter::to_pixel_rect(dirty_region));
}

DirtyRegionSnapshot DirtyRegionPlanner::begin_dirty_source(
    GraphModel& graph, int node_id, DirtyDomain domain,
    const PixelRect& source_roi) {
  return update_dirty_source_snapshot(graph, node_id, domain, &source_roi,
                                      nullptr,
                                      DirtySourceLifecycleState::Updating);
}

/** @copydoc
 * DirtyRegionPlanner::begin_dirty_source(GraphModel&,int,DirtyDomain,const
 * RegionSet&) */
DirtyRegionSnapshot DirtyRegionPlanner::begin_dirty_source(
    GraphModel& graph, int node_id, DirtyDomain domain,
    const RegionSet& source_region) {
  if (source_region.is_empty() || source_region.is_whole()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Dirty source Region must be a finite nonempty atom.");
  }
  RegionSet normalized = source_region;
  if (source_region.atoms().size() == 1U &&
      std::holds_alternative<ImageRect>(source_region.atoms().front())) {
    (void)region_image_adapter::to_pixel_rect(source_region);
  } else {
    if (domain != DirtyDomain::HighPrecision) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "TensorSlice dirty lifecycle is available only in HP.");
    }
    normalized = validate_and_clip_tensor_region_or_throw(
        roi_propagation_, graph, node_id, source_region);
  }
  return update_dirty_source_snapshot(graph, node_id, domain, nullptr,
                                      &normalized,
                                      DirtySourceLifecycleState::Updating);
}

DirtyRegionSnapshot DirtyRegionPlanner::update_dirty_source(
    GraphModel& graph, int node_id, DirtyDomain domain,
    const PixelRect& source_roi) {
  return update_dirty_source_snapshot(graph, node_id, domain, &source_roi,
                                      nullptr,
                                      DirtySourceLifecycleState::Updating);
}

/** @copydoc
 * DirtyRegionPlanner::update_dirty_source(GraphModel&,int,DirtyDomain,const
 * RegionSet&) */
DirtyRegionSnapshot DirtyRegionPlanner::update_dirty_source(
    GraphModel& graph, int node_id, DirtyDomain domain,
    const RegionSet& source_region) {
  if (source_region.is_empty() || source_region.is_whole()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Dirty source Region must be a finite nonempty atom.");
  }
  RegionSet normalized = source_region;
  if (source_region.atoms().size() == 1U &&
      std::holds_alternative<ImageRect>(source_region.atoms().front())) {
    (void)region_image_adapter::to_pixel_rect(source_region);
  } else {
    if (domain != DirtyDomain::HighPrecision) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "TensorSlice dirty lifecycle is available only in HP.");
    }
    normalized = validate_and_clip_tensor_region_or_throw(
        roi_propagation_, graph, node_id, source_region);
  }
  return update_dirty_source_snapshot(graph, node_id, domain, nullptr,
                                      &normalized,
                                      DirtySourceLifecycleState::Updating);
}

DirtyRegionSnapshot DirtyRegionPlanner::end_dirty_source(GraphModel& graph,
                                                         int node_id,
                                                         DirtyDomain domain) {
  return update_dirty_source_snapshot(graph, node_id, domain, nullptr, nullptr,
                                      DirtySourceLifecycleState::Settled);
}

DirtyRegionSnapshot DirtyRegionPlanner::update_dirty_source_snapshot(
    GraphModel& graph, int node_id, DirtyDomain domain,
    const PixelRect* source_roi, const RegionSet* source_region,
    DirtySourceLifecycleState lifecycle) {
  DirtyRegionSnapshot snapshot =
      graph.last_dirty_region_snapshot.value_or(DirtyRegionSnapshot{});
  if (snapshot.graph_generation == 0) {
    snapshot.graph_generation = ++graph.dirty_generation_counter;
  }
  snapshot_builder_.apply_source_lifecycle_event(
      graph, snapshot,
      DirtySourceLifecycleUpdate{node_id, domain, source_roi, lifecycle,
                                 source_region});
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

    bool has_region_records = false;
    auto region_it = snapshot.per_node_dirty_regions.find(source_node_id);
    if (region_it != snapshot.per_node_dirty_regions.end()) {
      for (const auto& region : region_it->second) {
        if (region.is_empty()) {
          continue;
        }
        has_region_records = true;
        state.source_regions.push_back(region);
        snapshot.source_region_records[source_node_id].push_back(
            {source_node_id, domain, region, snapshot.graph_generation});
      }
    }

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
      if (!has_region_records) {
        const RegionSet region = region_image_adapter::from_pixel_rect(roi);
        state.source_regions.push_back(region);
        snapshot.source_region_records[source_node_id].push_back(
            {source_node_id, domain, region, snapshot.graph_generation});
      }
    }
  }
  snapshot.dirty_updating_count = 0;
}

PixelSize DirtyRegionPlanner::infer_hp_size(
    GraphModel& graph, int node_id,
    std::unordered_map<int, PixelSize>& cache) const {
  if (cache.count(node_id))
    return cache.at(node_id);

  PixelSize size{0, 0};
  size = extent_resolver_.resolve_output_extent(graph, node_id, cache);
  return size;
}

int DirtyRegionPlanner::infer_halo_hp(const GraphModel& graph,
                                      const Node& node) const {
  if (requires_conservative_parameter_geometry(node) &&
      !has_stabilized_geometry(node.id)) {
    return 0;
  }
  const plugin::ParameterMap parameters =
      resolve_effective_parameter_snapshot(node, graph);
  return ops::builtin_input_halo_radius(node.type, node.subtype, parameters);
}

bool DirtyRegionPlanner::has_stabilized_geometry(int node_id) const noexcept {
  return stabilized_geometry_nodes_ &&
         stabilized_geometry_nodes_->count(node_id) != 0;
}

uint64_t DirtyRegionPlanner::select_plan_generation(
    GraphModel& graph) const noexcept {
  if (fixed_generation_) {
    return *fixed_generation_;
  }
  return ++graph.dirty_generation_counter;
}

std::string DirtyRegionPlanner::describe_snapshot(
    const DirtyRegionSnapshot& snapshot) {
  std::ostringstream out;
  out << "generation=" << snapshot.graph_generation
      << " sources=" << snapshot.dirty_source_nodes.size()
      << " updating=" << snapshot.dirty_updating_count << " actual="
      << std::max(snapshot.actual_dirty_rois.size(),
                  snapshot.actual_dirty_regions.size())
      << " tiles=" << snapshot.dirty_tiles.size()
      << " monolithic=" << snapshot.dirty_monolithic_nodes.size() << " nodes="
      << std::max(snapshot.per_node_dirty_rois.size(),
                  snapshot.per_node_dirty_regions.size())
      << " edges=" << snapshot.edge_mappings.size();
  return out.str();
}

}  // namespace ps::compute
