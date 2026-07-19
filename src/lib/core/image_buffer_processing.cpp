#include "core/image_buffer_processing.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

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
 * @brief Returns a borrowed byte address for one scalar sample.
 *
 * @param buffer Valid addressable CPU descriptor.
 * @param x Pixel column.
 * @param y Pixel row.
 * @param channel Channel index.
 * @return Pointer to the scalar's first byte.
 * @throws std::invalid_argument if the descriptor contains an unknown type.
 * @note The caller retains buffer.data for the complete access.
 */
const std::byte* scalar_address(const ImageBuffer& buffer, int x, int y,
                                int channel) {
  const std::size_t scalar_bytes = image_buffer_bytes_per_channel(buffer.type);
  const std::size_t sample_index =
      static_cast<std::size_t>(x) * static_cast<std::size_t>(buffer.channels) +
      static_cast<std::size_t>(channel);
  return static_cast<const std::byte*>(buffer.data.get()) +
         static_cast<std::size_t>(y) * buffer.step +
         sample_index * scalar_bytes;
}

/**
 * @brief Returns a borrowed writable byte address for one scalar sample.
 *
 * @param buffer Valid writable CPU descriptor.
 * @param x Pixel column.
 * @param y Pixel row.
 * @param channel Channel index.
 * @return Pointer to the scalar's first byte.
 * @throws std::invalid_argument if the descriptor contains an unknown type.
 * @note ImageBuffer cannot encode operating-system write permissions; callers
 *       supply writable storage.
 */
std::byte* mutable_scalar_address(const ImageBuffer& buffer, int x, int y,
                                  int channel) {
  return const_cast<std::byte*>(scalar_address(buffer, x, y, channel));
}

/**
 * @brief Loads one scalar into a common interpolation representation.
 *
 * @param address Borrowed scalar address with enough bytes for type.
 * @param type Scalar storage type.
 * @return Scalar converted to long double.
 * @throws std::invalid_argument if type is not declared.
 * @note memcpy permits descriptors whose scalar address is not naturally
 *       aligned.
 */
long double load_scalar(const std::byte* address, DataType type) {
  switch (type) {
    case DataType::UINT8: {
      std::uint8_t value = 0;
      std::memcpy(&value, address, sizeof(value));
      return value;
    }
    case DataType::INT8: {
      std::int8_t value = 0;
      std::memcpy(&value, address, sizeof(value));
      return value;
    }
    case DataType::UINT16: {
      std::uint16_t value = 0;
      std::memcpy(&value, address, sizeof(value));
      return value;
    }
    case DataType::INT16: {
      std::int16_t value = 0;
      std::memcpy(&value, address, sizeof(value));
      return value;
    }
    case DataType::FLOAT32: {
      float value = 0.0F;
      std::memcpy(&value, address, sizeof(value));
      return value;
    }
    case DataType::FLOAT64: {
      double value = 0.0;
      std::memcpy(&value, address, sizeof(value));
      return value;
    }
  }
  throw std::invalid_argument("Image processing scalar type is invalid.");
}

/**
 * @brief Clamps and rounds one scalar for an integral destination type.
 *
 * @tparam Scalar Signed or unsigned integral scalar type.
 * @param value Common interpolation value.
 * @return Representable scalar value.
 * @throws Nothing.
 * @note Rounding uses the nearest value with half cases away from zero.
 */
template <typename Scalar>
Scalar clamp_integral_scalar(long double value) noexcept {
  static_assert(std::is_integral<Scalar>::value,
                "Scalar must be an integral type");
  const long double minimum =
      static_cast<long double>(std::numeric_limits<Scalar>::lowest());
  const long double maximum =
      static_cast<long double>(std::numeric_limits<Scalar>::max());
  return static_cast<Scalar>(std::round(std::clamp(value, minimum, maximum)));
}

/**
 * @brief Stores one common interpolation value in a declared scalar format.
 *
 * @param address Borrowed writable scalar address.
 * @param type Destination storage type.
 * @param value Common value to convert.
 * @return Nothing.
 * @throws std::invalid_argument if type is not declared.
 * @note Floating values preserve finite/infinite/NaN conversions supported by
 *       the host compiler; integral values are clamped and rounded.
 */
