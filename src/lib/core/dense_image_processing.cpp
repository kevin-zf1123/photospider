#include "core/dense_image_processing.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "photospider/data/image_view.hpp"

namespace ps::dense_image_processing {
namespace {

/**
 * @brief Complete validated metadata and contiguous storage facts for output.
 * @throws std::bad_alloc when copied descriptor, Facet, or layout storage
 * fails.
 * @note The record owns no allocation, producer, revision, fence, or pointer.
 */
struct PreparedImage final {
  /** @brief Complete output logical descriptor. */
  DenseTensorDescriptor descriptor;
  /** @brief Complete output ordinary-image interpretation. */
  ImageFacet facet;
  /** @brief Exact positive output layout. */
  StridedLayout layout;
  /** @brief Exact positive allocation size. */
  std::size_t storage_size = 0U;
  /** @brief Positive output width. */
  std::size_t width = 0U;
  /** @brief Positive output height. */
  std::size_t height = 0U;
  /** @brief Positive output channel count. */
  std::size_t channels = 0U;
  /** @brief Positive whole-byte scalar size. */
  std::size_t element_bytes = 0U;
  /** @brief Exact interleaved pixel size. */
  std::size_t pixel_bytes = 0U;
  /** @brief Exact contiguous row size. */
  std::size_t row_bytes = 0U;
};

/**
 * @brief Multiplies positive byte/shape components with overflow checking.
 * @param left First component.
 * @param right Second component.
 * @return Exact product.
 * @throws std::overflow_error when the product exceeds size_t.
 */
std::size_t checked_multiply(std::size_t left, std::size_t right) {
  if (right != 0U && left > std::numeric_limits<std::size_t>::max() / right) {
    throw std::overflow_error("Dense image byte multiplication overflowed.");
  }
  return left * right;
}

/**
 * @brief Converts one validated image extent to the processing PixelSize ABI.
 * @param view Retaining ordinary-image view.
 * @return Positive bounded width and height.
 * @throws std::overflow_error when either dimension exceeds signed int.
 * @note No payload bytes are read beyond ImageView construction.
 */
PixelSize checked_pixel_size(const ImageView& view) {
  const std::size_t maximum =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  if (view.width() > maximum || view.height() > maximum) {
    throw std::overflow_error(
        "Dense image extent exceeds the PixelSize processing ABI.");
  }
  return PixelSize{static_cast<int>(view.width()),
                   static_cast<int>(view.height())};
}

/**
 * @brief Adds a positive extent to one signed data-window origin.
 * @param origin Signed half-open window origin.
 * @param extent Positive representable extent.
 * @return Exact exclusive endpoint.
 * @throws std::overflow_error when the endpoint exceeds int64.
 */
std::int64_t checked_window_end(std::int64_t origin, std::size_t extent) {
  if (extent >
          static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
      origin > std::numeric_limits<std::int64_t>::max() -
                   static_cast<std::int64_t>(extent)) {
    throw std::overflow_error("Dense image data-window endpoint overflowed.");
  }
  return origin + static_cast<std::int64_t>(extent);
}

/**
 * @brief Builds one contiguous interleaved output preserving image meaning.
 * @param source Valid retaining image view.
 * @param destination_size Positive output extent.
 * @param destination_channels Optional replacement channel count.
 * @return Complete validated output construction facts.
 * @throws std::invalid_argument for unsupported axes, encoding, or metadata.
 * @throws std::overflow_error or std::bad_alloc for owned layout construction.
 * @note Channel-count replacement deliberately rejects semantic channel facts
 * rather than inventing new stable channel/group/color identities.
 */
PreparedImage prepare_output(
    const ImageView& source, const PixelSize& destination_size,
    std::optional<std::size_t> destination_channels = std::nullopt) {
  if (destination_size.width <= 0 || destination_size.height <= 0) {
    throw std::invalid_argument(
        "Dense image processing requires a positive destination extent.");
  }
  PreparedImage output;
  output.descriptor = source.descriptor();
  output.facet = source.image_facet();
  if (output.descriptor.quantization.has_value() ||
      output.descriptor.storage_encoding.kind !=
          StorageEncodingKind::NativeScalar) {
    throw std::invalid_argument(
        "Dense image processing requires unquantized native storage.");
  }
  output.width = static_cast<std::size_t>(destination_size.width);
  output.height = static_cast<std::size_t>(destination_size.height);
  output.channels = destination_channels.value_or(source.channels());
  if (output.channels == 0U || output.channels > kMaximumImageChannels) {
    throw std::invalid_argument(
        "Dense image processing channel count is invalid.");
  }
  if (!output.facet.channel_axis.has_value() && output.channels != 1U) {
    throw std::invalid_argument(
        "Multi-channel output requires an explicit channel axis.");
  }
  if (destination_channels.has_value() &&
      output.channels != source.channels()) {
    if (output.facet.channel_schema.has_value() ||
        output.facet.color.has_value() ||
        (output.facet.sample_domain.has_value() &&
         !output.facet.sample_domain->per_channel.empty())) {
      throw std::invalid_argument(
          "Channel conversion requires an explicit semantic mapping.");
    }
  }

  output.descriptor.shape[output.facet.x_axis] = output.width;
  output.descriptor.shape[output.facet.y_axis] = output.height;
  if (output.facet.channel_axis.has_value()) {
    output.descriptor.shape[*output.facet.channel_axis] = output.channels;
  }
  output.facet.data_window.x_end =
      checked_window_end(output.facet.data_window.x_begin, output.width);
  output.facet.data_window.y_end =
      checked_window_end(output.facet.data_window.y_begin, output.height);
  validate_dense_tensor_image_metadata(output.descriptor, output.facet);

  output.element_bytes = dense_tensor_element_bytes(output.descriptor);
  output.pixel_bytes = checked_multiply(output.channels, output.element_bytes);
  output.row_bytes = checked_multiply(output.width, output.pixel_bytes);
  output.storage_size = checked_multiply(output.height, output.row_bytes);
  if (output.element_bytes > static_cast<std::size_t>(
                                 std::numeric_limits<std::ptrdiff_t>::max()) ||
      output.pixel_bytes > static_cast<std::size_t>(
                               std::numeric_limits<std::ptrdiff_t>::max()) ||
      output.row_bytes > static_cast<std::size_t>(
                             std::numeric_limits<std::ptrdiff_t>::max())) {
    throw std::overflow_error("Dense image output stride exceeds ptrdiff_t.");
  }
  output.layout.byte_strides.assign(
      output.descriptor.shape.size(),
      static_cast<std::ptrdiff_t>(output.element_bytes));
  output.layout.byte_strides[output.facet.x_axis] =
      static_cast<std::ptrdiff_t>(output.pixel_bytes);
  output.layout.byte_strides[output.facet.y_axis] =
      static_cast<std::ptrdiff_t>(output.row_bytes);
  if (output.facet.channel_axis.has_value()) {
    output.layout.byte_strides[*output.facet.channel_axis] =
        static_cast<std::ptrdiff_t>(output.element_bytes);
  }
  for (std::size_t axis = 0U; axis < output.descriptor.shape.size(); ++axis) {
    const bool assigned = axis == output.facet.x_axis ||
                          axis == output.facet.y_axis ||
                          (output.facet.channel_axis.has_value() &&
                           axis == *output.facet.channel_axis);
    if (!assigned && output.descriptor.shape[axis] != 1U) {
      throw std::invalid_argument(
          "Dense image processing requires singleton non-image axes.");
    }
  }
  return output;
}

/**
 * @brief Returns one mutable destination scalar address.
 * @param bytes Borrowed complete output allocation.
 * @param output Prepared contiguous output facts.
 * @param x Zero-based column.
 * @param y Zero-based row.
 * @param channel Zero-based channel.
 * @return Pointer to the exact scalar bytes.
 * @throws Nothing after prepared bounds and caller loop validation.
 */
std::byte* destination_address(std::byte* bytes, const PreparedImage& output,
                               std::size_t x, std::size_t y,
                               std::size_t channel) noexcept {
  return bytes + y * output.row_bytes + x * output.pixel_bytes +
         channel * output.element_bytes;
}

/**
 * @brief Loads one native scalar without alignment assumptions.
 * @param address Borrowed scalar bytes.
 * @param descriptor Valid native scalar descriptor.
 * @return Scalar converted to the interpolation domain.
 * @throws std::invalid_argument for an unsupported element combination.
 */
long double load_scalar(const std::byte* address,
                        const DenseTensorDescriptor& descriptor) {
  const std::uint32_t bits = descriptor.storage_encoding.bit_width;
  switch (descriptor.element_semantics) {
    case ElementSemantics::UnsignedInteger:
      if (bits == 8U) {
        std::uint8_t value = 0U;
        std::memcpy(&value, address, sizeof(value));
        return value;
      }
      if (bits == 16U) {
        std::uint16_t value = 0U;
        std::memcpy(&value, address, sizeof(value));
        return value;
      }
      break;
    case ElementSemantics::SignedInteger:
      if (bits == 8U) {
        std::int8_t value = 0;
        std::memcpy(&value, address, sizeof(value));
        return value;
      }
      if (bits == 16U) {
        std::int16_t value = 0;
        std::memcpy(&value, address, sizeof(value));
        return value;
      }
      break;
    case ElementSemantics::FloatingPoint:
      if (bits == 32U) {
        float value = 0.0F;
        std::memcpy(&value, address, sizeof(value));
        return value;
      }
      if (bits == 64U) {
        double value = 0.0;
        std::memcpy(&value, address, sizeof(value));
        return value;
      }
      break;
  }
  throw std::invalid_argument(
      "Dense image processing element storage is unsupported.");
}

/**
 * @brief Clamps and rounds one interpolation result for integral storage.
 * @tparam Scalar Signed or unsigned maintained integer scalar.
 * @param value Interpolated numeric value.
 * @return Nearest representable scalar with half cases away from zero.
 * @throws Nothing.
 */
template <typename Scalar>
Scalar clamp_integral(long double value) noexcept {
  static_assert(std::is_integral<Scalar>::value,
                "Dense image scalar must be integral");
  const long double minimum =
      static_cast<long double>(std::numeric_limits<Scalar>::lowest());
  const long double maximum =
      static_cast<long double>(std::numeric_limits<Scalar>::max());
  return static_cast<Scalar>(std::round(std::clamp(value, minimum, maximum)));
}

/**
 * @brief Stores one native scalar without alignment assumptions.
 * @param address Borrowed mutable scalar bytes.
 * @param descriptor Valid native scalar descriptor.
 * @param value Numeric value to store.
 * @return Nothing.
 * @throws std::invalid_argument for an unsupported element combination.
 */
void store_scalar(std::byte* address, const DenseTensorDescriptor& descriptor,
                  long double value) {
  const std::uint32_t bits = descriptor.storage_encoding.bit_width;
  switch (descriptor.element_semantics) {
    case ElementSemantics::UnsignedInteger:
      if (bits == 8U) {
        const std::uint8_t result = clamp_integral<std::uint8_t>(value);
        std::memcpy(address, &result, sizeof(result));
        return;
      }
      if (bits == 16U) {
        const std::uint16_t result = clamp_integral<std::uint16_t>(value);
        std::memcpy(address, &result, sizeof(result));
        return;
      }
      break;
    case ElementSemantics::SignedInteger:
      if (bits == 8U) {
        const std::int8_t result = clamp_integral<std::int8_t>(value);
        std::memcpy(address, &result, sizeof(result));
        return;
      }
      if (bits == 16U) {
        const std::int16_t result = clamp_integral<std::int16_t>(value);
        std::memcpy(address, &result, sizeof(result));
        return;
      }
      break;
    case ElementSemantics::FloatingPoint:
      if (bits == 32U) {
        const float result = static_cast<float>(value);
        std::memcpy(address, &result, sizeof(result));
        return;
      }
      if (bits == 64U) {
        const double result = static_cast<double>(value);
        std::memcpy(address, &result, sizeof(result));
        return;
      }
      break;
  }
  throw std::invalid_argument(
      "Dense image processing element storage is unsupported.");
}

/**
 * @brief Validates one nonempty zero-based ROI against an image extent.
 * @param roi Candidate rectangle.
 * @param width Positive enclosing width.
 * @param height Positive enclosing height.
 * @return Nothing after checked containment.
 * @throws std::out_of_range for empty, negative, or escaping rectangles.
 */
void validate_roi(const PixelRect& roi, std::size_t width, std::size_t height) {
  if (roi.x < 0 || roi.y < 0 || roi.width <= 0 || roi.height <= 0 ||
      static_cast<std::size_t>(roi.x) > width ||
      static_cast<std::size_t>(roi.y) > height ||
      static_cast<std::size_t>(roi.width) >
          width - static_cast<std::size_t>(roi.x) ||
      static_cast<std::size_t>(roi.height) >
          height - static_cast<std::size_t>(roi.y)) {
    throw std::out_of_range(
        "Dense image processing ROI is outside its extent.");
  }
}

/**
 * @brief Bilinear indices and upper-sample weight for one bounded axis.
 * @throws Nothing for ordinary value operations.
 */
struct InterpolationCoordinate final {
  /** @brief Lower zero-based coordinate. */
  std::size_t lower = 0U;
  /** @brief Upper zero-based coordinate. */
  std::size_t upper = 0U;
  /** @brief Weight assigned to the upper coordinate. */
  long double upper_weight = 0.0L;
};

/**
 * @brief Resolves one half-pixel coordinate inside a selected source span.
 * @param position Continuous zero-based coordinate.
 * @param begin Inclusive source span origin.
 * @param extent Positive source span length.
 * @return Border-replicated indices and interpolation weight.
 * @throws Nothing after caller ROI validation.
 */
InterpolationCoordinate interpolation_coordinate(long double position,
                                                 std::size_t begin,
                                                 std::size_t extent) noexcept {
  const std::size_t last = begin + extent - 1U;
  if (extent <= 1U || position <= static_cast<long double>(begin)) {
    return InterpolationCoordinate{begin, begin, 0.0L};
  }
  if (position >= static_cast<long double>(last)) {
    return InterpolationCoordinate{last, last, 0.0L};
  }
  const std::size_t lower = static_cast<std::size_t>(std::floor(position));
  return InterpolationCoordinate{lower, lower + 1U,
                                 position - static_cast<long double>(lower)};
}

/**
 * @brief Fills one prepared output ROI with a bilinear source ROI mapping.
 * @param source Valid retained source view.
 * @param source_roi Valid source storage ROI.
 * @param output Prepared destination facts.
 * @param destination_roi Valid destination storage ROI.
 * @param destination Borrowed mutable complete output allocation.
 * @return Nothing after every destination sample is written.
 * @throws Scalar storage validation failures unchanged.
 */
void resize_into(const ImageView& source, const PixelRect& source_roi,
                 const PreparedImage& output, const PixelRect& destination_roi,
                 std::byte* destination) {
  const long double scale_x =
      static_cast<long double>(source_roi.width) / destination_roi.width;
  const long double scale_y =
      static_cast<long double>(source_roi.height) / destination_roi.height;
  for (int relative_y = 0; relative_y < destination_roi.height; ++relative_y) {
    const long double source_y =
        static_cast<long double>(source_roi.y) +
        (static_cast<long double>(relative_y) + 0.5L) * scale_y - 0.5L;
    const InterpolationCoordinate y = interpolation_coordinate(
        source_y, static_cast<std::size_t>(source_roi.y),
        static_cast<std::size_t>(source_roi.height));
    for (int relative_x = 0; relative_x < destination_roi.width; ++relative_x) {
      const long double source_x =
          static_cast<long double>(source_roi.x) +
          (static_cast<long double>(relative_x) + 0.5L) * scale_x - 0.5L;
      const InterpolationCoordinate x = interpolation_coordinate(
          source_x, static_cast<std::size_t>(source_roi.x),
          static_cast<std::size_t>(source_roi.width));
      for (std::size_t channel = 0U; channel < output.channels; ++channel) {
        const long double top_left =
            load_scalar(source.channel_data(x.lower, y.lower, channel),
                        source.descriptor());
        const long double top_right =
            load_scalar(source.channel_data(x.upper, y.lower, channel),
                        source.descriptor());
        const long double bottom_left =
            load_scalar(source.channel_data(x.lower, y.upper, channel),
                        source.descriptor());
        const long double bottom_right =
            load_scalar(source.channel_data(x.upper, y.upper, channel),
                        source.descriptor());
        const long double top =
            top_left + (top_right - top_left) * x.upper_weight;
        const long double bottom =
            bottom_left + (bottom_right - bottom_left) * x.upper_weight;
        store_scalar(
            destination_address(
                destination, output,
                static_cast<std::size_t>(destination_roi.x + relative_x),
                static_cast<std::size_t>(destination_roi.y + relative_y),
                channel),
            output.descriptor, top + (bottom - top) * y.upper_weight);
      }
    }
  }
}

/**
 * @brief Returns the explicit opaque-alpha value for maintained storage.
 * @param descriptor Valid native scalar descriptor.
 * @return One for floating storage, otherwise the physical integer maximum.
 * @throws std::invalid_argument for an unsupported element combination.
 * @note This is the positional B/G/R/A algorithm contract, not sample-domain
 * inference or a file-codec conversion policy.
 */
long double opaque_alpha(const DenseTensorDescriptor& descriptor) {
  const std::uint32_t bits = descriptor.storage_encoding.bit_width;
  switch (descriptor.element_semantics) {
    case ElementSemantics::UnsignedInteger:
      if (bits == 8U)
        return std::numeric_limits<std::uint8_t>::max();
      if (bits == 16U)
        return std::numeric_limits<std::uint16_t>::max();
      break;
    case ElementSemantics::SignedInteger:
      if (bits == 8U)
        return std::numeric_limits<std::int8_t>::max();
      if (bits == 16U)
        return std::numeric_limits<std::int16_t>::max();
      break;
    case ElementSemantics::FloatingPoint:
      if (bits == 32U || bits == 64U)
        return 1.0L;
      break;
  }
  throw std::invalid_argument("Dense image alpha storage is unsupported.");
}

/**
 * @brief Tests one declared interval against a synthesized raw constant.
 * @param domain Valid finite inclusive declared interval.
 * @param constant Finite raw constant produced by normalization.
 * @return True only when `constant` is inside the inclusive endpoints.
 * @throws Nothing after metadata validation and maintained scalar selection.
 */
bool contains_raw_constant(const SampleDomain& domain,
                           long double constant) noexcept {
  return std::isfinite(constant) &&
         constant >= static_cast<long double>(domain.minimum) &&
         constant <= static_cast<long double>(domain.maximum);
}

/**
 * @brief Tests every applicable declaration against one raw constant.
 * @param facet Valid default and optional per-channel sample declarations.
 * @param constant Finite raw constant produced by normalization.
 * @return True only when the default and every override contain `constant`.
 * @throws Nothing for bounded metadata iteration.
 * @note Checking every override is conservative for geometry padding and
 * prevents a whole-facet authority claim when any channel excludes the value.
 */
bool contains_raw_constant(const SampleDomainFacet& facet,
                           long double constant) noexcept {
  return contains_raw_constant(facet.default_domain, constant) &&
         std::all_of(facet.per_channel.begin(), facet.per_channel.end(),
                     [constant](const ChannelSampleDomain& override_domain) {
                       return contains_raw_constant(override_domain.domain,
                                                    constant);
                     });
}

/**
 * @brief Restores the complete caller floating-point environment by RAII.
 * @throws std::runtime_error when capture or round-mode selection fails.
 * @note Destruction terminates on restoration failure because returning with a
 * contaminated arithmetic environment would violate the exact algorithm.
 */
class ScopedRoundToNearest final {
 public:
  /**
   * @brief Captures the environment and selects round-to-nearest.
   * @throws std::runtime_error when platform fenv operations fail.
   */
  ScopedRoundToNearest() {
    if (fegetenv(&saved_) != 0) {
      throw std::runtime_error(
          "Exact box average could not capture floating environment.");
    }
    captured_ = true;
    if (fesetround(FE_TONEAREST) != 0) {
      if (fesetenv(&saved_) != 0) {
        std::terminate();
      }
      captured_ = false;
      throw std::runtime_error(
          "Exact box average could not select round-to-nearest.");
    }
  }

