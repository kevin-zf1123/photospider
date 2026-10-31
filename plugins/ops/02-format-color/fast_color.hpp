// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "01-numeric/fast_dispatch.hpp"

namespace ps::plugin_internal::fast_color {
using Matrix = std::array<std::array<double, 3>, 3>;
using Triple = std::array<double, 3>;
template <class T>
void copy_channel(const T*, T*, std::size_t, unsigned);
struct Model;
template <unsigned Kind, class T>
bool packet_span(const Model&, const T*, T*, std::size_t, std::size_t,
                 std::size_t, const std::array<std::uint64_t, 3>&);
template <unsigned Kind, class T>
bool transcendental_span(const Model&, const T*, T*, std::size_t, std::size_t,
                         std::size_t, const std::array<std::uint64_t, 3>&);
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kDelta = 6.0 / 29.0;
constexpr double kEpsilon = 216.0 / 24389.0;
constexpr Matrix kXyzToLms{
    {{{0.8190224379967030, 0.3619062600528904, -0.1288737815209879}},
     {{0.0329836539323885, 0.9292868615863434, 0.0361446663506424}},
     {{0.0481771893596242, 0.2642395317527308,
       0.6335478284694309}}}};  // NOLINT(whitespace/indent_namespace)
constexpr Matrix kLmsToOklab{
    {{{0.2104542683093140, 0.7936177747023054, -0.0040720430116193}},
     {{1.9779985324311684, -2.4285922420485799, 0.4505937096174110}},
     {{0.0259040424655478, 0.7827717124575296,
       -0.8086757549230774}}}};  // NOLINT(whitespace/indent_namespace)
inline __attribute__((always_inline)) double dot(const Triple& row,
                                                 const Triple& x) {
  return std::fma(row[2], x[2], std::fma(row[1], x[1], row[0] * x[0]));
}
inline __attribute__((always_inline)) Triple multiply(const Matrix& matrix,
                                                      const Triple& x) {
  return {dot(matrix[0], x), dot(matrix[1], x), dot(matrix[2], x)};
}
inline Matrix multiply(const Matrix& a, const Matrix& b) {
  Matrix result{};
  for (unsigned r = 0; r < 3; ++r) {
    for (unsigned c = 0; c < 3; ++c) {
      result[r][c] = std::fma(a[r][2], b[2][c],
                              std::fma(a[r][1], b[1][c], a[r][0] * b[0][c]));
    }
  }
  return result;
}
inline Matrix inverse(const Matrix& m) {
  Matrix result{};
  const double determinant = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) +
                             m[0][1] * (m[1][2] * m[2][0] - m[1][0] * m[2][2]) +
                             m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  for (unsigned r = 0; r < 3; ++r) {
    for (unsigned c = 0; c < 3; ++c) {
      const auto i = (c + 1) % 3, j = (c + 2) % 3;
      const auto k = (r + 1) % 3, l = (r + 2) % 3;
      result[r][c] = (m[i][k] * m[j][l] - m[i][l] * m[j][k]) / determinant;
    }
  }
  return result;
}
inline double cie_f(double x) {
  return x > kEpsilon ? std::cbrt(x) : x / (3 * kDelta * kDelta) + 4.0 / 29;
}
inline double cie_g(double x) {
  return x > kDelta ? x * x * x : 3 * kDelta * kDelta * (x - 4.0 / 29);
}
inline Triple xyz_to_lab(const Triple& x, const Triple& white) {
  const double a = cie_f(x[0] / white[0]), b = cie_f(x[1]);
  const double c = cie_f(x[2] / white[2]);
  return {1.16 * b - 0.16, 500 * (a - b), 200 * (b - c)};
}
inline Triple lab_to_xyz(const Triple& x, const Triple& white) {
  const double b = (x[0] + 0.16) / 1.16;
  return {white[0] * cie_g(b + x[1] / 500), cie_g(b),
          white[2] * cie_g(b - x[2] / 200)};
}
inline Triple xyz_to_oklab(const Triple& x) {
  auto lms = multiply(kXyzToLms, x);
  for (auto& value : lms) {
    value = std::cbrt(value);
  }
  return multiply(kLmsToOklab, lms);
}
inline Triple oklab_to_xyz(const Triple& x) {
  static const Matrix first = inverse(kLmsToOklab);
  static const Matrix second = inverse(kXyzToLms);
  auto lms = multiply(first, x);
  for (auto& value : lms) {
    value = value * value * value;
  }
  return multiply(second, lms);
}

