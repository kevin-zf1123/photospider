#include "compute/dirty/node_executor.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "compute/compute_geometry.hpp"
#include "core/ops.hpp"
#include "core/param_utils.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Finds the first connected image input without changing slot order.
 * @param inputs Destination-indexed input pointers that may contain nulls.
 * @return First non-null output pointer, or nullptr when all slots are
 * disconnected.
 * @throws std::invalid_argument when a connected input lacks an image-faceted
 * canonical Value or uses an element type outside the current tiled ABI.
 * @throws std::overflow_error when image bounds cannot be represented.
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
 * @brief Multiplies tiled output byte components with overflow checking.
 * @param left First component.
 * @param right Second component.
 * @return Exact product.
 * @throws std::overflow_error when the product exceeds size_t.
 */
std::size_t checked_output_multiply(std::size_t left, std::size_t right) {
  if (right != 0U && left > std::numeric_limits<std::size_t>::max() / right) {
    throw std::overflow_error("Tiled output byte multiplication overflowed.");
  }
  return left * right;
}

/**
 * @brief Adds tiled output byte components with overflow checking.
 * @param left First component.
 * @param right Second component.
 * @return Exact sum.
 * @throws std::overflow_error when the sum exceeds size_t.
 */
std::size_t checked_output_add(std::size_t left, std::size_t right) {
  if (left > std::numeric_limits<std::size_t>::max() - right) {
    throw std::overflow_error("Tiled output byte addition overflowed.");
  }
  return left + right;
}

/**
 * @brief Adds a non-negative tiled coordinate to a signed data-window origin.
 * @param origin Signed immutable window origin.
 * @param offset Non-negative pixel offset.
 * @return Exact translated coordinate.
 * @throws std::overflow_error when translation exceeds int64.
 */
std::int64_t checked_image_coordinate(std::int64_t origin, int offset) {
  if (offset < 0 || origin > std::numeric_limits<std::int64_t>::max() -
                                 static_cast<std::int64_t>(offset)) {
    throw std::overflow_error("Tiled image coordinate overflowed int64.");
  }
  return origin + static_cast<std::int64_t>(offset);
}

/**
 * @brief Immutable primary allocation facts extracted without payload access.
 * @throws std::bad_alloc when copied ImageFacet storage cannot allocate.
 * @note The record owns no output interpretation decision, physical layout,
 * allocation, revision, readiness, or callback lifetime.
 */
struct TiledPrimaryFormat final {
  /** @brief Exact logical scalar semantics. */
  ElementSemantics semantics = ElementSemantics::FloatingPoint;

  /** @brief Exact whole-byte native scalar encoding. */
  StorageEncoding encoding{32U};

  /** @brief Positive output channel count. */
  std::size_t channels = 1U;

  /** @brief Complete primary interpretation available for explicit policy. */
  std::optional<ImageFacet> source_facet;
};

/**
 * @brief Extracts scalar, channel, and optional image facts from the first
 * connected input.
 *
 * @param inputs Exact operation inputs in destination-index order.
 * @return Valid primary facts, or FP32 one-channel defaults when disconnected.
 * @throws std::invalid_argument for unsupported representation, storage,
 * channel count, or missing ordinary-image metadata.
 * @throws std::bad_alloc when ImageFacet copying allocates.
 * @note The first connected slot is an allocation fallback only. No optional
 * output fact is authorized until a selected inference policy explicitly
 * projects source_facet.
 */
TiledPrimaryFormat primary_output_format(
    const std::vector<const NodeOutput*>& inputs) {
  const NodeOutput* input = first_connected_input(inputs);
  if (input == nullptr) {
    return TiledPrimaryFormat{};
  }
  if (!input->has_image_value() ||
      !input->image_value().image_facet().has_value()) {
    throw std::invalid_argument(
        "Tiled format inference requires a canonical image Value.");
  }
  const Value& value = input->image_value();
  if (value.representation_kind() != ValueRepresentationKind::DenseTensor ||
      value.storage_layout_kind() != StorageLayoutKind::Strided) {
    throw std::invalid_argument(
        "Tiled format inference requires a Strided DenseTensor Value.");
  }
  const DenseTensorDescriptor& descriptor = value.dense_tensor_descriptor();
  if (descriptor.quantization.has_value() ||
      descriptor.storage_encoding.kind != StorageEncodingKind::NativeScalar) {
    throw std::invalid_argument(
        "Tiled format inference requires unquantized native storage.");
  }
  static_cast<void>(dense_tensor_element_bytes(descriptor));
  const ImageFacet& facet = *value.image_facet();
  const std::size_t channels = facet.channel_axis.has_value()
                                   ? descriptor.shape[*facet.channel_axis]
                                   : 1U;
  if (channels == 0U || channels > kMaximumImageChannels) {
    throw std::invalid_argument("Tiled input channel count exceeds its bound.");
  }
  return TiledPrimaryFormat{descriptor.element_semantics,
                            descriptor.storage_encoding, channels, facet};
}

