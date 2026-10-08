#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../../examples/numeric_workflow/icc_fixture.hpp"
#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct Counts {
  unsigned starts = 0, destroys = 0, polls = 0, members = 0;
  bool returned = false;
};
ResultProgramPoll publish(const ResultProgramPhase& phase,
                          bool rounded = false) {
  auto builder = take(ResultBuilder::start(phase.resources,
                                           *phase.query.output.result_schema,
                                           phase.query.semantic_key));
  check(builder.bind_descriptor_relation(
      take(ResultRelation::cartesian(phase.resources, 1, {}))));
  auto support = take(ResultRelation::cartesian(phase.resources, 4, {}));
  auto samples = phase.query.tensor_outputs.value_or(take(Footprint::all({4})));
  check(samples.visit(
      [&](const auto& at) {
        const double value =
            rounded && at[0] == 1
                ? static_cast<double>(static_cast<float>(16777217))
                : static_cast<double>(at[0] + 2);
        return builder.publish_tensor(
            0, Region({{at[0], 1}}),
            {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)},
            support, {true, true, true, true});
      },
      4));
  return ResultPublication{take(builder.seal()), true};
}
Status atom_failure(AtomKey key) {
  FailureDetail detail{FailureOrigin::Domain, FailureScope::Atom};
  detail.atom = key;
  return {ErrorCode::OperationFailed, "local zero", FailureReason::DivideByZero,
          detail};
}
Status domain_failure(const ResultProgramQuery& query, bool partial = false) {
  FailureDetail detail{FailureOrigin::Domain, FailureScope::ValidationDomain};
  detail.domain = take(result_observation_domain(query));
  if (partial)
    detail.domain->extent[0] = 3;
  return {ErrorCode::OperationFailed, "fixed domain",
          FailureReason::InvalidDomain, detail};
}
struct Shared {
  std::shared_ptr<Counts> counts;
  unsigned mode, round = 0;
  Shared(std::shared_ptr<Counts> counts, unsigned mode)
      : counts(std::move(counts)), mode(mode) {}
  ~Shared() noexcept { ++counts->destroys; }
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& phase) {
    ++counts->polls;
    counts->members += phase.members.size();
    check(phase.consume_work(1));
    if (mode == 12)
      return Result<ResourceVector<ResultJointOutcome>>(
          Status{ErrorCode::BackendUnavailable,
                 "enclosing backend",
                 {},
                 {FailureOrigin::Backend, FailureScope::Group}});
    ResourceVector<ResultJointOutcome> outcomes;
    std::optional<QualityReport> report;
    if ((mode >= 7 && mode <= 11) || (mode >= 15 && mode <= 17) ||
        (mode == 18 && round) || mode == 19 || mode == 21 || mode == 22 ||
        mode == 26) {
      if (mode == 8 || mode == 11 || mode == 18 || mode == 19 || mode == 26) {
        report = take(QualityReport::measured_residual("integer-system", 4, .25,
                                                       phase.allocator));
      } else {
        std::array<std::int64_t, 4> a{2, 3, 4, 5}, x{2, 3, 4, 5},
            b{4, 9, 16, 25};
        if (mode == 9)
          x[1] = 100;
        if (mode == 17)
          x[1] = 16777217;
        ResourceBudget foreign;
        report = take(QualityReport::certify_integer_diagonal(
            "integer-system", a.data(), x.data(), b.data(), 4,
            mode == 15 ? foreign.allocator() : phase.allocator,
            phase.consume_work));
      }
    }
    for (const auto* member : phase.members) {
      const auto key = take(result_atom_key(member->query));
      const auto coordinate = key.coordinate[0];
      Result<ResultProgramPoll> reply(Status{ErrorCode::Internal, {}});
      if (mode == 1 && coordinate == 1) {
        reply = Result<ResultProgramPoll>(atom_failure(key));
      } else if (((mode == 4 || mode == 18) && !round) ||
                 (mode == 6 && !round && coordinate != 0) || mode == 10) {
        ResultProgramNeed need;
        need.io.push_back(ResultCreateTemporary{});
        reply = Result<ResultProgramPoll>(std::move(need));
      } else if (((mode == 4 || mode == 18) && round && coordinate == 1) ||
                 (mode == 6 && round && coordinate == 1) ||
                 ((mode == 5 || mode == 11 || mode == 16 || mode == 26) &&
                  coordinate == 1) ||
                 (mode == 19 && coordinate != 2)) {
        reply =
            Result<ResultProgramPoll>(domain_failure(member->query, mode == 5));
        if (mode == 19 && coordinate == 1) {
          auto conflicting = reply.status();
          conflicting.reason = FailureReason::DivideByZero;
          reply = Result<ResultProgramPoll>(std::move(conflicting));
        }
      } else if (mode == 14 && coordinate == 1) {
        auto wrong = key;
        wrong.coordinate[0] = 2;
        reply = Result<ResultProgramPoll>(atom_failure(wrong));
      } else {
        reply = Result<ResultProgramPoll>(publish(*member, mode == 17));
      }
      outcomes.push_back({key, std::move(reply), report});
    }
    ++round;
    if (mode == 2)
      outcomes.back().key = outcomes.front().key;
    if (mode == 3)
      outcomes.back().key.coordinate[0] = 99;
    if (mode == 20)
      outcomes.back().key.coordinate[7] = 1;
    std::reverse(outcomes.begin(), outcomes.end());
    if (mode == 21) {
      const auto& root = phase.members.front()->resources;
      check(root.consume({0, 1024 - root.statistics().issued.io_bytes}));
    }
    counts->returned = true;
    return Result<ResourceVector<ResultJointOutcome>>(std::move(outcomes));
  }
};
struct Single {
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    return Result<ResultProgramPoll>(publish(phase));
  }
};
OperationDefinition definition(std::shared_ptr<Counts> counts, unsigned mode) {
  OperationDefinition operation;
  operation.key = "test.result_atoms";
  auto output = multi_result::output("value");
  output.result_schema = multi_result::schema(ElementType::Float64, {4});
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  operation.traits.outputs = {output};
  operation.traits.input_count = 0;
  operation.traits.input_schema.clear();
  operation.traits.joint_contract = 2;
  operation.traits.joint_continuation_bytes = 512;
  operation.traits.joint_workspace_bytes = 4096;
  operation.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<Single>(allocator);
  };
  operation.start_result_joint = [counts, mode](const auto&,
                                                const auto& allocator) {
    ++counts->starts;
    return ResultJointContinuation::make<Shared>(allocator, counts, mode);
  };
  return operation;
}
int direct(unsigned mode, unsigned member_count = 3,
           std::optional<QualityReport>* retained_quality = nullptr,
           ResultRef* retained_result = nullptr) {
  ResourceLimits limits;
  if (mode == 21)
    limits.maximum_io_bytes = 1024;
  if (mode == 25)
    limits.maximum_work = 100000;
  ResourceBudget root(limits), foreign;
  ResourceAllocationScope scope(root);
  auto counts = std::make_shared<Counts>();
  OperationRegistry registry;
  check(registry.register_operation(definition(counts, mode)));
  auto outputs = take(infer_operation_outputs(
      take(registry.find_traits("test.result_atoms")), {}, {}));
  ResultProgramMetadata metadata{{}, outputs[0]};
  std::map<std::string, ParameterValue> parameters;
  ResourceVector<ResultProgramQuery> queries;
  queries.reserve(member_count);
  const std::array<std::string, 3> names{"atom-zero", "atom-one", "atom-two"};
  CancellationSource cancellation;
  for (unsigned i = 0; i < member_count; ++i) {
    queries.emplace_back(metadata, parameters);
    auto& query = queries.back();
    query.semantic_key = names[i];
    query.snapshot_identity = "coordinate-inputs";
    query.tensor_outputs =
        take(Footprint::from_regions({4}, {Region({{i, 1}})}));
    if ((mode == 13 || mode == 18 || mode == 22 || mode == 26) && i == 0)
      query.cancellation = cancellation.token();
  }
  auto duplicate = queries;
  if (member_count > 1) {
    duplicate[1].tensor_outputs = duplicate[0].tensor_outputs;
    PS_CHECK(!registry.start_result_joint("test.result_atoms", duplicate, root)
                  .ok());
  }
  auto state =
      take(registry.start_result_joint("test.result_atoms", queries, root));
  auto allocator = root.allocator();
  auto foreign_allocator = foreign.allocator();
  auto work = [&](std::uint64_t amount) { return root.consume({amount}); };
  auto reading_work = [&](std::uint64_t amount) {
    if (mode == 22 && counts->returned)
      cancellation.cancel();
    return work(amount);
  };

  ResultObjectInputs results;
  ResourceVector<ResultIoReply> io;
  ResourceVector<ResultProgramPhase> phases;
  ResourceVector<const ResultProgramPhase*> ready;
  phases.reserve(member_count);
  for (unsigned i = 0; i < member_count; ++i)
    phases.push_back({queries[i],
                      results,
                      io,
                      mode == 24 && i == 0 ? foreign_allocator : allocator,
                      root,
                      i == 0
                          ? std::function<Status(std::uint64_t)>(reading_work)
                          : std::function<Status(std::uint64_t)>(work),
                      {}});
  for (const auto& phase : phases)
    ready.push_back(&phase);
  if (mode == 13 || mode == 26)
    cancellation.cancel();
  if (mode == 25)
    check(root.consume({limits.maximum_work - root.statistics().issued.work}));
  auto response =
      state.poll({ready, mode == 23 ? foreign_allocator : allocator, work});
  if (mode == 4 || mode == 6 || mode == 18) {
    PS_CHECK(response.ok());
    ready.erase(ready.begin());
    if (mode == 18)
      cancellation.cancel();
    response = state.poll({ready, allocator, work});
  }
  if (mode == 2 || mode == 3 || mode == 5 || mode == 6 || mode == 9 ||
      mode == 10 || mode == 14 || mode == 15 || mode == 16 || mode == 17 ||
      mode == 19 || mode == 20 || mode == 23 || mode == 24) {
    PS_CHECK(!response.ok() &&
             response.status().detail.origin == FailureOrigin::Protocol);
    auto repeated = state.poll({ready, allocator, work});
    PS_CHECK(!repeated.ok() &&
             repeated.status().detail.origin == FailureOrigin::Protocol);
    if (mode == 9 || mode == 10 || mode == 15 || mode == 16 || mode == 17)
      PS_CHECK(response.status().reason == FailureReason::InvalidQuality);
  } else if (mode == 21 || mode == 25) {
    if (response.ok() || response.status().code != ErrorCode::ResourceExhausted)
      std::cerr << "resource mode " << mode << " got "
                << static_cast<int>(response.status().code) << '\n';
    PS_CHECK(!response.ok() &&
             response.status().code == ErrorCode::ResourceExhausted &&
             response.status().detail.origin == FailureOrigin::Unspecified &&
             response.status().reason == FailureReason::WorkLimit);
    if (mode == 25)
      PS_CHECK(counts->polls == 0);
  } else if (mode == 12) {
    PS_CHECK(!response.ok() &&
             response.status().detail.scope == FailureScope::Group &&
             response.status().message == "enclosing backend");
  } else {
    if (!response.ok())
      std::cerr << "Result atoms mode " << mode << ": "
                << response.status().message << '\n';
    PS_CHECK(response.ok());
    auto outcomes = response.take_value();
    PS_CHECK(outcomes.size() == member_count);
    for (std::size_t i = 0; i < outcomes.size(); ++i)
      for (std::size_t j = i + 1; j < outcomes.size(); ++j)
        PS_CHECK(outcomes[i].key != outcomes[j].key);
    for (const auto& outcome : outcomes) {
      const auto at = outcome.key.coordinate[0];
      PS_CHECK(outcome.key.canonical() && outcome.key.output_index == 0);
      if ((mode == 1 && at == 1) || mode == 4 || mode == 11 ||
          ((mode == 18 || mode == 26) && at != 0)) {
        PS_CHECK(!outcome.outcome.ok() &&
                 outcome.outcome.status().detail.origin ==
                     FailureOrigin::Domain);
        if (mode == 4 || mode == 11 || mode == 18 || mode == 26)
          PS_CHECK(outcome.outcome.status().detail.scope ==
                   FailureScope::ValidationDomain);
      } else if ((mode == 13 || mode == 18 || mode == 22 || mode == 26) &&
                 at == 0) {
        PS_CHECK(!outcome.outcome.ok() &&
                 outcome.outcome.status().code == ErrorCode::Cancelled &&
                 !outcome.quality);
      } else {
        PS_CHECK(outcome.outcome.ok());
        const auto& publication =
            std::get<ResultPublication>(outcome.outcome.value());
        auto facts = take(publication.result.descriptor());
        double number = 0;
        check(publication.result.read_tensor(facts, 0, {at}, &number, 8));
        PS_CHECK(number == at + 2);
      }
      if (mode == 7 || (mode == 22 && at != 0)) {
        PS_CHECK(outcome.quality && outcome.quality->error_bound() == 0 &&
                 take(outcome.quality->proof_row(at))[1] ==
                     static_cast<std::int64_t>(at + 2));
        if (mode == 7 && at == 0 && retained_quality && retained_result) {
          *retained_quality = outcome.quality;
          *retained_result =
              std::get<ResultPublication>(outcome.outcome.value()).result;
        }
      } else if (mode == 8 || mode == 11 ||
                 ((mode == 18 || mode == 26) && at != 0)) {
        PS_CHECK(outcome.quality && !outcome.quality->error_bound() &&
                 outcome.quality->residual() == .25);
      }
    }
  }
  state = {};
  PS_CHECK(counts->starts == 1 && counts->destroys == 1);
  return 0;
}
int grouped_key() {
  ResourceBudget root;
  ResourceAllocationScope scope(root);
  auto schema = multi_result::schema(ElementType::Float64, {3, 2});
  schema.tensors[0].batch_axes = {5};
  schema.tensors[0].atomic_trailing_axes = 1;
  ResultProgramMetadata metadata;
  metadata.output.result_schema =
      std::make_shared<const SchemaTemplate>(schema);
  std::map<std::string, ParameterValue> parameters;
  ResultProgramQuery query(metadata, parameters);
  query.tensor_outputs = take(
      Footprint::from_regions({5, 3, 2}, {Region({{2, 1}, {1, 1}, {1, 1}})}));
  auto key = take(result_atom_key(query));
  auto domain = take(result_observation_domain(query));
  PS_CHECK(key.rank == 2 && key.coordinate[0] == 2 && key.coordinate[1] == 1 &&
           domain.canonical() && domain.extent[0] == 5 &&
           domain.extent[1] == 3);
  query.tensor_outputs = take(Footprint::all({5, 3, 2}));
  PS_CHECK(!result_atom_key(query).ok());
  return 0;
}
int color_key() {
  ResourceBudget root;
  ResourceAllocationScope scope(root);
  auto schema = multi_result::schema(ElementType::Float64, {2, 3});
  schema.tensors[0].batch_axes = {4};
  schema.tensors[0].facets = {take(encode_color_array(ColorArrayDescriptor{}))};
  ResultProgramMetadata metadata;
  metadata.output.result_schema =
      std::make_shared<const SchemaTemplate>(schema);
  std::map<std::string, ParameterValue> parameters;
  ResultProgramQuery query(metadata, parameters);
  query.tensor_outputs = take(
      Footprint::from_regions({4, 2, 3}, {Region({{2, 1}, {1, 1}, {2, 1}})}));
  const auto key = take(result_atom_key(query));
  const auto domain = take(result_observation_domain(query));
  PS_CHECK(key.rank == 2 && key.coordinate[0] == 2 && key.coordinate[1] == 1 &&
           domain.extent[0] == 4 && domain.extent[1] == 2);
  const auto closed =
      take(schema.tensors[0].close_samples(*query.tensor_outputs));
  PS_CHECK(closed.boxes().size() == 1 &&
           closed.boxes()[0].dimensions()[2].offset == 0 &&
           closed.boxes()[0].dimensions()[2].extent == 3);
  auto builder = take(ResultBuilder::start(root, schema, "color-key"));
  check(builder.bind_descriptor_relation(
      take(ResultRelation::cartesian(root, 1, {}))));
  const std::array<double, 3> color{.1, .2, .3};
  check(builder.publish_tensor(
      0, closed.boxes()[0],
      {reinterpret_cast<const std::uint8_t*>(color.data()), sizeof(color)},
      take(ResultRelation::cartesian(root, 24, {})), {true, true, true, true}));
  const auto result = take(builder.seal());
  const auto facts = take(result.descriptor());
  for (unsigned channel = 0; channel < 3; ++channel) {
    double sample = 0;
    check(result.read_tensor(facts, 0, {2, 1, channel}, &sample, 8));
    PS_CHECK(sample == color[channel]);
  }
  return 0;
}

