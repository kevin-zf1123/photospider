#include <atomic>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
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
struct Counts {
  std::atomic<unsigned> prepared{0}, destroyed{0}, started{0};
};
struct Program {
  std::shared_ptr<Counts> counts;
  explicit Program(std::shared_ptr<Counts> stats) : counts(std::move(stats)) {}
  ~Program() { ++counts->destroyed; }
  const double value = 7;
};
struct State {
  const Program* program;
  explicit State(const Program* value) : program(value) {}
  ps::Result<ps::DependencyPoll> poll(const ps::DependencyPhase& phase) {
    require(phase.query.prepared && phase.query.prepared->state() == program,
            "session owns exact immutable program");
    auto writer = take(ps::MutableValue::allocate(
        phase.query.output.descriptor, phase.query.outputs.boxes()[0],
        phase.allocator));
    const auto count = take(phase.query.outputs.element_count());
    for (std::uint64_t i = 0; i < count; ++i)
      std::memcpy(writer.data() + i * 8, &program->value, 8);
    return ps::Result<ps::DependencyPoll>(take(ps::ValueFragments::create(
        phase.query.output.descriptor, {}, phase.query.outputs,
        {take(std::move(writer).publish())}, phase.sets)));
  }
};
struct Joint {
  ps::Result<std::vector<ps::DependencyAtomOutcome>> poll(
      const ps::DependencyJointPhase& phase) {
    std::vector<ps::DependencyAtomOutcome> result;
    for (const auto* member : phase.members) {
      require(member->query.prepared, "joint prepared pointer");
      State state(static_cast<const Program*>(member->query.prepared->state()));
      result.push_back(
          {take(ps::dependency_atom_key(member->query)), state.poll(*member)});
    }
    return ps::Result<std::vector<ps::DependencyAtomOutcome>>(
        std::move(result));
  }
};
ps::OperationDefinition definition(const std::shared_ptr<Counts>& counts) {
  ps::OperationDefinition operation;
  operation.key = "manual.prepared";
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.input_schema[0].element_type_mask = 4;
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {{"mask", ps::OperationParameterType::Float64}};
  traits.joint_contract = 2;
  traits.joint_continuation_bytes = sizeof(Joint);
  auto& value = traits.outputs[0];
  value.key = "values";
  value.region_rule = ps::OperationRegionRule::Dependency;
  value.dependency_version = 1;
  value.continuation_bytes = sizeof(State);
  value.maximum_dependency_stages = 1;
  value.failure_delivery = ps::FailureDelivery::PerAtomOutcome;
  traits.outputs.push_back(value);
  traits.outputs[1].key = "axis";
  operation.prepare_static =
      [counts](const auto& inputs,
               const auto&) -> ps::Result<ps::OperationPreparation> {
    if (inputs[0].descriptor.shape != std::vector<std::uint64_t>{1})
      return ps::Result<ps::OperationPreparation>(
          ps::Status{ps::ErrorCode::TypeMismatch, "expected scalar"});
    ++counts->prepared;
    ps::OperationPreparation prepared;
    prepared.outputs.resize(2);
    prepared.outputs[0].metadata.descriptor = {ps::ElementType::Float64, {5}};
    prepared.outputs[1].metadata.descriptor = {ps::ElementType::Float64, {3}};
    prepared.outputs[1].metadata.atomic_trailing_axes = 1;
    prepared.state = std::make_shared<const Program>(counts);
    return ps::Result<ps::OperationPreparation>(std::move(prepared));
  };
  operation.start_dependency = [counts](const auto& query,
                                        const auto& allocator) {
    ++counts->started;
    require(query.prepared, "prepared singleton query");
    return ps::DependencyContinuation::make<State>(
        allocator, static_cast<const Program*>(query.prepared->state()));
  };
  operation.start_joint = [](const auto& queries, const auto& allocator) {
    require(!queries.empty() && queries[0].prepared, "prepared joint start");
    for (const auto& query : queries)
      require(query.prepared == queries[0].prepared, "one joint program owner");
    return ps::DependencyJointContinuation::make<Joint>(allocator);
  };
  return operation;
}
std::shared_ptr<ps::OperationRegistry> registry(
    const std::shared_ptr<Counts>& counts) {
  auto result = std::make_shared<ps::OperationRegistry>();
  require(result->register_operation(definition(counts)).ok() &&
              result->freeze().ok(),
          "register prepared operation");
  return result;
}
void compiler_and_direct(const std::shared_ptr<Counts>& counts) {
  auto operations = registry(counts);
  auto scalar = ps::Value::from_float64(1);
  ps::WorkflowDocument document;
  document.inputs = {
      {1, "input", scalar.descriptor(), scalar.region(), scalar.layout(), {}}};
  document.nodes = {
      {1, "manual.prepared", {ps::WorkflowInputReference{1}}, {{"mask", 0.0}}}};
  document.outputs = {{"values", 1, "values"}, {"axis", 1, "axis"}};
  ps::GraphContext graph(document);
  const auto before = counts->prepared.load();
  auto compiled = take(ps::Compiler(operations).compile(graph));
  require(counts->prepared == before + 1, "compile prepares once");
  require(compiled.semantic.nodes()[0].prepared ==
                  compiled.optimized.nodes()[0].prepared &&
              compiled.plan.steps()[0].prepared ==
                  compiled.plan.steps()[1].prepared,
          "optimizer and outputs share program");
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(operations, config);
  ps::ExecutionBindings bindings;
  bindings.inputs = {{"input", scalar}};
  for (bool joint : {false, true}) {
    ps::ExecutionOptions options;
    options.enable_joint = joint;
    auto result = take(context.execute(compiled.plan, bindings, {}, options));
    require(result.values.size() == 2, "both compiled outputs");
  }
  bindings.inputs[0].value = ps::Value::from_float64(9);
  auto frozen = take(context.freeze(compiled.plan, bindings));
  auto result = take(context.execute_fragments(
      frozen,
      {{"values", take(ps::Footprint::from_regions(
                      {5}, {ps::Region({{1, 1}}), ps::Region({{4, 1}})}))}}));
  std::uint64_t raw = 0;
  require(result.values.at("values").read({4}, &raw, 8).ok() &&
              raw == UINT64_C(0x401c000000000000),
          "prepared sparse output");
  auto tile = take(compiled.plan.tile_plan("values", ps::Region({{2, 1}})));
  require(tile.steps()[0].prepared == compiled.plan.steps()[0].prepared,
          "tile keeps program");
  require(counts->prepared == before + 1,
          "multiple runs/outputs/ROI never reprepare");
  const std::vector<ps::Value> inputs{scalar};
  const std::vector<ps::Region> demands{scalar.region()};
  const std::map<std::string, ps::ParameterValue> parameters{{"mask", 0.0}};
  ps::OperationInvocation invocation(inputs, demands, parameters);
  auto direct = take(operations->invoke("manual.prepared", invocation));
  require(direct.descriptor().shape == std::vector<std::uint64_t>{5} &&
              counts->prepared == before + 2,
          "direct multi-atom prepares once");
}
void seals_and_lifetime(const std::shared_ptr<Counts>& counts) {
  static_assert(!std::is_copy_constructible_v<ps::PreparedOperation> &&
                !std::is_move_constructible_v<ps::PreparedOperation>);
  auto operations = registry(counts),
       other = registry(std::make_shared<Counts>());
  ps::DependencyRequest request;
  request.inputs = {{{ps::ElementType::Float64, {1}}, {}}};
  request.parameters = {{"mask", 0.0}};
  request.snapshot_identity = "static-owner";
  request.outputs = take(ps::Footprint::none({5}));
  request.prepared = take(operations->prepare_operation(
      "manual.prepared", request.inputs, request.parameters));
  const auto before = counts->prepared.load();
  require(operations->start_dependency("manual.prepared", request).ok(),
          "valid Empty preparation");
  auto foreign = other->start_dependency("manual.prepared", request);
  require(!foreign.ok() && foreign.status().code == ps::ErrorCode::Stale,
          "foreign preparation rejected");
  auto changed = request;
  changed.parameters["mask"] = -0.0;
  require(!operations->start_dependency("manual.prepared", changed).ok(),
          "signed zero static identity");
  changed = request;
  changed.inputs[0].descriptor.shape = {2};
  require(!operations->start_dependency("manual.prepared", changed).ok(),
          "metadata changed");
  std::uint64_t nan_bits = UINT64_C(0x7ff8000000000042);
  double nan = 0;
  std::memcpy(&nan, &nan_bits, 8);
  changed = request;
  changed.parameters["mask"] = nan;
  changed.prepared = take(operations->prepare_operation(
      "manual.prepared", changed.inputs, changed.parameters));
  require(operations->start_dependency("manual.prepared", changed).ok(),
          "identical NaN parameter bits match");
  ++nan_bits;
  std::memcpy(&nan, &nan_bits, 8);
  changed.parameters["mask"] = nan;
  require(!operations->start_dependency("manual.prepared", changed).ok(),
          "distinct NaN payload rejects");
  require(counts->prepared == before + 1, "validation never reparses");
  request.outputs =
      take(ps::Footprint::from_regions({5}, {ps::Region({{0, 1}})}));
  auto session = take(operations->start_dependency("manual.prepared", request));
  request.prepared.reset();
  changed.prepared.reset();
  operations.reset();
  auto completed = take(session->poll());
  require(std::holds_alternative<ps::DependencyResult>(completed),
          "session outlives registry and external program owner");
}
void direct_joint(const std::shared_ptr<Counts>& counts) {
  auto operations = registry(counts);
  ps::DependencyRequest first;
  first.inputs = {{{ps::ElementType::Float64, {1}}, {}}};
  first.parameters = {{"mask", 0.0}};
  first.snapshot_identity = "joint-static";
  first.outputs =
      take(ps::Footprint::from_regions({5}, {ps::Region({{0, 1}})}));
  auto second = first;
  second.outputs =
      take(ps::Footprint::from_regions({5}, {ps::Region({{1, 1}})}));
  const auto before = counts->prepared.load();
  auto session =
      take(operations->start_joint("manual.prepared", {first, second}));
  require(session->poll().ok() && counts->prepared == before + 1,
          "direct joint prepares once");
}
void numeric_function_counters() {
  ps::NumericDiagnostics report;
  report.profile = ps::CpuNumericProfile::Strict;
  const char identity[] = "manual-function-counters/1";
  std::memcpy(report.implementation.data(), identity, sizeof(identity));
  report.strict_math_calls = 3;
  report.strict_fallbacks = 2;
  report.fallback_reasons[1] = 2;
  report.function_fallbacks[static_cast<unsigned>(ps::NumericMathFunction::Sin)]
                           [1] = 1;
  ps::NumericDiagnostics total;
  require(ps::merge_numeric_diagnostics(&total, report).ok() &&
              total.strict_math_calls == 3 &&
              total.function_fallbacks[0][1] == 1,
          "unattributed reason counts enter Other");
  require(ps::merge_numeric_diagnostics(&total, report).ok() &&
              total.strict_math_calls == 6 &&
              total.function_fallbacks[static_cast<unsigned>(
                  ps::NumericMathFunction::Sin)][1] == 2,
          "function counts accumulate");
  const auto saved = total;
  report.function_fallbacks[1][1] = 2;
  require(!ps::merge_numeric_diagnostics(&total, report).ok() &&
              total.function_fallbacks == saved.function_fallbacks &&
              total.strict_math_calls == saved.strict_math_calls,
          "malformed attribution preserves destination");
  report.function_fallbacks[1][1] = 0;
  total.strict_math_calls = UINT64_MAX;
  require(!ps::merge_numeric_diagnostics(&total, report).ok() &&
              total.strict_math_calls == UINT64_MAX,
          "math call overflow does not wrap");
}
}  // namespace
int main() {
  try {
    auto counts = std::make_shared<Counts>();
    numeric_function_counters();
    compiler_and_direct(counts);
    seals_and_lifetime(counts);
    direct_joint(counts);
    require(counts->prepared == counts->destroyed,
            "all immutable programs released");
    std::cout
        << "prepared programs: compile/direct/joint once, outputs/ROI/tile "
           "reuse, exact static seals and session lifetime passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
