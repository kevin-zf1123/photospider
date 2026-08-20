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
#if defined(PHOTOSPIDER_INTERNAL_SAMPLE_CONVERSION_TESTING)
#include "core/sample_conversion_test_access.hpp"
#endif

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
 * @brief Copies one native scalar from possibly unaligned storage.
 * @tparam Scalar Native scalar type.
 * @param address Borrowed readable bytes.
 * @return Exact native scalar.
 * @throws Nothing.
 */
template <typename Scalar>
Scalar read_native(const std::byte* address) noexcept {
  Scalar scalar{};
  std::memcpy(&scalar, address, sizeof(scalar));
  return scalar;
}

/**
 * @brief Reports whether one integer is exactly promotable to `long double`.
 * @tparam Scalar Supported native integer type.
 * @param value Exact integer sample.
 * @return True when every value bit is preserved by the promotion.
 * @throws Nothing.
 * @note The conservative magnitude proof avoids a lossy trial conversion and
 * handles the signed minimum without signed overflow.
 */
template <typename Scalar>
bool exactly_promotable_integer(Scalar value) noexcept {
  static_assert(std::is_integral_v<Scalar>,
                "integer promotion proof requires an integer type");
  using Unsigned = std::make_unsigned_t<Scalar>;
  constexpr int kScalarDigits = std::numeric_limits<Unsigned>::digits;
  constexpr int kLongDoubleDigits = std::numeric_limits<long double>::digits;
  if constexpr (kLongDoubleDigits >= kScalarDigits) {
    return true;
  } else {
    const Unsigned magnitude = [&]() noexcept {
      if constexpr (std::is_signed_v<Scalar>) {
        if (value < 0) {
          return static_cast<Unsigned>(0U) - static_cast<Unsigned>(value);
        }
      }
      return static_cast<Unsigned>(value);
    }();
    const Unsigned exact_limit = Unsigned{1U} << kLongDoubleDigits;
    return magnitude <= exact_limit;
  }
}

/**
 * @brief Promotes one native scalar only when the conversion is provably exact.
 * @tparam Scalar Supported native scalar type.
 * @param address Borrowed readable scalar bytes.
 * @return Exact `long double` representation.
 * @throws std::domain_error when a wide integer cannot be represented exactly
 * by this platform's `long double` arithmetic.
 * @note Floating-point input is already no wider than the supported binary64
 * source storage and therefore converts without losing its source value.
 */
template <typename Scalar>
long double load_native_for_affine(const std::byte* address) {
  const Scalar scalar = read_native<Scalar>(address);
  if constexpr (std::is_integral_v<Scalar>) {
    if (!exactly_promotable_integer(scalar)) {
      throw std::domain_error(
          "Wide integer sample cannot enter exact affine arithmetic on this "
          "platform.");
    }
  }
  return static_cast<long double>(scalar);
}

/**
 * @brief Loads one supported whole-byte DenseTensor scalar.
 * @param address Borrowed readable scalar bytes.
 * @param descriptor Valid native-scalar descriptor.
 * @return Numeric value promoted to long double.
 * @throws std::invalid_argument for an unsupported semantic/width pair.
 * @throws std::domain_error when a wide integer cannot be promoted exactly on
 * this platform for non-identity affine arithmetic.
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
          return load_native_for_affine<std::uint8_t>(address);
        case 16U:
          return load_native_for_affine<std::uint16_t>(address);
        case 32U:
          return load_native_for_affine<std::uint32_t>(address);
        case 64U:
          return load_native_for_affine<std::uint64_t>(address);
        default:
          break;
      }
      break;
    case ElementSemantics::SignedInteger:
      switch (width) {
        case 8U:
          return load_native_for_affine<std::int8_t>(address);
        case 16U:
          return load_native_for_affine<std::int16_t>(address);
        case 32U:
          return load_native_for_affine<std::int32_t>(address);
        case 64U:
          return load_native_for_affine<std::int64_t>(address);
        default:
          break;
      }
      break;
    case ElementSemantics::FloatingPoint:
      if (width == 32U) {
        return load_native_for_affine<float>(address);
      }
      if (width == 64U) {
        return load_native_for_affine<double>(address);
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
 * @brief Stores one finite integral-valued sample after exact open-bound
 * checks.
 * @tparam Scalar Native integer destination type.
 * @param address Borrowed writable scalar bytes.
 * @param value Finite integral-valued sample after deterministic rounding.
 * @return Exact stored integer promoted to `long double`.
 * @throws std::domain_error when the value is outside the integer range.
 * @note The upper bound is exclusive, avoiding a rounded binary floating
 * representation of `UINT64_MAX` or `INT64_MAX` and guaranteeing the final
 * floating-to-integer cast is defined.
 */
