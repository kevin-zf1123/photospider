#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

#include "02-format-color/transfer_program.hpp"
#include "execution/work_consumer.hpp"

#ifndef PHOTOSPIDER_TRANSFER_FAST_MATH
#define PHOTOSPIDER_TRANSFER_FAST_MATH 1
#endif

namespace ps::plugin_internal::transfer_ops {
// Sixteen binary64 lanes, processed by the AVX2/NEON vectors in the existing
// private, pinned SLEEF adapter. Each branch is gathered from REQUESTED
// samples; no inactive branch, padding, or adjacent plane supplies a SIMD
// operand.
class FastMath final {
 public:
  static constexpr unsigned kLanes = 16;

 private:
  using I = numeric_ops::FastInterval;
  std::array<std::array<I, kLanes>, Program::kNodes> values_{};
  std::array<std::array<I, Program::kNodes>, 3> constants_{};
  std::array<std::array<bool, Program::kNodes>, 3> ready_{};
  static double down(double v) { return numeric_ops::numeric_down(v); }
  static double up(double v) { return numeric_ops::numeric_up(v); }
  static I constant(const Node& n) {
    if (n.code == Code::Binary)
      return I::point(numeric_double(n.bits));
    if (n.code == Code::Beta)
      return {0x1.27cbd51448a2dp-6, 0x1.27cbd51448a2ep-6};
    if (n.code == Code::Ln2)
      return {0x1.62e42fefa39efp-1, 0x1.62e42fefa39f0p-1};
    // All shipped rational numerators and denominators are exactly
    // representable as binary64 integers, including powers of ten <=10^16.
    return I::point(static_cast<double>(n.numerator)) /
           I::point(static_cast<double>(n.denominator));
  }

 public:
  Status evaluate(const Program& p, unsigned branch, const double* input,
                  unsigned count, bool narrow, bool strict,
                  std::uint64_t* output, bool* accepted,
                  const execution_internal::WorkConsumer& consume,
                  bool environment_active) {
    for (unsigned lane = 0; lane < count; ++lane)
      accepted[lane] = PHOTOSPIDER_TRANSFER_FAST_MATH && environment_active &&
                       numeric_ops::accelerated_math_available() &&
                       std::isfinite(input[lane]);
    for (unsigned i = 0; i < p.size; ++i) {
      const auto& status = consume(count * 16);
      if (!status.ok())
        return status;
      const auto& n = p.nodes[i];
      auto& result = values_[i];
      if (!n.varying && ready_[branch][i]) {
        for (unsigned lane = 0; lane < count; ++lane)
          result[lane] = constants_[branch][i];
        continue;
      }
      const bool function =
          n.code == Code::Power || n.code == Code::Exp || n.code == Code::Log;
      std::array<double, kLanes> lows, highs, lo_exponents, hi_exponents, lower,
          upper;
      bool any = false;
      for (unsigned lane = 0; lane < count; ++lane) {
        // Safe duplicated operands also protect SIMD's unused physical lanes.
        if (function) {
          lows[lane] = highs[lane] = n.code == Code::Exp ? 0 : 1;
          lo_exponents[lane] = hi_exponents[lane] = 1;
        }
        if (!accepted[lane])
          continue;
        const auto a = values_[n.a][lane], b = values_[n.b][lane];
        switch (n.code) {
          case Code::Input:
            result[lane] = I::point(input[lane]);
            break;
          case Code::Rational:
          case Code::Binary:
          case Code::Beta:
          case Code::Ln2:
            result[lane] = constant(n);
            break;
          case Code::Add:
            result[lane] = a + b;
            break;
          case Code::Subtract:
            result[lane] = a - b;
            break;
          case Code::Multiply:
            result[lane] = a * b;
            break;
          case Code::Divide:
            result[lane] = a / b;
            break;
          case Code::MaxZero:
            result[lane] = {std::max(0.0, a.low), std::max(0.0, a.high)};
            break;
          case Code::Sqrt:
            if (a.low < 0)
              accepted[lane] = false;
            else
              result[lane] = {down(std::sqrt(a.low)), up(std::sqrt(a.high))};
            break;
          case Code::Power:
            // Positive base/exponent domain. The U10 primitive's error is
            // enclosed at BOTH monotone extrema, including exponent error.
            accepted[lane] = a.finite() && b.finite() && a.low >= 0x1p-100 &&
                             a.high <= 0x1p100 && b.low > 0 && b.high <= 128;
            if (accepted[lane]) {
              lows[lane] = a.low;
              highs[lane] = a.high;
              lo_exponents[lane] = a.low < 1 ? b.high : b.low;
              hi_exponents[lane] = a.high < 1 ? b.low : b.high;
            }
            break;
          case Code::Exp:
          case Code::Log:
            accepted[lane] =
                a.finite() &&
                (n.code == Code::Exp ? a.low >= -80 && a.high <= 80
                                     : a.low >= 0x1p-100 && a.high <= 0x1p100);
            if (accepted[lane]) {
              lows[lane] = a.low;
              highs[lane] = a.high;
            }
            break;
        }
        if (!function)
          accepted[lane] = accepted[lane] && result[lane].finite();
        any = any || accepted[lane];
      }
      if (function && any) {
        const unsigned kind = n.code == Code::Power ? 10
                              : n.code == Code::Exp ? 0
                                                    : 1;
        photospider_sleef_evaluate(kind, lows.data(), lo_exponents.data(),
                                   lower.data(), count);
        photospider_sleef_evaluate(kind, highs.data(), hi_exponents.data(),
                                   upper.data(), count);
        for (unsigned lane = 0; lane < count; ++lane) {
          if (!accepted[lane])
            continue;
          const auto l = numeric_ops::accelerated_math_enclosure(lower[lane]);
          const auto h = numeric_ops::accelerated_math_enclosure(upper[lane]);
          result[lane] = {l.low, h.high};
          accepted[lane] = result[lane].finite();
        }
      }
      if (!n.varying) {
        for (unsigned lane = 0; lane < count; ++lane)
          if (accepted[lane]) {
            constants_[branch][i] = result[lane];
            ready_[branch][i] = true;
            break;
          }
      }
    }
    for (unsigned lane = 0; lane < count; ++lane) {
      if (!accepted[lane])
        continue;
      const auto interval = values_[p.result][lane];
      const auto lo = numeric_bits(interval.low, narrow),
                 hi = numeric_bits(interval.high, narrow);
      if (lo == hi) {
        output[lane] = lo;
        continue;
      }
      if (strict) {
        accepted[lane] = false;
        continue;
      }
      const double candidate = interval.low * 0.5 + interval.high * 0.5;
      const auto result = interval.accepted(candidate, narrow);
      accepted[lane] = result.has_value();
      if (result)
        output[lane] = *result;
    }
    return Status::success();
  }
};
}  // namespace ps::plugin_internal::transfer_ops
