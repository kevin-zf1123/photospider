#pragma once

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

#include "graph/graph_extent_resolver.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "photospider/core/compute_intent.hpp"
#include "photospider/core/device.hpp"
#include "photospider/data/region.hpp"

namespace ps {

/**
 * @brief Separates shared upstream demand from one dependency-LUT input route.
 *
 * Operator dirty propagation applies to every image input. A validated
 * dependency LUT names exactly one destination input index and therefore stays
 * separate until topology traversal selects an edge.
 *
 * @throws Nothing for ordinary value operations.
 * @note Geometry is stored in node-input coordinates and owns no graph state.
 */
struct UpstreamRoiProjection {
  /** @brief Operator/spatial demand shared by all image-input edges. */
  PixelRect shared_roi;
  /** @brief Input index selected by the dependency table, when present. */
  std::optional<std::size_t> dependency_input_index;
  /** @brief LUT-derived ROI applied only to dependency_input_index. */
  PixelRect dependency_roi;

  /**
   * @brief Returns demand routed to one destination image-input index.
   * @param input_index Destination input index from graph topology.
   * @return Shared ROI merged with dependency_roi only for its selected input.
   * @throws Nothing.
   */
  PixelRect roi_for_input(std::size_t input_index) const noexcept;

  /**
   * @brief Returns safely bounded demand for one destination image input.
   * @param input_index Destination input index from graph topology.
   * @param input_extent Current extent of the selected upstream image.
   * @return Union of independently clipped shared and selected dependency
   *         demand, or an empty rectangle when neither intersects the input.
   * @throws Nothing.
   * @note Clipping each contribution before union prevents an extreme but
   *       completely out-of-bounds shared ROI from discarding a valid LUT ROI
   *       through an unrepresentable intermediate union.
   */
  PixelRect roi_for_input(std::size_t input_index,
                          const PixelSize& input_extent) const noexcept;

  /**
   * @brief Returns a conservative union for legacy single-ROI callers.
   * @return Union of shared and dependency contributions.
   * @throws Nothing.
   * @note Graph traversal must prefer roi_for_input() to preserve LUT routing.
   */
  PixelRect combined_roi() const noexcept;
};

/**
 * @brief Computes operator-aware ROI propagation across graph topology.
 *
 * RoiPropagationService is the ROI/spatial propagation boundary. It consumes
 * GraphModel topology and GraphExtentResolver extents, applies operator dirty
 * and forward propagators from OpRegistry, merges spatial metadata and optional
 * dependency LUT results, and projects ROIs forward or backward through image
 * input edges.
 *
 * @note The service does not own graph topology, dirty snapshots, execution
 * queues, or compute task state. Callers provide graph state and request-local
 * size caches when needed. Each instance owns the route-visible device
 * inventory and compute intent whose implementation selection governs exact
 * TensorSlice eligibility.
 */
class RoiPropagationService {
 public:
  /**
   * @brief Binds Region propagation to one execution-selection context.
   *
   * @param available_devices Route-visible devices in runtime inventory order.
   * @param intent HP or RT selection policy used by the owning request.
   * @throws std::bad_alloc when copying the device inventory cannot allocate.
   * @note Default construction preserves the legacy CPU-only HP context used
   *       by Kernel control surfaces and standalone propagation tests. Dirty
   *       executors inject their request's actual route inventory and intent.
   */
  explicit RoiPropagationService(
      std::vector<DeviceBackend> available_devices = {DeviceBackend::CPU},
      ComputeIntent intent = ComputeIntent::GlobalHighPrecision);

  /**
   * @brief Selects one coherent implementation in this service's route
   * context.
   *
   * @param node Node whose operation key is selected.
   * @return Owned execution and propagation callbacks, metadata, exact
   *         revision identity, device, and callback-shape snapshot; or nullopt
   *         when no implementation can run on the bound inventory and intent.
   * @throws std::bad_alloc or any callback-copy exception from coherent
   *         registry snapshot selection.
   * @note This delegates to OpRegistry::select_implementation(): intent
   *       eligibility is checked before HP/RT device-shape priority and cost,
   *       followed only then by intent-specific scalar and legacy fallback.
   *       All callbacks belong to the selected revision; absence uses identity
   *       or no-dependency behavior and never a sibling callback. The returned
   *       value remains valid across registry mutation and may retain a plugin
   *       DSO lease. A caller invoking selected-implementation propagation must
   *       keep it alive through that synchronous callback. Region planning may
   *       then retain only callback-free identity, device, callback shape, and
   *       metadata fields and must release the selected value before returning
   *       a plan.
   */
  std::optional<OpImplementation> select_route_implementation(
      const Node& node) const;

  /**
   * @brief Returns the route-visible device inventory bound at construction.
   *
   * @return Borrowed immutable inventory valid for this service's lifetime.
   * @throws Nothing.
   * @note The inventory preserves caller order; consumers comparing route
   * contexts should canonicalize it as a set.
   */
  const std::vector<DeviceBackend>& available_devices() const noexcept {
    return available_devices_;
  }

