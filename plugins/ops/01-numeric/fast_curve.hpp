// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "01-numeric/fast_dispatch.hpp"

namespace ps::plugin_internal::fast_curve {
enum class Edge { Reject, Clamp, Linear };

// Native binary64 PCHIP. The caller owns all storage and guarantees finite,
// increasing knots. This profile deliberately does not certify NUM rounding.
inline double interior_slope(double h0, double h1, double d0, double d1) {
  if (d0 == 0 || d1 == 0 || std::signbit(d0) != std::signbit(d1)) {
    return 0;
  }
  const double scale = std::max(h0, h1);
  const double a = h0 / scale, b = h1 / scale;
  const double w0 = (2 * b + a) / (3 * (a + b));
  // Scale the harmonic mean to avoid reciprocal overflow for small secants.
  const double d = std::min(std::abs(d0), std::abs(d1));
  return std::copysign(
      d / (w0 * (d / std::abs(d0)) + (1 - w0) * (d / std::abs(d1))), d0);
}
inline double endpoint_slope(double h0, double h1, double d0, double d1) {
  const double scale = std::max(h0, h1);
  const double a = h0 / scale, b = h1 / scale;
  const double result = ((2 * a + b) * d0 - a * d1) / (a + b);
  if (result == 0 || std::signbit(result) != std::signbit(d0)) {
    return 0;
  }
  if (std::signbit(d0) != std::signbit(d1) &&
      std::abs(result) > 3 * std::abs(d0)) {
    return 3 * d0;
  }
  return result;
}

struct Curve final {
  const double* x = nullptr;
  const double* y = nullptr;
  double* slope = nullptr;
  double* a = nullptr;
  double* b = nullptr;
  double* c = nullptr;
  std::size_t size = 0;
  Edge edge = Edge::Reject;
  bool cubic = false;
  bool identity = false, constant = false;
  // Constant curves store extrapolated values instead of endpoint slopes.
  // The constant flag selects the active member, retaining the curve layout.
  union {
    double first_slope = 0;
    double first_constant;
  };
  union {
    double last_slope = 0;
    double last_constant;
  };
  double index_scale = 0;
  unsigned guide_count = 0;
  std::array<std::uint32_t, 257> guide{};

  // workspace has at least 4*K doubles. Preparation is separate from lookup.
  void prepare(const double* knots, const double* values, std::size_t count,
               bool pchip, Edge boundary, double* workspace) {
    x = knots;
    y = values;
    size = count;
    edge = boundary;
    cubic = pchip && count > 2;
    slope = workspace;
    a = slope + size;
    b = a + size;
    c = b + size;
    identity = constant = true;
    for (std::size_t i = 0; i < size; ++i) {
      identity &= x[i] == y[i];
      constant &= y[i] == y[0];
    }
    for (std::size_t i = 0; i + 1 < size; ++i) {
      const double h = x[i + 1] - x[i];
      a[i] = (y[i + 1] - y[i]) / h;
      b[i] = 1 / h;
    }
    slope[0] = a[0];
    slope[size - 1] = a[size - 2];
    if (cubic) {
      slope[0] = endpoint_slope(x[1] - x[0], x[2] - x[1], a[0], a[1]);
      slope[size - 1] =
          endpoint_slope(x[size - 1] - x[size - 2], x[size - 2] - x[size - 3],
                         a[size - 2], a[size - 3]);
      for (std::size_t i = 1; i + 1 < size; ++i) {
        slope[i] =
            interior_slope(x[i] - x[i - 1], x[i + 1] - x[i], a[i - 1], a[i]);
      }
      for (std::size_t i = 0; i + 1 < size; ++i) {
        const double h = x[i + 1] - x[i];
        const double delta = y[i + 1] - y[i];
        const double d0 = slope[i] * h, d1 = slope[i + 1] * h;
        a[i] = d0;
        b[i] = 3 * delta - 2 * d0 - d1;
        c[i] = d0 + d1 - 2 * delta;
      }
    }
    first_slope = slope[0];
    last_slope = slope[size - 1];
    if (constant) {
      // Outside the interval, the displacement has a fixed sign. Preserve
      // the endpoint FMA's zero sign without repeating arithmetic per query.
      first_constant = y[0] == 0 ? std::fma(-1.0, first_slope, y[0]) : y[0];
      last_constant = y[size - 1] == 0 ? std::fma(1.0, last_slope, y[size - 1])
                                       : y[size - 1];
    }
    for (std::size_t j = 0; j + 1 < size; ++j) {
      slope[j] = 1 / (x[j + 1] - x[j]);
    }
    guide_count =
        static_cast<unsigned>(std::min<std::size_t>(256, 4 * (size - 1)));
    index_scale = guide_count / (x[size - 1] - x[0]);
    std::size_t i = 0;
    for (unsigned j = 0; j <= guide_count; ++j) {
      const double q = j == guide_count
                           ? x[size - 1]
                           : x[0] + (x[size - 1] - x[0]) *
                                        (static_cast<double>(j) / guide_count);
      while (i + 1 < size - 1 && x[i + 1] <= q) {
        ++i;
      }
      guide[j] = static_cast<std::uint32_t>(i);
    }
  }

