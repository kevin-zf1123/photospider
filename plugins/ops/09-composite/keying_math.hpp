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

template <class T, class Poll>
void copy_elements(T* destination, const T* source, std::uint64_t count,
                   Poll poll) {
  for (std::uint64_t i = 0; i < count; i += 4096) {
    poll(0);
    std::memcpy(destination + i, source + i,
                std::min<std::uint64_t>(4096, count - i) * sizeof(T));
  }
}
template <class T, class Poll>
void fill_elements(T* destination, std::uint64_t count, T value, Poll poll) {
  for (std::uint64_t i = 0; i < count; i += 4096) {
    poll(0);
    std::fill(destination + i, destination + std::min(count, i + 4096), value);
  }
}

// Stable extrema: a tie always retains the earlier source, including -0.
template <bool Maximum, bool Stable = true, class T>
inline T extreme(T a, T b) {
  if constexpr (!Stable) {
#if PS_KEY_X86
    // This branch has only nonnegative finite matte values, so a tie has
    // identical source bits. This order maps directly to native MIN/MAX.
    return Maximum ? (a > b ? a : b) : (a < b ? a : b);
#else
    return Maximum ? std::fmax(a, b) : std::fmin(a, b);
#endif
  }
  return (Maximum ? b > a : b < a) ? b : a;
}
#if defined(__aarch64__)
template <bool Maximum, bool Stable, bool TrackSign>
inline std::uint64_t horizontal_seven_neon(const float* row, float* out,
                                           std::uint64_t x, std::uint64_t end,
                                           std::uint64_t width,
                                           bool* had_sign) {
  const auto combine = [](float32x4_t a, float32x4_t b) {
    if constexpr (Stable) {
      return vbslq_f32(Maximum ? vcgtq_f32(b, a) : vcltq_f32(b, a), b, a);
    }
    return Maximum ? vmaxq_f32(a, b) : vminq_f32(a, b);
  };
  auto signs = vdupq_n_u32(0);
  if (x + 4 <= end && x + 10 <= width) {
    const auto first = vld1q_f32(row + x - 3);
    const auto second = vld1q_f32(row + x + 1);
    auto q0 = combine(first, vld1q_f32(row + x - 2));
    auto q1 = combine(second, vld1q_f32(row + x + 2));
    if constexpr (TrackSign) {
      signs = vorrq_u32(vreinterpretq_u32_f32(first),
                        vreinterpretq_u32_f32(second));
    }
    auto s0 = combine(q0, vextq_f32(q0, q1, 2));
    auto last_source = vdupq_n_f32(0);
    do {
      const auto source = vld1q_f32(row + x + 5);
      last_source = vld1q_f32(row + x + 6);
      if constexpr (TrackSign) {
        signs = vorrq_u32(signs, vreinterpretq_u32_f32(source));
      }
      const auto q2 = combine(source, last_source);
      const auto s1 = combine(q1, vextq_f32(q1, q2, 2));
      vst1q_f32(out + x, combine(s0, vextq_f32(s0, s1, 3)));
      q1 = q2;
      s0 = s1;
      x += 4;
    } while (x + 4 <= end && x + 10 <= width);
    if constexpr (TrackSign) {
      signs = vorrq_u32(signs, vreinterpretq_u32_f32(last_source));
    }
  }
  for (; x + 4 <= end; x += 4) {
    auto value = vld1q_f32(row + x - 3);
    if constexpr (TrackSign) {
      signs = vorrq_u32(signs, vreinterpretq_u32_f32(value));
    }
    for (unsigned q = 1; q < 7; ++q) {
      const auto source = vld1q_f32(row + x - 3 + q);
      if constexpr (TrackSign) {
        signs = vorrq_u32(signs, vreinterpretq_u32_f32(source));
      }
      value = combine(value, source);
    }
    vst1q_f32(out + x, value);
  }
  if constexpr (TrackSign) {
    *had_sign |= (vmaxvq_u32(signs) & UINT32_C(0x80000000)) != 0;
  }
  return x;
}
#endif
#if PS_KEY_X86
template <unsigned Offset>
PS_KEY_AVX2 inline __m256 shift_lanes(__m256 a, __m256 b) {
  const auto halves = _mm256_permute2f128_ps(a, b, 0x21);
  return _mm256_castsi256_ps(_mm256_alignr_epi8(
      _mm256_castps_si256(halves), _mm256_castps_si256(a), 4 * Offset));
}
template <bool Maximum>
PS_KEY_AVX2 inline __m256 stable_extreme8(__m256 earlier, __m256 later) {
  return Maximum ? _mm256_max_ps(later, earlier)
                 : _mm256_min_ps(later, earlier);
}
template <bool Maximum>
PS_KEY_AVX2 std::uint64_t horizontal_seven_avx2(const float* row, float* out,
                                                std::uint64_t x,
                                                std::uint64_t end,
                                                std::uint64_t width) {
  if (x + 8 <= end && x + 22 <= width) {
    auto q0 = stable_extreme8<Maximum>(_mm256_loadu_ps(row + x - 3),
                                       _mm256_loadu_ps(row + x - 2));
    auto q1 = stable_extreme8<Maximum>(_mm256_loadu_ps(row + x + 5),
                                       _mm256_loadu_ps(row + x + 6));
    auto s0 = stable_extreme8<Maximum>(q0, shift_lanes<2>(q0, q1));
    do {
      const auto q2 = stable_extreme8<Maximum>(_mm256_loadu_ps(row + x + 13),
                                               _mm256_loadu_ps(row + x + 14));
      const auto s1 = stable_extreme8<Maximum>(q1, shift_lanes<2>(q1, q2));
      _mm256_storeu_ps(out + x,
                       stable_extreme8<Maximum>(s0, shift_lanes<3>(s0, s1)));
      q1 = q2;
      s0 = s1;
      x += 8;
    } while (x + 8 <= end && x + 22 <= width);
  }
  for (; x + 8 <= end; x += 8) {
    auto value = _mm256_loadu_ps(row + x - 3);
    for (unsigned q = 1; q < 7; ++q) {
      value = stable_extreme8<Maximum>(value, _mm256_loadu_ps(row + x - 3 + q));
    }
    _mm256_storeu_ps(out + x, value);
  }
  return x;
}
#endif

#if defined(__aarch64__)
// Trailing powers of two share work between neighboring packets. The final
// overlapping union has exactly 2*Radius+1 samples and retains earlier ties.
template <bool Maximum, bool Stable, unsigned Radius, bool TrackSign,
          class Poll>  // NOLINT(whitespace/indent_namespace)
PS_KEY_INLINE bool horizontal_power_neon(const float* row, float* output,
                                         std::uint64_t w, Poll poll) {
  constexpr unsigned kPower = Radius == 8 ? 16 : 8;
  constexpr unsigned kDelay = 2 * Radius + 1 - kPower;
  const float neutral = Maximum ? -std::numeric_limits<float>::infinity()
                                : std::numeric_limits<float>::infinity();
  const auto empty = vdupq_n_f32(neutral);
  auto previous_raw = empty, previous2 = empty, previous4 = empty;
  auto previous8 = empty, older8 = empty, previous16 = empty;
  auto signs = vdupq_n_u32(0);
  bool had_sign = false;
  const auto combine = [](float32x4_t earlier, float32x4_t later) {
    if constexpr (Stable) {
      return vbslq_f32(
          Maximum ? vcgtq_f32(later, earlier) : vcltq_f32(later, earlier),
          later, earlier);
    }
    return Maximum ? vmaxq_f32(earlier, later) : vminq_f32(earlier, later);
  };
  for (std::uint64_t end = 0; end < w + Radius; end += 4) {
    if ((end & 4095) == 0) {
      poll(0);
    }
    auto current = empty;
    if (end + 4 <= w) {
      current = vld1q_f32(row + end);
      if constexpr (TrackSign) {
        signs = vorrq_u32(signs, vreinterpretq_u32_f32(current));
      }
    } else {
      float tail[4] = {neutral, neutral, neutral, neutral};
      for (unsigned lane = 0; lane < 4 && end + lane < w; ++lane) {
        tail[lane] = row[end + lane];
        if constexpr (TrackSign) {
          had_sign |= std::signbit(tail[lane]);
        }
      }
      current = vld1q_f32(tail);
    }
    const auto two = combine(vextq_f32(previous_raw, current, 3), current);
    const auto four = combine(vextq_f32(previous2, two, 2), two);
    const auto eight = combine(previous4, four);
    auto value = empty;
    if constexpr (kPower == 16) {
      const auto sixteen = combine(older8, eight);
      value = combine(vextq_f32(previous16, sixteen, 4 - kDelay), sixteen);
      previous16 = sixteen;
    } else {
      value = combine(vextq_f32(previous8, eight, 4 - kDelay), eight);
    }
    previous_raw = current;
    previous2 = two;
    previous4 = four;
    older8 = previous8;
    previous8 = eight;
    if (end >= Radius && end + 4 <= w + Radius) {
      vst1q_f32(output + end - Radius, value);
    } else {
      float tail[4];
      vst1q_f32(tail, value);
      for (unsigned lane = 0; lane < 4; ++lane) {
        if (end + lane >= Radius && end + lane - Radius < w) {
          output[end + lane - Radius] = tail[lane];
        }
      }
    }
  }
  if constexpr (TrackSign) {
    had_sign |= (vmaxvq_u32(signs) >> 31) != 0;
  }
  return had_sign;
}
#endif
#if PS_KEY_X86
template <unsigned Delay>
PS_KEY_AVX2 inline __m256 delay_packet(__m256 previous, __m256 current) {
  const auto bridge = _mm256_permute2f128_ps(previous, current, 0x21);
  if constexpr (Delay == 4) {
    return bridge;
  }
  return _mm256_castsi256_ps(_mm256_alignr_epi8(_mm256_castps_si256(current),
                                                _mm256_castps_si256(bridge),
                                                4 * (4 - Delay)));
}
template <bool Maximum, unsigned Radius, class Poll>
PS_KEY_AVX2 bool horizontal_power_avx2(const float* row, float* output,
                                       std::uint64_t w, Poll poll) {
  constexpr unsigned kPower = Radius == 8 ? 16 : 8;
  constexpr unsigned kDelay = 2 * Radius + 1 - kPower;
  const float neutral = Maximum ? -std::numeric_limits<float>::infinity()
                                : std::numeric_limits<float>::infinity();
  const auto empty = _mm256_set1_ps(neutral);
  auto previous_raw = empty, previous2 = empty, previous4 = empty;
  auto previous8 = empty, previous16 = empty;
  for (std::uint64_t end = 0; end < w + Radius; end += 8) {
    if ((end & 4095) == 0) {
      poll(0);
    }
    auto current = empty;
    if (end + 8 <= w) {
      current = _mm256_loadu_ps(row + end);
    } else {
      float tail[8];
      std::fill(tail, tail + 8, neutral);
      for (unsigned lane = 0; lane < 8 && end + lane < w; ++lane) {
        tail[lane] = row[end + lane];
      }
      current = _mm256_loadu_ps(tail);
    }
    const auto two = stable_extreme8<Maximum>(
        delay_packet<1>(previous_raw, current), current);
    const auto four =
        stable_extreme8<Maximum>(delay_packet<2>(previous2, two), two);
    const auto eight =
        stable_extreme8<Maximum>(delay_packet<4>(previous4, four), four);
    auto value = empty;
    if constexpr (kPower == 16) {
      const auto sixteen = stable_extreme8<Maximum>(previous8, eight);
      value = stable_extreme8<Maximum>(
          delay_packet<kDelay>(previous16, sixteen), sixteen);
      previous16 = sixteen;
    } else {
      value = stable_extreme8<Maximum>(delay_packet<kDelay>(previous8, eight),
                                       eight);
    }
    previous_raw = current;
    previous2 = two;
    previous4 = four;
    previous8 = eight;
    if (end >= Radius && end + 8 <= w + Radius) {
      _mm256_storeu_ps(output + end - Radius, value);
    } else {
      float tail[8];
      _mm256_storeu_ps(tail, value);
      for (unsigned lane = 0; lane < 8; ++lane) {
        if (end + lane >= Radius && end + lane - Radius < w) {
          output[end + lane - Radius] = tail[lane];
        }
      }
    }
  }
  return false;
}
#endif

template <bool Maximum, unsigned Radius, bool Stable, bool TrackSign = false,
          class T, class Poll>  // NOLINT(whitespace/indent_namespace)
PS_KEY_INLINE bool horizontal_morphology(const T* row, T* target,
                                         std::uint64_t w, Poll poll) {
  constexpr unsigned kRows = 2 * Radius + 1;
  bool had_sign = false;
  poll(w * kRows);
  if constexpr (sizeof(T) == 4 && (Radius == 4 || Radius == 5 || Radius == 8)) {
#if defined(__aarch64__)
    return horizontal_power_neon<Maximum, Stable, Radius, TrackSign>(
        row, target, w, poll);
#elif PS_KEY_X86
    if (__builtin_cpu_supports("avx2")) {
      return horizontal_power_avx2<Maximum, Radius>(row, target, w, poll);
    }
#endif
  }
  const auto first = std::min<std::uint64_t>(Radius, w);
  const auto last = w > Radius ? std::max(first, w - Radius) : first;
  const auto border = [&](std::uint64_t x) {
    const auto lo = x > Radius ? x - Radius : 0;
    T value = row[lo];
    if constexpr (TrackSign) {
      had_sign |= std::signbit(value);
    }
    for (auto q = lo + 1; q < std::min(w, x + Radius + 1); ++q) {
      const T source = row[q];
      if constexpr (TrackSign) {
        had_sign |= std::signbit(source);
      }
      value = extreme<Maximum, Stable>(value, source);
    }
    target[x] = value;
  };
  for (std::uint64_t x = 0; x < first; ++x) {
    border(x);
  }
  for (auto block = first; block < last; block += 4096) {
    poll(0);
    auto x = block;
    const auto end = std::min(last, block + 4096);
    if constexpr (Radius == 3 && sizeof(T) == 4) {
#if defined(__aarch64__)
      x = horizontal_seven_neon<Maximum, Stable, TrackSign>(row, target, x, end,
                                                            w, &had_sign);
#elif PS_KEY_X86
      if (__builtin_cpu_supports("avx2")) {
        x = horizontal_seven_avx2<Maximum>(row, target, x, end, w);
      }
#endif
    }
    for (; x < end; ++x) {
      T value = row[x - Radius];
      if constexpr (TrackSign) {
        had_sign |= std::signbit(value);
      }
      for (unsigned q = 1; q < kRows; ++q) {
        const T source = row[x - Radius + q];
        if constexpr (TrackSign) {
          had_sign |= std::signbit(source);
        }
        value = extreme<Maximum, Stable>(value, source);
      }
      target[x] = value;
    }
  }
  for (auto x = last; x < w; ++x) {
    border(x);
  }
  return had_sign;
}

