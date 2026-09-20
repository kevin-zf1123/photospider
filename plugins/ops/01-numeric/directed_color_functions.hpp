#pragma once

#include <algorithm>

#include "01-numeric/directed_functions.hpp"

namespace ps::plugin_internal::numeric_ops {
// Color expressions retain real enclosures for intermediate results, including
// values outside the IEEE destination range. The elementary NUM exp adapter's
// final-result RangeResult classification must not be used at this boundary.
struct DirectedColorFunctions final {
  DirectedFunctions functions;
  using Interval = DirectedInterval::Interval;
  using Number = DirectedInterval::Number;
  using Frame = DirectedInterval::Frame;
  explicit DirectedColorFunctions(SequenceProfile profile)
      : functions(profile) {}

  void logarithm_point(Interval output, const Number& input) {
    auto& math = functions.math;
    if (input.negative || math.top(input) < 0)
      throw DirectedInterval::Unresolved{};
    Frame frame(math);
    const int exponent = math.top(input) - static_cast<int>(math.precision);
    auto normalized = math.interval(), one = math.interval(),
         numerator = math.interval(), denominator = math.interval();
    math.shift(normalized.low, input, -exponent, true);
    math.shift(normalized.high, input, -exponent, false);
    math.integer(one, 1);
    math.subtract(numerator, normalized, one);
    math.add(denominator, normalized, one);
    auto z = math.interval(), square = math.interval(), power = math.interval(),
         sum = math.interval(), term = math.interval();
    math.divide(z, numerator, denominator);
    math.multiply(square, z, z);
    math.copy(power, z);
    math.integer(sum, 0);
    // Exact normalized point lies in [1,2); z lies in [0,1/3).
    // Outward normalization/division retain it, and 3*next_power bounds
    // the mathematical remainder of the positive atanh series.
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
  void logarithm(Interval output, Interval input) {
    auto& math = functions.math;
    Frame frame(math);
    auto lower = math.interval(), upper = math.interval();
    logarithm_point(lower, input.low);
    logarithm_point(upper, input.high);
    math.copy(output.low, lower.low);
    math.copy(output.high, upper.high);
  }

  void exponential_point(Interval output, const Number& input) {
    auto& math = functions.math;
    Frame frame(math);
    auto limit = math.interval();
    math.integer(limit, -static_cast<int>(math.precision));
    if (math.compare(input, limit.low) <= 0) {
      // e>2 proves exp(x)<=2^-precision. Preserve the nonzero real as
      // [0,one fixed-point unit], even when it is below binary64 range.
      math.clear(output.low);
      math.clear(output.high);
      output.high.magnitude.words[0] = 1;
      return;
    }
    // Multiplications square Q_precision endpoints before rescaling. Keep
    // their raw products below 12288 bits, with space for outward error.
    auto capacity = math.interval();
    math.constant(limit, false);
    math.integer(capacity, 6144 - static_cast<int>(math.precision) - 64);
    math.multiply(limit, limit, capacity);
    if (math.compare(input, limit.low) > 0)
      DirectedInterval::capacity();
    auto reduced = math.interval(), term = math.interval(),
         sum = math.interval();
    math.shift(reduced.low, input, -17, true);
    math.shift(reduced.high, input, -17, false);
    math.integer(term, 1);
    math.integer(sum, 1);
    // The admitted x is within [-4096,8192], hence |x/2^17|<=1/16.
    // Twice the first omitted term bounds the Taylor remainder.
    for (unsigned n = 1; n < math.precision + 64; ++n) {
      math.multiply(term, term, reduced);
      math.divide_small(term, term, n);
      const auto bound = std::max(math.top(term.low), math.top(term.high));
      if (bound <= 0) {
        math.widen(sum, bound < 0 ? 0 : 2);
        for (unsigned j = 0; j < 17; ++j)
          math.multiply(sum, sum, sum);
        math.copy(output, sum);
        if (output.low.negative)
          math.clear(output.low);
        return;
      }
      math.add(sum, sum, term);
    }
    throw DirectedInterval::Unresolved{};
  }
  void exponential(Interval output, Interval input) {
    auto& math = functions.math;
    Frame frame(math);
    auto lower = math.interval(), upper = math.interval();
    exponential_point(lower, input.low);
    exponential_point(upper, input.high);
    math.copy(output.low, lower.low);
    math.copy(output.high, upper.high);
  }
  // Positive exponent, nonnegative base. A zero lower endpoint is handled
  // analytically; logarithm is never asked to approximate log(0).
  void power(Interval output, Interval base, Interval exponent) {
    auto& math = functions.math;
    if (base.low.negative || exponent.low.negative ||
        math.top(exponent.low) < 0)
      throw DirectedInterval::Unresolved{};
    if (math.top(base.high) < 0) {
      math.integer(output, 0);
      return;
    }
    Frame frame(math);
    auto log = math.interval(), product = math.interval();
    if (math.top(base.low) < 0) {
      logarithm_point(log, base.high);
      math.multiply(product, log, exponent);
      exponential(output, product);
      math.clear(output.low);
      return;
    }
    logarithm(log, base);
    math.multiply(product, log, exponent);
    exponential(output, product);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
