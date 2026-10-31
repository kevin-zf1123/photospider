// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__)
#include <immintrin.h>
#endif

namespace ps::plugin_internal::grade {

// Keep the power-of-two scale separate at extreme EVs. This avoids an
// overflowing gain turning a finite mathematical product into infinity.
// The ordinary path has one binary64 multiplication and one output conversion.
inline double exposed(double x, double ev) {
  if (ev == 0 || x == 0)
    return x;
  if (ev >= -1022 && ev <= 1023)
    return x * std::exp2(ev);
  if (ev > 2200)
    return std::copysign(std::numeric_limits<double>::infinity(), x);
  if (ev < -2200)
    return std::copysign(0.0, x);
  const double integral = std::floor(ev);
  int exponent = 0;
  const double mantissa = std::frexp(x, &exponent);
  return std::scalbn(mantissa * std::exp2(ev - integral),
                     exponent + static_cast<int>(integral));
}

template <class T>
inline void scalar_pixel(const T* source, T* destination, double ev,
                         unsigned channels, unsigned mask) {
  for (unsigned c = 0; c < channels; ++c) {
    if (!(mask & (1U << c)) || ev == 0) {
      std::memcpy(destination + c, source + c, sizeof(T));
    } else {
      destination[c] = static_cast<T>(exposed(source[c], ev));
    }
  }
}

// Chebyshev interpolation of exp(z) on [-ln(2)/2, ln(2)/2].
// Degree 6/8 is selected by the output dtype; all arithmetic is binary64.
// The interpolation remainder bounds are 3.9e-9/1.7e-12 relative, including
// conservative room for coefficient rounding. Identity lanes copy source bits.
// NOLINTBEGIN(whitespace/indent_namespace)
inline constexpr double kExpPolynomial6[] = {1.0,
                                             1.000000037716214,
                                             0.5000000047117757,
                                             0.16666415514653277,
                                             0.04166635289677516,
                                             0.008375126398153335,
                                             0.0013941108433972674};
inline constexpr double kExpPolynomial8[] = {1.0,
                                             0.9999999999797852,
                                             0.49999999999797934,
                                             0.16666666891045775,
                                             0.041666666890957,
                                             0.008333266097949614,
                                             0.0013888821677630362,
                                             0.00019915866926782682,
                                             2.4876164022625967e-05};

// NOLINTEND

#if defined(__aarch64__)
inline uint64x2_t neon_abs_bits(float64x2_t value) {
  return vandq_u64(vreinterpretq_u64_f64(value),
                   vdupq_n_u64(UINT64_C(0x7fffffffffffffff)));
}

template <class T>
inline float64x2_t neon_exp2(float64x2_t ev) {
  const auto n = vrndnq_f64(ev);
  const auto z =
      vmulq_n_f64(vsubq_f64(ev, n), 0.693147180559945309417232121458);
  const auto exponent =
      vshlq_n_s64(vaddq_s64(vcvtq_s64_f64(n), vdupq_n_s64(1023)), 52);
  constexpr unsigned degree = sizeof(T) == 4 ? 6 : 8;
  const auto* c = sizeof(T) == 4 ? kExpPolynomial6 : kExpPolynomial8;
  auto p = vdupq_n_f64(c[degree]);
  for (unsigned i = degree; i-- > 0;)
    p = vfmaq_f64(vdupq_n_f64(c[i]), z, p);
  return vmulq_f64(p, vreinterpretq_f64_s64(exponent));
}

template <class T>
inline float64x2_t neon_load(const T* p) {
  if constexpr (std::is_same_v<T, float>)
    return vcvt_f64_f32(vld1_f32(p));
  else
    return vld1q_f64(p);
}
template <class T>
inline void neon_save(T* p, float64x2_t value) {
  if constexpr (std::is_same_v<T, float>)
    vst1_f32(p, vcvt_f32_f64(value));
  else
    vst1q_f64(p, value);
}
template <class T>
inline void neon_uniform(const T* source, T* destination, std::size_t count,
                         double gain) {
  const auto g = vdupq_n_f64(gain);
  std::size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    const auto a = vmulq_f64(neon_load(source + i), g);
    const auto b = vmulq_f64(neon_load(source + i + 2), g);
    const auto c = vmulq_f64(neon_load(source + i + 4), g);
    const auto d = vmulq_f64(neon_load(source + i + 6), g);
    neon_save(destination + i, a);
    neon_save(destination + i + 2, b);
    neon_save(destination + i + 4, c);
    neon_save(destination + i + 6, d);
  }
  for (; i + 2 <= count; i += 2)
    neon_save(destination + i, vmulq_f64(neon_load(source + i), g));
  for (; i < count; ++i)
    destination[i] = static_cast<T>(static_cast<double>(source[i]) * gain);
}

