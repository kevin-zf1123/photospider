#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "05-filter/certified.hpp"

namespace ps::plugin_internal::filter::pyramid_math {
inline std::int64_t reflect(std::int64_t i, std::uint64_t n) {
  const auto period = static_cast<std::int64_t>(2 * n);
  auto t = i % period;
  if (t < 0)
    t += period;
  return t < static_cast<std::int64_t>(n) ? t : period - 1 - t;
}
template <class Sample>
std::uint64_t pyramid_sample(Sample input, std::uint64_t h, std::uint64_t w,
                             std::uint64_t y, std::uint64_t x, bool expand,
                             bool narrow, generation::Math& m,
                             const Fuel& fuel) {
  constexpr int weights[5] = {1, 4, 6, 4, 1};
  Ball sum;
  double scalar = 0, absolute = 0, roundoff = 0;
  bool finite = true, negative_zero = true;
  std::array<Atom, 25> values;
  std::array<int, 25> coefficients{};
  unsigned count = 0;
  std::array<std::uint64_t, 5> ys{}, xs{};
  std::array<int, 5> yw{}, xw{};
  unsigned yn = 0, xn = 0;
  for (int delta = -2; delta <= 2; ++delta) {
    if (!expand || !((static_cast<std::int64_t>(y) - delta) & 1)) {
      const auto at = expand ? (static_cast<std::int64_t>(y) - delta) / 2
                             : 2 * static_cast<std::int64_t>(y) + delta;
      ys[yn] =
          at >= 0 && static_cast<std::uint64_t>(at) < h ? at : reflect(at, h);
      yw[yn++] = weights[delta + 2];
    }
    if (!expand || !((static_cast<std::int64_t>(x) - delta) & 1)) {
      const auto at = expand ? (static_cast<std::int64_t>(x) - delta) / 2
                             : 2 * static_cast<std::int64_t>(x) + delta;
      xs[xn] =
          at >= 0 && static_cast<std::uint64_t>(at) < w ? at : reflect(at, w);
      xw[xn++] = weights[delta + 2];
    }
  }
  for (unsigned iy = 0; iy < yn; ++iy)
    for (unsigned ix = 0; ix < xn; ++ix) {
      fuel.poll();
      values[count] = {input(ys[iy] * w + xs[ix]), narrow};
      coefficients[count] = yw[iy] * xw[ix];
      const double v = values[count].value();
      if (narrow) {
        const double term = v * coefficients[count];
        const auto addition = Ball::two_sum(scalar, term);
        scalar = addition.first;
        if (addition.second != 0)
          roundoff =
              numeric_ops::numeric_up(roundoff + std::abs(addition.second));
        absolute += std::abs(term);
        finite &= std::isfinite(v);
        negative_zero &= v == 0 && std::signbit(v);
      } else {
        sum = sum + Ball::point(v).times(coefficients[count]);
      }
      ++count;
    }
  const auto divisor = expand ? 64 : 256;
  if (narrow && finite) {
    if (absolute == 0)
      return negative_zero ? UINT64_C(0x80000000) : 0;
    // Float32 * integer products and division by this power of two are
    // exact binary64 operations. TwoSum tracks the actual discarded sums;
    // zero roundoff proves the direct RN32, including exact midpoints.
    if (roundoff == 0)
      return generation::convert(scalar / divisor, true);
    const double error = roundoff;
    const auto low = generation::convert(
        numeric_ops::numeric_down(scalar - error) / divisor, true);
    const auto high = generation::convert(
        numeric_ops::numeric_up(scalar + error) / divisor, true);
    if (low == high)
      return low;
  }
  auto result = narrow ? std::optional<std::uint64_t>{}
                       : BoundedArithmetic{false}.finish(
                             BallValue{sum, Ball::point(divisor), true});
  if (!result) {
    ExactArithmetic e{m.exact,
                      narrow ? ElementType::Float32 : ElementType::Float64,
                      {}};
    auto sum = m.exact.integer(0);
    for (unsigned i = 0; i < count; ++i) {
      fuel.poll();
      const auto term = m.scale(e.from(values[i]), coefficients[i]);
      sum = i ? m.add(sum, term) : term;
    }
    result = e.finish(m.div(sum, m.exact.integer(divisor)));
  }
  return *result;
}
inline std::uint64_t combine(std::uint64_t a, std::uint64_t b, bool subtract,
                             bool narrow) {
  const Atom x{a, narrow}, y{b, narrow};
  const auto type = narrow ? ElementType::Float32 : ElementType::Float64;
  if (x.nan())
    return numeric_ops::converted_nan(a, type, type);
  if (y.nan())
    return numeric_ops::converted_nan(b, type, type);
  if (narrow) {
    const float aa = static_cast<float>(x.value()),
                bb = static_cast<float>(y.value());
    const float value = subtract ? aa - bb : aa + bb;
    return std::isnan(value) ? UINT64_C(0x7fc00000)
                             : generation::convert(value, true);
  }
  const double value = subtract ? x.value() - y.value() : x.value() + y.value();
  return std::isnan(value) ? UINT64_C(0x7ff8000000000000) : bits(value);
}
template <class Raw>
std::uint64_t pyramid_pixel(const Raw* input, std::uint64_t h, std::uint64_t w,
                            std::uint64_t y, std::uint64_t x, bool expand,
                            bool narrow, generation::Math& m,
                            const Fuel& fuel) {
  return pyramid_sample([&](std::uint64_t i) { return input[i]; }, h, w, y, x,
                        expand, narrow, m, fuel);
}
}  // namespace ps::plugin_internal::filter::pyramid_math
