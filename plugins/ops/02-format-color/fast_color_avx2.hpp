// Copyright 2026 Photospider contributors
#pragma once
#include <immintrin.h>

#include <algorithm>
#include <array>
namespace ps::plugin_internal::fast_color::avx2 {
template <class T>
struct Packet;
template <>
struct Packet<float> {
  using V = __m256;
  using Mask = V;
  static constexpr unsigned width = 8;
  static constexpr unsigned unroll = 1;
  static V set(float x) { return _mm256_set1_ps(x); }
  static V add(V a, V b) { return _mm256_add_ps(a, b); }
  static V sub(V a, V b) { return _mm256_sub_ps(a, b); }
  static V mul(V a, V b) { return _mm256_mul_ps(a, b); }
  static V div(V a, V b) { return _mm256_div_ps(a, b); }
  static V fma(V a, V b, V c) { return _mm256_fmadd_ps(a, b, c); }
  static V min(V a, V b) { return _mm256_min_ps(a, b); }
  static V max(V a, V b) { return _mm256_max_ps(a, b); }
  static V abs(V a) { return _mm256_andnot_ps(set(-0.0f), a); }
  static Mask eq(V a, V b) { return _mm256_cmp_ps(a, b, _CMP_EQ_OQ); }
  static Mask lt(V a, V b) { return _mm256_cmp_ps(a, b, _CMP_LT_OQ); }
  static Mask ge(V a, V b) { return _mm256_cmp_ps(a, b, _CMP_GE_OQ); }
  static V select(Mask m, V a, V b) { return _mm256_blendv_ps(b, a, m); }
  static bool all(Mask m) { return _mm256_movemask_ps(m) == 255; }
  static bool in_range(V h, V period) {
    return all(
        _mm256_and_ps(lt(h, period), _mm256_cmp_ps(h, set(0), _CMP_GE_OQ)));
  }
  static V load(const float* p) { return _mm256_loadu_ps(p); }
  static void store(float* p, V x) { _mm256_storeu_ps(p, x); }
  static std::array<V, 3> load3(const float* p) {
    const auto aa = load(p), bb = load(p + 8), cc = load(p + 16);
    const auto a = _mm256_permute2f128_ps(aa, bb, 0x30);
    const auto b = _mm256_permute2f128_ps(aa, cc, 0x21);
    const auto c = _mm256_permute2f128_ps(bb, cc, 0x30);
    const auto t0 = _mm256_shuffle_ps(a, b, _MM_SHUFFLE(1, 0, 3, 2));
    const auto t1 = _mm256_shuffle_ps(b, c, _MM_SHUFFLE(2, 1, 3, 2));
    const auto r = _mm256_shuffle_ps(a, t1, _MM_SHUFFLE(2, 0, 3, 0));
    const auto g = _mm256_shuffle_ps(a, b, _MM_SHUFFLE(3, 0, 1, 1));
    return {r, _mm256_shuffle_ps(g, t1, _MM_SHUFFLE(3, 1, 2, 0)),
            _mm256_shuffle_ps(t0, c, _MM_SHUFFLE(3, 0, 3, 0))};
  }
  static void store3(float* p, const std::array<V, 3>& v) {
    const auto rg = _mm256_unpacklo_ps(v[0], v[1]);
    const auto br = _mm256_shuffle_ps(v[2], v[0], _MM_SHUFFLE(1, 0, 1, 0));
    const auto a = _mm256_shuffle_ps(rg, br, _MM_SHUFFLE(3, 0, 1, 0));
    const auto gb = _mm256_unpacklo_ps(v[1], v[2]);
    const auto rgh = _mm256_unpackhi_ps(v[0], v[1]);
    const auto b = _mm256_shuffle_ps(gb, rgh, _MM_SHUFFLE(1, 0, 3, 2));
    const auto brh = _mm256_unpackhi_ps(v[2], v[0]);
    const auto gbh = _mm256_unpackhi_ps(v[1], v[2]);
    const auto c = _mm256_shuffle_ps(brh, gbh, _MM_SHUFFLE(3, 2, 3, 0));
    store(p, _mm256_permute2f128_ps(a, b, 0x20));
    store(p + 8, _mm256_permute2f128_ps(c, a, 0x30));
    store(p + 16, _mm256_permute2f128_ps(b, c, 0x31));
  }
};
template <>
struct Packet<double> {
  using V = __m256d;
  using Mask = V;
  static constexpr unsigned width = 4;
  static constexpr unsigned unroll = 1;
  static V set(double x) { return _mm256_set1_pd(x); }
  static V add(V a, V b) { return _mm256_add_pd(a, b); }
  static V sub(V a, V b) { return _mm256_sub_pd(a, b); }
  static V mul(V a, V b) { return _mm256_mul_pd(a, b); }
  static V div(V a, V b) { return _mm256_div_pd(a, b); }
  static V fma(V a, V b, V c) { return _mm256_fmadd_pd(a, b, c); }
  static V min(V a, V b) { return _mm256_min_pd(a, b); }
  static V max(V a, V b) { return _mm256_max_pd(a, b); }
  static V abs(V a) { return _mm256_andnot_pd(set(-0.0), a); }
  static Mask eq(V a, V b) { return _mm256_cmp_pd(a, b, _CMP_EQ_OQ); }
  static Mask lt(V a, V b) { return _mm256_cmp_pd(a, b, _CMP_LT_OQ); }
  static Mask ge(V a, V b) { return _mm256_cmp_pd(a, b, _CMP_GE_OQ); }
  static V select(Mask m, V a, V b) { return _mm256_blendv_pd(b, a, m); }
  static bool all(Mask m) { return _mm256_movemask_pd(m) == 15; }
  static bool in_range(V h, V period) {
    return all(
        _mm256_and_pd(lt(h, period), _mm256_cmp_pd(h, set(0), _CMP_GE_OQ)));
  }
  static V load(const double* p) { return _mm256_loadu_pd(p); }
  static void store(double* p, V x) { _mm256_storeu_pd(p, x); }
  static std::array<V, 3> load3(const double* p) {
    const auto aa = load(p), bb = load(p + 4), cc = load(p + 8);
    const auto a = _mm256_permute2f128_pd(aa, bb, 0x30);
    const auto b = _mm256_permute2f128_pd(aa, cc, 0x21);
    const auto c = _mm256_permute2f128_pd(bb, cc, 0x30);
    return {_mm256_shuffle_pd(a, b, 10), _mm256_shuffle_pd(a, c, 5),
            _mm256_shuffle_pd(b, c, 10)};
  }
  static void store3(double* p, const std::array<V, 3>& v) {
    const auto a = _mm256_unpacklo_pd(v[0], v[1]);
    const auto b = _mm256_shuffle_pd(v[2], v[0], 10);
    const auto c = _mm256_unpackhi_pd(v[1], v[2]);
    store(p, _mm256_permute2f128_pd(a, b, 0x20));
    store(p + 4, _mm256_permute2f128_pd(c, a, 0x30));
    store(p + 8, _mm256_permute2f128_pd(b, c, 0x31));
  }
};
}  // namespace ps::plugin_internal::fast_color::avx2
