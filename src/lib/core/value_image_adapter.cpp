#include "core/value_image_adapter.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/host_output_authorization.hpp"
#include "core/pending_value.hpp"
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
 * @brief Retains every owner behind one imported non-CPU ImageBuffer binding.
 *
 * @throws Nothing for destruction; construction moves already-retained shared
 * owners without allocation.
 * @note The enclosing shared control block becomes the single external Value
 * owner. Individual data/context deleters, including plugin lease wrappers,
 * therefore remain alive until the final Value copy retires.
 */
struct ImportedImageBindingOwner final
    : public ExternalBindingCompatibilityProjection {
  /**
   * @brief Takes complete ownership of one inbound compatibility descriptor.
   * @param retained_image Exact opaque backend descriptor and owners.
   * @throws Nothing because shared-owner moves are non-throwing.
   */
  explicit ImportedImageBindingOwner(ImageBuffer retained_image) noexcept
      : image(std::move(retained_image)) {}

  /** @brief Exact opaque descriptor retained through Value retirement. */
  ImageBuffer image;
};

/**
 * @brief Retains one canonical Value behind an ImageBuffer projection.
 * @throws Nothing for construction and destruction.
 * @note Aliasing ImageBuffer owners point at original backend addresses while
 * this state keeps the unique immutable binding and its DSO owners alive.
 */
struct ImageBufferProjectionLifetime final {
  /**
   * @brief Retains the exact canonical source for a callback projection.
   * @param retained_value Ready imported Value to keep alive.
   * @throws Nothing because Value movement is non-throwing.
   */
  explicit ImageBufferProjectionLifetime(Value retained_value) noexcept
      : value(std::move(retained_value)) {}

  /** @brief Canonical Value retained until every projection copy retires. */
  Value value;
};

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
 * @brief Tests whether complete logical image rows are byte-contiguous HWC.
 *
 * @param view Valid host-readable image view.
 * @return True when x advances by one complete pixel and an explicit channel
 *         axis advances by one element; singleton axes impose no stride
 *         requirement because their coordinate never changes.
 * @throws std::overflow_error when one logical pixel width is unrepresentable.
 * @note The y stride is deliberately unrestricted. Positive, zero, and
 *       negative row origins remain safe because DenseTensorView resolves each
 *       logical row start independently before a packed-row copy.
 */
bool has_contiguous_interleaved_rows(const ImageView& view) {
  const std::size_t pixel_bytes =
      checked_multiply(view.channels(), view.element_bytes());
  const ImageFacet& image = view.image_facet();
  const std::vector<std::ptrdiff_t>& strides = view.layout().byte_strides;
  if (view.width() > 1U &&
      (strides[image.x_axis] <= 0 ||
       static_cast<std::size_t>(strides[image.x_axis]) != pixel_bytes)) {
    return false;
  }
  if (image.channel_axis.has_value() && view.channels() > 1U &&
      (strides[*image.channel_axis] <= 0 ||
       static_cast<std::size_t>(strides[*image.channel_axis]) !=
           view.element_bytes())) {
    return false;
  }
  return true;
}

/**
 * @brief Obtains canonical immutable dense authority from one private output.
 * @param output Output whose permanent image port is inspected.
 * @return Existing image Value, or nullopt when the port is absent.
 * @throws Nothing.
 * @note Compatibility staging is deliberately ignored; callers must import it
 * explicitly before entering Value-backed merge or formal-cache paths.
 */
std::optional<Value> dense_value_from_output(const NodeOutput& output) {
  if (output.has_image_value()) {
    return output.image_value();
  }
  return std::nullopt;
}

/**
 * @brief Builds the single DI-2 output plan for an inbound ImageBuffer.
 * @param buffer Valid nonempty owned compatibility image.
 * @return Immutable exact-layout image output plan named `image`.
 * @throws std::invalid_argument for malformed or unrepresentable descriptors.
 * @throws std::overflow_error when byte-envelope arithmetic overflows.
 * @throws std::bad_alloc when plan metadata cannot allocate.
 * @note A zero opaque-device stride means tightly packed rows at this legacy
 * edge. The compatibility allocation is only an import source; the returned
 * plan creates no allocation or identity and never retains the source owner.
 */
