#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <numeric>
#include <optional>
#include <utility>

#include "01-numeric/directed_functions.hpp"
#include "01-numeric/numeric_nan.hpp"

namespace ps::plugin_internal::numeric_ops {
// Whole log-ratio / exponential interpolation. The adapter validates matching
// dtypes and 0<lower<upper before this entry, including special input values.
class ExactShaper final {
  DirectedFunctions functions_;
  RatioWorkspace exact_;
  using Interval = DirectedInterval::Interval;
  using Frame = DirectedInterval::Frame;
  struct Odd {
    std::uint64_t value;
    int exponent;
  };
  static Odd odd(BinaryParts value) {
    const auto trailing = __builtin_ctzll(value.significand);
    return {value.significand >> trailing, value.exponent + trailing};
  }
  void work(std::uint64_t count) { functions_.math.work(count); }
  std::uint64_t round(std::uint64_t numerator, std::uint64_t denominator,
                      bool narrow, int scale, bool negative = false) {
    work(2 * exact_.numerator.words.size());
    exact_.numerator.words.fill(0);
    exact_.numerator.words[0] = numerator;
    exact_.denominator.words.fill(0);
    exact_.denominator.words[0] = denominator;
    exact_.negative = negative;
    auto result = exact_.round(narrow, *functions_.math.consume, scale);
    if (!result.ok())
      throw result.status();
    return result.value();
  }
  static std::uint64_t infinity(bool narrow) {
    return narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
  }
  std::uint64_t dyadic(std::uint64_t core, int scale, bool narrow) {
    const auto leading = 63 - __builtin_clzll(core) + scale;
    if (leading >= (narrow ? 128 : 1024))
      return infinity(narrow);
    if (leading < (narrow ? -150 : -1075))
      return 0;
    return round(core, 1, narrow, scale);
  }
  std::optional<std::uint64_t> square_root(std::uint64_t value) {
    work(128);
    std::uint64_t low = 0, high = UINT64_C(1) << 27;
    while (high - low > 1) {
      const auto middle = (low + high) / 2;
      if (static_cast<unsigned __int128>(middle) * middle <= value)
        low = middle;
      else
        high = middle;
    }
    return low * low == value ? std::optional<std::uint64_t>(low)
                              : std::nullopt;
  }
  std::optional<std::uint64_t> inverse_dyadic(BinaryParts input,
                                              BinaryParts lower,
                                              BinaryParts upper, bool narrow) {
    const auto l = odd(lower), u = odd(upper), t = odd(input);
    const auto divisor = std::gcd(l.value, u.value);
    std::uint64_t a = u.value / divisor, b = l.value / divisor;
    int difference = u.exponent - l.exponent;
    if (t.exponent < 0) {
      const auto roots = static_cast<unsigned>(-t.exponent);
      if (roots >= 12 && difference)
        return {};
      if (roots < 12) {
        if (difference % (1 << roots))
          return {};
        difference /= (1 << roots);
      }
      if (roots >= 6 && (a != 1 || b != 1))
        return {};
      for (unsigned i = 0; i < roots && (a != 1 || b != 1); ++i) {
        const auto ar = square_root(a), br = square_root(b);
        if (!ar || !br)
          return {};
        a = *ar;
        b = *br;
      }
    }
    if (input.negative) {
      std::swap(a, b);
      difference = -difference;
    }
    if (a == 1 && b == 1) {
      if (!difference)
        return dyadic(l.value, l.exponent, narrow);
      const auto magnitude =
          static_cast<unsigned>(difference < 0 ? -difference : difference);
      // The source lower exponent is bounded by 1074. An additional binary
      // magnitude above 8192 is unambiguously outside both destination ranges.
      if (t.exponent >= 14)
        return difference < 0 ? 0 : infinity(narrow);
      const auto product = (static_cast<unsigned __int128>(t.value) * magnitude)
                           << std::max(t.exponent, 0);
      if (product > 8192)
        return difference < 0 ? 0 : infinity(narrow);
      const auto scale =
          l.exponent + (difference < 0 ? -static_cast<int>(product)
                                       : static_cast<int>(product));
      return dyadic(l.value, scale, narrow);
    }
    if (t.exponent > 6 || t.value > 64)
      return {};
    const auto multiplier = t.value << std::max(t.exponent, 0);
    if (multiplier > 64)
      return {};
    std::uint64_t core = l.value;
    // Coprime a,b make this divisibility condition necessary and sufficient
    // for a dyadic complete result. Keep lower in the formula during reduction.
    for (std::uint64_t i = 0; i < multiplier && b != 1; ++i) {
      work(1);
      if (core % b)
        return {};
      core /= b;
    }
    const auto limit = UINT64_C(1) << (narrow ? 25 : 54);
    for (std::uint64_t i = 0; i < multiplier; ++i) {
      work(1);
      if (core > (limit - 1) / a)
        return {};
      core *= a;
    }
    return dyadic(core, l.exponent + difference * static_cast<int>(multiplier),
                  narrow);
  }
  std::uint64_t round_scaled(const DirectedInterval::Number& value, bool narrow,
                             int scale) {
    auto& math = functions_.math;
    work(2 * DirectedInterval::kWords);
    math.rounding.numerator = value.magnitude;
    math.rounding.denominator.words.fill(0);
    math.rounding.denominator.words[0] = 1;
    math.rounding.negative = value.negative;
    auto result = math.rounding.round(narrow, *math.consume,
                                      scale - static_cast<int>(math.precision));
    if (!result.ok())
      throw result.status();
    return result.value();
  }
  int exponential(Interval output, Interval z) {
    auto& math = functions_.math;
    Frame frame(math);
    auto limit = math.interval();
    math.integer(limit, 1024);
    if (math.compare(z.low, limit.high) >= 0)
      throw DirectedFunctions::RangeResult{true};
    math.negate(limit, limit);
    if (math.compare(z.high, limit.low) <= 0)
      throw DirectedFunctions::RangeResult{false};
    if (math.compare(z.low, limit.low) < 0)
      throw DirectedInterval::Unresolved{};
    math.negate(limit, limit);
    if (math.compare(z.high, limit.high) > 0)
      throw DirectedInterval::Unresolved{};
    auto ln2 = math.interval(), quotient = math.interval(),
         multiple = math.interval(), residual = math.interval();
    math.constant(ln2, false);
    math.divide(quotient, z, ln2);
    auto& integral = math.number();
    math.shift(integral, quotient.low, -static_cast<int>(math.precision), true);
    if (math.top(integral) >= 12)
      throw DirectedInterval::Unresolved{};
    const auto magnitude = static_cast<int>(integral.magnitude.words[0]);
    const int exponent = integral.negative ? -magnitude : magnitude;
    math.integer(multiple, exponent);
    math.multiply(multiple, multiple, ln2);
    math.subtract(residual, z, multiple);
    math.integer(limit, 1);
    if (math.compare(residual.high, limit.high) > 0)
      throw DirectedInterval::Unresolved{};
    math.negate(limit, limit);
    if (math.compare(residual.low, limit.low) < 0)
      throw DirectedInterval::Unresolved{};
    functions_.exponential(output, residual);
    return exponent;
  }

