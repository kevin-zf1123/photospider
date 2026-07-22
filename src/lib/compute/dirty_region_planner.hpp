#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "compute/dirty_region_snapshot.hpp"
#include "compute/dirty_region_snapshot_builder.hpp"
#include "graph/graph_extent_resolver.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps {
class GraphTraversalService;
class RoiPropagationService;
}  // namespace ps

namespace ps::compute {

/**
 * @brief Per-node planning state for high-precision dirty execution.
 *
 * The entry stores the HP-space dirty ROI selected for one node, the resolved
 * HP output extent used for clipping, and the operator halo required by tiled
 * execution.
 *
 * @note All coordinates are in HP output space. The entry is transient and is
 * owned by HighPrecisionDirtyPlan for a single dirty update request.
 */
struct HpPlanEntry {
  /** @brief HP-space dirty ROI that should be recomputed for this node. */
  PixelRect roi_hp;

  /** @brief Resolved HP output extent used to clip roi_hp. */
  PixelSize hp_size;

  /** @brief HP-space halo radius required by neighborhood operators. */
  int halo_hp = 0;
};

/**
 * @brief Per-node planning state for real-time dirty execution.
 *
 * The entry keeps both HP-space ROI metadata and RT proxy-space work metadata.
 * HP coordinates remain the authoritative propagation and inspection space,
 * while RT coordinates drive proxy tile enumeration and execution.
 *
 * @note The RT fields are derived from HP fields using the current RT downscale
 * policy and must be refreshed whenever roi_hp or hp_size changes.
 */
struct RtPlanEntry {
  /** @brief HP-space dirty ROI used for propagation and debug metadata. */
  PixelRect roi_hp;

  /** @brief RT proxy-space dirty ROI used for RT tile execution. */
  PixelRect roi_rt;

  /** @brief Resolved HP output extent used to clip roi_hp. */
  PixelSize hp_size;

  /** @brief RT proxy output extent derived from hp_size. */
  PixelSize rt_size;

  /** @brief HP-space halo radius required by the source operator. */
  int halo_hp = 0;

  /** @brief RT proxy-space halo radius derived from halo_hp. */
  int halo_rt = 0;
};

/**
 * @brief Complete HP dirty planning result for one update request.
 *
 * The plan contains dependency order, per-node HP entries, and the graph-scoped
 * dirty snapshot consumed by dirty execution and debug inspection.
 *
 * @note The result is request-local; callers must not reuse it across graph
 * generations.
 */
struct HighPrecisionDirtyPlan {
  /** @brief Topological dependency order selected from the request target. */
  std::vector<int> execution_order;

  /** @brief HP dirty entry map keyed by graph node id. */
  std::unordered_map<int, HpPlanEntry> entries;

  /** @brief Snapshot describing dirty tiles, sources, ROIs, and edge mappings.
   */
  DirtyRegionSnapshot snapshot;
};

/**
 * @brief Complete RT dirty planning result for one update request.
 *
 * The plan mirrors the HP dirty plan but stores RT proxy work fields in each
 * entry and marks snapshot work in the RealTime domain.
 *
 * @note HP and RT dirty plans are separate single-domain plans; a single plan
 * must not mix task pools.
 */
struct RealTimeDirtyPlan {
  /** @brief Topological dependency order selected from the request target. */
  std::vector<int> execution_order;

  /** @brief RT dirty entry map keyed by graph node id. */
  std::unordered_map<int, RtPlanEntry> entries;

