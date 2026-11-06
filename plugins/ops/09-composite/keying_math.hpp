// Copyright 2026 Photospider Authors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PS_KEY_INLINE inline __attribute__((always_inline))
#define PS_KEY_ALWAYS_INLINE __attribute__((always_inline))
#else
#define PS_KEY_INLINE inline
#define PS_KEY_ALWAYS_INLINE
#endif
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define PS_KEY_X86 1
#include <immintrin.h>
#define PS_KEY_AVX2 __attribute__((target("avx2")))
#else
#define PS_KEY_X86 0
#endif

namespace ps::plugin_internal::keying {
enum class Kind {
  Range,
  Lab76,
  LabChroma,
  Difference,
  Foreground,
  Green,
  Directional,
  Nearest,
  Median,
  Erode,
  Dilate,
  Fill,
  Guided,
  Extend,
  Matting
};

template <class T>
inline T smooth(T a, T b, T x) {
  const T t = std::clamp((x - a) / (b - a), T(0), T(1));
  return t * t * (T(3) - T(2) * t);
}

#if defined(__aarch64__)
template <class T, unsigned Channels>
std::uint64_t green_neon(const T* input, const T* mask, T* out, std::uint64_t n,
                         T strength) {
  std::uint64_t i = 0;
  if constexpr (sizeof(T) == 4) {
    const auto zero = vdupq_n_f32(0);
    for (; i + 4 <= n; i += 4) {
      float32x4x3_t a{};
      float32x4x4_t b{};
      if constexpr (Channels == 3) {
        a = vld3q_f32(input + 3 * i);
      } else {
        b = vld4q_f32(input + 4 * i);
      }
      const auto r = Channels == 3 ? a.val[0] : b.val[0];
      const auto g = Channels == 3 ? a.val[1] : b.val[1];
      const auto blue = Channels == 3 ? a.val[2] : b.val[2];
      const auto maximum = vbslq_f32(vcgtq_f32(blue, r), blue, r);
      const auto excess = vsubq_f32(g, maximum);
      const auto factor = vmulq_n_f32(vld1q_f32(mask + i), strength);
      const auto correction = vmulq_f32(factor, excess);
      const auto change =
          vandq_u32(vcgtq_f32(excess, zero),
                    vandq_u32(vmvnq_u32(vceqq_f32(factor, zero)),
                              vmvnq_u32(vceqq_f32(correction, zero))));
      const auto value = vbslq_f32(change, vsubq_f32(g, correction), g);
      if constexpr (Channels == 3) {
        a.val[1] = value;
        vst3q_f32(out + 3 * i, a);
      } else {
        b.val[1] = value;
        vst4q_f32(out + 4 * i, b);
      }
    }
  } else {
    const auto zero = vdupq_n_f64(0);
    for (; i + 2 <= n; i += 2) {
      float64x2x3_t a{};
      float64x2x4_t b{};
      if constexpr (Channels == 3) {
        a = vld3q_f64(input + 3 * i);
      } else {
        b = vld4q_f64(input + 4 * i);
      }
      const auto r = Channels == 3 ? a.val[0] : b.val[0];
      const auto g = Channels == 3 ? a.val[1] : b.val[1];
      const auto blue = Channels == 3 ? a.val[2] : b.val[2];
      const auto maximum = vbslq_f64(vcgtq_f64(blue, r), blue, r);
      const auto excess = vsubq_f64(g, maximum);
      const auto factor = vmulq_n_f64(vld1q_f64(mask + i), strength);
      const auto correction = vmulq_f64(factor, excess);
      const auto copy =
          vorrq_u64(vceqq_f64(factor, zero), vceqq_f64(correction, zero));
      const auto value =
          vbslq_f64(vcgtq_f64(excess, zero),
                    vbslq_f64(copy, g, vsubq_f64(g, correction)), g);
      if constexpr (Channels == 3) {
        a.val[1] = value;
        vst3q_f64(out + 3 * i, a);
      } else {
        b.val[1] = value;
        vst4q_f64(out + 4 * i, b);
      }
    }
  }
  return i;
}
template <class T, unsigned Channels>
std::uint64_t directional_neon(const T* input, const T* mask, T* out,
                               std::uint64_t n, T strength,
                               const std::array<double, 3>& direction,
                               const std::array<double, 3>& weights) {
  const auto zero = vdupq_n_f64(0);
  std::uint64_t i = 0;
  for (; i + 2 <= n; i += 2) {
    float32x2x3_t f3{};
    float32x2x4_t f4{};
    float64x2x3_t d3{};
    float64x2x4_t d4{};
    float64x2_t rgb[3], factor;
    if constexpr (sizeof(T) == 4) {
      if constexpr (Channels == 3) {
        f3 = vld3_f32(input + Channels * i);
        for (unsigned c = 0; c < 3; ++c) {
          rgb[c] = vcvt_f64_f32(f3.val[c]);
        }
      } else {
        f4 = vld4_f32(input + Channels * i);
        for (unsigned c = 0; c < 3; ++c) {
          rgb[c] = vcvt_f64_f32(f4.val[c]);
        }
      }
      factor = vcvt_f64_f32(vmul_f32(vld1_f32(mask + i), vdup_n_f32(strength)));
    } else {
      if constexpr (Channels == 3) {
        d3 = vld3q_f64(input + Channels * i);
        for (unsigned c = 0; c < 3; ++c) {
          rgb[c] = d3.val[c];
        }
      } else {
        d4 = vld4q_f64(input + Channels * i);
        for (unsigned c = 0; c < 3; ++c) {
          rgb[c] = d4.val[c];
        }
      }
      factor = vmulq_f64(vld1q_f64(mask + i), vdupq_n_f64(strength));
    }
    const auto y = vaddq_f64(vaddq_f64(vmulq_n_f64(rgb[0], weights[0]),
                                       vmulq_n_f64(rgb[1], weights[1])),
                             vmulq_n_f64(rgb[2], weights[2]));
    const auto projection =
        vaddq_f64(vaddq_f64(vmulq_n_f64(vsubq_f64(rgb[0], y), direction[0]),
                            vmulq_n_f64(vsubq_f64(rgb[1], y), direction[1])),
                  vmulq_n_f64(vsubq_f64(rgb[2], y), direction[2]));
    const auto e = vbslq_f64(vcgtq_f64(projection, zero), projection, zero);
    const auto scale = vmulq_f64(factor, e);
    for (unsigned c = 0; c < 3; ++c) {
      const auto correction = vmulq_n_f64(scale, direction[c]);
      const auto copy =
          vorrq_u64(vceqq_f64(factor, zero),
                    vorrq_u64(vceqq_f64(e, zero), vceqq_f64(correction, zero)));
      const auto result = vsubq_f64(rgb[c], correction);
      if constexpr (sizeof(T) == 4) {
        const auto keep = vshrn_n_u64(copy, 32);
        if constexpr (Channels == 3) {
          f3.val[c] = vbsl_f32(keep, f3.val[c], vcvt_f32_f64(result));
        } else {
          f4.val[c] = vbsl_f32(keep, f4.val[c], vcvt_f32_f64(result));
        }
      } else {
        if constexpr (Channels == 3) {
          d3.val[c] = vbslq_f64(copy, rgb[c], result);
        } else {
          d4.val[c] = vbslq_f64(copy, rgb[c], result);
        }
      }
    }
    if constexpr (sizeof(T) == 4) {
      if constexpr (Channels == 3) {
        vst3_f32(out + Channels * i, f3);
      } else {
        vst4_f32(out + Channels * i, f4);
      }
    } else {
      if constexpr (Channels == 3) {
        vst3q_f64(out + Channels * i, d3);
      } else {
        vst4q_f64(out + Channels * i, d4);
      }
    }
  }
  return i;
}
#endif

template <Kind K, class T, unsigned FixedChannels = 0>
PS_KEY_INLINE void point_span_impl(const T* const* in, T* out, std::uint64_t n,
                                   const T* controls, unsigned channels,
                                   const std::array<double, 3>& direction,
                                   const std::array<double, 3>& weights,
                                   bool invert) {
  constexpr bool kDistance =
      K == Kind::Lab76 || K == Kind::LabChroma || K == Kind::Difference;
  if constexpr (FixedChannels) {
    channels = FixedChannels;
  }
  if constexpr (K == Kind::Range) {
    const T lo = controls[0], hi = controls[1], w = controls[2];
    if (w == T(0)) {
      for (std::uint64_t i = 0; i < n; ++i) {
        const bool inside = in[0][i] >= lo && in[0][i] <= hi;
        out[i] = T(inside != invert);
      }
    } else {
      for (std::uint64_t i = 0; i < n; ++i) {
        const T x = in[0][i];
        // Only the nearer shoulder can be active. Measure from the plateau
        // so a positive width smaller than an endpoint ULP stays positive.
        const T distance = std::max(T(0), std::max(lo - x, x - hi));
        const T t = T(1) - std::min(distance / w, T(1));
        const T v = t * t * (T(3) - T(2) * t);
        out[i] = invert ? T(1) - v : v;
      }
    }
  } else if constexpr (kDistance) {
    for (std::uint64_t i = 0; i < n; ++i) {
      T a, b, c = 0;
      if constexpr (K == Kind::Difference) {
        c = in[0][3 * i] - in[1][3 * i];
        a = in[0][3 * i + 1] - in[1][3 * i + 1];
        b = in[0][3 * i + 2] - in[1][3 * i + 2];
      } else {
        if constexpr (K == Kind::Lab76) {
          c = T(100) * (in[0][3 * i] - controls[2]);
        }
        a = in[0][3 * i + 1] - controls[3];
        b = in[0][3 * i + 2] - controls[4];
      }
      // The distance is explicitly rounded to the output scalar type.
      const T distance = std::sqrt((c * c + a * a) + b * b);
      out[i] = smooth(controls[0], controls[1], distance);
    }
  } else if constexpr (K == Kind::Foreground) {
    for (std::uint64_t i = 0; i < n; ++i) {
      const T a = in[1][i];
      if (a == T(0) || a == T(1)) {
        std::memcpy(out + 3 * i, (a == T(0) ? in[3] : in[0]) + 3 * i,
                    3 * sizeof(T));
      } else {
        const T lambda = *controls;
        const T denominator = a * a + lambda;
        for (unsigned c = 0; c < 3; ++c) {
          out[3 * i + c] =
              (a * (in[0][3 * i + c] - (T(1) - a) * in[2][3 * i + c]) +
               lambda * in[3][3 * i + c]) /
              denominator;
        }
      }
    }
  } else {
    if (*controls == T(0)) {
      std::memcpy(out, in[0], n * channels * sizeof(T));
      return;
    }
    std::uint64_t first = 0;
#if defined(__aarch64__)
    if constexpr (K == Kind::Directional &&
                  (FixedChannels == 3 || FixedChannels == 4)) {
      first = directional_neon<T, FixedChannels>(in[0], in[1], out, n,
                                                 *controls, direction, weights);
    }
    if constexpr (K == Kind::Green &&
                  (FixedChannels == 3 || FixedChannels == 4)) {
      first = green_neon<T, FixedChannels>(in[0], in[1], out, n, *controls);
    }
#endif
    for (std::uint64_t i = first; i < n; ++i) {
      const auto j = channels * i;
      std::memcpy(out + j, in[0] + j, channels * sizeof(T));
      const T factor = *controls * in[1][i];
      if (factor == T(0)) {
        continue;
      }
      if constexpr (K == Kind::Green) {
        const T e =
            std::max(T(0), in[0][j + 1] - std::max(in[0][j], in[0][j + 2]));
        if (e != T(0)) {
          out[j + 1] = in[0][j + 1] - factor * e;
        }
      } else {
        const double r = in[0][j], g = in[0][j + 1], b = in[0][j + 2];
        const double y = (weights[0] * r + weights[1] * g) + weights[2] * b;
        const double e =
            std::max(0.0, (direction[0] * (r - y) + direction[1] * (g - y)) +
                              direction[2] * (b - y));
        if (e != 0) {
          for (unsigned c = 0; c < 3; ++c) {
            const double correction =
                static_cast<double>(factor) * e * direction[c];
            if (correction != 0) {
              out[j + c] = static_cast<T>(in[0][j + c] - correction);
            }
          }
        }
      }
    }
  }
}

#if PS_KEY_X86
PS_KEY_AVX2 inline void transpose4(__m256d* r, __m256d* g, __m256d* b,
                                   __m256d* a) {
  const auto t0 = _mm256_unpacklo_pd(*r, *g);
  const auto t1 = _mm256_unpackhi_pd(*r, *g);
  const auto t2 = _mm256_unpacklo_pd(*b, *a);
  const auto t3 = _mm256_unpackhi_pd(*b, *a);
  *r = _mm256_permute2f128_pd(t0, t2, 0x20);
  *g = _mm256_permute2f128_pd(t1, t3, 0x20);
  *b = _mm256_permute2f128_pd(t0, t2, 0x31);
  *a = _mm256_permute2f128_pd(t1, t3, 0x31);
}

template <Kind K, class T>
PS_KEY_AVX2 std::uint64_t spill_avx2(const T* input, const T* mask, T* out,
                                     std::uint64_t n, T strength,
                                     const std::array<double, 3>& direction,
                                     const std::array<double, 3>& weights) {
  if (strength == T(0)) {
    std::memcpy(out, input, n * 4 * sizeof(T));
    return n;
  }
  std::uint64_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 f[4]{};
    __m256d d[4]{};
    __m256d factor;
    if constexpr (sizeof(T) == 4) {
      for (unsigned c = 0; c < 4; ++c) {
        f[c] = _mm_loadu_ps(input + 4 * (i + c));
      }
      _MM_TRANSPOSE4_PS(f[0], f[1], f[2], f[3]);
      if constexpr (K == Kind::Green) {
        const auto zero = _mm_setzero_ps();
        const auto maximum =
            _mm_blendv_ps(f[0], f[2], _mm_cmpgt_ps(f[2], f[0]));
        const auto excess = _mm_sub_ps(f[1], maximum);
        const auto scale =
            _mm_mul_ps(_mm_loadu_ps(mask + i), _mm_set1_ps(strength));
        const auto correction = _mm_mul_ps(scale, excess);
        const auto active =
            _mm_and_ps(_mm_cmpgt_ps(excess, zero),
                       _mm_and_ps(_mm_cmpneq_ps(scale, zero),
                                  _mm_cmpneq_ps(correction, zero)));
        f[1] = _mm_blendv_ps(f[1], _mm_sub_ps(f[1], correction), active);
      } else {
        for (unsigned c = 0; c < 3; ++c) {
          d[c] = _mm256_cvtps_pd(f[c]);
        }
        factor = _mm256_cvtps_pd(
            _mm_mul_ps(_mm_loadu_ps(mask + i), _mm_set1_ps(strength)));
      }
    } else {
      for (unsigned c = 0; c < 4; ++c) {
        d[c] = _mm256_loadu_pd(input + 4 * (i + c));
      }
      transpose4(&d[0], &d[1], &d[2], &d[3]);
      factor =
          _mm256_mul_pd(_mm256_loadu_pd(mask + i), _mm256_set1_pd(strength));
      if constexpr (K == Kind::Green) {
        const auto zero = _mm256_setzero_pd();
        const auto maximum =
            _mm256_blendv_pd(d[0], d[2], _mm256_cmp_pd(d[2], d[0], _CMP_GT_OQ));
        const auto excess = _mm256_sub_pd(d[1], maximum);
        const auto correction = _mm256_mul_pd(factor, excess);
        const auto active = _mm256_and_pd(
            _mm256_cmp_pd(excess, zero, _CMP_GT_OQ),
            _mm256_and_pd(_mm256_cmp_pd(factor, zero, _CMP_NEQ_UQ),
                          _mm256_cmp_pd(correction, zero, _CMP_NEQ_UQ)));
        d[1] = _mm256_blendv_pd(d[1], _mm256_sub_pd(d[1], correction), active);
      }
    }
    if constexpr (K == Kind::Directional) {
      const auto zero = _mm256_setzero_pd();
      const auto y = _mm256_add_pd(
          _mm256_add_pd(_mm256_mul_pd(d[0], _mm256_set1_pd(weights[0])),
                        _mm256_mul_pd(d[1], _mm256_set1_pd(weights[1]))),
          _mm256_mul_pd(d[2], _mm256_set1_pd(weights[2])));
      const auto projection = _mm256_add_pd(
          _mm256_add_pd(_mm256_mul_pd(_mm256_sub_pd(d[0], y),
                                      _mm256_set1_pd(direction[0])),
                        _mm256_mul_pd(_mm256_sub_pd(d[1], y),
                                      _mm256_set1_pd(direction[1]))),
          _mm256_mul_pd(_mm256_sub_pd(d[2], y), _mm256_set1_pd(direction[2])));
      const auto excess = _mm256_blendv_pd(
          zero, projection, _mm256_cmp_pd(projection, zero, _CMP_GT_OQ));
      const auto scale = _mm256_mul_pd(factor, excess);
      for (unsigned c = 0; c < 3; ++c) {
        const auto correction =
            _mm256_mul_pd(scale, _mm256_set1_pd(direction[c]));
        const auto keep = _mm256_or_pd(
            _mm256_cmp_pd(factor, zero, _CMP_EQ_OQ),
            _mm256_or_pd(_mm256_cmp_pd(excess, zero, _CMP_EQ_OQ),
                         _mm256_cmp_pd(correction, zero, _CMP_EQ_OQ)));
        const auto result = _mm256_sub_pd(d[c], correction);
        if constexpr (sizeof(T) == 4) {
          const auto selection =
              _mm_shuffle_ps(_mm_castpd_ps(_mm256_castpd256_pd128(keep)),
                             _mm_castpd_ps(_mm256_extractf128_pd(keep, 1)),
                             _MM_SHUFFLE(2, 0, 2, 0));
          f[c] = _mm_blendv_ps(_mm256_cvtpd_ps(result), f[c], selection);
        } else {
          d[c] = _mm256_blendv_pd(result, d[c], keep);
        }
      }
    }
    if constexpr (sizeof(T) == 4) {
      _MM_TRANSPOSE4_PS(f[0], f[1], f[2], f[3]);
      for (unsigned c = 0; c < 4; ++c) {
        _mm_storeu_ps(out + 4 * (i + c), f[c]);
      }
    } else {
      transpose4(&d[0], &d[1], &d[2], &d[3]);
      for (unsigned c = 0; c < 4; ++c) {
        _mm256_storeu_pd(out + 4 * (i + c), d[c]);
      }
    }
  }
  return i;
}
#endif

