#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/data/image_view.hpp"

namespace ps {
namespace {

/**
 * @brief Multiplies byte-envelope components with explicit overflow checking.
 *
 * @param left First non-negative component.
 * @param right Second non-negative component.
 * @return Exact product.
 * @throws std::overflow_error when the product exceeds std::size_t.
 * @note The helper performs no allocation and accepts zero operands.
 */
std::size_t checked_multiply(std::size_t left, std::size_t right) {
  if (right != 0U && left > std::numeric_limits<std::size_t>::max() / right) {
    throw std::overflow_error("DenseTensor byte envelope overflows size_t.");
  }
  return left * right;
}

/**
 * @brief Adds byte-envelope components with explicit overflow checking.
 *
 * @param left First non-negative component.
 * @param right Second non-negative component.
 * @return Exact sum.
 * @throws std::overflow_error when the sum exceeds std::size_t.
 * @note The helper performs no allocation.
 */
std::size_t checked_add(std::size_t left, std::size_t right) {
  if (left > std::numeric_limits<std::size_t>::max() - right) {
    throw std::overflow_error("DenseTensor byte envelope overflows size_t.");
  }
  return left + right;
}

/**
 * @brief Validates an optional explicit image-axis mapping.
 *
 * @param facet Optional facet to inspect.
 * @param rank Logical DenseTensor rank.
 * @throws std::invalid_argument when an axis is out of rank or duplicated.
 * @note A missing facet is valid for non-image DenseTensor Values.
 */
void validate_image_facet(const std::optional<ImageFacet>& facet,
                          std::size_t rank) {
  if (!facet.has_value()) {
    return;
  }
  if (facet->x_axis >= rank || facet->y_axis >= rank) {
    throw std::invalid_argument("ImageFacet axis is outside tensor rank.");
  }
  if (facet->x_axis == facet->y_axis) {
    throw std::invalid_argument("ImageFacet x and y axes must be distinct.");
  }
  if (facet->channel_axis.has_value() &&
      (*facet->channel_axis >= rank || *facet->channel_axis == facet->x_axis ||
       *facet->channel_axis == facet->y_axis)) {
    throw std::invalid_argument(
        "ImageFacet channel axis must be distinct and in rank.");
  }
}

/**
 * @brief Computes and validates the exact V-2 owned storage envelope.
 *
 * @param descriptor Logical descriptor with positive extents.
 * @param layout Physical layout with one positive stride per axis.
 * @param element_bytes Validated physical element width.
 * @return Highest addressable element end measured from storage base.
 * @throws std::invalid_argument for rank mismatch or a non-positive stride.
 * @throws std::overflow_error when an offset or envelope cannot be represented.
 * @note Positive strides make the sum of per-axis maximum offsets the complete
 *       upper address bound even when a layout aliases logical elements.
 */
std::size_t required_storage_size(const DenseTensorDescriptor& descriptor,
                                  const StridedLayout& layout,
                                  std::size_t element_bytes) {
  if (layout.byte_strides.size() != descriptor.shape.size()) {
    throw std::invalid_argument(
        "DenseTensor shape and stride ranks must match.");
  }

  std::size_t envelope = element_bytes;
  for (std::size_t axis = 0U; axis < descriptor.shape.size(); ++axis) {
    if (layout.byte_strides[axis] <= 0) {
      throw std::invalid_argument("V-2 DenseTensor strides must be positive.");
    }
    const std::size_t stride =
        static_cast<std::size_t>(layout.byte_strides[axis]);
    const std::size_t maximum_axis_offset =
        checked_multiply(descriptor.shape[axis] - 1U, stride);
    envelope = checked_add(envelope, maximum_axis_offset);
  }
  return envelope;
}

/**
 * @brief Reports whether one axis is explicitly assigned by an image facet.
 *
 * @param facet Valid explicit image-axis mapping.
 * @param axis Logical axis to inspect.
 * @return True when axis is x, y, or the optional channel axis.
 * @throws Nothing.
 */
bool is_image_axis(const ImageFacet& facet, std::size_t axis) noexcept {
  return axis == facet.x_axis || axis == facet.y_axis ||
         (facet.channel_axis.has_value() && axis == *facet.channel_axis);
}

}  // namespace

/**
 * @brief Immutable implementation for one validated CPU DenseTensor Value.
 *
 * @throws std::bad_alloc when vector members are moved from allocators with
 *         incompatible state; current standard allocators move without
 *         allocation.
 * @note Instances are published only through shared_ptr<const Impl>.
 */
struct Value::Impl final {
  /** @brief Validated concrete logical descriptor. */
  DenseTensorDescriptor descriptor;

