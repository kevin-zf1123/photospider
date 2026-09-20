#pragma once

#include <algorithm>
#include <cstdint>

#include "01-numeric/directed_interval.hpp"

namespace ps::plugin_internal::numeric_ops {
// Analytic remainders supplement directed rounding at every operation. A
// refinement may succeed only when both enclosing endpoints round identically.
struct DirectedFunctions final {
  struct RangeResult {
    bool overflow;
  };
  DirectedInterval math;
  using Interval = DirectedInterval::Interval;
  using Frame = DirectedInterval::Frame;
  explicit DirectedFunctions(SequenceProfile profile) : math(profile) {}
  // Precondition: finite positive input, already classified by the adapter.
  void logarithm(Interval output, const BinaryParts& input) {
    Frame frame(math);
    const int exponent =
        input.exponent + 63 - __builtin_clzll(input.significand);
    auto normalized = math.interval(), one = math.interval(),
         numerator = math.interval(), denominator = math.interval();
    math.dyadic(normalized, input.significand, input.exponent - exponent);
    math.integer(one, 1);
    math.subtract(numerator, normalized, one);
    math.add(denominator, normalized, one);
    auto z = math.interval(), square = math.interval(), power = math.interval(),
         sum = math.interval(), term = math.interval();
    math.divide(z, numerator, denominator);
    math.multiply(square, z, z);
    math.copy(power, z);
    math.integer(sum, 0);
    // z in [0,1/3]. The remainder after k terms is bounded above by
    // 2*z^(2k+1)/((2k+1)*(1-z^2)) <= 3*z^(2k+1).
    for (unsigned k = 0; k < math.precision + 64; ++k) {
      math.scale(term, power, 1);
      math.divide_small(term, term, 2 * k + 1);
      math.add(sum, sum, term);
      math.multiply(power, power, square);
      if (math.top(power.high) <= 0) {
        math.widen(sum, math.top(power.high) < 0 ? 0 : 3);
        auto constant = math.interval(), scale = math.interval();
        math.constant(constant, false);
        math.integer(scale, exponent);
        math.multiply(constant, constant, scale);
        math.add(output, sum, constant);
        return;
      }
    }
    throw DirectedInterval::Unresolved{};
  }
  void exponential(Interval output, Interval input) {
    Frame frame(math);
    auto limit = math.interval();
    math.integer(limit, 1024);
    if (math.compare(input.low, limit.high) >= 0)
      throw RangeResult{true};
    math.negate(limit, limit);
    // e > 8/3 > 2^(5/4), hence e^-1024 < 2^-1280 < half of
    // the smallest binary64 subnormal. These branches certify the final
    // destination range instead of fabricating a finite real enclosure.
    if (math.compare(input.high, limit.low) <= 0)
      throw RangeResult{false};
    math.integer(limit, 1024);
    if (math.compare(input.high, limit.high) > 0)
      throw DirectedInterval::Unresolved{};
    math.negate(limit, limit);
    if (math.compare(input.low, limit.low) < 0)
      throw DirectedInterval::Unresolved{};
    auto reduced = math.interval(), term = math.interval(),
         sum = math.interval();
    math.scale(reduced, input, -14);
    math.integer(term, 1);
    math.integer(sum, 1);
    // |r|<=1/16. After term n, the remaining absolute tail is less
    // than 2*|next term|. Directed term endpoints enclose that magnitude.
    for (unsigned n = 1; n < math.precision + 64; ++n) {
      math.multiply(term, term, reduced);
      math.divide_small(term, term, n);
      if (std::max(math.top(term.low), math.top(term.high)) <= 0) {
        math.widen(
            sum, std::max(math.top(term.low), math.top(term.high)) < 0 ? 0 : 2);
        for (unsigned j = 0; j < 14; ++j)
          math.multiply(sum, sum, sum);
        math.copy(output, sum);
        return;
      }
      math.add(sum, sum, term);
    }
    throw DirectedInterval::Unresolved{};
  }
  void sincos_series(Interval sine, Interval cosine, Interval input) {
    Frame frame(math);
    auto square = math.interval(), sin_term = math.interval(),
         cos_term = math.interval();
    auto sin_sum = math.interval(), cos_sum = math.interval();
    math.multiply(square, input, input);
    math.negate(square, square);
    math.copy(sin_term, input);
    math.copy(sin_sum, input);
    math.integer(cos_term, 1);
    math.integer(cos_sum, 1);
    // |r|<=pi/4<1. Alternating Taylor remainders are bounded by the
    // first omitted term; interval multiplication also retains input error.
    bool sin_done = false, cos_done = false;
    for (unsigned n = 1; n < math.precision + 64; ++n) {
      if (!sin_done) {
        math.multiply(sin_term, sin_term, square);
        math.divide_small(sin_term, sin_term, (2 * n) * (2 * n + 1));
        if (std::max(math.top(sin_term.low), math.top(sin_term.high)) <= 0)
          sin_done = true;
        else
          math.add(sin_sum, sin_sum, sin_term);
      }
      if (!cos_done) {
        math.multiply(cos_term, cos_term, square);
        math.divide_small(cos_term, cos_term, (2 * n - 1) * (2 * n));
        if (std::max(math.top(cos_term.low), math.top(cos_term.high)) <= 0)
          cos_done = true;
        else
          math.add(cos_sum, cos_sum, cos_term);
      }
      if (sin_done && cos_done) {
        math.widen(sin_sum,
                   std::max(math.top(sin_term.low), math.top(sin_term.high)) < 0
                       ? 0
                       : 1);
        math.widen(cos_sum,
                   std::max(math.top(cos_term.low), math.top(cos_term.high)) < 0
                       ? 0
                       : 1);
        math.copy(sine, sin_sum);
        math.copy(cosine, cos_sum);
        return;
      }
    }
    throw DirectedInterval::Unresolved{};
  }
  void sinc_series(Interval output, Interval input) {
    Frame frame(math);
    auto square = math.interval(), term = math.interval(),
         sum = math.interval();
    math.multiply(square, input, input);
    math.negate(square, square);
    math.integer(term, 1);
    math.integer(sum, 1);
    // sin(x)/x = sum (-x*x)^k/(2k+1)!, including x=0 by continuity.
    // Caller proves |x|<=1; the alternating remainder is the next term.
    for (unsigned k = 1; k < math.precision + 64; ++k) {
      math.multiply(term, term, square);
      math.divide_small(term, term, (2 * k) * (2 * k + 1));
      const auto bound = std::max(math.top(term.low), math.top(term.high));
      if (bound <= 0) {
        math.widen(sum, bound < 0 ? 0 : 1);
        math.copy(output, sum);
        return;
      }
      math.add(sum, sum, term);
    }
    throw DirectedInterval::Unresolved{};
  }
  void orient(Interval sine, Interval cosine, Interval s, Interval c,
              unsigned quadrant, bool negative) {
    Frame frame(math);
    auto sin_result = math.interval(), cos_result = math.interval();
    math.copy(sin_result, (quadrant & 1) ? c : s);
    math.copy(cos_result, (quadrant & 1) ? s : c);
    if ((quadrant >= 2) != negative)
      math.negate(sin_result, sin_result);
    if (quadrant == 1 || quadrant == 2)
      math.negate(cos_result, cos_result);
    math.copy(sine, sin_result);
    math.copy(cosine, cos_result);
  }
  void sincos(Interval sine, Interval cosine, const BinaryParts& input) {
    Frame frame(math);
    auto x = math.interval(), half_pi = math.interval(),
         quotient = math.interval(), half = math.interval();
    math.dyadic(x, input.significand, input.exponent);
    math.constant(half_pi, true);
    math.scale(half_pi, half_pi, -1);
    math.divide(quotient, x, half_pi);
    math.integer(half, 1);
    math.scale(half, half, -1);
    math.add(quotient, quotient, half);
    auto& lower = math.number();
    auto& upper = math.number();
    math.shift(lower, quotient.low, -static_cast<int>(math.precision), true);
    math.shift(upper, quotient.high, -static_cast<int>(math.precision), true);
    if (math.compare(lower, upper) != 0)
      throw DirectedInterval::Unresolved{};
    const auto quadrant = static_cast<unsigned>(lower.magnitude.words[0] & 3);
    auto multiple = math.interval(), reduced = math.interval(),
         s = math.interval(), c = math.interval();
    math.shift(multiple.low, lower, static_cast<int>(math.precision), true);
    math.copy(multiple.high, multiple.low);
    math.multiply(multiple, multiple, half_pi);
    math.subtract(reduced, x, multiple);
    // Rounding the exact quotient to its nearest integer proves the reduced
    // mathematical point is within pi/4. The enclosure can be slightly wider,
    // but must remain within [-1,1] for the series remainder proof.
    auto bound = math.interval();
    math.integer(bound, 1);
    if (math.compare(reduced.high, bound.high) > 0)
      throw DirectedInterval::Unresolved{};
    math.negate(bound, bound);
    if (math.compare(reduced.low, bound.low) < 0)
      throw DirectedInterval::Unresolved{};
    sincos_series(s, c, reduced);
    orient(sine, cosine, s, c, quadrant, input.negative);
  }
  void arctangent(Interval output, Interval input) {
    Frame frame(math);
    auto one = math.interval(), half = math.interval(), z = math.interval(),
         offset = math.interval();
    math.integer(one, 1);
    math.scale(half, one, -1);
    math.integer(offset, 0);
    // Caller supplies 0<=t<=1. For t>1/2, atan(t)=pi/4+
    // atan((t-1)/(t+1)); the Taylor argument then lies in [-1/3,0].
    if (math.compare(input.low, half.high) > 0) {
      auto numerator = math.interval(), denominator = math.interval();
      math.subtract(numerator, input, one);
      math.add(denominator, input, one);
      math.divide(z, numerator, denominator);
      math.constant(offset, true);
      math.scale(offset, offset, -2);
    } else {
      if (math.compare(input.high, half.high) > 0)
        throw DirectedInterval::Unresolved{};
      math.copy(z, input);
    }
    auto square = math.interval(), power = math.interval(),
         sum = math.interval(), term = math.interval();
    math.multiply(square, z, z);
    math.negate(square, square);
    math.copy(power, z);
    math.copy(sum, z);
    // |z|<=1/2. The alternating remainder is at most the omitted
    // z^(2k+1)/(2k+1), including negative z by oddness.
    for (unsigned k = 1; k < math.precision + 64; ++k) {
      math.multiply(power, power, square);
      math.divide_small(term, power, 2 * k + 1);
      if (std::max(math.top(term.low), math.top(term.high)) <= 0) {
        math.widen(
            sum, std::max(math.top(term.low), math.top(term.high)) < 0 ? 0 : 1);
        math.add(output, sum, offset);
        // Intersect with the proved atan(t)>=0 domain bound, keeping signed
        // underflow classification separate from interval roundoff.
        if (math.compare(input.low, one.low) < 0 && output.low.negative)
          math.clear(output.low);
        return;
      }
      math.add(sum, sum, term);
    }
    throw DirectedInterval::Unresolved{};
  }
};
}  // namespace ps::plugin_internal::numeric_ops
