#include "compute/tiled_input_normalizer.hpp"

#include <algorithm>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "adapter/buffer_adapter_opencv.hpp"
#include "core/param_utils.hpp"
#include "node.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {
namespace {

/**
 * @brief Compact shape description used for image_mixing normalization.
 *
 * The base image's width, height, and channel count are the only properties the
 * tiled image_mixing path normalizes today. Data type is intentionally
 * preserved from each source unless OpenCV channel conversion changes the Mat
 * type.
 *
 * @note This is a local value object; it does not borrow image memory.
 */
struct ImageShape {
  /** @brief Width in pixels. */
  int width = 0;

  /** @brief Height in pixels. */
  int height = 0;

  /** @brief Number of channels per pixel. */
  int channels = 0;
};

/**
 * @brief Reports whether the node needs image_mixing secondary normalization.
 *
 * @param node Node being prepared for tiled execution.
 * @param inputs Resolved image inputs.
 * @return True only for image_mixing nodes with at least two inputs.
 * @throws Nothing.
 * @note The first image is the normalization base and is never copied here.
 */
bool should_normalize_mixing_inputs(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  return node.type == "image_mixing" && inputs.size() >= 2;
}

/**
 * @brief Extracts the current shape of an ImageBuffer.
 *
 * @param buffer Image buffer to inspect.
 * @return Width, height, and channel count.
 * @throws Nothing.
 * @note The helper intentionally ignores data type because legacy behavior did
 * not coerce secondary input depth.
 */
ImageShape shape_of(const ImageBuffer& buffer) {
  return {buffer.width, buffer.height, buffer.channels};
}

/**
 * @brief Checks whether a buffer already matches the base image shape.
 *
 * @param buffer Candidate secondary input buffer.
 * @param base_shape Shape required by the base image.
 * @return True when width, height, and channel count match.
 * @throws Nothing.
 * @note Matching buffers pass through without temporary storage.
 */
bool matches_shape(const ImageBuffer& buffer, const ImageShape& base_shape) {
  return buffer.width == base_shape.width &&
         buffer.height == base_shape.height &&
         buffer.channels == base_shape.channels;
}

/**
 * @brief Validates that an image_mixing input exists and has non-zero extent.
 *
 * @param output NodeOutput pointer supplied by dependency resolution.
 * @param node_id Node id used in GraphError messages.
 * @param role Human-readable input role, such as "Base" or "Secondary".
 * @return Reference to the validated ImageBuffer.
 * @throws GraphError when the pointer is null or the image extent is empty.
 * @note Channel count is not validated here because channel conversion is a
 * later normalization phase.
 */
const ImageBuffer& require_non_empty_image(const NodeOutput* output,
                                           int node_id, const char* role) {
  if (!output) {
    throw GraphError(GraphErrc::MissingDependency,
                     std::string(role) + " image for image_mixing node " +
                         std::to_string(node_id) + " is missing.");
  }
  const ImageBuffer& buffer = output->image_buffer;
  if (buffer.width == 0 || buffer.height == 0) {
    throw GraphError(GraphErrc::InvalidParameter,
                     std::string(role) + " image for image_mixing node " +
                         std::to_string(node_id) + " is empty.");
  }
  return buffer;
}

/**
 * @brief Resizes a secondary image to the base extent.
 *
 * @param current_mat Secondary input matrix.
 * @param base_shape Required output shape.
 * @return Matrix resized to base width and height.
 * @throws cv::Exception when OpenCV resize fails.
 * @note Interpolation matches the previous tiled image_mixing path.
 */
cv::Mat resize_to_base(cv::Mat current_mat, const ImageShape& base_shape) {
  cv::resize(current_mat, current_mat,
             cv::Size(base_shape.width, base_shape.height), 0, 0,
             cv::INTER_LINEAR);
  return current_mat;
}

/**
 * @brief Crops and pads a secondary image to the base extent.
 *
 * @param current_mat Secondary input matrix.
 * @param base_shape Required output shape.
 * @return Matrix with source pixels copied into the top-left base-sized frame.
 * @throws cv::Exception when OpenCV allocation or copy fails.
 * @note This preserves the previous crop strategy, including zero padding when
 * the secondary image is smaller than the base.
 */
cv::Mat crop_to_base(const cv::Mat& current_mat, const ImageShape& base_shape) {
  cv::Rect crop_roi(0, 0, std::min(current_mat.cols, base_shape.width),
                    std::min(current_mat.rows, base_shape.height));
  cv::Mat cropped =
      cv::Mat::zeros(base_shape.height, base_shape.width, current_mat.type());
  current_mat(crop_roi).copyTo(cropped(crop_roi));
  return cropped;
}

/**
 * @brief Applies the configured image_mixing size strategy.
 *
 * @param current_mat Secondary input matrix.
 * @param base_shape Required base extent and channels.
 * @param strategy merge_strategy runtime parameter.
 * @param node_id Node id used in GraphError messages.
 * @return Matrix with base width and height.
 * @throws GraphError when strategy is unsupported.
 * @throws cv::Exception when OpenCV resize/crop operations fail.
 * @note Channel conversion is handled separately after size normalization.
 */
cv::Mat normalize_size(cv::Mat current_mat, const ImageShape& base_shape,
                       const std::string& strategy, int node_id) {
  if (current_mat.cols == base_shape.width &&
      current_mat.rows == base_shape.height) {
    return current_mat;
  }
  if (strategy == "resize") {
    return resize_to_base(current_mat, base_shape);
  }
  if (strategy == "crop") {
    return crop_to_base(current_mat, base_shape);
  }
  throw GraphError(GraphErrc::InvalidParameter,
                   "Unsupported merge_strategy '" + strategy +
                       "' for tiled image_mixing node " +
                       std::to_string(node_id) + ".");
}

/**
 * @brief Expands a single-channel matrix to the requested channel count.
 *
 * @param current_mat Single-channel input matrix.
 * @param target_channels Base image channel count.
 * @return Multi-channel matrix with duplicated source planes.
 * @throws cv::Exception when OpenCV merge fails.
 * @note Only 3- and 4-channel targets are supported to preserve legacy
 * behavior.
 */
cv::Mat expand_gray_to_color(const cv::Mat& current_mat, int target_channels) {
  std::vector<cv::Mat> planes(target_channels, current_mat);
  cv::Mat converted;
  cv::merge(planes, converted);
  return converted;
}

/**
 * @brief Converts an already sized secondary image to the base channel count.
 *
 * @param current_mat Secondary input matrix after size normalization.
 * @param base_shape Required base image shape.
 * @param node_id Node id used in GraphError messages.
 * @return Matrix with base channel count.
 * @throws GraphError when the channel conversion is unsupported.
 * @throws cv::Exception when OpenCV color conversion fails.
 * @note Conversion cases exactly mirror the previous tiled image_mixing logic.
 */
cv::Mat normalize_channels(cv::Mat current_mat, const ImageShape& base_shape,
                           int node_id) {
  const int current_channels = current_mat.channels();
  if (current_channels == base_shape.channels) {
    return current_mat;
  }
  if (current_channels == 1 &&
      (base_shape.channels == 3 || base_shape.channels == 4)) {
    return expand_gray_to_color(current_mat, base_shape.channels);
  }
  if ((current_channels == 3 || current_channels == 4) &&
      base_shape.channels == 1) {
    cv::cvtColor(current_mat, current_mat, cv::COLOR_BGR2GRAY);
    return current_mat;
  }
  if (current_channels == 4 && base_shape.channels == 3) {
    cv::cvtColor(current_mat, current_mat, cv::COLOR_BGRA2BGR);
    return current_mat;
  }
  if (current_channels == 3 && base_shape.channels == 4) {
    cv::cvtColor(current_mat, current_mat, cv::COLOR_BGR2BGRA);
    return current_mat;
  }
  throw GraphError(GraphErrc::InvalidParameter,
                   "Unsupported channel conversion for image_mixing node " +
                       std::to_string(node_id) + ": " +
                       std::to_string(current_channels) + " -> " +
                       std::to_string(base_shape.channels));
}

/**
 * @brief Normalizes one secondary image_mixing input when needed.
 *
 * @param input Secondary NodeOutput supplied by dependency resolution.
 * @param base_shape Base image shape.
 * @param strategy merge_strategy runtime parameter.
 * @param node_id Node id used in GraphError messages.
 * @return Normalized NodeOutput, or nullopt when input already matches base.
 * @throws GraphError when the input is invalid or unsupported.
 * @throws cv::Exception or std::runtime_error when image conversion fails.
 * @note Returned NodeOutput owns or references OpenCV storage through
 * fromCvMat.
 */
std::optional<NodeOutput> normalize_secondary_input(
    const NodeOutput* input, const ImageShape& base_shape,
    const std::string& strategy, int node_id) {
  const ImageBuffer& current_buffer =
      require_non_empty_image(input, node_id, "Secondary");
  if (matches_shape(current_buffer, base_shape)) {
    return std::nullopt;
  }
  cv::Mat current_mat = toCvMat(current_buffer);
  current_mat = normalize_size(current_mat, base_shape, strategy, node_id);
  current_mat = normalize_channels(current_mat, base_shape, node_id);

  NodeOutput normalized;
  normalized.image_buffer = fromCvMat(current_mat);
  return normalized;
}

/**
 * @brief Stores a normalized input and updates the context pointer table.
 *
 * @param context Context whose storage and pointer table are updated.
 * @param input_index Original input index being replaced.
 * @param normalized Temporary normalized output to store.
 * @throws std::bad_alloc if vector growth fails.
 * @note The caller reserves enough storage for all secondary inputs before the
 * loop, so pointers already written into context.inputs remain stable.
 */
void replace_input_with_normalized_output(TiledInputContext& context,
                                          size_t input_index,
                                          NodeOutput normalized) {
  context.normalized_storage.push_back(std::move(normalized));
  context.inputs[input_index] = &context.normalized_storage.back();
}

}  // namespace

TiledInputContext TiledInputNormalizer::normalize(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  TiledInputContext context;
  context.inputs = inputs;
  if (!should_normalize_mixing_inputs(node, inputs)) {
    return context;
  }

  const ImageBuffer& base_buffer =
      require_non_empty_image(inputs.front(), node.id, "Base");
  const ImageShape base_shape = shape_of(base_buffer);
  const std::string strategy =
      as_str(node.runtime_parameters, "merge_strategy", "resize");
  context.normalized_storage.reserve(inputs.size() - 1);

  for (size_t i = 1; i < inputs.size(); ++i) {
    std::optional<NodeOutput> normalized =
        normalize_secondary_input(inputs[i], base_shape, strategy, node.id);
    if (normalized) {
      replace_input_with_normalized_output(context, i, std::move(*normalized));
    }
  }
  return context;
}

}  // namespace ps::compute
