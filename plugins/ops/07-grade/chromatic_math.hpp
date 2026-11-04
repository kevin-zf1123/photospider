// Copyright 2026 Photospider Authors
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <type_traits>

#if defined(__x86_64__)
#include <emmintrin.h>
#endif

#include "07-grade/chroma_boundary.hpp"
#include "07-grade/statistical_common.hpp"
#include "07-grade/tonal_zones_math.hpp"

namespace ps::plugin_internal::grade::chromatic {
/** Shared pure-compute packed kernel. Component arrays are native, controls and
 * accumulations binary64. Identity branches retain input bits. Extreme hypot
 * falls back to the scaled library algorithm, avoiding overflow/underflow.
 */
struct PreparedSaturation {
  bool active = false, safe_offset = false;
  double coefficients[3]{};
  void prepare(double s, const double* lum) {
    const auto up = [](double x) { return std::nextafter(x, INFINITY); };
    const double norm =
        up(up(std::abs(lum[0]) + std::abs(lum[1])) + std::abs(lum[2]));
    const double bound = up(s + up(up(1.0 + s) * norm));
    active = s > 0.0 && s != 1.0 && std::isfinite(bound) && bound <= 32.0;
    if (active) {
      for (unsigned c = 0; c < 3; ++c) {
        coefficients[c] = (1.0 - s) * lum[c];
      }
      safe_offset =
          up(up(std::abs(coefficients[0]) + std::abs(coefficients[1])) +
             std::abs(coefficients[2])) <= 0.5;
    }
  }
  double offset(double r, double g, double b) const {
    return std::fma(coefficients[2], b,
                    std::fma(coefficients[1], g, coefficients[0] * r));
  }
};
struct SelectionTransition {
  double low = 0.0, high = 1.0, width = 1.0, inverse = 1.0;
  bool scaled = true;
  void prepare(double lo, double hi) {
    low = lo;
    high = hi;
    width = hi - lo;
    inverse = 1.0 / width;
    scaled = width > 0.0 && std::isfinite(width) && std::isfinite(inverse);
  }
  template <bool Scaled>
  __attribute__((always_inline)) double evaluate(double x) const {
    if constexpr (!Scaled) {
      if (!scaled) {
        return statistics::smooth((x - low) / width);
      }
    }
    if (x <= low) {
      return 0.0;
    }
    if (x >= high) {
      return 1.0;
    }
    return statistics::smooth((x - low) * inverse);
  }
};
struct ColorSelection {
  double center = 0.0;
  bool all_scaled = true;
  SelectionTransition transition[5];
  void prepare(const double* controls) {
    center = std::remainder(controls[0], 2.0);
    all_scaled = true;
    for (unsigned j = 0; j < 5; ++j) {
      transition[j].prepare(controls[1 + 2 * j], controls[2 + 2 * j]);
      all_scaled &= transition[j].scaled;
    }
  }
  // Reduction by the binary period is exact. Keep the subtraction residual
  // until after endpoint comparisons and transition subtraction, so neither
  // many windings nor a narrow transition loses the original phase.
  [[gnu::noinline]] double precise_hue(double h) const {
    h = std::remainder(h, 2.0);
    double d = h - center;
    const double v = d - h;
    double error = (h - (d - v)) - (center + v);
    if (d < 0 || (d == 0 && error < 0)) {
      d = -d;
      error = -error;
    }
    if (d > 1 || (d == 1 && error > 0)) {
      d = 2.0 - d;
      error = -error;
    }
    // Renormalize after folding; d can be zero with a nonzero residual.
    const double sum = d + error;
    const double part = sum - d;
    error = (d - (sum - part)) + (error - part);
    d = sum;
    const auto& t = transition[0];
    if (d < t.low || (d == t.low && error <= 0)) {
      return 1.0;
    }
    if (d > t.high || (d == t.high && error >= 0)) {
      return 0.0;
    }
    return 1.0 - statistics::smooth(((d - t.low) + error) / t.width);
  }
  template <bool AllScaled>
  __attribute__((always_inline)) double hue_weight(double h) const {
    const double d = std::abs(h - center);
    const auto& t = transition[0];
    // A rounded difference can straddle a binary64 endpoint only when it
    // equals that endpoint. Width bounds the continuous interpolation error.
    // Direct differences within one half-period need no further reduction.
    if (d <= 1 && t.width > .001 && d != t.low && d != t.high) {
      return 1.0 - t.evaluate<AllScaled>(d);
    }
    return precise_hue(h);
  }
  template <bool AllScaled>
  __attribute__((always_inline)) double evaluate(double l, double c,
                                                 double h) const {
    if (c == 0.0) {
      return 0.0;
    }
    const double hue = hue_weight<AllScaled>(h);
    const double chroma = transition[1].evaluate<AllScaled>(c) *
                          (1.0 - transition[2].evaluate<AllScaled>(c));
    const double lightness = transition[3].evaluate<AllScaled>(l) *
                             (1.0 - transition[4].evaluate<AllScaled>(l));
    return (hue * chroma) * lightness;
  }
  __attribute__((always_inline)) double operator()(double l, double c,
                                                   double h) const {
    return all_scaled ? evaluate<true>(l, c, h) : evaluate<false>(l, c, h);
  }
};
template <bool AllScaled, class T, unsigned C>
inline void color_selection_span(const ColorSelection& selection,
                                 const T* input, double* output,
                                 std::size_t n) {
  for (std::size_t j = 0; j < n; ++j) {
    output[j] = selection.evaluate<AllScaled>(input[C * j], input[C * j + 1],
                                              input[C * j + 2]);
  }
}
inline double chroma(double a, double b) {
  const double m = std::max(std::abs(a), std::abs(b));
  if (m > 0x1p-500 && m < 0x1p500) {
#if defined(__x86_64__)
    const double squared = a * a + b * b;
    return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(squared)));
#else
    return std::sqrt(a * a + b * b);
#endif
  }
  return std::hypot(a, b);
}
#if defined(__aarch64__) || defined(__x86_64__)
#if defined(__x86_64__)
#define PS_CHROMATIC_TARGET __attribute__((target("avx2,fma")))
#else
#define PS_CHROMATIC_TARGET
#endif
PS_CHROMATIC_TARGET inline affine::Mask chromatic_ge(affine::Packet a,
                                                     affine::Packet b) {
#if defined(__aarch64__)
  return native_simd::greater_equal(a, b);
#else
  return affine::ge(a, b);
#endif
}
PS_CHROMATIC_TARGET inline bool chromatic_any(affine::Mask mask) {
#if defined(__x86_64__)
  return _mm256_movemask_pd(mask) != 0;
#else
  return vmaxvq_u32(vreinterpretq_u32_u64(mask)) != 0;
#endif
}
template <unsigned Kind, class T, unsigned C, bool Prepared, bool CheckOffset>
PS_CHROMATIC_TARGET inline std::size_t saturation_packets_impl(
    const T* input, T* output, std::size_t count, const double* controls,
    const double* lum, const PreparedSaturation& saturation) {
  namespace v = affine;
  const double parameter = controls[0];
  if (((Kind == 0 || Kind == 1) && parameter == 1.0) ||
      (Kind == 2 && parameter == 0.0)) {
    std::memcpy(output, input, count * C * sizeof(T));
    return count;
  }
#if defined(__aarch64__)
  if constexpr (Kind == 1 && std::is_same_v<T, double> && C == 3) {
    // The two opponent coordinates are adjacent within each pixel. Load them
    // together without transposing pixels; lightness remains a raw copy.
    // This branch is after scale-one copy and keeps both multiplication
    // operands in the original order, including ordinary signed-zero
    // multiplication.
    for (std::size_t i = 0; i < count; ++i) {
      const auto ab = vld1q_f64(input + C * i + 1);
      const auto result = vmulq_n_f64(ab, parameter);
      std::memcpy(output + C * i, input + C * i, sizeof(T));
      vst1q_f64(output + C * i + 1, result);
    }
    return count;
  }
#endif
  const auto one = v::splat(1.0), zero = v::splat(0.0),
             scale = v::splat(parameter);
  const auto wr = v::splat(lum ? lum[0] : 0.0),
             wg = v::splat(lum ? lum[1] : 0.0),
             wb = v::splat(lum ? lum[2] : 0.0);
  const auto reference = v::splat(Kind == 2 ? controls[1] : 1.0);
  const auto qr = v::splat(saturation.coefficients[0]),
             qg = v::splat(saturation.coefficients[1]),
             qb = v::splat(saturation.coefficients[2]);
  std::size_t i = 0;
  for (; i + v::kLanes <= count; i += v::kLanes) {
    T source[C][v::kLanes], target[C][v::kLanes];
    for (unsigned c = 0; c < C; ++c) {
      for (unsigned j = 0; j < v::kLanes; ++j) {
        std::memcpy(&source[c][j], input + C * (i + j) + c, sizeof(T));
      }
    }
    const auto l = v::load(source[0]), a = v::load(source[1]),
               b = v::load(source[2]);
    v::Packet value[3]{l, a, b};
    auto copy = feature::equal(one, zero);
    if constexpr (Kind == 0) {
      const auto neutral = v::both(feature::equal(l, a), feature::equal(a, b));
      copy = neutral;
      if constexpr (Prepared) {
        const auto t = v::madd(qb, b, v::madd(qg, a, v::mul(qr, l)));
        if constexpr (!CheckOffset) {
          for (unsigned c = 0; c < 3; ++c) {
            value[c] = v::madd(scale, value[c], t);
          }
        } else {
          const auto usable = v::le(
              v::absolute(t), v::splat(std::numeric_limits<double>::max()));
          if (v::all(usable)) {
            for (unsigned c = 0; c < 3; ++c) {
              value[c] = v::madd(scale, value[c], t);
            }
          } else {
            const auto old_y =
                v::add(v::add(v::mul(wr, l), v::mul(wg, a)), v::mul(wb, b));
            for (unsigned c = 0; c < 3; ++c) {
              const auto fallback =
                  v::add(old_y, v::mul(scale, v::sub(value[c], old_y)));
              value[c] =
                  v::choose(usable, v::madd(scale, value[c], t), fallback);
            }
          }
        }
      } else {
        const auto y =
            v::add(v::add(v::mul(wr, l), v::mul(wg, a)), v::mul(wb, b));
        for (unsigned c = 0; c < 3; ++c) {
          value[c] = v::add(y, v::mul(scale, v::sub(value[c], y)));
        }
      }
    } else if constexpr (Kind == 1) {
      value[1] = v::mul(a, scale);
      value[2] = v::mul(b, scale);
    } else {
      const auto aa = v::absolute(a), ab = v::absolute(b);
      const auto magnitude = v::choose(chromatic_ge(aa, ab), aa, ab);
      const auto safe = feature::either(
          feature::equal(magnitude, zero),
          v::both(feature::greater(magnitude, v::splat(0x1p-500)),
                  feature::greater(v::splat(0x1p500), magnitude)));
      if (!v::all(safe)) {
        break;
      }
      const auto squared = v::add(v::mul(a, a), v::mul(b, b));
#if defined(__x86_64__)
      const auto radius = _mm256_sqrt_pd(squared);
#else
      const auto radius = vsqrtq_f64(squared);
#endif
      auto q = feature::divide(radius, reference);
      q = v::choose(chromatic_ge(q, one), one, q);
      copy = feature::equal(q, one);
      const auto t = v::sub(one, q);
      const auto gain = v::add(one, v::mul(v::mul(scale, t), t));
      value[1] = v::mul(a, gain);
      value[2] = v::mul(b, gain);
    }
    for (unsigned c = 0; c < C; ++c) {
      if (c >= 3 || ((Kind == 1 || Kind == 2) && c == 0)) {
        std::memcpy(target[c], source[c], sizeof(source[c]));
      } else {
        v::save_preserving(target[c], source[c], value[c], copy);
      }
      for (unsigned j = 0; j < v::kLanes; ++j) {
        std::memcpy(output + C * (i + j) + c, &target[c][j], sizeof(T));
      }
    }
  }
  return i;
}
template <unsigned Kind, class T, unsigned C>
PS_CHROMATIC_TARGET inline std::size_t saturation_packets(
    const T* input, T* output, std::size_t count, const double* controls,
    const double* lum, const PreparedSaturation& saturation) {
  if constexpr (Kind == 0 && std::is_same_v<T, double>) {
    if (saturation.active) {
      if (saturation.safe_offset) {
        return saturation_packets_impl<Kind, T, C, true, false>(
            input, output, count, controls, lum, saturation);
      }
      return saturation_packets_impl<Kind, T, C, true, true>(
          input, output, count, controls, lum, saturation);
    }
  }
  return saturation_packets_impl<Kind, T, C, false, false>(
      input, output, count, controls, lum, saturation);
}
template <unsigned Kind, class T, class Z, unsigned C>
PS_CHROMATIC_TARGET inline std::size_t zone_packets(
    const T* input, const Z* weights, T* output, std::size_t n,
    const double* controls, unsigned k, bool normalize) {
  namespace v = affine;
  const unsigned count = Kind == 3 ? 3 : k;
  const auto zero = v::splat(0.0), one = v::splat(1.0);
  std::size_t i = 0;
  for (; i + v::kLanes <= n; i += v::kLanes) {
    T source[C][v::kLanes], target[C][v::kLanes];
    for (unsigned c = 0; c < C; ++c) {
      for (unsigned lane = 0; lane < v::kLanes; ++lane) {
        std::memcpy(&source[c][lane], input + C * (i + lane) + c, sizeof(T));
      }
    }
    v::Packet sum = zero, delta[3]{zero, zero, zero};
    for (unsigned z = 0; z < count; ++z) {
      Z a[v::kLanes];
      for (unsigned lane = 0; lane < v::kLanes; ++lane) {
        a[lane] = weights[count * (i + lane) + z];
      }
      const auto weight = v::load(a);
      sum = v::add(sum, weight);
      for (unsigned c = Kind == 3 ? 1 : 0; c < 3; ++c) {
        delta[c] = v::add(
            delta[c],
            v::mul(
                weight,
                v::splat(controls[(Kind == 3 ? 2 : 3) * z + c - (Kind == 3)])));
      }
    }
    auto denominator = sum;
    if constexpr (Kind == 6) {
      denominator =
          normalize ? v::choose(feature::greater(sum, one), sum, one) : one;
    }
    for (unsigned c = 0; c < C; ++c) {
      if (c >= 3 || (Kind == 3 && c == 0)) {
        std::memcpy(target[c], source[c], sizeof(source[c]));
      } else {
        const auto value =
            v::add(v::load(source[c]), feature::divide(delta[c], denominator));
        v::save_preserving(target[c], source[c], value,
                           feature::equal(delta[c], zero));
      }
    }
    for (unsigned lane = 0; lane < v::kLanes; ++lane) {
      for (unsigned c = 0; c < C; ++c) {
        std::memcpy(output + C * (i + lane) + c, &target[c][lane], sizeof(T));
      }
    }
  }
  return i;
}
PS_CHROMATIC_TARGET inline affine::Packet selection_smooth(affine::Packet t) {
  namespace v = affine;
  t = v::choose(feature::greater(v::splat(0.0), t), v::splat(0.0), t);
  t = v::choose(feature::greater(t, v::splat(1.0)), v::splat(1.0), t);
  return v::mul(v::mul(t, t), v::sub(v::splat(3.0), v::mul(v::splat(2.0), t)));
}
PS_CHROMATIC_TARGET inline affine::Packet selection_transition(
    const SelectionTransition& transition, affine::Packet x) {
  namespace v = affine;
  auto t = selection_smooth(v::mul(v::sub(x, v::splat(transition.low)),
                                   v::splat(transition.inverse)));
  t = v::choose(v::le(x, v::splat(transition.low)), v::splat(0.0), t);
  return v::choose(chromatic_ge(x, v::splat(transition.high)), v::splat(1.0),
                   t);
}
template <unsigned Kind, class T, unsigned C>
PS_CHROMATIC_TARGET inline std::size_t selection_packets(
    const T* input, double* output, std::size_t n, const double* controls,
    const ColorSelection& selection) {
  namespace v = affine;
  if constexpr (Kind == 5) {
    if (!selection.all_scaled || selection.transition[0].width <= .001) {
      return 0;
    }
  } else {
    if (!(controls[1] - controls[0] > .001 * controls[1])) {
      return 0;
    }
  }
  const auto zero = v::splat(0.0), one = v::splat(1.0);
  std::size_t i = 0;
  for (; i + v::kLanes <= n; i += v::kLanes) {
    T data[3][v::kLanes];
    for (unsigned c = 0; c < 3; ++c) {
      for (unsigned lane = 0; lane < v::kLanes; ++lane) {
        data[c][lane] = input[C * (i + lane) + c];
      }
    }
    const auto l = v::load(data[0]), a = v::load(data[1]), b = v::load(data[2]);
    v::Packet result;
    if constexpr (Kind == 5) {
      const auto distance = v::absolute(v::sub(b, v::splat(selection.center)));
      const auto& t = selection.transition[0];
      const auto fallback = feature::either(
          feature::greater(distance, one),
          feature::either(feature::equal(distance, v::splat(t.low)),
                          feature::equal(distance, v::splat(t.high))));
      if (chromatic_any(fallback)) {
        break;
      }
      const auto hue = v::sub(one, selection_transition(t, distance));
      const auto chroma =
          v::mul(selection_transition(selection.transition[1], a),
                 v::sub(one, selection_transition(selection.transition[2], a)));
      const auto lightness =
          v::mul(selection_transition(selection.transition[3], l),
                 v::sub(one, selection_transition(selection.transition[4], l)));
      result = v::choose(feature::equal(a, zero), zero,
                         v::mul(v::mul(hue, chroma), lightness));
    } else {
      const auto aa = v::absolute(a), ab = v::absolute(b);
      const auto magnitude = v::choose(chromatic_ge(aa, ab), aa, ab);
      const auto safe = feature::either(
          feature::equal(magnitude, zero),
          v::both(feature::greater(magnitude, v::splat(0x1p-500)),
                  feature::greater(v::splat(0x1p500), magnitude)));
      if (!v::all(safe)) {
        break;
      }
      const auto squared = v::add(v::mul(a, a), v::mul(b, b));
#if defined(__x86_64__)
      const auto h = _mm256_sqrt_pd(squared);
#else
      const auto h = vsqrtq_f64(squared);
#endif
      const auto inner = v::splat(controls[0]), outer = v::splat(controls[1]);
      const auto error = v::choose(chromatic_ge(h, outer), h, outer);
      const auto e = v::mul(v::splat(0x1p-48), error);
      const auto uncertain =
          feature::either(v::le(v::absolute(v::sub(h, inner)), e),
                          v::le(v::absolute(v::sub(h, outer)), e));
      if (!v::all(feature::equal(v::choose(uncertain, one, zero), zero))) {
        break;
      }
      auto chroma = v::sub(
          one, selection_smooth(feature::divide(
                   v::sub(h, inner), v::splat(controls[1] - controls[0]))));
      const auto lo = selection_smooth(
          feature::divide(v::sub(l, v::splat(controls[2])),
                          v::splat(controls[3] - controls[2])));
      const auto hi = selection_smooth(
          feature::divide(v::sub(l, v::splat(controls[4])),
                          v::splat(controls[5] - controls[4])));
      result = v::mul(chroma, v::mul(lo, v::sub(one, hi)));
    }
    v::save(output + i, result);
  }
  return i;
}
#undef PS_CHROMATIC_TARGET
#endif
#if defined(__x86_64__)
/** Bounded RGB wheels batches use the existing admitted SLEEF exp2 adapter.
 * Raw zone sums determine copy bits before normalization. EV normalization and
 * gain-then-multiply-then-add staging match the scalar path; guarded_gray keeps
 * its unchanged extreme/cancellation fallback and conservative gain budget.
 */