struct WorkflowCounts {
  unsigned starts = 0, destroys = 0, members = 0, singles = 0;
};
struct WorkflowJoint {
  std::shared_ptr<WorkflowCounts> counts;
  unsigned mode, width, round = 0;
  WorkflowJoint(std::shared_ptr<WorkflowCounts> counters, unsigned behavior,
                unsigned size)
      : counts(std::move(counters)), mode(behavior), width(size) {}
  ~WorkflowJoint() noexcept { ++counts->destroys; }
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& phase) {
    if (mode == 8 || mode == 9) {
      NumericDiagnostics report;
      report.profile = CpuNumericProfile::Strict;
      const char identity[] = "test.Result.numeric/1";
      std::memcpy(report.implementation.data(), identity, sizeof(identity));
      report.evaluated_values = 1;
      report.strict_math_calls = 2;
      for (const auto* member : phase.members)
        check(member->report_numeric(report));
      if (mode == 9)
        return Result<ResourceVector<ResultJointOutcome>>(
            Status{ErrorCode::BackendUnavailable,
                   "reported shared failure",
                   {},
                   {FailureOrigin::Backend, FailureScope::Group}});
    }
    ResourceVector<ResultJointOutcome> outcomes;
    for (const auto* member : phase.members) {
      ++counts->members;
      auto key = take(result_atom_key(member->query));
      if (mode == 6 && !round) {
        ResultProgramNeed need;
        need.io.push_back(ResultCreateTemporary{});
        if (key.coordinate[0] != 1)
          need.io.push_back(ResultCreateTemporary{});
        outcomes.push_back({key, Result<ResultProgramPoll>(std::move(need))});
        continue;
      }
      if ((mode == 1 && key.coordinate[0] == 0) ||
          (mode == 2 && key.coordinate[0] == 64) || mode == 6 || mode == 7) {
        std::optional<QualityReport> report;
        if (mode == 7)
          report = take(QualityReport::measured_residual(
              "workflow-system", width, .25, phase.allocator));
        outcomes.push_back(
            {key, Result<ResultProgramPoll>(domain_failure(member->query)),
             report});
        continue;
      }
      if ((mode == 3 || mode == 8) && key.coordinate[0] == 1) {
        outcomes.push_back({key, Result<ResultProgramPoll>(atom_failure(key))});
        continue;
      }
      if (mode == 4) {
        return Result<ResourceVector<ResultJointOutcome>>(
            Status{ErrorCode::BackendUnavailable,
                   "shared failure",
                   {},
                   {FailureOrigin::Backend, FailureScope::Group}});
      }
      auto builder = take(ResultBuilder::start(
          member->resources, *member->query.output.result_schema,
          member->query.semantic_key));
      check(builder.bind_descriptor_relation(
          take(ResultRelation::cartesian(member->resources, 1, {}))));
      const double value = key.coordinate[0] + 2 + member->query.tensor_slot;
      check(builder.publish_tensor(
          member->query.tensor_slot, member->query.tensor_outputs->boxes()[0],
          {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)},
          take(ResultRelation::cartesian(member->resources, width, {})),
          {true, true, true, true}));
      std::optional<QualityReport> report;
      if (mode == 5)
        report = take(QualityReport::measured_residual("workflow-system", width,
                                                       .25, phase.allocator));
      outcomes.push_back({key,
                          Result<ResultProgramPoll>(
                              ResultPublication{take(builder.seal()), true}),
                          report});
    }
    ++round;
    std::reverse(outcomes.begin(), outcomes.end());
    return Result<ResourceVector<ResultJointOutcome>>(std::move(outcomes));
  }
};
OperationDefinition workflow_source(
    const std::shared_ptr<WorkflowCounts>& counts, unsigned mode,
    unsigned width) {
  auto op = definition(std::make_shared<Counts>(), 0);
  op.key = "test.workflow_atoms";
  op.traits.joint_workspace_bytes = 65536;
  op.traits.outputs[0].result_schema =
      multi_result::schema(ElementType::Float64, {width});
  op.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  op.start_result = [counts](const auto&, const auto& allocator) {
    ++counts->singles;
    return ResultContinuation::make<Single>(allocator);
  };
  op.start_result_joint = [counts, mode, width](const auto&,
                                                const auto& allocator) {
    ++counts->starts;
    return ResultJointContinuation::make<WorkflowJoint>(allocator, counts, mode,
                                                        width);
  };
  return op;
}
struct WorkflowSum {
  unsigned width, next = 0;
  double total = 0;
  explicit WorkflowSum(unsigned size) : width(size) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (next) {
      double value = 0;
      check(phase.tensors->at({0, 0}).read({next - 1}, &value, sizeof(value)));
      total += value;
    }
    if (next < width) {
      ResultProgramNeed need;
      need.tensors.push_back(
          {0, 0,
           take(Footprint::from_regions({width}, {Region({{next++, 1}})})), 1});
      return Result<ResultProgramPoll>(std::move(need));
    }
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    check(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(phase.resources, 1, {}))));
    check(builder.publish_tensor(
        0, Region::whole({1}),
        {reinterpret_cast<const std::uint8_t*>(&total), sizeof(total)},
        take(ResultRelation::cartesian(
            phase.resources, 1,
            {0, 1, 0, width, ResultSupportTarget::Tensor, 0})),
        {true, true, true, true}));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
