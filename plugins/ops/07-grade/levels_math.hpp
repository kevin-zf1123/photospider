// Copyright 2026 Photospider contributors
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "07-grade/affine_math.hpp"

namespace ps::plugin_internal::grade::levels {

// Controls belong to one invocation; dynamic payloads are never cached.
struct Control {
  double black = 0, white = 1, width = 1, inverse = 1;
  double power = 1, out0 = 0, out1 = 1, range = 1;
  bool copy = true, identity = true, fast = true;
  bool plain32 = false, folded32 = false;
  double slope = 1, offset = 0;
};
inline void prepare_linear32(Control* c, bool signed_levels);
inline Control make_control(double black, double white, double gamma,
                            double out0, double out1, bool signed_levels) {
  const double width = white - black;
  const double inverse = 1 / width;
  const double power = 1 / gamma;
  const double range = out1 - out0;
  const bool identity =
      black == 0 && white == 1 && gamma == 1 && out0 == 0 && out1 == 1;
  Control c{black,
            white,
            width,
            inverse,
            power,
            out0,
            out1,
            range,
            signed_levels && identity,
            identity,
            std::isnormal(width) && std::isnormal(inverse) &&
                std::isfinite(range) && power <= 64};
  prepare_linear32(&c, signed_levels);
  return c;
}
// Only omit endpoint selections when the Float32 rounding interval proves
// that the ordinary arithmetic already returns the exact endpoint bits.
inline bool plain_linear32(const Control& c) {
  const float lo = static_cast<float>(c.out0);
  const float mapped = static_cast<float>(std::fma(c.range, 0.0, c.out0));
  const float negative = static_cast<float>(std::fma(c.range, -0.0, c.out0));
  if (std::memcmp(&lo, &mapped, sizeof(lo)) != 0 ||
      std::memcmp(&lo, &negative, sizeof(lo)) != 0)
    return false;
  const float hi = static_cast<float>(c.out1);
  if (!std::isfinite(hi) || hi == 0)
    return false;
  const double below =
      std::nextafter(hi, -std::numeric_limits<float>::infinity());
  const double above =
      std::nextafter(hi, std::numeric_limits<float>::infinity());
  const double left = (static_cast<double>(hi) + below) * 0.5;
  const double right = (static_cast<double>(hi) + above) * 0.5;
  const double error = 16 * std::numeric_limits<double>::epsilon() *
                       (std::abs(c.range) + std::abs(c.out0) + 1.0);
  return c.out1 - left > error && right - c.out1 > error;
}
// These proofs depend only on this invocation's controls, not on a block.
inline void prepare_linear32(Control* c, bool signed_levels) {
  constexpr double boundary =
      static_cast<double>(std::numeric_limits<float>::max()) * (1.0 - 0x1p-24);
  if (c->identity || !c->fast || c->power != 1 || std::abs(c->out0) > 1.0 ||
      std::abs(c->out1) >= boundary)
    return;
  c->plain32 = plain_linear32(*c);
#if defined(__aarch64__)
  if (!signed_levels || !c->plain32 || std::abs(c->black) > 1.0 ||
      c->inverse > 1024.0 || std::abs(c->range) > 1024.0)
    return;
  c->slope = c->range * c->inverse;
  c->offset = std::fma(-c->black, c->slope, c->out0);
  const double error =
      64 * std::numeric_limits<double>::epsilon() *
      (std::abs(c->black * c->slope) + std::abs(c->white * c->slope) +
       std::abs(c->offset) + std::abs(c->range) + 1.0);
  const auto stable = [&](double endpoint) {
    const float t = static_cast<float>(endpoint);
    if (!std::isfinite(t) || t == 0)
      return false;
    const double l =
        (static_cast<double>(t) + std::nextafter(t, -INFINITY)) * 0.5;
    const double r =
        (static_cast<double>(t) + std::nextafter(t, INFINITY)) * 0.5;
    return endpoint - l > error && r - endpoint > error;
  };
  c->folded32 = stable(c->out0) && stable(c->out1);
#else
  static_cast<void>(signed_levels);
#endif
}
template <bool Signed>
inline double evaluate(double x, const Control& c) {
  if (c.copy || (c.identity && (Signed || (x >= 0 && x <= 1))))
    return x;
  const double u = (x - c.black) / c.width;
  if constexpr (Signed) {
    if (u == 0)
      return c.out0;
    if (u == 1)
      return c.out1;
  } else {
    if (u <= 0)
      return c.out0;
    if (u >= 1)
      return c.out1;
  }
  const double p =
      c.power == 1 ? u
                   : (Signed ? std::copysign(std::pow(std::abs(u), c.power), u)
                             : std::pow(u, c.power));
  return std::fma(c.range, p, c.out0);
}
template <class T, bool Signed>
inline void scalar(const T* source, T* target, const Control& c) {
  if (c.copy || (c.identity && (Signed || (*source >= 0 && *source <= 1))))
    std::memcpy(target, source, sizeof(T));
  else
    *target = static_cast<T>(evaluate<Signed>(*source, c));
}

#if defined(__aarch64__) || \
    (defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)))
