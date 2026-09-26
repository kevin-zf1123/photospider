#include "02-format-color/transfer_simd.hpp"

#include <cmath>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__AVX2__)
#include <immintrin.h>
#endif

namespace ps::plugin_internal::transfer_ops {
unsigned first_nonfinite(const std::uint8_t* input, unsigned count,
                         bool narrow) {
  unsigned i = 0;
  if (narrow) {
#if defined(__aarch64__)
    const auto mask = vdupq_n_u32(0x7f800000);
    for (; i + 4 <= count; i += 4) {
      uint32x4_t bits;
      std::memcpy(&bits, input + i * 4, sizeof(bits));
      if (vmaxvq_u32(vceqq_u32(vandq_u32(bits, mask), mask)))
        break;
    }
#elif defined(__AVX2__)
    const auto mask = _mm256_set1_epi32(0x7f800000);
    for (; i + 8 <= count; i += 8) {
      const auto bits =
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(input + i * 4));
      const auto failed =
          _mm256_cmpeq_epi32(_mm256_and_si256(bits, mask), mask);
      const auto lanes = _mm256_movemask_ps(_mm256_castsi256_ps(failed));
      if (lanes)
        return i + static_cast<unsigned>(__builtin_ctz(lanes));
    }
#endif
  } else {
#if defined(__aarch64__)
    const auto mask = vdupq_n_u64(UINT64_C(0x7ff0000000000000));
    for (; i + 2 <= count; i += 2) {
      uint64x2_t bits;
      std::memcpy(&bits, input + i * 8, sizeof(bits));
      const auto failed = vceqq_u64(vandq_u64(bits, mask), mask);
      if (vgetq_lane_u64(failed, 0))
        return i;
      if (vgetq_lane_u64(failed, 1))
        return i + 1;
    }
#elif defined(__AVX2__)
    const auto mask = _mm256_set1_epi64x(INT64_C(0x7ff0000000000000));
    for (; i + 4 <= count; i += 4) {
      const auto bits =
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(input + i * 8));
      const auto failed =
          _mm256_cmpeq_epi64(_mm256_and_si256(bits, mask), mask);
      const auto lanes = _mm256_movemask_pd(_mm256_castsi256_pd(failed));
      if (lanes)
        return i + static_cast<unsigned>(__builtin_ctz(lanes));
    }
#endif
  }
  const unsigned width = narrow ? 4 : 8;
  const auto mask =
      narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
  for (; i < count; ++i) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, input + i * width, width);
    if ((bits & mask) == mask)
      return i;
  }
  return count;
}
void gamma2_simd(const std::uint8_t* input, std::uint8_t* output,
                 unsigned count, bool narrow, bool encode) {
  unsigned i = 0;
  if (narrow) {
#if defined(__aarch64__)
    const auto sign = vdupq_n_u32(UINT32_C(0x80000000));
    const auto inf = vdupq_n_u32(UINT32_C(0x7f800000));
    const auto quiet = vdupq_n_u32(UINT32_C(0x00400000));
    for (; i + 4 <= count; i += 4) {
      uint32x4_t bits;
      std::memcpy(&bits, input + i * 4, sizeof(bits));
      const auto magnitude = vbicq_u32(bits, sign);
      const auto nan = vcgtq_u32(magnitude, inf);
      const auto x = vreinterpretq_f32_u32(vbicq_u32(magnitude, nan));
      const auto y = encode ? vsqrtq_f32(x) : vmulq_f32(x, x);
      const auto result =
          vbslq_u32(nan, vorrq_u32(bits, quiet),
                    vorrq_u32(vreinterpretq_u32_f32(y), vandq_u32(bits, sign)));
      std::memcpy(output + i * 4, &result, sizeof(result));
    }
#elif defined(__AVX2__)
    const auto sign = _mm256_set1_epi32(static_cast<int>(UINT32_C(0x80000000)));
    const auto inf = _mm256_set1_epi32(0x7f800000);
    const auto quiet = _mm256_set1_epi32(0x00400000);
    for (; i + 8 <= count; i += 8) {
      const auto bits =
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(input + i * 4));
      const auto magnitude = _mm256_andnot_si256(sign, bits);
      const auto nan = _mm256_cmpgt_epi32(magnitude, inf);
      const auto x = _mm256_castsi256_ps(_mm256_andnot_si256(nan, magnitude));
      const auto y = encode ? _mm256_sqrt_ps(x) : _mm256_mul_ps(x, x);
      const auto result = _mm256_blendv_epi8(
          _mm256_or_si256(_mm256_castps_si256(y), _mm256_and_si256(bits, sign)),
          _mm256_or_si256(bits, quiet), nan);
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(output + i * 4), result);
    }
