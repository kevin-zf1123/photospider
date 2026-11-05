// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

#include "07-grade/affine_math.hpp"

namespace ps::plugin_internal::grade::cdl {
using Triple = std::array<double, 3>;
struct Transform {
  Triple slope{1, 1, 1}, offset{}, power{1, 1, 1};
  double saturation = 1;
  bool reverse = false, clamp = true, identity = true, tabulated = false;
  std::array<std::array<double, 64>, 3> centers{};
  std::array<std::array<double, 33>, 3> scales{};
  std::array<std::array<double, 7>, 3> coefficients{};
  void prepare(std::size_t samples = 0) {
    identity = saturation == 1 && slope == Triple{1, 1, 1} &&
               offset == Triple{} && power == Triple{1, 1, 1};
    tabulated = !identity && samples >= 512;
    for (auto p : power)
      tabulated &= (reverse ? 1 / p : p) <= 4;
    if (!tabulated)
      return;
    for (unsigned c = 0; c < 3; ++c) {
      const double p = reverse ? 1 / power[c] : power[c];
      coefficients[c][0] = 1;
      for (unsigned degree = 1; degree <= 6; ++degree)
        coefficients[c][degree] =
            coefficients[c][degree - 1] * (p - degree + 1) / degree;
      for (unsigned j = 0; j < 64; ++j)
        centers[c][j] = std::pow(1 + (j + .5) / 64, p);
      for (unsigned j = 0; j < 33; ++j)
        scales[c][j] = std::exp2((static_cast<int>(j) - 16) * p);
    }
  }
#if defined(__aarch64__)
  inline static constexpr std::array<double, 64> inverse_centers = [] {
    std::array<double, 64> result{};
    for (unsigned j = 0; j < 64; ++j)
      result[j] = 1 / (1 + (j + .5) / 64);
    return result;
  }();
  void table_power(affine::Packet* values) const {
    using namespace affine;  // NOLINT(build/namespaces)
    Packet residual[3], scale[3], polynomial[3];
    for (unsigned c = 0; c < 3; ++c) {
      const auto m = mantissa(values[c]);
      const auto bits = vreinterpretq_u64_f64(values[c]);
      const auto indices = vandq_u64(vshrq_n_u64(bits, 46), vdupq_n_u64(63));
      const auto exponents =
          vsubq_u64(vshrq_n_u64(bits, 52), vdupq_n_u64(1007));
      const auto i = vgetq_lane_u64(indices, 0), j = vgetq_lane_u64(indices, 1);
      const auto center = vreinterpretq_f64_u64(
          vorrq_u64(vandq_u64(vreinterpretq_u64_f64(m),
                              vdupq_n_u64(~((UINT64_C(1) << 46) - 1))),
                    vdupq_n_u64(UINT64_C(1) << 45)));
      const double inverse[2]{inverse_centers[i], inverse_centers[j]};
      residual[c] = mul(sub(m, center), vld1q_f64(inverse));
      const double factors[2]{
          centers[c][i] * scales[c][vgetq_lane_u64(exponents, 0)],
          centers[c][j] * scales[c][vgetq_lane_u64(exponents, 1)]};
      scale[c] = vld1q_f64(factors);
      polynomial[c] = splat(coefficients[c][6]);
    }
    for (int degree = 5; degree >= 0; --degree)
      for (unsigned c = 0; c < 3; ++c)
        polynomial[c] =
            madd(polynomial[c], residual[c], splat(coefficients[c][degree]));
    for (unsigned c = 0; c < 3; ++c)
      values[c] = mul(polynomial[c], scale[c]);
  }
#endif
  Triple apply(Triple x) const {
    if (identity) {
      if (clamp)
        for (auto& v : x)
          v = std::clamp(v, 0.0, 1.0);
      return x;
    }
    if (reverse) {
      if (clamp)
        for (auto& v : x)
          v = std::clamp(v, 0.0, 1.0);
      if (saturation != 1) {
        const double l = (0.2126 * x[0] + 0.7152 * x[1]) + 0.0722 * x[2];
        for (auto& v : x)
          v = l + (v - l) / saturation;
      }
    }
    for (unsigned c = 0; c < 3; ++c) {
      double v = reverse ? x[c] : x[c] * slope[c] + offset[c];
      if (clamp)
        v = std::clamp(v, 0.0, 1.0);
      if (v >= 0 && power[c] != 1)
        v = std::pow(v, reverse ? 1 / power[c] : power[c]);
      x[c] = reverse ? (v - offset[c]) / slope[c] : v;
    }
    if (!reverse && saturation != 1) {
      const double l = (0.2126 * x[0] + 0.7152 * x[1]) + 0.0722 * x[2];
      for (auto& v : x)
        v = l + saturation * (v - l);
    }
    if (clamp)
      for (auto& v : x)
        v = std::clamp(v, 0.0, 1.0);
    return x;
  }
  template <class T, bool Packed>
  void dispatch(const T* source, T* destination, std::size_t n,
                unsigned channels, const unsigned* selected) const {
    if (reverse) {
      if (clamp)
        kernel<T, true, true, Packed>(source, destination, n, channels,
                                      selected);
      else
        kernel<T, true, false, Packed>(source, destination, n, channels,
                                       selected);
    } else {
      if (clamp)
        kernel<T, false, true, Packed>(source, destination, n, channels,
                                       selected);
      else
        kernel<T, false, false, Packed>(source, destination, n, channels,
                                        selected);
    }
  }
  template <class T>
  void span(const T* source, T* destination, std::size_t n, unsigned channels,
            const unsigned* selected) const {
    if (channels == 3 && selected[0] == 0 && selected[1] == 1 &&
        selected[2] == 2)
      dispatch<T, true>(source, destination, n, channels, selected);
    else
      dispatch<T, false>(source, destination, n, channels, selected);
  }
  template <class T, bool Reverse, bool Clamp, bool Packed>
  void kernel(const T* source, T* destination, std::size_t n, unsigned channels,
              const unsigned* selected) const {
    constexpr bool reverse = Reverse, clamp = Clamp, packed = Packed;
    if (identity && !clamp) {
      std::memcpy(destination, source, n * channels * sizeof(T));
      return;
    }
    const auto scalar = [&](std::size_t index) {
      const auto* s = source + index * channels;
      auto* d = destination + index * channels;
      std::memcpy(d, s, channels * sizeof(T));
      const auto value = apply({static_cast<double>(s[selected[0]]),
                                static_cast<double>(s[selected[1]]),
                                static_cast<double>(s[selected[2]])});
      for (unsigned c = 0; c < 3; ++c)
        d[selected[c]] = static_cast<T>(value[c]);
    };
    std::size_t j = 0;
#if defined(__aarch64__)
    bool vector_power = !identity && saturation >= .25 && saturation <= 4;
    if (reverse) {
      for (unsigned c = 0; c < 3; ++c)
        vector_power &=
            slope[c] >= .25 && slope[c] <= 4 && std::abs(offset[c]) <= 4;
    }
    for (auto p : power)
      vector_power &= (reverse ? 1 / p : p) <= 64;
    if (vector_power) {
      using namespace affine;  // NOLINT(build/namespaces)
      VectorControl controls[3]{};
      for (unsigned c = 0; c < 3; ++c)
        controls[c].power = splat(reverse ? 1 / power[c] : power[c]);
      for (; j + 2 <= n; j += 2) {
        Packet x[3], original[3], z[3];
        Mask keep[3];
        if (packed) {
          if constexpr (sizeof(T) == 4) {
            const auto tuple = vld3_f32(source + j * 3);
            for (unsigned c = 0; c < 3; ++c)
              x[c] = vcvt_f64_f32(tuple.val[c]);
          } else {
            const auto tuple = vld3q_f64(source + j * 3);
            for (unsigned c = 0; c < 3; ++c)
              x[c] = tuple.val[c];
          }
        } else {
          for (unsigned c = 0; c < 3; ++c) {
            double pair[2]{
                static_cast<double>(source[j * channels + selected[c]]),
                static_cast<double>(source[(j + 1) * channels + selected[c]])};
            x[c] = vld1q_f64(pair);
          }
        }
        if (reverse && clamp)
          for (auto& v : x)
            v = vminq_f64(vmaxq_f64(v, splat(0.0)), splat(1.0));
        if (reverse && saturation != 1) {
          const auto l =
              add(add(mul(splat(.2126), x[0]), mul(splat(.7152), x[1])),
                  mul(splat(.0722), x[2]));
          for (auto& v : x)
            v = add(l, vdivq_f64(sub(v, l), splat(saturation)));
        }
        for (unsigned c = 0; c < 3; ++c) {
          if (!reverse)
            x[c] = add(mul(x[c], splat(slope[c])), splat(offset[c]));
          if (clamp)
            x[c] = vminq_f64(vmaxq_f64(x[c], splat(0.0)), splat(1.0));
          original[c] = x[c];
          keep[c] = vcleq_f64(x[c], splat(0.0));
          if (power[c] == 1)
            keep[c] = vdupq_n_u64(UINT64_MAX);
          z[c] = choose(keep[c], splat(1.0), x[c]);
        }
        // Positive IEEE-754 values have the same order as their unsigned bits.
        // The upper bound also rejects either sign of NaN and negative values.
        auto valid = vdupq_n_u64(UINT64_MAX);
        const auto lower =
            vreinterpretq_u64_f64(splat(tabulated ? 0x1p-16 : 0x1p-1022));
        const auto upper = vreinterpretq_u64_f64(
            splat(tabulated ? 0x1.fffffffffffffp16 : 0x1p1023));
        for (auto v : z) {
          const auto bits = vreinterpretq_u64_f64(v);
          valid =
              both(valid, both(vcgeq_u64(bits, lower), vcleq_u64(bits, upper)));
        }
        bool safe = all(valid);
        if (tabulated) {
          if (safe)
            table_power(z);
        } else if (safe) {
          powers<double, 3>(z, controls);
          for (auto v : z)
            safe &= all(both(ge(v, splat(-1021.0)), le(v, splat(1022.0))));
          if (safe)
            exponentials<double, 3>(z);
        }
        if (!safe) {
          scalar(j);
          scalar(j + 1);
          continue;
        }
        for (unsigned c = 0; c < 3; ++c) {
          x[c] = choose(keep[c], original[c], z[c]);
          if (reverse) {
            const auto difference = sub(x[c], splat(offset[c]));
            safe &= all(ge(
                vabsq_f64(difference),
                mul(splat(sizeof(T) == 4 ? 0x1p-16
                          : tabulated    ? 0x1p-7
                                         : .25),
                    vmaxq_f64(vabsq_f64(x[c]), splat(std::abs(offset[c]))))));
            x[c] = vdivq_f64(difference, splat(slope[c]));
          }
        }
        const auto scale = vmaxq_f64(
            vmaxq_f64(vabsq_f64(x[0]), vabsq_f64(x[1])), vabsq_f64(x[2]));
        if (!reverse && saturation != 1) {
          const auto l =
              add(add(mul(splat(.2126), x[0]), mul(splat(.7152), x[1])),
                  mul(splat(.0722), x[2]));
          for (auto& v : x) {
            v = add(l, mul(splat(saturation), sub(v, l)));
            if (!clamp || saturation > 1)
              safe &= all(ge(add(vabsq_f64(v), splat(tabulated ? .0005 : .05)),
                             mul(scale, splat((sizeof(T) == 4 ? 0x1p-16
                                               : tabulated    ? 0x1p-7
                                                              : .125) *
                                              std::max(1.0, saturation)))));
          }
        }
        if (!safe) {
          scalar(j);
          scalar(j + 1);
          continue;
        }
        if (clamp)
          for (auto& v : x)
            v = vminq_f64(vmaxq_f64(v, splat(0.0)), splat(1.0));
        bool finite_output = true;
        if (!clamp)
          for (auto v : x)
            finite_output &= all(
                le(vabsq_f64(v),
                   splat(static_cast<double>(std::numeric_limits<T>::max()) *
                         (1 - 0x1p-32))));
        if (!finite_output) {
          scalar(j);
          scalar(j + 1);
          continue;
        }
        if (packed) {
          if constexpr (sizeof(T) == 4) {
            float32x2x3_t tuple;
            for (unsigned c = 0; c < 3; ++c)
              tuple.val[c] = vcvt_f32_f64(x[c]);
            vst3_f32(destination + j * 3, tuple);
          } else {
            float64x2x3_t tuple;
            for (unsigned c = 0; c < 3; ++c)
              tuple.val[c] = x[c];
            vst3q_f64(destination + j * 3, tuple);
          }
        } else {
          std::memcpy(destination + j * channels, source + j * channels,
                      2 * channels * sizeof(T));
          for (unsigned c = 0; c < 3; ++c) {
            double pair[2];
            vst1q_f64(pair, x[c]);
            destination[j * channels + selected[c]] = static_cast<T>(pair[0]);
            destination[(j + 1) * channels + selected[c]] =
                static_cast<T>(pair[1]);
          }
        }
      }
    }
#endif
    for (; j < n; ++j)
      scalar(j);
  }
};
}  // namespace ps::plugin_internal::grade::cdl
