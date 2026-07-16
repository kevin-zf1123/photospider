#pragma once

#include <cstddef>
#include <opencv2/core.hpp>
#include <optional>
#include <unordered_map>

#include "graph/graph_extent_resolver.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

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
  cv::Rect shared_roi;
  /** @brief Input index selected by the dependency table, when present. */
  std::optional<std::size_t> dependency_input_index;
  /** @brief LUT-derived ROI applied only to dependency_input_index. */
  cv::Rect dependency_roi;

  /**
   * @brief Returns demand routed to one destination image-input index.
   * @param input_index Destination input index from graph topology.
   * @return Shared ROI merged with dependency_roi only for its selected input.
   * @throws Nothing.
   */
  cv::Rect roi_for_input(std::size_t input_index) const noexcept;

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
  cv::Rect roi_for_input(std::size_t input_index,
                         const cv::Size& input_extent) const noexcept;

  /**
   * @brief Returns a conservative union for legacy single-ROI callers.
   * @return Union of shared and dependency contributions.
   * @throws Nothing.
   * @note Graph traversal must prefer roi_for_input() to preserve LUT routing.
   */
  cv::Rect combined_roi() const noexcept;
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
 * @note The service does not own graph topology, dirty snapshots, scheduler
 * queues, or compute task state. Callers provide graph state and request-local
 * size caches when needed.
 */
class RoiPropagationService {
 public:
  /**
   * @brief Computes shared and input-selected upstream ROI contributions.
   * @param node Node whose input demand is being computed.
   * @param downstream_roi ROI in node output coordinates.
   * @param graph Graph supplying topology, caches, and extent context.
   * @param size_cache Request-local output extent cache.
   * @return Projection retaining dependency input-index routing.
   * @throws GraphError or callback exceptions from extent/propagation logic.
   * @note The effective parameter snapshot and all input extents are resolved
   *       once and shared by dirty and dependency callbacks for this request.
   */
  UpstreamRoiProjection compute_upstream_projection(
      const Node& node, const cv::Rect& downstream_roi, const GraphModel& graph,
      std::unordered_map<int, cv::Size>& size_cache) const;

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
