#pragma once

#include <cstring>

#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
#include <immintrin.h>
#define PS_FRQ_VECTOR_TARGET __attribute__((target("avx,fma")))
#define PS_FRQ_PACKET_INLINE \
  inline __attribute__((always_inline, target("avx,fma")))
#define PS_FRQ_PACKETS 1
#elif defined(__aarch64__)
#include <arm_neon.h>
#define PS_FRQ_VECTOR_TARGET
// Preserve the packet abstraction without calls in mixed-radix -O2 kernels.
#define PS_FRQ_PACKET_INLINE inline __attribute__((always_inline))
#define PS_FRQ_PACKETS 1
#else
#define PS_FRQ_PACKETS 0
#endif

namespace ps::plugin_internal::frequency {
inline bool transform_packets_available() {
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
  return __builtin_cpu_supports("avx") && __builtin_cpu_supports("fma");
#elif defined(__aarch64__)
  return true;
#else
  return false;
#endif
}
template <class Scalar>
struct TransformPacket;

#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
template <>
struct TransformPacket<float> {
  using Register = __m256;
  static constexpr unsigned width = 4;
  static constexpr bool vector_prefix = true;
  static constexpr bool initial_radix4 = true;
  static PS_FRQ_PACKET_INLINE Register load_split(const void* real,
                                                  const void* imag) {
    __m128 re, im = _mm_setzero_ps();
    std::memcpy(&re, real, sizeof(re));
    if (imag)
      std::memcpy(&im, imag, sizeof(im));
    return _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_unpacklo_ps(re, im)),
                                _mm_unpackhi_ps(re, im), 1);
  }
  static PS_FRQ_PACKET_INLINE Register scale(Register v, float factor) {
    return _mm256_mul_ps(v, _mm256_set1_ps(factor));
  }
  template <bool Imag>
  static PS_FRQ_PACKET_INLINE void store_component(void* p, Register v,
                                                   float factor) {
    const auto lo = _mm256_castps256_ps128(v);
    const auto hi = _mm256_extractf128_ps(v, 1);
    const auto part = _mm_shuffle_ps(lo, hi, Imag ? 0xdd : 0x88);
    const auto scaled = _mm_mul_ps(part, _mm_set1_ps(factor));
    std::memcpy(p, &scaled, sizeof(scaled));
  }
  static PS_FRQ_PACKET_INLINE Register load(const void* p) {
    Register r;
    std::memcpy(&r, p, sizeof(r));
    return r;
  }
  static PS_FRQ_PACKET_INLINE void store(void* p, Register r) {
    std::memcpy(p, &r, sizeof(r));
  }
  static PS_FRQ_PACKET_INLINE Register broadcast(float re, float im) {
    return _mm256_setr_ps(re, im, re, im, re, im, re, im);
  }
  static PS_FRQ_PACKET_INLINE Register add(Register a, Register b) {
    return _mm256_add_ps(a, b);
  }
  static PS_FRQ_PACKET_INLINE Register minus(Register a, Register b) {
    return _mm256_sub_ps(a, b);
  }
  static PS_FRQ_PACKET_INLINE void transpose(Register& a, Register& b,
                                             Register& c, Register& d) {
    const auto ab0 =
        _mm256_unpacklo_pd(_mm256_castps_pd(a), _mm256_castps_pd(b));
    const auto ab1 =
        _mm256_unpackhi_pd(_mm256_castps_pd(a), _mm256_castps_pd(b));
    const auto cd0 =
        _mm256_unpacklo_pd(_mm256_castps_pd(c), _mm256_castps_pd(d));
    const auto cd1 =
        _mm256_unpackhi_pd(_mm256_castps_pd(c), _mm256_castps_pd(d));
    a = _mm256_castpd_ps(_mm256_permute2f128_pd(ab0, cd0, 0x20));
    b = _mm256_castpd_ps(_mm256_permute2f128_pd(ab1, cd1, 0x20));
    c = _mm256_castpd_ps(_mm256_permute2f128_pd(ab0, cd0, 0x31));
    d = _mm256_castpd_ps(_mm256_permute2f128_pd(ab1, cd1, 0x31));
  }
  static PS_FRQ_PACKET_INLINE Register first_identity(Register a, Register b) {
    return _mm256_blend_ps(b, a, 0x03);
  }
  static PS_FRQ_PACKET_INLINE Register product(Register a, Register b) {
    const auto re = _mm256_moveldup_ps(b);
    const auto im = _mm256_movehdup_ps(b);
    const auto swapped = _mm256_permute_ps(a, 0xb1);
    return _mm256_fmaddsub_ps(a, re, _mm256_mul_ps(swapped, im));
  }
  template <bool Inverse>
  static PS_FRQ_PACKET_INLINE Register rotate(Register a) {
    const auto sign = Inverse ? broadcast(-0.0f, 0.0f) : broadcast(0.0f, -0.0f);
    return _mm256_xor_ps(_mm256_permute_ps(a, 0xb1), sign);
  }
};
template <>
struct TransformPacket<double> {
  using Register = __m256d;
  static constexpr unsigned width = 2;
  static constexpr bool vector_prefix = true;
  static constexpr bool initial_radix4 = false;
  static PS_FRQ_PACKET_INLINE Register load_split(const void* real,
                                                  const void* imag) {
    __m128d re, im = _mm_setzero_pd();
    std::memcpy(&re, real, sizeof(re));
    if (imag)
      std::memcpy(&im, imag, sizeof(im));
    return _mm256_insertf128_pd(_mm256_castpd128_pd256(_mm_unpacklo_pd(re, im)),
                                _mm_unpackhi_pd(re, im), 1);
  }
  static PS_FRQ_PACKET_INLINE Register scale(Register v, double factor) {
    return _mm256_mul_pd(v, _mm256_set1_pd(factor));
  }
  template <bool Imag>
  static PS_FRQ_PACKET_INLINE void store_component(void* p, Register v,
                                                   double factor) {
    const auto lo = _mm256_castpd256_pd128(v);
    const auto hi = _mm256_extractf128_pd(v, 1);
    const auto part = Imag ? _mm_unpackhi_pd(lo, hi) : _mm_unpacklo_pd(lo, hi);
    const auto scaled = _mm_mul_pd(part, _mm_set1_pd(factor));
    std::memcpy(p, &scaled, sizeof(scaled));
  }
  static PS_FRQ_PACKET_INLINE Register load(const void* p) {
    Register r;
    std::memcpy(&r, p, sizeof(r));
    return r;
  }
  static PS_FRQ_PACKET_INLINE void store(void* p, Register r) {
    std::memcpy(p, &r, sizeof(r));
  }
  static PS_FRQ_PACKET_INLINE Register broadcast(double re, double im) {
    return _mm256_setr_pd(re, im, re, im);
  }
  static PS_FRQ_PACKET_INLINE Register add(Register a, Register b) {
    return _mm256_add_pd(a, b);
  }
  static PS_FRQ_PACKET_INLINE Register minus(Register a, Register b) {
    return _mm256_sub_pd(a, b);
  }
  static PS_FRQ_PACKET_INLINE Register first_identity(Register a, Register b) {
    return _mm256_blend_pd(b, a, 0x03);
  }
  static PS_FRQ_PACKET_INLINE Register product(Register a, Register b) {
    const auto re = _mm256_movedup_pd(b);
    const auto im = _mm256_permute_pd(b, 0xf);
    const auto swapped = _mm256_permute_pd(a, 0x5);
    return _mm256_fmaddsub_pd(a, re, _mm256_mul_pd(swapped, im));
  }
  template <bool Inverse>
  static PS_FRQ_PACKET_INLINE Register rotate(Register a) {
    const auto sign = Inverse ? broadcast(-0.0, 0.0) : broadcast(0.0, -0.0);
    return _mm256_xor_pd(_mm256_permute_pd(a, 0x5), sign);
  }
};
#elif defined(__aarch64__)
template <>
struct TransformPacket<float> {
  struct Register {
    float32x4_t re, im;
  };
  static constexpr unsigned width = 4;
  static constexpr bool vector_prefix = true;
  static constexpr bool initial_radix4 = false;
  static Register load_split(const void* real, const void* imag) {
    Register v{vdupq_n_f32(0), vdupq_n_f32(0)};
    std::memcpy(&v.re, real, sizeof(v.re));
    if (imag)
      std::memcpy(&v.im, imag, sizeof(v.im));
    return v;
  }
  static Register scale(Register v, float factor) {
    return {vmulq_n_f32(v.re, factor), vmulq_n_f32(v.im, factor)};
  }
  template <bool Imag>
  static void store_component(void* p, Register v, float factor) {
    const auto scaled = vmulq_n_f32(Imag ? v.im : v.re, factor);
    std::memcpy(p, &scaled, sizeof(scaled));
  }
  static Register load(const void* p) {
    float32x4_t a, b;
    std::memcpy(&a, p, sizeof(a));
    std::memcpy(&b, static_cast<const unsigned char*>(p) + sizeof(a),
                sizeof(b));
    return {vuzp1q_f32(a, b), vuzp2q_f32(a, b)};
  }
  static void store(void* p, Register r) {
    const auto a = vzip1q_f32(r.re, r.im);
    const auto b = vzip2q_f32(r.re, r.im);
    std::memcpy(p, &a, sizeof(a));
    std::memcpy(static_cast<unsigned char*>(p) + sizeof(a), &b, sizeof(b));
  }
  static Register broadcast(float re, float im) {
    return {vdupq_n_f32(re), vdupq_n_f32(im)};
  }
  static Register add(Register a, Register b) {
    return {vaddq_f32(a.re, b.re), vaddq_f32(a.im, b.im)};
  }
  static Register minus(Register a, Register b) {
    return {vsubq_f32(a.re, b.re), vsubq_f32(a.im, b.im)};
  }
  static void transpose_part(float32x4_t& a, float32x4_t& b, float32x4_t& c,
                             float32x4_t& d) {
    const auto ab = vtrnq_f32(a, b), cd = vtrnq_f32(c, d);
    a = vcombine_f32(vget_low_f32(ab.val[0]), vget_low_f32(cd.val[0]));
    b = vcombine_f32(vget_low_f32(ab.val[1]), vget_low_f32(cd.val[1]));
    c = vcombine_f32(vget_high_f32(ab.val[0]), vget_high_f32(cd.val[0]));
    d = vcombine_f32(vget_high_f32(ab.val[1]), vget_high_f32(cd.val[1]));
  }
  static void transpose(Register& a, Register& b, Register& c, Register& d) {
    transpose_part(a.re, b.re, c.re, d.re);
    transpose_part(a.im, b.im, c.im, d.im);
  }
  static Register first_identity(Register a, Register b) {
    return {vsetq_lane_f32(vgetq_lane_f32(a.re, 0), b.re, 0),
            vsetq_lane_f32(vgetq_lane_f32(a.im, 0), b.im, 0)};
  }
  static Register product(Register a, Register b) {
    return {vfmsq_f32(vmulq_f32(a.re, b.re), a.im, b.im),
            vfmaq_f32(vmulq_f32(a.re, b.im), a.im, b.re)};
  }
  template <bool Inverse>
  static Register rotate(Register a) {
    if constexpr (Inverse)
      return {vnegq_f32(a.im), a.re};
    return {a.im, vnegq_f32(a.re)};
  }
};
template <>
struct TransformPacket<double> {
  struct Register {
    float64x2_t re, im;
  };
  static constexpr unsigned width = 2;
  static constexpr bool vector_prefix = true;
  static constexpr bool initial_radix4 = false;
  static Register load_split(const void* real, const void* imag) {
    Register v{vdupq_n_f64(0), vdupq_n_f64(0)};
    std::memcpy(&v.re, real, sizeof(v.re));
    if (imag)
      std::memcpy(&v.im, imag, sizeof(v.im));
    return v;
  }
  static Register scale(Register v, double factor) {
    return {vmulq_n_f64(v.re, factor), vmulq_n_f64(v.im, factor)};
  }
  template <bool Imag>
  static void store_component(void* p, Register v, double factor) {
    const auto scaled = vmulq_n_f64(Imag ? v.im : v.re, factor);
    std::memcpy(p, &scaled, sizeof(scaled));
  }
  static Register load(const void* p) {
    float64x2_t a, b;
    std::memcpy(&a, p, sizeof(a));
    std::memcpy(&b, static_cast<const unsigned char*>(p) + sizeof(a),
                sizeof(b));
    return {vuzp1q_f64(a, b), vuzp2q_f64(a, b)};
  }
  static void store(void* p, Register r) {
    const auto a = vzip1q_f64(r.re, r.im);
    const auto b = vzip2q_f64(r.re, r.im);
    std::memcpy(p, &a, sizeof(a));
    std::memcpy(static_cast<unsigned char*>(p) + sizeof(a), &b, sizeof(b));
  }
  static Register broadcast(double re, double im) {
    return {vdupq_n_f64(re), vdupq_n_f64(im)};
  }
  static Register first_identity(Register a, Register b) {
    return {vcombine_f64(vget_low_f64(a.re), vget_high_f64(b.re)),
            vcombine_f64(vget_low_f64(a.im), vget_high_f64(b.im))};
  }
  static Register add(Register a, Register b) {
    return {vaddq_f64(a.re, b.re), vaddq_f64(a.im, b.im)};
  }
  static Register minus(Register a, Register b) {
    return {vsubq_f64(a.re, b.re), vsubq_f64(a.im, b.im)};
  }
  static Register product(Register a, Register b) {
    return {vfmsq_f64(vmulq_f64(a.re, b.re), a.im, b.im),
            vfmaq_f64(vmulq_f64(a.re, b.im), a.im, b.re)};
  }
  template <bool Inverse>
  static Register rotate(Register a) {
    if constexpr (Inverse)
      return {vnegq_f64(a.im), a.re};
    return {a.im, vnegq_f64(a.re)};
  }
};
#endif
}  // namespace ps::plugin_internal::frequency
