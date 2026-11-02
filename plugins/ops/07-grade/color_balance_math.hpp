// Copyright 2026 Photospider contributors
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__aarch64__)
#include <arm_neon.h>

#include "01-numeric/fast_native_simd.hpp"
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#endif

namespace ps::plugin_internal::grade::balance {
/** Invocation-local offsets in selected-coordinate order. Dynamic controls are
 * never cached by preparation. Inactive coordinates retain their source bits.
 */
struct Controls final {
  std::array<std::array<double, 3>, 3> offsets{};
  std::array<std::uint64_t, 3> selected{};
  std::array<bool, 3> active{};
  unsigned count = 3;
  bool copies = true;
  void classify() {
    copies = true;
    for (unsigned c = 0; c < count; ++c) {
      active[c] =
          offsets[c][0] != 0 || offsets[c][1] != 0 || offsets[c][2] != 0;
      copies = copies && !active[c];
    }
  }
  double delta(double a, double b, double c, unsigned k) const {
    const auto& o = offsets[k];
    return std::fma(c, o[2], std::fma(b, o[1], a * o[0])) / ((a + b) + c);
  }
};
template <class T>
inline void apply(const T* x, T* y, double delta) {
  if (delta == 0) {
    std::memcpy(y, x, sizeof(T));
  } else {
    *y = static_cast<T>(static_cast<double>(*x) + delta);
  }
}
template <class T, class Z>
inline void scalar(const T* x, const Z* w, T* y, std::size_t n,
                   unsigned channels, const Controls& op) {
  for (std::size_t i = 0; i < n; ++i) {
    std::memcpy(y + i * channels, x + i * channels, channels * sizeof(T));
    for (unsigned c = 0; c < op.count; ++c) {
      if (op.active[c]) {
        apply(x + i * channels + op.selected[c],
              y + i * channels + op.selected[c],
              op.delta(w[3 * i], w[3 * i + 1], w[3 * i + 2], c));
      }
    }
  }
}

#if defined(__aarch64__)
template <unsigned C, class T, class Z>
__attribute__((noinline)) std::size_t neon(const T* x, const Z* w, T* y,
                                           std::size_t n, const Controls& op) {
  std::array<std::array<float64x2_t, 3>, C> o{};
  std::array<bool, C> active{};
  for (unsigned c = 0; c < op.count; ++c) {
    for (unsigned z = 0; z < 3; ++z) {
      o[op.selected[c]][z] = vdupq_n_f64(op.offsets[c][z]);
      active[op.selected[c]] = op.active[c];
    }
  }
  const auto zero = vdupq_n_f64(0);
  std::size_t i = 0;
#if defined(__clang__)
#pragma clang loop unroll_count(4)
#endif
  for (; i + 2 <= n; i += 2) {
    float64x2_t a, b, c;
    if constexpr (sizeof(Z) == 4) {
      const auto v = vld3_f32(w + 3 * i);
      a = vcvt_f64_f32(v.val[0]);
      b = vcvt_f64_f32(v.val[1]);
      c = vcvt_f64_f32(v.val[2]);
    } else {
      const auto v = vld3q_f64(w + 3 * i);
      a = v.val[0];
      b = v.val[1];
      c = v.val[2];
    }
    const auto sum = vaddq_f64(vaddq_f64(a, b), c);
    std::array<float32x2_t, C> raw32;
    std::array<float64x2_t, C> raw64;
    if constexpr (sizeof(T) == 4) {
      if constexpr (C == 3) {
        const auto v = vld3_f32(x + C * i);
        for (unsigned k = 0; k < C; ++k)
          raw32[k] = v.val[k];
      } else {
        const auto v = vld4_f32(x + C * i);
        for (unsigned k = 0; k < C; ++k)
          raw32[k] = v.val[k];
      }
    } else {
      if constexpr (C == 3) {
        const auto v = vld3q_f64(x + C * i);
        for (unsigned k = 0; k < C; ++k)
          raw64[k] = v.val[k];
      } else {
        const auto v = vld4q_f64(x + C * i);
        for (unsigned k = 0; k < C; ++k)
          raw64[k] = v.val[k];
      }
    }
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (unsigned k = 0; k < C; ++k) {
      if (!active[k])
        continue;
      const auto slot = k;
      auto d =
          vfmaq_f64(vfmaq_f64(vmulq_f64(a, o[k][0]), b, o[k][1]), c, o[k][2]);
      d = vdivq_f64(d, sum);
      const auto copy = native_simd::equal(d, zero);
      if constexpr (sizeof(T) == 4) {
        const auto r = vcvt_f32_f64(vaddq_f64(vcvt_f64_f32(raw32[slot]), d));
        raw32[slot] = vbsl_f32(vmovn_u64(copy), raw32[slot], r);
      } else {
        raw64[slot] = vbslq_f64(copy, raw64[slot], vaddq_f64(raw64[slot], d));
      }
    }
    if constexpr (sizeof(T) == 4) {
      if constexpr (C == 3) {
        const float32x2x3_t v{{raw32[0], raw32[1], raw32[2]}};
        vst3_f32(y + C * i, v);
      } else {
        const float32x2x4_t v{{raw32[0], raw32[1], raw32[2], raw32[3]}};
        vst4_f32(y + C * i, v);
      }
    } else {
      if constexpr (C == 3) {
        const float64x2x3_t v{{raw64[0], raw64[1], raw64[2]}};
        vst3q_f64(y + C * i, v);
      } else {
        const float64x2x4_t v{{raw64[0], raw64[1], raw64[2], raw64[3]}};
        vst4q_f64(y + C * i, v);
      }
    }
  }
  return i;
}
#endif

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define PS_BALANCE_AVX __attribute__((target("avx2,fma")))
template <class T>
PS_BALANCE_AVX inline __m256d gather(const T* p, unsigned stride) {
  if constexpr (sizeof(T) == 4) {
    const auto ix = _mm_setr_epi32(0, stride, 2 * stride, 3 * stride);
    return _mm256_cvtps_pd(_mm_i32gather_ps(p, ix, 4));
  } else {
    const auto ix = _mm256_setr_epi64x(0, stride, 2 * stride, 3 * stride);
    return _mm256_i64gather_pd(p, ix, 8);
  }
}
template <unsigned C, class T, class Z>
PS_BALANCE_AVX __attribute__((noinline)) std::size_t avx(const T* x, const Z* w,
                                                         T* y, std::size_t n,
                                                         const Controls& op) {
  __m256d o[C][3]{};
  std::array<bool, C> active{};
  for (unsigned k = 0; k < op.count; ++k) {
    for (unsigned z = 0; z < 3; ++z) {
      o[op.selected[k]][z] = _mm256_set1_pd(op.offsets[k][z]);
      active[op.selected[k]] = op.active[k];
    }
  }
  const auto zero = _mm256_setzero_pd();
  const auto ix32 = _mm_setr_epi32(0, C, 2 * C, 3 * C);
  std::size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const auto a = gather(w + 3 * i, 3);
    const auto b = gather(w + 3 * i + 1, 3);
    const auto c = gather(w + 3 * i + 2, 3);
    const auto sum = _mm256_add_pd(_mm256_add_pd(a, b), c);
    __m256d raw64[4];
    __m128 raw32[4];
    for (unsigned k = 0; k < C; ++k) {
      if constexpr (sizeof(T) == 4) {
        raw32[k] = _mm_i32gather_ps(x + C * i + k, ix32, 4);
      } else {
        raw64[k] = gather(x + C * i + k, C);
      }
    }
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (unsigned k = 0; k < C; ++k) {
      if (!active[k])
        continue;
      const auto slot = k;
      auto d = _mm256_fmadd_pd(
          c, o[k][2], _mm256_fmadd_pd(b, o[k][1], _mm256_mul_pd(a, o[k][0])));
      d = _mm256_div_pd(d, sum);
      const auto copy = _mm256_cmp_pd(d, zero, _CMP_EQ_OQ);
      if constexpr (sizeof(T) == 4) {
        const auto result =
            _mm256_cvtpd_ps(_mm256_add_pd(_mm256_cvtps_pd(raw32[slot]), d));
        const auto mask =
            _mm_castsi128_ps(_mm256_castsi256_si128(_mm256_permutevar8x32_epi32(
                _mm256_castpd_si256(copy),
                _mm256_setr_epi32(0, 2, 4, 6, 0, 0, 0, 0))));
        raw32[slot] = _mm_blendv_ps(result, raw32[slot], mask);
      } else {
        raw64[slot] =
            _mm256_blendv_pd(_mm256_add_pd(raw64[slot], d), raw64[slot], copy);
      }
    }
    if constexpr (sizeof(T) == 4) {
      if constexpr (C == 3)
        raw32[3] = _mm_setzero_ps();
      _MM_TRANSPOSE4_PS(raw32[0], raw32[1], raw32[2], raw32[3]);
      for (unsigned k = 0; k < 4; ++k) {
        if constexpr (C == 4) {
          _mm_storeu_ps(y + C * (i + k), raw32[k]);
        } else {
          _mm_storel_pi(reinterpret_cast<__m64*>(y + C * (i + k)), raw32[k]);
          _mm_store_ss(y + C * (i + k) + 2,
                       _mm_shuffle_ps(raw32[k], raw32[k], 0xaa));
        }
      }
    } else {
      if constexpr (C == 3)
        raw64[3] = zero;
      const auto ab0 = _mm256_unpacklo_pd(raw64[0], raw64[1]);
      const auto ab1 = _mm256_unpackhi_pd(raw64[0], raw64[1]);
      const auto cd0 = _mm256_unpacklo_pd(raw64[2], raw64[3]);
      const auto cd1 = _mm256_unpackhi_pd(raw64[2], raw64[3]);
      const __m256d rows[4]{_mm256_permute2f128_pd(ab0, cd0, 0x20),
                            _mm256_permute2f128_pd(ab1, cd1, 0x20),
                            _mm256_permute2f128_pd(ab0, cd0, 0x31),
                            _mm256_permute2f128_pd(ab1, cd1, 0x31)};
      for (unsigned k = 0; k < 4; ++k) {
        if constexpr (C == 4) {
          _mm256_storeu_pd(y + C * (i + k), rows[k]);
        } else {
          _mm_storeu_pd(y + C * (i + k), _mm256_castpd256_pd128(rows[k]));
          _mm_store_sd(y + C * (i + k) + 2, _mm256_extractf128_pd(rows[k], 1));
        }
      }
    }
  }
  return i;
}
#undef PS_BALANCE_AVX
#endif

