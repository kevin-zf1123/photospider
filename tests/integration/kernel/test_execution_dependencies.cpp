#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
Value values(ElementType type, const std::vector<T>& samples) {
  auto output =
      MutableValue::allocate({type, {samples.size()}},
                             Region::whole({samples.size()}), BufferAllocator{})
          .take_value();
  std::memcpy(output.data(), samples.data(), samples.size() * sizeof(T));
  return std::move(output).publish().take_value();
}
SchemaTemplate schema(std::uint64_t count = 2,
                      ElementType type = ElementType::Float64) {
  auto result = multi_result::schema(type, {count});
  result.id = "test.result.input";
  result.tensors[0].key = "data";
  return result;
}
WorkflowInputDeclaration declaration(unsigned id, std::string name,
                                     std::uint64_t count = 2,
                                     ElementType type = ElementType::Float64) {
  return multi_result::declaration(id, std::move(name), schema(count, type));
}
ResultRef backing_source(const ResourceBudget& root, const Value& backing) {
  const auto spec =
      schema(backing.descriptor().shape[0], backing.descriptor().element_type);
  auto output =
      multi_result::take(ResultBuilder::start(root, spec, "test.source"));
  multi_result::check(output.publish_tensor(
      0, backing.region(), backing.layout(),
      multi_result::take(root.reference(backing.storage()))));
  return multi_result::take(output.seal());
}
ExecutionBinding binding(const ResourceBudget& root, std::string name,
                         const Value& backing) {
  return {std::move(name), backing_source(root, backing)};
}
struct Route {
  bool choose = false, whole = false, ready = false;
  explicit Route(bool choose, bool whole = false)
      : choose(choose), whole(whole) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    const auto shape =
        phase.query.output.result_schema->tensors[0].sample_shape();
    const auto outputs =
        phase.query.tensor_outputs.value_or(Footprint::all(shape).take_value());
    if (!ready && !outputs.empty()) {
      ready = true;
      ResultProgramNeed need;
      for (unsigned port = 0; port < phase.query.inputs.size(); ++port)
        need.tiles.push_back({port, true, false, {}});
      return Answer(std::move(need));
    }
    auto builder =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key, {},
                             phase.association ? std::vector<std::uint64_t>(
                                                     phase.association->begin(),
                                                     phase.association->end())
                                               : std::vector<std::uint64_t>{})
            .take_value();
    for (const auto& box : outputs.boxes()) {
      auto bytes =
          phase.allocator.allocate(box.element_count().take_value() * 8)
              .take_value();
      for (std::uint64_t i = 0; i < box.dimensions()[0].extent; ++i) {
        const auto at = box.dimensions()[0].offset + i;
        const auto port = choose && at == 1 ? 1U : 0U;
        double value = 0;
        auto status = phase.read_tensor(port, 0, {choose ? 0 : at}, &value, 8);
        if (!status.ok())
          return Answer(status);
        std::memcpy(bytes.data() + i * 8, &value, 8);
      }
      auto status =
          builder.publish_tensor(0, box, {0, {8}, {box.dimensions()[0].offset}},
                                 std::move(bytes).freeze());
      if (!status.ok())
        return Answer(status);
    }
    return Answer(ResultPublication{builder.seal().take_value(), true});
  }
};
OperationDefinition route(std::string key, unsigned inputs = 1,
                          bool choose = false, bool whole = false) {
  OperationDefinition op;
  op.key = std::move(key);
  op.traits.input_count = inputs;
  op.traits.workspace_bytes = 8192;
  op.traits.input_schema.resize(inputs);
  for (auto& input : op.traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.element_type = static_cast<unsigned>(ElementType::Float64);
    input.rank = 1;
  }
  op.traits.outputs[0] = multi_result::output("value", schema());
  op.traits.outputs[0].continuation_bytes = sizeof(Route);
  op.traits.outputs[0].maximum_dependency_stages = 4;
  op.traits.outputs[0].region_rule =
      whole ? OperationRegionRule::Whole : OperationRegionRule::Tiled;
  op.traits.requires_metadata_specialization = true;
  op.specialize_metadata = [](const std::vector<OperationMetadata>& inputs,
                              const std::map<std::string, ParameterValue>&) {
    OperationOutputSpecialization output;
    output.metadata = inputs[0];
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  op.start_result = [choose, whole](const ResultProgramQuery&,
                                    const BufferAllocator& allocator) {
    return ResultContinuation::make<Route>(allocator, choose, whole);
  };
  return op;
}
int late_delta() {
  ExecutionDependencies evidence;
  ResourceBudget retained_root;
  {
    auto registry = std::make_shared<OperationRegistry>();
    for (const auto* key : {"short", "long1", "long2", "choose", "result"}) {
      const bool choose = std::string(key) == "choose";
      auto op = route(key, choose ? 2 : 1, choose);
      PS_CHECK(registry->register_operation(std::move(op)).ok());
    }
    PS_CHECK(registry->freeze().ok());
    WorkflowDocument document;
    document.inputs = {declaration(1, "x")};
    document.nodes = {
        {1, "short", {WorkflowInputReference{1}}, {}},
        {2, "long1", {WorkflowInputReference{1}}, {}},
        {3, "long2", {WorkflowNodeOutput{2, "value"}}, {}},
        {4,
         "choose",
         {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{3, "value"}},
         {}},
        {5, "result", {WorkflowNodeOutput{4, "value"}}, {}}};
    document.outputs = {{"y", 5, "value"}};
    GraphContext graph(document);
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    ExecutionContext context(registry, {1, false, 4, 65536});
    retained_root = context.resource_budget().take_value();
    auto executed = context.execute(
        plan, {{binding(context.resource_budget().take_value(), "x",
                        values<double>(ElementType::Float64, {3, 5}))}});
    if (!executed.ok())
      throw std::runtime_error(
          std::to_string(static_cast<unsigned>(executed.status().code)) + ": " +
          executed.status().message);
    PS_CHECK(executed.ok());
    const auto& result = executed.value().results.at("y");
    PS_CHECK(multi_result::number(result, {0}) == 3 &&
             multi_result::number(result, {1}) == 3);
    evidence = executed.value().dependencies;
    PS_CHECK(evidence.valid());
    auto released = executed.take_value();
    released.results.clear();
    context.clear_result_cache();
  }
  PS_CHECK(retained_root.statistics().live[ResourceKind::Payload] == 0);
  for (std::size_t step = 0; step < 5; ++step) {
    auto record = evidence.tile_record({step, 0, 0});
    PS_REQUIRE_OK(record);
    PS_CHECK(record.value() &&
             record.value()->inputs.size() == (step == 3 ? 2 : 1));
    for (const auto& input : record.value()->inputs)
      PS_CHECK(input.whole && input.tiles.empty());
  }
  ResourceVector<TileChange> changes{
      ResourceAllocator<TileChange>(retained_root)};
  changes.push_back({PlanWorkflowInput{0}, 0,
                     TileSet::from_indices(retained_root, {0}).take_value(),
                     false});
  auto dirty = evidence.dirty_units(changes);
  PS_REQUIRE_OK(dirty);
  PS_CHECK(dirty.value().size() == 5);
  for (std::size_t step = 0; step < 5; ++step)
    PS_CHECK((dirty.value()[step] == TileUnit{step, 0, 0}));
  PS_CHECK(!evidence.tile_record({999, 0, 0}).value());
  return 0;
}
int whole_record() {
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(
      registry->register_operation(route("global", 100, false, true)).ok());
  PS_CHECK(registry->register_operation(route("leaf")).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {declaration(1, "x")};
  document.nodes = {{1, "global", {WorkflowInputReference{1}}, {}},
                    {2, "leaf", {WorkflowNodeOutput{1, "value"}}, {}}};
  document.nodes[0].inputs.assign(100, WorkflowInputReference{1});
  document.outputs = {{"y", 2, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(registry)
                  .compile(graph)
                  .take_value()
                  .plan.tile_plan("y", TileRequest{true, {}})
                  .take_value();
  ExecutionContext context(registry, {1, false, 4, 4096});
  auto executed = context.execute(
      plan, {{binding(context.resource_budget().take_value(), "x",
                      values<double>(ElementType::Float64, {3, 5}))}});
  PS_CHECK(executed.ok());
  const auto& evidence = executed.value().dependencies;
  auto global = evidence.tile_record({0, 0, 0}).take_value();
  auto leaf = evidence.tile_record({1, 0, 0}).take_value();
  PS_CHECK(global && global->inputs.size() == 1 && global->inputs[0].whole);
  PS_CHECK(leaf && leaf->inputs.size() == 1 && leaf->inputs[0].whole);
  const auto& result = executed.value().results.at("y");
  PS_CHECK(multi_result::number(result, {0}) == 3 &&
           multi_result::number(result, {1}) == 5);
  document.outputs = {{"global", 1, "value"}};
  GraphContext global_graph(document);
  const auto global_plan =
      Compiler(registry).compile(global_graph).take_value().plan;
  auto demand =
      context
          .open_demand(
              global_plan,
              {{binding(context.resource_budget().take_value(), "x",
                        values<double>(ElementType::Float64, {3, 5}))}})
          .take_value();
  auto complete = demand.request({{"global", {true, {}}}});
  PS_REQUIRE_OK(complete);
  auto independent = demand.request({{"global", {false, {}}}});
  PS_REQUIRE_OK(independent);
  PS_CHECK(!independent.value().dependencies.tile_record({0, 0, 0}).value());
  PS_CHECK(independent.value()
               .results.at("global")
               .descriptor()
               .value()
               .tensor_coverage(0)
               .empty());
  return 0;
}
int metadata_bounds() {
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->register_operation(route("copy")).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {declaration(1, "x"),
                     declaration(2, "unused", 64, ElementType::UInt8)};
  document.nodes = {{1, "copy", {WorkflowInputReference{1}}, {}}};
  for (unsigned i = 0; i < 17; ++i)
    document.outputs.push_back({"alias" + std::to_string(i), 1, "value"});
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContextConfig config{1, false, 4, 65536};
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Metadata] = 16 * 1024 * 1024;
  ExecutionContext context(registry, config);
  const auto root = context.resource_budget().take_value();
  ExecutionBindings bindings{
      {binding(root, "x", values<double>(ElementType::Float64, {3, 5})),
       binding(root, "unused",
               values<std::uint8_t>(ElementType::UInt8,
                                    std::vector<std::uint8_t>(64)))}};
  auto full = context.execute(plan, bindings);
  PS_REQUIRE_OK(full);
  PS_CHECK(full.value().results.size() == 17);
  const auto& evidence = full.value().dependencies;
  auto record = evidence.tile_record({0, 0, 0}).take_value();
  PS_CHECK(record && record->inputs.size() == 1);
  ResourceVector<TileChange> changes{ResourceAllocator<TileChange>(root)};
  changes.push_back({PlanWorkflowInput{0}, 0, {}, true});
  const auto expected = evidence.dirty_units(changes);
  PS_REQUIRE_OK(expected);
  PS_CHECK(expected.value().size() == 1 &&
           (expected.value()[0] == TileUnit{0, 0, 0}));
  {
    const auto remaining = root.available_capacity()[ResourceKind::Metadata] -
                           ResourceBudget::lease_metadata_bytes();
    const auto capacity = ResourceCapacity::host(remaining, remaining);
    auto reservation = root.reserve(capacity).take_value();
    auto rejected = evidence.dirty_units(changes);
    PS_CHECK(!rejected.ok() &&
             rejected.status().code == ErrorCode::ResourceExhausted);
  }
  auto recovered = evidence.dirty_units(changes);
  PS_REQUIRE_OK(recovered);
  PS_CHECK(recovered.value() == expected.value());
  changes[0].producer = PlanWorkflowInput{1};
  PS_CHECK(evidence.dirty_units(changes).value().empty());
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(late_delta() == 0);
  PS_CHECK(metadata_bounds() == 0);
  PS_CHECK(whole_record() == 0);
  return 0;
}
