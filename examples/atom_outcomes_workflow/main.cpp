#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "operations.hpp"  // NOLINT(build/include_subdir)
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
OperationDefinition coordinate_operation(int mode) {
  return atom_result::operation(
      mode, mode == -1 ? "example.scale" : "example.guarded_reciprocal");
}
struct QualityState {
  int mode;
  explicit QualityState(int mode) : mode(mode) {}
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& batch) {
    ResourceVector<ResultJointOutcome> outcomes;
    const std::int64_t a[3]{1, 3, 2}, x[3]{16777217, 1, 2},
        b[3]{16777217, 4, 4};
    auto quality = mode >= 2 ? QualityReport::measured_residual(
                                   "integer-system", 3, 1, batch.allocator)
                             : QualityReport::certify_integer_diagonal(
                                   "integer-system", a, x, b, 3,
                                   batch.allocator, batch.consume_work);
    if (!quality.ok())
      return Result<ResourceVector<ResultJointOutcome>>(quality.status());
    for (const auto* phase : batch.members) {
      const auto key = result_atom_key(phase->query).value();
      const auto value =
          mode == 1 && key.coordinate[0] == 0 ? 16777216 : x[key.coordinate[0]];
      auto outcome =
          mode >= 3 ? Result<ResultProgramPoll>(Status{
                          ErrorCode::OperationFailed,
                          "iteration stopped",
                          FailureReason::NotConverged,
                          {FailureOrigin::Domain, FailureScope::Atom, key}})
                    : Result<ResultProgramPoll>(atom_result::publish(
                          *phase, static_cast<double>(value), 0, false));
      if (mode == 4) {
        FailureDetail domain{FailureOrigin::Domain,
                             FailureScope::ValidationDomain};
        domain.domain = AtomDomain{{0, 1, {0}}, {3}};
        outcome = Result<ResultProgramPoll>(
            Status{ErrorCode::OperationFailed, "validation did not converge",
                   FailureReason::NotConverged, domain});
      }
      outcomes.push_back({key, std::move(outcome), quality.value()});
    }
    return Result<ResourceVector<ResultJointOutcome>>(std::move(outcomes));
  }
};

WorkflowDocument document(bool failed_source = false) {
  WorkflowDocument doc;
  doc.inputs = {atom_result::declaration(1, "primary"),
                atom_result::declaration(2, "fallback")};
  doc.nodes = {{11,
                "example.guarded_reciprocal",
                {WorkflowInputReference{1}, WorkflowInputReference{2}},
                {}},
               {22, "example.scale", {WorkflowNodeOutput{11, "value"}}, {}}};
  if (failed_source) {
    doc.nodes[0].inputs[0] = WorkflowNodeOutput{10, "value"};
    doc.nodes.insert(
        doc.nodes.begin(),
        {10, "example.transport", {WorkflowInputReference{1}}, {}});
  }
  doc.outputs = {{"sink", 22, "value"}};
  return doc;
}
void run(bool failed_source) {
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(coordinate_operation(0)).ok(),
        "register reciprocal");
  check(registry->register_operation(coordinate_operation(-1)).ok(),
        "register scale");
  if (failed_source)
    check(registry
              ->register_operation(
                  atom_result::operation(-2, "example.transport"))
              .ok(),
          "register failing transport");
  check(registry->freeze().ok(), "freeze");
  GraphContext graph(document(failed_source));
  auto compiled = take(Compiler(registry).compile(graph));
  ExecutionResult held;
  ResourceBudget root;
  {
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Host] = 1048576;
    ExecutionContext context(registry, config);
    root = take(context.resource_budget());
    ExecutionBindings bindings{
        {atom_result::binding(root, "primary", {2, 0, -1}),
         atom_result::binding(root, "fallback", {9, 8, 4})}};
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
                  failure.detail.node_id == 10 && !failure.detail.input_id &&
                  !failure.detail.atom &&
                  failure.detail.scope == FailureScope::Group,
              "source scope/provenance");
      else
        check(failure.reason == FailureReason::DivideByZero &&
                  failure.detail.node_id == 11 &&
                  failure.detail.atom == atom.key,
              "semantic origin");
      std::cout << coordinate
                << (failed_source ? ": ShortIo node=10 group\n"
                                  : ": DivideByZero node=11 atom=1\n");
    } else {
      check(atom.outcome.ok(), "independent success");
      double actual = 0;
      check(atom.outcome.value()
                .read_tensor(take(atom.outcome.value().descriptor()), 0,
                             {coordinate}, &actual, 8)
                .ok(),
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
    op.start_result_joint = [mode](const auto&, const BufferAllocator& host) {
      return ResultJointContinuation::make<QualityState>(host, mode);
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
    auto root = take(context.resource_budget());
    ExecutionBindings bindings{
        {atom_result::binding(root, "primary", {2, 0, -1}),
         atom_result::binding(root, "fallback", {9, 8, 4})}};
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
              "failure retains measured report");
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
