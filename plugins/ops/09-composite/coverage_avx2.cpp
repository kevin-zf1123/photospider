// Copyright 2026 Photospider Authors
#include <immintrin.h>

#include <cstddef>
#include <type_traits>

#include "09-composite/coverage_math.hpp"

namespace ps::plugin_internal::coverage {
namespace {
template <class T>
struct Vector;
template <>
struct Vector<double> {
  using V = __m256d;
  static V set(double x) { return _mm256_set1_pd(x); }
  static __m256i mask(unsigned n) {
    return _mm256_set_epi64x(n > 3 ? -1LL : 0, n > 2 ? -1LL : 0,
                             n > 1 ? -1LL : 0, n ? -1LL : 0);
  }
  static V load(const double* p, unsigned n) {
    return n == 4 ? _mm256_loadu_pd(p) : _mm256_maskload_pd(p, mask(n));
  }
  static void save(double* p, V v, unsigned n) {
    if (n == 4)
      _mm256_storeu_pd(p, v);
    else
      _mm256_maskstore_pd(p, mask(n), v);
  }
  static V add(V a, V b) { return _mm256_add_pd(a, b); }
  static V mul(V a, V b) { return _mm256_mul_pd(a, b); }
  static V div(V a, V b) { return _mm256_div_pd(a, b); }
  static V bound(V x) { return _mm256_min_pd(x, set(1)); }
  static V alpha(V x, double a, unsigned c) {
    const auto m = _mm256_xor_si256(mask(c), mask(c - 1));
    return _mm256_blendv_pd(x, set(a), _mm256_castsi256_pd(m));
  }
};
template <>
struct Vector<float> {
  using V = __m128;
  static V set(float x) { return _mm_set1_ps(x); }
  static __m128i mask(unsigned n) {
    return _mm_set_epi32(n > 3 ? -1 : 0, n > 2 ? -1 : 0, n > 1 ? -1 : 0,
                         n ? -1 : 0);
  }
  static V load(const float* p, unsigned n) {
    return n == 4 ? _mm_loadu_ps(p) : _mm_maskload_ps(p, mask(n));
  }
  static void save(float* p, V v, unsigned n) {
    if (n == 4)
      _mm_storeu_ps(p, v);
    else
      _mm_maskstore_ps(p, mask(n), v);
  }
  static V add(V a, V b) { return _mm_add_ps(a, b); }
  static V mul(V a, V b) { return _mm_mul_ps(a, b); }
  static V div(V a, V b) { return _mm_div_ps(a, b); }
  static V bound(V x) { return _mm_min_ps(x, set(1)); }
  static V alpha(V x, float a, unsigned c) {
    const auto m = _mm_xor_si128(mask(c), mask(c - 1));
    return _mm_blendv_ps(x, set(a), _mm_castsi128_ps(m));
  }
};

struct GrayPacket {
  using Value = __m256;
  template <unsigned Q>
  static Value constant() {
    return _mm256_set1_ps(static_cast<float>(Q) / 4);
  }
  static Value add(Value a, Value b) { return _mm256_add_ps(a, b); }
  static Value sub(Value a, Value b) { return _mm256_sub_ps(a, b); }
  static Value mul(Value a, Value b) { return _mm256_mul_ps(a, b); }
  static Value le(Value a, Value b) { return _mm256_cmp_ps(a, b, _CMP_LE_OQ); }
  static Value choose(Value m, Value a, Value b) {
    return _mm256_blendv_ps(b, a, m);
  }
};
template <Kind K>
void gray_packet(const float* s, const float* b, const float* f, float* out) {
  using O = GrayPacket;
  const auto alpha_mask = _mm256_set1_epi64x(INT64_C(-4294967296));
  const auto a = _mm256_movehdup_ps(_mm256_maskload_ps(s, alpha_mask));
  const auto d = _mm256_movehdup_ps(_mm256_maskload_ps(b, alpha_mask));
  const auto w = weights_ops<K, O>(a, d);
  const auto zero = _mm256_setzero_ps(), one = _mm256_set1_ps(1);
  const auto active = _mm256_cmp_ps(w.alpha, zero, _CMP_NEQ_OQ);
  const auto load = [&](const float* p, __m256 weight) {
    const auto mask =
        _mm256_and_ps(active, _mm256_cmp_ps(weight, zero, _CMP_NEQ_OQ));
    return _mm256_maskload_ps(p, _mm256_castps_si256(mask));
  };
  const auto weighted = [&](const __m256 weight, const __m256 value) {
    // Match scalar coefficient-zero +0, including a stored -0 alpha.
    const auto nonzero = _mm256_cmp_ps(weight, zero, _CMP_NEQ_OQ);
    return _mm256_and_ps(O::mul(weight, value), nonzero);
  };
  const auto sv = load(s, w.source), bv = load(b, w.background);
  __m256 p;
  if constexpr (source_only(K)) {
    p = sv;
  } else if constexpr (destination_only(K)) {
    p = bv;
  } else {
    const auto x = weighted(w.source, sv), y = weighted(w.background, bv);
    p = O::add(x, y);
    if constexpr (K == Kind::BlendedOver) {
      const auto fm =
          _mm256_and_ps(active, _mm256_cmp_ps(w.blended, zero, _CMP_NEQ_OQ));
      const auto packed_mask = _mm256_castps256_ps128(_mm256_permutevar8x32_ps(
          fm, _mm256_setr_epi32(0, 2, 4, 6, 0, 0, 0, 0)));
      const auto packed_f = _mm_maskload_ps(f, _mm_castps_si128(packed_mask));
      const auto fv =
          _mm256_permutevar8x32_ps(_mm256_castps128_ps256(packed_f),
                                   _mm256_setr_epi32(0, 0, 1, 1, 2, 2, 3, 3));
      p = O::add(O::add(y, x), weighted(w.blended, fv));
    }
    if constexpr (K == Kind::PlusBounded)
      p = _mm256_min_ps(p, one);
    if constexpr (K != Kind::SourceAtop && K != Kind::DestinationAtop)
      p = _mm256_div_ps(p, O::choose(active, w.alpha, one));
    p = _mm256_and_ps(p, active);
  }
  _mm256_storeu_ps(out, _mm256_blend_ps(p, w.alpha, 0xaa));
}

/** Colors vectorize within a pixel. Zero coefficients guard every color load;
 * masked loads/stores never touch a fourth supplied-F/raw-color component.
 */
template <Kind K, class T, unsigned C>
void arithmetic_span(const T* s, const T* b, const T* f, T* out,
                     std::size_t count, bool mass) {
  using O = Vector<T>;
  std::size_t i = 0;
  if constexpr (C == 2 && std::is_same_v<T, float> && K != Kind::PlusRaw) {
    for (; i + 4 <= count; i += 4)
      gray_packet<K>(s + 2 * i, b + 2 * i,
                     K == Kind::BlendedOver ? f + i : nullptr, out + 2 * i);
  }
  for (; i < count; ++i) {
    const auto w = weights<K>(s[C * i + C - 1], b[C * i + C - 1]);
    if constexpr (K == Kind::PlusRaw) {
      if (mass) {
        out[i] = w.alpha;
        continue;
      }
    }
    const auto zero = O::set(0);
    auto p = zero;
    if (K == Kind::PlusRaw || w.alpha != 0) {
      if constexpr (K == Kind::SourceIn || K == Kind::SourceOut) {
        p = O::load(s + C * i, C);
      } else if constexpr (destination_only(K)) {
        p = O::load(b + C * i, C);
      } else {
        const auto x = w.source == 0
                           ? zero
                           : O::mul(O::set(w.source), O::load(s + C * i, C));
        const auto y = w.background == 0 ? zero
                                         : O::mul(O::set(w.background),
                                                  O::load(b + C * i, C));
        if constexpr (K == Kind::BlendedOver) {
          const auto z =
              w.blended == 0
                  ? zero
                  : O::mul(O::set(w.blended), O::load(f + (C - 1) * i, C - 1));
          p = O::add(O::add(y, x), z);
        } else {
          p = O::add(x, y);
        }
        if constexpr (K == Kind::PlusBounded)
          p = O::bound(p);
        if constexpr (K != Kind::PlusRaw && K != Kind::SourceAtop &&
                      K != Kind::DestinationAtop)
          p = O::div(p, O::set(w.alpha));
      }
    }
    if constexpr (K == Kind::PlusRaw) {
      O::save(out + (C - 1) * i, p, C - 1);
    } else {
      O::save(out + C * i, O::alpha(p, w.alpha, C), C);
    }
  }
}
template <class T, unsigned C>
void dispatch(Kind kind, const T* s, const T* b, const T* f, T* out,
              std::size_t count, bool mass) {
#define PS_CMP_CASE(K)                                         \
  case Kind::K:                                                \
    arithmetic_span<Kind::K, T, C>(s, b, f, out, count, mass); \
    return
  switch (kind) {
    PS_CMP_CASE(SourceOver);
    PS_CMP_CASE(DestinationOver);
    PS_CMP_CASE(BlendedOver);
    PS_CMP_CASE(SourceIn);
    PS_CMP_CASE(DestinationIn);
    PS_CMP_CASE(SourceOut);
    PS_CMP_CASE(DestinationOut);
    PS_CMP_CASE(SourceAtop);
    PS_CMP_CASE(DestinationAtop);
    PS_CMP_CASE(Xor);
    PS_CMP_CASE(PlusRaw);
    PS_CMP_CASE(PlusBounded);
    default:
      return;
  }
#undef PS_CMP_CASE
}
}  // namespace
void span_avx2(Kind kind, const float* s, const float* b, const float* f,
               float* out, std::size_t count, unsigned channels, bool mass) {
  if (channels == 2)
    dispatch<float, 2>(kind, s, b, f, out, count, mass);
  else
    dispatch<float, 4>(kind, s, b, f, out, count, mass);
}
void span_avx2(Kind kind, const double* s, const double* b, const double* f,
               double* out, std::size_t count, unsigned channels, bool mass) {
  if (channels == 2)
    dispatch<double, 2>(kind, s, b, f, out, count, mass);
  else
    dispatch<double, 4>(kind, s, b, f, out, count, mass);
}
}  // namespace ps::plugin_internal::coverage