template <typename Scalar>
long double store_integral_native(std::byte* address, long double value) {
  static_assert(std::is_integral_v<Scalar>,
                "integral storage requires an integer type");
  constexpr int kValueBits = std::numeric_limits<Scalar>::digits;
  const long double upper_exclusive = std::ldexp(1.0L, kValueBits);
  if constexpr (std::is_unsigned_v<Scalar>) {
    if (value < 0.0L || value >= upper_exclusive) {
      throw std::domain_error(
          "Converted sample exceeds destination storage range.");
    }
  } else {
    if (value < -upper_exclusive || value >= upper_exclusive) {
      throw std::domain_error(
          "Converted sample exceeds destination storage range.");
    }
  }
  return store_native<Scalar>(address, value);
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

#if defined(PHOTOSPIDER_INTERNAL_SAMPLE_CONVERSION_TESTING)
/**
 * @brief Calling-thread working-type selection for deterministic affine tests.
 *
 * @throws Nothing for construction and destruction.
 * @note This state exists only in BUILD_TESTING runtime images. The public
 *       conversion request and production runtime contain no corresponding
 *       field, branch, or process-global mutation.
 */
struct SampleConversionTestState final {
  /** @brief Whether finite affine arithmetic uses binary64 on this thread. */
  bool force_binary64_affine = false;
};

/** @brief Calling thread's source-private sample-conversion test state. */
thread_local SampleConversionTestState sample_conversion_test_state;
#endif

/**
 * @brief Power-of-two normalized form of one finite nondegenerate interval.
 * @tparam Working Floating-point type used by the complete affine operation.
 * @throws Nothing for ordinary aggregate operations.
 * @note Scaling makes the largest endpoint magnitude lie in `[1, 2)` and
 *       therefore keeps the scaled span finite. A narrow subnormal interval
 *       is promoted as a unit, without requiring a wider floating type.
 */
template <typename Working>
struct ScaledInterval final {
  /** @brief Lower endpoint after power-of-two scaling. */
  Working minimum = Working{0};
  /** @brief Upper endpoint after power-of-two scaling. */
  Working maximum = Working{0};
  /** @brief Binary exponent removed from both endpoints. */
  int exponent = 0;
};

/**
 * @brief Scales one finite ordered interval into a bounded exponent domain.
 * @param minimum Finite inclusive lower endpoint.
 * @param maximum Finite inclusive upper endpoint greater than minimum.
 * @return Power-of-two-scaled endpoints and their removed exponent.
 * @throws std::logic_error when validated interval preconditions are broken or
 *         scaling does not retain a finite ordered interval.
 * @note At least one nonzero endpoint exists for a nondegenerate interval, so
 *       `ilogb` is never called with zero. The largest endpoint is scaled
 *       toward unity; exact affine endpoints bypass this helper, and the
 *       scaled interval is checked for retained order before use.
 */
template <typename Working>
ScaledInterval<Working> scale_finite_interval(Working minimum,
                                              Working maximum) {
  static_assert(std::is_floating_point_v<Working>,
                "sample affine work requires a floating-point type");
  if (!std::isfinite(minimum) || !std::isfinite(maximum) ||
      !(minimum < maximum)) {
    throw std::logic_error("Finite sample interval is not ordered.");
  }
  int exponent = std::numeric_limits<int>::min();
  if (minimum != Working{0}) {
    exponent = std::ilogb(std::fabs(minimum));
  }
  if (maximum != Working{0}) {
    exponent = std::max(exponent, std::ilogb(std::fabs(maximum)));
  }
  if (exponent == std::numeric_limits<int>::min()) {
    throw std::logic_error("Finite sample interval has no nonzero endpoint.");
  }
  ScaledInterval<Working> result;
  result.minimum = std::scalbn(minimum, -exponent);
  result.maximum = std::scalbn(maximum, -exponent);
  result.exponent = exponent;
  if (!std::isfinite(result.minimum) || !std::isfinite(result.maximum) ||
      !(result.minimum < result.maximum)) {
    throw std::logic_error("Finite sample interval scaling lost its order.");
  }
  return result;
}

/**
 * @brief Derives one affine scale from independently normalized intervals.
 * @tparam Working Floating-point type used by the complete affine operation.
 * @param source Power-of-two-normalized finite source interval.
 * @param destination Power-of-two-normalized finite destination interval.
 * @return Destination/source scale, including zero or infinity when the exact
 *         quotient is outside the working type's representable range.
 * @throws std::logic_error when a scaled-interval invariant is broken.
 * @note The bounded mantissa quotient is adjusted with the exact exponent
 *       difference. No original interval span is formed, and no source sample
 *       or endpoint distance is scaled before later fused mapping.
 */
template <typename Working>
Working derive_scale_from_scaled_intervals(
    const ScaledInterval<Working>& source,
    const ScaledInterval<Working>& destination) {
  const Working source_span = source.maximum - source.minimum;
  const Working destination_span = destination.maximum - destination.minimum;
  if (!std::isfinite(source_span) || source_span <= Working{0} ||
      !std::isfinite(destination_span) || destination_span <= Working{0}) {
    throw std::logic_error("Scaled sample interval span is invalid.");
  }
  const Working mantissa_scale = destination_span / source_span;
  if (!std::isfinite(mantissa_scale) || mantissa_scale <= Working{0}) {
    throw std::logic_error("Scaled sample interval quotient is invalid.");
  }
  const int exponent_difference = destination.exponent - source.exponent;
  return std::scalbn(mantissa_scale, exponent_difference);
}

/**
 * @brief Interpolates a destination from its nearer endpoint.
 * @tparam Working Floating-point type used by the complete affine operation.
 * @param minimum Finite destination lower endpoint.
 * @param maximum Finite destination upper endpoint greater than minimum.
 * @param fraction Clamped distance fraction in `[0, 1]`.
 * @param from_minimum True to add the fraction from the lower endpoint; false
 *        to subtract it from the upper endpoint.
 * @return Finite destination value rounded by the active working type.
 * @throws std::domain_error when validated finite inputs unexpectedly produce
 *         a non-finite result.
 * @throws std::logic_error when interval preconditions are broken.
 * @note A finite span uses one direct `fma`, so a subnormal result is rounded
 *       only once and cannot be lost through a pre-rounded half-span. Only an
 *       overflowing destination span requires power-of-two endpoint scaling
 *       before `fma` and one final `scalbn` restore.
 */
template <typename Working>
Working interpolate_finite_from_endpoint(Working minimum, Working maximum,
                                         Working fraction, bool from_minimum) {
  const Working bounded_fraction = std::clamp(fraction, Working{0}, Working{1});
  const Working direct_span = maximum - minimum;
  if (std::isfinite(direct_span)) {
    const Working mapped =
        from_minimum ? std::fma(bounded_fraction, direct_span, minimum)
                     : std::fma(-bounded_fraction, direct_span, maximum);
    if (!std::isfinite(mapped)) {
      throw std::domain_error(
          "Finite sample affine interpolation produced a non-finite result.");
    }
    return std::clamp(mapped, minimum, maximum);
  }

  const ScaledInterval<Working> destination =
      scale_finite_interval(minimum, maximum);
  const Working scaled_span = destination.maximum - destination.minimum;
  Working scaled =
      from_minimum
          ? std::fma(bounded_fraction, scaled_span, destination.minimum)
          : std::fma(-bounded_fraction, scaled_span, destination.maximum);
  scaled = std::clamp(scaled, destination.minimum, destination.maximum);
  const Working mapped = std::scalbn(scaled, destination.exponent);
  if (!std::isfinite(mapped)) {
    throw std::domain_error(
        "Finite sample affine interpolation produced a non-finite result.");
  }
  return std::clamp(mapped, minimum, maximum);
}

/**
 * @brief Applies one finite affine scale from the corresponding endpoints.
 * @tparam Working Floating-point type used by the complete affine operation.
 * @param minimum Finite destination lower endpoint.
 * @param maximum Finite destination upper endpoint greater than minimum.
 * @param distance Nonnegative source distance from the matching endpoint.
 * @param scale Positive finite destination-span/source-span ratio.
 * @param from_minimum True to add from the lower endpoints; false to subtract
 *        from the upper endpoints.
 * @return Finite destination value rounded once by fused multiply-add.
 * @throws std::domain_error when validated finite inputs unexpectedly produce
 *         a non-finite result.
 * @note The scale is used only after both spans and their ratio are proven
 *       finite and nonzero. Fusing the distance product with the destination
 *       endpoint preserves a representable final result even when computing
 *       the source fraction first would underflow.
 */
template <typename Working>
Working interpolate_finite_from_endpoint_scale(Working minimum, Working maximum,
                                               Working distance, Working scale,
                                               bool from_minimum) {
  const Working mapped = from_minimum ? std::fma(distance, scale, minimum)
                                      : std::fma(-distance, scale, maximum);
  if (!std::isfinite(mapped)) {
    throw std::domain_error(
        "Finite sample affine scale produced a non-finite result.");
  }
  return std::clamp(mapped, minimum, maximum);
}

/**
 * @brief Maps one sample with a finite nonzero affine scale and raw distance.
 * @tparam Working Floating-point type used by the complete affine operation.
 * @param value Finite in-domain source value that is not an endpoint.
 * @param source_minimum Finite source lower endpoint.
 * @param source_maximum Finite source upper endpoint.
 * @param source_span Direct source span, or infinity when it overflows.
 * @param destination_minimum Finite destination lower endpoint.
 * @param destination_maximum Finite destination upper endpoint.
 * @param scale Positive finite destination/source interval scale.
 * @return Finite destination result rounded once by fused mapping.
 * @throws std::domain_error when validated finite inputs unexpectedly produce
 *         a non-finite result.
 * @throws std::logic_error when no finite source anchor distance exists.
 * @note A symmetric destination prefers a source-midpoint anchor. When the
 *       direct source span overflows, the midpoint is reconstructed from
 *       scaled endpoints, but `value - midpoint` remains in the original
 *       exponent domain. Other destinations prefer the endpoint closest to
 *       zero and switch anchors only when its original distance overflows.
 */
template <typename Working>
Working map_finite_with_scale(Working value, Working source_minimum,
                              Working source_maximum, Working source_span,
                              Working destination_minimum,
                              Working destination_maximum, Working scale) {
  const Working destination_minimum_magnitude = std::fabs(destination_minimum);
  const Working destination_maximum_magnitude = std::fabs(destination_maximum);
  if (destination_minimum_magnitude == destination_maximum_magnitude) {
    const Working half = static_cast<Working>(0.5);
    Working source_midpoint = Working{0};
    bool stable_midpoint = false;
    if (std::isnormal(source_span)) {
      const Working source_radius = source_span * half;
      source_midpoint = std::fma(half, source_span, source_minimum);
      stable_midpoint = source_radius > Working{0} &&
                        std::isfinite(source_midpoint) &&
                        source_radius + source_radius == source_span;
    } else if (!std::isfinite(source_span)) {
      const ScaledInterval<Working> source =
          scale_finite_interval(source_minimum, source_maximum);
      const Working scaled_span = source.maximum - source.minimum;
      const Working scaled_radius = scaled_span * half;
      const Working scaled_midpoint =
          std::fma(half, scaled_span, source.minimum);
      source_midpoint = std::scalbn(scaled_midpoint, source.exponent);
      const Working source_radius = std::scalbn(scaled_radius, source.exponent);
      stable_midpoint = scaled_radius > Working{0} &&
                        scaled_radius + scaled_radius == scaled_span &&
                        source_radius > Working{0} &&
                        std::isfinite(source_radius) &&
                        std::isfinite(source_midpoint);
    }
    if (stable_midpoint) {
      const Working distance = value - source_midpoint;
      if (std::isfinite(distance)) {
        const Working mapped = std::fma(distance, scale, Working{0});
        if (!std::isfinite(mapped)) {
          throw std::domain_error(
              "Finite centered affine scale produced a non-finite result.");
        }
        return std::clamp(mapped, destination_minimum, destination_maximum);
      }
    }
  }

  const Working from_minimum = value - source_minimum;
  const Working from_maximum = source_maximum - value;
  const bool prefer_minimum =
      destination_minimum_magnitude <= destination_maximum_magnitude;
  if (prefer_minimum && std::isfinite(from_minimum)) {
    return interpolate_finite_from_endpoint_scale(
        destination_minimum, destination_maximum, from_minimum, scale, true);
  }
  if (!prefer_minimum && std::isfinite(from_maximum)) {
    return interpolate_finite_from_endpoint_scale(
        destination_minimum, destination_maximum, from_maximum, scale, false);
  }
  if (std::isfinite(from_minimum)) {
    return interpolate_finite_from_endpoint_scale(
        destination_minimum, destination_maximum, from_minimum, scale, true);
  }
  if (std::isfinite(from_maximum)) {
    return interpolate_finite_from_endpoint_scale(
        destination_minimum, destination_maximum, from_maximum, scale, false);
  }
  throw std::logic_error("Finite affine map has no finite source distance.");
}

/**
 * @brief Interpolates a destination from a centered position.
 * @tparam Working Floating-point type used by the complete affine operation.
 * @param minimum Finite destination lower endpoint.
 * @param maximum Finite destination upper endpoint greater than minimum.
 * @param centered_position Clamped affine position in `[-1, 1]`.
 * @return Finite rounded destination value.
 * @throws std::domain_error when validated finite inputs unexpectedly produce
 *         a non-finite result.
 * @throws std::logic_error when interval preconditions are broken.
 * @note Equal-magnitude endpoints multiply the centered position by the
 *       positive endpoint directly, retaining a representable near-midpoint
 *       sign without adding the displacement to one-half. Other intervals
 *       form the nearer endpoint fraction with one `fma` and delegate to the
 *       endpoint interpolator, so destination exponent scaling occurs only
 *       when its span actually overflows.
 */
template <typename Working>
Working interpolate_finite_centered(Working minimum, Working maximum,
                                    Working centered_position) {
  const Working bounded_position =
      std::clamp(centered_position, Working{-1}, Working{1});
  if (std::fabs(minimum) == std::fabs(maximum)) {
    const Working mapped = std::fma(bounded_position, maximum, Working{0});
    if (!std::isfinite(mapped)) {
      throw std::domain_error(
          "Finite centered affine interpolation produced a non-finite "
          "result.");
    }
    return std::clamp(mapped, minimum, maximum);
  }
  const bool from_minimum = bounded_position <= Working{0};
  const Working half = static_cast<Working>(0.5);
  const Working endpoint_fraction =
      from_minimum ? std::fma(half, bounded_position, half)
                   : std::fma(-half, bounded_position, half);
  return interpolate_finite_from_endpoint(minimum, maximum, endpoint_fraction,
                                          from_minimum);
}

/**
 * @brief Maps through scaled centered coordinates when a direct source span
 *        overflows.
 * @tparam Working Floating-point type used by the complete affine operation.
 * @param value Finite source value inside the inclusive source interval.
 * @param source_minimum Finite source lower endpoint.
 * @param source_maximum Finite source upper endpoint greater than minimum.
 * @param destination_minimum Finite destination lower endpoint.
 * @param destination_maximum Finite destination upper endpoint.
 * @return Finite rounded destination value.
 * @throws std::domain_error when finite inputs produce a non-finite result.
 * @throws std::logic_error when interval preconditions are broken.
 * @note The source is normalized by an exact power of two, making its span and
 *       half-span finite and non-subnormal. This fallback is used only after
 *       a normalized interval quotient is zero, subnormal, or infinite, so a
 *       displacement lost while forming the centered fraction cannot change
 *       a correctly rounded representable destination result.
 */
template <typename Working>
Working map_scaled_centered(Working value, Working source_minimum,
                            Working source_maximum, Working destination_minimum,
                            Working destination_maximum) {
  const ScaledInterval<Working> source =
      scale_finite_interval(source_minimum, source_maximum);
  const Working source_span = source.maximum - source.minimum;
  const Working half = static_cast<Working>(0.5);
  const Working source_radius = source_span * half;
  const Working source_midpoint = std::fma(half, source_span, source.minimum);
  if (source_radius == Working{0} || !std::isfinite(source_midpoint)) {
    throw std::logic_error("Scaled source interval could not be centered.");
  }
  const Working scaled_value = std::scalbn(value, -source.exponent);
  const Working centered_position =
      std::clamp((scaled_value - source_midpoint) / source_radius, Working{-1},
                 Working{1});

  return interpolate_finite_centered(destination_minimum, destination_maximum,
                                     centered_position);
}

/**
 * @brief Maps one in-domain finite value through an overflow-safe affine core.
 * @tparam Working Floating-point type used by the complete affine operation.
 * @param value Finite source value inside the inclusive source interval.
 * @param source_minimum Finite source lower endpoint.
 * @param source_maximum Finite source upper endpoint greater than minimum.
 * @param destination_minimum Finite destination lower endpoint.
 * @param destination_maximum Finite destination upper endpoint.
 * @return Finite destination value rounded by the active working type.
 * @throws std::domain_error when finite validated inputs unexpectedly produce
 *         a non-finite result.
 * @throws std::logic_error when interval preconditions are broken.
 * @note Exact endpoints and equal numeric domains avoid arithmetic. For finite
 *       spans, a finite nonzero destination/source scale is preferred and one
 *       endpoint-relative distance is fused directly with the destination
 *       endpoint closest to zero. Equal-magnitude destinations use a stable
 *       source midpoint when available so cross-zero near-center displacement
 *       is retained. If a direct finite-span scale underflows or overflows,
 *       the core falls back to endpoint-relative fraction plus fused
 *       interpolation; narrow subnormal intervals therefore never halve their
 *       span or subtract an unverified rounded midpoint. Overflowing spans are
 *       independently normalized to derive a bounded mantissa quotient and
 *       exponent difference. A normal derived scale multiplies the original,
 *       unscaled source anchor distance. A zero, subnormal, or infinite
 *       derived scale retains the scaled-position fallback, and an overflowing
 *       destination interpolation is restored once. The same helper is used
 *       for forward conversion and reverse precision validation.
 */
template <typename Working>
Working map_finite_affine_working(Working value, Working source_minimum,
                                  Working source_maximum,
                                  Working destination_minimum,
                                  Working destination_maximum) {
  if (value == source_minimum) {
    return destination_minimum;
  }
  if (value == source_maximum) {
    return destination_maximum;
  }
  if (source_minimum == destination_minimum &&
      source_maximum == destination_maximum) {
    return value;
  }

  const Working source_span = source_maximum - source_minimum;
  const Working destination_span = destination_maximum - destination_minimum;
  const bool finite_source_span =
      std::isfinite(source_span) && source_span > Working{0};
  const bool finite_destination_span =
      std::isfinite(destination_span) && destination_span > Working{0};
  if (finite_source_span && finite_destination_span) {
    const Working scale = destination_span / source_span;
    if (std::isfinite(scale) && scale > Working{0}) {
      return map_finite_with_scale(value, source_minimum, source_maximum,
                                   source_span, destination_minimum,
                                   destination_maximum, scale);
    }
  } else {
    const ScaledInterval<Working> scaled_source =
        scale_finite_interval(source_minimum, source_maximum);
    const ScaledInterval<Working> scaled_destination =
        scale_finite_interval(destination_minimum, destination_maximum);
    const Working scale =
        derive_scale_from_scaled_intervals(scaled_source, scaled_destination);
    if (std::isnormal(scale)) {
      return map_finite_with_scale(value, source_minimum, source_maximum,
                                   source_span, destination_minimum,
                                   destination_maximum, scale);
    }
  }

  if (finite_source_span) {
    const Working from_minimum = value - source_minimum;
    const Working from_maximum = source_maximum - value;
    const Working destination_minimum_magnitude =
        std::fabs(destination_minimum);
    const Working destination_maximum_magnitude =
        std::fabs(destination_maximum);
    if (destination_minimum_magnitude < destination_maximum_magnitude) {
      return interpolate_finite_from_endpoint(destination_minimum,
                                              destination_maximum,
                                              from_minimum / source_span, true);
    }
    if (destination_maximum_magnitude < destination_minimum_magnitude) {
      return interpolate_finite_from_endpoint(
          destination_minimum, destination_maximum, from_maximum / source_span,
          false);
    }
    Working centered_position = (from_minimum - from_maximum) / source_span;
    if (std::isnormal(source_span)) {
      const Working half = static_cast<Working>(0.5);
      const Working source_radius = source_span * half;
      const Working source_midpoint =
          std::fma(half, source_span, source_minimum);
      if (source_radius > Working{0} && std::isfinite(source_midpoint) &&
          source_radius + source_radius == source_span) {
        centered_position = (value - source_midpoint) / source_radius;
      }
    }
    return interpolate_finite_centered(destination_minimum, destination_maximum,
                                       centered_position);
  }
  return map_scaled_centered(value, source_minimum, source_maximum,
                             destination_minimum, destination_maximum);
}

/**
 * @brief Dispatches one finite affine map through the selected working type.
 *
 * Production always uses `long double`. BUILD_TESTING runtime images may
 * select binary64 on the current thread so the public conversion path proves
 * correctness without relying on a platform's extended working type.
 *
 * @param value Finite source value inside the inclusive source interval.
 * @param source_minimum Finite source lower endpoint.
 * @param source_maximum Finite source upper endpoint greater than minimum.
 * @param destination_minimum Finite destination lower endpoint.
 * @param destination_maximum Finite destination upper endpoint.
 * @return Finite destination value rounded by the selected working type.
 * @throws std::domain_error when finite validated inputs unexpectedly produce
 *         a non-finite result.
 * @throws std::logic_error when interval preconditions are broken.
 * @note Forward conversion and reverse precision validation call this same
 *       dispatcher. The test selection is thread-local and absent from
 *       production builds.
 */
long double map_finite_affine(long double value, long double source_minimum,
                              long double source_maximum,
                              long double destination_minimum,
                              long double destination_maximum) {
#if defined(PHOTOSPIDER_INTERNAL_SAMPLE_CONVERSION_TESTING)
  if (sample_conversion_test_state.force_binary64_affine) {
    return static_cast<long double>(map_finite_affine_working<double>(
        static_cast<double>(value), static_cast<double>(source_minimum),
        static_cast<double>(source_maximum),
        static_cast<double>(destination_minimum),
        static_cast<double>(destination_maximum)));
  }
#endif
  return map_finite_affine_working<long double>(
      value, source_minimum, source_maximum, destination_minimum,
      destination_maximum);
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
      descriptor.element_semantics == ElementSemantics::FloatingPoint &&
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
          stored = store_integral_native<std::uint8_t>(address, stored);
          break;
        case 16U:
          stored = store_integral_native<std::uint16_t>(address, stored);
          break;
        case 32U:
          stored = store_integral_native<std::uint32_t>(address, stored);
          break;
        case 64U:
          stored = store_integral_native<std::uint64_t>(address, stored);
          break;
        default:
          throw std::invalid_argument("Unsupported unsigned destination.");
      }
      break;
    case ElementSemantics::SignedInteger:
      switch (width) {
        case 8U:
          stored = store_integral_native<std::int8_t>(address, stored);
          break;
        case 16U:
          stored = store_integral_native<std::int16_t>(address, stored);
          break;
        case 32U:
          stored = store_integral_native<std::int32_t>(address, stored);
          break;
        case 64U:
          stored = store_integral_native<std::int64_t>(address, stored);
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
            : map_finite_affine(stored, destination_minimum,
                                destination_maximum, source_minimum,
                                source_maximum);
    exact_reverse = reversed == source_after_policy;
  }
  if ((!exact_destination || !exact_reverse) &&
      conversion.precision_loss == PrecisionLossPolicy::Reject) {
    throw std::domain_error("Sample conversion would lose numeric precision.");
  }
}