  /** @brief Optional validated explicit image-axis mapping. */
  std::optional<ImageFacet> image_facet;

  /** @brief Validated positive physical byte strides. */
  StridedLayout layout;

  /** @brief Exact immutable owned CPU byte envelope. */
  std::vector<std::byte> storage;

  /**
   * @brief Moves already validated Value state into immutable ownership.
   *
   * @param descriptor_in Logical descriptor.
   * @param image_facet_in Optional image facet.
   * @param layout_in Physical byte layout.
   * @param storage_in Exact owned byte envelope.
   * @throws std::bad_alloc only for allocator-incompatible vector moves.
   * @note The caller completes validation before construction.
   */
  Impl(DenseTensorDescriptor descriptor_in,
       std::optional<ImageFacet> image_facet_in, StridedLayout layout_in,
       std::vector<std::byte> storage_in)
      : descriptor(std::move(descriptor_in)),
        image_facet(std::move(image_facet_in)),
        layout(std::move(layout_in)),
        storage(std::move(storage_in)) {}
};

/** @copydoc ps::dense_tensor_element_bytes */
std::size_t dense_tensor_element_bytes(
    const DenseTensorDescriptor& descriptor) {
  switch (descriptor.element_semantics) {
    case ElementSemantics::UnsignedInteger:
    case ElementSemantics::SignedInteger:
      if (descriptor.storage_encoding.bit_width == 8U ||
          descriptor.storage_encoding.bit_width == 16U) {
        return descriptor.storage_encoding.bit_width / 8U;
      }
      break;
    case ElementSemantics::FloatingPoint:
      if (descriptor.storage_encoding.bit_width == 32U ||
          descriptor.storage_encoding.bit_width == 64U) {
        return descriptor.storage_encoding.bit_width / 8U;
      }
      break;
  }
  throw std::invalid_argument(
      "DenseTensor element semantics and encoding are unsupported.");
}

/** @copydoc ps::Value::Value */
Value::Value(std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

/** @copydoc ps::Value::from_cpu_dense_tensor */
Value Value::from_cpu_dense_tensor(DenseTensorDescriptor descriptor,
                                   std::optional<ImageFacet> image_facet,
                                   StridedLayout layout,
                                   std::vector<std::byte> storage) {
  if (descriptor.shape.empty()) {
    throw std::invalid_argument("V-2 DenseTensor rank must be positive.");
  }
  for (const std::size_t extent : descriptor.shape) {
    if (extent == 0U) {
      throw std::invalid_argument("DenseTensor extents must all be positive.");
    }
  }
  const std::size_t element_bytes = dense_tensor_element_bytes(descriptor);
  validate_image_facet(image_facet, descriptor.shape.size());
  const std::size_t required =
      required_storage_size(descriptor, layout, element_bytes);
  if (storage.size() != required) {
    throw std::invalid_argument(
        "DenseTensor storage size must equal its exact byte envelope.");
  }

  return Value(std::make_shared<const Impl>(
      std::move(descriptor), std::move(image_facet), std::move(layout),
      std::move(storage)));
}

/** @copydoc ps::Value::valid */
bool Value::valid() const noexcept {
  return impl_ != nullptr;
}

/** @copydoc ps::Value::dense_tensor_descriptor */
const DenseTensorDescriptor& Value::dense_tensor_descriptor() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no DenseTensor descriptor.");
  }
  return impl_->descriptor;
}

/** @copydoc ps::Value::image_facet */
const std::optional<ImageFacet>& Value::image_facet() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no ImageFacet.");
  }
  return impl_->image_facet;
}

/** @copydoc ps::Value::strided_layout */
const StridedLayout& Value::strided_layout() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no StridedLayout.");
  }
  return impl_->layout;
}

/** @copydoc ps::Value::storage_size */
std::size_t Value::storage_size() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no storage.");
  }
  return impl_->storage.size();
}

/** @copydoc ps::Value::data */
const std::byte* Value::data() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no storage.");
  }
  return impl_->storage.data();
}

/** @copydoc ps::DenseTensorView::DenseTensorView */
DenseTensorView::DenseTensorView(Value value) : value_(std::move(value)) {
  if (!value_.valid()) {
    throw std::invalid_argument(
        "DenseTensorView requires a valid DenseTensor Value.");
  }
}

/** @copydoc ps::DenseTensorView::value */
const Value& DenseTensorView::value() const noexcept {
  return value_;
}

/** @copydoc ps::DenseTensorView::descriptor */
const DenseTensorDescriptor& DenseTensorView::descriptor() const noexcept {
  return value_.dense_tensor_descriptor();
}

/** @copydoc ps::DenseTensorView::layout */
const StridedLayout& DenseTensorView::layout() const noexcept {
  return value_.strided_layout();
}

