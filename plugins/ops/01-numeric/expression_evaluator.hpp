#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "01-numeric/accelerated_expression.hpp"
#include "01-numeric/certified_math.hpp"
#include "01-numeric/exact_elementary.hpp"
#include "01-numeric/expression_program.hpp"
#include "photospider/ops/numeric/diagnostics.hpp"

namespace ps::plugin_internal::numeric_ops {
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
      const std::array<std::uint64_t, 256>& coefficients,
      const std::function<Status(std::uint64_t)>& consume) {
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
      auto result =
          function == NumericMathFunction::Other ||
                  function == NumericMathFunction::Sqrt
              ? elementary.evaluate(basic, ElementType::Float64, a, b, consume)
              : mathematics.evaluate(math, ElementType::Float64, a, b, consume,
                                     [] { return Status::success(); });
      if (!result.ok())
        return result;
      values[i] = result.value();
    }
    return Answer(values[program.size - 1]);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
