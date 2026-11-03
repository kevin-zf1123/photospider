// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "07-grade/tonal_zones_math.hpp"

extern "C" void photospider_sleef_log2(const double*, double*, std::size_t);
extern "C" void photospider_sleef_log2_clamped(const double*, double*, double,
                                               std::size_t);
extern "C" void photospider_sleef_grade_exp2(const double*, double*,
                                             std::size_t);
extern "C" void photospider_sleef_bilateral_step(const double*, const double*,
                                                 double*, double*, double,
                                                 double, std::size_t);

extern "C" void photospider_sleef_bilateral_weights(const double*,
                                                    const double*, double*,
                                                    double, double,
                                                    std::size_t);
extern "C" void photospider_sleef_weighted_step(const double*, const double*,
                                                const double*, double*, double*,
                                                std::size_t);

namespace ps::plugin_internal::grade::local {
constexpr std::size_t kBlock = 128;
inline bool vector_math() {
#if defined(__aarch64__)
  return true;
#elif defined(__x86_64__)
  return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
  return false;
#endif
}
template <class Poll>
inline void copy_bytes(const void* source, void* target, std::size_t bytes,
                       Poll poll) {
  const auto* input = static_cast<const unsigned char*>(source);
  auto* output = static_cast<unsigned char*>(target);
  while (bytes) {
    poll();
    const auto n = std::min<std::size_t>(bytes, 1U << 20);
    std::memcpy(output, input, n);
    input += n;
    output += n;
    bytes -= n;
  }
}
struct BaseParameters final {
  std::uint64_t radius = 3;
  double sigma_space = 2, sigma_range = 1, epsilon = 0x1p-16;
  double spatial(std::int64_t d) const {
    const double q = static_cast<double>(d) / sigma_space;
    return -.5 * q * q;
  }
};
struct Controls final {
  double pivot = -2, contrast = 1, shadow = 0, highlight = 0, detail = 1;
  tonal::Edges edges{{-8, -4, -1, 2}};
  bool base_identity() const {
    return contrast == 1 && shadow == 0 && highlight == 0;
  }
  bool identity() const { return base_identity() && detail == 1; }
  double exposure(double base, double z) const {
    double e = 0;
    // Zero coefficients bypass otherwise overflowing, unused intermediates.
    if (contrast != 1)
      e = (contrast - 1) * (base - pivot);
    if (shadow != 0 || highlight != 0) {
      const auto w = edges.apply(base);
      e += shadow * w[0] + highlight * w[2];
    }
    if (detail != 1 && z != base)
      e += (detail - 1) * (z - base);
    return e;
  }
};
inline void logarithms(double* values, std::size_t n) {
  if (vector_math()) {
    photospider_sleef_log2(values, values, n);
  } else {
    for (std::size_t i = 0; i < n; ++i)
      values[i] = std::log2(values[i]);
  }
}
template <class T>
inline void log_span(const T* input, double* z, std::size_t n,
                     std::size_t channels, const std::uint64_t* selected,
                     unsigned colors, const std::array<double, 3>& luma,
                     double epsilon) {
  if (channels == 1 && colors == 1 && selected[0] == 0 && vector_math()) {
    if constexpr (std::is_same_v<T, double>) {
      photospider_sleef_log2_clamped(input, z, epsilon, n);
      return;
    } else {
      std::size_t i = 0;
#if defined(__aarch64__)
      for (; i + 2 <= n; i += 2)
        vst1q_f64(z + i, vmaxq_f64(vcvt_f64_f32(vld1_f32(input + i)),
                                   vdupq_n_f64(epsilon)));
#elif defined(__x86_64__)
      for (; i + 2 <= n; i += 2) {
        const auto v = _mm_cvtps_pd(_mm_castsi128_ps(
            _mm_loadl_epi64(reinterpret_cast<const __m128i*>(input + i))));
        _mm_storeu_pd(z + i, _mm_max_pd(v, _mm_set1_pd(epsilon)));
      }
#endif
      for (; i < n; ++i)
        z[i] = std::max(static_cast<double>(input[i]), epsilon);
      logarithms(z, n);
      return;
    }
  }
  for (std::size_t i = 0; i < n; ++i) {
    const auto* p = input + i * channels;
    double y = static_cast<double>(p[selected[0]]);
    if (colors == 3)
      y = (y * luma[0] + static_cast<double>(p[selected[1]]) * luma[1]) +
          static_cast<double>(p[selected[2]]) * luma[2];
    z[i] = std::max(y, epsilon);
  }
  logarithms(z, n);
}
/** One neighbor offset, preserving a left fold for each output coordinate.
 * Accumulating weighted differences gives the same bilateral mean, while a
 * constant neighborhood returns its center exactly. The normalized difference
 * avoids sigma-squared underflow. A zero spatial weight contributes nothing.
 */
inline void scalar_step(const double* center, const double* neighbor,
                        double* numerator, double* denominator, double spatial,
                        double sigma_range, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    const double delta = neighbor[i] == center[i] ? 0 : neighbor[i] - center[i];
    const double q = delta / sigma_range;
    const double w = std::exp(spatial - .5 * q * q);
    numerator[i] += w * delta;
    denominator[i] += w;
  }
}
template <class Poll>
inline void base_span(const double* z, std::size_t height, std::size_t width,
                      std::size_t y, std::size_t x, std::size_t count,
                      const BaseParameters& p, double* output, Poll poll) {
  const double* center = z + y * width + x;
  if (!p.radius || (height == 1 && width == 1)) {
    std::memcpy(output, center, count * sizeof(double));
    return;
  }
  alignas(64) double numerator[kBlock]{}, denominator[kBlock]{};
  const auto radius = static_cast<std::int64_t>(p.radius);
  const auto yy = static_cast<std::int64_t>(y);
  const auto xx = static_cast<std::int64_t>(x);
  const auto end = xx + static_cast<std::int64_t>(count);
  const auto dy0 = std::max(-radius, -yy);
  const auto dy1 = std::min(radius, static_cast<std::int64_t>(height) - yy - 1);
  const auto dx0 = std::max(-radius, -end + 1);
  const auto dx1 = std::min(radius, static_cast<std::int64_t>(width) - xx - 1);
  unsigned until_poll = 0;
  const bool simd = vector_math();
  for (auto dy = dy0; dy <= dy1; ++dy) {
    const double sy = p.spatial(dy);
    for (auto dx = dx0; dx <= dx1; ++dx) {
      if (!(until_poll++ & 15U))
        poll();
      if (dy == 0 && dx == 0) {
        for (std::size_t i = 0; i < count; ++i)
          denominator[i] += 1;
        continue;
      }
      const auto first = std::max(xx, -dx);
      const auto last = std::min(end, static_cast<std::int64_t>(width) - dx);
      if (first >= last)
        continue;
      const double spatial = sy + p.spatial(dx);
      if (!std::isfinite(spatial))
        continue;
      const auto start = static_cast<std::size_t>(first - xx);
      const auto n = static_cast<std::size_t>(last - first);
      const auto* neighbor = z + (yy + dy) * width + first + dx;
      if (simd)
        photospider_sleef_bilateral_step(center + start, neighbor,
                                         numerator + start, denominator + start,
                                         spatial, p.sigma_range, n);
      else
        scalar_step(center + start, neighbor, numerator + start,
                    denominator + start, spatial, p.sigma_range, n);
    }
  }
  for (std::size_t i = 0; i < count; ++i)
    output[i] = center[i] + numerator[i] / denominator[i];
}
struct BaseCache final {
  std::size_t ry = 0, rx = 0, planes = 0, rows = 0, ring_words = 0, words = 0;
};
inline BaseCache base_cache(std::size_t height, std::size_t width,
                            std::size_t out_height, std::size_t out_width,
                            const BaseParameters& p) {
  if (!p.radius || !height || !width || !out_height || !out_width)
    return {};
  BaseCache c;
  c.ry = std::min<std::size_t>(p.radius, height - 1);
  c.rx = std::min<std::size_t>(p.radius, width - 1);
  c.planes = c.ry * (2 * c.rx + 1) + c.rx;
  c.rows = c.ry + 1;
  using Wide = unsigned __int128;
  const Wide count = static_cast<Wide>(out_height) * out_width;
  const Wide ring = static_cast<Wide>(c.rows) * c.planes * width;
  const Wide words = ring + 2 * static_cast<Wide>(out_width);
  if (!c.planes || count < 256 || words > (1U << 20))
    return {};
#if defined(__x86_64__)
  if (height == 1 && words > (1U << 16))
    return {};
#endif
  const Wide generated =
      (static_cast<Wide>(out_height) + c.ry) * width * c.planes;
  const Wide direct = count * ((2 * c.ry + 1) * (2 * c.rx + 1) - 1);
  if (generated * 4 > direct * 3)
    return {};
  c.ring_words = static_cast<std::size_t>(ring);
  c.words = static_cast<std::size_t>(words);
  return c;
}
/** Reuse symmetric weights; each output still has its original dy/dx fold.
 * Ring rows hold only forward offsets. Input views never alias this workspace.
 */
