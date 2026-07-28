#include "core/value_image_adapter.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/core/image_buffer.hpp"
#include "photospider/data/image_view.hpp"

namespace ps::value_image_adapter {
namespace {

/**
 * @brief Multiplies image-byte components with overflow checking.
 *
 * @param left First non-negative component.
 * @param right Second non-negative component.
 * @return Exact product.
 * @throws std::overflow_error when the product exceeds std::size_t.
 */
std::size_t checked_multiply(std::size_t left, std::size_t right) {
  if (right != 0U && left > std::numeric_limits<std::size_t>::max() / right) {
    throw std::overflow_error("Image Value byte arithmetic exceeds size_t.");
  }
  return left * right;
}

/**
 * @brief Adds image-byte components with overflow checking.
 *
 * @param left First non-negative component.
 * @param right Second non-negative component.
 * @return Exact sum.
 * @throws std::overflow_error when the sum exceeds std::size_t.
 */
std::size_t checked_add(std::size_t left, std::size_t right) {
  if (left > std::numeric_limits<std::size_t>::max() - right) {
    throw std::overflow_error("Image Value byte arithmetic exceeds size_t.");
  }
  return left + right;
}

/**
 * @brief Converts one current ImageBuffer storage type to DenseTensor facts.
 *
 * @param type Valid current ImageBuffer channel storage type.
 * @return Matching logical semantics and physical bit width.
 * @throws std::invalid_argument for an unknown DataType value.
 */
std::pair<ElementSemantics, StorageEncoding> dense_element_from_image_type(
    DataType type) {
  switch (type) {
    case DataType::UINT8:
      return {ElementSemantics::UnsignedInteger, StorageEncoding{8U}};
    case DataType::INT8:
      return {ElementSemantics::SignedInteger, StorageEncoding{8U}};
    case DataType::UINT16:
      return {ElementSemantics::UnsignedInteger, StorageEncoding{16U}};
    case DataType::INT16:
      return {ElementSemantics::SignedInteger, StorageEncoding{16U}};
    case DataType::FLOAT32:
      return {ElementSemantics::FloatingPoint, StorageEncoding{32U}};
    case DataType::FLOAT64:
      return {ElementSemantics::FloatingPoint, StorageEncoding{64U}};
  }
  throw std::invalid_argument("ImageBuffer DataType is not declared.");
}

/**
 * @brief Converts supported DenseTensor element facts to ImageBuffer type.
 *
 * @param descriptor Valid DenseTensor descriptor.
 * @return Equivalent current ImageBuffer channel type.
 * @throws std::invalid_argument when the element combination is unsupported.
 */
DataType image_type_from_dense_element(
    const DenseTensorDescriptor& descriptor) {
  const std::uint32_t bit_width = descriptor.storage_encoding.bit_width;
  switch (descriptor.element_semantics) {
    case ElementSemantics::UnsignedInteger:
      if (bit_width == 8U) {
        return DataType::UINT8;
      }
      if (bit_width == 16U) {
        return DataType::UINT16;
      }
      break;
    case ElementSemantics::SignedInteger:
      if (bit_width == 8U) {
        return DataType::INT8;
      }
      if (bit_width == 16U) {
        return DataType::INT16;
      }
      break;
    case ElementSemantics::FloatingPoint:
      if (bit_width == 32U) {
        return DataType::FLOAT32;
      }
      if (bit_width == 64U) {
        return DataType::FLOAT64;
      }
      break;
  }
  throw std::invalid_argument(
      "DenseTensor element cannot be adapted to ImageBuffer.");
}

}  // namespace

/** @copydoc snapshot_cpu_image_value */
Value snapshot_cpu_image_value(const ImageBuffer& buffer) {
  validate_image_buffer(buffer);
  if (buffer.device != Device::CPU || buffer.width <= 0 || buffer.height <= 0 ||
      buffer.channels <= 0 || !buffer.data) {
    throw std::invalid_argument(
        "Image Value snapshot requires a nonempty owned CPU ImageBuffer.");
  }
  if (buffer.step >
      static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
    throw std::invalid_argument(
        "ImageBuffer row stride exceeds the signed layout domain.");
  }

  const std::size_t element_bytes = image_buffer_bytes_per_channel(buffer.type);
  const std::size_t pixel_bytes = checked_multiply(
      static_cast<std::size_t>(buffer.channels), element_bytes);
  if (pixel_bytes > static_cast<std::size_t>(
                        std::numeric_limits<std::ptrdiff_t>::max()) ||
      element_bytes > static_cast<std::size_t>(
                          std::numeric_limits<std::ptrdiff_t>::max())) {
    throw std::invalid_argument(
        "ImageBuffer element stride exceeds the signed layout domain.");
  }

  const std::size_t row_bytes = image_buffer_row_bytes(buffer);
  const std::size_t preceding_rows = checked_multiply(
      static_cast<std::size_t>(buffer.height - 1), buffer.step);
  const std::size_t storage_size = checked_add(preceding_rows, row_bytes);
  std::vector<std::byte> storage(storage_size, std::byte{0});
  for (int row = 0; row < buffer.height; ++row) {
    std::memcpy(storage.data() + static_cast<std::size_t>(row) * buffer.step,
                image_buffer_row_data(buffer, row), row_bytes);
  }

  const auto [semantics, encoding] = dense_element_from_image_type(buffer.type);
  DenseTensorDescriptor descriptor{{static_cast<std::size_t>(buffer.height),
                                    static_cast<std::size_t>(buffer.width),
                                    static_cast<std::size_t>(buffer.channels)},
                                   semantics,
                                   encoding};
  ImageFacet image;
  image.x_axis = 1U;
  image.y_axis = 0U;
  image.channel_axis = 2U;
  StridedLayout layout{{static_cast<std::ptrdiff_t>(buffer.step),
                        static_cast<std::ptrdiff_t>(pixel_bytes),
                        static_cast<std::ptrdiff_t>(element_bytes)}};
  return Value::from_cpu_dense_tensor(std::move(descriptor), image,
                                      std::move(layout), std::move(storage));
}

/** @copydoc snapshot_cpu_image_buffer */
ImageBuffer snapshot_cpu_image_buffer(const Value& value) {
  ImageView view(value);
  const DataType type = image_type_from_dense_element(view.descriptor());
  ImageBuffer output = make_aligned_cpu_image_buffer(
      static_cast<int>(view.width()), static_cast<int>(view.height()),
      static_cast<int>(view.channels()), type);

  const std::size_t element_bytes = view.element_bytes();
  const std::size_t channels = view.channels();
  auto* output_base = static_cast<std::byte*>(output.data.get());
  for (std::size_t y = 0U; y < view.height(); ++y) {
    std::byte* output_row = output_base + y * output.step;
    for (std::size_t x = 0U; x < view.width(); ++x) {
      for (std::size_t channel = 0U; channel < channels; ++channel) {
        const std::size_t element_index =
            checked_add(checked_multiply(x, channels), channel);
        std::memcpy(output_row + checked_multiply(element_index, element_bytes),
                    view.channel_data(x, y, channel), element_bytes);
      }
    }
  }
  validate_image_buffer(output);
  return output;
}

/** @copydoc normalize_node_output_image_value */
void normalize_node_output_image_value(NodeOutput* output) {
  if (output == nullptr) {
    throw std::invalid_argument(
        "NodeOutput normalization requires a non-null destination.");
  }
  if (output->image_value.valid()) {
    return;
  }
  const ImageBuffer& buffer = output->image_buffer;
  if (buffer.device != Device::CPU || buffer.width <= 0 || buffer.height <= 0 ||
      buffer.channels <= 0 || !buffer.data) {
    return;
  }
  output->image_value = snapshot_cpu_image_value(buffer);
}

/** @copydoc full_node_output_region */
RegionSet full_node_output_region(const NodeOutput& output) {
  if (output.image_value.valid()) {
    if (output.image_value.image_facet().has_value()) {
      const ImageView view(output.image_value);
      return RegionSet::from_image_rect(
          {image_region_domain(), 0, static_cast<std::int64_t>(view.width()), 0,
           static_cast<std::int64_t>(view.height())});
    }

    const DenseTensorDescriptor& descriptor =
        output.image_value.dense_tensor_descriptor();
    std::vector<RegionInterval> axes;
    axes.reserve(descriptor.shape.size());
    for (const std::size_t extent : descriptor.shape) {
      if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (extent > std::numeric_limits<std::uint64_t>::max()) {
          throw std::overflow_error(
              "DenseTensor extent exceeds Region uint64 bounds.");
        }
      }
      axes.push_back({0U, static_cast<std::uint64_t>(extent)});
    }
    return RegionSet::from_tensor_slice(
        {dense_tensor_region_domain(), std::move(axes)});
  }

  const ImageBuffer& buffer = output.image_buffer;
  if (buffer.width > 0 && buffer.height > 0) {
    return RegionSet::from_image_rect(
        {image_region_domain(), 0, buffer.width, 0, buffer.height});
  }
  return RegionSet::whole();
}

}  // namespace ps::value_image_adapter
