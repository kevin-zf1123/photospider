#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "01-numeric/exact_ratio.hpp"

namespace ps::plugin_internal::numeric_ops {
// Error and one-sided relative tolerance in common exact 2^-2148 units.
// Finite binary64 operands and their products fit below bit 4198 in 4352 bits.
struct ExactBakeError {
  PredicateInteger difference, temporary, threshold, product;
  std::array<PredicateInteger, 3> maxima;
  RatioWorkspace rounding{SequenceProfile::Strict};
  static int compare(const PredicateInteger& a, const PredicateInteger& b) {
    for (std::size_t i = a.words.size(); i; --i)
      if (a.words[i - 1] != b.words[i - 1])
        return a.words[i - 1] < b.words[i - 1] ? -1 : 1;
    return 0;
  }
  Result<bool> check(std::uint64_t reference, std::uint64_t value,
                     std::uint64_t atol, std::uint64_t rtol,
                     const std::function<Status(std::uint64_t)>& consume) {
    auto charged = consume(1024);
    if (!charged.ok())
      return Result<bool>(charged);
    auto a = BinaryParts::decode(reference, false),
         b = BinaryParts::decode(value, false);
    const auto high = a.magnitude >= b.magnitude ? a : b,
               low = a.magnitude >= b.magnitude ? b : a;
    difference.set(high);
    temporary.set(low);
    if (a.negative != b.negative)
      difference.add(temporary);
    else
      difference.subtract(temporary);
    threshold.set(BinaryParts::decode(atol, false));
    product.set_product(BinaryParts::decode(rtol, false), a);
    threshold.add(product);
    return Result<bool>(compare(difference, threshold) <= 0);
  }
  Result<std::uint64_t> upward(
      unsigned component, const std::function<Status(std::uint64_t)>& consume) {
    auto work = consume(256);
    if (!work.ok())
      return Result<std::uint64_t>(work);
    rounding.numerator = maxima[component];
    rounding.denominator.words.fill(0);
    rounding.denominator.words[0] = 1;
    rounding.negative = false;
    auto rounded = rounding.round(false, consume, -2148);
    if (!rounded.ok())
      return rounded;
    if (rounded.value() == UINT64_C(0x7ff0000000000000))
      return rounded;
    temporary.set(BinaryParts::decode(rounded.value(), false));
    return Result<std::uint64_t>(
        rounded.value() + (compare(temporary, maxima[component]) < 0 ? 1 : 0));
  }
};
}  // namespace ps::plugin_internal::numeric_ops
