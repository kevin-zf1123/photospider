// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "01-numeric/accelerated_math.hpp"

namespace ps::plugin_internal::fast_color {
template <unsigned Kind, class T>
bool transcendental_span(const Model& model, const T* input, T* output,
                         std::size_t size, std::size_t ic, std::size_t oc,
                         const std::array<std::uint64_t, 3>& selected) {
  if constexpr (!(Kind == 0 || Kind == 2 || Kind == 3 || Kind == 4 ||
                  Kind == 6 || Kind == 7)) {
    return false;
  } else {
#if defined(__APPLE__) && defined(__aarch64__)
    // The platform scalar cbrt pipeline is faster than this SLEEF adapter.
    if constexpr (Kind == 0 || Kind == 4) {
      return false;
    }
#endif
    if (ic != 3 || oc != 3 ||
        selected != std::array<std::uint64_t, 3>{0, 1, 2} ||
        !numeric_ops::accelerated_math_available()) {
      return false;
    }
    constexpr std::size_t block = 128;
    alignas(64) double x[3][block], y[3][block];
    for (std::size_t base = 0; base < size; base += block) {
      const auto n = std::min(block, size - base);
      const auto* source = input + base * 3;
      auto* target = output + base * 3;
      for (std::size_t i = 0; i < n; ++i) {
        const Triple v{source[3 * i], source[3 * i + 1], source[3 * i + 2]};
        if constexpr (Kind == 0) {
          x[0][i] = v[0] / model.white[0];
          x[1][i] = v[1];
          x[2][i] = v[2] / model.white[2];
        } else if constexpr (Kind == 4) {
          const auto lms = multiply(model.first, v);
          for (unsigned c = 0; c < 3; ++c) {
            x[c][i] = lms[c];
          }
        } else if constexpr (Kind == 2 || Kind == 6) {
          x[0][i] = v[1];
          x[1][i] = v[2];
          const double hi = std::max(std::abs(v[1]), std::abs(v[2]));
          const double lo = std::min(std::abs(v[1]), std::abs(v[2]));
          const double r = hi == 0 ? 0 : lo / hi;
          x[2][i] = hi * std::sqrt(1 + r * r);
        } else {
          x[0][i] = v[1] == 0 ? 0 : model.input_pi ? v[2] * kPi : v[2];
          x[1][i] = v[1];
        }
      }
      if constexpr (Kind == 0 || Kind == 4) {
        for (unsigned c = 0; c < 3; ++c) {
          photospider_sleef_evaluate(12, x[c], x[c], y[c], n);
        }
      } else if constexpr (Kind == 2 || Kind == 6) {
        photospider_sleef_evaluate(11, x[1], x[0], y[0], n);
      } else {
        photospider_sleef_evaluate(2, x[0], x[0], y[0], n);
        photospider_sleef_evaluate(3, x[0], x[0], y[1], n);
      }
      for (std::size_t i = 0; i < n; ++i) {
        if constexpr (Kind == 0) {
          Triple f{};
          for (unsigned c = 0; c < 3; ++c) {
            f[c] = x[c][i] > kEpsilon
                       ? y[c][i]
                       : x[c][i] / (3 * kDelta * kDelta) + 4.0 / 29;
          }
          target[3 * i] = T(1.16 * f[1] - .16);
          target[3 * i + 1] = T(500 * (f[0] - f[1]));
          target[3 * i + 2] = T(200 * (f[1] - f[2]));
        } else if constexpr (Kind == 4) {
          const auto v =
              multiply(model.second, Triple{y[0][i], y[1][i], y[2][i]});
          for (unsigned c = 0; c < 3; ++c) {
            target[3 * i + c] = T(v[c]);
          }
        } else {
          std::memcpy(target + 3 * i, source + 3 * i, sizeof(T));
          if constexpr (Kind == 2 || Kind == 6) {
            target[3 * i + 1] = T(x[2][i]);
            target[3 * i + 2] =
                (x[0][i] == 0 && x[1][i] == 0)
                    ? T(0)
                    : T(model.output_pi ? y[0][i] / kPi : y[0][i]);
          } else {
            const auto chroma = x[1][i];
            target[3 * i + 1] = chroma == 0 ? T(0) : T(chroma * y[1][i]);
            target[3 * i + 2] = chroma == 0 ? T(0) : T(chroma * y[0][i]);
          }
        }
      }
    }
    return true;
  }
}
}  // namespace ps::plugin_internal::fast_color
