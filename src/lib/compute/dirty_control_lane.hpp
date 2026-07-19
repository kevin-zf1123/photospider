#pragma once

#include <cstddef>
#include <optional>

#include "compute/dirty_region_snapshot.hpp"

namespace ps {

class GraphModel;
class GraphTraversalService;
class RoiPropagationService;

namespace compute {

/**
 * @brief Publicly reportable phase of one dirty-source lifecycle transition.
 * @throws Nothing.
 */
enum class DirtyControlEvent {
  /** @brief A source entered the updating state with its first ROI. */
  Begin,
  /** @brief A source appended another ROI while remaining active. */
  Update,
  /** @brief A source ended its current updating interval. */
  End
};

/**
 * @brief Snapshot and dispatch hints produced by one dirty control transition.
 *
 * @throws std::bad_alloc when copied snapshot storage cannot allocate.
 * @note The value owns no graph or scheduler state. Dispatch consumers decide
 *       how to act on the hints after the graph-scoped transition completes.
 */
struct DirtyControlLaneResult {
  /** @brief Immutable-by-convention snapshot after the transition. */
  DirtyRegionSnapshot snapshot;

  /** @brief Lifecycle event represented by this result. */
  DirtyControlEvent event = DirtyControlEvent::Update;

  /** @brief Graph dirty generation copied from snapshot. */
  uint64_t generation = 0;

  /** @brief Number of sources still in the updating state. */
  size_t dirty_updating_count = 0;

  /** @brief Whether the snapshot exposes work that can wake a dispatcher. */
  bool should_wake_dispatcher = false;

  /** @brief Whether the last source ended after downstream work was derived. */
  bool cutoff_after_downstream = false;
};

/**
 * @brief Applies graph-scoped dirty-source lifecycle transitions.
 *
 * The lane delegates source-fact mutation and derived snapshot construction to
 * `DirtyRegionPlanner`, then converts the snapshot into dispatch/cutoff hints.
 *
 * @throws Nothing during construction.
 * @note The lane borrows traversal and ROI-propagation services. The caller
 *       must keep both services and the mutated graph alive for each call.
 */
class DirtyControlLane {
 public:
  /**
   * @brief Binds the graph services used by each lifecycle transition.
   * @param traversal Borrowed topology traversal service.
   * @param roi_propagation Borrowed ROI propagation service.
   * @throws Nothing.
   */
  DirtyControlLane(GraphTraversalService& traversal,
                   RoiPropagationService& roi_propagation);

  /**
   * @brief Begins one source update and records its first kernel-native ROI.
   * @param graph Graph whose dirty source facts and snapshot are updated.
   * @param node_id Source node entering the updating state.
   * @param domain Dirty domain associated with the source event.
   * @param source_roi Positive-area ROI in the source node coordinate space.
   * @return Snapshot and dispatch hints after the begin transition.
   * @throws GraphError for a missing node or invalid ROI.
   * @throws std::bad_alloc when planner or snapshot storage cannot allocate.
   * @note Geometry remains `PixelRect`; this layer performs no provider
   *       conversion or scheduler submission.
   */
  DirtyControlLaneResult begin_dirty_source(GraphModel& graph, int node_id,
                                            DirtyDomain domain,
                                            const PixelRect& source_roi) const;

  /**
   * @brief Appends one source ROI while retaining its updating lifecycle.
   * @param graph Graph whose dirty source facts and snapshot are updated.
   * @param node_id Source node receiving the incremental update.
   * @param domain Dirty domain associated with the source event.
   * @param source_roi Positive-area ROI in the source node coordinate space.
   * @return Snapshot and dispatch hints after the update transition.
   * @throws GraphError for a missing node or invalid ROI.
   * @throws std::bad_alloc when planner or snapshot storage cannot allocate.
   * @note The resulting snapshot may conservatively merge multiple source
   *       facts without transferring graph ownership to a scheduler.
   */
  DirtyControlLaneResult update_dirty_source(GraphModel& graph, int node_id,
                                             DirtyDomain domain,
                                             const PixelRect& source_roi) const;

  /**
   * @brief Ends one source update without appending another ROI.
   * @param graph Graph whose dirty source lifecycle is updated.
   * @param node_id Source node leaving the updating state.
   * @param domain Dirty domain associated with the source event.
   * @return Snapshot and dispatch/cutoff hints after the end transition.
   * @throws GraphError when node_id is missing.
   * @throws std::bad_alloc when planner or snapshot storage cannot allocate.
   * @note cutoff_after_downstream becomes true only when this transition leaves
   *       no updating sources and derived dirty work remains.
   */
  DirtyControlLaneResult end_dirty_source(GraphModel& graph, int node_id,
                                          DirtyDomain domain) const;

 private:
  /**
   * @brief Converts a finalized snapshot into control-lane result metadata.
   * @param snapshot Snapshot produced by the planner.
   * @param event Lifecycle event that produced snapshot.
   * @return Owned result with dispatch and cutoff hints.
   * @throws std::bad_alloc when snapshot copying cannot allocate.
   * @note The helper does not mutate graph state or submit work.
   */
  DirtyControlLaneResult build_result(const DirtyRegionSnapshot& snapshot,
                                      DirtyControlEvent event) const;

  /** @brief Borrowed topology service used to construct planners. */
  GraphTraversalService& traversal_;

  /** @brief Borrowed ROI service used to construct planners. */
  RoiPropagationService& roi_propagation_;
};

}  // namespace compute
}  // namespace ps
