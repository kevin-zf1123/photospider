// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "01-numeric/exact_predicate.hpp"
#include "01-numeric/fast_native_simd.hpp"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace ps::plugin_internal::nonseparable {
enum class Kind {
  Hue,
  ChromaZero,
  ChromaPreserve,
  ChromaSource,
  Color,
  Lightness,
  HueW3c,
  SaturationW3c,
  ColorW3c,
  LuminosityW3c
};
constexpr bool radial(Kind k) {
  return k <= Kind::ChromaSource;
}
constexpr bool w3c(Kind k) {
  return k >= Kind::HueW3c;
}
constexpr bool copy_only(Kind k) {
  return k == Kind::Color || k == Kind::Lightness;
}

template <class V>
struct Triple {
  V l, a, b;
};

/** Exact stored-input neutral predicate. Fixed storage covers all finite
 * binary64 products, including underflow/overflow of the native squared norm.
 * This refines branch decisions only; radial output arithmetic remains native.
 */
template <class T>
inline bool neutral_exact(T a, T b, T threshold) {
  using numeric_ops::BinaryParts;
  using numeric_ops::PredicateInteger;
  const auto parts = [](T v) {
    std::uint64_t raw = 0;
    std::memcpy(&raw, &v, sizeof(T));
    return BinaryParts::decode(raw, sizeof(T) == 4);
  };
  const auto x = parts(a), y = parts(b), e = parts(threshold);
  PredicateInteger sum, second, limit;
  sum.set_product(x, x);
  second.set_product(y, y);
  sum.add(second);
  limit.set_product(e, e);
  for (std::size_t i = sum.words.size(); i-- > 0;)
    if (sum.words[i] != limit.words[i])
      return sum.words[i] < limit.words[i];
  return true;
}

template <class T>
inline bool neutral(T a, T b, T threshold, T norm2) {
  if (threshold == 0)
    return a == 0 && b == 0;
  if (std::abs(a) > threshold || std::abs(b) > threshold)
    return false;
  const T e2 = threshold * threshold;
  const T error =
      (T(8) * std::numeric_limits<T>::epsilon()) * std::max(norm2, e2) +
      T(8) * std::numeric_limits<T>::denorm_min();
  if (std::isfinite(norm2) && std::isfinite(e2) && std::abs(norm2 - e2) > error)
    return norm2 < e2;
  return neutral_exact(a, b, threshold);
}

template <class T>
struct Scalar {
  using Value = T;
  using Mask = bool;
  using ScalarType = T;
  static constexpr unsigned lanes = 1;
  static T set(T x) { return x; }
  static T add(T a, T b) { return a + b; }
  static T sub(T a, T b) { return a - b; }
  static T mul(T a, T b) { return a * b; }
  static T div(T a, T b) { return a / b; }
  static T madd(T a, T b, T c) {
#if defined(__aarch64__)
    return std::fma(a, b, c);
#else
    return a * b + c;
#endif
  }
  static T root(T a) { return std::sqrt(a); }
  static T abs(T a) { return std::abs(a); }
  static T min(T a, T b) { return std::fmin(a, b); }
  static T max(T a, T b) { return std::fmax(a, b); }
  static bool le(T a, T b) { return a <= b; }
  static bool lt(T a, T b) { return a < b; }
  static bool eq(T a, T b) { return a == b; }
  static bool both(bool a, bool b) { return a && b; }
  static bool either(bool a, bool b) { return a || b; }
  static bool invert(bool a) { return !a; }
  static T choose(bool mask, T a, T b) { return mask ? a : b; }
};

/** Rounded squares have a conservative interval filter; only uncertain lanes
 * execute the exact predicate. e==0 is an exact component-zero comparison.
 */
