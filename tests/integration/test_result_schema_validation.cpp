#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "photospider/data/lut3d_bake.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;

struct ReportProgram {
  bool malformed;
  explicit ReportProgram(bool malformed) : malformed(malformed) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    const std::uint8_t passed = 1;
    const std::array<double, 9> axis{0, 1, 1, 0, 1, 1, 0, 1, 1};
    const std::int64_t count = 1, failures = malformed ? 1 : 0, first = -1;
    const std::array<double, 3> zeros{};
    const std::array<double, 9> points{.5, .5, .5, .5, .5, .5, .5, .5, .5};
    const std::array<std::int64_t, 3> indices{};
    const std::array<const void*, 11> fields{
        &passed,      axis.data(),   &count,         &failures,
        zeros.data(), points.data(), indices.data(), &first,
        zeros.data(), zeros.data(),  zeros.data()};
    const std::array<std::uint64_t, 11> sizes{1,  72, 8,  8,  24, 72,
                                              24, 8,  24, 24, 24};
    for (std::uint32_t field = 0; field < fields.size(); ++field) {
      const auto rows = (field == 1 || field == 5) ? 3U : 1U;
      check(builder.append(
          field, rows,
          ByteView(static_cast<const std::uint8_t*>(fields[field]),
                   sizes[field])));
      check(builder.publish(field, rows));
    }
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};

int reports() {
  Lut3dBakeDescription description;
  description.recipe_identity = std::string(64, 'a');
  auto schema = take(lut3d_bake_schema(description));
  PS_CHECK(schema.fields.size() == 11 && schema.validate(true).ok());
  auto wrong = schema;
  wrong.fields[1].record_shape = {2};
  PS_CHECK(!wrong.validate(true).ok());
  auto truncated = schema;
  truncated.metadata[0].payload.pop_back();
  PS_CHECK(!truncated.validate(true).ok());

  ResultRef retained;
  {
    auto registry = std::make_shared<OperationRegistry>();
    for (bool malformed : {false, true}) {
      OperationDefinition definition;
      definition.key = malformed ? "test.report_bad" : "test.report";
      definition.traits.outputs = {multi_result::output("report", schema)};
      definition.traits.outputs[0].region_rule = OperationRegionRule::Whole;
      definition.start_result = [malformed](const ResultProgramQuery&,
                                            const BufferAllocator& allocator) {
        return ResultContinuation::make<ReportProgram>(allocator, malformed);
      };
      check(registry->register_operation(std::move(definition)));
    }
    check(registry->freeze());
    ExecutionContext context(registry);
    for (bool malformed : {false, true}) {
      WorkflowDocument document;
      document.nodes = {
          {1, malformed ? "test.report_bad" : "test.report", {}, {}}};
      document.outputs = {{"report", 1, "report"}};
      GraphContext graph(document);
      auto plan = take(Compiler(registry).compile(graph)).plan;
      auto result = context.execute(plan);
      if (malformed) {
        PS_CHECK(!result.ok() &&
                 result.status().code == ErrorCode::OperationFailed &&
                 result.status().reason == FailureReason::InvalidDomain &&
                 result.status().detail.scope == FailureScope::Association);
      } else {
        PS_REQUIRE_OK(result);
        retained = result.value().results.at("report");
        ExecutionOptions options;
        options.maximum_result_window_bytes = 71;
        context.clear_result_cache();
        auto bounded = context.execute(plan, {}, {}, options);
        PS_CHECK(!bounded.ok() &&
                 bounded.status().code == ErrorCode::ResourceExhausted &&
                 bounded.status().reason == FailureReason::CapacityLimit);
      }
    }
  }
  const auto report = take(read_lut3d_bake_report(retained, 72));
  PS_CHECK(report.passed && report.validation_count == 1 &&
           report.failed_count == 0 && report.first_failure_index == -1);
  PS_CHECK(report.axis == (std::array<double, 9>{0, 1, 1, 0, 1, 1, 0, 1, 1}));
  PS_CHECK(report.max_abs_error == (std::array<double, 3>{}) &&
           report.max_error_index == (std::array<std::int64_t, 3>{}));
  PS_CHECK(report.max_error_point ==
           (std::array<double, 9>{.5, .5, .5, .5, .5, .5, .5, .5, .5}));
  PS_CHECK(report.first_failure_input == (std::array<double, 3>{}) &&
           report.first_failure_reference == (std::array<double, 3>{}) &&
           report.first_failure_lut == (std::array<double, 3>{}));
  CancellationSource cancellation;
  cancellation.cancel();
  PS_CHECK(read_lut3d_bake_report(retained, 72, cancellation.token())
               .status()
               .code == ErrorCode::Cancelled);
  return 0;
}
}  // namespace
int main() {
  try {
    return reports();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
