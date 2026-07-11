#include "compute/node_executor.hpp"

#include <algorithm>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "compute/compute_geometry.hpp"
#include "core/param_utils.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Infers the output channel count and data type for tiled allocation.
 *
 * @param inputs Normalized tiled inputs in execution order.
 * @return Channel count and DataType copied from the first input, or the legacy
 * FLOAT32 single-channel default for generator-style nodes.
 * @throws Nothing.
 * @note This preserves the pre-refactor allocation rule and intentionally does
 * not inspect secondary inputs.
 */
std::pair<int, DataType> infer_channels_and_type(
    const std::vector<const NodeOutput*>& inputs) {
  if (inputs.empty())
    return {1, DataType::FLOAT32};
  const auto& input = inputs.front()->image_buffer;
  return {input.channels, input.type};
}

/**
 * @brief Infers the full output image size for a tiled node invocation.
 *
 * @param node Node whose runtime width/height parameters are used for
 * generators.
 * @param inputs Normalized tiled inputs in execution order.
 * @param config Optional execution size override.
 * @return Output size for allocation or existing-buffer iteration.
 * @throws YAML conversion exceptions when generator width/height parameters are
 * present but invalid.
 * @note The first normalized input remains the size source when no explicit
 * output_size is supplied.
 */
cv::Size infer_output_size(const Node& node,
                           const std::vector<const NodeOutput*>& inputs,
                           const TiledExecutionConfig& config) {
  if (config.output_size)
    return *config.output_size;
  if (!inputs.empty()) {
    const auto& input = inputs.front()->image_buffer;
    return cv::Size(input.width, input.height);
  }
  return cv::Size(as_int_flexible(node.runtime_parameters, "width", 256),
                  as_int_flexible(node.runtime_parameters, "height", 256));
}

/**
 * @brief Detects legacy gaussian blur operators that need implicit halo.
 *
 * @param node Node whose type/subtype identify the operation.
 * @return True for image_process gaussian_blur variants.
 * @throws Nothing.
 * @note The metadata path can override this through forced_halo.
 */
bool needs_gaussian_halo(const Node& node) {
  return node.type == "image_process" &&
         node.subtype.find("gaussian_blur") != std::string::npos;
}

/**
 * @brief Re-throws an operation exception with node identity context.
 *
 * @param node Node whose operator failed.
 * @param e Original exception.
 * @throws GraphError always.
 * @note GraphError exceptions are propagated before this helper is used.
 */
[[noreturn]] void wrap_node_exception(const Node& node,
                                      const std::exception& e) {
  throw GraphError(GraphErrc::ComputeError,
                   "Node " + std::to_string(node.id) + " (" + node.name +
                       ") failed: " + std::string(e.what()));
}

/**
 * @brief Ensures a tiled operator has the inputs required by its node type.
 *
 * @param node Node being executed.
 * @param input_context Normalized input context.
 * @throws GraphError when a non-generator tiled node has no image input.
 * @note image_generator nodes intentionally allow an empty input list.
 */
void require_tiled_inputs(const Node& node,
                          const TiledInputContext& input_context) {
  if (input_context.inputs.empty() && node.type != "image_generator") {
    throw GraphError(
        GraphErrc::MissingDependency,
        "Tiled node '" + node.name + "' requires at least one image input");
  }
}

/**
 * @brief Resolves the output extent used by an existing tiled destination.
 *
 * @param output_buffer Destination buffer passed by the caller.
 * @param config Optional output_size override.
 * @return Size used for tile grid iteration.
 * @throws Nothing.
 * @note Dirty HP/RT paths pass output_size so iteration can use planned domain
 * extents even when the destination buffer was just allocated.
 */
cv::Size output_size_for_buffer(const ImageBuffer& output_buffer,
                                const TiledExecutionConfig& config) {
  return config.output_size
             ? *config.output_size
             : cv::Size(output_buffer.width, output_buffer.height);
}

/**
 * @brief Clips the configured output ROI to the full output extent.
 *
 * @param output_size Full output extent for this tiled invocation.
 * @param config Optional output_roi.
 * @return Work ROI that tile iteration should cover.
 * @throws Nothing.
 * @note Missing output_roi means the whole output image is recomputed.
 */