DenseImageOutputPlan compatibility_image_plan(const ImageBuffer& buffer) {
  const std::size_t element_bytes = image_buffer_bytes_per_channel(buffer.type);
  const std::size_t pixel_bytes = checked_multiply(
      static_cast<std::size_t>(buffer.channels), element_bytes);
  const std::size_t row_bytes = image_buffer_row_bytes(buffer);
  const std::size_t row_stride = buffer.step == 0U ? row_bytes : buffer.step;
  if (row_stride > static_cast<std::size_t>(
                       std::numeric_limits<std::ptrdiff_t>::max()) ||
      pixel_bytes > static_cast<std::size_t>(
                        std::numeric_limits<std::ptrdiff_t>::max()) ||
      element_bytes > static_cast<std::size_t>(
                          std::numeric_limits<std::ptrdiff_t>::max())) {
    throw std::invalid_argument(
        "ImageBuffer strides exceed the signed output-plan domain.");
  }
  const std::size_t storage_size = checked_add(
      checked_multiply(static_cast<std::size_t>(buffer.height - 1), row_stride),
      row_bytes);
  const auto [semantics, encoding] = dense_element_from_image_type(buffer.type);
  DenseTensorDescriptor descriptor{{static_cast<std::size_t>(buffer.height),
                                    static_cast<std::size_t>(buffer.width),
                                    static_cast<std::size_t>(buffer.channels)},
                                   semantics,
                                   encoding};
  const ImageFacet image = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  StridedLayout layout{{static_cast<std::ptrdiff_t>(row_stride),
                        static_cast<std::ptrdiff_t>(pixel_bytes),
                        static_cast<std::ptrdiff_t>(element_bytes)}};
  return DenseImageOutputPlan::create(std::string(NodeOutput::kImageOutputName),
                                      std::move(descriptor), image,
                                      std::move(layout), storage_size, 64U);
}

/**
 * @brief Validates one merge Region against concrete DenseTensor facts.
 * @param region Exact normalized selection to validate.
 * @param descriptor Shared logical tensor descriptor.
 * @param image_facet Optional complete ordinary-image interpretation.
 * @throws std::invalid_argument for unsupported atom count/domain/kind,
 *         missing image axes, or out-of-data-window ImageRect endpoints,
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
    const ImageBounds& bounds = image_facet->data_window;
    if (image->x_begin < bounds.x_begin || image->x_end > bounds.x_end ||
        image->y_begin < bounds.y_begin || image->y_end > bounds.y_end) {
      throw std::invalid_argument(
          "ImageRect output merge exceeds the image data window.");
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
 * @param image_facet Optional complete ordinary-image interpretation.
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
 * @brief Builds the canonical interleaved layout required by an image plan.
 * @param descriptor Valid whole-byte DenseTensor descriptor.
 * @param image_facet Valid ordinary-image interpretation whose unassigned axes
 * are singleton.
 * @return Exact positive layout and storage size in y/x/channel order.
 * @throws std::invalid_argument when an unassigned axis is non-singleton.
 * @throws std::overflow_error when strides or storage size are unrepresentable.
 * @throws std::bad_alloc when stride storage cannot allocate.
 * @note Logical axis order does not control physical image order; channel is
 * contiguous, followed by x pixels and y rows. Singleton axes receive the
 * element stride and therefore add no bytes to the exact envelope.
 */
