#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <vector>

#include "adapters/opencv/buffer_adapter_opencv.hpp"
#include "core/image_buffer_processing.hpp"

namespace ps::image_processing {
namespace {

/**
 * @brief Validates one nonempty addressable CPU descriptor.
 *
 * @param buffer Descriptor to validate.
 * @return Nothing.
 * @throws std::invalid_argument if validation fails or CPU data is absent.
 * @note Allocation capacity remains the producer's ImageBuffer contract.
 */
void validate_addressable_cpu_buffer(const ImageBuffer& buffer) {
  validate_image_buffer(buffer);
  if (buffer.width <= 0 || buffer.height <= 0 || buffer.channels <= 0 ||
      buffer.device != Device::CPU || !buffer.data) {
    throw std::invalid_argument(
        "Image processing requires a nonempty owned CPU buffer.");
  }
}

/**
 * @brief Validates a nonempty rectangle inside one image extent.
 *
 * @param rectangle Rectangle to validate.
 * @param size Enclosing image extent.
 * @return Nothing.
 * @throws std::out_of_range if the rectangle is empty, negative, or outside.
 * @note Subtraction after origin checks avoids signed endpoint overflow.
 */
void validate_nonempty_roi(const PixelRect& rectangle, const PixelSize& size) {
  if (rectangle.x < 0 || rectangle.y < 0 || rectangle.width <= 0 ||
      rectangle.height <= 0 || rectangle.x > size.width ||
      rectangle.y > size.height || rectangle.width > size.width - rectangle.x ||
      rectangle.height > size.height - rectangle.y) {
    throw std::out_of_range(
        "Image processing rectangle is outside the image extent.");
  }
}

/**
 * @brief Builds an OpenCV rectangle from kernel-owned geometry.
 *
 * @param rectangle Valid kernel pixel rectangle.
 * @return Equivalent OpenCV rectangle.
 * @throws Nothing.
 * @note The provider-specific value exists only for the immediate adapter call.
 */
cv::Rect to_opencv_rect(const PixelRect& rectangle) noexcept {
  return cv::Rect(rectangle.x, rectangle.y, rectangle.width, rectangle.height);
}

/**
 * @brief Expands a one-channel matrix into repeated color planes.
 *
 * @param source One-channel source matrix.
 * @param destination_channels Three or four destination channels.
 * @return Independent multi-channel OpenCV matrix.
 * @throws cv::Exception if OpenCV allocation or merge fails.
 * @throws std::bad_alloc if plane storage allocation fails.
 * @note Repeating every plane preserves the existing image-mixing behavior,
 *       including the gray value in the fourth channel.
 */
cv::Mat expand_gray_channels(const cv::Mat& source, int destination_channels) {
  std::vector<cv::Mat> planes(static_cast<std::size_t>(destination_channels),
                              source);
  cv::Mat converted;
  cv::merge(planes, converted);
  return converted;
}

}  // namespace

/** @copydoc clone_cpu_image_buffer */
ImageBuffer clone_cpu_image_buffer(const ImageBuffer& source) {
  validate_addressable_cpu_buffer(source);
  return fromCvMat(toCvMat(source).clone());
}

/** @copydoc resize_cpu_image_buffer */
ImageBuffer resize_cpu_image_buffer(const ImageBuffer& source,
                                    const PixelSize& destination_size) {
  validate_addressable_cpu_buffer(source);
  if (destination_size.width <= 0 || destination_size.height <= 0) {
    throw std::invalid_argument(
        "Image resize requires a positive destination extent.");
  }
  cv::Mat resized;
  cv::resize(toCvMat(source), resized,
             cv::Size(destination_size.width, destination_size.height), 0, 0,
             cv::INTER_LINEAR);
  return fromCvMat(resized);
}

/** @copydoc convert_cpu_image_buffer_channels */
ImageBuffer convert_cpu_image_buffer_channels(const ImageBuffer& source,
                                              int destination_channels) {
  validate_addressable_cpu_buffer(source);
  if (source.channels == destination_channels) {
    return clone_cpu_image_buffer(source);
  }

  cv::Mat current = toCvMat(source);
  if (source.channels == 1 &&
      (destination_channels == 3 || destination_channels == 4)) {
    return fromCvMat(expand_gray_channels(current, destination_channels));
  }
  if ((source.channels == 3 || source.channels == 4) &&
      destination_channels == 1) {
    cv::cvtColor(
        current, current,
        source.channels == 3 ? cv::COLOR_BGR2GRAY : cv::COLOR_BGRA2GRAY);
    return fromCvMat(current);
  }
  if (source.channels == 4 && destination_channels == 3) {
    cv::cvtColor(current, current, cv::COLOR_BGRA2BGR);
    return fromCvMat(current);
  }
  if (source.channels == 3 && destination_channels == 4) {
    cv::cvtColor(current, current, cv::COLOR_BGR2BGRA);
    return fromCvMat(current);
  }
  throw std::invalid_argument(
      "Image channel conversion is unsupported for the requested counts.");
}

/** @copydoc resize_cpu_image_buffer_region */
void resize_cpu_image_buffer_region(const ImageBuffer& source,
                                    const PixelRect& source_roi,
                                    const ImageBuffer& destination,
                                    const PixelRect& destination_roi) {
  validate_addressable_cpu_buffer(source);
  validate_addressable_cpu_buffer(destination);
  validate_nonempty_roi(source_roi, PixelSize{source.width, source.height});
  validate_nonempty_roi(destination_roi,
                        PixelSize{destination.width, destination.height});
  if (source.channels != destination.channels ||
      source.type != destination.type) {
    throw std::invalid_argument(
        "Image ROI resize requires matching channel and scalar formats.");
  }
  cv::Mat source_matrix = toCvMat(source);
  cv::Mat destination_matrix = toCvMat(destination);
  cv::Mat resized;
  cv::resize(source_matrix(to_opencv_rect(source_roi)), resized,
             cv::Size(destination_roi.width, destination_roi.height), 0, 0,
             cv::INTER_LINEAR);
  resized.copyTo(destination_matrix(to_opencv_rect(destination_roi)));
}

}  // namespace ps::image_processing
