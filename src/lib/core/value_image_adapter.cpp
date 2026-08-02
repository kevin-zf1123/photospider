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

/**
 * @brief Obtains one immutable dense Value from a current output boundary.
 * @param output Output whose sealed Value or CPU ImageBuffer is inspected.
 * @return Existing Value, a fresh ImageBuffer snapshot, or nullopt when no
 *         dependency-neutral dense payload is available.
 * @throws std::invalid_argument, std::overflow_error, or std::bad_alloc from
 *         CPU image snapshot publication.
 * @note Opaque non-CPU ImageBuffer payloads are never mapped implicitly.
 */
std::optional<Value> dense_value_from_output(const NodeOutput& output) {
  if (output.image_value.valid()) {
    return output.image_value;
  }
  const ImageBuffer& buffer = output.image_buffer;
  if (buffer.device == Device::CPU && buffer.width > 0 && buffer.height > 0 &&
      buffer.channels > 0 && buffer.data) {
    return snapshot_cpu_image_value(buffer);
  }
  return std::nullopt;
}

/**
 * @brief Validates one merge Region against concrete DenseTensor facts.
 * @param region Exact normalized selection to validate.
 * @param descriptor Shared logical tensor descriptor.
 * @param image_facet Optional explicit image-axis mapping.
 * @throws std::invalid_argument for unsupported atom count/domain/kind,
 *         missing image axes, negative or out-of-bounds ImageRect endpoints,
 *         or TensorSlice rank/bounds mismatch.
 * @note Whole and Empty require no finite-atom checks.
 */
void validate_merge_region(const RegionSet& region,
                           const DenseTensorDescriptor& descriptor,
                           const std::optional<ImageFacet>& image_facet) {
  if (region.is_whole() || region.is_empty()) {
    return;
  }
  if (region.atoms().size() != 1U) {
    throw std::invalid_argument(
        "Dense output merge accepts one exact Region atom.");
  }
  const RegionAtom& atom = region.atoms().front();
  if (const auto* image = std::get_if<ImageRect>(&atom)) {
    if (!(image->domain == image_region_domain()) || !image_facet.has_value()) {
      throw std::invalid_argument(
          "ImageRect output merge requires the built-in image domain and "
          "explicit ImageFacet.");
    }
    if (image->x_begin < 0 || image->y_begin < 0 ||
        image->x_end < image->x_begin || image->y_end < image->y_begin) {
      throw std::invalid_argument(
          "ImageRect output merge endpoints are invalid.");
    }
    if (static_cast<std::uint64_t>(image->x_end) >
            descriptor.shape[image_facet->x_axis] ||
        static_cast<std::uint64_t>(image->y_end) >
            descriptor.shape[image_facet->y_axis]) {
      throw std::invalid_argument(
          "ImageRect output merge exceeds the dense output bounds.");
    }
    return;
  }

  const TensorSlice& tensor = std::get<TensorSlice>(atom);
  if (!(tensor.domain == dense_tensor_region_domain()) ||
      tensor.axes.size() != descriptor.shape.size()) {
    throw std::invalid_argument(
        "TensorSlice output merge requires matching dense domain and rank.");
  }
  for (std::size_t axis = 0U; axis < tensor.axes.size(); ++axis) {
    if (tensor.axes[axis].end > descriptor.shape[axis]) {
      throw std::invalid_argument(
          "TensorSlice output merge exceeds the dense output bounds.");
    }
  }
}

/**
 * @brief Tests whether one logical coordinate belongs to a validated Region.
 * @param region Region validated by validate_merge_region().
 * @param image_facet Optional explicit image-axis mapping.
 * @param coordinates Complete logical tensor coordinate.
 * @return True for Whole or coordinates inside the exact atom.
 * @throws Nothing under validated rank, domain, and bounds preconditions.
 */
