#include "plugin/expression.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ps::expression_internal {
namespace {
Status invalid(const char* message) {
  return Status::failure(ErrorCode::InvalidArgument, message);
}
unsigned arity(Kind kind) {
  return kind == Kind::Add || kind == Kind::Subtract ||
                 kind == Kind::Multiply || kind == Kind::Divide ||
                 kind == Kind::Power || kind == Kind::Min || kind == Kind::Max
             ? 2
             : 1;
}
int precedence(Kind kind) {
  if (kind == Kind::Power)
    return 4;
  if (kind == Kind::Positive || kind == Kind::Negative)
    return 3;
  if (kind == Kind::Multiply || kind == Kind::Divide)
    return 2;
  return 1;
}
struct Operator {
  Kind kind = Kind::Add;
  bool parenthesis = false;
  bool function = false;
  std::uint16_t base = 0;
  std::uint16_t commas = 0;
};
class Parser {
 public:
  Parser(const std::string& source, std::uint64_t coefficients)
      : source_(source), coefficients_(coefficients) {}
  Result<Expression> run() {
    if (source_.empty() || source_.size() > 4096 || coefficients_ < 1 ||
        coefficients_ > 256)
      return Result<Expression>(invalid("expression source/table limit"));
    bool operand = true;
    while (position_ < source_.size()) {
      spaces();
      if (position_ == source_.size())
        break;
      const char c = source_[position_];
      if ((c >= '0' && c <= '9') || c == '.') {
        if (!operand || !literal())
          return failure();
        operand = false;
      } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        if (!operand)
          return Result<Expression>(invalid("missing expression operator"));
        const auto begin = position_++;
        while (position_ < source_.size() &&
               ((source_[position_] >= 'a' && source_[position_] <= 'z') ||
                (source_[position_] >= 'A' && source_[position_] <= 'Z') ||
                source_[position_] == '_'))
          ++position_;
        const auto name = source_.substr(begin, position_ - begin);
        if (name == "x") {
          if (!append({Kind::X}))
            return failure();
          operand = false;
        } else if (name == "c") {
          if (!coefficient())
            return failure();
          operand = false;
        } else {
          Kind kind;
          if (name == "abs")
            kind = Kind::Abs;
          else if (name == "sqrt")
            kind = Kind::Sqrt;
          else if (name == "exp")
            kind = Kind::Exp;
          else if (name == "log")
            kind = Kind::Log;
          else if (name == "sin")
            kind = Kind::Sin;
          else if (name == "cos")
            kind = Kind::Cos;
          else if (name == "min")
            kind = Kind::Min;
          else if (name == "max")
            kind = Kind::Max;
          else
            return Result<Expression>(invalid("unknown expression identifier"));
          spaces();
          if (!take('('))
            return Result<Expression>(invalid("function requires parentheses"));
          operators_.push_back({kind, true, true,
                                static_cast<std::uint16_t>(values_.size()), 0});
        }
      } else if (c == '(') {
        if (!operand)
          return Result<Expression>(
              invalid("missing operator before parentheses"));
        ++position_;
        operators_.push_back({Kind::Add, true, false,
                              static_cast<std::uint16_t>(values_.size()), 0});
      } else if (c == ')' || c == ',') {
        if (operand)
          return Result<Expression>(invalid("empty expression argument"));
        while (!operators_.empty() && !operators_.back().parenthesis)
          if (!reduce())
            return failure();
        if (operators_.empty())
          return Result<Expression>(invalid("unmatched expression delimiter"));
        auto& frame = operators_.back();
        if (values_.size() != frame.base + frame.commas + 1U)
          return Result<Expression>(invalid("malformed expression argument"));
        ++position_;
        if (c == ',') {
          if (!frame.function || ++frame.commas >= arity(frame.kind))
            return Result<Expression>(
                invalid("function argument count mismatch"));
          operand = true;
        } else {
          const auto closed = frame;
          operators_.pop_back();
          if (closed.function &&
              (closed.commas + 1U != arity(closed.kind) || !apply(closed.kind)))
            return failure();
          operand = false;
        }
      } else {
        Kind kind;
        if (c == '+')
          kind = operand ? Kind::Positive : Kind::Add;
        else if (c == '-')
          kind = operand ? Kind::Negative : Kind::Subtract;
        else if (c == '*')
          kind = Kind::Multiply;
        else if (c == '/')
          kind = Kind::Divide;
        else if (c == '^')
          kind = Kind::Power;
        else
          return Result<Expression>(invalid("unsupported expression token"));
        if (operand && kind != Kind::Positive && kind != Kind::Negative)
          return Result<Expression>(invalid("missing expression operand"));
        ++position_;
        if (!operand) {
          while (!operators_.empty() && !operators_.back().parenthesis &&
                 (precedence(operators_.back().kind) > precedence(kind) ||
                  (precedence(operators_.back().kind) == precedence(kind) &&
                   kind != Kind::Power)))
            if (!reduce())
              return failure();
        }
        operators_.push_back({kind, false, false, 0, 0});
        operand = true;
      }
    }
    if (operand)
      return Result<Expression>(invalid("incomplete expression"));
    while (!operators_.empty()) {
      if (operators_.back().parenthesis)
        return Result<Expression>(invalid("unclosed expression parentheses"));
      if (!reduce())
        return failure();
    }
    if (values_.size() != 1)
      return Result<Expression>(invalid("expression must have one result"));
    return Result<Expression>(std::move(expression_));
  }

