// Copyright 2026 Photospider contributors
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__)
#include <immintrin.h>
#endif

namespace ps::plugin_internal::restoration {
#if defined(__x86_64__)
inline bool wavelet_avx2_available() {
  static const bool available = __builtin_cpu_supports("avx2");
  return available;
}
#define PS_RES03_VECTOR __attribute__((target("avx2")))
#else
#define PS_RES03_VECTOR
#endif

#if defined(__aarch64__) || defined(__x86_64__)
// Positive IEEE magnitude bits have the same order as their numerical values.
// NaN magnitudes sort above infinity, so the integer comparison is ordered
// false for every valid threshold, including a native overflow to infinity.
// This also avoids scalarized NEON floating comparisons in strict FP builds.
template <class T, bool Soft>
PS_RES03_VECTOR inline std::size_t threshold_packets(const T* src, T* dst,
                                                     std::size_t n,
                                                     T threshold) {
  std::size_t i = 0;
#if defined(__aarch64__)
  if constexpr (std::is_same_v<T, float>) {
    const auto t = vdupq_n_f32(threshold);
    const auto threshold_bits = vreinterpretq_u32_f32(t);
    const auto sign = vdupq_n_u32(UINT32_C(0x80000000));
    for (; n - i >= 4; i += 4) {
      const auto v = vld1q_f32(src + i), a = vabsq_f32(v);
      if constexpr (Soft) {
        const auto bits = vorrq_u32(vandq_u32(vreinterpretq_u32_f32(v), sign),
                                    vreinterpretq_u32_f32(vsubq_f32(a, t)));
        vst1q_f32(
            dst + i,
            vreinterpretq_f32_u32(vbicq_u32(
                bits, vcleq_u32(vreinterpretq_u32_f32(a), threshold_bits))));
      } else {
        vst1q_f32(dst + i,
                  vreinterpretq_f32_u32(vbicq_u32(
                      vreinterpretq_u32_f32(v),
                      vcltq_u32(vreinterpretq_u32_f32(a), threshold_bits))));
      }
    }
  } else {
    const auto t = vdupq_n_f64(threshold);
    const auto threshold_bits = vreinterpretq_u64_f64(t);
    const auto sign = vdupq_n_u64(UINT64_C(0x8000000000000000));
    for (; n - i >= 2; i += 2) {
      const auto v = vld1q_f64(src + i), a = vabsq_f64(v);
      if constexpr (Soft) {
        const auto bits = vorrq_u64(vandq_u64(vreinterpretq_u64_f64(v), sign),
                                    vreinterpretq_u64_f64(vsubq_f64(a, t)));
        vst1q_f64(
            dst + i,
            vreinterpretq_f64_u64(vbicq_u64(
                bits, vcleq_u64(vreinterpretq_u64_f64(a), threshold_bits))));
      } else {
        vst1q_f64(dst + i,
                  vreinterpretq_f64_u64(vbicq_u64(
                      vreinterpretq_u64_f64(v),
                      vcltq_u64(vreinterpretq_u64_f64(a), threshold_bits))));
      }
    }
  }
#else
  if constexpr (std::is_same_v<T, float>) {
    const auto t = _mm256_set1_ps(threshold), sign = _mm256_set1_ps(-0.0f);
    for (; n - i >= 8; i += 8) {
      const auto v = _mm256_loadu_ps(src + i), a = _mm256_andnot_ps(sign, v);
      if constexpr (Soft) {
        const auto value =
            _mm256_or_ps(_mm256_and_ps(sign, v), _mm256_sub_ps(a, t));
        _mm256_storeu_ps(
            dst + i, _mm256_andnot_ps(_mm256_cmp_ps(a, t, _CMP_LE_OQ), value));
      } else {
        _mm256_storeu_ps(dst + i,
                         _mm256_andnot_ps(_mm256_cmp_ps(a, t, _CMP_LT_OQ), v));
      }
    }
  } else {
    const auto t = _mm256_set1_pd(threshold), sign = _mm256_set1_pd(-0.0);
    for (; n - i >= 4; i += 4) {
      const auto v = _mm256_loadu_pd(src + i), a = _mm256_andnot_pd(sign, v);
      if constexpr (Soft) {
        const auto value =
            _mm256_or_pd(_mm256_and_pd(sign, v), _mm256_sub_pd(a, t));
        _mm256_storeu_pd(
            dst + i, _mm256_andnot_pd(_mm256_cmp_pd(a, t, _CMP_LE_OQ), value));
      } else {
        _mm256_storeu_pd(dst + i,
                         _mm256_andnot_pd(_mm256_cmp_pd(a, t, _CMP_LT_OQ), v));
      }
    }
  }
#endif
  return i;
}

template <class T, bool Divide>
PS_RES03_VECTOR inline std::size_t accumulate_packets(const T* a, const T* b,
                                                      T* dst, std::size_t n,
                                                      T divisor) {
  std::size_t i = 0;
#if defined(__aarch64__)
  if constexpr (std::is_same_v<T, float>) {
    const auto div = vdupq_n_f32(divisor);
    for (; n - i >= 4; i += 4) {
      auto v = vaddq_f32(vld1q_f32(a + i), vld1q_f32(b + i));
      if constexpr (Divide)
        v = vdivq_f32(v, div);
      vst1q_f32(dst + i, v);
    }
  } else {
    const auto div = vdupq_n_f64(divisor);
    for (; n - i >= 2; i += 2) {
      auto v = vaddq_f64(vld1q_f64(a + i), vld1q_f64(b + i));
      if constexpr (Divide)
        v = vdivq_f64(v, div);
      vst1q_f64(dst + i, v);
    }
  }
#else
  if constexpr (std::is_same_v<T, float>) {
    const auto div = _mm256_set1_ps(divisor);
    for (; n - i >= 8; i += 8) {
      auto v = _mm256_add_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
      if constexpr (Divide)
        v = _mm256_div_ps(v, div);
      _mm256_storeu_ps(dst + i, v);
    }
  } else {
    const auto div = _mm256_set1_pd(divisor);
    for (; n - i >= 4; i += 4) {
      auto v = _mm256_add_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i));
      if constexpr (Divide)
        v = _mm256_div_pd(v, div);
      _mm256_storeu_pd(dst + i, v);
    }
  }
