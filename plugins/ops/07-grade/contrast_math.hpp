// Copyright 2026 Photospider contributors
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "07-grade/affine_math.hpp"

namespace ps::plugin_internal::grade::contrast {
// Invocation-local binary64 controls. Identity and bypass preserve source bits.
struct Control {
  double contrast = 1;
  double pivot = 0.18;
  double log_bias = 0;
  double factor = 1;
  bool copy = true;
};
inline Control make_control(double contrast, double pivot,
                            bool logarithmic = true) {
  const double bias = logarithmic && contrast != 1 && pivot > 0
                          ? (1 - contrast) * std::log2(pivot)
                          : 0;
  return {contrast, pivot, bias,
          logarithmic ? std::pow(pivot, 1 - contrast) : 1, contrast == 1};
}
template <bool Logarithmic>
inline double evaluate(double x, const Control& c) {
  if (c.copy)
    return x;
  if constexpr (!Logarithmic) {
    if (c.contrast == 0)
      return c.pivot;
    return std::fma(c.contrast, x - c.pivot, c.pivot);
  } else {
    if (x == 0 || x == c.pivot)
      return x;
    // A normal power and factor avoid logarithmic loss at the output range
    // boundary. Unrepresentable intermediates continue through the ratio path.
    const double power = std::pow(x, c.contrast);
    if (std::isnormal(power) && std::isnormal(c.factor))
      return power * c.factor;
    const double ratio = x / c.pivot;
    const double logarithm =
        ratio >= 0.5 && ratio <= 2
            ? std::log1p((x - c.pivot) / c.pivot) * 1.4426950408889634
        : std::isnormal(ratio) ? std::log2(ratio)
                               : std::log2(x) - std::log2(c.pivot);
    const double exponent = c.contrast * logarithm;
    if (std::isnormal(ratio) && std::abs(exponent) < 900)
      return c.pivot * std::exp2(exponent);
    // Adding log(pivot) to the ratio exponent keeps intermediate overflow
    // separate from the final result, including extremely large contrast.
    return std::exp2(exponent + std::log2(c.pivot));
  }
}
template <class T, bool Logarithmic>
inline void scalar(const T* source, T* target, const Control& c) {
  if (c.copy)
    std::memcpy(target, source, sizeof(T));
  else
    *target = static_cast<T>(evaluate<Logarithmic>(*source, c));
}

#if defined(__aarch64__) || \
    (defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)))
#if defined(__aarch64__)
#define PS_GRD03_TARGET
#else
#define PS_GRD03_TARGET __attribute__((target("avx2,fma")))
#endif
namespace simd = affine;
using simd::Mask;
using simd::Packet;
constexpr unsigned kLanes = simd::kLanes;

struct VectorControl {
  Packet contrast, pivot, bias, factor;
  Mask copy, zero;
};
PS_GRD03_TARGET inline Mask either(Mask a, Mask b) {
#if defined(__aarch64__)
  return vorrq_u64(a, b);
#else
  return _mm256_or_pd(a, b);
#endif
}
PS_GRD03_TARGET inline Mask equal(Packet a, Packet b) {
  return simd::both(simd::ge(a, b), simd::le(a, b));
}
template <unsigned C>
PS_GRD03_TARGET inline VectorControl packet_control(const Control* c,
                                                    unsigned phase) {
  double contrast[kLanes], pivot[kLanes], bias[kLanes], factor[kLanes];
  std::uint64_t copy[kLanes], zero[kLanes];
  for (unsigned j = 0; j < kLanes; ++j) {
    const auto& a = c[(phase + j) % C];
    contrast[j] = a.copy ? 1 : a.contrast;
    pivot[j] = a.copy ? 1 : a.pivot;
    bias[j] = a.copy ? 0 : a.log_bias;
    factor[j] = a.copy ? 1 : a.factor;
    copy[j] = a.copy ? UINT64_MAX : 0;
    zero[j] = a.contrast == 0 ? UINT64_MAX : 0;
  }
  return {simd::load(contrast), simd::load(pivot),     simd::load(bias),
          simd::load(factor),   simd::mask_load(copy), simd::mask_load(zero)};
}