  /** @brief Restores the captured environment or terminates on failure. */
  ~ScopedRoundToNearest() noexcept {
    if (captured_ && fesetenv(&saved_) != 0) {
      std::terminate();
    }
  }

  /** @brief Forbids duplicate restoration ownership. */
  ScopedRoundToNearest(const ScopedRoundToNearest&) = delete;
  /** @brief Forbids duplicate restoration assignment. */
  ScopedRoundToNearest& operator=(const ScopedRoundToNearest&) = delete;

 private:
  /** @brief Complete saved platform environment. */
  fenv_t saved_{};
  /** @brief Whether destruction must restore `saved_`. */
  bool captured_ = false;
};

}  // namespace

/** @copydoc project_normalized_sample_domain */
std::optional<SampleDomainFacet> project_normalized_sample_domain(
    const DenseTensorDescriptor& source_descriptor,
    const ImageFacet& source_facet, const PixelSize& destination_size,
    std::size_t destination_channels, SizeNormalizationMode size_mode) {
  validate_dense_tensor_image_metadata(source_descriptor, source_facet);
  if (source_descriptor.quantization.has_value() ||
      source_descriptor.storage_encoding.kind !=
          StorageEncodingKind::NativeScalar) {
    throw std::invalid_argument(
        "Dense image normalization proof requires native unquantized storage.");
  }
  if (destination_size.width <= 0 || destination_size.height <= 0 ||
      destination_channels == 0U ||
      destination_channels > kMaximumImageChannels) {
    throw std::invalid_argument(
        "Dense image normalization requires bounded positive output facts.");
  }
  const std::size_t source_width = image_bounds_width(source_facet.data_window);
  const std::size_t source_height =
      image_bounds_height(source_facet.data_window);
  const std::size_t source_channels =
      source_facet.channel_axis.has_value()
          ? source_descriptor.shape[*source_facet.channel_axis]
          : 1U;
  const std::size_t destination_width =
      static_cast<std::size_t>(destination_size.width);
  const std::size_t destination_height =
      static_cast<std::size_t>(destination_size.height);
  switch (size_mode) {
    case SizeNormalizationMode::Unchanged:
    case SizeNormalizationMode::Resize:
    case SizeNormalizationMode::CropOrPad:
      break;
    default:
      throw std::invalid_argument(
          "Dense image size normalization mode is unsupported.");
  }
  if (size_mode == SizeNormalizationMode::Unchanged &&
      (source_width != destination_width ||
       source_height != destination_height)) {
    throw std::invalid_argument(
        "Unchanged dense image normalization cannot alter the extent.");
  }
  if (!source_facet.sample_domain.has_value()) {
    return std::nullopt;
  }

  const SampleDomainFacet& samples = *source_facet.sample_domain;
  const bool synthesizes_zero =
      size_mode == SizeNormalizationMode::CropOrPad &&
      (destination_width > source_width || destination_height > source_height);
  if (synthesizes_zero && !contains_raw_constant(samples, 0.0L)) {
    return std::nullopt;
  }
  const bool synthesizes_opaque_alpha =
      source_channels == 3U && destination_channels == 4U;
  if (synthesizes_opaque_alpha &&
      !contains_raw_constant(samples, opaque_alpha(source_descriptor))) {
    return std::nullopt;
  }
  return samples;
}

/** @copydoc clone */
Value clone(const Value& source) {
  const ImageView view(source);
  const PreparedImage output = prepare_output(view, checked_pixel_size(view));
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      output.descriptor, output.facet, output.layout, output.storage_size);
  {
    WriteLease write = builder.acquire_write();
    for (std::size_t y = 0U; y < output.height; ++y) {
      for (std::size_t x = 0U; x < output.width; ++x) {
        for (std::size_t channel = 0U; channel < output.channels; ++channel) {
          std::memcpy(destination_address(write.data(), output, x, y, channel),
                      view.channel_data(x, y, channel), output.element_bytes);
        }
      }
    }
  }
  return builder.seal();
}

