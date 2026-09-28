#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <utility>

#include "execution/execution_test_hooks.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
std::atomic<unsigned> queries{0};
ErrorCode injected = ErrorCode::Ok;
ErrorCode fail_second_query() noexcept {
  return ++queries == 2 ? injected : ErrorCode::Ok;
}
struct Counts {
  unsigned gpu_starts = 0, cpu_starts = 0, destroyed = 0;
  unsigned gpu_polls = 0, cpu_polls = 0;
  bool ordered_retirement = true;
};
struct State {
  State(Counts* counts, Backend backend) : counts(counts), backend(backend) {
    if (backend == Backend::Cpu && counts->destroyed != counts->gpu_starts)
      counts->ordered_retirement = false;
    ++(backend == Backend::Gpu ? counts->gpu_starts : counts->cpu_starts);
  }
  ~State() noexcept { ++counts->destroyed; }
  Counts* counts;
  Backend backend;
  bool supplied = false;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    ++(backend == Backend::Gpu ? counts->gpu_polls : counts->cpu_polls);
    if (!supplied) {
      supplied = true;
      return Result<DependencyPoll>(
          DependencyNeedBatch{{{{0}, {{0, 1, phase.query.outputs, {}}}}}, {}});
    }
    float sample = 0;
    auto read = phase.read(0, {0}, &sample, sizeof(sample));
    if (!read.ok())
      return Result<DependencyPoll>(read);
    auto allocated =
        MutableValue::allocate(phase.query.output.descriptor,
                               phase.query.outputs.boxes()[0], phase.allocator);
    if (!allocated.ok())
      return Result<DependencyPoll>(allocated.status());
    auto writer = allocated.take_value();
    std::memcpy(writer.data(), &sample, sizeof(sample));
    auto output = std::move(writer).publish().take_value();
    return Result<DependencyPoll>(
        ValueFragments::create(phase.query.output.descriptor, {},
                               phase.query.outputs, {output})
            .take_value());
  }
};
int capacity_failure(ErrorCode error) {
  queries = 0;
  injected = error;
  Counts counts;
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "native.capacity";
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.supports_gpu = true;
  traits.allows_cpu_fallback = true;
  traits.outputs[0].output_element_type = ElementType::Float32;
  traits.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  traits.outputs[0].region_rule = OperationRegionRule::Dependency;
  traits.outputs[0].dependency_version = 1;
  traits.outputs[0].continuation_bytes = sizeof(State);
  traits.outputs[0].maximum_dependency_stages = 2;
  operation.start_dependency = [&](const DependencyQuery& query,
                                   const BufferAllocator& allocator) {
    return DependencyContinuation::make<State>(allocator, &counts,
                                               query.backend);
  };
  PS_CHECK(registry->register_operation(std::move(operation)).ok());
  PS_CHECK(registry->freeze().ok());
  auto writer = MutableValue::allocate({ElementType::Float32, {1}},
                                       Region::whole({1}), BufferAllocator{})
                    .take_value();
  const float expected = 7;
  std::memcpy(writer.data(), &expected, sizeof(expected));
  auto input = std::move(writer).publish().take_value();
  WorkflowDocument document;
  document.inputs = {
      {1, "input", input.descriptor(), input.region(), input.layout(), {}}};
  document.nodes = {{1, "native.capacity", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"output", 1, "value"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::NativeGpu;
  auto compiled = Compiler(registry).compile(graph, planning);
  PS_CHECK(compiled.ok());
  ExecutionContextConfig config;
  config.gpu_enabled = true;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  if (!context.gpu_enabled())
    return 77;
  auto result = context.execute(compiled.value().plan, {{{"input", input}}});
  PS_CHECK(queries == 2 && counts.gpu_starts == 1 && counts.gpu_polls == 1);
  if (error == ErrorCode::BackendUnavailable) {
    PS_CHECK(result.ok());
    PS_CHECK(counts.cpu_starts == 1 && counts.cpu_polls == 2 &&
             counts.destroyed == 2 && counts.ordered_retirement);
    PS_CHECK(result.value().diagnostics.fallback_reasons.size() == 1);
    PS_CHECK(result.value().diagnostics.selected_backends.at({1, 0}) ==
             Backend::Cpu);
    PS_CHECK(result.value().values.at("output").bytes() == input.bytes());
  } else {
    PS_CHECK(!result.ok() && result.status().code == error);
    PS_CHECK(result.status().message == "injected native capacity query");
    PS_CHECK(counts.cpu_starts == 0 && counts.destroyed == 1);
  }
  // A failed speculative session releases its owners before a later Run.
  injected = ErrorCode::Ok;
  queries = 0;
  auto retry = context.execute(compiled.value().plan, {{{"input", input}}});
  PS_CHECK(retry.ok() && retry.value().diagnostics.fallback_reasons.empty());
  PS_CHECK(retry.value().values.at("output").bytes() == input.bytes());
  PS_CHECK(counts.destroyed == counts.cpu_starts + counts.gpu_starts);
  return 0;
}
}  // namespace
int main() {
  ps::execution_testing::ExecutionTestHooks hooks;
  hooks.native_device = true;
  hooks.native_capacity_error = fail_second_query;
  ps::execution_testing::install_execution_test_hooks(&hooks);
  for (auto error :
       {ps::ErrorCode::BackendUnavailable, ps::ErrorCode::ResourceExhausted,
        ps::ErrorCode::OperationFailed}) {
    const int result = capacity_failure(error);
    if (result != 0)
      return result;
  }
  ps::execution_testing::install_execution_test_hooks(nullptr);
  std::cout
      << "native dependency capacity: typed errors, restart and retry passed\n";
  return 0;
}
