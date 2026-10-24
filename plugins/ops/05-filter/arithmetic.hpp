#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "05-filter/bounded_math.hpp"

namespace ps::plugin_internal::filter {
struct Atom {
  std::uint64_t raw;
  bool narrow = false;
  double value() const { return numeric_ops::numeric_double(raw, narrow); }
  bool zero() const {
    return (raw & (narrow ? UINT64_C(0x7fffffff)
                          : UINT64_C(0x7fffffffffffffff))) == 0;
  }
  bool nan() const {
    return narrow ? (raw & UINT64_C(0x7fffffff)) > UINT64_C(0x7f800000)
                  : (raw & UINT64_C(0x7fffffffffffffff)) >
                        UINT64_C(0x7ff0000000000000);
  }
};
struct WideArithmetic : BoundedArithmetic {
  explicit WideArithmetic(bool n) : BoundedArithmetic{n} {}
  BallValue from(Atom a) const { return BoundedArithmetic::from(a.value()); }
};
struct ExactArithmetic {
  Exact& e;
  ElementType output;
  std::optional<std::uint64_t> first_nan;
  Rational from(Atom a) {
    if (a.nan() && !first_nan) {
      first_nan = numeric_ops::converted_nan(
          a.raw, a.narrow ? ElementType::Float32 : ElementType::Float64,
          output);
    }
    return e.binary(a.raw, a.narrow);
  }
  Rational one() const { return e.integer(1); }
  Rational zero() const { return e.integer(0); }
  Rational add(const Rational& a, const Rational& b) const {
    return e.add(a, b);
  }
  Rational multiply(const Rational& a, const Rational& b) const {
    return e.multiply(a, b);
  }
  Rational divide(const Rational& a, const Rational& b) const {
    return e.divide(a, b);
  }
  Rational absolute(const Rational& a) const { return e.absolute(a); }
  bool is_zero(const Rational& a) const { return Exact::zero(a); }
  bool is_nan(const Rational& a) const { return Exact::nan(a); }
  std::optional<std::uint64_t> finish(const Rational& a) {
    check(e.status());
    if (first_nan) {
      return first_nan;
    }
    auto r = e.rounded(a, output == ElementType::Float32);
    check(e.status());
    return r;
  }
};
struct FastArithmetic {
  bool narrow;
  Profile profile;
  Fast from(Atom a) const { return Fast::point(a.value()); }
  Fast one() const { return Fast::point(1); }
  Fast zero() const { return Fast::point(0); }
  Fast add(Fast a, Fast b) const {
    if (a.low == a.high && b.low == b.high) {
      const double sum = a.low + b.low, part = sum - a.low;
      const double residual = (a.low - (sum - part)) + (b.low - part);
      if (std::isfinite(sum) && residual == 0) {
        return Fast::point(sum);
      }
    }
    return a + b;
  }
  Fast multiply(Fast a, Fast b) const {
    if (a.low == 1 && a.high == 1) {
      return b;
    }
    if (b.low == 1 && b.high == 1) {
      return a;
    }
    if (a.low == -1 && a.high == -1) {
      return -b;
    }
    if (b.low == -1 && b.high == -1) {
      return -a;
    }
    if (a.low == a.high && b.low == b.high) {
      const double product = a.low * b.low;
      // The residual cannot underflow in this range (at most 106 bits).
      if (std::isfinite(product) && std::abs(product) >= 0x1p-900 &&
          std::fma(a.low, b.low, -product) == 0) {
        return Fast::point(product);
      }
    }
    return a * b;
  }
  Fast divide(Fast a, Fast b) const {
    if (b.low == 1 && b.high == 1) {
      return a;
    }
    if (b.low == -1 && b.high == -1) {
      return -a;
    }
    return a / b;
  }
  // A certified point numerator/denominator permits a single hardware RN64
  // divide. For RN32, correct only a genuine binary32 midpoint using the exact
  // sign of the FMA remainder; the exponent guards exclude residual underflow.
  std::optional<std::uint64_t> quotient(Fast n, Fast d) const {
    if (n.low != n.high || d.low != d.high || !n.finite() || !d.finite() ||
        std::abs(n.low) < 0x1p-800 || std::abs(n.low) > 0x1p800 ||
        std::abs(d.low) < 0x1p-100 || std::abs(d.low) > 0x1p100) {
      return {};
    }
    const double q = n.low / d.low;
    if (!narrow) {
      return bits(q);
    }
    const double rounded =
        numeric_ops::numeric_double(generation::convert(q, true), true);
    double midpoint;
    if (std::isfinite(rounded)) {
      if (q == rounded) {
        return generation::convert(q, true);
      }
      const float adjacent = std::nextafter(static_cast<float>(rounded),
                                            q > rounded ? INFINITY : -INFINITY);
      const double next =
          std::isfinite(adjacent) ? adjacent : std::copysign(0x1p128, adjacent);
      midpoint = (rounded + next) * .5;
    } else {
      midpoint = std::copysign(0x1p128 - 0x1p103, q);
    }
    if (q == midpoint) {
      const double residual = std::fma(-q, d.low, n.low);
      if (residual != 0) {
        const bool up = (residual > 0) == (d.low > 0);
        return generation::convert(numeric_ops::numeric_neighbor(q, up), true);
      }
    }
    return generation::convert(q, true);
  }
  Fast absolute(Fast a) const {
    return a.low >= 0    ? a
           : a.high <= 0 ? -a
                         : Fast{0, std::max(-a.low, a.high)};
  }
  bool is_zero(Fast a) const { return a.low == 0 && a.high == 0; }
  bool is_nan(Fast a) const { return !a.finite(); }
  std::optional<std::uint64_t> finish(Fast a) const {
    if (!a.finite()) {
      return {};
    }
    const auto lo = generation::convert(a.low, narrow),
               hi = generation::convert(a.high, narrow);
    if (lo == hi) {
      return lo;
    }
    if (profile != Profile::Strict) {
      return a.accepted(a.low + (a.high - a.low) * .5, narrow);
    }
    return {};
  }
};
}  // namespace ps::plugin_internal::filter