std::pair<StridedLayout, std::size_t> interleaved_image_layout_and_size(
    const DenseTensorDescriptor& descriptor, const ImageFacet& image_facet) {
  const std::size_t element_bytes = dense_tensor_element_bytes(descriptor);
  const std::size_t channels = image_facet.channel_axis.has_value()
                                   ? descriptor.shape[*image_facet.channel_axis]
                                   : 1U;
  const std::size_t pixel_bytes = checked_multiply(channels, element_bytes);
  const std::size_t width = image_bounds_width(image_facet.data_window);
  const std::size_t height = image_bounds_height(image_facet.data_window);
  const std::size_t row_bytes = checked_multiply(width, pixel_bytes);
  const std::size_t storage_size = checked_multiply(height, row_bytes);
  if (element_bytes > static_cast<std::size_t>(
                          std::numeric_limits<std::ptrdiff_t>::max()) ||
      pixel_bytes > static_cast<std::size_t>(
                        std::numeric_limits<std::ptrdiff_t>::max()) ||
      row_bytes > static_cast<std::size_t>(
                      std::numeric_limits<std::ptrdiff_t>::max())) {
    throw std::overflow_error(
        "Interleaved image output stride exceeds ptrdiff_t.");
  }

  StridedLayout layout;
  layout.byte_strides.assign(descriptor.shape.size(),
                             static_cast<std::ptrdiff_t>(element_bytes));
  layout.byte_strides[image_facet.x_axis] =
      static_cast<std::ptrdiff_t>(pixel_bytes);
  layout.byte_strides[image_facet.y_axis] =
      static_cast<std::ptrdiff_t>(row_bytes);
  if (image_facet.channel_axis.has_value()) {
    layout.byte_strides[*image_facet.channel_axis] =
        static_cast<std::ptrdiff_t>(element_bytes);
  }
  for (std::size_t axis = 0U; axis < descriptor.shape.size(); ++axis) {
    const bool assigned = axis == image_facet.x_axis ||
                          axis == image_facet.y_axis ||
                          (image_facet.channel_axis.has_value() &&
                           axis == *image_facet.channel_axis);
    if (!assigned && descriptor.shape[axis] != 1U) {
      throw std::invalid_argument(
          "Interleaved image output requires singleton unassigned axes.");
    }
  }
  return {std::move(layout), storage_size};
}

/**
 * @brief Computes one checked producer-layout offset for logical coordinates.
 * @param coordinates Rank-matched coordinates inside the descriptor.
 * @param layout Positive producer layout with zero byte offset.
 * @return Exact allocation-relative byte offset.
 * @throws std::overflow_error when offset arithmetic is unrepresentable.
 * @note Descriptor bounds and positive strides are established by the caller.
 */
std::size_t producer_coordinate_offset(
    const std::vector<std::size_t>& coordinates, const StridedLayout& layout) {
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
  const std::size_t maximum_extent =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  if (view.width() > maximum_extent || view.height() > maximum_extent ||
      view.channels() > maximum_extent) {
    throw std::invalid_argument(
        "ImageBuffer adaptation requires positive-int image extents.");
  }
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
  DenseImageOutputPlan plan = compatibility_image_plan(buffer);
  const std::size_t row_bytes = image_buffer_row_bytes(buffer);
  HostOutputBinding binding = HostOutputBinding::allocate(std::move(plan));
  HostOutputWriteGrant grant = binding.grant_whole();
  std::byte* destination = grant.data(0U);
  for (int row = 0; row < buffer.height; ++row) {
    std::memcpy(destination + static_cast<std::size_t>(row) * buffer.step,
                image_buffer_row_data(buffer, row), row_bytes);
  }
  grant.retire_success();
  return binding.seal();
}

/** @copydoc snapshot_cpu_image_buffer */
ImageBuffer snapshot_cpu_image_buffer(const Value& value) {
  validate_image_buffer_compatible_value(value);
  ImageView view(value);
  DenseTensorView tensor_view(value);
  const DataType type = image_type_from_dense_element(view.descriptor());
  ImageBuffer output = make_aligned_cpu_image_buffer(
      static_cast<int>(view.width()), static_cast<int>(view.height()),
      static_cast<int>(view.channels()), type);

  const std::size_t element_bytes = view.element_bytes();
  const std::size_t channels = view.channels();
  const std::size_t row_bytes =
      checked_multiply(checked_multiply(view.width(), channels), element_bytes);
  const ImageFacet& image = view.image_facet();
  std::vector<std::size_t> coordinates(view.descriptor().shape.size(), 0U);
  auto* output_base = static_cast<std::byte*>(output.data.get());
  if (has_contiguous_interleaved_rows(view)) {
    for (std::size_t y = 0U; y < view.height(); ++y) {
      coordinates[image.y_axis] = y;
      std::memcpy(output_base + y * output.step,
                  tensor_view.element_data(coordinates), row_bytes);
    }
    validate_image_buffer(output);
    return output;
  }

  for (std::size_t y = 0U; y < view.height(); ++y) {
    coordinates[image.y_axis] = y;
    std::byte* output_row = output_base + y * output.step;
    for (std::size_t x = 0U; x < view.width(); ++x) {
      coordinates[image.x_axis] = x;
      for (std::size_t channel = 0U; channel < channels; ++channel) {
        if (image.channel_axis.has_value()) {
          coordinates[*image.channel_axis] = channel;
        }
        const std::size_t element_index =
            checked_add(checked_multiply(x, channels), channel);
        std::memcpy(output_row + checked_multiply(element_index, element_bytes),
                    tensor_view.element_data(coordinates), element_bytes);
      }
    }
  }
  validate_image_buffer(output);
  return output;
}

