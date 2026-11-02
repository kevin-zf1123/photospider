// Copyright 2026 Photospider contributors
#pragma once

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace ps::plugin_internal::grade::finishing {
#if defined(__aarch64__)
template <unsigned Kind>
inline std::array<float64x2_t, 3> packet_apply(
    const Transform& op, const std::array<float64x2_t, 3>& x) {
  const auto dot = [&](const Triple& w) {
    return vfmaq_n_f64(vfmaq_n_f64(vmulq_n_f64(x[0], w[0]), x[1], w[1]), x[2],
                       w[2]);
  };
  if constexpr (Kind == 0) {
    return {op.gains[0] == 1 ? x[0] : vmulq_n_f64(x[0], op.gains[0]),
            op.gains[1] == 1 ? x[1] : vmulq_n_f64(x[1], op.gains[1]),
            op.gains[2] == 1 ? x[2] : vmulq_n_f64(x[2], op.gains[2])};
  } else if constexpr (Kind <= 2) {
    return {dot(op.matrix[0]), dot(op.matrix[1]), dot(op.matrix[2])};
  } else if constexpr (Kind == 3) {
    auto y = vdupq_n_f64(op.bias);
    if (op.divisor == 1) {
      for (unsigned c = 0; c < 3; ++c)
        y = vfmaq_n_f64(y, x[c], op.weights[c]);
    } else {
      y = vaddq_f64(vdivq_f64(dot(op.weights), vdupq_n_f64(op.divisor)), y);
    }
    return {y, y, y};
  } else if constexpr (Kind == 4) {
    auto y = dot(op.forward[1]);
    return {y, y, y};
  } else if constexpr (Kind == 5) {
    auto y = dot(op.forward[1]);
    double lanes[2];
    vst1q_f64(lanes, y);
    for (auto& lane : lanes) {
      const double lightness = 1.16 * fast_color::cie_f(lane) - .16;
      lane = fast_color::cie_g((lightness + .16) / 1.16);
    }
    y = vld1q_f64(lanes);
    std::array<float64x2_t, 3> xyz{vmulq_n_f64(y, op.white[0]), y,
                                   vmulq_n_f64(y, op.white[2])};
    std::array<float64x2_t, 3> result;
    for (unsigned r = 0; r < 3; ++r)
      result[r] = vaddq_f64(vaddq_f64(vmulq_n_f64(xyz[0], op.inverse[r][0]),
                                      vmulq_n_f64(xyz[1], op.inverse[r][1])),
                            vmulq_n_f64(xyz[2], op.inverse[r][2]));
    return result;
  } else {
    std::array<float64x2_t, 3> roots{dot(op.to_lms[0]), dot(op.to_lms[1]),
                                     dot(op.to_lms[2])};
    for (auto& root : roots) {
      double lanes[2];
      vst1q_f64(lanes, root);
      lanes[0] = std::cbrt(lanes[0]);
      lanes[1] = std::cbrt(lanes[1]);
      root = vld1q_f64(lanes);
    }
    const auto& row = fast_color::kLmsToOklab[0];
    const auto lightness = vaddq_f64(
        vaddq_f64(vmulq_n_f64(roots[0], row[0]), vmulq_n_f64(roots[1], row[1])),
        vmulq_n_f64(roots[2], row[2]));
    std::array<float64x2_t, 3> cubes, result;
    for (unsigned r = 0; r < 3; ++r) {
      const auto v = vmulq_n_f64(lightness, op.neutral_roots[r]);
      cubes[r] = vmulq_f64(vmulq_f64(v, v), v);
    }
    for (unsigned r = 0; r < 3; ++r)
      result[r] = vaddq_f64(vaddq_f64(vmulq_n_f64(cubes[0], op.from_lms[r][0]),
                                      vmulq_n_f64(cubes[1], op.from_lms[r][1])),
                            vmulq_n_f64(cubes[2], op.from_lms[r][2]));
    return result;
  }
}
template <unsigned Kind, class T, unsigned C>
inline std::size_t packet_span(const Transform& op, const T* __restrict source,
                               T* __restrict target, std::size_t count) {
  std::size_t i = 0;
  if constexpr (Kind == 3 && sizeof(T) == 8) {
    if (op.divisor == 1) {
      const auto w0 = vdupq_n_f64(op.weights[0]),
                 w1 = vdupq_n_f64(op.weights[1]),
                 w2 = vdupq_n_f64(op.weights[2]), bias = vdupq_n_f64(op.bias);
      for (; i + 8 <= count; i += 8) {
        for (unsigned j = 0; j < 4; ++j) {
          const auto at = i + 2 * j;
          std::array<float64x2_t, 3> x;
          if constexpr (C == 3) {
            auto v = vld3q_f64(source + C * at);
            x = {v.val[0], v.val[1], v.val[2]};
          } else {
            auto v = vld4q_f64(source + C * at);
            x = {v.val[0], v.val[1], v.val[2]};
          }
          auto y = vfmaq_f64(bias, x[0], w0);
          y = vfmaq_f64(y, x[1], w1);
          y = vfmaq_f64(y, x[2], w2);
          vst1q_f64(target + at, y);
        }
      }
    }
  }
  if constexpr (Kind == 0 && sizeof(T) == 8) {
    if constexpr (C == 3) {
      if (op.gains[0] != 1 && op.gains[1] != 1 && op.gains[2] != 1) {
        const double factors[6] = {op.gains[0], op.gains[1], op.gains[2],
                                   op.gains[0], op.gains[1], op.gains[2]};
        const float64x2_t a = vld1q_f64(factors), b = vld1q_f64(factors + 2),
                          c = vld1q_f64(factors + 4);
        for (; i + 8 <= count; i += 8) {
          for (unsigned j = 0; j < 4; ++j) {
            const auto at = 3 * i + 6 * j;
            vst1q_f64(target + at, vmulq_f64(vld1q_f64(source + at), a));
            vst1q_f64(target + at + 2,
                      vmulq_f64(vld1q_f64(source + at + 2), b));
            vst1q_f64(target + at + 4,
                      vmulq_f64(vld1q_f64(source + at + 4), c));
          }
        }
      }
    }
  }
  if constexpr (sizeof(T) == 8) {
    for (; i + 2 <= count; i += 2) {
      std::array<float64x2_t, 3> x;
      float64x2_t alpha{};
      if constexpr (C == 3) {
        auto input = vld3q_f64(source + C * i);
        x = {input.val[0], input.val[1], input.val[2]};
      } else {
        auto input = vld4q_f64(source + C * i);
        x = {input.val[0], input.val[1], input.val[2]};
        alpha = input.val[3];
      }
      const auto y = packet_apply<Kind>(op, x);
      if constexpr (Kind == 3)
        vst1q_f64(target + i, y[0]);
      else if constexpr (C == 3) {
        const float64x2x3_t out{{y[0], y[1], y[2]}};
        vst3q_f64(target + C * i, out);
      } else {
        const float64x2x4_t out{{y[0], y[1], y[2], alpha}};
        vst4q_f64(target + C * i, out);
      }
    }
  } else {
    for (; i + 4 <= count; i += 4) {
      std::array<float32x4_t, 3> x;
      float32x4_t alpha{};
      if constexpr (C == 3) {
        auto input = vld3q_f32(source + C * i);
        x = {input.val[0], input.val[1], input.val[2]};
      } else {
        auto input = vld4q_f32(source + C * i);
        x = {input.val[0], input.val[1], input.val[2]};
        alpha = input.val[3];
      }
      std::array<float64x2_t, 3> lo, hi;
      for (unsigned r = 0; r < 3; ++r) {
        lo[r] = vcvt_f64_f32(vget_low_f32(x[r]));
        hi[r] = vcvt_f64_f32(vget_high_f32(x[r]));
      }
      const auto a = packet_apply<Kind>(op, lo), b = packet_apply<Kind>(op, hi);
      std::array<float32x4_t, 3> y;
      for (unsigned r = 0; r < 3; ++r)
        y[r] = vcombine_f32(vcvt_f32_f64(a[r]), vcvt_f32_f64(b[r]));
      if constexpr (Kind == 0) {
        for (unsigned r = 0; r < 3; ++r)
          if (op.gains[r] == 1)
            y[r] = x[r];
      }
      if constexpr (Kind == 3)
        vst1q_f32(target + i, y[0]);
      else if constexpr (C == 3) {
        const float32x4x3_t out{{y[0], y[1], y[2]}};
        vst3q_f32(target + C * i, out);
      } else {
        const float32x4x4_t out{{y[0], y[1], y[2], alpha}};
        vst4q_f32(target + C * i, out);
      }
    }
  }
  return i;
}
template <class T, unsigned C>
inline std::size_t posterize_packets(const Transform& op, const T* source,
                                     T* target, std::size_t count) {
  std::size_t i = 0;
  if constexpr (sizeof(T) == 4) {
    const auto n = vdupq_n_f32(static_cast<float>(op.levels));
    const auto quantize = [&](float32x4_t v) {
      if (op.clamp)
        v = vminq_f32(vdupq_n_f32(1), vmaxq_f32(vdupq_n_f32(0), v));
      return vdivq_f32(vrndnq_f32(vmulq_f32(v, n)), n);
    };
    for (; i + 4 <= count; i += 4) {
      if constexpr (C == 3) {
        for (unsigned j = 0; j < 3; ++j)
          vst1q_f32(target + C * i + 4 * j,
                    quantize(vld1q_f32(source + C * i + 4 * j)));
      } else {
        auto v = vld4q_f32(source + C * i);
        for (unsigned j = 0; j < 3; ++j)
          v.val[j] = quantize(v.val[j]);
        vst4q_f32(target + C * i, v);
      }
    }
  } else {
    const auto n = vdupq_n_f64(op.levels);
    const auto quantize = [&](float64x2_t v) {
      if (op.clamp)
        v = vminq_f64(vdupq_n_f64(1), vmaxq_f64(vdupq_n_f64(0), v));
      return vdivq_f64(vrndnq_f64(vmulq_f64(v, n)), n);
    };
    for (; i + 2 <= count; i += 2) {
      if constexpr (C == 3) {
        for (unsigned j = 0; j < 3; ++j)
          vst1q_f64(target + C * i + 2 * j,
                    quantize(vld1q_f64(source + C * i + 2 * j)));
      } else {
        auto v = vld4q_f64(source + C * i);
        for (unsigned j = 0; j < 3; ++j)
          v.val[j] = quantize(v.val[j]);
        vst4q_f64(target + C * i, v);
      }
    }
  }
  return i;
}
template <class T, unsigned C>
inline std::size_t dispatch_packets(const Transform& op, const T* source,
                                    T* target, std::size_t count) {
  switch (op.kind) {
    case 0:
      return packet_span<0, T, C>(op, source, target, count);
    case 1:
      return packet_span<1, T, C>(op, source, target, count);
    case 2:
      return packet_span<2, T, C>(op, source, target, count);
    case 3:
      return packet_span<3, T, C>(op, source, target, count);
    case 4:
      return packet_span<4, T, C>(op, source, target, count);
    case 5:
      return packet_span<5, T, C>(op, source, target, count);
    case 6:
      return packet_span<6, T, C>(op, source, target, count);
    case 7:
      return posterize_packets<T, C>(op, source, target, count);
    default:
      return 0;
  }
}
#endif
}  // namespace ps::plugin_internal::grade::finishing