#if defined(__aarch64__)
#define PS_GRD04_TARGET
#else
#define PS_GRD04_TARGET __attribute__((target("avx2,fma")))
#endif
namespace simd = affine;
using simd::Mask;
using simd::Packet;
constexpr unsigned kLanes = simd::kLanes;

PS_GRD04_TARGET inline Mask either(Mask a, Mask b) {
#if defined(__aarch64__)
  return vorrq_u64(a, b);
#else
  return _mm256_or_pd(a, b);
#endif
}
PS_GRD04_TARGET inline Mask equal(Packet a, Packet b) {
#if defined(__aarch64__)
  return vceqq_f64(a, b);
#else
  return _mm256_cmp_pd(a, b, _CMP_EQ_OQ);
#endif
}
PS_GRD04_TARGET inline Packet clamp_unit(Packet x) {
#if defined(__aarch64__)
  return vmaxq_f64(vdupq_n_f64(0.0), vminq_f64(vdupq_n_f64(1.0), x));
#else
  // Keep a NaN in the second operand of both x86 min/max instructions.
  return _mm256_max_pd(_mm256_setzero_pd(),
                       _mm256_min_pd(_mm256_set1_pd(1.0), x));
#endif
}
struct VectorControl {
  Packet black, white, inverse, power, out0, out1, range;
  Mask copy, identity;
};
template <unsigned C>
PS_GRD04_TARGET inline VectorControl packet_control(const Control* c,
                                                    unsigned phase) {
  double black[kLanes], white[kLanes], inverse[kLanes], power[kLanes];
  double out0[kLanes], out1[kLanes], range[kLanes];
  std::uint64_t copy[kLanes], identity[kLanes];
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
  for (unsigned j = 0; j < kLanes; ++j) {
    const auto& a = c[(phase + j) % C];
    black[j] = a.copy ? 0 : a.black;
    white[j] = a.copy ? 1 : a.white;
    inverse[j] = a.copy ? 1 : a.inverse;
    power[j] = a.copy ? 1 : a.power;
    out0[j] = a.copy ? 0 : a.out0;
    out1[j] = a.copy ? 1 : a.out1;
    range[j] = a.copy ? 1 : a.range;
    copy[j] = a.copy ? UINT64_MAX : 0;
    identity[j] = a.identity ? UINT64_MAX : 0;
  }
  return {simd::load(black), simd::load(white),     simd::load(inverse),
          simd::load(power), simd::load(out0),      simd::load(out1),
          simd::load(range), simd::mask_load(copy), simd::mask_load(identity)};
}

