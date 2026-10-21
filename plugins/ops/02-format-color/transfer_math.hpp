#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <optional>

#include "01-numeric/directed_color_functions.hpp"
#include "02-format-color/transfer_algebraic.hpp"
#include "02-format-color/transfer_constants.hpp"
#include "02-format-color/transfer_program.hpp"

namespace ps::plugin_internal::transfer_ops {
inline std::uint64_t nan_bits(bool narrow) {
  return narrow ? UINT64_C(0x7fc00000) : UINT64_C(0x7ff8000000000000);
}
inline std::uint64_t infinity_bits(bool narrow) {
  return narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
}
inline std::uint64_t sign_mask(bool narrow) {
  return UINT64_C(1) << (narrow ? 31 : 63);
}
inline std::optional<std::uint64_t> anchor(const CurveProgram& c,
                                           std::uint64_t bits, bool narrow) {
  const double x = numeric_double(bits, narrow), u = std::abs(x);
  const auto sign = c.signed_curve ? bits & sign_mask(narrow) : 0;
  const auto k = c.definition.curve;
  if (c.identity)
    return bits;
  if (c.signed_curve && (u == 0 || u == 1))
    return numeric_bits(u, narrow) | sign;
  if (k == TransferCurve::Bt1886) {
    const double lb = *c.definition.black_luminance,
                 lw = *c.definition.white_luminance;
    if (!c.encode && (x == 0 || x == 1))
      return numeric_bits(x == 0 ? lb : lw, narrow) & ~sign_mask(narrow);
    if (c.encode && (x == lb || x == lw))
      return numeric_bits(x == lb ? 0 : 1, narrow);
  }
  if (k == TransferCurve::Pq) {
    if (!c.encode && (x == 0 || x == 1))
      return numeric_bits(x == 0 ? 0 : 10000, narrow);
    if (c.encode && x == 10000)
      return numeric_bits(1, narrow);
  }
  return {};
}
// Handles NUM nonfinite outcomes before comparisons or inactive branches.
// A null result means the selected finite/constant program must still execute.
inline std::optional<std::uint64_t> nonfinite(const CurveProgram& c,
                                              std::uint64_t bits, bool narrow) {
  const auto p = numeric_ops::BinaryParts::decode(bits, narrow);
  if (c.identity)
    return bits;
  if (p.nan)
    return bits | (UINT64_C(1) << (narrow ? 22 : 51));
  if (!p.infinite)
    return {};
  if (c.signed_curve)
    return bits;
  const auto inf = infinity_bits(narrow), nan = nan_bits(narrow);
  switch (c.definition.curve) {
    case TransferCurve::Bt1886:
      return c.encode ? (p.negative ? nan : inf) : (p.negative ? 0 : inf);
    case TransferCurve::Pq:
      return nan;
    case TransferCurve::HlgOetf:
      return c.encode && p.negative ? nan : inf;
    case TransferCurve::Acescc:
      if (c.encode)
        return p.negative ? std::optional<std::uint64_t>{} : inf;
      return numeric_bits(p.negative ? -0x1p-15 : 65504, narrow);
    case TransferCurve::Acescct:
      return !c.encode && !p.negative ? numeric_bits(65504, narrow) : bits;
    default:
      return bits;
  }
}
template <class Math = numeric_ops::CheckedDirectedInterval>
class StrictMathStorage final {
  using Interval = typename Math::Interval;
  using Frame = typename Math::Frame;
  numeric_ops::DirectedColorFunctionsStorage<Math> functions_;
  std::array<typename Math::Number, Program::kNodes> cache_low_{},
      cache_high_{};
  std::array<bool, Program::kNodes> valid_{};
  const Program* cached_ = nullptr;
  unsigned cached_precision_ = 0;
  static_assert(Math::kChecked);
  using Fault = typename Math::Fault;
  AlgebraicMath algebraic_;
  std::optional<std::uint64_t> special_;
  void exponential(Interval out, Interval x, bool narrow) {
    auto& m = functions_.functions.math;
    if (!m.active())
      return;
    Frame frame(m);
    auto limit = m.interval();
    m.integer(limit, 2000);
    // All FMT-09 uses of exp/pow above this range are followed only by fixed
    // finite affine factors (<=10000 and >=1/12), never range-cancelling
    // ratios.
    if (m.compare(x.low, limit.high) > 0) {
      special_ = infinity_bits(narrow);
      return;
    }
    if (m.compare(x.high, limit.low) > 0) {
      m.retry();
      return;
    }
    functions_.exponential(out, x);
  }
  void power(Interval out, Interval base, Interval exponent, bool narrow) {
    auto& m = functions_.functions.math;
    if (!m.active())
      return;
    if (base.high.negative) {
      special_ = nan_bits(narrow);
      return;
    }
    if (base.low.negative) {
      m.retry();
      return;
    }
    if (m.top(base.high) < 0) {
      m.integer(out, 0);
      return;
    }
    Frame frame(m);
    auto log = m.interval(), product = m.interval();
    if (m.top(base.low) < 0) {
      // Monotone nonnegative base, strictly positive exact exponent. Refinement
      // supplies relative information when a small operand was truncated.
      functions_.logarithm_point(log, base.high);
      m.multiply(product, log, exponent);
      exponential(out, product, narrow);
      m.clear(out.low);
      return;
    }
    functions_.logarithm(log, base);
    m.multiply(product, log, exponent);
    exponential(out, product, narrow);
  }
  void evaluate_node(Interval out, const Node& n,
                     const std::array<unsigned, Program::kNodes>& indices,
                     double x, bool narrow) {
    auto& m = functions_.functions.math;
    if (!m.active())
      return;
    auto get = [&](unsigned i) -> Interval {
      return {m.pool[indices[i]], m.pool[indices[i] + 1]};
    };
    switch (n.code) {
      case Code::Input:
        m.raw(out, numeric_bits(x), false);
        break;
      case Code::Rational:
        m.integer(out, n.numerator);
        m.divide_small(out, out, n.denominator);
        break;
      case Code::Binary:
        m.raw(out, n.bits, false);
        break;
      case Code::Beta: {
        if constexpr (Math::kWords < 192) {
          m.constant_prefix(out, beta_floor_4096);
          break;
        }
        Frame frame(m);
        auto& value = m.number();
        std::copy(beta_floor_4096.begin(), beta_floor_4096.end(),
                  value.magnitude.words.begin());
        m.shift_unsigned(out.low.magnitude, value.magnitude,
                         static_cast<int>(m.precision) - 4096);
        out.low.negative = false;
        m.copy(out.high, out.low);
        m.increment(out.high.magnitude);
        break;
      }
      case Code::Ln2:
        m.constant(out, false);
        break;
      case Code::Add:
        m.add(out, get(n.a), get(n.b));
        break;
      case Code::Subtract:
        m.subtract(out, get(n.a), get(n.b));
        break;
      case Code::Multiply:
        m.multiply(out, get(n.a), get(n.b));
        break;
      case Code::Divide:
        m.divide(out, get(n.a), get(n.b));
        break;
      case Code::Power:
        power(out, get(n.a), get(n.b), narrow);
        break;
      case Code::Exp:
        exponential(out, get(n.a), narrow);
        break;
      case Code::Log:
        if (get(n.a).high.negative) {
          special_ = nan_bits(narrow);
          return;
        }
        functions_.logarithm(out, get(n.a));
        break;
      case Code::MaxZero:
        m.copy(out, get(n.a));
        if (out.low.negative)
          m.clear(out.low);
        if (out.high.negative)
          m.clear(out.high);
        break;
      case Code::Sqrt:
        m.fail(ErrorCode::Internal);
        break;
    }
  }

