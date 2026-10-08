#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "result_fixture.hpp"  // NOLINT(build/include_subdir)

namespace {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
WorkflowDocument graph(const Value& input, bool tiled, bool gpu = false) {
  WorkflowDocument result;
  perlin_fixture::declare(&result, input);
  result.nodes = {{1,
                   gpu     ? "noise.perlin2002_3d_v1_strict_gpu"
                   : tiled ? "noise.perlin2002_3d_v1_strict_cpu_tiled"
                           : "noise.perlin2002_3d_v1_strict_cpu_whole",
                   {WorkflowInputReference{1}},
                   {}}};
  result.outputs = {{"values", 1, "values"}};
  return result;
}
}  // namespace
int main(int argc, char** argv) try {
  const std::string mode = argc > 1 ? argv[1] : "tiled";
  const unsigned count = argc > 2 ? std::stoul(argv[2]) : 16384;
  const unsigned workers = argc > 3 ? std::stoul(argv[3]) : 4;
  const unsigned repeat = argc > 4 ? std::stoul(argv[4]) : 5;
  const unsigned tile_width = argc > 5 ? std::stoul(argv[5]) : 128;
  if ((mode != "whole" && mode != "tiled" && mode != "gpu") || !count ||
      count > 65536 || !workers || workers > 64 || !repeat || repeat > 10000 ||
      !tile_width || tile_width > 4096 || (tile_width & (tile_width - 1)))
    throw std::runtime_error(
        "usage: whole|tiled|gpu count[1..65536] workers[1..64] "
        "repeats[1..10000] "
        "tile_width[power-of-two,1..4096]");
  auto registry = make_default_operation_registry();
  auto input_writer = take(
      MutableValue::allocate({ElementType::Float64, {count, 3}},
                             Region::whole({count, 3}), BufferAllocator{}));
  for (unsigned i = 0; i < count * 3; ++i) {
    const double value =
        static_cast<double>((i * 1709U + 719U) % 131071U) / 65536. - 1.;
    std::memcpy(input_writer.data() + i * 8, &value, 8);
  }
  const auto input = take(std::move(input_writer).publish());
  ExecutionOptions options;
  options.dependencies.maximum_stages = 3 * count + 1;
  options.dependencies.maximum_work = UINT64_C(1000000000);
  options.maximum_dependency_work = UINT64_C(1000000000);
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.gpu_enabled = false;
  config.managed_resources = ResourceLimits{};
  ExecutionContext reference_context(registry, config);
  GraphContext reference_graph(graph(input, false));
  auto reference_plan = take(Compiler(registry).compile(reference_graph));
  const auto reference = take(reference_context.execute(
      reference_plan.plan,
      perlin_fixture::bind(take(reference_context.resource_budget()), input),
      {}, options));
  const auto reference_bytes =
      numeric_result_fixture::bytes(reference.results.at("values"));
  config.cpu_workers = workers;
  config.gpu_enabled = mode == "gpu";
  ExecutionContext context(registry, config);
  if (mode == "gpu" && !context.gpu_enabled()) {
    std::cerr << "native GPU unavailable\n";
    return 77;
  }
  GraphContext source(graph(input, mode == "tiled", mode == "gpu"));
  PlanningOptions planning;
  planning.tile_width = tile_width;
  if (mode == "gpu")
    planning.execution_mode = ExecutionMode::NativeGpu;
  auto compiled = take(Compiler(registry).compile(source, planning));
  std::vector<std::uint8_t> bytes(count * 8);
  std::vector<double> timings;
  ExecutionDiagnostics diagnostics;
  const auto budget = take(context.resource_budget());
  const auto bindings = perlin_fixture::bind(budget, input);
  std::uint64_t work = 0;
  for (unsigned iteration = 0; iteration <= repeat; ++iteration) {
    const auto prior = budget.statistics().issued.work;
    std::uint64_t next = 0;
    const auto start = std::chrono::steady_clock::now();
    auto seen = take(Footprint::none({count}));
    options.result_publication = [&](ValueRef ref, const ResultRef& object) {
      if (mode != "tiled" || ref.node_id != 1)
        return Status::success();
      const auto facts = take(object.descriptor(false));
      const auto added = take(facts.tensor_coverage(0).subtract(seen));
      for (const auto& box : added.boxes()) {
        const auto& span = box.dimensions()[0];
        if (span.offset != next || span.extent > tile_width ||
            span.offset + span.extent > count)
          return Status{ErrorCode::Internal, "invalid tile order or coverage"};
        auto window = take(object.acquire_tensor(facts, 0, box));
        auto run = take(window.row_run({span.offset}));
        if (run.samples != span.extent || run.sample_stride_bytes != 8)
          return Status{ErrorCode::Internal, "expected packed output tile"};
        std::memcpy(bytes.data() + span.offset * 8, run.data, span.extent * 8);
        next += span.extent;
      }
      seen = facts.tensor_coverage(0);
      return Status::success();
    };
    auto result = take(context.execute(compiled.plan, bindings, {}, options));
    diagnostics = std::move(result.diagnostics);
    if (mode == "tiled" && next != count)
      throw std::runtime_error("incomplete tile coverage");
    if (mode != "tiled")
      bytes = numeric_result_fixture::bytes(result.results.at("values"));
    const auto elapsed = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (mode == "gpu" && (!diagnostics.native_dispatch_count ||
                          !diagnostics.fallback_reasons.empty()))
      throw std::runtime_error("expected native GPU dispatch without fallback");
    if (bytes != reference_bytes)
      throw std::runtime_error(
          "result differs from one-worker Whole reference");
    if (iteration)
      timings.push_back(elapsed);
    work = budget.statistics().issued.work - prior;
  }
  std::sort(timings.begin(), timings.end());
  const auto stats = budget.statistics();
  std::uint64_t continuation_polls = 0;
  for (const auto& timing : diagnostics.operation_timings)
    continuation_polls += timing.invocation_count;
  std::cout << "{\"mode\":\"" << mode << "\",\"count\":" << count
            << ",\"tile_width\":" << tile_width << ",\"workers\":" << workers
            << ",\"median_ms\":" << timings[timings.size() / 2]
            << ",\"p95_ms\":" << timings[(95 * timings.size() + 99) / 100 - 1]
            << ",\"peak_host_bytes\":" << stats.peak[ResourceKind::Host]
            << ",\"issued_work\":" << work
            << ",\"native_dispatches\":" << diagnostics.native_dispatch_count
            << ",\"native_submissions\":" << diagnostics.native_submission_count
            << ",\"native_compute_us\":" << diagnostics.native_compute_us
            << ",\"continuation_polls\":" << continuation_polls
            << ",\"peak_active_tasks\":" << diagnostics.peak_active_tasks
            << ",\"bitwise_whole_reference\":true}\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
