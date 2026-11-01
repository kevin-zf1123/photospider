// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "01-numeric/fast_native_simd.hpp"
#include "07-grade/affine_math.hpp"

namespace ps::plugin_internal::grade::coordinates {
inline constexpr std::size_t kDirectBlock = 4096;
inline constexpr std::size_t kStreamBlock = 65536;
inline constexpr std::uint64_t kStreamFootprint = UINT64_C(32) << 20;
/** Native coordinate arithmetic; controls are dynamic Float64 operands. */
enum class Model { Hsl, Hsv, Cielch, Oklch, Lab, Oklab };
enum class Action { Copy, Add, Multiply, ClampAdd, ClampMultiply, Hue, Affine };
struct Control {
  Action action = Action::Copy;
  double scale = 1, offset = 0;
};
inline bool cartesian(Model model) {
  return model == Model::Lab || model == Model::Oklab;
}
inline unsigned hue_component(Model model) {
  return model == Model::Hsl || model == Model::Hsv ? 0 : 2;
}
inline Control control(Model model, unsigned component, double a,
                       double b = 0) {
  if (cartesian(model))
    return {a == 1 && b == 0 ? Action::Copy : Action::Affine, a, b};
  const bool bounded = model == Model::Hsl || model == Model::Hsv;
  if (component == hue_component(model))
    return {a == 0 ? Action::Copy : Action::Hue, 1, a};
  const bool multiply = component == 1 || model == Model::Hsv;
  if (multiply)
    return {a == 1    ? Action::Copy
            : bounded ? Action::ClampMultiply
                      : Action::Multiply,
            a, 0};
  return {a == 0    ? Action::Copy
          : bounded ? Action::ClampAdd
                    : Action::Add,
          1, a};
}
inline double evaluate(double x, const Control& c) {
  switch (c.action) {
    case Action::Multiply:
      return x * c.scale;
    case Action::ClampMultiply:
      return std::clamp(x * c.scale, 0.0, 1.0);
    case Action::ClampAdd:
      return std::clamp(x + c.offset, 0.0, 1.0);
    case Action::Affine:
      return std::fma(x, c.scale, c.offset);
    case Action::Add:
    case Action::Hue:
      return x + c.offset;
    case Action::Copy:
      return x;
  }
  return x;
}
template <class T>
inline void scalar(const T* source, T* target, const Control& c,
                   double gate = 1) {
  if (c.action == Action::Copy || (c.action == Action::Hue && !(gate > 0))) {
    std::memcpy(target, source, sizeof(T));
  } else {
    *target = static_cast<T>(evaluate(static_cast<double>(*source), c));
  }
}

#if defined(__aarch64__) || \
    (defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)))
