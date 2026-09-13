#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
void check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
Result<DependencyPoll> number(const DependencyPhase& phase, double value) {
  auto allocated =
      MutableValue::allocate(phase.query.output.descriptor,
                             phase.query.outputs.boxes()[0], phase.allocator);
  if (!allocated.ok())
    return Result<DependencyPoll>(allocated.status());
  auto bytes = allocated.take_value();
  std::memcpy(bytes.data(), &value, 8);
  auto published = std::move(bytes).publish();
  if (!published.ok())
    return Result<DependencyPoll>(published.status());
  auto fragments =
      ValueFragments::create(phase.query.output.descriptor, {},
                             phase.query.outputs, {published.take_value()});
  return fragments.ok() ? Result<DependencyPoll>(fragments.take_value())
                        : Result<DependencyPoll>(fragments.status());
}
struct Coordinates {
  int mode = 0;
  std::array<unsigned, 3> stage{};
  explicit Coordinates(int mode) : mode(mode) {}
  Result<DependencyPoll> member(const DependencyPhase& phase) {
    const auto key = dependency_atom_key(phase.query).value();
    auto index = key.coordinate[0];
    auto& step = stage[index];
    auto need = [&](unsigned port) {
      auto samples =
          Footprint::from_regions({3}, {Region({{index, 1}})}).take_value();
      return Result<DependencyPoll>(
          DependencyNeedBatch{{{{index}, {{port, 1, std::move(samples), {}}}}},
                              {}});
    };
    if (step == 0) {
      step = 1;
      return need(0);
    }
    double value = 0;
    auto read = phase.read(step == 1 ? 0 : 1, {index}, &value, 8);
    if (!read.ok())
      return Result<DependencyPoll>(read);
    if (mode == -1)
      return number(phase, value * 2);
    if (step == 1 && value < 0) {
      step = 2;
      return need(1);
    }
    if (value == 0)
      return Result<DependencyPoll>(
          Status{ErrorCode::OperationFailed,
                 "zero denominator",
                 FailureReason::DivideByZero,
                 {FailureOrigin::Domain, FailureScope::Atom, key}});
    return number(phase, 1 / value);
  }
  Result<std::vector<DependencyAtomOutcome>> poll(
      const DependencyJointPhase& phase) {
    std::vector<DependencyAtomOutcome> result;
    for (const auto* item : phase.members)
      result.push_back(
          {dependency_atom_key(item->query).value(), member(*item)});
    return Result<std::vector<DependencyAtomOutcome>>(std::move(result));
  }
};
OperationDefinition coordinate_operation(int mode) {
  OperationDefinition op;
  op.key = mode == -1 ? "example.scale" : "example.guarded_reciprocal";
  op.traits.input_count = mode == -1 ? 1 : 2;
  op.traits.input_schema.resize(op.traits.input_count);
  op.traits.joint_contract = 2;
  op.traits.joint_continuation_bytes = sizeof(Coordinates);
  op.traits.joint_workspace_bytes = 1024;
  auto& output = op.traits.outputs[0];
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(Coordinates);
  output.maximum_dependency_stages = 8;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {3};
  struct Single {
    Coordinates state;
    explicit Single(int mode) : state(mode) {}
    Result<DependencyPoll> poll(const DependencyPhase& phase) {
      return state.member(phase);
    }
  };
  op.start_dependency = [mode](const DependencyQuery&,
                               const BufferAllocator& host) {
    return DependencyContinuation::make<Single>(host, mode);
  };
  op.start_joint = [mode](const auto&, const BufferAllocator& host) {
    return DependencyJointContinuation::make<Coordinates>(host, mode);
  };
  return op;
}
struct QualityState {
  int mode;
  explicit QualityState(int mode) : mode(mode) {}
  Result<std::vector<DependencyAtomOutcome>> poll(
      const DependencyJointPhase& batch) {
    std::vector<DependencyAtomOutcome> outcomes;
    const std::int64_t a[3]{1, 3, 2}, x[3]{16777217, 1, 2},
        b[3]{16777217, 4, 4};
    auto quality = mode >= 2 ? QualityReport::measured_residual(
                                   "integer-system", 3, 1, batch.allocator)
                             : QualityReport::certify_integer_diagonal(
                                   "integer-system", a, x, b, 3,
                                   batch.allocator, batch.consume_work);
    if (!quality.ok())
      return Result<std::vector<DependencyAtomOutcome>>(quality.status());
    for (const auto* phase : batch.members) {
      const auto key = dependency_atom_key(phase->query).value();
      const auto value =
          mode == 1 && key.coordinate[0] == 0 ? 16777216 : x[key.coordinate[0]];
      auto outcome =
          mode >= 3 ? Result<DependencyPoll>(Status{
                          ErrorCode::OperationFailed,
                          "iteration stopped",
                          FailureReason::NotConverged,
                          {FailureOrigin::Domain, FailureScope::Atom, key}})
                    : number(*phase, static_cast<double>(value));
      if (mode == 4) {
        FailureDetail domain{FailureOrigin::Domain,
                             FailureScope::ValidationDomain};
        domain.domain = AtomDomain{{0, 1, {0}}, {3}};
        outcome = Result<DependencyPoll>(
            Status{ErrorCode::OperationFailed, "validation did not converge",
                   FailureReason::NotConverged, domain});
      }
      outcomes.push_back({key, std::move(outcome), quality.value()});
    }
    return Result<std::vector<DependencyAtomOutcome>>(std::move(outcomes));
  }
};

