#include <utility>

#include "photospider/photospider.hpp"

/**
 * @brief Compiles and executes one graph through only installed public headers.
 * @return Zero when the installed package produces the expected scalar.
 * @throws std::bad_alloc If consumer setup allocation fails.
 * @note Non-exception pipeline failures return distinct nonzero codes.
 */
int main() {
  ps::WorkflowDocument document;
  document.nodes = {
      ps::WorkflowNode{1U, "core.constant", {}, {{"value", 20.0}}},
      ps::WorkflowNode{2U, "core.constant", {}, {{"value", 22.0}}},
      ps::WorkflowNode{
          3U,
          "math.add",
          {ps::WorkflowInput{1U, "value"}, ps::WorkflowInput{2U, "value"}},
          {}},
  };
  document.outputs = {ps::WorkflowOutput{"answer", 3U, "value"}};

  auto operations = ps::make_default_operation_registry();
  ps::Compiler compiler(operations);
  ps::GraphContext graph(std::move(document));
  auto compiled = compiler.compile(graph);
  if (!compiled.ok()) {
    return 1;
  }
  ps::ExecutionContext execution(operations);
  auto result = execution.execute(compiled.value().plan);
  if (!result.ok()) {
    return 2;
  }
  const auto value = result.value().values.at("answer").as_float64();
  return value.ok() && value.value() == 42.0 ? 0 : 3;
}
