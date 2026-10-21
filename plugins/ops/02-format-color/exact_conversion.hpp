#pragma once

#include <cstdint>

#include "01-numeric/exact_product.hpp"

namespace ps::plugin_internal::format_numeric {
// A finite IEEE value or a signed 64-bit magnitude. Canonical odd significands
// make endpoint equality exact without floating arithmetic, including zero.
struct Dyadic final {
  std::uint64_t significand = 0;
  int exponent = 0;
  bool negative = false;
  static Dyadic normalized(std::uint64_t value, int power, bool sign) {
    if (!value)
      return {0, 0, sign};
    const auto zeros = __builtin_ctzll(value);
    return {value >> zeros, power + zeros, sign};
  }
  static Dyadic binary(std::uint64_t bits, bool narrow) {
    const auto value = numeric_ops::BinaryParts::decode(bits, narrow);
    return normalized(value.significand, value.exponent, value.negative);
  }
  bool equal(const Dyadic& other) const {
    return significand == other.significand &&
           (!significand ||
            (exponent == other.exponent && negative == other.negative));
  }
};
using ConversionInteger = numeric_ops::FixedInteger<68>;
struct ConversionMap final {
  Dyadic source[2], target[2];
  ConversionInteger width, slope, intercept;
  bool slope_negative = false, intercept_negative = false;
};
// All finite inputs/endpoints, in units 2^-1074, have magnitude <2^2098.
// Width/slope need <2099 bits; the affine numerator needs <4198 bits.
// Division alignment, 64 integer quotient bits and rounding carry fit in 4352
// bits. Every large mutable temporary resides in this invocation scratch.
struct ExactConversion final {
  using Integer = ConversionInteger;
  numeric_ops::ExactRatioWorkspace<68> ratio{
      numeric_ops::SequenceProfile::Strict};
  Integer value, other;

  static void import(const Dyadic& input, Integer* output) {
    output->words.fill(0);
    if (!input.significand)
      return;
    const auto shift = static_cast<unsigned>(input.exponent + 1074);
    const auto word = shift / 64, tail = shift % 64;
    output->words[word] = input.significand << tail;
    if (tail)
      output->words[word + 1] = input.significand >> (64 - tail);
  }
  void difference(const Dyadic& a, const Dyadic& b) {
    import(a, &ratio.numerator);
    import(b, &ratio.term);
    ratio.negative = a.negative;
    ratio.add_term(!b.negative);
  }
  Status initialize(ConversionMap* map,
                    const core_internal::WorkConsumer& consume) {
    difference(map->source[1], map->source[0]);
    map->width = ratio.numerator;
    difference(map->target[1], map->target[0]);
    map->slope = ratio.numerator;
    map->slope_negative = ratio.negative;
    import(map->target[0], &value);
    auto status = numeric_ops::multiply_fixed(value, map->width,
                                              &ratio.numerator, consume);
    if (!status.ok())
      return status;
    ratio.negative = map->target[0].negative;
    import(map->source[0], &value);
    status =
        numeric_ops::multiply_fixed(value, map->slope, &ratio.term, consume);
    if (!status.ok())
      return status;
    ratio.add_term(!(map->source[0].negative != map->slope_negative));
    map->intercept = ratio.numerator;
    map->intercept_negative = ratio.negative;
    return Status::success();
  }
  Status map_value(const Dyadic& input, const ConversionMap* map,
                   const core_internal::WorkConsumer& consume) {
    auto endpoint = input;
    if (map) {
      if (input.equal(map->source[0])) {
        endpoint = map->target[0];
      } else if (input.equal(map->source[1])) {
        endpoint = map->target[1];
      } else {
        import(input, &value);
        auto status = numeric_ops::multiply_fixed(value, map->slope,
                                                  &ratio.numerator, consume);
        if (!status.ok())
          return status;
        ratio.negative = input.negative != map->slope_negative;
        ratio.term = map->intercept;
        ratio.add_term(map->intercept_negative);
        // Ordinary affine exact zero is +0, even for same-sign zero terms.
        if (ratio.top(ratio.numerator) < 0)
          ratio.negative = false;
        ratio.denominator = map->width;
        return Status::success();
      }
    }
    import(endpoint, &ratio.numerator);
    ratio.denominator.words.fill(0);
    ratio.denominator.words[0] = 1;
    ratio.negative = endpoint.negative;
    return Status::success();
  }
  Result<std::uint64_t> floating(bool narrow, bool clip,
                                 const core_internal::WorkConsumer& consume) {
    using Answer = Result<std::uint64_t>;
    if (ratio.top(ratio.numerator) < 0)
      return Answer(static_cast<std::uint64_t>(ratio.negative)
                    << (narrow ? 31 : 63));
    auto result = ratio.round(narrow, consume, -1074);
    if (!result.ok())
      return result;
    auto bits = result.value();
    const auto infinity =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    if (clip && (bits & infinity) == infinity) {
      bits = (bits & (UINT64_C(1) << (narrow ? 31 : 63))) | (infinity - 1);
    }
    return Answer(bits);
  }
  Result<std::uint64_t> integer(std::uint64_t positive_limit,
                                std::uint64_t negative_limit, bool clip,
                                const core_internal::WorkConsumer& consume) {
    using Answer = Result<std::uint64_t>;
    // Convert denominator to the same 2^-2148 units as the numerator.
    // For a direct endpoint/pure cast, this also cancels the input's 2^-1074.
    ratio.shift(ratio.denominator, 1074, &other);
    const auto limit = ratio.negative ? negative_limit : positive_limit;
    const auto saturated = [&]() {
      return Answer(ratio.negative ? UINT64_C(0) - limit : limit);
    };
    // Saturation is the requested clip operation. Without clip, target
    // representability is an unchecked caller premise.
    if (clip) {
      ratio.shift(other, 64, &ratio.candidate);
      if (ratio.compare(ratio.numerator, ratio.candidate) >= 0)
        return saturated();
    }
    std::uint64_t quotient = 0;
    for (unsigned bit = 64; bit-- > 0;) {
      const auto& status = consume(256);
      if (!status.ok())
        return Answer(status);
      ratio.shift(other, bit, &ratio.candidate);
      if (ratio.compare(ratio.numerator, ratio.candidate) >= 0) {
        ratio.numerator.subtract(ratio.candidate);
        quotient |= UINT64_C(1) << bit;
      }
    }
    ratio.shift(ratio.numerator, 1, &ratio.candidate);
    const auto order = ratio.compare(ratio.candidate, other);
    if (order > 0 || (!order && (quotient & 1))) {
      if (clip && quotient == UINT64_MAX)
        return saturated();
      ++quotient;
    }
    if (clip && quotient > limit)
      return saturated();
    return Answer(ratio.negative ? UINT64_C(0) - quotient : quotient);
  }
};
}  // namespace ps::plugin_internal::format_numeric
