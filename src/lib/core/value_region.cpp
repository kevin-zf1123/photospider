#include "core/value_region.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace ps::value_region {
namespace {

/**
 * @brief Multiplies nonnegative size components without wraparound.
 * @param left First factor.
 * @param right Second factor.
 * @return Exact product.
 * @throws std::overflow_error when the result exceeds size_t.
 */
std::size_t checked_multiply(std::size_t left, std::size_t right) {
  if (right != 0U && left > std::numeric_limits<std::size_t>::max() / right) {
    throw std::overflow_error("Dense Value byte arithmetic exceeds size_t.");
  }
  return left * right;
}

/**
 * @brief Adds nonnegative size components without wraparound.
 * @param left First term.
 * @param right Second term.
 * @return Exact sum.
 * @throws std::overflow_error when the result exceeds size_t.
 */
std::size_t checked_add(std::size_t left, std::size_t right) {
  if (left > std::numeric_limits<std::size_t>::max() - right) {
    throw std::overflow_error("Dense Value byte arithmetic exceeds size_t.");
  }
  return left + right;
}

/**
 * @brief Borrows the canonical dense output when present.
 * @param output Output whose image port is inspected.
 * @return Retained Value copy, or nullopt when absent.
 * @throws Nothing for ordinary immutable Value copying.
 */
std::optional<Value> dense_value_from_output(const NodeOutput& output) {
  return output.has_image_value() ? std::optional<Value>(output.image_value())
                                  : std::nullopt;
}

/**
 * @brief Validates one exact merge selection against dense logical facts.
 * @param region Selection to validate.
 * @param descriptor Shared dense descriptor.
 * @param image_facet Optional shared ordinary-image interpretation.
 * @return Nothing after validation.
 * @throws std::invalid_argument for unsupported domains, ranks, or bounds.
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
          "ImageRect output merge requires ordinary-image metadata.");
    }
    const ImageBounds& bounds = image_facet->data_window;
    if (image->x_begin < bounds.x_begin || image->x_end > bounds.x_end ||
        image->y_begin < bounds.y_begin || image->y_end > bounds.y_end) {
      throw std::invalid_argument(
          "ImageRect output merge exceeds the image data window.");
    }
    return;
  }
  const auto* tensor = std::get_if<TensorSlice>(&atom);
  if (tensor == nullptr || !(tensor->domain == dense_tensor_region_domain()) ||
      tensor->axes.size() != descriptor.shape.size()) {
    throw std::invalid_argument(
        "TensorSlice output merge requires matching dense domain and rank.");
  }
  for (std::size_t axis = 0U; axis < tensor->axes.size(); ++axis) {
    if (tensor->axes[axis].end > descriptor.shape[axis]) {
      throw std::invalid_argument(
          "TensorSlice output merge exceeds the dense output bounds.");
    }
  }
}

/**
 * @brief Tests whether one coordinate belongs to a validated selection.
 * @param region Previously validated selection.
 * @param image_facet Optional image interpretation.
 * @param coordinates Complete logical tensor coordinate.
 * @return True for Whole or a coordinate inside the exact atom.
 * @throws Nothing under validate_merge_region preconditions.
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
    const ImageBounds& bounds = image_facet->data_window;
    const std::int64_t x =
        bounds.x_begin +
        static_cast<std::int64_t>(coordinates[image_facet->x_axis]);
    const std::int64_t y =
        bounds.y_begin +
        static_cast<std::int64_t>(coordinates[image_facet->y_axis]);
    return x >= image->x_begin && x < image->x_end && y >= image->y_begin &&
           y < image->y_end;
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
 * @brief Builds a contiguous row-major layout for one dense descriptor.
 * @param descriptor Valid positive-rank descriptor.
 * @return Positive layout and exact storage size.
 * @throws std::overflow_error when storage arithmetic is unrepresentable.
 * @throws std::bad_alloc when stride storage cannot allocate.
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
      throw std::overflow_error("Dense merge stride exceeds ptrdiff_t.");
    }
    layout.byte_strides[axis] = static_cast<std::ptrdiff_t>(stride);
    stride = checked_multiply(stride, descriptor.shape[axis]);
  }
  return {std::move(layout), stride};
}

/**
 * @brief Builds canonical interleaved storage for an ordinary image.
 * @param descriptor Valid whole-byte dense descriptor.
 * @param facet Valid image axes and data window.
 * @return Positive layout and exact tight storage size.
 * @throws std::invalid_argument for non-singleton unassigned axes.
 * @throws std::overflow_error when byte arithmetic is unrepresentable.
 * @throws std::bad_alloc when stride storage cannot allocate.
 */
std::pair<StridedLayout, std::size_t> interleaved_layout_and_size(
    const DenseTensorDescriptor& descriptor, const ImageFacet& facet) {
  const std::size_t element_bytes = dense_tensor_element_bytes(descriptor);
  const std::size_t channels = facet.channel_axis.has_value()
                                   ? descriptor.shape[*facet.channel_axis]
                                   : 1U;
  const std::size_t pixel_bytes = checked_multiply(channels, element_bytes);
  const std::size_t row_bytes =
      checked_multiply(image_bounds_width(facet.data_window), pixel_bytes);
  const std::size_t storage_size =
      checked_multiply(image_bounds_height(facet.data_window), row_bytes);
  const std::size_t maximum =
      static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
  if (element_bytes > maximum || pixel_bytes > maximum || row_bytes > maximum) {
    throw std::overflow_error("Dense image merge stride exceeds ptrdiff_t.");
  }
  StridedLayout layout;
  layout.byte_strides.assign(descriptor.shape.size(),
                             static_cast<std::ptrdiff_t>(element_bytes));
  layout.byte_strides[facet.x_axis] = static_cast<std::ptrdiff_t>(pixel_bytes);
  layout.byte_strides[facet.y_axis] = static_cast<std::ptrdiff_t>(row_bytes);
  if (facet.channel_axis.has_value()) {
    layout.byte_strides[*facet.channel_axis] =
        static_cast<std::ptrdiff_t>(element_bytes);
  }
  for (std::size_t axis = 0U; axis < descriptor.shape.size(); ++axis) {
    const bool assigned =
        axis == facet.x_axis || axis == facet.y_axis ||
        (facet.channel_axis.has_value() && axis == *facet.channel_axis);
    if (!assigned && descriptor.shape[axis] != 1U) {
      throw std::invalid_argument(
          "Dense image merge requires singleton non-image axes.");
    }
  }
  return {std::move(layout), storage_size};
}

/**
 * @brief Computes one checked positive-layout coordinate offset.
 * @param coordinates Rank-matched logical coordinates.
 * @param layout Positive layout created by this module.
 * @return Exact allocation-relative byte offset.
 * @throws std::overflow_error when offset arithmetic is unrepresentable.
 */
std::size_t coordinate_offset(const std::vector<std::size_t>& coordinates,
                              const StridedLayout& layout) {
  std::size_t offset = 0U;
  for (std::size_t axis = 0U; axis < coordinates.size(); ++axis) {
    offset = checked_add(
        offset,
        checked_multiply(coordinates[axis],
                         static_cast<std::size_t>(layout.byte_strides[axis])));
  }
  return offset;
}

/**
 * @brief Builds the complete TensorSlice for one dense descriptor.
 * @param descriptor Valid finite descriptor.
 * @return Exact rank-general Region covering every logical element.
 * @throws std::overflow_error when an extent exceeds uint64.
 * @throws std::bad_alloc when Region storage cannot allocate.
 */
RegionSet full_dense_tensor_region(const DenseTensorDescriptor& descriptor) {
  std::vector<RegionInterval> axes;
  axes.reserve(descriptor.shape.size());
  for (const std::size_t extent : descriptor.shape) {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
      if (extent > std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Dense extent exceeds Region bounds.");
      }
    }
    axes.push_back({0U, static_cast<std::uint64_t>(extent)});
  }
  return RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), std::move(axes)});
}

}  // namespace

