#include "compute/node_executor.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "compute/compute_geometry.hpp"
#include "core/param_utils.hpp"
#include "core/parameter_value_adapter.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Finds the first connected image input without changing slot order.
 * @param inputs Destination-indexed input pointers that may contain nulls.
 * @return First non-null output pointer, or nullptr when all slots are
 * disconnected.
 * @throws Nothing.
 * @note The helper is for format/size fallback only; callback vectors retain
 * every original slot.
 */
const NodeOutput* first_connected_input(
    const std::vector<const NodeOutput*>& inputs) noexcept {
  const auto found =
      std::find_if(inputs.begin(), inputs.end(),
                   [](const NodeOutput* input) { return input != nullptr; });
  return found == inputs.end() ? nullptr : *found;
}

/**
 * @brief Infers the output channel count and data type for tiled allocation.
 *
 * @param inputs Normalized tiled inputs in execution order.
 * @return Channel count and DataType copied from the first connected input, or
 * the legacy FLOAT32 single-channel default when every slot is disconnected.
 * @throws Nothing.
 * @note Slot identity remains unchanged; this helper only skips null entries
 * while choosing an allocation hint.
 */
std::pair<int, DataType> infer_channels_and_type(
    const std::vector<const NodeOutput*>& inputs) {
  const NodeOutput* input = first_connected_input(inputs);
  return input ? std::pair<int, DataType>{input->image_buffer.channels,
                                          input->image_buffer.type}
               : std::pair<int, DataType>{1, DataType::FLOAT32};
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
 * @note The first connected normalized input remains the size source when no
 * explicit output_size is supplied; null slots remain visible to callbacks.
 */
PixelSize infer_output_size(const Node& node,
                            const std::vector<const NodeOutput*>& inputs,
                            const TiledExecutionConfig& config) {
  if (config.output_size)
    return *config.output_size;
  if (const NodeOutput* input = first_connected_input(inputs)) {
    return PixelSize{input->image_buffer.width, input->image_buffer.height};
  }
  return PixelSize{as_int_flexible(node.runtime_parameters, "width", 256),
                   as_int_flexible(node.runtime_parameters, "height", 256)};
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
 * @throws GraphError when a non-generator tiled node has no connected image
 * input.
 * @note image_generator nodes intentionally allow an empty input list.
 */
void require_tiled_inputs(const Node& node,
                          const TiledInputContext& input_context) {
  if (!first_connected_input(input_context.inputs) &&
      node.type != "image_generator") {
    throw GraphError(
        GraphErrc::MissingDependency,
        "Tiled node '" + node.name + "' requires at least one image input");
  }
}

/**
 * @brief Captures all actual normalized image-input extents once per execution.
 * @param input_context Normalized inputs in destination-index order.
 * @return Extents supplied to every random-access callback in this execution.
 * @throws std::bad_alloc when vector storage allocation fails.
 * @note Disconnected slots contribute an empty extent. Connected values may
 * come from same-batch temporary results and intentionally do not consult
 * committed GraphModel caches.
 */
std::vector<PixelSize> actual_input_extents(
    const TiledInputContext& input_context) {
  std::vector<PixelSize> extents;
  extents.reserve(input_context.inputs.size());
  for (const NodeOutput* input : input_context.inputs) {
    extents.push_back(
        input ? PixelSize{input->image_buffer.width, input->image_buffer.height}
              : PixelSize{});
  }
  return extents;
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
PixelSize output_size_for_buffer(const ImageBuffer& output_buffer,
                                 const TiledExecutionConfig& config) {
  return config.output_size
             ? *config.output_size
             : PixelSize{output_buffer.width, output_buffer.height};
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
PixelRect clipped_work_roi(const PixelSize& output_size,
                           const TiledExecutionConfig& config) {
  const PixelRect full_roi{0, 0, output_size.width, output_size.height};
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
PixelRect make_output_tile_roi(int x, int y, const PixelRect& work_roi,
                               const PixelSize& output_size, int tile_size) {
  const std::int64_t work_right =
      static_cast<std::int64_t>(work_roi.x) + work_roi.width;
  const std::int64_t work_bottom =
      static_cast<std::int64_t>(work_roi.y) + work_roi.height;
  const std::int64_t right =
      std::min(work_right, static_cast<std::int64_t>(x) + tile_size);
  const std::int64_t bottom =
      std::min(work_bottom, static_cast<std::int64_t>(y) + tile_size);
  return clip_rect(rect_from_edges(x, y, right, bottom), output_size);
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
 * @param input_extents Actual normalized input extents captured once for the
 *        current execution.
 * @param input_tiles Destination vector reused across tile callbacks.
 * @throws std::bad_alloc when the destination vector grows.
 * @note A disconnected slot appends an empty InputTile, preserving destination
 * index identity for private and public tiled callbacks. The vector capacity
 * is retained between calls to avoid per-tile allocation churn.
 */
void populate_input_tiles(GraphModel& graph, const Node& node,
                          const TiledInputContext& input_context,
                          const PixelRect& output_roi,
                          const TiledExecutionConfig& config,
                          const std::vector<PixelSize>& input_extents,
                          std::vector<InputTile>* input_tiles) {
  input_tiles->clear();
  input_tiles->reserve(input_context.inputs.size());
  for (const auto* input : input_context.inputs) {
    if (!input) {
      input_tiles->emplace_back();
      continue;
    }
    const ImageBuffer& input_buffer = input->image_buffer;
    input_tiles->push_back(InputTile{
        &input_buffer,
        NodeExecutor::input_roi_for_tile(graph, node, output_roi, input_buffer,
                                         config, input_extents, nullptr,
                                         &input_context.inputs),
        &input->space});
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
  const PixelSize output_size = output_size_for_buffer(output_buffer, config);
  const PixelRect work_roi = clipped_work_roi(output_size, config);
  const std::vector<PixelSize> input_extents =
      actual_input_extents(input_context);
  TiledExecutionConfig roi_mapping_config = config;
  roi_mapping_config.output_size = output_size;

  TileTask task;
  task.node = &node;
  task.output_tile.buffer = &output_buffer;
  task.input_tiles.reserve(input_context.inputs.size());

  const std::int64_t work_right =
      static_cast<std::int64_t>(work_roi.x) + work_roi.width;
  const std::int64_t work_bottom =
      static_cast<std::int64_t>(work_roi.y) + work_roi.height;
  for (std::int64_t y = work_roi.y; y < work_bottom; y += config.tile_size) {
    for (std::int64_t x = work_roi.x; x < work_right; x += config.tile_size) {
      task.output_tile.roi =
          make_output_tile_roi(static_cast<int>(x), static_cast<int>(y),
                               work_roi, output_size, config.tile_size);
      if (is_rect_empty(task.output_tile.roi)) {
        continue;
      }
      populate_input_tiles(graph, node, input_context, task.output_tile.roi,
                           roi_mapping_config, input_extents,
                           &task.input_tiles);
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

            const PixelSize output_size =
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

/** @copydoc NodeExecutor::input_roi_for_tile */
PixelRect NodeExecutor::input_roi_for_tile(
    GraphModel& graph, const Node& node, const PixelRect& output_roi,
    const ImageBuffer& input_buffer, const TiledExecutionConfig& config,
    const std::vector<PixelSize>& known_input_extents,
    const plugin::ParameterMap* known_effective_parameters,
    const std::vector<const NodeOutput*>* available_inputs) {
  OpMetadata meta;
  if (config.metadata) {
    meta = *config.metadata;
  } else if (auto op_meta =
                 OpRegistry::instance().get_metadata(node.type, node.subtype)) {
    meta = *op_meta;
  }

  PixelRect input_roi;
  if (meta.access_pattern == OpMetadata::InputAccessPattern::RandomAccess) {
    auto prop_fn =
        OpRegistry::instance().get_dirty_propagator(node.type, node.subtype);
    const std::int64_t output_right =
        static_cast<std::int64_t>(output_roi.x) + output_roi.width;
    const std::int64_t output_bottom =
        static_cast<std::int64_t>(output_roi.y) + output_roi.height;
    const bool output_extent_representable =
        output_right > 0 && output_bottom > 0 &&
        output_right <= std::numeric_limits<int>::max() &&
        output_bottom <= std::numeric_limits<int>::max();
    const PixelSize inferred_output_extent =
        output_extent_representable ? PixelSize{static_cast<int>(output_right),
                                                static_cast<int>(output_bottom)}
                                    : PixelSize{};
    const PixelSize output_extent =
        config.output_size.value_or(inferred_output_extent);
    std::vector<PixelSize> input_extents =
        known_input_extents.empty() ? cached_image_input_extents(node, graph)
                                    : known_input_extents;
    if (input_extents.size() < node.image_inputs.size()) {
      input_extents.resize(node.image_inputs.size());
    }
    if (input_extents.size() == 1) {
      input_extents.front() =
          PixelSize{input_buffer.width, input_buffer.height};
    }
    plugin::ParameterMap execution_parameter_storage;
    if (!known_effective_parameters) {
      const YAML::Node& execution_parameters =
          node.runtime_parameters ? node.runtime_parameters : node.parameters;
      execution_parameter_storage =
          core::parameter_map_from_yaml(execution_parameters);
      known_effective_parameters = &execution_parameter_storage;
    }
    input_roi = prop_fn(node, output_roi, graph, output_extent, input_extents,
                        *known_effective_parameters, available_inputs);
  } else if (config.forced_halo.value_or(
                 needs_gaussian_halo(node) ? config.halo_size : 0) > 0) {
    input_roi =
        expand_rect(output_roi, config.forced_halo.value_or(config.halo_size));
  } else {
    input_roi = output_roi;
  }

  const PixelSize input_size{input_buffer.width, input_buffer.height};
  input_roi = clip_rect(input_roi, input_size);
  if (is_rect_empty(input_roi)) {
    input_roi = clip_rect(output_roi, input_size);
  }
  return input_roi;
}

void NodeExecutor::execute_tile_task(const TileTask& task,
                                     const TileOpFunc& tiled_op) {
  tiled_op(*task.node, task.output_tile, task.input_tiles);
}

}  // namespace ps::compute
