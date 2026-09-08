#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "image_fixture.hpp"  // NOLINT(build/include_subdir)

namespace {
constexpr const char* kOracleName = "s1-rgba32f-exposure-opacity-v1";

void require(bool condition, const std::string& detail) {
  if (!condition)
    throw std::runtime_error(detail);
}

void print_value(const std::string& label, const ps::Value& value) {
  std::cout << label << " Float32 shape=";
  for (auto extent : value.descriptor().shape)
    std::cout << extent << ',';
  std::cout << " region=";
  for (const auto& axis : value.region().dimensions())
    std::cout << axis.offset << ':' << axis.extent << ',';
  std::cout << " byte_offset=" << value.layout().byte_offset << " strides=";
  for (auto stride : value.layout().byte_strides)
    std::cout << stride << ',';
  std::cout << " facets=" << value.facets().size();
  for (const auto& facet : value.facets())
    std::cout << ' ' << facet.key << '@' << facet.version << '='
              << std::string(facet.payload.begin(), facet.payload.end());
  std::cout << " bytes=" << value.bytes().size() << " values=";
  for (std::size_t offset = 0; offset < value.bytes().size(); offset += 4) {
    float number = 0;
    std::memcpy(&number, value.bytes().data() + offset, sizeof(number));
    std::cout << number << ',';
  }
  std::cout << '\n';
}

bool diagnostics_match(const ps::ExecutionDiagnostics& diagnostics,
                       const std::string& plan) {
  if (diagnostics.plan_digest != plan || diagnostics.result_digest.empty() ||
      diagnostics.operation_timings.size() != 2 ||
      diagnostics.selected_backends.size() != 2 ||
      diagnostics.transfer_count != 0 || diagnostics.transfer_bytes != 0 ||
      diagnostics.peak_live_bytes != 48 ||
      !diagnostics.fallback_reasons.empty())
    return false;
  for (std::size_t index = 0; index < 2; ++index) {
    const auto& timing = diagnostics.operation_timings[index];
    const auto selected = diagnostics.selected_backends.find(timing.node_id);
    if (timing.node_id != (index + 1) * 10 ||
        timing.backend != ps::Backend::Cpu ||
        timing.outcome != ps::ErrorCode::Ok ||
        selected == diagnostics.selected_backends.end() ||
        selected->second != ps::Backend::Cpu)
      return false;
  }
  return true;
}

void print_diagnostics(const ps::ExecutionDiagnostics& diagnostics) {
  std::cout << "raw plan_digest=" << diagnostics.plan_digest
            << " result_digest=" << diagnostics.result_digest
            << " execute_us=" << diagnostics.execute_us
            << " callbacks=" << diagnostics.operation_timings.size()
            << " transfer_count=" << diagnostics.transfer_count
            << " transfer_bytes=" << diagnostics.transfer_bytes
            << " peak_allocated_bytes=" << diagnostics.peak_live_bytes
            << " fallback_count=" << diagnostics.fallback_reasons.size()
            << '\n';
  for (const auto& timing : diagnostics.operation_timings)
    std::cout << "raw node=" << timing.node_id
              << " backend=CPU duration_us=" << timing.duration_us
              << " outcome=" << static_cast<unsigned>(timing.outcome) << '\n';
}

void check_demand(const ps::ExecutionPlan& plan) {
  require(plan.steps().size() == 2 && plan.input_declarations().size() == 3,
          "expected three inputs and two operation steps");
  const auto image_demand = s1_fixture::demand().output_regions.at("result");
  for (const auto& step : plan.steps()) {
    require(step.backend == ps::Backend::Cpu && step.planned_bytes == 16 &&
                step.input_demands.size() == 2,
            "unexpected CPU plan or output allocation bound");
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const auto expected = image_demand.dimensions()[axis];
      const auto actual = step.input_demands[0].dimensions().at(axis);
      const auto output = step.output_demand.dimensions().at(axis);
      require(actual.offset == expected.offset &&
                  actual.extent == expected.extent &&
                  output.offset == expected.offset &&
                  output.extent == expected.extent,
              "image demand differs from pixel (0,1), all channels");
    }
    const auto& scalar = step.input_demands[1];
    require(scalar.rank() == 1 && scalar.dimensions()[0].offset == 0 &&
                scalar.dimensions()[0].extent == 1,
            "scalar demand must remain whole {1}");
  }
}