/** @copydoc full_node_output_region */
RegionSet full_node_output_region(const NodeOutput& output) {
  if (!output.has_image_value()) {
    return RegionSet::whole();
  }
  const Value& value = output.image_value();
  if (value.image_facet().has_value()) {
    const ImageBounds& bounds = value.image_bounds();
    return RegionSet::from_image_rect({image_region_domain(), bounds.x_begin,
                                       bounds.x_end, bounds.y_begin,
                                       bounds.y_end});
  }
  return full_dense_tensor_region(value.dense_tensor_descriptor());
}

/** @copydoc node_output_region_is_complete */
bool node_output_region_is_complete(const NodeOutput& output,
                                    const RegionSet& region) {
  const RegionSet full = full_node_output_region(output);
  if (region_contains(region, full) == RegionContainmentStatus::Contains) {
    return true;
  }
  if (!output.has_image_value()) {
    return false;
  }
  const RegionSet tensor_full =
      full_dense_tensor_region(output.image_value().dense_tensor_descriptor());
  return region_contains(region, tensor_full) ==
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
  if (!existing_value.has_value() || !update_value.has_value() ||
      !(existing_value->dense_tensor_descriptor() ==
        update_value->dense_tensor_descriptor()) ||
      existing_facet.has_value() != update_facet.has_value() ||
      (existing_facet.has_value() && !(*existing_facet == *update_facet))) {
    return std::nullopt;
  }

  const DenseTensorDescriptor descriptor =
      update_value->dense_tensor_descriptor();
  validate_merge_region(updated_region, descriptor, update_facet);
  auto layout_and_size =
      update_facet.has_value()
          ? interleaved_layout_and_size(descriptor, *update_facet)
          : contiguous_layout_and_size(descriptor);
  StridedLayout layout = std::move(layout_and_size.first);
  const std::size_t storage_size = layout_and_size.second;
  const std::size_t element_bytes = dense_tensor_element_bytes(descriptor);
  DenseTensorView existing_view(*existing_value);
  DenseTensorView update_view(*update_value);
  std::vector<std::size_t> coordinates(descriptor.shape.size(), 0U);
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      descriptor, update_facet, layout, storage_size);
  {
    WriteLease write = builder.acquire_write();
    const std::size_t element_count = std::accumulate(
        descriptor.shape.begin(), descriptor.shape.end(), std::size_t{1U},
        [](std::size_t count, std::size_t extent) {
          return checked_multiply(count, extent);
        });
    for (std::size_t index = 0U; index < element_count; ++index) {
      const DenseTensorView& source =
          merge_coordinate_selected(updated_region, update_facet, coordinates)
              ? update_view
              : existing_view;
      std::memcpy(write.data() + coordinate_offset(coordinates, layout),
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
  }

  NodeOutput merged = update;
  merged.replace_image_value(builder.seal());
  return merged;
}

}  // namespace ps::value_region
