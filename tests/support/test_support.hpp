#pragma once

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

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

/** @brief Prints the full failure before a test attempts to unwrap a result. */
inline bool require_ok(const Status& status, const char* expression,
                       const char* file, int line) {
  if (status.ok())
    return true;
  std::cerr << file << ':' << line << ": " << expression
            << " failed: code=" << static_cast<int>(status.code)
            << " reason=" << static_cast<int>(status.reason)
            << " origin=" << static_cast<int>(status.detail.origin)
            << " scope=" << static_cast<int>(status.detail.scope)
            << " node=" << status.detail.node_id
            << " input=" << status.detail.input_id
            << " association=" << status.detail.association;
  if (status.detail.atom) {
    const auto& atom = *status.detail.atom;
    std::cerr << " atom=" << atom.output_index << '[';
    for (std::uint32_t axis = 0; axis < atom.rank; ++axis)
      std::cerr << (axis ? "," : "") << atom.coordinate[axis];
    std::cerr << ']';
  }
  if (status.detail.domain) {
    const auto& domain = *status.detail.domain;
    std::cerr << " domain=" << domain.first.output_index << '[';
    for (std::uint32_t axis = 0; axis < domain.first.rank; ++axis)
      std::cerr << (axis ? "," : "") << domain.first.coordinate[axis] << '+'
                << domain.extent[axis];
    std::cerr << ']';
  }
  std::cerr << " message=" << status.message << '\n';
  return false;
}
template <class T>
inline bool require_ok(const Result<T>& result, const char* expression,
                       const char* file, int line) {
  return require_ok(result.status(), expression, file, line);
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
      WorkflowNode{
          3U,
          "math.add",
          {WorkflowNodeOutput{1U, "value"}, WorkflowNodeOutput{2U, "value"}},
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
                   {WorkflowNodeOutput{1U, "value"}},
                   {{"milliseconds", milliseconds}}},
  };
  document.outputs = {WorkflowOutput{"result", 2U, "value"}};
  return document;
}

/**
 * @brief Reads a named Float64 scalar from a Result tensor.
 * @param result Successful execution result.
 * @param name Exact result name.
 * @return Scalar value or NaN when missing/malformed.
 * @throws Nothing unless map/string comparison allocates on an exotic runtime.
 */
inline double named_scalar(const ExecutionResult& result,
                           const std::string& name) {
  const auto object = result.results.find(std::string_view(name));
  if (object != result.results.end()) {
    auto facts = object->second.descriptor();
    double number = std::numeric_limits<double>::quiet_NaN();
    if (!facts.ok() || object->second.schema().tensors.size() != 1 ||
        object->second.schema().tensors[0].descriptor.element_type !=
            ElementType::Float64 ||
        object->second.schema().tensors[0].sample_shape() !=
            std::vector<std::uint64_t>{1} ||
        !object->second
             .read_tensor(facts.value(), 0, {0}, &number, sizeof(number))
             .ok())
      return std::numeric_limits<double>::quiet_NaN();
    return number;
  }
  return std::numeric_limits<double>::quiet_NaN();
}

}  // namespace ps::test

#define PS_CHECK(expression)                                           \
  do {                                                                 \
    if (!::ps::test::check(static_cast<bool>(expression), #expression, \
                           __FILE__, __LINE__)) {                      \
      return 1;                                                        \
    }                                                                  \
  } while (false)

#define PS_REQUIRE_OK(expression)                                    \
  do {                                                               \
    if (!::ps::test::require_ok((expression), #expression, __FILE__, \
                                __LINE__))                           \
      return 1;                                                      \
  } while (false)
