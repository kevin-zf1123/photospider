#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../examples/numeric_workflow/result_fixture.hpp"
#include "execution/dependency_records.hpp"
#include "execution/memory_budget.hpp"
#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Poll = Result<ResultProgramPoll>;
Footprint point(std::uint64_t at, std::uint64_t size = 16) {
  return multi_result::take(
      Footprint::from_regions({size}, {Region({{at, 1}})}));
}
template <class T>
Value values(ElementType type, const std::vector<T>& samples) {
  auto writer = multi_result::take(MutableValue::allocate(
      {type, {samples.size()}}, Region::whole({samples.size()}),
      BufferAllocator{}));
  std::memcpy(writer.data(), samples.data(), writer.size());
  return multi_result::take(std::move(writer).publish());
}
SchemaTemplate schema(ElementType type = ElementType::Float64,
                      std::uint64_t size = 16) {
  auto result = multi_result::schema(type, {size});
  result.id = "manual.lowpass.input";
  result.tensors[0].key = "data";
  return result;
}
ExecutionBinding source(const ResourceBudget& root, const std::string& name,
                        const Value& backing) {
  auto builder = multi_result::take(ResultBuilder::start(
      root, numeric_result_fixture::source_schema(backing), "test.source"));
  multi_result::check(builder.bind_descriptor_relation(
      multi_result::take(ResultRelation::cartesian(root, 1, {}))));
  multi_result::check(builder.publish_tensor(
      0, backing.region(), backing.layout(),
      multi_result::take(root.reference(backing.storage())),
      multi_result::take(ResultRelation::cartesian(
          root,
          multi_result::take(
              builder.reference().schema().tensors[0].sample_count()),
          {})),
      {true, true, true, true}));
  return {name, multi_result::take(builder.seal())};
}
ResultBuilder builder(const ResultProgramPhase& phase) {
  auto result = multi_result::take(ResultBuilder::start(
      phase.resources, *phase.query.output.result_schema,
      phase.query.semantic_key, {},
      phase.association ? std::vector<std::uint64_t>(phase.association->begin(),
                                                     phase.association->end())
                        : std::vector<std::uint64_t>{}));
  multi_result::check(result.bind_descriptor_relation(
      multi_result::take(ResultRelation::cartesian(phase.resources, 1, {}))));
  return result;
}
Poll number(const ResultProgramPhase& phase, double value = 7,
            ResultRelation relation = {}) {
  const auto shape =
      phase.query.output.result_schema->tensors[0].sample_shape();
  const auto outputs = phase.query.tensor_outputs.value_or(
      multi_result::take(Footprint::all(shape)));
  auto output = builder(phase);
  if (!relation.valid())
    relation = multi_result::take(
        ResultRelation::cartesian(phase.resources, shape[0], {}));
  for (const auto& box : outputs.boxes()) {
    auto bytes = multi_result::take(
        phase.allocator.allocate(multi_result::take(box.element_count()) * 8));
    for (std::size_t i = 0; i < bytes.size(); i += 8)
      std::memcpy(bytes.data() + i, &value, 8);
    multi_result::check(output.publish_tensor(
        0, box, {0, {8}, {box.dimensions()[0].offset}},
        std::move(bytes).freeze(), relation, {true, true, true, true}));
  }
  return Poll(ResultPublication{multi_result::take(output.seal()), true});
}
struct Counts {
  std::atomic<int> starts{0}, destroyed{0}, polls{0};
};
struct ProbeState {
  std::function<Poll(const ResultProgramPhase&)> callback;
  std::shared_ptr<Counts> counts;
  ProbeState(std::function<Poll(const ResultProgramPhase&)> callback,
             std::shared_ptr<Counts> counts)
      : callback(std::move(callback)), counts(std::move(counts)) {
    ++this->counts->starts;
  }
  ~ProbeState() noexcept { ++counts->destroyed; }
  Poll poll(const ResultProgramPhase& phase) {
    ++counts->polls;
    return callback(phase);
  }
};
OperationTraits staged_traits(unsigned inputs, std::uint64_t state,
                              SchemaTemplate output_schema = schema()) {
  OperationTraits traits;
  traits.input_count = inputs;
  traits.input_schema.resize(inputs);
  for (auto& port : traits.input_schema) {
    port.kind = OperationPortKind::Result;
    port.rank = 1;
    port.element_type = static_cast<std::uint32_t>(ElementType::Float64);
  }
  traits.outputs[0] = multi_result::output("value", output_schema);
  traits.outputs[0].continuation_bytes = state;
  traits.outputs[0].maximum_dependency_stages = 32;
  traits.workspace_bytes = 4096;
  return traits;
}
OperationDefinition probe_definition(
    std::shared_ptr<Counts> counts,
    std::function<Poll(const ResultProgramPhase&)> callback =
        [](const ResultProgramPhase& phase) { return number(phase); }) {
  OperationDefinition operation;
  operation.key = "probe";
  operation.traits = staged_traits(1, sizeof(ProbeState));
  operation.start_result = [counts, callback](
                               const ResultProgramQuery&,
                               const BufferAllocator& allocator) {
    return ResultContinuation::make<ProbeState>(allocator, callback, counts);
  };
  return operation;
}
WorkflowDocument probe_document(const std::string& operation = "probe") {
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "x", schema())};
  document.nodes = {{1, operation, {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"result", 1, "value"}};
  return document;
}
Result<ExecutionResult> execute_probe(OperationDefinition operation,
                                      ExecutionOptions options = {}) {
  auto registry = std::make_shared<OperationRegistry>();
  multi_result::check(registry->register_operation(std::move(operation)));
  multi_result::check(registry->freeze());
  GraphContext graph(probe_document());
  auto plan = multi_result::take(Compiler(registry).compile(graph)).plan;
  plan = multi_result::take(plan.tile_plan("result", Region({{0, 1}})));
  ExecutionContext context(registry, {1, false, 8, 65536});
  const auto root = multi_result::take(context.resource_budget());
  return context.execute(
      plan,
      {{source(root, "x",
               values<double>(ElementType::Float64, std::vector<double>(16)))}},
      {}, options);
}
struct PointerState {
  std::shared_ptr<Counts> counts;
  ResourceVector<ResultRelation> controls;
  std::uint64_t next = 0;
  unsigned stage = 0;
  explicit PointerState(std::shared_ptr<Counts> counts)
      : counts(std::move(counts)) {
    ++this->counts->starts;
  }
  ~PointerState() noexcept { ++counts->destroyed; }
  Poll poll(const ResultProgramPhase& phase) {
    ++counts->polls;
    const auto shape =
        phase.query.output.result_schema->tensors[0].sample_shape();
    const auto outputs = phase.query.tensor_outputs.value_or(
        multi_result::take(Footprint::all(shape)));
    if (outputs.empty())
      return number(phase);
    const auto output = outputs.boxes()[0].dimensions()[0].offset;
    const auto request = [&](unsigned port, unsigned role, std::uint64_t at) {
      return Poll(ResultProgramNeed{{}, {}, {{port, 0, point(at), role}}});
    };
    if (stage == 0) {
      controls = ResourceVector<ResultRelation>(
          ResourceAllocator<ResultRelation>(phase.resources));
      next = output;
      stage = 1;
      return request(0, 2, next);
    }
    if (stage == 1) {
      std::int64_t pointer = 0;
      auto status = phase.read_tensor(0, 0, {next}, &pointer, 8);
      if (!status.ok())
        return Poll(status);
      controls.push_back(multi_result::take(ResultRelation::cartesian(
          phase.resources, shape[0],
          {0, 2, next, 1, ResultSupportTarget::Tensor, 0})));
      if (pointer >= 0) {
        next = pointer;
        return request(0, 2, next);
      }
      next = static_cast<std::uint64_t>(-(pointer + 1));
      stage = 2;
      return request(1, 1, next);
    }
    double value = 0;
    auto status = phase.read_tensor(1, 0, {next}, &value, 8);
    if (!status.ok())
      return Poll(status);
    std::vector<ResultRelation> support(controls.begin(), controls.end());
    support.push_back(multi_result::take(ResultRelation::cartesian(
        phase.resources, shape[0],
        {1, 1, next, 1, ResultSupportTarget::Tensor, 0})));
    return number(
        phase, value,
        multi_result::take(ResultRelation::unite(phase.resources, support)));
  }
};
OperationDefinition pointer_definition(std::shared_ptr<Counts> counts) {
  OperationDefinition operation;
  operation.key = "pointer";
  operation.traits = staged_traits(2, sizeof(PointerState));
  operation.traits.input_schema[0].element_type =
      static_cast<std::uint32_t>(ElementType::Int64);
  operation.start_result = [counts](const ResultProgramQuery&,
                                    const BufferAllocator& allocator) {
    return ResultContinuation::make<PointerState>(allocator, counts);
  };
  return operation;
}
struct TerminalState {
  bool supplied = false;
  Poll poll(const ResultProgramPhase& phase) {
    auto outputs = phase.query.tensor_outputs.value_or(
        multi_result::take(Footprint::all({16})));
    if (!supplied && !outputs.empty()) {
      supplied = true;
      return Poll(ResultProgramNeed{{}, {}, {{0, 0, outputs, 1}}});
    }
    auto output = builder(phase);
    for (const auto& box : outputs.boxes()) {
      auto bytes = multi_result::take(phase.allocator.allocate(
          multi_result::take(box.element_count()) * 8));
      for (std::uint64_t i = 0; i < box.dimensions()[0].extent; ++i) {
        const auto at = box.dimensions()[0].offset + i;
        double value = 0;
        multi_result::check(phase.read_tensor(0, 0, {at}, &value, 8));
        value += multi_result::take(outputs.element_count());
        std::memcpy(bytes.data() + i * 8, &value, 8);
      }
      auto relation = multi_result::take(ResultRelation::mapped(
          phase.resources, {16}, box, {16}, {{0, 0, 1, 1}},
          {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
      multi_result::check(output.publish_tensor(
          0, box, {0, {8}, {box.dimensions()[0].offset}},
          std::move(bytes).freeze(), relation, {true, true, true, true}));
    }
    return Poll(ResultPublication{multi_result::take(output.seal()), true});
  }
};
int progressive() {
  auto counts = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  multi_result::check(registry->register_operation(pointer_definition(counts)));
  multi_result::check(registry->freeze());
  WorkflowDocument document;
  document.inputs = {
      multi_result::declaration(1, "control", schema(ElementType::Int64)),
      multi_result::declaration(2, "payload", schema())};
  document.nodes = {{1,
                     "pointer",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"result", 1, "value"}};
  GraphContext graph(document);
  auto plan = multi_result::take(Compiler(registry).compile(graph)).plan;
  plan = multi_result::take(plan.tile_plan("result", Region({{0, 1}})));
  ExecutionContext context(registry, {1, false, 8, 65536});
  const auto root = multi_result::take(context.resource_budget());
  std::vector<std::int64_t> pointers(16, -1);
  pointers[0] = 1;
  pointers[1] = 3;
  pointers[3] = -16;
  std::vector<double> samples(16);
  for (unsigned i = 0; i < 16; ++i)
    samples[i] = i * 2;
  ExecutionBindings bindings{
      {source(root, "control", values(ElementType::Int64, pointers)),
       source(root, "payload", values(ElementType::Float64, samples))}};
  auto result = context.execute(plan, bindings);
  PS_CHECK(result.ok());
  double output = 0;
  PS_CHECK(numeric_result_fixture::read(result.value().results.at("result"),
                                        {0}, &output, 8)
               .ok() &&
           output == 30);
  PS_CHECK(counts->polls == 5 && counts->starts == 1 && counts->destroyed == 1);
  auto observations =
      multi_result::take(result.value().dependencies.source_observations());
  auto controls = multi_result::take(Footprint::none({16}));
  for (const auto& observation : observations)
    if (observation.input == "control" && observation.roles == 2)
      controls = multi_result::take(controls.unite(observation.samples));
  PS_CHECK(controls.element_count().value() == 3 && controls.contains({0}) &&
           controls.contains({1}) && controls.contains({3}));
  PS_CHECK(result.value()
               .dependencies.potential_dirty("control", point(3), 2)
               .value()
               .at("result") == point(0));
  PS_CHECK(result.value()
               .dependencies.potential_dirty("control", point(2), 2)
               .value()
               .at("result")
               .empty());
  auto frozen = multi_result::take(context.freeze(plan, bindings));
  pointers[3] = 0;
  bindings.inputs[0] =
      source(root, "control", values(ElementType::Int64, pointers));
  auto cycle = context.execute(plan, bindings);
  PS_CHECK(cycle.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(counts->starts == counts->destroyed);
  PS_CHECK(context.execute(frozen).ok());
  auto demand = multi_result::take(context.open_demand(plan, bindings));
  const auto before = counts->starts.load();
  const auto polls_before = counts->polls.load();
  auto empty =
      demand.request({{"result", multi_result::take(Footprint::none({16}))}});
  PS_CHECK(empty.ok() && counts->starts == before + 1 &&
           counts->polls == polls_before + 1 &&
           counts->starts == counts->destroyed &&
           empty.value().dependencies.source_observations().value().empty());
  const auto& empty_object = empty.value().results.at("result");
  PS_CHECK(empty_object.descriptor().value().tensor_coverage(0).empty());
  PS_CHECK(!numeric_result_fixture::read(empty_object, {0}, &output, 8).ok());
  return 0;
}
int terminal_and_graph() {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition terminal;
  terminal.key = "terminal";
  terminal.traits = staged_traits(1, sizeof(TerminalState));
  terminal.traits.outputs[0].observation_kind = ObservationKind::RequestRecord;
  terminal.start_result = [](const ResultProgramQuery&,
                             const BufferAllocator& allocator) {
    return ResultContinuation::make<TerminalState>(allocator);
  };
  multi_result::check(registry->register_operation(terminal));
  terminal.key = "atomic";
  terminal.traits.outputs[0].observation_kind = ObservationKind::Atomic;
  multi_result::check(registry->register_operation(terminal));
  multi_result::check(registry->freeze());
  auto document = probe_document("terminal");
  GraphContext graph(document);
  auto plan = multi_result::take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, {1, false, 8, 65536});
  auto binding =
      source(multi_result::take(context.resource_budget()), "x",
             values<double>(ElementType::Float64, std::vector<double>(16)));
  auto demand = multi_result::take(context.open_demand(plan, {{binding}}));
  const auto wide = multi_result::take(point(0).unite(point(1)));
  auto joint = demand.request({{"result", wide}});
  PS_CHECK(joint.ok());
  double value = 0;
  PS_CHECK(numeric_result_fixture::read(joint.value().results.at("result"), {0},
                                        &value, 8)
               .ok() &&
           value == 2);
  PS_CHECK(!joint.value().dependencies.restrict({{"result", point(0)}}).ok());
  auto singleton = demand.request({{"result", point(0)}});
  PS_CHECK(singleton.ok() &&
           numeric_result_fixture::read(singleton.value().results.at("result"),
                                        {0}, &value, 8)
               .ok() &&
           value == 1);
  document.nodes.push_back({2, "atomic", {WorkflowNodeOutput{1, "value"}}, {}});
  document.outputs = {{"result", 2, "value"}};
  GraphContext forbidden(document);
  PS_CHECK(Compiler(registry).compile(forbidden).status().code ==
           ErrorCode::InvalidArgument);
  document.nodes[1].inputs = {WorkflowInputReference{1}};
  document.outputs = {{"result", 2, "value"}, {"record", 1, "value"}};
  GraphContext allowed(document);
  auto compiled = Compiler(registry).compile(allowed);
  PS_CHECK(compiled.ok());
  PS_CHECK(!compiled.value().semantic.nodes()[0].outputs[0].effective_atomic &&
           compiled.value().semantic.nodes()[1].outputs[0].effective_atomic);
  PS_CHECK(compiled.value().plan.tile_plan("record", Region({{0, 2}})).ok());
  return 0;
}

int dependency_record_retirement() {
  using execution_internal::DependencyRecord;
  // Structural owners can outlive all pixel owners. Retiring a deep chain
  // must not recurse through the C++ stack or allocate during destruction.
  std::shared_ptr<const DependencyRecord> chain;
  std::weak_ptr<const DependencyRecord> leaf;
  for (unsigned i = 0; i < 20000; ++i) {
    auto record = std::shared_ptr<DependencyRecord>(new DependencyRecord(),
                                                    DependencyRecord::retire);
    record->step = i;
    if (chain)
      record->upstream.push_back(std::move(chain));
    chain = std::move(record);
    if (!i)
      leaf = chain;
  }
  PS_CHECK(!leaf.expired());
  chain.reset();
  PS_CHECK(leaf.expired());
  return 0;
}

int field_prefix_evidence() {
  auto registry = std::make_shared<OperationRegistry>();
  SchemaTemplate source_schema;
  source_schema.id = "test.prefix.source";
  ResultTensorSpec source_tensor;
  source_tensor.key = "samples";
  source_tensor.descriptor = {ElementType::Int64, {4}};
  source_schema.tensors.push_back(std::move(source_tensor));
  SchemaTemplate schema;
  schema.id = "test.prefix.evidence";
  schema.publication = PublishPolicy::StablePrefix;
  schema.fields = {
      {"rows", ElementType::Int64, {ResultExtentKind::RuntimeCount}, {}}};
  OperationDefinition op;
  op.key = "prefix";
  op.traits.input_count = 1;
  op.traits.input_schema.resize(1);
  op.traits.input_schema[0].kind = OperationPortKind::Result;
  op.traits.input_schema[0].result_schema_id = source_schema.id;
  op.traits.input_schema[0].result_schema_version = source_schema.version;
  auto& output = op.traits.outputs[0];
  output.continuation_bytes = 64;
  output.maximum_dependency_stages = 8;
  output.region_rule = OperationRegionRule::Dependency;
  output.result_schema = schema;
  output.output_schema.kind = OperationPortKind::Result;
  output.output_schema.result_schema_id = schema.id;
  output.output_schema.result_schema_version = schema.version;
  op.start_result = [](const ResultProgramQuery&, const BufferAllocator&) {
    return Result<ResultContinuation>(Status{ErrorCode::Internal, {}});
  };
  PS_CHECK(registry->register_operation(op).ok());
  op.key = "observe";
  op.traits.input_schema[0].kind = OperationPortKind::Result;
  op.traits.input_schema[0].result_schema_id = schema.id;
  op.traits.input_schema[0].result_schema_version = schema.version;
  PS_CHECK(registry->register_operation(op).ok() && registry->freeze().ok());
  WorkflowDocument doc;
  WorkflowInputDeclaration source;
  source.id = 1;
  source.name = "x";
  source.result_schema = std::make_shared<const SchemaTemplate>(source_schema);
  doc.inputs = {source};
  doc.nodes = {{1, "prefix", {WorkflowInputReference{1}}, {}},
               {2, "observe", {WorkflowNodeOutput{1, "value"}}, {}}};
  doc.outputs = {{"y", 2, "value"}};
  GraphContext graph(doc);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = context.resource_budget().take_value();
  ResourceAllocationScope scope(root);
  auto basis = ResultRelation::cartesian(
                   root, 1, {0, 8, 0, 0, ResultSupportTarget::Descriptor, 0})
                   .take_value();
  const auto witness = [&](bool future_placeholder) {
    std::vector<ResultRelationRow> rows;
    for (uint64_t i = 0; i < 4; ++i)
      rows.push_back({i,
                      {0, 1, future_placeholder && i ? 0 : i, 1,
                       ResultSupportTarget::Tensor, 0}});
    return ResultRelation::sample_rows(
               root, 4, rows.size(),
               [&](uint64_t i) {
                 return Result<ResultRelationRow>(rows.at(i));
               })
        .take_value();
  };
  const auto publish = [&](uint64_t count, const ResultRelation& relation) {
    auto builder =
        ResultBuilder::start(root, schema, "same-prefix").take_value();
    if (!builder.bind_descriptor_relation(basis).ok())
      throw std::runtime_error("prefix basis");
    if (count) {
      std::vector<uint8_t> bytes(count * 8);
      if (!builder.append(0, count, ByteView(bytes.data(), bytes.size())).ok())
        throw std::runtime_error("prefix append");
    }
    if (!builder.publish(0, count, relation, {true, true, true, true}).ok())
      throw std::runtime_error("prefix publish");
    return builder.reference().capture().take_value();
  };
  const auto complete = witness(false), placeholder = witness(true);
  for (auto counts :
       {std::vector<uint64_t>{3, 1, 0}, std::vector<uint64_t>{0, 1, 3}}) {
    execution_internal::DependencyRecords records(plan, "snapshot", {});
    for (auto count : counts) {
      const auto& relation = count == 3 ? complete : placeholder;
      auto result = publish(count, relation);
      PS_CHECK(records.bind_result(PlanStepInput{0}, result).ok());
      const auto samples = count ? Footprint::all({count}).take_value()
                                 : Footprint::none({1}).take_value();
      PS_CHECK(records
                   .append_relation(0, samples, relation, basis,
                                    ResultSupportTarget::Field)
                   .ok());
    }
    auto selected = ResultRelation::cartesian(
                        root, 1, {0, 1, 2, 1, ResultSupportTarget::Field, 0})
                        .take_value();
    const auto output = Footprint::all({1}).take_value();
    PS_CHECK(records
                 .append_relation(1, output, selected, {},
                                  ResultSupportTarget::Descriptor)
                 .ok());
    PS_CHECK(records.output("y", 1, output).ok());
    auto evidence = std::move(records).finish();
    PS_CHECK(evidence.source_support().value().at("x") == point(2, 4));
    PS_CHECK(evidence.potential_dirty("x", point(2, 4), 1).value().at("y") ==
             output);
    PS_CHECK(
        evidence.potential_dirty("x", point(0, 4), 1).value().at("y").empty());
  }
  {
    execution_internal::DependencyRecords records(plan, "attempt", {});
    auto old_prefix = publish(1, placeholder);
    PS_CHECK(records.bind_result(PlanStepInput{0}, old_prefix).ok());
    PS_CHECK(records
                 .append_relation(0, Footprint::all({1}).take_value(),
                                  placeholder, basis,
                                  ResultSupportTarget::Field)
                 .ok());
    auto first_row = ResultRelation::cartesian(
                         root, 1, {0, 1, 0, 1, ResultSupportTarget::Field, 0})
                         .take_value();
    const auto output = Footprint::all({1}).take_value();
    PS_CHECK(records
                 .append_relation(1, output, first_row, {},
                                  ResultSupportTarget::Descriptor)
                 .ok());
    PS_CHECK(records.output("y", 1, output).ok());
    const auto before_attempt = root.statistics().live;
    {
      auto trial = records.fork_result_attempt();
      PS_CHECK(trial.ok() && root.statistics().live[ResourceKind::Metadata] >
                                 before_attempt[ResourceKind::Metadata]);
    }
    PS_CHECK(root.statistics().live.values == before_attempt.values);
    {
      const auto available = root.available_capacity();
      const auto bytes = std::min(available[ResourceKind::Host],
                                  available[ResourceKind::Metadata]) -
                         ResourceBudget::lease_metadata_bytes();
      auto occupied =
          root.reserve(ResourceCapacity::host(bytes, bytes)).take_value();
      auto refused = records.fork_result_attempt();
      PS_CHECK(!refused.ok() &&
               refused.status().code == ErrorCode::ResourceExhausted);
    }
    PS_CHECK(root.statistics().live.values == before_attempt.values);
    auto saved = records.fork_result_attempt();
    PS_CHECK(saved.ok());
    auto expanded = publish(3, complete);
    PS_CHECK(records.bind_result(PlanStepInput{0}, expanded).ok());
    auto changed_basis =
        ResultRelation::cartesian(root, 1,
                                  {0, 2, 3, 1, ResultSupportTarget::Tensor, 0})
            .take_value();
    PS_CHECK(records
                 .append_relation(0, Footprint::all({3}).take_value(), complete,
                                  changed_basis, ResultSupportTarget::Field)
                 .ok());
    auto surviving_bundle = records.capture_bundle(0).take_value();
    auto speculative = records.snapshot().take_value();
    PS_CHECK(speculative.source_support().value().at("x") ==
             point(0, 4).unite(point(3, 4)).take_value());
    auto restored = saved.take_value();
    auto prior = restored->snapshot().take_value();
    PS_CHECK(prior.record_count() == 2 &&
             prior.source_support().value().at("x") == point(0, 4));
    PS_CHECK(
        prior.potential_dirty("x", point(3, 4), 2).value().at("y").empty());
    PS_CHECK(prior.potential_dirty("x", point(0, 4), 1).value().at("y") ==
             output);
    PS_CHECK(restored->import_bundle(*surviving_bundle, 0).ok());
    auto second = restored->snapshot().take_value();
    PS_CHECK(second.source_support().value().at("x") ==
             point(0, 4).unite(point(3, 4)).take_value());
  }
  {
    FootprintLimits tight;
    tight.maximum_boxes = 4;
    execution_internal::DependencyRecords records(plan, "bounded", tight);
    PS_CHECK(records
                 .append_relation(0, point(0, 4), complete, basis,
                                  ResultSupportTarget::Field)
                 .ok());
    PS_CHECK(records
                 .append_relation(0, point(2, 4), complete, basis,
                                  ResultSupportTarget::Field)
                 .code == ErrorCode::ResourceExhausted);
    PS_CHECK(records.source_observations().value().size() == 1 &&
             records.source_observations().value()[0].samples == point(0, 4));
  }

  return 0;
}

ResultRef block_state(const ResultProgramPhase& phase, double value,
                      bool foreign = false, bool bad_type = false) {
  auto root = foreign ? ResourceBudget{} : phase.resources;
  auto spec = schema(bad_type ? ElementType::Int64 : ElementType::Float64, 1);
  auto result =
      multi_result::take(ResultBuilder::start(root, spec, "test.block"));
  multi_result::check(result.bind_descriptor_relation(
      multi_result::take(ResultRelation::cartesian(root, 1, {}))));
  auto bytes = multi_result::take(root.allocator().allocate(8));
  std::memcpy(bytes.data(), &value, 8);
  multi_result::check(result.publish_tensor(
      0, Region::whole({1}), {0, {8}}, std::move(bytes).freeze(),
      multi_result::take(ResultRelation::cartesian(root, 1, {})),
      {true, true, true, true}));
  return multi_result::take(result.seal());
}
int block_services() {
  unsigned computations = 0;
  auto counts = std::make_shared<Counts>();
  auto operation =
      probe_definition(counts, [&](const ResultProgramPhase& phase) {
        auto incoming = block_state(phase, 1);
        auto result = phase.block(1, 0, 1, 1, incoming, [&] {
          ++computations;
          return Result<ResultRef>(block_state(phase, 2));
        });
        if (!result.ok())
          return number(phase);
        return number(phase, multi_result::number(result.value()));
      });
  auto registry = std::make_shared<OperationRegistry>();
  multi_result::check(registry->register_operation(operation));
  multi_result::check(registry->freeze());
  GraphContext graph(probe_document());
  auto plan = multi_result::take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, {1, false, 8, 65536, 8192});
  auto input =
      source(multi_result::take(context.resource_budget()), "x",
             values<double>(ElementType::Float64, std::vector<double>(16)));
  for (unsigned run = 0; run < 3; ++run) {
    auto selected =
        multi_result::take(plan.tile_plan("result", Region({{run, 1}})));
    auto executed = context.execute(selected, {{input}});
    PS_CHECK(executed.ok());
    double value = 0;
    PS_CHECK(numeric_result_fixture::read(executed.value().results.at("result"),
                                          {run}, &value, 8)
                 .ok() &&
             value == 2);
    PS_CHECK(computations == 1);
  }
  for (unsigned bad = 0; bad < 4; ++bad) {
    auto failing = probe_definition(
        std::make_shared<Counts>(), [bad](const ResultProgramPhase& phase) {
          auto incoming = block_state(phase, 1);
          auto result =
              phase.block(1, 0, 1, 1, incoming, [&]() -> Result<ResultRef> {
                if (bad == 0)
                  throw std::bad_alloc();
                if (bad == 1)
                  throw std::runtime_error("block host");
                return Result<ResultRef>(
                    block_state(phase, 2, bad == 2, bad == 3));
              });
          static_cast<void>(result);
          return number(phase);
        });
    auto failed = execute_probe(failing);
    PS_CHECK(failed.status().code == (bad == 0   ? ErrorCode::ResourceExhausted
                                      : bad == 1 ? ErrorCode::OperationFailed
                                                 : ErrorCode::InvalidArgument));
  }
  return 0;
}
int service_and_identity_regressions() {
  for (unsigned action = 0; action < 5; ++action) {
    auto counts = std::make_shared<Counts>();
    auto op =
        probe_definition(counts, [action](const ResultProgramPhase& phase) {
          if (action == 0)
            static_cast<void>(phase.allocator.allocate(4097));
          if (action == 1) {
            double value = 0;
            static_cast<void>(phase.read_tensor(0, 0, {0}, &value, 8));
          }
          if (action == 2)
            static_cast<void>(phase.consume_work(UINT64_MAX));
          if (action == 3)
            static_cast<void>(phase.checkpoint_before(1, 0));
          if (action == 4) {
            auto state = block_state(phase, 1);
            static_cast<void>(phase.checkpoint_publish(1, 0, state));
          }
          return number(phase);
        });
    if (action >= 3)
      op.traits.outputs[0].observation_kind = ObservationKind::RequestRecord;
    auto result = execute_probe(op);
    PS_CHECK(result.status().code == (action == 0 || action == 2
                                          ? ErrorCode::ResourceExhausted
                                          : ErrorCode::InvalidArgument));
    PS_CHECK(counts->starts == 1 && counts->destroyed == 1);
  }
  // Start exceptions cannot escape; cancellation takes precedence over them.
  for (unsigned action = 0; action < 4; ++action) {
    for (bool stopped : {false, true}) {
      auto counts = std::make_shared<Counts>();
      CancellationSource cancel;
      OperationRegistry registry;
      auto operation = probe_definition(counts);
      operation.start_result =
          [&](const ResultProgramQuery&,
              const BufferAllocator& allocator) -> Result<ResultContinuation> {
        auto state = ResultContinuation::make<ProbeState>(
            allocator,
            [](const ResultProgramPhase& phase) { return number(phase); },
            counts);
        if (stopped)
          cancel.cancel();
        if (action == 0)
          throw std::bad_alloc();
        if (action == 1)
          throw std::runtime_error("start failed");
        if (action == 2)
          throw 1;
        return Result<ResultContinuation>(ResultContinuation{});
      };
      multi_result::check(registry.register_operation(operation));
      ResultProgramMetadata metadata;
      metadata.inputs.resize(1);
      metadata.inputs[0].result_schema =
          std::make_shared<const SchemaTemplate>(schema());
      metadata.output.result_schema = metadata.inputs[0].result_schema;
      const std::map<std::string, ParameterValue> parameters;
      ResultProgramQuery query(metadata, parameters);
      query.semantic_key = "probe";
      query.tensor_outputs = point(0);
      query.cancellation = cancel.token();
      ResourceBudget root;
      auto started = registry.start_result("probe", query, root.allocator());
      PS_CHECK(started.status().code ==
               (stopped       ? ErrorCode::Cancelled
                : action == 0 ? ErrorCode::ResourceExhausted
                : action == 3 ? ErrorCode::InvalidArgument
                              : ErrorCode::OperationFailed));
      PS_CHECK(counts->starts == counts->destroyed);
    }
  }
  auto counts = std::make_shared<Counts>();
  auto bypass = probe_definition(counts);
  bypass.start_result = [counts](const ResultProgramQuery&,
                                 const BufferAllocator&) {
    return ResultContinuation::make<ProbeState>(
        BufferAllocator{},
        [](const ResultProgramPhase& phase) { return number(phase); }, counts);
  };
  auto rejected = execute_probe(bypass);
  PS_CHECK(rejected.status().code == ErrorCode::InvalidArgument);
  PS_CHECK(counts->starts == counts->destroyed);
  return 0;
}
struct DirectHost {
  ResourceBudget root;
  ResultProgramMetadata metadata;
  std::map<std::string, ParameterValue> parameters;
  ResultProgramQuery query;
  ResultObjectInputs inputs;
  ResourceVector<ResultIoReply> io;
  BufferAllocator allocator;
  std::shared_ptr<std::atomic<ErrorCode>> failure;
  ResultProgramPhase phase;
  DirectHost()
      : query(metadata, parameters),
        allocator(root.allocator().limited(4096)),
        failure(std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok)),
        phase{query,
              inputs,
              io,
              allocator,
              root,
              [this](std::uint64_t count) { return root.consume({count}); },
              failure} {
    metadata.inputs.resize(1);
    metadata.inputs[0].result_schema =
        std::make_shared<const SchemaTemplate>(schema());
    metadata.output.result_schema = metadata.inputs[0].result_schema;
    query.tensor_outputs = point(0);
    query.semantic_key = "probe";
    query.snapshot_identity = "bundle";
  }
};
int concurrent_and_reentrant() {
  auto counts = std::make_shared<Counts>();
  ResultContinuation* active = nullptr;
  auto definition =
      probe_definition(counts, [&](const ResultProgramPhase& phase) {
        const auto before = phase.resources.statistics();
        auto rejected = active->poll(phase);
        const auto after = phase.resources.statistics();
        if (rejected.status().code != ErrorCode::InvalidArgument ||
            rejected.status().reason != FailureReason::None ||
            rejected.status().detail.origin != FailureOrigin::Protocol ||
            rejected.status().detail.scope != FailureScope::Group ||
            phase.failure->load() != ErrorCode::Ok ||
            before.live.values != after.live.values ||
            before.issued.work != after.issued.work)
          return Poll(Status{ErrorCode::Internal, "reentry was not rejected"});
        return number(phase);
      });
  OperationRegistry registry;
  multi_result::check(registry.register_operation(definition));
  DirectHost host;
  auto continuation = multi_result::take(
      registry.start_result("probe", host.query, host.allocator));
  unsigned observations = 0;
  host.phase.failure_observer = [&](const Status&) { ++observations; };
  active = &continuation;
  auto first = continuation.poll(host.phase);
  PS_CHECK(first.ok() && observations == 0 && counts->polls == 1);
  continuation = {};
  PS_CHECK(counts->starts == counts->destroyed);
  std::promise<void> entered, release;
  auto entrance = entered.get_future();
  auto gate = release.get_future().share();
  definition = probe_definition(counts, [&](const ResultProgramPhase& phase) {
    entered.set_value();
    gate.wait();
    return number(phase);
  });
  OperationRegistry blocking;
  multi_result::check(blocking.register_operation(definition));
  DirectHost another;
  CancellationSource cancel;
  another.query.cancellation = cancel.token();
  auto running = multi_result::take(
      blocking.start_result("probe", another.query, another.allocator));
  auto future = std::async(std::launch::async,
                           [&] { return running.poll(another.phase); });
  entrance.wait();
  const auto before = another.root.statistics();
  auto rejected = running.poll(another.phase);
  const auto after = another.root.statistics();
  PS_CHECK(rejected.status().code == ErrorCode::InvalidArgument &&
           rejected.status().detail.origin == FailureOrigin::Protocol &&
           rejected.status().detail.scope == FailureScope::Group &&
           another.failure->load() == ErrorCode::Ok);
  PS_CHECK(before.live.values == after.live.values &&
           before.issued.work == after.issued.work);
  cancel.cancel();
  PS_CHECK(counts->starts == counts->destroyed + 1);
  release.set_value();
  auto completed = future.get();
  PS_CHECK(completed.ok());
  PS_CHECK(running.poll(another.phase).status().code == ErrorCode::Cancelled);
  running = {};
  PS_CHECK(counts->starts == counts->destroyed);
  // An accepted exception releases the guard through the complete fence.
  auto throws = probe_definition(counts, [](const ResultProgramPhase&) -> Poll {
    throw std::runtime_error("guard exception");
  });
  OperationRegistry throwing;
  multi_result::check(throwing.register_operation(throws));
  DirectHost exception_host;
  auto original = multi_result::take(throwing.start_result(
      "probe", exception_host.query, exception_host.allocator));
  auto moved = std::move(original);
  PS_CHECK(!original.valid() && moved.valid());
  PS_CHECK(original.poll(exception_host.phase).status().code ==
           ErrorCode::Stale);
  unsigned fenced_observations = 0;
  bool fenced_reentry = true;
  exception_host.phase.failure_observer = [&](const Status& first_failure) {
    ++fenced_observations;
    const auto nested = moved.poll(exception_host.phase);
    fenced_reentry =
        fenced_reentry && nested.status().code == ErrorCode::InvalidArgument &&
        nested.status().detail.origin == FailureOrigin::Protocol &&
        nested.status().detail.scope == FailureScope::Group &&
        first_failure.code == ErrorCode::OperationFailed &&
        first_failure.message == "guard exception" &&
        exception_host.failure->load() == ErrorCode::OperationFailed;
  };
  for (unsigned repeat = 0; repeat < 2; ++repeat) {
    exception_host.failure->store(ErrorCode::Ok);
    auto failure = moved.poll(exception_host.phase);
    PS_CHECK(failure.status().code == ErrorCode::OperationFailed &&
             failure.status().reason == FailureReason::HostException &&
             failure.status().message == "guard exception");
  }
  PS_CHECK(fenced_reentry && fenced_observations == 2);
  moved = {};
  PS_CHECK(counts->starts == counts->destroyed);
  return 0;
}
int allocator_lifetime() {
  auto budget = std::make_shared<execution_internal::MemoryBudget>(64);
  auto reservation = multi_result::take(budget->reserve(64));
  auto allocator = reservation->allocator().limited(16);
  auto a = multi_result::take(allocator.allocate(8));
  auto b = multi_result::take(allocator.allocate(8));
  PS_CHECK(!allocator.allocate(1).ok() && budget->live() == 16);
  reservation->seal();
  PS_CHECK(budget->live() == 16);
  a = {};
  b = {};
  PS_CHECK(budget->live() == 0 && budget->available() == 64);
  auto counts = std::make_shared<Counts>();
  OperationRegistry registry;
  multi_result::check(registry.register_operation(probe_definition(counts)));
  DirectHost host;
  auto continuation = multi_result::take(
      registry.start_result("probe", host.query, host.allocator));
  PS_CHECK(host.root.statistics().live[ResourceKind::Payload] ==
           sizeof(ProbeState));
  continuation = {};
  PS_CHECK(host.root.statistics().live[ResourceKind::Payload] == 0);
  PS_CHECK(counts->starts == counts->destroyed);
  auto small = host.root.allocator().limited(1);
  PS_CHECK(registry.start_result("probe", host.query, small).status().code ==
           ErrorCode::ResourceExhausted);
  return 0;
}
int root_work_resume() {
  for (const std::uint64_t limit : {10000U, 30000U}) {
    auto counts = std::make_shared<Counts>();
    std::uint64_t issued = 0;
    auto registry = std::make_shared<OperationRegistry>();
    auto upstream =
        probe_definition(counts, [&](const ResultProgramPhase& phase) {
          auto status = phase.consume_work(6000);
          if (!status.ok())
            return Poll(status);
          issued += 6000;
          return number(phase);
        });
    upstream.key = "fuel_upstream";
    multi_result::check(registry->register_operation(upstream));
    auto parent = probe_definition(counts);
    parent.key = "fuel_parent";
    parent.start_result = [counts, &issued](const ResultProgramQuery&,
                                            const BufferAllocator& allocator) {
      auto callback =
          [&issued, supplied = false](const ResultProgramPhase& phase) mutable {
            if (!supplied) {
              supplied = true;
              return Poll(ResultProgramNeed{{}, {}, {{0, 0, point(0), 1}}});
            }
            auto status = phase.consume_work(6000);
            if (!status.ok())
              return Poll(status);
            issued += 6000;
            return number(phase);
          };
      return ResultContinuation::make<ProbeState>(allocator, callback, counts);
    };
    multi_result::check(registry->register_operation(parent));
    multi_result::check(registry->freeze());
    auto document = probe_document("fuel_upstream");
    document.nodes.push_back(
        {2, "fuel_parent", {WorkflowNodeOutput{1, "value"}}, {}});
    document.outputs = {{"result", 2, "value"}};
    GraphContext graph(document);
    auto plan = multi_result::take(Compiler(registry).compile(graph)).plan;
    plan = multi_result::take(plan.tile_plan("result", Region({{0, 1}})));
    ExecutionContextConfig config{1, false, 8, 65536};
    config.managed_resources = ResourceLimits{};
    config.managed_resources->maximum_work = limit;
    ExecutionContext context(registry, config);
    auto input =
        source(multi_result::take(context.resource_budget()), "x",
               values<double>(ElementType::Float64, std::vector<double>(16)));
    ExecutionOptions options;
    options.enable_joint = false;
    options.maximum_dependency_work = limit;
    auto result = context.execute(plan, {{input}}, {}, options);
    PS_CHECK(issued == (limit == 10000 ? 6000 : 12000));
    PS_CHECK(limit == 10000
                 ? result.status().code == ErrorCode::ResourceExhausted
                 : result.ok());
    PS_CHECK(counts->starts == counts->destroyed);
  }
  return 0;
}
struct SiblingState {
  bool requested = false;
  Poll poll(const ResultProgramPhase& phase) {
    if (!requested) {
      requested = true;
      return Poll(ResultProgramNeed{
          {},
          {},
          {{0, 0, point(0, 131072), 1}, {1, 0, point(0, 131072), 1}}});
    }
    double sum = 0;
    for (unsigned port = 0; port < 2; ++port) {
      double value = 0;
      auto status = phase.read_tensor(port, 0, {0}, &value, 8);
      if (!status.ok())
        return Poll(status);
      sum += value;
    }
    auto support = multi_result::take(ResultRelation::unite(
        phase.resources,
        {multi_result::take(ResultRelation::cartesian(
             phase.resources, 1, {0, 1, 0, 1, ResultSupportTarget::Tensor, 0})),
         multi_result::take(ResultRelation::cartesian(
             phase.resources, 1,
             {1, 1, 0, 1, ResultSupportTarget::Tensor, 0}))}));
    return number(phase, sum, support);
  }
};
struct LargeSource {
  unsigned id;
  unsigned* calls;
  std::weak_ptr<const CpuStorage>* retained;
  LargeSource(unsigned id, unsigned* calls,
              std::weak_ptr<const CpuStorage>* retained)
      : id(id), calls(calls), retained(retained) {}
  Poll poll(const ResultProgramPhase& phase) {
    constexpr std::uint64_t mib = 1024 * 1024;
    ++calls[id];
    auto scratch = phase.allocator.allocate(3 * mib);
    if (!scratch.ok())
      return Poll(scratch.status());
    auto bytes = phase.allocator.allocate(mib);
    if (!bytes.ok())
      return Poll(bytes.status());
    auto output = bytes.take_value();
    const double value = id + 1;
    std::memcpy(output.data(), &value, 8);
    auto storage = std::move(output).freeze();
    retained[id] = storage;
    auto result = builder(phase);
    multi_result::check(
        result.publish_tensor(0, Region::whole({mib / 8}), {0, {8}}, storage,
                              multi_result::take(ResultRelation::cartesian(
                                  phase.resources, mib / 8, {})),
                              {true, true, true, true}));
    return Poll(ResultPublication{multi_result::take(result.seal()), true});
  }
};
int sibling_admission() {
  constexpr std::uint64_t mib = 1024 * 1024;
  auto registry = std::make_shared<OperationRegistry>();
  unsigned calls[2]{};
  std::weak_ptr<const CpuStorage> retained[2];
  for (unsigned id = 0; id < 2; ++id) {
    OperationDefinition operation;
    operation.key = id ? "sibling_b" : "sibling_a";
    operation.traits = staged_traits(0, sizeof(LargeSource),
                                     schema(ElementType::Float64, mib / 8));
    operation.traits.outputs[0].region_rule = OperationRegionRule::Whole;
    operation.traits.workspace_bytes = 4 * mib;
    operation.start_result = [&, id](const ResultProgramQuery&,
                                     const BufferAllocator& allocator) {
      return ResultContinuation::make<LargeSource>(allocator, id, calls,
                                                   retained);
    };
    multi_result::check(registry->register_operation(operation));
  }
  OperationDefinition parent;
  parent.key = "siblings";
  parent.traits =
      staged_traits(2, sizeof(SiblingState), schema(ElementType::Float64, 1));
  parent.start_result = [](const ResultProgramQuery&,
                           const BufferAllocator& allocator) {
    return ResultContinuation::make<SiblingState>(allocator);
  };
  multi_result::check(registry->register_operation(parent));
  multi_result::check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {
      {1, "sibling_a", {}, {}},
      {2, "sibling_b", {}, {}},
      {3,
       "siblings",
       {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{2, "value"}},
       {}}};
  document.outputs = {{"sum", 3, "value"}};
  GraphContext graph(document);
  auto plan = multi_result::take(Compiler(registry).compile(graph)).plan;
  ExecutionContext limited(registry, {1, false, 8, 4 * mib + 65536});
  auto failed = limited.execute(plan);
  PS_CHECK(failed.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(calls[0] == 1 && calls[1] == 1 && retained[0].expired() &&
           retained[1].expired());
  PS_CHECK(limited.resource_budget()
               .value()
               .statistics()
               .live[ResourceKind::Payload] == 0);
  document.nodes.resize(1);
  document.outputs = {{"a", 1, "value"}};
  GraphContext single_graph(document);
  auto single =
      multi_result::take(Compiler(registry).compile(single_graph)).plan;
  auto recovered = limited.execute(single);
  PS_CHECK(recovered.ok() && calls[0] == 2 && calls[1] == 1);
  PS_CHECK(recovered.value().diagnostics.peak_live_bytes >= 4 * mib);
  recovered = Result<ExecutionResult>(Status{ErrorCode::Cancelled, {}});
  PS_CHECK(retained[0].expired());
  ExecutionContext sufficient(registry, {1, false, 8, 5 * mib + 65536});
  auto complete = sufficient.execute(plan);
  PS_CHECK(complete.ok() && calls[0] == 3 && calls[1] == 2);
  PS_CHECK(test::named_scalar(complete.value(), "sum") == 3);
  PS_CHECK(complete.value().diagnostics.peak_live_bytes >= 5 * mib);
  PS_CHECK(retained[0].expired() && retained[1].expired());
  return 0;
}
OperationDefinition copy_definition(std::string key) {
  OperationDefinition copy;
  copy.key = std::move(key);
  copy.traits = staged_traits(1, sizeof(multi_result::Program));
  copy.traits.input_schema[0].element_type = 0;
  copy.traits.requires_metadata_specialization = true;
  copy.specialize_metadata = [](const std::vector<OperationMetadata>& inputs,
                                const std::map<std::string, ParameterValue>&) {
    OperationOutputSpecialization output;
    output.metadata = inputs[0];
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  copy.start_result = [](const ResultProgramQuery&,
                         const BufferAllocator& allocator) {
    return ResultContinuation::make<multi_result::Program>(allocator, 0);
  };
  return copy;
}
int execution_network() {
  auto registry = std::make_shared<OperationRegistry>();
  auto counts = std::make_shared<Counts>();
  multi_result::check(registry->register_operation(pointer_definition(counts)));
  multi_result::check(registry->register_operation(copy_definition("pass")));
  OperationDefinition terminal;
  terminal.key = "terminal";
  terminal.traits = staged_traits(1, sizeof(TerminalState));
  terminal.traits.outputs[0].observation_kind = ObservationKind::RequestRecord;
  terminal.start_result = [](const ResultProgramQuery&,
                             const BufferAllocator& allocator) {
    return ResultContinuation::make<TerminalState>(allocator);
  };
  multi_result::check(registry->register_operation(terminal));
  std::atomic<unsigned> effects{0};
  auto effect = probe_definition(std::make_shared<Counts>(),
                                 [&](const ResultProgramPhase& phase) {
                                   ++effects;
                                   return number(phase, 1);
                                 });
  effect.key = "effect";
  effect.traits.input_count = 0;
  effect.traits.input_schema.clear();
  effect.traits.side_effect_free = false;
  effect.traits.cacheable = false;
  effect.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  multi_result::check(registry->register_operation(effect));
  multi_result::check(registry->freeze());
  WorkflowDocument document;
  document.inputs = {
      multi_result::declaration(1, "control", schema(ElementType::Int64)),
      multi_result::declaration(2, "payload", schema())};
  document.nodes = {
      {1, "pass", {WorkflowInputReference{1}}, {}},
      {2, "pass", {WorkflowInputReference{2}}, {}},
      {3,
       "pointer",
       {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{2, "value"}},
       {}},
      {4, "pass", {WorkflowNodeOutput{3, "value"}}, {}},
      {99, "effect", {}, {}}};
  document.outputs = {{"result", 4, "value"}};
  GraphContext graph(document);
  auto plan = multi_result::take(Compiler(registry).compile(graph)).plan;
  plan = multi_result::take(plan.tile_plan("result", Region({{0, 1}})));
  ExecutionContext execution(registry, {1, false, 8, 65536});
  auto root = multi_result::take(execution.resource_budget());
  std::vector<std::int64_t> pointers(16, -1);
  pointers[0] = 1;
  pointers[1] = 3;
  pointers[3] = -16;
  std::vector<double> samples(16);
  for (unsigned i = 0; i < 16; ++i)
    samples[i] = i * 2;
  ExecutionBindings bindings{
      {source(root, "control", values(ElementType::Int64, pointers)),
       source(root, "payload", values(ElementType::Float64, samples))}};
  auto executed = execution.execute(plan, bindings);
  PS_CHECK(executed.ok() && effects == 1);
  double value = 0;
  PS_CHECK(numeric_result_fixture::read(executed.value().results.at("result"),
                                        {0}, &value, 8)
               .ok() &&
           value == 30);
  auto observations =
      multi_result::take(executed.value().dependencies.source_observations());
  auto control = multi_result::take(Footprint::none({16}));
  auto payload = control;
  for (const auto& observation : observations) {
    if (observation.input == "control")
      control = multi_result::take(control.unite(observation.samples));
    if (observation.input == "payload")
      payload = multi_result::take(payload.unite(observation.samples));
  }
  PS_CHECK(
      control ==
      multi_result::take(point(0).unite(point(1))).unite(point(3)).value());
  PS_CHECK(payload == point(15));
  unsigned deliveries = 0;
  ResultTensorReadWindow retained;
  ExecutionOptions publication;
  publication.result_publication = [&](ValueRef output,
                                       const ResultRef& result) {
    if (output == ValueRef{4, 0}) {
      ++deliveries;
      retained = multi_result::take(result.acquire_tensor(
          multi_result::take(result.descriptor(false)), 0, Region({{0, 1}})));
    }
    return Status::success();
  };
  PS_CHECK(execution.execute(plan, bindings, {}, publication).ok());
  PS_CHECK(deliveries == 1 && effects == 2 && retained.row_run({0}).ok());
  ExecutionOptions bounded;
  bounded.maximum_dependency_work = 1;
  const auto before = counts->starts.load();
  PS_CHECK(execution.execute(plan, bindings, {}, bounded).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(before == counts->starts);
  CancellationSource cancel;
  cancel.cancel();
  PS_CHECK(execution.execute(plan, bindings, cancel.token()).status().code ==
           ErrorCode::Cancelled);
  auto frozen = multi_result::take(execution.freeze(plan, bindings));
  PS_CHECK(graph.replace(document) > 0);
  PS_CHECK(execution.execute(plan, bindings).status().code == ErrorCode::Stale);
  PS_CHECK(execution.execute(frozen).ok());
  document.nodes = {{1, "terminal", {WorkflowInputReference{2}}, {}}};
  document.outputs = {{"record", 1, "value"}};
  GraphContext terminal_graph(document);
  auto terminal_plan =
      multi_result::take(Compiler(registry).compile(terminal_graph)).plan;
  terminal_plan =
      multi_result::take(terminal_plan.tile_plan("record", Region({{0, 2}})));
  auto terminal_result = execution.execute(terminal_plan, bindings);
  PS_CHECK(terminal_result.ok());
  PS_CHECK(numeric_result_fixture::read(
               terminal_result.value().results.at("record"), {0}, &value, 8)
               .ok() &&
           value == 2);
  CancellationSource auxiliary;
  auxiliary.cancel();
  ExecutionOptions stopped;
  stopped.dependencies.sets.cancellation = auxiliary.token();
  const auto prior = counts->starts.load();
  PS_CHECK(execution.execute(frozen, {}, stopped).status().code ==
           ErrorCode::Cancelled);
  PS_CHECK(counts->starts == prior);
  return 0;
}
int dependency_record_rollback() {
  auto registry = std::make_shared<OperationRegistry>();
  multi_result::check(registry->register_operation(copy_definition("copy")));
  multi_result::check(registry->freeze());
  auto doc = probe_document("copy");
  doc.nodes.push_back({2, "copy", {WorkflowNodeOutput{1, "value"}}, {}});
  doc.outputs = {{"a", 1, "value"}, {"b", 2, "value"}};
  GraphContext graph(doc);
  auto plan = multi_result::take(Compiler(registry).compile(graph)).plan;
  ResourceBudget root;
  ResourceAllocationScope scope(root);
  execution_internal::DependencyRecords records(plan, "snapshot", {});
  auto relation = multi_result::take(ResultRelation::mapped(
      root, {16}, Region::whole({16}), {16}, {{0, 0, 1, 1}},
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  PS_CHECK(records
               .append_relation(0, point(0), relation, {},
                                ResultSupportTarget::Tensor)
               .ok());
  PS_CHECK(records.output("a", 0, point(0)).ok());
  auto saved = multi_result::take(records.fork_result_attempt());
  PS_CHECK(records
               .append_relation(0, point(1), relation, {},
                                ResultSupportTarget::Tensor)
               .ok());
  PS_CHECK(records
               .append_relation(1, point(1), relation, {},
                                ResultSupportTarget::Tensor)
               .ok());
  auto shared = multi_result::take(records.capture_bundle(1));
  execution_internal::DependencyRecords restored(plan, "snapshot", {});
  PS_CHECK(restored.import_bundle(*shared, 1).ok());
  PS_CHECK(restored.output("b", 1, point(1)).ok());
  auto result = std::move(restored).finish();
  PS_CHECK(result.record_count() == 2);
  PS_CHECK(result.potential_dirty("x", point(0)).value().at("b").empty());
  PS_CHECK(result.potential_dirty("x", point(1)).value().at("b") == point(1));
  auto original = multi_result::take(saved->snapshot());
  PS_CHECK(original.coverage().at("a") == point(0));
  PS_CHECK(original.potential_dirty("x", point(1)).value().at("a").empty());
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(field_prefix_evidence() == 0);
  PS_CHECK(allocator_lifetime() == 0);
  PS_CHECK(service_and_identity_regressions() == 0);
  PS_CHECK(concurrent_and_reentrant() == 0);
  PS_CHECK(root_work_resume() == 0);
  PS_CHECK(dependency_record_rollback() == 0);
  PS_CHECK(dependency_record_retirement() == 0);
  PS_CHECK(sibling_admission() == 0);
  PS_CHECK(execution_network() == 0);
  PS_CHECK(progressive() == 0);
  PS_CHECK(terminal_and_graph() == 0);
  PS_CHECK(block_services() == 0);
  return 0;
}