  std::size_t segment(double q) const {
    if (size == 2) {
      return 0;
    }
    std::size_t lo = 0, hi = size - 1;
    if (std::isfinite(index_scale) && index_scale > 0) {
      const double position = (q - x[0]) * index_scale;
      const unsigned bucket = static_cast<unsigned>(std::max(
          0.0, std::min(static_cast<double>(guide_count - 1), position)));
      lo = guide[bucket];
      hi = std::min<std::size_t>(size - 1, guide[bucket + 1] + 1);
      // Rounding at bucket boundaries must never alter interval selection.
      if (q < x[lo] || q >= x[hi]) {
        lo = 0;
        hi = size - 1;
      }
    }
    while (lo + 1 < hi) {
      const auto mid = lo + (hi - lo) / 2;
      if (q < x[mid]) {
        hi = mid;
      } else {
        lo = mid;
      }
    }
    return lo;
  }

  // Exact knots select stored ordinates before this algebraic shortcut.
  // Interior shortcut zeros are positive; copied knot/edge zeros retain their
  // sign. Constant extrapolation uses the separately prepared endpoint values.
  double identity_or_constant(double q) const {
    const double value = identity ? q : y[0];
    return value == 0 ? 0.0 : value;
  }

  __attribute__((always_inline)) double evaluate(double q) const {
    if (q <= x[0]) {
      if (q == x[0] || edge != Edge::Linear) {
        return y[0];
      }
      return identity || constant
                 ? (identity ? identity_or_constant(q) : first_constant)
                 : std::fma(q - x[0], first_slope, y[0]);
    }
    if (q >= x[size - 1]) {
      if (q == x[size - 1] || edge != Edge::Linear) {
        return y[size - 1];
      }
      return identity || constant
                 ? (identity ? identity_or_constant(q) : last_constant)
                 : std::fma(q - x[size - 1], last_slope, y[size - 1]);
    }
    if ((identity && q != 0) || (constant && y[0] != 0)) {
      return identity_or_constant(q);
    }
    const auto i = segment(q);
    if (q == x[i]) {
      return y[i];
    }
    if (identity || constant) {
      return identity_or_constant(q);
    }
    if (!cubic) {
      return std::fma(q - x[i], a[i], y[i]);
    }
    const double t = (q - x[i]) * slope[i];
    return std::fma(t, std::fma(t, std::fma(t, c[i], b[i]), a[i]), y[i]);
  }