struct Model final {
  unsigned kind = 0;
  unsigned gray = 0;
  bool input_pi = false, output_pi = false;
  Triple white{.3127 / .3290, 1, (1 - .3127 - .3290) / .3290};
  std::array<double, 2> white_xy{.3127, .3290};
  Matrix first{}, second{};
  double threshold = 0;
  double black = 0, bright = 1;
  std::array<std::uint64_t, 2> level_bits{};
  bool literal_levels = false;
  template <class T>
  std::array<T, 2> levels() const {
    std::array<T, 2> result;
    if (literal_levels) {
      std::memcpy(&result[0], &level_bits[0], sizeof(T));
      std::memcpy(&result[1], &level_bits[1], sizeof(T));
    } else {
      result[0] = static_cast<T>(black);
      result[1] = static_cast<T>(bright);
    }
    return result;
  }

  template <unsigned Kind>
  Triple apply(Triple x) const {
    if constexpr (Kind == 0) {
      return xyz_to_lab(x, white);
    } else if constexpr (Kind == 1) {
      return lab_to_xyz(x, white);
    } else if constexpr (Kind == 2 || Kind == 6) {
      const double h = x[1] == 0 && x[2] == 0 ? 0 : std::atan2(x[2], x[1]);
      return {x[0], std::hypot(x[1], x[2]), output_pi ? h / kPi : h};
    } else if constexpr (Kind == 3 || Kind == 7) {
      // C=0 has no hue dependency in the mathematical model.
      if (x[1] == 0) {
        return {x[0], 0, 0};
      }
      const double h = input_pi ? x[2] * kPi : x[2];
      return {x[0], x[1] * std::cos(h), x[1] * std::sin(h)};
    } else if constexpr (Kind == 4 || Kind == 5) {
      auto v = multiply(first, x);
      for (auto& value : v) {
        if constexpr (Kind == 4) {
          value = std::cbrt(value);
        } else {
          value = value * value * value;
        }
      }
      return multiply(second, v);
    } else if constexpr (Kind == 8 || Kind == 10) {
      const double low = std::min({x[0], x[1], x[2]});
      const double high = std::max({x[0], x[1], x[2]});
      const double delta = high - low;
      if (delta == 0) {
        return {0, 0, high};
      }
      const double light = high * .5 + low * .5;
      double hue = 0, saturation = 0;
      if (delta != 0) {
        hue = high == x[0]   ? (x[1] - x[2]) / delta
              : high == x[1] ? 2 + (x[2] - x[0]) / delta
                             : 4 + (x[0] - x[1]) / delta;
        if (hue < 0) {
          hue += 6;
        }
        if constexpr (Kind == 8) {
          saturation =
              delta / (low >= 0 && high <= 1
                           ? std::fma(2.0, std::min(low, 1 - high), delta)
                           : 1 - std::abs(2 * light - 1));
        } else {
          saturation = delta / high;
        }
      }
      return {hue * (output_pi ? 1.0 / 3 : kPi / 3), saturation,
              Kind == 8 ? light : high};
    } else if constexpr (Kind == 9 || Kind == 11) {
      if (x[1] == 0) {
        return {x[2], x[2], x[2]};
      }
      const double chroma =
          Kind == 9 ? (1 - std::abs(2 * x[2] - 1)) * x[1] : x[2] * x[1];
      const double base = Kind == 9 ? x[2] - chroma * .5 : x[2] - chroma;
      if (chroma == 0) {
        return {base, base, base};
      }
      double h =
          std::fmod(x[0], input_pi ? 2.0 : 2 * kPi) * (input_pi ? 3 : 3 / kPi);
      if (h < 0) {
        h += 6;
      }
      if (h >= 6) {
        h = 0;
      }
      if (!std::isfinite(h)) {
        return {h, h, h};
      }
      const unsigned sector = std::min(5U, static_cast<unsigned>(h));
      const double other = chroma * (1 - std::abs(std::fmod(h, 2) - 1));
      Triple result;
      switch (sector) {
        case 0:
          result = {chroma, other, 0};
          break;
        case 1:
          result = {other, chroma, 0};
          break;
        case 2:
          result = {0, chroma, other};
          break;
        case 3:
          result = {0, other, chroma};
          break;
        case 4:
          result = {other, 0, chroma};
          break;
        default:
          result = {chroma, 0, other};
          break;
      }
      for (auto& v : result) {
        v += base;
      }
      return result;
    } else if constexpr (Kind == 12 || Kind == 13) {
      return multiply(first, x);
    } else if constexpr (Kind == 14) {
      const double sum = (x[0] + x[1]) + x[2];
      return sum == 0 ? Triple{white_xy[0], white_xy[1], x[1]}
                      : Triple{x[0] / sum, x[1] / sum, x[1]};
    } else if constexpr (Kind == 15) {
      if (x[2] == 0) {
        return {0, x[2], 0};
      }
      return {x[0] * x[2] / x[1], x[2], (1 - x[0] - x[1]) * x[2] / x[1]};
    } else if constexpr (Kind == 16) {
      return {x[gray == 0 ? 1 : 0], 0, 0};
    } else if constexpr (Kind == 17) {
      return gray == 0 ? Triple{x[0] * white[0], x[0], x[0] * white[2]}
                       : Triple{x[0], 0, 0};
    } else if constexpr (Kind == 18) {
      return {x[0] >= threshold ? 1.0 : 0.0, 0, 0};
    } else {
      return {x[0] == 0 ? black : bright, 0, 0};
    }
  }