template <class O>
inline typename O::Mask neutral_ops(typename O::Value a, typename O::Value b,
                                    typename O::ScalarType threshold,
                                    typename O::Value norm2) {
  using T = typename O::ScalarType;
  if constexpr (O::lanes == 1) {
    return neutral(a, b, threshold, norm2);
  } else {
    const auto zero = O::set(0);
    if (threshold == 0)
      return O::both(O::eq(a, zero), O::eq(b, zero));
    const auto e = O::set(threshold), e2 = O::set(threshold * threshold);
    const auto inside = O::both(O::le(O::abs(a), e), O::le(O::abs(b), e));
    const auto maximum = O::choose(O::le(norm2, e2), e2, norm2);
    const auto error = O::add(
        O::mul(O::set(T(8) * std::numeric_limits<T>::epsilon()), maximum),
        O::set(T(8) * std::numeric_limits<T>::denorm_min()));
    const auto uncertain = O::both(
        inside,
        O::either(
            O::le(O::abs(O::sub(norm2, e2)), error),
            O::invert(O::le(maximum, O::set(std::numeric_limits<T>::max())))));
    auto result = O::both(inside, O::le(norm2, e2));
    if (O::any(uncertain)) {
      std::array<T, O::lanes> aa, bb, flags, pending;
      O::save(aa.data(), a);
      O::save(bb.data(), b);
      O::save(flags.data(), O::choose(result, O::set(1), zero));
      O::save(pending.data(), O::choose(uncertain, O::set(1), zero));
      for (unsigned i = 0; i < O::lanes; ++i)
        if (pending[i] != 0)
          flags[i] = neutral_exact(aa[i], bb[i], threshold) ? T(1) : T(0);
      result = O::eq(O::load(flags.data()), O::set(1));
    }
    return result;
  }
}
template <class O>
inline typename O::Value minimum(typename O::Value a, typename O::Value b) {
  return O::min(a, b);
}
template <class O>
inline typename O::Value maximum(typename O::Value a, typename O::Value b) {
  return O::max(a, b);
}
template <class O>
inline typename O::Value lum(const Triple<typename O::Value>& c) {
  return O::madd(O::set(.11), c.b,
                 O::madd(O::set(.59), c.a, O::mul(O::set(.30), c.l)));
}
template <class O>
inline typename O::Value low(const Triple<typename O::Value>& c) {
  return minimum<O>(minimum<O>(c.l, c.a), c.b);
}
template <class O>
inline typename O::Value high(const Triple<typename O::Value>& c) {
  return maximum<O>(maximum<O>(c.l, c.a), c.b);
}
template <class O>
inline typename O::Value sat(const Triple<typename O::Value>& c) {
  return O::sub(high<O>(c), low<O>(c));
}
template <class O>
inline Triple<typename O::Value> set_sat(Triple<typename O::Value> c,
                                         typename O::Value target,
                                         typename O::Value* maximum_value) {
  const auto first_low = minimum<O>(c.l, c.a);
  const auto first_high = maximum<O>(c.l, c.a);
  const auto n = minimum<O>(first_low, c.b);
  const auto x = maximum<O>(first_high, c.b);
  const auto middle = maximum<O>(first_low, minimum<O>(first_high, c.b));
  const auto zero = O::set(0), one = O::set(1);
  const auto active = O::lt(n, x);
  *maximum_value = O::choose(active, target, zero);
  // Multiply before dividing: reciprocal scaling can overflow for bounded
  // RGB tuples whose nonzero range is subnormal.
  const auto quotient = O::div(O::mul(O::sub(middle, n), target),
                               O::choose(active, O::sub(x, n), one));
  const auto value = minimum<O>(target, maximum<O>(zero, quotient));
  const auto channel = [&](auto v) {
    return O::choose(O::eq(v, n), zero, O::choose(O::eq(v, x), target, value));
  };
  return {channel(c.l), channel(c.a), channel(c.b)};
}
template <class O, bool Saturated = false>
inline Triple<typename O::Value> set_lum(
    Triple<typename O::Value> c, typename O::Value target,
    typename O::Value maximum_value = O::set(0)) {
  const auto zero = O::set(0.0), one = O::set(1.0);
  // Lum(C + (target - Lum(C))) == target: the W3C weights sum to one.
  // Use this real-arithmetic identity as the native continuous profile.
  const auto l = minimum<O>(one, maximum<O>(zero, target));
  const auto delta = O::sub(l, lum<O>(c));
  c = {O::add(c.l, delta), O::add(c.a, delta), O::add(c.b, delta)};
  typename O::Value n, x;
  if constexpr (Saturated) {
    n = delta;
    x = O::add(maximum_value, delta);
  } else {
    n = low<O>(c);
    x = high<O>(c);
  }
  const auto negative = O::lt(n, zero), over = O::lt(one, x);
  // The input to SetLum is bounded: a negative offset cannot produce x>1,
  // and a positive offset cannot produce n<0. Thus corrections are exclusive.
  const auto active = O::either(negative, over);
  const auto numerator =
      O::choose(negative, l, O::choose(over, O::sub(one, l), one));
  const auto denominator =
      O::choose(negative, O::sub(l, n), O::choose(over, O::sub(x, l), one));
  // l is bounded: an active lower/upper correction has l-n>0 / x-l>0.
  const auto scale = O::div(numerator, denominator);
  const auto channel = [&](auto v) {
    v = O::choose(active, O::madd(O::sub(v, l), scale, l), v);
    // ClipColor is bounded. Correct only its native rounding overshoot so
    // another bounded RGB node can consume the result without a domain breach.
    return minimum<O>(one, maximum<O>(zero, v));
  };
  return {channel(c.l), channel(c.a), channel(c.b)};
}

