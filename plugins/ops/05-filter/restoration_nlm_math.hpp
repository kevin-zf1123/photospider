// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "05-filter/frequency_native_simd.hpp"

#if defined(__x86_64__)
#define PS_NLM_TARGET __attribute__((target("avx2,fma")))
#define PS_NLM_INLINE inline __attribute__((always_inline, target("avx2,fma")))
#else
#define PS_NLM_TARGET
#define PS_NLM_INLINE inline __attribute__((always_inline))
#endif

namespace ps::plugin_internal::restoration::nlm {
struct Rect {
  std::int64_t y0, x0, y1, x1;
  bool empty() const { return y1 <= y0 || x1 <= x0; }
  std::size_t rows() const { return static_cast<std::size_t>(y1 - y0); }
  std::size_t cols() const { return static_cast<std::size_t>(x1 - x0); }
};
inline Rect intersect(Rect a, Rect b) {
  return {std::max(a.y0, b.y0), std::max(a.x0, b.x0), std::min(a.y1, b.y1),
          std::min(a.x1, b.x1)};
}
template <class F>
void difference(Rect a, Rect b, F f) {
  if (a.empty())
    return;
  b = intersect(a, b);
  if (b.empty()) {
    f(a);
    return;
  }
  for (const auto r :
       {Rect{a.y0, a.x0, b.y0, a.x1}, Rect{b.y1, a.x0, a.y1, a.x1},
        Rect{b.y0, a.x0, b.y1, b.x0}, Rect{b.y0, b.x1, b.y1, a.x1}})
    if (!r.empty())
      f(r);
}
inline std::int64_t reflect(std::int64_t p, std::int64_t n) {
  p %= 2 * n;
  if (p < 0)
    p += 2 * n;
  return p < n ? p : 2 * n - 1 - p;
}
struct Geometry {
  std::int64_t height, width;
  Rect output;
  std::size_t patch, search, components;
  std::int64_t sy() const {
    return static_cast<std::int64_t>(std::min<std::size_t>(search, height - 1));
  }
  std::int64_t sx() const {
    return static_cast<std::int64_t>(std::min<std::size_t>(search, width - 1));
  }
  Rect input() const {
    return intersect({output.y0 - sy(), output.x0 - sx(), output.y1 + sy(),
                      output.x1 + sx()},
                     {0, 0, height, width});
  }
  Rect guide() const {
    const auto i = input();
    const auto r = static_cast<std::int64_t>(patch);
    return {i.y0 - r, i.x0 - r, i.y1 + r, i.x1 + r};
  }
  std::size_t workspace_elements() const {
    const auto g = guide();
    // Squared difference plane and horizontal box sums, reused for every
    // displacement. Only nonnegative sums are used, avoiding sliding
    // subtraction cancellation when a large SSD sample leaves a window.
    return 2 * g.rows() * g.cols() + output.rows() * output.cols();
  }
};
struct NoPoll {
  void operator()() const {}
};
template <class T>
struct Buffers {
  const T* input;
  const T* guide;
  T* output;
  T* scratch;
  std::array<T, 4> scales;
};
template <class T>
inline T weight(T distance, T norm, T h2, T correction) {
  return std::exp(-std::max(distance / norm - correction, T(0)) / h2);
}
#if PS_FRQ_PACKETS
template <class T>
struct Packet : frequency::NativePacket<T> {
  using P = frequency::NativePacket<T>;
  using V = typename P::Register;
  static PS_NLM_INLINE V set(T a) {
#if defined(__x86_64__)
    if constexpr (sizeof(T) == 4)
      return _mm256_set1_ps(a);
    else
      return _mm256_set1_pd(a);
#else
    if constexpr (sizeof(T) == 4)
      return vdupq_n_f32(a);
    else
      return vdupq_n_f64(a);
#endif
  }
  static PS_NLM_INLINE V mul(V a, V b) {
#if defined(__x86_64__)
    if constexpr (sizeof(T) == 4)
      return _mm256_mul_ps(a, b);
    else
      return _mm256_mul_pd(a, b);
#else
    if constexpr (sizeof(T) == 4)
      return vmulq_f32(a, b);
    else
      return vmulq_f64(a, b);
#endif
  }
  static PS_NLM_INLINE V fma(V a, V b, V c) {
#if defined(__x86_64__)
    if constexpr (sizeof(T) == 4)
      return _mm256_fmadd_ps(a, b, c);
    else
      return _mm256_fmadd_pd(a, b, c);
#else
    if constexpr (sizeof(T) == 4)
      return vfmaq_f32(c, a, b);
    else
      return vfmaq_f64(c, a, b);
#endif
  }
  static PS_NLM_INLINE V positive(V a) {
#if defined(__x86_64__)
    if constexpr (sizeof(T) == 4)
      return _mm256_max_ps(P::zero(), a);
    else
      return _mm256_max_pd(P::zero(), a);
#else
    if constexpr (sizeof(T) == 4)
      return vmaxq_f32(a, P::zero());
    else
      return vmaxq_f64(a, P::zero());
#endif
  }
  static PS_NLM_INLINE V consumed(V a, V w) {
#if defined(__x86_64__)
    if constexpr (sizeof(T) == 4)
      return _mm256_and_ps(a, _mm256_cmp_ps(w, P::zero(), _CMP_NEQ_UQ));
    else
      return _mm256_and_pd(a, _mm256_cmp_pd(w, P::zero(), _CMP_NEQ_UQ));
#else
    if constexpr (sizeof(T) == 4)
      return vreinterpretq_f32_u32(
          vbicq_u32(vreinterpretq_u32_f32(a), vceqq_f32(w, P::zero())));
    else
      return vreinterpretq_f64_u64(
          vbicq_u64(vreinterpretq_u64_f64(a), vceqq_f64(w, P::zero())));
#endif
  }
  static PS_NLM_INLINE bool ordinary(V x) {
#if defined(__x86_64__)
    if constexpr (sizeof(T) == 4)
      return _mm256_movemask_ps(_mm256_cmp_ps(x, set(-80), _CMP_GE_OQ)) == 255;
    else
      return _mm256_movemask_pd(_mm256_cmp_pd(x, set(-700), _CMP_GE_OQ)) == 15;
#else
    if constexpr (sizeof(T) == 4) {
      return vminvq_u32(vcgeq_f32(x, set(-80))) != 0;
    } else {
      const auto m = vcgeq_f64(x, set(-700));
      return vgetq_lane_u64(m, 0) && vgetq_lane_u64(m, 1);
    }
#endif
  }
  static PS_NLM_INLINE V exp_negative(V x) {
    if (ordinary(x))
      return exp_normal(x);
    alignas(32) T a[P::width];
    P::store(a, x);
    for (auto& v : a)
      v = std::exp(v);
    return P::load(a);
  }
  static PS_NLM_INLINE V exp_table(V x) {
    static constexpr double table[64] = {
        0x1.0000000000000p+0, 0x1.02c9a3e778061p+0, 0x1.059b0d3158574p+0,
        0x1.0874518759bc8p+0, 0x1.0b5586cf9890fp+0, 0x1.0e3ec32d3d1a2p+0,
        0x1.11301d0125b51p+0, 0x1.1429aaea92de0p+0, 0x1.172b83c7d517bp+0,
        0x1.1a35beb6fcb75p+0, 0x1.1d4873168b9aap+0, 0x1.2063b88628cd6p+0,
        0x1.2387a6e756238p+0, 0x1.26b4565e27cddp+0, 0x1.29e9df51fdee1p+0,
        0x1.2d285a6e4030bp+0, 0x1.306fe0a31b715p+0, 0x1.33c08b26416ffp+0,
        0x1.371a7373aa9cbp+0, 0x1.3a7db34e59ff7p+0, 0x1.3dea64c123422p+0,
        0x1.4160a21f72e2ap+0, 0x1.44e086061892dp+0, 0x1.486a2b5c13cd0p+0,
        0x1.4bfdad5362a27p+0, 0x1.4f9b2769d2ca7p+0, 0x1.5342b569d4f82p+0,
        0x1.56f4736b527dap+0, 0x1.5ab07dd485429p+0, 0x1.5e76f15ad2148p+0,
        0x1.6247eb03a5585p+0, 0x1.6623882552225p+0, 0x1.6a09e667f3bcdp+0,
        0x1.6dfb23c651a2fp+0, 0x1.71f75e8ec5f74p+0, 0x1.75feb564267c9p+0,
        0x1.7a11473eb0187p+0, 0x1.7e2f336cf4e62p+0, 0x1.82589994cce13p+0,
        0x1.868d99b4492edp+0, 0x1.8ace5422aa0dbp+0, 0x1.8f1ae99157736p+0,
        0x1.93737b0cdc5e5p+0, 0x1.97d829fde4e50p+0, 0x1.9c49182a3f090p+0,
        0x1.a0c667b5de565p+0, 0x1.a5503b23e255dp+0, 0x1.a9e6b5579fdbfp+0,
        0x1.ae89f995ad3adp+0, 0x1.b33a2b84f15fbp+0, 0x1.b7f76f2fb5e47p+0,
        0x1.bcc1e904bc1d2p+0, 0x1.c199bdd85529cp+0, 0x1.c67f12e57d14bp+0,
        0x1.cb720dcef9069p+0, 0x1.d072d4a07897cp+0, 0x1.d5818dcfba487p+0,
        0x1.da9e603db3285p+0, 0x1.dfc97337b9b5fp+0, 0x1.e502ee78b3ff6p+0,
        0x1.ea4afa2a490dap+0, 0x1.efa1bee615a27p+0, 0x1.f50765b6e4540p+0,
        0x1.fa7c1819e90d8p+0};
    const auto magic = set(T(0x1.8p52));
    const auto z = fma(x, set(T(0x1.71547652b82fep+6)), magic);
    const auto nn = P::sub(z, magic);
    const auto b = fma(nn, set(-T(0x1.cf79abc9e3b3ap-46)),
                       fma(nn, set(-T(0x1.62e42fefa0000p-7)), x));
    V power, factor;
#if defined(__x86_64__)
    const auto bits = _mm256_castpd_si256(z);
    const auto indices = _mm256_and_si256(bits, _mm256_set1_epi64x(63));
    factor = _mm256_i64gather_pd(table, indices, 8);
    power = _mm256_castsi256_pd(_mm256_add_epi64(
        _mm256_and_si256(_mm256_slli_epi64(bits, 46),
                         _mm256_set1_epi64x(INT64_C(0xfff0000000000000))),
        _mm256_set1_epi64x(INT64_C(0x3ff0000000000000))));
#else
    const auto bits = vreinterpretq_u64_f64(z);
    const auto indices = vandq_u64(bits, vdupq_n_u64(63));
    factor = vdupq_n_f64(table[vgetq_lane_u64(indices, 0)]);
    factor = vsetq_lane_f64(table[vgetq_lane_u64(indices, 1)], factor, 1);
    power = vreinterpretq_f64_u64(
        vaddq_u64(vandq_u64(vshlq_n_u64(bits, 46),
                            vdupq_n_u64(UINT64_C(0xfff0000000000000))),
                  vdupq_n_u64(UINT64_C(0x3ff0000000000000))));
#endif
    const auto b2 = mul(b, b);
    const auto p0 = P::add(set(T(1)), b);
    const auto p1 = fma(set(T(0x1.5555555555555p-3)), b, set(T(0.5)));
    const auto p2 =
        fma(set(T(0x1.1111111111111p-7)), b, set(T(0x1.5555555555555p-5)));
    const auto p =
        fma(b2, fma(b2, fma(b2, set(T(0x1.6c16c16c16c17p-10)), p2), p1), p0);
    return mul(mul(p, factor), power);
  }
  static PS_NLM_INLINE V exp_normal(V x) {
    if constexpr (sizeof(T) == 8) {
      return exp_table(x);
    } else {
      // Normal negative arguments; exceptional/subnormal arguments use libm.
      const auto z = fma(x, set(T(0x1.71547652b82fep+0)), set(T(0x1.8p23)));
      const auto n = P::sub(z, set(T(0x1.8p23)));
      V b, power;
      b = fma(n, set(-T(0x1.7f7d1cp-20)), fma(n, set(-T(0x1.62e4p-1)), x));
#if defined(__x86_64__)
      power = _mm256_castsi256_ps(
          _mm256_add_epi32(_mm256_slli_epi32(_mm256_castps_si256(z), 23),
                           _mm256_set1_epi32(0x3f800000)));
#else
      power = vreinterpretq_f32_u32(vaddq_u32(
          vshlq_n_u32(vreinterpretq_u32_f32(z), 23), vdupq_n_u32(0x3f800000)));
#endif
      const auto b2 = mul(b, b), b4 = mul(b2, b2);
      const auto p0 = P::add(set(T(1)), b);
      const auto p1 = fma(set(T(0x1.5555555555555p-3)), b, set(T(0.5)));
      const auto p2 =
          fma(set(T(0x1.1111111111111p-7)), b, set(T(0x1.5555555555555p-5)));
      const auto p3 =
          fma(set(T(0x1.a01a01a01a01ap-13)), b, set(T(0x1.6c16c16c16c17p-10)));
      return mul(fma(b4, fma(b2, p3, p2), fma(b2, p1, p0)), power);
    }
  }
};

template <class T, class Poll, unsigned Components>
PS_NLM_TARGET void squared_impl(const T* a, const T* b, T* out, std::size_t n,
                                std::size_t plane, std::size_t components,
                                const std::array<T, 4>& scale, Poll poll) {
  using P = Packet<T>;
  components = Components;
  std::size_t x = 0;
  for (; x + P::width <= n; x += P::width) {
    if (!(x & 255))
      poll();
    auto sum = P::zero();
    for (std::size_t c = 0; c < components; ++c) {
      const auto d = P::scale(
          P::sub(P::load(a + c * plane + x), P::load(b + c * plane + x)),
          scale[c]);
      sum = P::fma(d, d, sum);
    }
    P::store(out + x, sum);
  }
  for (; x < n; ++x) {
    T sum = 0;
    for (std::size_t c = 0; c < components; ++c) {
      const T d = (a[c * plane + x] - b[c * plane + x]) * scale[c];
      sum = std::fma(d, d, sum);
    }
    out[x] = sum;
  }
}
template <class T, class Poll>
PS_NLM_TARGET void squared(const T* a, const T* b, T* out, std::size_t n,
                           std::size_t plane, std::size_t components,
                           const std::array<T, 4>& scale, Poll poll) {
  switch (components) {
    case 1:
      squared_impl<T, Poll, 1>(a, b, out, n, plane, 1, scale, poll);
      break;
    case 2:
      squared_impl<T, Poll, 2>(a, b, out, n, plane, 2, scale, poll);
      break;
    case 3:
      squared_impl<T, Poll, 3>(a, b, out, n, plane, 3, scale, poll);
      break;
    default:
      squared_impl<T, Poll, 4>(a, b, out, n, plane, 4, scale, poll);
      break;
  }
}
template <class T, class Poll, unsigned Taps>
PS_NLM_TARGET void box_row_impl(const T* in, T* out, std::size_t n,
                                std::size_t taps, std::size_t stride,
                                Poll poll) {
  using P = Packet<T>;
  if constexpr (Taps)
    taps = Taps;
  std::size_t x = 0;
  for (; x + 4 * P::width <= n; x += 4 * P::width) {
    if (!(x & 255))
      poll();
    auto a = P::load(in + x), b = P::load(in + x + P::width);
    auto c = P::load(in + x + 2 * P::width), d = P::load(in + x + 3 * P::width);
    for (std::size_t k = 1; k < taps; ++k) {
      if constexpr (!Taps)
        if (!(k & 255))
          poll();
      a = P::add(a, P::load(in + x + k * stride));
      b = P::add(b, P::load(in + x + P::width + k * stride));
      c = P::add(c, P::load(in + x + 2 * P::width + k * stride));
      d = P::add(d, P::load(in + x + 3 * P::width + k * stride));
    }
    P::store(out + x, a);
    P::store(out + x + P::width, b);
    P::store(out + x + 2 * P::width, c);
    P::store(out + x + 3 * P::width, d);
  }
  for (; x + P::width <= n; x += P::width) {
    if (!(x & 255))
      poll();
    auto sum = P::load(in + x);
    for (std::size_t k = 1; k < taps; ++k) {
      if constexpr (!Taps)
        if (!(k & 255))
          poll();
      sum = P::add(sum, P::load(in + x + k * stride));
    }
    P::store(out + x, sum);
  }
  for (; x < n; ++x) {
    T sum = in[x];
    for (std::size_t k = 1; k < taps; ++k) {
      if constexpr (!Taps)
        if (!(k & 255))
          poll();
      sum += in[x + k * stride];
    }
    out[x] = sum;
  }
}
template <class T, class Poll>
PS_NLM_TARGET void box_row(const T* in, T* out, std::size_t n, std::size_t taps,
                           std::size_t stride, Poll poll) {
  switch (taps) {
    case 1:
      box_row_impl<T, Poll, 1>(in, out, n, taps, stride, poll);
      break;
    case 3:
      box_row_impl<T, Poll, 3>(in, out, n, taps, stride, poll);
      break;
    case 5:
      box_row_impl<T, Poll, 5>(in, out, n, taps, stride, poll);
      break;
    case 7:
      box_row_impl<T, Poll, 7>(in, out, n, taps, stride, poll);
      break;
    case 11:
      box_row_impl<T, Poll, 11>(in, out, n, taps, stride, poll);
      break;
    default:
      box_row_impl<T, Poll, 0>(in, out, n, taps, stride, poll);
      break;
  }
}
template <class T, class Poll>
PS_NLM_TARGET void make_weights(T* distances, std::size_t n, T norm, T h2,
                                T correction, Poll poll) {
  using P = Packet<T>;
  using V = typename P::V;
  const T inv_norm = T(1) / norm, inv_h2 = T(1) / h2;
  const bool ordinary = std::isnormal(h2) && std::isnormal(inv_h2);
  std::size_t x = 0;
  if (ordinary) {
    for (; x + 4 * P::width <= n; x += 4 * P::width) {
      if (!(x & 255))
        poll();
      V e[4], b[4], power[4], poly[4];
      bool normal = true;
#pragma unroll
      for (unsigned j = 0; j < 4; ++j) {
        e[j] = P::scale(
            P::positive(P::sub(
                P::scale(P::load(distances + x + j * P::width), inv_norm),
                P::set(correction))),
            -inv_h2);
        normal &= P::ordinary(e[j]);
      }
      if constexpr (sizeof(T) == 8) {
#pragma unroll
        for (unsigned j = 0; j < 4; ++j)
          P::store(distances + x + j * P::width,
                   normal ? P::exp_table(e[j]) : P::exp_negative(e[j]));
      } else if (normal) {
#pragma unroll
        for (unsigned j = 0; j < 4; ++j) {
          const auto magic = P::set(T(0x1.8p23));
          const auto z = P::fma(e[j], P::set(T(0x1.71547652b82fep+0)), magic);
          const auto nn = P::sub(z, magic);
          b[j] = P::fma(nn, P::set(-T(0x1.7f7d1cp-20)),
                        P::fma(nn, P::set(-T(0x1.62e4p-1)), e[j]));
#if defined(__x86_64__)
          power[j] = _mm256_castsi256_ps(
              _mm256_add_epi32(_mm256_slli_epi32(_mm256_castps_si256(z), 23),
                               _mm256_set1_epi32(0x3f800000)));
#else
          power[j] = vreinterpretq_f32_u32(
              vaddq_u32(vshlq_n_u32(vreinterpretq_u32_f32(z), 23),
                        vdupq_n_u32(0x3f800000)));
#endif
        }
        static constexpr T coefficient[] = {
            T(1),        T(1),         T(0.5),       T(1.0 / 6),
            T(1.0 / 24), T(1.0 / 120), T(1.0 / 720), T(1.0 / 5040)};
        constexpr int degree = 7;
#pragma unroll
        for (unsigned j = 0; j < 4; ++j)
          poly[j] = P::set(coefficient[degree]);
#if defined(__clang__)
#pragma clang loop unroll(disable)
#endif
        for (int k = degree - 1; k >= 0; --k) {
          const auto c = P::set(coefficient[k]);
#pragma unroll
          for (unsigned j = 0; j < 4; ++j)
            poly[j] = P::fma(poly[j], b[j], c);
        }
#pragma unroll
        for (unsigned j = 0; j < 4; ++j)
          P::store(distances + x + j * P::width, P::mul(poly[j], power[j]));
      } else {
        for (unsigned j = 0; j < 4; ++j)
          P::store(distances + x + j * P::width, P::exp_negative(e[j]));
      }
    }
  }
  if (ordinary) {
    for (; x + P::width <= n; x += P::width) {
      const auto e = P::scale(
          P::positive(P::sub(P::scale(P::load(distances + x), inv_norm),
                             P::set(correction))),
          -inv_h2);
      P::store(distances + x, P::exp_negative(e));
    }
  }
  for (; x < n; ++x) {
    if (!(x & 63))
      poll();
    distances[x] = weight(distances[x], norm, h2, correction);
  }
}
template <class T, class Poll>
PS_NLM_TARGET void apply_weights(const T* weights, const T* in, T* num, T* den,
                                 std::size_t n, Poll poll) {
  using P = Packet<T>;
  std::size_t x = 0;
  for (; x + P::width <= n; x += P::width) {
    if (!(x & 255))
      poll();
    const auto w = P::load(weights + x);
    P::store(num + x,
             P::fma(w, P::consumed(P::load(in + x), w), P::load(num + x)));
    P::store(den + x, P::add(P::load(den + x), w));
  }
  for (; x < n; ++x)
    if (weights[x] != 0) {
      num[x] = std::fma(weights[x], in[x], num[x]);
      den[x] += weights[x];
    }
}

#endif

template <class T, class Poll>
void squared_scalar(const T* a, const T* b, T* out, std::size_t n,
                    std::size_t plane, std::size_t components,
                    const std::array<T, 4>& scale, Poll poll) {
  for (std::size_t x = 0; x < n; ++x) {
    if (!(x & 63))
      poll();
    T sum = 0;
    for (std::size_t c = 0; c < components; ++c) {
      const T d = (a[c * plane + x] - b[c * plane + x]) * scale[c];
      sum = std::fma(d, d, sum);
    }
    out[x] = sum;
  }
}
inline bool packets_available() {
#if defined(__x86_64__)
  return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
  return PS_FRQ_PACKETS;
#endif
}
template <class T, class Poll = NoPoll>
void compute(const Geometry& g, const Buffers<T>& b, T h, T sigma,
             bool corrected, Poll poll = {}) {
  const auto i = g.input(), guide = g.guide(), r = g.output;
  const auto gw = guide.cols(), iw = i.cols(), ow = r.cols();
  const auto plane = guide.rows() * gw, count = r.rows() * ow;
  const auto input_at = [&](std::int64_t y, std::int64_t x) {
    return b.input + (y - i.y0) * iw + (x - i.x0);
  };
  const auto out_at = [&](std::int64_t y, std::int64_t x) {
    return static_cast<std::size_t>((y - r.y0) * ow + (x - r.x0));
  };
  for (std::int64_t y = r.y0; y < r.y1; ++y) {
    poll();
    for (std::size_t x = 0; x < ow; x += 256) {
      poll();
      std::memcpy(b.output + (y - r.y0) * ow + x, input_at(y, r.x0) + x,
                  std::min<std::size_t>(256, ow - x) * sizeof(T));
    }
  }
  if (!g.search)
    return;
  auto* diff = b.scratch;
  auto* horizontal = diff + plane;
  auto* den = horizontal + plane;
  for (std::size_t x = 0; x < count; x += 256) {
    poll();
    std::fill_n(den + x, std::min<std::size_t>(256, count - x), T(1));
  }
  const auto taps = 2 * g.patch + 1;
  const T norm = T(taps) * T(taps) * T(g.components);
  const T h2 = h * h, correction = corrected ? T(2) * (sigma * sigma) : T(0);
  // Magic-bias range reduction is admitted only under nearest rounding.
  // Preserve the caller's environment and use native scalar libm otherwise.
  const bool simd = packets_available() && std::fegetround() == FE_TONEAREST;
  const auto process = [&](Rect area, std::int64_t dy, std::int64_t dx, bool qa,
                           bool pb) {
    if (area.empty())
      return;
    const auto nx = area.cols(), ny = area.rows(), nw = nx + 2 * g.patch,
               nh = ny + 2 * g.patch;
    const auto start =
        (area.y0 - static_cast<std::int64_t>(g.patch) - guide.y0) * gw +
        area.x0 - static_cast<std::int64_t>(g.patch) - guide.x0;
    for (std::size_t y = 0; y < nh; ++y) {
      poll();
      const auto* a = b.guide + start + y * gw;
      const auto* other = a + dy * static_cast<std::int64_t>(gw) + dx;
#if PS_FRQ_PACKETS
      if (simd)
        squared(a, other, diff + y * nw, nw, plane, g.components, b.scales,
                poll);
      else
#endif
        squared_scalar(a, other, diff + y * nw, nw, plane, g.components,
                       b.scales, poll);
      if (!g.patch)
        continue;
#if PS_FRQ_PACKETS
      if (simd) {
        box_row(diff + y * nw, horizontal + y * nx, nx, taps, 1, poll);
      } else {
#endif
        for (std::size_t x = 0; x < nx; ++x) {
          T sum = 0;
          for (std::size_t k = 0; k < taps; ++k) {
            if (!(k & 255))
              poll();
            sum += diff[y * nw + x + k];
          }
          horizontal[y * nx + x] = sum;
        }
#if PS_FRQ_PACKETS
      }
#endif
    }
    if (g.patch) {
      for (std::size_t y = 0; y < ny; ++y) {
        poll();
#if PS_FRQ_PACKETS
        if (simd) {
          box_row(horizontal + y * nx, diff + y * nx, nx, taps, nx, poll);
        } else {
#endif
          for (std::size_t x = 0; x < nx; ++x) {
            T sum = 0;
            for (std::size_t k = 0; k < taps; ++k) {
              if (!(k & 255))
                poll();
              sum += horizontal[(y + k) * nx + x];
            }
            diff[y * nx + x] = sum;
          }
#if PS_FRQ_PACKETS
        }
#endif
      }
    }
#if PS_FRQ_PACKETS
    if (simd)
      make_weights(diff, nx * ny, norm, h2, correction, poll);
#endif
    for (std::size_t y = 0; y < ny; ++y) {
      poll();
      const auto yy = area.y0 + static_cast<std::int64_t>(y);
      const auto ai = qa ? out_at(yy, area.x0) : 0;
      const auto bi = pb ? out_at(yy + dy, area.x0 + dx) : 0;
      const T *a = input_at(yy, area.x0),
              *other = input_at(yy + dy, area.x0 + dx);
#if PS_FRQ_PACKETS
      if (simd) {
        if (qa)
          apply_weights(diff + y * nx, other, b.output + ai, den + ai, nx,
                        poll);
        if (pb)
          apply_weights(diff + y * nx, a, b.output + bi, den + bi, nx, poll);
      } else {
#endif
        for (std::size_t x = 0; x < nx; ++x) {
          if (!(x & 63))
            poll();
          const T w = weight(diff[y * nx + x], norm, h2, correction);
          if (w != 0) {
            if (qa) {
              b.output[ai + x] = std::fma(w, other[x], b.output[ai + x]);
              den[ai + x] += w;
            }
            if (pb) {
              b.output[bi + x] = std::fma(w, a[x], b.output[bi + x]);
              den[bi + x] += w;
            }
          }
        }
#if PS_FRQ_PACKETS
      }
#endif
    }
  };
  for (std::int64_t dy = 0; dy <= g.sy(); ++dy)
    for (std::int64_t dx = dy ? -g.sx() : 1; dx <= g.sx(); ++dx) {
      poll();
      const Rect domain{0, std::max<std::int64_t>(0, -dx), g.height - dy,
                        std::min(g.width, g.width - dx)};
      const Rect a = intersect(r, domain);
      const Rect bb =
          intersect({r.y0 - dy, r.x0 - dx, r.y1 - dy, r.x1 - dx}, domain);
      process(intersect(a, bb), dy, dx, true, true);
      difference(a, bb, [&](Rect z) { process(z, dy, dx, true, false); });
      difference(bb, a, [&](Rect z) { process(z, dy, dx, false, true); });
    }
  for (std::size_t k = 0; k < count; ++k) {
    if (!(k & 255))
      poll();
    b.output[k] /= den[k];
  }
}
}  // namespace ps::plugin_internal::restoration::nlm

#undef PS_NLM_TARGET
#undef PS_NLM_INLINE
