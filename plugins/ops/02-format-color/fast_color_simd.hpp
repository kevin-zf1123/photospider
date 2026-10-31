// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "01-numeric/fast_native_simd.hpp"

#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__)
#include <emmintrin.h>
#include <xmmintrin.h>
#endif

namespace ps::plugin_internal::fast_color {
#if defined(__aarch64__) || defined(__x86_64__)
template <class T>
struct Packet;
#if defined(__aarch64__)
template <>
struct Packet<float> {
  using V = float32x4_t;
  using Mask = uint32x4_t;
  static constexpr unsigned width = 4;
  static constexpr unsigned unroll = 4;
  static V set(float x) { return vdupq_n_f32(x); }
  static V add(V a, V b) { return vaddq_f32(a, b); }
  static V sub(V a, V b) { return vsubq_f32(a, b); }
  static V mul(V a, V b) { return vmulq_f32(a, b); }
  static V div(V a, V b) { return vdivq_f32(a, b); }
  static V fma(V a, V b, V c) { return vfmaq_f32(c, a, b); }
  static V min(V a, V b) { return vminq_f32(a, b); }
  static V max(V a, V b) { return vmaxq_f32(a, b); }
  static V abs(V a) { return vabsq_f32(a); }
  static Mask eq(V a, V b) { return native_simd::equal(a, b); }
  static Mask lt(V a, V b) { return native_simd::less(a, b); }
  static Mask ge(V a, V b) { return native_simd::greater_equal(a, b); }
  static V select(Mask m, V a, V b) { return vbslq_f32(m, a, b); }
  static bool all(Mask m) { return vminvq_u32(m) == UINT32_MAX; }
  static bool in_range(V h, V period) {
    const auto bad =
        vcgeq_u32(vreinterpretq_u32_f32(h), vreinterpretq_u32_f32(period));
    return vmaxvq_u32(bad) == 0;
  }
  static std::array<V, 3> load3(const float* p) {
    const auto v = vld3q_f32(p);
    return {v.val[0], v.val[1], v.val[2]};
  }
  static void store3(float* p, const std::array<V, 3>& v) {
    const float32x4x3_t result{{v[0], v[1], v[2]}};
    vst3q_f32(p, result);
  }
  static V load(const float* p) { return vld1q_f32(p); }
  static void store(float* p, V v) { vst1q_f32(p, v); }
};
template <>
struct Packet<double> {
  using V = float64x2_t;
  using Mask = uint64x2_t;
  static constexpr unsigned width = 2;
  static constexpr unsigned unroll = 4;
  static V set(double x) { return vdupq_n_f64(x); }
  static V add(V a, V b) { return vaddq_f64(a, b); }
  static V sub(V a, V b) { return vsubq_f64(a, b); }
  static V mul(V a, V b) { return vmulq_f64(a, b); }
  static V div(V a, V b) { return vdivq_f64(a, b); }
  static V fma(V a, V b, V c) { return vfmaq_f64(c, a, b); }
  static V min(V a, V b) { return vminq_f64(a, b); }
  static V max(V a, V b) { return vmaxq_f64(a, b); }
  static V abs(V a) { return vabsq_f64(a); }
  static Mask eq(V a, V b) { return native_simd::equal(a, b); }
  static Mask lt(V a, V b) { return native_simd::less(a, b); }
  static Mask ge(V a, V b) { return native_simd::greater_equal(a, b); }
  static V select(Mask m, V a, V b) { return vbslq_f64(m, a, b); }
  static bool all(Mask m) {
    return vgetq_lane_u64(m, 0) == UINT64_MAX &&
           vgetq_lane_u64(m, 1) == UINT64_MAX;
  }
  static bool in_range(V h, V period) {
    const auto bad =
        vcgeq_u64(vreinterpretq_u64_f64(h), vreinterpretq_u64_f64(period));
    return (vgetq_lane_u64(bad, 0) | vgetq_lane_u64(bad, 1)) == 0;
  }
  static std::array<V, 3> load3(const double* p) {
    const auto v = vld3q_f64(p);
    return {v.val[0], v.val[1], v.val[2]};
  }
  static void store3(double* p, const std::array<V, 3>& v) {
    const float64x2x3_t result{{v[0], v[1], v[2]}};
    vst3q_f64(p, result);
  }
  static V load(const double* p) { return vld1q_f64(p); }
  static void store(double* p, V v) { vst1q_f64(p, v); }
};
#else
template <>
struct Packet<double> {
  using V = __m128d;
  using Mask = V;
  static constexpr unsigned width = 2;
  static constexpr unsigned unroll = 4;
  static V set(double x) { return _mm_set1_pd(x); }
  static V add(V a, V b) { return _mm_add_pd(a, b); }
  static V sub(V a, V b) { return _mm_sub_pd(a, b); }
  static V mul(V a, V b) { return _mm_mul_pd(a, b); }
  static V div(V a, V b) { return _mm_div_pd(a, b); }
  static V fma(V a, V b, V c) { return add(mul(a, b), c); }
  static V min(V a, V b) { return _mm_min_pd(a, b); }
  static V max(V a, V b) { return _mm_max_pd(a, b); }
  static V abs(V a) { return _mm_andnot_pd(_mm_set1_pd(-0.0), a); }
  static Mask eq(V a, V b) { return _mm_cmpeq_pd(a, b); }
  static Mask lt(V a, V b) { return _mm_cmplt_pd(a, b); }
  static Mask ge(V a, V b) { return _mm_cmpge_pd(a, b); }
  static V select(Mask m, V a, V b) {
    return _mm_or_pd(_mm_and_pd(m, a), _mm_andnot_pd(m, b));
  }
  static bool all(Mask m) { return _mm_movemask_pd(m) == 3; }
  static bool in_range(V h, V period) {
    return all(_mm_and_pd(_mm_cmplt_pd(h, period), _mm_cmpge_pd(h, set(0))));
  }
  static std::array<V, 3> load3(const double* p) {
    const auto a = _mm_loadu_pd(p), b = _mm_loadu_pd(p + 2),
               c = _mm_loadu_pd(p + 4);
    return {_mm_shuffle_pd(a, b, 2), _mm_shuffle_pd(a, c, 1),
            _mm_shuffle_pd(b, c, 2)};
  }
  static void store3(double* p, const std::array<V, 3>& v) {
    _mm_storeu_pd(p, _mm_unpacklo_pd(v[0], v[1]));
    _mm_storeu_pd(p + 2, _mm_shuffle_pd(v[2], v[0], 2));
    _mm_storeu_pd(p + 4, _mm_unpackhi_pd(v[1], v[2]));
  }
  static V load(const double* p) { return _mm_loadu_pd(p); }
  static void store(double* p, V v) { _mm_storeu_pd(p, v); }
};
template <>
struct Packet<float> {
  using V = __m128;
  using Mask = V;
  static constexpr unsigned width = 4;
  static constexpr unsigned unroll = 4;
  static V set(float x) { return _mm_set1_ps(x); }
  static V add(V a, V b) { return _mm_add_ps(a, b); }
  static V sub(V a, V b) { return _mm_sub_ps(a, b); }
  static V mul(V a, V b) { return _mm_mul_ps(a, b); }
  static V div(V a, V b) { return _mm_div_ps(a, b); }
  static V fma(V a, V b, V c) { return add(mul(a, b), c); }
  static V min(V a, V b) { return _mm_min_ps(a, b); }
  static V max(V a, V b) { return _mm_max_ps(a, b); }
  static V abs(V a) { return _mm_andnot_ps(_mm_set1_ps(-0.0f), a); }
  static Mask eq(V a, V b) { return _mm_cmpeq_ps(a, b); }
  static Mask lt(V a, V b) { return _mm_cmplt_ps(a, b); }
  static Mask ge(V a, V b) { return _mm_cmpge_ps(a, b); }
  static V select(Mask m, V a, V b) {
    return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, b));
  }
  static bool all(Mask m) { return _mm_movemask_ps(m) == 15; }
  static bool in_range(V h, V period) {
    return all(_mm_and_ps(_mm_cmplt_ps(h, period), _mm_cmpge_ps(h, set(0))));
  }
  static std::array<V, 3> load3(const float* p) {
    const auto a = _mm_loadu_ps(p), b = _mm_loadu_ps(p + 4),
               c = _mm_loadu_ps(p + 8);
    const auto t0 = _mm_shuffle_ps(a, b, _MM_SHUFFLE(1, 0, 3, 2));
    const auto t1 = _mm_shuffle_ps(b, c, _MM_SHUFFLE(2, 1, 3, 2));
    const auto r = _mm_shuffle_ps(a, t1, _MM_SHUFFLE(2, 0, 3, 0));
    const auto g = _mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 0, 1, 1));
    const auto gg = _mm_shuffle_ps(g, t1, _MM_SHUFFLE(3, 1, 2, 0));
    const auto rr = _mm_shuffle_ps(r, r, _MM_SHUFFLE(3, 2, 1, 0));
    const auto bb = _mm_shuffle_ps(t0, c, _MM_SHUFFLE(3, 0, 3, 0));
    return {rr, gg, bb};
  }
  static void store3(float* p, const std::array<V, 3>& v) {
    const auto rg = _mm_unpacklo_ps(v[0], v[1]);
    const auto br = _mm_shuffle_ps(v[2], v[0], _MM_SHUFFLE(1, 0, 1, 0));
    _mm_storeu_ps(p, _mm_shuffle_ps(rg, br, _MM_SHUFFLE(3, 0, 1, 0)));
    const auto gb = _mm_unpacklo_ps(v[1], v[2]);
    const auto rgh = _mm_unpackhi_ps(v[0], v[1]);
    _mm_storeu_ps(p + 4, _mm_shuffle_ps(gb, rgh, _MM_SHUFFLE(1, 0, 3, 2)));
    const auto brh = _mm_unpackhi_ps(v[2], v[0]);
    const auto gbh = _mm_unpackhi_ps(v[1], v[2]);
    _mm_storeu_ps(p + 8, _mm_shuffle_ps(brh, gbh, _MM_SHUFFLE(3, 2, 3, 0)));
  }
  static V load(const float* p) { return _mm_loadu_ps(p); }
  static void store(float* p, V v) { _mm_storeu_ps(p, v); }
};
#endif
template <class T>
inline void copy_channel(const T* input, T* output, std::size_t count,
                         unsigned component) {
  std::size_t i = 0;
#if defined(__aarch64__)
  if constexpr (sizeof(T) == 8) {
    for (; i + 8 < count; i += 8) {
      const auto a = vld1q_f64(input + 3 * i + component);
      const auto b = vld1q_f64(input + 3 * (i + 1) + component);
      const auto c = vld1q_f64(input + 3 * (i + 2) + component);
      const auto d = vld1q_f64(input + 3 * (i + 3) + component);
      const auto e = vld1q_f64(input + 3 * (i + 4) + component);
      const auto f = vld1q_f64(input + 3 * (i + 5) + component);
      const auto g = vld1q_f64(input + 3 * (i + 6) + component);
      const auto h = vld1q_f64(input + 3 * (i + 7) + component);
      vst1q_f64(output + i, vuzp1q_f64(a, b));
      vst1q_f64(output + i + 2, vuzp1q_f64(c, d));
      vst1q_f64(output + i + 4, vuzp1q_f64(e, f));
      vst1q_f64(output + i + 6, vuzp1q_f64(g, h));
    }
  }
#endif
  for (; i < count; ++i) {
    std::memcpy(output + i, input + 3 * i + component, sizeof(T));
  }
}
template <class T, class S>
inline std::size_t packet_matrix_impl(const Matrix& matrix, const T* input,
                                      T* output, std::size_t count) {
  using V = typename S::V;
  std::array<std::array<V, 3>, 3> m;
  for (unsigned r = 0; r < 3; ++r) {
    for (unsigned c = 0; c < 3; ++c) {
      m[r][c] = S::set(T(matrix[r][c]));
    }
  }
  std::size_t i = 0;
  if constexpr (S::unroll >= 4) {
    for (; i + 4 * S::width <= count; i += 4 * S::width) {
      for (unsigned block = 0; block < 2; ++block) {
        const auto j = i + block * 2 * S::width;
        const auto x = S::load3(input + 3 * j);
        const auto next = S::load3(input + 3 * (j + S::width));
        std::array<V, 3> y, z;
        for (unsigned r = 0; r < 3; ++r) {
          y[r] = S::fma(m[r][2], x[2],
                        S::fma(m[r][1], x[1], S::mul(m[r][0], x[0])));
          z[r] = S::fma(m[r][2], next[2],
                        S::fma(m[r][1], next[1], S::mul(m[r][0], next[0])));
        }
        S::store3(output + 3 * j, y);
        S::store3(output + 3 * (j + S::width), z);
      }
    }
  }
  if constexpr (S::unroll >= 2) {
    for (; i + 2 * S::width <= count; i += 2 * S::width) {
      const auto x = S::load3(input + 3 * i);
      const auto next = S::load3(input + 3 * (i + S::width));
      std::array<V, 3> y, z;
      for (unsigned r = 0; r < 3; ++r) {
        y[r] =
            S::fma(m[r][2], x[2], S::fma(m[r][1], x[1], S::mul(m[r][0], x[0])));
        z[r] = S::fma(m[r][2], next[2],
                      S::fma(m[r][1], next[1], S::mul(m[r][0], next[0])));
      }
      S::store3(output + 3 * i, y);
      S::store3(output + 3 * (i + S::width), z);
    }
  }
  for (; i + S::width <= count; i += S::width) {
    const auto x = S::load3(input + 3 * i);
    std::array<V, 3> y;
    for (unsigned r = 0; r < 3; ++r) {
      y[r] =
          S::fma(m[r][2], x[2], S::fma(m[r][1], x[1], S::mul(m[r][0], x[0])));
    }
    S::store3(output + 3 * i, y);
  }
  return i;
}
template <class T>
inline std::size_t packet_matrix(const Matrix& m, const T* x, T* y,
                                 std::size_t n) {
  return packet_matrix_impl<T, Packet<T>>(m, x, y, n);
}
template <unsigned Kind, class T, class S>
bool packet_span_impl(const Model& model, const T* input, T* output,
                      std::size_t count, std::size_t ic, std::size_t oc,
                      const std::array<std::uint64_t, 3>& selected) {
  if constexpr (Kind < 8 || Kind > 19) {
    return false;
  }
  if (selected[0] != 0 || (ic >= 3 && (selected[1] != 1 || selected[2] != 2))) {
    return false;
  }
  if constexpr (Kind >= 8 && Kind <= 15) {
    if (ic != 3 || oc != 3) {
      return false;
    }
  } else if constexpr (Kind == 16) {
    if (ic != 3 || oc != 1) {
      return false;
    }
  } else if constexpr (Kind == 17) {
    if (ic != 1 || oc != 3) {
      return false;
    }
  } else {
    if (ic != 1 || oc != 1) {
      return false;
    }
  }
  using V = typename S::V;
  const auto zero = S::set(0), one = S::set(1), two = S::set(2),
             three = S::set(3), four = S::set(4), six = S::set(6);
  std::size_t i = 0;
  if constexpr (Kind == 18 || Kind == 19) {
    const auto constants = model.template levels<T>();
    if constexpr (Kind == 19) {
      std::uint64_t low_bits = 0, high_bits = 0;
      std::memcpy(&low_bits, &constants[0], sizeof(T));
      std::memcpy(&high_bits, &constants[1], sizeof(T));
      const auto one_bits =
          sizeof(T) == 4 ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000);
      if (low_bits == 0 && high_bits == one_bits) {
        // The binary domain is {+0,-0,+1}; clearing the sign gives +0/+1.
        for (; i + 8 * S::width <= count; i += 8 * S::width) {
          std::array<V, 8> values;
#pragma clang loop unroll(full)
          for (unsigned block = 0; block < 8; ++block) {
            values[block] = S::abs(S::load(input + i + block * S::width));
          }
#pragma clang loop unroll(full)
          for (unsigned block = 0; block < 8; ++block) {
            S::store(output + i + block * S::width, values[block]);
          }
        }
        for (; i + S::width <= count; i += S::width) {
          S::store(output + i, S::abs(S::load(input + i)));
        }
        for (; i < count; ++i) {
          output[i] = std::abs(input[i]);
        }
        return true;
      }
    }
    T threshold = static_cast<T>(model.threshold);
    if (static_cast<double>(threshold) < model.threshold) {
      threshold = std::nextafter(threshold, static_cast<T>(INFINITY));
    }
    const auto bound = S::set(threshold);
    const auto low = S::set(constants[0]), high = S::set(constants[1]);
    const auto evaluate = [&](V value) {
      if constexpr (Kind == 18) {
        return S::select(S::ge(value, bound), one, zero);
      } else {
        return S::select(S::eq(value, zero), low, high);
      }
    };
    for (; i + 8 * S::width <= count; i += 8 * S::width) {
      std::array<V, 8> values;
#pragma clang loop unroll(full)
      for (unsigned block = 0; block < 8; ++block) {
        values[block] = evaluate(S::load(input + i + block * S::width));
      }
#pragma clang loop unroll(full)
      for (unsigned block = 0; block < 8; ++block) {
        S::store(output + i + block * S::width, values[block]);
      }
    }
    for (; i + S::width <= count; i += S::width) {
      S::store(output + i, evaluate(S::load(input + i)));
    }
    for (; i < count; ++i) {
      if constexpr (Kind == 18) {
        output[i] = input[i] >= model.threshold ? T(1) : T(0);
      } else {
        std::memcpy(output + i, &constants[input[i] == 0 ? 0 : 1], sizeof(T));
      }
    }
    return true;
  }
  if constexpr (Kind == 12 || Kind == 13) {
    i = packet_matrix_impl<T, S>(model.first, input, output, count);
  } else {
    for (; i + S::width <= count; i += S::width) {
      const auto x = ic == 3 ? S::load3(input + i * ic)
                             : std::array<V, 3>{S::load(input + i), zero, zero};
      std::array<V, 3> y{};
      if constexpr (Kind == 8 || Kind == 10) {
        const auto hi = S::max(x[0], S::max(x[1], x[2]));
        const auto lo = S::min(x[0], S::min(x[1], x[2]));
        const auto d = S::sub(hi, lo);
        const auto gray = S::eq(d, zero);
        const auto safe = S::select(gray, one, d);
        const auto is_red = S::eq(hi, x[0]), is_green = S::eq(hi, x[1]);
        const auto numerator = S::select(
            is_red, S::sub(x[1], x[2]),
            S::select(is_green, S::sub(x[2], x[0]), S::sub(x[0], x[1])));
        const auto sector =
            S::select(is_red, zero, S::select(is_green, two, four));
        auto h = S::add(S::div(numerator, safe), sector);
        h = S::select(S::lt(h, zero), S::add(h, six), h);
        const auto light = S::mul(S::add(hi, lo), S::set(.5));
        auto den = hi;
        if constexpr (Kind == 8) {
          const auto stable = S::fma(two, S::min(lo, S::sub(one, hi)), d);
          const auto general =
              S::sub(one, S::abs(S::sub(S::mul(two, light), one)));
          den = S::select(S::lt(lo, zero), general,
                          S::select(S::lt(one, hi), general, stable));
        }
        y = {S::select(
                 gray, zero,
                 S::mul(h, S::set(T(model.output_pi ? 1.0 / 3 : kPi / 3)))),
             S::select(gray, zero, S::div(d, den)),
             Kind == 8 ? S::select(gray, hi, light) : hi};
      } else if constexpr (Kind == 9 || Kind == 11) {
        const auto period = S::set(T(model.input_pi ? 2 : 2 * kPi));
        if (!S::in_range(x[0], period)) {
          for (unsigned lane = 0; lane < S::width; ++lane) {
            const auto j = i + lane;
            const auto v = model.apply<Kind>(
                {input[3 * j], input[3 * j + 1], input[3 * j + 2]});
            for (unsigned c = 0; c < 3; ++c) {
              output[3 * j + c] = T(v[c]);
            }
          }
          continue;
        }
        const auto h = S::mul(x[0], S::set(T(model.input_pi ? 3 : 3 / kPi)));
        const auto chroma =
            Kind == 9
                ? S::mul(S::sub(one, S::abs(S::sub(S::mul(two, x[2]), one))),
                         x[1])
                : S::mul(x[2], x[1]);
        const auto base = Kind == 9 ? S::sub(x[2], S::mul(chroma, S::set(.5)))
                                    : S::sub(x[2], chroma);
        const auto r =
            S::min(one, S::max(zero, S::sub(S::abs(S::sub(h, three)), one)));
        const auto g =
            S::min(one, S::max(zero, S::sub(two, S::abs(S::sub(h, two)))));
        const auto b =
            S::min(one, S::max(zero, S::sub(two, S::abs(S::sub(h, four)))));
        y = {S::fma(chroma, r, base), S::fma(chroma, g, base),
             S::fma(chroma, b, base)};
        for (auto& v : y) {
          v = S::select(S::eq(x[1], zero), x[2], v);
        }
      } else if constexpr (Kind == 14) {
        const auto sum = S::add(S::add(x[0], x[1]), x[2]);
        y = {S::select(S::eq(sum, zero), S::set(T(model.white_xy[0])),
                       S::div(x[0], sum)),
             S::select(S::eq(sum, zero), S::set(T(model.white_xy[1])),
                       S::div(x[1], sum)),
             x[1]};
      } else if constexpr (Kind == 15) {
        const auto ratio = S::div(x[2], x[1]);
        y = {S::select(S::eq(x[2], zero), zero, S::mul(x[0], ratio)), x[2],
             S::select(S::eq(x[2], zero), zero,
                       S::mul(S::sub(S::sub(one, x[0]), x[1]), ratio))};
      } else if constexpr (Kind == 16) {
        y[0] = x[model.gray == 0 ? 1 : 0];
      } else if constexpr (Kind == 17) {
        y = {x[0], zero, zero};
        if (model.gray == 0) {
          y = {S::mul(x[0], S::set(T(model.white[0]))), x[0],
               S::mul(x[0], S::set(T(model.white[2])))};
        }
      } else if constexpr (Kind == 18) {
        T threshold = static_cast<T>(model.threshold);
        if (static_cast<double>(threshold) < model.threshold) {
          threshold = std::nextafter(threshold, static_cast<T>(INFINITY));
        }
        y[0] = S::select(S::ge(x[0], S::set(threshold)), one, zero);
      } else if constexpr (Kind == 19) {
        y[0] = S::select(S::eq(x[0], zero), S::set(T(model.black)),
                         S::set(T(model.bright)));
      }
      if (oc == 3) {
        S::store3(output + i * oc, y);
      } else {
        S::store(output + i, y[0]);
      }
    }
  }
  for (; i < count; ++i) {
    Triple x{};
    for (unsigned c = 0; c < (ic == 1 ? 1U : 3U); ++c) {
      x[c] = input[i * ic + c];
    }
    const auto y = model.apply<Kind>(x);
    for (unsigned c = 0; c < oc; ++c) {
      output[i * oc + c] = T(y[c]);
    }
  }
  return true;
}
template <unsigned Kind, class T>
bool packet_span(const Model& model, const T* x, T* y, std::size_t n,
                 std::size_t ic, std::size_t oc,
                 const std::array<std::uint64_t, 3>& selected) {
  return packet_span_impl<Kind, T, Packet<T>>(model, x, y, n, ic, oc, selected);
}
#else
template <class T>
inline void copy_channel(const T* input, T* output, std::size_t count,
                         unsigned component) {
  for (std::size_t i = 0; i < count; ++i) {
    std::memcpy(output + i, input + 3 * i + component, sizeof(T));
  }
}
template <class T>
inline std::size_t packet_matrix(const Matrix&, const T*, T*, std::size_t) {
  return 0;
}
template <unsigned Kind, class T>
bool packet_span(const Model&, const T*, T*, std::size_t, std::size_t,
                 std::size_t, const std::array<std::uint64_t, 3>&) {
  return false;
}
#endif
template <class T>
inline void matrix_span(const Matrix& matrix, const T* input, T* output,
                        std::size_t count) {
#if defined(__x86_64__)
  if (native_compute::available()) {
    native_compute::matrix(sizeof(T) == 4, matrix, input, output, count);
    return;
  }
#endif
  const auto begin = packet_matrix(matrix, input, output, count);
  for (std::size_t i = begin; i < count; ++i) {
    const auto v = multiply(
        matrix, Triple{input[3 * i], input[3 * i + 1], input[3 * i + 2]});
    for (unsigned c = 0; c < 3; ++c) {
      output[3 * i + c] = static_cast<T>(v[c]);
    }
  }
}
}  // namespace ps::plugin_internal::fast_color
