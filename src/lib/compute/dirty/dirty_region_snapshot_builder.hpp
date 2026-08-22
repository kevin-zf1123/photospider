#pragma once

#include <unordered_map>

#include "compute/dirty/dirty_region_snapshot.hpp"
#include "graph/graph_extent_resolver.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {

/**
 * @brief Source lifecycle transition requested by a dirty control event.
 *
 * @note Begin/update carries exactly one of source_roi or source_region.
 * End/settled carries neither. Any pointed value must outlive the
 * apply_source_lifecycle_event call.
 */
struct DirtySourceLifecycleUpdate {
  /** @brief Source node id receiving the lifecycle transition. */
  int node_id = -1;

  /** @brief Dirty domain associated with the source node. */
  DirtyDomain domain = DirtyDomain::HighPrecision;

  /** @brief Optional zero-based HP storage ROI for begin/update transitions. */
  const PixelRect* source_roi = nullptr;

  /** @brief New lifecycle state for the source node. */
  DirtySourceLifecycleState lifecycle = DirtySourceLifecycleState::Idle;

  /**
   * @brief Optional authoritative Region for a V-4 lifecycle event.
   * @note When null and source_roi is non-null, the builder resolves the
   * node's HP data window and performs one checked storage-to-logical
   * conversion at the current edge.
   */
  const RegionSet* source_region = nullptr;
};

/**
 * @brief Domain-local dirty work to append for one graph node.
 *
 * @note node must point to the GraphModel node identified by node_id and must
 * remain valid for append_node_work. PixelRect uses zero-based storage while
 * data_window supplies the logical metadata origin.
 */
struct DirtyNodeWorkRecord {
  /** @brief Node whose execution boundary determines record shape. */
  const Node* node = nullptr;

  /** @brief Graph node id stored in snapshot records. */
  int node_id = -1;

  /** @brief Dirty domain for the records. */
  DirtyDomain domain = DirtyDomain::HighPrecision;

  /** @brief Zero-based domain-local storage ROI to record. */
  PixelRect work_roi;

  /** @brief Logical data window used to translate work_roi metadata. */
  ImageBounds data_window;

  /** @brief Domain-local tile size for tiled records. */
  int tile_size = 0;
};

/**
 * @brief Tile enumeration request for one domain-local dirty ROI.
 *
 * @note The request is value-only and can be logged or inspected without
 * graph/runtime state. When nonempty, ROI is zero-based and contained by
 * data_window storage; data_window supplies its logical metadata origin.
 */
struct DirtyTileEnumeration {
  /** @brief Node id associated with emitted tile keys. */
  int node_id = -1;

  /** @brief Dirty domain for emitted tile records. */
  DirtyDomain domain = DirtyDomain::HighPrecision;

  /** @brief Tile granularity level to store. */
  DirtyTileLevel level = DirtyTileLevel::Micro;

  /** @brief Contained zero-based domain-local storage ROI to tile. */
  PixelRect roi;

  /** @brief Logical data window used to translate emitted tile Regions. */
  ImageBounds data_window;

  /** @brief Domain-local tile edge length. */
  int tile_size = 0;
};

/**
 * @brief Builds derived dirty-region snapshot records from source facts.
 *
 * DirtyRegionSnapshotBuilder centralizes the snapshot-only parts of dirty
 * planning: source lifecycle mutation, dirty source ROI normalization,
 * monolithic escalation recording, and tile key materialization. Request
 * planners and dirty control lanes keep ownership of graph generation and
 * storage, while this helper keeps derived snapshot writes consistent.
 *
 * @note The builder does not execute graph work, enqueue execution tasks, or
 * own compute request state. It only mutates the DirtyRegionSnapshot instance
 * supplied by the caller.
 */
class DirtyRegionSnapshotBuilder {
 public:
  /**
   * @brief Applies one source lifecycle transition to a dirty snapshot.
   *
   * @param graph Graph used to validate source membership.
   * @param snapshot Snapshot whose source membership and lifecycle are updated.
   * @param update Source lifecycle transition to apply.
   * @throws GraphError when node_id is missing, a supplied source fact is
   * empty, or both ROI and Region are supplied.
   * @throws std::invalid_argument or std::overflow_error when a storage ROI
   * cannot be translated through the node's signed HP data window.
   * @throws std::bad_alloc when source or Region snapshot storage grows.
   * @note dirty_updating_count is recomputed from source lifecycle states after
   * the transition. Existing source membership is intentionally preserved until
   * the dirty generation settles.
   */
  void apply_source_lifecycle_event(
      const GraphModel& graph, DirtyRegionSnapshot& snapshot,
      const DirtySourceLifecycleUpdate& update) const;

