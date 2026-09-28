#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto registry = std::make_shared<OperationRegistry>();
  unsigned calls = 0;
  OperationDefinition operation;
  operation.key = "test.gpu_only";
  operation.traits.supports_cpu = false;
  operation.traits.supports_gpu = true;
  operation.traits.outputs[0].shape_rule = OperationShapeRule::Fixed;
  operation.traits.outputs[0].fixed_output_shape = {1};
  operation.traits.outputs[0].output_element_type = ElementType::UInt8;
  operation.callback = [&](const OperationInvocation&) -> Result<Value> {
    ++calls;
    return Result<Value>(Status{ErrorCode::OperationFailed, "unexpected call"});
  };
  auto invalid = operation;
  invalid.key = "test.no_backend";
  invalid.traits.supports_gpu = false;
  PS_CHECK(registry->register_operation(std::move(invalid)).code ==
           ErrorCode::InvalidArgument);
  invalid = operation;
  invalid.key = "test.no_fallback_target";
  invalid.traits.allows_cpu_fallback = true;
  PS_CHECK(registry->register_operation(std::move(invalid)).code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(registry->register_operation(std::move(operation)).ok());
  PS_CHECK(registry->freeze().ok());
  const std::vector<Value> inputs;
  const std::vector<Region> demands;
  const std::map<std::string, ParameterValue> parameters;
  auto direct = registry->invoke(
      "test.gpu_only",
      OperationInvocation(inputs, demands, parameters, Backend::Cpu, {},
                          Region::whole({1})));
  PS_CHECK(!direct.ok() &&
           direct.status().code == ErrorCode::BackendUnavailable);
  WorkflowDocument document;
  document.nodes = {{1, "test.gpu_only", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  auto cpu = Compiler(registry).compile(graph);
  PS_CHECK(!cpu.ok() && cpu.status().code == ErrorCode::BackendUnavailable);
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::NativeGpu;
  auto gpu = Compiler(registry).compile(graph, planning);
  PS_CHECK(gpu.ok());
  ExecutionContextConfig config;
  config.gpu_enabled = false;
  ExecutionContext context(registry, config);
  auto disabled = context.execute(gpu.value().plan);
  PS_CHECK(!disabled.ok() &&
           disabled.status().code == ErrorCode::BackendUnavailable);
  PS_CHECK(calls == 0);
  return 0;
}