/**
 * @brief Compares an exact native integer with one finite binary64 endpoint.
 * @tparam Scalar Supported native integer type.
 * @param integer Exact integer sample.
 * @param endpoint Finite declared-domain endpoint.
 * @return Negative, zero, or positive when `integer` is respectively less
 * than, equal to, or greater than `endpoint`.
 * @throws Nothing.
 * @note Conversion is performed only after endpoint range checks make the
 * integer cast defined; this prevents binary64 rounding from hiding
 * `2^53 +/- 1` distinctions.
 */
template <typename Scalar>
int compare_integral_to_double(Scalar integer, double endpoint) noexcept {
  static_assert(std::is_integral_v<Scalar>,
                "domain comparison requires an integer type");
  if constexpr (std::is_unsigned_v<Scalar>) {
    if (endpoint < 0.0) {
      return 1;
    }
    constexpr double kUpperExclusive = 18446744073709551616.0;
    if (endpoint >= kUpperExclusive) {
      return -1;
    }
    const std::uint64_t truncated = static_cast<std::uint64_t>(endpoint);
    const std::uint64_t promoted = static_cast<std::uint64_t>(integer);
    if (promoted < truncated) {
      return -1;
    }
    if (promoted > truncated) {
      return 1;
    }
    return std::trunc(endpoint) == endpoint ? 0 : -1;
  } else {
    constexpr double kLowerInclusive = -9223372036854775808.0;
    constexpr double kUpperExclusive = 9223372036854775808.0;
    if (endpoint < kLowerInclusive) {
      return 1;
    }
    if (endpoint >= kUpperExclusive) {
      return -1;
    }
    const std::int64_t truncated = static_cast<std::int64_t>(endpoint);
    const std::int64_t promoted = static_cast<std::int64_t>(integer);
    if (promoted < truncated) {
      return -1;
    }
    if (promoted > truncated) {
      return 1;
    }
    if (std::trunc(endpoint) == endpoint) {
      return 0;
    }
    return endpoint < 0.0 ? 1 : -1;
  }
}

