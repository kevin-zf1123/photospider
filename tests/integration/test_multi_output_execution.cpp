#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Counts = std::array<std::atomic<unsigned>, 2>;

struct SelectState {
  bool requested = false;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    const auto port = phase.query.output_index;
    if (!requested) {
      requested = true;
      return Result<DependencyPoll>(
          DependencyNeedBatch{{{{0}, {{port, 1, phase.query.outputs, {}}}}},
                              {}});
    }
    double number = 0;
    auto read = phase.read(port, {0}, &number, sizeof(number));
    if (!read.ok())
      return Result<DependencyPoll>(read);
    auto made =
        MutableValue::allocate(phase.query.output.descriptor,
                               phase.query.outputs.boxes()[0], phase.allocator);
    if (!made.ok())
      return Result<DependencyPoll>(made.status());
    auto output = made.take_value();
    std::memcpy(output.data(), &number, sizeof(number));
    auto value = std::move(output).publish();
    if (!value.ok())
      return Result<DependencyPoll>(value.status());
    auto fragments =
        ValueFragments::create(phase.query.output.descriptor, {},
                               phase.query.outputs, {value.take_value()});
    if (!fragments.ok())
      return Result<DependencyPoll>(fragments.status());
    return Result<DependencyPoll>(fragments.take_value());
  }
};

double number(const DemandResult& result, const std::string& name) {
  double value = 0;
  const auto status = result.values.at(name).read({0}, &value, sizeof(value));
  if (!status.ok())
    throw std::runtime_error("invalid test fragment");
  return value;
}

