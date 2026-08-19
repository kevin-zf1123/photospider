#include "compute/dirty/tiled_input_normalizer.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/dense_image_processing.hpp"
#include "core/param_utils.hpp"
#include "graph/node.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {
namespace {

/**
 * @brief Compact canonical image shape used by mixing normalization.
 * @throws Nothing for ordinary value operations.
 * @note Scalar/sample/color metadata remains on the source Value and is never
 * projected into this routing-only record.
 */
struct ImageShape final {
  /** @brief Positive width within PixelSize bounds. */
  int width = 0;
  /** @brief Positive height within PixelSize bounds. */
  int height = 0;
  /** @brief Positive channel count within size_t. */
  std::size_t channels = 0U;
};

/**
 * @brief Reports whether a node needs secondary mixing normalization.
 * @param node Candidate destination node.
 * @param inputs Destination-indexed input pointers.
 * @return True only for image_mixing with at least two input slots.
 * @throws Nothing.
 */
bool should_normalize(const Node& node,
                      const std::vector<const NodeOutput*>& inputs) noexcept {
  return node.type == "image_mixing" && inputs.size() >= 2U;
}

/**
 * @brief Requires one input to carry a valid ordinary image Value.
 * @param output Candidate upstream output.
 * @param node_id Destination node used in diagnostics.
 * @param role Stable input role label.
 * @return Borrowed canonical image Value.
 * @throws GraphError when the input or image Value is absent.
 */
const Value& require_image(const NodeOutput* output, int node_id,
                           const char* role) {
  if (output == nullptr || !output->has_image_value()) {
    throw GraphError(GraphErrc::MissingDependency,
                     std::string(role) + " image for image_mixing node " +
                         std::to_string(node_id) + " is missing.");
  }
  return output->image_value();
}

/**
 * @brief Extracts a bounded shape from complete immutable image metadata.
 * @param value Valid ordinary image Value.
 * @return Positive PixelSize-compatible extent and channel count.
 * @throws std::invalid_argument for non-image Values.
 * @throws std::overflow_error when extent exceeds PixelSize.
 * @note This metadata-only query neither waits on readiness nor requests
 * payload access; unchanged inputs can remain on provider storage.
 */
ImageShape shape_of(const Value& value) {
  if (!value.valid() ||
      value.representation_kind() != ValueRepresentationKind::DenseTensor ||
      !value.image_facet().has_value()) {
    throw std::invalid_argument(
        "Tiled image normalization requires an ordinary image Value.");
  }
  const DenseTensorDescriptor& descriptor = value.dense_tensor_descriptor();
  const ImageFacet& facet = *value.image_facet();
  validate_dense_tensor_image_metadata(descriptor, facet);
  const std::size_t width = image_bounds_width(facet.data_window);
  const std::size_t height = image_bounds_height(facet.data_window);
  const std::size_t channels = facet.channel_axis.has_value()
                                   ? descriptor.shape[*facet.channel_axis]
                                   : 1U;
  const std::size_t maximum =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  if (width > maximum || height > maximum) {
    throw std::overflow_error(
        "Tiled image normalization extent exceeds PixelSize.");
  }
  return ImageShape{static_cast<int>(width), static_cast<int>(height),
                    channels};
}

/**
 * @brief Reports exact width/height/channel agreement.
 * @param value Candidate ordinary image Value.
 * @param required Required shape.
 * @return True only when all three logical extents match.
 * @throws Immutable metadata validation failures unchanged.
 */
bool matches_shape(const Value& value, const ImageShape& required) {
  const ImageShape current = shape_of(value);
  return current.width == required.width && current.height == required.height &&
         current.channels == required.channels;
}

/**
 * @brief Applies the explicit image_mixing size strategy.
 * @param value Secondary ordinary image Value.
 * @param required Required base shape.
 * @param strategy Exact `resize` or `crop` operation parameter.
 * @param node_id Destination node used in diagnostics.
 * @return Fresh Value with the required width and height.
 * @throws GraphError for unsupported strategy.
 * @throws Dense-image processing failures unchanged.
 */
Value normalize_size(const Value& value, const ImageShape& required,
                     const std::string& strategy, int node_id) {
  const ImageShape current = shape_of(value);
  if (current.width == required.width && current.height == required.height) {
    return value;
  }
  const PixelSize extent{required.width, required.height};
  if (strategy == "resize") {
    return dense_image_processing::resize(value, extent);
  }
  if (strategy == "crop") {
    return dense_image_processing::crop_or_pad(value, extent);
  }
  throw GraphError(GraphErrc::InvalidParameter,
                   "Unsupported merge_strategy '" + strategy +
                       "' for tiled image_mixing node " +
                       std::to_string(node_id) + ".");
}

/**
 * @brief Normalizes one secondary input into a fresh canonical Value.
 * @param input Existing upstream output.
 * @param required Required base shape.
 * @param strategy Exact size-normalization strategy.
 * @param node_id Destination node used in diagnostics.
 * @return Empty when already matching; otherwise copied output with a fresh
 * image Value and preserved parameter/spatial/debug/plugin-lifetime state.
 * @throws GraphError or dense-image processing failures unchanged.
 */
std::optional<NodeOutput> normalize_secondary(const NodeOutput* input,
                                              const ImageShape& required,
                                              const std::string& strategy,
                                              int node_id) {
  const Value& source = require_image(input, node_id, "Secondary");
  if (matches_shape(source, required)) {
    return std::nullopt;
  }
  Value normalized = normalize_size(source, required, strategy, node_id);
  const ImageShape sized = shape_of(normalized);
  if (sized.channels != required.channels) {
    normalized =
        dense_image_processing::convert_channels(normalized, required.channels);
  }
  NodeOutput output = *input;
  output.replace_image_value(std::move(normalized));
  return output;
}

/**
 * @brief Stores one normalized output and updates its stable pointer slot.
 * @param context Mutable context with pre-reserved normalized storage.
 * @param index Destination input index to replace.
 * @param output Complete temporary result.
 * @return Nothing.
 * @throws std::bad_alloc only if the caller's reservation invariant regresses.
 */
void retain_normalized(TiledInputContext* context, std::size_t index,
                       NodeOutput output) {
  context->normalized_storage.push_back(std::move(output));
  context->inputs[index] = &context->normalized_storage.back();
}

}  // namespace

/** @copydoc TiledInputNormalizer::normalize */
TiledInputContext TiledInputNormalizer::normalize(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  TiledInputContext context;
  context.inputs = inputs;
  if (!should_normalize(node, inputs)) {
    return context;
  }

  const Value& base = require_image(inputs.front(), node.id, "Base");
  const ImageShape required = shape_of(base);
  const std::string strategy =
      as_str(node.runtime_parameters, "merge_strategy", "resize");
  context.normalized_storage.reserve(inputs.size() - 1U);
  for (std::size_t index = 1U; index < inputs.size(); ++index) {
    std::optional<NodeOutput> normalized =
        normalize_secondary(inputs[index], required, strategy, node.id);
    if (normalized.has_value()) {
      retain_normalized(&context, index, std::move(*normalized));
    }
  }
  return context;
}

}  // namespace ps::compute