/**
 * @brief Builds one HWC logical inference from primary allocation facts.
 *
 * @param output_size Positive planned extent.
 * @param format Valid primary allocation facts.
 * @param preserve_interpretation Whether the complete primary ImageFacet is
 * operation-proven and may be projected.
 * @return Complete logical descriptor and conservative or preserved facet.
 * @throws std::invalid_argument for invalid extents or channel counts.
 * @throws std::overflow_error when a preserved signed endpoint overflows.
 * @throws std::bad_alloc when descriptor or facet storage cannot allocate.
 * @note A conservative result keeps only allocation type/channel facts and a
 * zero-origin required data window. It omits display, channel schema, sample,
 * and color authority rather than treating the primary as semantic truth.
 */
TiledOutputInferenceResult make_primary_output_inference(
    const PixelSize& output_size, TiledPrimaryFormat format,
    bool preserve_interpretation) {
  if (output_size.width <= 0 || output_size.height <= 0 ||
      format.channels == 0U || format.channels > kMaximumImageChannels) {
    throw std::invalid_argument(
        "Tiled output inference requires positive image dimensions.");
  }
  DenseTensorDescriptor descriptor{
      {static_cast<std::size_t>(output_size.height),
       static_cast<std::size_t>(output_size.width), format.channels},
      format.semantics,
      format.encoding};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  if (preserve_interpretation) {
    if (!format.source_facet.has_value()) {
      throw std::invalid_argument(
          "Interpretation-preserving tiled inference requires an image input.");
    }
    facet = std::move(*format.source_facet);
    facet.x_axis = 1U;
    facet.y_axis = 0U;
    facet.channel_axis = 2U;
    facet.data_window.x_end =
        checked_image_coordinate(facet.data_window.x_begin, output_size.width);
    facet.data_window.y_end =
        checked_image_coordinate(facet.data_window.y_begin, output_size.height);
  }
  validate_dense_tensor_image_metadata(descriptor, facet);
  return TiledOutputInferenceResult{std::move(descriptor), std::move(facet)};
}

/**
 * @brief Freezes the current zero-origin interleaved tiled output plan.
 * @param output_size Positive planned image extent.
 * @param inference Complete logical result frozen by selected metadata
 * inference or the conservative fallback.
 * @return Complete immutable 64-byte-aligned Host output plan.
 * @throws std::invalid_argument for invalid extents/type or unrepresentable
 * signed strides.
 * @throws std::overflow_error when row/envelope arithmetic overflows.
 * @throws std::bad_alloc when descriptor, layout, or Region storage allocates.
 * @note The plan is created before Host allocation or tiled callback entry and
 * cannot be replaced by callback-returned metadata.
 */
