#pragma once

#include <cmath>
#include <optional>
#include <utility>

#include "05-filter/common.hpp"

namespace ps::plugin_internal::filter {
// hi+lo denotes an unevaluated exact real sum. error encloses everything
// discarded by the double-double operations, including gradual underflow.
struct Ball {
  double hi = 0, lo = 0, error = 0;
  static Ball bad() { return {0, 0, INFINITY}; }
  static Ball point(double x) {
    return x == 0 || (std::abs(x) >= 0x1p-200 && std::abs(x) <= 0x1p200)
               ? Ball{x, 0, 0}
               : bad();
  }
  bool finite() const {
    return std::isfinite(hi) && std::isfinite(lo) && std::isfinite(error);
  }
  bool point() const { return lo == 0 && error == 0; }
  static double up(double x) { return x == 0 ? 0 : numeric_ops::numeric_up(x); }
  static std::pair<double, double> two_sum(double a, double b) {
    const auto s = a + b, p = s - a;
    return {s, (a - (s - p)) + (b - p)};
  }
  Ball operator-() const { return {-hi, -lo, error}; }
  Ball operator+(const Ball& b) const {
    if (!finite() || !b.finite()) {
      return bad();
    }
    const auto s = two_sum(hi, b.hi);
    const auto t = two_sum(lo, b.lo), v = two_sum(s.second, t.first);
    const auto r = two_sum(s.first, v.first);
    const auto local = up(std::abs(t.second) + std::abs(v.second));
    return {r.first, r.second, up(up(error + b.error) + local)};
  }
  Ball times(double b) const {
    if (!finite() || !std::isfinite(b)) {
      return bad();
    }
    if (b == 1) {
      return *this;
    }
    if (b == -1) {
      return -*this;
    }
    const double p = hi * b, e = std::fma(hi, b, -p), c = lo * b;
    if (!std::isfinite(p) || !std::isfinite(c)) {
      return bad();
    }
    const auto t = two_sum(e, c), r = two_sum(p, t.first);
    const double high_error =
        std::abs(p) >= 0x1p-900 || hi == 0 || b == 0 ? 0 : 0x1p-1074;
    const double low_error = lo == 0 || b == 0 ? 0
                             : std::abs(c) >= 0x1p-900
                                 ? std::abs(std::fma(lo, b, -c))
                                 : up(up(0x1p-52 * std::abs(c)) + 0x1p-1074);
    const auto local = up(up(high_error + low_error) + std::abs(t.second));
    // A positive radius must remain positive even if its scaled value rounds
    // to zero; exact zero radii still need no artificial error term.
    const auto propagated =
        b != 0 && error != 0 ? numeric_ops::numeric_up(std::abs(b) * error) : 0;
    return {r.first, r.second, up(propagated + local)};
  }
  Fast bounds() const {
    if (!finite()) {
      return {-INFINITY, INFINITY};
    }
    const auto center = hi + lo;
    return {
        numeric_ops::numeric_down(numeric_ops::numeric_down(center) - error),
        numeric_ops::numeric_up(numeric_ops::numeric_up(center) + error)};
  }
};
struct BallValue {
  Ball numerator;
  Ball denominator{1, 0, 0};
  bool divided = false;
};
struct BoundedArithmetic {
  bool narrow;
  BallValue from(double value) const { return {Ball::point(value)}; }
  BallValue one() const { return {Ball{1, 0, 0}}; }
  BallValue zero() const { return {Ball{0, 0, 0}}; }
  BallValue add(BallValue a, BallValue b) const {
    if (b.divided || (a.divided && !b.numerator.point())) {
      return {Ball::bad()};
    }
    if (a.divided) {
      a.numerator = a.numerator + a.denominator.times(b.numerator.hi);
    } else {
      a.numerator = a.numerator + b.numerator;
    }
    return a;
  }
  BallValue multiply(BallValue a, BallValue b) const {
    if (a.divided || b.divided || !b.numerator.point()) {
      return {Ball::bad()};
    }
    a.numerator = a.numerator.times(b.numerator.hi);
    return a;
  }
  BallValue divide(BallValue a, BallValue b) const {
    if (a.divided || b.divided) {
      return {Ball::bad()};
    }
    a.denominator = b.numerator;
    a.divided = true;
    return a;
  }
  BallValue absolute(BallValue a) const {
    if (a.numerator.hi < 0 || (a.numerator.hi == 0 && a.numerator.lo < 0)) {
      a.numerator = -a.numerator;
    }
    return a;
  }
  bool is_zero(const BallValue& a) const {
    return a.numerator.point() && a.numerator.hi == 0;
  }
  bool is_nan(const BallValue& a) const { return !a.numerator.finite(); }
  std::optional<std::uint64_t> finish(BallValue value) const {
    auto n = value.numerator, d = value.denominator;
    auto bounds = d.bounds();
    if (!n.finite() || !d.finite()) {
      return {};
    }
    if (bounds.high < 0) {
      n = -n;
      d = -d;
      bounds = d.bounds();
    }
    if (!(bounds.low > 0)) {
      return {};
    }
    const auto q0 = n.hi / d.hi;
    const auto residual = n + (-d.times(q0));
    const auto q = q0 + (residual.hi + residual.lo) / d.hi;
    const auto raw = generation::convert(q, narrow);
    const auto candidate = numeric_ops::numeric_double(raw, narrow);
    if (!std::isfinite(candidate) || std::abs(candidate) < 0x1p-800 ||
        std::abs(candidate) > 0x1p800) {
      return {};
    }
    const auto exact_remainder = n + (-d.times(candidate));
    const auto remainder = exact_remainder.bounds();
    double previous, next;
    if (narrow) {
      previous = std::nextafter(static_cast<float>(candidate), -INFINITY);
      next = std::nextafter(static_cast<float>(candidate), INFINITY);
      if (!std::isfinite(previous)) {
        previous = -0x1p128;
      }
      if (!std::isfinite(next)) {
        next = 0x1p128;
      }
    } else {
      previous = numeric_ops::numeric_neighbor(candidate, false);
      next = numeric_ops::numeric_neighbor(candidate, true);
    }
    if (d.point() && exact_remainder.point()) {
      const auto left = d.hi * ((candidate - previous) * .5),
                 right = d.hi * ((next - candidate) * .5);
      // Scaling an exact binary64 denominator by a normal power of two is
      // exact. Resolve a proved midpoint with the destination's even bit.
      // A subnormal exact product can round upward to the smallest normal.
      // Exclude that endpoint before treating the scale products as exact.
      if (std::isnormal(left) && std::isnormal(right) && left > 0x1p-1022 &&
          right > 0x1p-1022) {
        const auto r = exact_remainder.hi;
        if (r > -left && r < right) {
          return raw;
        }
        if (r == -left || r == right) {
          return (raw & 1) == 0
                     ? raw
                     : generation::convert(r < 0 ? previous : next, narrow);
        }
      }
    }
    const auto left =
        numeric_ops::numeric_down(bounds.low * ((candidate - previous) * .5));
    const auto right =
        numeric_ops::numeric_down(bounds.low * ((next - candidate) * .5));
    if (std::isfinite(left) && std::isfinite(right) && remainder.low > -left &&
        remainder.high < right) {
      return raw;
    }
    return {};
  }
};
}  // namespace ps::plugin_internal::filter