namespace simd = affine;
#if defined(__x86_64__)
#define PS_COORD_TARGET __attribute__((target("avx2,fma")))
#else
#define PS_COORD_TARGET
#endif
struct PacketControl {
  simd::Packet scale, offset;
  simd::Mask multiply, clamp, hue, copy;
};
template <unsigned C>
PS_COORD_TARGET inline PacketControl packet_control(const Control* c,
                                                    unsigned phase) {
  double scale[simd::kLanes], offset[simd::kLanes];
  std::uint64_t multiply[simd::kLanes], clamp[simd::kLanes];
  std::uint64_t hue[simd::kLanes], copy[simd::kLanes];
  for (unsigned j = 0; j < simd::kLanes; ++j) {
    const auto& p = c[(phase + j) % C];
    scale[j] = p.scale;
    offset[j] = p.offset;
    multiply[j] =
        p.action == Action::Multiply || p.action == Action::ClampMultiply
            ? UINT64_MAX
            : 0;
    clamp[j] = p.action == Action::ClampAdd || p.action == Action::ClampMultiply
                   ? UINT64_MAX
                   : 0;
    hue[j] = p.action == Action::Hue ? UINT64_MAX : 0;
    copy[j] = p.action == Action::Copy ? UINT64_MAX : 0;
  }
  return {simd::load(scale),         simd::load(offset),
          simd::mask_load(multiply), simd::mask_load(clamp),
          simd::mask_load(hue),      simd::mask_load(copy)};
}
PS_COORD_TARGET inline simd::Mask either(simd::Mask a, simd::Mask b) {
#if defined(__aarch64__)
  return vorrq_u64(a, b);
#else
  return _mm256_or_pd(a, b);
#endif
}
template <bool Clamp>
PS_COORD_TARGET inline simd::Packet adjusted(simd::Packet x,
                                             const PacketControl& c) {
  auto y =
      simd::choose(c.multiply, simd::mul(x, c.scale), simd::add(x, c.offset));
  if constexpr (!Clamp)
    return y;
  auto clipped = simd::choose(simd::ge(y, simd::splat(0)), y, simd::splat(0));
  clipped =
      simd::choose(simd::le(clipped, simd::splat(1)), clipped, simd::splat(1));
  return simd::choose(c.clamp, clipped, y);
}
template <class T, unsigned C>
PS_COORD_TARGET inline simd::Packet gather_gate(const T* source,
                                                std::size_t first,
                                                unsigned gate_component) {
  double values[simd::kLanes];
  for (unsigned lane = 0; lane < simd::kLanes; ++lane)
    values[lane] = source[(first + lane) / C * C + gate_component];
  return simd::load(values);
}
template <class T, unsigned C, bool Clamp>
PS_COORD_TARGET inline void vector_span(const T* source, T* target,
                                        std::size_t count, const Control* c,
                                        unsigned gate_component) {
  constexpr unsigned kPackets = C == 3 ? 3 : 4;
  constexpr unsigned kStep = kPackets * simd::kLanes;
  PacketControl v[kPackets];
  for (unsigned j = 0; j < kPackets; ++j)
    v[j] = packet_control<C>(c, j * simd::kLanes);
  const auto n = count * C;
  std::size_t i = 0;
  for (; i + kStep <= n; i += kStep) {
    simd::Packet x[kPackets];
    for (unsigned j = 0; j < kPackets; ++j)
      x[j] = simd::load(source + i + j * simd::kLanes);
#pragma unroll
    for (unsigned j = 0; j < kPackets; ++j) {
      simd::Packet gate;
#if defined(__x86_64__)
      if constexpr (C == 4) {
        switch (gate_component) {
          case 0:
            gate = _mm256_permute4x64_pd(x[j], 0x00);
            break;
          case 1:
            gate = _mm256_permute4x64_pd(x[j], 0x55);
            break;
          case 2:
            gate = _mm256_permute4x64_pd(x[j], 0xaa);
            break;
          default:
            gate = _mm256_permute4x64_pd(x[j], 0xff);
            break;
        }
      } else if (gate_component == 1) {
        if (j == 0)
          gate = _mm256_blend_pd(_mm256_permute4x64_pd(x[0], 0x55),
                                 _mm256_permute4x64_pd(x[1], 0x00), 8);
        else if (j == 1)
          gate = _mm256_permute4x64_pd(x[1], 0xf0);
        else
          gate = _mm256_blend_pd(_mm256_permute4x64_pd(x[2], 0xaa),
                                 _mm256_permute4x64_pd(x[1], 0xff), 1);
      } else {
        gate = gather_gate<T, C>(source, i + j * simd::kLanes, gate_component);
      }
#else
      gate = gather_gate<T, C>(source, i + j * simd::kLanes, gate_component);
#endif
      const auto copy = either(
          v[j].copy, simd::both(v[j].hue, simd::le(gate, simd::splat(0))));
      simd::save_preserving(target + i + j * simd::kLanes,
                            source + i + j * simd::kLanes,
                            adjusted<Clamp>(x[j], v[j]), copy);
    }
  }
  for (; i < n; ++i)
    scalar(source + i, target + i, c[i % C],
           source[i / C * C + gate_component]);
}
template <class T, Action A>
PS_COORD_TARGET inline void vector_plane_action(const T* source, const T* gate,
                                                T* target, std::size_t n,
                                                const Control& c) {
  constexpr unsigned kPackets = 8;
  constexpr unsigned kStep = kPackets * simd::kLanes;
  const auto scale = simd::splat(c.scale), offset = simd::splat(c.offset);
  const auto zero = simd::splat(0), one = simd::splat(1);
  std::size_t i = 0;
  for (; i + kStep <= n; i += kStep) {
    simd::Packet y[kPackets];
    for (unsigned j = 0; j < kPackets; ++j) {
      const auto x = simd::load(source + i + j * simd::kLanes);
      if constexpr (A == Action::Affine)
        y[j] = simd::madd(x, scale, offset);
      else if constexpr (A == Action::Multiply || A == Action::ClampMultiply)
        y[j] = simd::mul(x, scale);
      else
        y[j] = simd::add(x, offset);
    }
    for (unsigned j = 0; j < kPackets; ++j) {
      if constexpr (A == Action::ClampMultiply || A == Action::ClampAdd) {
#if defined(__aarch64__)
        y[j] = simd::choose(native_simd::less(y[j], zero), zero, y[j]);
        y[j] = simd::choose(native_simd::greater(y[j], one), one, y[j]);
#else
        y[j] = simd::choose(simd::ge(y[j], zero), y[j], zero);
        y[j] = simd::choose(simd::le(y[j], one), y[j], one);
#endif
      }
      if constexpr (A == Action::Hue) {
#if defined(__aarch64__)
        const auto copy = native_simd::less_equal(
            simd::load(gate + i + j * simd::kLanes), zero);
#else
        const auto copy =
            simd::le(simd::load(gate + i + j * simd::kLanes), zero);
#endif
        simd::save_preserving(target + i + j * simd::kLanes,
                              source + i + j * simd::kLanes, y[j], copy);
      } else {
        simd::save(target + i + j * simd::kLanes, y[j]);
      }
    }
  }
  for (; i + simd::kLanes <= n; i += simd::kLanes) {
    const auto x = simd::load(source + i);
    simd::Packet y;
    if constexpr (A == Action::Affine)
      y = simd::madd(x, scale, offset);
    else if constexpr (A == Action::Multiply || A == Action::ClampMultiply)
      y = simd::mul(x, scale);
    else
      y = simd::add(x, offset);
    if constexpr (A == Action::ClampMultiply || A == Action::ClampAdd) {
#if defined(__aarch64__)
      y = simd::choose(native_simd::less(y, zero), zero, y);
      y = simd::choose(native_simd::greater(y, one), one, y);
#else
      y = simd::choose(simd::ge(y, zero), y, zero);
      y = simd::choose(simd::le(y, one), y, one);
#endif
    }
    if constexpr (A == Action::Hue) {
#if defined(__aarch64__)
      const auto copy = native_simd::less_equal(simd::load(gate + i), zero);
#else
      const auto copy = simd::le(simd::load(gate + i), zero);
#endif
      simd::save_preserving(target + i, source + i, y, copy);
    } else {
      simd::save(target + i, y);
    }
  }
  for (; i < n; ++i)
    scalar(source + i, target + i, c, A == Action::Hue ? gate[i] : 1);
}
template <class T>
PS_COORD_TARGET inline void vector_plane(const T* source, const T* gate,
                                         T* target, std::size_t n,
                                         const Control& c) {
  switch (c.action) {
#define PS_COORD_PLANE(A)                                          \
  case Action::A:                                                  \
    vector_plane_action<T, Action::A>(source, gate, target, n, c); \
    break;
    PS_COORD_PLANE(Add)
    PS_COORD_PLANE(Multiply)
    PS_COORD_PLANE(ClampAdd)
    PS_COORD_PLANE(ClampMultiply)
    PS_COORD_PLANE(Hue)
    PS_COORD_PLANE(Affine)
#undef PS_COORD_PLANE
    case Action::Copy:
      std::memcpy(target, source, n * sizeof(T));
      break;
  }
}