/** @copydoc resize */
Value resize(const Value& source, const PixelSize& destination_size) {
  const ImageView view(source);
  const PixelSize source_size = checked_pixel_size(view);
  const PixelRect source_roi{0, 0, source_size.width, source_size.height};
  const PixelRect destination_roi{0, 0, destination_size.width,
                                  destination_size.height};
  return resize_region(source, source_roi, destination_size, destination_roi);
}

/** @copydoc resize_region */
Value resize_region(const Value& source, const PixelRect& source_roi,
                    const PixelSize& destination_size,
                    const PixelRect& destination_roi) {
  const ImageView view(source);
  validate_roi(source_roi, view.width(), view.height());
  if (destination_size.width <= 0 || destination_size.height <= 0) {
    throw std::invalid_argument(
        "Dense image resize requires a positive destination extent.");
  }
  validate_roi(destination_roi,
               static_cast<std::size_t>(destination_size.width),
               static_cast<std::size_t>(destination_size.height));
  const PreparedImage output = prepare_output(view, destination_size);
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      output.descriptor, output.facet, output.layout, output.storage_size);
  {
    WriteLease write = builder.acquire_write();
    std::memset(write.data(), 0, write.size());
    resize_into(view, source_roi, output, destination_roi, write.data());
  }
  return builder.seal();
}

