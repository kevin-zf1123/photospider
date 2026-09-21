#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>

#include "01-numeric/accelerated_math.hpp"
#include "01-numeric/exact_root.hpp"
#include "01-numeric/numeric_nan.hpp"
#include "photospider/data/value.hpp"

namespace ps::plugin_internal::numeric_ops {
enum class ElementaryKind {
  Abs,
  Neg,
  Sqrt,
  Floor,
  Ceil,
  Round,
  Sign,
  Reciprocal,
  Add,
  Subtract,
  Multiply,
  Divide,
  Minimum,
  Maximum
};
struct ExactElementary final {
  RatioWorkspace ratio;
  explicit ExactElementary(SequenceProfile profile) : ratio(profile) {}
  static Result<std::uint64_t> overflow() {
    return Result<std::uint64_t>(
        Status{ErrorCode::OperationFailed,
               "integer arithmetic overflow",
               FailureReason::ArithmeticOverflow,
               {FailureOrigin::Domain, FailureScope::Atom}});
  }
  Result<std::uint64_t> evaluate(
      ElementaryKind kind, ElementType dtype, std::uint64_t a, std::uint64_t b,
      const std::function<Status(std::uint64_t)>& consume,
      const input_internal::Float32Environment* borrowed_environment =
          nullptr) {
    using Answer = Result<std::uint64_t>;
    auto charged = consume(1024);
    if (!charged.ok())
      return Answer(charged);
    const bool binary = kind >= ElementaryKind::Add;
    const bool narrow = dtype == ElementType::Float32;
    if (dtype == ElementType::UInt8 || dtype == ElementType::Int64) {
      std::int64_t signed_a = 0, signed_b = 0;
      std::memcpy(&signed_a, &a, 8);
      std::memcpy(&signed_b, &b, 8);
      const __int128 x =
          dtype == ElementType::UInt8 ? static_cast<__int128>(a) : signed_a;
      const __int128 y =
          dtype == ElementType::UInt8 ? static_cast<__int128>(b) : signed_b;
      __int128 result = x;
      switch (kind) {
        case ElementaryKind::Abs:
          result = x < 0 ? -x : x;
          break;
        case ElementaryKind::Neg:
          result = -x;
          break;
        case ElementaryKind::Sign:
          result = x < 0 ? -1 : x > 0 ? 1 : 0;
          break;
        case ElementaryKind::Add:
          result = x + y;
          break;
        case ElementaryKind::Subtract:
          result = x - y;
          break;
        case ElementaryKind::Multiply:
          result = x * y;
          break;
        case ElementaryKind::Minimum:
          result = x < y ? x : y;
          break;
        case ElementaryKind::Maximum:
          result = x > y ? x : y;
          break;
        default:
          break;
      }
      const __int128 low =
          dtype == ElementType::UInt8 ? 0 : -(static_cast<__int128>(1) << 63);
      const __int128 high = dtype == ElementType::UInt8
                                ? 255
                                : (static_cast<__int128>(1) << 63) - 1;
      if (result < low || result > high)
        return overflow();
      return Answer(static_cast<std::uint64_t>(result));
    }
    const auto x = BinaryParts::decode(a, narrow),
               y = BinaryParts::decode(b, narrow);
    const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
    const auto quiet = UINT64_C(1) << (narrow ? 22 : 51);
    const auto infinity =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    const auto one =
        narrow ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000);
    if (x.nan || (binary && y.nan)) {
      auto value = converted_nan(x.nan ? a : b, dtype, dtype);
      if (kind == ElementaryKind::Abs)
        value &= ~sign;
      if (kind == ElementaryKind::Neg)
        value ^= sign;
      return Answer(value);
    }
    if (kind == ElementaryKind::Abs)
      return Answer(a & ~sign);
    if (kind == ElementaryKind::Neg)
      return Answer(a ^ sign);
    if (kind == ElementaryKind::Sign)
      return Answer(x.magnitude ? (one | (a & sign)) : a);
    if (kind == ElementaryKind::Floor || kind == ElementaryKind::Ceil ||
        kind == ElementaryKind::Round) {
      if (x.infinite || !x.magnitude || x.exponent >= 0)
        return Answer(a);
      if (x.magnitude < one) {
        const auto half =
            narrow ? UINT64_C(0x3f000000) : UINT64_C(0x3fe0000000000000);
        const bool increment = kind == ElementaryKind::Round
                                   ? x.magnitude > half
                               : kind == ElementaryKind::Floor ? x.negative
                                                               : !x.negative;
        return Answer((a & sign) | (increment ? one : 0));
      }
      const auto shift = static_cast<unsigned>(-x.exponent);
      const auto mask = (UINT64_C(1) << shift) - 1;
      const auto remainder = x.significand & mask;
      const auto halfway = UINT64_C(1) << (shift - 1);
      const bool increment =
          kind == ElementaryKind::Round
              ? remainder > halfway ||
                    (remainder == halfway && ((x.significand >> shift) & 1))
              : remainder &&
                    (kind == ElementaryKind::Floor ? x.negative : !x.negative);
      return Answer((a & ~mask) + (increment ? (UINT64_C(1) << shift) : 0));
    }
    if (kind == ElementaryKind::Minimum || kind == ElementaryKind::Maximum) {
      if (!x.magnitude && !y.magnitude)
        return Answer(kind == ElementaryKind::Minimum ? a | b : a & b);
      const auto x_key = x.order_key(), y_key = y.order_key();
      return Answer(
          (kind == ElementaryKind::Minimum ? x_key < y_key : x_key > y_key)
              ? a
              : b);
    }
    if (kind == ElementaryKind::Sqrt) {
      if (!x.magnitude)
        return Answer(a);
      if (x.negative)
        return Answer(infinity | quiet);
      if (x.infinite)
        return Answer(a);
    }
    if (kind == ElementaryKind::Reciprocal) {
      if (!x.magnitude)
        return Answer(infinity | (a & sign));
      if (x.infinite)
        return Answer(a & sign);
    }
    if (kind == ElementaryKind::Add || kind == ElementaryKind::Subtract) {
      const bool y_negative = y.negative != (kind == ElementaryKind::Subtract);
      if (x.infinite && y.infinite && x.negative != y_negative)
        return Answer(infinity | quiet);
      if (x.infinite)
        return Answer(a);
      if (y.infinite)
        return Answer(infinity | (y_negative ? sign : 0));
      if (!x.magnitude && !y.magnitude)
        return Answer(x.negative && y_negative ? sign : 0);
    }
    const bool negative = x.negative != y.negative;
    if (kind == ElementaryKind::Multiply) {
      if ((!x.magnitude && y.infinite) || (!y.magnitude && x.infinite))
        return Answer(infinity | quiet);
      if (x.infinite || y.infinite)
        return Answer(infinity | (negative ? sign : 0));
      if (!x.magnitude || !y.magnitude)
        return Answer(negative ? sign : 0);
    }
    if (kind == ElementaryKind::Divide) {
      if ((!x.magnitude && !y.magnitude) || (x.infinite && y.infinite))
        return Answer(infinity | quiet);
      if (x.infinite || !y.magnitude)
        return Answer(infinity | (negative ? sign : 0));
      if (y.infinite || !x.magnitude)
        return Answer(negative ? sign : 0);
    }
    // Single IEEE operations are correctly rounded by the hardware. Keep the
    // bit-level special table above; the owning guard restores flags/controls.
    // A Whole callback can lend its live RN-even/gradual-underflow guard.
    // A borrowed guard must live on this thread throughout the call; callers
    // must not alter fenv until its destruction restores the outer flags.
    // Standalone callers retain a local guard and restore their own flags.
    std::optional<input_internal::Float32Environment> environment;
    if (!borrowed_environment)
      environment.emplace();
    if (borrowed_environment ? borrowed_environment->active()
                             : environment->active()) {
      const double left = numeric_double(a, narrow),
                   right = numeric_double(b, narrow);
      if (narrow) {
        const float u = static_cast<float>(left), v = static_cast<float>(right);
        float result = 0;
        switch (kind) {
          case ElementaryKind::Sqrt:
            result = std::sqrt(u);
            break;
          case ElementaryKind::Reciprocal:
            result = 1.0f / u;
            break;
          case ElementaryKind::Add:
            result = u + v;
            break;
          case ElementaryKind::Subtract:
            result = u - v;
            break;
          case ElementaryKind::Multiply:
            result = u * v;
            break;
          case ElementaryKind::Divide:
            result = u / v;
            break;
          default:
            break;
        }
        return Answer(numeric_bits(result, true));
      }
      double result = 0;
      switch (kind) {
        case ElementaryKind::Sqrt:
          result = std::sqrt(left);
          break;
        case ElementaryKind::Reciprocal:
          result = 1.0 / left;
          break;
        case ElementaryKind::Add:
          result = left + right;
          break;
        case ElementaryKind::Subtract:
          result = left - right;
          break;
        case ElementaryKind::Multiply:
          result = left * right;
          break;
        case ElementaryKind::Divide:
          result = left / right;
          break;
        default:
          break;
      }
      return Answer(numeric_bits(result));
    }
    ratio.numerator.words.fill(0);
    ratio.denominator.words.fill(0);
    ratio.denominator.words[0] = 1;
    ratio.negative = false;
    if (kind == ElementaryKind::Sqrt) {
      ratio.numerator.set(x, 1074);
      return round_sqrt_ratio(&ratio, narrow, -1074, consume);
    }
    if (kind == ElementaryKind::Reciprocal) {
      ratio.numerator.words[1074 / 64] = UINT64_C(1) << (1074 % 64);
      ratio.denominator.set(x, 1074);
      ratio.negative = x.negative;
      return ratio.round(narrow, consume, 0);
    }
    if (kind == ElementaryKind::Multiply) {
      ratio.numerator.set_product(x, y);
      ratio.negative = negative;
      return ratio.round(narrow, consume, -2148);
    }
    if (kind == ElementaryKind::Divide) {
      ratio.numerator.set(x, 1074);
      ratio.denominator.set(y, 1074);
      ratio.negative = negative;
      return ratio.round(narrow, consume, 0);
    }
    ratio.term.set(x, 1074);
    ratio.add_term(x.negative);
    ratio.term.set(y, 1074);
    ratio.add_term(y.negative != (kind == ElementaryKind::Subtract));
    return ratio.round(narrow, consume, -1074);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