template <bool Maximum, unsigned Radius, class T, class Poll>
PS_KEY_INLINE void load_horizontal(const T* source, T* target, std::uint64_t w,
                                   bool* stable, Poll poll) {
#if defined(__aarch64__)
  if constexpr (sizeof(T) == 4 &&
                (Radius == 3 || Radius == 4 || Radius == 5 || Radius == 8)) {
    if (!*stable) {
      if (!horizontal_morphology<Maximum, Radius, false, true>(source, target,
                                                               w, poll)) {
        return;
      }
      *stable = true;
    }
    horizontal_morphology<Maximum, Radius, true>(source, target, w, poll);
    return;
  }
#endif
  if (!*stable) {
    for (std::uint64_t block = 0; block < w; block += 4096) {
      poll(std::min<std::uint64_t>(4096, w - block));
      for (auto x = block; x < std::min(w, block + 4096); ++x) {
        *stable |= std::signbit(source[x]);
      }
    }
  }
  if (*stable) {
    horizontal_morphology<Maximum, Radius, true>(source, target, w, poll);
  } else {
    horizontal_morphology<Maximum, Radius, false>(source, target, w, poll);
  }
}

template <bool Maximum, unsigned Radius, class T, class Poll>
PS_KEY_INLINE void small_morphology_impl(const T* in, T* out, T* temporary,
                                         std::uint64_t h, std::uint64_t w,
                                         Poll poll) {
  constexpr unsigned kRows = 2 * Radius + 1;
  std::uint64_t next = 0;
#if PS_KEY_X86
  bool stable = true;
#else
  bool stable = (Radius != 3 || sizeof(T) != 4) &&
                (w == 1 || (sizeof(T) == 4 && w <= 64));
#endif
  for (std::uint64_t y = 0; y < h; ++y) {
    const auto stop = std::min(h, y + Radius + 1);
    for (; next < stop; ++next) {
      const auto* row = in + next * w;
      auto* target = temporary + (next % kRows) * w;
      load_horizontal<Maximum, Radius>(row, target, w, &stable, poll);
    }
    const auto first = y > Radius ? y - Radius : 0;
    const auto count = stop - first;
    const T* rows[kRows]{};
    for (unsigned q = 0; q < count; ++q) {
      rows[q] = temporary + ((first + q) % kRows) * w;
    }
    const auto vertical = [&](auto mode) PS_KEY_ALWAYS_INLINE {
      constexpr bool kStable = decltype(mode)::value;
      poll(w * kRows);
      for (std::uint64_t block = 0; block < w; block += 4096) {
        poll(0);
        const auto end = std::min(w, block + 4096);
        if (count == kRows) {
          for (auto x = block; x < end; ++x) {
            T value = rows[0][x];
            for (unsigned q = 1; q < kRows; ++q) {
              value = extreme<Maximum, kStable>(value, rows[q][x]);
            }
            out[y * w + x] = value;
          }
        } else {
          for (auto x = block; x < end; ++x) {
            T value = rows[0][x];
            for (unsigned q = 1; q < count; ++q) {
              value = extreme<Maximum, kStable>(value, rows[q][x]);
            }
            out[y * w + x] = value;
          }
        }
      }
    };
    if (stable) {
      vertical(std::true_type{});
    } else {
      vertical(std::false_type{});
    }
  }
}

#if PS_KEY_X86
template <bool Maximum, unsigned Radius, class T, class Poll>
PS_KEY_AVX2 void small_morphology_avx2(const T* in, T* out, T* temporary,
                                       std::uint64_t h, std::uint64_t w,
                                       Poll poll) {
  return small_morphology_impl<Maximum, Radius>(in, out, temporary, h, w, poll);
}
#endif
template <bool Maximum, unsigned Radius, class T, class Poll>
void small_morphology(const T* in, T* out, T* temporary, std::uint64_t h,
                      std::uint64_t w, Poll poll) {
#if PS_KEY_X86
  if (__builtin_cpu_supports("avx2")) {
    return small_morphology_avx2<Maximum, Radius>(in, out, temporary, h, w,
                                                  poll);
  }
#endif
  return small_morphology_impl<Maximum, Radius>(in, out, temporary, h, w, poll);
}

#if defined(__aarch64__)
template <bool Maximum, bool Stable, unsigned Begin, unsigned End>
PS_KEY_INLINE float32x4_t ordered_rows_neon(const float* const* rows,
                                            std::uint64_t x) {
  if constexpr (End == Begin + 1) {
    return vld1q_f32(rows[Begin] + x);
  } else {
    constexpr unsigned middle = (Begin + End) / 2;
    const auto earlier =
        ordered_rows_neon<Maximum, Stable, Begin, middle>(rows, x);
    const auto later = ordered_rows_neon<Maximum, Stable, middle, End>(rows, x);
    if constexpr (Stable) {
      return vbslq_f32(
          Maximum ? vcgtq_f32(later, earlier) : vcltq_f32(later, earlier),
          later, earlier);
    }
    return Maximum ? vmaxq_f32(earlier, later) : vminq_f32(earlier, later);
  }
}
template <bool Maximum, bool Stable, unsigned Radius = 3>
inline std::uint64_t four_window_vertical_neon(const float* const* rows,
                                               float* const* output,
                                               std::uint64_t x,
                                               std::uint64_t end) {
  const auto combine = [](float32x4_t earlier, float32x4_t later) {
    if constexpr (Stable) {
      return vbslq_f32(
          Maximum ? vcgtq_f32(later, earlier) : vcltq_f32(later, earlier),
          later, earlier);
    }
    return Maximum ? vmaxq_f32(earlier, later) : vminq_f32(earlier, later);
  };
  for (; x + 4 <= end; x += 4) {
    const auto h2 = vld1q_f32(rows[2] + x);
    const auto common =
        ordered_rows_neon<Maximum, Stable, 3, 2 * Radius + 1>(rows, x);
    const auto left2 = combine(h2, common);
    const auto left1 = combine(vld1q_f32(rows[1] + x), left2);
    vst1q_f32(output[0] + x, combine(vld1q_f32(rows[0] + x), left1));
    const auto h7 = vld1q_f32(rows[2 * Radius + 1] + x);
    vst1q_f32(output[1] + x, combine(left1, h7));
    const auto right7 = combine(common, h7);
    const auto right8 = combine(right7, vld1q_f32(rows[2 * Radius + 2] + x));
    vst1q_f32(output[2] + x, combine(h2, right8));
    vst1q_f32(output[3] + x,
              combine(right8, vld1q_f32(rows[2 * Radius + 3] + x)));
  }
  return x;
}
#endif
#if PS_KEY_X86
template <bool Maximum, unsigned Begin, unsigned End>
PS_KEY_AVX2 inline __m256 ordered_rows_avx2(const float* const* rows,
                                            std::uint64_t x) {
  if constexpr (End == Begin + 1) {
    return _mm256_loadu_ps(rows[Begin] + x);
  } else {
    constexpr unsigned middle = (Begin + End) / 2;
    const auto earlier = ordered_rows_avx2<Maximum, Begin, middle>(rows, x);
    const auto later = ordered_rows_avx2<Maximum, middle, End>(rows, x);
    return stable_extreme8<Maximum>(earlier, later);
  }
}
template <bool Maximum, unsigned Radius = 3>
PS_KEY_AVX2 std::uint64_t four_window_vertical_avx2(const float* const* rows,
                                                    float* const* output,
                                                    std::uint64_t x,
                                                    std::uint64_t end) {
  for (; x + 8 <= end; x += 8) {
    const auto h2 = _mm256_loadu_ps(rows[2] + x);
    __m256 common;
    if constexpr (Maximum) {
      common = _mm256_loadu_ps(rows[3] + x);
      for (unsigned row = 4; row < 2 * Radius + 1; ++row) {
        common =
            stable_extreme8<Maximum>(common, _mm256_loadu_ps(rows[row] + x));
      }
    } else {
      common = ordered_rows_avx2<Maximum, 3, 2 * Radius + 1>(rows, x);
    }
    const auto left2 = stable_extreme8<Maximum>(h2, common);
    const auto left1 =
        stable_extreme8<Maximum>(_mm256_loadu_ps(rows[1] + x), left2);
    _mm256_storeu_ps(output[0] + x, stable_extreme8<Maximum>(
                                        _mm256_loadu_ps(rows[0] + x), left1));
    const auto h7 = _mm256_loadu_ps(rows[2 * Radius + 1] + x);
    _mm256_storeu_ps(output[1] + x, stable_extreme8<Maximum>(left1, h7));
    const auto right7 = stable_extreme8<Maximum>(common, h7);
    const auto right8 = stable_extreme8<Maximum>(
        right7, _mm256_loadu_ps(rows[2 * Radius + 2] + x));
    _mm256_storeu_ps(output[2] + x, stable_extreme8<Maximum>(h2, right8));
    _mm256_storeu_ps(output[3] + x,
                     stable_extreme8<Maximum>(
                         right8, _mm256_loadu_ps(rows[2 * Radius + 3] + x)));
  }
  return x;
}
#endif

// Four neighboring vertical windows share their central 2*Radius-2 rows.
template <bool Maximum, unsigned Radius = 3, class T, class Poll>
PS_KEY_INLINE void four_window_morphology_impl(const T* in, T* out,
                                               T* temporary, std::uint64_t h,
                                               std::uint64_t w, Poll poll) {
  constexpr unsigned kRows = 2 * Radius + 1, kRing = kRows + 3;
  auto stride = w;
  const auto misalignment = reinterpret_cast<std::uintptr_t>(temporary) & 15;
  const auto offset = ((16 - misalignment) & 15) / sizeof(T);
  const auto padded = ((w + 3) & ~std::uint64_t{3}) + 16;
  if (offset + kRing * padded <= h * w) {
    temporary += offset;
    stride = padded;
  }
  std::uint64_t loaded = 0;
#if PS_KEY_X86
  bool stable = true;
#else
  bool stable = false;
#endif
  const auto load_until = [&](std::uint64_t stop) PS_KEY_ALWAYS_INLINE {
    for (; loaded < stop; ++loaded) {
      const auto* source = in + loaded * w;
      auto* target = temporary + (loaded % kRing) * stride;
      load_horizontal<Maximum, Radius>(source, target, w, &stable, poll);
    }
  };
  const auto boundary = [&](std::uint64_t y) PS_KEY_ALWAYS_INLINE {
    const auto first = y > Radius ? y - Radius : 0,
               stop = std::min(h, y + Radius + 1);
    load_until(stop);
    const T* rows[kRows]{};
    for (auto row = first; row < stop; ++row) {
      rows[row - first] = temporary + (row % kRing) * stride;
    }
    const auto vertical = [&](auto mode) PS_KEY_ALWAYS_INLINE {
      constexpr bool kStable = decltype(mode)::value;
      for (std::uint64_t block = 0; block < w; block += 4096) {
        const auto end = std::min(w, block + 4096);
        poll((end - block) * kRows);
        for (auto x = block; x < end; ++x) {
          T value = rows[0][x];
          for (unsigned row = 1; row < stop - first; ++row) {
            value = extreme<Maximum, kStable>(value, rows[row][x]);
          }
          out[y * w + x] = value;
        }
      }
    };
    if (stable) {
      vertical(std::true_type{});
    } else {
      vertical(std::false_type{});
    }
  };
  // Repeating the first or last row preserves the clipped extremum and its
  // earliest tie. The AVX2 radius-eight packet also covers those border rows.
#if PS_KEY_X86
  const bool clipped_packets =
      Radius == 8 && w >= 8 && __builtin_cpu_supports("avx2");
#else
  constexpr bool clipped_packets = false;
#endif
  std::uint64_t y = 0;
  for (; !clipped_packets && y < Radius; ++y) {
    boundary(y);
  }
  for (; y + 3 < h && (clipped_packets || y + Radius + 3 < h); y += 4) {
    load_until(std::min(h, y + Radius + 4));
    const T* rows[kRing]{};
    T* output[4]{};
    for (unsigned row = 0; row < kRing; ++row) {
      const auto index =
          clipped_packets
              ? std::min(h - 1, y + row > Radius ? y + row - Radius : 0)
              : y - Radius + row;
      rows[row] = temporary + (index % kRing) * stride;
    }
    for (unsigned row = 0; row < 4; ++row) {
      output[row] = out + (y + row) * w;
    }
    const auto vertical = [&](auto mode) PS_KEY_ALWAYS_INLINE {
      constexpr bool kStable = decltype(mode)::value;
      for (std::uint64_t block = 0; block < w; block += 1024) {
        const auto end = std::min(w, block + 1024);
        poll((end - block) * (kRows + 9));
        auto x = block;
#if defined(__aarch64__)
        x = four_window_vertical_neon<Maximum, kStable, Radius>(rows, output, x,
                                                                end);
#elif PS_KEY_X86
        if (__builtin_cpu_supports("avx2")) {
          x = four_window_vertical_avx2<Maximum, Radius>(rows, output, x, end);
          if (clipped_packets && x < end) {
            four_window_vertical_avx2<Maximum, Radius>(rows, output, end - 8,
                                                       end);
            x = end;
          }
        }
#endif
        for (; x < end; ++x) {
          const T h2 = rows[2][x];
          T common = rows[3][x];
          for (unsigned row = 4; row < kRows; ++row) {
            common = extreme<Maximum, kStable>(common, rows[row][x]);
          }
          const T left2 = extreme<Maximum, kStable>(h2, common);
          const T left1 = extreme<Maximum, kStable>(rows[1][x], left2);
          output[0][x] = extreme<Maximum, kStable>(rows[0][x], left1);
          const T h7 = rows[kRows][x];
          output[1][x] = extreme<Maximum, kStable>(left1, h7);
          const T right7 = extreme<Maximum, kStable>(common, h7);
          const T right8 =
              extreme<Maximum, kStable>(right7, rows[kRows + 1][x]);
          output[2][x] = extreme<Maximum, kStable>(h2, right8);
          output[3][x] = extreme<Maximum, kStable>(right8, rows[kRows + 2][x]);
        }
      }
    };
    if (stable) {
      vertical(std::true_type{});
    } else {
      vertical(std::false_type{});
    }
  }
  for (; y < h; ++y) {
    boundary(y);
  }
}
#if PS_KEY_X86
template <bool Maximum, unsigned Radius = 3, class T, class Poll>
PS_KEY_AVX2 void four_window_morphology_avx2(const T* in, T* out, T* temporary,
                                             std::uint64_t h, std::uint64_t w,
                                             Poll poll) {
  return four_window_morphology_impl<Maximum, Radius>(in, out, temporary, h, w,
                                                      poll);
}
#endif
template <bool Maximum, unsigned Radius = 3, class T, class Poll>
void four_window_morphology(const T* in, T* out, T* temporary, std::uint64_t h,
                            std::uint64_t w, Poll poll) {
#if PS_KEY_X86
  if (__builtin_cpu_supports("avx2")) {
    return four_window_morphology_avx2<Maximum, Radius>(in, out, temporary, h,
                                                        w, poll);
  }
#endif
  return four_window_morphology_impl<Maximum, Radius>(in, out, temporary, h, w,
                                                      poll);
}