/**
 * @brief Executes one same-storage, equal-endpoint scalar transfer exactly.
 * @tparam Scalar Exact source and destination native scalar type.
 * @param source Borrowed readable source bytes.
 * @param destination Borrowed writable destination bytes.
 * @param descriptor Matching source/destination descriptor.
 * @param conversion Equal-endpoint identity conversion and closed policies.
 * @return Nothing after exact copy or explicit endpoint clamp.
 * @throws std::domain_error for rejected out-of-domain/non-finite input or a
 * forbidden clamp precision loss.
 * @note In-domain values are copied byte-for-byte without promotion. A clamp
 * uses the declared binary64 endpoint directly, never the out-of-domain wide
 * integer, before checked destination storage.
 */
template <typename Scalar>
void transfer_identity_native(const std::byte* source, std::byte* destination,
                              const DenseTensorDescriptor& descriptor,
                              const SampleConversion& conversion) {
  const Scalar scalar = read_native<Scalar>(source);
  bool below = false;
  bool above = false;
  if constexpr (std::is_floating_point_v<Scalar>) {
    if (!std::isfinite(scalar)) {
      if (conversion.non_finite != NonFinitePolicy::Preserve) {
        throw std::domain_error(
            "Sample conversion rejected a non-finite value.");
      }
      std::memcpy(destination, source, sizeof(Scalar));
      return;
    }
    below = scalar < conversion.source.domain.minimum;
    above = scalar > conversion.source.domain.maximum;
  } else {
    below = compare_integral_to_double(scalar,
                                       conversion.source.domain.minimum) < 0;
    above = compare_integral_to_double(scalar,
                                       conversion.source.domain.maximum) > 0;
  }
  if (!below && !above) {
    std::memcpy(destination, source, sizeof(Scalar));
    return;
  }
  if (conversion.out_of_domain == OutOfDomainPolicy::Reject) {
    throw std::domain_error(
        "Sample conversion rejected an out-of-domain value.");
  }
  const long double clamped = below ? conversion.source.domain.minimum
                                    : conversion.source.domain.maximum;
  store_scalar(destination, descriptor, clamped, clamped, conversion);
}