template <class T, unsigned C, bool Copy>
PS_COORD_TARGET inline void affine_span(const T* source, T* target,
                                        std::size_t count, const Control* c) {
  constexpr unsigned kPhases = C == 3 ? 3 : C / simd::kLanes;
  constexpr unsigned kPackets = kPhases * (C == 3 ? 2 : 4);
  constexpr unsigned kStep = kPackets * simd::kLanes;
  simd::Packet scale[kPhases], offset[kPhases];
  simd::Mask copy[kPhases];
  for (unsigned j = 0; j < kPhases; ++j) {
    double s[simd::kLanes], o[simd::kLanes];
    std::uint64_t keep[simd::kLanes];
    for (unsigned lane = 0; lane < simd::kLanes; ++lane) {
      const auto& control = c[(j * simd::kLanes + lane) % C];
      s[lane] = control.scale;
      o[lane] = control.offset;
      keep[lane] = control.action == Action::Copy ? UINT64_MAX : 0;
    }
    scale[j] = simd::load(s);
    offset[j] = simd::load(o);
    if constexpr (Copy)
      copy[j] = simd::mask_load(keep);
  }
  const auto n = count * C;
  std::size_t i = 0;
  for (; i + kStep <= n; i += kStep) {
    simd::Packet y[kPackets];
#pragma unroll
    for (unsigned j = 0; j < kPackets; ++j)
      y[j] = simd::madd(simd::load(source + i + j * simd::kLanes),
                        scale[j % kPhases], offset[j % kPhases]);
#pragma unroll
    for (unsigned j = 0; j < kPackets; ++j) {
      if constexpr (Copy)
        simd::save_preserving(target + i + j * simd::kLanes,
                              source + i + j * simd::kLanes, y[j],
                              copy[j % kPhases]);
      else
        simd::save(target + i + j * simd::kLanes, y[j]);
    }
  }
  unsigned phase = 0;
  for (; i + simd::kLanes <= n; i += simd::kLanes) {
    const auto y =
        simd::madd(simd::load(source + i), scale[phase], offset[phase]);
    if constexpr (Copy)
      simd::save_preserving(target + i, source + i, y, copy[phase]);
    else
      simd::save(target + i, y);
    phase = phase + 1 == kPhases ? 0 : phase + 1;
  }
  for (; i < n; ++i)
    scalar(source + i, target + i, c[i % C], 1);
}
template <class T, unsigned C>
PS_COORD_TARGET inline void affine_span(const T* source, T* target,
                                        std::size_t count, const Control* c) {
  bool copy = false;
  for (unsigned j = 0; j < C; ++j)
    copy |= c[j].action == Action::Copy;
  if (copy)
    affine_span<T, C, true>(source, target, count, c);
  else
    affine_span<T, C, false>(source, target, count, c);
}

