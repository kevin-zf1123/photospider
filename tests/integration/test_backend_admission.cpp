#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
struct Constant {
  ps::Result<ps::ResultProgramPoll> poll(
      const ps::ResultProgramPhase& phase) try {
    using multi_result::check;
    using multi_result::take;
    auto builder = take(ps::ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key));
    check(builder.bind_descriptor_relation(
        take(ps::ResultRelation::cartesian(phase.resources, 1, {}))));
    const std::uint8_t number = 17;
    check(builder.publish_tensor(
        0, ps::Region::whole({1}), ps::ByteView(&number, 1),
        take(ps::ResultRelation::cartesian(phase.resources, 1, {})),
        {true, true, true, true}));
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{take(builder.seal()), true});
  } catch (const multi_result::Failure& failure) {
    return ps::Result<ps::ResultProgramPoll>(failure.status);
  }
};
}  // namespace

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto registry = std::make_shared<OperationRegistry>();
  unsigned calls = 0;
  OperationDefinition operation;
  operation.key = "test.gpu_only";
  operation.traits.supports_cpu = false;
  operation.traits.supports_gpu = true;
  const auto schema = multi_result::schema(ElementType::UInt8);
  operation.traits.outputs = {multi_result::output("value", schema)};
  operation.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  operation.start_result =
      [&](const ResultProgramQuery&,
          const BufferAllocator&) -> Result<ResultContinuation> {
    ++calls;
    return Result<ResultContinuation>(
        Status{ErrorCode::OperationFailed, "unexpected call"});
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
  auto fallback = operation;
  fallback.key = "test.gpu_fallback";
  fallback.traits.supports_cpu = true;
  fallback.traits.allows_cpu_fallback = true;
  unsigned cpu_calls = 0, gpu_calls = 0;
  fallback.start_result = [&](const ResultProgramQuery& query,
                              const BufferAllocator& allocator) {
    if (query.backend == Backend::Gpu) {
      ++gpu_calls;
      return Result<ResultContinuation>(Status{
          ErrorCode::OperationFailed, "unavailable GPU factory entered"});
    }
    ++cpu_calls;
    return ResultContinuation::make<Constant>(allocator);
  };
  PS_CHECK(registry->register_operation(std::move(fallback)).ok());
  PS_CHECK(registry->register_operation(std::move(operation)).ok());
  PS_CHECK(registry->freeze().ok());
  const std::map<std::string, ParameterValue> parameters;
  ResultProgramMetadata metadata;
  metadata.output.result_schema =
      std::make_shared<const SchemaTemplate>(schema);
  ResultProgramQuery query(metadata, parameters);
  query.semantic_key = "test.gpu_only";
  ResourceBudget root;
  auto direct =
      registry->start_result("test.gpu_only", query, root.allocator());
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
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto disabled = context.execute(gpu.value().plan);
  PS_CHECK(!disabled.ok() &&
           disabled.status().code == ErrorCode::BackendUnavailable);
  PS_CHECK(calls == 0);
  document.nodes[0].operation = "test.gpu_fallback";
  GraphContext fallback_graph(document);
  auto fallback_gpu = Compiler(registry).compile(fallback_graph, planning);
  PS_CHECK(fallback_gpu.ok() &&
           fallback_gpu.value().plan.steps()[0].backend == Backend::Gpu);
  auto recovered = context.execute(fallback_gpu.value().plan);
  PS_CHECK(recovered.ok() && cpu_calls == 1 && gpu_calls == 0);
  PS_CHECK(recovered.value().diagnostics.fallback_reasons.size() == 1);
  const auto& output = recovered.value().results.at("value");
  auto descriptor = output.descriptor();
  std::uint8_t number = 0;
  PS_CHECK(descriptor.ok() &&
           output.read_tensor(descriptor.value(), 0, {0}, &number, 1).ok() &&
           number == 17);
  return 0;
}