cv::Rect clipped_work_roi(const cv::Size& output_size,
                          const TiledExecutionConfig& config) {
  const cv::Rect full_roi(0, 0, output_size.width, output_size.height);
  return config.output_roi ? clip_rect(*config.output_roi, output_size)
                           : full_roi;
}

/**
 * @brief Computes and clips one output tile ROI inside a work region.
 *
 * @param x Tile start x coordinate.
 * @param y Tile start y coordinate.
 * @param work_roi Clipped work region being tiled.
 * @param output_size Full output extent.
 * @param tile_size Nominal tile size.
 * @return Output tile ROI clipped to output_size.
 * @throws Nothing.
 * @note Edge tiles are shortened to the remaining work region size.
 */
cv::Rect make_output_tile_roi(int x, int y, const cv::Rect& work_roi,
                              const cv::Size& output_size, int tile_size) {
  const int width = std::min(tile_size, work_roi.x + work_roi.width - x);
  const int height = std::min(tile_size, work_roi.y + work_roi.height - y);
  return clip_rect(cv::Rect(x, y, width, height), output_size);
}

/**
 * @brief Ensures tile iteration cannot enter a non-advancing loop.
 *
 * @param config Tiled execution config supplied by the caller.
 * @throws GraphError when tile_size is not positive.
 * @note Previous callers used positive defaults; this guard makes the executor
 * boundary explicit without changing valid execution.
 */
void validate_tile_size(const TiledExecutionConfig& config) {
  if (config.tile_size <= 0) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Tiled execution requires a positive tile_size.");
  }
}

/**
 * @brief Rebuilds read-only input tile views for one output tile.
 *
 * @param graph Graph used for random-access ROI propagation.
 * @param node Node whose operator is being executed.
 * @param input_context Normalized tiled inputs.
 * @param output_roi Output tile ROI being computed.
 * @param config Tiled execution metadata and halo controls.
 * @param input_tiles Destination vector reused across tile callbacks.
 * @throws GraphError when a normalized input pointer is unexpectedly null.
 * @note The vector capacity is retained between calls to avoid per-tile
 * allocation churn.
 */
void populate_input_tiles(GraphModel& graph, const Node& node,
                          const TiledInputContext& input_context,
                          const cv::Rect& output_roi,
                          const TiledExecutionConfig& config,
                          std::vector<InputTile>* input_tiles) {
  input_tiles->clear();
  input_tiles->reserve(input_context.inputs.size());
  for (const auto* input : input_context.inputs) {
    if (!input) {
      throw GraphError(
          GraphErrc::MissingDependency,
          "Tiled node '" + node.name + "' received a missing image input");
    }
    const ImageBuffer& input_buffer = input->image_buffer;
    input_tiles->push_back(InputTile{
        &input_buffer, NodeExecutor::input_roi_for_tile(graph, node, output_roi,
                                                        input_buffer, config)});
  }
}

/**
 * @brief Runs tiled execution using an already normalized input context.
 *
 * @param graph Graph used for ROI propagation.
 * @param node Node whose tiled operator is executed.
 * @param tiled_op Tiled operator callback.
 * @param input_context Normalized input context that must outlive callbacks.
 * @param output_buffer Destination image buffer.
 * @param config Tiled execution controls.
 * @throws GraphError for invalid tile size or propagated operation failures.
 * @note This helper is shared by execute() and execute_tiled_into() so
 * normalization happens once per node invocation.
 */
void execute_tiled_context_into(GraphModel& graph, Node& node,
                                const TileOpFunc& tiled_op,
                                const TiledInputContext& input_context,
                                ImageBuffer& output_buffer,
                                const TiledExecutionConfig& config) {
  validate_tile_size(config);
  const cv::Size output_size = output_size_for_buffer(output_buffer, config);
  const cv::Rect work_roi = clipped_work_roi(output_size, config);

  TileTask task;
  task.node = &node;
  task.output_tile.buffer = &output_buffer;
  task.input_tiles.reserve(input_context.inputs.size());

  for (int y = work_roi.y; y < work_roi.y + work_roi.height;
       y += config.tile_size) {
    for (int x = work_roi.x; x < work_roi.x + work_roi.width;
         x += config.tile_size) {
      task.output_tile.roi =
          make_output_tile_roi(x, y, work_roi, output_size, config.tile_size);
      if (is_rect_empty(task.output_tile.roi)) {
        continue;
      }
      populate_input_tiles(graph, node, input_context, task.output_tile.roi,
                           config, &task.input_tiles);
      if (config.on_tile) {
        config.on_tile(task.output_tile.roi);
      }
      NodeExecutor::execute_tile_task(task, tiled_op);
    }
  }
}

}  // namespace