#if defined(__x86_64__)
template <bool Copy>
PS_COORD_TARGET inline void stream_plane64(const double* source, double* target,
                                           std::size_t count,
                                           const Control& c) {
  std::size_t i = 0;
  for (; i < count && reinterpret_cast<std::uintptr_t>(target + i) % 32; ++i) {
    if constexpr (Copy)
      std::memcpy(target + i, source + i, sizeof(double));
    else
      target[i] = std::fma(source[i], c.scale, c.offset);
  }
  const auto scale = _mm256_set1_pd(c.scale);
  const auto offset = _mm256_set1_pd(c.offset);
  for (; i + 16 <= count; i += 16) {
    __m256d value[4];
#pragma unroll
    for (unsigned j = 0; j < 4; ++j) {
      value[j] = _mm256_loadu_pd(source + i + 4 * j);
      if constexpr (!Copy)
        value[j] = _mm256_fmadd_pd(value[j], scale, offset);
    }
#pragma unroll
    for (unsigned j = 0; j < 4; ++j)
      _mm256_stream_pd(target + i + 4 * j, value[j]);
  }
  for (; i + 4 <= count; i += 4) {
    auto value = _mm256_loadu_pd(source + i);
    if constexpr (!Copy)
      value = _mm256_fmadd_pd(value, scale, offset);
    _mm256_stream_pd(target + i, value);
  }
  // Complete non-temporal writes before the caller can publish or cancel.
  _mm_sfence();
  for (; i < count; ++i) {
    if constexpr (Copy)
      std::memcpy(target + i, source + i, sizeof(double));
    else
      target[i] = std::fma(source[i], c.scale, c.offset);
  }
}
PS_COORD_TARGET inline void stream_span32(const float* source, float* target,
                                          std::size_t count, const Control* c) {
  const auto scale = _mm256_setr_pd(c[0].scale, c[1].scale, c[2].scale, 1.0);
  const auto offset =
      _mm256_setr_pd(c[0].offset, c[1].offset, c[2].offset, 0.0);
  std::size_t i = 0;
  for (; i < count && reinterpret_cast<std::uintptr_t>(target + 4 * i) % 64;
       ++i)
    for (unsigned j = 0; j < 4; ++j)
      scalar(source + 4 * i + j, target + 4 * i + j, c[j]);
  for (; i + 8 <= count; i += 8) {
    __m128 value[8];
    // Keep source loads ahead of stores, which fill two complete cache lines.
#pragma unroll
    for (unsigned j = 0; j < 8; ++j) {
      const auto raw = _mm_loadu_ps(source + 4 * (i + j));
      value[j] = _mm_blend_ps(
          _mm256_cvtpd_ps(_mm256_fmadd_pd(_mm256_cvtps_pd(raw), scale, offset)),
          raw, 8);
    }
#pragma unroll
    for (unsigned j = 0; j < 8; ++j)
      _mm_stream_ps(target + 4 * (i + j), value[j]);
  }
  _mm_sfence();
  for (; i < count; ++i)
    for (unsigned j = 0; j < 4; ++j)
      scalar(source + 4 * i + j, target + 4 * i + j, c[j]);
}
#endif