#if PS_KEY_X86
template <bool Maximum, unsigned Stage>
PS_KEY_AVX2 std::uint64_t rolling_vertical_avx2(float* target, float* previous,
                                                float* old_pair,
                                                float* old_quad,
                                                std::uint64_t x,
                                                std::uint64_t end) {
  const auto neutral =
      _mm256_set1_ps(Maximum ? -std::numeric_limits<float>::infinity()
                             : std::numeric_limits<float>::infinity());
  for (; x + 8 <= end; x += 8) {
    const auto value = Stage == 2 ? neutral : _mm256_loadu_ps(target + x);
    const auto pair =
        stable_extreme8<Maximum>(_mm256_loadu_ps(previous + x), value);
    const auto quad =
        stable_extreme8<Maximum>(_mm256_loadu_ps(old_pair + x), pair);
    if constexpr (Stage != 0) {
      _mm256_storeu_ps(target + x, stable_extreme8<Maximum>(
                                       _mm256_loadu_ps(old_quad + x), quad));
    }
    _mm256_storeu_ps(previous + x, value);
    _mm256_storeu_ps(old_pair + x, pair);
    _mm256_storeu_ps(old_quad + x, quad);
  }
  return x;
}

// A seven-row window is the union of two four-row windows separated by
// three rows. Streaming min2 -> min4 -> min7 reuses each partial reduction.
template <bool Maximum, class T, class Poll>
PS_KEY_INLINE void rolling_seven_morphology_impl(const T* in, T* out,
                                                 T* temporary, std::uint64_t h,
                                                 std::uint64_t w, Poll poll) {
  const T neutral = Maximum ? -std::numeric_limits<T>::infinity()
                            : std::numeric_limits<T>::infinity();
  auto* previous = temporary;
  auto* pairs = previous + w;
  auto* quads = pairs + 2 * w;
  poll(6 * w);
  fill_elements(temporary, 6 * w, neutral, poll);
#if PS_KEY_X86
  bool stable = true;
#else
  bool stable = w <= 64;
#endif
  for (std::uint64_t tick = 3; tick < h + 6; ++tick) {
    const auto input_row = tick - 3;
    const bool real = input_row < h, ready = tick >= 6;
    auto* target = out + (ready ? tick - 6 : 0) * w;
    if (real) {
      const auto* source = in + input_row * w;
      if (!stable) {
        for (std::uint64_t block = 0; block < w; block += 4096) {
          poll(std::min<std::uint64_t>(4096, w - block));
          for (auto x = block; x < std::min(w, block + 4096); ++x) {
            stable |= std::signbit(source[x]);
          }
        }
      }
      if (stable) {
        horizontal_morphology<Maximum, 3, true>(source, target, w, poll);
      } else {
        horizontal_morphology<Maximum, 3, false>(source, target, w, poll);
      }
    }
    auto* old_pair = pairs + (tick % 2) * w;
    auto* old_quad = quads + (tick % 3) * w;
    const auto vertical = [&](auto mode) PS_KEY_ALWAYS_INLINE {
      constexpr bool kStable = decltype(mode)::value;
      for (std::uint64_t block = 0; block < w; block += 4096) {
        const auto end = std::min(w, block + 4096);
        poll((end - block) * 8);
        auto x = block;
        if (__builtin_cpu_supports("avx2")) {
          if (!ready) {
            x = rolling_vertical_avx2<Maximum, 0>(target, previous, old_pair,
                                                  old_quad, x, end);
          } else if (real) {
            x = rolling_vertical_avx2<Maximum, 1>(target, previous, old_pair,
                                                  old_quad, x, end);
          } else {
            x = rolling_vertical_avx2<Maximum, 2>(target, previous, old_pair,
                                                  old_quad, x, end);
          }
        }
        for (; x < end; ++x) {
          const T value = real ? target[x] : neutral;
          const T pair = extreme<Maximum, kStable>(previous[x], value);
          const T quad = extreme<Maximum, kStable>(old_pair[x], pair);
          if (ready) {
            target[x] = extreme<Maximum, kStable>(old_quad[x], quad);
          }
          previous[x] = value;
          old_pair[x] = pair;
          old_quad[x] = quad;
        }
      }
    };
    if (stable) {
      vertical(std::true_type{});
    } else {
      vertical(std::false_type{});
    }
  }
}
#if PS_KEY_X86
template <bool Maximum, class T, class Poll>
PS_KEY_AVX2 void rolling_seven_morphology_avx2(const T* in, T* out,
                                               T* temporary, std::uint64_t h,
                                               std::uint64_t w, Poll poll) {
  return rolling_seven_morphology_impl<Maximum>(in, out, temporary, h, w, poll);
}
#endif
template <bool Maximum, class T, class Poll>
void rolling_seven_morphology(const T* in, T* out, T* temporary,
                              std::uint64_t h, std::uint64_t w, Poll poll) {
#if PS_KEY_X86
  if (__builtin_cpu_supports("avx2")) {
    return rolling_seven_morphology_avx2<Maximum>(in, out, temporary, h, w,
                                                  poll);
  }
#endif
  return rolling_seven_morphology_impl<Maximum>(in, out, temporary, h, w, poll);
}

#endif

template <bool Maximum, class T, class Poll>
PS_KEY_INLINE void large_morphology_impl(const T* in, T* out, T* temporary,
                                         std::uint64_t h, std::uint64_t w,
                                         std::uint64_t radius, Poll poll) {
  const auto length = 2 * radius + 1;
  const auto last_column_block = ((w - 1) / length) * length;
  const auto last_row_block = ((h - 1) / length) * length;
  poll(h * w * 12);
  for (std::uint64_t y = 0; y < h; ++y) {
    const auto row = y * w;
    for (std::uint64_t begin = 0; begin < w; begin += length) {
      const auto end = std::min(w, begin + length);
      poll(0);
      out[row + begin] = in[row + begin];
      for (auto x = begin + 1; x < end; ++x) {
        if ((x & 4095) == 0) {
          poll(0);
        }
        out[row + x] = extreme<Maximum>(out[row + x - 1], in[row + x]);
      }
      poll(0);
      temporary[row + end - 1] = in[row + end - 1];
      for (auto x = end - 1; x-- > begin;) {
        if ((x & 4095) == 0) {
          poll(0);
        }
        temporary[row + x] =
            extreme<Maximum>(in[row + x], temporary[row + x + 1]);
      }
    }
    for (std::uint64_t block = 0; block < w; block += 4096) {
      poll(0);
      for (auto x = block; x < std::min(w, block + 4096); ++x) {
        const auto lo = x > radius ? x - radius : 0;
        const auto hi = std::min(w - 1, x + radius);
        const T value =
            x < radius ? out[row + hi]
            : x + radius >= w && lo >= last_column_block
                ? temporary[row + lo]
                : extreme<Maximum>(temporary[row + lo], out[row + hi]);
        out[row + x] = value;
      }
    }
  }
  for (std::uint64_t y = 0; y < h; ++y) {
    if (y % length == 0) {
      copy_elements(temporary + y * w, out + y * w, w, poll);
      continue;
    }
    for (std::uint64_t block = 0; block < w; block += 4096) {
      poll(0);
      for (auto x = block; x < std::min(w, block + 4096); ++x) {
        temporary[y * w + x] =
            extreme<Maximum>(temporary[(y - 1) * w + x], out[y * w + x]);
      }
    }
  }
  for (auto y = h; y-- > 0;) {
    if (y + 1 == h || (y + 1) % length == 0) {
      poll(0);
      continue;
    }
    for (std::uint64_t block = 0; block < w; block += 4096) {
      poll(0);
      for (auto x = block; x < std::min(w, block + 4096); ++x) {
        out[y * w + x] = extreme<Maximum>(out[y * w + x], out[(y + 1) * w + x]);
      }
    }
  }
  for (auto y = h; y-- > 0;) {
    const auto lo = y > radius ? y - radius : 0;
    const auto hi = std::min(h - 1, y + radius);
    for (std::uint64_t block = 0; block < w; block += 4096) {
      poll(0);
      for (auto x = block; x < std::min(w, block + 4096); ++x) {
        const T value =
            y < radius ? temporary[hi * w + x]
            : y + radius >= h && lo >= last_row_block
                ? out[lo * w + x]
                : extreme<Maximum>(out[lo * w + x], temporary[hi * w + x]);
        out[y * w + x] = value;
      }
    }
  }
}
#if PS_KEY_X86
template <bool Maximum, class T, class Poll>
PS_KEY_AVX2 void large_morphology_avx2(const T* in, T* out, T* temporary,
                                       std::uint64_t h, std::uint64_t w,
                                       std::uint64_t radius, Poll poll) {
  return large_morphology_impl<Maximum>(in, out, temporary, h, w, radius, poll);
}
#endif
template <bool Maximum, class T, class Poll>
void large_morphology(const T* in, T* out, T* temporary, std::uint64_t h,
                      std::uint64_t w, std::uint64_t radius, Poll poll) {
#if PS_KEY_X86
  if (__builtin_cpu_supports("avx2")) {
    return large_morphology_avx2<Maximum>(in, out, temporary, h, w, radius,
                                          poll);
  }
#endif
  return large_morphology_impl<Maximum>(in, out, temporary, h, w, radius, poll);
}

template <bool Maximum, class T, class Poll>
void morphology(const T* in, T* out, T* temporary, std::uint64_t* queue,
                std::uint64_t h, std::uint64_t w, std::uint64_t radius,
                Poll poll) {
  if (!radius) {
    copy_elements(out, in, h * w, poll);
    return;
  }
  if (w == 1) {
    w = h;
    h = 1;
  }
  if constexpr (sizeof(T) == 4) {
#if PS_KEY_X86
    if (radius == 3 && h >= 6 && w <= 16384 / (6 * sizeof(T))) {
      rolling_seven_morphology<Maximum>(in, out, temporary, h, w, poll);
      return;
    }
#else
    if (radius == 3 && h >= 10) {
      four_window_morphology<Maximum>(in, out, temporary, h, w, poll);
      return;
    }
#endif
  }
  if constexpr (sizeof(T) == 4) {
    if (radius == 4 && h >= 12) {
      four_window_morphology<Maximum, 4>(in, out, temporary, h, w, poll);
      return;
    }
    if (radius == 5 && h >= 14) {
      four_window_morphology<Maximum, 5>(in, out, temporary, h, w, poll);
      return;
    }
    if (radius == 8 && h >= 20) {
      four_window_morphology<Maximum, 8>(in, out, temporary, h, w, poll);
      return;
    }
  }
  if (radius == 5) {
    small_morphology<Maximum, 5>(in, out, temporary, h, w, poll);
    return;
  }
  if (radius == 8) {
    small_morphology<Maximum, 8>(in, out, temporary, h, w, poll);
    return;
  }
  if (radius <= 4) {
    switch (radius) {
      case 1:
        small_morphology<Maximum, 1>(in, out, temporary, h, w, poll);
        break;
      case 2:
        small_morphology<Maximum, 2>(in, out, temporary, h, w, poll);
        break;
      case 3:
        small_morphology<Maximum, 3>(in, out, temporary, h, w, poll);
        break;
      default:
        small_morphology<Maximum, 4>(in, out, temporary, h, w, poll);
        break;
    }
  } else {
    (void)queue;
    large_morphology<Maximum>(in, out, temporary, h, w, radius, poll);
  }
}

