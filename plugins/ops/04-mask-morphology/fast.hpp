#pragma once

#include <algorithm>
#include <optional>

#include "01-numeric/accelerated_math.hpp"
#include "04-mask-morphology/common.hpp"

namespace ps::plugin_internal::mask {
using Fast = numeric_ops::FastInterval;
inline Fast unit(Fast v) {
  if (!v.finite())
    return v;
  return {std::clamp(v.low, 0.0, 1.0), std::clamp(v.high, 0.0, 1.0)};
}
inline std::optional<std::uint64_t> certified(Fast v, bool narrow) {
  if (!v.finite())
    return {};
  const auto low = convert(v.low, narrow), high = convert(v.high, narrow);
  return low == high ? std::optional<std::uint64_t>{low} : std::nullopt;
}
inline Fast square(Fast v) {
  auto result = v * v;
  if (result.finite())
    result.low = std::max(0.0, result.low);
  return result;
}
inline Fast fast_curve(Fast v, bool smooth) {
  v = unit(v);
  if (!v.finite())
    return v;
  if (v.low == v.high && (v.low == 0 || v.low == 1))
    return v;
  return smooth ? unit(v * v * (Fast::point(3) - Fast::point(2) * v)) : v;
}
inline std::optional<std::uint64_t> radial(Fast squared, double inner,
                                           double outer, bool smooth,
                                           bool narrow) {
  if (!squared.finite())
    return {};
  const Fast distance{
      std::max(0.0, numeric_ops::numeric_down(
                        std::sqrt(std::max(0.0, squared.low)))),
      numeric_ops::numeric_up(std::sqrt(std::max(0.0, squared.high)))};
  if (distance.high <= inner)
    return convert(1, narrow);
  if (distance.low >= outer && (inner != outer || distance.low > inner))
    return 0;
  if (inner == outer)
    return {};
  const auto t = (distance - Fast::point(inner)) /
                 (Fast::point(outer) - Fast::point(inner));
  return certified(unit(Fast::point(1) - fast_curve(t, smooth)), narrow);
}
}  // namespace ps::plugin_internal::mask
