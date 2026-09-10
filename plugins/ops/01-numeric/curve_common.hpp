#pragma once

#include <algorithm>
#include <limits>

#include "00-foundation/basic_execution.hpp"

namespace ps::plugin_internal::basic_ops {
inline bool same_sign(double a, double b) {
  return (a > 0 && b > 0) || (a < 0 && b < 0);
}
inline double end_slope(double h0, double h1, double d0, double d1) {
  const double ratio = h0 / finite(h0 + h1);
  double slope = finite(d0 + finite(ratio * finite(d0 - d1)));
  if (!same_sign(slope, d0))
    return 0;
  if (!same_sign(d0, d1) && std::abs(slope / 3) > std::abs(d0))
    slope = finite(3 * d0);
  return slope;
}
template <class T>
void curve(Kind kind, const OperationInvocation& call, MutableValue* output) {
  const auto& controls = call.inputs[0];
  generic(controls);
  const auto k = controls.descriptor().shape[0];
  require(controls.descriptor().shape[1] == 2 && k >= 2,
          ErrorCode::TypeMismatch, "curve requires Kx2 controls, K>=2");
  const auto n = integer(call, "count");
  const auto a = parameter(call, "domain_min");
  const auto b = parameter(call, "domain_max");
  require(a < b, ErrorCode::InvalidArgument, "curve domain must increase");
  const auto policy = text(call, "out_of_domain");
  choice(policy, "reject", "clip");
  auto x = [&](std::uint64_t i) { return read<T>(controls, {i, 0}); };
  auto y = [&](std::uint64_t i) { return read<T>(controls, {i, 1}); };
  for (std::uint64_t i = 0; i < k; ++i) {
    if ((i & 255U) == 0)
      poll(call);
    finite(x(i));
    finite(y(i));
    require(i == 0 || x(i - 1) < x(i), ErrorCode::OperationFailed,
            "curve abscissas must strictly increase");
  }
  require(policy == "clip" || (a >= x(0) && b <= x(k - 1)),
          ErrorCode::OperationFailed, "curve sampling outside control domain");
  // Three Float64 arrays fit three times the smallest Kx2 input size.
  require(k <= std::numeric_limits<std::uint64_t>::max() / 24,
          ErrorCode::ResourceExhausted, "curve scratch size overflow");
  MutableBuffer scratch;
  if (kind == Kind::Monotone)
    scratch = take(call.allocator.allocate(k * 24));
  auto get = [&](std::uint64_t index) {
    double value;
    std::memcpy(&value, scratch.data() + index * 8, 8);
    return value;
  };
  auto put = [&](std::uint64_t index, double value) {
    finite(value);
    std::memcpy(scratch.data() + index * 8, &value, 8);
  };
  if (kind == Kind::Monotone) {
    for (std::uint64_t i = 0; i + 1 < k; ++i) {
      if ((i & 255U) == 0)
        poll(call);
      const double h = finite(static_cast<double>(x(i + 1)) - x(i));
      const double d = finite((static_cast<double>(y(i + 1)) - y(i)) / h);
      require(h > 0 && (y(i + 1) == y(i) || d != 0), ErrorCode::OperationFailed,
              "PCHIP secant cannot be represented");
      put(i, h);
      put(k + i, d);
    }
    put(2 * k, get(k));
    put(3 * k - 1, get(2 * k - 2));
    if (k > 2) {
      put(2 * k, end_slope(get(0), get(1), get(k), get(k + 1)));
      put(3 * k - 1,
          end_slope(get(k - 2), get(k - 3), get(2 * k - 2), get(2 * k - 3)));
      for (std::uint64_t i = 1; i + 1 < k; ++i) {
        if ((i & 255U) == 0)
          poll(call);
        const double d0 = get(k + i - 1), d1 = get(k + i);
        double slope = 0;
        if (same_sign(d0, d1)) {
          // Normalize both lengths and secants before the harmonic mean.
          const double h = std::max(get(i - 1), get(i));
          const double h0 = get(i - 1) / h, h1 = get(i) / h;
          const double w1 = 2 * h1 + h0, w2 = h1 + 2 * h0;
          const double d = std::min(std::abs(d0), std::abs(d1));
          slope = finite(std::copysign(d, d0) /
                         ((w1 * (d / std::abs(d0)) + w2 * (d / std::abs(d1))) /
                          (w1 + w2)));
        }
        put(2 * k + i, slope);
      }
    }
  }
  std::uint64_t segment = 0;
  double previous = a;
  for (std::uint64_t i = 0; i < n; ++i) {
    if ((i & 255U) == 0)
      poll(call);
    const double query = interpolate(static_cast<double>(i), 0,
                                     static_cast<double>(n - 1), a, b);
    require(i == 0 || query > previous, ErrorCode::OperationFailed,
            "curve sampling coordinates collapse");
    previous = query;
    double result;
    if (query <= x(0)) {
      result = y(0);
    } else if (query >= x(k - 1)) {
      result = y(k - 1);
    } else {
      while (segment + 2 < k && query > x(segment + 1)) {
        if ((segment & 255U) == 0)
          poll(call);
        ++segment;
      }
      const double left = x(segment), right = x(segment + 1);
      const double y0 = y(segment), y1 = y(segment + 1);
      if (kind == Kind::Linear || k == 2 || query == left || query == right) {
        result = interpolate(query, left, right, y0, y1);
      } else {
        const double t = fraction(query, left, right), u = 1 - t;
        const double h = get(segment);
        const double m0 = finite(h * get(2 * k + segment));
        const double m1 = finite(h * get(2 * k + segment + 1));
        result = finite(finite(y0 * (1 + 2 * t) * u * u) +
                        finite(y1 * t * t * (3 - 2 * t)) +
                        finite(m0 * t * u * u) - finite(m1 * t * t * u));
        // Shape preservation also holds after final floating rounding.
        result = std::clamp(result, std::min(y0, y1), std::max(y0, y1));
      }
    }
    store<T>(output->data(), i, result);
  }
}
}  // namespace ps::plugin_internal::basic_ops
