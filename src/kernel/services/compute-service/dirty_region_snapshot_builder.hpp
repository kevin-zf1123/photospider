#pragma once

#include <opencv2/core.hpp>
#include <unordered_map>

#include "graph_model.hpp"  // NOLINT(build/include_subdir)
#include "kernel/services/compute-service/dirty_region_snapshot.hpp"
#include "kernel/services/graph_extent_resolver.hpp"

namespace ps::compute {

/**
 * @brief Source lifecycle transition requested by a dirty control event.
 *
 * @note source_roi is null only for transitions that do not append a new ROI,
 * such as end/settled events. The pointed ROI must outlive the
 * apply_source_lifecycle_event call.
 */
struct DirtySourceLifecycleUpdate {
  /** @brief Source node id receiving the lifecycle transition. */
  int node_id = -1;

  /** @brief Dirty domain associated with the source node. */
  DirtyDomain domain = DirtyDomain::HighPrecision;

  /** @brief Optional source ROI appended for begin/update transitions. */
  const cv::Rect* source_roi = nullptr;

  /** @brief New lifecycle state for the source node. */
  DirtySourceLifecycleState lifecycle = DirtySourceLifecycleState::Idle;
};

/**
 * @brief Domain-local dirty work to append for one graph node.
 *
 * @note node must point to the GraphModel node identified by node_id and must
 * remain valid for the append_node_work call.
 */
struct DirtyNodeWorkRecord {
  /** @brief Node whose execution boundary determines record shape. */
  const Node* node = nullptr;

  /** @brief Graph node id stored in snapshot records. */
  int node_id = -1;

  /** @brief Dirty domain for the records. */
  DirtyDomain domain = DirtyDomain::HighPrecision;

  /** @brief Domain-local ROI to record. */
  cv::Rect work_roi;

  /** @brief Domain-local tile size for tiled records. */
  int tile_size = 0;
};

/**
 * @brief Tile enumeration request for one domain-local dirty ROI.
 *
 * @note The request is value-only and can be logged or inspected without
 * holding graph or scheduler state.
 */
struct DirtyTileEnumeration {
  /** @brief Node id associated with emitted tile keys. */
  int node_id = -1;

  /** @brief Dirty domain for emitted tile records. */
  DirtyDomain domain = DirtyDomain::HighPrecision;

  /** @brief Tile granularity level to store. */
  DirtyTileLevel level = DirtyTileLevel::Micro;

  /** @brief Domain-local ROI to tile. */
  cv::Rect roi;

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
 * @note The builder does not execute graph work, enqueue scheduler tasks, or
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
   * @throws GraphError when node_id is missing or source_roi is empty.
   * @note dirty_updating_count is recomputed from source lifecycle states after
   * the transition. Existing source membership is intentionally preserved until
   * the dirty generation settles.
   */
  void apply_source_lifecycle_event(
      const GraphModel& graph, DirtyRegionSnapshot& snapshot,
      const DirtySourceLifecycleUpdate& update) const;

  /**
   * @brief Rebuilds derived dirty regions from stable source ROI records.
   *
   * @param graph Graph used for extent lookup and monolithic boundary checks.
   * @param snapshot Snapshot whose derived dirty regions are replaced.
   * @param domain Dirty domain to refresh.
   * @throws GraphError from extent lookup when graph metadata is invalid.
   * @note Source membership, lifecycle state, and source ROI records are
   * preserved. Only actual ROIs, tile keys, monolithic records, and edge
   * mappings are cleared and rebuilt.
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
   * @throws Nothing directly.
   * @note This is a local snapshot escalation. Propagation may still narrow
   * downstream ROIs after this boundary.
   */
  bool is_monolithic_boundary(const Node& node) const;

  /**
   * @brief Appends domain-local dirty work records for one node.
   *
   * @param snapshot Snapshot receiving tile or monolithic records.
   * @param record Domain-local node work record to append.
   * @throws std::bad_alloc if snapshot storage grows and allocation fails.
   * @note Empty work ROIs are ignored. Monolithic nodes receive a single
   * whole-output record; tiled nodes receive aligned micro tile keys.
   */
  void append_node_work(DirtyRegionSnapshot& snapshot,
                        const DirtyNodeWorkRecord& record) const;

  /**
   * @brief Enumerates micro dirty tiles covering one ROI.
   *
   * @param snapshot Snapshot receiving tile keys.
   * @param request Value-only tile enumeration request.
   * @throws std::bad_alloc if snapshot storage grows and allocation fails.
   * @note The ROI is aligned to request.tile_size before tile keys are
   * appended.
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
   * @param source_roi Source ROI recorded by a dirty lifecycle event.
   * @param hp_size_cache Shared HP extent cache for one refresh pass.
   * @return Domain-local dirty ROI, or an empty rect when no work remains.
   * @throws GraphError from extent lookup when graph metadata is invalid.
   * @note The current lifecycle path first clips source ROIs in HP space, then
   * projects RT snapshots down to proxy space. This preserves existing dirty
   * source semantics.
   */
  cv::Rect normalize_source_roi(
      const GraphModel& graph, int node_id, DirtyDomain domain,
      const cv::Rect& source_roi,
      std::unordered_map<int, cv::Size>& hp_size_cache) const;

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
  cv::Size infer_hp_size(const GraphModel& graph, int node_id,
                         std::unordered_map<int, cv::Size>& cache) const;

  /**
   * @brief Returns the micro tile size for a dirty domain.
   *
   * @param domain Dirty domain being materialized.
   * @return HP or RT micro tile size in domain-local pixels.
   * @throws Nothing.
   */
  int tile_size_for_domain(DirtyDomain domain) const;

  GraphExtentResolver extent_resolver_;
};

}  // namespace ps::compute
