#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <map>
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
  // Renumber sources and node, prune a sibling (physical indices change), and
  // retain both content reuse and the new graph's certificate/dirty routes.
  auto renamed = document;
  renamed.inputs[0].id = 20;
  renamed.inputs[1].id = 10;
  renamed.nodes[0].id = 99;
  renamed.nodes[0].inputs = {WorkflowInputReference{20},
                             WorkflowInputReference{10}};
  renamed.outputs = {{"right", 99, "right"}};
  GraphContext renamed_graph(renamed);
  auto renamed_plan = compiler.compile(renamed_graph).take_value().plan;
  const auto before_rename = (*counts)[1].load();
  auto renamed_frozen = execution.freeze(renamed_plan, bindings).take_value();
  auto renamed_result =
      execution.execute_fragments(renamed_frozen, {{"right", point}});
  PS_CHECK(renamed_result.ok() &&
           renamed_result.value().diagnostics.cache_hits == 1);
  PS_CHECK((*counts)[1] == before_rename &&
           number(renamed_result.value(), "right") == 11);
  PS_CHECK(renamed_result.value().dependencies.certificate({99, 1}).ok());
  PS_CHECK(!renamed_result.value().dependencies.certificate({1, 1}).ok());
  auto renamed_dirty =
      renamed_result.value().dependencies.potential_dirty("b", point);
  PS_CHECK(renamed_dirty.ok() && renamed_dirty.value().at("right") == point);
  // A cached descendant must rebind its complete upstream record DAG too.
  auto chain = document;
  chain.nodes.push_back(
      {2,
       op.key,
       {WorkflowNodeOutput{1, "left"}, WorkflowNodeOutput{1, "right"}},
       {}});
  chain.outputs = {{"left", 2, "left"}, {"right", 2, "right"}};
  GraphContext chain_graph(chain);
  auto chain_plan = compiler.compile(chain_graph).take_value().plan;
  auto chain_frozen = execution.freeze(chain_plan, bindings).take_value();
  auto seeded_chain = execution.execute_fragments(chain_frozen, query);
  PS_CHECK(seeded_chain.ok());
  chain.nodes[0].id = 77;
  chain.nodes[1].id = 88;
  chain.nodes[1].inputs = {WorkflowNodeOutput{77, "left"},
                           WorkflowNodeOutput{77, "right"}};
  chain.outputs = {{"right", 88, "right"}};
  GraphContext moved_chain(chain);
  auto moved_plan = compiler.compile(moved_chain).take_value().plan;
  auto moved_frozen = execution.freeze(moved_plan, bindings).take_value();
  const auto before_chain = (*counts)[1].load();
  auto moved_result =
      execution.execute_fragments(moved_frozen, {{"right", point}});
  PS_CHECK(moved_result.ok() &&
           moved_result.value().diagnostics.cache_hits == 1);
  PS_CHECK((*counts)[1] == before_chain);
  PS_CHECK(moved_result.value().dependencies.certificate({77, 1}).ok());
  PS_CHECK(moved_result.value().dependencies.certificate({88, 1}).ok());
  auto moved_dirty =
      moved_result.value().dependencies.potential_dirty("b", point);
  PS_CHECK(moved_dirty.ok() && moved_dirty.value().at("right") == point);
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

int projected_shapes_and_permutation() {
  auto registry = std::make_shared<OperationRegistry>();
  auto counts = std::make_shared<Counts>();
  OperationDefinition op;
  op.key = "test.projected_shapes";
  op.traits.input_count = 2;
  op.traits.input_schema.resize(2);
  op.traits.outputs.resize(2);
  for (std::uint32_t i = 0; i < 2; ++i) {
    auto& output = op.traits.outputs[i];
    output.key = i ? "right" : "left";
    output.input_indices = std::vector<std::uint32_t>{i};
    output.shape_rule = OperationShapeRule::Fixed;
    output.fixed_output_shape = {2U + i};
    output.output_dtype_rule = OperationDtypeRule::Input;
    output.output_dtype_input = i;
    output.region_rule = OperationRegionRule::Elementwise;
  }
  op.callback = [counts](const OperationInvocation& call) -> Result<Value> {
    ++(*counts)[call.output_index];
    if (call.input_metadata[0].descriptor.element_type !=
            ElementType::Float64 ||
        call.input_metadata[1].descriptor.element_type != ElementType::Int64)
      return Result<Value>(
          Status{ErrorCode::TypeMismatch, "original metadata order"});
    for (std::size_t i = 0; i < call.inputs.size(); ++i)
      if (call.input_indices[i] == call.output_index)
        return call.inputs[i].view(call.output_region);
    return Result<Value>(
        Status{ErrorCode::InvalidArgument, "missing original port"});
  };
  PS_CHECK(registry->register_operation(op).ok());
  auto a = Value::create({ElementType::Float64, {2}}, Region::whole({2}),
                         {0, {8}}, std::vector<std::uint8_t>(16))
               .take_value();
  auto b = Value::create({ElementType::Int64, {3}}, Region::whole({3}),
                         {0, {8}}, std::vector<std::uint8_t>(24))
               .take_value();
  const std::vector<Value> reversed{b, a};
  const std::vector<Region> demands{b.region(), a.region()};
  const std::map<std::string, ParameterValue> parameters;
  OperationInvocation call(reversed, demands, parameters);
  call.input_indices = {1, 0};
  for (std::uint32_t output = 0; output < 2; ++output) {
    call.output_index = output;
    auto direct = registry->invoke(op.key, call);
    PS_CHECK(direct.ok() && direct.value().descriptor().shape ==
                                std::vector<std::uint64_t>({2U + output}));
  }
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {{1, "a", a.descriptor(), a.region(), a.layout(), {}},
                     {2, "b", b.descriptor(), b.region(), b.layout(), {}}};
  document.nodes = {
      {1, op.key, {WorkflowInputReference{1}, WorkflowInputReference{2}}, {}}};
  document.outputs = {{"left", 1, "left"}, {"right", 1, "right"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.output_regions = {{"left", Region({{1, 1}})},
                             {"right", Region({{2, 1}})}};
  auto compiled = Compiler(registry).compile(graph, planning);
  if (!compiled.ok())
    std::cerr << compiled.status().message << '\n';
  PS_CHECK(compiled.ok());
  ExecutionContext execution(registry);
  auto result =
      execution.execute(compiled.value().plan, {{{"a", a}, {"b", b}}});
  PS_CHECK(result.ok());
  document.outputs = {{"left", 1, "left"}};
  GraphContext single(document);
  const auto right_before = (*counts)[1].load();
  auto selected = Compiler(registry).compile(single).take_value().plan;
  PS_CHECK(execution.execute(selected, {{{"a", a}, {"b", b}}}).ok());
  PS_CHECK((*counts)[1] == right_before);
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
  PS_CHECK(projected_shapes_and_permutation() == 0);
  PS_CHECK(synchronous_outputs() == 0);
  PS_CHECK(projected_sync_inputs() == 0);
  return 0;
}