/**
 * @brief Executes one monolithic or tiled operation variant for a node.
 *
 * @param graph Graph used for random-access ROI propagation.
 * @param node Node whose runtime parameters and identity drive execution.
 * @param op Selected operation implementation.
 * @param inputs Resolved upstream outputs in graph input order.
 * @param config Tiled execution controls and optional dirty clipping.
 * @return Output produced by the selected implementation.
 * @throws std::bad_alloc if normalization, allocation, or operation execution
 * exhausts memory.
 * @throws GraphError preserving graph failures and wrapping other operation
 * failures with node context.
 * @note Monolithic calls receive original inputs; tiled calls normalize input
 * views and stage output before returning it.
 */
NodeOutput NodeExecutor::execute(GraphModel& graph, Node& node,
                                 const OpRegistry::OpVariant& op,
                                 const std::vector<const NodeOutput*>& inputs,
                                 const TiledExecutionConfig& config) {
  try {
    return std::visit(
        [&](auto&& op_func) -> NodeOutput {
          using T = std::decay_t<decltype(op_func)>;
          if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
            return op_func(node, inputs);
          } else {
            TiledInputContext input_context =
                TiledInputNormalizer::normalize(node, inputs);
            require_tiled_inputs(node, input_context);

            const cv::Size output_size =
                infer_output_size(node, input_context.inputs, config);
            auto [channels, dtype] =
                infer_channels_and_type(input_context.inputs);
            NodeOutput output;
            output.image_buffer = make_aligned_cpu_image_buffer(
                output_size.width, output_size.height, channels, dtype);
            execute_tiled_context_into(graph, node, op_func, input_context,
                                       output.image_buffer, config);
            return output;
          }
        },
        op);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError&) {
    throw;
  } catch (const cv::Exception& e) {
    wrap_node_exception(node, e);
  } catch (const std::exception& e) {
    wrap_node_exception(node, e);
  } catch (...) {
    throw GraphError(GraphErrc::ComputeError,
                     "Node " + std::to_string(node.id) + " (" + node.name +
                         ") failed: unknown exception");
  }
}

void NodeExecutor::execute_tiled_into(
    GraphModel& graph, Node& node, const TileOpFunc& tiled_op,
    const std::vector<const NodeOutput*>& inputs, ImageBuffer& output_buffer,
    const TiledExecutionConfig& config) {
  TiledInputContext input_context =
      TiledInputNormalizer::normalize(node, inputs);
  require_tiled_inputs(node, input_context);
  execute_tiled_context_into(graph, node, tiled_op, input_context,
                             output_buffer, config);
}

cv::Rect NodeExecutor::input_roi_for_tile(GraphModel& graph, const Node& node,
                                          const cv::Rect& output_roi,
                                          const ImageBuffer& input_buffer,
                                          const TiledExecutionConfig& config) {
  OpMetadata meta;
  if (config.metadata) {
    meta = *config.metadata;
  } else if (auto op_meta =
                 OpRegistry::instance().get_metadata(node.type, node.subtype)) {
    meta = *op_meta;
  }

  cv::Rect input_roi;
  if (meta.access_pattern == OpMetadata::InputAccessPattern::RandomAccess) {
    auto prop_fn =
        OpRegistry::instance().get_dirty_propagator(node.type, node.subtype);
    input_roi = prop_fn(node, output_roi, graph);
  } else if (config.forced_halo.value_or(
                 needs_gaussian_halo(node) ? config.halo_size : 0) > 0) {
    input_roi =
        expand_rect(output_roi, config.forced_halo.value_or(config.halo_size));
  } else {
    input_roi = output_roi;
  }

  input_roi =
      clip_rect(input_roi, cv::Size(input_buffer.width, input_buffer.height));
  if (is_rect_empty(input_roi)) {
    input_roi = clip_rect(output_roi,
                          cv::Size(input_buffer.width, input_buffer.height));
  }
  return input_roi;
}

void NodeExecutor::execute_tile_task(const TileTask& task,
                                     const TileOpFunc& tiled_op) {
  tiled_op(*task.node, task.output_tile, task.input_tiles);
}

}  // namespace ps::compute
