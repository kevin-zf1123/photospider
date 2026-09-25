// Normal-range expf adapted from ik_llama.cpp iqk_utils.h, commit
// dad2cb3e55138cbdd7df988c66c0c3c54f6d34ad. Original attribution:
// Justine Tunney, adapted from Arm Limited's optimized routine.
// MIT terms and upstream copyright: third_party/IK_LLAMA_LICENSE.txt.
// The full-domain fallback is owned by NUM-04, not by this polynomial.
#include "01-numeric/exp_simd.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>
#endif

namespace ps::plugin_internal::numeric_ops {
namespace {
#if defined(__aarch64__)
inline float32x4_t exp_normal(float32x4_t x) {
  const float32x4_t r = vdupq_n_f32(0x1.8p23f);
  const float32x4_t z = vfmaq_f32(r, x, vdupq_n_f32(0x1.715476p+0f));
  const float32x4_t n = vsubq_f32(z, r);
  const float32x4_t b = vfmsq_f32(vfmsq_f32(x, n, vdupq_n_f32(0x1.62e4p-1f)), n,
                                  vdupq_n_f32(0x1.7f7d1cp-20f));
  const uint32x4_t e = vshlq_n_u32(vreinterpretq_u32_f32(z), 23);
  const float32x4_t k = vreinterpretq_f32_u32(
      vaddq_u32(e, vreinterpretq_u32_f32(vdupq_n_f32(1))));
  const float32x4_t u = vmulq_f32(b, b);
  const float32x4_t j =
      vfmaq_f32(vmulq_f32(vdupq_n_f32(0x1.ffffecp-1f), b),
                vfmaq_f32(vfmaq_f32(vdupq_n_f32(0x1.fffdb6p-2f),
                                    vdupq_n_f32(0x1.555e66p-3f), b),
                          vfmaq_f32(vdupq_n_f32(0x1.573e2ep-5f),
                                    vdupq_n_f32(0x1.0e4020p-7f), b),
                          u),
                u);
  return vfmaq_f32(k, j, k);
}
#elif defined(__AVX2__) && defined(__FMA__)
inline __m256 exp_normal(__m256 x) {
  const __m256 r = _mm256_set1_ps(0x1.8p23f);
  const __m256 z = _mm256_fmadd_ps(x, _mm256_set1_ps(0x1.715476p+0f), r);
  const __m256 n = _mm256_sub_ps(z, r);
  const __m256 b =
      _mm256_fnmadd_ps(n, _mm256_set1_ps(0x1.7f7d1cp-20f),
                       _mm256_fnmadd_ps(n, _mm256_set1_ps(0x1.62e4p-1f), x));
  const __m256i e = _mm256_slli_epi32(_mm256_castps_si256(z), 23);
  const __m256 k = _mm256_castsi256_ps(
      _mm256_add_epi32(e, _mm256_castps_si256(_mm256_set1_ps(1))));
  const __m256 u = _mm256_mul_ps(b, b);
  const __m256 j = _mm256_fmadd_ps(
      _mm256_fmadd_ps(_mm256_fmadd_ps(_mm256_set1_ps(0x1.0e4020p-7f), b,
                                      _mm256_set1_ps(0x1.573e2ep-5f)),
                      u,
                      _mm256_fmadd_ps(_mm256_set1_ps(0x1.555e66p-3f), b,
                                      _mm256_set1_ps(0x1.fffdb6p-2f))),
      u, _mm256_mul_ps(_mm256_set1_ps(0x1.ffffecp-1f), b));
  return _mm256_fmadd_ps(j, k, k);
}
#endif
}  // namespace

void exp_simd_f32(const float* input, float* output, std::size_t count) {
  // Caller supplies finite [-80,80] arguments, including duplicated tail lanes,
  // under nearest rounding with gradual underflow. See exp_bound.py.
#if defined(__aarch64__)
  constexpr std::size_t kWidth = 4;
#elif defined(__AVX2__) && defined(__FMA__)
  constexpr std::size_t kWidth = 8;
#else
  constexpr std::size_t kWidth = 1;
#endif
  for (std::size_t offset = 0; offset < count; offset += kWidth) {
    float tail[kWidth];
    const auto remaining = count - offset;
    const float* source = input + offset;
    if (remaining < kWidth) {
      for (std::size_t lane = 0; lane < kWidth; ++lane)
        tail[lane] = source[lane < remaining ? lane : 0];
      source = tail;
    }
#if defined(__aarch64__)
    const auto result = exp_normal(vld1q_f32(source));
    if (remaining >= kWidth) {
      vst1q_f32(output + offset, result);
    } else {
      vst1q_f32(tail, result);
      std::memcpy(output + offset, tail, remaining * sizeof(float));
    }
#elif defined(__AVX2__) && defined(__FMA__)
    const auto result = exp_normal(_mm256_loadu_ps(source));
    if (remaining >= kWidth) {
      _mm256_storeu_ps(output + offset, result);
    } else {
      _mm256_storeu_ps(tail, result);
      std::memcpy(output + offset, tail, remaining * sizeof(float));
    }
#else
    // This adapter is never selected without ISA admission.
    output[offset] = source[0];
#endif
  }
}
}  // namespace ps::plugin_internal::numeric_ops
