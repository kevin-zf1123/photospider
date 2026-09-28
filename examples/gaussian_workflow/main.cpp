#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
WorkflowDocument graph(unsigned side, const std::string& mode) {
  WorkflowDocument result;
  result.inputs = {{1,
                    "input",
                    {ElementType::Float64, {side, side}},
                    Region::whole({side, side}),
                    {0, {static_cast<std::int64_t>(side) * 8, 8}},
                    {}}};
  result.nodes = {{1,
                   mode == "gpu" ? "filter.gaussian_baked64_v1_strict_gpu"
                   : mode == "tiled"
                       ? "filter.gaussian_baked64_v1_strict_cpu_tiled"
                       : "filter.gaussian_baked64_v1_strict_cpu_whole",
                   {WorkflowInputReference{1}},
                   {{"sigma_x", 1.},
                    {"sigma_y", 1.},
                    {"radius_x", std::int64_t{2}},
                    {"radius_y", std::int64_t{2}},
                    {"x_axis", std::int64_t{1}},
                    {"y_axis", std::int64_t{0}},
                    {"boundary", std::string("clamp")},
                    {"cval", 0.}}}};
  result.outputs = {{"output", 1, "output"}};
  return result;
}
}  // namespace
int main(int argc, char** argv) try {
  const unsigned side = argc > 1 ? std::stoul(argv[1]) : 32;
  const unsigned workers = argc > 2 ? std::stoul(argv[2]) : 4;
  const unsigned repeat = argc > 3 ? std::stoul(argv[3]) : 5;
  const std::string mode = argc > 4 ? argv[4] : "whole";
  const unsigned tile_width = argc > 5 ? std::stoul(argv[5]) : 8;
  if ((mode != "whole" && mode != "tiled" && mode != "gpu") || !tile_width ||
      tile_width > 4096 || (tile_width & (tile_width - 1)) || !side ||
      side > 4096 || !workers || workers > 64 || !repeat || repeat > 10000)
    throw std::runtime_error(
        "usage: side[1..4096] workers[1..64] repeats[1..10000] "
        "whole|tiled|gpu tile_width[power-of-two,1..4096]");
  const auto count = side * side;
  auto registry = make_default_operation_registry();
  auto input_writer = take(
      MutableValue::allocate({ElementType::Float64, {side, side}},
                             Region::whole({side, side}), BufferAllocator{}));
  for (unsigned i = 0; i < count; ++i) {
    const double value =
        static_cast<double>((i * 1709U + 719U) % 131071U) / 65536. - 1.;
    std::memcpy(input_writer.data() + i * 8, &value, 8);
  }
  const auto input = take(std::move(input_writer).publish());
  ExecutionOptions options;
  options.dependencies.maximum_work = UINT64_C(1000000000000);
  options.maximum_dependency_work = UINT64_C(1000000000000);
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.gpu_enabled = false;
  config.managed_resources = ResourceLimits{};
  ExecutionContext reference_context(registry, config);
  GraphContext reference_graph(graph(side, "whole"));
  auto reference_plan = take(Compiler(registry).compile(reference_graph));
  const auto reference = take(reference_context.execute(
      reference_plan.plan, {{{"input", input}}}, {}, options));
  const auto reference_bytes = reference.values.at("output").copy_bytes();
  config.cpu_workers = workers;
  config.gpu_enabled = mode == "gpu";
  ExecutionContext context(registry, config);
  GraphContext source(graph(side, mode));
  PlanningOptions planning;
  planning.execution_mode =
      mode == "gpu" ? ExecutionMode::NativeGpu : ExecutionMode::CpuExact;
  planning.tile_width = tile_width;
  planning.tile_height = tile_width;
  auto compiled = take(Compiler(registry).compile(source, planning));
  std::vector<std::uint8_t> bytes(count * 8);
  std::vector<double> timings;
  ExecutionDiagnostics diagnostics;
  const auto budget = take(context.resource_budget());
  std::uint64_t work = 0;
  for (unsigned iteration = 0; iteration <= repeat; ++iteration) {
    const auto prior = budget.statistics().issued.work;
    const auto start = std::chrono::steady_clock::now();
    if (mode == "tiled") {
      std::uint64_t covered = 0;
      diagnostics = take(context.execute_stream(
          compiled.plan, {{{"input", input}}},
          [&](const std::string&, ValueView tile) {
            const auto& y = tile.region().dimensions()[0];
            const auto& x = tile.region().dimensions()[1];
            if (y.extent > tile_width || x.extent > tile_width ||
                y.offset + y.extent > side || x.offset + x.extent > side)
              return Status{ErrorCode::Internal, "invalid tile coverage"};
            for (std::uint64_t row = 0; row < y.extent; ++row)
              std::memcpy(
                  bytes.data() + ((y.offset + row) * side + x.offset) * 8,
                  tile.bytes().data() + row * x.extent * 8, x.extent * 8);
            covered += y.extent * x.extent;
            return Status::success();
          },
          {}, options));
      if (covered != count)
        throw std::runtime_error("incomplete tile coverage");
    } else {
      auto result = take(
          context.execute(compiled.plan, {{{"input", input}}}, {}, options));
      diagnostics = std::move(result.diagnostics);
      std::memcpy(bytes.data(), result.values.at("output").bytes().data(),
                  bytes.size());
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (mode == "gpu" && (!diagnostics.native_dispatch_count ||
                          !diagnostics.fallback_reasons.empty()))
      throw std::runtime_error("expected native GPU without fallback");
    if (bytes != reference_bytes)
      throw std::runtime_error(
          "result differs from one-worker Whole reference");
    if (iteration)
      timings.push_back(elapsed);
    work = budget.statistics().issued.work - prior;
  }
  std::sort(timings.begin(), timings.end());
  const auto stats = budget.statistics();
  std::uint64_t compute_calls = 0;
  for (const auto& timing : diagnostics.operation_timings)
    compute_calls += timing.computed_elements != 0;
  std::cout
      << "{\"mode\":\"" << mode << "\",\"side\":" << side
      << ",\"tile_width\":" << tile_width << ",\"workers\":" << workers
      << ",\"median_ms\":" << timings[timings.size() / 2]
      << ",\"p95_ms\":" << timings[(95 * timings.size() + 99) / 100 - 1]
      << ",\"peak_host_bytes\":" << stats.peak[ResourceKind::Host]
      << ",\"issued_work\":" << work << ",\"compute_calls\":" << compute_calls
      << ",\"peak_active_tasks\":" << diagnostics.peak_active_tasks
      << ",\"native_dispatch_count\":" << diagnostics.native_dispatch_count
      << ",\"native_submission_count\":" << diagnostics.native_submission_count
      << ",\"native_compute_us\":" << diagnostics.native_compute_us
      << ",\"native_constant_bytes\":" << diagnostics.native_constant_bytes
      << ",\"bitwise_whole_reference\":true}\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
