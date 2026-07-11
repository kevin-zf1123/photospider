#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {

/**
 * @brief Compute domain represented by dirty-region records.
 *
 * @note HP and RT dirty snapshots are single-domain views. A single snapshot
 * record must not model dependencies from HP tasks to RT tasks or vice versa.
 */
enum class DirtyDomain {
  /** @brief Full-resolution high-precision compute domain. */
  HighPrecision,
  /** @brief Downscaled real-time proxy compute domain. */
  RealTime,
};

/**
 * @brief Tile granularity stored in dirty snapshot keys.
 *
 * @note Current snapshot materialization emits Micro keys; Macro remains part
 * of the stable model for future retile/coarsening records.
 */
enum class DirtyTileLevel {
  /** @brief Fine-grained dirty tile used for active dirty work. */
  Micro,
  /** @brief Coarser dirty tile used at aggregation boundaries. */
  Macro,
};

/**
 * @brief Direction of an ROI mapping across a graph edge.
 *
 * @note ForwardAffected describes affected downstream output. BackwardDemand
 * describes required upstream input for recomputation.
 */
enum class DirtyEdgeDirection {
  /** @brief Dirty source ROI projected downstream to affected output. */
  ForwardAffected,
  /** @brief Target dirty ROI projected upstream to required input. */
  BackwardDemand,
};

/**
 * @brief Lifecycle state of a dirty source node in the current generation.
 *
 * @note Source membership can remain marked after a node settles because
 * downstream dirty work for the same generation may still be queued, running,
 * aborted, or stale.
 */
enum class DirtySourceLifecycleState {
  /** @brief Source has no active lifecycle state. */
  Idle,
  /** @brief Source is still emitting dirty ROI updates. */
  Updating,
  /** @brief Source has stopped emitting updates for this generation. */
  Settled,
};

/**
 * @brief Source ROI emitted by one dirty source lifecycle event.
 *
 * @note Records are stable source facts. Derived actual dirty ROIs, tiles, and
 * monolithic records are rebuilt from these records.
 */
struct DirtySourceRoiRecord {
  /** @brief Graph node id that emitted the dirty ROI. */
  int node_id = -1;
  /** @brief Dirty domain associated with the source event. */
  DirtyDomain domain = DirtyDomain::HighPrecision;
  /** @brief Node-local source ROI recorded for the event. */
  cv::Rect source_roi;
  /** @brief Dirty generation in which the source ROI was recorded. */
  uint64_t generation = 0;
};

/**
 * @brief Per-source lifecycle and ROI history for one dirty generation.
 *
 * @note This state is written by node lifecycle events only. Propagated actual
 * dirty regions are derived into other DirtyRegionSnapshot fields.
 */
struct DirtySourceNodeState {
  /** @brief Graph node id for the dirty source. */
  int node_id = -1;
  /** @brief Dirty domain associated with the current source state. */
  DirtyDomain domain = DirtyDomain::HighPrecision;
  /** @brief Current lifecycle state for the source node. */
  DirtySourceLifecycleState lifecycle = DirtySourceLifecycleState::Idle;
  /** @brief Dirty generation represented by this source state. */
  uint64_t generation = 0;
  /** @brief Source ROIs accumulated for the generation. */
  std::vector<cv::Rect> source_rois;
};

/**
 * @brief Stable key describing one dirty tile in domain-local coordinates.
 *
 * @note Tile records use node ids and value-type coordinates so they remain
 * inspectable across graph reload and replacement workflows.
 */
struct DirtyTileKey {
  /** @brief Graph node id whose output tile is dirty. */
  int node_id = -1;
  /** @brief Compute domain of the tile record. */
  DirtyDomain domain = DirtyDomain::HighPrecision;
  /** @brief Tile granularity represented by the key. */
  DirtyTileLevel level = DirtyTileLevel::Micro;
  /** @brief X tile index in domain-local tile coordinates. */
  int tile_x = 0;
  /** @brief Y tile index in domain-local tile coordinates. */
  int tile_y = 0;
  /** @brief Tile edge length in domain-local pixels. */
  int tile_size = 0;
  /** @brief Pixel ROI covered by this tile key. */
  cv::Rect pixel_roi;
};

