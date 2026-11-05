// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "07-grade/exposure_math.hpp"

namespace ps::plugin_internal::grade::vignette {
struct Geometry {
  double cx = 0, cy = 0, rx = 1, ry = 1;
  double cosine = 1, sine = 0, inner = 0, outer = 1;
  double center = 0, edge = -1;
  double ev(double x, double y) const {
    if (center == edge)
      return center;
    const double dx = x - cx, dy = y - cy;
    const double u = (cosine * dx + sine * dy) / rx;
    const double v = (-sine * dx + cosine * dy) / ry;
    const double a = std::max(std::abs(u), std::abs(v));
    const double r = a > 0x1p-500 && a < 0x1p500 ? std::sqrt(u * u + v * v)
                                                 : std::hypot(u, v);
    if (r <= inner)
      return center;
    if (r >= outer)
      return edge;
    const double t = (r - inner) / (outer - inner);
    const double f = t * t * (3 - 2 * t);
    // A convex sum also handles opposite finite endpoints near DBL_MAX.
    return (1 - f) * center + f * edge;
  }
  void field(const double* coordinates, double* output, std::size_t n) const {
    for (std::size_t j = 0; j < n; ++j)
      output[j] = ev(coordinates[2 * j], coordinates[2 * j + 1]);
  }
  template <class T>
  void apply(const T* source, const double* coordinates, T* output,
             std::size_t n, unsigned channels, const unsigned* selected,
             unsigned count) const {
    if (center == 0 && edge == 0) {
      std::memcpy(output, source, n * channels * sizeof(T));
      return;
    }
    constexpr std::size_t kBlock = 128;
    double values[kBlock];
    unsigned mask = 0;
    for (unsigned c = 0; c < count; ++c) {
      if (selected[c] < 32)
        mask |= 1U << selected[c];
    }
    for (std::size_t first = 0; first < n; first += kBlock) {
      const auto size = std::min(kBlock, n - first);
      field(coordinates + first * 2, values, size);
      const auto* src = source + first * channels;
      auto* dst = output + first * channels;
      if (channels == 1) {
        exposure_span<T, 1, true>(src, values, dst, size, mask);
      } else if (channels == 3) {
        exposure_span<T, 3, true>(src, values, dst, size, mask);
      } else if (channels == 4) {
        exposure_span<T, 4, true>(src, values, dst, size, mask);
      } else {
        std::memcpy(dst, src, size * channels * sizeof(T));
        for (std::size_t j = 0; j < size; ++j)
          if (values[j] != 0) {
            for (unsigned c = 0; c < count; ++c)
              dst[j * channels + selected[c]] =
                  exposed(src[j * channels + selected[c]], values[j]);
          }
      }
    }
  }
};
}  // namespace ps::plugin_internal::grade::vignette
