#pragma once

#include <cmath>

#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__)
#include <emmintrin.h>
#endif

namespace ps::plugin_internal::filter {
// Two binary64 lanes. No contraction or reassociation is assumed in the
// enclosing error proof, on either ISA.
struct Pair {
#if defined(__aarch64__)
  float64x2_t value;
  explicit Pair(double x) : value(vdupq_n_f64(x)) {}
  explicit Pair(float64x2_t x) : value(x) {}
  static Pair load(const double* p) { return Pair(vld1q_f64(p)); }
  static Pair single(double x) {
    return Pair(vsetq_lane_f64(x, vdupq_n_f64(0), 0));
  }
  void save(double* p) const { vst1q_f64(p, value); }
  Pair operator+(Pair b) const { return Pair(vaddq_f64(value, b.value)); }
  Pair operator*(Pair b) const { return Pair(vmulq_f64(value, b.value)); }
  Pair absolute() const { return Pair(vabsq_f64(value)); }
#elif defined(__x86_64__)
  __m128d value;
  explicit Pair(double x) : value(_mm_set1_pd(x)) {}
  explicit Pair(__m128d x) : value(x) {}
  static Pair load(const double* p) { return Pair(_mm_loadu_pd(p)); }
  static Pair single(double x) { return Pair(_mm_set_sd(x)); }
  void save(double* p) const { _mm_storeu_pd(p, value); }
  Pair operator+(Pair b) const { return Pair(_mm_add_pd(value, b.value)); }
  Pair operator*(Pair b) const { return Pair(_mm_mul_pd(value, b.value)); }
  Pair absolute() const {
    return Pair(_mm_andnot_pd(_mm_set1_pd(-0.0), value));
  }
#else
  double value[2];
  explicit Pair(double x) : value{x, x} {}
  Pair(double a, double b) : value{a, b} {}
  static Pair load(const double* p) { return Pair(p[0], p[1]); }
  static Pair single(double x) { return Pair(x, 0); }
  void save(double* p) const {
    p[0] = value[0];
    p[1] = value[1];
  }
  Pair operator+(Pair b) const {
    return Pair(value[0] + b.value[0], value[1] + b.value[1]);
  }
  Pair operator*(Pair b) const {
    return Pair(value[0] * b.value[0], value[1] * b.value[1]);
  }
  Pair absolute() const { return Pair(std::abs(value[0]), std::abs(value[1])); }
#endif
};
}  // namespace ps::plugin_internal::filter