// The first integer x where q is preferred over p. Integer arithmetic also
// resolves an exact distance tie by the original donor's row-major index.
template <class Wide>
inline std::int64_t intersection(std::int64_t p, std::int64_t q,
                                 std::int64_t donor_p, std::int64_t donor_q,
                                 std::uint64_t y, std::uint64_t w) {
  const Wide yp = Wide(y) - donor_p;
  const Wide yq = Wide(y) - donor_q;
  const Wide numerator = Wide(q) * q + yq * yq - Wide(p) * p - yp * yp;
  const Wide denominator = 2 * Wide(q - p);
  Wide cut = numerator / denominator;
  if (numerator < 0 && numerator % denominator) {
    --cut;
  }
  if (donor_q < donor_p && numerator % denominator == 0) {
    --cut;
  }
  ++cut;
  return static_cast<std::int64_t>(std::max(Wide(-1), std::min(Wide(w), cut)));
}
template <class Wide, class Index, class Poll>
PS_KEY_INLINE bool nearest_impl(const std::uint8_t* known, Index* sites,
                                Index* envelope, Index* starts, std::uint64_t h,
                                std::uint64_t w, Poll poll,
                                std::uint8_t* certificates = nullptr) {
  bool any = false;
  for (std::uint64_t block = 0; block < w; block += 4096) {
    poll(std::min<std::uint64_t>(4096, w - block) * 2);
    for (auto x = block; x < std::min(w, block + 4096); ++x) {
      sites[x] = known[x] ? 0 : -1;
      any |= known[x] != 0;
    }
  }
  for (std::uint64_t y = 1; y < h; ++y) {
    for (std::uint64_t block = 0; block < w; block += 4096) {
      poll(std::min<std::uint64_t>(4096, w - block) * 2);
      for (auto x = block; x < std::min(w, block + 4096); ++x) {
        const auto i = y * w + x;
        sites[i] = known[i] ? static_cast<Index>(y) : sites[i - w];
        any |= known[i] != 0;
      }
    }
  }
  if (!any) {
    return false;
  }
  for (std::uint64_t y = h - 1; y-- > 0;) {
    for (std::uint64_t block = 0; block < w; block += 4096) {
      poll(std::min<std::uint64_t>(4096, w - block) * 2);
      for (auto x = block; x < std::min(w, block + 4096); ++x) {
        const auto i = y * w + x;
        const auto upper = sites[i], lower = sites[i + w];
        const auto row = static_cast<Index>(y);
        sites[i] = lower > row && (upper < 0 || lower - row < row - upper)
                       ? lower
                       : upper;
      }
    }
  }
  if (w == 1) {
    return true;
  }
  for (std::uint64_t y = 0; y < h; ++y) {
    poll(w * 8);
    std::int64_t count = 0;
    std::uint64_t intersections = 0;
    // Copy this row because output replaces its vertical candidate array.
    auto* donors = starts + w;
    // Preserve each vertical candidate while encoding its local cost below.
    // A donor at squared distance < 25 is certified after checking columns
    // x-4..x+4. A bounded repair certifies isolated gaps below distance 9.
    // Other rows retain the linear lower-envelope transform below.
    bool certified = true;
    std::uint8_t certified_radius = 5;
    poll(w * 96);
    // Encode each column's vertical cost once. The candidate loop then needs
    // only constant horizontal offsets and integer minima. The envelope
    // storage is unused until the exact fallback replaces it.
    auto* nearby = reinterpret_cast<std::int32_t*>(envelope);
    for (std::uint64_t block = 0; block < w; block += 4096) {
      poll(0);
      for (auto x = block; x < std::min(w, block + 4096); ++x) {
        const auto donor = sites[y * w + x];
        donors[x] = donor;
        const auto delta = donor - static_cast<std::int64_t>(y);
        const auto dy = static_cast<std::int32_t>(
            std::clamp(delta, INT64_C(-5), INT64_C(5)));
        nearby[x] = ((donor >= 0) & (dy > -5) & (dy < 5))
                        ? dy * dy * 256 + (dy + 4) * 16 + 4
                        : INT32_C(6400);
      }
    }
    const auto finish = [&](std::uint64_t x, std::int32_t score) {
      const auto rank = static_cast<std::uint32_t>(score) & 255U;
      if constexpr (sizeof(Index) == 4) {
        // Unsigned offsets wrap back to the valid positive linear index.
        const auto bits = static_cast<std::uint32_t>(y * w) +
                          static_cast<std::uint32_t>(x) +
                          ((rank >> 4) - 4U) * static_cast<std::uint32_t>(w) +
                          (rank & 15U) - 4U;
        sites[y * w + x] = static_cast<Index>(bits & UINT32_C(0x7fffffff));
      } else {
        sites[y * w + x] = (static_cast<std::int64_t>(y) + (rank >> 4) - 4) *
                               static_cast<std::int64_t>(w) +
                           static_cast<std::int64_t>(x) + (rank & 15) - 4;
      }
    };
    const auto repair = [&](std::uint64_t x) {
      poll(17 * 12);
      std::int32_t best = 81 * 1024;
      std::uint64_t chosen = 0;
      const auto first_q = x > 8 ? x - 8 : 0;
      const auto last_q = std::min(w, x + 9);
      for (auto q = first_q; q < last_q; ++q) {
        const auto donor = donors[q];
        const auto dy = donor - static_cast<std::int64_t>(y);
        if (donor < 0 || dy < -8 || dy > 8) {
          continue;
        }
        const auto dx =
            static_cast<std::int64_t>(q) - static_cast<std::int64_t>(x);
        const auto score = static_cast<std::int32_t>(
            1024 * (dy * dy + dx * dx) + 32 * (dy + 8) + dx + 8);
        if (score < best) {
          best = score;
          chosen = static_cast<std::uint64_t>(donor) * w + q;
        }
      }
      // Every unvisited candidate has squared distance >= 81. The low
      // ten bits order exact ties by donor row, then donor column.
      if (best >= 81 * 1024) {
        return false;
      }
      const auto distance = best / 1024;
      while (certified_radius * certified_radius < distance) {
        ++certified_radius;
      }
      sites[y * w + x] = static_cast<Index>(chosen);
      return true;
    };
    const auto score_at = [&](std::uint64_t q, std::int32_t dx) {
      return nearby[q] + 256 * dx * dx + dx;
    };
    const auto border = [&](std::uint64_t x) {
      std::int32_t score = 6400;
      for (std::int32_t delta = -4; delta <= 4; ++delta) {
        if (static_cast<std::int64_t>(x) + delta >= 0 &&
            static_cast<std::int64_t>(x) + delta <
                static_cast<std::int64_t>(w)) {
          score = std::min(score, score_at(x + delta, delta));
        }
      }
      if (score < 6400) {
        finish(x, score);
      } else {
        certified &= repair(x);
      }
    };
    const auto first = std::min<std::uint64_t>(4, w);
    const auto last = w > 4 ? std::max(first, w - 4) : first;
    for (std::uint64_t x = 0; x < first; ++x) {
      border(x);
    }
    for (auto block = first; block < last && certified; block += 4096) {
      poll(0);
      const auto end = std::min(last, block + 4096);
      auto x = block;
#if defined(__aarch64__)
      if constexpr (sizeof(Index) == 4) {
        for (; x + 16 <= end; x += 16) {
          auto s0 = vdupq_n_s32(6400), s1 = s0, s2 = s0, s3 = s0;
          const auto consider = [&](auto displacement) PS_KEY_ALWAYS_INLINE {
            constexpr auto dx = decltype(displacement)::value;
            const auto offset = vdupq_n_s32(256 * dx * dx + dx);
            s0 = vminq_s32(s0, vaddq_s32(vld1q_s32(nearby + x + dx), offset));
            s1 = vminq_s32(s1,
                           vaddq_s32(vld1q_s32(nearby + x + 4 + dx), offset));
            s2 = vminq_s32(s2,
                           vaddq_s32(vld1q_s32(nearby + x + 8 + dx), offset));
            s3 = vminq_s32(s3,
                           vaddq_s32(vld1q_s32(nearby + x + 12 + dx), offset));
          };
          const auto maximum = [&]() PS_KEY_ALWAYS_INLINE {
            return vmaxvq_s32(vmaxq_s32(vmaxq_s32(s0, s1), vmaxq_s32(s2, s3)));
          };
          consider(std::integral_constant<int, -3>{});
          consider(std::integral_constant<int, -2>{});
          consider(std::integral_constant<int, -1>{});
          consider(std::integral_constant<int, 0>{});
          consider(std::integral_constant<int, 1>{});
          consider(std::integral_constant<int, 2>{});
          consider(std::integral_constant<int, 3>{});
          // The omitted columns cannot tie a squared distance below 16.
          if (maximum() >= 4096) {
            consider(std::integral_constant<int, -4>{});
            consider(std::integral_constant<int, 4>{});
            if (maximum() >= 6400) {
              std::int32_t scores[16];
              vst1q_s32(scores, s0);
              vst1q_s32(scores + 4, s1);
              vst1q_s32(scores + 8, s2);
              vst1q_s32(scores + 12, s3);
              for (std::uint64_t lane = 0; lane < 16; ++lane) {
                if (scores[lane] < 6400) {
                  finish(x + lane, scores[lane]);
                } else if (!repair(x + lane)) {
                  certified = false;
                  break;
                }
              }
              if (!certified) {
                break;
              }
              continue;
            }
          }
          const auto publish = [&](std::uint64_t xg,
                                   int32x4_t score) PS_KEY_ALWAYS_INLINE {
            const auto rank =
                vandq_u32(vreinterpretq_u32_s32(score), vdupq_n_u32(255));
            const uint32x4_t lanes = {0, 1, 2, 3};
            const auto base = vaddq_u32(
                vdupq_n_u32(static_cast<std::uint32_t>(y * w + xg)), lanes);
            auto bits = vaddq_u32(base, vandq_u32(rank, vdupq_n_u32(15)));
            bits = vmlaq_n_u32(bits,
                               vsubq_u32(vshrq_n_u32(rank, 4), vdupq_n_u32(4)),
                               static_cast<std::uint32_t>(w));
            bits = vandq_u32(vsubq_u32(bits, vdupq_n_u32(4)),
                             vdupq_n_u32(UINT32_C(0x7fffffff)));
            vst1q_s32(sites + y * w + xg, vreinterpretq_s32_u32(bits));
          };
          publish(x, s0);
          publish(x + 4, s1);
          publish(x + 8, s2);
          publish(x + 12, s3);
        }
      }
#endif
      if (!certified) {
        break;
      }
      for (; x < end; ++x) {
        std::int32_t score = 6400;
        for (std::int32_t delta = -4; delta <= 4; ++delta) {
          score = std::min(score, score_at(x + delta, delta));
        }
        if (score < 6400) {
          finish(x, score);
        } else if (!repair(x)) {
          certified = false;
          break;
        }
      }
    }
    for (auto x = last; x < w; ++x) {
      border(x);
    }
    if (certified) {
      if (certificates) {
        certificates[y] = certified_radius;
      }
      continue;
    }
    for (std::int64_t q = 0; q < static_cast<std::int64_t>(w); ++q) {
      if ((q & 4095) == 0) {
        poll(0);
      }
      if (donors[q] < 0) {
        continue;
      }
      std::int64_t cut = -1;
      while (count) {
        if ((++intersections & 4095) == 0) {
          poll(0);
        }
        const auto p = envelope[count - 1];
        cut = intersection<Wide>(p, q, donors[p], donors[q], y, w);
        if (cut > starts[count - 1]) {
          break;
        }
        --count;
      }
      envelope[count] = q;
      starts[count] = count ? cut : -1;
      ++count;
    }
    std::int64_t active = 0;
    for (std::uint64_t block = 0; block < w; block += 4096) {
      poll(0);
      for (std::uint64_t x = block; x < std::min(w, block + 4096); ++x) {
        while (active + 1 < count &&
               starts[active + 1] <= static_cast<std::int64_t>(x)) {
          ++active;
        }
        sites[y * w + x] =
            donors[envelope[active]] * static_cast<std::int64_t>(w) +
            envelope[active];
      }
    }
  }
  return true;
}

#if PS_KEY_X86
template <class Wide, class Index, class Poll>
PS_KEY_AVX2 bool nearest_typed_avx2(const std::uint8_t* known, Index* sites,
                                    Index* envelope, Index* starts,
                                    std::uint64_t h, std::uint64_t w, Poll poll,
                                    std::uint8_t* certificates) {
  return nearest_impl<Wide>(known, sites, envelope, starts, h, w, poll,
                            certificates);
}
#endif
template <class Wide, class Index, class Poll>
bool nearest_typed(const std::uint8_t* known, Index* sites, Index* envelope,
                   Index* starts, std::uint64_t h, std::uint64_t w, Poll poll,
                   std::uint8_t* certificates) {
#if PS_KEY_X86
  if (__builtin_cpu_supports("avx2")) {
    return nearest_typed_avx2<Wide>(known, sites, envelope, starts, h, w, poll,
                                    certificates);
  }
#endif
  return nearest_impl<Wide>(known, sites, envelope, starts, h, w, poll,
                            certificates);
}

// Compact coordinates for ordinary images; expand in reverse so scratch can
// be shared without an additional full-image allocation.
template <class Wide, class Poll>
PS_KEY_INLINE bool nearest_scalar(const std::uint8_t* known,
                                  std::int64_t* sites, std::int64_t* envelope,
                                  std::int64_t* starts, std::uint64_t h,
                                  std::uint64_t w, Poll poll) {
  if (h * w <= INT32_MAX) {
    auto* compact = reinterpret_cast<std::int32_t*>(sites);
    const bool found = nearest_impl<Wide>(
        known, compact, reinterpret_cast<std::int32_t*>(envelope),
        reinterpret_cast<std::int32_t*>(starts), h, w, poll);
    if (!found) {
      return false;
    }
    for (std::uint64_t end = h * w; end;) {
      const auto begin = end > 4096 ? end - 4096 : 0;
      poll(end - begin);
      while (end > begin) {
        --end;
        std::int32_t value;
        std::memcpy(&value,
                    reinterpret_cast<const unsigned char*>(sites) + 4 * end, 4);
        sites[end] = value;
      }
    }
    return true;
  }
  return nearest_impl<Wide>(known, sites, envelope, starts, h, w, poll);
}

#if PS_KEY_X86
template <class Wide, class Poll>
PS_KEY_AVX2 bool nearest_avx2(const std::uint8_t* known, std::int64_t* sites,
                              std::int64_t* envelope, std::int64_t* starts,
                              std::uint64_t h, std::uint64_t w, Poll poll) {
  return nearest_scalar<Wide>(known, sites, envelope, starts, h, w, poll);
}
#endif
template <class Wide, class Poll>
bool nearest(const std::uint8_t* known, std::int64_t* sites,
             std::int64_t* envelope, std::int64_t* starts, std::uint64_t h,
             std::uint64_t w, Poll poll) {
#if PS_KEY_X86
  if (__builtin_cpu_supports("avx2")) {
    return nearest_avx2<Wide>(known, sites, envelope, starts, h, w, poll);
  }
#endif
  return nearest_scalar<Wide>(known, sites, envelope, starts, h, w, poll);
}

// The binary flood uses one bit per pixel. Pack and expand complete words
// with SIMD; incomplete final words keep the scalar bounds checks.
inline std::uint64_t pack_zeros64(const std::uint8_t* input) {
  std::uint64_t result = 0;
#if defined(__aarch64__)
  const uint8x8_t weights = {1, 2, 4, 8, 16, 32, 64, 128};
  for (unsigned block = 0; block < 4; ++block) {
    const auto zero = vceqq_u8(vld1q_u8(input + 16 * block), vdupq_n_u8(0));
    const auto lo = vaddv_u8(vand_u8(vget_low_u8(zero), weights));
    const auto hi = vaddv_u8(vand_u8(vget_high_u8(zero), weights));
    result |= (std::uint64_t{lo} | (std::uint64_t{hi} << 8)) << (16 * block);
  }
#elif PS_KEY_X86
  for (unsigned block = 0; block < 4; ++block) {
    const auto values =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(input + 16 * block));
    const auto mask =
        _mm_movemask_epi8(_mm_cmpeq_epi8(values, _mm_setzero_si128()));
    result |= static_cast<std::uint64_t>(mask) << (16 * block);
  }
#else
  for (unsigned bit = 0; bit < 64; ++bit) {
    result |= static_cast<std::uint64_t>(input[bit] == 0) << bit;
  }
#endif
  return result;
}

inline void unpack_exterior64(std::uint8_t* output, std::uint64_t bits) {
#if defined(__aarch64__)
  const uint8x16_t weights = {1, 2, 4, 8, 16, 32, 64, 128,
                              1, 2, 4, 8, 16, 32, 64, 128};
  for (unsigned block = 0; block < 4; ++block) {
    const auto values = vcombine_u8(
        vdup_n_u8(static_cast<std::uint8_t>(bits >> (16 * block))),
        vdup_n_u8(static_cast<std::uint8_t>(bits >> (16 * block + 8))));
    const auto exterior = vshrq_n_u8(vtstq_u8(values, weights), 7);
    vst1q_u8(output + 16 * block, vsubq_u8(vdupq_n_u8(1), exterior));
  }
#elif PS_KEY_X86
  const auto weights =
      _mm_setr_epi8(1, 2, 4, 8, 16, 32, 64, -128, 1, 2, 4, 8, 16, 32, 64, -128);
  for (unsigned block = 0; block < 4; ++block) {
    const auto lo = _mm_set1_epi8(static_cast<char>(bits >> (16 * block)));
    const auto hi = _mm_set1_epi8(static_cast<char>(bits >> (16 * block + 8)));
    const auto values = _mm_unpacklo_epi64(lo, hi);
    const auto inside =
        _mm_cmpeq_epi8(_mm_and_si128(values, weights), _mm_setzero_si128());
    _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 16 * block),
                     _mm_and_si128(inside, _mm_set1_epi8(1)));
  }