/**
 * @brief Dirty record for a node that must recompute as one monolithic output.
 *
 * @note Monolithic escalation is local to the node. Downstream propagation may
 * still narrow affected regions again after this node.
 */
struct DirtyMonolithicRegion {
  /** @brief Graph node id whose output is dirty. */
  int node_id = -1;
  /** @brief Compute domain of the monolithic dirty record. */
  DirtyDomain domain = DirtyDomain::HighPrecision;
  /** @brief Domain-local pixel ROI represented by this record. */
  cv::Rect pixel_roi;
  /** @brief True when the node output is dirty as a whole unit. */
  bool whole_output = true;
};

/**
 * @brief ROI mapping recorded for one dirty propagation edge.
 *
 * @note Edge mappings are inspection/provenance data for dirty planning. They
 * are not runtime dependency counters or scheduler queue records.
 */
struct DirtyEdgeMapping {
  /** @brief Upstream node id for the mapping. */
  int from_node_id = -1;
  /** @brief Downstream node id for the mapping. */
  int to_node_id = -1;
  /** @brief Dirty domain represented by the mapping. */
  DirtyDomain domain = DirtyDomain::HighPrecision;
  /** @brief ROI on from_node_id side of the edge. */
  cv::Rect from_roi;
  /** @brief ROI on to_node_id side of the edge. */
  cv::Rect to_roi;
  /** @brief Direction of the recorded propagation. */
  DirtyEdgeDirection direction = DirtyEdgeDirection::BackwardDemand;
};

/**
 * @brief Graph-scoped dirty state and derived propagation records.
 *
 * DirtyRegionSnapshot separates stable dirty source facts from derived actual
 * dirty ROIs. Dirty source lifecycle events update source membership, source
 * records, and lifecycle counters; propagation and snapshot builders derive
 * tiles, monolithic records, per-node ROIs, and edge mappings from those facts.
 *
 * @note The snapshot intentionally excludes runtime dependency counters, task
 * reference counts, ready queues, scheduler queues, and scheduler policy.
 */
struct DirtyRegionSnapshot {
  /** @brief Dirty generation represented by this snapshot. */
  uint64_t graph_generation = 0;
  /** @brief Source node ids that emitted dirty state in this generation. */
  std::vector<int> dirty_source_nodes;
  /** @brief Lifecycle state keyed by dirty source node id. */
  std::unordered_map<int, DirtySourceNodeState> dirty_source_state;
  /** @brief Source ROI records keyed by dirty source node id. */
  std::unordered_map<int, std::vector<DirtySourceRoiRecord>> source_roi_records;
  /** @brief Count of dirty source nodes currently in Updating state. */
  size_t dirty_updating_count = 0;
  /** @brief Domain-local dirty tile keys derived for active dirty work. */
  std::vector<DirtyTileKey> dirty_tiles;
  /** @brief Monolithic dirty records derived for nodes that cannot tile. */
  std::vector<DirtyMonolithicRegion> dirty_monolithic_nodes;
  /** @brief Dirty ROIs keyed by node id after domain-specific normalization. */
  std::unordered_map<int, std::vector<cv::Rect>> per_node_dirty_rois;
  /** @brief Actual dirty ROIs exposed to inspection and materialization. */
  std::unordered_map<int, std::vector<cv::Rect>> actual_dirty_rois;
  /** @brief Edge-level ROI mappings produced by dirty propagation. */
  std::vector<DirtyEdgeMapping> edge_mappings;

  /**
   * @brief Checks whether the snapshot has no dirty source or derived records.
   *
   * @return True when all source and derived dirty-region containers are empty.
   * @throws Nothing.
   * @note graph_generation alone does not make a snapshot non-empty.
   */
  bool empty() const;
};

}  // namespace ps::compute