#endif
    for (; i < count; ++i) {
      std::uint32_t bits;
      std::memcpy(&bits, input + i * 4, 4);
      const auto magnitude = bits & UINT32_C(0x7fffffff);
      if (magnitude > UINT32_C(0x7f800000)) {
        bits |= UINT32_C(0x00400000);
      } else {
        float x;
        std::memcpy(&x, &magnitude, 4);
        const float y = encode ? std::sqrt(x) : x * x;
        auto sign = bits & UINT32_C(0x80000000);
        std::memcpy(&bits, &y, 4);
        bits |= sign;
      }
      std::memcpy(output + i * 4, &bits, 4);
    }
  } else {
#if defined(__aarch64__)
    const auto sign = vdupq_n_u64(UINT64_C(0x8000000000000000));
    const auto inf = vdupq_n_u64(UINT64_C(0x7ff0000000000000));
    const auto quiet = vdupq_n_u64(UINT64_C(0x0008000000000000));
    for (; i + 2 <= count; i += 2) {
      uint64x2_t bits;
      std::memcpy(&bits, input + i * 8, sizeof(bits));
      const auto magnitude = vbicq_u64(bits, sign);
      const auto nan = vcgtq_u64(magnitude, inf);
      const auto x = vreinterpretq_f64_u64(vbicq_u64(magnitude, nan));
      const auto y = encode ? vsqrtq_f64(x) : vmulq_f64(x, x);
      const auto result =
          vbslq_u64(nan, vorrq_u64(bits, quiet),
                    vorrq_u64(vreinterpretq_u64_f64(y), vandq_u64(bits, sign)));
      std::memcpy(output + i * 8, &result, sizeof(result));
    }
#elif defined(__AVX2__)
    const auto sign = _mm256_set1_epi64x(INT64_MIN);
    const auto inf = _mm256_set1_epi64x(INT64_C(0x7ff0000000000000));
    const auto quiet = _mm256_set1_epi64x(INT64_C(0x0008000000000000));
    for (; i + 4 <= count; i += 4) {
      const auto bits =
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(input + i * 8));
      const auto magnitude = _mm256_andnot_si256(sign, bits);
      const auto nan = _mm256_cmpgt_epi64(magnitude, inf);
      const auto x = _mm256_castsi256_pd(_mm256_andnot_si256(nan, magnitude));
      const auto y = encode ? _mm256_sqrt_pd(x) : _mm256_mul_pd(x, x);
      const auto result = _mm256_blendv_epi8(
          _mm256_or_si256(_mm256_castpd_si256(y), _mm256_and_si256(bits, sign)),
          _mm256_or_si256(bits, quiet), nan);
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(output + i * 8), result);
    }
#endif
    for (; i < count; ++i) {
      std::uint64_t bits;
      std::memcpy(&bits, input + i * 8, 8);
      const auto magnitude = bits & UINT64_C(0x7fffffffffffffff);
      if (magnitude > UINT64_C(0x7ff0000000000000)) {
        bits |= UINT64_C(0x0008000000000000);
      } else {
        double x;
        std::memcpy(&x, &magnitude, 8);
        const double y = encode ? std::sqrt(x) : x * x;
        auto sign = bits & UINT64_C(0x8000000000000000);
        std::memcpy(&bits, &y, 8);
        bits |= sign;
      }
      std::memcpy(output + i * 8, &bits, 8);
    }
  }
}
}  // namespace ps::plugin_internal::transfer_ops
