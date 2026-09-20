#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "01-numeric/exact_ratio.hpp"
#include "01-numeric/numeric_nan.hpp"
#include "photospider/data/value.hpp"

namespace ps::plugin_internal::numeric_ops {
// Four binary64 products plus one bias at 2^-2148 need fewer than 4200
// magnitude bits. Caller-owned 4352-bit workspace also covers final rounding.
struct ExactDot final {
  RatioWorkspace ratio;
  explicit ExactDot(SequenceProfile profile) : ratio(profile) {}
  Result<std::uint64_t> evaluate(
      const std::array<std::uint64_t, 4>& vectors,
      const std::array<std::uint64_t, 4>& matrix, std::uint64_t bias,
      unsigned count, ElementType dtype,
      const std::function<Status(std::uint64_t)>& consume) {
    using Answer = Result<std::uint64_t>;
    auto work = consume(512);
    if (!work.ok())
      return Answer(work);
    const bool narrow = dtype == ElementType::Float32;
    const auto infinity =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
    const auto nan = infinity | (UINT64_C(1) << (narrow ? 22 : 51));
    // All source reads/validation have already completed before this priority
    // pass; generated exceptional values cannot hide a later source NaN.
    for (const auto* source : {&vectors, &matrix})
      for (unsigned j = 0; j < count; ++j)
        if (BinaryParts::decode((*source)[j], narrow).nan)
          return Answer(converted_nan((*source)[j], dtype, dtype));
    const auto offset = BinaryParts::decode(bias, narrow);
    if (offset.nan)
      return Answer(converted_nan(bias, dtype, dtype));
    ratio.numerator.words.fill(0);
    ratio.negative = false;
    bool positive_inf = false, negative_inf = false;
    bool all_negative_zero = offset.negative && !offset.magnitude;
    for (unsigned j = 0; j < count; ++j) {
      work = consume(512);
      if (!work.ok())
        return Answer(work);
      const auto a = BinaryParts::decode(vectors[j], narrow);
      const auto b = BinaryParts::decode(matrix[j], narrow);
      const bool negative = a.negative != b.negative;
      if ((a.infinite && !b.magnitude) || (b.infinite && !a.magnitude))
        return Answer(nan);
      all_negative_zero &= negative && (!a.magnitude || !b.magnitude);
      if (a.infinite || b.infinite) {
        negative_inf |= negative;
        positive_inf |= !negative;
      } else {
        ratio.product_term(a, b);
      }
    }
    if (offset.infinite) {
      negative_inf |= offset.negative;
      positive_inf |= !offset.negative;
    } else {
      ratio.term.set(offset, 2148);
      ratio.add_term(offset.negative);
    }
    if (negative_inf && positive_inf)
      return Answer(nan);
    if (negative_inf || positive_inf)
      return Answer(infinity | (negative_inf ? sign : 0));
    if (RatioWorkspace::top(ratio.numerator) < 0)
      return Answer(all_negative_zero ? sign : 0);
    ratio.denominator.words.fill(0);
    ratio.denominator.words[0] = 1;
    return ratio.round(narrow, consume, -2148);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