void store_scalar(std::byte* address, DataType type, long double value) {
  switch (type) {
    case DataType::UINT8: {
      const std::uint8_t converted = clamp_integral_scalar<std::uint8_t>(value);
      std::memcpy(address, &converted, sizeof(converted));
      return;
    }
    case DataType::INT8: {
      const std::int8_t converted = clamp_integral_scalar<std::int8_t>(value);
      std::memcpy(address, &converted, sizeof(converted));
      return;
    }
    case DataType::UINT16: {
      const std::uint16_t converted =
          clamp_integral_scalar<std::uint16_t>(value);
      std::memcpy(address, &converted, sizeof(converted));
      return;
    }
    case DataType::INT16: {
      const std::int16_t converted = clamp_integral_scalar<std::int16_t>(value);
      std::memcpy(address, &converted, sizeof(converted));
      return;
    }
    case DataType::FLOAT32: {
      const float converted = static_cast<float>(value);
      std::memcpy(address, &converted, sizeof(converted));
      return;
    }
    case DataType::FLOAT64: {
      const double converted = static_cast<double>(value);
      std::memcpy(address, &converted, sizeof(converted));
      return;
    }
  }
  throw std::invalid_argument("Image processing scalar type is invalid.");
}

/**
 * @brief Returns the opaque-channel value used by 3-to-4 conversion.
 *
 * @param type Scalar storage type.
 * @return One for floating-point storage or the maximum integral value.
 * @throws std::invalid_argument if type is not declared.
 * @note This mirrors OpenCV color conversion's conventional opaque alpha.
 */
long double opaque_alpha(DataType type) {
  switch (type) {
    case DataType::UINT8:
      return std::numeric_limits<std::uint8_t>::max();
    case DataType::INT8:
      return std::numeric_limits<std::int8_t>::max();
    case DataType::UINT16:
      return std::numeric_limits<std::uint16_t>::max();
    case DataType::INT16:
      return std::numeric_limits<std::int16_t>::max();
    case DataType::FLOAT32:
    case DataType::FLOAT64:
      return 1.0L;
  }
  throw std::invalid_argument("Image processing scalar type is invalid.");
}

/**
 * @brief Bilinear source indices and upper-sample weight for one axis.
 *
 * @throws Nothing for value operations.
 * @note Border coordinates collapse both indices onto the nearest edge so
 *       half-pixel sampling matches replicated-border interpolation.
 */
struct InterpolationCoordinate {
  /** @brief Lower source index after border replication. */
  int lower = 0;

  /** @brief Upper source index after border replication. */
  int upper = 0;

  /** @brief Weight assigned to the upper source sample. */
  long double upper_weight = 0.0L;
};

/**
 * @brief Resolves one half-pixel coordinate against a source extent.
 *
 * @param position Continuous source coordinate.
 * @param extent Positive source-axis extent.
 * @return Border-replicated interpolation indices and weight.
 * @throws Nothing.
 * @note The caller validates extent before invoking this arithmetic helper.
 */
InterpolationCoordinate interpolation_coordinate(long double position,
                                                 int extent) noexcept {
  if (extent <= 1 || position <= 0.0L) {
    return InterpolationCoordinate{};
  }
  if (position >= static_cast<long double>(extent - 1)) {
    return InterpolationCoordinate{extent - 1, extent - 1, 0.0L};
  }
  const int lower = static_cast<int>(std::floor(position));
  return InterpolationCoordinate{lower, lower + 1,
                                 position - static_cast<long double>(lower)};
}

}  // namespace

/** @copydoc clone_cpu_image_buffer */
ImageBuffer clone_cpu_image_buffer(const ImageBuffer& source) {
  validate_addressable_cpu_buffer(source);
  ImageBuffer cloned = make_aligned_cpu_image_buffer(
      source.width, source.height, source.channels, source.type);
  copy_image_buffer_region(
      InputTileView{&source, PixelRect{0, 0, source.width, source.height}},
      OutputTileView{&cloned, PixelRect{0, 0, cloned.width, cloned.height}});
  return cloned;
}

