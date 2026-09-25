#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <optional>

#include "01-numeric/directed_color_functions.hpp"
#include "01-numeric/exact_root.hpp"
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
template <class Math = numeric_ops::DirectedInterval>
class StrictMathStorage final {
  using Interval = typename Math::Interval;
  using Frame = typename Math::Frame;
  numeric_ops::DirectedColorFunctionsStorage<Math> functions_;
  std::array<typename Math::Number, Program::kNodes> cache_low_{},
      cache_high_{};
  std::array<bool, Program::kNodes> valid_{};
  const Program* cached_ = nullptr;
  unsigned cached_precision_ = 0;
  struct Special {
    std::uint64_t bits;
  };

  // Whole-program rational fallback also resolves exact midpoint ties in toes,
  // squares and sqrt(3*x); no binary64 intermediate is introduced.
  std::uint64_t algebraic(const Program& p, double x, bool narrow,
                          const std::function<Status(std::uint64_t)>& consume) {
    std::array<Rational, Program::kNodes> values;
    for (unsigned i = 0; i < p.size; ++i) {
      const auto& n = p.nodes[i];
      auto status = consume(1);
      if (!status.ok())
        throw status;
      auto& out = values[i];
      switch (n.code) {
        case Code::Input:
          // The only admitted nonfinite input here is an inactive ACEScc floor.
          out = std::isfinite(x) ? Rational::binary(numeric_bits(x), false)
                                 : rational(0);
          break;
        case Code::Rational:
          out = rational(n.numerator, n.denominator);
          break;
        case Code::Binary:
          out = Rational::binary(n.bits, false);
          break;
        case Code::Add:
          out = Rational::add(values[n.a], values[n.b]);
          break;
        case Code::Subtract:
          out = Rational::subtract(values[n.a], values[n.b]);
          break;
        case Code::Multiply:
          out = Rational::multiply(values[n.a], values[n.b]);
          break;
        case Code::Divide:
          out = Rational::divide(values[n.a], values[n.b]);
          break;
        case Code::MaxZero:
          out = values[n.a].negative ? rational(0) : values[n.a];
          break;
        case Code::Sqrt: {
          const auto& value = values[n.a];
          if (value.negative && !value.n.words.empty())
            return nan_bits(narrow);
          if (value.n.words.size() > 2 * Math::kWords ||
              value.d.words.size() > 2 * Math::kWords)
            Math::capacity();
          auto& r = functions_.functions.math.rounding;
          r.numerator.words.fill(0);
          r.denominator.words.fill(0);
          r.negative = false;
          for (std::size_t j = 0; j < value.n.words.size(); ++j)
            r.numerator.words[j / 2] |=
                static_cast<std::uint64_t>(value.n.words[j]) << (32 * (j % 2));
          for (std::size_t j = 0; j < value.d.words.size(); ++j)
            r.denominator.words[j / 2] |=
                static_cast<std::uint64_t>(value.d.words[j]) << (32 * (j % 2));
          auto result = numeric_ops::round_sqrt_ratio(&r, narrow, 0, consume);
          if (!result.ok())
            throw result.status();
          return result.value();
        }
        default:
          throw std::logic_error("nonrational FMT-09 algebraic branch");
      }
    }
    auto value = values[p.result];
    // Formula-intrinsic exact zeros are +0, except separately restored signs.
    if (value.n.words.empty())
      value.negative = value.negative_zero = false;
    return value.floating_bits(narrow);
  }
  void exponential(Interval out, Interval x, bool narrow) {
    auto& m = functions_.functions.math;
    Frame frame(m);
    auto limit = m.interval();
    m.integer(limit, 2000);
    // All FMT-09 uses of exp/pow above this range are followed only by fixed
    // finite affine factors (<=10000 and >=1/12), never range-cancelling
    // ratios.
    if (m.compare(x.low, limit.high) > 0)
      throw Special{infinity_bits(narrow)};
    if (m.compare(x.high, limit.low) > 0)
      throw typename Math::Unresolved{};
    functions_.exponential(out, x);
  }
  void power(Interval out, Interval base, Interval exponent, bool narrow) {
    auto& m = functions_.functions.math;
    if (base.high.negative)
      throw Special{nan_bits(narrow)};
    if (base.low.negative)
      throw typename Math::Unresolved{};
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
        if (get(n.a).high.negative)
          throw Special{nan_bits(narrow)};
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
        throw std::logic_error("sqrt must use exact rational branch");
    }
  }

 public:
  explicit StrictMathStorage(SequenceProfile profile) : functions_(profile) {}
  Result<std::uint64_t> evaluate(
      const Program& p, double x, bool narrow,
      const std::function<Status(std::uint64_t)>& consume) {
    using R = Result<std::uint64_t>;
    auto& m = functions_.functions.math;
    m.consume = &consume;
    struct End {
      Math& m;
      ~End() {
        m.consume = nullptr;
        m.used = 0;
      }
    } end{m};
    try {
      if (p.final_sqrt || !std::isfinite(x)) {
        if constexpr (Math::kWords < 192)
          throw numeric_ops::DirectedCapacityRetry{};
        return R(algebraic(p, x, narrow, consume));
      }
      constexpr unsigned maximum = Math::kWords < 192 ? 256 : 4096;
      for (unsigned precision = 128; precision <= maximum; precision *= 2) {
        m.precision = precision;
        m.used = 0;
        if (cached_ != &p || cached_precision_ != precision) {
          valid_.fill(false);
          cached_ = &p;
          cached_precision_ = precision;
        }
        try {
          std::array<unsigned, Program::kNodes> indices{};
          for (unsigned i = 0; i < p.size; ++i) {
            indices[i] = static_cast<unsigned>(m.used);
            auto out = m.interval();
            const auto& node = p.nodes[i];
            if (!node.varying && valid_[i]) {
              m.copy(out.low, cache_low_[i]);
              m.copy(out.high, cache_high_[i]);
            } else {
              evaluate_node(out, node, indices, x, narrow);
              if (!node.varying) {
                m.copy(cache_low_[i], out.low);
                m.copy(cache_high_[i], out.high);
                valid_[i] = true;
              }
            }
          }
          const auto lo = m.rounded(m.pool[indices[p.result]], narrow);
          const auto hi = m.rounded(m.pool[indices[p.result] + 1], narrow);
          if (lo == hi)
            return R(lo);
        } catch (const typename Math::Unresolved&) {
        }
        if (p.rational) {
          if constexpr (Math::kWords < 192)
            throw numeric_ops::DirectedCapacityRetry{};
          return R(algebraic(p, x, narrow, consume));
        }
      }
      if constexpr (Math::kWords < 192)
        throw numeric_ops::DirectedCapacityRetry{};
      return R(Status{ErrorCode::ResourceExhausted,
                      "FMT-09 strict rounding exceeded 4096-bit refinement",
                      FailureReason::CapacityLimit});
    } catch (const Special& result) {
      return R(result.bits);
    } catch (const Status& status) {
      return R(status);
    } catch (const data_internal::format_numeric::ExactWorkFailure& failure) {
      return R(failure.status);
    }
  }
};
using StrictMath = StrictMathStorage<>;
// 1024-bit storage at 128/256-bit working precision. Exact endpoint agreement
// is still required; only internal capacity/rounding uncertainty retries the
// original 12288-bit storage, up to 4096-bit working precision.
using CompactMath = StrictMathStorage<numeric_ops::DirectedIntervalStorage<16>>;
}  // namespace ps::plugin_internal::transfer_ops
