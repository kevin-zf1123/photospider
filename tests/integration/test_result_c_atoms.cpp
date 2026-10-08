#include <dlfcn.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct Module {
  explicit Module(const char* path = PS_RESULT_ATOMS_FIXTURE)
      : handle(dlopen(path, RTLD_NOW | RTLD_LOCAL)) {}
  void* handle;
  ~Module() {
    if (handle)
      dlclose(handle);
  }
  std::array<std::uint64_t, 4> counts() const {
    auto fn = reinterpret_cast<void (*)(std::uint64_t*)>(
        dlsym(handle, "ps_result_atoms_counts"));
    if (!fn)
      throw std::runtime_error("missing atom counters");
    std::array<std::uint64_t, 4> result{};
    fn(result.data());
    return result;
  }
};
int direct(unsigned mode, bool cancelled_waiting = false,
           std::optional<QualityReport>* retained_quality = nullptr,
           ResultRef* retained_result = nullptr, unsigned member_count = 3,
           unsigned width = 3, const char* path = PS_RESULT_ATOMS_FIXTURE) {
  Module module(path);
  PS_CHECK(module.handle);
  ResourceLimits limits;
  if (mode == 24)
    limits.maximum_work = 100000;
  ResourceBudget root(limits);
  ResourceAllocationScope scope(root);
  OperationRegistry registry;
  check(registry.load_plugin(path));
  auto traits = take(registry.find_traits("fixture.result_atoms"));
  PS_CHECK(traits.joint_contract == 2 && traits.outputs[0].failure_delivery ==
                                             FailureDelivery::PerAtomOutcome);
  OperationMetadata input;
  input.result_schema = std::make_shared<const SchemaTemplate>(
      multi_result::schema(ElementType::Float64, {width}));
  std::vector<OperationMetadata> inputs{input};
  std::map<std::string, ParameterValue> parameters{
      {"mode", static_cast<std::int64_t>(mode)}};
  auto outputs = take(infer_operation_outputs(traits, inputs, parameters));
  ResultProgramMetadata metadata{inputs, outputs[0]};
  ResourceVector<ResultProgramQuery> queries;
  queries.reserve(member_count);
  std::vector<std::string> names;
  names.reserve(member_count);
  for (unsigned i = 0; i < member_count; ++i)
    names.push_back("C-atom-" + std::to_string(i));
  CancellationSource cancel;
  for (unsigned i = 0; i < member_count; ++i) {
    queries.emplace_back(metadata, parameters);
    queries.back().semantic_key = names[i];
    queries.back().snapshot_identity = "C-coordinate-inputs";
    queries.back().tensor_outputs =
        take(Footprint::from_regions({width}, {Region({{i, 1}})}));
    if (!i && (mode == 16 || mode == 26 || mode == 32 || cancelled_waiting))
      queries.back().cancellation = cancel.token();
  }
  const auto before = module.counts();
  if (member_count > 1) {
    auto duplicate = queries;
    duplicate[1].tensor_outputs = duplicate[0].tensor_outputs;
    PS_CHECK(
        !registry.start_result_joint("fixture.result_atoms", duplicate, root)
             .ok());
    PS_CHECK(module.counts() == before);
  }
  auto state =
      take(registry.start_result_joint("fixture.result_atoms", queries, root));
  auto allocator = root.allocator();
  auto work = [&](std::uint64_t count) { return root.consume({count}); };
  auto member_work = [&](std::uint64_t count) {
    if ((mode == 26 || mode == 32) && count == 77) {
      cancel.cancel();
      return Status{ErrorCode::Cancelled, {}};
    }
    if (mode == 33 && count == 77)
      return Status{ErrorCode::Cancelled,
                    "whole run cancel",
                    FailureReason::Cancelled,
                    {FailureOrigin::Cancellation, FailureScope::Run}};
    if (mode == 30 && count == 77)
      return Status{ErrorCode::ResourceExhausted,
                    "first member work",
                    FailureReason::WorkLimit,
                    {FailureOrigin::Resource, FailureScope::Run}};
    return work(count);
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
                      allocator,
                      root,
                      i == 0 ? std::function<Status(std::uint64_t)>(member_work)
                             : std::function<Status(std::uint64_t)>(work),
                      {}});
  for (const auto& phase : phases)
    ready.push_back(&phase);
  if (mode == 16)
    cancel.cancel();
  auto response = state.poll({ready, allocator, work});
  if (mode == 6 || mode == 17 || mode == 18) {
    if (!response.ok())
      std::cerr << "first Need mode " << mode << ": "
                << response.status().message << '\n';
    PS_CHECK(response.ok() && response.value().size() == member_count);
    for (const auto& out : response.value())
      PS_CHECK(out.outcome.ok() &&
               std::holds_alternative<ResultProgramNeed>(out.outcome.value()) &&
               !out.quality);
    ready.erase(ready.begin());
    if (cancelled_waiting)
      cancel.cancel();
    response = state.poll({ready, allocator, work});
  }
  const bool protocol = mode == 2 || mode == 3 || mode == 5 || mode == 7 ||
                        mode == 8 || mode == 9 || mode == 10 || mode == 11 ||
                        mode == 12 || mode == 13 || mode == 14 || mode == 15 ||
                        mode == 17 || mode == 18 || mode == 20 || mode == 22 ||
                        mode == 25 || mode == 28 || mode == 29 || mode == 31 ||
                        mode == 32;
  if (protocol) {
    if (response.ok() ||
        response.status().detail.origin != FailureOrigin::Protocol)
      std::cerr << "protocol mode " << mode << ": " << response.status().message
                << '\n';
    PS_CHECK(!response.ok() &&
             response.status().detail.origin == FailureOrigin::Protocol);
    auto again = state.poll({ready, allocator, work});
    PS_CHECK(!again.ok() &&
             again.status().message == response.status().message);
    if (mode == 5 || mode == 8 || mode == 9 || mode == 17 || mode == 25 ||
        mode == 28 || mode == 29)
      PS_CHECK(response.status().reason == FailureReason::InvalidQuality);
  } else if (mode == 33) {
    PS_CHECK(!response.ok() && response.status().code == ErrorCode::Cancelled &&
             response.status().detail.origin == FailureOrigin::Cancellation &&
             response.status().detail.scope == FailureScope::Run &&
             response.status().reason == FailureReason::Cancelled &&
             response.status().message == "whole run cancel");
  } else if (mode == 23 || mode == 24 || mode == 30) {
    PS_CHECK(!response.ok() && response.status().code ==
                                   (mode == 23 ? ErrorCode::BackendUnavailable
                                               : ErrorCode::ResourceExhausted));
    if (mode == 24 || mode == 30)
      PS_CHECK(response.status().reason == FailureReason::WorkLimit);
    if (mode == 30)
      PS_CHECK(response.status().message == "first member work" &&
               response.status().detail.origin == FailureOrigin::Resource &&
               response.status().detail.scope == FailureScope::Run);
  } else {
    if (!response.ok())
      std::cerr << "C atom mode " << mode << ": " << response.status().message
                << '\n';
    PS_CHECK(response.ok() && response.value().size() == member_count);
    for (const auto& out : response.value()) {
      const auto at = out.key.coordinate[0];
      PS_CHECK(out.key.canonical() && !out.key.output_index);
      if (!at &&
          (mode == 16 || mode == 26 || mode == 32 || cancelled_waiting)) {
        PS_CHECK(!out.outcome.ok() &&
                 out.outcome.status().code == ErrorCode::Cancelled &&
                 !out.quality);
      } else if (((mode == 1 || mode == 34) && at == 1) || mode == 6 ||
                 mode == 21) {
        const auto& status = out.outcome.status();
        PS_CHECK(!out.outcome.ok() &&
                 status.code == ErrorCode::InvalidArgument &&
                 status.detail.origin == FailureOrigin::Domain &&
                 status.message == "C local failure" &&
                 !status.detail.node_id && !status.detail.input_id);
        PS_CHECK(status.detail.scope == (mode == 1 || mode == 34
                                             ? FailureScope::Atom
                                             : FailureScope::ValidationDomain));
        if (mode == 1 || mode == 34)
          PS_CHECK(status.detail.atom == out.key &&
                   status.reason == FailureReason::DivideByZero);
        else
          PS_CHECK(status.detail.domain &&
                   status.detail.domain->extent[0] == 3);
        if (mode == 6 || mode == 34)
          PS_CHECK(out.quality &&
                   out.quality->evidence() == QualityEvidence::Measured &&
                   out.quality->residual() == .25);
      } else {
        PS_CHECK(out.outcome.ok());
        const auto& publication =
            std::get<ResultPublication>(out.outcome.value());
        PS_CHECK(multi_result::number(publication.result, {at}) == at + 2);
        if (mode == 4 || mode == 16) {
          PS_CHECK(out.quality && out.quality->error_bound() == 0 &&
                   out.quality->snapshot() == "integer-system" &&
                   take(out.quality->proof_row(at))[1] ==
                       static_cast<std::int64_t>(at + 2));
          if (retained_quality && retained_result && at == 0) {
            *retained_quality = out.quality;
            *retained_result = publication.result;
          }
        } else if (mode == 19 || mode == 27 || mode == 34) {
          PS_CHECK(out.quality && !out.quality->error_bound() &&
                   out.quality->residual() == .25);
        } else {
          PS_CHECK(!out.quality);
        }
      }
    }
    for (std::size_t i = 0; i < response.value().size(); ++i)
      for (std::size_t j = i + 1; j < response.value().size(); ++j)
        PS_CHECK(response.value()[i].key != response.value()[j].key);
  }
  state = {};
  const auto after = module.counts();
  PS_CHECK(after[0] == before[0] + 1 && after[1] == before[1] + 1 &&
           after[3] == before[3]);
  PS_CHECK(after[2] ==
           before[2] + (mode == 6 || mode == 17 || mode == 18 ? 2 : 1));
  PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
