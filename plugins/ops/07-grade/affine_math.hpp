// Copyright 2026 Photospider contributors
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "07-grade/affine_tables.hpp"

#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__)
#include <immintrin.h>
#endif

namespace ps::plugin_internal::grade::affine {

// Invocation-local controls. Identity rows bypass conversion and arithmetic.
struct Control {
  double scale = 1;
  double offset = 0;
  double power = 1;
  double gain = 1;
  bool copy = true;
};
inline Control affine_control(double gain, double offset) {
  return {gain, offset, 1, 1, gain == 1 && offset == 0};
}
inline Control lgg_control(double lift, double gamma, double gain) {
  return {1 - lift, lift, 1 / gamma, gain,
          lift == 0 && gamma == 1 && gain == 1};
}
template <bool Lgg>
inline double evaluate(double x, const Control& c) {
  const double t = std::fma(c.scale, x, c.offset);
  if constexpr (!Lgg)
    return t;
  const double p = t == 0 || c.power == 1
                       ? t
                       : std::copysign(std::pow(std::abs(t), c.power), t);
  return c.gain * p;
}
template <class T, bool Lgg>
inline void scalar(const T* source, T* target, const Control& c) {
  if (c.copy)
    std::memcpy(target, source, sizeof(T));
  else
    *target = static_cast<T>(evaluate<Lgg>(static_cast<double>(*source), c));
}

#if defined(__aarch64__) || \
    (defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)))