#else
  for (unsigned bit = 0; bit < 64; ++bit) {
    output[bit] = ((bits >> bit) & 1) ? 0 : 1;
  }
#endif
}

template <class Poll>
void fill_holes(std::uint8_t* state, std::uint64_t* memory, std::uint64_t h,
                std::uint64_t w, Poll poll) {
  if (h <= 1 || w <= 1) {
    return;
  }
  const auto columns = w / 64 + (w % 64 != 0), words = h * columns;
  auto* available = memory;
  auto* reached = available + words;
  auto* queue = reached + words;
  auto* queued = reinterpret_cast<std::uint8_t*>(queue + words);
  std::uint64_t first = 0, last = 0, count = 0;
  const auto enqueue = [&](std::uint64_t id) {
    if (queued[id]) {
      return;
    }
    queued[id] = 1;
    queue[last] = id;
    if (++last == words) {
      last = 0;
    }
    ++count;
  };
  for (std::uint64_t y = 0; y < h; ++y) {
    for (std::uint64_t c = 0; c < columns; ++c) {
      if ((c & 63) == 0) {
        poll(0);
      }
      const auto id = y * columns + c, start = c * 64;
      std::uint64_t mask = 0;
      if (start + 64 <= w) {
        mask = pack_zeros64(state + y * w + start);
      } else {
        for (std::uint64_t x = start; x < w; ++x) {
          mask |= static_cast<std::uint64_t>(state[y * w + x] == 0)
                  << (x - start);
        }
      }
      available[id] = mask;
      reached[id] = 0;
      queued[id] = 0;
    }
  }
  const auto seed = [&](std::uint64_t id, std::uint64_t bits) {
    const auto extra = bits & available[id] & ~reached[id];
    if (extra) {
      reached[id] |= extra;
      enqueue(id);
    }
  };
  for (std::uint64_t c = 0; c < columns; ++c) {
    if ((c & 63) == 0) {
      poll(0);
    }
    seed(c, UINT64_MAX);
    seed((h - 1) * columns + c, UINT64_MAX);
  }
  const auto right = UINT64_C(1) << ((w - 1) % 64);
  for (std::uint64_t y = 0; y < h; ++y) {
    if ((y & 4095) == 0) {
      poll(0);
    }
    seed(y * columns, 1);
    seed(y * columns + columns - 1, right);
  }
  std::uint64_t visits = 0;
  while (count) {
    if ((++visits & 63) == 0) {
      poll(0);
    }
    const auto id = queue[first];
    if (++first == words) {
      first = 0;
    }
    --count;
    queued[id] = 0;
    auto bits = reached[id];
    for (;;) {
      const auto next = bits | (((bits << 1) | (bits >> 1)) & available[id]);
      if (next == bits) {
        break;
      }
      bits = next;
    }
    reached[id] = bits;
    if (id >= columns) {
      seed(id - columns, bits);
    }
    if (id + columns < words) {
      seed(id + columns, bits);
    }
    const auto c = id % columns;
    if (c && (bits & 1)) {
      seed(id - 1, UINT64_C(1) << 63);
    }
    if (c + 1 < columns && (bits >> 63)) {
      seed(id + 1, 1);
    }
  }
  for (std::uint64_t y = 0; y < h; ++y) {
    for (std::uint64_t c = 0; c < columns; ++c) {
      if ((c & 63) == 0) {
        poll(0);
      }
      const auto id = y * columns + c;
      const auto bits = reached[id], start = c * 64;
      if (bits == available[id]) {
        continue;
      }
      if (!bits) {
        std::memset(state + y * w + start, 1,
                    std::min<std::uint64_t>(64, w - start));
      } else if (start + 64 <= w) {
        unpack_exterior64(state + y * w + start, bits);
      } else {
        for (std::uint64_t x = start; x < w; ++x) {
          state[y * w + x] = ((bits >> (x - start)) & 1) ? 0 : 1;
        }
      }
    }
  }
}

// Stream both box stages through row rings. A numerically suspect coefficient
// abandons this path before publication and is recomputed by the centered
// path.
#if defined(__aarch64__)
// Two neighboring moment tuples use ordinary vector loads and ordered zips.
// Return the current moments while subtracting the leaving row in its
// original interleaved order. This avoids shuffling that independent update.
PS_KEY_INLINE float64x2x4_t guided_take_moments(double* p, const double* old) {
  const auto a = vld1q_f64(p), b = vld1q_f64(p + 2);
  const auto c = vld1q_f64(p + 4), d = vld1q_f64(p + 6);
  if (old) {
    vst1q_f64(p, vsubq_f64(a, vld1q_f64(old)));
    vst1q_f64(p + 2, vsubq_f64(b, vld1q_f64(old + 2)));
    vst1q_f64(p + 4, vsubq_f64(c, vld1q_f64(old + 4)));
    vst1q_f64(p + 6, vsubq_f64(d, vld1q_f64(old + 6)));
  }
  return {
      {vzip1q_f64(a, c), vzip2q_f64(a, c), vzip1q_f64(b, d), vzip2q_f64(b, d)}};
}
PS_KEY_INLINE void guided_store_coefficients(double* p, float64x2x2_t v) {
  vst1q_f64(p, vzip1q_f64(v.val[0], v.val[1]));
  vst1q_f64(p + 2, vzip2q_f64(v.val[0], v.val[1]));
}
#endif

// Large-radius seeds and clipped borders can span 4097 elements. Keep
// their running sums intact across bounded cancellation checkpoints.
template <class Poll, class Step>
PS_KEY_INLINE void guided_boundary(std::uint64_t begin, std::uint64_t end,
                                   std::uint64_t radius, Poll poll, Step step) {
  if (radius <= 32) {
    for (auto x = begin; x < end; ++x) {
      step(x);
    }
    return;
  }
  for (auto block = begin; block < end; block += 4096) {
    poll(0);
    for (auto x = block; x < std::min(end, block + 4096); ++x) {
      step(x);
    }
  }
}

#if defined(__aarch64__)
// Fuse a single incoming moment row with coefficient evaluation. Each element
// still adds the incoming value before subtracting the leaving value.
template <class T, class Poll>
PS_KEY_INLINE bool guided_fused_row(const T* matte_row, const T* guide_row,
                                    double* stored_row, double* vertical,
                                    const double* leaving, double* a,
                                    std::uint64_t w, std::uint64_t r,
                                    std::uint64_t rows, double origin,
                                    double growth, double epsilon, Poll poll) {
  bool unstable = false;
  const auto coefficient = [&](std::uint64_t x, double scale) {
    const double mg = vertical[4 * x] * scale, mp = vertical[4 * x + 1] * scale;
    const double second = vertical[4 * x + 2] * scale,
                 cross = vertical[4 * x + 3] * scale;
    const double variance = second - mg * mg, covariance = cross - mg * mp;
    const double ve = growth * (std::abs(second) + std::abs(mg * mg));
    const double ce = growth * (std::abs(cross) + std::abs(mg * mp));
    unstable |= (!std::isfinite(variance)) | (!std::isfinite(covariance)) |
                (ve > 1e-8 * (std::max(0.0, variance) + epsilon)) |
                ((ce > 0) & (std::abs(covariance) <= ce));
    a[2 * x] = covariance / (std::max(0.0, variance) + epsilon);
    a[2 * x + 1] = mp - a[2 * x] * mg;
    if (leaving) {
      for (unsigned k = 0; k < 4; ++k) {
        vertical[4 * x + k] -= leaving[4 * x + k];
      }
    }
  };

  auto sums01 = vdupq_n_f64(0), sums23 = vdupq_n_f64(0);
  const auto change = [&](std::uint64_t x, double sign) PS_KEY_ALWAYS_INLINE {
    const double g = static_cast<double>(guide_row[x]) - origin;
    const double p = static_cast<double>(matte_row[x]);
    const float64x2_t gp = {g, p};
    sums01 = vaddq_f64(sums01, vmulq_f64(vdupq_n_f64(sign), gp));
    sums23 = vaddq_f64(sums23, vmulq_f64(vdupq_n_f64(sign * g), gp));
  };
  guided_boundary(0, r + 1, r, poll, [&](std::uint64_t x) { change(x, 1); });
  const auto scalar_step = [&](std::uint64_t x) PS_KEY_ALWAYS_INLINE {
    auto* stored = stored_row + 4 * x;
    auto* current = vertical + 4 * x;
    vst1q_f64(stored, sums01);
    vst1q_f64(stored + 2, sums23);
    vst1q_f64(current, vaddq_f64(vld1q_f64(current), sums01));
    vst1q_f64(current + 2, vaddq_f64(vld1q_f64(current + 2), sums23));
    const auto columns = std::min(w, x + r + 1) - (x > r ? x - r : 0);
    coefficient(x, 1.0 / static_cast<unsigned>(rows * columns));
    if (x >= r) {
      change(x - r, -1);
    }
    if (x + r + 1 < w) {
      change(x + r + 1, 1);
    }
  };
  const auto last = w - r - 1;
  const double interior_scale = 1.0 / static_cast<unsigned>(rows * (2 * r + 1));
  const auto zero = vdupq_n_f64(0), eps = vdupq_n_f64(epsilon);
  const auto finite = vdupq_n_f64(std::numeric_limits<double>::max());
  for (std::uint64_t block = 0; block < w; block += 2048) {
    poll(0);
    auto x = block;
    const auto end = std::min(w, block + 2048);
    for (; x < std::min(r, end); ++x) {
      scalar_step(x);
    }
    auto issues = vdupq_n_u64(0);
    for (; x + 2 <= std::min(last, end); x += 2) {
      const auto center = vdupq_n_f64(origin);
      const auto gl = vsubq_f64(vld1q_f64(guide_row + x - r), center);
      const auto gr = vsubq_f64(vld1q_f64(guide_row + x + r + 1), center);
      const auto pl = vld1q_f64(matte_row + x - r);
      const auto pr = vld1q_f64(matte_row + x + r + 1);
      const auto ll = vmulq_f64(gl, gl), lr = vmulq_f64(gl, pl);
      const auto rr = vmulq_f64(gr, gr), rp = vmulq_f64(gr, pr);
      const auto gp0 = sums01, products0 = sums23;
      const auto gp1 =
          vaddq_f64(vsubq_f64(gp0, vzip1q_f64(gl, pl)), vzip1q_f64(gr, pr));
      const auto products1 = vaddq_f64(vsubq_f64(products0, vzip1q_f64(ll, lr)),
                                       vzip1q_f64(rr, rp));
      sums01 =
          vaddq_f64(vsubq_f64(gp1, vzip2q_f64(gl, pl)), vzip2q_f64(gr, pr));
      sums23 = vaddq_f64(vsubq_f64(products1, vzip2q_f64(ll, lr)),
                         vzip2q_f64(rr, rp));
      auto* stored = stored_row + 4 * x;
      vst1q_f64(stored, gp0);
      vst1q_f64(stored + 2, products0);
      vst1q_f64(stored + 4, gp1);
      vst1q_f64(stored + 6, products1);
      auto* current = vertical + 4 * x;
      const auto q0 = vaddq_f64(vld1q_f64(current), gp0);
      const auto q1 = vaddq_f64(vld1q_f64(current + 2), products0);
      const auto q2 = vaddq_f64(vld1q_f64(current + 4), gp1);
      const auto q3 = vaddq_f64(vld1q_f64(current + 6), products1);
      if (leaving) {
        const auto* old = leaving + 4 * x;
        vst1q_f64(current, vsubq_f64(q0, vld1q_f64(old)));
        vst1q_f64(current + 2, vsubq_f64(q1, vld1q_f64(old + 2)));
        vst1q_f64(current + 4, vsubq_f64(q2, vld1q_f64(old + 4)));
        vst1q_f64(current + 6, vsubq_f64(q3, vld1q_f64(old + 6)));
      } else {
        vst1q_f64(current, q0);
        vst1q_f64(current + 2, q1);
        vst1q_f64(current + 4, q2);
        vst1q_f64(current + 6, q3);
      }
      const float64x2x4_t v{{vzip1q_f64(q0, q2), vzip2q_f64(q0, q2),
                             vzip1q_f64(q1, q3), vzip2q_f64(q1, q3)}};
      const auto mg = vmulq_n_f64(v.val[0], interior_scale);
      const auto mp = vmulq_n_f64(v.val[1], interior_scale);
      const auto second = vmulq_n_f64(v.val[2], interior_scale);
      const auto cross = vmulq_n_f64(v.val[3], interior_scale);
      const auto square = vmulq_f64(mg, mg), product = vmulq_f64(mg, mp);
      const auto variance = vsubq_f64(second, square),
                 covariance = vsubq_f64(cross, product);
      const auto ve =
          vmulq_n_f64(vaddq_f64(vabsq_f64(second), vabsq_f64(square)), growth);
      const auto ce =
          vmulq_n_f64(vaddq_f64(vabsq_f64(cross), vabsq_f64(product)), growth);
      const auto denominator = vaddq_f64(vmaxq_f64(zero, variance), eps);
      const auto good = vandq_u64(vcleq_f64(vabsq_f64(variance), finite),
                                  vcleq_f64(vabsq_f64(covariance), finite));
      const auto suspect = vorrq_u64(
          vcgtq_f64(ve, vmulq_n_f64(denominator, 1e-8)),
          vandq_u64(vcgtq_f64(ce, zero), vcleq_f64(vabsq_f64(covariance), ce)));
      issues = vorrq_u64(
          issues, vorrq_u64(vbicq_u64(vdupq_n_u64(UINT64_MAX), good), suspect));
      float64x2x2_t result;
      result.val[0] = vdivq_f64(covariance, denominator);
      result.val[1] = vsubq_f64(mp, vmulq_f64(result.val[0], mg));
      guided_store_coefficients(a + 2 * x, result);
    }
    unstable |= (vgetq_lane_u64(issues, 0) | vgetq_lane_u64(issues, 1)) != 0;
    for (; x < end; ++x) {
      scalar_step(x);
    }
  }
  return !unstable;
}
#endif

