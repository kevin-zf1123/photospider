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
  auto gpu_effect = operation;
  gpu_effect.key = "test.gpu_effect";
  gpu_effect.traits.side_effect_free = false;
  gpu_effect.traits.cacheable = false;
  PS_REQUIRE_OK(registry->register_operation(std::move(gpu_effect)));
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
  OperationDefinition metadata_consumer;
  metadata_consumer.key = "test.metadata_consumer";
  metadata_consumer.traits.input_count = 1;
  metadata_consumer.traits.input_schema.resize(1);
  metadata_consumer.traits.input_schema[0].kind = OperationPortKind::Result;
  metadata_consumer.traits.input_schema[0].tensor_key = "number";
  metadata_consumer.traits.input_schema[0].element_type =
      static_cast<std::uint32_t>(ElementType::UInt8);
  metadata_consumer.traits.outputs = {multi_result::output("value", schema)};
  metadata_consumer.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  metadata_consumer.traits.outputs[0].input_indices =
      std::vector<std::uint32_t>{};
  metadata_consumer.start_result = [](const ResultProgramQuery&,
                                      const BufferAllocator& allocator) {
    return ResultContinuation::make<Constant>(allocator);
  };
  unsigned effect_calls = 0;
  auto effect_consumer = metadata_consumer;
  effect_consumer.key = "test.metadata_effect";
  effect_consumer.traits.side_effect_free = false;
  effect_consumer.traits.cacheable = false;
  effect_consumer.start_result = [&](const ResultProgramQuery&,
                                     const BufferAllocator& allocator) {
    ++effect_calls;
    return ResultContinuation::make<Constant>(allocator);
  };
  PS_REQUIRE_OK(registry->register_operation(std::move(effect_consumer)));
  PS_REQUIRE_OK(registry->register_operation(std::move(metadata_consumer)));
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
  WorkflowDocument metadata_document;
  metadata_document.nodes = {
      {1, "test.gpu_only", {}, {}},
      {2, "test.metadata_consumer", {WorkflowNodeOutput{1, "value"}}, {}}};
  metadata_document.outputs = {{"value", 2, "value"}};
  GraphContext metadata_graph(metadata_document);
  auto metadata_cpu = Compiler(registry).compile(metadata_graph);
  PS_REQUIRE_OK(metadata_cpu);
  auto metadata_result = context.execute(metadata_cpu.value().plan);
  PS_REQUIRE_OK(metadata_result);
  PS_CHECK(calls == 0);
  auto metadata_native = Compiler(registry).compile(metadata_graph, planning);
  PS_REQUIRE_OK(metadata_native);
  PS_REQUIRE_OK(context.execute(metadata_native.value().plan));
  PS_CHECK(calls == 0);
  // A mandatory effect root still excludes its metadata-only GPU input.
  auto effect_document = metadata_document;
  effect_document.nodes.push_back(
      {3, "test.metadata_effect", {WorkflowNodeOutput{1, "value"}}, {}});
  GraphContext effect_graph(effect_document);
  auto effect_cpu = Compiler(registry).compile(effect_graph);
  PS_REQUIRE_OK(effect_cpu);
  PS_REQUIRE_OK(context.execute(effect_cpu.value().plan));
  PS_CHECK(calls == 0 && effect_calls == 1);
  auto tile = effect_cpu.value().plan.tile_plan("value", Region::whole({1}));
  PS_REQUIRE_OK(tile);
  PS_REQUIRE_OK(context.execute(tile.value()));
  PS_CHECK(calls == 0 && effect_calls == 2);
  // An unselected GPU-only effect is itself executable and must be admitted.
  auto unavailable_effect_document = metadata_document;
  unavailable_effect_document.nodes.push_back({3, "test.gpu_effect", {}, {}});
  GraphContext unavailable_effect_graph(unavailable_effect_document);
  auto unavailable_effect =
      Compiler(registry).compile(unavailable_effect_graph);
  PS_CHECK(!unavailable_effect.ok() &&
           unavailable_effect.status().code == ErrorCode::BackendUnavailable);
  // An excluded runtime input still has to meet the consumer's static schema.
  metadata_document.inputs = {multi_result::declaration(1, "wrong")};
  metadata_document.nodes = {
      {2, "test.metadata_consumer", {WorkflowInputReference{1}}, {}}};
  GraphContext malformed_metadata(metadata_document);
  PS_CHECK(Compiler(registry).compile(malformed_metadata).status().code ==
           ErrorCode::TypeMismatch);
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