#if defined(__aarch64__)
#define PS_GRD02_TARGET
using Packet = float64x2_t;
using Mask = uint64x2_t;
constexpr unsigned kLanes = 2;
inline Packet splat(double x) {
  return vdupq_n_f64(x);
}
inline Packet add(Packet a, Packet b) {
  return vaddq_f64(a, b);
}
inline Packet sub(Packet a, Packet b) {
  return vsubq_f64(a, b);
}
inline Packet mul(Packet a, Packet b) {
  return vmulq_f64(a, b);
}
inline Packet madd(Packet a, Packet b, Packet c) {
  return vfmaq_f64(c, a, b);
}
inline Packet absolute(Packet a) {
  return vabsq_f64(a);
}
inline Mask ge(Packet a, Packet b) {
  return vcgeq_f64(a, b);
}
inline Mask le(Packet a, Packet b) {
  return vcleq_f64(a, b);
}
inline Mask both(Mask a, Mask b) {
  return vandq_u64(a, b);
}
inline Packet choose(Mask m, Packet a, Packet b) {
  return vbslq_f64(m, a, b);
}
inline bool all(Mask m) {
  return vgetq_lane_u64(m, 0) && vgetq_lane_u64(m, 1);
}
inline Packet mantissa(Packet x) {
  return vreinterpretq_f64_u64(
      vorrq_u64(vandq_u64(vreinterpretq_u64_f64(x),
                          vdupq_n_u64(UINT64_C(0x000fffffffffffff))),
                vdupq_n_u64(UINT64_C(0x3ff0000000000000))));
}
inline Packet exponent(Packet x) {
  return sub(vcvtq_f64_u64(vshrq_n_u64(vreinterpretq_u64_f64(x), 52)),
             splat(1023));
}
inline Packet rounded(Packet x) {
  return vrndnq_f64(x);
}
inline Packet power_of_two(Packet x) {
  return vreinterpretq_f64_s64(
      vshlq_n_s64(vaddq_s64(vcvtq_s64_f64(x), vdupq_n_s64(1023)), 52));
}
inline Packet sign_from(Packet magnitude, Packet x) {
  return vreinterpretq_f64_u64(
      vorrq_u64(vreinterpretq_u64_f64(magnitude),
                vandq_u64(vreinterpretq_u64_f64(x),
                          vdupq_n_u64(UINT64_C(0x8000000000000000)))));
}
template <class T>
inline Packet load(const T* p) {
  if constexpr (std::is_same_v<T, float>)
    return vcvt_f64_f32(vld1_f32(p));
  else
    return vld1q_f64(p);
}
template <class T>
inline void save(T* p, Packet x) {
  if constexpr (std::is_same_v<T, float>)
    vst1_f32(p, vcvt_f32_f64(x));
  else
    vst1q_f64(p, x);
}
inline Mask mask_load(const std::uint64_t* p) {
  return vld1q_u64(p);
}
template <class T>
inline void save_preserving(T* p, const T* source, Packet x, Mask copy) {
  if constexpr (std::is_same_v<T, float>)
    vst1_f32(p, vbsl_f32(vmovn_u64(copy), vld1_f32(source), vcvt_f32_f64(x)));
  else
    vst1q_f64(p, vbslq_f64(copy, vld1q_f64(source), x));
}
#else
#define PS_GRD02_TARGET __attribute__((target("avx2,fma")))
using Packet = __m256d;
using Mask = __m256d;
constexpr unsigned kLanes = 4;
PS_GRD02_TARGET inline Packet splat(double x) {
  return _mm256_set1_pd(x);
}
PS_GRD02_TARGET inline Packet add(Packet a, Packet b) {
  return _mm256_add_pd(a, b);
}
PS_GRD02_TARGET inline Packet sub(Packet a, Packet b) {
  return _mm256_sub_pd(a, b);
}
PS_GRD02_TARGET inline Packet mul(Packet a, Packet b) {
  return _mm256_mul_pd(a, b);
}
PS_GRD02_TARGET inline Packet madd(Packet a, Packet b, Packet c) {
  return _mm256_fmadd_pd(a, b, c);
}
PS_GRD02_TARGET inline Packet absolute(Packet a) {
  return _mm256_andnot_pd(splat(-0.0), a);
}
PS_GRD02_TARGET inline Mask ge(Packet a, Packet b) {
  return _mm256_cmp_pd(a, b, _CMP_GE_OQ);
}
PS_GRD02_TARGET inline Mask le(Packet a, Packet b) {
  return _mm256_cmp_pd(a, b, _CMP_LE_OQ);
}
PS_GRD02_TARGET inline Mask both(Mask a, Mask b) {
  return _mm256_and_pd(a, b);
}
PS_GRD02_TARGET inline Packet choose(Mask m, Packet a, Packet b) {
  return _mm256_blendv_pd(b, a, m);
}
PS_GRD02_TARGET inline bool all(Mask m) {
  return _mm256_movemask_pd(m) == 15;
}
PS_GRD02_TARGET inline Packet mantissa(Packet x) {
  return _mm256_castsi256_pd(_mm256_or_si256(
      _mm256_and_si256(_mm256_castpd_si256(x),
                       _mm256_set1_epi64x(INT64_C(0x000fffffffffffff))),
      _mm256_set1_epi64x(INT64_C(0x3ff0000000000000))));
}
PS_GRD02_TARGET inline Packet exponent(Packet x) {
  const auto e = _mm256_srli_epi64(_mm256_castpd_si256(x), 52);
  const auto packed =
      _mm256_permutevar8x32_epi32(e, _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6));
  return sub(_mm256_cvtepi32_pd(_mm256_castsi256_si128(packed)), splat(1023));
}
PS_GRD02_TARGET inline Packet rounded(Packet x) {
  return _mm256_round_pd(x, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
}
PS_GRD02_TARGET inline Packet power_of_two(Packet x) {
  const auto n = _mm256_cvtepi32_epi64(_mm256_cvttpd_epi32(x));
  return _mm256_castsi256_pd(
      _mm256_slli_epi64(_mm256_add_epi64(n, _mm256_set1_epi64x(1023)), 52));
}
PS_GRD02_TARGET inline Packet sign_from(Packet magnitude, Packet x) {
  return _mm256_or_pd(magnitude, _mm256_and_pd(x, splat(-0.0)));
}
template <class T>
PS_GRD02_TARGET inline Packet load(const T* p) {
  if constexpr (std::is_same_v<T, float>)
    return _mm256_cvtps_pd(_mm_loadu_ps(p));
  else
    return _mm256_loadu_pd(p);
}
template <class T>
PS_GRD02_TARGET inline void save(T* p, Packet x) {
  if constexpr (std::is_same_v<T, float>)
    _mm_storeu_ps(p, _mm256_cvtpd_ps(x));
  else
    _mm256_storeu_pd(p, x);
}
PS_GRD02_TARGET inline Mask mask_load(const std::uint64_t* p) {
  return _mm256_castsi256_pd(
      _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)));
}
template <class T>
PS_GRD02_TARGET inline void save_preserving(T* p, const T* source, Packet x,
                                            Mask copy) {
  if constexpr (std::is_same_v<T, float>) {
    const auto k =
        _mm_castsi128_ps(_mm256_castsi256_si128(_mm256_permutevar8x32_epi32(
            _mm256_castpd_si256(copy),
            _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6))));
    _mm_storeu_ps(p,
                  _mm_blendv_ps(_mm256_cvtpd_ps(x), _mm_loadu_ps(source), k));
  } else {
    _mm256_storeu_pd(p, _mm256_blendv_pd(x, _mm256_loadu_pd(source), copy));
  }
}
#endif

