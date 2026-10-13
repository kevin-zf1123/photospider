#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

#include "support/result_diagnostics_fixture.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct ReportProgram {
  bool requested = false;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back({0, true, false, {}});
      return Result<ResultProgramPoll>(std::move(need));
    }
    std::array<double, 5> samples{};
    for (std::uint64_t i = 0; i < samples.size(); ++i)
      check(phase.read_tensor(0, 0, {i}, &samples[i], sizeof(double)));
    NumericDiagnostics report;
    report.profile = CpuNumericProfile::Strict;
    const char implementation[] = "test.numeric-report/1";
    std::memcpy(report.implementation.data(), implementation,
                sizeof(implementation));
    report.evaluated_values = samples.size();
    check(phase.report_numeric(report));
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
    NumericDiagnostics report;
    report.profile = CpuNumericProfile::Strict;
    report.implementation[0] = 'x';
    report.evaluated_values = 1;
    check(phase.report_numeric(report));
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
  const auto schema = multi_result::schema(ElementType::Float64, {5});
  OperationDefinition operation;
  operation.key = "test.numeric_report";
  operation.traits.input_count = 1;
  operation.traits.input_schema.resize(1);
  operation.traits.input_schema[0].result_schema_id = schema.id;
  operation.traits.input_schema[0].result_schema_version = schema.version;
  operation.traits.outputs = {multi_result::output("value", schema)};
  operation.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  operation.traits.outputs[0].continuation_bytes = sizeof(ReportProgram);
  operation.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<ReportProgram>(allocator);
  };
  check(registry->register_operation(std::move(operation)));
  OperationDefinition failed;
  failed.key = "test.failed_report";
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
    auto result = take(context.execute(
        compiled.plan, {{multi_result::binding(root, "input", 7, schema)}}));
    double sum = 0;
    for (unsigned i = 0; i < 5; ++i)
      sum += multi_result::number(result.results.at("value"), {i});
    PS_CHECK(sum == 35);
    PS_CHECK(result.diagnostics.operation_timings.size() == 1);
    const auto& timing = result.diagnostics.operation_timings[0];
    PS_CHECK(timing.invocation_count == 2 && timing.computed_elements == 5 &&
             timing.numeric.evaluated_values == 5);
  }
  // Numeric-report accounting must not replace an upstream failure when the
  // producer uses all remaining Root Host capacity before returning failure.
  document.inputs.clear();
  document.nodes = {
      {2, "test.failed_report", {}, {}},
      {1, "test.numeric_report", {WorkflowNodeOutput{2, "value"}}, {}}};
  GraphContext failure_graph(document);
  auto failure_plan = take(Compiler(registry).compile(failure_graph));
  auto failure = context.execute(failure_plan.plan);
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
  PS_CHECK(poll_reports() == 0);
  using result_diagnostics::verify_cpp_fixed_broadcast;
  PS_CHECK(verify_cpp_fixed_broadcast({4}) == 0);
  PS_CHECK(verify_cpp_fixed_broadcast({UINT64_MAX}) == 0);
  PS_CHECK(verify_cpp_fixed_broadcast({UINT64_MAX, 2}) == 0);
  PS_CHECK(verify_cpp_fixed_broadcast({UINT64_MAX}, true) == 0);
  return 0;
}