template <class T, class Poll, unsigned FixedRadius = 0>
PS_KEY_INLINE bool guided_stream(const T* matte, const T* guide, T* out,
                                 double* scratch, std::uint64_t h,
                                 std::uint64_t w, std::uint64_t radius,
                                 double epsilon, Poll poll) {
  const auto r = FixedRadius ? std::uint64_t{FixedRadius} : radius;
  const auto ring = std::min(h, 2 * r + 1), plane = ring * w;
  auto* input_rows = scratch;
  auto* vertical = input_rows + 4 * plane;
  auto* a = vertical + 4 * w;
  auto* coefficient_rows = a + 2 * w;
  auto* finish = coefficient_rows + 2 * plane;
  fill_elements(vertical, 4 * w, 0.0, poll);
  fill_elements(finish, 2 * w, 0.0, poll);
  poll(h * w * 72);
  const double origin = guide[0];
  const double growth = 64 * std::numeric_limits<double>::epsilon() *
                        static_cast<double>(w + h + 2);
  std::uint64_t loaded = 0, emitted = 0;
  const auto emit = [&](std::uint64_t y) {
    const auto rows = std::min(h, y + r + 1) - (y > r ? y - r : 0);
    for (std::uint64_t block = 0; block < w; block += 4096) {
      poll(0);
      for (auto x = block; x < std::min(w, block + 4096); ++x) {
        const auto columns = std::min(w, x + r + 1) - (x > r ? x - r : 0);
        const double value =
            (finish[2 * x] * (static_cast<double>(guide[y * w + x]) - origin) +
             finish[2 * x + 1]) /
            static_cast<unsigned>(rows * columns);
        out[y * w + x] = static_cast<T>(std::clamp(value, 0.0, 1.0));
      }
    }
    if (y >= r) {
      const auto row = ((y - r) % ring) * w;
      for (std::uint64_t block = 0; block < 2 * w; block += 4096) {
        poll(0);
        for (auto i = block; i < std::min(2 * w, block + 4096); ++i) {
          finish[i] -= coefficient_rows[2 * row + i];
        }
      }
    }
  };
  for (std::uint64_t y = 0; y < h; ++y) {
    if (w <= 2 * r + 2) {
      poll(0);
    }
    bool fused = false;
#if defined(__aarch64__)
    if constexpr (sizeof(T) == 8) {
      if (w > 2 * r + 2 && std::min(h, y + r + 1) == loaded + 1) {
        const auto rows = std::min(h, y + r + 1) - (y > r ? y - r : 0);
        const auto* leaving =
            y >= r ? input_rows + 4 * ((y - r) % ring) * w : nullptr;
        if (!guided_fused_row(matte + loaded * w, guide + loaded * w,
                              input_rows + 4 * (loaded % ring) * w, vertical,
                              leaving, a, w, r, rows, origin, growth, epsilon,
                              poll)) {
          return false;
        }
        ++loaded;
        fused = true;
      }
    }
#endif
    if (!fused) {
      for (; loaded < std::min(h, y + r + 1); ++loaded) {
        const auto* guide_row = guide + loaded * w;
        const auto* matte_row = matte + loaded * w;
        auto* stored_row = input_rows + 4 * (loaded % ring) * w;
#if defined(__aarch64__)
        auto sums01 = vdupq_n_f64(0), sums23 = vdupq_n_f64(0);
        const auto change = [&](std::uint64_t x,
                                double sign) PS_KEY_ALWAYS_INLINE {
          const double g = static_cast<double>(guide_row[x]) - origin;
          const double p = static_cast<double>(matte_row[x]);
          const float64x2_t gp = {g, p};
          sums01 = vaddq_f64(sums01, vmulq_f64(vdupq_n_f64(sign), gp));
          sums23 = vaddq_f64(sums23, vmulq_f64(vdupq_n_f64(sign * g), gp));
        };
        const auto publish = [&](std::uint64_t x) PS_KEY_ALWAYS_INLINE {
          auto* stored = stored_row + 4 * x;
          auto* current = vertical + 4 * x;
          vst1q_f64(stored, sums01);
          vst1q_f64(stored + 2, sums23);
          vst1q_f64(current, vaddq_f64(vld1q_f64(current), sums01));
          vst1q_f64(current + 2, vaddq_f64(vld1q_f64(current + 2), sums23));
        };
#else
        double sums[4]{};
        const auto change = [&](std::uint64_t x,
                                double sign) PS_KEY_ALWAYS_INLINE {
          const double g = static_cast<double>(guide_row[x]) - origin;
          const double p = matte_row[x];
          sums[0] += sign * g;
          sums[1] += sign * p;
          sums[2] += sign * g * g;
          sums[3] += sign * g * p;
        };
        const auto publish = [&](std::uint64_t x) PS_KEY_ALWAYS_INLINE {
          std::memcpy(stored_row + 4 * x, sums, 4 * sizeof(double));
          for (unsigned k = 0; k < 4; ++k) {
            vertical[4 * x + k] += sums[k];
          }
        };
#endif
        guided_boundary(0, std::min(w, r + 1), r, poll,
                        [&](std::uint64_t x) { change(x, 1); });
        const auto boundary = [&](std::uint64_t x) PS_KEY_ALWAYS_INLINE {
          publish(x);
          if (x >= r) {
            change(x - r, -1);
          }
          if (x + r + 1 < w) {
            change(x + r + 1, 1);
          }
        };
        const auto first = std::min(w, r);
        const auto last = w > r + 1 ? std::max(first, w - r - 1) : first;
        guided_boundary(0, first, r, poll, boundary);
        for (auto block = first; block < last; block += 4096) {
          poll(0);
          for (auto x = block; x < std::min(last, block + 4096); ++x) {
            publish(x);
            change(x - r, -1);
            change(x + r + 1, 1);
          }
        }
        guided_boundary(last, w, r, poll, boundary);
      }
      const auto rows = std::min(h, y + r + 1) - (y > r ? y - r : 0);
      bool unstable = false;
      const auto* leaving =
          y >= r ? input_rows + 4 * ((y - r) % ring) * w : nullptr;
      const auto coefficient = [&](std::uint64_t x, double scale) {
        const double mg = vertical[4 * x] * scale,
                     mp = vertical[4 * x + 1] * scale;
        const double second = vertical[4 * x + 2] * scale,
                     cross = vertical[4 * x + 3] * scale;
        const double variance = second - mg * mg, covariance = cross - mg * mp;
        const double ve = growth * (std::abs(second) + std::abs(mg * mg));
        const double ce = growth * (std::abs(cross) + std::abs(mg * mp));
        unstable |= (!std::isfinite(variance)) | (!std::isfinite(covariance)) |
                    (ve > 1e-8 * (std::max(0.0, variance) + epsilon)) |
                    ((ce > 0) & (std::abs(covariance) <= ce));
        a[2 * x] = covariance / (std::max(0.0, variance) + epsilon);
        a[2 * x + 1] = mp - a[2 * x] * mg;
        if (leaving) {
          for (unsigned k = 0; k < 4; ++k) {
            vertical[4 * x + k] -= leaving[4 * x + k];
          }
        }
      };
      const auto first = std::min(w, r),
                 last = w > r ? std::max(first, w - r) : first;
      guided_boundary(0, first, r, poll, [&](std::uint64_t x) {
        coefficient(x,
                    1.0 / static_cast<unsigned>(rows * std::min(w, x + r + 1)));
      });
      const double interior_scale =
          1.0 / static_cast<unsigned>(rows * (2 * r + 1));
      for (auto block = first; block < last; block += 4096) {
        poll(0);
        auto x = block;
        const auto end = std::min(last, block + 4096);
#if defined(__aarch64__)
        const auto zero = vdupq_n_f64(0), eps = vdupq_n_f64(epsilon);
        const auto finite = vdupq_n_f64(std::numeric_limits<double>::max());
        auto issues = vdupq_n_u64(0);
        for (; x + 2 <= end; x += 2) {
          const auto v = guided_take_moments(
              vertical + 4 * x, leaving ? leaving + 4 * x : nullptr);
          const auto mg = vmulq_n_f64(v.val[0], interior_scale);
          const auto mp = vmulq_n_f64(v.val[1], interior_scale);
          const auto second = vmulq_n_f64(v.val[2], interior_scale);
          const auto cross = vmulq_n_f64(v.val[3], interior_scale);
          const auto square = vmulq_f64(mg, mg), product = vmulq_f64(mg, mp);
          const auto variance = vsubq_f64(second, square),
                     covariance = vsubq_f64(cross, product);
          const auto ve = vmulq_n_f64(
              vaddq_f64(vabsq_f64(second), vabsq_f64(square)), growth);
          const auto ce = vmulq_n_f64(
              vaddq_f64(vabsq_f64(cross), vabsq_f64(product)), growth);
          const auto denominator = vaddq_f64(vmaxq_f64(zero, variance), eps);
          const auto good = vandq_u64(vcleq_f64(vabsq_f64(variance), finite),
                                      vcleq_f64(vabsq_f64(covariance), finite));
          const auto suspect =
              vorrq_u64(vcgtq_f64(ve, vmulq_n_f64(denominator, 1e-8)),
                        vandq_u64(vcgtq_f64(ce, zero),
                                  vcleq_f64(vabsq_f64(covariance), ce)));
          issues = vorrq_u64(
              issues,
              vorrq_u64(vbicq_u64(vdupq_n_u64(UINT64_MAX), good), suspect));
          float64x2x2_t result;
          result.val[0] = vdivq_f64(covariance, denominator);
          result.val[1] = vsubq_f64(mp, vmulq_f64(result.val[0], mg));
          guided_store_coefficients(a + 2 * x, result);
        }
        unstable |=
            (vgetq_lane_u64(issues, 0) | vgetq_lane_u64(issues, 1)) != 0;
#endif
        for (; x < end; ++x) {
          coefficient(x, interior_scale);
        }
      }
      guided_boundary(last, w, r, poll, [&](std::uint64_t x) {
        coefficient(
            x, 1.0 / static_cast<unsigned>(rows * (w - (x > r ? x - r : 0))));
      });
      if (unstable) {
        return false;
      }
    }
    const auto row = (y % ring) * w;
    const bool ready = y >= r;
    const auto output_y = ready ? y - r : 0;
    const auto output_rows =
        std::min(h, output_y + r + 1) - (output_y > r ? output_y - r : 0);
    const auto* old = ready && output_y >= r
                          ? coefficient_rows + 2 * ((output_y - r) % ring) * w
                          : nullptr;
    const double output_scale =
        1.0 / static_cast<unsigned>(output_rows * (2 * r + 1));
    double sa = 0, sb = 0;
    guided_boundary(0, std::min(w, r + 1), r, poll, [&](std::uint64_t x) {
      sa += a[2 * x];
      sb += a[2 * x + 1];
    });
    const auto step = [&](std::uint64_t x, double scale) PS_KEY_ALWAYS_INLINE {
      coefficient_rows[2 * (row + x)] = sa;
      coefficient_rows[2 * (row + x) + 1] = sb;
      double fa = finish[2 * x] + sa, fb = finish[2 * x + 1] + sb;
      if (ready) {
        const double value =
            (fa * (static_cast<double>(guide[output_y * w + x]) - origin) +
             fb) *
            scale;
        out[output_y * w + x] = static_cast<T>(std::clamp(value, 0.0, 1.0));
        if (old) {
          fa -= old[2 * x];
          fb -= old[2 * x + 1];
        }
      }
      finish[2 * x] = fa;
      finish[2 * x + 1] = fb;
    };
    const auto boundary = [&](std::uint64_t x) PS_KEY_ALWAYS_INLINE {
      const auto columns = std::min(w, x + r + 1) - (x > r ? x - r : 0);
      step(x, 1.0 / static_cast<unsigned>(output_rows * columns));
      if (x >= r) {
        sa -= a[2 * (x - r)];
        sb -= a[2 * (x - r) + 1];
      }
      if (x + r + 1 < w) {
        sa += a[2 * (x + r + 1)];
        sb += a[2 * (x + r + 1) + 1];
      }
    };
    const auto interior_first = std::min(w, r);
    const auto interior_last =
        w > r + 1 ? std::max(interior_first, w - r - 1) : interior_first;
    guided_boundary(0, interior_first, r, poll, boundary);
    const auto interior = [&](auto mode) PS_KEY_ALWAYS_INLINE {
      constexpr int kMode = decltype(mode)::value;
#if defined(__aarch64__)
      const auto* guide_row = guide + output_y * w;
      auto* output_row = out + output_y * w;
      auto* coefficient_row = coefficient_rows + 2 * row;
      const auto center = vdupq_n_f64(origin);
      const auto zero = vdupq_n_f64(0), one = vdupq_n_f64(1);
#endif
      for (auto block = interior_first; block < interior_last; block += 4096) {
        poll(0);
        const auto end = std::min(interior_last, block + 4096);
        auto x = block;
#if defined(__aarch64__)
        float64x2_t sums = {sa, sb};
        for (; x + 2 <= end; x += 2) {
          const auto next =
              vaddq_f64(vsubq_f64(sums, vld1q_f64(a + 2 * (x - r))),
                        vld1q_f64(a + 2 * (x + r + 1)));
          auto first = vaddq_f64(vld1q_f64(finish + 2 * x), sums);
          auto second = vaddq_f64(vld1q_f64(finish + 2 * (x + 1)), next);
          vst1q_f64(coefficient_row + 2 * x, sums);
          vst1q_f64(coefficient_row + 2 * (x + 1), next);
          if constexpr (kMode != 0) {
            const auto g = vsubq_f64(
                sizeof(T) == 4
                    ? vcvt_f64_f32(vld1_f32(
                          reinterpret_cast<const float*>(guide_row + x)))
                    : vld1q_f64(reinterpret_cast<const double*>(guide_row + x)),
                center);
            auto value =
                vmulq_n_f64(vaddq_f64(vmulq_f64(vzip1q_f64(first, second), g),
                                      vzip2q_f64(first, second)),
                            output_scale);
            value = vbslq_f64(vcltq_f64(value, zero), zero, value);
            value = vbslq_f64(vcgtq_f64(value, one), one, value);
            if constexpr (sizeof(T) == 4) {
              vst1_f32(output_row + x, vcvt_f32_f64(value));
            } else {
              vst1q_f64(output_row + x, value);
            }
            if constexpr (kMode == 2) {
              first = vsubq_f64(first, vld1q_f64(old + 2 * x));
              second = vsubq_f64(second, vld1q_f64(old + 2 * (x + 1)));
            }
          }
          vst1q_f64(finish + 2 * x, first);
          vst1q_f64(finish + 2 * (x + 1), second);
          sums = vaddq_f64(vsubq_f64(next, vld1q_f64(a + 2 * (x + 1 - r))),
                           vld1q_f64(a + 2 * (x + r + 2)));
        }
        sa = vgetq_lane_f64(sums, 0);
        sb = vgetq_lane_f64(sums, 1);
#endif
        for (; x < end; ++x) {
          step(x, output_scale);
          sa -= a[2 * (x - r)];
          sb -= a[2 * (x - r) + 1];
          sa += a[2 * (x + r + 1)];
          sb += a[2 * (x + r + 1) + 1];
        }
      }
    };
    if (!ready) {
      interior(std::integral_constant<int, 0>{});
    } else if (old) {
      interior(std::integral_constant<int, 2>{});
    } else {
      interior(std::integral_constant<int, 1>{});
    }
    guided_boundary(interior_last, w, r, poll, boundary);
    if (ready) {
      ++emitted;
    }
  }
  for (; emitted < h; ++emitted) {
    emit(emitted);
  }
  return true;
}