struct VectorControl {
  Packet scale, offset, power, gain;
  Mask copy;
  bool linear, fast;
};
template <unsigned C>
PS_GRD02_TARGET inline VectorControl packet_control(const Control* controls,
                                                    unsigned phase) {
  double s[kLanes], o[kLanes], p[kLanes], g[kLanes];
  std::uint64_t copy[kLanes];
  bool linear = true, fast = true;
  for (unsigned j = 0; j < kLanes; ++j) {
    const auto& c = controls[(phase + j) % C];
    s[j] = c.copy ? 1 : c.scale;
    o[j] = c.copy ? 0 : c.offset;
    p[j] = c.copy ? 1 : c.power;
    g[j] = c.copy ? 1 : c.gain;
    copy[j] = c.copy ? UINT64_MAX : 0;
    linear &= c.copy || c.power == 1;
    fast &= c.copy || c.power <= 64;
  }
  return {load(s), load(o), load(p), load(g), mask_load(copy), linear, fast};
}
struct LogPair {
  Packet inverse, logarithm;
};
PS_GRD02_TARGET inline LogPair log_lookup(Packet x) {
#if defined(__aarch64__)
  const auto index =
      vandq_u64(vshrq_n_u64(vreinterpretq_u64_f64(x), 45), vdupq_n_u64(127));
  const auto a = vld1q_f64(kLogTable + 2 * vgetq_lane_u64(index, 0));
  const auto b = vld1q_f64(kLogTable + 2 * vgetq_lane_u64(index, 1));
  return {vtrn1q_f64(a, b), vtrn2q_f64(a, b)};
#else
  const auto raw = _mm256_and_si256(
      _mm256_srli_epi64(_mm256_castpd_si256(x), 44), _mm256_set1_epi64x(254));
  const auto packed = _mm256_permutevar8x32_epi32(
      raw, _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6));
  const auto index = _mm256_castsi256_si128(packed);
  return {_mm256_i32gather_pd(kLogTable, index, 8),
          _mm256_i32gather_pd(kLogTable + 1, index, 8)};