template <class T>
inline void neon_masked_uniform(const T* source, T* destination,
                                std::size_t count, double gain, unsigned mask) {
  const auto g = vdupq_n_f64(gain);
  const auto m0 = vsetq_lane_u64(-(std::uint64_t)((mask >> 1) & 1),
                                 vdupq_n_u64(-(std::uint64_t)(mask & 1)), 1);
  const auto m1 =
      vsetq_lane_u64(-(std::uint64_t)((mask >> 3) & 1),
                     vdupq_n_u64(-(std::uint64_t)((mask >> 2) & 1)), 1);
  for (std::size_t i = 0; i < count; ++i) {
    if constexpr (std::is_same_v<T, float>) {
      const auto x = vld1q_f32(source + 4 * i);
      const auto a = vcvt_f32_f64(vmulq_f64(vcvt_f64_f32(vget_low_f32(x)), g));
      const auto b = vcvt_f32_f64(vmulq_f64(vcvt_f64_f32(vget_high_f32(x)), g));
      const auto bits = vcombine_u32(vmovn_u64(m0), vmovn_u64(m1));
      vst1q_f32(destination + 4 * i, vbslq_f32(bits, vcombine_f32(a, b), x));
    } else {
      const auto a = vld1q_f64(source + 4 * i);
      const auto b = vld1q_f64(source + 4 * i + 2);
      vst1q_f64(destination + 4 * i, vbslq_f64(m0, vmulq_f64(a, g), a));
      vst1q_f64(destination + 4 * i + 2, vbslq_f64(m1, vmulq_f64(b, g), b));
    }
  }
}

