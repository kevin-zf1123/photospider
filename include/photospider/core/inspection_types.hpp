#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "photospider/core/compute_intent.hpp"
#include "photospider/core/geometry.hpp"

/**
 * @file inspection_types.hpp
 * @brief Stable graph, node, execution, and dirty-region inspection snapshots.
 *
 * The snapshot values in this header are designed for frontend display and
 * future IPC serialization. They carry copied scalar/string/container data and
 * do not expose mutable backend Graph, runtime, planner, policy, executor, or
 * service objects.
 */

namespace ps {

/**
 * @brief Stable identifier for a graph session.
 *
 * @throws Nothing for value operations except string allocation on mutation.
 * @note The value is a frontend/session label, not a pointer or runtime handle.
 */
struct GraphSessionId {
  /** @brief Human-readable graph/session name or opaque serialized id. */
  std::string value;
};

/**
 * @brief Stable identifier for a graph node.
 *
 * @throws Nothing.
 * @note Negative ids represent an absent or unresolved node in snapshots.
 */
struct NodeId {
  /** @brief Numeric node id used by the loaded graph. */
  int value = -1;
};

/**
 * @brief Copied debug metadata for one node output.
 *
 * @throws Nothing for value operations except string allocation on mutation.
 * @note This is observational metadata; it must not be used as execution
 *       synchronization state.
 */
struct DebugMetadataSnapshot {
  /** @brief Worker id that produced the output, or -1 when unknown. */
  int computed_by_worker_id = -1;

  /** @brief Producer timestamp in microseconds since the active clock epoch. */
  uint64_t timestamp_us = 0;

  /** @brief Execution time reported by the producer in milliseconds. */
  uint64_t execution_time_ms = 0;

  /** @brief Minimum sampled output value, when available. */
  double min_val = 0.0;

  /** @brief Maximum sampled output value, when available. */
  double max_val = 0.0;

  /** @brief Whether the output contained at least one NaN sample. */
  bool has_nan = false;

  /** @brief Device label reported by the compute path. */
  std::string compute_device = "UNKNOWN";
};

/**
 * @brief Copied spatial metadata for one node output.
 *
 * @throws Nothing for value operations.
 * @note Matrices use row-major 3x3 affine/homogeneous storage.
 */
struct SpatialSnapshot {
  /** @brief Output extent in local pixels. */
  PixelSize extent;

  /** @brief Absolute ROI covered by the output. */
  PixelRect absolute_roi;

  /** @brief X scale from local output space to graph/global space. */
  double global_scale_x = 1.0;

  /** @brief Y scale from local output space to graph/global space. */
  double global_scale_y = 1.0;

  /** @brief Row-major transform matrix from local to graph/global space. */
  double transform_matrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

  /** @brief Row-major inverse transform matrix. */
  double inverse_matrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

  /** @brief Row-major local inverse transform used for upstream ROI mapping. */
  double local_inverse_matrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

/**
 * @brief Copied inspection metadata for one graph node.
 *
 * @throws Nothing for value operations except string/container allocation on
 *         mutation.
 * @note Parameters are exposed as strings to avoid forcing YAML dependencies
 *       onto host or IPC clients.
 */
struct NodeInspectionView {
  /** @brief Stable node id in the loaded graph. */
  NodeId id;

  /** @brief Human-readable node name. */
  std::string name;

  /** @brief Operation type label. */
  std::string type;

  /** @brief Operation subtype label. */
  std::string subtype;

  /** @brief Parameter values serialized for inspection by parameter name. */
  std::map<std::string, std::string> parameters;

  /** @brief True when a cached output exists for the node. */
  bool has_cached_output = false;

  /** @brief Optional source label associated with the node output. */
  std::optional<std::string> source_label;

  /** @brief Optional debug metadata copied from the latest output. */
  std::optional<DebugMetadataSnapshot> debug;

  /** @brief Optional spatial metadata copied from the latest output. */
  std::optional<SpatialSnapshot> space;
};

/**
 * @brief Copied inspection snapshot for a loaded graph.
 *
 * @throws Nothing for value operations except vector/string allocation on
 *         mutation.
 * @note The snapshot is immutable by convention once published to a frontend.
 */
struct GraphInspectionView {
  /** @brief Session id for the graph that was inspected. */
  GraphSessionId session;

  /** @brief Nodes visible in the graph at inspection time. */
  std::vector<NodeInspectionView> nodes;
};

/**
 * @brief Dirty-region compute domain represented by inspection records.
 *
 * @throws Nothing.
 * @note This is a public snapshot label, not an internal execution queue id.
 */
enum class DirtyDomain {
  /** @brief Full-resolution high-precision domain. */
  HighPrecision,