/** @copydoc ps::DenseTensorView::storage_size */
std::size_t DenseTensorView::storage_size() const noexcept {
  return value_.storage_size();
}

/** @copydoc ps::DenseTensorView::data */
const std::byte* DenseTensorView::data() const noexcept {
  return value_.data();
}

/** @copydoc ps::DenseTensorView::element_data */
const std::byte* DenseTensorView::element_data(
    const std::vector<std::size_t>& coordinates) const {
  const DenseTensorDescriptor& tensor_descriptor = descriptor();
  if (coordinates.size() != tensor_descriptor.shape.size()) {
    throw std::invalid_argument(
        "DenseTensor coordinate rank must match tensor rank.");
  }

  std::size_t offset = 0U;
  for (std::size_t axis = 0U; axis < coordinates.size(); ++axis) {
    if (coordinates[axis] >= tensor_descriptor.shape[axis]) {
      throw std::out_of_range("DenseTensor coordinate is outside its extent.");
    }
    offset += coordinates[axis] *
              static_cast<std::size_t>(layout().byte_strides[axis]);
  }
  return data() + offset;
}

/** @copydoc ps::ImageView::ImageView */
ImageView::ImageView(Value value) : tensor_(std::move(value)) {
  const std::optional<ImageFacet>& facet = tensor_.value().image_facet();
  if (!facet.has_value()) {
    throw std::invalid_argument("ImageView requires an explicit ImageFacet.");
  }
  image_facet_ = *facet;

  const DenseTensorDescriptor& tensor_descriptor = tensor_.descriptor();
  for (std::size_t axis = 0U; axis < tensor_descriptor.shape.size(); ++axis) {
    if (!is_image_axis(image_facet_, axis) &&
        tensor_descriptor.shape[axis] != 1U) {
      throw std::invalid_argument(
          "ImageView unassigned tensor axes must be singleton.");
    }
  }

  width_ = tensor_descriptor.shape[image_facet_.x_axis];
  height_ = tensor_descriptor.shape[image_facet_.y_axis];
  channels_ = image_facet_.channel_axis.has_value()
                  ? tensor_descriptor.shape[*image_facet_.channel_axis]
                  : 1U;
  const std::size_t maximum_image_extent =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  if (width_ > maximum_image_extent || height_ > maximum_image_extent ||
      channels_ > maximum_image_extent) {
    throw std::invalid_argument(
        "ImageView extent exceeds the current ImageBuffer adapter domain.");
  }
  element_bytes_ = dense_tensor_element_bytes(tensor_descriptor);
}

/** @copydoc ps::ImageView::value */
const Value& ImageView::value() const noexcept {
  return tensor_.value();
}

/** @copydoc ps::ImageView::descriptor */
const DenseTensorDescriptor& ImageView::descriptor() const noexcept {
  return tensor_.descriptor();
}

/** @copydoc ps::ImageView::image_facet */
const ImageFacet& ImageView::image_facet() const noexcept {
  return image_facet_;
}

/** @copydoc ps::ImageView::layout */
const StridedLayout& ImageView::layout() const noexcept {
  return tensor_.layout();
}

/** @copydoc ps::ImageView::width */
std::size_t ImageView::width() const noexcept {
  return width_;
}

/** @copydoc ps::ImageView::height */
std::size_t ImageView::height() const noexcept {
  return height_;
}

/** @copydoc ps::ImageView::channels */
std::size_t ImageView::channels() const noexcept {
  return channels_;
}

/** @copydoc ps::ImageView::element_bytes */
std::size_t ImageView::element_bytes() const noexcept {
  return element_bytes_;
}

/** @copydoc ps::ImageView::row_stride */
std::ptrdiff_t ImageView::row_stride() const noexcept {
  return layout().byte_strides[image_facet_.y_axis];
}

/** @copydoc ps::ImageView::channel_data */
const std::byte* ImageView::channel_data(std::size_t x, std::size_t y,
                                         std::size_t channel) const {
  if (x >= width_ || y >= height_ || channel >= channels_) {
    throw std::out_of_range("ImageView coordinate is outside its extent.");
  }
  const StridedLayout& tensor_layout = layout();
  std::size_t offset =
      x * static_cast<std::size_t>(
              tensor_layout.byte_strides[image_facet_.x_axis]) +
      y * static_cast<std::size_t>(
              tensor_layout.byte_strides[image_facet_.y_axis]);
  if (image_facet_.channel_axis.has_value()) {
    offset +=
        channel * static_cast<std::size_t>(
                      tensor_layout.byte_strides[*image_facet_.channel_axis]);
  }
  return tensor_.data() + offset;
}

}  // namespace ps
