#include "02-format-color/model_simd.hpp"
#if defined(__x86_64__) && defined(__clang__)
#include <immintrin.h>
#endif
#if defined(__APPLE__) && defined(__aarch64__)
#include <arm_neon.h>
#endif
namespace ps::plugin_internal::model_ops {
namespace {
#if defined(__x86_64__) && defined(__clang__)
__attribute__((target("avx2"))) std::size_t avx2_dot(
    const std::array<const double*, 3>& x, const std::array<double, 3>& k,
    unsigned mask, double* out, std::size_t count) {
  std::size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    auto result = _mm256_setzero_pd();
    for (unsigned c = 0; c < 3; ++c)
      if (mask & (1u << c))
        result = _mm256_add_pd(result, _mm256_mul_pd(_mm256_loadu_pd(x[c] + i),
                                                     _mm256_set1_pd(k[c])));
    _mm256_storeu_pd(out + i, result);
  }
  return i;
}
#endif
}  // namespace
void dot_candidates(const std::array<const double*, 3>& x,
                    const std::array<double, 3>& k, unsigned mask, double* out,
                    std::size_t count, numeric_ops::SequenceProfile profile,
                    bool vectorize) {
  std::size_t i = 0;
#if defined(__x86_64__) && defined(__clang__)
  if (vectorize && profile == numeric_ops::SequenceProfile::X86Avx2)
    i = avx2_dot(x, k, mask, out, count);
#endif
#if defined(__APPLE__) && defined(__aarch64__)
  if (vectorize && profile == numeric_ops::SequenceProfile::AppleSilicon) {
    for (; i + 2 <= count; i += 2) {
      auto result = vdupq_n_f64(0);
      for (unsigned c = 0; c < 3; ++c)
        if (mask & (1u << c))
          result = vaddq_f64(result, vmulq_n_f64(vld1q_f64(x[c] + i), k[c]));
      vst1q_f64(out + i, result);
    }
  }
#endif
  (void)profile;
  (void)vectorize;
  for (; i < count; ++i) {
    double result = 0;
    for (unsigned c = 0; c < 3; ++c)
      if (mask & (1u << c))
        result += k[c] * x[c][i];
    out[i] = result;
  }
}
}  // namespace ps::plugin_internal::model_ops