  /** @brief Snapshot describing RT dirty tiles, sources, ROIs, and edge
   * mappings. */
  DirtyRegionSnapshot snapshot;
};

/**
 * @brief Builds graph-scoped dirty-region plans and lifecycle snapshots.
 *
 * DirtyRegionPlanner translates node-local dirty ROI requests into per-domain
 * HP or RT execution plans by walking graph dependencies backward through
 * RoiPropagationService, clipping ROIs to resolved extents, escalating
 * monolithic nodes, and materializing DirtyRegionSnapshot records for later
 * execution and inspection.
 *
 * @note The planner mutates only dirty-generation/snapshot state on GraphModel;
 * it does not execute node work, own execution queues, or promote RT output to
 * HP cache authority.
 */
class DirtyRegionPlanner {
 public:
  /**
   * @brief Constructs a planner with borrowed traversal and ROI services.
   *
   * @param traversal Topology service used to obtain dependency order.
   * @param roi_propagation ROI propagation service used for backward demand.
   * @param stabilized_geometry_nodes Optional request-local exact-geometry
   * node identities.
   * @param forced_parameter_producers Optional RT image-producing parameter
   * nodes that must remain executable.
   * @param fixed_generation Optional generation already reserved for sibling
   * HP/RT plans.
   * @throws Nothing directly.
   * @note Both services must outlive the planner instance.
   */
  DirtyRegionPlanner(
      GraphTraversalService& traversal, RoiPropagationService& roi_propagation,
      const std::unordered_set<int>* stabilized_geometry_nodes = nullptr,
      const std::unordered_set<int>* forced_parameter_producers = nullptr,
      std::optional<uint64_t> fixed_generation = std::nullopt);

  /**
   * @brief Plans an HP dirty update rooted at a target node.
   *
   * @param graph Graph whose topology, extent metadata, and dirty generation
   * are updated.
   * @param node_id Target node that requested dirty recomputation.
   * @param dirty_roi HP-space dirty ROI in the target output coordinate space.
   * @return HP dirty plan with HP tile/monolithic snapshot records.
   * @throws GraphError when the node is missing, dirty_roi is empty, ROI
   * clipping removes all work, or no executable dirty entries remain.
   * @note The returned plan keeps all ROI metadata in HP coordinates.
   */
  HighPrecisionDirtyPlan plan_high_precision(GraphModel& graph, int node_id,
                                             const PixelRect& dirty_roi);

  /**
   * @brief Plans an RT dirty update rooted at a target node.
   *
   * @param graph Graph whose topology, extent metadata, and dirty generation
   * are updated.
   * @param node_id Target node that requested dirty recomputation.
   * @param dirty_roi HP-space dirty ROI that will be projected to RT proxy
   * space.
   * @return RT dirty plan with RT tile/monolithic snapshot records and HP-space
   * inspection ROIs.
   * @throws GraphError when the node is missing, dirty_roi is empty, ROI
   * clipping removes all work, RT projection collapses the ROI, or no
   * executable dirty entries remain.
   * @note RT planning never creates HP task dependencies; HP coordinates are
   * used only for propagation and inspection metadata.
   */
  RealTimeDirtyPlan plan_real_time(GraphModel& graph, int node_id,
                                   const PixelRect& dirty_roi);

  /**
   * @brief Records the beginning of a dirty source lifecycle event.
   *
   * @param graph Graph whose dirty snapshot is updated.
   * @param node_id Source node that started producing dirty regions.
   * @param domain Dirty domain associated with the source ROI.
   * @param source_roi Node-local dirty ROI in domain-authoritative coordinates.
   * @return Updated graph-scoped dirty snapshot.
   * @throws GraphError when node_id is missing or source_roi is empty.
   * @note Source membership remains until the dirty generation settles.
   */
  DirtyRegionSnapshot begin_dirty_source(GraphModel& graph, int node_id,
                                         DirtyDomain domain,
                                         const PixelRect& source_roi);

  /**
   * @brief Records an incremental dirty source ROI update.
   *
   * @param graph Graph whose dirty snapshot is updated.
   * @param node_id Source node that produced another dirty ROI.
   * @param domain Dirty domain associated with the source ROI.
   * @param source_roi Node-local dirty ROI in domain-authoritative coordinates.
   * @return Updated graph-scoped dirty snapshot.
   * @throws GraphError when node_id is missing or source_roi is empty.
   * @note The actual dirty region is refreshed from accumulated source records.
   */
  DirtyRegionSnapshot update_dirty_source(GraphModel& graph, int node_id,
                                          DirtyDomain domain,
                                          const PixelRect& source_roi);