template <class T, class Poll>
PS_KEY_INLINE bool guided_try_stream(const T* matte, const T* guide, T* out,
                                     double* scratch, std::uint64_t h,
                                     std::uint64_t w, std::uint64_t r,
                                     double epsilon, Poll poll) {
  // Extreme regularizers need scaled local statistics. In this ordinary
  // range, subnormal moments are far below the absolute output tolerance.
  if (h < 2 || epsilon < 1e-250 || epsilon > 1e250) {
    return false;
  }
  switch (r) {
    case 1:
      return guided_stream<T, Poll, 1>(matte, guide, out, scratch, h, w, r,
                                       epsilon, poll);
    case 2:
      return guided_stream<T, Poll, 2>(matte, guide, out, scratch, h, w, r,
                                       epsilon, poll);
    case 3:
      return guided_stream<T, Poll, 3>(matte, guide, out, scratch, h, w, r,
                                       epsilon, poll);
    case 4:
      return guided_stream<T, Poll, 4>(matte, guide, out, scratch, h, w, r,
                                       epsilon, poll);
    default:
      return guided_stream(matte, guide, out, scratch, h, w, r, epsilon, poll);
  }
}

// Complete centered fallback, including scaled local statistics when needed.
template <class T, class Poll>
PS_KEY_INLINE void guided_centered(const T* matte, const T* guide, T* out,
                                   double* scratch, std::uint64_t h,
                                   std::uint64_t w, std::uint64_t r,
                                   double epsilon, Poll poll) {
  const auto n = h * w;
  auto* horizontal = scratch;         // 4*N
  auto* a = horizontal + 4 * n;       // N
  auto* b = a + n;                    // N: flags and local scale exponents
  auto* local_mean = b + n;           // N: relative to each window anchor
  auto* matte_mean = local_mean + n;  // N
  auto* vertical = matte_mean + n;    // 4*W
  bool constant = true;
  for (std::uint64_t i = 0; i < n; ++i) {
    if ((i & 4095) == 0) {
      poll(std::min<std::uint64_t>(4096, n - i));
    }
    constant &= matte[i] == matte[0];
  }
  if (constant) {
    for (std::uint64_t i = 0; i < n; i += 4096) {
      poll(0);
      std::memcpy(out + i, matte + i,
                  std::min<std::uint64_t>(4096, n - i) * sizeof(T));
    }
    return;
  }
  const double origin = guide[0];
  for (std::uint64_t y = 0; y < h; ++y) {
    poll(w * 24);
    double sums[4]{};
    const auto change = [&](std::uint64_t x, double sign) {
      const double g = static_cast<double>(guide[y * w + x]) - origin;
      const double p = matte[y * w + x];
      sums[0] += sign * g;
      sums[1] += sign * p;
      sums[2] += sign * g * g;
      sums[3] += sign * g * p;
    };
    for (std::uint64_t x = 0; x < std::min(w, r + 1); ++x) {
      if (x && (x & 4095) == 0) {
        poll(0);
      }
      change(x, 1);
    }
    for (std::uint64_t block = 0; block < w; block += 4096) {
      poll(0);
      for (std::uint64_t x = block; x < std::min(w, block + 4096); ++x) {
        for (unsigned k = 0; k < 4; ++k) {
          horizontal[k * n + y * w + x] = sums[k];
        }
        if (x >= r) {
          change(x - r, -1);
        }
        if (x + r + 1 < w) {
          change(x + r + 1, 1);
        }
      }
    }
  }
  fill_elements(vertical, 4 * w, 0.0, poll);
  for (std::uint64_t y = 0; y < std::min(h, r + 1); ++y) {
    for (unsigned k = 0; k < 4; ++k) {
      for (std::uint64_t block = 0; block < w; block += 4096) {
        poll(0);
        for (std::uint64_t x = block; x < std::min(w, block + 4096); ++x) {
          vertical[k * w + x] += horizontal[k * n + y * w + x];
        }
      }
    }
  }
  bool robust = false;
  for (std::uint64_t y = 0; y < h; ++y) {
    poll(w * 24);
    const auto rows = std::min(h, y + r + 1) - (y > r ? y - r : 0);
    for (std::uint64_t block = 0; block < w; block += 4096) {
      poll(0);
      for (std::uint64_t x = block; x < std::min(w, block + 4096); ++x) {
        const auto columns = std::min(w, x + r + 1) - (x > r ? x - r : 0);
        const double scale = 1.0 / static_cast<double>(rows * columns);
        const double mg = vertical[x] * scale;
        double mp = vertical[w + x] * scale;
        const double second = vertical[2 * w + x] * scale;
        const double cross = vertical[3 * w + x] * scale;
        double variance = second - mg * mg, covariance = cross - mg * mp;
        const double growth = 64 * std::numeric_limits<double>::epsilon() *
                              static_cast<double>(w + h + 2);
        const double variance_error =
            growth * (std::abs(second) + std::abs(mg * mg));
        const double covariance_error =
            growth * (std::abs(cross) + std::abs(mg * mp));
        const double denominator = std::max(0.0, variance) + epsilon;
        const bool unstable =
            !std::isfinite(variance) || !std::isfinite(covariance) ||
            !std::isfinite(denominator) ||
            (std::abs(second) < 1e-280 && covariance != 0) ||
            (variance_error > 1e-8 * denominator) ||
            (covariance_error > 0 && std::abs(covariance) <= covariance_error);
        robust |= unstable;
        a[y * w + x] = covariance / (std::max(0.0, variance) + epsilon);
        b[y * w + x] = mp - a[y * w + x] * mg;
        local_mean[y * w + x] = mg;
        matte_mean[y * w + x] = unstable ? -mp - 1.0 : mp;
      }
    }
    for (unsigned k = 0; k < 4; ++k) {
      for (std::uint64_t block = 0; block < w; block += 4096) {
        poll(0);
        for (std::uint64_t x = block; x < std::min(w, block + 4096); ++x) {
          if (y >= r) {
            vertical[k * w + x] -= horizontal[k * n + (y - r) * w + x];
          }
          if (y + r + 1 < h) {
            vertical[k * w + x] += horizontal[k * n + (y + r + 1) * w + x];
          }
        }
      }
    }
  }
  if (!robust) {
    for (std::uint64_t y = 0; y < h; ++y) {
      poll(w * 8);
      double sa = 0, sb = 0;
      for (std::uint64_t x = 0; x < std::min(w, r + 1); ++x) {
        if (x && (x & 4095) == 0) {
          poll(0);
        }
        sa += a[y * w + x];
        sb += b[y * w + x];
      }
      for (std::uint64_t block = 0; block < w; block += 4096) {
        poll(0);
        for (auto x = block; x < std::min(w, block + 4096); ++x) {
          horizontal[y * w + x] = sa;
          horizontal[n + y * w + x] = sb;
          if (x >= r) {
            sa -= a[y * w + x - r];
            sb -= b[y * w + x - r];
          }
          if (x + r + 1 < w) {
            sa += a[y * w + x + r + 1];
            sb += b[y * w + x + r + 1];
          }
        }
      }
    }
    fill_elements(vertical, 2 * w, 0.0, poll);
    for (std::uint64_t y = 0; y < std::min(h, r + 1); ++y) {
      for (std::uint64_t block = 0; block < w; block += 4096) {
        poll(0);
        for (auto x = block; x < std::min(w, block + 4096); ++x) {
          vertical[x] += horizontal[y * w + x];
          vertical[w + x] += horizontal[n + y * w + x];
        }
      }
    }
    for (std::uint64_t y = 0; y < h; ++y) {
      const auto rows = std::min(h, y + r + 1) - (y > r ? y - r : 0);
      for (std::uint64_t block = 0; block < w; block += 4096) {
        poll(std::min<std::uint64_t>(4096, w - block) * 16);
        for (auto x = block; x < std::min(w, block + 4096); ++x) {
          const auto columns = std::min(w, x + r + 1) - (x > r ? x - r : 0);
          const double value =
              (vertical[x] * (static_cast<double>(guide[y * w + x]) - origin) +
               vertical[w + x]) /
              static_cast<double>(rows * columns);
          out[y * w + x] = static_cast<T>(std::clamp(value, 0.0, 1.0));
          if (y >= r) {
            vertical[x] -= horizontal[(y - r) * w + x];
            vertical[w + x] -= horizontal[n + (y - r) * w + x];
          }
          if (y + r + 1 < h) {
            vertical[x] += horizontal[(y + r + 1) * w + x];
            vertical[w + x] += horizontal[n + (y + r + 1) * w + x];
          }
        }
      }
    }
    return;
  }
  for (std::uint64_t y = 0; y < h; ++y) {
    const auto yy0 = y > r ? y - r : 0;
    const auto rows = std::min(h, y + r + 1) - yy0;
    for (std::uint64_t x = 0; x < w; ++x) {
      if ((x & 4095) == 0) {
        poll(0);
      }
      const auto xx0 = x > r ? x - r : 0;
      const auto columns = std::min(w, x + r + 1) - xx0;
      const auto scale = 1.0 / static_cast<double>(rows * columns);
      const double anchor = guide[yy0 * w + xx0];
      const bool unstable = matte_mean[y * w + x] < 0;
      double mp =
          unstable ? -matte_mean[y * w + x] - 1.0 : matte_mean[y * w + x];
      double local = local_mean[y * w + x] - (anchor - origin);
      double variance = 0, covariance = 0;
      double local_epsilon = epsilon;
      int exponent = 0;
      bool rescaled = false;
      if (unstable) {
        poll(rows * columns * 16);
        double sg = 0, sp = 0, cg = 0, cp = 0;
        std::uint64_t visits = 0;
        const auto add = [](double v, double* sum, double* correction) {
          const double adjusted = v - *correction;
          const double next = *sum + adjusted;
          *correction = (next - *sum) - adjusted;
          *sum = next;
        };
        for (auto yy = yy0; yy < std::min(h, y + r + 1); ++yy) {
          for (auto xx = xx0; xx < std::min(w, x + r + 1); ++xx) {
            if ((++visits & 4095) == 0) {
              poll(0);
            }
            add(static_cast<double>(guide[yy * w + xx]) - anchor, &sg, &cg);
            add(static_cast<double>(matte[yy * w + xx]), &sp, &cp);
          }
        }
        local = sg * scale;
        mp = sp * scale;
        variance = covariance = 0;
        double cv = 0, cc = 0;
        for (auto yy = yy0; yy < std::min(h, y + r + 1); ++yy) {
          for (auto xx = xx0; xx < std::min(w, x + r + 1); ++xx) {
            if ((++visits & 4095) == 0) {
              poll(0);
            }
            const double g =
                (static_cast<double>(guide[yy * w + xx]) - anchor) - local;
            const double p = static_cast<double>(matte[yy * w + xx]) - mp;
            add(g * g, &variance, &cv);
            add(g * p, &covariance, &cc);
          }
        }
        variance *= scale;
        covariance *= scale;
        if (!std::isfinite(local) || !std::isfinite(variance) ||
            !std::isfinite(covariance) ||
            !std::isfinite(std::max(0.0, variance) + epsilon) ||
            (std::abs(variance) < 1e-280 && covariance != 0)) {
          poll(rows * columns * 32);
          double maximum = 0;
          for (auto yy = yy0; yy < std::min(h, y + r + 1); ++yy) {
            for (auto xx = xx0; xx < std::min(w, x + r + 1); ++xx) {
              if ((++visits & 4095) == 0) {
                poll(0);
              }
              maximum = std::max(
                  maximum, std::abs(static_cast<double>(guide[yy * w + xx])));
            }
          }
          if (maximum == 0) {
            local = variance = covariance = 0;
          } else {
            exponent = -std::ilogb(maximum);
            const double scaled_anchor = std::scalbn(anchor, exponent);
            sg = cg = 0;
            for (auto yy = yy0; yy < std::min(h, y + r + 1); ++yy) {
              for (auto xx = xx0; xx < std::min(w, x + r + 1); ++xx) {
                if ((++visits & 4095) == 0) {
                  poll(0);
                }
                add(std::scalbn(static_cast<double>(guide[yy * w + xx]),
                                exponent) -
                        scaled_anchor,
                    &sg, &cg);
              }
            }
            local = sg * scale;
            variance = covariance = cv = cc = 0;
            for (auto yy = yy0; yy < std::min(h, y + r + 1); ++yy) {
              for (auto xx = xx0; xx < std::min(w, x + r + 1); ++xx) {
                if ((++visits & 4095) == 0) {
                  poll(0);
                }
                const double g =
                    (std::scalbn(static_cast<double>(guide[yy * w + xx]),
                                 exponent) -
                     scaled_anchor) -
                    local;
                const double p = static_cast<double>(matte[yy * w + xx]) - mp;
                add(g * g, &variance, &cv);
                add(g * p, &covariance, &cc);
              }
            }
            variance *= scale;
            covariance *= scale;
            local_epsilon = std::scalbn(epsilon, 2 * exponent);
            rescaled = true;
          }
        }
      }
      if (unstable) {
        a[y * w + x] =
            variance == 0 || std::isinf(local_epsilon)
                ? 0.0
                : covariance / (std::max(0.0, variance) + local_epsilon);
      }
      // Nonzero flags are exact positive integers, including scaled windows.
      b[y * w + x] = rescaled ? static_cast<double>(2048 + exponent)
                              : (unstable ? 1.0 : 0.0);
      local_mean[y * w + x] = local;
      matte_mean[y * w + x] = mp;
    }
  }
  for (std::uint64_t y = 0; y < h; ++y) {
    poll(w * 8);
    double sa = 0, sb = 0, unstable = 0;
    // Flagged coefficients are evaluated directly. Zero placeholders prevent
    // their extreme temporary intercepts from contaminating later windows.
    const auto slope = [&](std::uint64_t x) {
      return b[y * w + x] == 0 ? a[y * w + x] : 0.0;
    };
    const auto intercept = [&](std::uint64_t x) {
      if (b[y * w + x] != 0) {
        return 0.0;
      }
      const auto yy = y > r ? y - r : 0, xx = x > r ? x - r : 0;
      return matte_mean[y * w + x] -
             a[y * w + x] *
                 ((static_cast<double>(guide[yy * w + xx]) - origin) +
                  local_mean[y * w + x]);
    };
    for (std::uint64_t x = 0; x < std::min(w, r + 1); ++x) {
      if (x && (x & 4095) == 0) {
        poll(0);
      }
      sa += slope(x);
      sb += intercept(x);
      unstable += b[y * w + x];
    }
    for (std::uint64_t block = 0; block < w; block += 4096) {
      poll(0);
      for (std::uint64_t x = block; x < std::min(w, block + 4096); ++x) {
        horizontal[y * w + x] = sa;
        horizontal[n + y * w + x] = sb;
        horizontal[2 * n + y * w + x] = unstable;
        if (x >= r) {
          sa -= slope(x - r);
          sb -= intercept(x - r);
          unstable -= b[y * w + x - r];
        }
        if (x + r + 1 < w) {
          sa += slope(x + r + 1);
          sb += intercept(x + r + 1);
          unstable += b[y * w + x + r + 1];
        }
      }
    }
  }
  fill_elements(vertical, 3 * w, 0.0, poll);
  for (std::uint64_t y = 0; y < std::min(h, r + 1); ++y) {
    for (std::uint64_t block = 0; block < w; block += 4096) {
      poll(0);
      for (std::uint64_t x = block; x < std::min(w, block + 4096); ++x) {
        vertical[x] += horizontal[y * w + x];
        vertical[w + x] += horizontal[n + y * w + x];
        vertical[2 * w + x] += horizontal[2 * n + y * w + x];
      }
    }
  }
  for (std::uint64_t y = 0; y < h; ++y) {
    poll(w * 24);
    const auto rows = std::min(h, y + r + 1) - (y > r ? y - r : 0);
    for (std::uint64_t block = 0; block < w; block += 4096) {
      poll(0);
      for (std::uint64_t x = block; x < std::min(w, block + 4096); ++x) {
        const auto columns = std::min(w, x + r + 1) - (x > r ? x - r : 0);

        double value =
            (vertical[x] * (static_cast<double>(guide[y * w + x]) - origin) +
             vertical[w + x]) /
            static_cast<double>(rows * columns);
        if (vertical[2 * w + x] != 0 || !std::isfinite(value)) {
          poll(rows * columns * 10);
          double sum = 0, compensation = 0;
          std::uint64_t visits = 0;
          for (auto yy = y > r ? y - r : 0; yy < std::min(h, y + r + 1); ++yy) {
            for (auto xx = x > r ? x - r : 0; xx < std::min(w, x + r + 1);
                 ++xx) {
              if ((++visits & 4095) == 0) {
                poll(0);
              }
              const auto ay = yy > r ? yy - r : 0, ax = xx > r ? xx - r : 0;
              double g = 0;
              if (b[yy * w + xx] > 1) {
                const auto shift = static_cast<int>(b[yy * w + xx]) - 2048;
                // This output coordinate belongs to the coefficient window,
                // so both scaled guide values remain within its finite range.
                g = (std::scalbn(static_cast<double>(guide[y * w + x]), shift) -
                     std::scalbn(static_cast<double>(guide[ay * w + ax]),
                                 shift)) -
                    local_mean[yy * w + xx];
              } else {
                g = (static_cast<double>(guide[y * w + x]) -
                     static_cast<double>(guide[ay * w + ax])) -
                    local_mean[yy * w + xx];
              }
              const double term = a[yy * w + xx] * g + matte_mean[yy * w + xx];
              const double adjusted = term - compensation;
              const double next = sum + adjusted;
              compensation = (next - sum) - adjusted;
              sum = next;
            }
          }
          value = sum / static_cast<double>(rows * columns);
        }
        out[y * w + x] = static_cast<T>(std::clamp(value, 0.0, 1.0));
        if (y >= r) {
          vertical[x] -= horizontal[(y - r) * w + x];
          vertical[w + x] -= horizontal[n + (y - r) * w + x];
          vertical[2 * w + x] -= horizontal[2 * n + (y - r) * w + x];
        }
        if (y + r + 1 < h) {
          vertical[x] += horizontal[(y + r + 1) * w + x];
          vertical[w + x] += horizontal[n + (y + r + 1) * w + x];
          vertical[2 * w + x] += horizontal[2 * n + (y + r + 1) * w + x];
        }
      }
    }
  }
}