/** @copydoc crop_or_pad */
Value crop_or_pad(const Value& source, const PixelSize& destination_size) {
  const ImageView view(source);
  PreparedImage output = prepare_output(view, destination_size);
  output.facet.sample_domain = project_normalized_sample_domain(
      view.descriptor(), view.image_facet(), destination_size, view.channels(),
      SizeNormalizationMode::CropOrPad);
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      output.descriptor, output.facet, output.layout, output.storage_size);
  {
    WriteLease write = builder.acquire_write();
    std::memset(write.data(), 0, write.size());
    const std::size_t copied_width = std::min(view.width(), output.width);
    const std::size_t copied_height = std::min(view.height(), output.height);
    for (std::size_t y = 0U; y < copied_height; ++y) {
      for (std::size_t x = 0U; x < copied_width; ++x) {
        for (std::size_t channel = 0U; channel < output.channels; ++channel) {
          std::memcpy(destination_address(write.data(), output, x, y, channel),
                      view.channel_data(x, y, channel), output.element_bytes);
        }
      }
    }
  }
  return builder.seal();
}

/** @copydoc convert_channels */
Value convert_channels(const Value& source, std::size_t destination_channels) {
  const ImageView view(source);
  const std::size_t source_channels = view.channels();
  if (source_channels == destination_channels) {
    return clone(source);
  }
  const bool supported =
      (source_channels == 1U &&
       (destination_channels == 3U || destination_channels == 4U)) ||
      ((source_channels == 3U || source_channels == 4U) &&
       destination_channels == 1U) ||
      (source_channels == 4U && destination_channels == 3U) ||
      (source_channels == 3U && destination_channels == 4U);
  if (!supported) {
    throw std::invalid_argument(
        "Dense image channel conversion is unsupported.");
  }
  PreparedImage output =
      prepare_output(view, checked_pixel_size(view), destination_channels);
  output.facet.sample_domain = project_normalized_sample_domain(
      view.descriptor(), view.image_facet(), checked_pixel_size(view),
      destination_channels, SizeNormalizationMode::Unchanged);
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      output.descriptor, output.facet, output.layout, output.storage_size);
  {
    WriteLease write = builder.acquire_write();
    for (std::size_t y = 0U; y < output.height; ++y) {
      for (std::size_t x = 0U; x < output.width; ++x) {
        if (source_channels == 1U) {
          const long double gray =
              load_scalar(view.channel_data(x, y, 0U), view.descriptor());
          for (std::size_t channel = 0U; channel < destination_channels;
               ++channel) {
            store_scalar(
                destination_address(write.data(), output, x, y, channel),
                output.descriptor, gray);
          }
          continue;
        }

        const long double blue =
            load_scalar(view.channel_data(x, y, 0U), view.descriptor());
        const long double green =
            load_scalar(view.channel_data(x, y, 1U), view.descriptor());
        const long double red =
            load_scalar(view.channel_data(x, y, 2U), view.descriptor());
        if (destination_channels == 1U) {
          store_scalar(destination_address(write.data(), output, x, y, 0U),
                       output.descriptor,
                       blue * 0.114L + green * 0.587L + red * 0.299L);
          continue;
        }
        store_scalar(destination_address(write.data(), output, x, y, 0U),
                     output.descriptor, blue);
        store_scalar(destination_address(write.data(), output, x, y, 1U),
                     output.descriptor, green);
        store_scalar(destination_address(write.data(), output, x, y, 2U),
                     output.descriptor, red);
        if (destination_channels == 4U) {
          store_scalar(destination_address(write.data(), output, x, y, 3U),
                       output.descriptor, opaque_alpha(output.descriptor));
        }
      }
    }
  }
  return builder.seal();
}

