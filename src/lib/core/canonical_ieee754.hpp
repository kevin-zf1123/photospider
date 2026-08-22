/**
 * @file canonical_ieee754.hpp
 * @brief Source-private numeric IEEE-754 canonical bit-pattern helpers.
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace ps::internal {
namespace canonical_ieee754_detail {

/**
 * @brief Describes one supported native floating-point interchange profile.
 * @tparam Float Native floating-point type.
 * @tparam Unsigned Unsigned integer carrying the interchange bits.
 * @tparam ExponentBits Number of interchange exponent bits.
 * @throws Nothing for compile-time use.
 * @note `digits`, `min_exponent`, and the integer width supply every remaining
 *       binary interchange constant without inspecting object bytes.
 */
template <typename Float, typename Unsigned, int ExponentBits>
struct BinaryProfile final {
  /** @brief Native floating-point type. */
  using FloatType = Float;

  /** @brief Unsigned integer interchange carrier. */
  using UnsignedType = Unsigned;

  /** @brief Number of explicit fraction bits. */
  static constexpr int kFractionBits = std::numeric_limits<Float>::digits - 1;

  /** @brief Number of encoded exponent bits. */
  static constexpr int kExponentBits = ExponentBits;

  /** @brief IEEE biased-exponent offset. */
  static constexpr int kExponentBias =
      2 - std::numeric_limits<Float>::min_exponent;

  /** @brief Power used to turn a subnormal magnitude into an integer. */
  static constexpr int kSubnormalScale =
      std::numeric_limits<Float>::digits -
      std::numeric_limits<Float>::min_exponent;
};

/** @brief Supported IEC 559 binary32 native profile. */
using Binary32Profile = BinaryProfile<float, std::uint32_t, 8>;

/** @brief Supported IEC 559 binary64 native profile. */
using Binary64Profile = BinaryProfile<double, std::uint64_t, 11>;

static_assert(sizeof(float) == sizeof(std::uint32_t) &&
                  std::numeric_limits<float>::is_iec559 &&
                  std::numeric_limits<float>::radix == 2 &&
                  std::numeric_limits<float>::digits == 24 &&
                  std::numeric_limits<float>::min_exponent == -125 &&
                  std::numeric_limits<float>::max_exponent == 128 &&
                  std::numeric_limits<float>::has_denorm == std::denorm_present,
              "canonical binary32 requires IEC 559 binary32 with subnormals");

static_assert(sizeof(double) == sizeof(std::uint64_t) &&
                  std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<double>::radix == 2 &&
                  std::numeric_limits<double>::digits == 53 &&
                  std::numeric_limits<double>::min_exponent == -1021 &&
                  std::numeric_limits<double>::max_exponent == 1024 &&
                  std::numeric_limits<double>::has_denorm ==
                      std::denorm_present,
              "canonical binary64 requires IEC 559 binary64 with subnormals");

/**
 * @brief Derives one canonical interchange word from a finite numeric value.
 * @tparam Profile Supported binary interchange profile.
 * @param value Finite native numeric value.
 * @return Canonical sign/exponent/fraction word with both zeros normalized to
 *         positive zero.
 * @throws std::invalid_argument when `value` is NaN or infinity.
 * @note The computation uses only numeric classification, `frexp`, and
 *       power-of-two scaling. Native object byte order and floating word order
 *       are never observed.
 */
template <typename Profile>
typename Profile::UnsignedType canonical_bits(
    typename Profile::FloatType value) {
  using Float = typename Profile::FloatType;
  using Unsigned = typename Profile::UnsignedType;
  static_assert(std::is_floating_point_v<Float>);
  static_assert(std::is_unsigned_v<Unsigned>);

  if (!std::isfinite(value)) {
    throw std::invalid_argument(
        "Canonical IEEE-754 encoding requires a finite value.");
  }
  if (value == Float{0}) {
    return Unsigned{0};
  }

  constexpr int kWordBits = std::numeric_limits<Unsigned>::digits;
  constexpr Unsigned kSignBit = Unsigned{1} << (kWordBits - 1);
  constexpr Unsigned kImplicitBit = Unsigned{1} << Profile::kFractionBits;
  Unsigned bits = std::signbit(value) ? kSignBit : Unsigned{0};
  const Float magnitude = std::fabs(value);
  if (magnitude < std::numeric_limits<Float>::min()) {
    bits |=
        static_cast<Unsigned>(std::ldexp(magnitude, Profile::kSubnormalScale));
    return bits;
  }

  int exponent = 0;
  const Float fraction = std::frexp(magnitude, &exponent);
  const Unsigned significand = static_cast<Unsigned>(
      std::ldexp(fraction, std::numeric_limits<Float>::digits));
  const Unsigned biased_exponent =
      static_cast<Unsigned>(exponent + Profile::kExponentBias - 1);
  bits |= (biased_exponent << Profile::kFractionBits) |
          (significand - kImplicitBit);
  return bits;
}