// The partition is anchored to the complete image, independently of requested
// output tiles. Two radii of input halo contain both box stages for every
// published pixel; clipped windows differ only outside the published center.
template <class T, class Poll>
PS_KEY_INLINE void guided_impl(const T* matte, const T* guide, T* out,
                               double* scratch, std::uint64_t h,
                               std::uint64_t w, std::uint64_t r, double epsilon,
                               Poll poll) {
  if (!r) {
    copy_elements(out, matte, h * w, poll);
    return;
  }
  if (guided_try_stream(matte, guide, out, scratch, h, w, r, epsilon, poll)) {
    return;
  }
  constexpr std::uint64_t side = 256;
  const auto max_h = std::min(h, side + 4 * r);
  const bool wide =
      __uint128_t(11) * max_h * w + 4 * w <= __uint128_t(8) * h * w + 4 * w;
  const auto max_w = wide ? w : std::min(w, side + 4 * r);
  const auto max_n = max_h * max_w;
  const bool fits =
      __uint128_t(11) * max_n + 4 * max_w <= __uint128_t(8) * h * w + 4 * w;
  if (h < 2 || r > 32 || epsilon < 1e-250 || epsilon > 1e250 || !fits) {
    guided_centered(matte, guide, out, scratch, h, w, r, epsilon, poll);
    return;
  }
  auto* local_matte = reinterpret_cast<T*>(scratch);
  auto* local_guide = reinterpret_cast<T*>(scratch + max_n);
  auto* local_out = reinterpret_cast<T*>(scratch + 2 * max_n);
  auto* local_scratch = scratch + 3 * max_n;
  struct Region {
    std::uint64_t y0, y1, x0, x1;
  };
  // Four subdivisions from side 256 to side 16 need at most 13 pending nodes.
  std::array<Region, 16> pending{};
  __uint128_t attempted = 0;
  const auto attempt_limit = __uint128_t(2) * h * w;
  for (std::uint64_t by = 0; by < h; by += side) {
    if (wide) {
      const auto y0 = by > 2 * r ? by - 2 * r : 0;
      const auto stop = std::min(h, by + side);
      const auto y1 = std::min(h, stop + 2 * r);
      attempted += (y1 - y0) * w;
      poll(1);
      if (attempted > attempt_limit) {
        guided_centered(matte, guide, out, scratch, h, w, r, epsilon, poll);
        return;
      }
      // Stream directly into the final rows, retaining the completed leading
      // halo. Later bands overwrite the following halo before publication.
      const auto prefix = (by - y0) * w;
      poll(prefix);
      copy_elements(local_out, out + y0 * w, prefix, poll);
      const bool stable =
          guided_try_stream(matte + y0 * w, guide + y0 * w, out + y0 * w,
                            local_scratch, y1 - y0, w, r, epsilon, poll);
      poll(prefix);
      copy_elements(out + y0 * w, local_out, prefix, poll);
      if (stable) {
        continue;
      }
    }
    for (std::uint64_t bx = 0; bx < w; bx += side) {
      std::uint64_t used = 1;
      pending[0] = {by, std::min(h, by + side), bx, std::min(w, bx + side)};
      while (used) {
        poll(1);
        const auto region = pending[--used];
        const auto y0 = region.y0 > 2 * r ? region.y0 - 2 * r : 0;
        const auto x0 = region.x0 > 2 * r ? region.x0 - 2 * r : 0;
        const auto y1 = std::min(h, region.y1 + 2 * r);
        const auto x1 = std::min(w, region.x1 + 2 * r);
        const auto rows = y1 - y0, columns = x1 - x0;
        attempted += rows * columns;
        if (attempted > attempt_limit) {
          // Dense instability must not multiply halo work at every leaf.
          // The full fallback overwrites all previously published patches.
          guided_centered(matte, guide, out, scratch, h, w, r, epsilon, poll);
          return;
        }
        for (auto y = y0; y < y1; ++y) {
          poll(2 * columns);
          std::memcpy(local_matte + (y - y0) * columns, matte + y * w + x0,
                      columns * sizeof(T));
          std::memcpy(local_guide + (y - y0) * columns, guide + y * w + x0,
                      columns * sizeof(T));
        }
        const bool stable =
            guided_try_stream(local_matte, local_guide, local_out,
                              local_scratch, rows, columns, r, epsilon, poll);
        if (!stable) {
          if (region.y1 - region.y0 > 16 || region.x1 - region.x0 > 16) {
            const auto my = region.y1 - region.y0 > 16
                                ? region.y0 + (region.y1 - region.y0) / 2
                                : region.y1;
            const auto mx = region.x1 - region.x0 > 16
                                ? region.x0 + (region.x1 - region.x0) / 2
                                : region.x1;
            const auto push = [&](std::uint64_t a, std::uint64_t b,
                                  std::uint64_t c, std::uint64_t d) {
              if (a < b && c < d) {
                pending[used++] = {a, b, c, d};
              }
            };
            push(my, region.y1, mx, region.x1);
            push(my, region.y1, region.x0, mx);
            push(region.y0, my, mx, region.x1);
            push(region.y0, my, region.x0, mx);
            continue;
          }
          guided_centered(local_matte, local_guide, local_out, local_scratch,
                          rows, columns, r, epsilon, poll);
        }
        for (auto y = region.y0; y < region.y1; ++y) {
          const auto width = region.x1 - region.x0;
          poll(width);
          std::memcpy(out + y * w + region.x0,
                      local_out + (y - y0) * columns + region.x0 - x0,
                      width * sizeof(T));
        }
      }
    }
  }
}

#if PS_KEY_X86
template <class T, class Poll>
PS_KEY_AVX2 void guided_avx2(const T* matte, const T* guide, T* out,
                             double* scratch, std::uint64_t h, std::uint64_t w,
                             std::uint64_t r, double epsilon, Poll poll) {
  return guided_impl(matte, guide, out, scratch, h, w, r, epsilon, poll);
}
#endif
template <class T, class Poll>
void guided(const T* matte, const T* guide, T* out, double* scratch,
            std::uint64_t h, std::uint64_t w, std::uint64_t r, double epsilon,
            Poll poll) {
#if PS_KEY_X86
  if (__builtin_cpu_supports("avx2")) {
    return guided_avx2(matte, guide, out, scratch, h, w, r, epsilon, poll);
  }
#endif
  return guided_impl(matte, guide, out, scratch, h, w, r, epsilon, poll);
}

template <class T>
inline T midpoint(T a, T b) {
  // Avoid both overflow of a+b and premature underflow of subnormal halves.
  if (std::abs(a) <= std::numeric_limits<T>::max() / T(2) &&
      std::abs(b) <= std::numeric_limits<T>::max() / T(2)) {
    return (a + b) / T(2);
  }
  return a / T(2) + b / T(2);
}
template <class T>
struct TemporalSample final {
  T value;
  std::uint64_t frame;
};
template <class T, class Poll>
bool temporal(const T* images, const std::uint8_t* valid, T* plate,
              T* confidence, std::uint64_t frames, std::uint64_t pixels,
              TemporalSample<T>* scratch, Poll poll,
              bool allow_missing = false) {
  if (frames == 3 && plate) {
    bool all = true;
    for (std::uint64_t i = 0; i < 3 * pixels; ++i) {
      if ((i & 4095) == 0) {
        poll(std::min<std::uint64_t>(4096, 3 * pixels - i));
      }
      all &= valid[i] != 0;
    }
    if (all) {
      for (std::uint64_t start = 0; start < 3 * pixels; start += 4096) {
        const auto stop = std::min(3 * pixels, start + 4096);
        poll((stop - start) * 6);
        for (auto i = start; i < stop; ++i) {
          T a = images[i], b = images[3 * pixels + i],
            c = images[6 * pixels + i];
          if (b < a) {
            std::swap(a, b);
          }
          if (c < b) {
            std::swap(b, c);
          }
          if (b < a) {
            std::swap(a, b);
          }
          plate[i] = b;
        }
      }
      if (confidence) {
        fill_elements(confidence, pixels, T(1), poll);
      }
      return true;
    }
  }
  bool complete = true;
  std::uint64_t comparisons = 0;
  const auto less = [&](const auto& a, const auto& b) {
    if ((++comparisons & 4095) == 0) {
      poll(0);
    }
    return a.value < b.value || (a.value == b.value && a.frame < b.frame);
  };
  for (std::uint64_t i = 0; i < pixels; ++i) {
    if ((i & 255) == 0) {
      poll(std::min<std::uint64_t>(256, pixels - i) * (frames * 12 + 1));
    }
    std::uint64_t count = 0;
    for (std::uint64_t f = 0; f < frames; ++f) {
      if (f && (f & 4095) == 0) {
        poll(0);
      }
      count += valid[f * pixels + i] != 0;
    }
    if (confidence) {
      confidence[i] = static_cast<T>(static_cast<double>(count) /
                                     static_cast<double>(frames));
    }
    if (!plate) {
      continue;
    }
    if (!count) {
      if (!allow_missing) {
        return false;
      }
      complete = false;
      std::fill(plate + 3 * i, plate + 3 * i + 3, T(0));
      continue;
    }
    for (unsigned c = 0; c < 3; ++c) {
      std::uint64_t j = 0;
      for (std::uint64_t f = 0; f < frames; ++f) {
        if (f && (f & 4095) == 0) {
          poll(0);
        }
        if (valid[f * pixels + i]) {
          scratch[j++] = {images[(f * pixels + i) * 3 + c], f};
        }
      }
      const auto middle = count / 2;
      std::nth_element(scratch, scratch + middle, scratch + count, less);
      if (count & 1) {
        plate[3 * i + c] = scratch[middle].value;
      } else {
        const auto lower = std::max_element(scratch, scratch + middle, less);
        plate[3 * i + c] = midpoint(lower->value, scratch[middle].value);
      }
    }
  }
  return complete;
}
}  // namespace ps::plugin_internal::keying

#undef PS_KEY_ALWAYS_INLINE
#undef PS_KEY_INLINE
#undef PS_KEY_X86
#if defined(PS_KEY_AVX2)
#undef PS_KEY_AVX2
#endif
