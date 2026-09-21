#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

#include "01-numeric/accelerated_expression.hpp"
#include "01-numeric/certified_math.hpp"
#include "01-numeric/exact_elementary.hpp"
#include "01-numeric/expression_program.hpp"
#include "photospider/core/numeric_diagnostics.hpp"

namespace ps::plugin_internal::numeric_ops {
inline std::string expression_coordinate(std::uint64_t bits) {
  double value = 0;
  std::memcpy(&value, &bits, 8);
  std::array<char, 64> text{};
  const auto written = std::to_chars(text.data(), text.data() + text.size(),
                                     value, std::chars_format::general);
  return written.ec == std::errc{} ? std::string(text.data(), written.ptr)
                                   : "unavailable";
}
inline Status expression_failure(std::uint64_t index, bool have_x,
                                 std::uint64_t x, FailureReason reason,
                                 const std::string& message) {
  AtomKey atom;
  atom.rank = 1;
  atom.coordinate[0] = index;
  return Status{ErrorCode::OperationFailed,
                "sample=" + std::to_string(index) + " x=" +
                    (have_x ? expression_coordinate(x) : "unavailable") + " " +
                    message,
                reason,
                {FailureOrigin::Domain, FailureScope::Atom, atom}};
}
struct ExpressionEvaluator final {
  AcceleratedExpression accelerated;
  ExactElementary elementary;
  CertifiedMath mathematics;
  SequenceProfile profile;
  std::array<std::uint64_t, 256> values{};
  explicit ExpressionEvaluator(SequenceProfile selected)
      : elementary(selected),
        mathematics(SequenceProfile::Strict),
        profile(selected) {}
  Result<std::uint64_t> evaluate(
      const ExpressionProgram& program, std::uint64_t x,
      const std::array<std::uint64_t, 256>& coefficients, std::uint64_t sample,
      const std::function<Status(std::uint64_t)>& consume,
      const std::function<Status(NumericMathFunction, bool)>& report) {
    using Answer = Result<std::uint64_t>;
    using K = ExpressionKind;
    for (unsigned i = 0; i < program.size; ++i) {
      auto charged = consume(1);
      if (!charged.ok())
        return Answer(charged);
      const auto& node = program.nodes[i];
      const bool leaf = node.kind == K::Literal || node.kind == K::X ||
                        node.kind == K::Coefficient;
      const auto a = leaf ? 0 : values[node.left],
                 b = leaf ? 0 : values[node.right];
      const auto left = BinaryParts::decode(a, false),
                 right = BinaryParts::decode(b, false);
      const auto fail = [&](FailureReason reason, const char* message) {
        return Answer(expression_failure(sample, true, x, reason,
                                         "span=[" + std::to_string(node.begin) +
                                             "," + std::to_string(node.end) +
                                             ") " + message));
      };
      if (node.kind == K::Literal) {
        values[i] = node.bits;
        continue;
      }
      if (node.kind == K::X) {
        values[i] = x;
        continue;
      }
      if (node.kind == K::Coefficient) {
        values[i] = coefficients[node.left];
        continue;
      }
      if (node.kind == K::Positive) {
        values[i] = a;
        continue;
      }
      if (node.kind == K::Negative) {
        values[i] = a ^ (UINT64_C(1) << 63);
        continue;
      }
      if (node.kind == K::Divide && !right.magnitude)
        return fail(FailureReason::DivideByZero, "division by zero");
      if (node.kind == K::Sqrt && left.negative && left.magnitude)
        return fail(FailureReason::InvalidDomain, "sqrt negative operand");
      if (node.kind == K::Ln && (!left.magnitude || left.negative))
        return fail(FailureReason::InvalidDomain, "ln nonpositive operand");
      if (node.kind == K::Power) {
        if (!left.magnitude && right.negative && right.magnitude)
          return fail(FailureReason::DivideByZero, "zero to negative power");
        const bool integer =
            right.exponent >= 0 || !right.magnitude ||
            (-right.exponent < 64 &&
             (right.significand & ((UINT64_C(1) << -right.exponent) - 1)) == 0);
        if (left.negative && left.magnitude && !integer)
          return fail(FailureReason::InvalidDomain,
                      "negative base with noninteger exponent");
      }
      ElementaryKind basic = ElementaryKind::Abs;
      CertifiedKind math = CertifiedKind::Exp;
      NumericMathFunction function = NumericMathFunction::Other;
      switch (node.kind) {
        case K::Add:
          basic = ElementaryKind::Add;
          break;
        case K::Subtract:
          basic = ElementaryKind::Subtract;
          break;
        case K::Multiply:
          basic = ElementaryKind::Multiply;
          break;
        case K::Divide:
          basic = ElementaryKind::Divide;
          break;
        case K::Abs:
          basic = ElementaryKind::Abs;
          break;
        case K::Min:
          basic = ElementaryKind::Minimum;
          break;
        case K::Max:
          basic = ElementaryKind::Maximum;
          break;
        case K::Sqrt:
          basic = ElementaryKind::Sqrt;
          function = NumericMathFunction::Sqrt;
          break;
        case K::Power:
          math = CertifiedKind::Pow;
          function = NumericMathFunction::Pow;
          break;
        case K::Exp:
          math = CertifiedKind::Exp;
          function = NumericMathFunction::Exp;
          break;
        case K::Ln:
          math = CertifiedKind::Ln;
          function = NumericMathFunction::Ln;
          break;
        case K::Sin:
          math = CertifiedKind::Sin;
          function = NumericMathFunction::Sin;
          break;
        case K::Cos:
          math = CertifiedKind::Cos;
          function = NumericMathFunction::Cos;
          break;
        case K::Tan:
          math = CertifiedKind::Tan;
          function = NumericMathFunction::Tan;
          break;
        default:
          break;
      }
      if (function != NumericMathFunction::Other) {
        charged = report(function, false);
        if (!charged.ok())
          return Answer(charged);
      }
      auto result =
          function == NumericMathFunction::Other ||
                  function == NumericMathFunction::Sqrt
              ? elementary.evaluate(basic, ElementType::Float64, a, b, consume)
              : mathematics.evaluate(math, ElementType::Float64, a, b, consume,
                                     [&] {
                                       return profile == SequenceProfile::Strict
                                                  ? Status::success()
                                                  : report(function, true);
                                     });
      if (!result.ok())
        return result;
      const auto value = BinaryParts::decode(result.value(), false);
      if (value.nan || value.infinite)
        return fail(FailureReason::ArithmeticOverflow,
                    "nonfinite intermediate");
      values[i] = result.value();
    }
    return Answer(values[program.size - 1]);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