/** @copydoc project_image_value_for_image_buffer_edge */
ImageBuffer project_image_value_for_image_buffer_edge(const Value& value) {
  if (!value.valid() ||
      value.representation_kind() != ValueRepresentationKind::DenseTensor ||
      value.storage_layout_kind() != StorageLayoutKind::Strided ||
      !value.image_facet().has_value()) {
    throw std::invalid_argument(
        "ImageBuffer projection requires a valid Strided image Value.");
  }
  const StorageBinding binding = value.storage_binding();
  if (binding.host_visible) {
    return snapshot_cpu_image_buffer(value);
  }
  if (binding.memory_domain != MemoryDomain::Imported) {
    throw BufferAccessError();
  }

  const std::shared_ptr<const ExternalBindingCompatibilityProjection>
      projection =
          PendingDeviceValuePublisher::retained_compatibility_projection(value);
  const auto imported =
      std::dynamic_pointer_cast<const ImportedImageBindingOwner>(projection);
  if (!imported) {
    throw BufferAccessError();
  }

  validate_image_buffer(imported->image);
  if (imported->image.device == Device::CPU) {
    throw std::logic_error(
        "Imported ImageBuffer projection unexpectedly describes CPU storage.");
  }
  const DenseImageOutputPlan plan = compatibility_image_plan(imported->image);
  if (!(value.dense_tensor_descriptor() == plan.descriptor()) ||
      !(*value.image_facet() == plan.image_facet()) ||
      !(value.strided_layout() == plan.layout()) ||
      value.storage_size() != plan.storage_size() ||
      binding.byte_size != plan.storage_size() || binding.host_visible ||
      binding.device != DeviceId(device_backend(imported->image.device))) {
    throw std::logic_error(
        "Imported ImageBuffer projection disagrees with canonical Value "
        "facts.");
  }

  ImageBuffer result = imported->image;
  void* data_address = result.data.get();
  void* context_address = result.context.get();
  auto lifetime = std::make_shared<ImageBufferProjectionLifetime>(value);
  result.data = data_address != nullptr
                    ? std::shared_ptr<void>(lifetime, data_address)
                    : std::shared_ptr<void>{};
  result.context = context_address != nullptr
                       ? std::shared_ptr<void>(lifetime, context_address)
                       : std::shared_ptr<void>{};
  validate_image_buffer(result);
  return result;
}

