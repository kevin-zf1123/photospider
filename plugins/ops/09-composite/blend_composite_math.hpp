// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "01-numeric/exact_predicate.hpp"
#include "02-format-color/rgb_basis_math.hpp"
#include "03-generation/runtime.hpp"
#include "09-composite/blend_math.hpp"

namespace ps::plugin_internal::blend {
enum class CompositeKind {
  DarkerY,
  LighterY,
  DarkerOklab,
  LighterOklab,
  DarkerLab,
  LighterLab,
  Dissolve,
  Emission,
  MixLinear,
  MixEncoded
};
inline bool whole_color(CompositeKind k) {
  return k <= CompositeKind::LighterLab;
}
inline bool linear_y(CompositeKind k) {
  return k <= CompositeKind::LighterY;
}
inline bool lighter(CompositeKind k) {
  return k == CompositeKind::LighterY || k == CompositeKind::LighterOklab ||
         k == CompositeKind::LighterLab;
}
struct MeasureWorkspace final {
  basis_ops::RowWorkspace exact;
  std::array<std::uint64_t, 6> previous{};
  bool valid = false, previous_background = false;
};

/** Exact normalization of the published binary64 basis Y row. */
inline basis_ops::Matrix normalized_y(const std::array<double, 3>& raw) {
  using namespace basis_ops;  // NOLINT(build/namespaces)
  const auto sum = add(add(number(raw[0]), number(raw[1])), number(raw[2]));
  if (sum.compare(number(0)) <= 0)
    throw Status{ErrorCode::InvalidArgument, "BLN requires a positive Y sum"};
  Matrix result{};
  for (unsigned c = 0; c < 3; ++c)
    result[c] = divide(number(raw[c]), sum);
  return result;
}
struct Luma final {
  basis_ops::ExactRow row;
  bool relative_certificate = true, nonnegative = true;
  explicit Luma(const std::array<double, 3>& raw) : row(normalized_y(raw), 0) {
    for (unsigned c = 0; c < 3; ++c) {
      relative_certificate &=
          !row.nonzero[c] || std::isnormal(row.approximate[c]);
      nonnegative &= raw[c] >= 0;
    }
  }