  /**
   * @brief Rebuilds derived dirty work from stable source Region records.
   *
   * @param graph Graph used for extent lookup and monolithic boundary checks.
   * @param snapshot Snapshot whose derived dirty regions are replaced.
   * @param domain Dirty domain to refresh.
   * @throws GraphError from extent lookup when graph metadata is invalid.
   * @throws std::invalid_argument or std::overflow_error when a source/work
   * Region cannot cross its logical/storage boundary exactly.
   * @throws std::bad_alloc if cache, snapshot, implementation, or callback
   *         snapshot storage cannot be copied or grown.
   * @throws Any exception raised while copying a registered callback target.
   * @note Source membership, lifecycle state, and source facts are preserved.
   * Region records are authoritative; legacy ROI-only snapshots are accepted
   * as an image fallback. Only actual work, tile keys, monolithic records, and
   * edge mappings are cleared and rebuilt.
   */
  void refresh_actual_dirty_regions(const GraphModel& graph,
                                    DirtyRegionSnapshot& snapshot,
                                    DirtyDomain domain) const;

  /**
   * @brief Detects whether a node must record monolithic dirty work.
   *
   * @param node Node whose registered implementations are inspected.
   * @return True when the node has a monolithic HP implementation and no tiled
   * HP implementation.
   * @throws std::bad_alloc if the implementation snapshot or its callbacks
   *         cannot be copied.
   * @throws Any exception raised while copying a registered callback target.
   * @note This is a local snapshot escalation. Propagation may still narrow
   * downstream ROIs after this boundary.
   */
  bool is_monolithic_boundary(const Node& node) const;

  /**
   * @brief Appends domain-local dirty work records for one node.
   *
   * @param snapshot Snapshot receiving tile or monolithic records.
   * @param record Domain-local node work record to append.
   * @throws std::bad_alloc if snapshot or implementation snapshot storage
   *         allocation fails.
   * @throws std::invalid_argument or std::overflow_error when the work ROI
   * cannot be translated through record.data_window.
   * @throws Any exception raised while copying a registered callback target.
   * @note Empty work ROIs are ignored. PixelRect remains storage-relative;
   * retained logical Region metadata is translated through data_window.
   * Monolithic nodes receive one record and tiled nodes receive micro keys.
   */
  void append_node_work(DirtyRegionSnapshot& snapshot,
                        const DirtyNodeWorkRecord& record) const;

  /**
   * @brief Enumerates micro dirty tiles covering one ROI.
   *
   * @param snapshot Snapshot receiving tile keys.
   * @param request Value-only tile enumeration request.
   * @throws std::invalid_argument or std::overflow_error when the request ROI
   * is not contained by request.data_window or its bounded logical tile Region
   * cannot be translated exactly.
   * @throws std::bad_alloc if snapshot or Region storage grows.
   * @note An empty ROI or nonpositive tile_size is a no-op. Otherwise, the
   * zero-based ROI is aligned before tile keys are appended. A boundary key
   * intentionally retains its full grid-aligned pixel_roi even when that key
   * extends beyond storage; only its logical Region is clipped to
   * request.data_window before origin translation.
   */
  void enumerate_tiles(DirtyRegionSnapshot& snapshot,
                       const DirtyTileEnumeration& request) const;

 private:
  /**
   * @brief Normalizes one source ROI into the requested dirty domain.
   *
   * @param graph Graph used for HP-authoritative extent lookup.
   * @param node_id Node id owning the source ROI.
   * @param domain Dirty domain to materialize.
   * @param source_roi Zero-based HP storage ROI recorded by a lifecycle event.
   * @param hp_size_cache Shared HP extent cache for one refresh pass.
   * @return Domain-local dirty ROI, or an empty rect when no work remains.
   * @throws GraphError from extent lookup when graph metadata is invalid.
   * @note The current lifecycle path first clips source ROIs in HP storage,
   * then projects RT snapshots down to proxy storage. This preserves existing
   * dirty source semantics independently of logical data-window origin.
   */
  PixelRect normalize_source_roi(
      const GraphModel& graph, int node_id, DirtyDomain domain,
      const PixelRect& source_roi,
      std::unordered_map<int, PixelSize>& hp_size_cache) const;

  /**
   * @brief Resolves the HP-authoritative output extent for one node.
   *
   * @param graph Graph whose output extent is queried.
   * @param node_id Node id to resolve.
   * @param cache Mutable memoization cache shared by one refresh pass.
   * @return HP output extent, or an empty size when no extent can be inferred.
   * @throws GraphError from GraphExtentResolver on invalid graph metadata.
   * @note RT source snapshots still derive from HP-authoritative extents.
   */
  PixelSize infer_hp_size(const GraphModel& graph, int node_id,
                          std::unordered_map<int, PixelSize>& cache) const;

  /**
   * @brief Returns the micro tile size for a dirty domain.
   *
   * @param domain Dirty domain being materialized.
   * @return HP or RT micro tile size in domain-local pixels.
   * @throws Nothing.
   */
  int tile_size_for_domain(DirtyDomain domain) const;

  /** @brief Dependency-neutral HP extent resolver used during normalization. */
  GraphExtentResolver extent_resolver_;
};

}  // namespace ps::compute
