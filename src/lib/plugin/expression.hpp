#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "photospider/core/status.hpp"
#include "photospider/execution/cancellation.hpp"

namespace ps::expression_internal {
enum class Kind : std::uint8_t {
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
  Log,
  Sin,
  Cos,
  Min,
  Max
};
struct Node {
  Kind kind;
  std::uint16_t left = 0, right = 0;
  std::uint16_t depth = 1;
  double number = 0;
};
struct Expression {
  std::vector<Node> nodes;
};
/** @brief Iterative bounded parser; same AST rules in metadata and execution.
 */
Result<Expression> parse(const std::string& source, std::uint64_t coefficients);
/** @brief Evaluates postorder nodes into caller-owned 256-double byte scratch.
 * Coefficient scratch holds 256 doubles; read/write use memcpy, no alignment
 * requirement. Parsed node references are bounded and never retain Run data.
 */
Result<double> evaluate(const Expression& expression, double x,
                        const std::uint8_t* coefficients, std::uint8_t* scratch,
                        const CancellationToken& cancellation);
}  // namespace ps::expression_internal
