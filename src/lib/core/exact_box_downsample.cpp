/**
 * @file exact_box_downsample.cpp
 * @brief Implements dependency-neutral exact factor-four FP32 box averaging.
 */
#include <fenv.h>  // NOLINT(build/c++11)

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <stdexcept>

#include "core/image_buffer_processing.hpp"
#include "core/image_buffer_storage.hpp"

namespace ps::image_processing {
namespace {

/**
 * @brief Restores the complete caller floating-point environment by RAII.
 *
 * @throws std::runtime_error when construction cannot capture the environment
 * or select `FE_TONEAREST`.
 * @note Destruction is no-throw; a platform failure to restore the captured
 * environment terminates because returning with changed arithmetic state would
 * violate the product contract.
 */
class ScopedRoundToNearest final {
 public:
  /**
   * @brief Captures the complete environment and selects round-to-nearest.
   * @throws std::runtime_error when `fegetenv` or `fesetround` fails.
   * @note If restoration after a failed `fesetround` also fails, construction
   * terminates rather than returning with a contaminated environment.
   */
  ScopedRoundToNearest() {
    if (fegetenv(&saved_environment_) != 0) {
      throw std::runtime_error(
          "Exact box average could not capture floating-point environment.");
    }
    captured_ = true;
    if (fesetround(FE_TONEAREST) != 0) {
      if (fesetenv(&saved_environment_) != 0) {
        std::terminate();
      }
      captured_ = false;
      throw std::runtime_error(
          "Exact box average could not select round-to-nearest.");
    }
  }

  /**
   * @brief Restores rounding mode and exception state captured at construction.
   * @throws Nothing; restoration failure terminates.
   */
  ~ScopedRoundToNearest() noexcept {
    if (captured_ && fesetenv(&saved_environment_) != 0) {
      std::terminate();
    }
  }

  /**
   * @brief Prevents copying environment-restoration ownership.
   * @param other Owner that cannot be copied.
   * @throws Nothing because the operation is deleted.
   */
  ScopedRoundToNearest(const ScopedRoundToNearest& other) = delete;

  /**
   * @brief Prevents assignment of environment-restoration ownership.
   * @param other Owner that cannot be assigned.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ScopedRoundToNearest& operator=(const ScopedRoundToNearest& other) = delete;

 private:
  /** @brief Complete platform environment captured before arithmetic. */
  fenv_t saved_environment_{};

  /** @brief True only while destruction must restore the captured value. */
  bool captured_ = false;
};

/**
 * @brief Validates one nonempty addressable CPU FP32 descriptor.
 * @param buffer Descriptor to validate.
 * @return Nothing.
 * @throws std::invalid_argument when descriptor, device, type, or ownership is
 * not valid for exact scalar access.
 * @note Allocation capacity remains the producer's ImageBuffer contract.
 */
void validate_exact_fp32_buffer(const ImageBuffer& buffer) {
  validate_image_buffer(buffer);
  if (buffer.width <= 0 || buffer.height <= 0 || buffer.channels <= 0 ||
      buffer.device != Device::CPU || buffer.type != DataType::FLOAT32 ||
      !buffer.data) {
    throw std::invalid_argument(
        "Exact box average requires a nonempty owned CPU FP32 buffer.");
  }
}

/**
 * @brief Validates one nonempty ROI against an image extent.
 * @param roi Rectangle to validate.
 * @param extent Enclosing positive extent.
 * @return Nothing.
 * @throws std::out_of_range when the rectangle is empty, negative, or outside.
 * @note Difference comparisons avoid signed endpoint overflow.
 */
void validate_exact_roi(const PixelRect& roi, const PixelSize& extent) {
  if (roi.x < 0 || roi.y < 0 || roi.width <= 0 || roi.height <= 0 ||
      roi.x > extent.width || roi.y > extent.height ||
      roi.width > extent.width - roi.x || roi.height > extent.height - roi.y) {
    throw std::out_of_range(
        "Exact box average destination ROI is outside the image extent.");
  }
}

/**
 * @brief Loads one possibly unaligned binary32 sample by byte copy.
 * @param buffer Valid CPU FP32 descriptor.
 * @param x Pixel column.
 * @param y Pixel row.
 * @param channel Channel index.
 * @return Exact stored binary32 value.
 * @throws Nothing after descriptor and coordinate validation by the caller.
 */
float load_fp32(const ImageBuffer& buffer, int x, int y, int channel) noexcept {
  const std::size_t sample =
      (static_cast<std::size_t>(x) * static_cast<std::size_t>(buffer.channels) +
       static_cast<std::size_t>(channel)) *
      sizeof(float);
  const std::byte* address = static_cast<const std::byte*>(buffer.data.get()) +
                             static_cast<std::size_t>(y) * buffer.step + sample;
  float value = 0.0F;
  std::memcpy(&value, address, sizeof(value));
  return value;
}

/**
 * @brief Stores one binary32 sample through a possibly unaligned address.
 * @param buffer Valid writable CPU FP32 descriptor.
 * @param x Pixel column.
 * @param y Pixel row.
 * @param channel Channel index.
 * @param value Exact binary32 result to store.
 * @return Nothing.
 * @throws Nothing after descriptor and coordinate validation by the caller.
 */
void store_fp32(const ImageBuffer& buffer, int x, int y, int channel,
                float value) noexcept {
  const std::size_t sample =
      (static_cast<std::size_t>(x) * static_cast<std::size_t>(buffer.channels) +
       static_cast<std::size_t>(channel)) *
      sizeof(float);
  std::byte* address = static_cast<std::byte*>(buffer.data.get()) +
                       static_cast<std::size_t>(y) * buffer.step + sample;
  std::memcpy(address, &value, sizeof(value));
}

}  // namespace

/** @copydoc exact_box_average_factor_four_region */
void exact_box_average_factor_four_region(const ImageBuffer& source,
                                          const ImageBuffer& destination,
                                          const PixelRect& destination_roi) {
  validate_exact_fp32_buffer(source);
  validate_exact_fp32_buffer(destination);
  validate_exact_roi(destination_roi,
                     PixelSize{destination.width, destination.height});
  if (source.channels != destination.channels ||
      static_cast<std::int64_t>(source.width) !=
          static_cast<std::int64_t>(destination.width) * 4 ||
      static_cast<std::int64_t>(source.height) !=
          static_cast<std::int64_t>(destination.height) * 4) {
    throw std::invalid_argument(
        "Exact box average requires matching channels and factor-four "
        "extents.");
  }
  if (detail::image_buffer_storage_envelopes_may_overlap(source, destination)) {
    throw std::invalid_argument(
        "Exact box average source and destination storage must not overlap.");
  }

  ScopedRoundToNearest round_to_nearest;
  for (int destination_y = destination_roi.y;
       destination_y < destination_roi.y + destination_roi.height;
       ++destination_y) {
    const int source_y = destination_y * 4;
    for (int destination_x = destination_roi.x;
         destination_x < destination_roi.x + destination_roi.width;
         ++destination_x) {
      const int source_x = destination_x * 4;
      for (int channel = 0; channel < destination.channels; ++channel) {
        double sum = 0.0;
        for (int block_y = 0; block_y < 4; ++block_y) {
          for (int block_x = 0; block_x < 4; ++block_x) {
            sum += static_cast<double>(load_fp32(source, source_x + block_x,
                                                 source_y + block_y, channel));
          }
        }
        const float mean = static_cast<float>(sum * 0.0625);
        store_fp32(destination, destination_x, destination_y, channel, mean);
      }
    }
  }
}

}  // namespace ps::image_processing