#undef PS_COORD_TARGET
#endif

/** Large disjoint views may avoid allocating destination cache lines.
 * The threshold covers source plus destination bytes, never a shape label. */
template <class T>
inline bool prefer_streaming(Model model, bool planar, std::uint64_t count,
                             unsigned components) {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  if (!cartesian(model) || !components)
    return false;
  if constexpr (sizeof(T) == sizeof(double)) {
    if (!planar)
      return false;
  } else {
    if (planar || components != 4)
      return false;
  }
  const auto bytes_per_sample = 2 * sizeof(T) * components;
  if (count < (kStreamFootprint + bytes_per_sample - 1) / bytes_per_sample)
    return false;
  static const bool available =
      __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  return available;
#else
  (void)model;
  (void)planar;
  (void)count;
  (void)components;
  return false;
#endif
}
template <class T>
inline bool stream_plane(const T* source, T* target, std::size_t count,
                         const Control& c) {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  if constexpr (sizeof(T) == sizeof(double)) {
    if (c.action == Action::Copy)
      stream_plane64<true>(source, target, count, c);
    else if (c.action == Action::Affine)
      stream_plane64<false>(source, target, count, c);
    else
      return false;
    return true;
  }
#endif
  (void)source;
  (void)target;
  (void)count;
  (void)c;
  return false;
}
template <class T, unsigned C>
inline bool stream_span(const T* source, T* target, std::size_t count,
                        const Control* c) {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  if constexpr (sizeof(T) == sizeof(float) && C == 4) {
    if (reinterpret_cast<std::uintptr_t>(target) % 16 == 0 &&
        c[0].action == Action::Affine && c[1].action == Action::Affine &&
        c[2].action == Action::Affine && c[3].action == Action::Copy) {
      stream_span32(source, target, count, c);
      return true;
    }
  }
#endif
  (void)source;
  (void)target;
  (void)count;
  (void)c;
  return false;
}

