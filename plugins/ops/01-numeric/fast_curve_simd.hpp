// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "01-numeric/fast_native_simd.hpp"
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace ps::plugin_internal::fast_curve {
#if defined(__aarch64__)
inline uint8x16x4_t table8(const double* values, std::size_t count,
                           double pad = 0) {
  if (count == 8) {
    const auto a = vld1q_f64(values), b = vld1q_f64(values + 2);
    const auto c = vld1q_f64(values + 4), d = vld1q_f64(values + 6);
    return {{vreinterpretq_u8_f64(a), vreinterpretq_u8_f64(b),
             vreinterpretq_u8_f64(c), vreinterpretq_u8_f64(d)}};
  }
  std::array<double, 8> copied{};
  std::fill(copied.begin(), copied.end(), pad);
  std::copy_n(values, count, copied.begin());
  return table8(copied.data(), 8);
}
inline float64x2_t gather8(uint8x16x4_t table, uint8x16_t offset) {
  const std::uint8_t bytes[]{0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7};
  return vreinterpretq_f64_u8(
      vqtbl4q_u8(table, vaddq_u8(offset, vld1q_u8(bytes))));
}
template <class T>
inline float64x2_t load_pair(const T* input) {
  if constexpr (sizeof(T) == 4) {
    return vcvt_f64_f32(vld1_f32(input));
  } else {
    return vld1q_f64(input);
  }
}
template <class T>
inline void store_pair(T* output, float64x2_t value) {
  if constexpr (sizeof(T) == 4) {
    vst1_f32(output, vcvt_f32_f64(value));
  } else {
    vst1q_f64(output, value);
  }
}
template <bool Cubic, bool Shortcut, class Input, class Output>
inline void small_span(const Curve& curve, const Input* input, Output* output,
                       std::size_t count) {
  const auto xs = table8(curve.x, curve.size, INFINITY);
  const auto ys = table8(curve.y, curve.size);
  std::array<double, 8> ca{}, cb{}, ccoeff{};
  bool scaled = true;
  for (std::size_t k = 0; k + 1 < curve.size; ++k) {
    const double h = Cubic ? curve.slope[k] : 1;
    ca[k] = curve.a[k] * h;
    cb[k] = Cubic ? (curve.b[k] * h) * h : 0;
    ccoeff[k] = Cubic ? ((curve.c[k] * h) * h) * h : 0;
    scaled &= std::isfinite(ca[k]) && std::isfinite(cb[k]) &&
              std::isfinite(ccoeff[k]) && (ca[k] != 0 || curve.a[k] == 0) &&
              (!Cubic || cb[k] != 0 || curve.b[k] == 0) &&
              (!Cubic || ccoeff[k] != 0 || curve.c[k] == 0);
  }
  if (!scaled) {
    for (std::size_t j = 0; j < count; ++j) {
      output[j] = static_cast<Output>(curve.evaluate(input[j]));
    }
    return;
  }
  const auto aa = table8(ca.data(), 8);
  const auto bb = table8(cb.data(), 8);
  const auto cc = table8(ccoeff.data(), 8);
  const auto middle = vdupq_n_f64(curve.size > 4 ? curve.x[4] : INFINITY);
  const auto first = vdupq_n_f64(curve.x[0]),
             last = vdupq_n_f64(curve.x[curve.size - 1]);
  const auto yfirst = vdupq_n_f64(curve.y[0]),
             ylast = vdupq_n_f64(curve.y[curve.size - 1]);
  const auto cap = vdupq_n_u8(static_cast<unsigned char>(8 * (curve.size - 2)));
  std::size_t j = 0;
  for (; j + 2 <= count; j += 2) {
    const auto q = load_pair(input + j);
    auto index =
        vbslq_u8(vreinterpretq_u8_u64(native_simd::greater_equal(q, middle)),
                 vdupq_n_u8(32), vdupq_n_u8(0));
    for (unsigned step : {16U, 8U}) {
      const auto mid =
          vaddq_u8(index, vdupq_n_u8(static_cast<unsigned char>(step)));
      const auto yes = native_simd::greater_equal(q, gather8(xs, mid));
      index = vbslq_u8(vreinterpretq_u8_u64(yes), mid, index);
    }
    index = vminq_u8(index, cap);
    const auto x = gather8(xs, index), y = gather8(ys, index);
    float64x2_t value;
    if constexpr (Shortcut) {
      if (curve.identity) {
        const auto zero = vdupq_n_f64(0);
        value = vbslq_f64(native_simd::equal(q, zero), zero, q);
      } else {
        value = vdupq_n_f64(curve.identity_or_constant(0));
        if (curve.edge == Edge::Linear && curve.y[0] == 0) {
          value = vbslq_f64(native_simd::less(q, first),
                            vdupq_n_f64(curve.first_constant), value);
          value = vbslq_f64(native_simd::greater(q, last),
                            vdupq_n_f64(curve.last_constant), value);
        }
      }
    } else {
      const auto delta = vsubq_f64(q, x);
      if constexpr (Cubic) {
        value = vfmaq_f64(y, delta,
                          vfmaq_f64(gather8(aa, index), delta,
                                    vfmaq_f64(gather8(bb, index), delta,
                                              gather8(cc, index))));
      } else {
        value = vfmaq_f64(y, delta, gather8(aa, index));
      }
    }
    value = vbslq_f64(native_simd::equal(q, x), y, value);
    if (curve.edge == Edge::Linear) {
      if constexpr (!Shortcut) {
        const auto low =
            vfmaq_n_f64(yfirst, vsubq_f64(q, first), curve.first_slope);
        const auto high =
            vfmaq_n_f64(ylast, vsubq_f64(q, last), curve.last_slope);
        value = vbslq_f64(native_simd::less(q, first), low, value);
        value = vbslq_f64(native_simd::greater(q, last), high, value);
      }
      value = vbslq_f64(native_simd::equal(q, first), yfirst, value);
      value = vbslq_f64(native_simd::equal(q, last), ylast, value);
    } else {
      value = vbslq_f64(native_simd::less_equal(q, first), yfirst, value);
      value = vbslq_f64(native_simd::greater_equal(q, last), ylast, value);
    }
    store_pair(output + j, value);
  }
  for (; j < count; ++j) {
    output[j] = static_cast<Output>(curve.evaluate(input[j]));
  }
}
#endif

template <class Input, class Output>
inline bool small_curve_span(const Curve& curve, const Input* input,
                             Output* output, std::size_t count) {
#if defined(__aarch64__)
  if (curve.size <= 8 && count >= 16) {
    if (curve.cubic) {
      if (curve.identity || curve.constant)
        small_span<true, true>(curve, input, output, count);
      else
        small_span<true, false>(curve, input, output, count);
    } else {
      if (curve.identity || curve.constant)
        small_span<false, true>(curve, input, output, count);
      else
        small_span<false, false>(curve, input, output, count);
    }
    return true;
  }
#endif
  return false;
}

#if defined(__aarch64__)
template <unsigned Channels, bool Cubic, class Input, class Output>
inline void ordered_span(const Curve* curves, const Input* input,
                         Output* output, std::size_t count, int direction) {
  const auto& axis = curves[0];
  std::size_t begin = 0;
  while (begin < count) {
    const double q = input[begin];
    if (!(q > axis.x[0] && q < axis.x[axis.size - 1])) {
      for (unsigned c = 0; c < Channels; ++c) {
        output[begin * Channels + c] =
            static_cast<Output>(curves[c].evaluate(q));
      }
      ++begin;
      continue;
    }
    const auto interval = axis.segment(q);
    std::size_t low = begin + 1, high = count;
    while (low < high) {
      const auto mid = low + (high - low) / 2;
      const bool inside = direction > 0 ? input[mid] < axis.x[interval + 1]
                                        : input[mid] >= axis.x[interval];
      if (inside) {
        low = mid + 1;
      } else {
        high = mid;
      }
    }
    const auto end = low;
    const auto origin = vdupq_n_f64(axis.x[interval]);
    const auto inverse = vdupq_n_f64(axis.slope[interval]);
    const auto zero = vdupq_n_f64(0);
    std::array<float64x2_t, Channels> a, b, c, y;
    for (unsigned k = 0; k < Channels; ++k) {
      a[k] = vdupq_n_f64(curves[k].a[interval]);
      b[k] = vdupq_n_f64(Cubic ? curves[k].b[interval] : 0);
      c[k] = vdupq_n_f64(Cubic ? curves[k].c[interval] : 0);
      y[k] = vdupq_n_f64(curves[k].y[interval]);
    }
    for (; begin + 2 <= end; begin += 2) {
      const auto query = load_pair(input + begin);
      const auto delta = vsubq_f64(query, origin);
      const auto t = Cubic ? vmulq_f64(delta, inverse) : delta;
      std::array<float64x2_t, Channels> value;
      for (unsigned k = 0; k < Channels; ++k) {
        if constexpr (Cubic) {
          value[k] =
              vfmaq_f64(y[k], t, vfmaq_f64(a[k], t, vfmaq_f64(b[k], t, c[k])));
        } else {
          value[k] = vfmaq_f64(y[k], t, a[k]);
        }
        if (curves[k].identity) {
          value[k] = vbslq_f64(native_simd::equal(query, zero), zero, query);
        } else if (curves[k].constant) {
          value[k] = vdupq_n_f64(curves[k].identity_or_constant(0));
        }
        value[k] = vbslq_f64(native_simd::equal(query, origin), y[k], value[k]);
      }
      if constexpr (Channels == 1) {
        store_pair(output + begin, value[0]);
      } else if constexpr (sizeof(Output) == 4) {
        const float32x2x3_t packed{{vcvt_f32_f64(value[0]),
                                    vcvt_f32_f64(value[1]),
                                    vcvt_f32_f64(value[2])}};
        vst3_f32(output + 3 * begin, packed);
      } else {
        const float64x2x3_t packed{{value[0], value[1], value[2]}};
        vst3q_f64(output + 3 * begin, packed);
      }
    }
    for (; begin < end; ++begin) {
      for (unsigned k = 0; k < Channels; ++k) {
        output[begin * Channels + k] =
            static_cast<Output>(curves[k].evaluate(input[begin]));
      }
    }
  }
}
#endif
template <class Input, class Output>
inline bool ordered_curve_span(const Curve* curves, unsigned columns,
                               const Input* input, Output* output,
                               std::size_t count, int direction) {
#if defined(__aarch64__)
  if (count >= 16 && (columns == 1 || columns == 3)) {
    if (columns == 1) {
      if (curves[0].cubic) {
        ordered_span<1, true>(curves, input, output, count, direction);
      } else {
        ordered_span<1, false>(curves, input, output, count, direction);
      }
    } else {
      if (curves[0].cubic) {
        ordered_span<3, true>(curves, input, output, count, direction);
      } else {
        ordered_span<3, false>(curves, input, output, count, direction);
      }
    }
    return true;
  }
#endif
  return false;
}

template <class Input, class Output>
inline void lut_span(const Input* input, Output* output, std::size_t count,
                     const double* values, std::size_t size, double minimum,
                     double maximum, Edge edge) {
  const double factor = size > 1 ? (size - 1) / (maximum - minimum) : 1;
  if (!std::isfinite(factor) || factor == 0) {
    for (std::size_t j = 0; j < count; ++j)
      output[j] = static_cast<Output>(
          lut(input[j], values, size, minimum, maximum, edge));
    return;
  }
#if defined(__x86_64__)
  if (native_compute::available()) {
    native_compute::lut(sizeof(Input) == 4, sizeof(Output) == 4, input, output,
                        count, values, size, minimum, maximum,
                        static_cast<unsigned>(edge));
    return;
  }
#endif
  if (size == 1) {
    std::fill_n(output, count, static_cast<Output>(values[0]));
    return;
  }
  std::size_t j = 0;
#if defined(__aarch64__)
  const auto low = vdupq_n_f64(minimum), high = vdupq_n_f64(maximum);
  const auto scale = vdupq_n_f64((size - 1) / (maximum - minimum));
  const auto cap = vdupq_n_f64(static_cast<double>(size - 2));
  const auto limit = vdupq_n_f64(static_cast<double>(size - 1));
  const auto zero = vdupq_n_f64(0), one = vdupq_n_f64(1);
  for (; j + 2 <= count; j += 2) {
    const auto q = load_pair(input + j);
    auto u = vmulq_f64(vsubq_f64(q, low), scale);
    if (edge == Edge::Linear) {
      const auto infinite = vceqq_f64(vabsq_f64(u), vdupq_n_f64(HUGE_VAL));
      if (vgetq_lane_u64(infinite, 0) || vgetq_lane_u64(infinite, 1)) {
        for (unsigned k = 0; k < 2; ++k)
          output[j + k] = static_cast<Output>(
              lut(input[j + k], values, size, minimum, maximum, edge));
        continue;
      }
    }
    if (edge != Edge::Linear) {
      u = vminq_f64(limit, vmaxq_f64(zero, u));
    }
    const auto position = vminq_f64(cap, vmaxq_f64(zero, vrndmq_f64(u)));
    const auto indexes = vcvtq_u64_f64(position);
    const auto i0 = vgetq_lane_u64(indexes, 0), i1 = vgetq_lane_u64(indexes, 1);
    const double left[]{values[i0], values[i1]},
        right[]{values[i0 + 1], values[i1 + 1]};
    const auto a = vld1q_f64(left), b = vld1q_f64(right),
               t = vsubq_f64(u, position);
    auto value = vfmaq_f64(a, t, vsubq_f64(b, a));
    value = vbslq_f64(native_simd::equal(t, zero), a, value);
    value = vbslq_f64(native_simd::equal(t, one), b, value);
    value =
        vbslq_f64(native_simd::equal(q, low), vdupq_n_f64(values[0]), value);
    value = vbslq_f64(native_simd::equal(q, high),
                      vdupq_n_f64(values[size - 1]), value);
    store_pair(output + j, value);
  }
#endif
  for (; j < count; ++j) {
    output[j] = static_cast<Output>(
        lut(input[j], values, size, minimum, maximum, edge));
  }
}
template <class Input, class Output>
inline void lut_strided_span(const Input* input, Output* output,
                             std::size_t count, const double* values,
                             std::size_t size, double start, double end,
                             Edge edge, std::ptrdiff_t input_step,
                             std::ptrdiff_t output_step) {
#if defined(__x86_64__)
  if (native_compute::available()) {
    native_compute::lut(sizeof(Input) == 4, sizeof(Output) == 4, input, output,
                        count, values, size, start, end,
                        static_cast<unsigned>(edge), input_step, output_step);
    return;
  }
#endif
  if (input_step == 1 && output_step == 1) {
    lut_span(input, output, count, values, size, start, end, edge);
    return;
  }
  for (std::size_t i = 0; i < count; ++i) {
    output[i * output_step] = static_cast<Output>(
        size == 1 ? values[0]
                  : lut(input[i * input_step], values, size, start, end, edge));
  }
}
}  // namespace ps::plugin_internal::fast_curve
