#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>

#include "01-numeric/exact_product.hpp"

namespace ps::plugin_internal::numeric_ops {
// Direct RN(sqrt(A/B * 2^scale)); no rounded variance is formed. Scratch lives
// in the supplied host-owned ratio workspace; numerator/denominator are
// destructively reused. Callers supply a bounded scale and prove scaled
// operands and midpoint products fit Words. NUM-11 needs at most 4280 of 4352
// bits.
template <std::size_t Words>
Result<std::uint64_t> round_sqrt_ratio(
    ExactRatioWorkspace<Words>* ratio, bool narrow, int scale,
    const std::function<Status(std::uint64_t)>& consume) {
  using Workspace = ExactRatioWorkspace<Words>;
  using Answer = Result<std::uint64_t>;
  const auto capacity = [] {
    return Answer(Status{ErrorCode::ResourceExhausted, "exact root capacity",
                         FailureReason::CapacityLimit});
  };
  auto status = consume(1024);
  if (!status.ok())
    return Answer(status);
  const auto a_top = Workspace::top(ratio->numerator);
  const auto b_top = Workspace::top(ratio->denominator);
  if (b_top < 0 || ratio->negative)
    return Answer(
        Status{ErrorCode::InvalidArgument, "invalid nonnegative root ratio"});
  if (a_top < 0)
    return Answer(UINT64_C(0));
  int top = a_top - b_top;
  if (top >= 0) {
    if (!Workspace::shift(ratio->denominator, static_cast<unsigned>(top),
                          &ratio->candidate))
      return capacity();
    if (ratio->compare(ratio->numerator, ratio->candidate) < 0)
      --top;
  } else {
    if (!Workspace::shift(ratio->numerator, static_cast<unsigned>(-top),
                          &ratio->candidate))
      return capacity();
    if (ratio->compare(ratio->candidate, ratio->denominator) < 0)
      --top;
  }
  const int scaled_top = top + scale;
  const int root_top = scaled_top >= 0 ? scaled_top / 2 : (scaled_top - 1) / 2;
  const unsigned fraction = narrow ? 23 : 52;
  const int minimum = narrow ? -149 : -1074;
  int quantum = std::max(minimum, root_top - static_cast<int>(fraction));
  const int alignment = 2 * quantum - scale;
  if (alignment >= 0) {
    if (!Workspace::shift(ratio->denominator, static_cast<unsigned>(alignment),
                          &ratio->shifted))
      return capacity();
  } else {
    if (!Workspace::shift(ratio->numerator, static_cast<unsigned>(-alignment),
                          &ratio->candidate))
      return capacity();
    ratio->numerator = ratio->candidate;
    ratio->shifted = ratio->denominator;
  }
  std::uint64_t significand = 0;
  for (unsigned i = fraction + 1; i; --i) {
    status = consume(512);
    if (!status.ok())
      return Answer(status);
    const auto next = significand | (UINT64_C(1) << (i - 1));
    integer_coefficient(static_cast<unsigned __int128>(next) * next,
                        &ratio->candidate);
    status =
        multiply_fixed(ratio->shifted, ratio->candidate, &ratio->term, consume);
    if (!status.ok())
      return Answer(status);
    if (ratio->compare(ratio->term, ratio->numerator) <= 0)
      significand = next;
  }
  status = consume(1024);
  if (!status.ok())
    return Answer(status);
  const auto midpoint = 2 * significand + 1;
  integer_coefficient(static_cast<unsigned __int128>(midpoint) * midpoint,
                      &ratio->candidate);
  status =
      multiply_fixed(ratio->shifted, ratio->candidate, &ratio->term, consume);
  if (!status.ok())
    return Answer(status);
  if (!Workspace::shift(ratio->numerator, 2, &ratio->denominator))
    return capacity();
  const auto order = ratio->compare(ratio->denominator, ratio->term);
  if (order > 0 || (!order && (significand & 1)))
    ++significand;
  if (significand == (UINT64_C(1) << (fraction + 1))) {
    significand >>= 1;
    ++quantum;
  }
  const auto implicit = UINT64_C(1) << fraction;
  std::uint64_t exponent = 0;
  if (significand >= implicit) {
    exponent = quantum - minimum + 1;
    significand -= implicit;
  }
  const auto maximum = narrow ? 255U : 2047U;
  if (exponent >= maximum) {
    exponent = maximum;
    significand = 0;
  }
  return Answer((exponent << fraction) | significand);
}
}  // namespace ps::plugin_internal::numeric_ops
