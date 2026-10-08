#include <cstring>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "../../examples/atom_outcomes_workflow/operations.hpp"
#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct Counts {
  unsigned start = 0, poll = 0, source = 0;
};
struct State {
  Counts* counts;
  bool input, requested = false;
  State(Counts* counts, bool input) : counts(counts), input(input) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ++counts->poll;
    if (input && !requested) {
      requested = true;
      ResultProgramNeed need;
      need.tensors.push_back({0, 0, take(Footprint::all({1})), 1});
      return Result<ResultProgramPoll>(std::move(need));
    }
    if (input) {
      double number = 0;
      check(phase.read_tensor(0, 0, {0}, &number, sizeof(number)));
      if (number != 7)
        return Result<ResultProgramPoll>(
            Status{ErrorCode::Internal, "source read"});
    }
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        std::vector<std::uint64_t>(phase.association->begin(),
                                   phase.association->end())));
    check(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(phase.resources, 1, {}))));
    const double number = 19;
    check(builder.publish_tensor(
        0, Region::whole({1}),
        {reinterpret_cast<const std::uint8_t*>(&number), sizeof(number)},
        take(ResultRelation::cartesian(
            phase.resources, 1,
            input ? ResultSupport{0, 1, 0, 1, ResultSupportTarget::Tensor, 0}
                  : ResultSupport{})),
        {true, true, true, true}));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
struct JointState {
  Counts* counts;
  Result<ResultProgramPoll> member(const ResultProgramPhase& phase) {
    return Result<ResultProgramPoll>(atom_result::publish(phase, 19, 0, false));
  }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ++counts->poll;
    return member(phase);
  }
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& phase) {
    ++counts->poll;
    ResourceVector<ResultJointOutcome> replies;
    for (const auto* item : phase.members)
      replies.push_back({take(result_atom_key(item->query)), member(*item)});
    return Result<ResourceVector<ResultJointOutcome>>(std::move(replies));
  }
};
OperationDefinition operation(Counts* counts, bool input) {
  OperationDefinition op;
  op.key = "dispatch_probe";
  op.traits.cacheable = false;
  op.traits.input_count = input ? 1 : 0;
  op.traits.input_schema.resize(op.traits.input_count);
  if (input) {
    op.traits.input_schema[0].kind = OperationPortKind::Result;
    op.traits.input_schema[0].tensor_key = "number";
  }
  op.traits.outputs = {multi_result::output("value")};
  op.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  op.traits.outputs[0].continuation_bytes = sizeof(State);
  op.start_result = [counts, input](const auto&, const auto& allocator) {
    ++counts->start;
    return ResultContinuation::make<State>(allocator, counts, input);
  };
  return op;
}
int stages(unsigned mode, std::uint64_t maximum, std::uint64_t queue) {
  Counts count;
  auto registry = std::make_shared<OperationRegistry>();
  auto op = operation(&count, mode == 1 || mode == 3);
  if (mode == 1)
    op.traits.outputs[0].region_rule = OperationRegionRule::Dependency;
  if (mode == 2) {
    op = atom_result::operation(0, "dispatch_probe");
    op.traits.cacheable = false;
    op.traits.input_count = 0;
    op.traits.input_schema.clear();
    op.traits.outputs[0].result_schema = atom_result::schema(1);
    op.traits.outputs[0].continuation_bytes = sizeof(JointState);
    op.traits.joint_continuation_bytes = sizeof(JointState);
    op.start_result = [&](const auto&, const auto& allocator) {
      ++count.start;
      return ResultContinuation::make<JointState>(allocator,
                                                  JointState{&count});
    };
    op.start_result_joint = [&](const auto&, const auto& allocator) {
      ++count.start;
      return ResultJointContinuation::make<JointState>(allocator,
                                                       JointState{&count});
    };
  }
  check(registry->register_operation(std::move(op)));
  if (mode == 3) {
    auto source = operation(&count, false);
    source.key = "source";
    source.traits.outputs[0].continuation_bytes = sizeof(multi_result::Program);
    source.traits.workspace_bytes = 8;
    source.start_result = [&](const auto&, const auto& allocator) {
      ++count.source;
      return ResultContinuation::make<multi_result::Program>(allocator, -1, 7);
    };
    check(registry->register_operation(std::move(source)));
  }
  check(registry->freeze());
  WorkflowDocument doc;
  doc.nodes = {{1, "dispatch_probe", {}, {}}};
  doc.outputs = {{"value", 1, "value"}};
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 0;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->maximum_stages = maximum;
  config.managed_resources->capacity[ResourceKind::Queue] = queue;
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  ExecutionBindings bindings;
  if (mode == 1) {
    doc.inputs = {multi_result::declaration(7, "source")};
    doc.nodes[0].inputs = {WorkflowInputReference{7}};
    bindings.inputs.push_back(multi_result::binding(root, "source", 7));
  }
  if (mode == 3) {
    doc.nodes = {{7, "source", {}, {}},
                 {1, "dispatch_probe", {WorkflowNodeOutput{7, "value"}}, {}}};
  }
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph));
  const auto execute = [&] {
    return mode == 2
               ? context.execute_atoms(plan.plan, bindings,
                                       {{"value", take(Footprint::all({1}))}})
               : context.execute(plan.plan, bindings);
  };
  const auto failed = [&](const Result<ExecutionResult>& result) {
    if (!result.ok())
      return result.status().code == ErrorCode::ResourceExhausted;
    return mode == 2 && result.value().atoms.size() == 1 &&
           !result.value().atoms[0].outcome.ok() &&
           result.value().atoms[0].outcome.status().code ==
               ErrorCode::ResourceExhausted;
  };
  auto result = execute();
  if (!maximum || !queue) {
    PS_CHECK(failed(result));
    PS_CHECK(count.start == 0 && count.poll == 0 && count.source == 0);
    PS_CHECK(root.statistics().issued.stages == 0);
  } else {
    if (!result.ok())
      std::cerr << "mode=" << mode
                << " code=" << static_cast<int>(result.status().code)
                << " stages=" << root.statistics().issued.stages
                << " start=" << count.start << " poll=" << count.poll
                << " source=" << count.source << " " << result.status().message
                << '\n';
    PS_CHECK(result.ok());
    if (mode == 2)
      PS_CHECK(result.value().atoms.at(0).outcome.ok());
    else
      PS_CHECK(multi_result::number(result.value().results.at("value")) == 19);
    PS_CHECK(root.statistics().issued.stages == maximum);
    const auto before = count.start + count.poll + count.source;
    result = execute();
    PS_CHECK(failed(result));
    PS_CHECK(count.start + count.poll + count.source == before);
    PS_CHECK(root.statistics().issued.stages == maximum);
  }
  PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