/**
 * @brief Dispatches exact identity transfer for one supported native scalar.
 * @param source Borrowed readable source scalar.
 * @param destination Borrowed writable destination scalar.
 * @param descriptor Matching source/destination native descriptor.
 * @param conversion Equal-endpoint identity request.
 * @return Nothing after exact transfer.
 * @throws std::invalid_argument for unsupported storage.
 * @throws std::domain_error from explicit identity policies.
 */
void transfer_identity_scalar(const std::byte* source, std::byte* destination,
                              const DenseTensorDescriptor& descriptor,
                              const SampleConversion& conversion) {
  const std::uint32_t width = descriptor.storage_encoding.bit_width;
  switch (descriptor.element_semantics) {
    case ElementSemantics::UnsignedInteger:
      if (width == 8U) {
        return transfer_identity_native<std::uint8_t>(source, destination,
                                                      descriptor, conversion);
      }
      if (width == 16U) {
        return transfer_identity_native<std::uint16_t>(source, destination,
                                                       descriptor, conversion);
      }
      if (width == 32U) {
        return transfer_identity_native<std::uint32_t>(source, destination,
                                                       descriptor, conversion);
      }
      if (width == 64U) {
        return transfer_identity_native<std::uint64_t>(source, destination,
                                                       descriptor, conversion);
      }
      break;
    case ElementSemantics::SignedInteger:
      if (width == 8U) {
        return transfer_identity_native<std::int8_t>(source, destination,
                                                     descriptor, conversion);
      }
      if (width == 16U) {
        return transfer_identity_native<std::int16_t>(source, destination,
                                                      descriptor, conversion);
      }
      if (width == 32U) {
        return transfer_identity_native<std::int32_t>(source, destination,
                                                      descriptor, conversion);
      }
      if (width == 64U) {
        return transfer_identity_native<std::int64_t>(source, destination,
                                                      descriptor, conversion);
      }
      break;
    case ElementSemantics::FloatingPoint:
      if (width == 32U) {
        return transfer_identity_native<float>(source, destination, descriptor,
                                               conversion);
      }
      if (width == 64U) {
        return transfer_identity_native<double>(source, destination, descriptor,
                                                conversion);
      }
      break;
  }
  throw std::invalid_argument("Unsupported sample identity storage.");
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
 * @brief Applies source-domain policy and overflow-safe affine endpoint map.
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
  return {source, map_finite_affine(source, source_minimum, source_maximum,
                                    destination_minimum, destination_maximum)};
}

}  // namespace

