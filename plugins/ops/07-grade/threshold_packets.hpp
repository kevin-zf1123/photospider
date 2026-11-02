// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <limits>

namespace ps::plugin_internal::grade::threshold {
template <unsigned Kind, class Poll>
void local_row(const Filter& filter, const double* __restrict source,
               const double* __restrict scratch, std::size_t y,
               std::size_t first, std::size_t count, double offset,
               std::uint8_t* __restrict target, Poll poll) {
  const auto height = filter.height, width = filter.width,
             radius = filter.radius;
  const auto* weights = filter.weights.data();
  const double k = filter.k, dynamic_range = filter.dynamic_range;
  const auto last = first + count;
  std::size_t x = first;
#if defined(__aarch64__)
  if (radius) {
    const auto begin = Kind == 2 ? first : std::max(first, radius);
    const auto end =
        Kind == 2 ? last : std::min(last, width > radius ? width - radius : 0);
    for (; x < std::min(begin, last); ++x)
      target[x - first] = filter.value(source, scratch, y, x, offset, poll);
    const auto y0 = y > radius ? y - radius : 0;
    const auto y1 = std::min(height, y + radius + 1);
    const double n =
        static_cast<double>(y1 - y0) * static_cast<double>(2 * radius + 1);
    const double bound =
        64 * std::numeric_limits<double>::epsilon() *
        static_cast<double>(height) * static_cast<double>(width) *
        std::max(1.0, scratch[filter.workspace_count()]) *
        (Kind == 2 ? 1
                   : std::max(1.0, static_cast<double>(height + width) / n));
    double denominator = 0;
    if (Kind == 2) {
      for (auto j = y0; j < y1; ++j)
        denominator += weights[j > y ? j - y : y - j];
    }
    const double magnitude = scratch[filter.workspace_count()];
    const double span = static_cast<double>(2 * radius + 1);
    const bool finite_bulk =
        std::isfinite((8 * magnitude * static_cast<double>(height) *
                           static_cast<double>(width) +
                       std::abs(offset) * span * span) *
                      (Kind == 3 ? 1 + std::abs(k / dynamic_range) : 1));
    const auto decide = [&](std::size_t at, float64x2_t left, float64x2_t right,
                            float64x2_t guard) __attribute__((always_inline)) {
      auto result = native_simd::greater_equal(left, right);
      auto near =
          native_simd::less_equal(vabsq_f64(vsubq_f64(left, right)), guard);
      if (!finite_bulk) {
        const auto limit = vdupq_n_f64(std::numeric_limits<double>::max());
        const auto finite = vandq_u64(
            vandq_u64(native_simd::less_equal(vabsq_f64(left), limit),
                      native_simd::less_equal(vabsq_f64(right), limit)),
            native_simd::less_equal(vabsq_f64(guard), limit));
        near = vorrq_u64(near, veorq_u64(finite, vdupq_n_u64(UINT64_MAX)));
      }
      return std::array<uint64x2_t, 2>{result, near};
    };  // NOLINT(readability/braces): attributed lambda, not a function.
    const auto evaluate = [&](std::size_t at) __attribute__((always_inline)) {
      if constexpr (Kind != 3) {
        float64x2_t anchor, sum;
        double scale;
        if constexpr (Kind == 2) {
          anchor = vdupq_n_f64(0);
          if (radius == 3 && y >= 3 && y + 3 < height) {
            sum = vld1q_f64(scratch + y * width + at);
            for (unsigned j = 1; j <= 3; ++j)
              sum = vfmaq_n_f64(
                  sum,
                  vaddq_f64(vld1q_f64(scratch + (y - j) * width + at),
                            vld1q_f64(scratch + (y + j) * width + at)),
                  weights[j]);
          } else {
            sum = vdupq_n_f64(0);
            for (auto j = y0; j < y1; ++j)
              sum = vfmaq_n_f64(sum, vld1q_f64(scratch + j * width + at),
                                weights[j > y ? j - y : y - j]);
          }
          scale = denominator;
        } else {
          const auto stride = width + 1;
          sum = vsubq_f64(
              vsubq_f64(vld1q_f64(scratch + y1 * stride + at + radius + 1),
                        vld1q_f64(scratch + y0 * stride + at + radius + 1)),
              vsubq_f64(vld1q_f64(scratch + y1 * stride + at - radius),
                        vld1q_f64(scratch + y0 * stride + at - radius)));
          anchor = vdupq_n_f64(source[0]);
          scale = n;
        }
        const auto center = vld1q_f64(source + y * width + at);
        float64x2_t left;
        if constexpr (Kind == 2)
          left = vfmaq_n_f64(vdupq_n_f64(offset * scale), center, scale);
        else
          left = vmulq_n_f64(
              vaddq_f64(vsubq_f64(center, anchor), vdupq_n_f64(offset)), scale);
        return decide(at, left, sum, vdupq_n_f64(bound * scale));
      }
      auto mean = vdupq_n_f64(0), variance = vdupq_n_f64(0);
      if (Kind == 2) {
        const auto anchor = vld1q_f64(scratch + y * width + at);
        auto sum = vdupq_n_f64(0);
        for (auto j = y0; j < y1; ++j)
          sum = vfmaq_n_f64(
              sum, vsubq_f64(vld1q_f64(scratch + j * width + at), anchor),
              weights[j > y ? j - y : y - j]);
        mean = vaddq_f64(anchor, vdivq_f64(sum, vdupq_n_f64(denominator)));
      } else {
        const auto stride = width + 1;
        const auto rectangle = [&](const double* p) {
          return vsubq_f64(
              vsubq_f64(vld1q_f64(p + y1 * stride + at + radius + 1),
                        vld1q_f64(p + y0 * stride + at + radius + 1)),
              vsubq_f64(vld1q_f64(p + y1 * stride + at - radius),
                        vld1q_f64(p + y0 * stride + at - radius)));
        };
        const auto delta = vmulq_n_f64(rectangle(scratch), 1 / n);
        mean = vaddq_f64(vdupq_n_f64(source[0]), delta);
        if (Kind == 3)
          variance = vmaxq_f64(
              vdupq_n_f64(0),
              vsubq_f64(vmulq_n_f64(rectangle(scratch + filter.prefix_count()),
                                    1 / n),
                        vmulq_f64(delta, delta)));
      }
      auto threshold = vsubq_f64(mean, vdupq_n_f64(offset));
      auto guard = vdupq_n_f64(bound);
      if (Kind == 3) {
        const auto deviation = vsqrtq_f64(variance);
        const double ratio = k / dynamic_range;
        threshold =
            vmulq_f64(mean, vfmaq_n_f64(vdupq_n_f64(1 - k), deviation, ratio));
        const double variance_bound = bound * (4 + 3 * bound);
        guard = vfmaq_n_f64(
            vdupq_n_f64(bound * (1 + std::abs(k)) +
                        std::abs(ratio) * std::sqrt(variance_bound)),
            deviation, bound * std::abs(ratio));
      }
      const auto center = vld1q_f64(source + y * width + at);
      return decide(at, center, threshold, guard);
    };  // NOLINT(readability/braces): attributed lambda, not a function.
    for (; x + 8 <= end; x += 8) {
      const auto a = evaluate(x), b = evaluate(x + 2), c = evaluate(x + 4),
                 d = evaluate(x + 6);
      const auto lo = vmovn_u32(vcombine_u32(vmovn_u64(a[0]), vmovn_u64(b[0])));
      const auto hi = vmovn_u32(vcombine_u32(vmovn_u64(c[0]), vmovn_u64(d[0])));
      vst1_u8(target + x - first,
              vand_u8(vmovn_u16(vcombine_u16(lo, hi)), vdup_n_u8(1)));
      const auto near = vorrq_u64(vorrq_u64(a[1], b[1]), vorrq_u64(c[1], d[1]));
      if (vgetq_lane_u64(near, 0) | vgetq_lane_u64(near, 1)) {
        const std::array<std::array<uint64x2_t, 2>, 4> packets{a, b, c, d};
        for (unsigned j = 0; j < 4; ++j) {
          if (vgetq_lane_u64(packets[j][1], 0))
            target[x - first + 2 * j] =
                filter.refine(source, y, x + 2 * j, offset, poll);
          if (vgetq_lane_u64(packets[j][1], 1))
            target[x - first + 2 * j + 1] =
                filter.refine(source, y, x + 2 * j + 1, offset, poll);
        }
      }
    }
    for (; x + 2 <= end; x += 2) {
      const auto v = evaluate(x);
      target[x - first] = vgetq_lane_u64(v[1], 0)
                              ? filter.refine(source, y, x, offset, poll)
                              : (vgetq_lane_u64(v[0], 0) & 1);
      target[x - first + 1] =
          vgetq_lane_u64(v[1], 1)
              ? filter.refine(source, y, x + 1, offset, poll)
              : (vgetq_lane_u64(v[0], 1) & 1);
    }
  }
#endif
  for (; x < last; ++x)
    target[x - first] = filter.value(source, scratch, y, x, offset, poll);
}
template <class Poll>
void Filter::row(const double* source, const double* scratch, std::size_t y,
                 std::size_t first, std::size_t count, double offset,
                 std::uint8_t* target, Poll poll) const {
  if (kind == 2)
    local_row<2>(*this, source, scratch, y, first, count, offset, target, poll);
  else if (kind == 3 && k != 0)
    local_row<3>(*this, source, scratch, y, first, count, offset, target, poll);
  else
    local_row<1>(*this, source, scratch, y, first, count, offset, target, poll);
}
}  // namespace ps::plugin_internal::grade::threshold
