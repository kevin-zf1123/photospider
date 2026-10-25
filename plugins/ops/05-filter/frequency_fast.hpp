#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include "05-filter/frequency.hpp"

namespace ps::plugin_internal::frequency {
inline Ball sqrt_ball(Ball x) {
  const auto bounds = x.bounds();
  if (!x.finite() || bounds.low <= 0 || bounds.high > 0x1p200)
    return Ball::bad();
  const auto r = std::sqrt(x.hi);
  const Ball initial{r, 0, 0};
  auto result =
      initial + dd_divide(x + (-dd_product(initial, initial)), {2 * r, 0, 0});
  const auto residual = (x + (-dd_product(result, result))).bounds();
  const auto lower = numeric_ops::numeric_down(std::sqrt(bounds.low));
  const auto denominator =
      numeric_ops::numeric_down(lower + result.bounds().low);
  if (!(denominator > 0))
    return Ball::bad();
  result.error = Ball::up(
      result.error + numeric_ops::numeric_up(std::max(std::abs(residual.low),
                                                      std::abs(residual.high)) /
                                             denominator));
  return result;
}
inline std::optional<std::uint64_t> finish_ball(Ball value, bool narrow,
                                                Profile profile) {
  if (value.point() && value.hi == 0)
    return 0;
  return profile == Profile::Strict
             ? dd_round(value, narrow)
             : FastArithmetic{narrow, profile}.finish(value.bounds());
}
inline Ball frequency_coordinate(std::uint64_t k, std::uint64_t n,
                                 double spacing) {
  const auto signed_k =
      k <= n / 2 ? static_cast<std::int64_t>(k)
                 : static_cast<std::int64_t>(k) - static_cast<std::int64_t>(n);
  return dd_divide(Ball::point(static_cast<double>(signed_k)),
                   Ball::point(spacing).times(static_cast<double>(n)));
}
template <unsigned Degree = 24>
inline Ball exp_ball(Ball exponent, const Fuel& fuel) {
  struct PolynomialFuel {
    const Fuel& fuel;
    void poll(std::uint64_t n) const { fuel.poll(n * 128); }
  };
  const auto value = dd_exp_enclosure<Degree>(exponent, PolynomialFuel{fuel});
  return value ? *value : Ball::bad();
}
inline Ball low_ball(Ball r2, double cutoff, const std::string& mode,
                     std::int64_t order, const Fuel& fuel) {
  auto u = dd_divide(r2, dd_product(Ball::point(cutoff), Ball::point(cutoff)));
  if (mode == "gaussian")
    return exp_ball(u.times(-.5), fuel);
  Ball power{1, 0, 0};
  for (auto n = static_cast<std::uint64_t>(order); n; n >>= 1) {
    fuel.poll(3);
    if (n & 1)
      power = dd_product(power, u);
    if (n > 1)
      u = dd_product(u, u);
  }
  return dd_divide({1, 0, 0}, sqrt_ball(power + Ball{1, 0, 0}));
}
inline std::optional<bool> integral_ball(Ball v) {
  if (!v.finite())
    return {};
  if (v.point())
    return std::floor(v.hi) == v.hi;
  const auto b = v.bounds();
  if (std::ceil(b.low) > std::floor(b.high))
    return false;
  return {};
}
inline Ball torus_ball(Ball f, double center, double spacing) {
  const auto u = (f + Ball::point(-center));
  const auto period = dd_divide({1, 0, 0}, Ball::point(spacing));
  const auto quotient = dd_divide(u, period);
  if (!quotient.finite() || std::abs(quotient.hi) > 4)
    return Ball::bad();
  const double integer = std::nearbyint(quotient.hi + quotient.lo);
  const auto reduced = quotient + Ball{-integer, 0, 0};
  const auto bounds = reduced.bounds();
  if (!(reduced.point() && std::abs(reduced.hi) <= .5) &&
      (bounds.low < -.5 || bounds.high > .5))
    return Ball::bad();
  return u + (-period.times(integer));
}
inline std::optional<std::uint64_t> response_candidate(const Frequency& s,
                                                       std::uint64_t y,
                                                       std::uint64_t x,
                                                       const Fuel& fuel) {
  const auto ny = (s.dims[0] - y) % s.dims[0], nx = (s.dims[1] - x) % s.dims[1];
  if (ny * s.dims[1] + nx < y * s.dims[1] + x) {
    y = ny;
    x = nx;
  }
  const auto fy = frequency_coordinate(y, s.dims[0], s.grid.dy);
  const auto fx = frequency_coordinate(x, s.dims[1], s.grid.dx);
  if (s.kind == Kind::Notch) {
    if (s.depth == 0)
      return bits(1);
    Ball value{1, 0, 0};
    for (const auto& center : s.centers) {
      const auto self_y = integral_ball(
          dd_product(Ball::point(center.first), Ball::point(s.grid.dy))
              .times(2));
      const auto self_x = integral_ball(
          dd_product(Ball::point(center.second), Ball::point(s.grid.dx))
              .times(2));
      if (!self_y || !self_x)
        return {};
      for (int sign : {1, -1}) {
        if (sign < 0 && *self_y && *self_x)
          break;
        const auto dy = dd_divide(
            torus_ball(fy, sign * center.first, s.grid.dy), Ball::point(s.sy));
        const auto dx = dd_divide(
            torus_ball(fx, sign * center.second, s.grid.dx), Ball::point(s.sx));
        const auto exponent =
            (dd_product(dy, dy) + dd_product(dx, dx)).times(-.5);
        if (s.depth == 1 && exponent.point() && exponent.hi == 0)
          return 0;
        value = dd_product(
            value, Ball{1, 0, 0} + (-exp_ball(exponent, fuel).times(s.depth)));
      }
    }
    return finish_ball(value, false, s.profile);
  }
  const auto r2 = dd_product(fy, fy) + dd_product(fx, fx);
  if (x == 0 && y == 0)
    return bits(s.pass == "low" || s.pass == "reject" ? 1 : 0);
  if (s.mode == "ideal") {
    const auto compare = [&](double cutoff) -> std::optional<bool> {
      const auto delta =
          r2 + (-dd_product(Ball::point(cutoff), Ball::point(cutoff)));
      if (delta.point() && delta.hi == 0)
        return true;
      const auto b = delta.bounds();
      if (b.high <= 0)
        return true;
      if (b.low > 0)
        return false;
      return {};
    };
    auto high = compare(s.high), low = s.kind == Kind::Band
                                           ? compare(s.low)
                                           : std::optional<bool>(false);
    if (!high || !low)
      return {};
    const bool inside = *high && !*low;
    return bits(
        (s.pass == "high" || s.pass == "reject" ? !inside : inside) ? 1 : 0);
  }
  auto value = low_ball(r2, s.high, s.mode, s.order, fuel);
  if (s.kind == Kind::Band)
    value = value + (-low_ball(r2, s.low, s.mode, s.order, fuel));
  if (s.pass == "high" || s.pass == "reject")
    value = Ball{1, 0, 0} + (-value);
  return finish_ball(value, false, s.profile);
}
}  // namespace ps::plugin_internal::frequency
