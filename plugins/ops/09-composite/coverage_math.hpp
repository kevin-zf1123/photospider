// Copyright 2026 Photospider Authors
#pragma once

#include <algorithm>
#include <cstddef>

#include "09-composite/blend_math.hpp"

namespace ps::plugin_internal::coverage {
/** CMP equations in the caller's native coordinate domain and dtype. */
enum class Kind {
  Clear,
  Source,
  Destination,
  SourceOver,
  DestinationOver,
  BlendedOver,
  SourceIn,
  DestinationIn,
  SourceOut,
  DestinationOut,
  SourceAtop,
  DestinationAtop,
  Xor,
  PlusRaw,
  PlusBounded
};
#if defined(__x86_64__) && defined(__clang__)
void span_avx2(Kind, const float*, const float*, const float*, float*,
               std::size_t, unsigned, bool);
void span_avx2(Kind, const double*, const double*, const double*, double*,
               std::size_t, unsigned, bool);
#endif
constexpr bool source_only(Kind k) {
  return k == Kind::SourceIn || k == Kind::SourceOut;
}
constexpr bool destination_only(Kind k) {
  return k == Kind::DestinationIn || k == Kind::DestinationOut;
}
constexpr bool source_color(Kind k) {
  return k != Kind::DestinationIn && k != Kind::DestinationOut;
}
constexpr bool background_color(Kind k) {
  return k != Kind::SourceIn && k != Kind::SourceOut;
}
template <class T>
struct Weights {
  T source, background, blended, alpha;
};

/** Native weight staging shared by scalar, SIMD and strided execution.
 *
 * This implementation uses separate nearest/ties-to-even operations in T,
 * with gradual underflow and contraction disabled. R below rounds to T;
 * s, d and f are the source, background and supplied-blend coefficients.
 * These stages document this implementation. The CMP specification permits
 * other graphs under COM's comparison policy and CMP's exact behavior rules.
 *
 * @code
 * Kind             s                 d                 f
 * SourceOver       a                 R(R(1-a)*b)        0
 * DestinationOver  R(R(1-b)*a)        b                 0
 * BlendedOver      R(R(1-b)*a)        R(R(1-a)*b)        R(a*b)
 * SourceIn         R(b*a)            +0                0
 * DestinationIn    +0                R(a*b)            0
 * SourceOut        R(R(1-b)*a)       +0                0
 * DestinationOut   +0                R(R(1-a)*b)       0
 * SourceAtop       a                 R(1-a)            0
 * DestinationAtop  R(1-b)            b                 0
 * Xor              R(R(1-b)*a)        R(R(1-a)*b)        0
 * PlusRaw/Bounded  a                 b                 0
 * @endcode
 *
 * Alpha is R(s+d), including the +0 addition for in/out. SourceAtop copies b;
 * DestinationAtop copies a. BlendedOver uses R(a+R(R(1-a)*b)). PlusBounded
 * clamps R(s+d) to 1. Products may underflow; sums may overflow.
 */
template <Kind K, class O>
inline Weights<typename O::Value> weights_ops(typename O::Value a,
                                              typename O::Value b) {
  const auto one = O::template constant<4>(), zero = O::template constant<0>();
  auto s = zero, d = zero, f = zero;
  if constexpr (K == Kind::SourceOver) {
    s = a;
    d = O::mul(O::sub(one, a), b);
  } else if constexpr (K == Kind::DestinationOver) {
    s = O::mul(O::sub(one, b), a);
    d = b;
  } else if constexpr (K == Kind::BlendedOver) {
    s = O::mul(O::sub(one, b), a);
    d = O::mul(O::sub(one, a), b);
    f = O::mul(a, b);
  } else if constexpr (K == Kind::SourceIn) {
    s = O::mul(b, a);
  } else if constexpr (K == Kind::DestinationIn) {
    d = O::mul(a, b);
  } else if constexpr (K == Kind::SourceOut) {
    s = O::mul(O::sub(one, b), a);
  } else if constexpr (K == Kind::DestinationOut) {
    d = O::mul(O::sub(one, a), b);
  } else if constexpr (K == Kind::SourceAtop) {
    s = a;
    d = O::sub(one, a);
  } else if constexpr (K == Kind::DestinationAtop) {
    s = O::sub(one, b);
    d = b;
  } else if constexpr (K == Kind::Xor) {
    s = O::mul(O::sub(one, b), a);
    d = O::mul(O::sub(one, a), b);
  } else {
    s = a;
    d = b;
  }
  auto alpha = O::add(s, d);
  if constexpr (K == Kind::SourceAtop)
    alpha = b;
  else if constexpr (K == Kind::DestinationAtop)
    alpha = a;
  else if constexpr (K == Kind::BlendedOver)
    alpha = O::add(a, O::mul(O::sub(one, a), b));
  else if constexpr (K == Kind::PlusBounded)
    alpha = O::choose(O::le(one, alpha), one, alpha);
  return {s, d, f, alpha};
}
template <Kind K, class T>
inline Weights<T> weights(T a, T b) {
  return weights_ops<K, blend::Scalar<T>>(a, b);
}

/** Evaluate color using the implementation's native rounding stages.
 *
 * With R as in weights_ops(), term(w,C) is +0 without a color read when w is
 * zero, and R(w*C) otherwise. Generated zero alpha returns +0 before color
 * access. At positive alpha, in/out return the selected stored color directly.
 *
 * BlendedOver forms P=R(R(term(d,B)+term(s,S))+term(f,F)); other weighted
 * members form P=R(term(s,S)+term(d,B)). PlusRaw and atop return P directly.
 * PlusBounded first clamps P to 1; the remaining members return R(P/alpha).
 * The SIMD paths reproduce this staging, including the guarded reads.
 */
template <Kind K, class T, class S, class B, class F>
inline T color(const Weights<T>& w, S source, B background, F blended) {
  if constexpr (K != Kind::PlusRaw)
    if (w.alpha == 0)
      return T(0);
  // The sole nonzero color weight cancels with positive generated alpha.
  // Native alpha staging determines the zero/underflow branch above.
  if constexpr (K == Kind::SourceIn || K == Kind::SourceOut)
    return source();
  if constexpr (K == Kind::DestinationIn || K == Kind::DestinationOut)
    return background();
  const T s = w.source == 0 ? T(0) : w.source * source();
  const T b = w.background == 0 ? T(0) : w.background * background();
  T p;
  if constexpr (K == Kind::BlendedOver) {
    const T f = w.blended == 0 ? T(0) : w.blended * blended();
    p = (b + s) + f;
  } else {
    p = s + b;
  }
  if constexpr (K == Kind::PlusRaw || K == Kind::SourceAtop ||
                K == Kind::DestinationAtop)
    return p;
  if constexpr (K == Kind::PlusBounded)
    p = std::min(T(1), p);
  return p / w.alpha;
}

#if defined(__aarch64__)
template <class T, unsigned C>
struct Interleaved;
#define PS_CMP_INTERLEAVED(T, S, C, V)                          \
  template <>                                                   \
  struct Interleaved<T, C> {                                    \
    using Packet = V;                                           \
    static Packet load(const T* p) { return vld##C##q_##S(p); } \
    static void save(T* p, Packet v) { vst##C##q_##S(p, v); }   \
  }
PS_CMP_INTERLEAVED(float, f32, 2, float32x4x2_t);
PS_CMP_INTERLEAVED(double, f64, 2, float64x2x2_t);
PS_CMP_INTERLEAVED(float, f32, 3, float32x4x3_t);
PS_CMP_INTERLEAVED(double, f64, 3, float64x2x3_t);
PS_CMP_INTERLEAVED(float, f32, 4, float32x4x4_t);
PS_CMP_INTERLEAVED(double, f64, 4, float64x2x4_t);
#undef PS_CMP_INTERLEAVED

// Guard before interleaved color loads, including native-underflow weights.
// In/out load only the one color image in their mathematical support.
template <Kind K, class T, unsigned C>
inline bool packet(const T* s, const T* b, const T* f, T* out,
                   bool raw_mass = false) {
  using O = blend::Neon<T>;
  T sa[O::lanes], ba[O::lanes];
  for (unsigned j = 0; j < O::lanes; ++j) {
    sa[j] = s[C * j + C - 1];
    ba[j] = b[C * j + C - 1];
  }
  const auto w = weights_ops<K, O>(O::load(sa), O::load(ba));
  if constexpr (K == Kind::PlusRaw) {
    if (raw_mass) {
      O::save(out, w.alpha);
      return true;
    }
  }
  const auto zero = O::template constant<0>(), one = O::template constant<4>();
  auto inactive = vdupq_n_u32(0);
  const auto include_zero = [&](typename O::Value v) {
    const auto mask = O::eq(v, zero);
    uint32x4_t bits;
    std::memcpy(&bits, &mask, sizeof(bits));
    inactive = vorrq_u32(inactive, bits);
  };
  if constexpr (K != Kind::PlusRaw)
    include_zero(w.alpha);
  if constexpr (source_color(K))
    include_zero(w.source);
  if constexpr (background_color(K))
    include_zero(w.background);
  if constexpr (K == Kind::BlendedOver)
    include_zero(w.blended);
  if (vmaxvq_u32(inactive))
    return false;
  typename Interleaved<T, C>::Packet sv{}, bv{}, result{};
  if constexpr (source_color(K))
    sv = Interleaved<T, C>::load(s);
  if constexpr (background_color(K))
    bv = Interleaved<T, C>::load(b);
  typename O::Value fv[3];
  if constexpr (K == Kind::BlendedOver) {
    if constexpr (C == 2) {
      fv[0] = O::load(f);
    } else {
      const auto v = Interleaved<T, 3>::load(f);
      for (unsigned c = 0; c < 3; ++c)
        fv[c] = v.val[c];
    }
  }
  for (unsigned c = 0; c < C - 1; ++c) {
    if constexpr (K == Kind::SourceIn || K == Kind::SourceOut) {
      result.val[c] = sv.val[c];
    } else if constexpr (destination_only(K)) {
      result.val[c] = bv.val[c];
    } else {
      const auto x = source_color(K) ? O::mul(w.source, sv.val[c]) : zero;
      const auto y =
          background_color(K) ? O::mul(w.background, bv.val[c]) : zero;
      auto p = O::add(x, y);
      if constexpr (K == Kind::BlendedOver)
        p = O::add(O::add(y, x), O::mul(w.blended, fv[c]));
      if constexpr (K == Kind::PlusBounded)
        p = O::choose(O::le(one, p), one, p);
      if constexpr (K != Kind::PlusRaw && K != Kind::SourceAtop &&
                    K != Kind::DestinationAtop)
        p = O::div(p, w.alpha);
      result.val[c] = p;
    }
  }
  if constexpr (K == Kind::PlusRaw) {
    if constexpr (C == 2) {
      O::save(out, result.val[0]);
    } else {
      typename Interleaved<T, 3>::Packet v;
      for (unsigned c = 0; c < 3; ++c)
        v.val[c] = result.val[c];
      Interleaved<T, 3>::save(out, v);
    }
  } else {
    result.val[C - 1] = w.alpha;
    Interleaved<T, C>::save(out, result);
  }
  return true;
}
#endif

/** Packed GrayA/RGBA path. No unselected AOVs occur in this layout. */
template <Kind K, class T, unsigned C>
inline void span(const T* source, const T* background, const T* blended,
                 T* output, std::size_t count) {
#if defined(__x86_64__) && defined(__clang__)
  if (count >= 8 && __builtin_cpu_supports("avx2")) {
    span_avx2(K, source, background, blended, output, count, C, false);
    return;
  }
#endif
  for (std::size_t i = 0; i < count;) {
#if defined(__aarch64__)
    if (i + blend::Neon<T>::lanes <= count &&
        packet<K, T, C>(
            source + C * i, background + C * i,
            K == Kind::BlendedOver ? blended + (C - 1) * i : nullptr,
            output + C * i)) {
      i += blend::Neon<T>::lanes;
      continue;
    }
#endif
    const auto w = weights<K>(source[C * i + C - 1], background[C * i + C - 1]);
    for (unsigned c = 0; c < C - 1; ++c)
      output[C * i + c] = color<K>(
          w, [&] { return source[C * i + c]; },
          [&] { return background[C * i + c]; },
          [&] { return blended[(C - 1) * i + c]; });
    output[C * i + C - 1] = w.alpha;
    ++i;
  }
}
template <class T, unsigned C>
inline void raw_span(const T* source, const T* background, T* output,
                     std::size_t count, bool mass) {
#if defined(__x86_64__) && defined(__clang__)
  if (count >= 8 && __builtin_cpu_supports("avx2")) {
    span_avx2(Kind::PlusRaw, source, background, nullptr, output, count, C,
              mass);
    return;
  }
#endif
  for (std::size_t i = 0; i < count;) {
#if defined(__aarch64__)
    if (i + blend::Neon<T>::lanes <= count &&
        packet<Kind::PlusRaw, T, C>(source + C * i, background + C * i, nullptr,
                                    output + (mass ? 1 : C - 1) * i, mass)) {
      i += blend::Neon<T>::lanes;
      continue;
    }
#endif
    if (mass) {
      output[i] = source[C * i + C - 1] + background[C * i + C - 1];
    } else {
      const auto w = weights<Kind::PlusRaw>(source[C * i + C - 1],
                                            background[C * i + C - 1]);
      for (unsigned c = 0; c < C - 1; ++c)
        output[(C - 1) * i + c] = color<Kind::PlusRaw>(
            w, [&] { return source[C * i + c]; },
            [&] { return background[C * i + c]; }, [] { return T(0); });
    }
    ++i;
  }
}
}  // namespace ps::plugin_internal::coverage
