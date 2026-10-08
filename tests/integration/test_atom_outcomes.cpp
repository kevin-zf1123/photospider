#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../../examples/atom_outcomes_workflow/operations.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct CaughtHostState {
  ResultRef prior;
  explicit CaughtHostState(ResultRef prior) : prior(std::move(prior)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    try {
      static_cast<void>(phase.allocator.allocate(8));
    } catch (...) {
    }
    return Result<ResultProgramPoll>(ResultPublication{prior, true});
  }
};
int caught_host_exceptions() {
  ResourceBudget root;
  ResourceAllocationScope scope(root);
  auto schema = atom_result::schema(1);
  auto builder =
      atom_result::take(ResultBuilder::start(root, schema, "caught"));
  atom_result::check(builder.bind_descriptor_relation(
      atom_result::take(ResultRelation::cartesian(root, 1, {}))));
  const double expected = 17;
  atom_result::check(builder.publish_tensor(
      0, Region::whole({1}),
      {reinterpret_cast<const std::uint8_t*>(&expected), 8},
      atom_result::take(ResultRelation::cartesian(root, 1, {})),
      {true, true, true, true}));
  auto prior = atom_result::take(builder.seal());
  OperationRegistry registry;
  OperationDefinition operation;
  operation.key = "test.caught_host";
  auto& output = operation.traits.outputs[0];
  output.output_schema.kind = OperationPortKind::Result;
  output.output_schema.result_schema_id = schema.id;
  output.output_schema.result_schema_version = schema.version;
  output.result_schema = schema;
  output.region_rule = OperationRegionRule::Dependency;
  output.observation_kind = ObservationKind::RequestRecord;
  output.continuation_bytes = sizeof(CaughtHostState);
  output.maximum_dependency_stages = 2;
  operation.start_result = [prior](const ResultProgramQuery&,
                                   const BufferAllocator& host) {
    return ResultContinuation::make<CaughtHostState>(host, prior);
  };
  PS_CHECK(registry.register_operation(std::move(operation)).ok());
  PS_CHECK(registry.freeze().ok());
  ResultProgramMetadata metadata;
  metadata.output.result_schema =
      std::make_shared<const SchemaTemplate>(schema);
  const std::map<std::string, ParameterValue> parameters;
  ResultProgramQuery query(metadata, parameters);
  query.tensor_outputs = Footprint::all({1}).take_value();
  query.semantic_key = "caught";
  ResultObjectInputs inputs;
  ResourceVector<ResultIoReply> io;
  for (bool allocation : {false, true}) {
    auto session = atom_result::take(
        registry.start_result("test.caught_host", query, root.allocator()));
    unsigned notifications = 0;
    auto failure = std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok);
    BufferAllocator throwing(
        [allocation](std::uint64_t) -> Result<std::shared_ptr<void>> {
          if (allocation)
            throw std::bad_alloc();
          throw std::runtime_error("host reservation exception");
        });
    auto watched = throwing.limited(4096, [&](ErrorCode code) {
      ++notifications;
      failure->store(code);
    });
    ResultProgramPhase phase{
        query,  inputs,
        io,     watched,
        root,   [&](std::uint64_t units) { return root.consume({units}); },
        failure};
    auto polled = session.poll(phase);
    PS_CHECK(!polled.ok());
    PS_CHECK(polled.status().code == (allocation ? ErrorCode::ResourceExhausted
                                                 : ErrorCode::OperationFailed));
    PS_CHECK(notifications == 1);
    PS_CHECK(session.poll(phase).status().code == polled.status().code);
  }
  return 0;
}
struct DomainResult65 {
  std::uint64_t trigger;
  explicit DomainResult65(std::uint64_t trigger) : trigger(trigger) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto key = atom_result::take(result_atom_key(phase.query));
    if (key.coordinate[0] == trigger) {
      FailureDetail detail{FailureOrigin::Domain,
                           FailureScope::ValidationDomain};
      detail.domain = atom_result::take(result_observation_domain(phase.query));
      return Result<ResultProgramPoll>(
          Status{ErrorCode::OperationFailed, "fixed domain invalid",
                 FailureReason::InvalidDomain, detail});
    }
    return Result<ResultProgramPoll>(atom_result::publish(phase, 7, 0, false));
  }
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& batch) {
    ResourceVector<ResultJointOutcome> outcomes;
    for (const auto* phase : batch.members)
      outcomes.push_back(
          {atom_result::take(result_atom_key(phase->query)), poll(*phase)});
    return Result<ResourceVector<ResultJointOutcome>>(std::move(outcomes));
  }
};
int cross_batch_domain() {
  for (std::uint64_t trigger : {0u, 64u}) {
    auto registry = std::make_shared<OperationRegistry>();
    auto op = atom_result::operation(0, "test.coordinates");
    op.traits.input_count = 0;
    op.traits.input_schema.clear();
    op.traits.outputs[0].result_schema = atom_result::schema(65);
    op.traits.joint_continuation_bytes = sizeof(DomainResult65);
    op.start_result = [trigger](const auto&, const BufferAllocator& host) {
      return ResultContinuation::make<DomainResult65>(host, trigger);
    };
    op.start_result_joint = [trigger](const auto&,
                                      const BufferAllocator& host) {
      return ResultJointContinuation::make<DomainResult65>(host, trigger);
    };
    PS_CHECK(registry->register_operation(std::move(op)).ok());
    PS_CHECK(registry->freeze().ok());
    WorkflowDocument doc;
    doc.nodes = {{11, "test.coordinates", {}, {}}};
    doc.outputs = {{"sink", 11, "value"}};
    GraphContext graph(doc);
    auto compiled = Compiler(registry).compile(graph).take_value();
    ExecutionContextConfig config{1, false, 16, 1048576, 1048576};
    config.maximum_dependency_cache_metadata = 1048576;
    ExecutionContext context(registry, config);
    ExecutionOptions options;
    options.maximum_dependency_work = 10000000;
    options.maximum_dependency_cache_work = 10000000;
    options.dependencies.sets.maximum_work = 10000000;
    auto result = context.execute_atoms(
        compiled.plan, {}, {{"sink", Footprint::all({65}).take_value()}}, {},
        options);
    if (trigger == 64) {
      PS_CHECK(!result.ok() &&
               result.status().detail.origin == FailureOrigin::Protocol);
      ExecutionContext cached(registry, config);
      DemandQuery prefix{
          {"sink",
           Footprint::from_regions({65}, {Region({{0, 64}})}).take_value()}};
      auto warm = cached.execute_atoms(compiled.plan, {}, prefix, {}, options);
      PS_CHECK(warm.ok());
      auto hit = cached.execute_atoms(compiled.plan, {}, prefix, {}, options);
      PS_CHECK(hit.ok() && hit.value().diagnostics.cache_hits == 64);
      auto late = cached.execute_atoms(
          compiled.plan, {}, {{"sink", Footprint::all({65}).take_value()}}, {},
          options);
      PS_CHECK(!late.ok() &&
               late.status().detail.origin == FailureOrigin::Protocol);
    } else {
      PS_CHECK(result.ok() && result.value().atoms.size() == 65);
      for (const auto& atom : result.value().atoms)
        PS_CHECK(!atom.outcome.ok());
      doc.outputs = {{"a-empty", 11, "value"}, {"b-value", 11, "value"}};
      GraphContext aliases(doc);
      auto alias_plan = Compiler(registry).compile(aliases).take_value().plan;
      auto empty_first = context.execute_atoms(
          alias_plan, {},
          {{"a-empty", Footprint::none({65}).take_value()},
           {"b-value", Footprint::all({65}).take_value()}});
      PS_CHECK(empty_first.ok() && empty_first.value().atoms.size() == 65);
      for (const auto& atom : empty_first.value().atoms)
        PS_CHECK(atom.name == "b-value" && !atom.outcome.ok() &&
                 atom.outcome.status().detail.origin == FailureOrigin::Domain &&
                 atom.outcome.status().detail.scope ==
                     FailureScope::ValidationDomain);
    }
  }
  return 0;
}
int dag_cases() {
  for (int mode : {0, 1}) {
    auto registry = std::make_shared<OperationRegistry>();
    PS_CHECK(registry
                 ->register_operation(
                     atom_result::operation(mode, "test.coordinates"))
                 .ok());
    PS_CHECK(
        registry->register_operation(atom_result::operation(-1, "test.scale"))
            .ok());
    PS_CHECK(registry->freeze().ok());
    WorkflowDocument doc;
    doc.inputs = {atom_result::declaration(1, "primary"),
                  atom_result::declaration(2, "fallback")};
    doc.nodes = {{11,
                  "test.coordinates",
                  {WorkflowInputReference{1}, WorkflowInputReference{2}},
                  {}},
                 {22, "test.scale", {WorkflowNodeOutput{11, "value"}}, {}}};
    doc.outputs = {{"sink", 22, "value"}};
    GraphContext graph(doc);
    auto compiled = Compiler(registry).compile(graph);
    if (!compiled.ok())
      std::cerr << compiled.status().message << '\n';
    PS_CHECK(compiled.ok());
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    auto root = atom_result::take(context.resource_budget());
    ExecutionBindings bindings;
    bindings.inputs = {atom_result::binding(root, "primary", {2, 0, -1}),
                       atom_result::binding(root, "fallback", {9, 8, 4})};
    auto result =
        context.execute_atoms(compiled.value().plan, bindings,
                              {{"sink", Footprint::all({3}).take_value()}});
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    if (mode) {
      PS_CHECK(!result.ok() &&
               result.status().detail.origin == FailureOrigin::Protocol);
      PS_CHECK(result.status().detail.node_id == 11);
      continue;
    }
    PS_CHECK(result.ok());
    PS_CHECK(result.value().atoms.size() == 3);
    PS_CHECK(result.value().diagnostics.joint_groups == 2 &&
             result.value().diagnostics.joint_fallbacks == 0);
    auto support =
        atom_result::take(result.value().dependencies.source_support());
    auto successes =
        Footprint::from_regions({3}, {Region({{0, 1}}), Region({{2, 1}})})
            .take_value();
    auto fallback =
        Footprint::from_regions({3}, {Region({{2, 1}})}).take_value();
    PS_CHECK(support.at("primary") == successes &&
             support.at("fallback") == fallback);
    auto dirty = atom_result::take(result.value().dependencies.potential_dirty(
        "primary", fallback, 2, {}, ResultSupportTarget::Tensor, 0));
    PS_CHECK(dirty.at("sink") == fallback);
    for (const auto& atom : result.value().atoms) {
      PS_CHECK(atom.output.node_id == 22);
      if (atom.key.coordinate[0] == 1) {
        PS_CHECK(!atom.outcome.ok());
        PS_CHECK(atom.outcome.status().reason == FailureReason::DivideByZero &&
                 atom.outcome.status().detail.node_id == 11 &&
                 atom.outcome.status().detail.atom == atom.key);
      } else {
        PS_CHECK(atom.outcome.ok());
        double actual = 0;
        PS_CHECK(atom.outcome.value()
                     .read_tensor(atom.outcome.value().descriptor().value(), 0,
                                  {atom.key.coordinate[0]}, &actual, 8)
                     .ok());
        PS_CHECK(actual == (atom.key.coordinate[0] == 0 ? 1 : .5));
      }
    }
  }
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(caught_host_exceptions() == 0);
  PS_CHECK(cross_batch_domain() == 0);
  PS_CHECK(dag_cases() == 0);
  return 0;
}