#endif
}
// Independent packet chains hide the FMA latency. A 128-cell range
// reduction bounds |r| below 1/256; the log1p polynomial uses 3/5 terms.
template <class T, unsigned N>
PS_GRD02_TARGET inline void powers(Packet* value,
                                   const VectorControl* controls) {
  Packet z[N], e[N], p[N];
  for (unsigned j = 0; j < N; ++j) {
    const auto m = mantissa(value[j]);
    const auto table = log_lookup(m);
    e[j] = add(exponent(value[j]), table.logarithm);
    z[j] = madd(m, table.inverse, splat(-1));
  }
  constexpr double lc[] = {1, -0.5, 1.0 / 3, -0.25, 0.2};
  constexpr int ld = sizeof(T) == 4 ? 2 : 4;
  for (unsigned j = 0; j < N; ++j)
    p[j] = splat(lc[ld]);
  for (int k = ld - 1; k >= 0; --k)
    for (unsigned j = 0; j < N; ++j)
      p[j] = madd(p[j], z[j], splat(lc[k]));
  for (unsigned j = 0; j < N; ++j)
    value[j] = mul(madd(mul(z[j], splat(1.4426950408889634)), p[j], e[j]),
                   controls[j].power);
}
// Chebyshev interpolation of exp on [-ln(2)/2, ln(2)/2]. The
// degree-4/8 interpolation remainder is below 5.3e-6/2e-12 relative;
// coefficients and every operation remain binary64. The output boundary
// guards below exceed these errors before deciding finite versus overflow.
template <class T, unsigned N>
PS_GRD02_TARGET inline void exponentials(Packet* value) {
  Packet z[N], n[N], p[N];
  for (unsigned j = 0; j < N; ++j) {
    n[j] = rounded(value[j]);
    z[j] = mul(sub(value[j], n[j]), splat(0.6931471805599453));
  }
  constexpr double c4[] = {1, 0.9999622946507601, 0.4999937213862748,
                           0.16792143016520938, 0.041875644452153644};
  constexpr double c8[] = {1,
                           0.9999999999797852,
                           0.49999999999797934,
                           0.16666666891045775,
                           0.041666666890957,
                           0.008333266097949614,
                           0.0013888821677630362,
                           0.00019915866926782682,
                           2.4876164022625967e-05};
  constexpr int degree = sizeof(T) == 4 ? 4 : 8;
  const auto* c = sizeof(T) == 4 ? c4 : c8;
  for (unsigned j = 0; j < N; ++j)
    p[j] = splat(c[degree]);
  for (int k = degree - 1; k >= 0; --k)
    for (unsigned j = 0; j < N; ++j)
      p[j] = madd(p[j], z[j], splat(c[k]));
  for (unsigned j = 0; j < N; ++j)
    value[j] = mul(p[j], power_of_two(n[j]));
}
template <class T, unsigned C, bool Lgg>
PS_GRD02_TARGET inline void vector_span(const T* source, T* target,
                                        std::size_t count,
                                        const Control* controls) {
  constexpr unsigned kPackets = C == 3 ? 3 : (C == 1 && Lgg ? 2 : 4);
  VectorControl v[kPackets];
  bool linear = true, fast = true, no_copy = true;
  for (unsigned j = 0; j < kPackets; ++j) {
    v[j] = packet_control<C>(controls, j * kLanes);
    linear &= v[j].linear;
    fast &= v[j].fast;
  }
  for (unsigned c = 0; c < C; ++c)
    no_copy &= !controls[c].copy;
  constexpr unsigned step = kLanes * kPackets;
  const auto n = count * C;
  std::size_t i = 0;
  for (; i + step <= n; i += step) {
    Packet t[kPackets], y[kPackets];
    auto safe = ge(splat(1), splat(0));
    for (unsigned j = 0; j < kPackets; ++j) {
      auto x = load(source + i + j * kLanes);
      if (!no_copy)
        x = choose(v[j].copy, splat(1), x);
      t[j] = madd(x, v[j].scale, v[j].offset);
      y[j] = t[j];
    }
    bool accepted = true;
    if constexpr (Lgg) {
      if (!linear) {
        for (unsigned j = 0; j < kPackets; ++j) {
          y[j] = absolute(t[j]);
          safe = both(
              safe, both(ge(y[j], splat(std::numeric_limits<double>::min())),
                         le(y[j], splat(std::numeric_limits<double>::max()))));
        }
        accepted = fast && all(safe);
        if (accepted) {
          powers<T, kPackets>(y, v);
          for (unsigned j = 0; j < kPackets; ++j)
            safe = both(safe, le(absolute(y[j]), splat(900)));
          accepted = all(safe);
          if (accepted) {
            exponentials<T, kPackets>(y);
            for (unsigned j = 0; j < kPackets; ++j)
              y[j] = choose(
                  both(ge(v[j].power, splat(1)), le(v[j].power, splat(1))),
                  t[j], sign_from(y[j], t[j]));
          }
        }
      }
      constexpr double boundary =
          static_cast<double>(std::numeric_limits<T>::max()) *
          (1.0 - (sizeof(T) == 4 ? 0x1p-16 : 0x1p-30));
      for (unsigned j = 0; j < kPackets; ++j) {
        y[j] = mul(y[j], v[j].gain);
        safe = both(safe, le(absolute(y[j]), splat(boundary)));
      }
      accepted &= all(safe);
    }
    if (accepted) {
      for (unsigned j = 0; j < kPackets; ++j) {
        if (no_copy)
          save(target + i + j * kLanes, y[j]);
        else
          save_preserving(target + i + j * kLanes, source + i + j * kLanes,
                          y[j], v[j].copy);
      }
    } else {
      for (unsigned j = 0; j < step; ++j)
        scalar<T, Lgg>(source + i + j, target + i + j, controls[(i + j) % C]);
    }
  }
  for (; i < n; ++i)
    scalar<T, Lgg>(source + i, target + i, controls[i % C]);
}
#if defined(__aarch64__)
// Two RGBA pixels stay deinterleaved; alpha never enters arithmetic.
template <class T>
inline void rgba_affine_span(const T* source, T* target, std::size_t count,
                             const Control* controls) {
  Packet scales[3], offsets[3];
  for (unsigned c = 0; c < 3; ++c) {
    scales[c] = splat(controls[c].scale);
    offsets[c] = splat(controls[c].offset);
  }
  std::size_t i = 0;
  for (; i + 2 <= count; i += 2) {
    if constexpr (std::is_same_v<T, float>) {
      auto pixels = vld4_f32(source + 4 * i);
      for (unsigned c = 0; c < 3; ++c)
        pixels.val[c] = vcvt_f32_f64(
            madd(vcvt_f64_f32(pixels.val[c]), scales[c], offsets[c]));
      vst4_f32(target + 4 * i, pixels);
    } else {
      auto pixels = vld4q_f64(source + 4 * i);
      for (unsigned c = 0; c < 3; ++c)
        pixels.val[c] = madd(pixels.val[c], scales[c], offsets[c]);
      vst4q_f64(target + 4 * i, pixels);
    }
  }
  for (; i < count; ++i)
    for (unsigned c = 0; c < 4; ++c)
      scalar<T, false>(source + 4 * i + c, target + 4 * i + c, controls[c]);
}
// Struct loads collect two pixels into three useful color packets. The
// fourth component stays in its original representation throughout.
template <class T>
inline bool rgb_pair(const Packet* x, Packet* y, const VectorControl* v,
                     bool linear, bool fast) {
  Packet t[3];
  auto safe = ge(splat(1), splat(0));
  for (unsigned j = 0; j < 3; ++j) {
    t[j] = madd(x[j], v[j].scale, v[j].offset);
    y[j] = t[j];
  }
  if (!linear) {
    if (!fast)
      return false;
    for (unsigned j = 0; j < 3; ++j) {
      y[j] = absolute(t[j]);
      safe =
          both(safe, both(ge(y[j], splat(std::numeric_limits<double>::min())),
                          le(y[j], splat(std::numeric_limits<double>::max()))));
    }
    if (!all(safe))
      return false;
    powers<T, 3>(y, v);
    for (unsigned j = 0; j < 3; ++j)
      safe = both(safe, le(absolute(y[j]), splat(900)));
    if (!all(safe))
      return false;
    exponentials<T, 3>(y);
    for (unsigned j = 0; j < 3; ++j)
      y[j] = choose(both(ge(v[j].power, splat(1)), le(v[j].power, splat(1))),
                    t[j], sign_from(y[j], t[j]));
  }
  constexpr double boundary =
      static_cast<double>(std::numeric_limits<T>::max()) *
      (1.0 - (sizeof(T) == 4 ? 0x1p-16 : 0x1p-30));
  for (unsigned j = 0; j < 3; ++j) {
    y[j] = mul(y[j], v[j].gain);
    safe = both(safe, le(absolute(y[j]), splat(boundary)));
  }
  return all(safe);
}
template <class T, unsigned C>
inline void rgb_span(const T* source, T* target, std::size_t count,
                     const Control* controls) {
  VectorControl v[3];
  bool linear = true, fast = true;
  for (unsigned c = 0; c < 3; ++c) {
    v[c] = packet_control<1>(controls + c, 0);
    linear &= v[c].linear;
    fast &= v[c].fast;
  }
  std::size_t i = 0;
  for (; i + 2 <= count; i += 2) {
    Packet x[3], y[3];
    bool done = false;
    if constexpr (std::is_same_v<T, float>) {
      if constexpr (C == 4) {
        auto pixels = vld4_f32(source + C * i);
        for (unsigned c = 0; c < 3; ++c)
          x[c] = vcvt_f64_f32(pixels.val[c]);
        if (rgb_pair<T>(x, y, v, linear, fast)) {
          for (unsigned c = 0; c < 3; ++c)
            pixels.val[c] = vcvt_f32_f64(y[c]);
          vst4_f32(target + C * i, pixels);
          done = true;
        }
      } else {
        auto pixels = vld3_f32(source + C * i);
        for (unsigned c = 0; c < 3; ++c)
          x[c] = vcvt_f64_f32(pixels.val[c]);
        if (rgb_pair<T>(x, y, v, linear, fast)) {
          for (unsigned c = 0; c < 3; ++c)
            pixels.val[c] = vcvt_f32_f64(y[c]);
          vst3_f32(target + C * i, pixels);
          done = true;
        }
      }
    } else {
      if constexpr (C == 4) {
        auto pixels = vld4q_f64(source + C * i);
        if (rgb_pair<T>(pixels.val, y, v, linear, fast)) {
          for (unsigned c = 0; c < 3; ++c)
            pixels.val[c] = y[c];
          vst4q_f64(target + C * i, pixels);
          done = true;
        }
      } else {
        auto pixels = vld3q_f64(source + C * i);
        if (rgb_pair<T>(pixels.val, y, v, linear, fast)) {
          for (unsigned c = 0; c < 3; ++c)
            pixels.val[c] = y[c];
          vst3q_f64(target + C * i, pixels);
          done = true;
        }
      }
    }
    if (!done) {
      for (unsigned j = 0; j < 2 * C; ++j)
        scalar<T, true>(source + C * i + j, target + C * i + j,
                        controls[j % C]);
    }
  }
  for (; i < count; ++i)
    for (unsigned c = 0; c < C; ++c)
      scalar<T, true>(source + C * i + c, target + C * i + c, controls[c]);
}
#endif
#undef PS_GRD02_TARGET
#endif

