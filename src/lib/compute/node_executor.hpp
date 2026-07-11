#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "graph_model.hpp"  // NOLINT(build/include_subdir)
#include "kernel/services/compute-service/tiled_input_normalizer.hpp"
#include "ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {

/**
 * @brief Runtime controls for tiled node execution.
 *
 * TiledExecutionConfig scopes tile size, halo policy, output extent, optional
 * dirty ROI clipping, and metadata overrides for one tiled node invocation.
 *
 * @note The config is borrowed by NodeExecutor only for the duration of the
 * call. on_tile is invoked synchronously before each tile callback.
 */
struct TiledExecutionConfig {
  /** @brief Nominal square tile size in output pixels. */
  int tile_size = 256;

  /** @brief Default halo expansion used by operators that need neighbor pixels.
   */
  int halo_size = 16;

  /** @brief Optional output ROI limiting which output pixels are recomputed. */
  std::optional<cv::Rect> output_roi;

  /** @brief Optional full output size when output_buffer is not the size
   * source. */
  std::optional<cv::Size> output_size;

  /** @brief Optional halo override; zero disables implicit halo expansion. */
  std::optional<int> forced_halo;

  /** @brief Optional operator metadata override for ROI access pattern
   * handling. */
  std::optional<OpMetadata> metadata;

  /** @brief Synchronous hook called with each output tile ROI before execution.
   */
  std::function<void(const cv::Rect&)> on_tile;
};

/**
 * @brief Executes monolithic and tiled operator implementations for one node.
 *
 * NodeExecutor owns node-local execution mechanics: input normalization for
 * tiled image_mixing, output buffer allocation, tile ROI clipping, input ROI
 * mapping, and exception wrapping. Higher-level compute services still own
 * dependency resolution, cache policy, scheduling, and commit decisions.
 *
 * @note Tiled input views are read-only InputTile objects, while output views
 * are writable OutputTile objects. The executor never casts away constness from
 * upstream NodeOutput buffers.
 */
class NodeExecutor {
 public:
  /**
   * @brief Executes a selected operator variant and returns the node output.
   *
   * @param graph Graph used for random-access ROI propagation.
   * @param node Node whose runtime parameters and identity drive execution.
   * @param op Selected monolithic or tiled operator implementation.
   * @param inputs Resolved upstream image outputs in graph input order.
   * @param config Optional tiled execution controls.
   * @return NodeOutput produced by the operator.
   * @throws std::bad_alloc if input normalization, output allocation, or the
   *         selected operation exhausts memory.
   * @throws GraphError for other dependency, parameter, or compute failures.
   * @note Monolithic operators receive the original inputs. Tiled operators
   * receive normalized input views when image_mixing requires
   * resize/crop/channel conversion.
   */
  static NodeOutput execute(GraphModel& graph, Node& node,
                            const OpRegistry::OpVariant& op,
                            const std::vector<const NodeOutput*>& inputs,
                            const TiledExecutionConfig& config = {});

  /**
   * @brief Executes a tiled operator into an existing output buffer.
   *
   * @param graph Graph used for random-access ROI propagation.
   * @param node Node whose tiled operator is being executed.
   * @param tiled_op Selected tiled operator implementation.
   * @param inputs Resolved upstream image outputs in graph input order.
   * @param output_buffer Destination buffer that receives tiled output.
   * @param config Optional tiled execution controls.
   * @return Nothing.
   * @throws std::bad_alloc if normalization, allocation, or tile execution
   *         exhausts memory.
   * @throws GraphError when required inputs are missing or tile execution
   *         otherwise fails.
   * @note Used by dirty HP/RT paths that already own their destination buffers.
   */
  static void execute_tiled_into(GraphModel& graph, Node& node,
                                 const TileOpFunc& tiled_op,
                                 const std::vector<const NodeOutput*>& inputs,
                                 ImageBuffer& output_buffer,
                                 const TiledExecutionConfig& config = {});

  /**
   * @brief Maps one output tile ROI to the required input ROI.
   *
   * @param graph Graph used by random-access dirty propagators.
   * @param node Node whose operator metadata defines access behavior.
   * @param output_roi Output tile ROI being computed.
   * @param input_buffer Input buffer whose bounds clip the mapped ROI.
   * @param config Tiled execution metadata and halo overrides.
   * @return Input ROI clipped to input_buffer bounds.
   * @throws GraphError or propagator exceptions from random-access ROI mapping.
   * @note Empty propagated ROIs fall back to clipped output_roi, matching
   * legacy tiled execution behavior.
   */
  static cv::Rect input_roi_for_tile(GraphModel& graph, const Node& node,
                                     const cv::Rect& output_roi,
                                     const ImageBuffer& input_buffer,
                                     const TiledExecutionConfig& config);

  /**
   * @brief Invokes the tiled operator for a prepared tile task.
   *
   * @param task Prepared node/output/input tile views.
   * @param tiled_op Tiled operator implementation to call.
   * @throws Any exception thrown by the tiled operator.
   * @note The task does not own buffers; all referenced storage must outlive
   * the callback.
   */
  static void execute_tile_task(const TileTask& task,
                                const TileOpFunc& tiled_op);
};

}  // namespace ps::compute