#if defined(PHOTOSPIDER_INTERNAL_SAMPLE_CONVERSION_TESTING)
namespace testing {

/**
 * @copydoc ScopedBinary64AffineForTesting::ScopedBinary64AffineForTesting()
 */
ScopedBinary64AffineForTesting::ScopedBinary64AffineForTesting() noexcept
    : previous_mode_(sample_conversion_test_state.force_binary64_affine) {
  sample_conversion_test_state.force_binary64_affine = true;
}

/** @copydoc
 * ScopedBinary64AffineForTesting::~ScopedBinary64AffineForTesting()
 */
ScopedBinary64AffineForTesting::~ScopedBinary64AffineForTesting() noexcept {
  sample_conversion_test_state.force_binary64_affine = previous_mode_;
}

}  // namespace testing
#endif

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
  if (source.dense_tensor_descriptor().quantization.has_value()) {
    throw std::invalid_argument(
        "Sample conversion requires unquantized source storage.");
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
  const DenseTensorDescriptor& source_descriptor =
      source.dense_tensor_descriptor();
  const bool exact_storage_identity =
      !source_descriptor.quantization.has_value() &&
      source_descriptor.storage_encoding.kind ==
          StorageEncodingKind::NativeScalar &&
      source_descriptor.element_semantics ==
          destination_descriptor.element_semantics &&
      source_descriptor.storage_encoding ==
          destination_descriptor.storage_encoding &&
      conversion.source.encoding == conversion.destination.encoding &&
      conversion.source.domain == conversion.destination.domain;
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
      std::byte* output =
          destination_address(write.data(), coordinates, destination_layout);
      const std::byte* input = source_tensor.element_data(coordinates);
      if (exact_storage_identity) {
        transfer_identity_scalar(input, output, destination_descriptor,
                                 conversion);
      } else {
        const long double source_sample = load_scalar(input, source_descriptor);
        const auto [source_after_policy, mapped] =
            map_sample(source_sample, conversion);
        store_scalar(output, destination_descriptor, mapped,
                     source_after_policy, conversion);
      }
    } while (advance_coordinate(&coordinates, destination_descriptor.shape));
  }
  return builder.seal();
}

}  // namespace ps