struct SourceState {
  bool throwing;
  Result<ResultProgramPoll> poll(const ResultProgramPhase&) {
    if (throwing)
      throw 7;
    return Result<ResultProgramPoll>(
        Status{ErrorCode::OperationFailed,
               "source failed",
               FailureReason::ShortIo,
               {FailureOrigin::Io, FailureScope::Group}});
  }
};
int source_failure() {
  for (bool throwing : {false, true}) {
    unsigned callbacks = 0;
    auto registry = std::make_shared<OperationRegistry>();
    auto source = operation(nullptr, false);
    source.key = "source";
    source.traits.outputs[0].continuation_bytes = sizeof(SourceState);
    source.start_result = [throwing](const auto&, const auto& allocator) {
      return ResultContinuation::make<SourceState>(allocator,
                                                   SourceState{throwing});
    };
    check(registry->register_operation(std::move(source)));
    auto downstream = operation(nullptr, true);
    downstream.start_result = [&](const auto&, const auto& allocator) {
      struct Need {
        unsigned* callbacks;
        bool requested = false;
        Result<ResultProgramPoll> poll(const ResultProgramPhase&) {
          if (!requested) {
            requested = true;
            ResultProgramNeed need;
            need.tensors.push_back({0, 0, take(Footprint::all({1})), 1});
            return Result<ResultProgramPoll>(std::move(need));
          }
          ++*callbacks;
          return Result<ResultProgramPoll>(
              Status{ErrorCode::Internal, "unexpected source delivery"});
        }
      };
      return ResultContinuation::make<Need>(allocator, Need{&callbacks});
    };
    check(registry->register_operation(std::move(downstream)));
    check(registry->freeze());
    WorkflowDocument doc;
    doc.nodes = {{7, "source", {}, {}},
                 {11, "dispatch_probe", {WorkflowNodeOutput{7, "value"}}, {}}};
    doc.outputs = {{"hist", 11, "value"}};
    GraphContext graph(doc);
    auto plan = take(Compiler(registry).compile(graph));
    ExecutionContext context(registry);
    auto result = context.execute(plan.plan);
    PS_CHECK(!result.ok() && callbacks == 0);
    PS_CHECK(result.status().code == ErrorCode::OperationFailed);
    PS_CHECK(result.status().detail.node_id == 7);
    if (!throwing)
      PS_CHECK(result.status().detail.origin == FailureOrigin::Io &&
               result.status().detail.scope == FailureScope::Group &&
               result.status().reason == FailureReason::ShortIo);
    else
      PS_CHECK(result.status().reason == FailureReason::HostException);
    PS_CHECK(take(context.resource_budget())
                 .statistics()
                 .live[ResourceKind::Queue] == 0);
  }
  return 0;
}
}  // namespace
int main() try {
  for (unsigned mode = 0; mode < 4; ++mode) {
    PS_CHECK(stages(mode, 0, 1) == 0);
    PS_CHECK(stages(mode, 10, 0) == 0);
    // Every physical Result start and poll is admitted separately. A direct
    // publication needs two dispatches; a tensor Need adds a third. The
    // upstream source in mode 3 contributes its own start/publication pair.
    PS_CHECK(stages(mode,
                    mode == 0 || mode == 2 ? 2
                    : mode == 1            ? 3
                                           : 5,
                    1) == 0);
  }
  PS_CHECK(source_failure() == 0);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
