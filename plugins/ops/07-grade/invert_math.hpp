// Copyright 2026 Photospider contributors
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(__aarch64__)
#include <arm_neon.h>
#if defined(__APPLE__) && defined(PHOTOSPIDER_HAS_ACCELERATE)
#include <Accelerate/Accelerate.h>
#endif
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#endif

namespace ps::plugin_internal::grade::invert {
/** A dynamic bound sum and an independent bit-preserving bypass. */
struct Control final {
  double sum = 0;
  bool active = false;
};

template <class T>
inline void scalar(const T* x, T* y, const Control& c) {
  if (c.active)
    *y = static_cast<T>(c.sum - static_cast<double>(*x));
  else
    std::memcpy(y, x, sizeof(T));
}

#if defined(__aarch64__)
// Deinterleave complete double-precision pixels so bypass lanes need neither
// arithmetic nor a blend. Two blocks keep the live vector set bounded.
template <unsigned Mask>
inline std::size_t neon_four(const double* x, double* y, std::size_t count,
                             const Control* controls) {
  float64x2_t sums[4];
  for (unsigned c = 0; c < 4; ++c)
    sums[c] = vdupq_n_f64(controls[c].sum);
  std::size_t i = 0;
  for (; i + 16 <= count; i += 16) {
    float64x2x4_t values[2];
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (unsigned j = 0; j < 2; ++j) {
      values[j] = vld4q_f64(x + i + j * 8);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
      for (unsigned c = 0; c < 4; ++c)
        if (Mask & (1U << c))
          values[j].val[c] = vsubq_f64(sums[c], values[j].val[c]);
    }
    asm volatile("" ::: "memory");
    vst4q_f64(y + i, values[0]);
    vst4q_f64(y + i + 8, values[1]);
  }
  return i;
}
template <class T, unsigned C, bool Narrow, bool All>
inline std::size_t neon(const T* x, T* y, std::size_t count,
                        const Control* controls) {
  constexpr unsigned kLanes = sizeof(T) == 4 ? 4 : 2;
  constexpr unsigned kPackets = C == 3 ? 3 : (C == 4 && kLanes == 2 ? 2 : 1);
  constexpr unsigned kBlock = kLanes * kPackets;
  float64x2_t sums[kPackets][2];
  float32x4_t sums32[kPackets];
  uint64x2_t masks64[kPackets];
  uint32x4_t masks32[kPackets];
  for (unsigned k = 0; k < kPackets; ++k) {
    double s[4]{};
    std::uint64_t m64[2]{};
    std::uint32_t m32[4]{};
    for (unsigned l = 0; l < kLanes; ++l) {
      const auto& c = controls[(k * kLanes + l) % C];
      s[l] = c.sum;
      m32[l] = c.active ? UINT32_MAX : 0;
      if (l < 2)
        m64[l] = c.active ? UINT64_MAX : 0;
    }
    sums[k][0] = vld1q_f64(s);
    sums[k][1] = vld1q_f64(s + 2);
    sums32[k] =
        vcombine_f32(vcvt_f32_f64(sums[k][0]), vcvt_f32_f64(sums[k][1]));
    masks64[k] = vld1q_u64(m64);
    masks32[k] = vld1q_u32(m32);
  }
  std::size_t i = 0;
  constexpr unsigned kUnroll = C == 3 ? 2 : 8;
  using Vector = std::conditional_t<sizeof(T) == 4, float32x4_t, float64x2_t>;
  for (; i + kUnroll * kBlock <= count; i += kUnroll * kBlock) {
    if constexpr (C == 1 && All) {
      // A bounded read hint helps cache-resident uniform reflection runs.
      constexpr std::size_t kPrefetch = 2048 / sizeof(T);
      if (count <= 16 * 1024 * 1024 / sizeof(T) && i + kPrefetch < count)
        __builtin_prefetch(x + i + kPrefetch, 0, 0);
    }
    Vector values[kUnroll * kPackets];
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (unsigned j = 0; j < kUnroll; ++j) {
      for (unsigned k = 0; k < kPackets; ++k) {
        if constexpr (sizeof(T) == 4) {
          const auto v = vld1q_f32(x + i + j * kBlock + k * kLanes);
          float32x4_t result;
          if constexpr (Narrow) {
            result = vsubq_f32(sums32[k], v);
          } else {
            const auto lo =
                vsubq_f64(sums[k][0], vcvt_f64_f32(vget_low_f32(v)));
            const auto hi =
                vsubq_f64(sums[k][1], vcvt_f64_f32(vget_high_f32(v)));
            result = vcombine_f32(vcvt_f32_f64(lo), vcvt_f32_f64(hi));
          }
          if constexpr (!All)
            result = vbslq_f32(masks32[k], result, v);
          values[j * kPackets + k] = result;
        } else {
          const auto v = vld1q_f64(x + i + j * kBlock + k * kLanes);
          auto result = vsubq_f64(sums[k][0], v);
          if constexpr (!All)
            result = vbslq_f64(masks64[k], result, v);
          values[j * kPackets + k] = result;
        }
      }
    }
    // Keep independent loads ahead of stores, including on 4 KiB aliases.
    asm volatile("" ::: "memory");
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (unsigned j = 0; j < kUnroll; ++j) {
      for (unsigned k = 0; k < kPackets; ++k) {
        if constexpr (sizeof(T) == 4) {
          vst1q_f32(y + i + j * kBlock + k * kLanes, values[j * kPackets + k]);
        } else {
          vst1q_f64(y + i + j * kBlock + k * kLanes, values[j * kPackets + k]);
        }
      }
    }
  }
  for (; i + kBlock <= count; i += kBlock) {
    for (unsigned k = 0; k < kPackets; ++k) {
      if constexpr (sizeof(T) == 4) {
        const auto v = vld1q_f32(x + i + k * kLanes);
        float32x4_t result;
        if constexpr (Narrow) {
          result = vsubq_f32(sums32[k], v);
        } else {
          const auto lo = vsubq_f64(sums[k][0], vcvt_f64_f32(vget_low_f32(v)));
          const auto hi = vsubq_f64(sums[k][1], vcvt_f64_f32(vget_high_f32(v)));
          result = vcombine_f32(vcvt_f32_f64(lo), vcvt_f32_f64(hi));
        }
        if constexpr (!All)
          result = vbslq_f32(masks32[k], result, v);
        vst1q_f32(y + i + k * kLanes, result);
      } else {
        const auto v = vld1q_f64(x + i + k * kLanes);
        auto result = vsubq_f64(sums[k][0], v);
        if constexpr (!All)
          result = vbslq_f64(masks64[k], result, v);
        vst1q_f64(y + i + k * kLanes, result);
      }
    }
  }
  return i;
}
#endif

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define PS_INVERT_AVX __attribute__((target("avx2")))
template <class T, unsigned C, bool Narrow, bool All, bool Streaming = false>
PS_INVERT_AVX inline std::size_t avx(const T* x, T* y, std::size_t count,
                                     const Control* controls) {
  constexpr unsigned kLanes = sizeof(T) == 4 && Narrow ? 8 : 4;
  constexpr unsigned kPackets = C == 3 ? 3 : 1;
  constexpr unsigned kBlock = kLanes * kPackets;
  std::size_t i = 0;
  // Coefficients below follow the channel phase after the alignment prefix.
  if (Streaming || count * sizeof(T) >= (C > 1 ? 4096 : 16 * 1024)) {
    constexpr unsigned kAlignment = Streaming ? 64 : 32;
    while (i < count && reinterpret_cast<std::uintptr_t>(y + i) % kAlignment) {
      scalar(x + i, y + i, controls[i % C]);
      ++i;
    }
  }
  __m256d sums[kPackets];
  __m256d masks64[kPackets];
  __m128 masks32[kPackets];
  __m256 sums32[kPackets], masks8[kPackets];
  for (unsigned k = 0; k < kPackets; ++k) {
    double s[kLanes];
    std::uint64_t m64[kLanes];
    std::uint32_t m32[kLanes];
    for (unsigned l = 0; l < kLanes; ++l) {
      const auto& c = controls[(i + k * kLanes + l) % C];
      s[l] = c.sum;
      m64[l] = c.active ? UINT64_MAX : 0;
      m32[l] = c.active ? UINT32_MAX : 0;
    }
    sums[k] = _mm256_loadu_pd(s);
    masks64[k] = _mm256_castsi256_pd(
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(m64)));
    masks32[k] = _mm_castsi128_ps(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(m32)));
    if constexpr (Narrow && sizeof(T) == 4) {
      float f[8];
      for (unsigned l = 0; l < 8; ++l)
        f[l] = static_cast<float>(s[l]);
      sums32[k] = _mm256_loadu_ps(f);
      masks8[k] = _mm256_castsi256_ps(
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(m32)));
    }
  }
  constexpr unsigned kUnroll =
      C == 3 ? (Streaming && sizeof(T) == 4 && !Narrow ? 4 : 2) : 8;
  using Vector =
      std::conditional_t<sizeof(T) == 4,
                         std::conditional_t<Narrow, __m256, __m128>, __m256d>;
  for (; i + kUnroll * kBlock <= count; i += kUnroll * kBlock) {
    Vector values[kUnroll * kPackets];
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (unsigned j = 0; j < kUnroll; ++j) {
      for (unsigned k = 0; k < kPackets; ++k) {
        if constexpr (sizeof(T) == 4 && Narrow) {
          const auto v = _mm256_loadu_ps(x + i + j * kBlock + k * kLanes);
          auto result = _mm256_sub_ps(sums32[k], v);
          if constexpr (!All)
            result = _mm256_blendv_ps(v, result, masks8[k]);
          values[j * kPackets + k] = result;
        } else if constexpr (sizeof(T) == 4) {
          const auto v = _mm_loadu_ps(x + i + j * kBlock + k * kLanes);
          auto result =
              _mm256_cvtpd_ps(_mm256_sub_pd(sums[k], _mm256_cvtps_pd(v)));
          if constexpr (!All)
            result = _mm_blendv_ps(v, result, masks32[k]);
          values[j * kPackets + k] = result;
        } else {
          const auto v = _mm256_loadu_pd(x + i + j * kBlock + k * kLanes);
          auto result = _mm256_sub_pd(sums[k], v);
          if constexpr (!All)
            result = _mm256_blendv_pd(v, result, masks64[k]);
          values[j * kPackets + k] = result;
        }
      }
    }
    // Keep independent loads ahead of stores, including on 4 KiB aliases.
    asm volatile("" ::: "memory");
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (unsigned j = 0; j < kUnroll; ++j) {
      for (unsigned k = 0; k < kPackets; ++k) {
        if constexpr (sizeof(T) == 4 && Narrow) {
          if constexpr (Streaming)
            _mm256_stream_ps(y + i + j * kBlock + k * kLanes,
                             values[j * kPackets + k]);
          else
            _mm256_storeu_ps(y + i + j * kBlock + k * kLanes,
                             values[j * kPackets + k]);
        } else if constexpr (sizeof(T) == 4) {
          if constexpr (Streaming)
            _mm_stream_ps(y + i + j * kBlock + k * kLanes,
                          values[j * kPackets + k]);
          else
            _mm_storeu_ps(y + i + j * kBlock + k * kLanes,
                          values[j * kPackets + k]);
        } else {
          if constexpr (Streaming)
            _mm256_stream_pd(y + i + j * kBlock + k * kLanes,
                             values[j * kPackets + k]);
          else
            _mm256_storeu_pd(y + i + j * kBlock + k * kLanes,
                             values[j * kPackets + k]);
        }
      }
    }
  }
  // The streamed loop writes complete cache lines; cached tails start after it.
  if constexpr (Streaming)
    _mm_sfence();
  for (; i + kBlock <= count; i += kBlock) {
    for (unsigned k = 0; k < kPackets; ++k) {
      if constexpr (sizeof(T) == 4 && Narrow) {
        const auto v = _mm256_loadu_ps(x + i + k * kLanes);
        auto result = _mm256_sub_ps(sums32[k], v);
        if constexpr (!All)
          result = _mm256_blendv_ps(v, result, masks8[k]);
        _mm256_storeu_ps(y + i + k * kLanes, result);
      } else if constexpr (sizeof(T) == 4) {
        const auto v = _mm_loadu_ps(x + i + k * kLanes);
        auto result =
            _mm256_cvtpd_ps(_mm256_sub_pd(sums[k], _mm256_cvtps_pd(v)));
        if constexpr (!All)
          result = _mm_blendv_ps(v, result, masks32[k]);
        _mm_storeu_ps(y + i + k * kLanes, result);
      } else {
        const auto v = _mm256_loadu_pd(x + i + k * kLanes);
        auto result = _mm256_sub_pd(sums[k], v);
        if constexpr (!All)
          result = _mm256_blendv_pd(v, result, masks64[k]);
        _mm256_storeu_pd(y + i + k * kLanes, result);
      }
    }
  }
  return i;
}
// The caller chooses streaming from the complete output size; every span
// completes its stores before returning, including before a cancellation poll.
template <class T>
PS_INVERT_AVX inline void stream_uniform(const T* x, T* y, std::size_t n,
                                         T sum) {
  std::size_t i = 0;
  while (i < n && reinterpret_cast<std::uintptr_t>(y + i) % 64) {
    y[i] = sum - x[i];
    ++i;
  }
  constexpr unsigned kLanes = 32 / sizeof(T);
  using Vector = std::conditional_t<sizeof(T) == 4, __m256, __m256d>;
  for (; i + 8 * kLanes <= n; i += 8 * kLanes) {
    Vector values[8];
    for (unsigned j = 0; j < 8; ++j) {
      if constexpr (sizeof(T) == 4)
        values[j] = _mm256_sub_ps(_mm256_set1_ps(sum),
                                  _mm256_loadu_ps(x + i + j * kLanes));
      else
        values[j] = _mm256_sub_pd(_mm256_set1_pd(sum),
                                  _mm256_loadu_pd(x + i + j * kLanes));
    }
    asm volatile("" ::: "memory");
    for (unsigned j = 0; j < 8; ++j) {
      if constexpr (sizeof(T) == 4)
        _mm256_stream_ps(y + i + j * kLanes, values[j]);
      else
        _mm256_stream_pd(y + i + j * kLanes, values[j]);
    }
  }
  _mm_sfence();
  for (; i < n; ++i)
    y[i] = sum - x[i];
}
#undef PS_INVERT_AVX
#endif