/**
 * @brief Reconstructs one finite native number from a canonical interchange
 *        word.
 * @tparam Profile Supported binary interchange profile.
 * @param bits Canonical finite sign/exponent/fraction word.
 * @return Exact native numeric value; zero is always positive.
 * @throws std::invalid_argument for a NaN/infinity exponent or negative-zero
 *         spelling, neither of which is canonical finite artifact metadata.
 * @note Integer-to-floating conversion is exact because every significand has
 *       at most the native type's declared `digits`; `ldexp` then applies only
 *       a power of two. No native object bytes are written.
 */
template <typename Profile>
typename Profile::FloatType decode_canonical_bits(
    typename Profile::UnsignedType bits) {
  using Float = typename Profile::FloatType;
  using Unsigned = typename Profile::UnsignedType;
  constexpr int kWordBits = std::numeric_limits<Unsigned>::digits;
  constexpr Unsigned kSignBit = Unsigned{1} << (kWordBits - 1);
  constexpr Unsigned kFractionMask =
      (Unsigned{1} << Profile::kFractionBits) - 1U;
  constexpr Unsigned kExponentMask =
      (Unsigned{1} << Profile::kExponentBits) - 1U;

  const bool negative = (bits & kSignBit) != 0U;
  const Unsigned fraction = bits & kFractionMask;
  const Unsigned exponent = (bits >> Profile::kFractionBits) & kExponentMask;
  if (exponent == kExponentMask) {
    throw std::invalid_argument("Canonical IEEE-754 metadata must be finite.");
  }
  if (exponent == 0U && fraction == 0U) {
    if (negative) {
      throw std::invalid_argument(
          "Canonical IEEE-754 metadata forbids negative zero.");
    }
    return Float{0};
  }

  Float magnitude = Float{0};
  if (exponent == 0U) {
    magnitude =
        std::ldexp(static_cast<Float>(fraction), -Profile::kSubnormalScale);
  } else {
    constexpr Unsigned kImplicitBit = Unsigned{1} << Profile::kFractionBits;
    const Unsigned significand = kImplicitBit | fraction;
    const int power = static_cast<int>(exponent) - Profile::kExponentBias -
                      Profile::kFractionBits;
    magnitude = std::ldexp(static_cast<Float>(significand), power);
  }
  return negative ? -magnitude : magnitude;
}

}  // namespace canonical_ieee754_detail

/**
 * @brief Derives canonical binary32 bits from one finite float.
 * @param value Finite IEC 559 binary32 numeric value.
 * @return Canonical binary32 word with signed zero normalized to positive.
 * @throws std::invalid_argument for NaN or infinity.
 * @note Compilation fails closed unless native float is IEC 559 binary32 with
 *       subnormal support. Object and floating word order are irrelevant.
 */
inline std::uint32_t canonical_binary32_bits(float value) {
  return canonical_ieee754_detail::canonical_bits<
      canonical_ieee754_detail::Binary32Profile>(value);
}

/**
 * @brief Derives canonical binary64 bits from one finite double.
 * @param value Finite IEC 559 binary64 numeric value.
 * @return Canonical binary64 word with signed zero normalized to positive.
 * @throws std::invalid_argument for NaN or infinity.
 * @note Compilation fails closed unless native double is IEC 559 binary64
 *       with subnormal support. Object and floating word order are irrelevant.
 */
inline std::uint64_t canonical_binary64_bits(double value) {
  return canonical_ieee754_detail::canonical_bits<
      canonical_ieee754_detail::Binary64Profile>(value);
}

/**
 * @brief Decodes one canonical finite binary32 word numerically.
 * @param bits Canonical binary32 sign/exponent/fraction word.
 * @return Exact native float with canonical positive zero.
 * @throws std::invalid_argument for nonfinite or negative-zero spellings.
 * @note No native object representation is read or written.
 */
inline float decode_canonical_binary32(std::uint32_t bits) {
  return canonical_ieee754_detail::decode_canonical_bits<
      canonical_ieee754_detail::Binary32Profile>(bits);
}

/**
 * @brief Decodes one canonical finite binary64 word numerically.
 * @param bits Canonical binary64 sign/exponent/fraction word.
 * @return Exact native double with canonical positive zero.
 * @throws std::invalid_argument for nonfinite or negative-zero spellings.
 * @note No native object representation is read or written.
 */
inline double decode_canonical_binary64(std::uint64_t bits) {
  return canonical_ieee754_detail::decode_canonical_bits<
      canonical_ieee754_detail::Binary64Profile>(bits);
}

}  // namespace ps::internal
