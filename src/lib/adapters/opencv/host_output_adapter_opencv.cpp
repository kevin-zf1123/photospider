#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include "adapters/opencv/buffer_adapter_opencv.hpp"

namespace ps {
namespace {

/**
 * @brief Converts one frozen Host output plan to an OpenCV matrix type.
 *
 * @param plan Complete validated ordinary-image output plan.
 * @return OpenCV CV_* type matching the plan's element facts and channels.
 * @throws std::invalid_argument when the element combination or channel count
 * cannot be represented by the OpenCV adapter.
 * @note The plan remains metadata authority; this callback-local projection
 * creates no ImageBuffer descriptor or allocation identity.
 */
int to_cv_type(const DenseImageOutputPlan& plan) {
  if (plan.channels() > static_cast<std::size_t>(CV_CN_MAX)) {
    throw std::invalid_argument(
        "OpenCV output conversion requires a supported channel count");
  }
  const int channels = static_cast<int>(plan.channels());
  const DenseTensorDescriptor& descriptor = plan.descriptor();
  switch (descriptor.element_semantics) {
    case ElementSemantics::UnsignedInteger:
      if (descriptor.storage_encoding.bit_width == 8U) {
        return CV_8UC(channels);
      }
      if (descriptor.storage_encoding.bit_width == 16U) {
        return CV_16UC(channels);
      }
      break;
    case ElementSemantics::SignedInteger:
      if (descriptor.storage_encoding.bit_width == 8U) {
        return CV_8SC(channels);
      }
      if (descriptor.storage_encoding.bit_width == 16U) {
        return CV_16SC(channels);
      }
      break;
    case ElementSemantics::FloatingPoint:
      if (descriptor.storage_encoding.bit_width == 32U) {
        return CV_32FC(channels);
      }
      if (descriptor.storage_encoding.bit_width == 64U) {
        return CV_64FC(channels);
      }
      break;
  }
  throw std::invalid_argument(
      "OpenCV output conversion does not support the planned element type");
}

/**
 * @brief Validates one OutputTile and returns its first writable grant byte.
 *
 * Validation proves the integer ROI exactly matches the logical grant, every
 * row span has the planned width and allocation offset, and all derived row
 * addresses remain active before an OpenCV header is constructed.
 *
 * @param tile Output tile supplied to one trusted provider callback.
 * @return Mutable pointer to the first selected row.
 * @throws std::runtime_error when the plan or grant pointer is absent.
 * @throws std::invalid_argument when the ROI/grant/plan relationship differs.
 * @throws std::overflow_error when endpoint or byte arithmetic exceeds its
 * representation.
 * @throws std::logic_error when the grant has already retired or been revoked.
 * @throws std::system_error when grant synchronization fails.
 * @note The pointer and all derived OpenCV views must stop being used before
 * grant retirement. The function creates no whole-allocation owner.
 */
std::byte* validate_output_tile(const OutputTile& tile) {
  if (tile.plan == nullptr || tile.grant == nullptr) {
    throw std::runtime_error(
        "OpenCV output conversion requires a plan and active grant");
  }
  if (tile.roi.width <= 0 || tile.roi.height <= 0) {
    throw std::invalid_argument(
        "OpenCV output conversion requires a nonempty tile ROI");
  }

  const ImageRect& granted = tile.grant->image_region();
  const std::int64_t roi_x_end = static_cast<std::int64_t>(tile.roi.x) +
                                 static_cast<std::int64_t>(tile.roi.width);
  const std::int64_t roi_y_end = static_cast<std::int64_t>(tile.roi.y) +
                                 static_cast<std::int64_t>(tile.roi.height);
  if (!(granted.domain == image_region_domain()) ||
      granted.x_begin != static_cast<std::int64_t>(tile.roi.x) ||
      granted.x_end != roi_x_end ||
      granted.y_begin != static_cast<std::int64_t>(tile.roi.y) ||
      granted.y_end != roi_y_end) {
    throw std::invalid_argument(
        "OpenCV output tile ROI does not match its Host grant");
  }

  const ImageBounds& bounds = tile.plan->image_facet().data_window;
  if (granted.x_begin < bounds.x_begin || granted.x_end > bounds.x_end ||
      granted.y_begin < bounds.y_begin || granted.y_end > bounds.y_end) {
    throw std::invalid_argument(
        "OpenCV output grant exceeds its immutable output plan");
  }
  const std::size_t height = static_cast<std::size_t>(tile.roi.height);
  const std::size_t width = static_cast<std::size_t>(tile.roi.width);
  if (tile.grant->span_count() != height ||
      width >
          std::numeric_limits<std::size_t>::max() / tile.plan->pixel_bytes()) {
    throw std::overflow_error(
        "OpenCV output tile span geometry is unrepresentable");
  }
  const std::size_t row_bytes = width * tile.plan->pixel_bytes();
  const std::uint64_t y_distance = static_cast<std::uint64_t>(granted.y_begin) -
                                   static_cast<std::uint64_t>(bounds.y_begin);
  const std::uint64_t x_distance = static_cast<std::uint64_t>(granted.x_begin) -
                                   static_cast<std::uint64_t>(bounds.x_begin);
  if (y_distance > std::numeric_limits<std::size_t>::max() ||
      x_distance > std::numeric_limits<std::size_t>::max()) {
    throw std::overflow_error(
        "OpenCV output tile coordinate distance exceeds size_t");
  }
  const std::size_t y = static_cast<std::size_t>(y_distance);
  const std::size_t x = static_cast<std::size_t>(x_distance);
  if (y > std::numeric_limits<std::size_t>::max() / tile.plan->row_stride() ||
      x > std::numeric_limits<std::size_t>::max() / tile.plan->pixel_bytes()) {
    throw std::overflow_error(
        "OpenCV output tile byte offset is unrepresentable");
  }
  const std::size_t y_offset = y * tile.plan->row_stride();
  const std::size_t x_offset = x * tile.plan->pixel_bytes();
  if (y_offset > std::numeric_limits<std::size_t>::max() - x_offset) {
    throw std::overflow_error(
        "OpenCV output tile byte offset is unrepresentable");
  }
  const std::size_t first_offset = y_offset + x_offset;
  std::byte* first_row = nullptr;
  for (std::size_t row = 0U; row < height; ++row) {
    if (row >
            std::numeric_limits<std::size_t>::max() / tile.plan->row_stride() ||
        first_offset > std::numeric_limits<std::size_t>::max() -
                           row * tile.plan->row_stride()) {
      throw std::overflow_error(
          "OpenCV output tile row offset is unrepresentable");
    }
    const std::size_t expected_offset =
        first_offset + row * tile.plan->row_stride();
    const HostOutputWriteSpan& span = tile.grant->span(row);
    if (span.allocation_offset != expected_offset ||
        span.byte_size != row_bytes) {
      throw std::invalid_argument(
          "OpenCV output tile spans do not match its immutable plan");
    }
    std::byte* const row_pointer = tile.grant->data(row);
    if (row == 0U) {
      first_row = row_pointer;
    } else if (row_pointer != first_row + row * tile.plan->row_stride()) {
      throw std::invalid_argument(
          "OpenCV output tile rows do not share the planned stride");
    }
  }
  return first_row;
}

}  // namespace

/** @copydoc toCvMat(const OutputTile&) */
cv::Mat toCvMat(const OutputTile& tile) {
  std::byte* const first_row = validate_output_tile(tile);
  return cv::Mat(tile.roi.height, tile.roi.width, to_cv_type(*tile.plan),
                 first_row, tile.plan->row_stride());
}

/** @copydoc toCvUMat(const OutputTile&) */
cv::UMat toCvUMat(const OutputTile& tile) {
  cv::Mat output(tile.roi.height, tile.roi.width, to_cv_type(*tile.plan),
                 validate_output_tile(tile), tile.plan->row_stride());
  return output.getUMat(cv::ACCESS_WRITE);
}

}  // namespace ps
