// Copyright 2026 Photospider contributors
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "01-numeric/fast_native_simd.hpp"
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace ps::plugin_internal::grade::threshold {
template <class T>
inline void fixed_span(const T* source, std::uint8_t* target, std::size_t count,
                       double threshold) {
  std::size_t i = 0;
#if defined(__aarch64__)
  if constexpr (sizeof(T) == 4) {
    // The least Float32 >= threshold gives exactly the promoted comparison
    // for finite Float32 samples, including a threshold between adjacent
    // values.
    float cutoff = static_cast<float>(threshold);
    if (static_cast<double>(cutoff) < threshold)
      cutoff = std::nextafter(cutoff, std::numeric_limits<float>::infinity());
    const auto level = vdupq_n_f32(cutoff);
    for (; i + 16 <= count; i += 16) {
      const auto a =
          vmovn_u32(native_simd::greater_equal(vld1q_f32(source + i), level));
      const auto b = vmovn_u32(
          native_simd::greater_equal(vld1q_f32(source + i + 4), level));
      const auto c = vmovn_u32(
          native_simd::greater_equal(vld1q_f32(source + i + 8), level));
      const auto d = vmovn_u32(
          native_simd::greater_equal(vld1q_f32(source + i + 12), level));
      const auto lo = vmovn_u16(vcombine_u16(a, b));
      const auto hi = vmovn_u16(vcombine_u16(c, d));
      vst1q_u8(target + i, vandq_u8(vcombine_u8(lo, hi), vdupq_n_u8(1)));
    }
  } else {
    const auto level = vdupq_n_f64(threshold);
    for (; i + 8 <= count; i += 8) {
      const auto a =
          vmovn_u64(native_simd::greater_equal(vld1q_f64(source + i), level));
      const auto b = vmovn_u64(
          native_simd::greater_equal(vld1q_f64(source + i + 2), level));
      const auto c = vmovn_u64(
          native_simd::greater_equal(vld1q_f64(source + i + 4), level));
      const auto d = vmovn_u64(
          native_simd::greater_equal(vld1q_f64(source + i + 6), level));
      const auto lo = vmovn_u32(vcombine_u32(a, b));
      const auto hi = vmovn_u32(vcombine_u32(c, d));
      vst1_u8(target + i,
              vand_u8(vmovn_u16(vcombine_u16(lo, hi)), vdup_n_u8(1)));
    }
  }
#endif
  for (; i < count; ++i)
    target[i] = static_cast<double>(source[i]) >= threshold;
}
struct NoPoll final {
  void operator()(std::uint64_t) const {}
};
struct Filter final {
  unsigned kind = 0;
  std::size_t height = 0, width = 0, radius = 0;
  double sigma = 2, k = .2, dynamic_range = .5;
  std::vector<double> weights;
  void prepare() {
    if (kind != 2)
      return;
    weights.resize(radius + 1);
    weights[0] = 1;
    for (std::size_t i = 1; i <= radius; ++i)
      weights[i] = std::exp(-.5 * (static_cast<double>(i) / sigma) *
                            (static_cast<double>(i) / sigma));
  }
  std::size_t prefix_count() const { return (height + 1) * (width + 1); }
  std::size_t workspace_count() const {
    return kind == 2 ? height * width
                     : prefix_count() * (kind == 3 && k != 0 ? 2 : 1);
  }
  std::size_t scratch_count() const {
    if (!radius)
      return 0;
    return height * width + workspace_count() + 1;
  }
  template <class Poll>
  void build(const double* __restrict source, double* __restrict scratch,
             Poll poll) const {
    double magnitude = 0;
    if (kind == 2) {
      double full_denominator = 1;
      for (std::size_t j = 1; j <= radius; ++j)
        full_denominator += 2 * weights[j];
      for (std::size_t y = 0; y < height; ++y) {
        poll(width * (2 * radius + 1));
        std::size_t x = 0;
        const auto scalar = [&](std::size_t at) {
          const auto lo = at > radius ? at - radius : 0;
          const auto hi = std::min(width - 1, at + radius);
          const double anchor = source[y * width + at];
          magnitude = std::max(magnitude, std::abs(anchor));
          double sum = 0, denominator = 0;
          for (auto j = lo; j <= hi; ++j) {
            const auto w = weights[j > at ? j - at : at - j];
            sum += w * (source[y * width + j] - anchor);
            denominator += w;
          }
          scratch[y * width + at] = anchor + sum / denominator;
        };
        for (; x < std::min(radius, width); ++x)
          scalar(x);
#if defined(__aarch64__)
        auto maximum = vdupq_n_f64(0);
        if (radius == 3) {
          for (; x + 8 <= width && 3 <= width - x - 8; x += 8) {
            std::array<float64x2_t, 7> window;
            for (unsigned j = 0; j < 7; ++j)
              window[j] = vld1q_f64(source + y * width + x - 3 + 2 * j);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
            for (unsigned packet = 0; packet < 4; ++packet) {
              const auto a = window[packet], b = window[packet + 1],
                         c = window[packet + 2], d = window[packet + 3];
              const auto center = vextq_f64(b, c, 1);
              maximum = vmaxq_f64(maximum, vabsq_f64(center));
              auto sum = vfmaq_n_f64(center, vaddq_f64(b, c), weights[1]);
              sum = vfmaq_n_f64(
                  sum, vaddq_f64(vextq_f64(a, b, 1), vextq_f64(c, d, 1)),
                  weights[2]);
              sum = vfmaq_n_f64(sum, vaddq_f64(a, d), weights[3]);
              vst1q_f64(scratch + y * width + x + 2 * packet,
                        vmulq_n_f64(sum, 1 / full_denominator));
            }
          }
        }
        for (; x + 2 <= width && radius <= width - x - 2; x += 2) {
          const auto center = vld1q_f64(source + y * width + x);
          maximum = vmaxq_f64(maximum, vabsq_f64(center));
          auto sum = center;
          for (std::size_t j = 1; j <= radius; ++j) {
            const auto pair = vaddq_f64(vld1q_f64(source + y * width + x - j),
                                        vld1q_f64(source + y * width + x + j));
            sum = vfmaq_n_f64(sum, pair, weights[j]);
          }
          const auto mean = vmulq_n_f64(sum, 1 / full_denominator);
          vst1q_f64(scratch + y * width + x, mean);
        }
        magnitude = std::max({magnitude, vgetq_lane_f64(maximum, 0),
                              vgetq_lane_f64(maximum, 1)});
#endif
        for (; x < width; ++x)
          scalar(x);
      }
      scratch[workspace_count()] = magnitude;
      return;
    }
    const auto stride = width + 1, count = prefix_count();
    auto* square = scratch + count;
    const bool variance = kind == 3 && k != 0;
    std::fill_n(scratch, stride, 0);
    if (variance)
      std::fill_n(square, stride, 0);
    const double anchor = source[0];
    for (std::size_t y = 0; y < height; ++y) {
      poll(width * (variance ? 8 : 4));
      double sum = 0, sum2 = 0;
      scratch[(y + 1) * stride] = 0;
      if (variance)
        square[(y + 1) * stride] = 0;
      std::size_t x = 0;
#if defined(__aarch64__)
      auto maximum = vdupq_n_f64(0);
#if defined(__clang__)
#pragma clang loop unroll_count(4)
#endif
      for (; x + 2 <= width; x += 2) {
        const auto samples = vld1q_f64(source + y * width + x);
        maximum = vmaxq_f64(maximum, vabsq_f64(samples));
        const auto v = vsubq_f64(samples, vdupq_n_f64(anchor));
        auto partial = vaddq_f64(v, vextq_f64(vdupq_n_f64(0), v, 1));
        partial = vaddq_f64(partial, vdupq_n_f64(sum));
        sum = vgetq_lane_f64(partial, 1);
        const auto at = (y + 1) * stride + x + 1;
        vst1q_f64(scratch + at,
                  vaddq_f64(vld1q_f64(scratch + at - stride), partial));
        if (variance) {
          const auto vv = vmulq_f64(v, v);
          auto partial2 = vaddq_f64(vv, vextq_f64(vdupq_n_f64(0), vv, 1));
          partial2 = vaddq_f64(partial2, vdupq_n_f64(sum2));
          sum2 = vgetq_lane_f64(partial2, 1);
          vst1q_f64(square + at,
                    vaddq_f64(vld1q_f64(square + at - stride), partial2));
        }
      }
      magnitude = std::max(
          {magnitude, vgetq_lane_f64(maximum, 0), vgetq_lane_f64(maximum, 1)});
#endif
      for (; x < width; ++x) {
        const double sample = source[y * width + x];
        magnitude = std::max(magnitude, std::abs(sample));
        const double v = sample - anchor;
        sum += v;
        const auto at = (y + 1) * stride + x + 1;
        scratch[at] = scratch[at - stride] + sum;
        if (variance) {
          sum2 += v * v;
          square[at] = square[at - stride] + sum2;
        }
      }
    }
    scratch[workspace_count()] = magnitude;
  }
  // Near a discrete boundary, use a tile-independent centered local sum.
  // It has no arbitrary mask epsilon and gives exact ties for constants.
  template <class Poll>
  bool refine(const double* source, std::size_t y, std::size_t x, double offset,
              Poll poll) const {
    const auto y0 = y > radius ? y - radius : 0;
    const auto x0 = x > radius ? x - radius : 0;
    const auto y1 = std::min(height - 1, y + radius);
    const auto x1 = std::min(width - 1, x + radius);
    const double center = source[y * width + x];
    double sum = 0, denominator = 0;
    for (auto j = y0; j <= y1; ++j) {
      poll((x1 - x0 + 1) * (kind == 2 ? 8 : 3));
      for (auto i = x0; i <= x1; ++i) {
        const double w =
            kind == 2 ? static_cast<double>(weights[j > y ? j - y : y - j]) *
                            weights[i > x ? i - x : x - i]
                      : 1;
        if (w == 0)
          continue;
        sum += w * (static_cast<double>(source[j * width + i]) - center);
        denominator += w;
      }
    }
    const double mean = center + sum / denominator;
    if (kind != 3 || k == 0)
      return center >= mean - offset;
    double squared = 0;
    for (auto j = y0; j <= y1; ++j) {
      poll((x1 - x0 + 1) * 3);
      for (auto i = x0; i <= x1; ++i) {
        const double d = source[j * width + i] - mean;
        squared += d * d;
      }
    }
    const double threshold =
        mean * (1 + static_cast<double>(k) *
                        (std::sqrt(squared / denominator) / dynamic_range - 1));
    return center >= threshold;
  }
  template <class Poll = NoPoll>
  void row(const double*, const double*, std::size_t, std::size_t, std::size_t,
           double, std::uint8_t*, Poll = {}) const;
  template <class Poll = NoPoll>
  bool value(const double* source, const double* scratch, std::size_t y,
             std::size_t x, double offset, Poll poll = {}) const {
    const double center = source[y * width + x];
    if (!radius) {
      if (kind == 3)
        return center >= center * (1 - k);
      return center >= center - offset;
    }
    const auto y0 = y > radius ? y - radius : 0;
    const auto x0 = x > radius ? x - radius : 0;
    const auto y1 = std::min(height, y + radius + 1);
    const auto x1 = std::min(width, x + radius + 1);
    double mean = 0, variance = 0;
    if (kind == 2) {
      const double anchor = scratch[y * width + x];
      double sum = 0, denominator = 0;
      for (auto j = y0; j < y1; ++j) {
        const double w = weights[j > y ? j - y : y - j];
        sum += w * (scratch[j * width + x] - anchor);
        denominator += w;
      }
      mean = anchor + sum / denominator;
    } else {
      const auto stride = width + 1;
      const auto rectangle = [&](const double* p) {
        return (p[y1 * stride + x1] - p[y0 * stride + x1]) -
               (p[y1 * stride + x0] - p[y0 * stride + x0]);
      };
      const double n =
          static_cast<double>(y1 - y0) * static_cast<double>(x1 - x0);
      const double delta = rectangle(scratch) / n;
      mean = source[0] + delta;
      if (kind == 3 && k != 0)
        variance = std::max(
            0.0, rectangle(scratch + prefix_count()) / n - delta * delta);
    }
    const double target =
        kind == 3 && k != 0
            ? mean * (1 + k * (std::sqrt(variance) / dynamic_range - 1))
            : mean - offset;
    // Prefix cancellation and the variance square root amplify arithmetic
    // error near ties. This is a refinement trigger, never a label tolerance.
    double bound = 64 * std::numeric_limits<double>::epsilon() *
                   static_cast<double>(height) * static_cast<double>(width) *
                   std::max(1.0, scratch[workspace_count()]);
    if (kind != 2)
      bound *= std::max(1.0, static_cast<double>(height + width) /
                                 (static_cast<double>(y1 - y0) *
                                  static_cast<double>(x1 - x0)));
    if (kind == 3 && k != 0) {
      // On [0,1], the exact mean is at most one. Propagate both moment
      // errors through variance and sqrt, including the quadratic term.
      const double variance_bound = bound * (4 + 3 * bound);
      bound = bound * (1 + std::abs(k)) +
              std::abs(k / dynamic_range) *
                  (std::sqrt(variance_bound) + bound * std::sqrt(variance));
    }
    if (!std::isfinite(target) || !std::isfinite(bound) ||
        std::abs(center - target) <= bound)
      return refine(source, y, x, offset, poll);
    return center >= target;
  }
};
}  // namespace ps::plugin_internal::grade::threshold

#include "07-grade/threshold_packets.hpp"
