// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "01-numeric/fast_curve.hpp"
#include "02-format-color/fast_color.hpp"

namespace ps::plugin_internal::grade::gradient {
struct Feature final {
  unsigned mode = 0;  // 0 max RGB, 1 linear Y, 2 CIELAB l, 3 OKLab L.
  fast_color::Matrix rgb_to_xyz{}, rgb_to_lms{};
  double operator()(const fast_color::Triple& rgb) const {
    if (!mode)
      return std::max(rgb[0], std::max(rgb[1], rgb[2]));
    if (mode == 3) {
      auto roots = fast_color::multiply(rgb_to_lms, rgb);
      for (auto& x : roots)
        x = std::cbrt(x);
      return fast_color::dot(fast_color::kLmsToOklab[0], roots);
    }
    const double y = fast_color::dot(rgb_to_xyz[1], rgb);
    if (mode == 1)
      return y;
    constexpr double epsilon = 216.0 / 24389.0;
    return y > epsilon ? (116 * std::cbrt(y) - 16) / 100
                       : (24389.0 / 27.0) * y / 100;
  }
};
inline fast_color::Triple evaluate(const fast_curve::Curve* curves, double q) {
  const auto& curve = curves[0];
  std::size_t row = 0;
  if (q <= curve.x[0]) {
    row = 0;
  } else if (q >= curve.x[curve.size - 1]) {
    row = curve.size - 1;
  } else {
    row = curve.segment(q);
    if (q != curve.x[row]) {
      const double t = q - curve.x[row];
      fast_color::Triple result;
      for (unsigned c = 0; c < 3; ++c) {
        const auto& column = curves[c];
        result[c] = column.identity || column.constant
                        ? column.identity_or_constant(q)
                        : std::fma(t, column.a[row], column.y[row]);
      }
      return result;
    }
  }
  return {curves[0].y[row], curves[1].y[row], curves[2].y[row]};
}
template <class T, unsigned Channels>
inline bool small_image_span(const T*, T*, std::size_t, const Feature&,
                             const fast_curve::Curve*);
template <class T>
inline bool small_feature_span(const T*, T*, std::size_t,
                               const fast_curve::Curve*);
template <class T, unsigned Channels>
inline void image_span(const T* source, T* output, std::size_t count,
                       const Feature& feature,
                       const fast_curve::Curve* curves) {
#if defined(__x86_64__)
  if (native_compute::available()) {
    native_compute::gradient_map(sizeof(T) == 4, Channels, feature, curves,
                                 source, output, count);
    return;
  }
#endif
  if (small_image_span<T, Channels>(source, output, count, feature, curves))
    return;
  for (std::size_t i = 0; i < count; ++i) {
    const auto rgb = evaluate(
        curves, feature({source[i * Channels], source[i * Channels + 1],
                         source[i * Channels + 2]}));
    for (unsigned c = 0; c < 3; ++c)
      output[i * Channels + c] = static_cast<T>(rgb[c]);
    if constexpr (Channels == 4)
      std::memcpy(output + i * Channels + 3, source + i * Channels + 3,
                  sizeof(T));
  }
}
template <class T>
inline void feature_span(const T* source, T* output, std::size_t count,
                         const fast_curve::Curve* curves) {
  if (small_feature_span(source, output, count, curves))
    return;
  fast_curve::span_multi(curves, 3, source, output, count);
}
}  // namespace ps::plugin_internal::grade::gradient

#include "07-grade/gradient_map_simd.hpp"