DenseImageOutputPlan freeze_tiled_output_plan_impl(
    const PixelSize& output_size, TiledOutputInferenceResult inference) {
  if (output_size.width <= 0 || output_size.height <= 0) {
    throw std::invalid_argument(
        "Tiled output plan requires positive image dimensions.");
  }
  DenseTensorDescriptor descriptor = std::move(inference.descriptor);
  ImageFacet facet = std::move(inference.image_facet);
  if (descriptor.shape.size() != 3U ||
      descriptor.shape[0] != static_cast<std::size_t>(output_size.height) ||
      descriptor.shape[1] != static_cast<std::size_t>(output_size.width) ||
      descriptor.shape[2] == 0U ||
      descriptor.shape[2] > kMaximumImageChannels || facet.y_axis != 0U ||
      facet.x_axis != 1U ||
      facet.channel_axis != std::optional<std::size_t>(2U)) {
    throw std::invalid_argument(
        "Tiled output inference must provide one exact HWC descriptor.");
  }
  if (descriptor.quantization.has_value() ||
      descriptor.storage_encoding.kind != StorageEncodingKind::NativeScalar) {
    throw std::invalid_argument(
        "Tiled output inference requires unquantized native storage.");
  }
  validate_dense_tensor_image_metadata(descriptor, facet);
  const std::size_t channels = descriptor.shape[2];
  const std::size_t element_bytes = dense_tensor_element_bytes(descriptor);
  const std::size_t pixel_bytes =
      checked_output_multiply(channels, element_bytes);
  const std::size_t row_bytes = checked_output_multiply(
      static_cast<std::size_t>(output_size.width), pixel_bytes);
  constexpr std::size_t kAlignment = 64U;
  const std::size_t padded = checked_output_add(row_bytes, kAlignment - 1U);
  const std::size_t row_stride = padded & ~(kAlignment - 1U);
  if (element_bytes > static_cast<std::size_t>(
                          std::numeric_limits<std::ptrdiff_t>::max()) ||
      pixel_bytes > static_cast<std::size_t>(
                        std::numeric_limits<std::ptrdiff_t>::max()) ||
      row_stride > static_cast<std::size_t>(
                       std::numeric_limits<std::ptrdiff_t>::max())) {
    throw std::invalid_argument("Tiled output plan stride exceeds ptrdiff_t.");
  }
  const std::size_t storage_size = checked_output_add(
      checked_output_multiply(static_cast<std::size_t>(output_size.height - 1),
                              row_stride),
      row_bytes);
  StridedLayout layout{{static_cast<std::ptrdiff_t>(row_stride),
                        static_cast<std::ptrdiff_t>(pixel_bytes),
                        static_cast<std::ptrdiff_t>(element_bytes)}};
  return DenseImageOutputPlan::create(
      std::string(NodeOutput::kImageOutputName), std::move(descriptor),
      std::move(facet), std::move(layout), storage_size, kAlignment);
}

/**
 * @brief Infers the full output image size for a tiled node invocation.
 *
 * @param node Node whose runtime width/height parameters are used for
 * generators.
 * @param inputs Normalized tiled inputs in execution order.
 * @param config Optional execution size override.
 * @return Output size for allocation or existing-buffer iteration.
 * @throws std::invalid_argument when canonical image bounds exceed PixelSize.
 * @throws std::overflow_error when immutable data-window arithmetic overflows.
 * @note The first connected normalized input remains the size source when no
 * explicit output_size is supplied; null slots remain visible to callbacks.
 */
PixelSize infer_output_size(const Node& node,
                            const std::vector<const NodeOutput*>& inputs,
                            const TiledExecutionConfig& config) {
  if (config.output_size)
    return *config.output_size;
  if (const NodeOutput* input = first_connected_input(inputs)) {
    if (!input->has_image_value() ||
        !input->image_value().image_facet().has_value()) {
      throw std::invalid_argument(
          "Tiled size inference requires a canonical image Value.");
    }
    const ImageBounds& bounds = input->image_value().image_bounds();
    const std::size_t width = image_bounds_width(bounds);
    const std::size_t height = image_bounds_height(bounds);
    const std::size_t maximum_extent =
        static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (width > maximum_extent || height > maximum_extent) {
      throw std::invalid_argument("Tiled input extent exceeds PixelSize.");
    }
    return PixelSize{static_cast<int>(width), static_cast<int>(height)};
  }
  return PixelSize{as_int_flexible(node.runtime_parameters, "width", 256),
                   as_int_flexible(node.runtime_parameters, "height", 256)};
}

/**
 * @brief Detects built-in gaussian blur operators that need implicit halo.
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
 * @throws std::invalid_argument or std::overflow_error when canonical image
 * metadata is absent or exceeds PixelSize.
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
    if (input == nullptr) {
      extents.emplace_back();
      continue;
    }
    if (!input->has_image_value() ||
        !input->image_value().image_facet().has_value()) {
      throw std::invalid_argument(
          "Tiled input extent requires a canonical image Value.");
    }
    const ImageBounds& bounds = input->image_value().image_bounds();
    const std::size_t width = image_bounds_width(bounds);
    const std::size_t height = image_bounds_height(bounds);
    const std::size_t maximum_extent =
        static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (width > maximum_extent || height > maximum_extent) {
      throw std::invalid_argument("Tiled input extent exceeds PixelSize.");
    }
    extents.push_back(
        PixelSize{static_cast<int>(width), static_cast<int>(height)});
  }
  return extents;
}

/**
 * @brief Resolves the output extent fixed by a Host output binding.
 *
 * @param binding Destination binding passed by the caller.
 * @param config Optional output_size override.
 * @return Size used for tile grid iteration.
 * @throws std::invalid_argument when plan extents exceed PixelSize or a caller
 * override disagrees with the frozen plan.
 * @note Dirty HP/RT paths pass output_size as an assertion of their planned
 * domain; it can never resize or reinterpret the binding.
 */
