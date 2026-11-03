// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#if defined(__aarch64__)
#include <arm_neon.h>

#include "01-numeric/fast_native_simd.hpp"
#elif defined(__x86_64__)
#include <emmintrin.h>
#endif

#if defined(__x86_64__)
extern "C" void photospider_sleef_exp2(const double* input, double* output,
                                       std::size_t count);
#endif

namespace ps::plugin_internal::grade::tonal {
/** Invocation-local edges. Compare endpoints before division so endpoint masks
 * remain exact even when an edge difference overflows or is subnormal.
 */
struct Edges final {
  std::array<double, 4> values{};
  static double smooth(double x, double lo, double hi) {
    if (x <= lo)
      return 0;
    if (x >= hi)
      return 1;
    const double width = hi - lo;
    const double t = std::isinf(width)
                         ? (x * .5 - lo * .5) / (hi * .5 - lo * .5)
                         : (x - lo) / width;
    return (t * t) * (3 - 2 * t);
  }
  std::array<double, 3> apply(double x) const {
    const auto a = smooth(x, values[0], values[1]);
    const auto b = smooth(x, values[2], values[3]);
    return {1 - a, a - b, b};
  }
};
/** Dynamic controls are loaded afresh for each invocation. Accumulation and
 * normalization use Float64; an identity copies the original source bits.
 */
struct Controls final {
  std::array<double, 3> ev{}, offsets{};
  bool copies = true;
  void classify() {
    copies = true;
    for (unsigned z = 0; z < 3; ++z)
      copies = copies && ev[z] == 0 && offsets[z] == 0;
  }
  std::array<double, 2> apply(double a, double b, double c) const {
    const auto sum = (a + b) + c;
    return {((a * ev[0] + b * ev[1]) + c * ev[2]) / sum,
            ((a * offsets[0] + b * offsets[1]) + c * offsets[2]) / sum};
  }
};
// The reciprocal path is restricted to normal, finite widths and reciprocals.
// Extreme/subnormal edges retain direct division and exact endpoint branches.
inline bool regular(const Edges& e) {
  for (unsigned j : {0U, 2U}) {
    const auto width = e.values[j + 1] - e.values[j];
    if (!std::isnormal(width) || !std::isnormal(1 / width))
      return false;
  }
  return true;
}
#if defined(__aarch64__)
inline float64x2_t smooth_packet(float64x2_t x, double lo, double hi) {
  auto t = vmulq_n_f64(vsubq_f64(x, vdupq_n_f64(lo)), 1 / (hi - lo));
  t = vmaxq_f64(vdupq_n_f64(0), vminq_f64(t, vdupq_n_f64(1)));
  t = vbslq_f64(native_simd::greater_equal(x, vdupq_n_f64(hi)), vdupq_n_f64(1),
                t);
  t = vbslq_f64(native_simd::less_equal(x, vdupq_n_f64(lo)), vdupq_n_f64(0), t);
  return vmulq_f64(vmulq_f64(t, t),
                   vsubq_f64(vdupq_n_f64(3), vmulq_n_f64(t, 2)));
}
template <class T>
__attribute__((noinline)) std::size_t masks_packet(const T* x, T* y,
                                                   std::size_t n,
                                                   const Edges& e) {
  std::size_t i = 0;
  for (; i + 2 <= n; i += 2) {
    float64x2_t v;
    if constexpr (sizeof(T) == 4)
      v = vcvt_f64_f32(vld1_f32(x + i));
    else
      v = vld1q_f64(x + i);
    const auto a = smooth_packet(v, e.values[0], e.values[1]);
    const auto b = smooth_packet(v, e.values[2], e.values[3]);
    const auto s = vsubq_f64(vdupq_n_f64(1), a);
    const auto m = vsubq_f64(a, b);
    if constexpr (sizeof(T) == 4) {
      const float32x2x3_t out{
          {vcvt_f32_f64(s), vcvt_f32_f64(m), vcvt_f32_f64(b)}};
      vst3_f32(y + 3 * i, out);
    } else {
      const float64x2x3_t out{{s, m, b}};
      vst3q_f64(y + 3 * i, out);
    }
  }
  return i;
}
#elif defined(__x86_64__)
inline __m128d smooth_packet(__m128d x, double lo, double hi) {
  auto t =
      _mm_mul_pd(_mm_sub_pd(x, _mm_set1_pd(lo)), _mm_set1_pd(1 / (hi - lo)));
  t = _mm_max_pd(_mm_setzero_pd(), _mm_min_pd(t, _mm_set1_pd(1)));
  const auto upper = _mm_cmpge_pd(x, _mm_set1_pd(hi));
  t = _mm_or_pd(_mm_and_pd(upper, _mm_set1_pd(1)), _mm_andnot_pd(upper, t));
  t = _mm_andnot_pd(_mm_cmple_pd(x, _mm_set1_pd(lo)), t);
  return _mm_mul_pd(_mm_mul_pd(t, t),
                    _mm_sub_pd(_mm_set1_pd(3), _mm_add_pd(t, t)));
}
template <class T>
__attribute__((noinline)) std::size_t masks_packet(const T* x, T* y,
                                                   std::size_t n,
                                                   const Edges& e) {
  std::size_t i = 0;
  for (; i + 2 <= n; i += 2) {
    __m128d v;
    if constexpr (sizeof(T) == 4)
      v = _mm_cvtps_pd(_mm_loadl_pi(_mm_setzero_ps(),
                                    reinterpret_cast<const __m64*>(x + i)));
    else
      v = _mm_loadu_pd(x + i);
    const auto a = smooth_packet(v, e.values[0], e.values[1]);
    const auto b = smooth_packet(v, e.values[2], e.values[3]);
    const auto s = _mm_sub_pd(_mm_set1_pd(1), a), m = _mm_sub_pd(a, b);
    const __m128d pairs[]{_mm_unpacklo_pd(s, m), _mm_shuffle_pd(b, s, 2),
                          _mm_unpackhi_pd(m, b)};
    for (unsigned j = 0; j < 3; ++j) {
      if constexpr (sizeof(T) == 4)
        _mm_storel_pi(reinterpret_cast<__m64*>(y + 3 * i + 2 * j),
                      _mm_cvtpd_ps(pairs[j]));
      else
        _mm_storeu_pd(y + 3 * i + 2 * j, pairs[j]);
    }
  }
  return i;
}
#endif
template <class T>
inline void masks_span(const T* x, T* y, std::size_t n, const Edges& edges) {
  std::size_t i = 0;
#if defined(__aarch64__) || defined(__x86_64__)
  if (regular(edges))
    i = masks_packet(x, y, n, edges);
#endif
  for (; i < n; ++i) {
    const auto v = edges.apply(x[i]);
    for (unsigned c = 0; c < 3; ++c)
      y[3 * i + c] = static_cast<T>(v[c]);
  }
}
template <class Z>
inline void control_packet(const Z* w, const Controls& control, double* ev,
                           double* offsets) {
#if defined(__aarch64__)
  float64x2_t a, b, c;
  if constexpr (sizeof(Z) == 4) {
    const auto v = vld3_f32(w);
    a = vcvt_f64_f32(v.val[0]);
    b = vcvt_f64_f32(v.val[1]);
    c = vcvt_f64_f32(v.val[2]);
  } else {
    const auto v = vld3q_f64(w);
    a = v.val[0];
    b = v.val[1];
    c = v.val[2];
  }
  const auto sum = vaddq_f64(vaddq_f64(a, b), c);
  const auto weighted = [&](const auto& coefficients) {
    return vdivq_f64(vaddq_f64(vaddq_f64(vmulq_n_f64(a, coefficients[0]),
                                         vmulq_n_f64(b, coefficients[1])),
                               vmulq_n_f64(c, coefficients[2])),
                     sum);
  };
  vst1q_f64(ev, weighted(control.ev));
  vst1q_f64(offsets, weighted(control.offsets));
#elif defined(__x86_64__)
  const auto a = _mm_set_pd(w[3], w[0]);
  const auto b = _mm_set_pd(w[4], w[1]);
  const auto c = _mm_set_pd(w[5], w[2]);
  const auto sum = _mm_add_pd(_mm_add_pd(a, b), c);
  const auto weighted = [&](const auto& coefficients) {
    return _mm_div_pd(
        _mm_add_pd(_mm_add_pd(_mm_mul_pd(a, _mm_set1_pd(coefficients[0])),
                              _mm_mul_pd(b, _mm_set1_pd(coefficients[1]))),
                   _mm_mul_pd(c, _mm_set1_pd(coefficients[2]))),
        sum);
  };
  _mm_storeu_pd(ev, weighted(control.ev));
  _mm_storeu_pd(offsets, weighted(control.offsets));
#else
  for (unsigned i = 0; i < 2; ++i) {
    const auto v = control.apply(w[3 * i], w[3 * i + 1], w[3 * i + 2]);
    ev[i] = v[0];
    offsets[i] = v[1];
  }
#endif
}
/** Admit a vector gain only when its error cannot dominate the final result.
 * The conservative 2e-14 gain budget also covers the native multiply/add
 * difference. A near-overflow product must fall back before an offset can
 * conceal it; this preserves the scalar gain-then-multiply IEEE staging.
 * Fall back to library exp2 outside [-1022,1023], for a nonfinite result,
 * within 2^-30 of the product/output type limits, or when the gain error
 * estimate exceeds one eighth of the output comparison tolerance.
 */
template <class T>
inline double guarded_gray(double x, double ev, double offset, double gain) {
  constexpr double atol = sizeof(T) == 4 ? 2e-6 : 2e-12;
  constexpr double rtol = sizeof(T) == 4 ? 2e-5 : 2e-11;
  constexpr double product_limit =
      std::numeric_limits<double>::max() * (1 - 0x1p-30);
  constexpr double output_limit =
      static_cast<double>(std::numeric_limits<T>::max()) * (1 - 0x1p-30);
  const double product = x * gain, value = product + offset;
  if (!(ev >= -1022 && ev <= 1023) || !std::isfinite(value) ||
      std::abs(product) >= product_limit || std::abs(value) >= output_limit ||
      std::abs(product) * 2e-14 > .125 * (atol + rtol * std::abs(value)))
    return x * std::exp2(ev) + offset;
  return value;
}
#if defined(__x86_64__)
// Apply the same admission bounds to two lanes without scalar branch overhead.
template <class T>
inline bool guarded_gray_pair(const T* x, const double* ev,
                              const double* offsets, const double* gain, T* y) {
  constexpr double atol = sizeof(T) == 4 ? 2e-6 : 2e-12;
  constexpr double rtol = sizeof(T) == 4 ? 2e-5 : 2e-11;
  constexpr double product_limit =
      std::numeric_limits<double>::max() * (1 - 0x1p-30);
  constexpr double output_limit =
      static_cast<double>(std::numeric_limits<T>::max()) * (1 - 0x1p-30);
  __m128d source;
  if constexpr (sizeof(T) == 4)
    source = _mm_cvtps_pd(
        _mm_loadl_pi(_mm_setzero_ps(), reinterpret_cast<const __m64*>(x)));
  else
    source = _mm_loadu_pd(x);
  const auto e = _mm_loadu_pd(ev), o = _mm_loadu_pd(offsets);
  const auto product = _mm_mul_pd(source, _mm_loadu_pd(gain));
  const auto value = _mm_add_pd(product, o);
  const auto ap = _mm_andnot_pd(_mm_set1_pd(-0.0), product);
  const auto av = _mm_andnot_pd(_mm_set1_pd(-0.0), value);
  auto admit = _mm_and_pd(_mm_cmpge_pd(e, _mm_set1_pd(-1022)),
                          _mm_cmple_pd(e, _mm_set1_pd(1023)));
  admit = _mm_and_pd(admit, _mm_cmplt_pd(ap, _mm_set1_pd(product_limit)));
  admit = _mm_and_pd(admit, _mm_cmplt_pd(av, _mm_set1_pd(output_limit)));
  const auto bound = _mm_mul_pd(ap, _mm_set1_pd(2e-14));
  const auto budget = _mm_mul_pd(
      _mm_add_pd(_mm_set1_pd(atol), _mm_mul_pd(_mm_set1_pd(rtol), av)),
      _mm_set1_pd(.125));
  admit = _mm_and_pd(admit, _mm_cmple_pd(bound, budget));
  const auto identity = _mm_and_pd(_mm_cmpeq_pd(e, _mm_setzero_pd()),
                                   _mm_cmpeq_pd(o, _mm_setzero_pd()));
  admit = _mm_andnot_pd(identity, admit);
  if (_mm_movemask_pd(admit) != 3)
    return false;
  if constexpr (sizeof(T) == 4)
    _mm_storel_pi(reinterpret_cast<__m64*>(y), _mm_cvtpd_ps(value));
  else
    _mm_storeu_pd(y, value);
  return true;
}
#endif
/** Contiguous Gray spans use private SLEEF batches of at most 256 samples on
 * x86-64. ISA admission precedes every call into its AVX2/FMA object.
 * Normalization retains the Float64 order in Controls::apply. Guarded output
 * evaluation retains library exp2 for extreme gains and numerical cancellation;
 * other layouts use library exp2 directly.
 */
template <class T, class Z>
inline bool gray_batch(const T* x, const Z* w, T* y, std::size_t n,
                       const Controls& controls) {
#if defined(__x86_64__)
  if (n < 16 || !__builtin_cpu_supports("avx2") ||
      !__builtin_cpu_supports("fma"))
    return false;
  alignas(32) std::array<double, 256> ev{}, offsets{}, gain{};
  for (std::size_t first = 0; first < n;) {
    const auto count = std::min<std::size_t>(ev.size(), n - first);
    std::size_t i = 0;
    for (; i + 2 <= count; i += 2)
      control_packet(w + 3 * (first + i), controls, ev.data() + i,
                     offsets.data() + i);
    if (i < count) {
      const auto v = controls.apply(w[3 * (first + i)], w[3 * (first + i) + 1],
                                    w[3 * (first + i) + 2]);
      ev[i] = v[0];
      offsets[i] = v[1];
    }
    photospider_sleef_exp2(ev.data(), gain.data(), count);
    for (i = 0; i < count;) {
      if (i + 2 <= count &&
          guarded_gray_pair(x + first + i, ev.data() + i, offsets.data() + i,
                            gain.data() + i, y + first + i)) {
        i += 2;
        continue;
      }
      if (ev[i] == 0 && offsets[i] == 0)
        std::memcpy(y + first + i, x + first + i, sizeof(T));
      else
        y[first + i] = static_cast<T>(guarded_gray<T>(
            static_cast<double>(x[first + i]), ev[i], offsets[i], gain[i]));
      ++i;
    }
    first += count;
  }
  return true;
#else
  return false;
#endif
}
template <class T, class Z, unsigned C>
inline void grade_fixed(const T* x, const Z* w, T* y, std::size_t n,
                        std::uint64_t selected, const Controls& controls) {
  const bool all = selected == ((UINT64_C(1) << C) - 1);
  std::size_t i = 0;
  for (; i + 2 <= n; i += 2) {
    double ev[2], offsets[2];
    control_packet(w + 3 * i, controls, ev, offsets);
    for (unsigned lane = 0; lane < 2; ++lane) {
      const auto j = i + lane;
      if (ev[lane] == 0 && offsets[lane] == 0) {
        std::memcpy(y + C * j, x + C * j, C * sizeof(T));
        continue;
      }
      const double gain = std::exp2(ev[lane]);
      for (unsigned c = 0; c < C; ++c) {
        if (all || (selected & (UINT64_C(1) << c)))
          y[C * j + c] = static_cast<T>(
              static_cast<double>(x[C * j + c]) * gain + offsets[lane]);
        else
          std::memcpy(y + C * j + c, x + C * j + c, sizeof(T));
      }
    }
  }
  for (; i < n; ++i) {
    const auto v = controls.apply(w[3 * i], w[3 * i + 1], w[3 * i + 2]);
    if (v[0] == 0 && v[1] == 0) {
      std::memcpy(y + C * i, x + C * i, C * sizeof(T));
      continue;
    }
    const double gain = std::exp2(v[0]);
    for (unsigned c = 0; c < C; ++c) {
      if (all || (selected & (UINT64_C(1) << c)))
        y[C * i + c] =
            static_cast<T>(static_cast<double>(x[C * i + c]) * gain + v[1]);
      else
        std::memcpy(y + C * i + c, x + C * i + c, sizeof(T));
    }
  }
}
template <class T, class Z>
inline void grade_span(const T* x, const Z* w, T* y, std::size_t n,
                       unsigned channels, std::uint64_t selected,
                       const Controls& controls) {
  if (controls.copies) {
    std::memcpy(y, x, n * channels * sizeof(T));
    return;
  }
  if (channels == 1) {
    if ((selected & 1U) && gray_batch(x, w, y, n, controls))
      return;
    grade_fixed<T, Z, 1>(x, w, y, n, selected, controls);
    return;
  }
  if (channels == 3) {
    grade_fixed<T, Z, 3>(x, w, y, n, selected, controls);
    return;
  }
  if (channels == 4) {
    grade_fixed<T, Z, 4>(x, w, y, n, selected, controls);
    return;
  }
  for (std::size_t i = 0; i < n; ++i) {
    const auto v = controls.apply(w[3 * i], w[3 * i + 1], w[3 * i + 2]);
    const bool identity = v[0] == 0 && v[1] == 0;
    const double gain = identity ? 1 : std::exp2(v[0]);
    for (unsigned c = 0; c < channels; ++c) {
      if (!(selected & (UINT64_C(1) << c)) || identity)
        std::memcpy(y + i * channels + c, x + i * channels + c, sizeof(T));
      else
        y[i * channels + c] = static_cast<T>(
            static_cast<double>(x[i * channels + c]) * gain + v[1]);
    }
  }
}
}  // namespace ps::plugin_internal::grade::tonal
