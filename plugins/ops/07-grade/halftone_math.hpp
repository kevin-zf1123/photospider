// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "07-grade/dither_math.hpp"

namespace ps::plugin_internal::grade::halftone {
inline double dot_rank(double square) {
  if (square == 0)
    return 0;
  if (square >= .5)
    return 1;
  const double pi = std::acos(-1.0);
  if (square <= .25)
    return pi * square;
  return pi * square - 4 * square * std::acos(.5 / std::sqrt(square)) +
         2 * std::sqrt(square - .25);
}
inline unsigned covered_samples(const double* ranks, unsigned count,
                                double coverage) {
#if defined(__aarch64__)
  if (count == 16) {
    // Screen ranks and non-endpoint coverage are nonnegative. Integer bit
    // order gives the same strict comparison without data-dependent branches.
    const auto limit = vreinterpretq_u64_f64(vdupq_n_f64(coverage));
    auto total = vdupq_n_u64(0);
    for (unsigned j = 0; j < 16; j += 2)
      total = vsubq_u64(
          total, vcltq_u64(vreinterpretq_u64_f64(vld1q_f64(ranks + j)), limit));
    return static_cast<unsigned>(vaddvq_u64(total));
  }
  std::uint64_t limit;
  std::memcpy(&limit, &coverage, sizeof(limit));
  unsigned first = 0, remaining = count;
  while (remaining) {
    const auto half = remaining / 2, middle = first + half;
    std::uint64_t rank;
    std::memcpy(&rank, ranks + middle, sizeof(rank));
    const bool below = rank < limit;
    first = below ? middle + 1 : first;
    remaining = below ? remaining - half - 1 : half;
  }
  return first;
#else
  return static_cast<unsigned>(
      std::lower_bound(ranks, ranks + count, coverage) - ranks);
#endif
}
struct Screen {
  double a = 1, b = 0, c = 0, d = 1, ox = 0, oy = 0;
  unsigned samples = 4, period_x = 0, period_y = 0;
  bool dot = true, clamp = false, exact_inverse = false;
  std::vector<double> ranks;
  void basis(const double* m) {
    double determinant = m[0] * m[3] - m[1] * m[2];
    if (std::isfinite(determinant) && determinant != 0) {
      a = m[3] / determinant;
      b = -m[1] / determinant;
      c = -m[2] / determinant;
      d = m[0] / determinant;
    } else {
      const double scale = std::max(
          {std::abs(m[0]), std::abs(m[1]), std::abs(m[2]), std::abs(m[3])});
      const double p = m[0] / scale, q = m[1] / scale, r = m[2] / scale,
                   s = m[3] / scale;
      determinant = p * s - q * r;
      a = (s / determinant) / scale;
      b = (-q / determinant) / scale;
      c = (-r / determinant) / scale;
      d = (p / determinant) / scale;
    }
    const auto exact_dot = [](double x, double y, double z, double w,
                              double expected) {
      const double p = x * y, q = z * w, sum = p + q;
      if (!std::isfinite(sum) || sum != expected || std::fma(x, y, -p) != 0 ||
          std::fma(z, w, -q) != 0)
        return false;
      const double t = sum - p;
      return (p - (sum - t)) + (q - t) == 0;
    };
    exact_inverse =
        exact_dot(m[0], a, m[1], c, 1) && exact_dot(m[0], b, m[1], d, 0) &&
        exact_dot(m[2], a, m[3], c, 0) && exact_dot(m[2], b, m[3], d, 1);
  }
  double rank_relative(double x, double y, unsigned i, unsigned j) const {
    const double dx = x + (i + .5) / samples;
    const double dy = y + (j + .5) / samples;
    const double uu = a * dx + b * dy;
    const double vv = c * dx + d * dy;
    const double u = (uu - std::floor(uu)) - .5;
    if (!dot)
      return 2 * std::abs(u);
    const double v = (vv - std::floor(vv)) - .5;
    return dot_rank(u * u + v * v);
  }
  double rank(double x, double y, unsigned i, unsigned j) const {
    return rank_relative(x - ox, y - oy, i, j);
  }
  // Keep the larger direct sampler out of the cached row kernel. Factoring
  // the matrix products preserves their native rounding and avoids repeating
  // each x-dependent product for every subpixel row.
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((noinline))
#endif
  unsigned sampled_coverage(double coverage, double x, double y) const {
    std::array<double, 16> ux{}, vx{};
    for (unsigned i = 0; i < samples; ++i) {
      const double dx = x + (i + .5) / samples;
      ux[i] = a * dx;
      if (dot)
        vx[i] = c * dx;
    }
    unsigned count = 0;
    for (unsigned j = 0; j < samples; ++j) {
      const double dy = y + (j + .5) / samples;
      const double uy = b * dy, vy = dot ? d * dy : 0;
      for (unsigned i = 0; i < samples; ++i) {
        const double uu = ux[i] + uy;
        const double u = (uu - std::floor(uu)) - .5;
        double rank = 2 * std::abs(u);
        if (dot) {
          const double vv = vx[i] + vy;
          const double v = (vv - std::floor(vv)) - .5;
          rank = dot_rank(u * u + v * v);
        }
        count += rank < coverage;
      }
    }
    return count;
  }
  static double relative(__int128 coordinate, double origin) {
    const double high = static_cast<double>(coordinate);
    const auto remainder = coordinate - static_cast<__int128>(high);
    if (remainder == 0 || std::abs(origin) >= 0x1p126)
      return high - origin;
    // Subtract the integral part before rounding. Adding the lost coordinate
    // bits to an already rounded difference can otherwise round twice.
    const auto integral = static_cast<__int128>(origin);
    const double fraction = origin - static_cast<double>(integral);
    const auto whole = coordinate - integral;
    const double rounded = static_cast<double>(whole);
    const auto missed = whole - static_cast<__int128>(rounded);
    if (missed == 0)
      return rounded - fraction;
    if (fraction != 0 && ((missed > 0) != (fraction > 0))) {
      const double neighbor = std::nextafter(
          rounded, missed > 0 ? std::numeric_limits<double>::infinity()
                              : -std::numeric_limits<double>::infinity());
      // A fractional origin can change an integer rounding decision only at
      // an exact midpoint. Compare integers to avoid rounding that decision.
      const auto half =
          (static_cast<__int128>(neighbor) - static_cast<__int128>(rounded)) /
          2;
      if (missed == half)
        return neighbor;
    }
    return rounded;
  }
  // Exact integer periods of the computed native inverse permit bounded static
  // phase tables. Input coordinates are reduced before floating conversion.
  // No image payload is included in this immutable prepared geometry.
  void prepare() {
    period_x = period_y = 0;
    ranks.clear();
    if (!exact_inverse)
      return;
    const auto integral_product = [](double x, unsigned p) {
      const double product = x * p;
      return std::isfinite(product) && product == std::floor(product) &&
             std::fma(x, static_cast<double>(p), -product) == 0;
    };
    for (unsigned p = 1; p <= 64; ++p) {
      if (!period_x && integral_product(a, p) &&
          (!dot || integral_product(c, p)))
        period_x = p;
      if (!period_y && integral_product(b, p) &&
          (!dot || integral_product(d, p)))
        period_y = p;
    }
    const auto n = samples * samples;
    if (!period_x || !period_y || period_x * period_y * n > 65536) {
      period_x = period_y = 0;
      return;
    }
    const double ix = std::floor(ox), iy = std::floor(oy);
    const auto phase_x =
        static_cast<std::int64_t>(std::fmod(ix, static_cast<double>(period_x)));
    const auto phase_y =
        static_cast<std::int64_t>(std::fmod(iy, static_cast<double>(period_y)));
    const double fraction_x = ox - ix, fraction_y = oy - iy;
    ranks.resize(period_x * period_y * n);
    for (unsigned y = 0; y < period_y; ++y)
      for (unsigned x = 0; x < period_x; ++x) {
        auto* values = ranks.data() + (y * period_x + x) * n;
        // Canonical phase operands keep an equal image/origin translation
        // from changing strict coverage decisions through reassociation.
        const double xx = static_cast<double>(dither::wrap(
                              static_cast<__int128>(x) - phase_x, period_x)) -
                          fraction_x;
        const double yy = static_cast<double>(dither::wrap(
                              static_cast<__int128>(y) - phase_y, period_y)) -
                          fraction_y;
        for (unsigned j = 0; j < samples; ++j)
          for (unsigned i = 0; i < samples; ++i) {
            const double value = rank_relative(xx, yy, i, j);
            if (!std::isfinite(value)) {
              // A different representative of an exact period can overflow
              // with an extreme inverse basis. Keep the direct native path;
              // nonfinite ranks cannot form an ordered lookup table.
              ranks.clear();
              period_x = period_y = 0;
              return;
            }
            values[j * samples + i] = value;
          }
        std::sort(values, values + n);
      }
  }
  double apply(double coverage, __int128 x, __int128 y) const {
    if (clamp)
      coverage = std::clamp(coverage, 0.0, 1.0);
    if (coverage == 0 || coverage == 1)
      return coverage;
    const auto n = samples * samples;
    unsigned count = 0;
    if (!ranks.empty()) {
      const auto xx = dither::wrap(x, period_x), yy = dither::wrap(y, period_y);
      const auto* first = ranks.data() + (yy * period_x + xx) * n;
      count = covered_samples(first, n, coverage);
    } else {
      const auto xx = relative(x, ox), yy = relative(y, oy);
      if (samples >= 4) {
        count = sampled_coverage(coverage, xx, yy);
      } else {
        for (unsigned j = 0; j < samples; ++j)
          for (unsigned i = 0; i < samples; ++i)
            count += rank_relative(xx, yy, i, j) < coverage;
      }
    }
    return static_cast<double>(count) / n;
  }
  template <class T>
  void row(const T* source, T* output, std::size_t n, __int128 x, __int128 y,
           std::size_t stride = 1) const {
    if (!ranks.empty()) {
      auto xx = dither::wrap(x, period_x);
      const auto yy = dither::wrap(y, period_y);
      const auto count = samples * samples;
      const auto* row = ranks.data() + yy * period_x * count;
      for (std::size_t j = 0; j < n; ++j) {
        double coverage = static_cast<double>(source[j * stride]);
        if (clamp)
          coverage = std::clamp(coverage, 0.0, 1.0);
        const auto* first = row + xx * count;
        output[j * stride] = static_cast<T>(
            coverage == 0 || coverage == 1
                ? coverage
                : static_cast<double>(covered_samples(first, count, coverage)) /
                      count);
        if (++xx == period_x)
          xx = 0;
      }
      return;
    }
    for (std::size_t j = 0; j < n; ++j)
      output[j * stride] = static_cast<T>(
          apply(static_cast<double>(source[j * stride]), x + j, y));
  }
};
template <class T>
void colorize(const T* ink, T* out, std::size_t pixels, unsigned layers,
              const double* colors, const double* background) {
  for (std::size_t j = 0; j < pixels; ++j) {
    std::array<double, 3> value{background[0], background[1], background[2]};
    for (unsigned k = 0; k < layers; ++k) {
      const double a = static_cast<double>(ink[j * layers + k]);
      if (a == 0)
        continue;
      if (a == 1) {
        value = {colors[3 * k], colors[3 * k + 1], colors[3 * k + 2]};
        continue;
      }
      const double inverse = 1 - a;
      for (unsigned c = 0; c < 3; ++c)
        value[c] = inverse * value[c] + a * colors[3 * k + c];
    }
    for (unsigned c = 0; c < 3; ++c)
      out[j * 3 + c] = static_cast<T>(value[c]);
  }
}
}  // namespace ps::plugin_internal::grade::halftone