int workflow(unsigned mode, bool aliases, bool enable_joint = true) {
  const unsigned width = aliases ? 3 : 65;
  auto counts = std::make_shared<WorkflowCounts>();
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(workflow_source(counts, mode, width)));
  OperationDefinition sum;
  sum.key = "test.workflow_sum";
  sum.traits.input_count = 1;
  sum.traits.input_schema.resize(1);
  sum.traits.input_schema[0].kind = OperationPortKind::Result;
  sum.traits.input_schema[0].result_schema_id = "test.multi_output";
  sum.traits.input_schema[0].result_schema_version = 1;
  sum.traits.cacheable = false;
  sum.traits.outputs = {multi_result::output("value")};
  sum.traits.outputs[0].maximum_dependency_stages = width + 1;
  sum.start_result = [width](const auto&, const auto& allocator) {
    return ResultContinuation::make<WorkflowSum>(allocator, width);
  };
  check(registry->register_operation(std::move(sum)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{10, "test.workflow_atoms", {}, {}}};
  PlanningOptions compile;
  if (aliases) {
    document.outputs = {{"zero", 10, "value"},
                        {"one", 10, "value"},
                        {"two", 10, "value"}};
    compile.output_regions = {{"zero", Region({{0, 1}})},
                              {"one", Region({{1, 1}})},
                              {"two", Region({{2, 1}})}};
  } else {
    document.nodes.push_back(
        {20, "test.workflow_sum", {WorkflowNodeOutput{10, "value"}}, {}});
    document.outputs = {{"total", 20, "value"}};
  }
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(graph, compile)).plan;
  ExecutionContextConfig config{1, false, 16, 1024 * 1024, 1024 * 1024};
  config.maximum_dependency_cache_metadata = 1048576;
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  ExecutionOptions options;
  options.enable_joint = enable_joint;
  options.maximum_dependency_work = 10000000;
  options.dependencies.sets.maximum_work = 10000000;
  auto result = context.execute(plan, {}, {}, options);
  if (mode == 0 || mode == 5) {
    if (!result.ok())
      std::cerr << "workflow atom failure: " << result.status().message << '\n';
    PS_CHECK(result.ok());
    PS_CHECK(result.value().diagnostics.joint_fallbacks == 0);
    if (aliases) {
      PS_CHECK(multi_result::number(result.value().results.at("zero"), {0}) ==
               2);
      PS_CHECK(multi_result::number(result.value().results.at("one"), {1}) ==
               3);
      PS_CHECK(multi_result::number(result.value().results.at("two"), {2}) ==
               4);
      PS_CHECK(counts->starts == (enable_joint ? 1 : width));
    } else {
      PS_CHECK(multi_result::number(result.value().results.at("total")) ==
               width * (width + 3) / 2);
      PS_CHECK(counts->starts == width);
    }
    // A quality-bearing producer cannot become an evidence-free cache hit.
    if (mode == 5 && !aliases) {
      auto repeated = context.execute(plan, {}, {}, options);
      PS_CHECK(repeated.ok() && counts->starts == width * 2);
    }
    if (mode == 0 && !aliases) {
      PS_CHECK(context.cache_statistics().entries >= width);
      auto repeated = context.execute(plan, {}, {}, options);
      if (counts->starts != width)
        std::cerr << "repeated C2 workflow starts=" << counts->starts
                  << " cache_hits="
                  << (repeated.ok() ? repeated.value().diagnostics.cache_hits
                                    : 0)
                  << " status=" << repeated.status().message << '\n';
      PS_CHECK(repeated.ok() && counts->starts == width &&
               repeated.value().diagnostics.cache_hits >= width);
    }
  } else {
    PS_CHECK(!result.ok() && result.status().detail.node_id == 10);
    if (mode == 1 || mode == 7)
      PS_CHECK(result.status().detail.scope == FailureScope::ValidationDomain);
    if (mode == 2)
      PS_CHECK(result.status().detail.origin == FailureOrigin::Protocol);
    if (mode == 2 && !aliases) {
      // The previous Run retained the first 64 successes. They must still
      // prevent a later full-domain failure when this Run observes cache hits.
      auto repeated = context.execute(plan, {}, {}, options);
      PS_CHECK(!repeated.ok() &&
               repeated.status().detail.origin == FailureOrigin::Protocol &&
               repeated.status().detail.node_id == 10 &&
               counts->starts == width + 1);
    }
    if (mode == 6)
      PS_CHECK(result.status().detail.scope == FailureScope::ValidationDomain &&
               counts->starts == 1 && counts->members == 4);
    if (mode == 3)
      PS_CHECK(result.status().reason == FailureReason::DivideByZero &&
               result.status().detail.atom &&
               result.status().detail.atom->coordinate[0] == 1);
    if (mode == 4)
      PS_CHECK(result.status().detail.origin == FailureOrigin::Backend &&
               result.status().detail.scope == FailureScope::Group &&
               result.status().code == ErrorCode::BackendUnavailable);
  }
  PS_CHECK(counts->singles == 0 && counts->starts == counts->destroys);
  PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}

struct SlotPair {
  bool requested = false;
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& phase) {
    ResourceVector<ResultJointOutcome> replies;
    for (const auto* member : phase.members) {
      const auto slot = member->query.output_index;
      const auto key = take(result_atom_key(member->query));
      if (!requested) {
        ResultProgramNeed need;
        need.tensors.push_back({0, slot, take(Footprint::all({1})), 1});
        replies.push_back({key, Result<ResultProgramPoll>(std::move(need))});
        continue;
      }
      double number = 0;
      check(member->tensors->at({0, slot}).read({0}, &number, sizeof(number)));
      auto builder = take(ResultBuilder::start(
          member->resources, *member->query.output.result_schema,
          member->query.semantic_key));
      check(builder.bind_descriptor_relation(
          take(ResultRelation::cartesian(member->resources, 1, {}))));
      check(builder.publish_tensor(
          0, Region::whole({1}),
          {reinterpret_cast<const std::uint8_t*>(&number), sizeof(number)},
          take(ResultRelation::cartesian(
              member->resources, 1,
              {0, 1, 0, 1, ResultSupportTarget::Tensor, slot})),
          {true, true, true, true}));
      replies.push_back({key, Result<ResultProgramPoll>(ResultPublication{
                                  take(builder.seal()), true})});
    }
    requested = true;
    return Result<ResourceVector<ResultJointOutcome>>(std::move(replies));
  }
};
int tensor_slots() {
  auto counts = std::make_shared<WorkflowCounts>();
  auto registry = std::make_shared<OperationRegistry>();
  auto source = workflow_source(counts, 0, 1);
  auto& schema = *source.traits.outputs[0].result_schema;
  schema.tensors.push_back(schema.tensors[0]);
  schema.tensors.back().key = "other";
  check(registry->register_operation(std::move(source)));
  OperationDefinition pair;
  pair.key = "test.slot_pair";
  pair.traits.input_count = 1;
  pair.traits.input_schema.resize(1);
  pair.traits.input_schema[0].kind = OperationPortKind::Result;
  pair.traits.input_schema[0].result_schema_id = "test.multi_output";
  pair.traits.input_schema[0].result_schema_version = 1;
  pair.traits.outputs = {multi_result::output("left"),
                         multi_result::output("right")};
  pair.traits.joint_contract = 1;
  pair.traits.joint_continuation_bytes = 512;
  pair.traits.joint_workspace_bytes = 4096;
  pair.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<Single>(allocator);
  };
  pair.start_result_joint = [](const auto&, const auto& allocator) {
    return ResultJointContinuation::make<SlotPair>(allocator);
  };
  check(registry->register_operation(std::move(pair)));
  check(registry->freeze());
  WorkflowDocument doc;
  doc.nodes = {{10, "test.workflow_atoms", {}, {}},
               {20, "test.slot_pair", {WorkflowNodeOutput{10, "value"}}, {}}};
  doc.outputs = {{"left", 20, "left"}, {"right", 20, "right"}};
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, {1, false, 16, 1024 * 1024, 0});
  auto root = take(context.resource_budget());
  auto result = context.execute(plan);
  if (!result.ok())
    std::cerr << "tensor-slot workflow: " << result.status().message << '\n';
  PS_CHECK(result.ok());
  PS_CHECK(multi_result::number(result.value().results.at("left")) == 2);
  PS_CHECK(multi_result::number(result.value().results.at("right")) == 3);
  PS_CHECK(counts->starts == 2 && counts->destroys == 2 &&
           counts->singles == 0);
  PS_CHECK(result.value().diagnostics.joint_groups == 3 &&
           result.value().diagnostics.joint_fallbacks == 0);
  PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}