int staged_outputs() {
  auto registry = std::make_shared<OperationRegistry>();
  auto counts = std::make_shared<Counts>();
  OperationDefinition op;
  op.key = "test.independent";
  op.traits.input_count = 2;
  op.traits.input_schema.resize(2);
  op.traits.outputs.resize(2);
  for (std::uint32_t i = 0; i < 2; ++i) {
    auto& output = op.traits.outputs[i];
    output.key = i ? "right" : "left";
    output.input_indices = std::vector<std::uint32_t>{i};
    output.region_rule = OperationRegionRule::Dependency;
    output.dependency_version = 1;
    output.continuation_bytes = sizeof(SelectState);
    output.maximum_dependency_stages = 2;
  }
  op.start_dependency = [counts](const DependencyQuery& query,
                                 const BufferAllocator& allocator) {
    ++(*counts)[query.output_index];
    return DependencyContinuation::make<SelectState>(allocator);
  };
  PS_CHECK(registry->register_operation(op).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {
      {1, "a", {ElementType::Float64, {1}}, Region::whole({1}), {0, {8}}, {}},
      {2, "b", {ElementType::Float64, {1}}, Region::whole({1}), {0, {8}}, {}}};
  document.nodes = {
      {1, op.key, {WorkflowInputReference{1}, WorkflowInputReference{2}}, {}}};
  document.outputs = {{"left", 1, "left"}, {"right", 1, "right"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto compiled = compiler.compile(graph);
  PS_CHECK(compiled.ok());
  ExecutionContext execution(registry, {1, false, 16, 4096, 2048});
  ExecutionBindings bindings{
      {{"a", Value::from_float64(7)}, {"b", Value::from_float64(11)}}};
  auto frozen = execution.freeze(compiled.value().plan, bindings);
  PS_CHECK(frozen.ok());
  const DemandQuery query{{"left", Footprint::all({1}).take_value()},
                          {"right", Footprint::all({1}).take_value()}};
  auto first = execution.execute_fragments(frozen.value(), query);
  PS_CHECK(first.ok());
  PS_CHECK(number(first.value(), "left") == 7);
  PS_CHECK(number(first.value(), "right") == 11);
  PS_CHECK((*counts)[0] == 1 && (*counts)[1] == 1);
  PS_CHECK(first.value().dependencies.certificate({1, 0}).ok());
  PS_CHECK(first.value().dependencies.certificate({1, 1}).ok());
  auto point = Footprint::all({1}).take_value();
  auto dirty = first.value().dependencies.potential_dirty("a", point);
  PS_CHECK(dirty.ok() && dirty.value().at("left") == point);
  PS_CHECK(dirty.value().at("right").empty());
  bindings.inputs[0].value = Value::from_float64(13);
  auto replacement = execution.freeze(compiled.value().plan, bindings);
  PS_CHECK(replacement.ok());
  auto changed = execution.execute_fragments(replacement.value(), query);
  PS_CHECK(changed.ok());
  PS_CHECK(number(changed.value(), "left") == 13);
  PS_CHECK(number(changed.value(), "right") == 11);
  PS_CHECK((*counts)[0] == 2 && (*counts)[1] == 1);
  auto old = execution.execute_fragments(frozen.value(), query);
  PS_CHECK(old.ok() && number(old.value(), "left") == 7);
  execution.clear_result_cache();
  PS_CHECK(first.value().dependencies.potential_dirty("a", point).ok());
  document.outputs = {{"right", 1, "right"}};
  GraphContext single(document);
  auto selected = compiler.compile(single);
  PS_CHECK(selected.ok() && selected.value().plan.steps().size() == 1);
  const auto left_before = (*counts)[0].load();
  PS_CHECK(execution.execute(selected.value().plan, bindings).ok());
  PS_CHECK((*counts)[0] == left_before);
  return 0;
}

int projected_sync_inputs() {
  auto registry = std::make_shared<OperationRegistry>();
  auto unwanted_calls = std::make_shared<std::atomic<unsigned>>(0);
  OperationDefinition unwanted;
  unwanted.key = "test.unwanted";
  unwanted.callback = [unwanted_calls](const OperationInvocation&) {
    ++*unwanted_calls;
    return Result<Value>(
        Status{ErrorCode::OperationFailed, "unrequested input evaluated"});
  };
  PS_CHECK(registry->register_operation(unwanted).ok());
  OperationDefinition projected;
  projected.key = "test.projected";
  projected.traits.input_count = 2;
  projected.traits.input_schema.resize(2);
  projected.traits.outputs.resize(2);
  projected.traits.outputs[0].key = "pass";
  projected.traits.outputs[0].input_indices = std::vector<std::uint32_t>{0};
  projected.traits.outputs[1].key = "constant";
  projected.traits.outputs[1].input_indices = std::vector<std::uint32_t>{};
  projected.callback = [](const OperationInvocation& call) -> Result<Value> {
    if (call.input_metadata.size() != 2)
      return Result<Value>(
          Status{ErrorCode::OperationFailed, "missing static metadata"});
    if (call.output_index == 0) {
      if (call.inputs.size() != 1 ||
          call.input_indices != std::vector<std::uint32_t>{0})
        return Result<Value>(
            Status{ErrorCode::OperationFailed, "wrong projection"});
      return Result<Value>(call.inputs[0]);
    }
    if (!call.inputs.empty())
      return Result<Value>(
          Status{ErrorCode::OperationFailed, "unexpected input"});
    auto made = MutableValue::allocate({ElementType::Float64, {1}},
                                       call.output_region, call.allocator);
    if (!made.ok())
      return Result<Value>(made.status());
    auto output = made.take_value();
    const double value = 29;
    std::memcpy(output.data(), &value, sizeof(value));
    return std::move(output).publish();
  };
  PS_CHECK(registry->register_operation(projected).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {
      {1, "a", {ElementType::Float64, {1}}, Region::whole({1}), {0, {8}}, {}}};
  document.nodes = {
      {1, unwanted.key, {}, {}},
      {2,
       projected.key,
       {WorkflowInputReference{1}, WorkflowNodeOutput{1, "value"}},
       {}}};
  document.outputs = {{"pass", 2, "pass"}, {"constant", 2, "constant"}};
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph);
  PS_CHECK(compiled.ok());
  ExecutionContext execution(registry, {1, false, 8, 4096});
  auto result = execution.execute(compiled.value().plan,
                                  {{{"a", Value::from_float64(7)}}});
  if (!result.ok())
    std::cerr << result.status().message << "\n";
  PS_CHECK(result.ok());
  PS_CHECK(test::named_scalar(result.value(), "pass") == 7);
  PS_CHECK(test::named_scalar(result.value(), "constant") == 29);
  PS_CHECK(unwanted_calls->load() == 0);
  return 0;
}

int synchronous_outputs() {
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->load_plugin(PS_MULTI_OUTPUT_FIXTURE).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.nodes = {{1, "test.c_results", {}, {}}};
  document.outputs = {{"first", 1, "first"}, {"second", 1, "second"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto compiled = compiler.compile(graph);
  PS_CHECK(compiled.ok());
  ExecutionContext execution(registry, {1, false, 16, 4096, 2048});
  for (unsigned i = 0; i < 2; ++i) {
    auto result = execution.execute(compiled.value().plan);
    PS_CHECK(result.ok());
    PS_CHECK(test::named_scalar(result.value(), "first") == 10);
    PS_CHECK(test::named_scalar(result.value(), "second") == 11);
    PS_CHECK(result.value().diagnostics.selected_backends.size() == 2);
  }
  return 0;
}
}  // namespace

int main() {
  PS_CHECK(staged_outputs() == 0);
  PS_CHECK(synchronous_outputs() == 0);
  PS_CHECK(projected_sync_inputs() == 0);
  return 0;
}
