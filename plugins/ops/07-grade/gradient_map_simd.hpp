// Copyright 2026 Photospider contributors
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <type_traits>

namespace ps::plugin_internal::grade::gradient {
#if defined(__aarch64__)
template <class T, unsigned Channels>
inline std::array<float64x2_t, Channels> load_gradient_pair(const T* p) {
  std::array<float64x2_t, Channels> value{};
  if constexpr (sizeof(T) == 4) {
    if constexpr (Channels == 1) {
      value[0] = vcvt_f64_f32(vld1_f32(p));
    } else if constexpr (Channels == 3) {
      const auto raw = vld3_f32(p);
      for (unsigned c = 0; c < Channels; ++c)
        value[c] = vcvt_f64_f32(raw.val[c]);
    } else {
      const auto raw = vld4_f32(p);
      for (unsigned c = 0; c < 3; ++c)
        value[c] = vcvt_f64_f32(raw.val[c]);
    }
  } else {
    if constexpr (Channels == 1)
      value[0] = vld1q_f64(p);
    else if constexpr (Channels == 3) {
      const auto raw = vld3q_f64(p);
      for (unsigned c = 0; c < Channels; ++c)
        value[c] = raw.val[c];
    } else {
      const auto raw = vld4q_f64(p);
      for (unsigned c = 0; c < Channels; ++c)
        value[c] = raw.val[c];
    }
  }
  return value;
}
inline float64x2_t gradient_dot(const fast_color::Triple& row,
                                const std::array<float64x2_t, 3>& rgb) {
  return vfmaq_n_f64(vfmaq_n_f64(vmulq_n_f64(rgb[0], row[0]), rgb[1], row[1]),
                     rgb[2], row[2]);
}
inline float64x2_t gradient_roots(float64x2_t x) {
  return {std::cbrt(vgetq_lane_f64(x, 0)), std::cbrt(vgetq_lane_f64(x, 1))};
}
template <unsigned Mode>
inline float64x2_t gradient_feature(const Feature& feature,
                                    const std::array<float64x2_t, 3>& rgb) {
  if constexpr (Mode == 0)
    return vmaxq_f64(rgb[0], vmaxq_f64(rgb[1], rgb[2]));
  if constexpr (Mode == 3) {
    std::array<float64x2_t, 3> roots;
    for (unsigned c = 0; c < 3; ++c)
      roots[c] = gradient_roots(gradient_dot(feature.rgb_to_lms[c], rgb));
    return gradient_dot(fast_color::kLmsToOklab[0], roots);
  }
  const auto y = gradient_dot(feature.rgb_to_xyz[1], rgb);
  if constexpr (Mode == 1)
    return y;
  const auto high =
      vdivq_f64(vsubq_f64(vmulq_n_f64(gradient_roots(y), 116), vdupq_n_f64(16)),
                vdupq_n_f64(100));
  const auto low = vdivq_f64(vmulq_n_f64(y, 24389.0 / 27), vdupq_n_f64(100));
  return vbslq_f64(native_simd::greater(y, vdupq_n_f64(216.0 / 24389)), high,
                   low);
}
// A shared branch-free interval lookup serves all three destination columns.
// This native path keeps Float32 inputs in binary64 until publication.
template <unsigned SourceChannels, unsigned Mode, class T>
inline void small_gradient_span(const T* source, T* output, std::size_t count,
                                const Feature& feature,
                                const fast_curve::Curve* curves) {
  const auto& axis = curves[0];
  const auto xs = fast_curve::table8(axis.x, axis.size, INFINITY);
  std::array<uint8x16x4_t, 3> ys, slopes;
  for (unsigned c = 0; c < 3; ++c) {
    ys[c] = fast_curve::table8(curves[c].y, axis.size);
    slopes[c] = fast_curve::table8(curves[c].a, axis.size - 1);
  }
  const auto middle = vdupq_n_f64(axis.size > 4 ? axis.x[4] : INFINITY);
  const auto cap = vdupq_n_u8(static_cast<unsigned char>(8 * (axis.size - 2)));
  const auto first = vdupq_n_f64(axis.x[0]),
             last = vdupq_n_f64(axis.x[axis.size - 1]), zero = vdupq_n_f64(0);
  constexpr unsigned OutputChannels = SourceChannels == 1 ? 3 : SourceChannels;
  std::size_t j = 0;
  for (; j + 2 <= count; j += 2) {
    const auto input =
        load_gradient_pair<T, SourceChannels>(source + j * SourceChannels);
    float64x2_t query;
    if constexpr (SourceChannels == 1)
      query = input[0];
    else
      query = gradient_feature<Mode>(feature, {input[0], input[1], input[2]});
    auto index = vbslq_u8(
        vreinterpretq_u8_u64(native_simd::greater_equal(query, middle)),
        vdupq_n_u8(32), vdupq_n_u8(0));
    for (unsigned step : {16U, 8U}) {
      const auto mid =
          vaddq_u8(index, vdupq_n_u8(static_cast<unsigned char>(step)));
      index = vbslq_u8(vreinterpretq_u8_u64(native_simd::greater_equal(
                           query, fast_curve::gather8(xs, mid))),
                       mid, index);
    }
    index = vminq_u8(index, cap);
    const auto x = fast_curve::gather8(xs, index), delta = vsubq_f64(query, x);
    std::array<float64x2_t, 3> result;
    for (unsigned c = 0; c < 3; ++c) {
      const auto y = fast_curve::gather8(ys[c], index);
      auto v = vfmaq_f64(y, delta, fast_curve::gather8(slopes[c], index));
      if (curves[c].identity)
        v = vbslq_f64(native_simd::equal(query, zero), zero, query);
      else if (curves[c].constant)
        v = vdupq_n_f64(curves[c].identity_or_constant(0));
      v = vbslq_f64(native_simd::equal(query, x), y, v);
      v = vbslq_f64(native_simd::less_equal(query, first),
                    vdupq_n_f64(curves[c].y[0]), v);
      result[c] = vbslq_f64(native_simd::greater_equal(query, last),
                            vdupq_n_f64(curves[c].y[axis.size - 1]), v);
    }
    auto* target = output + j * OutputChannels;
    if constexpr (sizeof(T) == 4) {
      const float32x2x3_t rgb{{vcvt_f32_f64(result[0]), vcvt_f32_f64(result[1]),
                               vcvt_f32_f64(result[2])}};
      if constexpr (OutputChannels == 3) {
        vst3_f32(target, rgb);
      } else {
        const auto alpha = vld4_f32(source + j * SourceChannels).val[3];
        const float32x2x4_t rgba{{rgb.val[0], rgb.val[1], rgb.val[2], alpha}};
        vst4_f32(target, rgba);
      }
    } else {
      if constexpr (OutputChannels == 3) {
        const float64x2x3_t rgb{{result[0], result[1], result[2]}};
        vst3q_f64(target, rgb);
      } else {
        const float64x2x4_t rgba{{result[0], result[1], result[2], input[3]}};
        vst4q_f64(target, rgba);
      }
    }
  }
  for (; j < count; ++j) {
    double query;
    if constexpr (SourceChannels == 1)
      query = source[j];
    else
      query =
          feature({source[j * SourceChannels], source[j * SourceChannels + 1],
                   source[j * SourceChannels + 2]});
    const auto rgb = evaluate(curves, query);
    for (unsigned c = 0; c < 3; ++c)
      output[j * OutputChannels + c] = static_cast<T>(rgb[c]);
    if constexpr (OutputChannels == 4)
      std::memcpy(output + j * 4 + 3, source + j * 4 + 3, sizeof(T));
  }
}
#endif
template <class T, unsigned Channels>
inline bool small_image_span(const T* source, T* output, std::size_t count,
                             const Feature& feature,
                             const fast_curve::Curve* curves) {
#if defined(__aarch64__)
  if (curves[0].size <= 8 && count >= 16) {
    switch (feature.mode) {
      case 0:
        small_gradient_span<Channels, 0>(source, output, count, feature,
                                         curves);
        break;
      case 1:
        small_gradient_span<Channels, 1>(source, output, count, feature,
                                         curves);
        break;
      case 2:
        small_gradient_span<Channels, 2>(source, output, count, feature,
                                         curves);
        break;
      default:
        small_gradient_span<Channels, 3>(source, output, count, feature,
                                         curves);
        break;
    }
    return true;
  }
#endif
  return false;
}
template <class T>
inline bool small_feature_span(const T* source, T* output, std::size_t count,
                               const fast_curve::Curve* curves) {
#if defined(__aarch64__)
  if (curves[0].size <= 8 && count >= 16) {
    small_gradient_span<1, 0>(source, output, count, Feature{}, curves);
    return true;
  }
#endif
  return false;
}
}  // namespace ps::plugin_internal::grade::gradient