int collector(unsigned mode, unsigned width = 65, bool grouping = true) {
  auto counts = std::make_shared<WorkflowCounts>();
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(workflow_source(counts, mode, width)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{10, "test.workflow_atoms", {}, {}}};
  document.outputs = {{"value", 10, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContextConfig config{1, false, 16, 1024 * 1024, 1024 * 1024};
  config.maximum_dependency_cache_metadata = 1048576;
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  ExecutionOptions options;
  options.enable_joint = grouping;
  options.maximum_dependency_work = 10000000;
  options.maximum_dependency_cache_work = 10000000;
  options.dependencies.sets.maximum_work = 10000000;
  auto all = take(Footprint::all({width}));
  auto result = context.execute_atoms(plan, {}, {{"value", all}}, {}, options);
  if (mode == 2) {
    PS_CHECK(!result.ok() &&
             result.status().detail.origin == FailureOrigin::Protocol &&
             result.status().detail.node_id == 10);
  } else {
    if (!result.ok())
      std::cerr << "Result collector: " << result.status().message << '\n';
    PS_CHECK(result.ok() && result.value().atoms.size() == width);
    for (const auto& atom : result.value().atoms) {
      PS_CHECK(atom.output.node_id == 10 && atom.name == "value");
      if (mode == 1 || mode == 4 || mode == 7 || mode == 9 ||
          ((mode == 3 || mode == 8) && atom.key.coordinate[0] == 1)) {
        PS_CHECK(!atom.outcome.ok() &&
                 atom.outcome.status().detail.node_id == 10);
        if (mode == 1 || mode == 7)
          PS_CHECK(atom.outcome.status().detail.scope ==
                   FailureScope::ValidationDomain);
        if (mode == 4 || mode == 9)
          PS_CHECK(atom.outcome.status().detail.scope == FailureScope::Group &&
                   atom.outcome.status().detail.origin ==
                       FailureOrigin::Backend &&
                   !atom.quality);
        if (mode == 3 || mode == 8)
          PS_CHECK(atom.outcome.status().detail.atom == atom.key &&
                   atom.outcome.status().reason == FailureReason::DivideByZero);
        if (mode == 7)
          PS_CHECK(atom.quality && atom.quality->dimension() == width);
      } else {
        if (!atom.outcome.ok())
          std::cerr << "collector mode=" << mode
                    << " atom=" << atom.key.coordinate[0]
                    << " cause=" << atom.outcome.status().message << '\n';
        PS_CHECK(atom.outcome.ok());
        PS_CHECK(multi_result::number(atom.outcome.value(),
                                      {atom.key.coordinate[0]}) ==
                 atom.key.coordinate[0] + 2);
        auto coverage =
            take(atom.outcome.value().descriptor()).tensor_coverage(0);
        PS_CHECK(take(coverage.element_count()) == 1);
        if (mode == 5)
          PS_CHECK(atom.quality && atom.quality->dimension() == width);
      }
    }
    PS_CHECK(result.value().diagnostics.joint_fallbacks == 0);
    if (mode == 8 || mode == 9) {
      const auto& timings = result.value().diagnostics.operation_timings;
      PS_CHECK(timings.size() == 1);
      PS_CHECK(timings[0].invocation_count == width &&
               timings[0].numeric.evaluated_values == width &&
               timings[0].numeric.strict_math_calls == 2 * width &&
               timings[0].computed_elements == (mode == 8 ? width - 1 : 0));
    }
    if (mode == 8) {
      auto warm =
          take(context.execute_atoms(plan, {}, {{"value", all}}, {}, options));
      PS_CHECK(warm.diagnostics.cache_hits == width - 1);
      PS_CHECK(warm.diagnostics.operation_timings.size() == 1);
      PS_CHECK(warm.diagnostics.operation_timings[0].computed_elements == 0);
      PS_CHECK(
          warm.diagnostics.operation_timings[0].invocation_count == 1 &&
          warm.diagnostics.operation_timings[0].numeric.evaluated_values == 1 &&
          warm.diagnostics.operation_timings[0].numeric.strict_math_calls == 2);
    }
    if (mode == 0) {
      PS_CHECK(counts->starts == (grouping ? (width + 63) / 64 : width));
      PS_CHECK(result.value().dependencies.coverage().at("value") == all);
      auto point =
          take(Footprint::from_regions({width}, {Region({{width - 1, 1}})}));
      auto subset =
          take(result.value().dependencies.restrict({{"value", point}}));
      PS_CHECK(subset.coverage().at("value") == point);
      PS_CHECK(take(subset.source_support()).empty());
      auto warm =
          context.execute_atoms(plan, {}, {{"value", all}}, {}, options);
      PS_CHECK(warm.ok() &&
               counts->starts == (grouping ? (width + 63) / 64 : width) &&
               warm.value().diagnostics.cache_hits == width);
    }
  }
  PS_CHECK(counts->starts == counts->destroys && counts->singles == 0);
  ExecutionOptions limited = options;
  limited.maximum_atom_observations = 0;
  const auto prior = counts->starts;
  auto empty = context.execute_atoms(
      plan, {}, {{"value", take(Footprint::none({width}))}}, {}, limited);
  PS_CHECK(empty.ok() && empty.value().atoms.empty() &&
           counts->starts == prior);
  auto refused = context.execute_atoms(plan, {}, {{"value", all}}, {}, limited);
  PS_CHECK(!refused.ok() &&
           refused.status().code == ErrorCode::ResourceExhausted &&
           counts->starts == prior);
  limited = options;
  limited.dependencies.maximum_stages = 0;
  auto stage = context.execute_atoms(plan, {}, {{"value", all}}, {}, limited);
  PS_CHECK(!stage.ok() && stage.status().code == ErrorCode::ResourceExhausted &&
           counts->starts == prior);
  PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}

struct FailedScalar {
  Result<ResultProgramPoll> poll(const ResultProgramPhase&) {
    return Result<ResultProgramPoll>(
        Status{ErrorCode::OperationFailed,
               "scalar source failed",
               FailureReason::ShortIo,
               {FailureOrigin::Io, FailureScope::Group}});
  }
};
int empty_scalar_guard(bool contract2) {
  auto source_counts = std::make_shared<WorkflowCounts>();
  auto output_counts = std::make_shared<WorkflowCounts>();
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition source;
  source.key = "test.failed_scalar";
  source.traits.outputs = {multi_result::output(
      "value", multi_result::schema(ElementType::Float32, {1}))};
  source.start_result = [source_counts](const auto&, const auto& allocator) {
    ++source_counts->starts;
    return ResultContinuation::make<FailedScalar>(allocator);
  };
  check(registry->register_operation(std::move(source)));
  auto output = workflow_source(output_counts, 0, 4);
  output.key = "test.empty_scalar_guard";
  output.traits.input_count = 1;
  output.traits.input_schema.resize(1);
  auto& input = output.traits.input_schema[0];
  input.kind = OperationPortKind::Result;
  input.result_schema_id = "test.multi_output";
  input.result_schema_version = 1;
  input.scalar_bounds = true;
  input.minimum = 0;
  input.maximum = 1;
  if (!contract2) {
    output.traits.joint_contract = 0;
    output.traits.joint_continuation_bytes = 0;
    output.traits.joint_workspace_bytes = 0;
    output.traits.outputs[0].failure_delivery =
        FailureDelivery::RequestFailureOnly;
    output.start_result_joint = {};
  }
  check(registry->register_operation(std::move(output)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {
      {10, "test.failed_scalar", {}, {}},
      {20, "test.empty_scalar_guard", {WorkflowNodeOutput{10, "value"}}, {}}};
  document.outputs = {{"out", 20, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, {1, false, 16, 1048576, 0});
  auto root = take(context.resource_budget());
  auto frozen = take(context.freeze(plan, {}));
  auto empty =
      context.execute_fragments(frozen, {{"out", take(Footprint::none({4}))}});
  if (contract2) {
    PS_CHECK(empty.ok() && take(empty.value().results.at("out").descriptor())
                               .tensor_coverage(0)
                               .empty());
    PS_CHECK(take(empty.value().dependencies.source_support()).empty());
    PS_CHECK(source_counts->starts == 0 && output_counts->starts == 0 &&
             output_counts->singles == 0);
    auto nonempty = context.execute_fragments(
        frozen,
        {{"out", take(Footprint::from_regions({4}, {Region({{0, 1}})}))}});
    PS_CHECK(!nonempty.ok() &&
             nonempty.status().code == ErrorCode::OperationFailed);
  } else {
    PS_CHECK(!empty.ok() && empty.status().code == ErrorCode::OperationFailed);
  }
  PS_CHECK(source_counts->starts == 1 && output_counts->starts == 0 &&
           output_counts->singles == 0);
  PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
int empty_domain_finality() {
  auto counts = std::make_shared<WorkflowCounts>();
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(workflow_source(counts, 1, 4)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{10, "test.workflow_atoms", {}, {}}};
  document.outputs = {{"a_empty", 10, "value"}, {"z_domain", 10, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, {1, false, 16, 1048576, 0});
  ResultRef captured;
  ExecutionOptions options;
  options.result_publication = [&](ValueRef, const ResultRef& result) {
    captured = result;
    return Status::success();
  };
  auto run = context.execute_fragments(
      take(context.freeze(plan, {})),
      {{"a_empty", take(Footprint::none({4}))},
       {"z_domain", take(Footprint::from_regions({4}, {Region({{0, 1}})}))}},
      {}, options);
  PS_CHECK(captured.valid() &&
           take(captured.descriptor()).tensor_coverage(0).empty());
  PS_CHECK(!run.ok() && run.status().code == ErrorCode::OperationFailed &&
           run.status().detail.origin == FailureOrigin::Domain &&
           run.status().detail.scope == FailureScope::ValidationDomain &&
           run.status().detail.node_id == 10);
  PS_CHECK(counts->starts == 1 && counts->singles == 0);
  return 0;
}

SchemaTemplate field_schema(std::string id, unsigned rows) {
  SchemaTemplate schema;
  schema.id = id;
  schema.fields.push_back(
      {"number", ElementType::Float64, {ResultExtentKind::Fixed, rows}, {}});
  return schema;
}
struct FieldJoint {
  unsigned stage = 0;
  unsigned row;
  explicit FieldJoint(unsigned row = 1) : row(row) {}
  std::optional<ResultBuilder> builder;
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& phase) {
    const auto& member = *phase.members[0];
    Result<ResultProgramPoll> reply(Status{ErrorCode::Internal, {}});
    if (!stage) {
      ResultProgramNeed need;
      need.results.push_back({0, 0, true, 0});
      reply = Result<ResultProgramPoll>(std::move(need));
    } else if (stage == 1) {
      builder = take(ResultBuilder::start(member.resources,
                                          *member.query.output.result_schema,
                                          member.query.semantic_key));
      check(builder->bind_descriptor_relation(
          take(ResultRelation::cartesian(member.resources, 1, {}))));
      auto input = member.results.at(0);
      auto read = take(input.prepare_read(take(input.descriptor()), 0, row, 1));
      ResultProgramNeed need;
      need.io.push_back(std::move(read));
      reply = Result<ResultProgramPoll>(std::move(need));
    } else if (stage == 2) {
      auto bytes = std::get<std::shared_ptr<const CpuStorage>>(member.io[0]);
      ResultProgramNeed need;
      need.io.push_back(take(builder->prepare_append(0, 1, bytes)));
      reply = Result<ResultProgramPoll>(std::move(need));
    } else {
      check(
          builder->publish(0, 1,
                           take(ResultRelation::cartesian(
                               member.resources, 1,
                               {0, 1, row, 1, ResultSupportTarget::Field, 0})),
                           {true, true, true, true}));
      reply = Result<ResultProgramPoll>(
          ResultPublication{take(builder->seal()), true});
    }
    ++stage;
    ResourceVector<ResultJointOutcome> outputs;
    outputs.push_back({take(result_atom_key(member.query)), std::move(reply)});
    return Result<ResourceVector<ResultJointOutcome>>(std::move(outputs));
  }
};
int field_collector(bool computed = false, bool grouping = true) {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition op;
  op.key = "test.field_atom";
  op.traits.input_count = 1;
  op.traits.input_schema.resize(1);
  op.traits.input_schema[0].kind = OperationPortKind::Result;
  op.traits.input_schema[0].result_schema_id = "test.field_input";
  op.traits.input_schema[0].result_schema_version = 1;
  auto output =
      multi_result::output("value", field_schema("test.field_output", 1));
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  output.maximum_dependency_stages = 4;
  op.traits.outputs = {output};
  op.traits.joint_contract = 2;
  op.traits.joint_continuation_bytes = 1024;
  op.traits.joint_workspace_bytes = 4096;
  op.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<Single>(allocator);
  };
  op.start_result_joint = [](const auto&, const auto& allocator) {
    return ResultJointContinuation::make<FieldJoint>(allocator);
  };
  if (computed) {
    auto consumer = op;
    consumer.key = "test.computed_field";
    consumer.traits.input_schema[0].result_schema_id = "test.field_output";
    consumer.traits.outputs[0] =
        multi_result::output("value", field_schema("test.computed_field", 1));
    consumer.traits.outputs[0].failure_delivery =
        FailureDelivery::PerAtomOutcome;
    consumer.traits.outputs[0].maximum_dependency_stages = 4;
    consumer.start_result_joint = [](const auto&, const auto& allocator) {
      return ResultJointContinuation::make<FieldJoint>(allocator, 0);
    };
    check(registry->register_operation(std::move(consumer)));
  }
  check(registry->register_operation(std::move(op)));
  check(registry->freeze());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(
      1, "source", field_schema("test.field_input", 3))};
  document.nodes = {{10, "test.field_atom", {WorkflowInputReference{1}}, {}}};
  if (computed)
    document.nodes.push_back(
        {20, "test.computed_field", {WorkflowNodeOutput{10, "value"}}, {}});
  document.outputs = {{"value", computed ? 20u : 10u, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, {1, false, 16, 1024 * 1024, 0});
  auto root = take(context.resource_budget());
  auto input = take(ResultBuilder::start(
      root, field_schema("test.field_input", 3), "field-input"));
  check(input.bind_descriptor_relation(
      take(ResultRelation::cartesian(root, 1, {}))));
  const double numbers[3] = {5, 7, 9};
  check(input.append(
      0, 3, {reinterpret_cast<const std::uint8_t*>(numbers), sizeof(numbers)}));
  check(input.publish(0, 3, take(ResultRelation::cartesian(root, 3, {})),
                      {true, true, true, true}));
  ExecutionBinding binding;
  binding.name = "source";
  binding.result = take(input.seal());
  ExecutionOptions options;
  options.enable_joint = grouping;
  auto result = context.execute_atoms(
      plan, {{binding}}, {{"value", take(Footprint::all({1}))}}, {}, options);
  if (!result.ok())
    std::cerr << "field collector: " << result.status().message << '\n';
  PS_CHECK(result.ok() && result.value().atoms.size() == 1 &&
           result.value().atoms[0].outcome.ok());
  PS_CHECK(result.value().atoms[0].key == (AtomKey{0, 1, {0}}));
  auto object = result.value().atoms[0].outcome.value();
  auto bytes = take(
      take(object.prepare_read(take(object.descriptor()), 0, 0, 1)).load(8));
  double actual = 0;
  std::memcpy(&actual, bytes->bytes().data(), sizeof(actual));
  PS_CHECK(actual == 7);
  PS_CHECK(result.value().diagnostics.operation_timings.size() ==
           (computed ? 2u : 1u));
  PS_CHECK(
      result.value().diagnostics.operation_timings[0].invocation_count == 4 &&
      result.value().diagnostics.operation_timings[0].computed_elements == 1);
  auto support = take(result.value().dependencies.source_support());
  auto point = take(Footprint::from_regions({3}, {Region({{1, 1}})}));
  PS_CHECK(support.at("source") == point);
  auto dirty = take(result.value().dependencies.potential_dirty(
      "source", point, 1, {}, ResultSupportTarget::Field, 0));
  PS_CHECK(dirty.at("value") == take(Footprint::all({1})));
  auto empty = take(result.value().dependencies.restrict(
      {{"value", take(Footprint::none({1}))}}));
  PS_CHECK(empty.coverage().at("value").empty() &&
           take(empty.source_support()).empty());
  PS_CHECK(take(empty.source_observations()).empty());
  auto direct = context.execute(plan, {{binding}}, {}, options);
  PS_CHECK(direct.ok());
  auto direct_object = direct.value().results.at("value");
  auto direct_bytes = take(take(direct_object.prepare_read(
                                    take(direct_object.descriptor()), 0, 0, 1))
                               .load(8));
  std::memcpy(&actual, direct_bytes->bytes().data(), 8);
  PS_CHECK(actual == 7);
  auto no_atoms = context.execute_atoms(
      plan, {{binding}}, {{"value", take(Footprint::none({1}))}}, {}, options);
  PS_CHECK(no_atoms.ok() && no_atoms.value().atoms.empty());
  return 0;
}

struct ControlJoint {
  bool requested = false;
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& phase) {
    ResourceVector<ResultJointOutcome> replies;
    for (const auto* member : phase.members) {
      const auto key = take(result_atom_key(member->query));
      const auto at = key.coordinate[0];
      if (!requested) {
        ResultProgramNeed need;
        need.tensors.push_back(
            {0, 0, take(Footprint::from_regions({3}, {Region({{at, 1}})})), 2});
        replies.push_back({key, Result<ResultProgramPoll>(std::move(need))});
        continue;
      }
      double value = 0;
      check(member->tensors->at({0, 0}).read({at}, &value, sizeof(value)));
      auto builder = take(ResultBuilder::start(
          member->resources, *member->query.output.result_schema,
          member->query.semantic_key));
      check(builder.bind_descriptor_relation(take(ResultRelation::cartesian(
          member->resources, 1,
          {0, 2, at, 1, ResultSupportTarget::Tensor, 0}))));
      check(builder.publish_tensor(
          0, member->query.tensor_outputs->boxes()[0],
          {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)},
          take(ResultRelation::cartesian(member->resources, 3, {})),
          {true, true, true, true}));
      replies.push_back({key, Result<ResultProgramPoll>(ResultPublication{
                                  take(builder.seal()), true})});
    }
    requested = true;
    return Result<ResourceVector<ResultJointOutcome>>(std::move(replies));
  }
};
int control_empty() {
  auto registry = std::make_shared<OperationRegistry>();
  auto op = workflow_source(std::make_shared<WorkflowCounts>(), 0, 3);
  op.traits.input_count = 1;
  op.traits.input_schema.resize(1);
  op.traits.input_schema[0].kind = OperationPortKind::Result;
  op.traits.input_schema[0].result_schema_id = "test.multi_output";
  op.traits.input_schema[0].result_schema_version = 1;
  op.start_result_joint = [](const auto&, const auto& allocator) {
    return ResultJointContinuation::make<ControlJoint>(allocator);
  };
  check(registry->register_operation(std::move(op)));
  check(registry->freeze());
  WorkflowDocument document;
  auto schema = multi_result::schema(ElementType::Float64, {3});
  document.inputs = {multi_result::declaration(1, "control", schema)};
  document.nodes = {
      {10, "test.workflow_atoms", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"value", 10, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, {1, false, 16, 1024 * 1024, 0});
  auto root = take(context.resource_budget());
  auto result = take(context.execute_atoms(
      plan, {{multi_result::binding(root, "control", 1, schema)}},
      {{"value", take(Footprint::all({3}))}}));
  PS_CHECK(take(result.dependencies.source_support()).at("control") ==
           take(Footprint::all({3})));
  auto point = take(Footprint::from_regions({3}, {Region({{2, 1}})}));
  auto selected = take(result.dependencies.restrict({{"value", point}}));
  PS_CHECK(take(selected.source_support()).at("control") == point);
  auto empty = take(
      result.dependencies.restrict({{"value", take(Footprint::none({3}))}}));
  PS_CHECK(empty.coverage().at("value").empty());
  PS_CHECK(take(empty.source_support()).empty() &&
           take(empty.source_observations()).empty());
  PS_CHECK(take(empty.potential_dirty("control", take(Footprint::all({3})), 2,
                                      {}, ResultSupportTarget::Tensor, 0))
               .at("value")
               .empty());
  return 0;
}
struct GroupedJoint {
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& phase) {
    ResourceVector<ResultJointOutcome> outcomes;
    for (const auto* member : phase.members) {
      auto key = take(result_atom_key(member->query));
      auto builder = take(ResultBuilder::start(
          member->resources, *member->query.output.result_schema,
          member->query.semantic_key));
      check(builder.bind_descriptor_relation(
          take(ResultRelation::cartesian(member->resources, 1, {}))));
      const double values[3] = {static_cast<double>(key.coordinate[0]) + .1,
                                static_cast<double>(key.coordinate[1]) + .2,
                                .3};
      check(builder.publish_tensor(
          0, member->query.tensor_outputs->boxes()[0],
          {reinterpret_cast<const std::uint8_t*>(values), sizeof(values)},
          take(ResultRelation::cartesian(member->resources, 18, {})),
          {true, true, true, true}));
      outcomes.push_back({key, Result<ResultProgramPoll>(ResultPublication{
                                   take(builder.seal()), true})});
    }
    return Result<ResourceVector<ResultJointOutcome>>(std::move(outcomes));
  }
};
int grouped_collector(bool color) {
  auto registry = std::make_shared<OperationRegistry>();
  auto op = workflow_source(std::make_shared<WorkflowCounts>(), 0, 3);
  auto& tensor = op.traits.outputs[0].result_schema->tensors[0];
  tensor.descriptor.shape = {3, 3};
  tensor.batch_axes = {2};
  if (color)
    tensor.facets = {take(encode_color_array(ColorArrayDescriptor{}))};
  else
    tensor.atomic_trailing_axes = 1;
  op.start_result_joint = [](const auto&, const auto& allocator) {
    return ResultJointContinuation::make<GroupedJoint>(allocator);
  };
  check(registry->register_operation(std::move(op)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{10, "test.workflow_atoms", {}, {}}};
  document.outputs = {{"value", 10, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, {1, false, 16, 1024 * 1024, 0});
  // The request selects just one channel; closure still yields complete atoms.
  auto demand = take(
      Footprint::from_regions({2, 3, 3}, {Region({{0, 2}, {1, 2}, {2, 1}})}));
  auto result = context.execute_atoms(plan, {}, {{"value", demand}});
  if (!result.ok())
    std::cerr << "grouped collector: " << result.status().message << '\n';
  PS_CHECK(result.ok() && result.value().atoms.size() == 4);
  for (const auto& atom : result.value().atoms) {
    PS_CHECK(atom.outcome.ok() && atom.key.rank == 2 &&
             atom.key.coordinate[0] < 2 && atom.key.coordinate[1] >= 1 &&
             atom.key.coordinate[1] < 3);
    const auto& object = atom.outcome.value();
    auto facts = take(object.descriptor());
    PS_CHECK(take(facts.tensor_coverage(0).element_count()) == 3);
    for (unsigned channel = 0; channel < 3; ++channel) {
      double actual = multi_result::number(
          object, {atom.key.coordinate[0], atom.key.coordinate[1], channel});
      const double expected =
          channel == 0   ? static_cast<double>(atom.key.coordinate[0]) + .1
          : channel == 1 ? static_cast<double>(atom.key.coordinate[1]) + .2
                         : .3;
      PS_CHECK(actual == expected);
    }
  }
  return 0;
}
struct MetadataConsumer {
  unsigned mode, round = 0;
  bool denied;
  std::optional<ResultTensorInput>* retained;
  MetadataConsumer(unsigned selected, bool reject,
                   std::optional<ResultTensorInput>* escaped)
      : mode(selected), denied(reject), retained(escaped) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    const auto point = take(Footprint::from_regions({4}, {Region({{2, 1}})}));
    if (!round++) {
      ResultProgramNeed need;
      need.tensors.push_back({0, 0,
                              (mode == 1 || mode == 4)
                                  ? take(Footprint::none({4}))
                                  : take(Footprint::all({4})),
                              (mode == 1 || mode == 4) ? 13U : 8U});
      if (mode == 2)
        need.tensors.push_back({0, 0, point, 1});
      return Answer(std::move(need));
    }
    const auto& input = phase.tensors->at({0, 0});
    if (input.spec().sample_shape() != std::vector<std::uint64_t>{4} ||
        input.coverage() !=
            (mode == 2 || round > 2 ? point : take(Footprint::none({4}))) ||
        !input.object_id() || !phase.results.empty())
      return Answer(Status{ErrorCode::OperationFailed,
                           "metadata-only tensor capability facts"});
    *retained = input;
    if (denied) {
      double ignored = 0;
      static_cast<void>(input.read({0}, &ignored, 8));
    }
    if ((mode == 3 || mode == 4) && round == 2) {
      ResultProgramNeed need;
      need.tensors.push_back({0, 0, point, 1});
      return Answer(std::move(need));
    }
    double value = 4;
    if (mode >= 2)
      check(input.read({2}, &value, 8));
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{}));
    check(builder.bind_descriptor_relation(take(ResultRelation::cartesian(
        phase.resources, 1,
        {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0}))));
    check(builder.publish_tensor(
        0, Region::whole({1}),
        {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)},
        take(ResultRelation::cartesian(
            phase.resources, 1,
            {0, 1, 2, mode >= 2 ? 1U : 0U, ResultSupportTarget::Tensor, 0})),
        {true, true, true, true}));
    return Answer(ResultPublication{take(builder.seal()), true});
  }
};
int metadata_tensor_need(unsigned mode, bool denied = false,
                         bool grouping = true, bool failing_source = false) {
  auto counts = std::make_shared<WorkflowCounts>();
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(
      workflow_source(counts, failing_source ? 7 : 0, 4)));
  std::optional<ResultTensorInput> retained;
  OperationDefinition consumer;
  consumer.key = "test.metadata_consumer";
  consumer.traits.input_count = 1;
  consumer.traits.input_schema.resize(1);
  consumer.traits.input_schema[0].kind = OperationPortKind::Result;
  consumer.traits.input_schema[0].element_type =
      static_cast<unsigned>(ElementType::Float64);
  consumer.traits.outputs = {multi_result::output("value")};
  consumer.traits.outputs[0].maximum_dependency_stages = 3;
  consumer.traits.outputs[0].continuation_bytes = sizeof(MetadataConsumer);
  consumer.start_result = [&](const auto&, const auto& allocator) {
    return ResultContinuation::make<MetadataConsumer>(allocator, mode, denied,
                                                      &retained);
  };
  check(registry->register_operation(std::move(consumer)));
  check(registry->freeze());
  ResourceBudget root;
  ResultRef held;
  {
    WorkflowDocument document;
    document.nodes = {
        {10, "test.workflow_atoms", {}, {}},
        {20, "test.metadata_consumer", {WorkflowNodeOutput{10, "value"}}, {}}};
    document.outputs = {{"out", 20, "value"}, {"source", 10, "value"}};
    GraphContext graph(document);
    auto plan = take(Compiler(registry).compile(graph)).plan;
    ExecutionContext context(registry, {1, false, 16, 1048576, 0});
    root = take(context.resource_budget());
    auto frozen = take(context.freeze(plan));
    ExecutionOptions options;
    options.enable_joint = grouping;
    auto run = context.execute_fragments(
        frozen, {{"out", take(Footprint::all({1}))}}, {}, options);
    if (denied) {
      PS_CHECK(!run.ok() &&
               run.status().reason == FailureReason::UnauthorizedRead &&
               run.status().detail.origin == FailureOrigin::Protocol);
    } else if (failing_source && mode >= 2) {
      PS_CHECK(!run.ok() &&
               run.status().detail.scope == FailureScope::ValidationDomain &&
               run.status().reason == FailureReason::InvalidDomain);
    } else {
      if (!run.ok())
        std::cerr << "metadata tensor mode=" << mode << " "
                  << run.status().message << '\n';
      PS_CHECK(run.ok());
      held = run.value().results.at("out");
      PS_CHECK(multi_result::number(held) == 4 &&
               held.association().size() == (mode == 3 || mode == 4 ? 2u : 1u));
    }
    const auto data_calls = mode >= 2 ? 1U : 0U;
    PS_CHECK(counts->starts == data_calls && counts->members == data_calls &&
             counts->singles == 0);
    if (!failing_source || mode < 2) {
      // Metadata-only completion must not finalize the producer's real domain.
      auto actual = context.execute_fragments(
          frozen,
          {{"source", take(Footprint::from_regions({4}, {Region({{0, 1}})}))}},
          {}, options);
      if (failing_source) {
        PS_CHECK(!actual.ok() &&
                 actual.status().detail.scope ==
                     FailureScope::ValidationDomain &&
                 actual.status().reason == FailureReason::InvalidDomain);
      } else {
        PS_CHECK(actual.ok() &&
                 multi_result::number(actual.value().results.at("source"),
                                      {0}) == 2);
      }
      PS_CHECK(counts->starts == data_calls + 1);
    }
  }
  PS_CHECK(retained &&
           retained->spec().sample_shape() == std::vector<std::uint64_t>{4});
  held = {};
  retained.reset();
  for (auto live : root.statistics().live.values)
    PS_CHECK(!live);
  return 0;
}

