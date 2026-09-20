#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>

#include "01-numeric/accelerated_math.hpp"
#include "photospider/core/status.hpp"

namespace ps::plugin_internal::numeric_ops {
struct CurveNumber {
  double value;
  FastInterval bound;
  static CurveNumber point(double x) { return {x, FastInterval::point(x)}; }
  CurveNumber operator+(CurveNumber b) const {
    return {value + b.value, bound + b.bound};
  }
  CurveNumber operator-(CurveNumber b) const {
    return {value - b.value, bound - b.bound};
  }
  CurveNumber operator*(CurveNumber b) const {
    return {value * b.value, bound * b.bound};
  }
  CurveNumber operator/(CurveNumber b) const {
    return {value / b.value, bound / b.bound};
  }
  bool valid() const { return std::isfinite(value) && bound.finite(); }
};
// The selected exact stencil and all branch predicates belong to the adapter.
// Bounds include real arithmetic; ambiguous slope-limit predicates fall back.
inline std::optional<CurveNumber> accelerated_curve(
    bool pchip, unsigned knots, unsigned first, unsigned count,
    unsigned segment, double query, const std::array<std::uint64_t, 4>& xs,
    const std::array<std::uint64_t, 4>& ys) {
  std::array<CurveNumber, 4> x{}, y{};
  std::array<CurveNumber, 3> h{}, delta{};
  for (unsigned i = 0; i < count; ++i) {
    x[i] = CurveNumber::point(numeric_double(xs[i]));
    y[i] = CurveNumber::point(numeric_double(ys[i]));
    if (!x[i].valid() || !y[i].valid())
      return {};
    if (i) {
      h[i - 1] = x[i] - x[i - 1];
      delta[i - 1] = (y[i] - y[i - 1]) / h[i - 1];
      if (!h[i - 1].valid() || h[i - 1].bound.low <= 0 || !delta[i - 1].valid())
        return {};
    }
  }
  const unsigned j = segment - first;
  const auto one = CurveNumber::point(1), two = CurveNumber::point(2),
             three = CurveNumber::point(3);
  const auto t = (CurveNumber::point(query) - x[j]) / h[j];
  if (!pchip || knots == 2) {
    auto result = y[j] * (one - t) + y[j + 1] * t;
    return result.valid() ? std::optional<CurveNumber>(result) : std::nullopt;
  }
  const auto sign = [](double a, double b) {
    return a > b ? 1 : a < b ? -1 : 0;
  };
  const auto slope = [&](unsigned node) -> std::optional<CurveNumber> {
    if (node && node + 1 != knots) {
      const auto k = node - first;
      const auto a = sign(y[k].value, y[k - 1].value),
                 b = sign(y[k + 1].value, y[k].value);
      if (!a || a != b)
        return CurveNumber::point(0);
      const auto w1 = two * h[k] + h[k - 1], w2 = h[k] + two * h[k - 1];
      auto result = (w1 + w2) / (w1 / delta[k - 1] + w2 / delta[k]);
      return result.valid() ? std::optional<CurveNumber>(result) : std::nullopt;
    }
    const unsigned a = node ? node - first - 1 : 0, b = node ? a - 1 : 1;
    const auto sign_a = sign(y[a + 1].value, y[a].value),
               sign_b = sign(y[b + 1].value, y[b].value);
    if (!sign_a)
      return CurveNumber::point(0);
    auto result =
        ((two * h[a] + h[b]) * delta[a] - h[a] * delta[b]) / (h[a] + h[b]);
    if (!result.valid())
      return {};
    if ((sign_a > 0 && result.bound.high < 0) ||
        (sign_a < 0 && result.bound.low > 0))
      return CurveNumber::point(0);
    if (!result.bound.nonzero())
      return {};
    if (sign_a != sign_b) {
      const auto limit = three * delta[a];
      const auto excess = result.bound - limit.bound;
      if (!excess.nonzero())
        return {};
      if ((sign_a > 0 && excess.low > 0) || (sign_a < 0 && excess.high < 0))
        result = limit;
    }
    return result;
  };
  auto m0 = slope(segment), m1 = slope(segment + 1);
  if (!m0 || !m1)
    return {};
  CurveNumber result;
  if (query < x[j].value) {
    result = y[j] + (*m0) * (CurveNumber::point(query) - x[j]);
  } else if (query > x[j + 1].value) {
    result = y[j + 1] + (*m1) * (CurveNumber::point(query) - x[j + 1]);
  } else {
    const auto u = one - t;
    result = y[j] * u * u * (one + two * t) +
             y[j + 1] * t * t * (three - two * t) +
             h[j] * t * u * (u * (*m0) - t * (*m1));
  }
  return result.valid() ? std::optional<CurveNumber>(result) : std::nullopt;
}
}  // namespace ps::plugin_internal::numeric_ops