  /** @brief Downscaled real-time proxy domain. */
  RealTime,
};

/**
 * @brief Lifecycle state of a dirty source in an inspection snapshot.
 *
 * @throws Nothing.
 * @note Source lifecycle is copied state and must not be used as a lock-free
 *       coordination primitive by consumers.
 */
enum class DirtySourceLifecycleState {
  /** @brief Source is not currently part of an active dirty lifecycle. */
  Idle,

  /** @brief Source is still emitting dirty updates. */
  Updating,

  /** @brief Source stopped emitting updates for the current generation. */
  Settled,
};

/**
 * @brief Direction of a dirty ROI mapping across a graph edge.
 *
 * @throws Nothing.
 * @note The value describes diagnostic propagation provenance only; it is not a
 *       task dependency direction or mutable Graph edge handle.
 */
enum class DirtyEdgeDirection {
  /** @brief Dirty source ROI projected downstream to affected output. */
  ForwardAffected,

  /** @brief Target dirty ROI projected upstream to required input. */
  BackwardDemand,
};

/**
 * @brief Dirty source state copied for frontend inspection.
 *
 * @throws Nothing for value operations except vector allocation on mutation.
 * @note ROIs are source-local pixel rectangles.
 */
struct DirtySourceSnapshot {
  /** @brief Source node id. */
  NodeId node;

  /** @brief Compute domain for the source record. */
  DirtyDomain domain = DirtyDomain::HighPrecision;

  /** @brief Lifecycle state for the current generation. */
  DirtySourceLifecycleState lifecycle = DirtySourceLifecycleState::Idle;

  /** @brief Dirty generation represented by this source state. */
  uint64_t generation = 0;

  /** @brief Source-local dirty ROIs accumulated for the generation. */
  std::vector<PixelRect> source_rois;
};

/**
 * @brief Dirty tile record copied for frontend inspection.
 *
 * @throws Nothing.
 * @note Tile coordinates are domain-local and do not expose execution task
 *       handles or dependency counters.
 */
struct DirtyTileSnapshot {
  /** @brief Node whose output tile is dirty. */
  NodeId node;

  /** @brief Compute domain represented by the tile. */
  DirtyDomain domain = DirtyDomain::HighPrecision;

  /** @brief X tile index in domain-local coordinates. */
  int tile_x = 0;

  /** @brief Y tile index in domain-local coordinates. */
  int tile_y = 0;

  /** @brief Tile edge length in domain-local pixels. */
  int tile_size = 0;

  /** @brief Pixel ROI covered by the tile. */
  PixelRect pixel_roi;
};

/**
 * @brief Monolithic dirty work record copied for frontend inspection.
 *
 * The record identifies a node whose dirty work could not be represented as
 * tiles and therefore must be recomputed as a single domain-local region.
 *
 * @throws Nothing.
 * @note This is diagnostic planning state. It does not expose task ownership,
 *       execution queues, or mutable node output buffers.
 */
struct DirtyMonolithicRegionSnapshot {
  /** @brief Node whose output region is dirty. */
  NodeId node;

  /** @brief Compute domain represented by the dirty record. */
  DirtyDomain domain = DirtyDomain::HighPrecision;

  /** @brief Domain-local pixel ROI covered by the monolithic record. */
  PixelRect pixel_roi;

  /** @brief True when the whole node output is represented by this record. */
  bool whole_output = true;
};

/**
 * @brief Dirty ROI propagation record copied for frontend inspection.
 *
 * The record preserves the source and target node ids plus the ROI on both
 * sides of one graph edge so diagnostic views can explain why upstream or
 * downstream dirty work was selected.
 *
 * @throws Nothing.
 * @note Edge mappings are value snapshots. They must not be interpreted as
 *       runtime dependency counters or as writable graph topology.
 */
struct DirtyEdgeMappingSnapshot {
  /** @brief Upstream node id for the mapped edge. */
  NodeId from_node;

  /** @brief Downstream node id for the mapped edge. */
  NodeId to_node;

  /** @brief Compute domain represented by the mapping. */
  DirtyDomain domain = DirtyDomain::HighPrecision;

  /** @brief ROI on the upstream side of the edge. */
  PixelRect from_roi;

  /** @brief ROI on the downstream side of the edge. */
  PixelRect to_roi;

  /** @brief Direction of the recorded dirty propagation. */
  DirtyEdgeDirection direction = DirtyEdgeDirection::BackwardDemand;
};

/**
 * @brief Dirty-region snapshot copied from the kernel for inspection.
 *
 * @throws Nothing for value operations except vector allocation on mutation.
 * @note This snapshot excludes ready queues, execution ownership, task
 *       reference counts, and mutable graph runtime state.
 */
struct DirtyRegionInspectionSnapshot {
  /** @brief Dirty generation represented by the snapshot. */
  uint64_t graph_generation = 0;