  /**
   * @brief Records the end of a dirty source lifecycle event.
   *
   * @param graph Graph whose dirty snapshot is updated.
   * @param node_id Source node whose dirty update lifecycle settled.
   * @param domain Dirty domain associated with the source node.
   * @return Updated graph-scoped dirty snapshot.
   * @throws GraphError when node_id is missing.
   * @note Ending a source does not erase its dirty membership for the
   * generation.
   */
  DirtyRegionSnapshot end_dirty_source(GraphModel& graph, int node_id,
                                       DirtyDomain domain);

  /**
   * @brief Formats a compact debug summary for a dirty snapshot.
   *
   * @param snapshot Snapshot to summarize.
   * @return Stable human-readable count summary for inspection output.
   * @throws std::bad_alloc if string growth fails.
   * @note This is display-only text; structured consumers should use the
   * snapshot fields directly.
   */
  static std::string describe_snapshot(const DirtyRegionSnapshot& snapshot);

 private:
  /**
   * @brief Runs the shared dirty ROI planning flow for one HP or RT policy.
   *
   * @tparam Policy Domain policy that defines entry type, ROI alignment, RT
   * projection, snapshot domain, and tile enumeration parameters.
   * @param graph Graph whose topology and dirty generation are used.
   * @param node_id Target node for the dirty request.
   * @param dirty_roi Incoming HP-space dirty ROI.
   * @return Domain-specific dirty plan.
   * @throws GraphError from request validation, ROI projection, propagation, or
   * empty plan detection.
   * @note The template is defined in the .cpp and instantiated only for the
   * built-in HP and RT policies.
   */
  template <typename Policy>
  typename Policy::Plan plan_dirty_domain(GraphModel& graph, int node_id,
                                          const PixelRect& dirty_roi);

  /**
   * @brief Ensures a per-node plan entry has extent and halo metadata.
   *
   * @tparam Policy Domain policy that supplies the entry type and derived-field
   * refresh hooks.
   * @param graph Graph used for extent and halo inference.
   * @param plan Plan whose entry map is updated.
   * @param node_id Node id for the requested entry.
   * @param hp_size_cache Shared HP extent cache for the planning request.
   * @return Mutable entry for node_id.
   * @throws GraphError from extent resolution when graph metadata is invalid.
   * @note Existing entries preserve accumulated ROIs; only missing extent or
   * halo metadata is refreshed.
   */
  template <typename Policy>
  typename Policy::Entry& ensure_plan_entry(
      GraphModel& graph, typename Policy::Plan& plan, int node_id,
      std::unordered_map<int, PixelSize>& hp_size_cache);

  /**
   * @brief Propagates dirty demand backward through image-input edges.
   *
   * @tparam Policy Domain policy controlling upstream alignment and edge
   * records.
   * @param graph Graph whose image-input edges are traversed.
   * @param plan Plan whose entries and edge mappings are updated.
   * @param hp_size_cache Shared HP extent cache for parent entry creation.
   * @throws GraphError from ROI propagation or extent resolution.
   * @note The method preserves policy-specific HP and RT clipping order.
   */
  template <typename Policy>
  void propagate_dirty_entries(
      GraphModel& graph, typename Policy::Plan& plan,
      std::unordered_map<int, PixelSize>& hp_size_cache);

  /**
   * @brief Finalizes dirty entries into snapshot tiles and monolithic records.
   *
   * @tparam Policy Domain policy controlling final ROI projection and tile
   * size.
   * @param graph Graph whose node metadata determines monolithic boundaries.
   * @param plan Plan whose entries are clipped, erased, and snapshotted.
   * @throws GraphError only through helper calls that inspect graph metadata.
   * @note Empty or extentless entries are erased before source metadata is
   * built.
   */
  template <typename Policy>
  void finalize_dirty_entries(GraphModel& graph, typename Policy::Plan& plan);

