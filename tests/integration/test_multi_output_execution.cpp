#include <algorithm>
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
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Counts = std::array<std::atomic<unsigned>, 2>;

double number(const DemandResult& result, const std::string& name) {
  return multi_result::number(result.results.at(name.c_str()));
}

int verify_sources(const ExecutionDependencies& dependencies,
                   std::vector<std::string> expected,
                   const Footprint& samples) {
  auto observed = dependencies.source_observations();
  PS_CHECK(observed.ok());
  std::vector<std::string> names;
  for (const auto& source : observed.value()) {
    PS_CHECK(source.target == ResultSupportTarget::Tensor);
    PS_CHECK(source.slot == 0 && source.roles == 1);
    PS_CHECK(source.samples == samples);
    names.emplace_back(source.input.begin(), source.input.end());
  }
  std::sort(names.begin(), names.end());
  std::sort(expected.begin(), expected.end());
  PS_CHECK(names == expected);
  return 0;
}

int staged_outputs() {
  auto registry = std::make_shared<OperationRegistry>();
  auto counts = std::make_shared<Counts>();
  OperationDefinition op;
  op.key = "test.independent";
  op.traits.input_count = 2;
  op.traits.input_schema.resize(2);
  for (auto& input : op.traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.tensor_key = "number";
  }
  op.traits.outputs.resize(2);
  for (std::uint32_t i = 0; i < 2; ++i) {
    auto& output = op.traits.outputs[i];
    output = multi_result::output(i ? "right" : "left");
    output.input_indices = std::vector<std::uint32_t>{i};
  }
  op.start_result = [counts](const ResultProgramQuery& query,
                             const BufferAllocator& allocator) {
    ++(*counts)[query.output_index];
    return ResultContinuation::make<multi_result::Program>(allocator,
                                                           query.output_index);
  };
  multi_result::check(registry->register_operation(op));
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "a"),
                     multi_result::declaration(2, "b")};
  document.nodes = {
      {1, op.key, {WorkflowInputReference{1}, WorkflowInputReference{2}}, {}}};
  document.outputs = {{"left", 1, "left"}, {"right", 1, "right"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto compiled = compiler.compile(graph);
  PS_CHECK(compiled.ok());
  ExecutionContext execution(registry, {1, false, 16, 4096, 2048});
  const auto root = multi_result::take(execution.resource_budget());
  ExecutionBindings bindings{{multi_result::binding(root, "a", 7),
                              multi_result::binding(root, "b", 11)}};
  auto frozen = execution.freeze(compiled.value().plan, bindings);
  PS_CHECK(frozen.ok());
  const DemandQuery query{{"left", Footprint::all({1}).take_value()},
                          {"right", Footprint::all({1}).take_value()}};
  auto first = execution.execute_fragments(frozen.value(), query);
  PS_CHECK(first.ok());
  PS_CHECK(number(first.value(), "left") == 7);
  PS_CHECK(number(first.value(), "right") == 11);
  PS_CHECK((*counts)[0] == 1 && (*counts)[1] == 1);
  PS_CHECK(first.value().dependencies.record_count() == 4);
  auto point = Footprint::all({1}).take_value();
  PS_CHECK(verify_sources(first.value().dependencies, {"a", "b"}, point) == 0);
  auto dirty = first.value().dependencies.potential_dirty("a", point);
  PS_CHECK(dirty.ok() && dirty.value().at("left") == point);
  PS_CHECK(dirty.value().at("right").empty());
  bindings.inputs[0] = multi_result::binding(root, "a", 13);
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
  PS_CHECK(renamed_result.value().dependencies.record_count() == 2);
  PS_CHECK(verify_sources(renamed_result.value().dependencies, {"b"}, point) ==
           0);
  PS_CHECK(
      renamed_result.value().diagnostics.selected_backends.count({99, 1}) == 1);
  PS_CHECK(renamed_result.value().diagnostics.selected_backends.count({1, 1}) ==
           0);
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
  const auto left_before_chain = (*counts)[0].load();
  auto moved_result =
      execution.execute_fragments(moved_frozen, {{"right", point}});
  PS_CHECK(moved_result.ok() &&
           moved_result.value().diagnostics.cache_hits == 2);
  PS_CHECK((*counts)[1] == before_chain && (*counts)[0] == left_before_chain);
  PS_CHECK(number(moved_result.value(), "right") == 11);
  PS_CHECK(moved_result.value().diagnostics.selected_backends.count({77, 1}) ==
           1);
  PS_CHECK(moved_result.value().dependencies.record_count() == 4);
  PS_CHECK(verify_sources(moved_result.value().dependencies, {"b"}, point) ==
           0);
  PS_CHECK(moved_result.value().diagnostics.selected_backends.count({88, 1}) ==
           1);
  PS_CHECK(moved_result.value().diagnostics.selected_backends.count({2, 1}) ==
           0);
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

int projected_sync_inputs(bool terminal) {
  auto registry = std::make_shared<OperationRegistry>();
  auto unwanted_calls = std::make_shared<std::atomic<unsigned>>(0);
  OperationDefinition unwanted;
  unwanted.key = "test.unwanted";
  unwanted.traits.deterministic = false;
  unwanted.traits.cacheable = false;
  unwanted.traits.outputs[0] = multi_result::output("value");
  unwanted.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  if (terminal) {
    unwanted.traits.deterministic = true;
    unwanted.traits.outputs[0].observation_kind =
        ObservationKind::RequestRecord;
  }
  unwanted.start_result = [unwanted_calls](const ResultProgramQuery&,
                                           const BufferAllocator&) {
    ++*unwanted_calls;
    return Result<ResultContinuation>(
        Status{ErrorCode::OperationFailed, "unrequested input evaluated"});
  };
  PS_CHECK(registry->register_operation(unwanted).ok());
  OperationDefinition projected;
  projected.key = "test.projected";
  projected.traits.workspace_bytes = 8;
  projected.traits.input_count = 2;
  projected.traits.input_schema.resize(2);
  for (auto& input : projected.traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.tensor_key = "number";
  }
  projected.traits.outputs = {multi_result::output("pass"),
                              multi_result::output("constant")};
  projected.traits.outputs[0].input_indices = std::vector<std::uint32_t>{0};
  projected.traits.outputs[1].input_indices = std::vector<std::uint32_t>{};
  auto projected_calls = std::make_shared<Counts>();
  projected.start_result = [projected_calls](const ResultProgramQuery& query,
                                             const BufferAllocator& allocator) {
    ++(*projected_calls)[query.output_index];
    if (query.inputs.size() != 2)
      return Result<ResultContinuation>(
          Status{ErrorCode::OperationFailed, "missing static metadata"});
    return ResultContinuation::make<multi_result::Program>(
        allocator, query.output_index == 0 ? 0 : -1, 29.0);
  };
  PS_CHECK(registry->register_operation(projected).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "a")};
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
  ExecutionContext execution(registry, {1, false, 8, 4096, 2048});
  const auto root = multi_result::take(execution.resource_budget());
  const ExecutionBindings bindings{{multi_result::binding(root, "a", 7)}};
  auto result = execution.execute(compiled.value().plan, bindings);
  if (!result.ok())
    std::cerr << result.status().message << "\n";
  PS_CHECK(result.ok());
  PS_CHECK(multi_result::number(result.value().results.at("pass")) == 7);
  PS_CHECK(multi_result::number(result.value().results.at("constant")) == 29);
  PS_CHECK(unwanted_calls->load() == 0);
  execution.clear_result_cache();
  auto frozen = execution
                    .freeze(compiled.value().plan,
                            {{multi_result::binding(root, "a", 17)}})
                    .take_value();
  auto independent = execution
                         .freeze(compiled.value().plan,
                                 {{multi_result::binding(root, "a", 23)}})
                         .take_value();
  const DemandQuery only_constant{
      {"constant", Footprint::all({1}).take_value()}};
  const auto before = (*projected_calls)[1].load();
  auto first = execution.execute_fragments(frozen, only_constant);
  auto cached = execution.execute_fragments(independent, only_constant);
  PS_CHECK(first.ok() && cached.ok() &&
           cached.value().diagnostics.cache_hits == 1);
  PS_CHECK((*projected_calls)[1] == before + 1 && unwanted_calls->load() == 0);
  PS_CHECK(number(cached.value(), "constant") == 29);
  return 0;
}

int projected_shapes_and_permutation() {
  auto registry = std::make_shared<OperationRegistry>();
  auto counts = std::make_shared<Counts>();
  OperationDefinition op;
  op.key = "test.projected_shapes";
  op.traits.input_count = 2;
  op.traits.input_schema.resize(2);
  for (auto& input : op.traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.tensor_key = "number";
  }
  op.traits.outputs.resize(2);
  const auto left_schema = multi_result::schema(ElementType::Float64, {2});
  const auto right_schema = multi_result::schema(ElementType::Int64, {3});
  for (std::uint32_t i = 0; i < 2; ++i) {
    op.traits.outputs[i] = multi_result::output(i ? "right" : "left",
                                                i ? right_schema : left_schema);
    op.traits.outputs[i].input_indices = std::vector<std::uint32_t>{i};
  }
  op.start_result = [counts](const ResultProgramQuery& query,
                             const BufferAllocator& allocator) {
    ++(*counts)[query.output_index];
    return ResultContinuation::make<multi_result::Program>(
        allocator, query.output_index, 0.0, true);
  };
  multi_result::check(registry->register_operation(op));
  ResultProgramMetadata metadata;
  metadata.inputs.resize(2);
  metadata.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(left_schema);
  metadata.inputs[1].result_schema =
      std::make_shared<const SchemaTemplate>(right_schema);
  const std::map<std::string, ParameterValue> parameters;
  ResourceBudget direct_root;
  auto allocator = direct_root.allocator();

  ResultObjectInputs objects;
  ResourceVector<ResultIoReply> io;
  for (unsigned output = 0; output < 2; ++output) {
    metadata.output.result_schema = std::make_shared<const SchemaTemplate>(
        output ? right_schema : left_schema);
    ResultProgramQuery query(metadata, parameters);
    query.output_index = output;
    query.semantic_key = op.key;
    query.tensor_outputs =
        Footprint::from_regions({2U + output}, {Region({{1U + output, 1}})})
            .take_value();
    auto started = registry->start_result(op.key, query, allocator);
    PS_CHECK(started.ok());
    auto continuation = started.take_value();
    ResultProgramPhase phase{
        query,
        objects,
        io,
        allocator,
        direct_root,
        [&](std::uint64_t work) { return direct_root.consume({work}); },
        std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok)};
    auto polled = continuation.poll(phase);
    PS_CHECK(polled.ok() &&
             std::holds_alternative<ResultProgramNeed>(polled.value()));
    const auto& need = std::get<ResultProgramNeed>(polled.value());
    PS_CHECK(need.tensors.size() == 1 && need.tensors[0].input == output);
    PS_CHECK(need.tensors[0].slot == 0 && need.tensors[0].roles == 1);
    PS_CHECK(need.tensors[0].samples == *query.tensor_outputs);
  }
  ResultProgramQuery bad_query(metadata, parameters);
  bad_query.semantic_key = op.key;
  bad_query.output_index = 2;
  const auto starts = (*counts)[0].load() + (*counts)[1].load();
  PS_CHECK(registry->start_result(op.key, bad_query, allocator).status().code ==
           ErrorCode::InvalidArgument);
  bad_query.output_index = 1;
  metadata.output.result_schema = metadata.inputs[0].result_schema;
  PS_CHECK(registry->start_result(op.key, bad_query, allocator).status().code ==
           ErrorCode::TypeMismatch);
  PS_CHECK((*counts)[0] + (*counts)[1] == starts);
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "a", left_schema),
                     multi_result::declaration(2, "b", right_schema)};
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
  const auto root = multi_result::take(execution.resource_budget());
  const auto a = multi_result::binding(root, "a", 7, left_schema);
  const auto b = multi_result::binding(root, "b", 11, right_schema);
  auto result = execution.execute(compiled.value().plan, {{b, a}});
  PS_CHECK(result.ok());
  const auto& left = result.value().results.at("left");
  const auto& right = result.value().results.at("right");
  PS_CHECK(left.schema().tensors[0].sample_shape() ==
           std::vector<std::uint64_t>{2});
  PS_CHECK(right.schema().tensors[0].sample_shape() ==
           std::vector<std::uint64_t>{3});
  PS_CHECK(multi_result::number(left, {1}) == 7);
  std::int64_t integer = 0;
  PS_CHECK(
      right.read_tensor(right.descriptor().value(), 0, {2}, &integer, 8).ok() &&
      integer == 11);
  PS_CHECK(
      !right.read_tensor(right.descriptor().value(), 0, {1}, &integer, 8).ok());
  document.outputs = {{"left", 1, "left"}};
  GraphContext single(document);
  const auto right_before = (*counts)[1].load();
  auto selected = Compiler(registry).compile(single).take_value().plan;
  PS_CHECK(execution.execute(selected, {{b, a}}).ok());
  PS_CHECK((*counts)[1] == right_before);
  return 0;
}