template <class T, class Z, unsigned C>
__attribute__((target("avx2,fma"))) inline bool wheels_batch(
    const T* input, const Z* weights, T* output, std::size_t n,
    const double* controls) {
  static_assert(C == 3 || C == 4);
  if (n < 16) {
    return false;
  }
  constexpr std::size_t kPixels = 64;
  alignas(32) double ev[3 * kPixels], offsets[3 * kPixels], gains[3 * kPixels];
  bool copies[3 * kPixels];
  T source[C * kPixels];
  for (std::size_t first = 0; first < n; first += kPixels) {
    const auto count = std::min(kPixels, n - first);
    std::memcpy(source, input + C * first, count * C * sizeof(T));
    for (std::size_t j = 0; j < count; ++j) {
      double sum = 0.0, raw_ev[3]{}, raw_offset[3]{};
      for (unsigned z = 0; z < 3; ++z) {
        const double weight = weights[3 * (first + j) + z];
        sum += weight;
        for (unsigned c = 0; c < 3; ++c) {
          raw_ev[c] += weight * controls[6 * z + 2 * c];
          raw_offset[c] += weight * controls[6 * z + 2 * c + 1];
        }
      }
      for (unsigned c = 0; c < 3; ++c) {
        const auto at = 3 * j + c;
        copies[at] = raw_ev[c] == 0.0 && raw_offset[c] == 0.0;
        ev[at] = copies[at] ? 0.0 : raw_ev[c] / sum;
        offsets[at] = copies[at] ? 0.0 : raw_offset[c] / sum;
      }
    }
    photospider_sleef_exp2(ev, gains, 3 * count);
    for (std::size_t j = 0; j < count; ++j) {
      unsigned c = 0;
      while (c < 3) {
        const auto at = 3 * j + c;
        auto* destination = output + C * (first + j) + c;
        if (c + 1 < 3 && !copies[at] && !copies[at + 1] &&
            tonal::guarded_gray_pair(source + C * j + c, ev + at, offsets + at,
                                     gains + at, destination)) {
          c += 2;
          continue;
        }
        if (copies[at]) {
          std::memcpy(destination, source + C * j + c, sizeof(T));
        } else {
          *destination = static_cast<T>(tonal::guarded_gray<T>(
              source[C * j + c], ev[at], offsets[at], gains[at]));
        }
        ++c;
      }
      if constexpr (C == 4) {
        std::memcpy(output + C * (first + j) + 3, source + C * j + 3,
                    sizeof(T));
      }
    }
  }
  return true;
}
#endif
template <unsigned Kind, class T, class Z, unsigned C>
inline __attribute__((always_inline)) void packed_span_impl(
    const T* x, const Z* weights, T* y, double* mask, std::size_t n,
    const double* controls, const double* lum, unsigned k = 3,
    bool normalize = true, const PreparedSaturation* prepared = nullptr) {
#pragma clang fp exceptions(ignore)
  ColorSelection selection;
  PreparedSaturation saturation;
  if constexpr (Kind == 0 && std::is_same_v<T, double>) {
    if (prepared) {
      saturation = *prepared;
    } else {
      saturation.prepare(controls[0], lum);
    }
  }
  if constexpr (Kind == 5) {
    selection.prepare(controls);
    std::size_t first = 0;
#if defined(__aarch64__) || defined(__x86_64__)
#if defined(__x86_64__)
    if (statistics::native_fma_available())
#endif
      first = selection_packets<5, T, C>(x, mask, n, controls, selection);
#endif
    if (selection.all_scaled) {
      color_selection_span<true, T, C>(selection, x + C * first, mask + first,
                                       n - first);
    } else {
      color_selection_span<false, T, C>(selection, x + C * first, mask + first,
                                        n - first);
    }
    return;
  }
#if defined(__x86_64__)
  if constexpr (Kind == 4) {
    if (statistics::native_fma_available() &&
        wheels_batch<T, Z, C>(x, weights, y, n, controls)) {
      return;
    }
  }
#endif
  std::size_t first = 0;
#if defined(__aarch64__) || defined(__x86_64__)
  if constexpr (Kind == 0 || Kind == 1 || Kind == 2) {
#if defined(__x86_64__)
    if (statistics::native_fma_available())
#endif
      first =
          saturation_packets<Kind, T, C>(x, y, n, controls, lum, saturation);
  } else if constexpr (Kind == 3 || Kind == 6) {
#if defined(__x86_64__)
    if (statistics::native_fma_available())
#endif
      first =
          zone_packets<Kind, T, Z, C>(x, weights, y, n, controls, k, normalize);
  } else if constexpr (Kind == 7) {
#if defined(__x86_64__)
    if (statistics::native_fma_available())
#endif
      first = selection_packets<7, T, C>(x, mask, n, controls, selection);
  }
#endif
  for (std::size_t i = first; i < n; ++i) {
    const double l = x[C * i], a = x[C * i + 1], b = x[C * i + 2];
    if constexpr (Kind == 5 || Kind == 7) {
      if constexpr (Kind == 5) {
        mask[i] = selection(l, a, b);
      } else {
        mask[i] = neutral_chroma_weight(a, b, chroma(a, b), controls[0],
                                        controls[1]) *
                  statistics::plateau(l, controls + 2);
      }
    } else {
      double v[3]{l, a, b};
      bool copy[3]{};
      if constexpr (Kind == 0) {
        const double s = controls[0];
        const bool same = s == 1 || (l == a && a == b);
        double source = saturation.active
                            ? saturation.offset(l, a, b)
                            : lum[0] * l + lum[1] * a + lum[2] * b;
        const bool prepared_pixel =
            saturation.active &&
            (saturation.safe_offset || std::isfinite(source));
        if (saturation.active && !prepared_pixel) {
          source = lum[0] * l + lum[1] * a + lum[2] * b;
        }
        for (unsigned c = 0; c < 3; ++c) {
          copy[c] = same;
          v[c] = prepared_pixel ? std::fma(s, v[c], source)
                                : source + s * (v[c] - source);
        }
      } else if constexpr (Kind == 1 || Kind == 2) {
        double gain = controls[0];
        bool same = gain == 1;
        if constexpr (Kind == 2) {
          const double q = std::min(1.0, chroma(a, b) / controls[1]);
          same = controls[0] == 0 || q == 1;
          gain = 1.0 + controls[0] * (1.0 - q) * (1.0 - q);
        }
        copy[0] = true;
        copy[1] = copy[2] = same;
        v[1] *= gain;
        v[2] *= gain;
      } else {
        const unsigned count = Kind == 6 ? k : 3;
        double sum = 0, delta[3]{}, ev[3]{};
        for (unsigned z = 0; z < count; ++z) {
          const double w = weights[count * i + z];
          sum += w;
          for (unsigned c = Kind == 3 ? 1 : 0; c < 3; ++c) {
            if constexpr (Kind == 4) {
              ev[c] += w * controls[6 * z + 2 * c];
              delta[c] += w * controls[6 * z + 2 * c + 1];
            } else {
              delta[c] +=
                  w * controls[(Kind == 3 ? 2 : 3) * z + c - (Kind == 3)];
            }
          }
        }
        const double denominator =
            Kind == 6 ? (normalize ? std::max(1.0, sum) : 1.0) : sum;
        for (unsigned c = 0; c < 3; ++c) {
          if constexpr (Kind == 4) {
            copy[c] = ev[c] == 0 && delta[c] == 0;
            v[c] =
                v[c] * std::exp2(ev[c] / denominator) + delta[c] / denominator;
          } else {
            copy[c] = delta[c] == 0 || (Kind == 3 && c == 0);
            v[c] += delta[c] / denominator;
          }
        }
      }
      for (unsigned c = 0; c < C; ++c) {
        if (c >= 3 || copy[c]) {
          std::memcpy(y + C * i + c, x + C * i + c, sizeof(T));
        } else {
          y[C * i + c] = static_cast<T>(v[c]);
        }
      }
    }
  }
}
#if defined(__x86_64__)
template <unsigned Kind, class T, class Z, unsigned C>
__attribute__((target("avx2,fma"))) inline void packed_span_fma(
    const T* x, const Z* weights, T* y, double* mask, std::size_t n,
    const double* controls, const double* lum, unsigned k, bool normalize,
    const PreparedSaturation* prepared) {
  packed_span_impl<Kind, T, Z, C>(x, weights, y, mask, n, controls, lum, k,
                                  normalize, prepared);
}
#endif
template <unsigned Kind, class T, class Z, unsigned C>
inline void packed_span(const T* x, const Z* weights, T* y, double* mask,
                        std::size_t n, const double* controls,
                        const double* lum, unsigned k = 3,
                        bool normalize = true,
                        const PreparedSaturation* prepared = nullptr) {
#if defined(__x86_64__)
  if (statistics::native_fma_available()) {
    packed_span_fma<Kind, T, Z, C>(x, weights, y, mask, n, controls, lum, k,
                                   normalize, prepared);
    return;
  }
#endif
  packed_span_impl<Kind, T, Z, C>(x, weights, y, mask, n, controls, lum, k,
                                  normalize, prepared);
}
}  // namespace ps::plugin_internal::grade::chromatic