std::shared_ptr<const RegionalSource> source(std::array<double, 3> numbers,
                                             bool fail_middle = false) {
  auto result = std::make_shared<RegionalSource>();
  result->descriptor = {ElementType::Float64, {3}};
  result->read = [numbers, fail_middle](const Region& region,
                                        std::uint8_t* bytes, std::uint64_t size,
                                        const BufferAllocator&,
                                        const CancellationToken&) {
    auto offset = region.dimensions()[0].offset;
    if (fail_middle && offset <= 1 &&
        offset + region.dimensions()[0].extent > 1)
      return Result<Region>(Status{ErrorCode::OperationFailed,
                                   "source transport failed",
                                   FailureReason::ShortIo,
                                   {FailureOrigin::Io, FailureScope::Group}});
    if (size != region.dimensions()[0].extent * 8)
      return Result<Region>(Status{ErrorCode::InvalidArgument, "source size"});
    std::memcpy(bytes, numbers.data() + offset, size);
    return Result<Region>(region);
  };
  return result;
}
WorkflowDocument document() {
  WorkflowDocument doc;
  doc.inputs = {{1,
                 "primary",
                 {ElementType::Float64, {3}},
                 Region::whole({3}),
                 {0, {8}},
                 {}},
                {2,
                 "fallback",
                 {ElementType::Float64, {3}},
                 Region::whole({3}),
                 {0, {8}},
                 {}}};
  doc.nodes = {{11,
                "example.guarded_reciprocal",
                {WorkflowInputReference{1}, WorkflowInputReference{2}},
                {}},
               {22, "example.scale", {WorkflowNodeOutput{11, "value"}}, {}}};
  doc.outputs = {{"sink", 22, "value"}};
  return doc;
}
void run(bool failed_source) {
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(coordinate_operation(0)).ok(),
        "register reciprocal");
  check(registry->register_operation(coordinate_operation(-1)).ok(),
        "register scale");
  check(registry->freeze().ok(), "freeze");
  GraphContext graph(document());
  auto compiled = take(Compiler(registry).compile(graph));
  ExecutionResult held;
  ResourceBudget root;
  {
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Host] = 262144;
    ExecutionContext context(registry, config);
    root = take(context.resource_budget());
    ExecutionBindings bindings{
        {{"primary", {}, source({2, 0, -1}, failed_source)},
         {"fallback", {}, source({9, 8, 4})}}};
    const DemandQuery all{{"sink", take(Footprint::all({3}))}};
    held = take(context.execute_atoms(compiled.plan, bindings, all));
    check(held.atoms.size() == 3, "three terminal observations");
    check(held.diagnostics.joint_groups == 2 &&
              held.diagnostics.joint_fallbacks == 0,
          "coordinate batches must not fall back");
    ExecutionOptions limited;
    limited.maximum_atom_observations = 2;
    check(context.execute_atoms(compiled.plan, bindings, all, {}, limited)
                  .status()
                  .code == ErrorCode::ResourceExhausted,
          "bounded collected count");
    limited.maximum_atom_observations = 0;
    auto empty = take(context.execute_atoms(
        compiled.plan, bindings, {{"sink", take(Footprint::none({3}))}}, {},
        limited));
    check(empty.atoms.empty(), "empty demand");
  }
  // Independent scalar reference: choose primary >=0, otherwise fallback;
  // zero is DivideByZero. Multiply the reciprocal by two.
  for (const auto& atom : held.atoms) {
    const auto coordinate = atom.key.coordinate[0];
    if (coordinate == 1) {
      check(!atom.outcome.ok(), "middle must fail");
      const auto& failure = atom.outcome.status();
      if (failed_source)
        check(failure.reason == FailureReason::ShortIo &&
                  failure.detail.input_id == 1 && !failure.detail.node_id &&
                  !failure.detail.atom &&
                  failure.detail.scope == FailureScope::Group,
              "source scope/provenance");
      else
        check(failure.reason == FailureReason::DivideByZero &&
                  failure.detail.node_id == 11 &&
                  failure.detail.atom == atom.key,
              "semantic origin");
      std::cout << coordinate
                << (failed_source ? ": ShortIo input=1 group\n"
                                  : ": DivideByZero node=11 atom=1\n");
    } else {
      check(atom.outcome.ok(), "independent success");
      double actual = 0;
      check(atom.outcome.value().read({coordinate}, &actual, 8).ok(),
            "owned read after context");
      check(actual == (coordinate == 0 ? 1 : .5),
            "independent scalar reference");
      std::cout << coordinate << ": " << actual << '\n';
    }
  }
  held = {};
  for (auto live : root.statistics().live.values)
    check(live == 0, "last result releases root resources");
}
void quality_example() {
  for (int mode : {0, 1, 2, 3, 4}) {
    auto registry = std::make_shared<OperationRegistry>();
    auto op = coordinate_operation(0);
    op.traits.joint_continuation_bytes = sizeof(QualityState);
    op.traits.joint_workspace_bytes = 4096;
    op.start_joint = [mode](const auto&, const BufferAllocator& host) {
      return DependencyJointContinuation::make<QualityState>(host, mode);
    };
    check(registry->register_operation(std::move(op)).ok(),
          "quality operation");
    check(registry->register_operation(coordinate_operation(-1)).ok(),
          "quality consumer");
    check(registry->freeze().ok(), "quality freeze");
    auto doc = document();
    if (mode < 3)
      doc.nodes.resize(1);
    doc.outputs = {{"estimate", mode >= 3 ? 22u : 11u, "value"}};
    GraphContext graph(doc);
    auto compiled = take(Compiler(registry).compile(graph));
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    ExecutionBindings bindings{{{"primary", {}, source({2, 0, -1})},
                                {"fallback", {}, source({9, 8, 4})}}};
    auto result = context.execute_atoms(
        compiled.plan, bindings, {{"estimate", take(Footprint::all({3}))}});
    if (mode == 1) {
      check(!result.ok() &&
                result.status().reason == FailureReason::InvalidQuality,
            "rounded estimate must not inherit an unrelated exact proof");
      continue;
    }
    auto value = take(std::move(result));
    for (const auto& atom : value.atoms) {
      check(atom.quality.has_value(), "runtime quality attachment");
      if (mode >= 3)
        check(!atom.outcome.ok() &&
                  atom.outcome.status().reason == FailureReason::NotConverged,
              "failure retains measured report without a Value");
      const auto& q = *atom.quality;
      check(q.residual() == 1, "integer residual reference");
      if (mode == 0) {
        // max|a*x-b|=1, min|a|=1. Upward rounding deliberately exceeds 1.
        check(q.evidence() == QualityEvidence::CertifiedBound &&
                  *q.error_bound() > 1,
              "certified integer bound");
      } else {
        check(q.evidence() == QualityEvidence::Measured && !q.error_bound(),
              "measured residual has no certified bound");
      }
    }
  }
  std::cout
      << "quality: Measured, CertifiedBound, mismatched estimate rejected\n";
}
}  // namespace
int main() {
  try {
    run(false);
    run(true);
    quality_example();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