bool merge_coordinate_selected(
    const RegionSet& region, const std::optional<ImageFacet>& image_facet,
    const std::vector<std::size_t>& coordinates) noexcept {
  if (region.is_whole()) {
    return true;
  }
  if (region.is_empty()) {
    return false;
  }
  const RegionAtom& atom = region.atoms().front();
  if (const auto* image = std::get_if<ImageRect>(&atom)) {
    const std::size_t x = coordinates[image_facet->x_axis];
    const std::size_t y = coordinates[image_facet->y_axis];
    return x >= static_cast<std::uint64_t>(image->x_begin) &&
           x < static_cast<std::uint64_t>(image->x_end) &&
           y >= static_cast<std::uint64_t>(image->y_begin) &&
           y < static_cast<std::uint64_t>(image->y_end);
  }
  const TensorSlice& tensor = std::get<TensorSlice>(atom);
  for (std::size_t axis = 0U; axis < coordinates.size(); ++axis) {
    if (coordinates[axis] < tensor.axes[axis].begin ||
        coordinates[axis] >= tensor.axes[axis].end) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Builds a contiguous row-major producer layout and storage size.
 * @param descriptor Valid positive-rank DenseTensor descriptor.
 * @return Positive-stride layout and exact byte storage size.
 * @throws std::overflow_error when stride or storage arithmetic overflows.
 * @throws std::bad_alloc when stride storage cannot allocate.
 * @note The final logical axis is contiguous at one element stride.
 */
std::pair<StridedLayout, std::size_t> contiguous_layout_and_size(
    const DenseTensorDescriptor& descriptor) {
  const std::size_t element_bytes = dense_tensor_element_bytes(descriptor);
  std::size_t stride = element_bytes;
  StridedLayout layout;
  layout.byte_strides.resize(descriptor.shape.size());
  for (std::size_t reverse = descriptor.shape.size(); reverse > 0U; --reverse) {
    const std::size_t axis = reverse - 1U;
    if (stride >
        static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
      throw std::overflow_error("Dense output merge stride exceeds ptrdiff_t.");
    }
    layout.byte_strides[axis] = static_cast<std::ptrdiff_t>(stride);
    stride = checked_multiply(stride, descriptor.shape[axis]);
  }
  return {std::move(layout), stride};
}

/**
 * @brief Builds the complete TensorSlice for one concrete dense descriptor.
 * @param descriptor Valid DenseTensor descriptor with finite extents.
 * @return Exact rank-general Region covering every logical element.
 * @throws std::overflow_error when an extent exceeds Region uint64 bounds.
 * @throws std::bad_alloc when interval or Region storage cannot allocate.
 * @note This representation remains valid even when the Value also exposes an
 * ImageFacet and its compatibility full Region is an ImageRect.
 */
RegionSet full_dense_tensor_region(const DenseTensorDescriptor& descriptor) {
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

}  // namespace

/** @copydoc validate_image_buffer_compatible_value */
void validate_image_buffer_compatible_value(const Value& value) {
  if (!value.valid()) {
    throw std::invalid_argument(
        "ImageBuffer adaptation requires a valid Value.");
  }
  const DenseTensorDescriptor& descriptor = value.dense_tensor_descriptor();
  if (value.storage_layout_kind() != StorageLayoutKind::Strided ||
      descriptor.quantization.has_value() || !value.image_facet().has_value()) {
    throw std::invalid_argument(
        "ImageBuffer adaptation requires an unquantized image-faceted "
        "Strided Value.");
  }
  const ImageView view(value);
  (void)image_type_from_dense_element(view.descriptor());
}

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
  validate_image_buffer_compatible_value(value);
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

    return full_dense_tensor_region(
        output.image_value.dense_tensor_descriptor());
  }

  const ImageBuffer& buffer = output.image_buffer;
  if (buffer.width > 0 && buffer.height > 0) {
    return RegionSet::from_image_rect(
        {image_region_domain(), 0, buffer.width, 0, buffer.height});
  }
  return RegionSet::whole();
}

/** @copydoc node_output_region_is_complete */
bool node_output_region_is_complete(const NodeOutput& output,
                                    const RegionSet& region) {
  const RegionSet full = full_node_output_region(output);
  if (region_contains(region, full) == RegionContainmentStatus::Contains) {
    return true;
  }
  if (!output.image_value.valid()) {
    return false;
  }
  const RegionSet dense_full =
      full_dense_tensor_region(output.image_value.dense_tensor_descriptor());
  return region_contains(region, dense_full) ==
         RegionContainmentStatus::Contains;
}

/** @copydoc merge_node_output_region */
std::optional<NodeOutput> merge_node_output_region(
    const NodeOutput& existing, const NodeOutput& update,
    const RegionSet& updated_region) {
  if (updated_region.is_empty()) {
    return existing;
  }
  if (updated_region.is_whole()) {
    return update;
  }

  const std::optional<Value> existing_value = dense_value_from_output(existing);
  const std::optional<Value> update_value = dense_value_from_output(update);
  const std::optional<ImageFacet> existing_facet =
      existing_value.has_value() ? existing_value->image_facet() : std::nullopt;
  const std::optional<ImageFacet> update_facet =
      update_value.has_value() ? update_value->image_facet() : std::nullopt;
  const bool facets_match =
      existing_facet.has_value() == update_facet.has_value() &&
      (!existing_facet.has_value() || *existing_facet == *update_facet);
  if (!existing_value.has_value() || !update_value.has_value() ||
      !(existing_value->dense_tensor_descriptor() ==
        update_value->dense_tensor_descriptor()) ||
      !facets_match) {
    return std::nullopt;
  }

  const DenseTensorDescriptor descriptor =
      update_value->dense_tensor_descriptor();
  const std::optional<ImageFacet> image_facet = update_facet;
  validate_merge_region(updated_region, descriptor, image_facet);
  const auto [layout, storage_size] = contiguous_layout_and_size(descriptor);
  const std::size_t element_bytes = dense_tensor_element_bytes(descriptor);
  std::vector<std::byte> storage(storage_size, std::byte{0});
  DenseTensorView existing_view(*existing_value);
  DenseTensorView update_view(*update_value);
  std::vector<std::size_t> coordinates(descriptor.shape.size(), 0U);

  const std::size_t element_count = storage_size / element_bytes;
  for (std::size_t index = 0U; index < element_count; ++index) {
    const DenseTensorView& source =
        merge_coordinate_selected(updated_region, image_facet, coordinates)
            ? update_view
            : existing_view;
    std::memcpy(storage.data() + checked_multiply(index, element_bytes),
                source.element_data(coordinates), element_bytes);

    for (std::size_t reverse = coordinates.size(); reverse > 0U; --reverse) {
      const std::size_t axis = reverse - 1U;
      ++coordinates[axis];
      if (coordinates[axis] < descriptor.shape[axis]) {
        break;
      }
      coordinates[axis] = 0U;
    }
  }

  NodeOutput merged = update;
  merged.image_value = Value::from_cpu_dense_tensor(descriptor, image_facet,
                                                    layout, std::move(storage));
  if (image_facet.has_value()) {
    merged.image_buffer = snapshot_cpu_image_buffer(merged.image_value);
  } else {
    merged.image_buffer = ImageBuffer{};
  }
  return merged;
}

}  // namespace ps::value_image_adapter
