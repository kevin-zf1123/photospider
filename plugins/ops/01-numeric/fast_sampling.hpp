// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "01-numeric/accelerated_math.hpp"
#include "01-numeric/expression_program.hpp"

namespace ps::plugin_internal::fast_sampling {
constexpr std::size_t kBatch = 128;
inline double coordinate(double start, double end, std::uint64_t index,
                         std::uint64_t count) {
  if (index == 0 || count == 1) {
    return start;
  }
  if (index == count - 1) {
    return end;
  }
  const double t = static_cast<double>(index) / (count - 1);
  const double difference = end - start;
  return std::isfinite(difference) ? std::fma(t, difference, start)
                                   : std::fma(t, end, (1 - t) * start);
}
inline double bezier(const double* controls, unsigned degree, double t) {
  if (t == 0) {
    return controls[0];
  }
  if (t == 1) {
    return controls[degree];
  }
  const double a = std::fma(t, controls[1] - controls[0], controls[0]);
  const double b = std::fma(t, controls[2] - controls[1], controls[1]);
  if (degree == 2) {
    return std::fma(t, b - a, a);
  }
  const double c = std::fma(t, controls[3] - controls[2], controls[2]);
  const double d = std::fma(t, b - a, a);
  const double e = std::fma(t, c - b, b);
  return std::fma(t, e - d, d);
}
inline double derivative(const double* controls, unsigned degree, double t) {
  const double a = controls[1] - controls[0];
  const double b = controls[2] - controls[1];
  if (degree == 2) {
    return 2 * std::fma(t, b - a, a);
  }
  const double c = controls[3] - controls[2];
  const double d = std::fma(t, b - a, a);
  const double e = std::fma(t, c - b, b);
  return 3 * std::fma(t, e - d, d);
}
inline double bezier_function(const double* x, const double* y, unsigned degree,
                              double query) {
  if (query == x[0]) {
    return y[0];
  }
  if (query == x[degree]) {
    return y[degree];
  }
  double low = 0, high = 1;
  double t = (query - x[0]) / (x[degree] - x[0]);
  // Flat end tangents need a root-order estimate: a linear initial estimate
  // can remain hundreds of exponents away from the root for subnormal q.
  if (t < .5 && x[1] == x[0]) {
    if (degree == 3 && x[2] == x[0]) {
      t = std::cbrt(t);
    } else {
      t = std::sqrt((query - x[0]) / ((degree == 3 ? 3 : 1) * (x[2] - x[0])));
    }
  } else if (t >= .5 && x[degree - 1] == x[degree]) {
    const double u = (x[degree] - query) / (x[degree] - x[0]);
    if (degree == 3 && x[1] == x[degree]) {
      t = 1 - std::cbrt(u);
    } else {
      t = 1 - std::sqrt((x[degree] - query) /
                        ((degree == 3 ? 3 : 1) * (x[degree] - x[degree - 2])));
    }
  }
  t = std::max(0.0, std::min(1.0, t));
  for (unsigned iteration = 0; iteration < 128; ++iteration) {
    const double value = bezier(x, degree, t);
    if (value == query) {
      break;
    }
    if (value < query) {
      low = t;
    } else {
      high = t;
    }
    const double slope = derivative(x, degree, t);
    const double candidate = t - (value - query) / slope;
    const double next = candidate > low && candidate < high
                            ? candidate
                            : low + (high - low) * .5;
    if (next == t || high - low <= 0x1p-51) {
      break;
    }
    t = next;
  }
  return bezier(y, degree, t);
}
// Postorder vector interpreter. workspace holds program.size * kBatch doubles;
// all arithmetic is native binary64, with a single destination conversion.
inline void expression(const numeric_ops::ExpressionProgram& program,
                       const double* coefficients, const double* queries,
                       double* output, std::size_t count, double* workspace) {
  using K = numeric_ops::ExpressionKind;
  const bool vector_math = numeric_ops::accelerated_math_available();
  for (unsigned node = 0; node < program.size; ++node) {
    const auto& instruction = program.nodes[node];
    auto* result = workspace + node * kBatch;
    const auto* a = workspace + instruction.left * kBatch;
    const auto* b = workspace + instruction.right * kBatch;
    unsigned function = 99;
    switch (instruction.kind) {
      case K::Exp:
        function = 0;
        break;
      case K::Ln:
        function = 1;
        break;
      case K::Sin:
        function = 2;
        break;
      case K::Cos:
        function = 3;
        break;
      case K::Tan:
        function = 4;
        break;
      case K::Power:
        function = 10;
        break;
      default:
        break;
    }
    if (function != 99 && vector_math) {
      photospider_sleef_evaluate(function, a, b, result, count);
      continue;
    }
    switch (instruction.kind) {
      case K::Literal:
        std::fill_n(result, count,
                    numeric_ops::numeric_double(instruction.bits));
        break;
      case K::X:
        std::copy_n(queries, count, result);
        break;
      case K::Coefficient:
        std::fill_n(result, count, coefficients[instruction.left]);
        break;
      case K::Positive:
        std::copy_n(a, count, result);
        break;
#define PS_NATIVE_EXPRESSION_LOOP(KIND, VALUE) \
  case K::KIND:                                \
    for (std::size_t i = 0; i < count; ++i)    \
      result[i] = (VALUE);                     \
    break
        PS_NATIVE_EXPRESSION_LOOP(Negative, -a[i]);
        PS_NATIVE_EXPRESSION_LOOP(Add, a[i] + b[i]);
        PS_NATIVE_EXPRESSION_LOOP(Subtract, a[i] - b[i]);
        PS_NATIVE_EXPRESSION_LOOP(Multiply, a[i] * b[i]);
        PS_NATIVE_EXPRESSION_LOOP(Divide, a[i] / b[i]);
        PS_NATIVE_EXPRESSION_LOOP(Power, std::pow(a[i], b[i]));
        PS_NATIVE_EXPRESSION_LOOP(Abs, std::abs(a[i]));
        PS_NATIVE_EXPRESSION_LOOP(Sqrt, std::sqrt(a[i]));
        PS_NATIVE_EXPRESSION_LOOP(Exp, std::exp(a[i]));
        PS_NATIVE_EXPRESSION_LOOP(Ln, std::log(a[i]));
        PS_NATIVE_EXPRESSION_LOOP(Sin, std::sin(a[i]));
        PS_NATIVE_EXPRESSION_LOOP(Cos, std::cos(a[i]));
        PS_NATIVE_EXPRESSION_LOOP(Tan, std::tan(a[i]));
        PS_NATIVE_EXPRESSION_LOOP(Min, std::fmin(a[i], b[i]));
        PS_NATIVE_EXPRESSION_LOOP(Max, std::fmax(a[i], b[i]));
#undef PS_NATIVE_EXPRESSION_LOOP
    }
  }
  std::copy_n(workspace + (program.size - 1) * kBatch, count, output);
}
}  // namespace ps::plugin_internal::fast_sampling

#include "01-numeric/fast_polynomial.hpp"
