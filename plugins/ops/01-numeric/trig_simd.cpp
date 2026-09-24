// Taylor polynomials with independently bounded coefficient/FMA/rounding error.
// Certificate and coefficient check: examples/numeric_workflow/trig_bound.py.
#include "01-numeric/trig_simd.hpp"

#include <cstddef>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>
#endif

namespace ps::plugin_internal::numeric_ops {
namespace {
constexpr float kSinc[] = {
    0x1p0f,           -0x1.555556p-3f, 0x1.111112p-7f,
    -0x1.a01a02p-13f, 0x1.71de3ap-19f, -0x1.ae6456p-26f,
};
constexpr float kCos[] = {
    0x1p0f,          -0x1p-1f,         0x1.555556p-5f,  -0x1.6c16c2p-10f,
    0x1.a01a02p-16f, -0x1.27e4fcp-22f, 0x1.1eed8ep-29f,
};
constexpr float kSinpi[] = {
    0x1.921fb6p1f,   -0x1.4abbcep2f, 0x1.466bc6p1f,
    -0x1.32d2ccp-1f, 0x1.507834p-4f, -0x1.e30750p-8f,
};
constexpr float kCospi[] = {
    0x1p0f,         -0x1.3bd3ccp2f,  0x1.03c1f0p2f,   -0x1.55d3c8p0f,
    0x1.e1f506p-3f, -0x1.a6d1f2p-6f, 0x1.f9d38ap-10f,
};
constexpr double kSincpiWide[] = {
    0x1.0000000000000p+0,  -0x1.a51a6625307d3p+0,  0x1.9f9cb402bc46cp-1,
    -0x1.86a8e4720db67p-3, 0x1.ac6805cf350a6p-6,   -0x1.33816aa4607abp-9,
    0x1.374719fab3915p-13, -0x1.d42498d1ce099p-18, 0x1.0fc992ff39e13p-22,
};
#if defined(__aarch64__)
using Vector = float32x4_t;
constexpr std::size_t kWidth = 4;
Vector load(const float* x) {
  return vld1q_f32(x);
}
void store(float* x, Vector v) {
  vst1q_f32(x, v);
}
Vector splat(float x) {
  return vdupq_n_f32(x);
}
Vector multiply(Vector a, Vector b) {
  return vmulq_f32(a, b);
}
Vector madd(Vector a, Vector b, Vector c) {
  return vfmaq_f32(c, a, b);
}
#elif defined(__AVX2__) && defined(__FMA__)
using Vector = __m256;
constexpr std::size_t kWidth = 8;
Vector load(const float* x) {
  return _mm256_loadu_ps(x);
}
void store(float* x, Vector v) {
  _mm256_storeu_ps(x, v);
}
Vector splat(float x) {
  return _mm256_set1_ps(x);
}
Vector multiply(Vector a, Vector b) {
  return _mm256_mul_ps(a, b);
}
Vector madd(Vector a, Vector b, Vector c) {
  return _mm256_fmadd_ps(a, b, c);
}
#endif

template <bool Sine, std::size_t N>
void polynomial(const float (&coefficients)[N], const float* input,
                float* output, std::size_t count) {
#if defined(__aarch64__) || (defined(__AVX2__) && defined(__FMA__))
  for (std::size_t offset = 0; offset < count; offset += kWidth) {
    float tail[kWidth];
    const auto remaining = count - offset;
    const auto* source = input + offset;
    if (remaining < kWidth) {
      for (std::size_t lane = 0; lane < kWidth; ++lane)
        tail[lane] = source[lane < remaining ? lane : 0];
      source = tail;
    }
    const auto x = load(source), t = multiply(x, x);
    auto p = splat(coefficients[N - 1]);
    for (std::size_t j = N - 1; j; --j)
      p = madd(p, t, splat(coefficients[j - 1]));
    if constexpr (Sine)
      p = multiply(x, p);
    if (remaining >= kWidth) {
      store(output + offset, p);
    } else {
      store(tail, p);
      std::memcpy(output + offset, tail, remaining * sizeof(float));
    }
  }
#else
  // Unreachable after ISA admission.
  std::memcpy(output, input, count * sizeof(float));
#endif
}
// Exact integer-unit reduction, central sincpi polynomial, then (-1)^n*r/x.
// Binary64 intermediates give the division/product their own proved error
// budget. Inputs and final storage remain binary32; no rounded pi*x is formed.
void sincpi_reduced(const float* input, float* output, std::size_t count) {
#if defined(__aarch64__) || (defined(__AVX2__) && defined(__FMA__))
#if defined(__aarch64__)
  constexpr std::size_t kDoubleWidth = 2;
#else
  constexpr std::size_t kDoubleWidth = 4;
#endif
  for (std::size_t offset = 0; offset < count; offset += kDoubleWidth) {
    double xs[kDoubleWidth], ys[kDoubleWidth];
    bool integers[kDoubleWidth];
    bool central = true;
    const auto remaining = count - offset;
    for (std::size_t lane = 0; lane < kDoubleWidth; ++lane) {
      std::uint32_t bits;
      std::memcpy(&bits, input + offset + (lane < remaining ? lane : 0), 4);
      bits &= UINT32_C(0x7fffffff);
      // All finite binary32 values >=2^23 are integers. Domain admission
      // excludes NaN/Inf before this helper; do not convert huge values to int.
      integers[lane] = bits >= UINT32_C(0x4b000000);
      if (integers[lane])
        bits = 0;
      float value;
      std::memcpy(&value, &bits, 4);
      xs[lane] = value;
      central = central && value <= .5f;
    }
#if defined(__aarch64__)
    const auto x = vld1q_f64(xs), n = vrndnq_f64(x), r = vsubq_f64(x, n);
    const auto t = vmulq_f64(r, r);
    auto p = vdupq_n_f64(kSincpiWide[8]);
    for (unsigned j = 8; j; --j)
      p = vfmaq_f64(vdupq_n_f64(kSincpiWide[j - 1]), p, t);
    if (!central) {
      const auto at_origin = vceqq_f64(n, vdupq_n_f64(0));
      const auto divisor = vbslq_f64(at_origin, vdupq_n_f64(1), x);
      auto q = vmulq_f64(vdivq_f64(r, divisor), p);
      const auto parity = vshlq_n_u64(
          vandq_u64(vreinterpretq_u64_s64(vcvtq_s64_f64(n)), vdupq_n_u64(1)),
          63);
      q = vreinterpretq_f64_u64(veorq_u64(vreinterpretq_u64_f64(q), parity));
      // Nonzero integer arguments have positive zero, independent of parity.
      q = vbslq_f64(vceqq_f64(r, vdupq_n_f64(0)), vdupq_n_f64(0), q);
      p = vbslq_f64(at_origin, p, q);
    }
    vst1q_f64(ys, p);
#else
    const auto x = _mm256_loadu_pd(xs);
    const auto n =
        _mm256_round_pd(x, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    const auto r = _mm256_sub_pd(x, n), t = _mm256_mul_pd(r, r);
    auto p = _mm256_set1_pd(kSincpiWide[8]);
    for (unsigned j = 8; j; --j)
      p = _mm256_fmadd_pd(p, t, _mm256_set1_pd(kSincpiWide[j - 1]));
    if (!central) {
      const auto at_origin = _mm256_cmp_pd(n, _mm256_setzero_pd(), _CMP_EQ_OQ);
      const auto divisor = _mm256_blendv_pd(x, _mm256_set1_pd(1), at_origin);
      auto q = _mm256_mul_pd(_mm256_div_pd(r, divisor), p);
      const auto parity =
          _mm256_slli_epi64(_mm256_cvtepi32_epi64(_mm256_cvttpd_epi32(n)), 63);
      q = _mm256_xor_pd(q, _mm256_castsi256_pd(parity));
      q = _mm256_blendv_pd(q, _mm256_setzero_pd(),
                           _mm256_cmp_pd(r, _mm256_setzero_pd(), _CMP_EQ_OQ));
      p = _mm256_blendv_pd(q, p, at_origin);
    }
    _mm256_storeu_pd(ys, p);
#endif
    for (std::size_t lane = 0; lane < kDoubleWidth && lane < remaining; ++lane)
      output[offset + lane] = integers[lane] ? 0 : static_cast<float>(ys[lane]);
  }
#else
  std::memcpy(output, input, count * sizeof(float));
#endif
}
}  // namespace
void trig_simd_f32(unsigned kind, const float* input, float* output,
                   std::size_t count) {
  switch (kind) {
    case 2:
      return polynomial<true>(kSinc, input, output, count);
    case 3:
      return polynomial<false>(kCos, input, output, count);
    case 5:
      return polynomial<true>(kSinpi, input, output, count);
    case 6:
      return polynomial<false>(kCospi, input, output, count);
    case 8:
      return polynomial<false>(kSinc, input, output, count);
    case 9:
      return sincpi_reduced(input, output, count);
  }
}
}  // namespace ps::plugin_internal::numeric_ops
