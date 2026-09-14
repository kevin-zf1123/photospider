#pragma once

#include <cstdint>
#include <functional>

#include "01-numeric/exact_ratio.hpp"

namespace ps::plugin_internal::numeric_ops {
// Finite binary64 differences need at most 2099 bits. The smoothstep cubic
// needs fewer than 6300 bits; division alignment plus 53 quotient bits fits
// in 6656 bits. Every large temporary belongs to the host continuation.
struct InterpolationWorkspace final {
  using Integer = FixedInteger<104>;
  ExactRatioWorkspace<104> ratio;
  Integer distance, width, factor, square;
  explicit InterpolationWorkspace(SequenceProfile profile) : ratio(profile) {}
  Status multiply(const Integer& a, const Integer& b, Integer* output,
                  const std::function<Status(std::uint64_t)>& consume) {
    // Output is distinct from both inputs; a == b is allowed for squaring.
    output->words.fill(0);
    const auto a_size = (ratio.top(a) + 64) / 64;
    const auto b_size = (ratio.top(b) + 64) / 64;
    for (int i = 0; i < a_size; ++i) {
      auto status = consume(16 * b_size + 1);
      if (!status.ok())
        return status;
      unsigned __int128 carry = 0;
      for (int j = 0; j < b_size; ++j) {
        const auto slot = static_cast<std::size_t>(i + j);
        if (slot >= output->words.size())
          return Status{ErrorCode::ResourceExhausted,
                        "interpolation product capacity",
                        FailureReason::CapacityLimit};
        const auto product =
            static_cast<unsigned __int128>(a.words[i]) * b.words[j] +
            output->words[slot] + carry;
        output->words[slot] = static_cast<std::uint64_t>(product);
        carry = product >> 64;
      }
      const auto slot = static_cast<std::size_t>(i + b_size);
      if (carry) {
        if (slot >= output->words.size())
          return Status{ErrorCode::ResourceExhausted,
                        "interpolation carry capacity",
                        FailureReason::CapacityLimit};
        output->words[slot] = static_cast<std::uint64_t>(carry);
      }
    }
    return Status::success();
  }
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
    status = multiply(distance, distance, &square, consume);
    if (status.ok())
      status = multiply(square, factor, &ratio.numerator, consume);
    if (status.ok())
      status = multiply(width, width, &square, consume);
    if (status.ok())
      status = multiply(square, width, &ratio.denominator, consume);
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
