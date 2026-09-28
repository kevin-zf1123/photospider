#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <utility>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
Result<Value> compute(const BufferAllocator& allocator, std::uint64_t size) {
  auto allocated = allocator.allocate(size ? size : 1);
  if (!allocated.ok())
    return Result<Value>(allocated.status());
  auto scratch = allocated.take_value();
  if (size)
    std::memset(scratch.data(), 17, size);
  auto made = MutableValue::allocate({ElementType::UInt8, {1}},
                                     Region::whole({1}), allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  *output.data() = size ? scratch.data()[size - 1] : 0;
  return std::move(output).publish();
}
struct Tile final {
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    auto value =
        compute(phase.allocator,
                std::get<std::int64_t>(phase.query.parameters.at("bytes")));
    if (!value.ok())
      return Result<DependencyPoll>(value.status());
    auto fragments =
        ValueFragments::create(phase.query.output.descriptor, {},
                               phase.query.outputs, {value.take_value()});
    return fragments.ok() ? Result<DependencyPoll>(fragments.take_value())
                          : Result<DependencyPoll>(fragments.status());
  }
};
}  // namespace
int main() {
  auto registry = std::make_shared<OperationRegistry>();
  unsigned callbacks = 0;
  for (bool tiled : {false, true}) {
    OperationDefinition operation;
    operation.key = tiled ? "test.prepared_tile" : "test.prepared_whole";
    operation.traits.cacheable = false;
    operation.traits.workspace_bytes = 16;
    operation.traits.requires_metadata_specialization = true;
    operation.traits.parameter_schema = {
        {"bytes", OperationParameterType::Int64, true}};
    auto& output = operation.traits.outputs[0];
    output.output_element_type = ElementType::UInt8;
    output.shape_rule = OperationShapeRule::Fixed;
    output.fixed_output_shape = {1};
    if (tiled) {
      output.region_rule = OperationRegionRule::Dependency;
      output.dependency_version = 1;
      output.continuation_bytes = sizeof(Tile);
      output.maximum_dependency_stages = 1;
      operation.start_dependency = [](const DependencyQuery&,
                                      const BufferAllocator& allocator) {
        return DependencyContinuation::make<Tile>(allocator);
      };
    } else {
      operation.callback = [&](const OperationInvocation& invocation) {
        ++callbacks;
        return compute(
            invocation.allocator,
            std::get<std::int64_t>(invocation.parameters.at("bytes")));
      };
    }
    operation.prepare_static = [](const auto&, const auto& parameters) {
      OperationPreparation result;
      OperationOutputSpecialization output;
      output.metadata.descriptor = {ElementType::UInt8, {1}};
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
      auto result = context.execute(compiled.value().plan);
      if (!result.ok())
        std::cerr << result.status().message << '\n';
      PS_CHECK(result.ok());
      PS_CHECK(result.value().values.at("value").bytes()[0] ==
               (bytes ? 17 : 0));
      if (bytes) {
        auto limited_config = config;
        limited_config.managed_resources->capacity[ResourceKind::Payload] =
            2048;
        ExecutionContext limited(registry, limited_config);
        auto failure = limited.execute(compiled.value().plan);
        PS_CHECK(!failure.ok() &&
                 failure.status().code == ErrorCode::ResourceExhausted);
      }
    }
    auto overflow = registry->prepare_operation(
        key, {}, {{"bytes", static_cast<std::int64_t>(-1)}});
    PS_CHECK(!overflow.ok() &&
             overflow.status().code == ErrorCode::ResourceExhausted);
  }
  PS_CHECK(callbacks == 2);
  return 0;
}