template <class T, unsigned C, bool Field>
inline void neon_pixels(const T* source, const double* ev, T* destination,
                        std::size_t count, unsigned mask) {
  const double uniform_ev = Field ? 0 : ev[0];
  const double uniform_gain = Field ? 1 : std::exp2(uniform_ev);
  std::size_t i = 0;
  for (; i + 2 <= count; i += 2) {
    const auto e = Field ? vld1q_f64(ev + i) : vdupq_n_f64(uniform_ev);
    const auto magnitude = neon_abs_bits(e);
    const auto safe =
        vcleq_u64(magnitude, vdupq_n_u64(UINT64_C(0x408c200000000000)));
    if (vminvq_u32(vreinterpretq_u32_u64(safe)) != UINT32_MAX) {
      scalar_pixel(source + C * i, destination + C * i,
                   Field ? ev[i] : uniform_ev, C, mask);
      scalar_pixel(source + C * (i + 1), destination + C * (i + 1),
                   Field ? ev[i + 1] : uniform_ev, C, mask);
      continue;
    }
    const auto gain = Field ? neon_exp2<T>(e) : vdupq_n_f64(uniform_gain);
    const auto identity = vceqq_u64(magnitude, vdupq_n_u64(0));
    // Approximation error must not change finite/overflow classification.
    // The dtype-specific margin exceeds the polynomial error and rounding.
    constexpr double limit_value =
        static_cast<double>(std::numeric_limits<T>::max()) *
        (1.0 - (sizeof(T) == 4 ? 0x1p-24 : 0x1p-28));
    std::uint64_t limit_bits = 0;
    std::memcpy(&limit_bits, &limit_value, sizeof(limit_bits));
    const auto limit = vdupq_n_u64(limit_bits);
    auto boundary = vdupq_n_u64(0);
    if constexpr (C == 3 && Field) {
      if (mask == 7) {
        const float64x2_t gains[3] = {vdupq_laneq_f64(gain, 0), gain,
                                      vdupq_laneq_f64(gain, 1)};
        const uint64x2_t identities[3] = {vdupq_laneq_u64(identity, 0),
                                          identity,
                                          vdupq_laneq_u64(identity, 1)};
        for (unsigned j = 0; j < 3; ++j) {
          const auto* src = source + 3 * i + 2 * j;
          auto* dst = destination + 3 * i + 2 * j;
          const auto y = vmulq_f64(neon_load(src), gains[j]);
          boundary = vorrq_u64(
              boundary,
              vbicq_u64(vcgeq_u64(neon_abs_bits(y), limit), identities[j]));
          if constexpr (std::is_same_v<T, float>) {
            vst1_f32(dst, vbsl_f32(vmovn_u64(identities[j]), vld1_f32(src),
                                   vcvt_f32_f64(y)));
          } else {
            vst1q_f64(dst, vbslq_f64(identities[j], vld1q_f64(src), y));
          }
        }
        if (vmaxvq_u32(vreinterpretq_u32_u64(boundary))) {
          scalar_pixel(source + 3 * i, destination + 3 * i, ev[i], 3, mask);
          scalar_pixel(source + 3 * (i + 1), destination + 3 * (i + 1),
                       ev[i + 1], 3, mask);
        }
        continue;
      }
    }
    if constexpr (std::is_same_v<T, double>) {
      float64x2_t x[C];
      if constexpr (C == 1)
        x[0] = vld1q_f64(source + i);
      if constexpr (C == 3) {
        const auto v = vld3q_f64(source + 3 * i);
        for (unsigned c = 0; c < 3; ++c)
          x[c] = v.val[c];
      }
      if constexpr (C == 4) {
        const auto v = vld4q_f64(source + 4 * i);
        for (unsigned c = 0; c < 4; ++c)
          x[c] = v.val[c];
      }
      for (unsigned c = 0; c < C; ++c) {
        if (mask & (1U << c)) {
          const auto y = vmulq_f64(x[c], gain);
          if constexpr (Field)
            boundary = vorrq_u64(
                boundary,
                vbicq_u64(vcgeq_u64(neon_abs_bits(y), limit), identity));
          x[c] = vbslq_f64(identity, x[c], y);
        }
      }
      if constexpr (C == 1)
        vst1q_f64(destination + i, x[0]);
      if constexpr (C == 3)
        vst3q_f64(destination + 3 * i, (float64x2x3_t{{x[0], x[1], x[2]}}));
      if constexpr (C == 4)
        vst4q_f64(destination + 4 * i,
                  (float64x2x4_t{{x[0], x[1], x[2], x[3]}}));
    } else {
      float32x2_t x[C];
      if constexpr (C == 1)
        x[0] = vld1_f32(source + i);
      if constexpr (C == 3) {
        const auto v = vld3_f32(source + 3 * i);
        for (unsigned c = 0; c < 3; ++c)
          x[c] = v.val[c];
      }
      if constexpr (C == 4) {
        const auto v = vld4_f32(source + 4 * i);
        for (unsigned c = 0; c < 4; ++c)
          x[c] = v.val[c];
      }
      const auto copy = vmovn_u64(identity);
      for (unsigned c = 0; c < C; ++c) {
        if (mask & (1U << c)) {
          const auto y = vmulq_f64(vcvt_f64_f32(x[c]), gain);
          if constexpr (Field)
            boundary = vorrq_u64(
                boundary,
                vbicq_u64(vcgeq_u64(neon_abs_bits(y), limit), identity));
          x[c] = vbsl_f32(copy, x[c], vcvt_f32_f64(y));
        }
      }
      if constexpr (C == 1)
        vst1_f32(destination + i, x[0]);
      if constexpr (C == 3)
        vst3_f32(destination + 3 * i, (float32x2x3_t{{x[0], x[1], x[2]}}));
      if constexpr (C == 4)
        vst4_f32(destination + 4 * i,
                 (float32x2x4_t{{x[0], x[1], x[2], x[3]}}));
    }
    if constexpr (Field) {
      if (vmaxvq_u32(vreinterpretq_u32_u64(boundary))) {
        scalar_pixel(source + C * i, destination + C * i, ev[i], C, mask);
        scalar_pixel(source + C * (i + 1), destination + C * (i + 1), ev[i + 1],
                     C, mask);
      }
    }
  }
  for (; i < count; ++i)
    scalar_pixel(source + C * i, destination + C * i,
                 Field ? ev[i] : uniform_ev, C, mask);
}
#endif