#if PS_KEY_X86
template <Kind K, class T, unsigned FixedChannels = 0>
PS_KEY_AVX2 void point_span_avx2(const T* const* in, T* out, std::uint64_t n,
                                 const T* controls, unsigned channels,
                                 const std::array<double, 3>& direction,
                                 const std::array<double, 3>& weights,
                                 bool invert) {
  if constexpr ((K == Kind::Green || K == Kind::Directional) &&
                FixedChannels == 4) {
    const auto first =
        spill_avx2<K>(in[0], in[1], out, n, *controls, direction, weights);
    const T* tail[4]{in[0] + 4 * first, in[1] + first, nullptr, nullptr};
    return point_span_impl<K, T, FixedChannels>(tail, out + 4 * first,
                                                n - first, controls, channels,
                                                direction, weights, invert);
  }
  return point_span_impl<K, T, FixedChannels>(in, out, n, controls, channels,
                                              direction, weights, invert);
}
#endif
#if PS_KEY_X86
template <class T>
PS_KEY_AVX2 bool span_equal_avx2(const T* source, std::uint64_t n, T value) {
  std::uint64_t i = 0;
  if constexpr (sizeof(T) == 4) {
    const auto expected = _mm256_set1_ps(value);
    for (; i + 8 <= n; i += 8) {
      if (_mm256_movemask_ps(_mm256_cmp_ps(_mm256_loadu_ps(source + i),
                                           expected, _CMP_EQ_OQ)) != 255) {
        return false;
      }
    }
  } else {
    const auto expected = _mm256_set1_pd(value);
    for (; i + 4 <= n; i += 4) {
      if (_mm256_movemask_pd(_mm256_cmp_pd(_mm256_loadu_pd(source + i),
                                           expected, _CMP_EQ_OQ)) != 15) {
        return false;
      }
    }
  }
  for (; i < n; ++i) {
    if (source[i] != value) {
      return false;
    }
  }
  return true;
}
#endif
template <class T>
bool span_equal(const T* source, std::uint64_t n, T value) {
#if PS_KEY_X86
  if (__builtin_cpu_supports("avx2")) {
    return span_equal_avx2(source, n, value);
  }
#endif
  std::uint64_t i = 0;
#if defined(__aarch64__)
  if constexpr (sizeof(T) == 4) {
    const auto expected = vdupq_n_f32(value);
    for (; i + 4 <= n; i += 4) {
      if (vminvq_u32(vceqq_f32(vld1q_f32(source + i), expected)) !=
          UINT32_MAX) {
        return false;
      }
    }
  } else {
    return std::all_of(source, source + n,
                       [value](T current) { return current == value; });
  }
#endif
  for (; i < n; ++i) {
    if (source[i] != value) {
      return false;
    }
  }
  return true;
}