  /**
   * @brief Returns the registry selection intent bound at construction.
   *
   * @return GlobalHighPrecision or RealTimeUpdate selection policy.
   * @throws Nothing.
   */
  ComputeIntent intent() const noexcept { return intent_; }

  /**
   * @brief Reports whether the route-selected operation has exact tensor work.
   *
   * @param node Node whose current operation implementation is selected.
   * @return True only when the selected revisioned implementation is the exact
   *         source-private core dense monolithic callback.
   * @throws std::bad_alloc or callback-copy exceptions from registry snapshot
   *         selection.
   * @note Selection uses this instance's complete device inventory and intent.
   *       The actual route candidate is selected before core identity testing;
   *       the identity predicate never filters candidates and thereby falls
   *       back to a different scalar implementation.
   */
  bool supports_tensor_region_execution(const Node& node) const;

  /**
   * @brief Projects one logical Region forward through graph image edges.
   *
   * @param graph Graph whose topology and current operation contracts apply.
   * @param start_node_id Node where affected work originates.
   * @param start_region Exact normalized Region in the start node domain.
   * @param target_node_id Downstream node whose affected Region is requested.
   * @return Exact ImageRect/TensorSlice result or a typed failure.
   * @throws GraphError or callback exceptions from exact ImageRect propagation.
   * @throws std::bad_alloc when traversal or result storage cannot allocate.
   * @note Current v2 callbacks are invoked only for one exact ImageRect through
   *       the checked private adapter. TensorSlice traverses only the explicit
   *       source-private core dense identity contract.
   */
  RegionOperationResult project_region_forward(const GraphModel& graph,
                                               int start_node_id,
                                               const RegionSet& start_region,
                                               int target_node_id) const;

  /**
   * @brief Projects one logical Region backward through graph image edges.
   *
   * @param graph Graph whose topology and current operation contracts apply.
   * @param target_node_id Downstream node where demand originates.
   * @param target_region Exact normalized Region in the target node domain.
   * @param source_node_id Upstream node whose required Region is requested.
   * @return Exact ImageRect/TensorSlice result or a typed failure.
   * @throws GraphError or callback exceptions from exact ImageRect propagation.
   * @throws std::bad_alloc when traversal or result storage cannot allocate.
   * @note A missing TensorSlice transform returns Unsupported and never invokes
   *       a rectangular callback or widens to Whole.
   */
  RegionOperationResult project_region_backward(const GraphModel& graph,
                                                int target_node_id,
                                                const RegionSet& target_region,
                                                int source_node_id) const;

  /**
   * @brief Computes one node's immediate logical upstream demand.
   *
   * @param node Destination node whose transform is applied.
   * @param downstream_region Exact normalized output Region.
   * @param graph Graph supplying topology, extents, and cached metadata.
   * @param size_cache Request-local image extent cache.
   * @return Exact logical upstream demand or a typed unsupported outcome.
   * @throws GraphError or callback exceptions for the ImageRect path.
   * @throws std::bad_alloc when result storage cannot allocate.
   * @note TensorSlice is preserved only by the explicit core dense identity
   *       operation; other current private core operations expose only
   *       rectangular propagation semantics. Operation ABI v1 Region suites
   *       use their separately validated rank-general contract.
   */
  RegionOperationResult compute_upstream_region(
      const Node& node, const RegionSet& downstream_region,
      const GraphModel& graph,
      std::unordered_map<int, PixelSize>& size_cache) const;

  /**
   * @brief Computes shared and input-selected upstream ROI contributions.
   *
   * @param node Node whose input demand is being computed.
   * @param downstream_roi ROI in node output coordinates.
   * @param graph Graph supplying topology, caches, and extent context.
   * @param size_cache Request-local output extent cache.
   * @return Projection retaining dependency input-index routing.
   * @throws GraphError or callback exceptions from extent, parameter, dirty,
   *         spatial, or dependency propagation.
   * @throws std::bad_alloc when route selection or request-local snapshots
   *         cannot allocate.
   * @throws Any exception raised while copying a selected callback target.
   * @note One exact implementation is selected using this service's intent and
   *       device context, then its coherent dirty/dependency callbacks and
   *       metadata consume one effective parameter and input-extent snapshot.
   *       A selected revision with no dirty callback uses identity propagation;
   *       one with no dependency builder adds no dependency demand. Only when
   *       no implementation is selectable does this compatibility API consult
   *       operation-level legacy propagation registrations. The selected
   *       temporary and any DSO lease remain alive through the synchronous
   *       propagation callback, then are released; no callback-bearing value
   *       enters a Region snapshot.
   */
  UpstreamRoiProjection compute_upstream_projection(
      const Node& node, const PixelRect& downstream_roi,
      const GraphModel& graph,
      std::unordered_map<int, PixelSize>& size_cache) const;