template <class Poll, class Emit>
inline void cached_base(const double* z, std::size_t height, std::size_t width,
                        std::size_t oy, std::size_t ox, std::size_t oh,
                        std::size_t ow, const BaseParameters& p,
                        const BaseCache& c, double* workspace, Poll poll,
                        Emit emit) {
  const auto ry = static_cast<std::int64_t>(c.ry);
  const auto rx = static_cast<std::int64_t>(c.rx);
  const auto plane = [&](std::int64_t dy, std::int64_t dx) {
    return dy ? static_cast<std::size_t>((dy - 1) * (2 * rx + 1) + dx + rx)
              : c.ry * (2 * c.rx + 1) + static_cast<std::size_t>(dx - 1);
  };
  const auto slot = [&](std::size_t y, std::size_t k) {
    return workspace + ((y % c.rows) * c.planes + k) * width;
  };
  auto* numerator = workspace + c.ring_words;
  auto* denominator = numerator + ow;
  const bool simd = vector_math();
  const auto begin_y = oy > c.ry ? oy - c.ry : 0;
  for (std::size_t y = begin_y; y < oy + oh; ++y) {
    for (std::int64_t dy = 0; dy <= ry && y + dy < height; ++dy) {
      const double sy = p.spatial(dy);
      for (std::int64_t dx = dy ? -rx : 1; dx <= rx; ++dx) {
        const auto first =
            static_cast<std::size_t>(std::max<std::int64_t>(0, -dx));
        const auto last = std::min<std::int64_t>(
            width, static_cast<std::int64_t>(width) - dx);
        const double spatial = sy + p.spatial(dx);
        auto* w = slot(y, plane(dy, dx));
        for (std::size_t x = first; x < static_cast<std::size_t>(last);) {
          poll();
          const auto n = std::min<std::size_t>(4096, last - x);
          const auto* a = z + y * width + x;
          const auto* b = z + (y + dy) * width + x + dx;
          if (!std::isfinite(spatial)) {
            std::fill_n(w + x, n, 0.);
          } else if (simd) {
            photospider_sleef_bilateral_weights(a, b, w + x, spatial,
                                                p.sigma_range, n);
          } else {
            for (std::size_t i = 0; i < n; ++i) {
              const double delta = b[i] == a[i] ? 0 : b[i] - a[i];
              const double q = delta / p.sigma_range;
              w[x + i] = std::exp(spatial - .5 * q * q);
            }
          }
          x += n;
        }
      }
    }
    if (y < oy)
      continue;
    std::fill_n(numerator, ow, 0.);
    std::fill_n(denominator, ow, 0.);
    const auto dy0 = std::max(-ry, -static_cast<std::int64_t>(y));
    const auto dy1 = std::min(ry, static_cast<std::int64_t>(height - y - 1));
    for (auto dy = dy0; dy <= dy1; ++dy) {
      for (auto dx = -rx; dx <= rx; ++dx) {
        poll();
        if (!dy && !dx) {
          for (std::size_t i = 0; i < ow; ++i)
            denominator[i] += 1;
          continue;
        }
        // Match direct evaluation: a zero spatial contribution does not read
        // the cached weight or form a possibly nonfinite neighbor difference.
        if (!std::isfinite(p.spatial(dy) + p.spatial(dx)))
          continue;
        const auto first = std::max<std::int64_t>(ox, -dx);
        const auto last = std::min<std::int64_t>(
            ox + ow, static_cast<std::int64_t>(width) - dx);
        if (first >= last)
          continue;
        const bool reverse = dy < 0 || (!dy && dx < 0);
        const auto* w = reverse ? slot(y + dy, plane(-dy, -dx)) + first + dx
                                : slot(y, plane(dy, dx)) + first;
        const auto* a = z + y * width + first;
        const auto* b = z + (y + dy) * width + first + dx;
        const auto offset = static_cast<std::size_t>(first) - ox;
        const auto total = static_cast<std::size_t>(last - first);
        for (std::size_t i = 0; i < total;) {
          poll();
          const auto n = std::min<std::size_t>(4096, total - i);
          if (simd) {
            photospider_sleef_weighted_step(a + i, b + i, w + i,
                                            numerator + offset + i,
                                            denominator + offset + i, n);
          } else {
            for (std::size_t j = i; j < i + n; ++j) {
              const double delta = b[j] == a[j] ? 0 : b[j] - a[j];
              numerator[offset + j] += w[j] * delta;
              denominator[offset + j] += w[j];
            }
          }
          i += n;
        }
      }
    }
    const auto* center = z + y * width + ox;
    for (std::size_t i = 0; i < ow; ++i)
      numerator[i] = center[i] + numerator[i] / denominator[i];
    for (std::size_t i = 0; i < ow; i += kBlock) {
      poll();
      emit(y, ox + i, std::min(kBlock, ow - i), numerator + i);
    }
  }
}