// Disjoint contiguous buffers, C stored channels, count complete pixels.
// Controls are read anew by each invocation; there is no payload-derived cache.
template <class T, unsigned C, bool Lgg>
inline void span(const T* source, T* target, std::size_t count,
                 const Control* controls) {
  bool copy = true;
  for (unsigned c = 0; c < C; ++c)
    copy &= controls[c].copy;
  if (copy) {
    std::memcpy(target, source, count * C * sizeof(T));
    return;
  }
#if defined(__aarch64__)
  if constexpr (!Lgg && C == 4) {
    if (!controls[0].copy && !controls[1].copy && !controls[2].copy &&
        controls[3].copy) {
      rgba_affine_span(source, target, count, controls);
      return;
    }
  }
  if constexpr (Lgg && (C == 3 || C == 4)) {
    if (!controls[0].copy && !controls[1].copy && !controls[2].copy &&
        (C == 3 || controls[3].copy)) {
      rgb_span<T, C>(source, target, count, controls);
      return;
    }
  }
  vector_span<T, C, Lgg>(source, target, count, controls);
  return;
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  static const bool avx2 =
      __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  if (avx2) {
    vector_span<T, C, Lgg>(source, target, count, controls);
    return;
  }
#endif
  for (std::size_t i = 0; i < count * C; ++i)
    scalar<T, Lgg>(source + i, target + i, controls[i % C]);
}
}  // namespace ps::plugin_internal::grade::affine