/** One operation graph for SIMD, scalar tails and arbitrary layouts.
 * Implicit contraction is disabled; W3C uses its explicit native FMA profile.
 * Copied coordinates undergo no floating operation.
 */
template <Kind K, class O>
inline Triple<typename O::Value> evaluate(
    Triple<typename O::Value> b, Triple<typename O::Value> s,
    typename O::ScalarType threshold = 0) {
  if constexpr (K == Kind::Color) {
    return {b.l, s.a, s.b};
  } else if constexpr (K == Kind::Lightness) {
    return {s.l, b.a, b.b};
  } else if constexpr (radial(K)) {
    const auto zero = O::set(0), one = O::set(1);
    const auto b2 = O::add(O::mul(b.a, b.a), O::mul(b.b, b.b));
    const auto s2 = O::add(O::mul(s.a, s.a), O::mul(s.b, s.b));
    if constexpr (K == Kind::Hue) {
      const auto neutral_s = neutral_ops<O>(s.a, s.b, threshold, s2);
      const auto neutral_b = O::both(O::eq(b.a, zero), O::eq(b.b, zero));
      const auto scale = O::root(O::div(b2, O::choose(neutral_s, one, s2)));
      const auto a = O::choose(neutral_b, zero, O::mul(s.a, scale));
      const auto v = O::choose(neutral_b, zero, O::mul(s.b, scale));
      return {b.l, O::choose(neutral_s, b.a, a), O::choose(neutral_s, b.b, v)};
    } else {
      const auto neutral_b = neutral_ops<O>(b.a, b.b, threshold, b2);
      const auto scale = O::root(O::div(s2, O::choose(neutral_b, one, b2)));
      auto a = O::mul(b.a, scale), v = O::mul(b.b, scale);
      if constexpr (K == Kind::ChromaZero) {
        a = O::choose(neutral_b, O::root(s2), a);
        v = O::choose(neutral_b, zero, v);
      } else if constexpr (K == Kind::ChromaPreserve) {
        a = O::choose(neutral_b, b.a, a);
        v = O::choose(neutral_b, b.b, v);
      } else {
        a = O::choose(neutral_b, s.a, a);
        v = O::choose(neutral_b, s.b, v);
      }
      return {b.l, a, v};
    }
  } else if constexpr (K == Kind::HueW3c) {
    typename O::Value maximum_value;
    const auto c = set_sat<O>(s, sat<O>(b), &maximum_value);
    return set_lum<O, true>(c, lum<O>(b), maximum_value);
  } else if constexpr (K == Kind::SaturationW3c) {
    typename O::Value maximum_value;
    const auto c = set_sat<O>(b, sat<O>(s), &maximum_value);
    return set_lum<O, true>(c, lum<O>(b), maximum_value);
  } else if constexpr (K == Kind::ColorW3c) {
    return set_lum<O>(s, lum<O>(b));
  } else {
    return set_lum<O>(b, lum<O>(s));
  }
}

