#pragma once

#include <algorithm>
#include <cstdint>

#include "01-numeric/directed_functions.hpp"

namespace ps::plugin_internal::numeric_ops {
enum class LowpassKernel { Hann, Hamming, Blackman, Kaiser, Gaussian };
// Raw validated static parameters. Radius is in sample or coordinate units.
struct LowpassParameters {
  LowpassKernel kernel;
  std::uint64_t radius = 0, cutoff = 0, beta = 0, sigma = 0;
};
// Kernel evaluation encloses mathematical coefficients, never rounded NUM
// outputs. The caller owns/admit this fixed arena and supplies
// work/cancellation.
struct DirectedLowpassKernel {
  DirectedFunctions functions{SequenceProfile::Strict};
  using Interval = DirectedInterval::Interval;
  using Frame = DirectedInterval::Frame;
  // sin/cos on |x|<=2: after n>=2 both term magnitudes decrease. The next
  // omitted term bounds the alternating real remainder, with directed input
  // uncertainty retained separately by each interval operation.
  void sincos_small(Interval sine, Interval cosine, Interval input) {
    auto& m = functions.math;
    Frame frame(m);
    auto bound = m.interval();
    m.integer(bound, 2);
    if (m.compare(input.high, bound.high) > 0)
      throw DirectedInterval::Unresolved{};
    m.negate(bound, bound);
    if (m.compare(input.low, bound.low) < 0)
      throw DirectedInterval::Unresolved{};
    auto square = m.interval(), st = m.interval(), ct = m.interval(),
         ss = m.interval(), cs = m.interval();
    m.multiply(square, input, input);
    m.negate(square, square);
    m.copy(st, input);
    m.copy(ss, input);
    m.integer(ct, 1);
    m.integer(cs, 1);
    bool sd = false, cd = false;
    for (unsigned n = 1; n < m.precision + 64; ++n) {
      if (!sd) {
        m.multiply(st, st, square);
        m.divide_small(st, st, (2 * n) * (2 * n + 1));
        sd = n >= 2 && std::max(m.top(st.low), m.top(st.high)) <= 0;
        if (!sd)
          m.add(ss, ss, st);
      }
      if (!cd) {
        m.multiply(ct, ct, square);
        m.divide_small(ct, ct, (2 * n - 1) * (2 * n));
        cd = n >= 2 && std::max(m.top(ct.low), m.top(ct.high)) <= 0;
        if (!cd)
          m.add(cs, cs, ct);
      }
      if (sd && cd) {
        m.widen(ss, std::max(m.top(st.low), m.top(st.high)) < 0 ? 0 : 1);
        m.widen(cs, std::max(m.top(ct.low), m.top(ct.high)) < 0 ? 0 : 1);
        m.copy(sine, ss);
        m.copy(cosine, cs);
        return;
      }
    }
    throw DirectedInterval::Unresolved{};
  }
  void sincos_pi(Interval sine, Interval cosine, Interval phase) {
    auto& m = functions.math;
    Frame frame(m);
    auto doubled = m.interval(), multiple = m.interval(), delta = m.interval(),
         pi = m.interval(), s = m.interval(), c = m.interval();
    m.scale(doubled, phase, 1);
    auto& integer = m.number();
    m.shift(integer, doubled.low, -static_cast<int>(m.precision), true);
    auto quadrant = static_cast<unsigned>(integer.magnitude.words[0] & 3);
    if (integer.negative)
      quadrant = (4 - quadrant) & 3;
    m.shift(multiple.low, integer, static_cast<int>(m.precision) - 1, true);
    m.copy(multiple.high, multiple.low);
    m.subtract(delta, phase, multiple);
    m.constant(pi, true);
    m.multiply(delta, delta, pi);
    sincos_small(s, c, delta);
    functions.orient(sine, cosine, s, c, quadrant, false);
  }
  void sinc_pi(Interval output, Interval phase) {
    auto& m = functions.math;
    Frame frame(m);
    auto angle = m.interval(), pi = m.interval(), one = m.interval();
    m.constant(pi, true);
    m.multiply(angle, phase, pi);
    m.integer(one, 1);
    bool small = m.compare(angle.high, one.high) <= 0;
    m.negate(one, one);
    small = small && m.compare(angle.low, one.low) >= 0;
    if (small) {
      functions.sinc_series(output, angle);
      return;
    }
    auto sine = m.interval(), cosine = m.interval();
    sincos_pi(sine, cosine, phase);
    m.divide(output, sine, angle);
  }
  // exp(x) for x<=0. Below -precision, e>2 proves a positive upper
  // bound of one Q_precision unit. This is a real enclosure, not a final
  // destination underflow classification; the tap remains logically present.
  void negative_exponential(Interval output, Interval input) {
    auto& m = functions.math;
    Frame frame(m);
    auto limit = m.interval();
    m.integer(limit, -static_cast<std::int64_t>(m.precision));
    if (m.compare(input.high, limit.low) <= 0) {
      m.integer(output, 0);
      output.high.magnitude.words[0] = 1;
      return;
    }
    auto reduced = m.interval(), term = m.interval(), sum = m.interval();
    m.integer(limit, -1);
    m.scale(limit, limit, -4);
    unsigned squarings = 0;
    m.copy(reduced, input);
    while (m.compare(reduced.low, limit.low) < 0) {
      m.scale(reduced, reduced, -1);
      if (++squarings > 32)
        throw DirectedInterval::Unresolved{};
    }
    // Clamp roundoff above zero only using the caller's exact x<=0 proof.
    if (!reduced.high.negative)
      m.clear(reduced.high);
    m.integer(term, 1);
    m.integer(sum, 1);
    for (unsigned n = 1; n < m.precision + 64; ++n) {
      m.multiply(term, term, reduced);
      m.divide_small(term, term, n);
      const auto top = std::max(m.top(term.low), m.top(term.high));
      if (top <= 0) {
        m.widen(sum, top < 0 ? 0 : 2);
        for (unsigned j = 0; j < squarings; ++j)
          m.multiply(sum, sum, sum);
        if (sum.low.negative)
          m.clear(sum.low);
        m.copy(output, sum);
        return;
      }
      m.add(sum, sum, term);
    }
    throw DirectedInterval::Unresolved{};
  }
  // I0(sqrt(4z)) = sum z^n/(n!)^2, z>=0. No rounded sqrt is used.
  // Once z/(n+1)^2 <= 1/2, all following term ratios are <=1/2;
  // the omitted positive tail is at most twice the next term.
  void bessel_series(Interval output, Interval z) {
    auto& m = functions.math;
    Frame frame(m);
    auto term = m.interval(), sum = m.interval(), ratio = m.interval(),
         half = m.interval();
    m.integer(term, 1);
    m.integer(sum, 1);
    m.integer(half, 1);
    m.scale(half, half, -1);
    for (unsigned n = 1; n <= 16384; ++n) {
      m.multiply(term, term, z);
      m.divide_small(term, term, static_cast<std::uint64_t>(n) * n);
      m.divide_small(ratio, z, static_cast<std::uint64_t>(n + 1) * (n + 1));
      if (m.compare(ratio.high, half.low) <= 0 && m.top(term.high) <= 0) {
        if (m.top(term.high) >= 0) {
          auto error = m.interval();
          m.integer(error, 0);
          error.high.magnitude.words[0] = 2;
          m.add(sum, sum, error);
        }
        m.copy(output, sum);
        return;
      }
      m.add(sum, sum, term);
    }
    throw DirectedInterval::Unresolved{};
  }
  // Positive binary inputs: divide significands before applying their exact
  // exponent difference, avoiding a rounded-to-zero tiny denominator.
  void binary_ratio(Interval output, std::uint64_t numerator,
                    std::uint64_t denominator) {
    auto& m = functions.math;
    Frame frame(m);
    const auto a = BinaryParts::decode(numerator, false);
    const auto b = BinaryParts::decode(denominator, false);
    auto n = m.interval(), d = m.interval();
    m.dyadic(n, a.significand, 0);
    m.dyadic(d, b.significand, 0);
    m.divide(output, n, d);
    m.scale(output, output, a.exponent - b.exponent);
  }
  // u is a normalized support coordinate in [-1,1]. I0(beta) cancels
  // from the whole normalized expression; omit that common divisor exactly.
  void coefficient(Interval output, Interval u, const LowpassParameters& p) {
    auto& m = functions.math;
    Frame frame(m);
    auto square = m.interval(), parameter = m.interval(), radius = m.interval(),
         argument = m.interval(), one = m.interval(), window = m.interval();
    m.integer(one, 1);
    m.raw(radius, p.radius, false);
    m.multiply(square, u, u);
    if (p.kernel == LowpassKernel::Gaussian) {
      binary_ratio(argument, p.radius, p.sigma);
      m.multiply(argument, argument, argument);
      m.multiply(argument, argument, square);
      m.scale(argument, argument, -1);
      m.negate(argument, argument);
      negative_exponential(output, argument);
      return;
    }
    if (p.kernel == LowpassKernel::Kaiser) {
      m.raw(parameter, p.beta, false);
      m.multiply(parameter, parameter, parameter);
      m.scale(parameter, parameter, -2);
      m.subtract(argument, one, square);
      // The exact support argument is nonnegative, including both endpoints.
      if (argument.low.negative)
        m.clear(argument.low);
      m.multiply(argument, argument, parameter);
      bessel_series(window, argument);
    } else {
      auto sine = m.interval(), cosine = m.interval();
      sincos_pi(sine, cosine, u);
      if (p.kernel == LowpassKernel::Hann) {
        m.add(window, one, cosine);
        m.scale(window, window, -1);
      } else if (p.kernel == LowpassKernel::Hamming) {
        m.integer(parameter, 23);
        m.multiply(window, parameter, cosine);
        m.integer(parameter, 27);
        m.add(window, window, parameter);
        m.divide_small(window, window, 50);
      } else {
        // (1+c)*(17+8c)/50 = .42+.5c+.08cos(2*pi*u).
        m.add(window, one, cosine);
        m.scale(argument, cosine, 3);
        m.integer(parameter, 17);
        m.add(argument, argument, parameter);
        m.multiply(window, window, argument);
        m.divide_small(window, window, 50);
      }
    }
    m.raw(parameter, p.cutoff, false);
    m.multiply(argument, parameter, radius);
    m.multiply(argument, argument, u);
    m.scale(argument, argument, 1);
    sinc_pi(output, argument);
    m.multiply(output, output, window);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
