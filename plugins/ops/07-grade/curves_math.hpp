// Copyright 2026 Photospider contributors
#pragma once

#include <array>
#include <cstddef>

#include "01-numeric/fast_curve.hpp"
#include "02-format-color/fast_color.hpp"

namespace ps::plugin_internal::grade::curves {
struct Brightness final {
  unsigned route = 0;  // 0 ratio, 1 XYZ Y, 2 CIELAB l, 3 OKLab L.
  fast_color::Matrix forward{}, inverse{}, adaptation{}, unadaptation{};
  fast_color::Triple white{1, 1, 1};
  fast_color::Matrix rgb_to_lms{}, lms_to_rgb{};
  fast_color::Triple lightness_direction{};
  void prepare() {
    rgb_to_lms = fast_color::multiply(
        fast_color::kXyzToLms, fast_color::multiply(adaptation, forward));
    lms_to_rgb = fast_color::multiply(
        inverse, fast_color::multiply(
                     unadaptation, fast_color::inverse(fast_color::kXyzToLms)));
    const auto m = fast_color::inverse(fast_color::kLmsToOklab);
    for (unsigned c = 0; c < 3; ++c) {
      lightness_direction[c] = m[c][0];
    }
  }
  __attribute__((always_inline)) fast_color::Triple from_roots(
      const fast_color::Triple& rgb, const fast_color::Triple& roots,
      const fast_curve::Curve& curve, bool* changed) const {
    const double source = fast_color::dot(fast_color::kLmsToOklab[0], roots);
    const double target = curve.evaluate(source);
    *changed = target != source;
    if (!*changed) {
      return rgb;
    }
    const double delta = target - source;
    fast_color::Triple cubes;
    for (unsigned c = 0; c < 3; ++c) {
      const double r = roots[c], t = lightness_direction[c] * delta;
      cubes[c] = t * std::fma(t, t, std::fma(3 * r, t, 3 * r * r));
    }
    const auto correction = fast_color::multiply(lms_to_rgb, cubes);
    return {rgb[0] + correction[0], rgb[1] + correction[1],
            rgb[2] + correction[2]};
  }

  __attribute__((always_inline)) fast_color::Triple apply(
      const fast_color::Triple& rgb, const fast_curve::Curve& curve,
      bool* changed) const {
    if (route <= 1) {
      const double source = fast_color::dot(forward[1], rgb);
      const double target = curve.evaluate(source);
      *changed = target != source;
      if (!*changed) {
        return rgb;
      }
      if (route == 0) {
        if (source == 0) {
          return {target, target, target};
        }
        const double ratio = target / source;
        return {rgb[0] * ratio, rgb[1] * ratio, rgb[2] * ratio};
      }
      const double delta = target - source;
      return {std::fma(inverse[0][1], delta, rgb[0]),
              std::fma(inverse[1][1], delta, rgb[1]),
              std::fma(inverse[2][1], delta, rgb[2])};
    }
    if (route == 3) {
      auto roots = fast_color::multiply(rgb_to_lms, rgb);
      for (auto& value : roots) {
        value = std::cbrt(value);
      }
      return from_roots(rgb, roots, curve, changed);
    }
    auto xyz = fast_color::multiply(forward, rgb);
    auto coordinates = fast_color::xyz_to_lab(xyz, white);
    const double source = coordinates[0];
    const double target = curve.evaluate(source);
    *changed = target != source;
    if (!*changed) {
      return rgb;
    }
    coordinates[0] = target;
    xyz = fast_color::lab_to_xyz(coordinates, white);
    return fast_color::multiply(inverse, xyz);
  }
  template <class T, unsigned Channels>
  void span(const T* source, T* output, std::size_t count,
            const fast_curve::Curve& curve) const {
#if defined(__x86_64__)
    if (native_compute::available()) {
      native_compute::brightness(sizeof(T) == 4, Channels, *this, curve, source,
                                 output, count);
      return;
    }
#endif
    for (std::size_t i = 0; i < count; ++i) {
      const fast_color::Triple rgb{source[i * Channels],
                                   source[i * Channels + 1],
                                   source[i * Channels + 2]};
      bool changed;
      const auto result = apply(rgb, curve, &changed);
      if (!changed) {
        std::memcpy(output + i * Channels, source + i * Channels,
                    Channels * sizeof(T));
      } else {
        for (unsigned c = 0; c < 3; ++c) {
          output[i * Channels + c] = static_cast<T>(result[c]);
        }
        if constexpr (Channels == 4) {
          std::memcpy(output + i * Channels + 3, source + i * Channels + 3,
                      sizeof(T));
        }
      }
    }
  }
};
template <class T, unsigned Channels>
inline void component_span(const T* input, T* output, std::size_t count,
                           const fast_curve::Curve* curves,
                           unsigned selected = Channels) {
#if defined(__x86_64__)
  if (native_compute::available()) {
    native_compute::components(sizeof(T) == 4, Channels, selected, curves,
                               input, output, count);
    return;
  }
#endif
  for (std::size_t i = 0; i < count; ++i) {
    for (unsigned c = 0; c < Channels; ++c) {
      if (c < selected) {
        output[i * Channels + c] =
            static_cast<T>(curves[c].evaluate(input[i * Channels + c]));
      } else {
        std::memcpy(output + i * Channels + c, input + i * Channels + c,
                    sizeof(T));
      }
    }
  }
}
}  // namespace ps::plugin_internal::grade::curves