// Both output dtypes use the binary64 logarithm and exponential polynomials.
// Endpoint lanes enter the power approximation as one, then select exact
// endpoints. Exceptional ranges and severe output cancellation use libm.
template <class T, unsigned N, bool Signed, bool Linear>
PS_GRD04_TARGET __attribute__((always_inline)) inline bool packets(
    Packet* x, Packet* y, Mask* preserve, const VectorControl* c) {
  Packet u[N];
  Mask low[N], high[N];
  simd::VectorControl powers[N];
  auto safe = simd::ge(simd::splat(1), simd::splat(0));
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
  for (unsigned j = 0; j < N; ++j) {
    preserve[j] = c[j].copy;
    if constexpr (!Signed)
      preserve[j] = either(
          preserve[j], simd::both(c[j].identity,
                                  simd::both(simd::ge(x[j], simd::splat(0)),
                                             simd::le(x[j], simd::splat(1)))));
    const auto value = simd::choose(preserve[j], simd::splat(0.5), x[j]);
    u[j] = simd::mul(simd::sub(value, c[j].black), c[j].inverse);
    if constexpr (Signed) {
      low[j] = equal(u[j], simd::splat(0));
      high[j] = either(equal(u[j], simd::splat(1)), equal(value, c[j].white));
    } else {
      low[j] = simd::le(u[j], simd::splat(0));
      high[j] =
          either(simd::ge(u[j], simd::splat(1)), equal(value, c[j].white));
    }
    const auto endpoint = either(preserve[j], either(low[j], high[j]));
    y[j] = simd::choose(endpoint, simd::splat(1), simd::absolute(u[j]));
    safe = simd::both(
        safe,
        simd::both(
            simd::ge(y[j], simd::splat(std::numeric_limits<double>::min())),
            simd::le(y[j], simd::splat(std::numeric_limits<double>::max()))));
    powers[j].power = c[j].power;
  }
  if constexpr (!Linear) {
    if (!simd::all(safe))
      return false;
    simd::powers<double, N>(y, powers);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (unsigned j = 0; j < N; ++j)
      safe = simd::both(safe, simd::le(simd::absolute(y[j]), simd::splat(900)));
    if (!simd::all(safe))
      return false;
    simd::exponentials<double, N>(y);
  }
  constexpr double boundary =
      static_cast<double>(std::numeric_limits<T>::max()) * (1 - 0x1p-30);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
  for (unsigned j = 0; j < N; ++j) {
    auto p = Linear ? u[j] : y[j];
    if constexpr (Signed)
      if constexpr (!Linear)
        p = simd::sign_from(p, u[j]);
    if constexpr (!Linear)
      p = simd::choose(equal(c[j].power, simd::splat(1)), u[j], p);
    y[j] = simd::madd(c[j].range, p, c[j].out0);
    if constexpr (!Linear) {
      // Reserve one output unit for absolute error near cancellation, matching
      // the regular profile's abs(out0) <= 1 bound.
      const auto allowance = simd::add(simd::absolute(y[j]), simd::splat(1));
      safe = simd::both(
          safe, either(either(low[j], high[j]),
                       simd::le(simd::absolute(simd::mul(c[j].range, p)),
                                allowance)));
      safe = simd::both(
          safe, either(either(low[j], high[j]),
                       simd::le(simd::absolute(y[j]), simd::splat(boundary))));
    }
    y[j] = simd::choose(high[j], c[j].out1, y[j]);
    y[j] = simd::choose(low[j], c[j].out0, y[j]);
  }
  return Linear || simd::all(safe);
}
template <class T, unsigned C, bool Signed, bool Linear>
PS_GRD04_TARGET inline void vector_span(const T* source, T* target,
                                        std::size_t count,
                                        const Control* controls) {
  constexpr unsigned N = C == 3 ? 3 : 4;
  constexpr unsigned step = N * kLanes;
  VectorControl c[N];
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
  for (unsigned j = 0; j < N; ++j)
    c[j] = packet_control<C>(controls, j * kLanes);
  const auto n = count * C;
  std::size_t i = 0;
  for (; i + step <= n; i += step) {
    Packet x[N], y[N];
    Mask preserve[N];
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (unsigned j = 0; j < N; ++j)
      x[j] = simd::load(source + i + j * kLanes);
    if (packets<T, N, Signed, Linear>(x, y, preserve, c)) {
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
      for (unsigned j = 0; j < N; ++j)
        simd::save_preserving(target + i + j * kLanes, source + i + j * kLanes,
                              y[j], preserve[j]);
    } else {
      for (unsigned j = 0; j < step; ++j)
        scalar<T, Signed>(source + i + j, target + i + j,
                          controls[(i + j) % C]);
    }
  }
  for (; i < n; ++i)
    scalar<T, Signed>(source + i, target + i, controls[i % C]);
}