template <class T, unsigned C, bool Narrow, bool All>
inline void dispatch(const T* x, T* y, std::size_t count,
                     const Control* controls, bool streaming) {
  std::size_t i = 0;
#if defined(__aarch64__)
  static_cast<void>(streaming);
  i = neon<T, C, Narrow, All>(x, y, count, controls);
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  if (__builtin_cpu_supports("avx2")) {
    if (streaming)
      i = avx<T, C, Narrow, All, true>(x, y, count, controls);
    else
      i = avx<T, C, Narrow, All>(x, y, count, controls);
  }
#endif
  for (; i < count; ++i)
    scalar(x + i, y + i, controls[i % C]);
}

/** Reflect C interleaved channels with Float64 semantics and one final
 * conversion to T. Subtraction of two finite binary32 operands has the same
 * final binary32 result when evaluated directly or through binary64; use that
 * specialization only when the binary64 bound sum is exactly binary32.
 * Source and destination ranges are independent; bypass bits are copied.
 */
template <class T, unsigned C>
inline void span(const T* x, T* y, std::size_t pixels, const Control* controls,
                 bool streaming = false) {
  bool all = true, narrow = sizeof(T) == 4, uniform = true, any = false;
  for (unsigned c = 0; c < C; ++c) {
    all = all && controls[c].active;
    any = any || controls[c].active;
    uniform = uniform && controls[c].sum == controls[0].sum;
    if constexpr (sizeof(T) == 4)
      narrow = narrow &&
               (!controls[c].active ||
                (std::isfinite(controls[c].sum) &&
                 static_cast<double>(static_cast<float>(controls[c].sum)) ==
                     controls[c].sum));
  }
  const auto count = pixels * C;
  if (!any) {
    std::memcpy(y, x, count * sizeof(T));
    return;
  }
  if constexpr (C > 1) {
    if (all && uniform) {
      span<T, 1>(x, y, count, controls, streaming);
      return;
    }
  }
#if defined(__aarch64__) && defined(__APPLE__) && \
    defined(PHOTOSPIDER_HAS_ACCELERATE)
  // Multiplication by -1 is exact under the admitted binary32 sum profile.
  // Keep short and large memory spans in the local NEON loop.
  if constexpr (sizeof(T) == 4 && C == 1) {
    if (all && narrow && count >= 4096 && count <= 2 * 1024 * 1024) {
      const float negative = -1;
      const auto sum = static_cast<float>(controls[0].sum);
      vDSP_vsmsa(x, 1, &negative, &sum, y, 1, count);
      return;
    }
  }
#endif
#if defined(__aarch64__)
  if constexpr (sizeof(T) == 8 && C == 4) {
    if ((streaming || count * sizeof(T) >= 1024 * 1024) && count >= 512) {
      unsigned mask = 0;
      for (unsigned c = 0; c < 4; ++c)
        mask |= static_cast<unsigned>(controls[c].active) << c;
      if (mask == 7 || mask == 5) {
        std::size_t i = mask == 7 ? neon_four<7>(x, y, count, controls)
                                  : neon_four<5>(x, y, count, controls);
        for (; i < count; ++i)
          scalar(x + i, y + i, controls[i % C]);
        return;
      }
    }
  }
#endif
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  streaming = (streaming || count * sizeof(T) >= 1024 * 1024) &&
              count * sizeof(T) >= 16 * 1024;
  if constexpr (C == 1) {
    if (all && (sizeof(T) == 8 || narrow) && streaming &&
        __builtin_cpu_supports("avx2")) {
      stream_uniform(x, y, count, static_cast<T>(controls[0].sum));
      return;
    }
  }
#else
  static_cast<void>(streaming);
#endif
  if (all) {
    if (narrow)
      dispatch<T, C, true, true>(x, y, count, controls, streaming);
    else
      dispatch<T, C, false, true>(x, y, count, controls, streaming);
  } else {
    if (narrow)
      dispatch<T, C, true, false>(x, y, count, controls, streaming);
    else
      dispatch<T, C, false, false>(x, y, count, controls, streaming);
  }
}

template <class T>
inline void plane(const T* x, T* y, std::size_t count, const Control& control,
                  bool streaming = false) {
  span<T, 1>(x, y, count, &control, streaming);
}
}  // namespace ps::plugin_internal::grade::invert
