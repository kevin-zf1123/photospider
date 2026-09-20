#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "01-numeric/exact_product.hpp"
#include "01-numeric/numeric_nan.hpp"

namespace ps::plugin_internal::numeric_ops {
struct QuantilePosition final {
  std::uint64_t index = 0;
  std::array<std::uint64_t, 2> remainder{};
  bool fractional() const { return remainder[0] || remainder[1]; }
  unsigned __int128 weight() const {
    return static_cast<unsigned __int128>(remainder[1]) << 64 | remainder[0];
  }
  unsigned shift = 0;
};
// The caller validates 1<=count<=2^40. q is a finite binary32/64 in [0,1];
// its decoded significand times count-1 fits 93 bits. No floating rank or
// preliminary sample conversion participates in selection.
inline Result<QuantilePosition> quantile_position(std::uint64_t raw,
                                                  ElementType type,
                                                  std::uint64_t count) {
  const bool narrow = type == ElementType::Float32;
  const auto q = BinaryParts::decode(raw, narrow);
  const auto one = narrow ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000);
  if (q.nan || q.infinite || (q.negative && q.magnitude) || q.magnitude > one)
    return Result<QuantilePosition>(
        Status{ErrorCode::InvalidArgument,
               "InvalidQuantileProbability: require finite q in [0,1]",
               FailureReason::InvalidDomain,
               {FailureOrigin::Domain, FailureScope::Unspecified}});
  const auto product =
      static_cast<unsigned __int128>(count - 1) * q.significand;
  const auto shift = static_cast<unsigned>(-q.exponent);
  if (shift >= 128)
    return Result<QuantilePosition>(
        QuantilePosition{0,
                         {static_cast<std::uint64_t>(product),
                          static_cast<std::uint64_t>(product >> 64)},
                         shift});
  const auto remaining =
      product & ((static_cast<unsigned __int128>(1) << shift) - 1);
  return Result<QuantilePosition>(
      QuantilePosition{static_cast<std::uint64_t>(product >> shift),
                       {static_cast<std::uint64_t>(remaining),
                        static_cast<std::uint64_t>(remaining >> 64)},
                       shift});
}
// All finite endpoints are represented in units of 2^-1074. A*2^k needs at
// most 3172 bits for k<=1074; weighting/cancellation and final RN alignment
// fit the 4352-bit workspace. finish destructively reuses scratch.
struct ExactQuantile final {
  RatioWorkspace ratio;
  RatioWorkspace::Integer source, coefficient;
  explicit ExactQuantile(SequenceProfile profile) : ratio(profile) {}
  bool endpoint(std::uint64_t bits, ElementType type) {
    if (type == ElementType::Float32 || type == ElementType::Float64) {
      const auto parts =
          BinaryParts::decode(bits, type == ElementType::Float32);
      source.set(parts, 1074);
      return parts.negative;
    }
    const bool negative = type == ElementType::Int64 && (bits >> 63);
    const auto magnitude = negative ? UINT64_C(0) - bits : bits;
    integer_coefficient(magnitude, &ratio.candidate);
    RatioWorkspace::shift(ratio.candidate, 1074, &source);
    return negative;
  }
  Result<std::uint64_t> finish(
      std::uint64_t a, std::uint64_t b, ElementType type,
      ElementType destination, const QuantilePosition& position,
      const std::function<Status(std::uint64_t)>& consume) {
    using Answer = Result<std::uint64_t>;
    auto status = consume(2048);
    if (!status.ok())
      return Answer(status);
    const bool narrow = destination == ElementType::Float32;
    const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
    const auto infinity =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    const auto quiet = UINT64_C(1) << (narrow ? 22 : 51);
    const bool floating =
        type == ElementType::Float32 || type == ElementType::Float64;
    const auto x = floating
                       ? BinaryParts::decode(a, type == ElementType::Float32)
                       : BinaryParts{};
    const auto y = floating
                       ? BinaryParts::decode(b, type == ElementType::Float32)
                       : BinaryParts{};
    // A source-line NaN has already been selected in original logical order
    // by the caller. These endpoint cases also support the singleton path.
    if (floating && x.nan)
      return Answer(converted_nan(a, type, destination));
    if (!position.fractional()) {
      if (floating && x.infinite)
        return Answer(infinity | (x.negative ? sign : 0));
      if (floating && !x.magnitude)
        return Answer(x.negative ? sign : 0);
      ratio.negative = endpoint(a, type);
      ratio.numerator = source;
      integer_coefficient(1, &ratio.denominator);
      return ratio.round(narrow, consume, -1074);
    }
    if (floating) {
      if (y.nan)
        return Answer(converted_nan(b, type, destination));
      if (x.infinite || y.infinite) {
        if (x.infinite && y.infinite && x.negative != y.negative)
          return Answer(infinity | quiet);
        return Answer(infinity |
                      ((x.infinite ? x.negative : y.negative) ? sign : 0));
      }
      if (!x.magnitude && !y.magnitude)
        return Answer(x.negative && y.negative ? sign : 0);
    }
    ratio.denominator.words.fill(0);
    ratio.denominator.words[position.shift / 64] = UINT64_C(1)
                                                   << (position.shift % 64);
    integer_coefficient(position.weight(), &coefficient);
    const bool a_negative = endpoint(a, type);
    ratio.negative = a_negative;
    status =
        multiply_fixed(source, ratio.denominator, &ratio.numerator, consume);
    if (!status.ok())
      return Answer(status);
    status = multiply_fixed(source, coefficient, &ratio.term, consume);
    if (!status.ok())
      return Answer(status);
    ratio.add_term(!a_negative);
    const bool b_negative = endpoint(b, type);
    status = multiply_fixed(source, coefficient, &ratio.term, consume);
    if (!status.ok())
      return Answer(status);
    ratio.add_term(b_negative);
    return ratio.round(narrow, consume, -1074);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