  /** @brief Source nodes and source-local ROI history. */
  std::vector<DirtySourceSnapshot> sources;

  /** @brief Domain-local dirty tiles derived for recomputation. */
  std::vector<DirtyTileSnapshot> dirty_tiles;

  /** @brief Monolithic dirty records derived for non-tiled recomputation. */
  std::vector<DirtyMonolithicRegionSnapshot> dirty_monolithic_nodes;

  /** @brief Dirty ROIs keyed by node id value after propagation. */
  std::map<int, std::vector<PixelRect>> actual_dirty_rois;

  /** @brief Edge-level ROI mappings produced by dirty propagation. */
  std::vector<DirtyEdgeMappingSnapshot> edge_mappings;
};

/**
 * @brief Sample of one planned compute task copied for inspection.
 *
 * The sample describes the task shape, node, ROI, dirty-generation metadata,
 * and dependency ids from a bounded planning summary. It intentionally omits
 * runtime queues, task closures, and mutable node output ownership.
 *
 * @throws Nothing for value operations except vector allocation on mutation.
 * @note Task ids are local to the planning snapshot that produced them.
 */
struct ComputePlanningTaskSnapshot {
  /** @brief Dense task id inside the copied planning snapshot. */
  int task_id = -1;

  /** @brief Graph node represented by the sampled task. */
  NodeId node;

  /** @brief Human-readable task shape, such as node, tile, or monolithic. */
  std::string kind;

  /** @brief Compute domain represented by the task. */
  DirtyDomain domain = DirtyDomain::HighPrecision;

  /** @brief Output pixel ROI covered by the task. */
  PixelRect output_roi;

  /** @brief Tile x index for tile tasks, or -1 for non-tile tasks. */
  int tile_x = -1;

  /** @brief Tile y index for tile tasks, or -1 for non-tile tasks. */
  int tile_y = -1;

  /** @brief Tile side length for tile tasks, or 0 for non-tile tasks. */
  int tile_size = 0;

  /** @brief True when the task represents a full node output. */
  bool whole_output = false;

  /** @brief True when dirty planning selected the task for the generation. */
  bool dirty_selected = false;

  /** @brief Dirty generation associated with dirty_selected, if any. */
  uint64_t dirty_generation = 0;

  /** @brief Upstream task ids that must complete first in this snapshot. */
  std::vector<int> dependency_task_ids;
};

/**
 * @brief Bounded planning diagnostic copied for frontend inspection.
 *
 * The snapshot summarizes the most recent compute planning result or one entry
 * from bounded history. It exposes counts, node samples, and task samples as
 * values so UI, CLI, and future IPC clients can reason about planning without
 * holding backend objects.
 *
 * @throws Nothing for value operations except string/vector allocation on
 *         mutation.
 * @note The cache key is diagnostic text only and must not be used as an
 *       ownership handle.
 */
struct ComputePlanningInspectionSnapshot {
  /** @brief Compute intent represented by the planning snapshot. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;

  /** @brief Target node requested by the caller. */
  NodeId target_node;

  /** @brief True when the caller requested parallel private execution. */
  bool parallel = false;

  /** @brief Graph topology generation used for expansion reuse. */
  uint64_t topology_generation = 0;

  /** @brief Diagnostic cache key for the full expansion, when known. */
  std::string expansion_cache_key;

  /** @brief Number of planned nodes. */
  size_t planned_node_count = 0;

  /** @brief Number of executable tasks in the planning snapshot. */
  size_t task_count = 0;

  /** @brief Number of tile-shaped tasks. */
  size_t tile_task_count = 0;

  /** @brief Number of monolithic tasks. */
  size_t monolithic_task_count = 0;

  /** @brief Number of generic node-shaped tasks. */
  size_t node_task_count = 0;

  /** @brief Number of node-level dependency records. */
  size_t dependency_count = 0;

  /** @brief Number of initially ready tasks. */
  size_t initial_task_count = 0;

  /** @brief Number of active tasks after dirty selection, if any. */
  size_t active_task_count = 0;

  /** @brief Number of dirty-source tasks selected by the planner. */
  size_t dirty_source_task_count = 0;

  /** @brief Number of downstream dirty tasks selected by the planner. */
  size_t downstream_task_count = 0;

  /** @brief Number of initially ready downstream dirty tasks. */
  size_t initial_downstream_task_count = 0;

  /** @brief Prefix sample of planned node ids. */
  std::vector<NodeId> planned_node_sample;

  /** @brief Prefix sample of planned task diagnostics. */
  std::vector<ComputePlanningTaskSnapshot> task_sample;
};

}  // namespace ps