  /**
   * @brief Applies a dirty source lifecycle update and stores the snapshot.
   *
   * @param graph Graph whose dirty snapshot is updated.
   * @param node_id Source node receiving the lifecycle transition.
   * @param domain Dirty domain associated with the source node.
   * @param source_roi Optional ROI appended for begin/update transitions.
   * @param lifecycle New lifecycle state for the source node.
   * @return Updated graph-scoped dirty snapshot.
   * @throws GraphError when node_id is missing or source_roi is empty.
   * @note This helper owns the shared begin/update/end storage flow; snapshot
   * derivation is delegated to DirtyRegionSnapshotBuilder.
   */
  DirtyRegionSnapshot update_dirty_source_snapshot(
      GraphModel& graph, int node_id, DirtyDomain domain,
      const PixelRect* source_roi, DirtySourceLifecycleState lifecycle);

  /**
   * @brief Populates dirty source metadata from finalized plan entries.
   *
   * @tparam EntryMap Map type keyed by node id with HP or RT entry values.
   * @param graph Graph used to identify planned image parents.
   * @param snapshot Snapshot receiving source-node records.
   * @param domain Dirty domain assigned to source records.
   * @param entries Finalized non-empty plan entries.
   * @throws std::bad_alloc if snapshot containers grow and allocation fails.
   * @note A source node is any planned entry without another planned image
   * parent.
   */
  template <typename EntryMap>
  void populate_dirty_source_metadata(GraphModel& graph,
                                      DirtyRegionSnapshot& snapshot,
                                      DirtyDomain domain,
                                      const EntryMap& entries) const;

  /**
   * @brief Resolves an HP output extent with request-local memoization.
   *
   * @param graph Graph whose output extent is queried.
   * @param node_id Node id to resolve.
   * @param cache Mutable memoization cache shared by one planning pass.
   * @return HP output extent, or an empty size when no extent can be inferred.
   * @throws GraphError from GraphExtentResolver on invalid graph metadata.
   * @note HP extent remains the authoritative propagation bound for RT
   * planning.
   */
  PixelSize infer_hp_size(GraphModel& graph, int node_id,
                          std::unordered_map<int, PixelSize>& cache) const;

  /**
   * @brief Infers an HP-space operator halo for dirty tiled execution.
   *
   * @param graph Graph used to resolve one exact effective parameter snapshot.
   * @param node Node whose static parameters are inspected.
   * @return Non-negative HP halo radius.
   * @throws std::bad_alloc if the effective ParameterMap cannot be copied.
   * @note Connected parameter producers return zero here because their current
   *       request value is not yet proven. The propagation pass detects that
   *       case, selects each parameter producer, promotes output and all image
   *       inputs to full extents, and installs an extent-bounded halo instead
   *       of invoking a propagator or dependency LUT with stale cached data.
   */
  int infer_halo_hp(const GraphModel& graph, const Node& node) const;

  /**
   * @brief Checks whether request-local parameter geometry is exact for a node.
   * @param node_id Graph node id to inspect.
   * @return True when stabilization marked the node or its image descendant as
   * geometry-affected for this request.
   * @throws Nothing.
   */
  bool has_stabilized_geometry(int node_id) const noexcept;

  /**
   * @brief Selects the graph generation for one newly planned domain.
   * @param graph Graph whose counter is incremented for standalone plans.
   * @return Fixed request generation when supplied, otherwise a new counter.
   * @throws Nothing.
   * @note A fixed generation is reserved by ComputeService before HP/RT
   * siblings start, so this method must not increment the live counter.
   */
  uint64_t select_plan_generation(GraphModel& graph) const noexcept;

  GraphTraversalService& traversal_;
  RoiPropagationService& roi_propagation_;
  GraphExtentResolver extent_resolver_;
  DirtyRegionSnapshotBuilder snapshot_builder_;
  /** @brief Optional exact-geometry node identities owned by the request. */
  const std::unordered_set<int>* stabilized_geometry_nodes_ = nullptr;
  /** @brief Optional RT image producers forced despite parameter snapshot. */
  const std::unordered_set<int>* forced_parameter_producers_ = nullptr;

  /** @brief Optional generation shared by sibling domain plans. */
  std::optional<uint64_t> fixed_generation_;
};

}  // namespace ps::compute