#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
#define PS_GRD_AVX2 __attribute__((target("avx2,fma")))
inline bool has_avx2() {
  static const bool available =
      __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  return available;
}
template <class T>
PS_GRD_AVX2 inline __m256d avx_exp2(__m256d ev) {
  const auto n =
      _mm256_round_pd(ev, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
  const auto z = _mm256_mul_pd(
      _mm256_sub_pd(ev, n), _mm256_set1_pd(0.693147180559945309417232121458));
  const auto exponent =
      _mm256_slli_epi64(_mm256_cvtepi32_epi64(_mm_add_epi32(
                            _mm256_cvttpd_epi32(n), _mm_set1_epi32(1023))),
                        52);
  constexpr unsigned degree = sizeof(T) == 4 ? 6 : 8;
  const auto* c = sizeof(T) == 4 ? kExpPolynomial6 : kExpPolynomial8;
  auto p = _mm256_set1_pd(c[degree]);
  for (unsigned i = degree; i-- > 0;)
    p = _mm256_fmadd_pd(p, z, _mm256_set1_pd(c[i]));
  return _mm256_mul_pd(p, _mm256_castsi256_pd(exponent));
}
template <class T>
PS_GRD_AVX2 inline __m256d avx_load(const T* p) {
  if constexpr (std::is_same_v<T, float>)
    return _mm256_cvtps_pd(_mm_loadu_ps(p));
  else
    return _mm256_loadu_pd(p);
}
template <class T>
PS_GRD_AVX2 inline void avx_save(T* p, __m256d value) {
  if constexpr (std::is_same_v<T, float>)
    _mm_storeu_ps(p, _mm256_cvtpd_ps(value));
  else
    _mm256_storeu_pd(p, value);
}
template <class T>
PS_GRD_AVX2 inline void avx_uniform(const T* source, T* destination,
                                    std::size_t count, double gain) {
  const auto g = _mm256_set1_pd(gain);
  std::size_t i = 0;
  for (; i + 16 <= count; i += 16) {
    const auto a = _mm256_mul_pd(avx_load(source + i), g);
    const auto b = _mm256_mul_pd(avx_load(source + i + 4), g);
    const auto c = _mm256_mul_pd(avx_load(source + i + 8), g);
    const auto d = _mm256_mul_pd(avx_load(source + i + 12), g);
    avx_save(destination + i, a);
    avx_save(destination + i + 4, b);
    avx_save(destination + i + 8, c);
    avx_save(destination + i + 12, d);
  }
  for (; i + 4 <= count; i += 4)
    avx_save(destination + i, _mm256_mul_pd(avx_load(source + i), g));
  for (; i < count; ++i)
    destination[i] = static_cast<T>(static_cast<double>(source[i]) * gain);
}

template <class T>
PS_GRD_AVX2 inline void avx_masked_uniform(const T* source, T* destination,
                                           std::size_t count, double gain,
                                           unsigned mask) {
  const auto g = _mm256_set1_pd(gain);
  const auto keep = _mm256_castsi256_pd(
      _mm256_setr_epi64x(-static_cast<std::int64_t>(!(mask & 1)),
                         -static_cast<std::int64_t>(!(mask & 2)),
                         -static_cast<std::int64_t>(!(mask & 4)),
                         -static_cast<std::int64_t>(!(mask & 8))));
  const auto keep32 = _mm_castsi128_ps(_mm_setr_epi32(
      -static_cast<int>(!(mask & 1)), -static_cast<int>(!(mask & 2)),
      -static_cast<int>(!(mask & 4)), -static_cast<int>(!(mask & 8))));
  for (std::size_t i = 0; i < count; ++i) {
    const auto* src = source + 4 * i;
    auto* dst = destination + 4 * i;
    if constexpr (std::is_same_v<T, float>) {
      const auto x = _mm_loadu_ps(src);
      const auto y = _mm256_cvtpd_ps(_mm256_mul_pd(_mm256_cvtps_pd(x), g));
      _mm_storeu_ps(dst, _mm_blendv_ps(y, x, keep32));
    } else {
      const auto x = _mm256_loadu_pd(src);
      _mm256_storeu_pd(dst, _mm256_blendv_pd(_mm256_mul_pd(x, g), x, keep));
    }
  }
}

template <class T, unsigned C, bool Field>
PS_GRD_AVX2 inline void avx_pixels(const T* source, const double* ev,
                                   T* destination, std::size_t count,
                                   unsigned mask) {
  const double ue = Field ? 0 : ev[0];
  const auto ug = _mm256_set1_pd(Field ? 1 : std::exp2(ue));
  std::size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    const auto e = Field ? _mm256_loadu_pd(ev + i) : _mm256_set1_pd(ue);
    const auto safe = _mm256_cmp_pd(_mm256_andnot_pd(_mm256_set1_pd(-0.0), e),
                                    _mm256_set1_pd(900.0), _CMP_LE_OQ);
    if (_mm256_movemask_pd(safe) != 15) {
      for (unsigned j = 0; j < 4; ++j)
        scalar_pixel(source + C * (i + j), destination + C * (i + j),
                     Field ? ev[i + j] : ue, C, mask);
      continue;
    }
    const auto g = Field ? avx_exp2<T>(e) : ug;
    const auto copy = _mm256_cmp_pd(e, _mm256_setzero_pd(), _CMP_EQ_OQ);
    __m256d gains[C], identities[C];
    if constexpr (C == 1) {
      gains[0] = g;
      identities[0] = copy;
    }
    if constexpr (C == 3) {
      gains[0] = _mm256_permute4x64_pd(g, 0x40);
      gains[1] = _mm256_permute4x64_pd(g, 0xa5);
      gains[2] = _mm256_permute4x64_pd(g, 0xfe);
      identities[0] = _mm256_permute4x64_pd(copy, 0x40);
      identities[1] = _mm256_permute4x64_pd(copy, 0xa5);
      identities[2] = _mm256_permute4x64_pd(copy, 0xfe);
    }
    if constexpr (C == 4) {
      gains[0] = _mm256_permute4x64_pd(g, 0x00);
      gains[1] = _mm256_permute4x64_pd(g, 0x55);
      gains[2] = _mm256_permute4x64_pd(g, 0xaa);
      gains[3] = _mm256_permute4x64_pd(g, 0xff);
      identities[0] = _mm256_permute4x64_pd(copy, 0x00);
      identities[1] = _mm256_permute4x64_pd(copy, 0x55);
      identities[2] = _mm256_permute4x64_pd(copy, 0xaa);
      identities[3] = _mm256_permute4x64_pd(copy, 0xff);
    }
    auto boundary = _mm256_setzero_pd();
    constexpr double limit_value =
        static_cast<double>(std::numeric_limits<T>::max()) *
        (1.0 - (sizeof(T) == 4 ? 0x1p-24 : 0x1p-28));
    const auto limit = _mm256_set1_pd(limit_value);
    for (unsigned j = 0; j < C; ++j) {
      const auto bypass = _mm256_castsi256_pd(
          _mm256_set_epi64x(-(std::int64_t)!(mask & (1U << ((4 * j + 3) % C))),
                            -(std::int64_t)!(mask & (1U << ((4 * j + 2) % C))),
                            -(std::int64_t)!(mask & (1U << ((4 * j + 1) % C))),
                            -(std::int64_t)!(mask & (1U << ((4 * j) % C)))));
      const auto keep = _mm256_or_pd(identities[j], bypass);
      const auto* s = source + C * i + 4 * j;
      auto* d = destination + C * i + 4 * j;
      const auto y = _mm256_mul_pd(avx_load(s), gains[j]);
      if constexpr (Field) {
        const auto near = _mm256_cmp_pd(
            _mm256_andnot_pd(_mm256_set1_pd(-0.0), y), limit, _CMP_GE_OQ);
        boundary = _mm256_or_pd(boundary, _mm256_andnot_pd(keep, near));
      }
      if constexpr (std::is_same_v<T, float>) {
        const auto k =
            _mm_castsi128_ps(_mm256_castsi256_si128(_mm256_permutevar8x32_epi32(
                _mm256_castpd_si256(keep),
                _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6))));
        const auto original = _mm_loadu_ps(s);
        const auto result = _mm256_cvtpd_ps(y);
        _mm_storeu_ps(d, _mm_blendv_ps(result, original, k));
      } else {
        _mm256_storeu_pd(d, _mm256_blendv_pd(y, _mm256_loadu_pd(s), keep));
      }
    }
    if constexpr (Field) {
      if (_mm256_movemask_pd(boundary)) {
        for (unsigned j = 0; j < 4; ++j)
          scalar_pixel(source + C * (i + j), destination + C * (i + j),
                       ev[i + j], C, mask);
      }
    }
  }
  for (; i < count; ++i)
    scalar_pixel(source + C * i, destination + C * i, Field ? ev[i] : ue, C,
                 mask);
}
#undef PS_GRD_AVX2
#endif