#endif
  return i;
}
#endif
#undef PS_RES03_VECTOR

template <class T, bool Soft>
inline void threshold_tail(const T* source, T* output, std::size_t i,
                           std::size_t n, T t) {
  using Word = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
  constexpr Word sign = Word(1) << (8 * sizeof(T) - 1);
  Word threshold_bits;
  std::memcpy(&threshold_bits, &t, sizeof(T));
#if defined(__clang__)
#pragma clang loop vectorize(disable) interleave(disable)
#endif
  for (; i < n; ++i) {
    Word bits;
    std::memcpy(&bits, source + i, sizeof(T));
    const Word magnitude = bits & ~sign;
    if constexpr (Soft) {
      const T difference = std::fabs(source[i]) - t;
      Word result;
      std::memcpy(&result, &difference, sizeof(T));
      result = (result & ~sign) | (bits & sign);
      result &= Word(0) - Word(magnitude > threshold_bits);
      std::memcpy(output + i, &result, sizeof(T));
    } else {
      bits &= Word(0) - Word(magnitude >= threshold_bits);
      std::memcpy(output + i, &bits, sizeof(T));
    }
  }
}
// Zero thresholds copy bits, including signalling NaNs and signed zero.
// Ordered comparisons retain NaNs in the arithmetic branch. Clearing lanes
// with a mask produces positive zero in both modes.
template <class T, bool Soft>
inline void threshold_span(const T* source, T* output, std::size_t n, T t) {
  if (t == T(0)) {
    if (source != output)
      std::memcpy(output, source, n * sizeof(T));
    return;
  }
  std::size_t i = 0;
#if defined(__aarch64__)
  i = threshold_packets<T, Soft>(source, output, n, t);
#elif defined(__x86_64__)
  if (n >= 32 / sizeof(T) && wavelet_avx2_available())
    i = threshold_packets<T, Soft>(source, output, n, t);
#endif
  threshold_tail<T, Soft>(source, output, i, n, t);
}

#if defined(__x86_64__)
#define PS_RES03_ROWS __attribute__((target("avx2")))
#else
#define PS_RES03_ROWS
#endif
#if defined(__aarch64__) || defined(__x86_64__)
// The caller bounds this row block before entering the SIMD target. Dispatch
// once per block so narrow rows do not repeatedly cross an ISA call boundary.
template <class T, bool Soft>
PS_RES03_ROWS inline void threshold_row_packets(
    const T* source, T* output, std::size_t rows, std::size_t width,
    std::ptrdiff_t source_stride, std::ptrdiff_t output_stride, T threshold) {
  for (std::size_t y = 0; y < rows; ++y) {
    const auto* src = source + static_cast<std::ptrdiff_t>(y) * source_stride;
    auto* dst = output + static_cast<std::ptrdiff_t>(y) * output_stride;
    const auto first = threshold_packets<T, Soft>(src, dst, width, threshold);
    threshold_tail<T, Soft>(src, dst, first, width, threshold);
  }
}
#endif
#undef PS_RES03_ROWS
template <class T, bool Soft>
inline void threshold_rows(const T* source, T* output, std::size_t rows,
                           std::size_t width, std::ptrdiff_t source_stride,
                           std::ptrdiff_t output_stride, T threshold) {
  if (threshold == T(0)) {
    for (std::size_t y = 0; y < rows; ++y) {
      const auto* src = source + static_cast<std::ptrdiff_t>(y) * source_stride;
      auto* dst = output + static_cast<std::ptrdiff_t>(y) * output_stride;
      if (src != dst)
        std::memcpy(dst, src, width * sizeof(T));
    }
    return;
  }
#if defined(__aarch64__)
  threshold_row_packets<T, Soft>(source, output, rows, width, source_stride,
                                 output_stride, threshold);
  return;
#elif defined(__x86_64__)
  if (wavelet_avx2_available()) {
    threshold_row_packets<T, Soft>(source, output, rows, width, source_stride,
                                   output_stride, threshold);
    return;
  }
#endif
  for (std::size_t y = 0; y < rows; ++y)
    threshold_tail<T, Soft>(
        source + static_cast<std::ptrdiff_t>(y) * source_stride,
        output + static_cast<std::ptrdiff_t>(y) * output_stride, 0, width,
        threshold);
}

template <class T, bool Divide>
inline void accumulate_native(const T* a, const T* b, T* out, std::size_t n,
                              T divisor) {
  std::size_t i = 0;
#if defined(__aarch64__)
  i = accumulate_packets<T, Divide>(a, b, out, n, divisor);
#elif defined(__x86_64__)
  if (n >= 32 / sizeof(T) && wavelet_avx2_available())
    i = accumulate_packets<T, Divide>(a, b, out, n, divisor);
#endif
  for (; i < n; ++i) {
    const T sum = a[i] + b[i];
    out[i] = Divide ? sum / divisor : sum;
  }
}
template <class T>
inline void accumulate_span(const T* a, const T* b, T* out, std::size_t n,
                            T divisor) {
  if (divisor == T(1))
    accumulate_native<T, false>(a, b, out, n, divisor);
  else
    accumulate_native<T, true>(a, b, out, n, divisor);
}
}  // namespace ps::plugin_internal::restoration
