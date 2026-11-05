// Copyright 2026 Photospider contributors
#include <immintrin.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "09-composite/nonseparable_math.hpp"

namespace ps::plugin_internal::nonseparable {
namespace {
// The AVX2 intrinsic signature specifically requires long long pointers.
using IntrinsicInt64 = long long;  // NOLINT(runtime/int)
template <class T>
struct Avx;
#define PS_NBL_X86(T, V, S, I)                                                 \
  template <>                                                                  \
  struct Avx<T> {                                                              \
    using Value = V;                                                           \
    using Mask = V;                                                            \
    using ScalarType = T;                                                      \
    static constexpr unsigned lanes = 4;                                       \
    static V set(T x) { return I##_set1_##S(x); }                              \
    static V load(const T* p) { return I##_loadu_##S(p); }                     \
    static void save(T* p, V v) { I##_storeu_##S(p, v); }                      \
    static V add(V a, V b) { return I##_add_##S(a, b); }                       \
    static V sub(V a, V b) { return I##_sub_##S(a, b); }                       \
    static V mul(V a, V b) { return I##_mul_##S(a, b); }                       \
    static V div(V a, V b) { return I##_div_##S(a, b); }                       \
    static V madd(V a, V b, V c) { return I##_add_##S(I##_mul_##S(a, b), c); } \
    static V root(V a) { return I##_sqrt_##S(a); }                             \
    static V abs(V a) { return I##_andnot_##S(set(-T(0)), a); }                \
    static V min(V a, V b) { return I##_min_##S(a, b); }                       \
    static V max(V a, V b) { return I##_max_##S(a, b); }                       \
    static V le(V a, V b) { return I##_cmp_##S(a, b, _CMP_LE_OQ); }            \
    static V lt(V a, V b) { return I##_cmp_##S(a, b, _CMP_LT_OQ); }            \
    static V eq(V a, V b) { return I##_cmp_##S(a, b, _CMP_EQ_OQ); }            \
    static V both(V a, V b) { return I##_and_##S(a, b); }                      \
    static V either(V a, V b) { return I##_or_##S(a, b); }                     \
    static V invert(V a) { return I##_xor_##S(a, eq(set(0), set(0))); }        \
    static bool any(V a) { return I##_movemask_##S(a) != 0; }                  \
    static V choose(V m, V a, V b) { return I##_blendv_##S(b, a, m); }         \
  }
PS_NBL_X86(float, __m128, ps, _mm);
PS_NBL_X86(double, __m256d, pd, _mm256);
#undef PS_NBL_X86

template <class O>
inline Triple<typename O::Value> load3(const typename O::ScalarType* p) {
  const auto a = O::load(p), b = O::load(p + 4), c = O::load(p + 8);
  const auto r = __builtin_shufflevector(a, b, 0, 3, 6, 0);
  const auto g = __builtin_shufflevector(a, b, 1, 4, 7, 0);
  const auto v = __builtin_shufflevector(a, b, 2, 5, 0, 0);
  return {__builtin_shufflevector(r, c, 0, 1, 2, 5),
          __builtin_shufflevector(g, c, 0, 1, 2, 6),
          __builtin_shufflevector(v, c, 0, 1, 4, 7)};
}
template <class O>
inline void save3(typename O::ScalarType* p, Triple<typename O::Value> c) {
  const auto a = __builtin_shufflevector(c.l, c.a, 0, 4, 1, 5);
  const auto b = __builtin_shufflevector(c.l, c.a, 2, 6, 3, 7);
  const auto middle = __builtin_shufflevector(a, c.b, 3, 5, 0, 0);
  O::save(p, __builtin_shufflevector(a, c.b, 0, 1, 4, 2));
  O::save(p + 4, __builtin_shufflevector(middle, b, 0, 1, 4, 5));
  O::save(p + 8, __builtin_shufflevector(b, c.b, 6, 2, 3, 7));
}
template <class T>
inline void copy_pixel(const T* a, const T* b, T* o) {
  std::memcpy(o, a, sizeof(T));
  std::memcpy(o + 1, b + 1, 2 * sizeof(T));
}
template <class T, bool Stream>
void copy_channels(const T* a, const T* b, T* out, std::size_t n) {
  std::size_t i = 0;
  if constexpr (Stream) {
    while (i < n && reinterpret_cast<std::uintptr_t>(out + 3 * i) % 32) {
      copy_pixel(a + 3 * i, b + 3 * i, out + 3 * i);
      ++i;
    }
  }
  constexpr unsigned w = 32 / sizeof(T);
  const __m256i p0 = sizeof(T) == 4
                         ? _mm256_setr_epi32(-1, 0, 0, -1, 0, 0, -1, 0)
                         : _mm256_setr_epi64x(-1, 0, 0, -1);
  const __m256i p1 = sizeof(T) == 4
                         ? _mm256_setr_epi32(0, -1, 0, 0, -1, 0, 0, -1)
                         : _mm256_setr_epi64x(0, 0, -1, 0);
  const __m256i p2 = sizeof(T) == 4
                         ? _mm256_setr_epi32(0, 0, -1, 0, 0, -1, 0, 0)
                         : _mm256_setr_epi64x(0, -1, 0, 0);
  const auto transfer = [&](std::size_t j, __m256i mask) {
    __m256i x, y;
    const auto inverse = _mm256_xor_si256(mask, _mm256_set1_epi32(-1));
    if constexpr (sizeof(T) == 4) {
      x = _mm256_maskload_epi32(reinterpret_cast<const int*>(a + j), mask);
      y = _mm256_maskload_epi32(reinterpret_cast<const int*>(b + j), inverse);
    } else {
      x = _mm256_maskload_epi64(reinterpret_cast<const IntrinsicInt64*>(a + j),
                                mask);
      y = _mm256_maskload_epi64(reinterpret_cast<const IntrinsicInt64*>(b + j),
                                inverse);
    }
    const auto v = _mm256_or_si256(x, y);
    if constexpr (Stream)
      _mm256_stream_si256(reinterpret_cast<__m256i*>(out + j), v);
    else
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + j), v);
  };
  for (; i + w <= n; i += w) {
    transfer(3 * i, p0);
    transfer(3 * i + w, p1);
    transfer(3 * i + 2 * w, p2);
  }
  for (; i < n; ++i)
    copy_pixel(a + 3 * i, b + 3 * i, out + 3 * i);
  if constexpr (Stream)
    _mm_sfence();
}

template <Kind K, class T>
void packets(const T* b, const T* s, T threshold, T* out, std::size_t n) {
  if constexpr (copy_only(K)) {
    const auto* l = K == Kind::Color ? b : s;
    const auto* ab = K == Kind::Color ? s : b;
    if (n * 3 * sizeof(T) >= 4 * 1024 * 1024)
      copy_channels<T, true>(l, ab, out, n);
    else
      copy_channels<T, false>(l, ab, out, n);
    return;
  }
  std::size_t i = 0;
  if constexpr (!copy_only(K)) {
    using O = Avx<T>;
    for (; i + 4 <= n; i += 4)
      save3<O>(out + 3 * i, evaluate<K, O>(load3<O>(b + 3 * i),
                                           load3<O>(s + 3 * i), threshold));
  }
  scalar_span<K>(b + 3 * i, s + 3 * i, threshold, out + 3 * i, n - i);
}
template <class T>
void dispatch(Kind k, const T* b, const T* s, T threshold, T* out,
              std::size_t n) {
#define PS_NBL_CASE(K)                         \
  case Kind::K:                                \
    packets<Kind::K>(b, s, threshold, out, n); \
    break
  switch (k) {
    PS_NBL_CASE(Hue);
    PS_NBL_CASE(ChromaZero);
    PS_NBL_CASE(ChromaPreserve);
    PS_NBL_CASE(ChromaSource);
    PS_NBL_CASE(Color);
    PS_NBL_CASE(Lightness);
    PS_NBL_CASE(HueW3c);
    PS_NBL_CASE(SaturationW3c);
    PS_NBL_CASE(ColorW3c);
    PS_NBL_CASE(LuminosityW3c);
  }
#undef PS_NBL_CASE
}
}  // namespace
void span_avx2(Kind k, const float* b, const float* s, float threshold,
               float* out, std::size_t n) {
  dispatch(k, b, s, threshold, out, n);
}
void span_avx2(Kind k, const double* b, const double* s, double threshold,
               double* out, std::size_t n) {
  dispatch(k, b, s, threshold, out, n);
}
}  // namespace ps::plugin_internal::nonseparable