// Contiguous, disjoint buffers; dynamic EVs are consumed on every call.
// C is 1, 3 or 4 and mask names the active stored slots. Bypasses copy bits.
template <class T, unsigned C, bool Field>
inline void exposure_span(const T* source, const double* ev, T* destination,
                          std::size_t count, unsigned mask) {
  if (!count)
    return;
  if (!Field && ev[0] == 0) {
    std::memcpy(destination, source, count * C * sizeof(T));
    return;
  }
#if defined(__aarch64__)
  if constexpr (!Field && C == 4) {
    if (ev[0] >= -1022 && ev[0] <= 1023) {
      neon_masked_uniform(source, destination, count, std::exp2(ev[0]), mask);
      return;
    }
  }
#elif defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
  if constexpr (!Field && C == 4) {
    if (has_avx2() && ev[0] >= -1022 && ev[0] <= 1023) {
      avx_masked_uniform(source, destination, count, std::exp2(ev[0]), mask);
      return;
    }
  }
#endif
  if constexpr (!Field) {
    if (mask == (1U << C) - 1 && ev[0] >= -1022 && ev[0] <= 1023) {
      const double gain = std::exp2(ev[0]);
#if defined(__aarch64__)
      neon_uniform(source, destination, count * C, gain);
      return;
#elif defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
      if (has_avx2()) {
        avx_uniform(source, destination, count * C, gain);
        return;
      }
#endif
      for (std::size_t i = 0; i < count * C; ++i)
        destination[i] = static_cast<T>(static_cast<double>(source[i]) * gain);
      return;
    }
  }
#if defined(__aarch64__)
  neon_pixels<T, C, Field>(source, ev, destination, count, mask);
  return;
#elif defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
  if (has_avx2()) {
    avx_pixels<T, C, Field>(source, ev, destination, count, mask);
    return;
  }
#endif
  for (std::size_t i = 0; i < count; ++i)
    scalar_pixel(source + C * i, destination + C * i, Field ? ev[i] : ev[0], C,
                 mask);
}
}  // namespace ps::plugin_internal::grade