  template <class Input, class Output>
  void span(const Input* source, Output* target, std::size_t count,
            std::ptrdiff_t source_step = 1,
            std::ptrdiff_t target_step = 1) const {
    for (std::size_t i = 0; i < count; ++i) {
      target[static_cast<std::ptrdiff_t>(i) * target_step] =
          static_cast<Output>(
              evaluate(source[static_cast<std::ptrdiff_t>(i) * source_step]));
    }
  }
};

// One interval search serves every ordinate column. Ordered batches use a
// cursor after a complete order check; no user query is presumed sorted.
template <unsigned Channels, bool Cubic, int Direction, class Query, class T>
inline void shared_span(const Curve* curves, unsigned columns, Query query,
                        T* output, std::size_t count) {
  const auto nc = Channels ? Channels : columns;
  if (!count || !nc) {
    return;
  }
  const auto& axis = curves[0];
  const auto last = axis.size - 1;
  std::size_t interval = Direction < 0 ? last - 1 : 0;
  bool positioned = false;
  for (std::size_t j = 0; j < count; ++j) {
    const double q = query(j);
    auto* destination = output + j * nc;
    if (q <= axis.x[0] || q >= axis.x[last]) {
      const bool upper = q >= axis.x[last];
      const auto row = upper ? last : 0;
      for (unsigned c = 0; c < nc; ++c) {
        const auto& curve = curves[c];
        const double value =
            q == axis.x[row] || curve.edge != Edge::Linear ? curve.y[row]
            : curve.identity ? curve.identity_or_constant(q)
            : curve.constant
                ? (upper ? curve.last_constant : curve.first_constant)
                : std::fma(q - axis.x[row],
                           upper ? curve.last_slope : curve.first_slope,
                           curve.y[row]);
        destination[c] = static_cast<T>(value);
      }
      continue;
    }
    if constexpr (!Direction) {
      interval = axis.segment(q);
    } else if (!positioned) {
      interval = axis.segment(q);
      positioned = true;
    } else if constexpr (Direction > 0) {
      while (interval + 1 < last && q >= axis.x[interval + 1]) {
        ++interval;
      }
    } else {
      while (interval && q < axis.x[interval]) {
        --interval;
      }
    }
    if (q == axis.x[interval]) {
      for (unsigned c = 0; c < nc; ++c) {
        destination[c] = static_cast<T>(curves[c].y[interval]);
      }
      continue;
    }
    const double delta = q - axis.x[interval];
    const double t = Cubic ? delta * axis.slope[interval] : delta;
    for (unsigned c = 0; c < nc; ++c) {
      const auto& curve = curves[c];
      const double value =
          curve.identity || curve.constant ? curve.identity_or_constant(q)
          : Cubic
              ? std::fma(
                    t,
                    std::fma(t,
                             std::fma(t, curve.c[interval], curve.b[interval]),
                             curve.a[interval]),
                    curve.y[interval])
              : std::fma(t, curve.a[interval], curve.y[interval]);
      destination[c] = static_cast<T>(value);
    }
  }
}
template <unsigned Channels, class Query, class T>
inline void shared_dispatch(const Curve* curves, unsigned columns, Query query,
                            T* output, std::size_t count, int direction) {
#define PS_SHARED_CURVE(CUBIC)                                               \
  if (direction > 0)                                                         \
    shared_span<Channels, CUBIC, 1>(curves, columns, query, output, count);  \
  else if (direction < 0)                                                    \
    shared_span<Channels, CUBIC, -1>(curves, columns, query, output, count); \
  else                                                                       \
    shared_span<Channels, CUBIC, 0>(curves, columns, query, output, count)
  if (curves[0].cubic) {
    PS_SHARED_CURVE(true);
  } else {
    PS_SHARED_CURVE(false);
  }
#undef PS_SHARED_CURVE
}
template <class Query, class T>
__attribute__((noinline)) void constant_triplet_span(const Curve* curves,
                                                     Query query, T* output,
                                                     std::size_t count) {
  const auto& axis = curves[0];
  const auto last = axis.size - 1;
  std::array<T, 3> left, middle, right;
  for (unsigned c = 0; c < 3; ++c) {
    const auto& curve = curves[c];
    left[c] = static_cast<T>(curve.edge == Edge::Linear ? curve.first_constant
                                                        : curve.y[0]);
    right[c] = static_cast<T>(curve.edge == Edge::Linear ? curve.last_constant
                                                         : curve.y[last]);
    middle[c] = static_cast<T>(curve.identity_or_constant(0));
  }
  for (std::size_t j = 0; j < count; ++j) {
    const double q = query(j);
    const T* row = nullptr;
    std::size_t knot = 0;
    if (q < axis.x[0]) {
      row = left.data();
    } else if (q > axis.x[last]) {
      row = right.data();
    } else {
      knot = q == axis.x[last] ? last : axis.segment(q);
      if (q != axis.x[knot])
        row = middle.data();
    }
    if (row) {
      std::copy_n(row, 3, output + 3 * j);
    } else {
      for (unsigned c = 0; c < 3; ++c)
        output[3 * j + c] = static_cast<T>(curves[c].y[knot]);
    }
  }
}
template <class Query, class T>
inline void shared(const Curve* curves, unsigned columns, Query query,
                   T* output, std::size_t count, int direction) {
  if (!columns || !count) {
    return;
  }
  if (columns == 1) {
    shared_dispatch<1>(curves, columns, query, output, count, direction);
  } else if (columns == 3) {
    if (!direction && count >= 16 && curves[0].constant && curves[1].constant &&
        curves[2].constant)
      constant_triplet_span(curves, query, output, count);
    else
      shared_dispatch<3>(curves, columns, query, output, count, direction);
  } else {
    shared_dispatch<0>(curves, columns, query, output, count, direction);
  }
}
template <class Input, class Output>
inline bool small_curve_span(const Curve&, const Input*, Output*, std::size_t);
template <class Input, class Output>
inline bool ordered_curve_span(const Curve*, unsigned, const Input*, Output*,
                               std::size_t, int);

template <class Input, class Output>
inline void span_multi(const Curve* curves, unsigned columns,
                       const Input* input, Output* output, std::size_t count) {
  if (count && columns == 1 && curves[0].constant && curves[0].y[0] != 0) {
    // Every knot, clamp and extrapolated value has the same nonzero ordinate.
    // Zero constants still need knot selection and directional zero signs.
    std::fill_n(output, count, static_cast<Output>(curves[0].y[0]));
    return;
  }
#if defined(__x86_64__)
  if (native_compute::available()) {
    native_compute::curve(sizeof(Input) == 4, sizeof(Output) == 4, curves,
                          columns, input, output, count);
    return;
  }
#endif
  int direction = 0;
  if (count > 1) {
    direction = input[0] <= input[count - 1] ? 1 : -1;
    for (std::size_t i = 1; i < count; ++i) {
      if (!(direction > 0 ? input[i] >= input[i - 1]
                          : input[i] <= input[i - 1])) {
        direction = 0;
        break;
      }
    }
  }
  if (direction &&
      ordered_curve_span(curves, columns, input, output, count, direction)) {
    return;
  }
  if (!direction && columns == 1 &&
      small_curve_span(curves[0], input, output, count)) {
    return;
  }
  shared(
      curves, columns, [input](auto i) { return input[i]; }, output, count,
      direction);
}

template <class T>
inline double load(const void* source, std::ptrdiff_t stride,
                   std::size_t index) {
  T value;
  std::memcpy(&value,
              static_cast<const unsigned char*>(source) +
                  static_cast<std::ptrdiff_t>(index) * stride,
              sizeof(value));
  return value;
}
// Keep the ordinary reciprocal path, but do not let a reciprocal overflow
// turn a valid strictly ordered subnormal axis into an endpoint hit.
inline double normalized_position(double query, double start, double end,
                                  std::size_t size, double scale) {
  const double delta = query - start;
  if (std::isfinite(delta) && std::isfinite(scale) && scale != 0)
    return delta * scale;
  const double width = end - start;
  if (std::isfinite(delta) && std::isfinite(width))
    return (delta / width) * (size - 1);
  return ((query * .5 - start * .5) / (end * .5 - start * .5)) * (size - 1);
}
inline double lut(double query, const double* values, std::size_t size,
                  double minimum, double maximum, Edge edge) {
  if (query == minimum) {
    return values[0];
  }
  if (query == maximum) {
    return values[size - 1];
  }
  const double u = normalized_position(query, minimum, maximum, size,
                                       (size - 1) / (maximum - minimum));
  if (edge != Edge::Linear) {
    if (u <= 0) {
      return values[0];
    }
    if (u >= size - 1) {
      return values[size - 1];
    }
  }
  const auto i = static_cast<std::size_t>(
      std::max(0.0, std::min(static_cast<double>(size - 2), std::floor(u))));
  const double t = u - static_cast<double>(i);
  if (t == 0) {
    return values[i];
  }
  if (t == 1) {
    return values[i + 1];
  }
  return std::fma(t, values[i + 1] - values[i], values[i]);
}
}  // namespace ps::plugin_internal::fast_curve

#include "01-numeric/fast_curve_simd.hpp"