// Reuse the GRD02 binary64 range reduction, with a GRD03 log-axis offset.
// The final exponent incorporates the pivot, avoiding an overflowing power
// followed by a small scale. Exceptional exponent ranges use scalar libm.
template <class T, unsigned N>
PS_GRD03_TARGET inline bool logarithms(Packet* x, Packet* y, Mask* preserve,
                                       const VectorControl* c) {
  affine::VectorControl powers[N];
  auto safe = simd::ge(simd::splat(1), simd::splat(0));
  for (unsigned j = 0; j < N; ++j) {
    preserve[j] = either(c[j].copy, either(equal(x[j], simd::splat(0)),
                                           equal(x[j], c[j].pivot)));
    y[j] = simd::choose(preserve[j], simd::splat(1), x[j]);
    safe = simd::both(
        safe,
        simd::both(
            simd::ge(y[j], simd::splat(std::numeric_limits<double>::min())),
            simd::le(y[j], simd::splat(std::numeric_limits<double>::max()))));
    powers[j].power = c[j].contrast;
  }
  if (!simd::all(safe))
    return false;
  simd::powers<T, N>(y, powers);
  for (unsigned j = 0; j < N; ++j) {
    y[j] = simd::add(y[j], c[j].bias);
    safe = simd::both(safe, simd::le(simd::absolute(y[j]), simd::splat(900)));
  }
  if (!simd::all(safe))
    return false;
  simd::exponentials<T, N>(y);
  constexpr double boundary =
      static_cast<double>(std::numeric_limits<T>::max()) *
      (1.0 - (sizeof(T) == 4 ? 0x1p-16 : 0x1p-30));
  for (unsigned j = 0; j < N; ++j)
    safe = simd::both(safe, simd::le(y[j], simd::splat(boundary)));
  return simd::all(safe);
}
template <class T, unsigned N>
PS_GRD03_TARGET inline bool positive_powers(Packet* y, const VectorControl* c) {
  affine::VectorControl powers[N];
  auto safe = simd::ge(simd::splat(1), simd::splat(0));
  for (unsigned j = 0; j < N; ++j) {
    safe = simd::both(
        safe,
        simd::both(
            simd::ge(y[j], simd::splat(std::numeric_limits<double>::min())),
            simd::le(y[j], simd::splat(std::numeric_limits<double>::max()))));
    powers[j].power = c[j].contrast;
  }
  if (!simd::all(safe))
    return false;
  simd::powers<T, N>(y, powers);
  constexpr double upper = sizeof(T) == 4 ? 127.99997 : 1023.999999998;
  for (unsigned j = 0; j < N; ++j) {
    safe = simd::both(safe, simd::le(simd::absolute(y[j]), simd::splat(900)));
    safe = simd::both(safe,
                      simd::le(simd::add(y[j], c[j].bias), simd::splat(upper)));
  }
  if (!simd::all(safe))
    return false;
  simd::exponentials<T, N>(y);
  for (unsigned j = 0; j < N; ++j)
    y[j] = simd::mul(y[j], c[j].factor);
  return true;
}
template <class T, unsigned C, bool Logarithmic>
PS_GRD03_TARGET inline void regular_span(const T* source, T* target,
                                         std::size_t count,
                                         const Control* controls) {
#if defined(__aarch64__)
  constexpr unsigned N = C == 3 || Logarithmic ? 3 : 4;
#else
  constexpr unsigned N = C == 1 && Logarithmic ? 8 : (C == 3 ? 3 : 4);
#endif
  VectorControl c[N];
  for (unsigned j = 0; j < N; ++j)
    c[j] = packet_control<C>(controls, j * kLanes);
  constexpr unsigned step = N * kLanes;
  const auto n = count * C;
  std::size_t i = 0;
  for (; i + step <= n; i += step) {
    Packet y[N];
    for (unsigned j = 0; j < N; ++j) {
      const auto x = simd::load(source + i + j * kLanes);
      if constexpr (Logarithmic)
        y[j] = x;
      else
        y[j] = simd::madd(simd::sub(x, c[j].pivot), c[j].contrast, c[j].pivot);
    }
    bool accepted = true;
    if constexpr (Logarithmic)
      accepted = positive_powers<T, N>(y, c);
    if (accepted) {
      for (unsigned j = 0; j < N; ++j)
        simd::save(target + i + j * kLanes, y[j]);
    } else {
      for (unsigned j = 0; j < step; ++j)
        scalar<T, Logarithmic>(source + i + j, target + i + j,
                               controls[(i + j) % C]);
    }
  }
  for (; i < n; ++i)
    scalar<T, Logarithmic>(source + i, target + i, controls[i % C]);
}
#if defined(__aarch64__)
template <class T, unsigned C, bool Logarithmic>
inline void rgb_span(const T* source, T* target, std::size_t count,
                     const Control* controls) {
  VectorControl c[3];
  for (unsigned j = 0; j < 3; ++j)
    c[j] = packet_control<1>(controls + j, 0);
  std::size_t i = 0;
  for (; i + 2 <= count; i += 2) {
    Packet y[3];
    bool accepted = true;
    if constexpr (std::is_same_v<T, float>) {
      // vld3/vld4 keep the bypass component in its original storage bits.
      float32x2x4_t pixel{};
      if constexpr (C == 4) {
        pixel = vld4_f32(source + i * C);
      } else {
        const auto input = vld3_f32(source + i * C);
        for (unsigned j = 0; j < 3; ++j)
          pixel.val[j] = input.val[j];
      }
      for (unsigned j = 0; j < 3; ++j) {
        const auto x = vcvt_f64_f32(pixel.val[j]);
        if constexpr (Logarithmic)
          y[j] = x;
        else
          y[j] =
              simd::madd(simd::sub(x, c[j].pivot), c[j].contrast, c[j].pivot);
      }
      if constexpr (Logarithmic)
        accepted = positive_powers<T, 3>(y, c);
      if (accepted) {
        for (unsigned j = 0; j < 3; ++j)
          pixel.val[j] = vcvt_f32_f64(y[j]);
        if constexpr (C == 4) {
          vst4_f32(target + i * C, pixel);
        } else {
          float32x2x3_t output{{pixel.val[0], pixel.val[1], pixel.val[2]}};
          vst3_f32(target + i * C, output);
        }
      }
    } else {
      float64x2x4_t pixel{};
      if constexpr (C == 4) {
        pixel = vld4q_f64(source + i * C);
      } else {
        const auto input = vld3q_f64(source + i * C);
        for (unsigned j = 0; j < 3; ++j)
          pixel.val[j] = input.val[j];
      }
      for (unsigned j = 0; j < 3; ++j) {
        if constexpr (Logarithmic)
          y[j] = pixel.val[j];
        else
          y[j] = simd::madd(simd::sub(pixel.val[j], c[j].pivot), c[j].contrast,
                            c[j].pivot);
      }
      if constexpr (Logarithmic)
        accepted = positive_powers<T, 3>(y, c);
      if (accepted) {
        for (unsigned j = 0; j < 3; ++j)
          pixel.val[j] = y[j];
        if constexpr (C == 4) {
          vst4q_f64(target + i * C, pixel);
        } else {
          float64x2x3_t output{{pixel.val[0], pixel.val[1], pixel.val[2]}};
          vst3q_f64(target + i * C, output);
        }
      }
    }
    if (!accepted) {
      for (unsigned j = 0; j < 2 * C; ++j)
        scalar<T, Logarithmic>(source + i * C + j, target + i * C + j,
                               controls[j % C]);
    }
  }
  for (; i < count; ++i)
    for (unsigned j = 0; j < C; ++j)
      scalar<T, Logarithmic>(source + i * C + j, target + i * C + j,
                             controls[j]);
}
#endif