 public:
  explicit ExactShaper(SequenceProfile profile)
      : functions_(SequenceProfile::Strict), exact_(profile) {}
  Result<std::uint64_t> evaluate(
      std::uint64_t bits, std::uint64_t lower, std::uint64_t upper,
      bool inverse, bool narrow,
      const std::function<Status(std::uint64_t)>& consume,
      const std::function<Status()>& interval_attempt) {
    using Answer = Result<std::uint64_t>;
    auto& math = functions_.math;
    math.used = 0;
    math.consume = &consume;
    struct End {
      DirectedInterval& math;
      ~End() { math.consume = nullptr; }
    } end{math};
    try {
      work(16);
      const auto x = BinaryParts::decode(bits, narrow),
                 l = BinaryParts::decode(lower, narrow),
                 u = BinaryParts::decode(upper, narrow);
      const auto type = narrow ? ElementType::Float32 : ElementType::Float64;
      const auto one =
          narrow ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000);
      if (x.nan)
        return Answer(converted_nan(bits, type, type));
      if (inverse) {
        if (!x.magnitude)
          return Answer(lower);
        if (bits == one)
          return Answer(upper);
        if (x.infinite)
          return Answer(x.negative ? 0 : infinity(narrow));
        if (auto exact = inverse_dyadic(x, l, u, narrow))
          return Answer(*exact);
      } else {
        if (!x.magnitude)
          return Answer(infinity(narrow) | (UINT64_C(1) << (narrow ? 31 : 63)));
        if (x.negative)
          return Answer(narrow ? UINT64_C(0x7fc00000)
                               : UINT64_C(0x7ff8000000000000));
        if (x.infinite)
          return Answer(infinity(narrow));
        if (bits == lower)
          return Answer(0);
        if (bits == upper)
          return Answer(one);
        const auto ox = odd(x), ol = odd(l), ou = odd(u);
        if (ox.value == ol.value && ol.value == ou.value) {
          const auto numerator = ox.exponent - ol.exponent;
          return Answer(round(numerator < 0 ? -numerator : numerator,
                              ou.exponent - ol.exponent, narrow, 0,
                              numerator < 0));
        }
      }
      auto reported = interval_attempt();
      if (!reported.ok())
        return Answer(reported);
      for (unsigned precision = 128; precision <= 4096; precision *= 2) {
        work(1);
        math.precision = precision;
        Frame frame(math);
        try {
          auto log_lower = math.interval(), log_upper = math.interval(),
               delta = math.interval(), input = math.interval(),
               numerator = math.interval(), output = math.interval();
          functions_.logarithm(log_lower, l);
          functions_.logarithm(log_upper, u);
          math.subtract(delta, log_upper, log_lower);
          int scale = 0;
          if (inverse) {
            math.raw(input, bits, narrow);
            math.multiply(numerator, input, delta);
            math.add(numerator, numerator, log_lower);
            scale = exponential(output, numerator);
          } else {
            functions_.logarithm(input, x);
            math.subtract(numerator, input, log_lower);
            math.divide(output, numerator, delta);
          }
          const auto low = round_scaled(output.low, narrow, scale);
          const auto high = round_scaled(output.high, narrow, scale);
          if (low == high)
            return Answer(low);
        } catch (const DirectedInterval::Unresolved&) {
          // Refine the complete expression; work failures are not retries.
        }
      }
      return Answer(Status{ErrorCode::ResourceExhausted,
                           "log shaper rounding precision limit",
                           FailureReason::CapacityLimit});
    } catch (const DirectedFunctions::RangeResult& range) {
      return Answer(range.overflow ? infinity(narrow) : 0);
    } catch (const Status& status) {
      return Answer(status);
    }
  }
};
}  // namespace ps::plugin_internal::numeric_ops
