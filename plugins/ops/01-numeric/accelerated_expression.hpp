#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include "01-numeric/accelerated_math.hpp"
#include "01-numeric/expression_program.hpp"
#include "photospider/core/status.hpp"

namespace ps::plugin_internal::numeric_ops {
// Accounted as part of ExpressionEvaluator's continuation. Four samples cover
// two NEON vectors or one AVX2 vector. Intervals enclose the stepwise RN64 AST,
// not an exact-real reassociation of the whole source expression.
struct AcceleratedExpression final {
  static constexpr std::size_t kLanes = 4;
  struct Node {
    std::array<double, kLanes> value{};
    std::array<FastInterval, kLanes> bound{};
  };
  std::array<Node, 256> nodes;
  Status evaluate(const ExpressionProgram& program, const std::uint64_t* x,
                  const std::array<std::uint64_t, 256>& coefficients,
                  std::size_t count, bool narrow, std::uint64_t* output,
                  bool* accepted,
                  const std::function<Status(std::uint64_t)>& consume,
                  bool environment_established = false) {
    using K = ExpressionKind;
    // Whole callers may hold the environment across all coordinate/AST batches.
    std::optional<input_internal::Float32Environment> environment;
    if (!environment_established)
      environment.emplace();
    for (std::size_t lane = 0; lane < count; ++lane)
      accepted[lane] = (environment_established || environment->active()) &&
                       accelerated_math_available();
    for (unsigned i = 0; i < program.size; ++i) {
      auto charged = consume(count * 32);
      if (!charged.ok())
        return charged;
      const auto& node = program.nodes[i];
      auto& result = nodes[i];
      const auto& left = nodes[node.left];
      const auto& right = nodes[node.right];
      const bool function =
          (node.kind >= K::Exp && node.kind <= K::Tan) || node.kind == K::Power;
      std::array<double, kLanes> inputs{}, lows{}, highs{}, middle{}, lower{},
          upper{}, exponents{}, exponent_low{}, exponent_high{}, cross_low{},
          cross_high{};
      const unsigned kind = node.kind == K::Exp     ? 0
                            : node.kind == K::Ln    ? 1
                            : node.kind == K::Sin   ? 2
                            : node.kind == K::Cos   ? 3
                            : node.kind == K::Power ? 10
                                                    : 4;
      for (std::size_t lane = 0; lane < count; ++lane) {
        if (!accepted[lane])
          continue;
        const auto a = left.bound[lane], b = right.bound[lane];
        const double u = left.value[lane], v = right.value[lane];
        double value = 0;
        FastInterval bound;
        switch (node.kind) {
          case K::Literal:
            value = numeric_double(node.bits);
            bound = FastInterval::point(value);
            break;
          case K::X:
            value = numeric_double(x[lane]);
            bound = FastInterval::point(value);
            break;
          case K::Coefficient:
            value = numeric_double(coefficients[node.left]);
            bound = FastInterval::point(value);
            break;
          case K::Positive:
            value = u;
            bound = a;
            break;
          case K::Negative:
            value = -u;
            bound = -a;
            break;
          case K::Add:
            value = u + v;
            bound = a + b;
            break;
          case K::Subtract:
            value = u - v;
            bound = a - b;
            break;
          case K::Multiply:
            value = u * v;
            bound = a * b;
            break;
          case K::Divide:
            if (!b.nonzero()) {
              accepted[lane] = false;
              break;
            }
            value = u / v;
            bound = a / b;
            break;
          case K::Abs:
            value = std::abs(u);
            bound = {
                a.nonzero() ? std::min(std::abs(a.low), std::abs(a.high)) : 0,
                std::max(std::abs(a.low), std::abs(a.high))};
            break;
          case K::Sqrt:
            if (a.low < 0) {
              accepted[lane] = false;
              break;
            }
            value = std::sqrt(u);
            bound = {numeric_down(std::sqrt(a.low)),
                     numeric_up(std::sqrt(a.high))};
            break;
          case K::Min:
            value = std::min(u, v);
            bound = {std::min(a.low, b.low), std::min(a.high, b.high)};
            break;
          case K::Max:
            value = std::max(u, v);
            bound = {std::max(a.low, b.low), std::max(a.high, b.high)};
            break;
          default:
            accepted[lane] = accelerated_math_domain(kind, a.low, b.low) &&
                             accelerated_math_domain(kind, a.high, b.high) &&
                             accelerated_math_domain(kind, u, v);
            if (accepted[lane]) {
              inputs[lane] = u;
              lows[lane] = a.low;
              highs[lane] = a.high;
              exponents[lane] = v;
              exponent_low[lane] = b.low;
              exponent_high[lane] = b.high;
            }
            break;
        }
        if (!function) {
          result.value[lane] = value;
          result.bound[lane] = bound;
          accepted[lane] =
              accepted[lane] && std::isfinite(value) && bound.finite();
        }
      }
      if (function) {
        bool any = false;
        for (std::size_t lane = 0; lane < count; ++lane) {
          any = any || accepted[lane];
          if (!accepted[lane])
            inputs[lane] = lows[lane] = highs[lane] =
                (kind == 1 || kind == 10) ? 1 : 0;
        }
        if (!any)
          continue;
        photospider_sleef_evaluate(kind, inputs.data(), exponents.data(),
                                   middle.data(), count);
        photospider_sleef_evaluate(kind, lows.data(), exponent_low.data(),
                                   lower.data(), count);
        photospider_sleef_evaluate(kind, highs.data(), exponent_high.data(),
                                   upper.data(), count);
        if (kind == 10) {
          photospider_sleef_evaluate(kind, lows.data(), exponent_high.data(),
                                     cross_low.data(), count);
          photospider_sleef_evaluate(kind, highs.data(), exponent_low.data(),
                                     cross_high.data(), count);
        }
        for (std::size_t lane = 0; lane < count; ++lane) {
          if (!accepted[lane])
            continue;
          auto l = accelerated_math_enclosure(lower[lane]);
          auto h = accelerated_math_enclosure(upper[lane]);
          FastInterval bound{std::min(l.low, h.low), std::max(l.high, h.high)};
          if (kind == 10) {
            const auto a = accelerated_math_enclosure(cross_low[lane]);
            const auto b = accelerated_math_enclosure(cross_high[lane]);
            bound.low = std::min({bound.low, a.low, b.low});
            bound.high = std::max({bound.high, a.high, b.high});
          }
          if (kind == 3 && left.bound[lane].low <= 0 &&
              left.bound[lane].high >= 0)
            bound.high = 1;
          result.value[lane] = middle[lane];
          result.bound[lane] = bound;
          accepted[lane] = bound.finite() && std::isfinite(middle[lane]);
        }
      }
    }
    const auto& last = nodes[program.size - 1];
    for (std::size_t lane = 0; lane < count; ++lane) {
      if (!accepted[lane])
        continue;
      auto bits = last.bound[lane].accepted(last.value[lane], narrow);
      accepted[lane] = bits.has_value();
      if (bits)
        output[lane] = *bits;
    }
    return Status::success();
  }
};
}  // namespace ps::plugin_internal::numeric_ops
