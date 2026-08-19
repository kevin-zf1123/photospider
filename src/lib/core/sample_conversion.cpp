#include "photospider/data/sample_conversion.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "photospider/data/image_view.hpp"

namespace ps {
namespace {

/**
 * @brief Multiplies byte-domain values without unsigned wraparound.
 * @param left First nonnegative factor.
 * @param right Second nonnegative factor.
 * @return Exact product.
 * @throws std::overflow_error when the product exceeds size_t.
 */
std::size_t checked_multiply(std::size_t left, std::size_t right) {
  if (right != 0U && left > std::numeric_limits<std::size_t>::max() / right) {
    throw std::overflow_error("Sample conversion byte count overflowed.");
  }
  return left * right;
}

/**
 * @brief Adds byte-domain values without unsigned wraparound.
 * @param left First nonnegative term.
 * @param right Second nonnegative term.
 * @return Exact sum.
 * @throws std::overflow_error when the sum exceeds size_t.
 */
std::size_t checked_add(std::size_t left, std::size_t right) {
  if (left > std::numeric_limits<std::size_t>::max() - right) {
    throw std::overflow_error("Sample conversion byte offset overflowed.");
  }
  return left + right;
}

/**
 * @brief Validates one closed policy enum.
 * @param conversion Candidate complete request.
 * @return Nothing.
 * @throws std::invalid_argument when any policy enum is unknown.
 */
void validate_policy_enums(const SampleConversion& conversion) {
  switch (conversion.out_of_domain) {
    case OutOfDomainPolicy::Reject:
    case OutOfDomainPolicy::Clamp:
      break;
    default:
      throw std::invalid_argument("Unknown out-of-domain policy.");
  }
  switch (conversion.rounding) {
    case SampleRoundingMode::NearestEven:
    case SampleRoundingMode::TowardZero:
    case SampleRoundingMode::Floor:
    case SampleRoundingMode::Ceil:
      break;
    default:
      throw std::invalid_argument("Unknown sample rounding mode.");
  }
  switch (conversion.non_finite) {
    case NonFinitePolicy::Reject:
    case NonFinitePolicy::Preserve:
      break;
    default:
      throw std::invalid_argument("Unknown non-finite sample policy.");
  }
  switch (conversion.precision_loss) {
    case PrecisionLossPolicy::Reject:
    case PrecisionLossPolicy::Allow:
      break;
    default:
      throw std::invalid_argument("Unknown precision-loss policy.");
  }
}

/**
 * @brief Validates one versioned finite conversion endpoint.
 * @param endpoint Candidate endpoint.
 * @return Nothing.
 * @throws std::invalid_argument for unknown versions/kinds or invalid bounds.
 */
void validate_endpoint(const SampleEndpoint& endpoint) {
  if (endpoint.encoding.structural_version != 1U ||
      !std::isfinite(endpoint.domain.minimum) ||
      !std::isfinite(endpoint.domain.maximum) ||
      endpoint.domain.minimum > endpoint.domain.maximum) {
    throw std::invalid_argument("Sample conversion endpoint is malformed.");
  }
  switch (endpoint.encoding.kind) {
    case SampleEncodingKind::Value:
    case SampleEncodingKind::Normalized:
    case SampleEncodingKind::CodeValue:
      break;
    default:
      throw std::invalid_argument("Unknown sample encoding kind.");
  }
  switch (endpoint.domain.kind) {
    case SampleDomainKind::Normalized:
    case SampleDomainKind::Legal:
    case SampleDomainKind::CodeValue:
      break;
    default:
      throw std::invalid_argument("Unknown sample domain kind.");
  }
}

/**
 * @brief Loads one native scalar without alignment assumptions.
 * @tparam Scalar Native scalar type.
 * @param address Borrowed readable bytes.
 * @return Exact scalar promoted to long double.
 * @throws Nothing.
 */
template <typename Scalar>
long double load_native(const std::byte* address) noexcept {
  Scalar scalar{};
  std::memcpy(&scalar, address, sizeof(scalar));
  return static_cast<long double>(scalar);
}

/**
 * @brief Loads one supported whole-byte DenseTensor scalar.
 * @param address Borrowed readable scalar bytes.
 * @param descriptor Valid native-scalar descriptor.
 * @return Numeric value promoted to long double.
 * @throws std::invalid_argument for an unsupported semantic/width pair.
 */
long double load_scalar(const std::byte* address,
                        const DenseTensorDescriptor& descriptor) {
  if (descriptor.storage_encoding.kind != StorageEncodingKind::NativeScalar) {
    throw std::invalid_argument(
        "Sample conversion requires native whole-byte storage.");
  }
  const std::uint32_t width = descriptor.storage_encoding.bit_width;
  switch (descriptor.element_semantics) {
    case ElementSemantics::UnsignedInteger:
      switch (width) {
        case 8U:
          return load_native<std::uint8_t>(address);
        case 16U:
          return load_native<std::uint16_t>(address);
        case 32U:
          return load_native<std::uint32_t>(address);
        case 64U:
          return load_native<std::uint64_t>(address);
        default:
          break;
      }
      break;
    case ElementSemantics::SignedInteger:
      switch (width) {
        case 8U:
          return load_native<std::int8_t>(address);
        case 16U:
          return load_native<std::int16_t>(address);
        case 32U:
          return load_native<std::int32_t>(address);
        case 64U:
          return load_native<std::int64_t>(address);
        default:
          break;
      }
      break;
    case ElementSemantics::FloatingPoint:
      if (width == 32U) {
        return load_native<float>(address);
      }
      if (width == 64U) {
        return load_native<double>(address);
      }
      break;
  }
  throw std::invalid_argument("Unsupported sample source storage.");
}

/**
 * @brief Stores one native scalar without alignment assumptions.
 * @tparam Scalar Native scalar type.
 * @param address Borrowed writable bytes.
 * @param value Already range-checked value.
 * @return Numeric value actually stored, promoted back to long double.
 * @throws Nothing.
 */
template <typename Scalar>
long double store_native(std::byte* address, long double value) noexcept {
  const Scalar scalar = static_cast<Scalar>(value);
  std::memcpy(address, &scalar, sizeof(scalar));
  return static_cast<long double>(scalar);
}

/**
 * @brief Applies deterministic integral rounding.
 * @param value Finite value to round.
 * @param mode Explicit closed rounding mode.
 * @return Integral-valued long double.
 * @throws std::invalid_argument for an unknown mode.
 * @note Nearest-even does not inspect or mutate the process floating-point
 *       environment.
 */
long double round_integral(long double value, SampleRoundingMode mode) {
  switch (mode) {
    case SampleRoundingMode::TowardZero:
      return std::trunc(value);
    case SampleRoundingMode::Floor:
      return std::floor(value);
    case SampleRoundingMode::Ceil:
      return std::ceil(value);
    case SampleRoundingMode::NearestEven: {
      const long double lower = std::floor(value);
      const long double fraction = value - lower;
      if (fraction < 0.5L) {
        return lower;
      }
      if (fraction > 0.5L) {
        return lower + 1.0L;
      }
      const long double half = lower / 2.0L;
      return std::floor(half) == half ? lower : lower + 1.0L;
    }
  }
  throw std::invalid_argument("Unknown sample rounding mode.");
}

/**
 * @brief Stores one converted scalar under the complete loss policy.
 * @param address Borrowed destination bytes.
 * @param descriptor Valid destination descriptor.
 * @param mapped Converted numeric value before storage quantization.
 * @param source_after_policy Finite source value after explicit clamp, or the
 *        preserved non-finite source value.
 * @param conversion Complete explicit policy.
 * @return Nothing after exact checked storage.
 * @throws std::domain_error for forbidden range or precision loss.
 * @throws std::invalid_argument for unsupported destination storage.
 */
void store_scalar(std::byte* address, const DenseTensorDescriptor& descriptor,
                  long double mapped, long double source_after_policy,
                  const SampleConversion& conversion) {
  const StorageRepresentableRange range =
      storage_representable_range(descriptor);
  long double stored = mapped;
  if (!std::isfinite(mapped)) {
    if (conversion.non_finite != NonFinitePolicy::Preserve ||
        descriptor.element_semantics != ElementSemantics::FloatingPoint ||
        (std::isnan(mapped) && !range.supports_nan) ||
        (std::isinf(mapped) && mapped > 0.0L &&
         !range.supports_positive_infinity) ||
        (std::isinf(mapped) && mapped < 0.0L &&
         !range.supports_negative_infinity)) {
      throw std::domain_error(
          "Destination storage cannot preserve a non-finite sample.");
    }
  } else if (descriptor.element_semantics != ElementSemantics::FloatingPoint) {
    stored = round_integral(mapped, conversion.rounding);
  }
  if (std::isfinite(stored) &&
      (stored < static_cast<long double>(range.finite_minimum) ||
       stored > static_cast<long double>(range.finite_maximum))) {
    throw std::domain_error(
        "Converted sample exceeds destination storage range.");
  }
  const std::uint32_t width = descriptor.storage_encoding.bit_width;
  switch (descriptor.element_semantics) {
    case ElementSemantics::UnsignedInteger:
      switch (width) {
        case 8U:
          stored = store_native<std::uint8_t>(address, stored);
          break;
        case 16U:
          stored = store_native<std::uint16_t>(address, stored);
          break;
        case 32U:
          stored = store_native<std::uint32_t>(address, stored);
          break;
        case 64U:
          stored = store_native<std::uint64_t>(address, stored);
          break;
        default:
          throw std::invalid_argument("Unsupported unsigned destination.");
      }
      break;
    case ElementSemantics::SignedInteger:
      switch (width) {
        case 8U:
          stored = store_native<std::int8_t>(address, stored);
          break;
        case 16U:
          stored = store_native<std::int16_t>(address, stored);
          break;
        case 32U:
          stored = store_native<std::int32_t>(address, stored);
          break;
        case 64U:
          stored = store_native<std::int64_t>(address, stored);
          break;
        default:
          throw std::invalid_argument("Unsupported signed destination.");
      }
      break;
    case ElementSemantics::FloatingPoint:
      if (width == 32U) {
        stored = store_native<float>(address, stored);
      } else if (width == 64U) {
        stored = store_native<double>(address, stored);
      } else {
        throw std::invalid_argument("Unsupported floating destination.");
      }
      break;
  }
  const bool exact_destination =
      (std::isnan(mapped) && std::isnan(stored)) || stored == mapped;
  bool exact_reverse = exact_destination;
  if (std::isfinite(source_after_policy) && std::isfinite(stored)) {
    const long double source_minimum = conversion.source.domain.minimum;
    const long double source_maximum = conversion.source.domain.maximum;
    const long double destination_minimum =
        conversion.destination.domain.minimum;
    const long double destination_maximum =
        conversion.destination.domain.maximum;
    const long double reversed =
        destination_maximum == destination_minimum
            ? stored
            : source_minimum +
                  (stored - destination_minimum) *
                      ((source_maximum - source_minimum) /
                       (destination_maximum - destination_minimum));
    exact_reverse = reversed == source_after_policy;
  }
  if ((!exact_destination || !exact_reverse) &&
      conversion.precision_loss == PrecisionLossPolicy::Reject) {
    throw std::domain_error("Sample conversion would lose numeric precision.");
  }
}

/**
 * @brief Builds a tight interleaved Layout for one ordinary image descriptor.
 * @param descriptor Valid output tensor descriptor.
 * @param facet Valid output ImageFacet.
 * @return Layout with channel-contiguous pixels and tight positive rows.
 * @throws std::overflow_error when byte arithmetic cannot be represented.
 * @throws std::invalid_argument when a stride exceeds ptrdiff_t.
 * @throws std::bad_alloc when stride storage cannot allocate.
 */
StridedLayout make_interleaved_layout(const DenseTensorDescriptor& descriptor,
                                      const ImageFacet& facet) {
  const std::size_t element_bytes = dense_tensor_element_bytes(descriptor);
  const std::size_t channels = facet.channel_axis.has_value()
                                   ? descriptor.shape[*facet.channel_axis]
                                   : 1U;
  const std::size_t pixel_bytes = checked_multiply(channels, element_bytes);
  const std::size_t row_bytes =
      checked_multiply(descriptor.shape[facet.x_axis], pixel_bytes);
  const std::size_t maximum_stride =
      static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
  if (element_bytes > maximum_stride || pixel_bytes > maximum_stride ||
      row_bytes > maximum_stride) {
    throw std::invalid_argument(
        "Sample conversion layout exceeds signed stride range.");
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
  return layout;
}

/**
 * @brief Computes an exact positive-layout byte envelope.
 * @param descriptor Valid positive shape.
 * @param layout Matching positive Layout.
 * @return Exact highest addressed element end.
 * @throws std::overflow_error when arithmetic cannot be represented.
 */
std::size_t storage_size(const DenseTensorDescriptor& descriptor,
                         const StridedLayout& layout) {
  std::size_t result = dense_tensor_element_bytes(descriptor);
  for (std::size_t axis = 0U; axis < descriptor.shape.size(); ++axis) {
    result = checked_add(
        result,
        checked_multiply(descriptor.shape[axis] - 1U,
                         static_cast<std::size_t>(layout.byte_strides[axis])));
  }
  return result;
}

/**
 * @brief Advances one complete row-major tensor coordinate.
 * @param coordinates Mutable complete coordinate.
 * @param shape Positive tensor shape.
 * @return True after advancing; false after the final coordinate wraps.
 * @throws Nothing under matching-rank preconditions.
 */
bool advance_coordinate(std::vector<std::size_t>* coordinates,
                        const std::vector<std::size_t>& shape) noexcept {
  for (std::size_t reverse = shape.size(); reverse > 0U; --reverse) {
    const std::size_t axis = reverse - 1U;
    ++(*coordinates)[axis];
    if ((*coordinates)[axis] < shape[axis]) {
      return true;
    }
    (*coordinates)[axis] = 0U;
  }
  return false;
}

/**
 * @brief Computes one destination address from a validated positive Layout.
 * @param base Writable allocation base.
 * @param coordinates Complete in-bounds coordinate.
 * @param layout Matching positive Layout.
 * @return Writable element start.
 * @throws std::overflow_error when offset arithmetic cannot be represented.
 */
std::byte* destination_address(std::byte* base,
                               const std::vector<std::size_t>& coordinates,
                               const StridedLayout& layout) {
  std::size_t offset = layout.byte_offset;
  for (std::size_t axis = 0U; axis < coordinates.size(); ++axis) {
    offset = checked_add(
        offset,
        checked_multiply(coordinates[axis],
                         static_cast<std::size_t>(layout.byte_strides[axis])));
  }
  return base + offset;
}

/**
 * @brief Applies source-domain policy and exact affine endpoint mapping.
 * @param source Numeric source sample.
 * @param conversion Complete explicit policy.
 * @return Effective source value and mapped numeric destination sample.
 * @throws std::domain_error for forbidden exceptional/out-of-domain input.
 */
std::pair<long double, long double> map_sample(
    long double source, const SampleConversion& conversion) {
  if (!std::isfinite(source)) {
    if (conversion.non_finite != NonFinitePolicy::Preserve) {
      throw std::domain_error("Sample conversion rejected a non-finite value.");
    }
    return {source, source};
  }
  const long double source_minimum = conversion.source.domain.minimum;
  const long double source_maximum = conversion.source.domain.maximum;
  if (source < source_minimum || source > source_maximum) {
    if (conversion.out_of_domain == OutOfDomainPolicy::Reject) {
      throw std::domain_error(
          "Sample conversion rejected an out-of-domain value.");
    }
    source = std::clamp(source, source_minimum, source_maximum);
  }
  const long double destination_minimum = conversion.destination.domain.minimum;
  const long double destination_maximum = conversion.destination.domain.maximum;
  if (source_maximum == source_minimum) {
    return {source, source};
  }
  return {source, destination_minimum +
                      (source - source_minimum) *
                          ((destination_maximum - destination_minimum) /
                           (source_maximum - source_minimum))};
}

}  // namespace

/** @copydoc convert_dense_image_samples */
Value convert_dense_image_samples(const Value& source,
                                  const SampleConversion& conversion) {
  validate_policy_enums(conversion);
  validate_endpoint(conversion.source);
  validate_endpoint(conversion.destination);
  const bool source_degenerate =
      conversion.source.domain.minimum == conversion.source.domain.maximum;
  const bool destination_degenerate = conversion.destination.domain.minimum ==
                                      conversion.destination.domain.maximum;
  if ((source_degenerate || destination_degenerate) &&
      (!(conversion.source.encoding == conversion.destination.encoding) ||
       !(conversion.source.domain == conversion.destination.domain))) {
    throw std::invalid_argument(
        "Degenerate sample domains require an equal-domain identity transfer.");
  }
  if (!source.valid() ||
      source.representation_kind() != ValueRepresentationKind::DenseTensor ||
      source.storage_layout_kind() != StorageLayoutKind::Strided ||
      !source.image_facet().has_value()) {
    throw std::invalid_argument(
        "Sample conversion requires a Strided ordinary DenseImage Value.");
  }
  const ImageFacet& source_facet = *source.image_facet();
  if (!source_facet.sample_domain.has_value() ||
      !source_facet.sample_domain->per_channel.empty() ||
      !(source_facet.sample_domain->encoding == conversion.source.encoding) ||
      !(source_facet.sample_domain->default_domain ==
        conversion.source.domain)) {
    throw std::invalid_argument(
        "Sample conversion source endpoint does not match Value metadata.");
  }
  DenseTensorDescriptor destination_descriptor =
      source.dense_tensor_descriptor();
  destination_descriptor.element_semantics =
      conversion.destination_element_semantics;
  destination_descriptor.storage_encoding =
      conversion.destination_storage_encoding;
  destination_descriptor.quantization.reset();
  (void)dense_tensor_element_bytes(destination_descriptor);
  ImageFacet destination_facet = source_facet;
  destination_facet.sample_domain =
      SampleDomainFacet{1U,
                        conversion.destination.encoding,
                        conversion.destination.domain,
                        {}};
  validate_dense_tensor_image_metadata(destination_descriptor,
                                       destination_facet);
  const StridedLayout destination_layout =
      make_interleaved_layout(destination_descriptor, destination_facet);
  const std::size_t destination_size =
      storage_size(destination_descriptor, destination_layout);

  ImageView source_image(source);
  (void)source_image;
  DenseTensorView source_tensor(source);
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      destination_descriptor, destination_facet, destination_layout,
      destination_size);
  {
    WriteLease write = builder.acquire_write();
    std::memset(write.data(), 0, write.size());
    std::vector<std::size_t> coordinates(destination_descriptor.shape.size(),
                                         0U);
    do {
      const long double input =
          load_scalar(source_tensor.element_data(coordinates),
                      source.dense_tensor_descriptor());
      const auto [source_after_policy, mapped] = map_sample(input, conversion);
      store_scalar(
          destination_address(write.data(), coordinates, destination_layout),
          destination_descriptor, mapped, source_after_policy, conversion);
    } while (advance_coordinate(&coordinates, destination_descriptor.shape));
  }
  return builder.seal();
}

}  // namespace ps
