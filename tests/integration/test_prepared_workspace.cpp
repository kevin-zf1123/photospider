#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct Computation final {
  unsigned* completed;
  explicit Computation(unsigned* completed) : completed(completed) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    using multi_result::check;
    using multi_result::take;
    const auto size = static_cast<std::uint64_t>(
        std::get<std::int64_t>(phase.query.parameters.at("bytes")));
    auto scratch = take(phase.allocator.allocate(size ? size : 1));
    if (size)
      std::memset(scratch.data(), 17, size);
    auto output = take(phase.allocator.allocate(1));
    *output.data() = size ? scratch.data()[size - 1] : 0;
    if (completed)
      ++*completed;
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, {}, phase.query.tile_height,
        phase.query.tile_width, phase.query.resources));
    check(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(phase.resources, 1, {}))));
    check(builder.publish_tensor(
        0, Region::whole({1}), {0, {1}}, std::move(output).freeze(),
        take(ResultRelation::cartesian(phase.resources, 1, {})),
        {true, true, true, true}));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  } catch (const multi_result::Failure& failure) {
    return Result<ResultProgramPoll>(failure.status);
  }
};
}  // namespace
int main() {
  auto registry = std::make_shared<OperationRegistry>();
  unsigned completed_whole = 0;
  unsigned preparations = 0;
  const auto schema = multi_result::schema(ElementType::UInt8);
  for (bool tiled : {false, true}) {
    OperationDefinition operation;
    operation.key = tiled ? "test.prepared_tile" : "test.prepared_whole";
    operation.traits.cacheable = false;
    operation.traits.workspace_bytes = 16;
    operation.traits.requires_metadata_specialization = true;
    operation.traits.parameter_schema = {
        {"bytes", OperationParameterType::Int64, true}};
    auto& output = operation.traits.outputs[0];
    output.output_schema.kind = OperationPortKind::Result;
    output.output_schema.result_schema_id = std::string(schema.id);
    output.output_schema.result_schema_version = schema.version;
    output.result_schema = schema;
    output.region_rule =
        tiled ? OperationRegionRule::Dependency : OperationRegionRule::Whole;
    output.dependency_version = 2;
    output.continuation_bytes = sizeof(Computation);
    output.maximum_dependency_stages = 1;
    operation.start_result = [&, tiled](const ResultProgramQuery&,
                                        const BufferAllocator& allocator) {
      return ResultContinuation::make<Computation>(
          allocator, tiled ? nullptr : &completed_whole);
    };
    operation.prepare_static = [&](const auto&, const auto& parameters) {
      ++preparations;
      OperationPreparation result;
      OperationOutputSpecialization output;
      output.metadata.result_schema =
          std::make_shared<const SchemaTemplate>(schema);
      result.outputs.push_back(std::move(output));
      const auto bytes = std::get<std::int64_t>(parameters.at("bytes"));
      result.additional_workspace_bytes = bytes < 0 ? UINT64_MAX : bytes;
      return Result<OperationPreparation>(std::move(result));
    };
    PS_CHECK(registry->register_operation(std::move(operation)).ok());
  }
  PS_CHECK(registry->freeze().ok());
  ExecutionContextConfig config;
  config.gpu_enabled = false;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = context.resource_budget();
  PS_CHECK(root.ok());
  for (const auto* key : {"test.prepared_whole", "test.prepared_tile"}) {
    for (std::int64_t bytes : {0, 4096}) {
      auto prepared = registry->prepare_operation(key, {}, {{"bytes", bytes}});
      PS_CHECK(prepared.ok());
      PS_CHECK(prepared.value()->traits().workspace_bytes ==
               16U + static_cast<std::uint64_t>(bytes));
      WorkflowDocument document;
      document.nodes = {{1, key, {}, {{"bytes", bytes}}}};
      document.outputs = {{"value", 1, "value"}};
      GraphContext graph(document);
      auto compiled = Compiler(registry).compile(graph);
      PS_CHECK(compiled.ok());
      const auto preparations_before_execution = preparations;
      for (unsigned repeat = 0; repeat < 2; ++repeat) {
        auto result = context.execute(compiled.value().plan);
        if (!result.ok())
          std::cerr << result.status().message << '\n';
        PS_CHECK(result.ok());
        const auto& output = result.value().results.at("value");
        auto descriptor = output.descriptor();
        PS_CHECK(descriptor.ok());
        std::uint8_t observed = 255;
        PS_CHECK(
            output.read_tensor(descriptor.value(), 0, {0}, &observed, 1).ok());
        PS_CHECK(observed == (bytes ? 17 : 0));
        PS_CHECK(preparations == preparations_before_execution);
      }
      PS_CHECK(root.value().statistics().live[ResourceKind::Payload] == 0);
      if (bytes) {
        auto limited_config = config;
        limited_config.managed_resources->capacity[ResourceKind::Payload] =
            2048;
        ExecutionContext limited(registry, limited_config);
        auto failure = limited.execute(compiled.value().plan);
        PS_CHECK(!failure.ok() &&
                 failure.status().code == ErrorCode::ResourceExhausted);
        PS_CHECK(preparations == preparations_before_execution);
        auto limited_root = limited.resource_budget();
        PS_CHECK(limited_root.ok());
        PS_CHECK(
            limited_root.value().statistics().live[ResourceKind::Payload] == 0);
      }
    }
    auto overflow = registry->prepare_operation(
        key, {}, {{"bytes", static_cast<std::int64_t>(-1)}});
    PS_CHECK(!overflow.ok() &&
             overflow.status().code == ErrorCode::ResourceExhausted);
  }
  PS_CHECK(completed_whole == 4);
  return 0;
}
