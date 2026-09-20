#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "01-numeric/exact_decimal.hpp"

namespace ps::plugin_internal::numeric_ops {
enum class ExpressionKind : std::uint8_t {
  Literal,
  X,
  Coefficient,
  Positive,
  Negative,
  Add,
  Subtract,
  Multiply,
  Divide,
  Power,
  Abs,
  Sqrt,
  Exp,
  Ln,
  Sin,
  Cos,
  Tan,
  Min,
  Max
};
struct ExpressionNode final {
  ExpressionKind kind = ExpressionKind::Literal;
  std::uint16_t left = 0, right = 0, height = 1, begin = 0, end = 0;
  std::uint64_t bits = 0;
};
struct ExpressionProgram final {
  std::array<ExpressionNode, 256> nodes{};
  std::uint16_t size = 0;
  std::vector<std::string> names;
  bool uses_x = false;
};
class ExpressionParser final {
 public:
  explicit ExpressionParser(const std::string& source) : source_(source) {}
  Result<ExpressionProgram> run() {
    using K = ExpressionKind;
    if (source_.empty() || source_.size() > 4096)
      return fail("source limit");
    bool operand = true;
    while (position_ < source_.size()) {
      spaces();
      if (position_ == source_.size())
        break;
      const auto begin = position_;
      const char c = source_[position_];
      if (digit(c) || c == '.') {
        if (!operand || !literal())
          return fail("literal/operator");
        operand = false;
      } else if (alpha(c)) {
        if (!operand)
          return fail("missing operator");
        ++position_;
        while (position_ < source_.size() &&
               (alpha(source_[position_]) || digit(source_[position_])))
          ++position_;
        if (position_ - begin > 63)
          return fail("identifier limit");
        auto name = source_.substr(begin, position_ - begin);
        ExpressionNode node;
        node.begin = static_cast<std::uint16_t>(begin);
        node.end = static_cast<std::uint16_t>(position_);
        if (name == "x") {
          node.kind = K::X;
          program_.uses_x = true;
        } else if (name == "pi" || name == "e") {
          node.bits = name == "pi" ? UINT64_C(0x400921fb54442d18)
                                   : UINT64_C(0x4005bf0a8b145769);
        } else {
          K function;
          if (function_kind(name, &function)) {
            spaces();
            if (!take('('))
              return fail("function parentheses");
            operators_[operators_size_++] = {
                function,     true, true,
                values_size_, 0,    static_cast<std::uint16_t>(begin)};
            continue;
          }
          if (reserved(name))
            return fail("reserved identifier");
          auto found =
              std::find(program_.names.begin(), program_.names.end(), name);
          if (found == program_.names.end()) {
            program_.names.push_back(std::move(name));
            found = program_.names.end() - 1;
          }
          node.kind = K::Coefficient;
          node.left =
              static_cast<std::uint16_t>(found - program_.names.begin());
        }
        if (!append(node))
          return fail("AST limit");
        operand = false;
      } else if (c == '(') {
        if (!operand)
          return fail("missing operator before parenthesis");
        ++position_;
        operators_[operators_size_++] = {
            K::Add,       true, false,
            values_size_, 0,    static_cast<std::uint16_t>(begin)};
      } else if (c == ')' || c == ',') {
        if (operand)
          return fail("empty argument");
        while (operators_size_ && !operators_[operators_size_ - 1].parenthesis)
          if (!reduce())
            return fail("AST limit");
        if (!operators_size_)
          return fail("unmatched delimiter");
        auto& frame = operators_[operators_size_ - 1];
        if (values_size_ != frame.base + frame.commas + 1)
          return fail("malformed argument");
        ++position_;
        if (c == ',') {
          if (!frame.function || ++frame.commas >= arity(frame.kind))
            return fail("function arity");
          operand = true;
        } else {
          const auto closed = frame;
          --operators_size_;
          if (closed.function &&
              (closed.commas + 1 != arity(closed.kind) || !apply(closed)))
            return fail("function arity/AST limit");
          auto& node = program_.nodes[values_[values_size_ - 1]];
          node.begin = closed.begin;
          node.end = static_cast<std::uint16_t>(position_);
          operand = false;
        }
      } else {
        K kind;
        if (c == '+')
          kind = operand ? K::Positive : K::Add;
        else if (c == '-')
          kind = operand ? K::Negative : K::Subtract;
        else if (c == '*')
          kind = K::Multiply;
        else if (c == '/')
          kind = K::Divide;
        else if (c == '^')
          kind = K::Power;
        else
          return fail("unsupported token");
        if (operand && kind != K::Positive && kind != K::Negative)
          return fail("missing operand");
        ++position_;
        if (!operand)
          while (operators_size_ &&
                 !operators_[operators_size_ - 1].parenthesis &&
                 (precedence(operators_[operators_size_ - 1].kind) >
                      precedence(kind) ||
                  (precedence(operators_[operators_size_ - 1].kind) ==
                       precedence(kind) &&
                   kind != K::Power)))
            if (!reduce())
              return fail("AST limit");
        operators_[operators_size_++] = {
            kind, false, false, 0, 0, static_cast<std::uint16_t>(begin)};
        operand = true;
      }
    }
    if (operand)
      return fail("incomplete expression");
    while (operators_size_) {
      if (operators_[operators_size_ - 1].parenthesis)
        return fail("unclosed parenthesis");
      if (!reduce())
        return fail("AST limit");
    }
    if (values_size_ != 1)
      return fail("result count");
    auto sorted = program_.names;
    std::sort(sorted.begin(), sorted.end());
    for (unsigned i = 0; i < program_.size; ++i) {
      auto& node = program_.nodes[i];
      if (node.kind == K::Coefficient)
        node.left = static_cast<std::uint16_t>(
            std::lower_bound(sorted.begin(), sorted.end(),
                             program_.names[node.left]) -
            sorted.begin());
    }
    program_.names = std::move(sorted);
    return Result<ExpressionProgram>(std::move(program_));
  }

