#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

#include "05-filter/certified.hpp"

namespace ps::plugin_internal::filter {
// A cheap proof of exact binary64 evaluation. Failed proofs leave the wider
// enclosure/rational path in charge; this is not an input-domain validation.
struct ExactDouble {
  double value;
  bool exact = true;
  static ExactDouble point(double v) { return {v, std::isfinite(v)}; }
  ExactDouble add(ExactDouble b) const {
    const auto s = Ball::two_sum(value, b.value);
    return {s.first,
            exact && b.exact && std::isfinite(s.first) && s.second == 0};
  }
  ExactDouble multiply(ExactDouble b) const {
    const double p = value * b.value;
    const bool zero = value == 0 || b.value == 0;
    const bool safe = std::abs(p) >= 0x1p-900 && std::abs(p) <= 0x1p900;
    return {p, exact && b.exact && std::isfinite(p) &&
                   (zero || (safe && std::fma(value, b.value, -p) == 0))};
  }
  ExactDouble divide(ExactDouble b) const {
    const double q = value / b.value;
    const bool safe =
        std::abs(value) >= 0x1p-400 && std::abs(value) <= 0x1p400 &&
        std::abs(b.value) >= 0x1p-400 && std::abs(b.value) <= 0x1p400;
    return {q, exact && b.exact && safe && std::isfinite(q) &&
                   std::fma(-q, b.value, value) == 0};
  }
};
// A deliberately loose binary64 exponential enclosure for final RN32 proofs.
// For |r|<=.36, the degree-13 Taylor tail is <2^-56. Rounded coefficients,
// 13 FMA Horner steps, and split-ln2 reduction contribute <2^-47 absolute;
// 2^-44 leaves more than an eightfold margin. Input uncertainty is bounded by
// exp's derivative <=1 on [-64,0]. Ambiguous RN32 cases keep the DD fallback.
inline std::optional<double> exp13(double x, const Fuel& fuel) {
  if (x < -64.00001 || x > .00001)
    return {};
  constexpr double ln2_hi = 0x1.62e42fefa39efp-1;
  constexpr double ln2_lo = 0x1.abc9e3b39803fp-56;
  const int k = static_cast<int>(std::nearbyint(x / ln2_hi));
  const double r = std::fma(-static_cast<double>(k), ln2_hi, x) -
                   static_cast<double>(k) * ln2_lo;
  if (std::abs(r) > .36)
    return {};
  constexpr double coefficients[] = {1,
                                     1,
                                     .5,
                                     0x1.5555555555555p-3,
                                     0x1.5555555555555p-5,
                                     0x1.1111111111111p-7,
                                     0x1.6c16c16c16c17p-10,
                                     0x1.a01a01a01a01ap-13,
                                     0x1.a01a01a01a01ap-16,
                                     0x1.71de3a556c734p-19,
                                     0x1.27e4fb7789f5cp-22,
                                     0x1.ae64567f544e4p-26,
                                     0x1.1eed8eff8d898p-29,
                                     0x1.6124613a86d09p-33};
  double value = coefficients[13];
  fuel.poll(26);
  for (int i = 12; i >= 0; --i)
    value = std::fma(value, r, coefficients[i]);
  value = std::ldexp(value, k);
  return value;
}
inline std::optional<Ball> float_exp_enclosure(Ball x, const Fuel& fuel) {
  if (!x.finite())
    return {};
  const auto bounds = x.bounds();
  if (bounds.low < -64 || bounds.high > 0)
    return {};
  const auto approximation = exp13(x.hi, fuel);
  if (!approximation)
    return {};
  const double value = *approximation;
  const double radius =
      std::max(std::abs(bounds.low - x.hi), std::abs(bounds.high - x.hi));
  if (radius > 1e-5)
    return {};
  return Ball{value, 0, numeric_ops::numeric_up(0x1p-44 + 2 * radius)};
}
// This scalar certificate avoids DD bookkeeping for ordinary Float32 data.
// On the guarded exponent range every binary64 operation is normal. The
// reduction/exp13 error and the (1-e)/(1+e) sensitivity give |delta
// tanh|<2^-41; 2^-39*|amount*scale| plus 2^-48*(|a|+|amount*scale|) also covers
// the input subtraction/division, amplitude and final multiply/add rounding.
// Both RN32 endpoints must have identical bits, including their zero signs.
inline std::optional<std::uint64_t> contrast_float(Atom a, Atom b,
                                                   double amount, double scale,
                                                   const Fuel& fuel) {
  if (!a.narrow || !b.narrow || !std::isfinite(a.value()) ||
      !std::isfinite(b.value()) || std::abs(amount) < 0x1p-100 ||
      std::abs(amount) > 0x1p100 || scale < 0x1p-100 || scale > 0x1p100)
    return {};
  const double d = a.value() - b.value();
  const double x = std::abs(d) / scale;
  double t = 1;
  if (x < 32) {
    const auto exponential = exp13(-2 * x, fuel);
    if (!exponential)
      return {};
    t = (1 - *exponential) / (1 + *exponential);
  }
  if (std::signbit(d))
    t = -t;
  const double factor = amount * scale;
  const double value = a.value() + factor * t;
  const double error = std::ldexp(std::abs(factor), -39) +
                       std::ldexp(std::abs(a.value()) + std::abs(factor), -48);
  const auto lo =
      generation::convert(numeric_ops::numeric_down(value - error), true);
  const auto hi =
      generation::convert(numeric_ops::numeric_up(value + error), true);
  return lo == hi ? std::optional<std::uint64_t>(lo) : std::nullopt;
}
inline std::optional<std::uint64_t> dyadic_remap(Atom input, Atom guide,
                                                 double sigma, double alpha,
                                                 double beta) {
  if (!input.narrow || !guide.narrow)
    return {};
  const auto v = ExactDouble::point(input.value());
  const auto g = ExactDouble::point(guide.value());
  const auto d = v.add(ExactDouble::point(-g.value));
  if (!d.exact)
    return {};
  const auto z = ExactDouble::point(std::abs(d.value));
  ExactDouble f{0, false};
  if (z.value <= sigma) {
    if (alpha == 1)
      return input.zero() ? 0 : input.raw;
    if (alpha == 2)
      f = z.multiply(z).divide(ExactDouble::point(sigma));
  } else {
    f = z.add(ExactDouble::point(-sigma))
            .multiply(ExactDouble::point(beta))
            .add(ExactDouble::point(sigma));
  }
  if (d.value < 0)
    f.value = -f.value;
  const auto output = g.add(f);
  if (output.exact)
    return output.value == 0 ? 0 : generation::convert(output.value, true);
  return {};
}
inline bool disk_contains(double r, std::int64_t y, std::int64_t x,
                          generation::Math& m) {
  if (r <= 0x1p26) {
    // Integer squared distance is exact through 2^53. If RN64(r*r)
    // equals that integer, the exact FMA residual resolves the boundary.
    const double distance =
        static_cast<double>(y) * y + static_cast<double>(x) * x;
    const double radius = r * r;
    return distance < radius ||
           (distance == radius && std::fma(r, r, -radius) >= 0);
  }
  constexpr std::int64_t exact_integer = INT64_C(1) << 53;
  if (x >= -exact_integer && x <= exact_integer && y >= -exact_integer &&
      y <= exact_integer) {
    // Ball must start from the exact integer, not a rounded conversion.
    const auto radius = Ball::point(r).times(r);
    const auto distance = Ball::point(static_cast<double>(y)).times(y) +
                          Ball::point(static_cast<double>(x)).times(x);
    const auto d = distance + (-radius);
    if (d.point())
      return d.hi <= 0;
    const auto bounds = d.bounds();
    if (bounds.high <= 0)
      return true;
    if (bounds.low > 0)
      return false;
  }
  auto& e = m.exact;
  const auto result = e.order(m.add(m.sq(e.integer(y)), m.sq(e.integer(x))),
                              m.sq(m.value(r))) <= 0;
  check(e.status());
  return result;
}
}  // namespace ps::plugin_internal::filter
