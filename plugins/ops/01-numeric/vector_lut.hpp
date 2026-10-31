// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "01-numeric/fast_curve.hpp"

namespace ps::plugin_internal::fast_curve {
// One native normalized coordinate serves every column. Direct selection
// preserves stored zero signs, including singleton, clamp and normalized hits.
struct VectorLut final {
  const double* table = nullptr;
  std::size_t length = 0, columns = 0;
  double start = 0, end = 0, scale = 0;
  Edge edge = Edge::Clamp;
  struct Position {
    std::size_t row;
    double fraction;
    bool direct;
  };
  VectorLut(const double* data, std::size_t rows, std::size_t components,
            double first, double last, Edge policy)
      : table(data),
        length(rows),
        columns(components),
        start(first),
        end(last),
        scale(rows > 1 ? (rows - 1) / (last - first) : 0),
        edge(policy) {}
  Position position(double q) const {
    if (length == 1 || q == start)
      return {0, 0, true};
    if (q == end)
      return {length - 1, 0, true};
    const double u = normalized_position(q, start, end, length, scale);
    if (edge != Edge::Linear) {
      if (u <= 0)
        return {0, 0, true};
      if (u >= length - 1)
        return {length - 1, 0, true};
    }
    const auto row = static_cast<std::size_t>(std::max(
        0.0, std::min(static_cast<double>(length - 2), std::floor(u))));
    const double t = u - static_cast<double>(row);
    if (t == 0)
      return {row, 0, true};
    if (t == 1)
      return {row + 1, 0, true};
    return {row, t, false};
  }
  double value(Position p, std::size_t c) const {
    const auto* a = table + p.row * columns;
    return p.direct ? a[c] : std::fma(p.fraction, a[columns + c] - a[c], a[c]);
  }
  template <unsigned C, class Query, class Output>
  void generate(Query query, Output* output, std::size_t count) const {
    const auto nc = C ? C : columns;
    if (length == 1) {
      if constexpr (C != 0) {
        Output row[C];
        for (unsigned c = 0; c < C; ++c)
          row[c] = static_cast<Output>(table[c]);
        std::size_t i = 0;
#if defined(__aarch64__)
        constexpr unsigned kLanes = 16 / sizeof(Output);
        Output pattern[C * kLanes];
        for (unsigned j = 0; j < C * kLanes; ++j)
          pattern[j] = row[j % C];
        if constexpr (sizeof(Output) == 4) {
          float32x4_t vectors[C];
          for (unsigned c = 0; c < C; ++c) {
            vectors[c] = vld1q_f32(pattern + c * kLanes);
            // Keep packed stores through strict-fenv scalar legalization.
            __asm__ __volatile__("" : "+w"(vectors[c]));
          }
          for (; i + 2 * kLanes <= count; i += 2 * kLanes) {
            for (unsigned c = 0; c < C; ++c)
              vst1q_f32(output + i * C + c * kLanes, vectors[c]);
            for (unsigned c = 0; c < C; ++c)
              vst1q_f32(output + (i + kLanes) * C + c * kLanes, vectors[c]);
          }
        } else {
          float64x2_t vectors[C];
          for (unsigned c = 0; c < C; ++c)
            vectors[c] = vld1q_f64(pattern + c * kLanes);
          for (; i + kLanes <= count; i += kLanes)
            for (unsigned c = 0; c < C; ++c)
              vst1q_f64(output + i * C + c * kLanes, vectors[c]);
        }
#endif
        for (; i < count; ++i)
          std::copy_n(row, C, output + i * C);
      } else if (count) {
        for (std::size_t c = 0; c < nc; ++c)
          output[c] = static_cast<Output>(table[c]);
        for (std::size_t done = 1; done < count;) {
          const auto n = std::min(done, count - done);
          std::memcpy(output + done * nc, output, n * nc * sizeof(Output));
          done += n;
        }
      }
      return;
    }
    for (std::size_t i = 0; i < count; ++i) {
      const auto p = position(length == 1 ? 0 : query(i));
      const auto* a = table + p.row * columns;
      auto* target = output + i * nc;
      if (p.direct) {
        for (std::size_t c = 0; c < nc; ++c)
          target[c] = static_cast<Output>(a[c]);
      } else {
        for (std::size_t c = 0; c < nc; ++c)
          target[c] = static_cast<Output>(
              std::fma(p.fraction, a[columns + c] - a[c], a[c]));
      }
    }
  }
  template <class Query, class Output>
  void generate(Query query, Output* output, std::size_t count) const {
    if (columns == 3)
      generate<3>(query, output, count);
    else if (columns == 4)
      generate<4>(query, output, count);
    else if (columns == 1)
      generate<1>(query, output, count);
    else
      generate<0>(query, output, count);
  }
  template <class Input, class Output>
  void span(const Input* input, Output* output, std::size_t count,
            std::ptrdiff_t stride = 1) const {
#if defined(__x86_64__)
    if (native_compute::available()) {
      native_compute::vector_lut(sizeof(Input) == 4, sizeof(Output) == 4, *this,
                                 input, output, count, stride);
      return;
    }
#endif
    generate(
        [input, stride](std::size_t i) {
          return input[static_cast<std::ptrdiff_t>(i) * stride];
        },
        output, count);
  }
};
}  // namespace ps::plugin_internal::fast_curve
