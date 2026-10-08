#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "image_fixture.hpp"  // NOLINT(build/include_subdir)

namespace {
constexpr const char* kOracleName = "s1-result-exposure-opacity-v1";
void require(bool condition, const std::string& detail) {
  if (!condition)
    throw std::runtime_error(detail);
}
void print_result(const std::string& label, const ps::ResultRef& result) {
  const auto descriptor = s1_fixture::take(result.descriptor());
  std::cout << label << " schema=" << result.schema().id << " sample_shape=";
  for (auto extent : result.schema().tensors[0].sample_shape())
    std::cout << extent << ',';
  std::cout << " values=";
  s1_fixture::check(descriptor.tensor_coverage(0).visit(
      [&](const auto& at) {
        float value = 0;
        auto status = result.read_tensor(descriptor, 0, at, &value, 4);
        if (status.ok())
          std::cout << value << ',';
        return status;
      },
      16));
  std::cout << '\n';
}
bool diagnostics_match(const ps::ExecutionDiagnostics& d,
                       const std::string& plan) {
  if (std::string_view(d.plan_digest) != plan || d.result_digest.empty() ||
      d.operation_timings.size() != 2 || d.selected_backends.size() != 2 ||
      !d.fallback_reasons.empty())
    return false;
  for (const auto& timing : d.operation_timings)
    if (timing.backend != ps::Backend::Cpu ||
        timing.outcome != ps::ErrorCode::Ok ||
        d.selected_backends.at(timing.output) != ps::Backend::Cpu)
      return false;
  return true;
}
void run(const std::shared_ptr<ps::OperationRegistry>& registry) {
  ps::ExecutionContextConfig config;
  config.gpu_enabled = false;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(registry, config);
  auto root = s1_fixture::take(execution.resource_budget());
  ps::Compiler compiler(registry);
  ps::GraphContext graph(s1_fixture::document());
  auto compiled =
      s1_fixture::take(compiler.compile(graph, s1_fixture::demand()));
  const auto plan = compiled.plan.digest().value;
  require(compiled.plan.steps().size() == 2, "expected two operations");
  std::vector<ps::ResourceString> digests;
  for (bool second : {false, true}) {
    auto bindings = s1_fixture::bindings(root, second);
    auto result = s1_fixture::take(execution.execute(compiled.plan, bindings));
    require(diagnostics_match(result.diagnostics, plan),
            "execution diagnostics");
    require(s1_fixture::oracle(result, second), "independent bit oracle");
    const auto support = s1_fixture::take(result.dependencies.source_support());
    require(support.at("image") ==
                s1_fixture::take(ps::Footprint::from_regions(
                    {1, 1, 2, 2, 4},
                    {s1_fixture::demand().output_regions.at("result")})),
            "image support must equal the requested pixel");
    for (const auto& name : {"gain", "opacity"})
      require(support.at(name) == s1_fixture::take(ps::Footprint::all({1})),
              "control support must contain its whole scalar");
    std::cout << "payload=" << (second ? 'B' : 'A') << '\n';
    print_result("result", result.results.at("result"));
    std::cout << "execute_us=" << result.diagnostics.execute_us
              << " peak_payload_bytes="
              << root.statistics().peak[ps::ResourceKind::Payload]
              << " oracle=" << kOracleName << " bit_exact=true\n";
    digests.push_back(result.diagnostics.result_digest);
  }
  require(digests[0] != digests[1],
          "different payloads need different observed digests");
  ps::RawBenchmarkRunner runner(&compiler, &execution);
  for (bool second : {false, true}) {
    ps::RawBenchmarkOptions options;
    options.iterations = 2;
    options.planning = s1_fixture::demand();
    options.bindings = s1_fixture::bindings(root, second);
    options.oracle_name = kOracleName;
    options.correctness_oracle = [second](const ps::ExecutionResult& result) {
      return ps::CorrectnessObservation{s1_fixture::oracle(result, second),
                                        "binary-fraction oracle"};
    };
    auto report = s1_fixture::take(runner.run(graph, options));
    require(report.samples.size() == 2, "benchmark sample count");
    for (const auto& sample : report.samples)
      require(sample.outcome == ps::ErrorCode::Ok &&
                  sample.correctness_checked && sample.correctness_accepted &&
                  diagnostics_match(sample.execution, plan) &&
                  sample.execution.result_digest == digests[second ? 1 : 0],
              "benchmark sample oracle and identity");
  }
  std::cout << "compile_once_executions=2 benchmark_samples=4 oracle=passed\n";
}
}  // namespace
int main(int argc, char**) {
  try {
    require(argc == 1, "usage: photospider_image_vertical");
    std::cout << std::boolalpha << std::setprecision(9);
    run(ps::make_default_operation_registry());
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
