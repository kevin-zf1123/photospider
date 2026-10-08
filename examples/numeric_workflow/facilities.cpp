#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
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
void check(ps::Status status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
ps::SchemaTemplate schema(unsigned count, unsigned grouping = 0) {
  ps::SchemaTemplate result;
  result.id = "manual.numeric_report";
  ps::ResultTensorSpec tensor;
  tensor.key = "number";
  tensor.descriptor = {ps::ElementType::Float64, {count}};
  tensor.atomic_trailing_axes = grouping;
  result.tensors.push_back(std::move(tensor));
  return result;
}
ps::ResultProgramPoll publish(const ps::ResultProgramPhase& phase, double value,
                              unsigned input_count = 0,
                              unsigned input_begin = 0) {
  const auto& output = *phase.query.output.result_schema;
  auto builder = take(ps::ResultBuilder::start(
      phase.resources, output, phase.query.semantic_key, {},
      phase.association ? std::vector<std::uint64_t>(phase.association->begin(),
                                                     phase.association->end())
                        : std::vector<std::uint64_t>{}));
  check(builder.bind_descriptor_relation(
      take(ps::ResultRelation::cartesian(phase.resources, 1, {}))));
  auto relation = take(ps::ResultRelation::cartesian(
      phase.resources, take(output.tensors[0].sample_count()),
      input_count ? ps::ResultSupport{0, 1, input_begin, input_count,
                                      ps::ResultSupportTarget::Tensor, 0}
                  : ps::ResultSupport{}));
  for (const auto& box : phase.query.tensor_outputs->boxes()) {
    std::vector<double> values(take(box.element_count()), value);
    check(builder.publish_tensor(
        0, box,
        {reinterpret_cast<const std::uint8_t*>(values.data()),
         values.size() * sizeof(value)},
        relation, {true, true, true, true}));
  }
  return ps::ResultPublication{take(builder.seal()), true};
}
struct NumericProbe {
  bool fail, upstream, defer;
  std::array<bool, 5> ready{};
  explicit NumericProbe(bool failure = true, bool input_failure = false,
                        bool defer_report = false)
      : fail(failure), upstream(input_failure), defer(defer_report) {}
  ps::Result<ps::ResultProgramPoll> member(
      const ps::ResultProgramPhase& phase) {
    using Answer = ps::Result<ps::ResultProgramPoll>;
    const auto key = take(ps::result_atom_key(phase.query));
    if (defer && !ready.at(key.coordinate[0])) {
      ready[key.coordinate[0]] = true;
      ps::ResultProgramNeed need;
      need.tensors.push_back({0, 0,
                              take(ps::Footprint::from_regions(
                                  {5}, {ps::Region({{key.coordinate[0], 1}})})),
                              1});
      return Answer(std::move(need));
    }
    ps::NumericDiagnostics report;
    report.profile = ps::CpuNumericProfile::Strict;
    const char identity[] = "manual-numeric-probe/2";
    std::memcpy(report.implementation.data(), identity, sizeof(identity));
    report.evaluated_values = take(phase.query.tensor_outputs->element_count());
    auto status = phase.report_numeric(report);
    if (!status.ok())
      return Answer(status);
    if (upstream && key.coordinate[0] == 1) {
      ps::ResultProgramNeed need;
      need.tensors.push_back({0, 0, take(ps::Footprint::all({1})), 5});
      return Answer(std::move(need));
    }
    if (fail && key.coordinate[0] == 1)
      return Answer(
          ps::Status{ps::ErrorCode::OperationFailed,
                     "expected atom failure",
                     ps::FailureReason::InvalidDomain,
                     {ps::FailureOrigin::Domain, ps::FailureScope::Atom, key}});
    double value = 7;
    if (defer) {
      status = phase.tensors->at({0, 0}).read({key.coordinate[0]}, &value,
                                              sizeof(value));
      if (!status.ok())
        return Answer(status);
    }
    return Answer(publish(phase, value, defer ? 1 : 0, key.coordinate[0]));
  }
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    return member(phase);
  }
  ps::Result<ps::ResourceVector<ps::ResultJointOutcome>> poll(
      const ps::ResultJointPhase& phase) {
    ps::ResourceVector<ps::ResultJointOutcome> outcomes;
    for (const auto* item : phase.members)
      outcomes.push_back(
          {take(ps::result_atom_key(item->query)), member(*item)});
    return ps::Result<ps::ResourceVector<ps::ResultJointOutcome>>(
        std::move(outcomes));
  }
};
ps::OperationDefinition probe(bool fail = true, bool upstream = false,
                              bool defer = false, unsigned grouping = 0) {
  ps::OperationDefinition operation;
  operation.key = "manual.numeric_probe";
  auto& traits = operation.traits;
  traits.input_count = upstream || defer ? 1 : 0;
  traits.input_schema.resize(traits.input_count);
  if (traits.input_count) {
    traits.input_schema[0].kind = ps::OperationPortKind::Result;
    traits.input_schema[0].result_schema_id = "manual.numeric_report";
    traits.input_schema[0].result_schema_version = 1;
  }
  traits.joint_contract = 2;
  traits.joint_continuation_bytes = sizeof(NumericProbe);
  auto& output = traits.outputs[0];
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.output_schema.result_schema_id = "manual.numeric_report";
  output.output_schema.result_schema_version = 1;
  output.result_schema = schema(fail ? 2 : 5, grouping);
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.continuation_bytes = sizeof(NumericProbe);
  output.maximum_dependency_stages = 2;
  output.failure_delivery = ps::FailureDelivery::PerAtomOutcome;
  operation.start_result = [fail, upstream, defer](const auto&,
                                                   const auto& allocator) {
    return ps::ResultContinuation::make<NumericProbe>(allocator, fail, upstream,
                                                      defer);
  };
  operation.start_result_joint = [fail, upstream, defer](
                                     const auto&, const auto& allocator) {
    return ps::ResultJointContinuation::make<NumericProbe>(allocator, fail,
                                                           upstream, defer);
  };
  return operation;
}
struct FailedSource {
  bool exhaust_host;
  ps::ResourceLease fill;
  explicit FailedSource(bool exhaust_host) : exhaust_host(exhaust_host) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    if (exhaust_host) {
      ps::NumericDiagnostics report;
      report.profile = ps::CpuNumericProfile::Strict;
      const char identity[] = "manual-numeric-probe/2";
      std::memcpy(report.implementation.data(), identity, sizeof(identity));
      report.evaluated_values = 1;
      check(phase.report_numeric(report));
      const auto available =
          phase.resources.available_capacity()[ps::ResourceKind::Host];
      fill = take(phase.resources.reserve(ps::ResourceCapacity::host(
          available - ps::ResourceBudget::lease_metadata_bytes())));
      require(phase.resources.available_capacity()[ps::ResourceKind::Host] == 0,
              "failed callback reaches Host capacity");
    }
    return ps::Result<ps::ResultProgramPoll>(
        ps::Status{ps::ErrorCode::OperationFailed,
                   "upstream failed",
                   ps::FailureReason::ShortIo,
                   {ps::FailureOrigin::Io, ps::FailureScope::Group}});
  }
};
ps::OperationDefinition failed_source(bool exhaust_host) {
  auto operation = probe();
  operation.key = "manual.failed_source";
  operation.traits.joint_contract = 0;
  operation.traits.joint_continuation_bytes = 0;
  auto& output = operation.traits.outputs[0];
  output.result_schema = schema(1);
  output.failure_delivery = ps::FailureDelivery::RequestFailureOnly;
  output.continuation_bytes = sizeof(FailedSource);
  operation.start_result = [exhaust_host](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<FailedSource>(allocator, exhaust_host);
  };
  operation.start_result_joint = {};
  return operation;
}
struct StructuredSum {
  bool ready = false;
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    using Answer = ps::Result<ps::ResultProgramPoll>;
    const auto count =
        phase.query.inputs[0].result_schema->tensors[0].descriptor.shape[0];
    if (!ready) {
      ready = true;
      ps::ResultProgramNeed need;
      need.tensors.push_back({0, 0, take(ps::Footprint::all({count})), 1});
      return Answer(std::move(need));
    }
    double sum = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
      double value = 0;
      auto status = phase.tensors->at({0, 0}).read({i}, &value, sizeof(value));
      if (!status.ok())
        return Answer(status);
      sum += value;
    }
    return Answer(publish(phase, sum, count));
  }
};
ps::OperationDefinition consumer(std::uint32_t grouping) {
  ps::OperationDefinition operation;
  operation.key = "manual.structured_sum";
  operation.traits.input_count = 1;
  operation.traits.input_schema.resize(1);
  operation.traits.input_schema[0].kind = ps::OperationPortKind::Result;
  operation.traits.input_schema[0].result_schema_id = "manual.numeric_report";
  operation.traits.input_schema[0].result_schema_version = 1;
  auto& output = operation.traits.outputs[0];
  output.output_schema = operation.traits.input_schema[0];
  output.result_schema = schema(1);
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.continuation_bytes = sizeof(StructuredSum);
  output.maximum_dependency_stages = 2;
  operation.validate_dependency = [grouping](const auto& inputs, const auto&) {
    return inputs[0].result_schema->tensors[0].atomic_trailing_axes == grouping
               ? ps::Status::success()
               : ps::Status{ps::ErrorCode::TypeMismatch,
                            "producer grouping was lost"};
  };
  operation.start_result = [](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<StructuredSum>(allocator);
  };
  return operation;
}
double number(const ps::ResultRef& result, std::uint64_t at = 0) {
  double value = 0;
  check(result.read_tensor(take(result.descriptor()), 0, {at}, &value,
                           sizeof(value)));
  return value;
}
void structured_diagnostics() {
  auto registry = std::make_shared<ps::OperationRegistry>();
  check(registry->register_operation(probe(false, false, true)));
  check(registry->register_operation(consumer(0)));
  check(registry->freeze());
  ps::WorkflowDocument document;
  ps::WorkflowInputDeclaration declaration;
  declaration.id = 1;
  declaration.name = "source";
  declaration.result_schema =
      std::make_shared<const ps::SchemaTemplate>(schema(5));
  document.inputs.push_back(std::move(declaration));
  document.nodes = {
      {1, "manual.numeric_probe", {ps::WorkflowInputReference{1}}, {}},
      {2, "manual.structured_sum", {ps::WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"result", 2, "value"}};
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(registry, config);
  const auto root = take(execution.resource_budget());
  auto builder = take(ps::ResultBuilder::start(root, schema(5), "source"));
  check(builder.bind_descriptor_relation(
      take(ps::ResultRelation::cartesian(root, 1, {}))));
  const double values[] = {7, 7, 7, 7, 7};
  check(builder.publish_tensor(
      0, ps::Region::whole({5}),
      {reinterpret_cast<const std::uint8_t*>(values), sizeof(values)},
      take(ps::ResultRelation::cartesian(root, 5, {})),
      {true, true, true, true}));
  auto result = take(
      execution.execute(compiled.plan, {{{"source", take(builder.seal())}}}));
  require(number(result.results.at("result")) == 35,
          "structured independent sum");
  bool found = false;
  for (const auto& timing : result.diagnostics.operation_timings)
    if (timing.output.node_id == 1) {
      require(timing.invocation_count == 10 && timing.computed_elements == 5 &&
                  timing.numeric.evaluated_values == 5,
              "structured Result must accumulate every observation");
      found = true;
    }
  require(found, "structured Result numeric timing");
  std::cout << "structured Result numeric reports: 5 observations, 10 polls, "
               "sum=35, evaluated_values=5, computed_elements=5\n";

  auto grouped_registry = std::make_shared<ps::OperationRegistry>();
  check(grouped_registry->register_operation(probe(false, false, true, 1)));
  check(grouped_registry->register_operation(consumer(1)));
  check(grouped_registry->freeze());
  auto grouped_plan = take(ps::Compiler(grouped_registry).compile(graph));
  require(grouped_plan.plan.steps()
                  .back()
                  .structured_metadata->inputs[0]
                  .result_schema->tensors[0]
                  .atomic_trailing_axes == 1,
          "structured plan must preserve producer grouping");
}
void joint_metadata() {
  auto operation = probe(true, true);
  auto registry = std::make_shared<ps::OperationRegistry>();
  check(registry->register_operation(std::move(operation)));
  check(registry->freeze());
  ps::OperationMetadata first_input, second_input;
  first_input.result_schema =
      std::make_shared<const ps::SchemaTemplate>(schema(3));
  second_input.result_schema =
      std::make_shared<const ps::SchemaTemplate>(schema(3, 1));
  const auto traits = take(registry->find_traits("manual.numeric_probe"));
  auto outputs = take(ps::infer_operation_outputs(traits, {first_input}, {}));
  ps::ResultProgramMetadata first{{first_input}, outputs[0]},
      second{{second_input}, outputs[0]};
  std::map<std::string, ps::ParameterValue> parameters;
  ps::ResourceBudget root;
  ps::ResourceAllocationScope scope(root);
  ps::ResourceVector<ps::ResultProgramQuery> queries;
  queries.emplace_back(first, parameters);
  queries.emplace_back(second, parameters);
  for (unsigned i = 0; i < queries.size(); ++i) {
    queries[i].snapshot_identity = "manual-grouping-check";
    queries[i].semantic_key = i ? "second" : "first";
    queries[i].tensor_outputs =
        take(ps::Footprint::from_regions({2}, {ps::Region({{i, 1}})}));
  }
  auto started =
      registry->start_result_joint("manual.numeric_probe", queries, root);
  require(!started.ok() && started.status().code == ps::ErrorCode::Stale,
          "joint must reject differing static producer grouping");
}
void failure_diagnostics(bool joint, bool upstream = false,
                         bool exhaust_host = false) {
  auto registry = std::make_shared<ps::OperationRegistry>();
  check(registry->register_operation(probe(true, upstream)));
  if (upstream)
    check(registry->register_operation(failed_source(exhaust_host)));
  check(registry->freeze());
  ps::WorkflowDocument document;
  document.nodes = {{1, "manual.numeric_probe", {}, {}}};
  if (upstream) {
    document.nodes.insert(document.nodes.begin(),
                          {2, "manual.failed_source", {}, {}});
    document.nodes[1].inputs.push_back(ps::WorkflowNodeOutput{2, "value"});
  }
  document.outputs = {{"result", 1, "value"}};
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ps::ResourceLimits{};
  if (exhaust_host) {
    config.managed_resources->capacity[ps::ResourceKind::Host] = 4194304;
    config.managed_resources->capacity[ps::ResourceKind::Metadata] = 4194304;
  }
  ps::ExecutionContext execution(registry, config);
  ps::ExecutionOptions options;
  options.enable_joint = joint;
  auto result = take(execution.execute_atoms(
      compiled.plan, {}, {{"result", take(ps::Footprint::all({2}))}}, {},
      options));
  require(result.atoms.size() == 2, "independent atom count");
  require(result.atoms[0].outcome.ok() &&
              number(result.atoms[0].outcome.value()) == 7,
          "successful numeric Result atom");
  const auto& failure = result.atoms[1].outcome;
  require(
      !failure.ok() && failure.status().code == ps::ErrorCode::OperationFailed,
      "independent failed atom");
  require(
      failure.status().reason == (upstream
                                      ? ps::FailureReason::ShortIo
                                      : ps::FailureReason::InvalidDomain) &&
          failure.status().detail.origin ==
              (upstream ? ps::FailureOrigin::Io : ps::FailureOrigin::Domain) &&
          failure.status().detail.node_id == (upstream ? 2u : 1u),
      "failed Result atom retains original provenance");
  ps::NumericDiagnostics actual;
  for (const auto& timing : result.diagnostics.operation_timings) {
    check(ps::merge_numeric_diagnostics(&actual, timing.numeric));
  }
  require(actual.evaluated_values == (exhaust_host ? 3u : 2u),
          "failed atom must retain its actual numeric work");
  if (exhaust_host) {
    const auto root = take(execution.resource_budget());
    require(root.statistics().peak[ps::ResourceKind::Host] == 4194304 &&
                root.available_capacity()[ps::ResourceKind::Host] > 0,
            "failed callback releases its full Host reservation");
  }
  std::cout << "Result numeric failure accounting joint=" << joint
            << " upstream=" << upstream << " host_exhausted=" << exhaust_host
            << " evaluated_values=" << actual.evaluated_values << '\n';
}
}  // namespace
int main() {
  try {
    failure_diagnostics(false);
    failure_diagnostics(true);
    failure_diagnostics(false, true);
    failure_diagnostics(true, true);
    failure_diagnostics(false, true, true);
    failure_diagnostics(true, true, true);
    structured_diagnostics();
    joint_metadata();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
