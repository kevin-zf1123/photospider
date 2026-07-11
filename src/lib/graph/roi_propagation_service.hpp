#pragma once

#include <opencv2/core.hpp>
#include <optional>
#include <unordered_map>

#include "graph/graph_extent_resolver.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps {

/**
 * @brief Computes operator-aware ROI propagation across graph topology.
 *
 * RoiPropagationService is the ROI/spatial propagation boundary. It consumes
 * GraphModel topology and GraphExtentResolver extents, applies operator dirty
 * and forward propagators from OpRegistry, merges spatial metadata and optional
 * dependency LUT results, and projects ROIs forward or backward through image
 * input edges.
 *
 * @note The service does not own graph topology, dirty snapshots, scheduler
 * queues, or compute task state. Callers provide graph state and request-local
 * size caches when needed.
 */
class RoiPropagationService {
 public:
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
   * @note Formal extents remain HP-authoritative; RT-only transient output is
   * not used as a propagation extent.
   */
  cv::Rect compute_upstream_roi(
      const Node& node, const cv::Rect& downstream_roi, const GraphModel& graph,
      std::unordered_map<int, cv::Size>& size_cache) const;

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
  std::optional<cv::Rect> project_roi_forward(const GraphModel& graph,
                                              int start_node_id,
                                              const cv::Rect& start_roi,
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
  std::optional<cv::Rect> project_roi_backward(const GraphModel& graph,
                                               int target_node_id,
                                               const cv::Rect& target_roi,
                                               int source_node_id) const;

 private:
  GraphExtentResolver extent_resolver_;
};

}  // namespace ps
