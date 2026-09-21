#pragma once

#include <cstdint>
#include <functional>

#include "01-numeric/exact_aggregate.hpp"
#include "01-numeric/exact_product.hpp"

namespace ps::plugin_internal::numeric_ops {
struct ExactCalculus final {
  ExactAggregate sum;
  RatioWorkspace ratio;
  std::uint64_t first = 0, last = 0, count = 0;
  ExactCalculus(SequenceProfile profile, ElementType dtype)
      : sum(profile, AggregateKind::Sum, dtype), ratio(profile) {}
  Status add(std::uint64_t bits,
             const std::function<Status(std::uint64_t)>& consume) {
    auto status = sum.add(bits, consume);
    if (!status.ok())
      return status;
    if (!count)
      first = bits;
    last = bits;
    ++count;
    return Status::success();
  }
  Result<std::uint64_t> derivative(
      std::uint64_t low, std::uint64_t high, std::uint64_t step, bool interior,
      const std::function<Status(std::uint64_t)>& consume) {
    using Answer = Result<std::uint64_t>;
    auto charged = consume(1024);
    if (!charged.ok())
      return Answer(charged);
    const bool narrow = sum.type == ElementType::Float32;
    const auto a = BinaryParts::decode(low, narrow);
    const auto b = BinaryParts::decode(high, narrow);
    const auto h = BinaryParts::decode(step, narrow);
    if (a.nan || b.nan)
      return Answer(converted_nan(a.nan ? low : high, sum.type, sum.type));
    const auto infinity =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    if (a.infinite && b.infinite && a.negative == b.negative)
      return Answer(infinity | (UINT64_C(1) << (narrow ? 22 : 51)));
    if (a.infinite || b.infinite) {
      const bool negative =
          (b.infinite ? b.negative : !a.negative) != h.negative;
      return Answer(infinity | (static_cast<std::uint64_t>(negative)
                                << (narrow ? 31 : 63)));
    }
    ratio.numerator.words.fill(0);
    ratio.negative = false;
    ratio.term.set(b, 1074);
    ratio.add_term(b.negative);
    ratio.term.set(a, 1074);
    ratio.add_term(!a.negative);
    ratio.denominator.set(h, 1074);
    if (interior) {
      if (!RatioWorkspace::shift(ratio.denominator, 1, &ratio.shifted))
        return Answer(Status{ErrorCode::ResourceExhausted,
                             "derivative denominator capacity",
                             FailureReason::CapacityLimit});
      ratio.denominator = ratio.shifted;
    }
    ratio.negative = ratio.negative != h.negative;
    return ratio.round(narrow, consume, 0, true);
  }
  // With N<=2^40, the weighted sum 2*sum-first-last has <=2139 bits
  // in 2^-1074 units. Its product with step plus aligned initial fits within
  // 4238 bits. Final scale -2149 incorporates the trapezoidal factor one half.
  Result<std::uint64_t> integral(
      std::uint64_t step, std::uint64_t initial,
      const std::function<Status(std::uint64_t)>& consume) {
    using Answer = Result<std::uint64_t>;
    auto charged = consume(2048);
    if (!charged.ok())
      return Answer(charged);
    const bool narrow = sum.type == ElementType::Float32;
    const auto offset = BinaryParts::decode(initial, narrow);
    const auto h = BinaryParts::decode(step, narrow);
    const auto infinity =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    const auto nan = infinity | (UINT64_C(1) << (narrow ? 22 : 51));
    const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
    if (offset.nan || sum.has_nan)
      return Answer(converted_nan(offset.nan ? initial : sum.first_nan,
                                  sum.type, sum.type));
    if (sum.negative_inf && sum.positive_inf)
      return Answer(nan);
    if (sum.negative_inf || sum.positive_inf) {
      const bool negative = sum.negative_inf != h.negative;
      if (offset.infinite && offset.negative != negative)
        return Answer(nan);
      return Answer(infinity | (negative ? sign : 0));
    }
    if (offset.infinite)
      return Answer(initial);
    if (!RatioWorkspace::shift(sum.ratio.numerator, 1, &ratio.numerator))
      return Answer(Status{ErrorCode::ResourceExhausted,
                           "integration sum capacity",
                           FailureReason::CapacityLimit});
    ratio.negative = sum.ratio.negative;
    for (auto endpoint : {first, last}) {
      const auto parts = BinaryParts::decode(endpoint, narrow);
      ratio.term.set(parts, 1074);
      ratio.add_term(!parts.negative);
    }
    ratio.term.set(h, 1074);
    auto status =
        multiply_fixed(ratio.numerator, ratio.term, &ratio.shifted, consume);
    if (!status.ok())
      return Answer(status);
    ratio.numerator = ratio.shifted;
    ratio.negative = ratio.negative != h.negative;
    ratio.term.set(offset, 2149);
    ratio.add_term(offset.negative);
    ratio.denominator.words.fill(0);
    ratio.denominator.words[0] = 1;
    return ratio.round(narrow, consume, -2149, true);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