  /**
   * @brief Computes upstream demand with one already selected exact revision.
   *
   * @param node Node whose input demand is being computed.
   * @param downstream_roi ROI in node output coordinates.
   * @param graph Graph supplying topology, caches, and extent context.
   * @param size_cache Request-local output extent cache.
   * @param selected Exact implementation returned by
   *        select_route_implementation() for this node and service context.
   *        The caller must keep it alive throughout this synchronous call.
   * @return Projection retaining dependency input-index routing.
   * @throws GraphError or callback exceptions from extent, parameter, dirty,
   *         spatial, or dependency propagation.
   * @throws std::bad_alloc when request-local snapshots or selected callback
   *         temporaries cannot allocate.
   * @throws Any exception raised while copying a selected callback target.
   * @note Dirty and dependency callbacks, their metadata, and the execution
   *       identity all come from `selected`. If that revision omits a dirty
   *       callback, identity propagation is used; if it omits a dependency
   *       builder, no dependency contribution is produced. Neither absence
   *       may borrow a callback from a sibling revision. The caller must keep
   *       `selected` and any plugin DSO lease alive through the immediate
   *       propagation callback. Afterward Region planning may retain only the
   *       operation key plus callback-free identity, device, callback shape,
   *       and metadata fields; `selected` must not enter a long-lived plan or
   *       Region snapshot.
   */
  UpstreamRoiProjection compute_upstream_projection_for_selected_implementation(
      const Node& node, const PixelRect& downstream_roi,
      const GraphModel& graph, std::unordered_map<int, PixelSize>& size_cache,
      const OpImplementation& selected) const;

  /**
   * @brief Computes the upstream input ROI required by one node output ROI.
   *
   * The method clips the downstream ROI to the node output extent, applies the
   * registered dirty propagator, merges single-input spatial inverse metadata
   * when a high-precision cached output exists, and merges data-dependent LUT
   * lookup results when the operator registers a dependency builder.
   *
   * @param node Node whose input demand is being computed.
   * @param downstream_roi ROI in the node output coordinate space.
   * @param graph Graph supplying topology and extent context.
   * @param size_cache Request-local output extent cache shared by propagation
   * callers.
   * @return Required upstream ROI, or an empty rect when no valid demand can be
   * derived.
   * @throws GraphError or operator-specific exceptions when extent resolution
   * or registered propagation logic fails.
   * @note This conservative compatibility view unions input-selected LUT
   * demand. Graph traversal uses compute_upstream_projection() instead.
   */
  PixelRect compute_upstream_roi(
      const Node& node, const PixelRect& downstream_roi,
      const GraphModel& graph,
      std::unordered_map<int, PixelSize>& size_cache) const;

  /**
   * @brief Projects a dirty ROI forward through downstream image-input edges.
   *
   * @param graph Graph whose topology is traversed.
   * @param start_node_id Node where the ROI originates.
   * @param start_roi ROI in start_node_id output coordinates.
   * @param target_node_id Downstream node whose affected ROI is requested.
   * @return Affected ROI in target_node_id output coordinates, or nullopt when
   * no valid path/ROI reaches the target.
   * @throws GraphError or operator-specific exceptions when extent resolution
   * or registered forward propagation fails.
   * @note Traversal stores stable node ids and value ROIs only; it does not
   * mutate dirty-region snapshots.
   */
  std::optional<PixelRect> project_roi_forward(const GraphModel& graph,
                                               int start_node_id,
                                               const PixelRect& start_roi,
                                               int target_node_id) const;

  /**
   * @brief Projects a target output ROI backward to an upstream source node.
   *
   * @param graph Graph whose topology is traversed.
   * @param target_node_id Downstream node where demand starts.
   * @param target_roi ROI in target_node_id output coordinates.
   * @param source_node_id Upstream source node whose required ROI is requested.
   * @return Required ROI in source_node_id output coordinates, or nullopt when
   * no valid path/ROI reaches the source.
   * @throws GraphError or operator-specific exceptions when extent resolution,
   * dirty propagation, spatial metadata, or dependency LUT logic fails.
   * @note This is graph-level demand projection; graph traversal remains
   * topology-only and does not own ROI propagation semantics.
   */
  std::optional<PixelRect> project_roi_backward(const GraphModel& graph,
                                                int target_node_id,
                                                const PixelRect& target_roi,
                                                int source_node_id) const;

 private:
  /** @brief Owned route-visible devices for implementation selection. */
  std::vector<DeviceBackend> available_devices_;
  /** @brief Request compute intent controlling registry candidate ordering. */
  ComputeIntent intent_;
  /** @brief Stateless graph extent resolver for rectangular propagation. */
  GraphExtentResolver extent_resolver_;
};

}  // namespace ps
