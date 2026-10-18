#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <utility>

#include "support/diagnostic_counter_fixture.hpp"
#include "support/result_diagnostics_fixture.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
int diagnostic_owner_restoration() {
  struct Restore {
    std::fenv_t saved{};
    Restore() { std::fegetenv(&saved); }
    ~Restore() { std::fesetenv(&saved); }
  } restore;
  for (unsigned mode = 0; mode < 3; ++mode) {
    unsigned definitions_destroyed = 0, owners_destroyed = 0;
    PS_CHECK(std::fesetround(FE_DOWNWARD) == 0);
    std::feclearexcept(FE_ALL_EXCEPT);
    std::feraiseexcept(FE_DIVBYZERO);
    ResourceBudget root;
    {
      DiagnosticAccumulator accumulator(root);
      auto type = std::shared_ptr<DiagnosticDefinition>(
          new DiagnosticDefinition(diagnostic_fixture::definition()),
          [&](DiagnosticDefinition* definition) {
            delete definition;
            ++definitions_destroyed;
            std::fesetround(FE_UPWARD);
            std::feraiseexcept(FE_INVALID);
          });
      type->owner =
          std::shared_ptr<const void>(new int(1), [&](const void* owner) {
            delete static_cast<const int*>(owner);
            ++owners_destroyed;
            std::fesetround(FE_TOWARDZERO);
            std::feraiseexcept(FE_OVERFLOW);
          });
      check(accumulator.initialize({type}));
      type.reset();
      if (mode == 1)
        check(accumulator.initialize({}));
      if (mode == 2)
        accumulator = DiagnosticAccumulator(root);
      PS_CHECK(std::fegetround() == FE_DOWNWARD &&
               std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO);
    }
    PS_CHECK(definitions_destroyed == 1 && owners_destroyed == 1);
    PS_CHECK(std::fegetround() == FE_DOWNWARD &&
             std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO);
    for (auto live : root.statistics().live.values)
      PS_CHECK(live == 0);
  }
  return 0;
}
struct ReportProgram {
  bool requested = false;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back(
          {0, 0, false, take(TileSet::range(phase.resources, 0, 1))});
      return Result<ResultProgramPoll>(std::move(need));
    }
    std::array<double, 5> samples{};
    for (std::uint64_t i = 0; i < samples.size(); ++i)
      check(phase.read_tensor(0, 0, {i}, &samples[i], sizeof(double)));
    check(diagnostic_fixture::report(phase, samples.size()));
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    check(builder.publish_tensor(
        0, Region::whole({samples.size()}),
        ByteView(reinterpret_cast<const std::uint8_t*>(samples.data()),
                 sizeof(samples))));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