inline void exposure_values(const double* base, const double* z, double* e,
                            std::size_t n, const Controls& control) {
  std::size_t i = 0;
#if defined(__aarch64__) || defined(__x86_64__)
  if (tonal::regular(control.edges)) {
    for (; i + 2 <= n; i += 2) {
#if defined(__aarch64__)
      const auto b = vld1q_f64(base + i);
      auto v = vdupq_n_f64(0);
      if (control.contrast != 1)
        v = vmulq_n_f64(vsubq_f64(b, vdupq_n_f64(control.pivot)),
                        control.contrast - 1);
      if (control.shadow != 0) {
        const auto w = vsubq_f64(
            vdupq_n_f64(1), tonal::smooth_packet(b, control.edges.values[0],
                                                 control.edges.values[1]));
        v = vaddq_f64(v, vmulq_n_f64(w, control.shadow));
      }
      if (control.highlight != 0)
        v = vaddq_f64(
            v, vmulq_n_f64(tonal::smooth_packet(b, control.edges.values[2],
                                                control.edges.values[3]),
                           control.highlight));
      if (control.detail != 1) {
        const auto log = vld1q_f64(z + i);
        const auto delta = vbslq_f64(native_simd::equal(log, b), vdupq_n_f64(0),
                                     vsubq_f64(log, b));
        v = vaddq_f64(v, vmulq_n_f64(delta, control.detail - 1));
      }
      vst1q_f64(e + i, v);
#else
      const auto b = _mm_loadu_pd(base + i);
      auto v = _mm_setzero_pd();
      if (control.contrast != 1)
        v = _mm_mul_pd(_mm_sub_pd(b, _mm_set1_pd(control.pivot)),
                       _mm_set1_pd(control.contrast - 1));
      if (control.shadow != 0) {
        const auto w = _mm_sub_pd(
            _mm_set1_pd(1), tonal::smooth_packet(b, control.edges.values[0],
                                                 control.edges.values[1]));
        v = _mm_add_pd(v, _mm_mul_pd(w, _mm_set1_pd(control.shadow)));
      }
      if (control.highlight != 0)
        v = _mm_add_pd(
            v, _mm_mul_pd(tonal::smooth_packet(b, control.edges.values[2],
                                               control.edges.values[3]),
                          _mm_set1_pd(control.highlight)));
      if (control.detail != 1) {
        const auto log = _mm_loadu_pd(z + i);
        const auto delta =
            _mm_andnot_pd(_mm_cmpeq_pd(log, b), _mm_sub_pd(log, b));
        v = _mm_add_pd(v, _mm_mul_pd(delta, _mm_set1_pd(control.detail - 1)));
      }
      _mm_storeu_pd(e + i, v);
#endif
    }
  }
#endif
#if defined(__aarch64__) || defined(__x86_64__)
  if (i < n && tonal::regular(control.edges)) {
    // Duplicate only the requested tail sample. Exposure rounding and the
    // exact-zero copy decision must not depend on packet or tile boundaries.
    const double b[2]{base[i], base[i]};
    const double detail[2]{z ? z[i] : base[i], z ? z[i] : base[i]};
    double tail[2];
    exposure_values(b, detail, tail, 2, control);
    e[i++] = tail[0];
  }
#endif
  for (; i < n; ++i)
    e[i] = control.exposure(base[i], z ? z[i] : base[i]);
}
inline void exposures(const double* base, const double* z, double* e, double* g,
                      std::size_t n, const Controls& control) {
  exposure_values(base, z, e, n, control);
  if (vector_math()) {
    photospider_sleef_grade_exp2(e, g, n);
  } else {
    for (std::size_t i = 0; i < n; ++i)
      g[i] = std::exp2(e[i]);
  }
}
/** Keep native gain-then-multiply staging at the destination overflow edge.
 * Away from that edge, the common native tolerance admits the vector gain.
 */
