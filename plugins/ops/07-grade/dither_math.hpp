// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "02-format-color/fast_color.hpp"
#include "07-grade/canonical_root.hpp"

namespace ps::plugin_internal::grade::dither {
using Triple = std::array<double, 3>;
using Matrix = fast_color::Matrix;
inline std::uint64_t wrap(__int128 value, std::uint64_t period) {
  if ((period & (period - 1)) == 0)
    return static_cast<std::uint64_t>(value) & (period - 1);
  if (value >= 0 && static_cast<unsigned __int128>(value) <= UINT64_MAX)
    return static_cast<std::uint64_t>(value) % period;
  if (value >= INT64_MIN && value < 0 && period <= INT64_MAX) {
    const auto r =
        static_cast<std::int64_t>(value) % static_cast<std::int64_t>(period);
    return r < 0 ? static_cast<std::uint64_t>(r + period)
                 : static_cast<std::uint64_t>(r);
  }
  auto r = value % static_cast<__int128>(period);
  return static_cast<std::uint64_t>(r < 0 ? r + period : r);
}
inline unsigned bayer_rank(unsigned x, unsigned y, unsigned order) {
  unsigned rank = 0;
  for (unsigned k = 0; k < order; ++k) {
    const unsigned xb = (x >> k) & 1U, yb = (y >> k) & 1U;
    const unsigned quadrant = yb ? (xb ? 1U : 3U) : (xb ? 2U : 0U);
    rank = rank * 4 + quadrant;
  }
  return rank;
}
inline double quantize(double x, double threshold, unsigned levels,
                       bool clamp) {
  const double u = clamp ? std::clamp(x, 0.0, 1.0) : x;
  if (u == 0 || u == 1)
    return u;
  const double z = u * static_cast<double>(levels - 1);
  const double low = std::floor(z);
  const double q = std::min(static_cast<double>(levels - 1),
                            low + static_cast<double>(threshold < z - low));
  return q / static_cast<double>(levels - 1);
}
inline float quantize_float(float x, float threshold, unsigned levels,
                            bool clamp) {
  const float u = clamp ? std::clamp(x, 0.0F, 1.0F) : x;
  if (u == 0 || u == 1)
    return u;
  const float length = static_cast<float>(levels - 1);
  const float z = u * length;
  const float low = std::floor(z);
  const float q =
      std::min(length, low + static_cast<float>(threshold < z - low));
  return q / length;
}
// Packed selected components retain the calculation dtype at every stage.
// Threshold lookup remains caller supplied for noise, phase maps and tiles.
template <class T, class Threshold>
void uniform_span(const T* source, T* out, std::size_t n, unsigned levels,
                  bool clamp, bool narrow, Threshold threshold) {
  std::size_t j = 0;
#if defined(__aarch64__)
  if constexpr (sizeof(T) == 4) {
    if (narrow) {
      const auto zero = vdupq_n_f32(0.0F), one = vdupq_n_f32(1.0F);
      const auto magnitude = [](float32x4_t value) {
        return vandq_u32(vreinterpretq_u32_f32(value),
                         vdupq_n_u32(UINT32_C(0x7fffffff)));
      };
      const auto length = vdupq_n_f32(static_cast<float>(levels - 1));
      for (; j + 4 <= n; j += 4) {
        const auto x = vld1q_f32(source + j);
        const auto u = clamp ? vminq_f32(vmaxq_f32(x, zero), one) : x;
        const auto z = vmulq_f32(u, length);
        const auto low = vrndmq_f32(z);
        const float t[4]{static_cast<float>(threshold(j)),
                         static_cast<float>(threshold(j + 1)),
                         static_cast<float>(threshold(j + 2)),
                         static_cast<float>(threshold(j + 3))};
        // Valid thresholds and fractions are nonnegative. Unsigned bit order
        // is exact on this domain; masking the sign also normalizes -0.
        const auto decision =
            vcltq_u32(magnitude(vld1q_f32(t)), magnitude(vsubq_f32(z, low)));
        // u is in [0,1]. If low == length, the fraction is zero and the
        // nonnegative threshold cannot select an increment; otherwise the
        // integer low is at most length-1. No final upper clamp is needed.
        const auto q = vaddq_f32(low, vbslq_f32(decision, one, zero));
        const auto endpoint = vorrq_u32(
            vceqq_u32(magnitude(x), vdupq_n_u32(0)),
            vceqq_u32(vreinterpretq_u32_f32(x), vreinterpretq_u32_f32(one)));
        vst1q_f32(out + j, vbslq_f32(endpoint, x, vdivq_f32(q, length)));
      }
    }
  }
  if (!narrow) {
    const auto zero = vdupq_n_f64(0.0), one = vdupq_n_f64(1.0);
    const auto magnitude = [](float64x2_t value) {
      return vandq_u64(vreinterpretq_u64_f64(value),
                       vdupq_n_u64(UINT64_C(0x7fffffffffffffff)));
    };
    const auto length = vdupq_n_f64(static_cast<double>(levels - 1));
    for (; j + 2 <= n; j += 2) {
      float64x2_t x;
      if constexpr (sizeof(T) == 4)
        x = vcvt_f64_f32(vld1_f32(source + j));
      else
        x = vld1q_f64(source + j);
      const auto u = clamp ? vminq_f64(vmaxq_f64(x, zero), one) : x;
      const auto z = vmulq_f64(u, length);
      const auto low = vrndmq_f64(z);
      const double t[2]{threshold(j), threshold(j + 1)};
      const auto decision =
          vcltq_u64(magnitude(vld1q_f64(t)), magnitude(vsubq_f64(z, low)));
      const auto q = vaddq_f64(low, vbslq_f64(decision, one, zero));
      const auto endpoint = vorrq_u64(
          vceqq_u64(magnitude(x), vdupq_n_u64(0)),
          vceqq_u64(vreinterpretq_u64_f64(x), vreinterpretq_u64_f64(one)));
      const auto result = vbslq_f64(endpoint, x, vdivq_f64(q, length));
      if constexpr (sizeof(T) == 4)
        vst1_f32(out + j, vcvt_f32_f64(result));
      else
        vst1q_f64(out + j, result);
    }
  }
#endif
  for (; j < n; ++j)
    out[j] = static_cast<T>(
        narrow ? static_cast<double>(quantize_float(
                     static_cast<float>(source[j]),
                     static_cast<float>(threshold(j)), levels, clamp))
               : quantize(source[j], threshold(j), levels, clamp));
}
// The caller proves a repeating threshold sequence with 1..256 entries.
// Construct only values consumed by this span; no dynamic state is retained.
template <class T, class Threshold>
void periodic_uniform_span(const T* source, T* out, std::size_t n,
                           unsigned levels, bool clamp, std::size_t cycle,
                           Threshold threshold) {
  std::array<double, 256> values;
  const auto length = std::min(cycle, n);
  for (std::size_t j = 0; j < length; ++j)
    values[j] = threshold(j);
  uniform_span(source, out, n, levels, clamp, false,
               [&](std::size_t j) { return values[j % length]; });
}
inline double ties_even(double x) {
  const double f = std::floor(x);
  const double d = x - f;
  return f + (d > .5 || (d == .5 && std::fmod(f, 2.0) != 0));
}
struct Segment {
  Triple start, delta;
  double denominator;
  unsigned first, second;
};
struct Palette {
  const Triple* lab = nullptr;
  unsigned size = 0;
  Segment* segments = nullptr;
  unsigned segment_count = 0;
  void prepare() {
    segment_count = 0;
    for (unsigned i = 0; i < size; ++i)
      for (unsigned j = i + 1; j < size; ++j) {
        if (lab[i] == lab[j])
          continue;
        const Triple d{lab[j][0] - lab[i][0], lab[j][1] - lab[i][1],
                       lab[j][2] - lab[i][2]};
        segments[segment_count++] = {
            lab[i], d, (d[0] * d[0] + d[1] * d[1]) + d[2] * d[2], i, j};
      }
  }
  unsigned segment(const Triple& x, double threshold) const {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    double best = std::numeric_limits<double>::infinity(), fraction = 0;
    unsigned a = 0, b = 0;
    unsigned j = 0;
    for (; j < segment_count; ++j) {
      const auto& s = segments[j];
      const Triple d{x[0] - s.start[0], x[1] - s.start[1], x[2] - s.start[2]};
      const double f = std::clamp(
          ((d[0] * s.delta[0] + d[1] * s.delta[1]) + d[2] * s.delta[2]) /
              s.denominator,
          0.0, 1.0);
      const Triple residual{x[0] - (s.start[0] + f * s.delta[0]),
                            x[1] - (s.start[1] + f * s.delta[1]),
                            x[2] - (s.start[2] + f * s.delta[2])};
      const double distance =
          (residual[0] * residual[0] + residual[1] * residual[1]) +
          residual[2] * residual[2];
      if (distance < best) {
        best = distance;
        fraction = f;
        a = s.first;
        b = s.second;
      }
    }
    return threshold < fraction ? b : a;
  }
  std::array<unsigned, 2> segment_pair(const Triple& first,
                                       const Triple& second,
                                       double first_threshold,
                                       double second_threshold) const {
#if defined(__aarch64__)
    float64x2_t x[3];
    for (unsigned c = 0; c < 3; ++c) {
      const double values[2]{first[c], second[c]};
      x[c] = vld1q_f64(values);
    }
    auto best = vdupq_n_f64(std::numeric_limits<double>::infinity());
    auto fraction = vdupq_n_f64(0);
    auto a = vdupq_n_u64(0), b = vdupq_n_u64(0);
    for (unsigned j = 0; j < segment_count; ++j) {
      const auto& s = segments[j];
      float64x2_t d[3], r[3];
      for (unsigned c = 0; c < 3; ++c)
        d[c] = vsubq_f64(x[c], vdupq_n_f64(s.start[c]));
      const auto numerator = vaddq_f64(vaddq_f64(vmulq_n_f64(d[0], s.delta[0]),
                                                 vmulq_n_f64(d[1], s.delta[1])),
                                       vmulq_n_f64(d[2], s.delta[2]));
      const auto f = vminq_f64(
          vdupq_n_f64(1),
          vmaxq_f64(vdupq_n_f64(0),
                    vdivq_f64(numerator, vdupq_n_f64(s.denominator))));
      for (unsigned c = 0; c < 3; ++c)
        r[c] = vsubq_f64(x[c], vaddq_f64(vdupq_n_f64(s.start[c]),
                                         vmulq_n_f64(f, s.delta[c])));
      const auto distance =
          vaddq_f64(vaddq_f64(vmulq_f64(r[0], r[0]), vmulq_f64(r[1], r[1])),
                    vmulq_f64(r[2], r[2]));
      const auto update = vcltq_f64(distance, best);
      best = vbslq_f64(update, distance, best);
      fraction = vbslq_f64(update, f, fraction);
      a = vbslq_u64(update, vdupq_n_u64(s.first), a);
      b = vbslq_u64(update, vdupq_n_u64(s.second), b);
    }
    const double t[2]{first_threshold, second_threshold};
    const auto chosen = vbslq_u64(vcltq_f64(vld1q_f64(t), fraction), b, a);
    return {static_cast<unsigned>(vgetq_lane_u64(chosen, 0)),
            static_cast<unsigned>(vgetq_lane_u64(chosen, 1))};
#else
    return {segment(first, first_threshold), segment(second, second_threshold)};
#endif
  }
  unsigned nearest(const Triple& x) const {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    double best = std::numeric_limits<double>::infinity();
    unsigned selected = 0;
    for (unsigned j = 0; j < size; ++j) {
      const double a = x[0] - lab[j][0], b = x[1] - lab[j][1],
                   c = x[2] - lab[j][2];
      const double d = (a * a + b * b) + c * c;
      if (d < best) {
        best = d;
        selected = j;
      }
    }
    return selected;
  }
};
inline Triple oklab(const Triple& rgb, const Matrix& to_lms) {
  const auto roots =
      statistics::canonical_roots(fast_color::multiply(to_lms, rgb));
  return fast_color::multiply(fast_color::kLmsToOklab, roots);
}
template <class Read, class Threshold, class Emit>
void palette_points(std::size_t n, const Palette& palette, const Matrix& to_lms,
                    Read read, Threshold threshold, Emit emit) {
  if (palette.size == 1) {
    for (std::size_t j = 0; j < n; ++j)
      emit(j, 0);
    return;
  }
  std::size_t j = 0;
  for (; j + 2 <= n; j += 2) {
    const auto a = oklab(read(j), to_lms), b = oklab(read(j + 1), to_lms);
    const auto pair =
        palette.segment_pair(a, b, threshold(j), threshold(j + 1));
    emit(j, pair[0]);
    emit(j + 1, pair[1]);
  }
  if (j < n)
    emit(j, palette.segment(oklab(read(j), to_lms), threshold(j)));
}
// Rounding boundaries are part of the discrete state machine. Do not replace
// division by a reciprocal or fuse the error multiplication and addition.
template <unsigned C, class Read, class Choose, class Emit, class Poll>
void diffuse(std::uint64_t h, std::uint64_t w, std::int64_t origin_y,
             bool serpentine, double* rows, Read read, Choose choose, Emit emit,
             Poll poll) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  auto* current = rows;
  auto* next = rows + (w + 2) * C;
  std::fill(rows, rows + 2 * (w + 2) * C, 0.0);
  for (std::uint64_t y = 0; y < h; ++y) {
    const bool reverse =
        serpentine && ((static_cast<std::uint64_t>(origin_y) + y) & 1U);
    const std::int64_t direction = reverse ? -1 : 1;
    std::fill(next, next + (w + 2) * C, 0.0);
    for (std::uint64_t step = 0; step < w; ++step) {
      if ((step & 255U) == 0)
        poll();
      const auto x = reverse ? w - step - 1 : step;
      auto v = read(y, x);
      for (unsigned c = 0; c < C; ++c)
        v[c] = v[c] + current[(x + 1) * C + c];
      const auto q = choose(v);
      emit(y, x, q.first);
      for (unsigned c = 0; c < C; ++c) {
        const double e = v[c] - q.second[c];
        const auto center = static_cast<std::int64_t>(x + 1);
        const auto forward = static_cast<std::uint64_t>(center + direction);
        const auto backward = static_cast<std::uint64_t>(center - direction);
        const double a = e * (7.0 / 16);
        current[forward * C + c] = current[forward * C + c] + a;
        const double b = e * (3.0 / 16);
        next[backward * C + c] = next[backward * C + c] + b;
        const double d = e * (5.0 / 16);
        next[(x + 1) * C + c] = next[(x + 1) * C + c] + d;
        const double f = e * (1.0 / 16);
        next[forward * C + c] = next[forward * C + c] + f;
      }
    }
    std::swap(current, next);
  }
}
}  // namespace ps::plugin_internal::grade::dither