struct BroadConsumer {
  unsigned mode;
  bool requested = false;
  std::optional<ResultTensorInput>* retained;
  ResultTensorReadWindow* retained_window;
  unsigned* block_calls;
  BroadConsumer(unsigned mode, std::optional<ResultTensorInput>* retained,
                ResultTensorReadWindow* window, unsigned* block_calls)
      : mode(mode),
        retained(retained),
        retained_window(window),
        block_calls(block_calls) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    const bool sparse = mode == 1 || mode == 2;
    auto samples =
        sparse
            ? take(Footprint::from_regions(
                  {65}, {Region({{0, 1}}), Region({{2, 1}}), Region({{4, 1}})}))
            : take(Footprint::all({65}));
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tensors.push_back({0, 0, samples, 1});
      need.tensors.push_back({0, 0,
                              take(Footprint::from_regions(
                                  {65}, {Region({{sparse ? 4u : 64u, 1}})})),
                              4});
      return Answer(std::move(need));
    }
    const auto& input = phase.tensors->at({0, 0});
    if (input.object_id() || input.coverage() != samples ||
        !phase.results.empty() || !phase.association ||
        phase.association->size() != (sparse ? 3u : 65u))
      return Answer(Status{ErrorCode::OperationFailed,
                           "compound grant identity or object permission"});
    for (auto id : *phase.association)
      if (!id)
        return Answer(Status{ErrorCode::OperationFailed,
                             "compound sentinel in association"});
    *retained = input;
    if (mode == 2) {
      double ignored = 0;
      static_cast<void>(input.read({1}, &ignored, 8));
    }
    double total = 0;
    if (mode == 3 || mode == 5) {
      auto window = take(input.acquire(Region::whole({65})));
      std::uint64_t next = 0;
      while (next < 65) {
        auto row = take(window.row_run({next}));
        if (!row.samples || row.samples > 65 - next)
          return Answer(
              Status{ErrorCode::OperationFailed, "compound row exceeds piece"});
        for (std::uint64_t i = 0; i < row.samples; ++i) {
          double value = 0;
          std::memcpy(&value, row.data + i * row.sample_stride_bytes,
                      sizeof(value));
          total += value;
        }
        next += row.samples;
      }
      *retained_window = std::move(window);
    } else if (mode == 4) {
      FootprintLimits limits;
      limits.consume_work = phase.consume_work;
      auto plan = take(FragmentAtlasPlan::prepare(input, {4}, limits));
      auto atlas = take(plan.materialize(input, phase.allocator, limits));
      for (unsigned i = 0; i < 65; ++i) {
        const auto offset = take(atlas.address({i}));
        double value = 0;
        std::memcpy(&value, atlas.payload.bytes().data() + offset,
                    sizeof(value));
        total += value;
      }
    } else {
      check(samples.visit(
          [&](const auto& at) {
            double value = 0;
            auto status = input.read(at, &value, sizeof(value));
            total += value;
            return status;
          },
          65));
    }
    if (mode == 6 || mode == 7) {
      auto incoming =
          multi_result::binding(phase.resources, "incoming", 0).result;
      const auto compute = [&]() -> Result<ResultRef> {
        ++*block_calls;
        double sum = 0;
        for (unsigned i = 0; i < 65; ++i) {
          double value = 0;
          auto status = input.read({i}, &value, 8);
          if (!status.ok())
            return Result<ResultRef>(status);
          sum += value;
        }
        return Result<ResultRef>(
            multi_result::binding(phase.resources, "computed", sum).result);
      };
      auto first = take(phase.block(1, 0, 65, 0, incoming, compute));
      auto second = take(phase.block(1, 0, 65, 0, incoming, compute));
      if (multi_result::number(first) != total ||
          multi_result::number(second) != total)
        return Answer(Status{ErrorCode::OperationFailed, "compound block sum"});
    }
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        {phase.association->begin(), phase.association->end()}));
    check(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(phase.resources, 1, {}))));
    auto relation = take(ResultRelation::cartesian(
        phase.resources, mode == 5 ? 65 : 1,
        {0, 1, 0, 65, ResultSupportTarget::Tensor, 0}));
    if (sparse) {
      std::vector<ResultRelation> parts;
      for (std::uint64_t at : {0u, 2u, 4u})
        parts.push_back(take(ResultRelation::cartesian(
            phase.resources, 1,
            {0, 1, at, 1, ResultSupportTarget::Tensor, 0})));
      relation = take(ResultRelation::unite(phase.resources, parts));
    }
    if (mode == 5) {
      ResultTensorViewTransform transform;
      transform.source_axes.push_back({0, 0, 1, 1});
      check(builder.publish_tensor_view(
          0, Region::whole({65}), *retained_window, transform,
          std::move(relation), {true, true, true, true}));
    } else {
      check(builder.publish_tensor(
          0, Region::whole({1}),
          {reinterpret_cast<const std::uint8_t*>(&total), sizeof(total)},
          std::move(relation), {true, true, true, true}));
    }
    return Answer(ResultPublication{take(builder.seal()), true});
  }
};
SchemaTemplate rich_atom_schema() {
  auto schema = multi_result::schema(ElementType::Float64, {65});
  schema.id = "test.rich_atom";
  schema.fields.push_back(
      {"associated", ElementType::Float64, {ResultExtentKind::Fixed, 1}, {}});
  return schema;
}
struct ResourceOwners {
  std::vector<std::weak_ptr<const CpuStorage>> profiles;
};
struct RichAtomSource {
  std::shared_ptr<WorkflowCounts> counts;
  std::vector<WeakResultRef>* observed;
  ResourceOwners* resource_owners;
  ResourceVector<unsigned> stages;
  ResourceVector<std::optional<ResultBuilder>> builders;
  ResourceVector<std::optional<ResultTensorReadWindow>> windows;
  RichAtomSource(std::shared_ptr<WorkflowCounts> counts,
                 std::vector<WeakResultRef>* observed,
                 ResourceOwners* resource_owners = nullptr)
      : counts(std::move(counts)),
        observed(observed),
        resource_owners(resource_owners),
        stages(65),
        builders(65),
        windows(65) {}
  ~RichAtomSource() noexcept { ++counts->destroys; }
  Result<ResultProgramPoll> member(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    const auto key = take(result_atom_key(phase.query));
    const auto at = key.coordinate[0];
    auto& stage = stages[at];
    if (!stage++) {
      ResultProgramNeed need;
      need.tensors.push_back({0, 0, *phase.query.tensor_outputs, 1});
      return Answer(std::move(need));
    }
    if (stage == 2) {
      const auto& input = phase.tensors->at({0, 0});
      windows[at] = take(input.acquire(phase.query.tensor_outputs->boxes()[0]));
      ResourceBindings resources;
      if (resource_owners) {
        const auto bytes = numeric_fixture::fixture();
        auto profile = take(
            IccProfile::import({bytes.data(), bytes.size()}, phase.resources));
        resource_owners->profiles.push_back(profile.storage());
        resources = take(ResourceBindings::create({profile}, phase.resources));
      }
      builders[at] = take(ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key, {},
          {phase.association->begin(), phase.association->end()}, 128, 128,
          std::move(resources)));
      check(builders[at]->bind_descriptor_relation(
          take(ResultRelation::cartesian(phase.resources, 1, {}))));
      auto buffer = take(phase.allocator.allocate(8));
      const double value = 1000 + at;
      std::memcpy(buffer.data(), &value, sizeof(value));
      ResultProgramNeed need;
      need.io.push_back(
          take(builders[at]->prepare_append(0, 1, std::move(buffer).freeze())));
      return Answer(std::move(need));
    }
    const auto channels =
        phase.query.tensor_outputs->boxes()[0].dimensions().back().extent;
    const auto samples =
        take(phase.query.output.result_schema->tensors[0].sample_count());
    const auto support_begin = resource_owners ? at * channels : at;
    auto relation = take(ResultRelation::cartesian(
        phase.resources, 1,
        {0, 1, support_begin, resource_owners ? channels : 1,
         ResultSupportTarget::Tensor, 0}));
    check(builders[at]->publish(0, 1, std::move(relation),
                                {true, true, true, true}));
    ResultTensorViewTransform identity;
    for (unsigned axis = 0; axis < windows[at]->region().rank(); ++axis)
      identity.source_axes.push_back(
          {static_cast<std::int32_t>(axis), 0, 1, 1});
    check(builders[at]->publish_tensor_view(
        0, phase.query.tensor_outputs->boxes()[0], *windows[at], identity,
        take(ResultRelation::identity(phase.resources, samples, 0, 1,
                                      ResultSupportTarget::Tensor, 0)),
        {true, true, true, true}));
    auto result = take(builders[at]->seal());
    observed->push_back(result.weak());
    return Answer(ResultPublication{std::move(result), true});
  }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ++counts->singles;
    return member(phase);
  }
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& phase) {
    ResourceVector<ResultJointOutcome> outcomes;
    for (const auto* item : phase.members)
      outcomes.push_back({take(result_atom_key(item->query)), member(*item)});
    return Result<ResourceVector<ResultJointOutcome>>(std::move(outcomes));
  }
};
int broad_tensor_need(unsigned mode, bool grouping = true) {
  auto counts = std::make_shared<WorkflowCounts>();
  auto registry = std::make_shared<OperationRegistry>();
  std::vector<WeakResultRef> observed;
  auto source = workflow_source(counts, 0, 65);
  if (mode == 6 || mode == 7)
    source.traits.cacheable = false;
  if (mode == 5) {
    source.traits.workspace_bytes = 8;
    source.traits.input_count = 1;
    source.traits.input_schema.resize(1);
    source.traits.input_schema[0].kind = OperationPortKind::Result;
    source.traits.input_schema[0].result_schema_id = "test.multi_output";
    source.traits.input_schema[0].result_schema_version = 1;
    source.traits.outputs[0] =
        multi_result::output("value", rich_atom_schema());
    source.traits.outputs[0].failure_delivery = FailureDelivery::PerAtomOutcome;
    source.traits.outputs[0].maximum_dependency_stages = 4;
    source.traits.outputs[0].continuation_bytes = sizeof(RichAtomSource);
    source.traits.joint_continuation_bytes = sizeof(RichAtomSource);
    source.start_result = [counts, &observed](const auto&,
                                              const auto& allocator) {
      return ResultContinuation::make<RichAtomSource>(allocator, counts,
                                                      &observed);
    };
    source.start_result_joint = [counts, &observed](const auto&,
                                                    const auto& allocator) {
      ++counts->starts;
      return ResultJointContinuation::make<RichAtomSource>(allocator, counts,
                                                           &observed);
    };
  }
  check(registry->register_operation(std::move(source)));
  std::optional<ResultTensorInput> held_input;
  ResultTensorReadWindow held_window;
  unsigned block_calls = 0;
  OperationDefinition consumer;
  consumer.key = "test.broad_consumer";
  consumer.traits.input_count = 1;
  consumer.traits.input_schema.resize(1);
  consumer.traits.input_schema[0].kind = OperationPortKind::Result;
  consumer.traits.input_schema[0].result_schema_id =
      mode == 5 ? "test.rich_atom" : "test.multi_output";
  consumer.traits.input_schema[0].result_schema_version = 1;
  consumer.traits.workspace_bytes = 65536;
  consumer.traits.outputs[0] = multi_result::output(
      "value",
      multi_result::schema(ElementType::Float64, {mode == 5 ? 65u : 1u}));
  consumer.traits.outputs[0].continuation_bytes = sizeof(BroadConsumer);
  consumer.start_result = [mode, &held_input, &held_window, &block_calls](
                              const auto&, const auto& allocator) {
    return ResultContinuation::make<BroadConsumer>(allocator, mode, &held_input,
                                                   &held_window, &block_calls);
  };
  check(registry->register_operation(std::move(consumer)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {
      {10, "test.workflow_atoms", {}, {}},
      {20, "test.broad_consumer", {WorkflowNodeOutput{10, "value"}}, {}}};
  if (mode == 5) {
    document.inputs = {multi_result::declaration(
        1, "source", multi_result::schema(ElementType::Float64, {65}))};
    document.nodes[0].inputs = {WorkflowInputReference{1}};
  }
  document.outputs = {{"value", 20, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ResourceBudget root;
  ExecutionResult held;
  {
    ExecutionContextConfig config{1, false, 16, 4194304,
                                  mode == 6 || mode == 7 ? 1048576u : 0u};
    ExecutionContext context(registry, config);
    root = take(context.resource_budget());
    ExecutionBindings bindings;
    if (mode == 5) {
      auto builder = take(ResultBuilder::start(
          root, *document.inputs[0].result_schema, "base"));
      check(builder.bind_descriptor_relation(
          take(ResultRelation::cartesian(root, 1, {}))));
      std::array<double, 65> values{};
      for (unsigned i = 0; i < values.size(); ++i)
        values[i] = i + 2;
      check(builder.publish_tensor(
          0, Region::whole({65}),
          {reinterpret_cast<const std::uint8_t*>(values.data()),
           sizeof(values)},
          take(ResultRelation::cartesian(root, 65, {})),
          {true, true, true, true}));
      bindings.inputs = {{"source", take(builder.seal())}};
    }
    ExecutionOptions options;
    options.enable_joint = grouping;
    options.maximum_dependency_work = 10000000;
    options.maximum_dependency_cache_work = mode == 6 ? 2048 : 10000000;
    options.dependencies.sets.maximum_work = 10000000;
    auto run = context.execute(plan, bindings, {}, options);
    if (mode == 2) {
      PS_CHECK(!run.ok() &&
               run.status().reason == FailureReason::UnauthorizedRead &&
               run.status().detail.origin == FailureOrigin::Protocol);
    } else {
      if (!run.ok())
        std::cerr << "broad mode=" << mode
                  << " code=" << static_cast<unsigned>(run.status().code)
                  << " node=" << run.status().detail.node_id << " "
                  << run.status().message << '\n';
      PS_CHECK(run.ok());
      held = run.take_value();
      PS_CHECK(multi_result::number(held.results.at("value")) == (mode == 1 ? 12
                                                                  : mode == 5
                                                                      ? 2
                                                                      : 2210));
      PS_CHECK(held.results.at("value").association().size() ==
               (mode == 1 ? 3u : 65u));
      if (mode == 6 || mode == 7) {
        PS_CHECK(block_calls == (mode == 6 ? 2u : 1u));
        PS_CHECK(held.diagnostics.dependency_cache_work <=
                 options.maximum_dependency_cache_work);
        PS_CHECK(context.cache_statistics().entries == (mode == 6 ? 0u : 1u));
      }
    }
    const auto count = mode == 1 || mode == 2 ? 3u : 65u;
    PS_CHECK(counts->starts == (grouping ? (count + 63) / 64 : count) &&
             counts->singles == 0);
    PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  }
  PS_CHECK(held_input && held_input->object_id() == 0);
  if (mode == 0) {
    CancellationSource cancelled;
    cancelled.cancel();
    auto before = root.statistics().live;
    auto denied = held_input->acquire(Region::whole({65}), cancelled.token());
    PS_CHECK(!denied.ok() && denied.status().code == ErrorCode::Cancelled);
    PS_CHECK(root.statistics().live.values == before.values);
    const auto available = root.available_capacity()[ResourceKind::Host];
    const auto metadata = ResourceBudget::lease_metadata_bytes();
    auto fill =
        take(root.reserve(ResourceCapacity::host(available - metadata)));
    auto refused = held_input->acquire(Region::whole({65}));
    PS_CHECK(!refused.ok() &&
             refused.status().code == ErrorCode::ResourceExhausted);
    fill = {};
    PS_CHECK(root.statistics().live.values == before.values);
    auto recovered = take(held_input->acquire(Region::whole({65})));
    auto last = take(recovered.row_run({64}));
    double value = 0;
    std::memcpy(&value, last.data, sizeof(value));
    PS_CHECK(value == 66);
  }
  double last = 0;
  check(held_input->read({mode == 1 || mode == 2 ? 4u : 64u}, &last, 8));
  PS_CHECK(last == (mode == 1 || mode == 2 ? 6 : 66));
  if (held_window.valid()) {
    auto row = take(held_window.row_run({64}));
    std::memcpy(&last, row.data, sizeof(last));
    PS_CHECK(last == 66 && row.samples == 1);
  }
  held_input.reset();
  held_window = {};
  if (mode == 5) {
    PS_CHECK(observed.size() == 65);
    for (const auto& weak : observed) {
      auto object = weak.lock();
      PS_CHECK(object.valid());
      auto facts = take(object.descriptor());
      PS_CHECK(facts.field_count() == 1 && facts.rows(0) == 1);
      auto data = take(take(object.prepare_read(facts, 0, 0, 1)).load(8));
      std::memcpy(&last, data->bytes().data(), sizeof(last));
      const auto at =
          facts.tensor_coverage(0).boxes()[0].dimensions()[0].offset;
      PS_CHECK(last == 1000 + at);
    }
    PS_CHECK(multi_result::number(held.results.at("value"), {64}) == 66);
  }
  held = {};
  for (const auto& weak : observed)
    PS_CHECK(!weak.lock().valid());
  for (auto live : root.statistics().live.values)
    PS_CHECK(!live);
  return 0;
}

struct ResourceViewConsumer {
  bool requested = false;
  std::optional<ResultTensorInput>* input;
  ResultTensorReadWindow* window;
  ResourceViewConsumer(std::optional<ResultTensorInput>* input,
                       ResultTensorReadWindow* window)
      : input(input), window(window) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tensors.push_back({0, 0, take(Footprint::all({65, 4})), 1});
      return Answer(std::move(need));
    }
    *input = phase.tensors->at({0, 0});
    *window = take(input->value().acquire(Region::whole({65, 4})));
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        {phase.association->begin(), phase.association->end()}, 128, 128,
        phase.query.resources));
    check(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(phase.resources, 1, {}))));
    ResultTensorViewTransform identity;
    identity.source_axes = {{0, 0, 1, 1}, {1, 0, 1, 1}};
    check(builder.publish_tensor_view(
        0, Region::whole({65, 4}), *window, identity,
        take(ResultRelation::identity(phase.resources, 260, 0, 1,
                                      ResultSupportTarget::Tensor, 0)),
        {true, true, true, true}));
    return Answer(ResultPublication{take(builder.seal()), true});
  }
};
int compound_profile_owners(bool grouping, bool exhaust = false,
                            bool work_exhaust = false) {
  ResourceBudget profile_root;
  ResourceBudget root;
  ResourceOwners owners;
  std::vector<WeakResultRef> observed;
  std::optional<ResultTensorInput> retained_input;
  ResultTensorReadWindow retained_window;
  ResultRef output;
  ColorProfileIdentity identity;
  std::size_t profile_bytes = 0;
  std::uintptr_t base_last_row = 0;
  {
    const auto bytes = numeric_fixture::fixture();
    auto profile =
        take(IccProfile::import({bytes.data(), bytes.size()}, profile_root));
    identity = profile.identity();
    profile_bytes = profile.bytes().size();
    auto resources = take(ResourceBindings::create({profile}, profile_root));
    auto schema = multi_result::schema(ElementType::Float64, {65, 4});
    schema.tensors[0].atomic_trailing_axes = 1;
    TensorDescription description;
    description.channel_axis = 1;
    description.model = "cmyk";
    description.profile = identity;
    description.channels = {{"C", "cyan", "relative"},
                            {"M", "magenta", "relative"},
                            {"Y", "yellow", "relative"},
                            {"K", "black", "relative"}};
    schema.tensors[0].facets = {take(encode_tensor_description(description))};
    auto rich = schema;
    rich.id = "test.resource_atom";
    rich.fields.push_back(
        {"associated", ElementType::Float64, {ResultExtentKind::Fixed, 1}, {}});
    auto counts = std::make_shared<WorkflowCounts>();
    auto registry = std::make_shared<OperationRegistry>();
    auto source = workflow_source(counts, 0, 65);
    source.traits.workspace_bytes = 8;
    source.traits.input_count = 1;
    source.traits.input_schema.resize(1);
    source.traits.input_schema[0].kind = OperationPortKind::Result;
    source.traits.input_schema[0].result_schema_id = "test.multi_output";
    source.traits.input_schema[0].result_schema_version = 1;
    source.traits.outputs[0] = multi_result::output("value", rich);
    source.traits.outputs[0].failure_delivery = FailureDelivery::PerAtomOutcome;
    source.traits.outputs[0].maximum_dependency_stages = 4;
    source.traits.outputs[0].continuation_bytes = sizeof(RichAtomSource);
    source.traits.joint_continuation_bytes = sizeof(RichAtomSource);
    source.start_result = [](const auto&,
                             const auto&) -> Result<ResultContinuation> {
      return Result<ResultContinuation>(
          Status{ErrorCode::Internal, "C2 source used scalar start"});
    };
    source.start_result_joint = [counts, &observed, &owners](
                                    const auto&, const auto& allocator) {
      ++counts->starts;
      return ResultJointContinuation::make<RichAtomSource>(allocator, counts,
                                                           &observed, &owners);
    };
    check(registry->register_operation(std::move(source)));
    OperationDefinition consumer;
    consumer.key = "test.resource_view";
    consumer.traits.input_count = 1;
    consumer.traits.input_schema.resize(1);
    consumer.traits.input_schema[0].kind = OperationPortKind::Result;
    consumer.traits.input_schema[0].result_schema_id = "test.resource_atom";
    consumer.traits.input_schema[0].result_schema_version = 1;
    consumer.traits.outputs[0] = multi_result::output("value", schema);
    consumer.traits.outputs[0].continuation_bytes =
        sizeof(ResourceViewConsumer);
    consumer.start_result = [&retained_input, &retained_window](
                                const auto&, const auto& allocator) {
      return ResultContinuation::make<ResourceViewConsumer>(
          allocator, &retained_input, &retained_window);
    };
    check(registry->register_operation(std::move(consumer)));
    check(registry->freeze());
    WorkflowDocument document;
    document.inputs = {multi_result::declaration(1, "source", schema)};
    document.nodes = {
        {10, "test.workflow_atoms", {WorkflowInputReference{1}}, {}},
        {20, "test.resource_view", {WorkflowNodeOutput{10, "value"}}, {}}};
    document.outputs = {{"value", 20, "value"}};
    GraphContext graph(document);
    auto plan = take(Compiler(registry).compile(graph, {}, resources)).plan;
    {
      ExecutionContextConfig config{1, false, 16, 4194304, 0};
      config.managed_resources = ResourceLimits{};
      config.managed_resources->capacity[ResourceKind::Host] = 4194304;
      config.managed_resources->capacity[ResourceKind::Metadata] = 4194304;
      config.managed_resources->capacity[ResourceKind::Payload] =
          (exhaust ? 1 : 65) * profile_bytes + 260 * 8 + 8192;
      ExecutionContext context(registry, config);
      root = take(context.resource_budget());
      auto builder = take(ResultBuilder::start(root, schema, "base", {}, {},
                                               128, 128, resources));
      check(builder.bind_descriptor_relation(
          take(ResultRelation::cartesian(root, 1, {}))));
      std::array<double, 260> values{};
      for (unsigned i = 0; i < values.size(); ++i)
        values[i] = i + 1;
      check(builder.publish_tensor(
          0, Region::whole({65, 4}),
          {reinterpret_cast<const std::uint8_t*>(values.data()),
           sizeof(values)},
          take(ResultRelation::cartesian(root, 260, {})),
          {true, true, true, true}));
      ExecutionBindings bindings{{{"source", take(builder.seal())}}};
      {
        const auto& base = bindings.inputs[0].result;
        auto window = take(base.acquire_tensor(take(base.descriptor()), 0,
                                               Region({{64, 1}, {0, 4}})));
        base_last_row = reinterpret_cast<std::uintptr_t>(
            take(window.row_run({64, 0})).data);
      }
      ExecutionOptions options;
      options.enable_joint = grouping;
      options.maximum_dependency_work = work_exhaust ? 10000000 : 100000000;
      options.dependencies.sets.maximum_work = 100000000;
      auto run = context.execute(plan, bindings, {}, options);
      if (exhaust || work_exhaust) {
        PS_CHECK(!run.ok() &&
                 run.status().code == ErrorCode::ResourceExhausted);
        PS_CHECK(!owners.profiles.empty() && owners.profiles.size() < 65);
        if (work_exhaust)
          PS_CHECK(run.status().reason == FailureReason::WorkLimit &&
                   run.status().message == "structured Run work exhausted");
      } else {
        if (!run.ok())
          std::cerr << "resource compound code="
                    << static_cast<unsigned>(run.status().code)
                    << " reason=" << static_cast<unsigned>(run.status().reason)
                    << " scope="
                    << static_cast<unsigned>(run.status().detail.scope)
                    << " node=" << run.status().detail.node_id
                    << " profiles=" << owners.profiles.size()
                    << " objects=" << observed.size() << " host_peak="
                    << root.statistics().peak[ResourceKind::Host]
                    << " payload_peak="
                    << root.statistics().peak[ResourceKind::Payload] << " "
                    << run.status().message << '\n';
        PS_CHECK(run.ok());
        output = run.value().results.at("value");
        PS_CHECK(counts->starts == (grouping ? 2u : 65u));
        PS_CHECK(output.association().size() == 65);
        PS_CHECK(retained_input && !retained_input->object_id());
      }
    }
  }
  if (!exhaust && !work_exhaust) {
    PS_CHECK(owners.profiles.size() == 65 && observed.size() == 65);
    std::set<const CpuStorage*> storage, retained_profiles;
    for (const auto& weak : owners.profiles) {
      auto owner = weak.lock();
      PS_CHECK(owner && owner->bytes().size() == profile_bytes);
      storage.insert(owner.get());
    }
    PS_CHECK(storage.size() == 65);
    retained_input.reset();
    retained_window = {};
    for (const auto& weak : observed) {
      auto source = weak.lock();
      PS_CHECK(source.valid());
      auto retained_profile = take(source.resources().icc_profile(identity));
      PS_CHECK(storage.count(retained_profile.storage().get()) == 1);
      retained_profiles.insert(retained_profile.storage().get());
      auto facts = take(source.descriptor());
      auto field = take(take(source.prepare_read(facts, 0, 0, 1)).load(8));
      double number = 0;
      std::memcpy(&number, field->bytes().data(), 8);
      PS_CHECK(number ==
               1000 +
                   facts.tensor_coverage(0).boxes()[0].dimensions()[0].offset);
    }
    PS_CHECK(retained_profiles.size() == 65);
    PS_CHECK(multi_result::number(output, {64, 3}) == 260);
    auto final_window = take(output.acquire_tensor(take(output.descriptor()), 0,
                                                   Region({{64, 1}, {0, 4}})));
    output = {};
    for (const auto& weak : owners.profiles)
      PS_CHECK(!weak.expired());
    auto row = take(final_window.row_run({64, 0}));
    PS_CHECK(reinterpret_cast<std::uintptr_t>(row.data) == base_last_row);
    double last = 0;
    std::memcpy(&last, row.data + 3 * row.sample_stride_bytes, 8);
    PS_CHECK(last == 260);
    final_window = {};
  }
  for (const auto& weak : owners.profiles)
    PS_CHECK(weak.expired());
  for (const auto& weak : observed)
    PS_CHECK(!weak.lock().valid());
  for (auto live : root.statistics().live.values)
    PS_CHECK(!live);
  for (auto live : profile_root.statistics().live.values)
    PS_CHECK(!live);
  return 0;
}

}  // namespace
int main() try {
  PS_CHECK(compound_profile_owners(true) == 0);
  PS_CHECK(compound_profile_owners(false) == 0);
  PS_CHECK(compound_profile_owners(true, true) == 0);
  PS_CHECK(compound_profile_owners(false, false, true) == 0);
  for (bool grouping : {false, true}) {
    for (unsigned mode = 0; mode < 4; ++mode)
      PS_CHECK(metadata_tensor_need(mode, false, grouping) == 0);
    PS_CHECK(metadata_tensor_need(0, true, grouping) == 0);
    PS_CHECK(metadata_tensor_need(1, true, grouping) == 0);
    PS_CHECK(metadata_tensor_need(0, false, grouping, true) == 0);
    PS_CHECK(metadata_tensor_need(1, false, grouping, true) == 0);
    PS_CHECK(metadata_tensor_need(3, false, grouping, true) == 0);
    PS_CHECK(metadata_tensor_need(4, false, grouping) == 0);
    PS_CHECK(metadata_tensor_need(4, false, grouping, true) == 0);
  }
  for (unsigned mode = 0; mode <= 7; ++mode)
    PS_CHECK(broad_tensor_need(mode) == 0);
  PS_CHECK(broad_tensor_need(0, false) == 0);
  PS_CHECK(control_empty() == 0);
  PS_CHECK(empty_scalar_guard(true) == 0);
  PS_CHECK(empty_scalar_guard(false) == 0);
  PS_CHECK(empty_domain_finality() == 0);
  PS_CHECK(grouped_collector(false) == 0);
  PS_CHECK(grouped_collector(true) == 0);
  PS_CHECK(field_collector() == 0);
  PS_CHECK(field_collector(true) == 0);
  PS_CHECK(field_collector(true, false) == 0);
  for (unsigned mode = 0; mode <= 7; ++mode)
    if (mode != 6)
      PS_CHECK(collector(mode) == 0);
  PS_CHECK(collector(0, 3, false) == 0);
  PS_CHECK(collector(8) == 0);
  PS_CHECK(collector(8, 3, false) == 0);
  PS_CHECK(collector(9) == 0);
  PS_CHECK(collector(9, 3, false) == 0);
  PS_CHECK(tensor_slots() == 0);
  PS_CHECK(workflow(0, true) == 0);
  PS_CHECK(workflow(0, true, false) == 0);
  PS_CHECK(workflow(6, true) == 0);
  for (unsigned mode = 0; mode <= 7; ++mode)
    if (mode != 6)
      PS_CHECK(workflow(mode, false) == 0);
  PS_CHECK(grouped_key() == 0);
  PS_CHECK(color_key() == 0);
  PS_CHECK(direct(0, 1) == 0);
  for (unsigned mode = 0; mode <= 26; ++mode)
    PS_CHECK(direct(mode) == 0);
  std::optional<QualityReport> quality;
  ResultRef result;
  PS_CHECK(direct(7, 3, &quality, &result) == 0);
  PS_CHECK(quality && quality->dimension() == 4 &&
           quality->error_bound() == 0 && take(quality->proof_row(0))[1] == 2);
  double number = 0;
  check(result.read_tensor(take(result.descriptor()), 0, {0}, &number, 8));
  PS_CHECK(number == 2);
  return 0;
} catch (const multi_result::Failure& failure) {
  std::cerr << failure.what() << '\n';
  return 1;
}