 public:
  explicit StrictMathStorage(SequenceProfile profile) : functions_(profile) {}
  // A compact capacity/rounding retry is returned separately from operational
  // failure. Cancellation and caller resource failures are never retried.
  Result<std::uint64_t> evaluate(const Program& p, double x, bool narrow,
                                 const core_internal::WorkConsumer& consume,
                                 bool* needs_wide = nullptr) {
    using R = Result<std::uint64_t>;
    if (needs_wide)
      *needs_wide = false;
    if (p.rational)
      return algebraic_.evaluate(p, x, narrow, consume);
    auto& m = functions_.functions.math;
    m.reset();
    m.consume = &consume;
    struct End {
      Math& m;
      ~End() {
        m.consume = nullptr;
        m.used = 0;
      }
    } end{m};
    constexpr unsigned maximum = Math::kWords < 192 ? 256 : 4096;
    for (unsigned precision = 128; precision <= maximum; precision *= 2) {
      m.reset();
      m.precision = precision;
      special_.reset();
      if (cached_ != &p || cached_precision_ != precision) {
        valid_.fill(false);
        cached_ = &p;
        cached_precision_ = precision;
      }
      std::array<unsigned, Program::kNodes> indices{};
      for (unsigned i = 0; i < p.size && m.active(); ++i) {
        indices[i] = static_cast<unsigned>(m.used);
        auto out = m.interval();
        if (!m.active())
          break;
        const auto& node = p.nodes[i];
        if (!node.varying && valid_[i]) {
          m.copy(out.low, cache_low_[i]);
          m.copy(out.high, cache_high_[i]);
        } else {
          evaluate_node(out, node, indices, x, narrow);
          if (special_ && m.active())
            return R(*special_);
          if (!node.varying && m.active()) {
            m.copy(cache_low_[i], out.low);
            m.copy(cache_high_[i], out.high);
            if (m.active())
              valid_[i] = true;
          }
        }
      }
      if (m.active()) {
        const auto lo = m.rounded(m.pool[indices[p.result]], narrow);
        const auto hi = m.rounded(m.pool[indices[p.result] + 1], narrow);
        if (m.active() && lo == hi)
          return R(lo);
      }
      if (m.fault == Fault::Failure) {
        if constexpr (Math::kWords < 192) {
          if (m.capacity_exceeded && needs_wide) {
            *needs_wide = true;
            return R(UINT64_C(0));
          }
        }
        return R(m.status());
      }
    }
    if constexpr (Math::kWords < 192) {
      if (needs_wide) {
        *needs_wide = true;
        return R(UINT64_C(0));
      }
    }
    return R(
        Status{ErrorCode::ResourceExhausted, {}, FailureReason::CapacityLimit});
  }
};
using StrictMath = StrictMathStorage<>;
// 512-bit compact storage at 128/256-bit working precision. Endpoint agreement
// is mandatory; only internal precision/capacity outcomes request the wide
// tier.
using CompactMath = StrictMathStorage<numeric_ops::DirectedIntervalStorage<
    8, true>>;  // NOLINT(whitespace/indent_namespace)
}  // namespace ps::plugin_internal::transfer_ops