template <class T, class Z>
inline void span(const T* x, const Z* w, T* y, std::size_t n, unsigned channels,
                 const Controls& op) {
  if (op.copies) {
    std::memcpy(y, x, n * channels * sizeof(T));
    return;
  }
  std::size_t done = 0;
#if defined(__aarch64__)
  if (channels == 3)
    done = neon<3>(x, w, y, n, op);
  if (channels == 4)
    done = neon<4>(x, w, y, n, op);
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
    if (channels == 3)
      done = avx<3>(x, w, y, n, op);
    if (channels == 4)
      done = avx<4>(x, w, y, n, op);
  }
#endif
  scalar(x + done * channels, w + 3 * done, y + done * channels, n - done,
         channels, op);
}

#if defined(__aarch64__)
template <class T>
inline float64x2_t plane_load(const T* x) {
  if constexpr (sizeof(T) == 4)
    return vcvt_f64_f32(vld1_f32(x));
  else
    return vld1q_f64(x);
}
template <class T, class Z>
inline std::size_t plane_neon(const T* x, const Z* a, const Z* b, const Z* c,
                              T* y, std::size_t n, const double* o) {
  const auto p = vdupq_n_f64(o[0]), q = vdupq_n_f64(o[1]),
             r = vdupq_n_f64(o[2]);
  std::size_t i = 0;
#if defined(__clang__)
#pragma clang loop unroll_count(4)
#endif
  for (; i + 2 <= n; i += 2) {
    const auto va = plane_load(a + i), vb = plane_load(b + i),
               vc = plane_load(c + i);
    const auto sum = vaddq_f64(vaddq_f64(va, vb), vc);
    const auto delta =
        vdivq_f64(vfmaq_f64(vfmaq_f64(vmulq_f64(va, p), vb, q), vc, r), sum);
    const auto copy = native_simd::equal(delta, vdupq_n_f64(0));
    const auto result = vaddq_f64(plane_load(x + i), delta);
    if constexpr (sizeof(T) == 4) {
      vst1_f32(y + i, vbsl_f32(vmovn_u64(copy), vld1_f32(x + i),
                               vcvt_f32_f64(result)));
    } else {
      vst1q_f64(y + i, vbslq_f64(copy, vld1q_f64(x + i), result));
    }
  }
  return i;
}
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define PS_BALANCE_AVX __attribute__((target("avx2,fma")))
template <class T>
PS_BALANCE_AVX inline __m256d plane_load(const T* x) {
  if constexpr (sizeof(T) == 4)
    return _mm256_cvtps_pd(_mm_loadu_ps(x));
  else
    return _mm256_loadu_pd(x);
}
template <class T, class Z>
PS_BALANCE_AVX inline std::size_t plane_avx(const T* x, const Z* a, const Z* b,
                                            const Z* c, T* y, std::size_t n,
                                            const double* o) {
  const auto p = _mm256_set1_pd(o[0]), q = _mm256_set1_pd(o[1]),
             r = _mm256_set1_pd(o[2]);
  std::size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const auto va = plane_load(a + i), vb = plane_load(b + i),
               vc = plane_load(c + i);
    const auto sum = _mm256_add_pd(_mm256_add_pd(va, vb), vc);
    const auto delta = _mm256_div_pd(
        _mm256_fmadd_pd(vc, r, _mm256_fmadd_pd(vb, q, _mm256_mul_pd(va, p))),
        sum);
    const auto copy = _mm256_cmp_pd(delta, _mm256_setzero_pd(), _CMP_EQ_OQ);
    const auto result = _mm256_add_pd(plane_load(x + i), delta);
    if constexpr (sizeof(T) == 4) {
      const auto mask =
          _mm_castsi128_ps(_mm256_castsi256_si128(_mm256_permutevar8x32_epi32(
              _mm256_castpd_si256(copy),
              _mm256_setr_epi32(0, 2, 4, 6, 0, 0, 0, 0))));
      _mm_storeu_ps(y + i, _mm_blendv_ps(_mm256_cvtpd_ps(result),
                                         _mm_loadu_ps(x + i), mask));
    } else {
      _mm256_storeu_pd(y + i,
                       _mm256_blendv_pd(result, _mm256_loadu_pd(x + i), copy));
    }
  }
  return i;
}
#undef PS_BALANCE_AVX
#endif

template <class T, class Z>
inline void plane(const T* x, const Z* a, const Z* b, const Z* c, T* y,
                  std::size_t n, const std::array<double, 3>& o) {
  if (o[0] == 0 && o[1] == 0 && o[2] == 0) {
    std::memcpy(y, x, n * sizeof(T));
    return;
  }
  std::size_t done = 0;
#if defined(__aarch64__)
  done = plane_neon(x, a, b, c, y, n, o.data());
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
    done = plane_avx(x, a, b, c, y, n, o.data());
#endif
  for (std::size_t i = done; i < n; ++i) {
    const double sum = (static_cast<double>(a[i]) + b[i]) + c[i];
    const double delta = std::fma(static_cast<double>(c[i]), o[2],
                                  std::fma(static_cast<double>(b[i]), o[1],
                                           static_cast<double>(a[i]) * o[0])) /
                         sum;
    apply(x + i, y + i, delta);
  }
}

}  // namespace ps::plugin_internal::grade::balance