struct CompoundSource {
  std::array<bool, 65> requested{};
  std::vector<WeakResultRef>* observed;
  explicit CompoundSource(std::vector<WeakResultRef>* observed)
      : observed(observed) {}
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& phase) {
    ResourceVector<ResultJointOutcome> outcomes;
    for (const auto* member : phase.members) {
      auto key = take(result_atom_key(member->query));
      if (!requested[key.coordinate[0]]) {
        requested[key.coordinate[0]] = true;
        ResultProgramNeed need;
        need.tensors.push_back({0, 0, *member->query.tensor_outputs, 1});
        outcomes.push_back({key, Result<ResultProgramPoll>(std::move(need))});
        continue;
      }
      auto source = take(member->tensors->at({0, 0}).acquire(
          member->query.tensor_outputs->boxes()[0]));
      auto builder = take(ResultBuilder::start(
          member->resources, *member->query.output.result_schema,
          member->query.semantic_key, {},
          {member->association->begin(), member->association->end()}));
      check(builder.bind_descriptor_relation(
          take(ResultRelation::cartesian(member->resources, 1, {}))));
      ResultTensorViewTransform transform;
      transform.source_axes.push_back({0, 0, 1, 1});
      check(builder.publish_tensor_view(
          0, member->query.tensor_outputs->boxes()[0], source, transform,
          take(ResultRelation::identity(member->resources, 65, 0, 1,
                                        ResultSupportTarget::Tensor, 0)),
          {true, true, true, true}));
      auto result = take(builder.seal());
      observed->push_back(result.weak());
      outcomes.push_back({key, Result<ResultProgramPoll>(ResultPublication{
                                   std::move(result), true})});
    }
    return Result<ResourceVector<ResultJointOutcome>>(std::move(outcomes));
  }
};
int compound_transport(unsigned mode) {
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->load_plugin(PS_RESULT_ATOMS_FIXTURE));
  unsigned starts = 0, singles = 0;
  std::vector<WeakResultRef> observed;
  OperationDefinition source;
  source.key = "test.compound_source";
  source.traits.input_count = 1;
  source.traits.input_schema.resize(1);
  source.traits.input_schema[0].kind = OperationPortKind::Result;
  source.traits.input_schema[0].result_schema_id = "test.multi_output";
  source.traits.input_schema[0].result_schema_version = 1;
  auto schema = multi_result::schema(ElementType::Float64, {65});
  source.traits.outputs = {multi_result::output("value", schema)};
  source.traits.outputs[0].failure_delivery = FailureDelivery::PerAtomOutcome;
  source.traits.outputs[0].maximum_dependency_stages = 3;
  source.traits.joint_contract = 2;
  source.traits.joint_continuation_bytes = sizeof(CompoundSource);
  source.traits.joint_workspace_bytes = 16384;
  source.start_result = [&singles](const auto&, const auto& allocator) {
    ++singles;
    return ResultContinuation::make<multi_result::Program>(allocator, 0);
  };
  source.start_result_joint = [&starts, &observed](const auto&,
                                                   const auto& allocator) {
    ++starts;
    return ResultJointContinuation::make<CompoundSource>(allocator, &observed);
  };
  check(registry->register_operation(std::move(source)));
  check(registry->freeze());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "source", schema)};
  document.nodes = {
      {10, "test.compound_source", {WorkflowInputReference{1}}, {}},
      {20,
       "fixture.compound_tensor",
       {WorkflowNodeOutput{10, "value"}},
       {{"mode", static_cast<std::int64_t>(mode)}}}};
  document.outputs = {{"value", 20, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ResourceBudget root;
  ExecutionResult held;
  {
    ExecutionContextConfig config{1, false, 16, 4194304, 0};
    ExecutionContext context(registry, config);
    root = take(context.resource_budget());
    auto builder = take(ResultBuilder::start(root, schema, "source"));
    check(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(root, 1, {}))));
    std::array<double, 65> numbers{};
    for (unsigned i = 0; i < numbers.size(); ++i)
      numbers[i] = i + 2;
    check(builder.publish_tensor(
        0, Region::whole({65}),
        {reinterpret_cast<const std::uint8_t*>(numbers.data()),
         sizeof(numbers)},
        take(ResultRelation::cartesian(root, 65, {})),
        {true, true, true, true}));
    ExecutionOptions options;
    options.maximum_dependency_work = 10000000;
    options.maximum_dependency_cache_work = 10000000;
    options.dependencies.sets.maximum_work = 10000000;
    auto run = context.execute(plan, {{{"source", take(builder.seal())}}}, {},
                               options);
    if (mode == 2 || mode == 3) {
      PS_CHECK(!run.ok() && run.status().code == ErrorCode::InvalidArgument &&
               run.status().reason == FailureReason::UnauthorizedRead &&
               run.status().detail.origin == FailureOrigin::Protocol &&
               run.status().detail.scope == FailureScope::Group &&
               run.status().detail.node_id == 20);
    } else {
      if (!run.ok())
        std::cerr << "C compound mode=" << mode << " " << run.status().message
                  << '\n';
      PS_CHECK(run.ok());
      held = run.take_value();
      PS_CHECK(multi_result::number(held.results.at("value")) == (mode == 1 ? 12
                                                                  : mode == 5
                                                                      ? 66
                                                                      : 2210));
      auto association = held.results.at("value").association();
      PS_CHECK(association.size() >= (mode == 1 ? 3u : 65u));
      for (auto id : association)
        PS_CHECK(id);
      if (mode == 4)
        PS_CHECK(association.size() <= 66);
    }
    PS_CHECK(starts == (mode == 1 || mode == 2 ? 1u
                        : mode == 4            ? 3u
                                               : 2u) &&
             !singles);
    PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  }
  if (mode == 5) {
    PS_CHECK(observed.size() == 65);
    for (const auto& weak : observed)
      PS_CHECK(weak.lock().valid());
    PS_CHECK(multi_result::number(held.results.at("value")) == 66);
  }
  held = {};
  for (const auto& weak : observed)
    PS_CHECK(!weak.lock().valid());
  for (auto live : root.statistics().live.values)
    PS_CHECK(!live);
  return 0;
}

}  // namespace
int main() try {
  for (unsigned mode = 0; mode <= 5; ++mode)
    PS_CHECK(compound_transport(mode) == 0);
  for (unsigned mode = 0; mode <= 26; ++mode)
    PS_CHECK(direct(mode) == 0);
  PS_CHECK(direct(6, true) == 0);
  {
    Module keep_fixture_loaded;
    PS_CHECK(keep_fixture_loaded.handle);
    PS_CHECK(direct(27) == 0);
    PS_CHECK(direct(28) == 0);
  }
  PS_CHECK(direct(29) == 0);
  PS_CHECK(direct(30) == 0);
  PS_CHECK(direct(31) == 0);
  PS_CHECK(direct(32) == 0);
  PS_CHECK(direct(33) == 0);
  PS_CHECK(direct(34) == 0);
  PS_CHECK(direct(0, false, nullptr, nullptr, 1) == 0);
  PS_CHECK(direct(0, false, nullptr, nullptr, 64, 64,
                  PS_RESULT_ATOMS_WIDE_FIXTURE) == 0);
  std::optional<QualityReport> quality;
  ResultRef result;
  PS_CHECK(direct(4, false, &quality, &result) == 0);
  PS_CHECK(quality && quality->error_bound() == 0 &&
           take(quality->proof_row(2))[1] == 4);
  PS_CHECK(multi_result::number(result) == 2);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