  struct Interval {
    double low, high;
  };
  Interval enclosure(const double* x) const {
    if (x[0] == x[1] && x[1] == x[2])
      return {x[0], x[0]};
    if (!row.candidate_available)
      return {-INFINITY, INFINITY};
    const double a = x[0] * row.approximate[0];
    const double b = x[1] * row.approximate[1];
    const double c = x[2] * row.approximate[2];
    const double v = (a + b) + c;
    const double m = (std::abs(a) + std::abs(b)) + std::abs(c);
    const double e =
        (std::abs(x[0]) * row.error[0] + std::abs(x[1]) * row.error[1]) +
        std::abs(x[2]) * row.error[2];
    const double error =
        basis_ops::outward((m * 0x1p-48 + e * 2) + 0x1p-1020, true);
    if (!std::isfinite(v) || !std::isfinite(error))
      return {-INFINITY, INFINITY};
    return {basis_ops::outward(v - error, false),
            basis_ops::outward(v + error, true)};
  }
  template <class T>
  bool background(const T* b, const T* s, bool high, MeasureWorkspace* scratch,
                  const core_internal::WorkConsumer& work) const {
    if (b[0] == s[0] && b[1] == s[1] && b[2] == s[2])
      return true;
    const double db[3] = {b[0], b[1], b[2]};
    const double ds[3] = {s[0], s[1], s[2]};
    const auto bi = enclosure(db), si = enclosure(ds);
    if (bi.high < si.low)
      return !high;
    if (si.high < bi.low)
      return high;
    if (bi.low == bi.high && si.low == si.high)
      return high ? bi.low >= si.low : bi.low <= si.low;
    std::array<std::uint64_t, 6> raw;
    for (unsigned c = 0; c < 3; ++c) {
      raw[c] = basis_ops::bits(db[c]);
      raw[c + 3] = basis_ops::bits(ds[c]);
    }
    if (scratch->valid && scratch->previous == raw)
      return scratch->previous_background;
    auto bv =
        row.evaluate({raw[0], raw[1], raw[2]}, false, &scratch->exact, work);
    if (!bv.ok())
      throw bv.status();
    auto sv =
        row.evaluate({raw[3], raw[4], raw[5]}, false, &scratch->exact, work);
    if (!sv.ok())
      throw sv.status();
    const double by = generation::number(bv.value()),
                 sy = generation::number(sv.value());
    const bool answer = high ? by >= sy : by <= sy;
    scratch->valid = true;
    scratch->previous = raw;
    scratch->previous_background = answer;
    return answer;
  }
};

inline std::uint64_t counter_mix(std::uint64_t z) {
  z += UINT64_C(0x9e3779b97f4a7c15);
  z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
  return z ^ (z >> 31);
}
inline std::uint64_t counter(std::uint64_t seed_state, std::uint64_t x,
                             std::uint64_t y, std::uint64_t frame) {
  return counter_mix(counter_mix(counter_mix(seed_state ^ x) ^ y) ^ frame);
}
template <class T>
inline bool coverage_hit(std::uint64_t u, T alpha) {
  if (alpha == 0)
    return false;
  if (alpha == 1)
    return true;
  std::uint64_t raw = 0;
  std::memcpy(&raw, &alpha, sizeof(T));
  const auto p = numeric_ops::BinaryParts::decode(raw, sizeof(T) == 4);
  const int shift = p.exponent + 64;
  std::uint64_t cutoff;
  if (shift >= 0) {
    cutoff = p.significand << shift;
  } else {
    const unsigned k = static_cast<unsigned>(-shift);
    cutoff = k >= 64 ? 1
                     : (p.significand >> k) +
                           ((p.significand & ((UINT64_C(1) << k) - 1)) != 0);
  }
  return u < cutoff;
}

#if defined(__aarch64__)
template <class T, unsigned C>
struct Interleaved;
#define PS_BLEND_INTERLEAVED(T, S, C, V)                        \
  template <>                                                   \
  struct Interleaved<T, C> {                                    \
    using Packet = V;                                           \
    static Packet load(const T* p) { return vld##C##q_##S(p); } \
    static void save(T* p, Packet v) { vst##C##q_##S(p, v); }   \
  }
PS_BLEND_INTERLEAVED(float, f32, 2, float32x4x2_t);
PS_BLEND_INTERLEAVED(double, f64, 2, float64x2x2_t);
PS_BLEND_INTERLEAVED(float, f32, 3, float32x4x3_t);
PS_BLEND_INTERLEAVED(double, f64, 3, float64x2x3_t);
PS_BLEND_INTERLEAVED(float, f32, 4, float32x4x4_t);
PS_BLEND_INTERLEAVED(double, f64, 4, float64x2x4_t);
#undef PS_BLEND_INTERLEAVED

inline float64x2_t outward_packet(float64x2_t value, bool up) {
  const auto raw = vreinterpretq_u64_f64(value);
  const auto magnitude =
      vandq_u64(raw, vdupq_n_u64(UINT64_C(0x7fffffffffffffff)));
  const auto zero = vceqq_u64(magnitude, vdupq_n_u64(0));
  const auto sign = vshrq_n_u64(raw, 63);
  const auto increment = vceqq_u64(sign, vdupq_n_u64(up ? 0 : 1));
  const auto changed = vbslq_u64(increment, vaddq_u64(raw, vdupq_n_u64(1)),
                                 vsubq_u64(raw, vdupq_n_u64(1)));
  const auto inf = vdupq_n_u64(UINT64_C(0x7ff0000000000000));
  const auto keep = vorrq_u64(vcgtq_u64(magnitude, inf),
                              vandq_u64(vceqq_u64(magnitude, inf), increment));
  return vreinterpretq_f64_u64(vbslq_u64(
      keep, raw,
      vbslq_u64(zero, vdupq_n_u64(up ? 1 : UINT64_C(0x8000000000000001)),
                changed)));
}
struct PacketInterval {
  float64x2_t low, high;
  uint64x2_t valid;
};
inline PacketInterval measure_packet(const float64x2_t* x, const Luma& luma) {
  const auto& row = luma.row;
  const auto a = vmulq_n_f64(x[0], row.approximate[0]);
  const auto b = vmulq_n_f64(x[1], row.approximate[1]);
  const auto c = vmulq_n_f64(x[2], row.approximate[2]);
  const auto value = vaddq_f64(vaddq_f64(a, b), c);
  const auto magnitude =
      vaddq_f64(vaddq_f64(vabsq_f64(a), vabsq_f64(b)), vabsq_f64(c));
  const auto coefficient =
      vaddq_f64(vaddq_f64(vmulq_n_f64(vabsq_f64(x[0]), row.error[0]),
                          vmulq_n_f64(vabsq_f64(x[1]), row.error[1])),
                vmulq_n_f64(vabsq_f64(x[2]), row.error[2]));
  const auto error =
      outward_packet(vaddq_f64(vaddq_f64(vmulq_n_f64(magnitude, 0x1p-48),
                                         vmulq_n_f64(coefficient, 2)),
                               vdupq_n_f64(0x1p-1020)),
                     true);
  const auto maximum = vdupq_n_f64(std::numeric_limits<double>::max());
  const auto valid = vandq_u64(Neon<double>::le(vabsq_f64(value), maximum),
                               Neon<double>::le(error, maximum));
  return {outward_packet(vsubq_f64(value, error), false),
          outward_packet(vaddq_f64(value, error), true), valid};
}
inline uint64x2_t strict_less(float64x2_t a, float64x2_t b) {
  uint64x2_t result;
  asm("fcmgt %0.2d, %2.2d, %1.2d" : "=w"(result) : "w"(a), "w"(b));
  return result;
}
template <class T, bool High>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
inline uint64x2_t measure_choice(const T* b, const T* s, const float64x2_t* bv,
                                 const float64x2_t* sv, const Luma& luma,
                                 MeasureWorkspace* scratch,
                                 const core_internal::WorkConsumer& work) {
  using O = Neon<double>;
  uint64x2_t valid, take;
  if (luma.relative_certificate && luma.nonnegative) {
    auto magnitude_bits = vreinterpretq_u64_f64(bv[0]);
    for (unsigned c = 1; c < 3; ++c)
      magnitude_bits = vorrq_u64(magnitude_bits, vreinterpretq_u64_f64(bv[c]));
    for (unsigned c = 0; c < 3; ++c)
      magnitude_bits = vorrq_u64(magnitude_bits, vreinterpretq_u64_f64(sv[c]));
    const auto magnitude = vreinterpretq_f64_u64(
        vandq_u64(magnitude_bits, vdupq_n_u64(UINT64_C(0x7fffffffffffffff))));
    float64x2_t value[2];
    const float64x2_t* input[2] = {bv, sv};
    for (unsigned side = 0; side < 2; ++side) {
      value[side] = vmulq_n_f64(input[side][0], luma.row.approximate[0]);
      for (unsigned c = 1; c < 3; ++c)
        value[side] =
            vfmaq_n_f64(value[side], input[side][c], luma.row.approximate[c]);
    }
    const auto difference = vsubq_f64(value[0], value[1]);
    // Positive normalized weights sum exactly to one. The OR magnitude
    // bounds every input abs value when finite; 128u*m+128eta exceeds the
    // <23u*m+5eta error of the two published RN64 measures and difference.
    // Explicit FMA is only a certificate candidate, never a published value.
    auto error =
        vaddq_f64(vmulq_n_f64(magnitude, 0x1p-46), vdupq_n_f64(0x1p-1067));
    error = vreinterpretq_f64_u64(
        vaddq_u64(vreinterpretq_u64_f64(error), vdupq_n_u64(1)));
    const auto positive = strict_less(error, difference);
    const auto negative = strict_less(difference, vnegq_f64(error));
    valid = vandq_u64(
        vorrq_u64(positive, negative),
        vandq_u64(O::le(magnitude,
                        vdupq_n_f64(std::numeric_limits<double>::max() / 4)),
                  O::le(vabsq_f64(difference),
                        vdupq_n_f64(std::numeric_limits<double>::max()))));
    take = High ? positive : negative;
  } else if (luma.relative_certificate) {
    float64x2_t value[2], magnitude[2];
    const float64x2_t* input[2] = {bv, sv};
    for (unsigned side = 0; side < 2; ++side) {
      const auto a = vmulq_n_f64(input[side][0], luma.row.approximate[0]);
      const auto c = vmulq_n_f64(input[side][1], luma.row.approximate[1]);
      const auto d = vmulq_n_f64(input[side][2], luma.row.approximate[2]);
      value[side] = vaddq_f64(vaddq_f64(a, c), d);
      magnitude[side] =
          vaddq_f64(vaddq_f64(vabsq_f64(a), vabsq_f64(c)), vabsq_f64(d));
    }
    const auto difference = vsubq_f64(value[0], value[1]);
    const auto total = vaddq_f64(magnitude[0], magnitude[1]);
    // Normal coefficients have error <= 2u*abs(coefficient). 128u*M+128eta
    // bounds both published RN64 measures and their candidate difference.
    // M<=MAX/2 excludes overflow in either exact measure.
    auto error = vaddq_f64(vmulq_n_f64(total, 0x1p-46), vdupq_n_f64(0x1p-1067));
    error = vreinterpretq_f64_u64(
        vaddq_u64(vreinterpretq_u64_f64(error), vdupq_n_u64(1)));
    const auto positive = strict_less(error, difference);
    const auto negative = strict_less(difference, vnegq_f64(error));
    valid = vandq_u64(
        vorrq_u64(positive, negative),
        vandq_u64(
            Neon<double>::le(
                total, vdupq_n_f64(std::numeric_limits<double>::max() / 2)),
            Neon<double>::le(vabsq_f64(difference),
                             vdupq_n_f64(std::numeric_limits<double>::max()))));
    take = High ? positive : negative;
  } else {
    const auto bi = measure_packet(bv, luma), si = measure_packet(sv, luma);
    const auto blo = strict_less(bi.high, si.low),
               slo = strict_less(si.high, bi.low);
    valid = vandq_u64(vandq_u64(bi.valid, si.valid), vorrq_u64(blo, slo));
    take = High ? slo : blo;
  }
  if (vminvq_u32(vreinterpretq_u32_u64(valid)) == UINT32_MAX)
    return take;
  // Exact tuple/gray shortcuts are needed only for uncertified lanes.
  const auto equal = vandq_u64(
      vandq_u64(O::eq(bv[0], sv[0]), O::eq(bv[1], sv[1])), O::eq(bv[2], sv[2]));
  const auto gray =
      vandq_u64(vandq_u64(O::eq(bv[0], bv[1]), O::eq(bv[0], bv[2])),
                vandq_u64(O::eq(sv[0], sv[1]), O::eq(sv[0], sv[2])));
  const auto exact = vorrq_u64(equal, gray);
  const auto exact_take = vorrq_u64(
      equal, vandq_u64(gray, High ? O::le(sv[0], bv[0]) : O::le(bv[0], sv[0])));
  valid = vorrq_u64(valid, exact);
  take = vbslq_u64(exact, exact_take, take);
  if (vminvq_u32(vreinterpretq_u32_u64(valid)) != UINT32_MAX) {
    std::uint64_t bits[2], ok[2];
    vst1q_u64(bits, take);
    vst1q_u64(ok, valid);
    for (unsigned j = 0; j < 2; ++j)
      if (!ok[j])
        bits[j] = luma.background(b + 3 * j, s + 3 * j, High, scratch, work)
                      ? UINT64_MAX
                      : 0;
    take = vld1q_u64(bits);
  }
  return take;
}
#endif

template <class T, bool Y, bool High>
inline void whole_span(const T* b, const T* s, T* out, std::size_t count,
                       const Luma* luma, MeasureWorkspace* scratch,
                       const core_internal::WorkConsumer& work) {
  std::size_t i = 0;
#if defined(__aarch64__)
  if constexpr (Y) {
    if (luma->row.candidate_available) {
      for (; i + Neon<T>::lanes <= count; i += Neon<T>::lanes) {
        auto bv = Interleaved<T, 3>::load(b + 3 * i);
        const auto sv = Interleaved<T, 3>::load(s + 3 * i);
        if constexpr (std::is_same_v<T, double>) {
          const auto take = measure_choice<T, High>(
              b + 3 * i, s + 3 * i, bv.val, sv.val, *luma, scratch, work);
          for (unsigned c = 0; c < 3; ++c)
            bv.val[c] = Neon<T>::choose(take, bv.val[c], sv.val[c]);
        } else {
          float64x2_t db[3], ds[3];
          for (unsigned c = 0; c < 3; ++c) {
            db[c] = vcvt_f64_f32(vget_low_f32(bv.val[c]));
            ds[c] = vcvt_f64_f32(vget_low_f32(sv.val[c]));
          }
          const auto lo = measure_choice<T, High>(b + 3 * i, s + 3 * i, db, ds,
                                                  *luma, scratch, work);
          for (unsigned c = 0; c < 3; ++c) {
            db[c] = vcvt_f64_f32(vget_high_f32(bv.val[c]));
            ds[c] = vcvt_f64_f32(vget_high_f32(sv.val[c]));
          }
          const auto hi = measure_choice<T, High>(
              b + 3 * (i + 2), s + 3 * (i + 2), db, ds, *luma, scratch, work);
          const auto take = vcombine_u32(vmovn_u64(lo), vmovn_u64(hi));
          for (unsigned c = 0; c < 3; ++c)
            bv.val[c] = Neon<T>::choose(take, bv.val[c], sv.val[c]);
        }
        Interleaved<T, 3>::save(out + 3 * i, bv);
      }
    }
  }
#endif
  for (; i < count; ++i) {
    bool take_b;
    if constexpr (Y)
      take_b = luma->background(b + 3 * i, s + 3 * i, High, scratch, work);
    else
      take_b = High ? b[3 * i] >= s[3 * i] : b[3 * i] <= s[3 * i];
    std::memcpy(out + 3 * i, (take_b ? b : s) + 3 * i, 3 * sizeof(T));
  }
}

struct DissolveWorkspace final {
  std::array<std::uint64_t, 4096> prefix;
  std::uint64_t begin = 0, size = 0;
};
inline const std::uint64_t* dissolve_prefix(DissolveWorkspace* workspace,
                                            std::uint64_t seed_state,
                                            std::uint64_t x, std::size_t n) {
  if (!workspace || n > workspace->prefix.size())
    return nullptr;
  if (workspace->begin != x || workspace->size != n) {
    for (std::size_t i = 0; i < n; ++i)
      workspace->prefix[i] = counter_mix(seed_state ^ (x + i));
    workspace->begin = x;
    workspace->size = n;
  }
  return workspace->prefix.data();
}
template <class T, unsigned C>
inline void dissolve_span(const T* source, const T* background, T* output,
                          std::size_t n, std::uint64_t seed_state,
                          std::uint64_t x, std::uint64_t y, std::uint64_t frame,
                          const std::uint64_t* prefix = nullptr) {
  static constexpr T one = T(1);
  std::size_t i = 0;
#if defined(__aarch64__)
  for (; i + 8 <= n; i += 8) {
    std::uint64_t random[8];
#pragma clang loop unroll(full)
    for (unsigned j = 0; j < 8; ++j) {
      const auto initial =
          prefix ? prefix[i + j] : counter_mix(seed_state ^ (x + i + j));
      random[j] = counter_mix(counter_mix(initial ^ y) ^ frame);
    }
#pragma clang loop unroll(full)
    for (unsigned j = 0; j < 8; j += 2) {
      const double alpha[2] = {source[C * (i + j) + C - 1],
                               source[C * (i + j + 1) + C - 1]};
      const auto av = vld1q_f64(alpha);
      const auto cutoff = vcvtq_u64_f64(vrndpq_f64(vmulq_n_f64(av, 0x1p64)));
      const auto hit = vorrq_u64(vcltq_u64(vld1q_u64(random + j), cutoff),
                                 Neon<double>::eq(av, vdupq_n_f64(1)));
      std::uint64_t flags[2];
      vst1q_u64(flags, hit);
#pragma clang loop unroll(full)
      for (unsigned lane = 0; lane < 2; ++lane) {
        const auto at = C * (i + j + lane);
        const auto* selected = (flags[lane] ? source : background) + at;
        const auto* selected_alpha =
            flags[lane] ? &one : background + at + C - 1;
        // Preserve the selected-address dependency through instruction
        // selection.
        asm("" : "+r"(selected), "+r"(selected_alpha));
        std::memcpy(output + at, selected, (C - 1) * sizeof(T));
        std::memcpy(output + at + C - 1, selected_alpha, sizeof(T));
      }
    }
  }
#endif
  for (; i < n; ++i) {
    const auto initial = prefix ? prefix[i] : counter_mix(seed_state ^ (x + i));
    const auto u = counter_mix(counter_mix(initial ^ y) ^ frame);
    const bool hit = coverage_hit(u, source[C * i + C - 1]);
    const auto* selected = (hit ? source : background) + C * i;
    const auto* selected_alpha = hit ? &one : background + C * i + C - 1;
#if defined(__aarch64__)
    asm("" : "+r"(selected), "+r"(selected_alpha));
#endif
    std::memcpy(output + C * i, selected, (C - 1) * sizeof(T));
    std::memcpy(output + C * i + C - 1, selected_alpha, sizeof(T));
  }
}

template <class T>
inline void emission_span(const T* background, const T* emission, T gain,
                          T* output, std::size_t count) {
  if (gain == 0) {
    std::memcpy(output, background, 4 * count * sizeof(T));
    return;
  }
  std::size_t i = 0;
#if defined(__aarch64__)
  using O = Neon<T>;
  T g[O::lanes];
  std::fill_n(g, O::lanes, gain);
  const auto multiplier = O::load(g);
  for (; i + O::lanes <= count; i += O::lanes) {
    auto b = Interleaved<T, 4>::load(background + 4 * i);
    const auto e = Interleaved<T, 3>::load(emission + 3 * i);
    for (unsigned c = 0; c < 3; ++c)
      b.val[c] = O::add(b.val[c], O::mul(multiplier, e.val[c]));
    Interleaved<T, 4>::save(output + 4 * i, b);
  }
#endif
  for (; i < count; ++i) {
    for (unsigned c = 0; c < 3; ++c)
      output[4 * i + c] = background[4 * i + c] + gain * emission[3 * i + c];
    std::memcpy(output + 4 * i + 3, background + 4 * i + 3, sizeof(T));
  }
}
template <class T, unsigned C>
inline void mix_pixel(const T* first, const T* second, T t, T* out) {
  if (t == 0 || t == 1) {
    std::memcpy(out, t == 0 ? first : second, C * sizeof(T));
    return;
  }
  const T a = (T(1) - t) * first[C - 1], b = t * second[C - 1];
  const T alpha = a + b;
  for (unsigned c = 0; c < C - 1; ++c) {
    // A zero native weight has no color dependency, including underflow.
    out[c] = alpha > 0 ? ((a == 0 ? T(0) : a * first[c]) +
                          (b == 0 ? T(0) : b * second[c])) /
                             alpha
                       : T(0);
  }
  out[C - 1] = alpha;
}
template <class T, unsigned C>
inline void mix_span(const T* first, const T* second, const T* mix, T* output,
                     std::size_t count) {
  std::size_t i = 0;
#if defined(__aarch64__)
  using O = Neon<T>;
  for (; i + O::lanes <= count; i += O::lanes) {
    T a[O::lanes], b[O::lanes];
    bool active = true;
    // Read the control fields first, never colors of inactive branches.
    for (unsigned lane = 0; lane < O::lanes; ++lane) {
      const T t = mix[i + lane];
      if (t == 0 || t == 1) {
        active = false;
        break;
      }
      a[lane] = (T(1) - t) * first[C * (i + lane) + C - 1];
      b[lane] = t * second[C * (i + lane) + C - 1];
      if (a[lane] == 0 || b[lane] == 0) {
        active = false;
        break;
      }
    }
    if (!active) {
      for (unsigned lane = 0; lane < O::lanes; ++lane)
        mix_pixel<T, C>(first + C * (i + lane), second + C * (i + lane),
                        mix[i + lane], output + C * (i + lane));
      continue;
    }
    const auto av = O::load(a), bv = O::load(b), alpha = O::add(av, bv);
    auto f = Interleaved<T, C>::load(first + C * i);
    const auto s = Interleaved<T, C>::load(second + C * i);
    for (unsigned c = 0; c < C - 1; ++c)
      f.val[c] =
          O::div(O::add(O::mul(av, f.val[c]), O::mul(bv, s.val[c])), alpha);
    f.val[C - 1] = alpha;
    Interleaved<T, C>::save(output + C * i, f);
  }
#endif
  for (; i < count; ++i)
    mix_pixel<T, C>(first + C * i, second + C * i, mix[i], output + C * i);
}
}  // namespace ps::plugin_internal::blend