 private:
  struct Operator {
    ExpressionKind kind = ExpressionKind::Add;
    bool parenthesis = false, function = false;
    std::uint16_t base = 0, commas = 0, begin = 0;
  };
  static bool digit(char c) { return c >= '0' && c <= '9'; }
  static bool alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  }
  static unsigned arity(ExpressionKind kind) {
    using K = ExpressionKind;
    return kind == K::Add || kind == K::Subtract || kind == K::Multiply ||
                   kind == K::Divide || kind == K::Power || kind == K::Min ||
                   kind == K::Max
               ? 2
               : 1;
  }
  static int precedence(ExpressionKind kind) {
    using K = ExpressionKind;
    return kind == K::Power                             ? 4
           : kind == K::Positive || kind == K::Negative ? 3
           : kind == K::Multiply || kind == K::Divide   ? 2
                                                        : 1;
  }
  static bool function_kind(const std::string& name, ExpressionKind* kind) {
    using K = ExpressionKind;
    for (const auto& function :
         {std::make_pair("abs", K::Abs), std::make_pair("sqrt", K::Sqrt),
          std::make_pair("exp", K::Exp), std::make_pair("ln", K::Ln),
          std::make_pair("sin", K::Sin), std::make_pair("cos", K::Cos),
          std::make_pair("tan", K::Tan), std::make_pair("min", K::Min),
          std::make_pair("max", K::Max)})
      if (name == function.first) {
        *kind = function.second;
        return true;
      }
    return false;
  }
  static bool reserved(const std::string& name) {
    for (const auto* word : {"start", "end", "count", "dtype", "values", "axis",
                             "coefficient_names", "nan", "inf", "NaN", "Inf"})
      if (name == word)
        return true;
    return false;
  }
  Result<ExpressionProgram> fail(const char* message) const {
    return Result<ExpressionProgram>(
        Status{ErrorCode::InvalidArgument,
               std::string("expression ") + message + " at byte " +
                   std::to_string(position_),
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  }
  void spaces() {
    while (position_ < source_.size() &&
           (source_[position_] == ' ' || source_[position_] == '\t' ||
            source_[position_] == '\n' || source_[position_] == '\r'))
      ++position_;
  }
  bool take(char c) {
    if (position_ == source_.size() || source_[position_] != c)
      return false;
    ++position_;
    return true;
  }
  bool append(ExpressionNode node) {
    if (program_.size == 256 || node.height > 32)
      return false;
    values_[values_size_++] = program_.size;
    program_.nodes[program_.size++] = node;
    return true;
  }
  bool apply(const Operator& op) {
    const auto count = arity(op.kind);
    if (values_size_ < count)
      return false;
    ExpressionNode node;
    node.kind = op.kind;
    node.left = values_[values_size_ - count];
    node.right = values_[values_size_ - 1];
    node.height = 1 + std::max(program_.nodes[node.left].height,
                               program_.nodes[node.right].height);
    node.begin = count == 2 ? program_.nodes[node.left].begin : op.begin;
    node.end = program_.nodes[node.right].end;
    values_size_ -= count;
    return append(node);
  }
  bool reduce() {
    const auto op = operators_[--operators_size_];
    return apply(op);
  }
  bool literal() {
    const auto begin = position_;
    unsigned digits = 0;
    while (position_ < source_.size() && digit(source_[position_])) {
      ++digits;
      ++position_;
    }
    if (take('.')) {
      while (position_ < source_.size() && digit(source_[position_])) {
        ++digits;
        ++position_;
      }
    }
    if (!digits)
      return false;
    if (position_ < source_.size() &&
        (source_[position_] == 'e' || source_[position_] == 'E')) {
      ++position_;
      if (position_ < source_.size() &&
          (source_[position_] == '+' || source_[position_] == '-'))
        ++position_;
      const auto exponent = position_;
      while (position_ < source_.size() && digit(source_[position_]))
        ++position_;
      if (exponent == position_)
        return false;
    }
    auto bits = decimal_binary64(
        std::string_view(source_).substr(begin, position_ - begin));
    if (!bits.ok())
      return false;
    ExpressionNode node;
    node.bits = bits.value();
    node.begin = static_cast<std::uint16_t>(begin);
    node.end = static_cast<std::uint16_t>(position_);
    return append(node);
  }
  const std::string& source_;
  std::size_t position_ = 0;
  ExpressionProgram program_;
  // One character consumes at least one source byte. No recursive parser stack.
  std::array<Operator, 4096> operators_{};
  std::array<std::uint16_t, 256> values_{};
  std::uint16_t operators_size_ = 0, values_size_ = 0;
};
}  // namespace ps::plugin_internal::numeric_ops
