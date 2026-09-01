#pragma once

#include <cmath>
#include <iostream>
#include <limits>
#include <string>

#include "photospider/photospider.hpp"

namespace ps::test {

/**
 * @brief Reports one failed test condition with source location.
 * @param condition Evaluated condition.
 * @param expression Source expression text.
 * @param file Source filename.
 * @param line Source line.
 * @return True when the condition passed.
 * @throws Nothing.
 */
inline bool check(bool condition, const char* expression, const char* file,
                  int line) noexcept {
  if (!condition) {
    std::cerr << file << ':' << line << ": check failed: " << expression
              << '\n';
  }
  return condition;
}

/**
 * @brief Builds a deterministic two-constant addition document.
 * @param left First scalar.
 * @param right Second scalar.
 * @return Source document whose `sum` output equals left plus right.
 * @throws std::bad_alloc If container/string allocation fails.
 */
inline WorkflowDocument addition_document(double left, double right) {
  WorkflowDocument document;
  document.nodes = {
      WorkflowNode{1U, "core.constant", {}, {{"value", left}}},
      WorkflowNode{2U, "core.constant", {}, {{"value", right}}},
      WorkflowNode{3U,
                   "math.add",
                   {WorkflowInput{1U, "value"}, WorkflowInput{2U, "value"}},
                   {}},
  };
  document.outputs = {WorkflowOutput{"sum", 3U, "value"}};
  return document;
}

/**
 * @brief Builds a constant followed by a cooperative delay.
 * @param milliseconds Delay duration in the maintained 0..5000 range.
 * @return Source document whose `result` output preserves the constant.
 * @throws std::bad_alloc If container/string allocation fails.
 */
inline WorkflowDocument delayed_document(std::int64_t milliseconds) {
  WorkflowDocument document;
  document.nodes = {
      WorkflowNode{1U, "core.constant", {}, {{"value", 7.0}}},
      WorkflowNode{2U,
                   "core.delay",
                   {WorkflowInput{1U, "value"}},
                   {{"milliseconds", milliseconds}}},
  };
  document.outputs = {WorkflowOutput{"result", 2U, "value"}};
  return document;
}

/**
 * @brief Reads one named Float64 scalar result.
 * @param result Successful execution result.
 * @param name Exact result name.
 * @return Scalar value or NaN when missing/malformed.
 * @throws Nothing unless map/string comparison allocates on an exotic runtime.
 */
inline double named_scalar(const ExecutionResult& result,
                           const std::string& name) {
  const auto iterator = result.values.find(name);
  if (iterator == result.values.end()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  auto value = iterator->second.as_float64();
  return value.ok() ? value.value() : std::numeric_limits<double>::quiet_NaN();
}

}  // namespace ps::test

#define PS_CHECK(expression)                                           \
  do {                                                                 \
    if (!::ps::test::check(static_cast<bool>(expression), #expression, \
                           __FILE__, __LINE__)) {                      \
      return 1;                                                        \
    }                                                                  \
  } while (false)
