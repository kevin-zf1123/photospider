// Copyright 2026 Photospider Authors
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace ps::plugin_internal::grade::histogram {
inline double domain(double x, bool clamp) {
  return clamp ? std::clamp(x, 0.0, 1.0) : x;
}
struct Binning {
  std::int32_t last;
  double scale;
  explicit Binning(std::uint64_t bins)
      : last(static_cast<std::int32_t>(bins) - 1),
        scale(static_cast<double>(static_cast<std::int32_t>(bins))) {}
  // On the finite Float32 [0,1] domain, scaling up by a power of two through
  // 2^16 is exact, including subnormals. Truncation therefore gives precisely
  // the binary64-promoted bin index. Other bin counts retain binary64 math.
  bool power_of_two() const { return ((last + 1) & last) == 0; }
  std::uint32_t operator()(double x) const {
    return static_cast<std::uint32_t>(
        std::min(last, static_cast<std::int32_t>(x * scale)));
  }
};
inline std::uint64_t bin(double x, std::uint64_t bins) {
  return Binning(bins)(x);
}
#if defined(__x86_64__) || defined(__aarch64__)
#if defined(__x86_64__)
#define PS_BINARY_MAP_TARGET __attribute__((target("avx2")))
#else
#define PS_BINARY_MAP_TARGET
#endif
PS_BINARY_MAP_TARGET inline bool binary_local_mapping(double* counts,
                                                      double* out,
                                                      std::int32_t bins,
                                                      double clip_factor) {
  double total = 0.0;
  unsigned positive = 0;
  std::int32_t i = 0;
#if defined(__x86_64__)
  auto sum = _mm256_setzero_pd();
  for (; i + 4 <= bins; i += 4) {
    const auto x = _mm256_loadu_pd(counts + i);
    sum = _mm256_add_pd(sum, x);
    positive += __builtin_popcount(
        _mm256_movemask_pd(_mm256_cmp_pd(x, _mm256_setzero_pd(), _CMP_GT_OQ)));
  }
  double lanes[4];
  _mm256_storeu_pd(lanes, sum);
  total = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
#else
  auto sum = vdupq_n_f64(0.0), sum1 = sum, sum2 = sum, sum3 = sum;
  auto positive0 = vdupq_n_u64(0), positive1 = positive0, positive2 = positive0,
       positive3 = positive0;
  for (; i + 8 <= bins; i += 8) {
    const auto a = vld1q_f64(counts + i), b = vld1q_f64(counts + i + 2),
               c = vld1q_f64(counts + i + 4), d = vld1q_f64(counts + i + 6);
    sum = vaddq_f64(sum, a);
    sum1 = vaddq_f64(sum1, b);
    sum2 = vaddq_f64(sum2, c);
    sum3 = vaddq_f64(sum3, d);
    positive0 = vsubq_u64(positive0, vcgtq_f64(a, vdupq_n_f64(0.0)));
    positive1 = vsubq_u64(positive1, vcgtq_f64(b, vdupq_n_f64(0.0)));
    positive2 = vsubq_u64(positive2, vcgtq_f64(c, vdupq_n_f64(0.0)));
    positive3 = vsubq_u64(positive3, vcgtq_f64(d, vdupq_n_f64(0.0)));
  }
  sum = vaddq_f64(vaddq_f64(sum, sum1), vaddq_f64(sum2, sum3));
  positive = static_cast<unsigned>(vaddvq_u64(vaddq_u64(
      vaddq_u64(positive0, positive1), vaddq_u64(positive2, positive3))));
  for (; i + 2 <= bins; i += 2) {
    const auto x = vld1q_f64(counts + i);
    sum = vaddq_f64(sum, x);
    const auto mask = vcgtq_f64(x, vdupq_n_f64(0.0));
    positive += (vgetq_lane_u64(mask, 0) != 0) + (vgetq_lane_u64(mask, 1) != 0);
  }
  total = vgetq_lane_f64(sum, 0) + vgetq_lane_f64(sum, 1);
#endif
  for (; i < bins; ++i) {
    total += counts[i];
    positive += counts[i] > 0.0;
  }
  if (positive <= 1) {
    const double scale = 1.0 / static_cast<double>(bins - 1);
    for (i = 0; i < bins; ++i) {
      out[i] = static_cast<double>(i) * scale;
    }
    out[bins - 1] = 1.0;
    return true;
  }
  const double limit = std::max(1.0, std::floor(clip_factor * total / bins));
  double kept = 0.0;
  i = 0;
#if defined(__x86_64__)
  sum = _mm256_setzero_pd();
  for (; i + 4 <= bins; i += 4) {
    const auto x =
        _mm256_min_pd(_mm256_loadu_pd(counts + i), _mm256_set1_pd(limit));
    _mm256_storeu_pd(counts + i, x);
    sum = _mm256_add_pd(sum, x);
  }
  _mm256_storeu_pd(lanes, sum);
  kept = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
#else
  sum = sum1 = sum2 = sum3 = vdupq_n_f64(0.0);
  for (; i + 8 <= bins; i += 8) {
    const auto a = vminq_f64(vld1q_f64(counts + i), vdupq_n_f64(limit)),
               b = vminq_f64(vld1q_f64(counts + i + 2), vdupq_n_f64(limit)),
               c = vminq_f64(vld1q_f64(counts + i + 4), vdupq_n_f64(limit)),
               d = vminq_f64(vld1q_f64(counts + i + 6), vdupq_n_f64(limit));
    vst1q_f64(counts + i, a);
    vst1q_f64(counts + i + 2, b);
    vst1q_f64(counts + i + 4, c);
    vst1q_f64(counts + i + 6, d);
    sum = vaddq_f64(sum, a);
    sum1 = vaddq_f64(sum1, b);
    sum2 = vaddq_f64(sum2, c);
    sum3 = vaddq_f64(sum3, d);
  }
  sum = vaddq_f64(vaddq_f64(sum, sum1), vaddq_f64(sum2, sum3));
  for (; i + 2 <= bins; i += 2) {
    const auto x = vminq_f64(vld1q_f64(counts + i), vdupq_n_f64(limit));
    vst1q_f64(counts + i, x);
    sum = vaddq_f64(sum, x);
  }
  kept = vgetq_lane_f64(sum, 0) + vgetq_lane_f64(sum, 1);
#endif
  for (; i < bins; ++i) {
    counts[i] = std::min(counts[i], limit);
    kept += counts[i];
  }
  const auto excess = static_cast<std::uint64_t>(total - kept);
  const double delta = static_cast<double>(
      static_cast<std::int64_t>(excess / static_cast<std::uint32_t>(bins)));
  const auto remainder =
      static_cast<std::int32_t>(excess % static_cast<std::uint32_t>(bins));
  const double reciprocal = 1.0 / total;
  double prefix = 0.0;
  i = 0;
#if defined(__x86_64__)
  const auto one = _mm256_set1_pd(1.0), add = _mm256_set1_pd(delta),
             scale = _mm256_set1_pd(reciprocal);
  for (; i + 4 <= bins; i += 4) {
    const auto mask = _mm256_cvtepi32_pd(
        _mm_setr_epi32(i < remainder, i + 1 < remainder, i + 2 < remainder,
                       i + 3 < remainder));
    auto x =
        _mm256_add_pd(_mm256_add_pd(_mm256_loadu_pd(counts + i), add), mask);
    // Exact integer inclusive prefix [a,a+b,a+b+c,a+b+c+d].
    x = _mm256_add_pd(
        x, _mm256_blend_pd(_mm256_setzero_pd(), _mm256_permute_pd(x, 0), 10));
    x = _mm256_add_pd(x, _mm256_blend_pd(_mm256_setzero_pd(),
                                         _mm256_permute4x64_pd(x, 0x55), 12));
    x = _mm256_add_pd(x, _mm256_set1_pd(prefix));
    _mm256_storeu_pd(lanes, x);
    prefix = lanes[3];
    _mm256_storeu_pd(out + i, _mm256_min_pd(one, _mm256_mul_pd(x, scale)));
  }
#else
  const auto one = vdupq_n_f64(1.0), add = vdupq_n_f64(delta),
             scale = vdupq_n_f64(reciprocal);
  // Binary masses and all redistributed prefixes are nonnegative integers
  // bounded by the admitted total <=2^40. Pair/group/carry additions are exact
  // subsets of that total; reciprocal and output multiplication stay unchanged.
  for (; i + 8 <= bins; i += 8) {
    const auto pair = [&](std::int32_t at) {
      const double adjustments[2]{at < remainder ? 1.0 : 0.0,
                                  at + 1 < remainder ? 1.0 : 0.0};
      auto x = vaddq_f64(vaddq_f64(vld1q_f64(counts + at), add),
                         vld1q_f64(adjustments));
      return vaddq_f64(
          x, vsetq_lane_f64(vgetq_lane_f64(x, 0), vdupq_n_f64(0.0), 1));
    };
    auto a = pair(i), b = pair(i + 2), c = pair(i + 4), d = pair(i + 6);
    const double ab = vgetq_lane_f64(a, 1) + vgetq_lane_f64(b, 1);
    const double abc = ab + vgetq_lane_f64(c, 1);
    const auto carry = vdupq_n_f64(prefix);
    d = vaddq_f64(vaddq_f64(d, vdupq_n_f64(abc)), carry);
    c = vaddq_f64(vaddq_f64(c, vdupq_n_f64(ab)), carry);
    b = vaddq_f64(vaddq_f64(b, vdupq_n_f64(vgetq_lane_f64(a, 1))), carry);
    a = vaddq_f64(a, carry);
    prefix = vgetq_lane_f64(d, 1);
    vst1q_f64(out + i, vminq_f64(one, vmulq_f64(a, scale)));
    vst1q_f64(out + i + 2, vminq_f64(one, vmulq_f64(b, scale)));
    vst1q_f64(out + i + 4, vminq_f64(one, vmulq_f64(c, scale)));
    vst1q_f64(out + i + 6, vminq_f64(one, vmulq_f64(d, scale)));
  }
  for (; i + 2 <= bins; i += 2) {
    const double adjustments[2]{i < remainder ? 1.0 : 0.0,
                                i + 1 < remainder ? 1.0 : 0.0};
    auto x = vaddq_f64(vaddq_f64(vld1q_f64(counts + i), add),
                       vld1q_f64(adjustments));
    x = vaddq_f64(x, vsetq_lane_f64(vgetq_lane_f64(x, 0), vdupq_n_f64(0.0), 1));
    x = vaddq_f64(x, vdupq_n_f64(prefix));
    prefix = vgetq_lane_f64(x, 1);
    vst1q_f64(out + i, vminq_f64(one, vmulq_f64(x, scale)));
  }
#endif
  for (; i < bins; ++i) {
    prefix += counts[i] + delta + (i < remainder);
    out[i] = std::min(1.0, prefix * reciprocal);
  }
  out[bins - 1] = 1.0;
  return false;
}
// Guarded positive global tail CDF. For n<=65536, two-/four-lane sums and
// scans have depth <=ceil(n/2)+2. Relative to the scalar left fold, the bound
// 2*(gamma_n+gamma_depth)+10u <2.183e-11 fits the unchanged continuous profile
// 2e-12+2e-11*CDF on [0,1]. Integer binary masses remain bit-exact. Before any
// addition/write, the maximum guard prevents reassociation overflow; tiny tail
// reciprocal overflow returns to the original direct-division scalar path.
PS_BINARY_MAP_TARGET inline bool global_prefix_mapping(const double* counts,
                                                       double* out,
                                                       std::int32_t bins,
                                                       std::int32_t first,
                                                       std::int32_t last) {
  double maximum = 0.0;
  std::int32_t i = first + 1;
#if defined(__x86_64__)
  auto max_packet = _mm256_setzero_pd();
  for (; i + 4 <= last + 1; i += 4) {
    max_packet = _mm256_max_pd(max_packet, _mm256_loadu_pd(counts + i));
  }
  double lanes[4];
  _mm256_storeu_pd(lanes, max_packet);
  maximum =
      std::max(std::max(lanes[0], lanes[1]), std::max(lanes[2], lanes[3]));
#else
  auto max_packet = vdupq_n_f64(0.0), max1 = max_packet, max2 = max_packet,
       max3 = max_packet;
  for (; i + 8 <= last + 1; i += 8) {
    max_packet = vmaxq_f64(max_packet, vld1q_f64(counts + i));
    max1 = vmaxq_f64(max1, vld1q_f64(counts + i + 2));
    max2 = vmaxq_f64(max2, vld1q_f64(counts + i + 4));
    max3 = vmaxq_f64(max3, vld1q_f64(counts + i + 6));
  }
  max_packet = vmaxq_f64(vmaxq_f64(max_packet, max1), vmaxq_f64(max2, max3));
  for (; i + 2 <= last + 1; i += 2) {
    max_packet = vmaxq_f64(max_packet, vld1q_f64(counts + i));
  }
  maximum =
      std::max(vgetq_lane_f64(max_packet, 0), vgetq_lane_f64(max_packet, 1));
#endif
  for (; i <= last; ++i)
    maximum = std::max(maximum, counts[i]);
  if (maximum > std::numeric_limits<double>::max() / (2.0 * bins))
    return false;
  double denominator = 0.0;
  i = first + 1;
#if defined(__x86_64__)
  auto sum = _mm256_setzero_pd();
  for (; i + 4 <= last + 1; i += 4)
    sum = _mm256_add_pd(sum, _mm256_loadu_pd(counts + i));
  _mm256_storeu_pd(lanes, sum);
  denominator = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
#else
  auto sum = vdupq_n_f64(0.0), sum1 = sum, sum2 = sum, sum3 = sum;
  for (; i + 8 <= last + 1; i += 8) {
    sum = vaddq_f64(sum, vld1q_f64(counts + i));
    sum1 = vaddq_f64(sum1, vld1q_f64(counts + i + 2));
    sum2 = vaddq_f64(sum2, vld1q_f64(counts + i + 4));
    sum3 = vaddq_f64(sum3, vld1q_f64(counts + i + 6));
  }
  sum = vaddq_f64(vaddq_f64(sum, sum1), vaddq_f64(sum2, sum3));
  for (; i + 2 <= last + 1; i += 2)
    sum = vaddq_f64(sum, vld1q_f64(counts + i));
  denominator = vgetq_lane_f64(sum, 0) + vgetq_lane_f64(sum, 1);
#endif
  for (; i <= last; ++i)
    denominator += counts[i];
  const double reciprocal = 1.0 / denominator;
  if (!std::isfinite(reciprocal))
    return false;
  std::fill(out, out + first + 1, 0.0);
  double prefix = 0.0;
  i = first + 1;
#if defined(__x86_64__)
  const auto one = _mm256_set1_pd(1.0), scale = _mm256_set1_pd(reciprocal);
  for (; i + 4 <= last; i += 4) {
    auto x = _mm256_loadu_pd(counts + i);
    x = _mm256_add_pd(
        x, _mm256_blend_pd(_mm256_setzero_pd(), _mm256_permute_pd(x, 0), 10));
    x = _mm256_add_pd(x, _mm256_blend_pd(_mm256_setzero_pd(),
                                         _mm256_permute4x64_pd(x, 0x55), 12));
    x = _mm256_add_pd(x, _mm256_set1_pd(prefix));
    _mm256_storeu_pd(lanes, x);
    prefix = lanes[3];
    _mm256_storeu_pd(out + i, _mm256_min_pd(one, _mm256_mul_pd(x, scale)));
  }
#else
  const auto one = vdupq_n_f64(1.0), scale = vdupq_n_f64(reciprocal);
  for (; i + 8 <= last; i += 8) {
    auto a = vld1q_f64(counts + i), b = vld1q_f64(counts + i + 2),
         c = vld1q_f64(counts + i + 4), d = vld1q_f64(counts + i + 6);
    const auto pair_prefix = [](float64x2_t x) {
      return vaddq_f64(
          x, vsetq_lane_f64(vgetq_lane_f64(x, 0), vdupq_n_f64(0.0), 1));
    };
    a = pair_prefix(a);
    b = pair_prefix(b);
    c = pair_prefix(c);
    d = pair_prefix(d);
    const double ab = vgetq_lane_f64(a, 1) + vgetq_lane_f64(b, 1);
    const double abc = ab + vgetq_lane_f64(c, 1);
    const auto carry = vdupq_n_f64(prefix);
    d = vaddq_f64(vaddq_f64(d, vdupq_n_f64(abc)), carry);
    c = vaddq_f64(vaddq_f64(c, vdupq_n_f64(ab)), carry);
    b = vaddq_f64(vaddq_f64(b, vdupq_n_f64(vgetq_lane_f64(a, 1))), carry);
    a = vaddq_f64(a, carry);
    // Carry is the last emitted prefix. Nonnegative additions then preserve
    // monotonicity across both pair and eight-bin block boundaries.
    prefix = vgetq_lane_f64(d, 1);
    vst1q_f64(out + i, vminq_f64(one, vmulq_f64(a, scale)));
    vst1q_f64(out + i + 2, vminq_f64(one, vmulq_f64(b, scale)));
    vst1q_f64(out + i + 4, vminq_f64(one, vmulq_f64(c, scale)));
    vst1q_f64(out + i + 6, vminq_f64(one, vmulq_f64(d, scale)));
  }
  for (; i + 2 <= last; i += 2) {
    auto x = vld1q_f64(counts + i);
    x = vaddq_f64(x, vsetq_lane_f64(vgetq_lane_f64(x, 0), vdupq_n_f64(0.0), 1));
    x = vaddq_f64(x, vdupq_n_f64(prefix));
    prefix = vgetq_lane_f64(x, 1);
    vst1q_f64(out + i, vminq_f64(one, vmulq_f64(x, scale)));
  }
#endif
  for (; i < last; ++i) {
    prefix += counts[i];
    out[i] = std::min(1.0, prefix * reciprocal);
  }
  std::fill(out + last, out + bins, 1.0);
  return true;
}
#undef PS_BINARY_MAP_TARGET
#endif
// Counts are exact integers through 2^40 in binary mode. Weighted mode uses
// binary64 accumulators. Caller supplies charged, invocation-owned workspace.
inline bool mapping(double* counts, double* out, std::uint64_t bin_count,
                    bool local, bool weighted, double clip_factor) {
  const auto bins = static_cast<std::int32_t>(bin_count);
#if defined(__x86_64__) || defined(__aarch64__)
  // Binary local counts, clipped counts, redistributed increments, and prefixes
  // are nonnegative integers with total <=2^40. Every SIMD reassociation below
  // is therefore exact; weighted accumulation remains on its original path.
  if (local && !weighted) {
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2"))
#endif
      return binary_local_mapping(counts, out, bins, clip_factor);
  }
#endif
  if (!local) {
    std::int32_t first = 0, last = bins - 1;
    while (first < bins && !(counts[first] > 0.0))
      ++first;
    while (last > first && !(counts[last] > 0.0))
      --last;
    if (first >= last) {
      const double scale = 1.0 / static_cast<double>(bins - 1);
      for (std::int32_t i = 0; i < bins; ++i) {
        out[i] = static_cast<double>(i) * scale;
      }
      out[bins - 1] = 1.0;
      return true;
    }
#if defined(__x86_64__) || defined(__aarch64__)
    if (last - first >= 16) {
#if defined(__x86_64__)
      if (__builtin_cpu_supports("avx2"))
#endif
        if (global_prefix_mapping(counts, out, bins, first, last))
          return false;
    }
#endif
    double denominator = 0.0;
    for (std::int32_t i = first + 1; i <= last; ++i) {
      if (counts[i] > 0.0)
        denominator += counts[i];
    }
    std::fill(out, out + first + 1, 0.0);
    const double reciprocal = 1.0 / denominator;
    const bool scaled = std::isfinite(reciprocal);
    double prefix = 0.0;
    for (std::int32_t i = first + 1; i < last; ++i) {
      prefix += counts[i];
      out[i] =
          std::min(1.0, scaled ? prefix * reciprocal : prefix / denominator);
    }
    std::fill(out + last, out + bins, 1.0);
    return false;
  }
  double total = 0;
  std::int32_t positive = 0;
  for (std::int32_t i = 0; i < bins; ++i) {
    total += counts[i];
    if (counts[i] > 0) {
      ++positive;
    }
  }
  if (positive <= 1) {
    const double scale = 1.0 / static_cast<double>(bins - 1);
    for (std::int32_t i = 0; i < bins; ++i) {
      out[i] = static_cast<double>(i) * scale;
    }
    out[bins - 1] = 1.0;
    return true;
  }
  if (local) {
    const double limit =
        weighted ? clip_factor * total / bins
                 : std::max(1.0, std::floor(clip_factor * total / bins));
    double kept = 0;
    for (std::int32_t i = 0; i < bins; ++i) {
      counts[i] = std::min(counts[i], limit);
      kept += counts[i];
    }
    const double excess = total - kept;
    const auto integer_excess =
        weighted ? 0 : static_cast<std::uint64_t>(excess);
    const double delta =
        weighted ? excess / bins
                 : static_cast<double>(static_cast<std::int64_t>(
                       integer_excess / static_cast<std::uint32_t>(bins)));
    const auto remainder =
        weighted ? 0U
                 : static_cast<std::uint32_t>(integer_excess %
                                              static_cast<std::uint32_t>(bins));
    double prefix = 0;
    const double reciprocal = 1.0 / total;
    const bool scaled = std::isfinite(reciprocal);
    for (std::int32_t i = 0; i < bins; ++i) {
      prefix += counts[i] + delta + (i < remainder);
      out[i] = std::min(1.0, scaled ? prefix * reciprocal : prefix / total);
    }
    // The exact redistributed mass equals total. Floating prefix accumulation
    // must not open the map's [0,1] domain or move its terminal value from one.
    out[bins - 1] = 1;
  }
  return false;
}
#if defined(__x86_64__) || defined(__aarch64__)
#if defined(__x86_64__)
#define PS_HIST_PACKET_TARGET __attribute__((target("avx2")))
#else
#define PS_HIST_PACKET_TARGET
#endif
// Binary counts are exact through the admitted 2^40 samples. SIMD computes
// double-precision bins; the lane-ordered scalar updates retain all collisions.
// A partial packet falls back before loading any zero-selected source sample.
template <class T>
PS_HIST_PACKET_TARGET inline std::uint64_t histogram_packets(
    const T* input, const std::uint8_t* selection, std::uint64_t n,
    double* counts, const Binning& bin, bool clamp) {
  std::uint64_t j = 0;
#if defined(__x86_64__)
  const auto zero = _mm256_setzero_pd(), one = _mm256_set1_pd(1.0),
             scale = _mm256_set1_pd(bin.scale);
  const auto last = _mm_set1_epi32(bin.last);
  if constexpr (std::is_same_v<T, float>) {
    if (bin.power_of_two()) {
      const auto scale32 = _mm256_set1_ps(static_cast<float>(bin.scale));
      for (; j + 8 <= n; j += 8) {
        bool all = true;
        for (unsigned lane = 0; lane < 8; ++lane) {
          all &= selection[j + lane] != 0;
        }
        if (!all) {
          break;
        }
        auto x = _mm256_loadu_ps(input + j);
        if (clamp) {
          x = _mm256_min_ps(_mm256_max_ps(x, _mm256_setzero_ps()),
                            _mm256_set1_ps(1.0f));
        }
        std::int32_t indices[8];
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(indices),
            _mm256_min_epi32(_mm256_cvttps_epi32(_mm256_mul_ps(x, scale32)),
                             _mm256_set1_epi32(bin.last)));
        for (unsigned lane = 0; lane < 8; ++lane) {
          counts[indices[lane]] += 1.0;
        }
      }
    }
  }
  for (; j + 4 <= n; j += 4) {
    if (!selection[j] || !selection[j + 1] || !selection[j + 2] ||
        !selection[j + 3]) {
      break;
    }
    __m256d x;
    if constexpr (std::is_same_v<T, float>) {
      x = _mm256_cvtps_pd(_mm_loadu_ps(input + j));
    } else {
      x = _mm256_loadu_pd(input + j);
    }
    if (clamp) {
      x = _mm256_min_pd(_mm256_max_pd(x, zero), one);
    }
    std::int32_t index[4];
    _mm_storeu_si128(
        reinterpret_cast<__m128i*>(index),
        _mm_min_epi32(_mm256_cvttpd_epi32(_mm256_mul_pd(x, scale)), last));
    for (unsigned lane = 0; lane < 4; ++lane) {
      counts[index[lane]] += 1.0;
    }
  }
#else
  const auto zero = vdupq_n_f64(0.0), one = vdupq_n_f64(1.0),
             scale = vdupq_n_f64(bin.scale);
  const auto last = vdup_n_u32(bin.last);
  if constexpr (std::is_same_v<T, float>) {
    if (bin.power_of_two()) {
      for (; j + 4 <= n; j += 4) {
        if (!selection[j] || !selection[j + 1] || !selection[j + 2] ||
            !selection[j + 3]) {
          break;
        }
        auto x = vld1q_f32(input + j);
        if (clamp) {
          x = vminq_f32(vmaxq_f32(x, vdupq_n_f32(0.0f)), vdupq_n_f32(1.0f));
        }
        std::uint32_t indices[4];
        vst1q_u32(indices, vminq_u32(vcvtq_u32_f32(vmulq_n_f32(
                                         x, static_cast<float>(bin.scale))),
                                     vdupq_n_u32(bin.last)));
        for (unsigned lane = 0; lane < 4; ++lane) {
          counts[indices[lane]] += 1.0;
        }
      }
    }
  }
  for (; j + 2 <= n; j += 2) {
    if (!selection[j] || !selection[j + 1]) {
      break;
    }
    float64x2_t x;
    if constexpr (std::is_same_v<T, float>) {
      x = vcvt_f64_f32(vld1_f32(input + j));
    } else {
      x = vld1q_f64(input + j);
    }
    if (clamp) {
      x = vminq_f64(vmaxq_f64(x, zero), one);
    }
    const auto index =
        vmin_u32(vmovn_u64(vcvtq_u64_f64(vmulq_f64(x, scale))), last);
    counts[vget_lane_u32(index, 0)] += 1.0;
    counts[vget_lane_u32(index, 1)] += 1.0;
  }
#endif
  return j;
}
#undef PS_HIST_PACKET_TARGET
#endif
template <class T, class W>
inline std::pair<std::uint64_t, double> histogram_span(
    const void* input, std::ptrdiff_t input_stride, const void* selection,
    std::ptrdiff_t selection_stride, std::uint64_t n, double* counts,
    std::uint64_t bins, bool clamp, bool need_feature) {
#pragma clang fp exceptions(ignore)
  const Binning index(bins);
  std::uint64_t count = 0;
  double mass = 0;
#if defined(__x86_64__) || defined(__aarch64__)
  if constexpr (std::is_same_v<W, std::uint8_t>) {
    if (need_feature && input_stride == sizeof(T) && selection_stride == 1 &&
        reinterpret_cast<std::uintptr_t>(input) % alignof(T) == 0) {
#if defined(__x86_64__)
      if (__builtin_cpu_supports("avx2"))
#endif
        count = histogram_packets(static_cast<const T*>(input),
                                  static_cast<const std::uint8_t*>(selection),
                                  n, counts, index, clamp);
    }
  }
#endif
  for (std::uint64_t j = count; j < n; ++j) {
    W weight;
    std::memcpy(&weight,
                static_cast<const unsigned char*>(selection) +
                    static_cast<std::ptrdiff_t>(j) * selection_stride,
                sizeof(W));
    if (weight == 0) {
      continue;
    }
    ++count;
    if constexpr (std::is_same_v<W, double>) {
      mass += weight;
    }
    if (need_feature) {
      T value;
      std::memcpy(&value,
                  static_cast<const unsigned char*>(input) +
                      static_cast<std::ptrdiff_t>(j) * input_stride,
                  sizeof(T));
      if constexpr (std::is_same_v<W, double>) {
        counts[index(domain(value, clamp))] += weight;
      } else {
        counts[index(domain(value, clamp))] += 1.0;
      }
    }
  }
  return {count, std::is_same_v<W, double>
                     ? mass
                     : static_cast<double>(static_cast<std::int64_t>(count))};
}
// Only global step-map output conversion is moved here. The table is refreshed
// in invocation-owned workspace on every Run, never retained across inputs.
inline constexpr std::uint64_t kPreparedMapCapacity = 256;
#if defined(__x86_64__)
__attribute__((target("avx2"))) inline void convert_apply_map(
    const double* map, float* prepared, std::uint64_t bins) {
  std::uint64_t i = 0;
  for (; i + 4 <= bins; i += 4) {
    _mm_storeu_ps(prepared + i, _mm256_cvtpd_ps(_mm256_loadu_pd(map + i)));
  }
  for (; i < bins; ++i) {
    prepared[i] = static_cast<float>(map[i]);
  }
}
#endif
inline bool can_prepare_apply_map(const float* workspace, std::uint64_t bins,
                                  std::uint64_t samples, bool identity) {
#if defined(__x86_64__)
  // Conversion uses exactly the old gather-then-narrow instruction. Preparing
  // all entries is legal for the admitted full-map support, but identity must
  // bypass even a null map. Four uses per entry amortize the bounded setup.
  return !identity && workspace && bins <= kPreparedMapCapacity &&
         Binning(bins).power_of_two() && samples >= 4 * bins &&
         __builtin_cpu_supports("avx2");
#else
  (void)workspace;
  (void)bins;
  (void)samples;
  (void)identity;
  return false;
#endif
}
inline const float* prepare_apply_map(const double* map, float* workspace,
                                      std::uint64_t bins, std::uint64_t samples,
                                      bool identity) {
#if defined(__x86_64__)
  if (can_prepare_apply_map(workspace, bins, samples, identity)) {
    convert_apply_map(map, workspace, bins);
    return workspace;
  }
#else
  (void)map;
  (void)workspace;
  (void)bins;
  (void)samples;
  (void)identity;
#endif
  return nullptr;
}
#if defined(__x86_64__)
template <class T>
__attribute__((target("avx2"))) inline std::uint64_t apply_packets(
    const T* input, T* output, std::uint64_t n, const double* map,
    const Binning& bin, bool clamp) {
  std::uint64_t j = 0;
  const auto scale = _mm256_set1_pd(bin.scale), zero = _mm256_setzero_pd(),
             one = _mm256_set1_pd(1.0);
  const auto last = _mm_set1_epi32(bin.last);
  // Snapshot a bounded block before stores, limiting partial-address
  // dependencies without assuming any particular buffer alignment.
  if constexpr (std::is_same_v<T, double>) {
    for (; j + 16 <= n; j += 16) {
      __m256d x[4], y[4];
      for (unsigned p = 0; p < 4; ++p) {
        x[p] = _mm256_loadu_pd(input + j + 4 * p);
      }
      for (unsigned p = 0; p < 4; ++p) {
        if (clamp) {
          x[p] = _mm256_min_pd(_mm256_max_pd(x[p], zero), one);
        }
        const auto index = _mm_min_epi32(
            _mm256_cvttpd_epi32(_mm256_mul_pd(x[p], scale)), last);
        y[p] = _mm256_i32gather_pd(map, index, 8);
      }
      for (unsigned p = 0; p < 4; ++p) {
        _mm256_storeu_pd(output + j + 4 * p, y[p]);
      }
    }
  }
  if constexpr (std::is_same_v<T, float>) {
    if (bin.power_of_two()) {
      const auto scale32 = _mm256_set1_ps(static_cast<float>(bin.scale));
      for (; j + 8 <= n; j += 8) {
        auto x = _mm256_loadu_ps(input + j);
        if (clamp) {
          x = _mm256_min_ps(_mm256_max_ps(x, _mm256_setzero_ps()),
                            _mm256_set1_ps(1.0f));
        }
        const auto indices =
            _mm256_min_epi32(_mm256_cvttps_epi32(_mm256_mul_ps(x, scale32)),
                             _mm256_set1_epi32(bin.last));
        const auto lo =
            _mm256_i32gather_pd(map, _mm256_castsi256_si128(indices), 8);
        const auto hi =
            _mm256_i32gather_pd(map, _mm256_extracti128_si256(indices, 1), 8);
        _mm256_storeu_ps(
            output + j,
            _mm256_insertf128_ps(_mm256_castps128_ps256(_mm256_cvtpd_ps(lo)),
                                 _mm256_cvtpd_ps(hi), 1));
      }
    }
  }
  for (; j + 4 <= n; j += 4) {
    auto x = std::is_same_v<T, float>
                 ? _mm256_cvtps_pd(
                       _mm_loadu_ps(reinterpret_cast<const float*>(input + j)))
                 : _mm256_loadu_pd(reinterpret_cast<const double*>(input + j));
    if (clamp) {
      x = _mm256_min_pd(_mm256_max_pd(x, zero), one);
    }
    const auto index =
        _mm_min_epi32(_mm256_cvttpd_epi32(_mm256_mul_pd(x, scale)), last);
    const auto y = _mm256_i32gather_pd(map, index, 8);
    if constexpr (std::is_same_v<T, float>) {
      _mm_storeu_ps(output + j, _mm256_cvtpd_ps(y));
    } else {
      _mm256_storeu_pd(output + j, y);
    }
  }
  return j;
}
#elif defined(__aarch64__)
template <class T>
inline std::uint64_t apply_packets(const T* input, T* output, std::uint64_t n,
                                   const double* map, const Binning& bin,
                                   bool clamp) {
  std::uint64_t j = 0;
  const auto scale = vdupq_n_f64(bin.scale), zero = vdupq_n_f64(0.0),
             one = vdupq_n_f64(1.0);
  const auto last = vdup_n_u32(bin.last);
  if constexpr (std::is_same_v<T, float>) {
    if (bin.power_of_two()) {
      for (; j + 4 <= n; j += 4) {
        auto x = vld1q_f32(input + j);
        if (clamp) {
          x = vminq_f32(vmaxq_f32(x, vdupq_n_f32(0.0f)), vdupq_n_f32(1.0f));
        }
        std::uint32_t indices[4];
        vst1q_u32(indices, vminq_u32(vcvtq_u32_f32(vmulq_n_f32(
                                         x, static_cast<float>(bin.scale))),
                                     vdupq_n_u32(bin.last)));
        double values[4];
        for (unsigned lane = 0; lane < 4; ++lane) {
          std::memcpy(values + lane, map + indices[lane], sizeof(double));
        }
        vst1q_f32(output + j,
                  vcombine_f32(vcvt_f32_f64(vld1q_f64(values)),
                               vcvt_f32_f64(vld1q_f64(values + 2))));
      }
    }
  }
  for (; j + 2 <= n; j += 2) {
    float64x2_t x;
    if constexpr (std::is_same_v<T, float>) {
      x = vcvt_f64_f32(vld1_f32(input + j));
    } else {
      x = vld1q_f64(input + j);
    }
    if (clamp) {
      x = vminq_f64(vmaxq_f64(x, zero), one);
    }
    const auto index =
        vmin_u32(vmovn_u64(vcvtq_u64_f64(vmulq_f64(x, scale))), last);
    double values[2];
    std::memcpy(values, map + vget_lane_u32(index, 0), 8);
    std::memcpy(values + 1, map + vget_lane_u32(index, 1), 8);
    const auto y = vld1q_f64(values);
    if constexpr (std::is_same_v<T, float>) {
      vst1_f32(output + j, vcvt_f32_f64(y));
    } else {
      vst1q_f64(output + j, y);
    }
  }
  return j;
}
#endif
template <class T>
inline void apply_span(const void* input, std::ptrdiff_t input_stride,
                       void* output, std::ptrdiff_t output_stride,
                       std::uint64_t n, const double* map, std::uint64_t bins,
                       bool identity, bool clamp) {
#pragma clang fp exceptions(ignore)
  const Binning index(bins);
  std::uint64_t first = 0;
#if defined(__x86_64__) || defined(__aarch64__)
  // Identity does not consume a map, including a null placeholder map.
  if (!identity && n >= 4 && input_stride == sizeof(T) &&
      output_stride == sizeof(T) &&
      reinterpret_cast<std::uintptr_t>(input) % alignof(T) == 0 &&
      reinterpret_cast<std::uintptr_t>(output) % alignof(T) == 0) {
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2"))
#endif
      first = apply_packets(static_cast<const T*>(input),
                            static_cast<T*>(output), n, map, index, clamp);
  }
#endif
  for (std::uint64_t j = first; j < n; ++j) {
    T x;
    std::memcpy(&x,
                static_cast<const unsigned char*>(input) +
                    static_cast<std::ptrdiff_t>(j) * input_stride,
                sizeof(T));
    const double u = domain(x, clamp);
    T y = identity ? (u == x ? x : static_cast<T>(u))
                   : static_cast<T>(map[index(u)]);
    std::memcpy(static_cast<unsigned char*>(output) +
                    static_cast<std::ptrdiff_t>(j) * output_stride,
                &y, sizeof(T));
  }
}
#if defined(__x86_64__)
__attribute__((target("avx2"))) inline std::uint64_t apply_prepared_packets(
    const float* input, float* output, std::uint64_t n, const float* prepared,
    const Binning& bin, bool clamp) {
  const auto scale = _mm256_set1_ps(static_cast<float>(bin.scale));
  const auto last = _mm256_set1_epi32(bin.last);
  std::uint64_t j = 0;
  for (; j + 8 <= n; j += 8) {
    auto x = _mm256_loadu_ps(input + j);
    if (clamp) {
      x = _mm256_min_ps(_mm256_max_ps(x, _mm256_setzero_ps()),
                        _mm256_set1_ps(1.0f));
    }
    const auto indices =
        _mm256_min_epi32(_mm256_cvttps_epi32(_mm256_mul_ps(x, scale)), last);
    _mm256_storeu_ps(output + j, _mm256_i32gather_ps(prepared, indices, 4));
  }
  return j;
}
#endif
// Keep the original generic/F64/ARM call structure independent of preparation.
inline void apply_prepared_span(const void* input, std::ptrdiff_t input_stride,
                                void* output, std::ptrdiff_t output_stride,
                                std::uint64_t n, const double* map,
                                std::uint64_t bins, bool identity, bool clamp,
                                const float* prepared) {
  std::uint64_t first = 0;
#if defined(__x86_64__)
  if (!identity && prepared && n >= 8 && input_stride == sizeof(float) &&
      output_stride == sizeof(float) &&
      reinterpret_cast<std::uintptr_t>(input) % alignof(float) == 0 &&
      reinterpret_cast<std::uintptr_t>(output) % alignof(float) == 0) {
    first = apply_prepared_packets(static_cast<const float*>(input),
                                   static_cast<float*>(output), n, prepared,
                                   Binning(bins), clamp);
  }
#else
  (void)prepared;
#endif
  if (first < n) {
    apply_span<float>(static_cast<const unsigned char*>(input) +
                          static_cast<std::ptrdiff_t>(first) * input_stride,
                      input_stride,
                      static_cast<unsigned char*>(output) +
                          static_cast<std::ptrdiff_t>(first) * output_stride,
                      output_stride, n - first, map, bins, identity, clamp);
  }
}
struct LocalMap {
  const unsigned char* data = nullptr;
  std::ptrdiff_t stride = 8;
  bool identity = true;
};
#if defined(__x86_64__) || defined(__aarch64__)
#if defined(__x86_64__)
#define PS_LOCAL_PACKET_TARGET __attribute__((target("avx2")))
#else
#define PS_LOCAL_PACKET_TARGET
#endif
template <class T, unsigned LoadMask>
PS_LOCAL_PACKET_TARGET inline std::uint64_t local_lookup_packets(
    const T* input, T* output, std::uint64_t begin, std::uint64_t end,
    const unsigned char* const* tables, const Binning& bin, bool clamp,
    double fy, double fx, double step) {
  if constexpr (LoadMask == 0) {
    return begin;
  }
  for (unsigned c = 0; c < 4; ++c) {
    if ((LoadMask & (1U << c)) &&
        reinterpret_cast<std::uintptr_t>(tables[c]) % alignof(double)) {
      return begin;
    }
  }
  std::uint64_t j = begin;
#if defined(__x86_64__)
  const auto scale = _mm256_set1_pd(bin.scale), zero = _mm256_setzero_pd(),
             one = _mm256_set1_pd(1.0), ys = _mm256_set1_pd(fy),
             wy = _mm256_set1_pd(1.0 - fy), origin = _mm256_set1_pd(fx),
             dx = _mm256_set1_pd(step);
  const auto last = _mm_set1_epi32(bin.last);
  for (; j + 4 <= end; j += 4) {
    __m256d u;
    if constexpr (std::is_same_v<T, float>) {
      u = _mm256_cvtps_pd(_mm_loadu_ps(input + j));
    } else {
      u = _mm256_loadu_pd(input + j);
    }
    if (clamp) {
      u = _mm256_min_pd(_mm256_max_pd(u, zero), one);
    }
    const auto index =
        _mm_min_epi32(_mm256_cvttpd_epi32(_mm256_mul_pd(u, scale)), last);
    auto a = u, b = u, c = u, d = u;
    if constexpr (LoadMask & 1) {
      a = _mm256_i32gather_pd(reinterpret_cast<const double*>(tables[0]), index,
                              8);
    }
    if constexpr (LoadMask & 2) {
      b = _mm256_i32gather_pd(reinterpret_cast<const double*>(tables[1]), index,
                              8);
    }
    if constexpr (LoadMask & 4) {
      c = _mm256_i32gather_pd(reinterpret_cast<const double*>(tables[2]), index,
                              8);
    }
    if constexpr (LoadMask & 8) {
      d = _mm256_i32gather_pd(reinterpret_cast<const double*>(tables[3]), index,
                              8);
    }
    const auto offsets =
        _mm256_set_pd(static_cast<double>(static_cast<std::int64_t>(j + 3)),
                      static_cast<double>(static_cast<std::int64_t>(j + 2)),
                      static_cast<double>(static_cast<std::int64_t>(j + 1)),
                      static_cast<double>(static_cast<std::int64_t>(j)));
    const auto t = _mm256_add_pd(origin, _mm256_mul_pd(offsets, dx)),
               wt = _mm256_sub_pd(one, t);
    auto y = _mm256_add_pd(
        _mm256_mul_pd(wy,
                      _mm256_add_pd(_mm256_mul_pd(wt, a), _mm256_mul_pd(t, b))),
        _mm256_mul_pd(
            ys, _mm256_add_pd(_mm256_mul_pd(wt, c), _mm256_mul_pd(t, d))));
    y = _mm256_blendv_pd(y, zero, _mm256_cmp_pd(y, zero, _CMP_LT_OQ));
    y = _mm256_blendv_pd(y, one, _mm256_cmp_pd(y, one, _CMP_GT_OQ));
    if constexpr (std::is_same_v<T, float>) {
      _mm_storeu_ps(output + j, _mm256_cvtpd_ps(y));
    } else {
      _mm256_storeu_pd(output + j, y);
    }
  }
#else
  const auto scale = vdupq_n_f64(bin.scale), zero = vdupq_n_f64(0.0),
             one = vdupq_n_f64(1.0), ys = vdupq_n_f64(fy),
             wy = vdupq_n_f64(1.0 - fy), origin = vdupq_n_f64(fx),
             dx = vdupq_n_f64(step);
  const auto last = vdup_n_u32(bin.last);
  for (; j + 2 <= end; j += 2) {
    float64x2_t u;
    if constexpr (std::is_same_v<T, float>) {
      u = vcvt_f64_f32(vld1_f32(input + j));
    } else {
      u = vld1q_f64(input + j);
    }
    if (clamp) {
      u = vminq_f64(vmaxq_f64(u, zero), one);
    }
    const auto index =
        vmin_u32(vmovn_u64(vcvtq_u64_f64(vmulq_f64(u, scale))), last);
    const auto gather = [&](unsigned c) {
      double values[2];
      std::memcpy(values, tables[c] + 8 * vget_lane_u32(index, 0), 8);
      std::memcpy(values + 1, tables[c] + 8 * vget_lane_u32(index, 1), 8);
      return vld1q_f64(values);
    };
    auto a = u, b = u, c = u, d = u;
    if constexpr (LoadMask & 1) {
      a = gather(0);
    }
    if constexpr (LoadMask & 2) {
      b = gather(1);
    }
    if constexpr (LoadMask & 4) {
      c = gather(2);
    }
    if constexpr (LoadMask & 8) {
      d = gather(3);
    }
    const double offsets[]{
        static_cast<double>(static_cast<std::int64_t>(j)),
        static_cast<double>(static_cast<std::int64_t>(j + 1))};
    const auto t = vaddq_f64(origin, vmulq_f64(vld1q_f64(offsets), dx)),
               wt = vsubq_f64(one, t);
    auto y =
        vaddq_f64(vmulq_f64(wy, vaddq_f64(vmulq_f64(wt, a), vmulq_f64(t, b))),
                  vmulq_f64(ys, vaddq_f64(vmulq_f64(wt, c), vmulq_f64(t, d))));
    y = vbslq_f64(vcltq_f64(y, zero), zero, y);
    y = vbslq_f64(vcgtq_f64(y, one), one, y);
    if constexpr (std::is_same_v<T, float>) {
      vst1_f32(output + j, vcvt_f32_f64(y));
    } else {
      vst1q_f64(output + j, y);
    }
  }
#endif
  return j;
}
#undef PS_LOCAL_PACKET_TARGET
#endif
template <class T, unsigned LoadMask>
inline void contiguous_local(const T* input, T* output, std::uint64_t begin,
                             std::uint64_t end,
                             const unsigned char* const* tables,
                             const Binning& index, bool clamp, double fy,
                             double fx, double step) {
#if defined(__x86_64__) || defined(__aarch64__)
  if constexpr (LoadMask != 0) {
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2"))
#endif
      begin = local_lookup_packets<T, LoadMask>(
          input, output, begin, end, tables, index, clamp, fy, fx, step);
  }
#endif
  for (auto j = begin; j < end; ++j) {
    const T x = input[j];
    const double u = domain(x, clamp);
    if constexpr (LoadMask == 0) {
      if (u == x) {
        std::memcpy(output + j, input + j, sizeof(T));
      } else {
        output[j] = static_cast<T>(u);
      }
    } else {
      const auto b = index(u);
      double v0 = u, v1 = u, v2 = u, v3 = u;
      if constexpr (LoadMask & 1) {
        std::memcpy(&v0, tables[0] + 8 * b, 8);
      }
      if constexpr (LoadMask & 2) {
        std::memcpy(&v1, tables[1] + 8 * b, 8);
      }
      if constexpr (LoadMask & 4) {
        std::memcpy(&v2, tables[2] + 8 * b, 8);
      }
      if constexpr (LoadMask & 8) {
        std::memcpy(&v3, tables[3] + 8 * b, 8);
      }
      const double t =
          fx + static_cast<double>(static_cast<std::int64_t>(j)) * step;
      output[j] =
          static_cast<T>(std::clamp((1.0 - fy) * ((1.0 - t) * v0 + t * v1) +
                                        fy * ((1.0 - t) * v2 + t * v3),
                                    0.0, 1.0));
    }
  }
}
template <class T>
inline bool contiguous_local_dispatch(const void* input,
                                      std::ptrdiff_t input_stride, void* output,
                                      std::ptrdiff_t output_stride,
                                      std::uint64_t n, const LocalMap* maps,
                                      const Binning& index, bool clamp,
                                      double fy, double fx, double step) {
  if (n < 4 || input_stride != sizeof(T) || output_stride != sizeof(T) ||
      reinterpret_cast<std::uintptr_t>(input) % alignof(T) ||
      reinterpret_cast<std::uintptr_t>(output) % alignof(T)) {
    return false;
  }
  unsigned active = (fy == 0.0 ? 3U : fy == 1.0 ? 12U : 15U), identity = 0;
  const unsigned char* tables[4]{};
  for (unsigned c = 0; c < 4; ++c) {
    const auto map = maps[c];
    if (map.identity) {
      identity |= 1U << c;
    }
    tables[c] = map.data;
    if ((active & (1U << c)) && !map.identity && map.stride != 8) {
      return false;
    }
  }
  std::uint64_t begin = 0, end = n;
  if (step == 0.0) {
    if (fx == 0.0) {
      active &= 5U;
    } else if (fx == 1.0) {
      active &= 10U;
    } else if (!(fx > 0.0 && fx < 1.0)) {
      return false;
    }
  } else {
    if (n < 4) {
      return false;
    }
    if (fx == 0.0) {
      begin = 1;
    }
    const double last =
        fx + static_cast<double>(static_cast<std::int64_t>(n - 1)) * step;
    if (last == 1.0) {
      --end;
    }
    if (!(fx + static_cast<double>(static_cast<std::int64_t>(begin)) * step >
              0.0 &&
          fx + static_cast<double>(static_cast<std::int64_t>(end - 1)) * step <
              1.0)) {
      return false;
    }
  }
  const auto* source = static_cast<const T*>(input);
  auto* destination = static_cast<T*>(output);
  const unsigned mask = active & ~identity;
#define PS_LOCAL_CASE(M)                                                   \
  case M:                                                                  \
    contiguous_local<T, M>(source, destination, begin, end, tables, index, \
                           clamp, fy, fx, step);                           \
    break
  switch (mask) {
    PS_LOCAL_CASE(0);
    PS_LOCAL_CASE(1);
    PS_LOCAL_CASE(2);
    PS_LOCAL_CASE(3);
    PS_LOCAL_CASE(4);
    PS_LOCAL_CASE(5);
    PS_LOCAL_CASE(6);
    PS_LOCAL_CASE(7);
    PS_LOCAL_CASE(8);
    PS_LOCAL_CASE(9);
    PS_LOCAL_CASE(10);
    PS_LOCAL_CASE(11);
    PS_LOCAL_CASE(12);
    PS_LOCAL_CASE(13);
    PS_LOCAL_CASE(14);
    PS_LOCAL_CASE(15);
  }
#undef PS_LOCAL_CASE
  // Peeling endpoints retains the exact original-j coefficient and never
  // reads a zero-contribution neighbor. Endpoint n is at most two pixels.
  const auto endpoint = [&](std::uint64_t j, unsigned horizontal) {
    const unsigned mask0 = active & horizontal & ~identity;
    const T x = source[j];
    const double u = domain(x, clamp);
    if (!mask0) {
      if (u == x) {
        std::memcpy(destination + j, source + j, sizeof(T));
      } else {
        destination[j] = static_cast<T>(u);
      }
      return;
    }
    const auto b = index(u);
    double v[4]{u, u, u, u};
    for (unsigned c = 0; c < 4; ++c) {
      if (mask0 & (1U << c)) {
        std::memcpy(v + c, tables[c] + 8 * b, 8);
      }
    }
    destination[j] = static_cast<T>(
        std::clamp(horizontal == 5U ? (1.0 - fy) * v[0] + fy * v[2]
                                    : (1.0 - fy) * v[1] + fy * v[3],
                   0.0, 1.0));
  };
  if (begin) {
    endpoint(0, 5U);
  }
  if (end < n) {
    endpoint(n - 1, 10U);
  }
  return true;
}
template <class T>
inline void local_apply_span(const void* input, std::ptrdiff_t input_stride,
                             void* output, std::ptrdiff_t output_stride,
                             std::uint64_t n, const LocalMap* maps,
                             std::uint64_t bins, bool clamp, double fy,
                             double fx, double step) {
#pragma clang fp exceptions(ignore)
  const Binning index(bins);
  if (contiguous_local_dispatch<T>(input, input_stride, output, output_stride,
                                   n, maps, index, clamp, fy, fx, step)) {
    return;
  }
  const bool all_identity = maps[0].identity && maps[1].identity &&
                            maps[2].identity && maps[3].identity;
  if (all_identity) {
    apply_span<T>(input, input_stride, output, output_stride, n, nullptr, bins,
                  true, clamp);
    return;
  }
  // The frequent four-map case has no per-sample flag or identity tests.
  // Zero-contribution dimensions retain their separate guarded path below.
  if (!maps[0].identity && !maps[1].identity && !maps[2].identity &&
      !maps[3].identity && fy > 0.0 && fy < 1.0 && fx > 0.0 &&
      fx + static_cast<double>(static_cast<std::int64_t>(n - 1)) * step < 1.0) {
    const double wy = 1.0 - fy;
    for (std::uint64_t j = 0; j < n; ++j) {
      T x;
      std::memcpy(&x,
                  static_cast<const unsigned char*>(input) +
                      static_cast<std::ptrdiff_t>(j) * input_stride,
                  sizeof(T));
      const auto b = index(domain(x, clamp));
      double v[4];
      for (unsigned c = 0; c < 4; ++c) {
        std::memcpy(
            v + c,
            maps[c].data + static_cast<std::ptrdiff_t>(b) * maps[c].stride, 8);
      }
      const double t =
          fx + static_cast<double>(static_cast<std::int64_t>(j)) * step;
      const T y =
          static_cast<T>(std::clamp(wy * ((1.0 - t) * v[0] + t * v[1]) +
                                        fy * ((1.0 - t) * v[2] + t * v[3]),
                                    0.0, 1.0));
      std::memcpy(static_cast<unsigned char*>(output) +
                      static_cast<std::ptrdiff_t>(j) * output_stride,
                  &y, sizeof(T));
    }
    return;
  }
  const double wy = 1.0 - fy;
  for (std::uint64_t j = 0; j < n; ++j) {
    T x;
    std::memcpy(&x,
                static_cast<const unsigned char*>(input) +
                    static_cast<std::ptrdiff_t>(j) * input_stride,
                sizeof(T));
    const double u = domain(x, clamp);
    double value = u;
    const double t =
        fx + static_cast<double>(static_cast<std::int64_t>(j)) * step;
    // At an exact cell center a zero-weight neighbor has no identity or
    // payload dependency. Copy decisions use contributing transforms only.
    const bool identity =
        all_identity ||
        ((fy == 1 ||
          ((t == 1 || maps[0].identity) && (t == 0 || maps[1].identity))) &&
         (fy == 0 ||
          ((t == 1 || maps[2].identity) && (t == 0 || maps[3].identity))));
    if (!identity) {
      const auto b = index(u);
      double v[4];
      for (unsigned c = 0; c < 4; ++c) {
        if (maps[c].identity || (c >= 2 ? fy == 0 : fy == 1) ||
            (c % 2 ? t == 0 : t == 1)) {
          v[c] = u;
        } else {
          std::memcpy(
              v + c,
              maps[c].data + static_cast<std::ptrdiff_t>(b) * maps[c].stride,
              8);
        }
      }
      value = std::clamp(wy * ((1.0 - t) * v[0] + t * v[1]) +
                             fy * ((1.0 - t) * v[2] + t * v[3]),
                         0.0, 1.0);
    }
    T y = identity && u == x ? x : static_cast<T>(value);
    std::memcpy(static_cast<unsigned char*>(output) +
                    static_cast<std::ptrdiff_t>(j) * output_stride,
                &y, sizeof(T));
  }
}
struct Bracket {
  std::uint64_t lo, hi;
  double fraction;
};
inline double center(std::uint64_t index, std::uint64_t cell,
                     std::uint64_t size) {
  const auto start = index * cell;
  return .5 * static_cast<double>(static_cast<std::int64_t>(
                  start + std::min(size, start + cell)));
}
inline Bracket bracket(std::uint64_t pixel, std::uint64_t cell,
                       std::uint64_t size) {
  const auto cells = (size - 1) / cell + 1;
  const double p = static_cast<double>(static_cast<std::int64_t>(pixel)) + .5;
  if (p <= center(0, cell, size)) {
    return {0, 0, 0};
  }
  if (p >= center(cells - 1, cell, size)) {
    return {cells - 1, cells - 1, 0};
  }
  const auto guess = static_cast<std::uint64_t>(std::max(
      0.0, std::floor(p / static_cast<double>(static_cast<std::int64_t>(cell)) -
                      .5)));
  const auto lo = std::min(guess, cells - 2);
  return {lo, lo + 1,
          (p - center(lo, cell, size)) /
              (center(lo + 1, cell, size) - center(lo, cell, size))};
}
}  // namespace ps::plugin_internal::grade::histogram