PixelSize output_size_for_binding(const HostOutputBinding& binding,
                                  const TiledExecutionConfig& config) {
  const DenseImageOutputPlan& plan = binding.plan();
  const std::size_t maximum_extent =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  if (plan.width() > maximum_extent || plan.height() > maximum_extent) {
    throw std::invalid_argument("Tiled output plan exceeds PixelSize.");
  }
  const PixelSize planned{static_cast<int>(plan.width()),
                          static_cast<int>(plan.height())};
  if (config.output_size.has_value() &&
      (config.output_size->width != planned.width ||
       config.output_size->height != planned.height)) {
    throw std::invalid_argument(
        "Tiled execution size disagrees with frozen output plan.");
  }
  return planned;
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
  for (std::size_t input_index = 0U; input_index < input_context.inputs.size();
       ++input_index) {
    const NodeOutput* input = input_context.inputs[input_index];
    if (!input) {
      input_tiles->emplace_back();
      continue;
    }
    input_tiles->push_back(
        InputTile{&input->image_value(),
                  NodeExecutor::input_roi_for_tile(
                      graph, node, output_roi, input_extents.at(input_index),
                      config, input_extents, nullptr, &input_context.inputs),
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
 * @param output_binding Destination Host binding whose plan is frozen.
 * @param config Tiled execution controls.
 * @throws GraphError for invalid tile size or propagated operation failures.
 * @note This helper is shared by execute() and
 * execute_tiled_into_binding() so normalization happens once per node
 * invocation. Each callback receives one exact tile grant; any exception
 * fails the shared binding before propagating.
 */
void execute_tiled_context_into(GraphModel& graph, Node& node,
                                const TileOpFunc& tiled_op,
                                const TiledInputContext& input_context,
                                HostOutputBinding& output_binding,
                                const TiledExecutionConfig& config) {
  validate_tile_size(config);
  const PixelSize output_size = output_size_for_binding(output_binding, config);
  const PixelRect work_roi = clipped_work_roi(output_size, config);
  const std::vector<PixelSize> input_extents =
      actual_input_extents(input_context);
  TiledExecutionConfig roi_mapping_config = config;
  roi_mapping_config.output_size = output_size;

  TileTask task;
  task.node = &node;
  task.output_tile.plan = &output_binding.plan();
  task.input_tiles.reserve(input_context.inputs.size());

  try {
    const std::int64_t work_right =
        static_cast<std::int64_t>(work_roi.x) + work_roi.width;
    const std::int64_t work_bottom =
        static_cast<std::int64_t>(work_roi.y) + work_roi.height;
    for (std::int64_t y = work_roi.y; y < work_bottom; y += config.tile_size) {
      for (std::int64_t x = work_roi.x; x < work_right; x += config.tile_size) {
        const PixelRect output_roi =
            make_output_tile_roi(static_cast<int>(x), static_cast<int>(y),
                                 work_roi, output_size, config.tile_size);
        if (is_rect_empty(output_roi)) {
          continue;
        }
        populate_input_tiles(graph, node, input_context, output_roi,
                             roi_mapping_config, input_extents,
                             &task.input_tiles);
        if (config.on_tile) {
          config.on_tile(output_roi);
        }
        const ImageBounds& bounds =
            output_binding.plan().image_facet().data_window;
        const std::int64_t x_begin =
            checked_image_coordinate(bounds.x_begin, output_roi.x);
        const std::int64_t x_end =
            checked_image_coordinate(x_begin, output_roi.width);
        const std::int64_t y_begin =
            checked_image_coordinate(bounds.y_begin, output_roi.y);
        const std::int64_t y_end =
            checked_image_coordinate(y_begin, output_roi.height);
        HostOutputWriteGrant grant = output_binding.grant_tile(
            ImageRect{image_region_domain(), x_begin, x_end, y_begin, y_end});
        task.output_tile.grant = &grant;
        task.output_tile.roi = output_roi;
        try {
          NodeExecutor::execute_tile_task(task, tiled_op);
          grant.retire_success();
        } catch (...) {
          if (grant.active()) {
            grant.retire_failure("Tiled producer callback failed.");
          }
          throw;
        }
      }
    }
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    try {
      output_binding.cancel("Tiled execution failed or was cancelled.");
    } catch (...) {
    }
    std::rethrow_exception(failure);
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
            if (config.output_region.has_value()) {
              if (auto region_operation =
                      ops::find_core_region_monolithic_operation(
                          node.type, node.subtype, op_func)) {
                return (*region_operation)(node, inputs, *config.output_region);
              }
            }
            return op_func(node, inputs);
          } else {
            TiledInputContext input_context =
                TiledInputNormalizer::normalize(node, inputs);
            require_tiled_inputs(node, input_context);

            const PixelSize output_size =
                infer_output_size(node, input_context.inputs, config);
            HostOutputBinding output_binding = HostOutputBinding::allocate(
                NodeExecutor::freeze_tiled_output_plan(
                    node, input_context.inputs, output_size,
                    config.tiled_output_inference));
            NodeOutput output;
            execute_tiled_context_into(graph, node, op_func, input_context,
                                       output_binding, config);
            output.publish_image_value(output_binding.seal());
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

void NodeExecutor::execute_tiled_into_binding(
    GraphModel& graph, Node& node, const TileOpFunc& tiled_op,
    const std::vector<const NodeOutput*>& inputs,
    HostOutputBinding& output_binding, const TiledExecutionConfig& config) {
  TiledInputContext input_context =
      TiledInputNormalizer::normalize(node, inputs);
  require_tiled_inputs(node, input_context);
  execute_tiled_context_into(graph, node, tiled_op, input_context,
                             output_binding, config);
}

/** @copydoc NodeExecutor::freeze_tiled_output_plan */
DenseImageOutputPlan NodeExecutor::freeze_tiled_output_plan(
    const Node& node, const std::vector<const NodeOutput*>& inputs,
    const PixelSize& output_size,
    const std::optional<TiledOutputInferenceFunc>& output_inference) {
  TiledOutputInferenceResult inference =
      output_inference.has_value()
          ? (*output_inference)(node, inputs, output_size)
          : make_primary_output_inference(output_size,
                                          primary_output_format(inputs), false);
  return freeze_tiled_output_plan_impl(output_size, std::move(inference));
}

/** @copydoc NodeExecutor::allocate_tiled_output_binding */
HostOutputBinding NodeExecutor::allocate_tiled_output_binding(
    const Node& node, const std::vector<const NodeOutput*>& inputs,
    const PixelSize& output_size,
    const std::optional<TiledOutputInferenceFunc>& output_inference) {
  return HostOutputBinding::allocate(
      freeze_tiled_output_plan(node, inputs, output_size, output_inference));
}

/** @copydoc NodeExecutor::infer_interpretation_preserving_output */
TiledOutputInferenceResult NodeExecutor::infer_interpretation_preserving_output(
    const Node& node, const std::vector<const NodeOutput*>& inputs,
    const PixelSize& output_size) {
  static_cast<void>(node);
  return make_primary_output_inference(output_size,
                                       primary_output_format(inputs), true);
}

/** @copydoc NodeExecutor::input_roi_for_tile */
PixelRect NodeExecutor::input_roi_for_tile(
    GraphModel& graph, const Node& node, const PixelRect& output_roi,
    const PixelSize& input_size, const TiledExecutionConfig& config,
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
    DirtyRoiPropFunc prop_fn;
    if (config.dirty_propagator.has_value()) {
      prop_fn = *config.dirty_propagator;
    } else if (config.implementation_identity != 0U) {
      prop_fn = [](const Node&, const PixelRect& roi, const GraphModel&,
                   const PixelSize&, const std::vector<PixelSize>&,
                   const plugin::ParameterMap&,
                   const std::vector<const NodeOutput*>*) { return roi; };
    } else {
      prop_fn =
          OpRegistry::instance().get_dirty_propagator(node.type, node.subtype);
    }
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
      input_extents.front() = input_size;
    }
    if (!known_effective_parameters) {
      known_effective_parameters = node.runtime_parameters.empty()
                                       ? &node.parameters
                                       : &node.runtime_parameters;
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