struct FailingReportProgram {
  ResourceLease fill;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    check(diagnostic_fixture::report(phase, 1));
    const auto available =
        phase.resources.available_capacity()[ResourceKind::Host];
    fill = take(phase.resources.reserve(ResourceCapacity::host(
        available - ResourceBudget::lease_metadata_bytes())));
    if (phase.resources.available_capacity()[ResourceKind::Host] != 0)
      return Result<ResultProgramPoll>(
          Status{ErrorCode::Internal, "Host fixture failed to fill capacity"});
    return Result<ResultProgramPoll>(
        Status{ErrorCode::OperationFailed,
               "upstream report failed",
               FailureReason::ShortIo,
               {FailureOrigin::Io, FailureScope::Group}});
  }
};
int poll_reports() {
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_diagnostic(diagnostic_fixture::definition()));
  const auto schema = multi_result::schema(ElementType::Float64, {5});
  OperationDefinition operation;
  operation.key = "test.numeric_report";
  operation.traits.diagnostics = {diagnostic_fixture::identity()};
  operation.traits.input_count = 1;
  operation.traits.input_schema.resize(1);
  operation.traits.input_schema[0].result_schema_id = schema.id;
  operation.traits.input_schema[0].result_schema_version = schema.version;
  operation.traits.outputs = {multi_result::output("value", schema)};
  operation.traits.outputs[0].continuation_bytes = sizeof(ReportProgram);
  operation.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<ReportProgram>(allocator);
  };
  check(registry->register_operation(std::move(operation)));
  OperationDefinition failed;
  failed.key = "test.failed_report";
  failed.traits.diagnostics = {diagnostic_fixture::identity()};
  failed.traits.outputs = {multi_result::output("value", schema)};
  failed.traits.outputs[0].continuation_bytes = sizeof(FailingReportProgram);
  failed.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<FailingReportProgram>(allocator);
  };
  check(registry->register_operation(std::move(failed)));
  check(registry->freeze());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "input", schema)};
  document.nodes = {
      {1, "test.numeric_report", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  auto compiled = take(Compiler(registry).compile(graph));
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Host] = 4194304;
  config.managed_resources->capacity[ResourceKind::Metadata] = 4194304;
  ExecutionContext context(registry, config);
  const auto root = take(context.resource_budget());
  {
    auto result = take(
        context.execute(compiled.plan.value(),
                        {{multi_result::binding(root, "input", 7, schema)}}));
    double sum = 0;
    for (unsigned i = 0; i < 5; ++i)
      sum += multi_result::number(result.results.at("value"), {i});
    PS_CHECK(sum == 35);
    PS_CHECK(result.diagnostics.operation_timings.size() == 2);
    const auto& request = result.diagnostics.operation_timings[0];
    const auto& timing = result.diagnostics.operation_timings[1];
    PS_CHECK(
        request.output.node_id == 1 && request.backend == Backend::Cpu &&
        request.outcome == ErrorCode::Ok && request.invocation_count == 1 &&
        request.computed_elements == 0 && request.reports &&
        request.reports->find(diagnostic_fixture::identity()).status().code ==
            ErrorCode::NotFound);
    PS_CHECK(timing.output.node_id == 1 && timing.backend == Backend::Cpu &&
             timing.outcome == ErrorCode::Ok && timing.invocation_count == 1 &&
             timing.computed_elements == 5 &&
             diagnostic_fixture::count(*timing.reports) == 5);
  }
  // Numeric-report accounting must not replace an upstream failure when the
  // producer uses all remaining Root Host capacity before returning failure.
  document.inputs.clear();
  document.nodes = {
      {2, "test.failed_report", {}, {}},
      {1, "test.numeric_report", {WorkflowNodeOutput{2, "value"}}, {}}};
  GraphContext failure_graph(document);
  auto failure_plan = take(Compiler(registry).compile(failure_graph));
  auto failure = context.execute(failure_plan.plan.value());
  if (failure.ok() || failure.status().code != ErrorCode::OperationFailed)
    std::cerr << "report failure: "
              << static_cast<unsigned>(failure.status().code) << " "
              << failure.status().message << "\n";
  PS_CHECK(!failure.ok() &&
           failure.status().code == ErrorCode::OperationFailed);
  PS_CHECK(failure.status().reason == FailureReason::ShortIo &&
           failure.status().detail.origin == FailureOrigin::Io &&
           failure.status().detail.node_id == 2);
  PS_CHECK(root.statistics().peak[ResourceKind::Host] == 4194304 &&
           root.available_capacity()[ResourceKind::Host] > 0);
  return 0;
}
}  // namespace

int main() {
  PS_CHECK(diagnostic_owner_restoration() == 0);
  PS_CHECK(poll_reports() == 0);
  using result_diagnostics::verify_cpp_fixed_broadcast;
  PS_CHECK(verify_cpp_fixed_broadcast({4}) == 0);
  PS_CHECK(verify_cpp_fixed_broadcast({UINT64_MAX}) == 0);
  PS_CHECK(verify_cpp_fixed_broadcast({UINT64_MAX, 2}) == 0);
  PS_CHECK(verify_cpp_fixed_broadcast({UINT64_MAX}, true) == 0);
  return 0;
}
