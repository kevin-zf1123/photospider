#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "01-numeric/accelerated_math.hpp"
#include "03-generation/noise_math.hpp"

namespace ps::plugin_internal::generation {
using Fast = numeric_ops::FastInterval;
inline Fast fast_abs(Fast x) {
  if (x.low >= 0)
    return x;
  if (x.high <= 0)
    return -x;
  return {0, std::max(-x.low, x.high)};
}
inline Fast fast_sqrt(Fast x) {
  if (!x.finite() || x.high < 0)
    return {-INFINITY, INFINITY};
  return {x.low <= 0 ? 0 : numeric_ops::numeric_down(std::sqrt(x.low)),
          numeric_ops::numeric_up(std::sqrt(x.high))};
}
inline Fast fast_center(std::int64_t origin, std::uint64_t index) {
  const __int128 twice = 2 * static_cast<__int128>(origin) + 2 * index + 1;
  const double value = static_cast<double>(twice) * .5;
  if (twice >= -(INT64_C(1) << 53) && twice <= (INT64_C(1) << 53))
    return Fast::point(value);
  return {numeric_ops::numeric_down(value), numeric_ops::numeric_up(value)};
}
// A successful FP32 test proves RN32 of the complete real expression.
// Accelerated FP64 uses the existing final-output NUM bound. Uncertain,
// zero, overflowing, and out-of-profile values retain exact arithmetic.
inline std::optional<std::uint64_t> fast_result(Fast value, bool narrow,
                                                bool accelerated) {
  if (!value.finite() || !value.nonzero())
    return {};
  if (narrow) {
    const auto lower = convert(value.low, true);
    const auto upper = convert(value.high, true);
    if (lower == upper && std::isfinite(static_cast<float>(value.low)))
      return lower;
    return {};
  }
  if (accelerated)
    return value.accepted(value.low + (value.high - value.low) * .5, false);
  return {};
}
inline std::optional<Fast> fast_turns(Fast y, Fast x) {
  if (!x.finite() || !y.finite() || !x.nonzero() || !y.nonzero())
    return {};
  const auto ax = fast_abs(x), ay = fast_abs(y);
  bool swap;
  if (ay.high <= ax.low)
    swap = false;
  else if (ax.high <= ay.low)
    swap = true;
  else
    return {};
  const auto one = Fast::point(1);
  auto z = (swap ? ax : ay) / (swap ? ay : ax);
  // Twice applying tan(theta/2) puts z in [0,tan(pi/16)] < 1/4.
  // The alternating atan series through z^33/33 has remainder <= z^35/35.
  z = z / (one + fast_sqrt(one + z * z));
  z = z / (one + fast_sqrt(one + z * z));
  if (!z.finite() || z.low < 0 || z.high > .25)
    return {};
  const auto square = z * z;
  auto power = z, angle = z;
  for (unsigned k = 1; k <= 16; ++k) {
    power = power * square;
    const auto term = power / Fast::point(2 * k + 1);
    angle = k & 1 ? angle - term : angle + term;
  }
  const auto remainder = fast_abs(power * square / Fast::point(35));
  angle = (angle + Fast{-remainder.high, remainder.high}) * Fast::point(4);
  // Adjacent binary64 values enclosing mathematical pi.
  const Fast pi{0x1.921fb54442d18p+1, 0x1.921fb54442d19p+1};
  if (swap)
    angle = pi * Fast::point(.5) - angle;
  if (x.high < 0)
    angle = pi - angle;
  if (y.high < 0)
    angle = -angle;
  return angle / (pi * Fast::point(2));
}
inline std::optional<Fast> fast_basis(const std::array<Fast, 3>& q, bool perlin,
                                      std::uint64_t seed, std::uint64_t stream,
                                      std::uint64_t frame,
                                      const std::array<std::int64_t, 2>& period,
                                      const Work& work) {
  const unsigned dimensions = perlin ? 3 : 2;
  std::array<Fast, 3> f, fade;
  std::array<std::int64_t, 3> cell{};
  const auto one = Fast::point(1);
  for (unsigned axis = 0; axis < dimensions; ++axis) {
    if (!q[axis].finite() || q[axis].low < -0x1p31 ||
        q[axis].high >= 0x1p31 - 1)
      return {};
    const auto low = std::floor(q[axis].low);
    if (low != std::floor(q[axis].high))
      return {};
    cell[axis] = static_cast<std::int64_t>(low);
    f[axis] = q[axis] - Fast::point(low);
    const auto v = f[axis];
    fade[axis] = v * v * v *
                 (v * (v * Fast::point(6) - Fast::point(15)) + Fast::point(10));
  }
  Fast sum = Fast::point(0);
  constexpr int gx[8] = {1, -1, 0, 0, 1, -1, 1, -1};
  constexpr int gy[8] = {0, 0, 1, -1, 1, 1, -1, -1};
  for (unsigned corner = 0; corner < (1U << dimensions); ++corner) {
    work.poll(perlin ? 1 : 10);
    std::array<Fast, 3> delta;
    for (unsigned axis = 0; axis < dimensions; ++axis)
      delta[axis] = f[axis] - Fast::point((corner >> axis) & 1);
    Fast dot;
    if (perlin) {
      const auto hash =
          perm(perm(perm(static_cast<unsigned>(cell[0]) + (corner & 1)) +
                    static_cast<unsigned>(cell[1]) + ((corner >> 1) & 1)) +
               static_cast<unsigned>(cell[2]) + (corner >> 2)) &
          15;
      auto u = delta[hash < 8 ? 0 : 1];
      auto v = delta[hash < 4 ? 1 : (hash == 12 || hash == 14) ? 0 : 2];
      if (hash & 1)
        u = -u;
      if (hash & 2)
        v = -v;
      dot = u + v;
    } else {
      std::array<std::int64_t, 2> address{cell[0] + (corner & 1),
                                          cell[1] + (corner >> 1)};
      for (unsigned axis = 0; axis < 2; ++axis) {
        if (period[axis]) {
          address[axis] %= period[axis];
          if (address[axis] < 0)
            address[axis] += period[axis];
        }
      }
      const auto hash =
          philox(address[0], address[1], 0, 6, seed, stream, frame)[0] & 7;
      dot = delta[0] * Fast::point(gx[hash]) + delta[1] * Fast::point(gy[hash]);
    }
    for (unsigned axis = 0; axis < dimensions; ++axis)
      dot = dot * ((corner & (1U << axis)) ? fade[axis] : one - fade[axis]);
    sum = sum + dot;
  }
  return sum;
}
}  // namespace ps::plugin_internal::generation
