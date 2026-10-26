#pragma once

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "05-filter/frequency.hpp"

namespace ps::plugin_internal::frequency {
// Addition retains the Ball hi/lo evaluation order. Multiplication uses four
// explicit rounded products/corrections and omits the low-low term. TwoSum
// normalizes each result; RN, gradual underflow and no overflow are required.
struct TransformDD {
  double hi = 0, lo = 0;
};
struct TransformDDComplex {
  TransformDD re, im;
};
static_assert(sizeof(TransformDDComplex) == 32);
inline TransformDD dd_center_add(TransformDD a, TransformDD b) {
  const auto s = Ball::two_sum(a.hi, b.hi);
  const auto t = Ball::two_sum(a.lo, b.lo);
  const auto v = Ball::two_sum(s.second, t.first);
  const auto r = Ball::two_sum(s.first, v.first);
  return {r.first, r.second};
}
inline TransformDD dd_center_negate(TransformDD a) {
  return {-a.hi, -a.lo};
}
inline TransformDD dd_center_product(TransformDD a, TransformDD b) {
  const double p = a.hi * b.hi;
  const double e = std::fma(a.hi, b.hi, -p);
  const double c = a.hi * b.lo, d = a.lo * b.hi;
  const double first = e + c, second = first + d;
  const auto r = Ball::two_sum(p, second);
  return {r.first, r.second};
}
inline TransformDDComplex dd_center_add_scalar(TransformDDComplex a,
                                               TransformDDComplex b) {
  return {dd_center_add(a.re, b.re), dd_center_add(a.im, b.im)};
}
inline TransformDDComplex dd_center_minus_scalar(TransformDDComplex a,
                                                 TransformDDComplex b) {
  return {dd_center_add(a.re, dd_center_negate(b.re)),
          dd_center_add(a.im, dd_center_negate(b.im))};
}
inline TransformDDComplex dd_center_product_scalar(TransformDDComplex a,
                                                   TransformDDComplex b) {
  return {dd_center_add(dd_center_product(a.re, b.re),
                        dd_center_negate(dd_center_product(a.im, b.im))),
          dd_center_add(dd_center_product(a.re, b.im),
                        dd_center_product(a.im, b.re))};
}
#if defined(__aarch64__) && defined(__ARM_NEON)
struct TransformDDVector {
  float64x2_t hi, lo;
};
inline TransformDDVector dd_vector(TransformDDComplex a) {
  const double hi[2] = {a.re.hi, a.im.hi};
  const double lo[2] = {a.re.lo, a.im.lo};
  return {vld1q_f64(hi), vld1q_f64(lo)};
}
inline TransformDDComplex dd_complex(TransformDDVector a) {
  double hi[2], lo[2];
  vst1q_f64(hi, a.hi);
  vst1q_f64(lo, a.lo);
  return {{hi[0], lo[0]}, {hi[1], lo[1]}};
}
inline TransformDDVector dd_vector_sum(float64x2_t a, float64x2_t b) {
  const auto s = vaddq_f64(a, b), p = vsubq_f64(s, a);
  const auto e = vaddq_f64(vsubq_f64(a, vsubq_f64(s, p)), vsubq_f64(b, p));
  return {s, e};
}
inline TransformDDVector dd_vector_add(TransformDDVector a,
                                       TransformDDVector b) {
  const auto s = dd_vector_sum(a.hi, b.hi), t = dd_vector_sum(a.lo, b.lo);
  const auto v = dd_vector_sum(s.lo, t.hi);
  return dd_vector_sum(s.hi, v.hi);
}
inline TransformDDVector dd_vector_product(TransformDDVector a,
                                           TransformDDVector b) {
  const auto p = vmulq_f64(a.hi, b.hi);
  const auto e = vfmaq_f64(vnegq_f64(p), a.hi, b.hi);
  const auto c = vmulq_f64(a.hi, b.lo), d = vmulq_f64(a.lo, b.hi);
  const auto first = vaddq_f64(e, c), second = vaddq_f64(first, d);
  return dd_vector_sum(p, second);
}
inline TransformDDComplex dd_center_add(TransformDDComplex a,
                                        TransformDDComplex b) {
  return dd_complex(dd_vector_add(dd_vector(a), dd_vector(b)));
}
inline TransformDDComplex dd_center_minus(TransformDDComplex a,
                                          TransformDDComplex b) {
  auto bv = dd_vector(b);
  bv.hi = vnegq_f64(bv.hi);
  bv.lo = vnegq_f64(bv.lo);
  return dd_complex(dd_vector_add(dd_vector(a), bv));
}
inline TransformDDComplex dd_center_product(TransformDDComplex a,
                                            TransformDDComplex b) {
  const auto av = dd_vector(a);
  const TransformDDVector wr{vdupq_n_f64(b.re.hi), vdupq_n_f64(b.re.lo)};
  const TransformDDVector wi{vdupq_n_f64(b.im.hi), vdupq_n_f64(b.im.lo)};
  const auto r = dd_vector_product(av, wr), i = dd_vector_product(av, wi);
  const TransformDDVector left{vzip1q_f64(r.hi, i.hi), vzip1q_f64(r.lo, i.lo)};
  const TransformDDVector right{vzip2q_f64(vnegq_f64(i.hi), r.hi),
                                vzip2q_f64(vnegq_f64(i.lo), r.lo)};
  return dd_complex(dd_vector_add(left, right));
}
#else
inline TransformDDComplex dd_center_add(TransformDDComplex a,
                                        TransformDDComplex b) {
  return dd_center_add_scalar(a, b);
}
inline TransformDDComplex dd_center_minus(TransformDDComplex a,
                                          TransformDDComplex b) {
  return dd_center_minus_scalar(a, b);
}
inline TransformDDComplex dd_center_product(TransformDDComplex a,
                                            TransformDDComplex b) {
  return dd_center_product_scalar(a, b);
}
#endif
// Nonnegative operations are individually rounded outward. In particular a
// rounded-to-zero positive product receives a positive subnormal upper bound.
inline double dd_bound_add(double a, double b) {
  return Ball::up(a + b);
}
inline double dd_bound_product(double a, double b) {
  return a == 0 || b == 0 ? 0 : numeric_ops::numeric_up(a * b);
}
// A sufficient RN64 cell test for a certified unevaluated hi+lo center.
// The exponent guard makes both neighboring gaps normal exact powers of two.
// Strict inequalities intentionally leave true midpoints to Ball replay.
inline std::optional<std::uint64_t> dd_cell_round(Ball value) {
  if (!value.finite() || std::abs(value.hi) < 0x1p-800 ||
      std::abs(value.hi) > 0x1p800)
    return {};
  const auto previous = numeric_ops::numeric_neighbor(value.hi, false);
  const auto next = numeric_ops::numeric_neighbor(value.hi, true);
  const auto left = (value.hi - previous) * .5;
  const auto right = (next - value.hi) * .5;
  const auto low = numeric_ops::numeric_down(value.lo - value.error);
  const auto high = numeric_ops::numeric_up(value.lo + value.error);
  if (low > -left && high < right)
    return bits(value.hi);
  return {};
}
struct TransformDDCertificate {
  // For every exact complex intermediate z and stored center c (hi+lo),
  // amplitude bounds |z|_2 and error bounds |c-z|_2. Hence |c|_2 <= A+E.
  // root_error bounds the complex root error; component radii sum to an
  // outward upper bound on that Euclidean norm. All recurrences round out.
  double amplitude = 0, error = 0;
  void stage(unsigned radix, double root_error) {
    const auto value = dd_bound_add(amplitude, error);
    const auto magnitude = dd_bound_product(value, dd_bound_add(1, root_error));
    // Normalized scalar add: 4*u^2*(|a|+|b|). The short FMA
    // product: 16*u^2*|a*b|+8*eta, including the omitted low-low
    // product and residual underflow. Four real products and two sums give
    // complex product 64*u^2*|z*w|+64*eta. Three such products plus
    // two addition layers fit 256*u^2*M+256*eta. Exact unit roots
    // propagate inherited errors by radix, not by component L1 norms.
    const auto local =
        dd_bound_add(dd_bound_product(0x1p-98, magnitude), 256 * 0x1p-1074);
    error = dd_bound_add(
        dd_bound_add(
            dd_bound_product(radix, error),
            dd_bound_product(radix - 1, dd_bound_product(root_error, value))),
        local);
    amplitude = dd_bound_product(radix, amplitude);
  }
  void axis(std::uint64_t n, double root_error, const Fuel& fuel) {
    unsigned levels = 0;
    for (auto size = n; size > 1; size >>= 1)
      ++levels;
    if (levels & 1) {
      fuel.poll(128);
      const auto value = dd_bound_add(amplitude, error);
      error = dd_bound_add(dd_bound_product(2, error),
                           dd_bound_product(0x1p-103, value));
      amplitude = dd_bound_product(2, amplitude);
    }
    for (unsigned i = 0; i < levels / 2; ++i) {
      fuel.poll(128);
      stage(4, root_error);
    }
  }
};
}  // namespace ps::plugin_internal::frequency