void run(const std::shared_ptr<ps::OperationRegistry>& operations) {
  ps::Compiler compiler(operations);
  ps::GraphContext graph(s1_fixture::document());
  // This single compiled plan is retained for both direct executions below.
  auto compiled = compiler.compile(graph, s1_fixture::demand());
  require(compiled.ok(), compiled.status().message);
  const auto& workflow = compiled.value();
  check_demand(workflow.plan);
  const auto plan = workflow.plan.digest().value;
  ps::ExecutionContext execution(operations, {1, false, 16, 128});
  std::cout << "reuse compile_count=1 execute_count=2 demand=0:1,1:1,0:4\n"
            << "raw analyze_us=" << workflow.diagnostics.analyze_us
            << " optimize_us=" << workflow.diagnostics.optimize_us
            << " plan_us=" << workflow.diagnostics.plan_us << '\n';
  std::vector<std::string> result_digests;
  for (bool second : {false, true}) {
    const auto bindings = s1_fixture::bindings(second);
    const auto result = execution.execute(workflow.plan, bindings);
    require(result.ok(), result.status().message);
    require(diagnostics_match(result.value().diagnostics, plan),
            "direct execution diagnostics mismatch");
    std::cout << "payload=" << (second ? 'B' : 'A') << '\n';
    for (const auto& input : bindings.inputs)
      print_value(input.name, input.value);
    for (const auto& output : result.value().values)
      print_value(output.first, output.second);
    print_diagnostics(result.value().diagnostics);
    const bool accepted = s1_fixture::oracle(result.value(), second);
    std::cout << "correctness oracle=" << kOracleName
              << " checked=true accepted=" << accepted
              << " comparison=bit-exact\n";
    require(accepted,
            "named output differs from frozen/independent CPU oracle");
    result_digests.push_back(result.value().diagnostics.result_digest);
  }
  require(
      result_digests[0] != result_digests[1],
      "distinct fixture outputs must have distinct observed result digests");

  // RawBenchmarkRunner deliberately compiles each sample independently.
  // Keep its measurements separate from the compile-once reuse observation.
  ps::RawBenchmarkRunner runner(&compiler, &execution);
  for (bool second : {false, true}) {
    ps::RawBenchmarkOptions options;
    options.iterations = 2;
    options.planning = s1_fixture::demand();
    options.bindings = s1_fixture::bindings(second);
    options.oracle_name = kOracleName;
    options.correctness_oracle = [second](const ps::ExecutionResult& result) {
      return ps::CorrectnessObservation{s1_fixture::oracle(result, second),
                                        "bounded 16-channel CPU reference"};
    };
    const auto report = runner.run(graph, options);
    require(report.ok(), report.status().message);
    require(report.value().oracle_name == kOracleName &&
                report.value().samples.size() == options.iterations,
            "raw benchmark report identity/count mismatch");
    for (const auto& sample : report.value().samples) {
      require(
          sample.outcome == ps::ErrorCode::Ok && sample.correctness_checked &&
              sample.correctness_accepted &&
              sample.oracle_name == kOracleName &&
              diagnostics_match(sample.execution, plan) &&
              sample.execution.result_digest == result_digests[second ? 1 : 0],
          "raw benchmark sample mismatch: " + sample.reason);
      std::cout << "benchmark payload=" << (second ? 'B' : 'A')
                << " iteration=" << sample.iteration
                << " analyze_us=" << sample.compilation.analyze_us
                << " optimize_us=" << sample.compilation.optimize_us
                << " plan_us=" << sample.compilation.plan_us << '\n';
      print_diagnostics(sample.execution);
      std::cout << "correctness oracle=" << sample.oracle_name
                << " checked=" << sample.correctness_checked
                << " accepted=" << sample.correctness_accepted
                << " detail=" << sample.reason << '\n';
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 1 || argc == 2,
            "usage: photospider_image_vertical [trusted-operation-library]");
    std::cout << std::boolalpha << std::setprecision(9);
    auto operations = argc == 2 ? std::make_shared<ps::OperationRegistry>()
                                : ps::make_default_operation_registry();
    if (argc == 2) {
      const auto status = operations->load_plugin(argv[1]);
      require(status.ok(), status.message);
      operations->freeze();
    }
    std::cout << "operations=" << (argc == 2 ? "rgba32f-package" : "built-in")
              << '\n';
    run(operations);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "image vertical failed: " << error.what() << '\n';
    return 1;
  }
}