int projected_result_programs() {
  for (bool managed : {false, true}) {
    for (unsigned outputs : {1U, 2U}) {
      auto registry = std::make_shared<OperationRegistry>();
      unsigned unwanted_calls = 0, calls = 0;
      OperationDefinition unwanted;
      unwanted.key = "test.result_unwanted";
      unwanted.traits.cacheable = false;
      unwanted.traits.outputs[0] = multi_result::output("value");
      unwanted.start_result = [&](const ResultProgramQuery&,
                                  const BufferAllocator&) {
        ++unwanted_calls;
        return Result<ResultContinuation>(Status{
            ErrorCode::OperationFailed, "unselected Result input evaluated"});
      };
      PS_CHECK(registry->register_operation(std::move(unwanted)).ok());
      OperationDefinition projected;
      projected.key = "test.result_projected";
      projected.traits.workspace_bytes = 8;
      projected.traits.input_count = 2;
      projected.traits.input_schema.resize(2);
      for (auto& input : projected.traits.input_schema) {
        input.kind = OperationPortKind::Result;
        input.tensor_key = "number";
      }
      projected.traits.cacheable = false;
      projected.traits.outputs.resize(outputs);
      for (unsigned i = 0; i < outputs; ++i) {
        auto& output = projected.traits.outputs[i];
        output = multi_result::output(i ? "constant" : "pass");
        output.region_rule = OperationRegionRule::Whole;
        output.input_indices =
            i ? std::vector<std::uint32_t>{} : std::vector<std::uint32_t>{0};
      }
      projected.start_result = [&](const ResultProgramQuery& query,
                                   const BufferAllocator& allocator) {
        ++calls;
        if (query.inputs.size() != 2 || !query.inputs[0].result_schema ||
            !query.inputs[1].result_schema ||
            query.inputs[0].result_schema->tensors[0].key != "number" ||
            query.inputs[1].result_schema->tensors[0].key != "number")
          return Result<ResultContinuation>(
              Status{ErrorCode::OperationFailed,
                     "projected static metadata mismatch"});
        return ResultContinuation::make<multi_result::Program>(
            allocator, query.output_index ? -1 : 0, 29.0);
      };
      PS_CHECK(registry->register_operation(std::move(projected)).ok());
      PS_CHECK(registry->freeze().ok());
      WorkflowDocument document;
      document.inputs = {multi_result::declaration(1, "source")};
      document.nodes = {
          {1, "test.result_unwanted", {}, {}},
          {2,
           "test.result_projected",
           {WorkflowInputReference{1}, WorkflowNodeOutput{1, "value"}},
           {}}};
      document.outputs = {{"pass", 2, "pass"}};
      if (outputs == 2)
        document.outputs.push_back({"constant", 2, "constant"});
      GraphContext graph(document);
      auto compiled = Compiler(registry).compile(graph);
      PS_CHECK(compiled.ok());
      ExecutionContextConfig config;
      config.cpu_workers = 1;
      if (managed)
        config.managed_resources = ResourceLimits{};
      ExecutionContext context(registry, config);
      const auto root = multi_result::take(context.resource_budget());
      auto result = context.execute(
          compiled.value().plan, {{multi_result::binding(root, "source", 7)}});
      if (!result.ok())
        std::cerr << result.status().message
                  << " code=" << static_cast<unsigned>(result.status().code)
                  << " reason=" << static_cast<unsigned>(result.status().reason)
                  << " calls=" << calls << " unwanted=" << unwanted_calls
                  << '\n';
      PS_CHECK(result.ok() && calls == outputs && !unwanted_calls);
      PS_CHECK(multi_result::number(result.value().results.at("pass")) == 7);
      if (outputs == 2)
        PS_CHECK(multi_result::number(result.value().results.at("constant")) ==
                 29);
    }
  }
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
    PS_CHECK(multi_result::number(result.value().results.at("first")) == 10);
    PS_CHECK(multi_result::number(result.value().results.at("second")) == 11);
    PS_CHECK(result.value().diagnostics.selected_backends.size() == 2);
  }
  return 0;
}
}  // namespace

int main() {
  PS_CHECK(projected_result_programs() == 0);
  PS_CHECK(staged_outputs() == 0);
  PS_CHECK(projected_shapes_and_permutation() == 0);
  PS_CHECK(synchronous_outputs() == 0);
  PS_CHECK(projected_sync_inputs(false) == 0);
  PS_CHECK(projected_sync_inputs(true) == 0);
  return 0;
}