// A degree-six Taylor polynomial is sufficient at the Float32 output boundary;
// every intermediate remains binary64. Float64 keeps the degree-eight path.
template <class T, unsigned N>
PS_GRD04_TARGET __attribute__((always_inline)) inline void exponential(
    Packet* y) {
  if constexpr (sizeof(T) == 8) {
    simd::exponentials<double, N>(y);
  } else {
    Packet n[N], z[N], p[N];
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (unsigned j = 0; j < N; ++j) {
      n[j] = simd::rounded(y[j]);
      z[j] = simd::mul(simd::sub(y[j], n[j]), simd::splat(0.6931471805599453));
      p[j] = simd::splat(1.0 / 720.0);
    }
    constexpr double c[] = {1.0, 1.0, 0.5, 1.0 / 6.0, 1.0 / 24.0, 1.0 / 120.0};
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (int k = 5; k >= 0; --k)
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
      for (unsigned j = 0; j < N; ++j)
        p[j] = simd::madd(p[j], z[j], simd::splat(c[k]));
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (unsigned j = 0; j < N; ++j)
      y[j] = simd::mul(p[j], simd::power_of_two(n[j]));
  }
}
template <class T, unsigned N, bool Signed, bool Linear, bool Plain = false>
PS_GRD04_TARGET __attribute__((always_inline)) inline bool regular_packets(
    const Packet* x, Packet* y, const VectorControl* c) {
  Packet u[N], base[N];
  Mask endpoint[N];
  simd::VectorControl power[N];
  auto safe = simd::ge(simd::splat(1.0), simd::splat(0.0));
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
  for (unsigned j = 0; j < N; ++j) {
    u[j] = simd::mul(simd::sub(x[j], c[j].black), c[j].inverse);
    if constexpr (Linear) {
      const auto value = Plain && !Signed ? clamp_unit(u[j]) : u[j];
      y[j] = simd::madd(c[j].range, value, c[j].out0);
      if constexpr (Signed && !Plain) {
        y[j] = simd::choose(equal(u[j], simd::splat(0.0)), c[j].out0, y[j]);
        y[j] = simd::choose(
            either(equal(u[j], simd::splat(1.0)), equal(x[j], c[j].white)),
            c[j].out1, y[j]);
      } else if constexpr (!Signed && !Plain) {
        y[j] = simd::choose(simd::le(u[j], simd::splat(0.0)), c[j].out0, y[j]);
        y[j] = simd::choose(
            either(simd::ge(u[j], simd::splat(1.0)), equal(x[j], c[j].white)),
            c[j].out1, y[j]);
      }
    } else {
      if constexpr (Signed) {
        // Zero is uncommon in this path and is handled by the scalar fallback.
        base[j] = simd::absolute(u[j]);
        endpoint[j] =
            either(equal(u[j], simd::splat(1.0)), equal(x[j], c[j].white));
      } else {
        endpoint[j] = either(simd::le(u[j], simd::splat(0.0)),
                             simd::ge(u[j], simd::splat(1.0)));
        base[j] = simd::choose(endpoint[j], simd::splat(1.0), u[j]);
      }
      y[j] = base[j];
      safe = simd::both(
          safe,
          simd::both(
              simd::ge(y[j], simd::splat(std::numeric_limits<double>::min())),
              simd::le(y[j], simd::splat(std::numeric_limits<double>::max()))));
      power[j].power = c[j].power;
    }
  }
  if constexpr (Linear)
    return true;
  if (!simd::all(safe))
    return false;
  simd::powers<T, N>(y, power);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
  for (unsigned j = 0; j < N; ++j)
    safe = simd::both(safe, simd::le(simd::absolute(y[j]), simd::splat(900.0)));
  if (!simd::all(safe))
    return false;
  exponential<T, N>(y);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
  for (unsigned j = 0; j < N; ++j) {
    if constexpr (Signed)
      y[j] = simd::sign_from(y[j], u[j]);
    y[j] = simd::madd(c[j].range, y[j], c[j].out0);
    if constexpr (Signed) {
      y[j] = simd::choose(endpoint[j], c[j].out1, y[j]);
      constexpr double boundary =
          static_cast<double>(std::numeric_limits<T>::max()) * (1.0 - 0x1p-24);
      safe = simd::both(safe,
                        simd::le(simd::absolute(y[j]), simd::splat(boundary)));
    } else {
      y[j] = simd::choose(simd::le(u[j], simd::splat(0.0)), c[j].out0, y[j]);
      y[j] = simd::choose(
          either(simd::ge(u[j], simd::splat(1.0)), equal(x[j], c[j].white)),
          c[j].out1, y[j]);
    }
  }
  return simd::all(safe);
}
template <class T, unsigned C, bool Signed, bool Linear, bool Plain = false>
PS_GRD04_TARGET inline void regular_span(const T* source, T* target,
                                         std::size_t count,
                                         const Control* controls) {
#if defined(__aarch64__)
  constexpr unsigned N = 3;
#else
  constexpr unsigned N = C == 3 ? 3 : 4;
#endif
  VectorControl c[N];
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
  for (unsigned j = 0; j < N; ++j) {
#if defined(__aarch64__)
    c[j] = packet_control<1>(controls + (C == 1 ? 0 : j), 0);
#else
    c[j] = packet_control<C>(controls, j * kLanes);
#endif
  }
  std::size_t i = 0;
#if defined(__aarch64__)
  if constexpr (C == 1 && sizeof(T) == 4 && Linear && Signed && Plain) {
    const auto& a = controls[0];
    // The input guard keeps the folded binary64 affine away from overflow.
    if (a.folded32) {
      const auto factor = vdupq_n_f64(a.slope), offset = vdupq_n_f64(a.offset);
      for (; i + 32 <= count; i += 32) {
        float32x4_t input[8];
        auto peak = vdupq_n_f32(0.0F);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
        for (unsigned j = 0; j < 8; ++j) {
          input[j] = vld1q_f32(source + i + 4 * j);
          peak = vmaxq_f32(peak, vabsq_f32(input[j]));
        }
        if (vmaxvq_f32(peak) <= 0x1p100F) {
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
          for (unsigned j = 0; j < 8; ++j) {
            const auto low =
                vfmaq_f64(offset, vcvt_f64_f32(vget_low_f32(input[j])), factor);
            const auto high = vfmaq_f64(
                offset, vcvt_f64_f32(vget_high_f32(input[j])), factor);
            vst1q_f32(target + i + 4 * j,
                      vcombine_f32(vcvt_f32_f64(low), vcvt_f32_f64(high)));
          }
        } else {
          for (unsigned j = 0; j < 32; ++j)
            scalar<T, Signed>(source + i + j, target + i + j, a);
        }
      }
      for (; i < count; ++i)
        scalar<T, Signed>(source + i, target + i, a);
      return;
    }
  }
  if constexpr (C == 1 && sizeof(T) == 4 && Linear) {
    VectorControl gray[4] = {c[0], c[0], c[0], c[0]};
    for (; i + 8 <= count; i += 8) {
      const auto a = vld1q_f32(source + i), b = vld1q_f32(source + i + 4);
      Packet x[4] = {
          vcvt_f64_f32(vget_low_f32(a)), vcvt_f64_f32(vget_high_f32(a)),
          vcvt_f64_f32(vget_low_f32(b)), vcvt_f64_f32(vget_high_f32(b))};
      Packet y[4];
      regular_packets<T, 4, Signed, true, Plain>(x, y, gray);
      vst1q_f32(target + i,
                vcombine_f32(vcvt_f32_f64(y[0]), vcvt_f32_f64(y[1])));
      vst1q_f32(target + i + 4,
                vcombine_f32(vcvt_f32_f64(y[2]), vcvt_f32_f64(y[3])));
    }
    for (; i < count; ++i)
      scalar<T, Signed>(source + i, target + i, controls[0]);
    return;
  }
  if constexpr (C == 3 || C == 4) {
    for (; i + 2 <= count; i += 2) {
      Packet x[3], y[3];
      bool done = false;
      if constexpr (sizeof(T) == 4) {
        auto v = [&] {
          if constexpr (C == 3)
            return vld3_f32(source + C * i);
          else
            return vld4_f32(source + C * i);
        }();
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
        for (unsigned j = 0; j < 3; ++j)
          x[j] = vcvt_f64_f32(v.val[j]);
        if (regular_packets<T, 3, Signed, Linear, Plain>(x, y, c)) {
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
          for (unsigned j = 0; j < 3; ++j)
            v.val[j] = vcvt_f32_f64(y[j]);
          if constexpr (C == 3)
            vst3_f32(target + C * i, v);
          else
            vst4_f32(target + C * i, v);
          done = true;
        }
      } else {
        auto v = [&] {
          if constexpr (C == 3)
            return vld3q_f64(source + C * i);
          else
            return vld4q_f64(source + C * i);
        }();
        if (regular_packets<T, 3, Signed, Linear, Plain>(v.val, y, c)) {
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
          for (unsigned j = 0; j < 3; ++j)
            v.val[j] = y[j];
          if constexpr (C == 3)
            vst3q_f64(target + C * i, v);
          else
            vst4q_f64(target + C * i, v);
          done = true;
        }
      }
      if (!done) {
        for (unsigned j = 0; j < 2 * C; ++j)
          scalar<T, Signed>(source + C * i + j, target + C * i + j,
                            controls[j % C]);
      }
    }
    for (; i < count; ++i)
      for (unsigned j = 0; j < C; ++j)
        scalar<T, Signed>(source + C * i + j, target + C * i + j, controls[j]);
    return;
  }
#endif
  constexpr unsigned step = N * kLanes;
  const auto n = count * C;
  for (; i + step <= n; i += step) {
    Packet x[N], y[N];
#if defined(__x86_64__)
    std::conditional_t<sizeof(T) == 4, __m128, __m256d> original[N];
#endif
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (unsigned j = 0; j < N; ++j) {
#if defined(__x86_64__)
      if constexpr (C == 4) {
        if constexpr (sizeof(T) == 4) {
          original[j] = _mm_loadu_ps(source + i + j * kLanes);
          x[j] =
              _mm256_cvtps_pd(_mm_blend_ps(original[j], _mm_set1_ps(0.5F), 8));
        } else {
          original[j] = _mm256_loadu_pd(source + i + j * kLanes);
          x[j] = _mm256_blend_pd(original[j], _mm256_set1_pd(0.5), 8);
        }
      } else {
        x[j] = simd::load(source + i + j * kLanes);
      }
#else
      x[j] = simd::load(source + i + j * kLanes);
#endif
    }
    if (regular_packets<T, N, Signed, Linear, Plain>(x, y, c)) {
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
      for (unsigned j = 0; j < N; ++j) {
#if defined(__x86_64__)
        if constexpr (C == 4) {
          // Keep bypass bits in registers; no source reload after output
          // stores.
          if constexpr (sizeof(T) == 4)
            _mm_storeu_ps(target + i + j * kLanes,
                          _mm_blend_ps(_mm256_cvtpd_ps(y[j]), original[j], 8));
          else
            _mm256_storeu_pd(target + i + j * kLanes,
                             _mm256_blend_pd(y[j], original[j], 8));
        } else {
          simd::save(target + i + j * kLanes, y[j]);
        }
#else
        simd::save(target + i + j * kLanes, y[j]);
#endif
      }
    } else {
      for (unsigned j = 0; j < step; ++j)
        scalar<T, Signed>(source + i + j, target + i + j,
                          controls[(i + j) % C]);
    }
  }
  for (; i < n; ++i)
    scalar<T, Signed>(source + i, target + i, controls[i % C]);
}

