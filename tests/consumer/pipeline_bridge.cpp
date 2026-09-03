#include <exception>
#include <utility>

#include "photospider/photospider.hpp"

#if defined(_MSVC_LANG)
static_assert(_MSVC_LANG >= 201703L, "the kernel target must propagate C++17");
#else
static_assert(__cplusplus >= 201703L, "the kernel target must propagate C++17");
#endif

/**
 * @brief Compiles and executes one graph through the installed kernel target.
 *
 * The downstream shared bridge constructs a source document, compiles it, and
 * executes the resulting plan before validating the scalar output. Keeping the
 * complete pipeline in this shared object proves that a default static kernel
 * archive can be embedded in a downstream shared library.
 *
 * @return Zero when the installed package produces the expected scalar; one
 * for compilation failure, two for execution failure, three for an unexpected
 * result, or five when a C++ exception reaches the bridge boundary.
 * @throws Nothing; every C++ exception is fenced at the C linkage boundary.
 * @note The function borrows no caller state and releases all graph, registry,
 * execution, and Value ownership before returning.
 */
extern "C" int photospider_consumer_run_pipeline(void) {
  try {
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
  } catch (const std::exception&) {
    return 5;
  } catch (...) {
    return 5;
  }
}
