#pragma once

#include <cstdint>
#include <functional>

#include "01-numeric/exact_product.hpp"

namespace ps::plugin_internal::numeric_ops {
// Finite binary64 differences need at most 2099 bits. The smoothstep cubic
// needs fewer than 6300 bits; division alignment plus 53 quotient bits fits
// in 6656 bits. Every large temporary belongs to the host continuation.
struct InterpolationWorkspace final {
  using Integer = FixedInteger<104>;
  ExactRatioWorkspace<104> ratio;
  Integer distance, width, factor, square;
  explicit InterpolationWorkspace(SequenceProfile profile) : ratio(profile) {}
  void difference(const BinaryParts& upper, const BinaryParts& lower,
                  Integer* output) {
    ratio.numerator.set(upper, 1074);
    ratio.negative = upper.negative;
    ratio.term.set(lower, 1074);
    ratio.add_term(!lower.negative);
    *output = ratio.numerator;
  }
  Result<std::uint64_t> smoothstep(
      const BinaryParts& x, const BinaryParts& lower, const BinaryParts& upper,
      bool narrow, const std::function<Status(std::uint64_t)>& consume) {
    auto status = consume(2048);
    if (!status.ok())
      return Result<std::uint64_t>(status);
    difference(x, lower, &distance);
    difference(upper, lower, &width);
    factor = width;
    factor.add(width);
    factor.add(width);
    factor.subtract(distance);
    factor.subtract(distance);
    status = multiply_fixed(distance, distance, &square, consume);
    if (status.ok())
      status = multiply_fixed(square, factor, &ratio.numerator, consume);
    if (status.ok())
      status = multiply_fixed(width, width, &square, consume);
    if (status.ok())
      status = multiply_fixed(square, width, &ratio.denominator, consume);
    if (!status.ok())
      return Result<std::uint64_t>(status);
    ratio.negative = false;
    return ratio.round(narrow, consume, 0);
  }
  Result<std::uint64_t> mix(
      const BinaryParts& a, const BinaryParts& b, const BinaryParts& t,
      bool narrow, const std::function<Status(std::uint64_t)>& consume) {
    auto status = consume(2048);
    if (!status.ok())
      return Result<std::uint64_t>(status);
    ratio.denominator.words.fill(0);
    ratio.denominator.words[1074 / 64] = UINT64_C(1) << (1074 % 64);
    ratio.numerator.set(a);
    ratio.negative = a.negative;
    ratio.product_term(t, b);
    ratio.product_term(t, a, true);
    if (!a.magnitude && !b.magnitude && a.negative && b.negative)
      return Result<std::uint64_t>(UINT64_C(1) << (narrow ? 31 : 63));
    return ratio.round(narrow, consume);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