 private:
  Result<Expression> failure() {
    return Result<Expression>(
        invalid("invalid expression literal, arity, index or AST limit"));
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
  bool append(Node node) {
    if (expression_.nodes.size() == 256 || node.depth > 32)
      return false;
    values_.push_back(static_cast<std::uint16_t>(expression_.nodes.size()));
    expression_.nodes.push_back(node);
    return true;
  }
  bool apply(Kind kind) {
    const auto count = arity(kind);
    if (values_.size() < count)
      return false;
    Node node{kind};
    node.left = values_[values_.size() - count];
    node.right = values_.back();
    node.depth = static_cast<std::uint16_t>(
        1 + std::max(expression_.nodes[node.left].depth,
                     expression_.nodes[node.right].depth));
    values_.resize(values_.size() - count);
    return append(node);
  }
  bool reduce() {
    const auto op = operators_.back();
    operators_.pop_back();
    return apply(op.kind);
  }
  bool literal() {
    const auto begin = position_;
    unsigned digits = 0;
    while (position_ < source_.size() && source_[position_] >= '0' &&
           source_[position_] <= '9') {
      ++digits;
      ++position_;
    }
    if (take('.')) {
      while (position_ < source_.size() && source_[position_] >= '0' &&
             source_[position_] <= '9') {
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
      while (position_ < source_.size() && source_[position_] >= '0' &&
             source_[position_] <= '9')
        ++position_;
      if (position_ == exponent)
        return false;
    }
    Node node{Kind::Literal};
    const auto parsed =
        std::from_chars(source_.data() + begin, source_.data() + position_,
                        node.number, std::chars_format::general);
    return parsed.ec == std::errc{} &&
           parsed.ptr == source_.data() + position_ &&
           std::isfinite(node.number) && append(node);
  }
  bool coefficient() {
    spaces();
    if (!take('['))
      return false;
    spaces();
    const auto begin = position_;
    unsigned index = 0;
    while (position_ < source_.size() && source_[position_] >= '0' &&
           source_[position_] <= '9') {
      index = index * 10 + static_cast<unsigned>(source_[position_++] - '0');
      if (index >= coefficients_)
        return false;
    }
    if (position_ == begin)
      return false;
    spaces();
    return take(']') &&
           append({Kind::Coefficient, static_cast<std::uint16_t>(index)});
  }
  const std::string& source_;
  std::uint64_t coefficients_;
  std::size_t position_ = 0;
  Expression expression_;
  std::vector<std::uint16_t> values_;
  std::vector<Operator> operators_;
};
double read(const std::uint8_t* bytes, std::uint16_t index) {
  double value;
  std::memcpy(&value, bytes + index * sizeof(double), sizeof(value));
  return value;
}
}  // namespace
Result<Expression> parse(const std::string& source,
                         std::uint64_t coefficients) {
  return Parser(source, coefficients).run();
}
Result<double> evaluate(const Expression& expression, double x,
                        const std::uint8_t* coefficients, std::uint8_t* scratch,
                        const CancellationToken& cancellation) {
  double value = 0;
  for (std::size_t i = 0; i < expression.nodes.size(); ++i) {
    if ((i & 31U) == 0 && cancellation.cancelled()) {
      Status s;
      s.code = ErrorCode::Cancelled;
      return Result<double>(s);
    }
    const auto& node = expression.nodes[i];
    const bool leaf = node.kind == Kind::Literal || node.kind == Kind::X ||
                      node.kind == Kind::Coefficient;
    const double a = leaf ? 0 : read(scratch, node.left),
                 b = leaf ? 0 : read(scratch, node.right);
    switch (node.kind) {
      case Kind::Literal:
        value = node.number;
        break;
      case Kind::X:
        value = x;
        break;
      case Kind::Coefficient:
        value = read(coefficients, node.left);
        break;
      case Kind::Positive:
        value = a;
        break;
      case Kind::Negative:
        value = -a;
        break;
      case Kind::Add:
        value = a + b;
        break;
      case Kind::Subtract:
        value = a - b;
        break;
      case Kind::Multiply:
        value = a * b;
        break;
      case Kind::Divide:
        if (b == 0)
          return Result<double>(Status::failure(ErrorCode::OperationFailed,
                                                "expression division by zero"));
        value = a / b;
        break;
      case Kind::Power:
        value = a == 0 && b == 0 ? 1 : std::pow(a, b);
        break;
      case Kind::Abs:
        value = std::abs(a);
        break;
      case Kind::Sqrt:
        value = std::sqrt(a);
        break;
      case Kind::Exp:
        value = std::exp(a);
        break;
      case Kind::Log:
        value = std::log(a);
        break;
      case Kind::Sin:
        value = std::sin(a);
        break;
      case Kind::Cos:
        value = std::cos(a);
        break;
      case Kind::Min:
        value = std::min(a, b);
        break;
      case Kind::Max:
        value = std::max(a, b);
        break;
    }
    if (!std::isfinite(value))
      return Result<double>(Status::failure(
          ErrorCode::OperationFailed, "nonfinite expression subexpression"));
    std::memcpy(scratch + i * sizeof(double), &value, sizeof(value));
  }
  return Result<double>(value);
}
}  // namespace ps::expression_internal