template <class T>
inline double grade_product(double source, double exposure, double gain) {
  const double value = source * gain;
  constexpr double limit =
      static_cast<double>(std::numeric_limits<T>::max()) * (1 - 0x1p-30);
  return std::abs(value) >= limit ? source * std::exp2(exposure) : value;
}
#if defined(__aarch64__)
template <class T>
inline float64x2_t grade_product_packet(float64x2_t source, const double* e,
                                        const double* gain) {
  const auto value = vmulq_f64(source, vld1q_f64(gain));
  constexpr double limit =
      static_cast<double>(std::numeric_limits<T>::max()) * (1 - 0x1p-30);
  const auto edge =
      native_simd::greater_equal(vabsq_f64(value), vdupq_n_f64(limit));
  if (!(vgetq_lane_u64(edge, 0) | vgetq_lane_u64(edge, 1)))
    return value;
  double x[2], result[2];
  vst1q_f64(x, source);
  for (unsigned i = 0; i < 2; ++i)
    result[i] = grade_product<T>(x[i], e[i], gain[i]);
  return vld1q_f64(result);
}
#endif
template <class T, unsigned C>
inline void apply_fixed(const T* input, T* output, const double* e,
                        const double* gain, std::size_t n) {
  std::size_t i = 0;
#if defined(__aarch64__)
  for (; i + 2 <= n; i += 2) {
    const auto identity = native_simd::equal(vld1q_f64(e + i), vdupq_n_f64(0));
    if constexpr (sizeof(T) == 4) {
      const auto scale = [&](float32x2_t x) {
        return vbsl_f32(vmovn_u64(identity), x,
                        vcvt_f32_f64(grade_product_packet<T>(vcvt_f64_f32(x),
                                                             e + i, gain + i)));
      };
      if constexpr (C == 1) {
        vst1_f32(output + i, scale(vld1_f32(input + i)));
      } else if constexpr (C == 3) {
        auto v = vld3_f32(input + 3 * i);
        for (unsigned c = 0; c < 3; ++c)
          v.val[c] = scale(v.val[c]);
        vst3_f32(output + 3 * i, v);
      } else {
        auto v = vld4_f32(input + 4 * i);
        for (unsigned c = 0; c < 3; ++c)
          v.val[c] = scale(v.val[c]);
        vst4_f32(output + 4 * i, v);
      }
    } else {
      const auto scale = [&](float64x2_t x) {
        return vbslq_f64(identity, x,
                         grade_product_packet<T>(x, e + i, gain + i));
      };
      if constexpr (C == 1) {
        vst1q_f64(output + i, scale(vld1q_f64(input + i)));
      } else if constexpr (C == 3) {
        auto v = vld3q_f64(input + 3 * i);
        for (unsigned c = 0; c < 3; ++c)
          v.val[c] = scale(v.val[c]);
        vst3q_f64(output + 3 * i, v);
      } else {
        auto v = vld4q_f64(input + 4 * i);
        for (unsigned c = 0; c < 3; ++c)
          v.val[c] = scale(v.val[c]);
        vst4q_f64(output + 4 * i, v);
      }
    }
  }
#endif
  for (; i < n; ++i) {
    if (e[i] == 0) {
      std::memcpy(output + C * i, input + C * i, C * sizeof(T));
      continue;
    }
    for (unsigned c = 0; c < (C == 1 ? 1U : 3U); ++c)
      output[C * i + c] =
          static_cast<T>(grade_product<T>(input[C * i + c], e[i], gain[i]));
    if constexpr (C == 4)
      std::memcpy(output + C * i + 3, input + C * i + 3, sizeof(T));
  }
}
template <class T>
inline void apply_span(const T* input, T* output, const double* base,
                       const double* z, std::size_t n, unsigned channels,
                       const std::uint64_t* selected, unsigned colors,
                       const Controls& control) {
  if (control.identity()) {
    std::memcpy(output, input, n * channels * sizeof(T));
    return;
  }
  alignas(64) double e[kBlock], gain[kBlock];
  exposures(base, z, e, gain, n, control);
  if (channels == 1 && selected[0] == 0) {
    apply_fixed<T, 1>(input, output, e, gain, n);
    return;
  }
  if (colors == 3 && selected[0] == 0 && selected[1] == 1 && selected[2] == 2) {
    if (channels == 3) {
      apply_fixed<T, 3>(input, output, e, gain, n);
      return;
    }
    if (channels == 4) {
      apply_fixed<T, 4>(input, output, e, gain, n);
      return;
    }
  }
  const bool all = channels == colors;
  for (std::size_t i = 0; i < n; ++i) {
    if (!all || e[i] == 0)
      std::memcpy(output + i * channels, input + i * channels,
                  channels * sizeof(T));
    if (e[i] == 0)
      continue;
    for (unsigned c = 0; c < colors; ++c) {
      const auto at = i * channels + selected[c];
      output[at] = static_cast<T>(grade_product<T>(input[at], e[i], gain[i]));
    }
  }
}
}  // namespace ps::plugin_internal::grade::local