  __attribute__((always_inline)) Triple apply(const Triple& x) const {
    switch (kind) {
#define PS_FAST_MODEL_CASE(K) \
  case K:                     \
    return apply<K>(x)
      PS_FAST_MODEL_CASE(0);
      PS_FAST_MODEL_CASE(1);
      PS_FAST_MODEL_CASE(2);
      PS_FAST_MODEL_CASE(3);
      PS_FAST_MODEL_CASE(4);
      PS_FAST_MODEL_CASE(5);
      PS_FAST_MODEL_CASE(6);
      PS_FAST_MODEL_CASE(7);
      PS_FAST_MODEL_CASE(8);
      PS_FAST_MODEL_CASE(9);
      PS_FAST_MODEL_CASE(10);
      PS_FAST_MODEL_CASE(11);
      PS_FAST_MODEL_CASE(12);
      PS_FAST_MODEL_CASE(13);
      PS_FAST_MODEL_CASE(14);
      PS_FAST_MODEL_CASE(15);
      PS_FAST_MODEL_CASE(16);
      PS_FAST_MODEL_CASE(17);
      PS_FAST_MODEL_CASE(18);
      PS_FAST_MODEL_CASE(19);
#undef PS_FAST_MODEL_CASE
    }
    return x;
  }

  template <unsigned Kind, class T>
  void span(const T* input, T* output, std::size_t count,
            std::size_t input_channels, std::size_t output_channels,
            const std::array<std::uint64_t, 3>& selected) const {
#if defined(__x86_64__)
    if (native_compute::available()) {
      native_compute::model(Kind, sizeof(T) == 4, *this, input, output, count,
                            input_channels, output_channels, selected);
      return;
    }
#endif
    if constexpr (Kind == 16) {
      const auto component = selected[gray == 0 ? 1 : 0];
      if (input_channels == 3 && output_channels == 1) {
        copy_channel(input, output, count, component);
        return;
      }
    }
    if (transcendental_span<Kind>(*this, input, output, count, input_channels,
                                  output_channels, selected) ||
        packet_span<Kind>(*this, input, output, count, input_channels,
                          output_channels, selected)) {
      return;
    }
    if constexpr (Kind == 19) {
      const auto constants = levels<T>();
      for (std::size_t i = 0; i < count; ++i) {
        const auto c = input[i * input_channels + selected[0]] == 0 ? 0 : 1;
        std::memcpy(output + i * output_channels, &constants[c], sizeof(T));
      }
      return;
    }
    for (std::size_t i = 0; i < count; ++i) {
      Triple x{};
      constexpr unsigned n = Kind >= 17 ? 1 : 3;
      for (unsigned j = 0; j < n; ++j) {
        x[j] = input[i * input_channels + selected[j]];
      }
      const auto y = apply<Kind>(x);
      const unsigned outputs = Kind == 16 || Kind >= 18 ? 1 : 3;
      for (unsigned j = 0; j < outputs; ++j) {
        output[i * output_channels + j] = static_cast<T>(y[j]);
      }
    }
  }
};
}  // namespace ps::plugin_internal::fast_color

#include "02-format-color/fast_color_simd.hpp"
#include "02-format-color/fast_color_transcendental.hpp"
