// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <type_traits>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "02-format-color/fast_color.hpp"
#include "07-grade/cie_observer.hpp"

namespace ps::plugin_internal::grade::finishing {
using fast_color::Matrix;
using fast_color::Triple;
inline constexpr Matrix kIdentity{{{{1, 0, 0}}, {{0, 1, 0}}, {{0, 0, 1}}}};
inline Triple white_xyz(const std::array<double, 2>& xy) {
  return {xy[0] / xy[1], 1, (1 - xy[0] - xy[1]) / xy[1]};
}
// The common spectral scale 1e-30 cancels. Differentiate the very same
// integer-nanometre Planck samples analytically, before normalizing the white.
inline Triple planck_white(double temperature, double tint) {
  constexpr double c2 = 6.62607015e-34 * 299792458 / 1.380649e-23;
  Triple sum{}, derivative{};
  for (unsigned i = 0; i < kObserver.size(); ++i) {
    const double wavelength = (360.0 + i) * 1e-9;
    const double z = c2 / (wavelength * temperature);
    const double denominator = std::expm1(z);
    const double p = 1e-30 / (std::pow(wavelength, 5) * denominator);
    const double dp = p * z / temperature * (1 + 1 / denominator);
    for (unsigned c = 0; c < 3; ++c) {
      sum[c] += p * kObserver[i][c];
      derivative[c] += dp * kObserver[i][c];
    }
  }
  const double d = sum[0] + 15 * sum[1] + 3 * sum[2];
  const double dd = derivative[0] + 15 * derivative[1] + 3 * derivative[2];
  double u = 4 * sum[0] / d, v = 6 * sum[1] / d;
  const double du = 4 * (derivative[0] * d - sum[0] * dd) / (d * d);
  const double dv = 6 * (derivative[1] * d - sum[1] * dd) / (d * d);
  const double sign = du < 0 ? -1 : 1;
  const double scale = sign * tint / std::hypot(du, dv);
  u -= dv * scale;
  v += du * scale;
  // Algebraic inverse UCS -> XYZ, with Y=1, avoiding a second xy division.
  return {1.5 * u / v, 1, (4 - u - 10 * v) / (2 * v)};
}
inline Matrix adaptation(const Triple& source, const Triple& target,
                         unsigned method) {
  constexpr Matrix h[] = {kIdentity,
                          {{{{.8951, .2664, -.1614}},
                            {{-.7502, 1.7135, .0367}},
                            {{.0389, -.0685, 1.0296}}}},
                          {{{{.7328, .4296, -.1624}},
                            {{-.7036, 1.6975, .0061}},
                            {{.0030, .0136, .9834}}}},
                          {{{{.401288, .650173, -.051461}},
                            {{-.250268, 1.204414, .045854}},
                            {{-.002079, .048952, .953127}}}}};
  if (source == target)
    return kIdentity;
  const auto s = fast_color::multiply(h[method], source);
  const auto t = fast_color::multiply(h[method], target);
  auto scaled = h[method];
  for (unsigned r = 0; r < 3; ++r)
    for (auto& value : scaled[r])
      value *= t[r] / s[r];
  return fast_color::multiply(fast_color::inverse(h[method]), scaled);
}
// Kind: gains, adaptation, temperature, weighted feature, Y, CIELAB, OKLab,
// posterize. All feature and color stages are Float64, including FP32 images.
struct Transform;
template <class T, unsigned C>
std::size_t dispatch_packets(const Transform&, const T*, T*, std::size_t);
struct Transform final {
  unsigned kind = 0;
  Matrix matrix = kIdentity, forward{}, inverse{}, to_lms{}, from_lms{};
  Triple white{1, 1, 1}, gains{1, 1, 1}, weights{};
  Triple neutral_roots{};
  double bias = 0, divisor = 1, levels = 7;
  bool identity = false, clamp = true;
  Triple apply(const Triple& rgb) const {
    if (kind == 0)
      return {rgb[0] * gains[0], rgb[1] * gains[1], rgb[2] * gains[2]};
    if (kind <= 2)
      return fast_color::multiply(matrix, rgb);
    if (kind == 3) {
      const double v =
          ((weights[0] * rgb[0] + weights[1] * rgb[1]) + weights[2] * rgb[2]) /
              divisor +
          bias;
      return {v, v, v};
    }
    if (kind == 4) {
      const double y = fast_color::dot(forward[1], rgb);
      return {y, y, y};
    }
    if (kind == 5) {
      const double y = fast_color::dot(forward[1], rgb);
      const double l = 1.16 * fast_color::cie_f(y) - .16;
      const double neutral_y = fast_color::cie_g((l + .16) / 1.16);
      return fast_color::multiply(
          inverse,
          Triple{white[0] * neutral_y, neutral_y, white[2] * neutral_y});
    }
    auto roots = fast_color::multiply(to_lms, rgb);
    for (auto& value : roots)
      value = std::cbrt(value);
    const double lightness = fast_color::dot(fast_color::kLmsToOklab[0], roots);
    Triple cubes{};
    for (unsigned c = 0; c < 3; ++c) {
      const double root = neutral_roots[c] * lightness;
      cubes[c] = root * root * root;
    }
    return fast_color::multiply(from_lms, cubes);
  }
  template <class T>
  T posterize(T x) const {
    // Explicit ties-even, independent of the process rounding direction.
    const T u = clamp ? std::min(T(1), std::max(T(0), x)) : x;
    if (u == 0 || u == 1)
      return u;
    // levels is an integer control: GRD-13 computes in the image dtype.
    const T count = static_cast<T>(levels);
    const T product = u * count;
    const T low = std::floor(product), fraction = product - low;
    const T q =
        low + T(fraction > T(.5) ||
                (fraction == T(.5) && (static_cast<unsigned>(low) & 1U)));
    return q / count;
  }
  template <class T, unsigned C>
  void span(const T* source, T* target, std::size_t count,
            const std::array<unsigned, 3>& selected) const {
    if (identity || (kind == 0 && gains == Triple{1, 1, 1})) {
      std::memcpy(target, source, count * C * sizeof(T));
      return;
    }
    std::size_t i = 0;
#if defined(__aarch64__)
    if (selected == std::array<unsigned, 3>{0, 1, 2})
      i = dispatch_packets<T, C>(*this, source, target, count);
#endif
    for (; i < count; ++i) {
      const T* x = source + i * C;
      T* y = target + i * (kind == 3 ? 1 : C);
      if (kind == 7) {
        for (unsigned c = 0; c < C; ++c) {
          if (std::find(selected.begin(), selected.end(), c) != selected.end())
            y[c] = posterize(x[c]);
          else
            std::memcpy(y + c, x + c, sizeof(T));
        }
        continue;
      }
      const auto result =
          apply({x[selected[0]], x[selected[1]], x[selected[2]]});
      if (kind == 3) {
        y[0] = static_cast<T>(result[0]);
        continue;
      }
      for (unsigned c = 0; c < C; ++c) {
        unsigned j = 0;
        while (j < 3 && selected[j] != c)
          ++j;
        if (j == 3 || (kind == 0 && gains[j] == 1))
          std::memcpy(y + c, x + c, sizeof(T));
        else
          y[c] = static_cast<T>(result[j]);
      }
    }
  }
};
}  // namespace ps::plugin_internal::grade::finishing

#include "07-grade/finishing_packets.hpp"