/** @copydoc exact_box_average_factor_four */
Value exact_box_average_factor_four(const Value& source,
                                    const PixelRect& destination_roi) {
  const ImageView view(source);
  if (view.descriptor().element_semantics != ElementSemantics::FloatingPoint ||
      view.descriptor().storage_encoding.kind !=
          StorageEncodingKind::NativeScalar ||
      view.descriptor().storage_encoding.bit_width != 32U ||
      view.width() % 4U != 0U || view.height() % 4U != 0U) {
    throw std::invalid_argument(
        "Exact box average requires factor-four FP32 image storage.");
  }
  const std::size_t destination_width = view.width() / 4U;
  const std::size_t destination_height = view.height() / 4U;
  if (destination_width >
          static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      destination_height >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::overflow_error(
        "Exact box average destination exceeds PixelSize.");
  }
  validate_roi(destination_roi, destination_width, destination_height);
  const PreparedImage output =
      prepare_output(view, PixelSize{static_cast<int>(destination_width),
                                     static_cast<int>(destination_height)});
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      output.descriptor, output.facet, output.layout, output.storage_size);
  {
    WriteLease write = builder.acquire_write();
    std::memset(write.data(), 0, write.size());
    ScopedRoundToNearest round_to_nearest;
    for (int relative_y = 0; relative_y < destination_roi.height;
         ++relative_y) {
      const std::size_t destination_y =
          static_cast<std::size_t>(destination_roi.y + relative_y);
      for (int relative_x = 0; relative_x < destination_roi.width;
           ++relative_x) {
        const std::size_t destination_x =
            static_cast<std::size_t>(destination_roi.x + relative_x);
        for (std::size_t channel = 0U; channel < output.channels; ++channel) {
          double sum = 0.0;
          for (std::size_t block_y = 0U; block_y < 4U; ++block_y) {
            for (std::size_t block_x = 0U; block_x < 4U; ++block_x) {
              sum += static_cast<double>(load_scalar(
                  view.channel_data(destination_x * 4U + block_x,
                                    destination_y * 4U + block_y, channel),
                  view.descriptor()));
            }
          }
          const float mean = static_cast<float>(sum * 0.0625);
          std::memcpy(destination_address(write.data(), output, destination_x,
                                          destination_y, channel),
                      &mean, sizeof(mean));
        }
      }
    }
  }
  return builder.seal();
}

}  // namespace ps::dense_image_processing