#if defined(__aarch64__)
inline float64x2_t neon_value(float64x2_t x, const Control& c) {
  auto y = c.action == Action::Multiply || c.action == Action::ClampMultiply
               ? vmulq_n_f64(x, c.scale)
               : vaddq_f64(x, vdupq_n_f64(c.offset));
  if (c.action == Action::ClampAdd || c.action == Action::ClampMultiply) {
    y = vbslq_f64(native_simd::less(y, vdupq_n_f64(0)), vdupq_n_f64(0), y);
    y = vbslq_f64(native_simd::greater(y, vdupq_n_f64(1)), vdupq_n_f64(1), y);
  }
  return y;
}
template <unsigned C, unsigned Gate>
inline void neon_values(float32x4_t* pixels, const Control* c) {
  const auto gate = native_simd::greater(pixels[Gate], vdupq_n_f32(0));
#pragma unroll
  for (unsigned j = 0; j < C; ++j) {
    if (c[j].action == Action::Copy)
      continue;
    const auto lo = neon_value(vcvt_f64_f32(vget_low_f32(pixels[j])), c[j]);
    const auto hi = neon_value(vcvt_f64_f32(vget_high_f32(pixels[j])), c[j]);
    auto y = vcombine_f32(vcvt_f32_f64(lo), vcvt_f32_f64(hi));
    if (c[j].action == Action::Hue)
      y = vbslq_f32(gate, y, pixels[j]);
    pixels[j] = y;
  }
}
template <unsigned C, unsigned Gate>
inline void neon_values(float64x2_t* pixels, const Control* c) {
  const auto gate = native_simd::greater(pixels[Gate], vdupq_n_f64(0));
#pragma unroll
  for (unsigned j = 0; j < C; ++j) {
    if (c[j].action == Action::Copy)
      continue;
    auto y = neon_value(pixels[j], c[j]);
    if (c[j].action == Action::Hue)
      y = vbslq_f64(gate, y, pixels[j]);
    pixels[j] = y;
  }
}
template <class T, unsigned C, unsigned Gate>
inline void neon_span(const T* source, T* target, std::size_t count,
                      const Control* c) {
  constexpr unsigned step = sizeof(T) == 4 ? 4 : 2;
  std::size_t i = 0;
  for (; i + step <= count; i += step) {
    if constexpr (sizeof(T) == 4) {
      if constexpr (C == 3) {
        auto pixels = vld3q_f32(source + C * i);
        neon_values<C, Gate>(pixels.val, c);
        vst3q_f32(target + C * i, pixels);
      } else {
        auto pixels = vld4q_f32(source + C * i);
        neon_values<C, Gate>(pixels.val, c);
        vst4q_f32(target + C * i, pixels);
      }
    } else {
      if constexpr (C == 3) {
        auto pixels = vld3q_f64(source + C * i);
        neon_values<C, Gate>(pixels.val, c);
        vst3q_f64(target + C * i, pixels);
      } else {
        auto pixels = vld4q_f64(source + C * i);
        neon_values<C, Gate>(pixels.val, c);
        vst4q_f64(target + C * i, pixels);
      }
    }
  }
  for (; i < count; ++i)
    for (unsigned j = 0; j < C; ++j)
      scalar(source + C * i + j, target + C * i + j, c[j],
             source[C * i + Gate]);
}
#endif

/** Disjoint contiguous buffers. C is the stored channel count, not a model.
 * The gate is always the unmodified input saturation/chroma component. */
template <class T, unsigned C>
inline void span(const T* source, T* target, std::size_t count,
                 const Control* c, unsigned gate_component = 1) {
  bool copy = true, linear = true;
  for (unsigned j = 0; j < C; ++j) {
    copy &= c[j].action == Action::Copy;
    linear &= c[j].action == Action::Copy || c[j].action == Action::Affine;
  }
  if (copy) {
    std::memcpy(target, source, count * C * sizeof(T));
    return;
  }
#if defined(__aarch64__)
  if (linear) {
    affine_span<T, C>(source, target, count, c);
    return;
  }
  switch (gate_component) {
    case 0:
      neon_span<T, C, 0>(source, target, count, c);
      break;
    case 1:
      neon_span<T, C, 1>(source, target, count, c);
      break;
    case 2:
      neon_span<T, C, 2>(source, target, count, c);
      break;
    default:
      if constexpr (C == 4)
        neon_span<T, C, 3>(source, target, count, c);
      break;
  }
  return;
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  static const bool available =
      __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  if (available) {
    if (linear) {
      affine_span<T, C>(source, target, count, c);
      return;
    }
    bool bounded = false;
    for (unsigned j = 0; j < C; ++j)
      bounded |= c[j].action == Action::ClampAdd ||
                 c[j].action == Action::ClampMultiply;
    if (bounded)
      vector_span<T, C, true>(source, target, count, c, gate_component);
    else
      vector_span<T, C, false>(source, target, count, c, gate_component);
    return;
  }
#endif
  for (std::size_t i = 0; i < count * C; ++i)
    scalar(source + i, target + i, c[i % C],
           source[i / C * C + gate_component]);
}
/** A component-only run; gate may be null unless this is an active hue row. */
template <class T>
inline void plane(const T* source, const T* gate, T* target, std::size_t n,
                  const Control& c) {
  if (c.action == Action::Copy) {
    std::memcpy(target, source, n * sizeof(T));
    return;
  }
#if defined(__aarch64__)
  vector_plane(source, gate, target, n, c);
  return;
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  static const bool available =
      __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  if (available) {
    vector_plane(source, gate, target, n, c);
    return;
  }
#endif
  for (std::size_t i = 0; i < n; ++i)
    scalar(source + i, target + i, c, c.action == Action::Hue ? gate[i] : 1);
}
}  // namespace ps::plugin_internal::grade::coordinates
