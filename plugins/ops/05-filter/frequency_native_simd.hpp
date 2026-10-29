// Copyright 2026 Photospider contributors
#pragma once
#include <cstring>
#include <type_traits>

#include "05-filter/frequency_fft_simd.hpp"

namespace ps::plugin_internal::frequency {
#if PS_FRQ_PACKETS
// Real packets share FFT's ISA admission, without complex interleaving.
template <class S>
struct NativePacket;
template <>
struct NativePacket<float> {
#if defined(__x86_64__)
  using Register = __m256;
  static constexpr unsigned width = 8;
#else
  using Register = float32x4_t;
  static constexpr unsigned width = 4;
#endif
  struct Pair {
    Register even, odd;
  };
  static PS_FRQ_PACKET_INLINE Register load(const float* p) {
    Register v;
    std::memcpy(&v, p, sizeof(v));
    return v;
  }
  static PS_FRQ_PACKET_INLINE void store(float* p, Register v) {
    std::memcpy(p, &v, sizeof(v));
  }
  static PS_FRQ_PACKET_INLINE Register zero() {
#if defined(__x86_64__)
    return _mm256_setzero_ps();
#else
    return vdupq_n_f32(0);
#endif
  }
  static PS_FRQ_PACKET_INLINE Register add(Register a, Register b) {
#if defined(__x86_64__)
    return _mm256_add_ps(a, b);
#else
    return vaddq_f32(a, b);
#endif
  }
  static PS_FRQ_PACKET_INLINE Register sub(Register a, Register b) {
#if defined(__x86_64__)
    return _mm256_sub_ps(a, b);
#else
    return vsubq_f32(a, b);
#endif
  }
  static PS_FRQ_PACKET_INLINE Register scale(Register a, float b) {
#if defined(__x86_64__)
    return _mm256_mul_ps(a, _mm256_set1_ps(b));
#else
    return vmulq_n_f32(a, b);
#endif
  }
  static PS_FRQ_PACKET_INLINE Register madd(Register sum, Register a, float b) {
#if defined(__x86_64__)
    return _mm256_fmadd_ps(a, _mm256_set1_ps(b), sum);
#else
    return vfmaq_n_f32(sum, a, b);
#endif
  }
  static PS_FRQ_PACKET_INLINE Pair pairs(const float* p) {
#if defined(__x86_64__)
    const auto a = load(p), b = load(p + width);
    const auto lo = _mm256_permute2f128_ps(a, b, 0x20);
    const auto hi = _mm256_permute2f128_ps(a, b, 0x31);
    return {_mm256_shuffle_ps(lo, hi, 0x88), _mm256_shuffle_ps(lo, hi, 0xdd)};
#else
    const auto a = vld2q_f32(p);
    return {a.val[0], a.val[1]};
#endif
  }
  static PS_FRQ_PACKET_INLINE void pairs_store(float* p, Register even,
                                               Register odd) {
#if defined(__x86_64__)
    const auto lo = _mm256_unpacklo_ps(even, odd);
    const auto hi = _mm256_unpackhi_ps(even, odd);
    store(p, _mm256_permute2f128_ps(lo, hi, 0x20));
    store(p + width, _mm256_permute2f128_ps(lo, hi, 0x31));
#else
    const float32x4x2_t a{{even, odd}};
    vst2q_f32(p, a);
#endif
  }
};
template <>
struct NativePacket<double> {
#if defined(__x86_64__)
  using Register = __m256d;
  static constexpr unsigned width = 4;
#else
  using Register = float64x2_t;
  static constexpr unsigned width = 2;
#endif
  struct Pair {
    Register even, odd;
  };
  static PS_FRQ_PACKET_INLINE Register load(const double* p) {
    Register v;
    std::memcpy(&v, p, sizeof(v));
    return v;
  }
  static PS_FRQ_PACKET_INLINE void store(double* p, Register v) {
    std::memcpy(p, &v, sizeof(v));
  }
  static PS_FRQ_PACKET_INLINE Register zero() {
#if defined(__x86_64__)
    return _mm256_setzero_pd();
#else
    return vdupq_n_f64(0);
#endif
  }
  static PS_FRQ_PACKET_INLINE Register add(Register a, Register b) {
#if defined(__x86_64__)
    return _mm256_add_pd(a, b);
#else
    return vaddq_f64(a, b);
#endif
  }
  static PS_FRQ_PACKET_INLINE Register sub(Register a, Register b) {
#if defined(__x86_64__)
    return _mm256_sub_pd(a, b);
#else
    return vsubq_f64(a, b);
#endif
  }
  static PS_FRQ_PACKET_INLINE Register scale(Register a, double b) {
#if defined(__x86_64__)
    return _mm256_mul_pd(a, _mm256_set1_pd(b));
#else
    return vmulq_n_f64(a, b);
#endif
  }
  static PS_FRQ_PACKET_INLINE Register madd(Register sum, Register a,
                                            double b) {
#if defined(__x86_64__)
    return _mm256_fmadd_pd(a, _mm256_set1_pd(b), sum);
#else
    return vfmaq_n_f64(sum, a, b);
#endif
  }
  static PS_FRQ_PACKET_INLINE Pair pairs(const double* p) {
#if defined(__x86_64__)
    const auto a = load(p), b = load(p + width);
    const auto lo = _mm256_permute2f128_pd(a, b, 0x20);
    const auto hi = _mm256_permute2f128_pd(a, b, 0x31);
    return {_mm256_unpacklo_pd(lo, hi), _mm256_unpackhi_pd(lo, hi)};
#else
    const auto a = vld2q_f64(p);
    return {a.val[0], a.val[1]};
#endif
  }
  static PS_FRQ_PACKET_INLINE void pairs_store(double* p, Register even,
                                               Register odd) {
#if defined(__x86_64__)
    const auto lo = _mm256_unpacklo_pd(even, odd);
    const auto hi = _mm256_unpackhi_pd(even, odd);
    store(p, _mm256_permute2f128_pd(lo, hi, 0x20));
    store(p + width, _mm256_permute2f128_pd(lo, hi, 0x31));
#else
    const float64x2x2_t a{{even, odd}};
    vst2q_f64(p, a);
#endif
  }
};
#endif
}  // namespace ps::plugin_internal::frequency