#undef PS_GRD04_TARGET
#endif

// Disjoint contiguous spans of count complete C-channel pixels.
template <class T, unsigned C, bool Signed>
inline void span(const T* source, T* target, std::size_t count,
                 const Control* controls) {
  bool copy = true, fast = true, linear = true;
  for (unsigned c = 0; c < C; ++c) {
    copy &= controls[c].copy;
    fast &= controls[c].copy || controls[c].fast;
    linear &= controls[c].copy || controls[c].power == 1;
  }
  if (copy) {
    std::memcpy(target, source, count * C * sizeof(T));
    return;
  }
  bool regular = true, plain = sizeof(T) == 4 && linear;
  constexpr double boundary =
      static_cast<double>(std::numeric_limits<T>::max()) * (1.0 - 0x1p-24);
  for (unsigned c = 0; c < C; ++c) {
    const auto& a = controls[c];
    if (C == 4 && c == 3) {
      regular &= a.copy;
    } else {
      if (plain)
        plain = a.plain32;
      regular &= !a.copy && !a.identity && (linear || a.power != 1) &&
                 std::abs(a.out0) <= 1.0 && std::abs(a.out1) < boundary;
    }
  }
#if defined(__aarch64__)
  if (fast && regular) {
    if (plain)
      regular_span<T, C, Signed, true, true>(source, target, count, controls);
    else if (linear)
      regular_span<T, C, Signed, true>(source, target, count, controls);
    else
      regular_span<T, C, Signed, false>(source, target, count, controls);
    return;
  }
  if (fast) {
    if (linear)
      vector_span<T, C, Signed, true>(source, target, count, controls);
    else
      vector_span<T, C, Signed, false>(source, target, count, controls);
    return;
  }
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  static const bool avx2 =
      __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  if (avx2 && fast && regular) {
    if (plain)
      regular_span<T, C, Signed, true, true>(source, target, count, controls);
    else if (linear)
      regular_span<T, C, Signed, true>(source, target, count, controls);
    else
      regular_span<T, C, Signed, false>(source, target, count, controls);
    return;
  }
  if (avx2 && fast) {
    if (linear)
      vector_span<T, C, Signed, true>(source, target, count, controls);
    else
      vector_span<T, C, Signed, false>(source, target, count, controls);
    return;
  }
#endif
  for (std::size_t i = 0; i < count * C; ++i)
    scalar<T, Signed>(source + i, target + i, controls[i % C]);
}
}  // namespace ps::plugin_internal::grade::levels