/** @copydoc resize_cpu_image_buffer */
ImageBuffer resize_cpu_image_buffer(const ImageBuffer& source,
                                    const PixelSize& destination_size) {
  validate_addressable_cpu_buffer(source);
  if (destination_size.width <= 0 || destination_size.height <= 0) {
    throw std::invalid_argument(
        "Image resize requires a positive destination extent.");
  }

  ImageBuffer resized = make_aligned_cpu_image_buffer(
      destination_size.width, destination_size.height, source.channels,
      source.type);
  const long double scale_x =
      static_cast<long double>(source.width) / destination_size.width;
  const long double scale_y =
      static_cast<long double>(source.height) / destination_size.height;
  for (int destination_y = 0; destination_y < destination_size.height;
       ++destination_y) {
    const long double source_y =
        (static_cast<long double>(destination_y) + 0.5L) * scale_y - 0.5L;
    const InterpolationCoordinate y_coordinate =
        interpolation_coordinate(source_y, source.height);
    for (int destination_x = 0; destination_x < destination_size.width;
         ++destination_x) {
      const long double source_x =
          (static_cast<long double>(destination_x) + 0.5L) * scale_x - 0.5L;
      const InterpolationCoordinate x_coordinate =
          interpolation_coordinate(source_x, source.width);
      for (int channel = 0; channel < source.channels; ++channel) {
        const long double top_left =
            load_scalar(scalar_address(source, x_coordinate.lower,
                                       y_coordinate.lower, channel),
                        source.type);
        const long double top_right =
            load_scalar(scalar_address(source, x_coordinate.upper,
                                       y_coordinate.lower, channel),
                        source.type);
        const long double bottom_left =
            load_scalar(scalar_address(source, x_coordinate.lower,
                                       y_coordinate.upper, channel),
                        source.type);
        const long double bottom_right =
            load_scalar(scalar_address(source, x_coordinate.upper,
                                       y_coordinate.upper, channel),
                        source.type);
        const long double top =
            top_left + (top_right - top_left) * x_coordinate.upper_weight;
        const long double bottom = bottom_left + (bottom_right - bottom_left) *
                                                     x_coordinate.upper_weight;
        store_scalar(mutable_scalar_address(resized, destination_x,
                                            destination_y, channel),
                     resized.type,
                     top + (bottom - top) * y_coordinate.upper_weight);
      }
    }
  }
  return resized;
}

/** @copydoc convert_cpu_image_buffer_channels */
ImageBuffer convert_cpu_image_buffer_channels(const ImageBuffer& source,
                                              int destination_channels) {
  validate_addressable_cpu_buffer(source);
  if (source.channels == destination_channels) {
    return clone_cpu_image_buffer(source);
  }
  const bool supported =
      (source.channels == 1 &&
       (destination_channels == 3 || destination_channels == 4)) ||
      ((source.channels == 3 || source.channels == 4) &&
       destination_channels == 1) ||
      (source.channels == 4 && destination_channels == 3) ||
      (source.channels == 3 && destination_channels == 4);
  if (!supported) {
    throw std::invalid_argument(
        "Image channel conversion is unsupported for the requested counts.");
  }

  ImageBuffer converted = make_aligned_cpu_image_buffer(
      source.width, source.height, destination_channels, source.type);
  for (int y = 0; y < source.height; ++y) {
    for (int x = 0; x < source.width; ++x) {
      if (source.channels == 1) {
        const long double gray =
            load_scalar(scalar_address(source, x, y, 0), source.type);
        for (int channel = 0; channel < destination_channels; ++channel) {
          store_scalar(mutable_scalar_address(converted, x, y, channel),
                       converted.type, gray);
        }
        continue;
      }

      const long double blue =
          load_scalar(scalar_address(source, x, y, 0), source.type);
      const long double green =
          load_scalar(scalar_address(source, x, y, 1), source.type);
      const long double red =
          load_scalar(scalar_address(source, x, y, 2), source.type);
      if (destination_channels == 1) {
        const long double gray = blue * 0.114L + green * 0.587L + red * 0.299L;
        store_scalar(mutable_scalar_address(converted, x, y, 0), converted.type,
                     gray);
        continue;
      }

      store_scalar(mutable_scalar_address(converted, x, y, 0), converted.type,
                   blue);
      store_scalar(mutable_scalar_address(converted, x, y, 1), converted.type,
                   green);
      store_scalar(mutable_scalar_address(converted, x, y, 2), converted.type,
                   red);
      if (destination_channels == 4) {
        store_scalar(mutable_scalar_address(converted, x, y, 3), converted.type,
                     opaque_alpha(converted.type));
      }
    }
  }
  return converted;
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

  ImageBuffer cropped = make_aligned_cpu_image_buffer(
      source_roi.width, source_roi.height, source.channels, source.type);
  copy_image_buffer_region(
      InputTileView{&source, source_roi},
      OutputTileView{&cropped, PixelRect{0, 0, cropped.width, cropped.height}});
  ImageBuffer resized = resize_cpu_image_buffer(
      cropped, PixelSize{destination_roi.width, destination_roi.height});
  copy_image_buffer_region(
      InputTileView{&resized, PixelRect{0, 0, resized.width, resized.height}},
      OutputTileView{&destination, destination_roi});
}

}  // namespace ps::image_processing