/** @copydoc import_node_output_compatibility_image */
void import_node_output_compatibility_image(NodeOutput* output) {
  if (output == nullptr) {
    throw std::invalid_argument(
        "NodeOutput compatibility import requires a non-null destination.");
  }
  const bool has_staging = output->has_compatibility_image();
  if (output->has_image_value()) {
    if (has_staging) {
      throw std::logic_error(
          "NodeOutput cannot retain canonical and compatibility image peers.");
    }
    return;
  }
  if (!has_staging) {
    return;
  }
  const ImageBuffer& buffer = output->compatibility_image;
  validate_image_buffer(buffer);
  if (buffer.width <= 0 || buffer.height <= 0 || buffer.channels <= 0) {
    throw std::invalid_argument(
        "NodeOutput compatibility import requires a nonempty image.");
  }
  if (buffer.device == Device::CPU) {
    if (!buffer.data) {
      throw std::invalid_argument(
          "NodeOutput CPU compatibility import requires pixel data.");
    }
    Value published = snapshot_cpu_image_value(buffer);
    output->publish_image_value(std::move(published));
    output->compatibility_image = ImageBuffer{};
    return;
  }

  DenseImageOutputPlan plan = compatibility_image_plan(buffer);
  void* native_handle =
      buffer.context ? buffer.context.get() : buffer.data.get();
  auto owner = std::make_shared<ImportedImageBindingOwner>(buffer);
  std::shared_ptr<void> binding_owner = owner;
  std::shared_ptr<const ExternalBindingCompatibilityProjection>
      compatibility_projection = owner;
  PendingDeviceValuePublication publication =
      PendingDeviceValuePublisher::publish_dense_tensor(
          plan.descriptor(), plan.image_facet(), plan.layout(),
          std::move(binding_owner), native_handle, nullptr, plan.storage_size(),
          DeviceId(device_backend(buffer.device)), MemoryDomain::Imported,
          std::nullopt, std::move(compatibility_projection));
  if (!publication.producer.complete_ready()) {
    throw std::logic_error(
        "ImageBuffer device import lost its pending publication authority.");
  }
  output->publish_image_value(std::move(publication.value));
  output->compatibility_image = ImageBuffer{};
}

/** @copydoc full_node_output_region */
RegionSet full_node_output_region(const NodeOutput& output) {
  if (output.has_compatibility_image()) {
    throw std::logic_error(
        "Formal output Region cannot derive from compatibility staging.");
  }
  if (output.has_image_value()) {
    const Value& image_value = output.image_value();
    if (image_value.image_facet().has_value()) {
      const ImageBounds& bounds = image_value.image_bounds();
      return RegionSet::from_image_rect({image_region_domain(), bounds.x_begin,
                                         bounds.x_end, bounds.y_begin,
                                         bounds.y_end});
    }

    return full_dense_tensor_region(image_value.dense_tensor_descriptor());
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
  if (!output.has_image_value()) {
    return false;
  }
  const RegionSet dense_full =
      full_dense_tensor_region(output.image_value().dense_tensor_descriptor());
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
  auto layout_and_size =
      image_facet.has_value()
          ? interleaved_image_layout_and_size(descriptor, *image_facet)
          : contiguous_layout_and_size(descriptor);
  StridedLayout layout = std::move(layout_and_size.first);
  const std::size_t storage_size = layout_and_size.second;
  const std::size_t element_bytes = dense_tensor_element_bytes(descriptor);
  DenseTensorView existing_view(*existing_value);
  DenseTensorView update_view(*update_value);
  std::vector<std::size_t> coordinates(descriptor.shape.size(), 0U);

  const auto populate = [&](std::byte* destination) {
    const std::size_t element_count = std::accumulate(
        descriptor.shape.begin(), descriptor.shape.end(), std::size_t{1U},
        [](std::size_t count, std::size_t extent) {
          return checked_multiply(count, extent);
        });
    for (std::size_t index = 0U; index < element_count; ++index) {
      const DenseTensorView& source =
          merge_coordinate_selected(updated_region, image_facet, coordinates)
              ? update_view
              : existing_view;
      const std::size_t destination_offset =
          producer_coordinate_offset(coordinates, layout);
      std::memcpy(destination + destination_offset,
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
  };

  Value merged_value;
  if (image_facet.has_value()) {
    DenseImageOutputPlan plan = DenseImageOutputPlan::create(
        std::string(NodeOutput::kImageOutputName), descriptor, *image_facet,
        layout, storage_size, 64U);
    HostOutputBinding binding = HostOutputBinding::allocate(std::move(plan));
    HostOutputWriteGrant grant = binding.grant_whole();
    populate(grant.data(0U));
    grant.retire_success();
    merged_value = binding.seal();
  } else {
    ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
        descriptor, std::nullopt, layout, storage_size);
    {
      WriteLease lease = builder.acquire_write();
      populate(lease.data());
    }
    merged_value = builder.seal();
  }

  NodeOutput merged = update;
  if (merged.has_image_value()) {
    merged.replace_image_value(std::move(merged_value));
  } else {
    merged.publish_image_value(std::move(merged_value));
  }
  merged.compatibility_image = ImageBuffer{};
  return merged;
}

}  // namespace ps::value_image_adapter
