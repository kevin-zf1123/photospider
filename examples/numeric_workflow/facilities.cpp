#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
struct NumericProbe {
  bool fail, upstream, defer, ready = false;
  explicit NumericProbe(bool failure = true, bool input_failure = false,
                        bool defer_report = false)
      : fail(failure), upstream(input_failure), defer(defer_report) {}
  ps::Result<ps::DependencyPoll> poll(const ps::DependencyPhase& phase) {
    if (defer && !ready) {
      ready = true;
      const auto key = take(ps::dependency_atom_key(phase.query));
      return ps::Result<ps::DependencyPoll>(ps::DependencyNeedBatch{
          {{{key.coordinate.begin(), key.coordinate.begin() + key.rank}, {}}},
          {}});
    }
    ps::NumericDiagnostics report;
    report.profile = ps::CpuNumericProfile::Strict;
    const char identity[] = "manual-numeric-probe/1";
    std::memcpy(report.implementation.data(), identity, sizeof(identity));
    report.evaluated_values = 1;
    auto status = phase.report_numeric(report);
    if (!status.ok())
      return ps::Result<ps::DependencyPoll>(status);
    const auto key = take(ps::dependency_atom_key(phase.query));
    if (upstream && key.coordinate[0] == 1) {
      ps::DependencyNeedBatch need;
      need.associations.push_back(
          {{key.coordinate.begin(), key.coordinate.begin() + key.rank},
           {{0, 5, take(ps::Footprint::all({1})), {}}}});
      return ps::Result<ps::DependencyPoll>(std::move(need));
    }
    if (fail && key.coordinate[0] == 1)
      return ps::Result<ps::DependencyPoll>(
          ps::Status{ps::ErrorCode::OperationFailed,
                     "expected atom failure",
                     ps::FailureReason::InvalidDomain,
                     {ps::FailureOrigin::Domain, ps::FailureScope::Atom, key}});
    auto buffer = ps::MutableValue::allocate(phase.query.output.descriptor,
                                             phase.query.outputs.boxes()[0],
                                             phase.allocator);
    if (!buffer.ok())
      return ps::Result<ps::DependencyPoll>(buffer.status());
    auto writer = buffer.take_value();
    const double value = 7;
    std::memcpy(writer.data(), &value, 8);
    auto output = std::move(writer).publish();
    if (!output.ok())
      return ps::Result<ps::DependencyPoll>(output.status());
    auto fragments = ps::ValueFragments::create(
        phase.query.output.descriptor, {}, phase.query.outputs,
        {output.take_value()}, phase.sets);
    if (!fragments.ok())
      return ps::Result<ps::DependencyPoll>(fragments.status());
    return ps::Result<ps::DependencyPoll>(fragments.take_value());
  }
};
struct NumericJointProbe {
  bool fail, upstream;
  explicit NumericJointProbe(bool failure = true, bool input_failure = false)
      : fail(failure), upstream(input_failure) {}
  ps::Result<std::vector<ps::DependencyAtomOutcome>> poll(
      const ps::DependencyJointPhase& phase) {
    std::vector<ps::DependencyAtomOutcome> outcomes;
    NumericProbe probe(fail, upstream);
    for (const auto* member : phase.members)
      outcomes.push_back(
          {take(ps::dependency_atom_key(member->query)), probe.poll(*member)});
    return ps::Result<std::vector<ps::DependencyAtomOutcome>>(
        std::move(outcomes));
  }
};
ps::OperationDefinition probe(bool fail = true, bool upstream = false,
                              bool defer = false) {
  ps::OperationDefinition operation;
  operation.key = "manual.numeric_probe";
  auto& traits = operation.traits;
  traits.input_count = upstream ? 1 : 0;
  traits.input_schema.resize(traits.input_count);
  traits.joint_contract = 2;
  traits.joint_continuation_bytes = sizeof(NumericJointProbe);
  auto& output = traits.outputs[0];
  output.shape_rule = ps::OperationShapeRule::Fixed;
  output.fixed_output_shape = {fail ? 2U : 5U};
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(NumericProbe);
  output.maximum_dependency_stages = 2;
  output.failure_delivery = ps::FailureDelivery::PerAtomOutcome;
  operation.start_dependency = [fail, upstream, defer](const auto&,
                                                       const auto& allocator) {
    return ps::DependencyContinuation::make<NumericProbe>(allocator, fail,
                                                          upstream, defer);
  };
  operation.start_joint = [fail, upstream](const auto&, const auto& allocator) {
    return ps::DependencyJointContinuation::make<NumericJointProbe>(
        allocator, fail, upstream);
  };
  return operation;
}
struct StructuredSum {
  bool ready = false;
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    using Answer = ps::Result<ps::ResultProgramPoll>;
    const auto count = phase.query.inputs[0].descriptor.shape[0];
    if (!ready) {
      ready = true;
      ps::ResultProgramNeed need;
      need.values = ps::ResourceVector<ps::ResultValueNeed>(
          ps::ResourceAllocator<ps::ResultValueNeed>(phase.resources));
      need.values.push_back({0, take(ps::Footprint::all({count}))});
      return Answer(std::move(need));
    }
    double sum = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
      double value = 0;
      auto status = phase.read(0, {i}, &value, 8);
      if (!status.ok())
        return Answer(status);
      sum += value;
    }
    auto writer = take(ps::MutableValue::allocate(phase.query.output.descriptor,
                                                  ps::Region::whole({1}),
                                                  phase.allocator));
    std::memcpy(writer.data(), &sum, 8);
    auto fragments = take(ps::ValueFragments::create(
        phase.query.output.descriptor, {}, *phase.query.value_outputs,
        {take(std::move(writer).publish())}));
    auto relation = take(
        ps::ResultRelation::cartesian(phase.resources, 1, {0, 15, 0, count}));
    return Answer(
        ps::ResultValuePublication{std::move(fragments), std::move(relation)});
  }
};
ps::OperationDefinition consumer(std::uint32_t grouping) {
  ps::OperationDefinition operation;
  operation.key = "manual.structured_sum";
  operation.traits.input_count = 1;
  operation.traits.input_schema.resize(1);
  auto& output = operation.traits.outputs[0];
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.dependency_version = 2;
  output.continuation_bytes = sizeof(StructuredSum);
  output.maximum_dependency_stages = 2;
  operation.validate_dependency = [grouping](const auto& inputs, const auto&) {
    return inputs[0].atomic_trailing_axes == grouping
               ? ps::Status::success()
               : ps::Status{ps::ErrorCode::TypeMismatch,
                            "producer grouping was lost"};
  };
  operation.start_result = [](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<StructuredSum>(allocator);
  };
  return operation;
}
void structured_diagnostics() {
  auto registry = std::make_shared<ps::OperationRegistry>();
  require(registry->register_operation(probe(false, false, true)).ok(),
          "register bridge producer");
  require(registry->register_operation(consumer(0)).ok(),
          "register bridge consumer");
  require(registry->freeze().ok(), "freeze bridge registry");
  ps::WorkflowDocument document;
  document.nodes = {
      {1, "manual.numeric_probe", {}, {}},
      {2, "manual.structured_sum", {ps::WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"result", 2, "value"}};
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(registry, config);
  auto result = take(execution.execute(compiled.plan));
  require(take(result.values.at("result").as_float64()) == 35,
          "structured independent sum");
  bool found = false;
  for (const auto& timing : result.diagnostics.operation_timings)
    if (timing.output.node_id == 1) {
      require(timing.invocation_count == 10 && timing.computed_elements == 5 &&
                  timing.numeric.evaluated_values == 5,
              "structured bridge must accumulate every observation");
      found = true;
    }
  require(found, "structured bridge numeric timing");
  std::cout << "structured numeric bridge: 5 observations, 10 polls, sum=35, "
               "actual counts=5\n";

  auto grouped = probe(false);
  grouped.traits.outputs[0].atomic_trailing_axes = 1;
  auto grouped_registry = std::make_shared<ps::OperationRegistry>();
  require(grouped_registry->register_operation(std::move(grouped)).ok(),
          "register grouped metadata");
  require(grouped_registry->register_operation(consumer(1)).ok(),
          "register metadata validator");
  require(grouped_registry->freeze().ok(), "freeze grouped metadata");
  auto grouped_plan = take(ps::Compiler(grouped_registry).compile(graph));
  require(grouped_plan.plan.steps()
                  .back()
                  .structured_metadata->inputs[0]
                  .atomic_trailing_axes == 1,
          "structured plan must preserve producer grouping");
}
void joint_metadata() {
  auto operation = probe();
  operation.traits.input_count = 1;
  operation.traits.input_schema.resize(1);
  auto registry = std::make_shared<ps::OperationRegistry>();
  require(registry->register_operation(std::move(operation)).ok(),
          "register joint metadata probe");
  require(registry->freeze().ok(), "freeze joint metadata probe");
  ps::DependencyRequest first;
  first.inputs = {{{ps::ElementType::Float64, {3}}, {}}};
  first.snapshot_identity = "manual-grouping-check";
  first.outputs =
      take(ps::Footprint::from_regions({2}, {ps::Region({{0, 1}})}));
  auto second = first;
  second.inputs[0].atomic_trailing_axes = 1;
  second.outputs =
      take(ps::Footprint::from_regions({2}, {ps::Region({{1, 1}})}));
  auto started = registry->start_joint("manual.numeric_probe", {first, second});
  require(
      !started.ok() && started.status().code == ps::ErrorCode::InvalidArgument,
      "joint must reject differing static producer grouping");
}
void failure_diagnostics(bool joint, bool upstream = false) {
  auto registry = std::make_shared<ps::OperationRegistry>();
  require(registry->register_operation(probe(true, upstream)).ok(),
          "register numeric probe");
  require(registry->freeze().ok(), "freeze numeric probe");
  ps::WorkflowDocument document;
  document.nodes = {{1, "manual.numeric_probe", {}, {}}};
  ps::ExecutionBindings bindings;
  if (upstream) {
    const auto scalar = ps::Value::from_float64(1);
    document.inputs.push_back({1,
                               "input",
                               scalar.descriptor(),
                               scalar.region(),
                               scalar.layout(),
                               {}});
    document.nodes[0].inputs.push_back(ps::WorkflowInputReference{1});
    auto source = std::make_shared<ps::RegionalSource>();
    source->descriptor = scalar.descriptor();
    source->read = [](const auto&, auto*, auto, const auto&,
                      const auto&) -> ps::Result<ps::Region> {
      return ps::Result<ps::Region>(ps::Status{ps::ErrorCode::OperationFailed,
                                               "expected upstream failure"});
    };
    bindings.inputs.push_back({"input", {}, std::move(source)});
  }
  document.outputs = {{"result", 1, "value"}};
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(registry, config);
  ps::ExecutionOptions options;
  options.enable_joint = joint;
  auto result = take(execution.execute_atoms(
      compiled.plan, bindings, {{"result", take(ps::Footprint::all({2}))}}, {},
      options));
  require(result.atoms.size() == 2, "independent atom count");
  ps::NumericDiagnostics actual;
  for (const auto& timing : result.diagnostics.operation_timings)
    require(ps::merge_numeric_diagnostics(&actual, timing.numeric).ok(),
            "merge numeric attempts");
  require(actual.evaluated_values == 2,
          "failed atom must retain its actual numeric work");
  std::cout << "numeric failure accounting joint=" << joint
            << " upstream=" << upstream
            << " evaluated_values=" << actual.evaluated_values << '\n';
}
}  // namespace
int main() {
  try {
    failure_diagnostics(false);
    failure_diagnostics(true);
    failure_diagnostics(false, true);
    failure_diagnostics(true, true);
    structured_diagnostics();
    joint_metadata();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
