#include <exception>
#include <memory>
#include <utility>

#include "image_vertical/image_fixture.hpp"
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
        ps::WorkflowNode{3U,
                         "math.add",
                         {ps::WorkflowNodeOutput{1U, "value"},
                          ps::WorkflowNodeOutput{2U, "value"}},
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
    if (!value.ok() || value.value() != 42.0)
      return 3;
    for (bool plugin : {false, true}) {
      auto image_operations = plugin ? std::make_shared<ps::OperationRegistry>()
                                     : ps::make_default_operation_registry();
      if (plugin) {
        if (!image_operations->load_plugin(PS_IMAGE_CONSUMER_PLUGIN_PATH).ok())
          return 6;
        image_operations->freeze();
      }
      ps::Compiler image_compiler(image_operations);
      ps::GraphContext image_graph(s1_fixture::document());
      auto image_compiled =
          image_compiler.compile(image_graph, s1_fixture::demand());
      if (!image_compiled.ok())
        return 7;
      ps::ExecutionContext image_execution(image_operations);
      for (bool second : {false, true}) {
        auto image_result = image_execution.execute(
            image_compiled.value().plan, s1_fixture::bindings(second));
        if (!image_result.ok() ||
            !s1_fixture::oracle(image_result.value(), second))
          return 8;
      }
    }
    return 0;
  } catch (const std::exception&) {
    return 5;
  } catch (...) {
    return 5;
  }
}
