#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <numeric>
#include <optional>
#include <string>

#include "01-numeric/directed_functions.hpp"
#include "01-numeric/exact_root.hpp"
#include "01-numeric/numeric_nan.hpp"
#include "photospider/data/value.hpp"

namespace ps::plugin_internal::numeric_ops {
enum class CertifiedKind {
  Exp,
  Ln,
  Sin,
  Cos,
  Tan,
  Sinpi,
  Cospi,
  Tanpi,
  Sinc,
  Sincpi,
  Pow,
  Atan2,
  Atan2pi,
  SinpiRational,
  CospiRational,
  TanpiRational,
  SincpiRational
};
struct CertifiedMath final {
  DirectedFunctions functions;
  RatioWorkspace algebraic;
  explicit CertifiedMath(SequenceProfile profile)
      : functions(SequenceProfile::Strict), algebraic(profile) {}
  using Interval = DirectedInterval::Interval;
  using Frame = DirectedInterval::Frame;
  static bool pi_function(CertifiedKind kind) {
    return kind == CertifiedKind::Sinpi || kind == CertifiedKind::Cospi ||
           kind == CertifiedKind::Tanpi || kind == CertifiedKind::Sincpi ||
           kind >= CertifiedKind::SinpiRational;
  }
  static bool rational(CertifiedKind kind) {
    return kind >= CertifiedKind::SinpiRational;
  }
  static bool sine(CertifiedKind kind) {
    return kind == CertifiedKind::Sin || kind == CertifiedKind::Sinpi ||
           kind == CertifiedKind::SinpiRational;
  }
  static bool cosine(CertifiedKind kind) {
    return kind == CertifiedKind::Cos || kind == CertifiedKind::Cospi ||
           kind == CertifiedKind::CospiRational;
  }
  static bool tangent(CertifiedKind kind) {
    return kind == CertifiedKind::Tan || kind == CertifiedKind::Tanpi ||
           kind == CertifiedKind::TanpiRational;
  }
  static bool cardinal(CertifiedKind kind) {
    return kind == CertifiedKind::Sinc || kind == CertifiedKind::Sincpi ||
           kind == CertifiedKind::SincpiRational;
  }
  static bool odd_integer(const BinaryParts& value) {
    if (value.exponent > 0)
      return false;
    if (value.exponent == 0)
      return value.significand & 1;
    const auto shift = static_cast<unsigned>(-value.exponent);
    return shift < 64 && !(value.significand & ((UINT64_C(1) << shift) - 1)) &&
           ((value.significand >> shift) & 1);
  }
  static bool integer(const BinaryParts& value) {
    if (value.exponent >= 0 || !value.magnitude)
      return true;
    const auto shift = static_cast<unsigned>(-value.exponent);
    return shift < 64 && !(value.significand & ((UINT64_C(1) << shift) - 1));
  }
  std::uint64_t root(unsigned numerator, unsigned denominator, bool negative,
                     bool narrow,
                     const std::function<Status(std::uint64_t)>& work) {
    algebraic.numerator.words.fill(0);
    algebraic.numerator.words[0] = numerator;
    algebraic.denominator.words.fill(0);
    algebraic.denominator.words[0] = denominator;
    algebraic.negative = false;
    auto result = round_sqrt_ratio(&algebraic, narrow, 0, work);
    if (!result.ok())
      throw result.status();
    return result.value() |
           (static_cast<std::uint64_t>(negative) << (narrow ? 31 : 63));
  }
  std::optional<std::uint64_t> landmark(
      CertifiedKind kind, std::uint64_t numerator, std::uint64_t denominator,
      bool negative, bool narrow,
      const std::function<Status(std::uint64_t)>& work) {
    const auto one =
        narrow ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000);
    const auto half =
        narrow ? UINT64_C(0x3f000000) : UINT64_C(0x3fe0000000000000);
    const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
    const auto nan =
        narrow ? UINT64_C(0x7fc00000) : UINT64_C(0x7ff8000000000000);
    if (!numerator)
      return cardinal(kind) || cosine(kind) ? one : negative ? sign : 0;
    const auto divisor = std::gcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
    if (denominator == 1) {
      if (cardinal(kind))
        return 0;
      if (cosine(kind))
        return one | ((numerator & 1) ? sign : 0);
      return negative ? sign : 0;
    }
    if (cardinal(kind))
      return {};
    if (denominator != 2 && denominator != 3 && denominator != 4 &&
        denominator != 6)
      return {};
    const auto position = static_cast<unsigned>(numerator % (2 * denominator));
    if (denominator == 2) {
      if (cosine(kind))
        return 0;
      if (tangent(kind))
        return nan;
      return one | (((position == 3) != negative) ? sign : 0);
    }
    if (denominator == 4) {
      if (tangent(kind))
        return one | ((((position % 4) == 3) != negative) ? sign : 0);
      const bool minus = sine(kind) ? ((position >= 4) != negative)
                                    : (position == 3 || position == 5);
      return root(1, 2, minus, narrow, work);
    }
    if (denominator == 3) {
      if (sine(kind))
        return root(3, 4, (position > 3) != negative, narrow, work);
      if (cosine(kind))
        return half | ((position == 2 || position == 4) ? sign : 0);
      return root(3, 1, ((position % 3) == 2) != negative, narrow, work);
    }
    if (sine(kind))
      return half | (((position > 6) != negative) ? sign : 0);
    if (cosine(kind))
      return root(3, 4, position == 5 || position == 7, narrow, work);
    return root(1, 3, ((position % 6) == 5) != negative, narrow, work);
  }
  std::uint64_t pi_angle(unsigned quarters, bool negative, bool narrow) {
    auto& math = functions.math;
    for (unsigned precision = 128; precision <= 4096; precision *= 2) {
      math.precision = precision;
      Frame frame(math);
      auto pi = math.interval(), factor = math.interval();
      math.constant(pi, true);
      math.integer(factor, quarters);
      math.multiply(pi, pi, factor);
      math.scale(pi, pi, -2);
      const auto low = math.rounded(pi.low, narrow),
                 high = math.rounded(pi.high, narrow);
      if (low == high)
        return low |
               (static_cast<std::uint64_t>(negative) << (narrow ? 31 : 63));
    }
    DirectedInterval::capacity();
  }
  // Resolve every dyadic midpoint power without constructing an unbounded
  // integer. A binary32/64 midpoint has at most 25/54 odd significand bits.
  std::optional<std::uint64_t> dyadic_power(
      BinaryParts base, BinaryParts exponent, bool negative, bool narrow,
      const std::function<Status(std::uint64_t)>& work) {
    const auto trailing =
        static_cast<unsigned>(__builtin_ctzll(base.significand));
    std::uint64_t odd = base.significand >> trailing;
    int power = base.exponent + static_cast<int>(trailing);
    const auto exponent_trailing =
        static_cast<unsigned>(__builtin_ctzll(exponent.significand));
    std::uint64_t multiplier = exponent.significand >> exponent_trailing;
    const int shift = exponent.exponent + static_cast<int>(exponent_trailing);
    if (shift < 0) {
      const auto roots = static_cast<unsigned>(-shift);
      if (roots >= 12 && power)
        return {};
      if (roots < 12 && power % (1 << roots))
        return {};
      if (roots < 12)
        power /= (1 << roots);
      if (roots >= 6 && odd != 1)
        return {};
      for (unsigned j = 0; j < roots && odd != 1; ++j) {
        auto status = work(128);
        if (!status.ok())
          throw status;
        std::uint64_t low = 0, high = UINT64_C(1) << 27;
        while (high - low > 1) {
          const auto middle = (low + high) / 2;
          if (static_cast<unsigned __int128>(middle) * middle <= odd)
            low = middle;
          else
            high = middle;
        }
        if (low * low != odd)
          return {};
        odd = low;
      }
    }
    if (exponent.negative && odd != 1)
      return {};
    const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
    const auto infinity =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    if (odd == 1) {
      if (!power)
        return (narrow ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000)) |
               (negative ? sign : 0);
      const bool minus = (power < 0) != exponent.negative;
      const auto magnitude = static_cast<unsigned>(power < 0 ? -power : power);
      // |binary exponent|>4096 is safely out of destination range.
      if (shift >= 13 || multiplier > 4096 / magnitude)
        return (minus ? 0 : infinity) | (negative ? sign : 0);
      const auto total = static_cast<std::uint64_t>(magnitude) * multiplier *
                         (shift > 0 ? (UINT64_C(1) << shift) : 1);
      if (total > 4096)
        return (minus ? 0 : infinity) | (negative ? sign : 0);
      algebraic.numerator.words.fill(0);
      algebraic.numerator.words[0] = 1;
      algebraic.denominator.words.fill(0);
      algebraic.denominator.words[0] = 1;
      algebraic.negative = negative;
      auto result = algebraic.round(
          narrow, work,
          minus ? -static_cast<int>(total) : static_cast<int>(total));
      if (!result.ok())
        throw result.status();
      return result.value();
    }
    if (shift >= 6 || multiplier > 64)
      return {};
    if (shift > 0)
      multiplier <<= shift;
    if (multiplier > 64)
      return {};
    const auto limit = UINT64_C(1) << (narrow ? 25 : 54);
    std::uint64_t core = 1;
    for (std::uint64_t j = 0; j < multiplier; ++j) {
      auto status = work(1);
      if (!status.ok())
        throw status;
      if (core > (limit - 1) / odd)
        return {};
      core *= odd;
    }
    const int scale = power * static_cast<int>(multiplier);
    const int leading = 63 - __builtin_clzll(core) + scale;
    if (leading >= (narrow ? 128 : 1024))
      return infinity | (negative ? sign : 0);
    if (leading < (narrow ? -150 : -1075))
      return negative ? sign : 0;
    algebraic.numerator.words.fill(0);
    algebraic.numerator.words[0] = core;
    algebraic.denominator.words.fill(0);
    algebraic.denominator.words[0] = 1;
    algebraic.negative = negative;
    auto result = algebraic.round(narrow, work, scale);
    if (!result.ok())
      throw result.status();
    return result.value();
  }
  void pi_pair(Interval s, Interval c, const BinaryParts& input,
               std::uint64_t numerator, std::uint64_t denominator,
               bool is_rational) {
    auto& math = functions.math;
    Frame frame(math);
    auto reduced = math.interval(), half = math.interval(),
         angle = math.interval();
    unsigned quadrant = 0;
    if (is_rational) {
      const auto period = static_cast<unsigned __int128>(denominator) * 2;
      const auto remainder = static_cast<unsigned __int128>(numerator) % period;
      quadrant = static_cast<unsigned>(
          (4 * remainder + denominator) /
          (2 * static_cast<unsigned __int128>(denominator)));
      auto top = math.interval(), bottom = math.interval();
      math.unsigned_integer(top, remainder);
      math.unsigned_integer(bottom, denominator);
      math.divide(reduced, top, bottom);
    } else {
      const auto bits = static_cast<unsigned>(-input.exponent);
      const auto remainder =
          bits >= 63 ? input.significand
                     : input.significand & ((UINT64_C(1) << (bits + 1)) - 1);
      if (bits < 64)
        quadrant = static_cast<unsigned>(
            (static_cast<unsigned __int128>(remainder) * 4 +
             (UINT64_C(1) << bits)) >>
            (bits + 1));
      math.dyadic(reduced, remainder, input.exponent);
    }
    math.integer(half, quadrant);
    math.scale(half, half, -1);
    math.subtract(reduced, reduced, half);
    math.constant(angle, true);
    math.multiply(angle, angle, reduced);
    auto sine_result = math.interval(), cosine_result = math.interval();
    functions.sincos_series(sine_result, cosine_result, angle);
    functions.orient(s, c, sine_result, cosine_result, quadrant & 3,
                     input.negative);
  }
  Result<std::uint64_t> evaluate(
      CertifiedKind kind, ElementType dtype, std::uint64_t a, std::uint64_t b,
      const std::function<Status(std::uint64_t)>& work,
      const std::function<Status()>& strict_fallback) {
    using Answer = Result<std::uint64_t>;
    auto& math = functions.math;
    struct Binding {
      DirectedInterval& math;
      ~Binding() {
        math.consume = nullptr;
        math.used = 0;
      }
    } binding{math};
    math.consume = &work;
    try {
      auto admitted = work(DirectedInterval::kSlots * DirectedInterval::kWords);
      if (!admitted.ok())
        return Answer(admitted);
      const bool narrow = dtype == ElementType::Float32;
      const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
      const auto infinity =
          narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
      const auto one =
          narrow ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000);
      const auto nan = infinity | (UINT64_C(1) << (narrow ? 22 : 51));
      auto x = BinaryParts::decode(a, narrow),
           y = BinaryParts::decode(b, narrow);
      const bool is_rational = rational(kind);
      bool negative_result = false;
      std::uint64_t numerator = 0, denominator = 1;
      if (is_rational) {
        if (!b || (b >> 63))
          return Answer(Status{
              ErrorCode::InvalidArgument,
              "InvalidRationalDenominator: port=1 bits=" + std::to_string(b),
              FailureReason::InvalidDomain,
              {FailureOrigin::Domain, FailureScope::Atom}});
        x.negative = (a >> 63) != 0;
        numerator = x.negative ? UINT64_C(0) - a : a;
        denominator = b;
        auto exact =
            landmark(kind, numerator, denominator, x.negative, narrow, work);
        if (exact)
          return Answer(*exact);
      } else {
        if (kind == CertifiedKind::Pow && (!y.magnitude || a == one))
          return Answer(one);
        const bool binary = kind == CertifiedKind::Pow ||
                            kind == CertifiedKind::Atan2 ||
                            kind == CertifiedKind::Atan2pi;
        if (x.nan || (binary && y.nan))
          return Answer(converted_nan(x.nan ? a : b, dtype, dtype));
        if (kind == CertifiedKind::Pow) {
          if (y.infinite) {
            if (x.magnitude == one)
              return Answer(one);
            return Answer((x.magnitude > one) != y.negative ? infinity : 0);
          }
          negative_result = x.negative && odd_integer(y);
          if (!x.magnitude)
            return Answer((y.negative ? infinity : 0) |
                          (negative_result ? sign : 0));
          if (x.infinite)
            return Answer((y.negative ? 0 : infinity) |
                          (negative_result ? sign : 0));
          if (x.negative && !integer(y))
            return Answer(nan);
          auto exact = dyadic_power(x, y, negative_result, narrow, work);
          if (exact)
            return Answer(*exact);
        } else if (kind == CertifiedKind::Atan2 ||
                   kind == CertifiedKind::Atan2pi) {
          std::optional<unsigned> quarters;
          if (!x.magnitude)
            quarters = y.negative ? 4 : 0;
          else if (x.infinite)
            quarters = y.infinite ? (y.negative ? 3 : 1) : 2;
          else if (!y.magnitude)
            quarters = 2;
          else if (y.infinite)
            quarters = y.negative ? 4 : 0;
          if (quarters) {
            if (!*quarters)
              return Answer(a & sign);
            if (kind == CertifiedKind::Atan2)
              return Answer(pi_angle(*quarters, x.negative, narrow));
            const auto value =
                *quarters == 4   ? one
                : *quarters == 2 ? (narrow ? UINT64_C(0x3f000000)
                                           : UINT64_C(0x3fe0000000000000))
                : *quarters == 1 ? (narrow ? UINT64_C(0x3e800000)
                                           : UINT64_C(0x3fd0000000000000))
                                 : (narrow ? UINT64_C(0x3f400000)
                                           : UINT64_C(0x3fe8000000000000));
            return Answer(value | (a & sign));
          }
        } else if (kind == CertifiedKind::Exp) {
          if (!x.magnitude)
            return Answer(one);
          if (x.infinite)
            return Answer(x.negative ? 0 : infinity);
        } else if (kind == CertifiedKind::Ln) {
          if (!x.magnitude)
            return Answer(infinity | sign);
          if (x.negative)
            return Answer(nan);
          if (x.infinite)
            return Answer(infinity);
          if (a == one)
            return Answer(UINT64_C(0));
        } else {
          if (x.infinite)
            return Answer(cardinal(kind) ? 0 : nan);
          if (!x.magnitude)
            return Answer(cardinal(kind) || cosine(kind) ? one : a);
          if (pi_function(kind)) {
            if (x.exponent >= 0) {
              if (cardinal(kind))
                return Answer(UINT64_C(0));
              if (cosine(kind))
                return Answer(
                    one |
                    ((x.exponent == 0 && (x.significand & 1)) ? sign : 0));
              return Answer(a & sign);
            }
            const auto shift = static_cast<unsigned>(-x.exponent);
            if (shift < 64) {
              auto exact = landmark(kind, x.significand, UINT64_C(1) << shift,
                                    x.negative, narrow, work);
              if (exact)
                return Answer(*exact);
            }
          }
        }
      }
      auto fallback = strict_fallback();
      if (!fallback.ok())
        return Answer(fallback);
      for (unsigned precision = 128; precision <= 4096; precision *= 2) {
        math.precision = precision;
        Frame frame(math);
        try {
          auto output = math.interval(), input = math.interval();
          if (!is_rational)
            math.raw(input, a, narrow);
          if (kind == CertifiedKind::Exp) {
            functions.exponential(output, input);
          } else if (kind == CertifiedKind::Ln) {
            functions.logarithm(output, x);
          } else if (kind == CertifiedKind::Pow) {
            auto logarithm = math.interval(), exponent = math.interval();
            functions.logarithm(logarithm, x);
            math.raw(exponent, b, narrow);
            math.multiply(logarithm, logarithm, exponent);
            functions.exponential(output, logarithm);
            if (negative_result)
              math.negate(output, output);
          } else if (kind == CertifiedKind::Atan2 ||
                     kind == CertifiedKind::Atan2pi) {
            const bool swap = x.magnitude > y.magnitude;
            const auto& large = swap ? x : y;
            const auto& small = swap ? y : x;
            const auto scale =
                large.exponent + 63 - __builtin_clzll(large.significand);
            auto lower = math.interval(), higher = math.interval(),
                 ratio = math.interval(), pi = math.interval();
            math.dyadic(lower, small.significand, small.exponent - scale);
            math.dyadic(higher, large.significand, large.exponent - scale);
            math.divide(ratio, lower, higher);
            functions.arctangent(output, ratio);
            math.constant(pi, true);
            if (swap) {
              auto half = math.interval();
              math.scale(half, pi, -1);
              math.subtract(output, half, output);
            }
            if (y.negative)
              math.subtract(output, pi, output);
            if (kind == CertifiedKind::Atan2pi)
              math.divide(output, output, pi);
            if (x.negative)
              math.negate(output, output);
          } else {
            auto s = math.interval(), c = math.interval(),
                 absolute = math.interval();
            if (is_rational) {
              auto top = math.interval(), bottom = math.interval();
              math.unsigned_integer(top, numerator);
              math.unsigned_integer(bottom, denominator);
              math.divide(absolute, top, bottom);
            } else {
              math.dyadic(absolute, x.significand, x.exponent);
            }
            auto threshold = math.interval();
            math.integer(threshold, 1);
            if (kind != CertifiedKind::Sinc)
              math.scale(threshold, threshold, -2);
            if (cardinal(kind) &&
                math.compare(absolute.high, threshold.low) <= 0) {
              if (kind != CertifiedKind::Sinc) {
                auto pi = math.interval();
                math.constant(pi, true);
                math.multiply(absolute, absolute, pi);
              }
              functions.sinc_series(output, absolute);
            } else {
              if (pi_function(kind))
                pi_pair(s, c, x, numerator, denominator, is_rational);
              else
                functions.sincos(s, c, x);
              if (sine(kind)) {
                math.copy(output, s);
              } else if (cosine(kind)) {
                math.copy(output, c);
              } else if (tangent(kind)) {
                math.divide(output, s, c);
              } else {
                if (x.negative)
                  math.negate(s, s);
                if (kind != CertifiedKind::Sinc) {
                  auto pi = math.interval();
                  math.constant(pi, true);
                  math.multiply(absolute, absolute, pi);
                }
                math.divide(output, s, absolute);
              }
            }
          }
          const auto low = math.rounded(output.low, narrow),
                     high = math.rounded(output.high, narrow);
          if (low == high)
            return Answer(low);
          // Every exact zero/landmark is handled above. When a nonzero real
          // result is enclosed on one side of zero and both magnitudes round
          // to zero, its mathematical sign follows that one-sided enclosure.
          if (!(low & ~sign) && !(high & ~sign)) {
            if (math.top(output.high) < 0 && output.low.negative)
              return Answer(sign);
            if (math.top(output.low) < 0 && !output.high.negative)
              return Answer(UINT64_C(0));
          }
        } catch (const DirectedInterval::Unresolved&) {
        } catch (const DirectedFunctions::RangeResult& result) {
          return Answer((result.overflow ? infinity : 0) |
                        (negative_result ? sign : 0));
        }
      }
      return Answer(
          Status{ErrorCode::ResourceExhausted,
                 "certified math rounding unresolved at 4096-bit precision",
                 FailureReason::CapacityLimit});
    } catch (const Status& status) {
      return Answer(status);
    }
  }
};
}  // namespace ps::plugin_internal::numeric_ops