#if defined(__aarch64__)
template <class T>
struct Neon;
#define PS_NBL_NEON(T, S, V, M, P, N, U)                            \
  template <>                                                       \
  struct Neon<T> {                                                  \
    using ScalarType = T;                                           \
    using Value = V;                                                \
    using Mask = M;                                                 \
    static constexpr unsigned lanes = N;                            \
    static V set(T x) { return vdupq_n_##S(x); }                    \
    static V load(const T* p) { return vld1q_##S(p); }              \
    static void save(T* p, V v) { vst1q_##S(p, v); }                \
    static V add(V a, V b) { return vaddq_##S(a, b); }              \
    static V sub(V a, V b) { return vsubq_##S(a, b); }              \
    static V mul(V a, V b) { return vmulq_##S(a, b); }              \
    static V div(V a, V b) { return vdivq_##S(a, b); }              \
    static V madd(V a, V b, V c) { return vfmaq_##S(c, a, b); }     \
    static V root(V a) { return vsqrtq_##S(a); }                    \
    static V abs(V a) { return vabsq_##S(a); }                      \
    static V min(V a, V b) { return vminq_##S(a, b); }              \
    static V max(V a, V b) { return vmaxq_##S(a, b); }              \
    static M le(V a, V b) { return native_simd::less_equal(a, b); } \
    static M lt(V a, V b) { return native_simd::less(a, b); }       \
    static M eq(V a, V b) { return native_simd::equal(a, b); }      \
    static M both(M a, M b) { return vandq_##U(a, b); }             \
    static M either(M a, M b) { return vorrq_##U(a, b); }           \
    static M invert(M a) { return veorq_##U(a, vdupq_n_##U(-1)); }  \
    static bool any(M a) {                                          \
      std::uint64_t bits[2];                                        \
      std::memcpy(bits, &a, sizeof(a));                             \
      return (bits[0] | bits[1]) != 0;                              \
    }                                                               \
    static V choose(M m, V a, V b) { return vbslq_##S(m, a, b); }   \
    static Triple<V> load3(const T* p) {                            \
      const auto v = vld3q_##S(p);                                  \
      return {v.val[0], v.val[1], v.val[2]};                        \
    }                                                               \
    static void save3(T* p, Triple<V> v) {                          \
      P x;                                                          \
      x.val[0] = v.l;                                               \
      x.val[1] = v.a;                                               \
      x.val[2] = v.b;                                               \
      vst3q_##S(p, x);                                              \
    }                                                               \
  }
PS_NBL_NEON(float, f32, float32x4_t, uint32x4_t, float32x4x3_t, 4, u32);
PS_NBL_NEON(double, f64, float64x2_t, uint64x2_t, float64x2x3_t, 2, u64);
#undef PS_NBL_NEON
#endif

template <Kind K, class T>
inline void scalar_span(const T* b, const T* s, T e, T* out,
                        std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    if constexpr (copy_only(K)) {
      // Read precisely the coordinates that this member copies.
      const T* l = K == Kind::Color ? b : s;
      const T* ab = K == Kind::Color ? s : b;
      std::memcpy(out + 3 * i, l + 3 * i, sizeof(T));
      std::memcpy(out + 3 * i + 1, ab + 3 * i + 1, 2 * sizeof(T));
    } else {
      Triple<T> bv{b[3 * i], b[3 * i + 1], b[3 * i + 2]};
      Triple<T> sv{T(0), s[3 * i + 1], s[3 * i + 2]};
      if constexpr (w3c(K))
        sv.l = s[3 * i];
      const auto v = evaluate<K, Scalar<T>>(bv, sv, e);
      out[3 * i] = v.l;
      out[3 * i + 1] = v.a;
      out[3 * i + 2] = v.b;
    }
  }
}

#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
void span_avx2(Kind, const float*, const float*, float, float*, std::size_t);
void span_avx2(Kind, const double*, const double*, double, double*,
               std::size_t);
#endif

#if defined(PHOTOSPIDER_HAS_NBL_SME)
bool sme_copy_available();
void copy_sme(const void*, const void*, void*, std::size_t, bool);
#endif

template <Kind K, class T>
inline void span(const T* b, const T* s, T threshold, T* out,
                 std::size_t count) {
#if defined(PHOTOSPIDER_HAS_NBL_SME)
  if constexpr (copy_only(K)) {
    if (count >= 1024 && sme_copy_available()) {
      copy_sme(K == Kind::Color ? b : s, K == Kind::Color ? s : b, out, count,
               sizeof(T) == 4);
      return;
    }
  }
#endif
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
  if (__builtin_cpu_supports("avx2")) {
    span_avx2(K, b, s, threshold, out, count);
    return;
  }
#endif
  std::size_t i = 0;
#if defined(__aarch64__)
  if constexpr (!copy_only(K)) {
    using O = Neon<T>;
    for (; i + 2 * O::lanes <= count; i += 2 * O::lanes) {
      const auto b0 = O::load3(b + 3 * i), s0 = O::load3(s + 3 * i);
      const auto b1 = O::load3(b + 3 * (i + O::lanes));
      const auto s1 = O::load3(s + 3 * (i + O::lanes));
      if constexpr (K == Kind::HueW3c || K == Kind::SaturationW3c) {
        const auto l0 = lum<O>(b0), l1 = lum<O>(b1);
        const auto t0 = sat<O>(K == Kind::HueW3c ? b0 : s0);
        const auto t1 = sat<O>(K == Kind::HueW3c ? b1 : s1);
        typename O::Value maximum0, maximum1;
        const auto c0 = set_sat<O>(K == Kind::HueW3c ? s0 : b0, t0, &maximum0);
        const auto c1 = set_sat<O>(K == Kind::HueW3c ? s1 : b1, t1, &maximum1);
        O::save3(out + 3 * i, set_lum<O, true>(c0, l0, maximum0));
        O::save3(out + 3 * (i + O::lanes), set_lum<O, true>(c1, l1, maximum1));
      } else {
        const auto v0 = evaluate<K, O>(b0, s0, threshold);
        const auto v1 = evaluate<K, O>(b1, s1, threshold);
        O::save3(out + 3 * i, v0);
        O::save3(out + 3 * (i + O::lanes), v1);
      }
    }
    for (; i + O::lanes <= count; i += O::lanes) {
      const auto v =
          evaluate<K, O>(O::load3(b + 3 * i), O::load3(s + 3 * i), threshold);
      O::save3(out + 3 * i, v);
    }
  }
#endif
  scalar_span<K>(b + 3 * i, s + 3 * i, threshold, out + 3 * i, count - i);
}
}  // namespace ps::plugin_internal::nonseparable