template <Kind K, class T, unsigned FixedChannels = 0>
void point_span(const T* const* in, T* out, std::uint64_t n, const T* controls,
                unsigned channels, const std::array<double, 3>& direction,
                const std::array<double, 3>& weights, bool invert) {
  if constexpr (K == Kind::Green || K == Kind::Directional) {
    if (*controls == T(0) ||
        (n && in[1][0] == T(0) && span_equal(in[1], n, T(0)))) {
      std::memcpy(out, in[0], n * channels * sizeof(T));
      return;
    }
  } else if constexpr (K == Kind::Foreground) {
    if (n && (in[1][0] == T(0) || in[1][0] == T(1))) {
      const T endpoint = in[1][0];
      if (span_equal(in[1], n, endpoint)) {
        std::memcpy(out, endpoint == T(0) ? in[3] : in[0], n * 3 * sizeof(T));
        return;
      }
    }
  }

#if PS_KEY_X86
  if (__builtin_cpu_supports("avx2")) {
    return point_span_avx2<K, T, FixedChannels>(in, out, n, controls, channels,
                                                direction, weights, invert);
  }
#endif
  return point_span_impl<K, T, FixedChannels>(in, out, n, controls, channels,
                                              direction, weights, invert);
}

}  // namespace ps::plugin_internal::keying

#undef PS_KEY_ALWAYS_INLINE
#undef PS_KEY_INLINE
#undef PS_KEY_X86
#if defined(PS_KEY_AVX2)
#undef PS_KEY_AVX2
#endif