template <class T, unsigned C, bool Logarithmic>
PS_GRD03_TARGET inline void vector_span(const T* source, T* target,
                                        std::size_t count,
                                        const Control* controls) {
  bool regular = true;
  for (unsigned j = 0; j < C; ++j) {
    if (C == 4 && j == 3 && controls[j].copy)
      continue;
    regular &= !controls[j].copy && controls[j].contrast != 0;
    if constexpr (Logarithmic)
      regular &=
          controls[j].contrast <= 64 && std::isnormal(controls[j].factor);
  }
#if defined(__aarch64__)
  if constexpr (C == 3 || C == 4) {
    if (regular && (C == 3 || controls[3].copy)) {
      rgb_span<T, C, Logarithmic>(source, target, count, controls);
      return;
    }
  }
#endif
  if constexpr (C != 4) {
    if (regular) {
      regular_span<T, C, Logarithmic>(source, target, count, controls);
      return;
    }
  }
  constexpr unsigned kPackets = C == 3 ? 3 : (C == 1 && Logarithmic ? 2 : 4);
  VectorControl c[kPackets];
  bool fast = true, no_copy = true, no_zero = true;
  for (unsigned j = 0; j < kPackets; ++j)
    c[j] = packet_control<C>(controls, j * kLanes);
  for (unsigned j = 0; j < C; ++j) {
    fast &= controls[j].copy || controls[j].contrast <= 64;
    no_copy &= !controls[j].copy;
    no_zero &= controls[j].contrast != 0;
  }
  constexpr unsigned step = kLanes * kPackets;
  const auto n = count * C;
  std::size_t i = 0;
  for (; i + step <= n; i += step) {
    Packet x[kPackets], y[kPackets];
    Mask preserve[kPackets];
    for (unsigned j = 0; j < kPackets; ++j)
      x[j] = simd::load(source + i + j * kLanes);
    bool accepted = true;
    if constexpr (Logarithmic) {
      accepted = fast && logarithms<T, kPackets>(x, y, preserve, c);
    } else {
      for (unsigned j = 0; j < kPackets; ++j) {
        auto a = x[j];
        if (!no_copy || !no_zero)
          a = simd::choose(either(c[j].copy, c[j].zero), c[j].pivot, a);
        y[j] = simd::madd(simd::sub(a, c[j].pivot), c[j].contrast, c[j].pivot);
        if (!no_zero)
          y[j] = simd::choose(c[j].zero, c[j].pivot, y[j]);
        preserve[j] = c[j].copy;
      }
    }
    if (accepted) {
      for (unsigned j = 0; j < kPackets; ++j) {
        if constexpr (!Logarithmic) {
          if (no_copy) {
            simd::save(target + i + j * kLanes, y[j]);
            continue;
          }
        }
        simd::save_preserving(target + i + j * kLanes, source + i + j * kLanes,
                              y[j], preserve[j]);
      }
    } else {
      for (unsigned j = 0; j < step; ++j)
        scalar<T, Logarithmic>(source + i + j, target + i + j,
                               controls[(i + j) % C]);
    }
  }
  for (; i < n; ++i)
    scalar<T, Logarithmic>(source + i, target + i, controls[i % C]);
}
#undef PS_GRD03_TARGET
#endif

// Disjoint contiguous buffers, count complete pixels, C stored components.
// The runtime supplies bounded spans and preserves cancellation/work
// accounting.
template <class T, unsigned C, bool Logarithmic>
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
  vector_span<T, C, Logarithmic>(source, target, count, controls);
  return;
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  static const bool avx2 =
      __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  if (avx2) {
    vector_span<T, C, Logarithmic>(source, target, count, controls);
    return;
  }
#endif
  for (std::size_t i = 0; i < count * C; ++i)
    scalar<T, Logarithmic>(source + i, target + i, controls[i % C]);
}
}  // namespace ps::plugin_internal::grade::contrast
