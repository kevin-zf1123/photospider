#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "allocation_profile.hpp"  // NOLINT(build/include_subdir)
#include "copy_profile.hpp"        // NOLINT(build/include_subdir)
#include "photospider/photospider.hpp"
#include "result_workflow.hpp"  // NOLINT(build/include_subdir)

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Clock = std::chrono::steady_clock;
constexpr unsigned kKinds = 11, kSeeds = 4;
// NOLINTBEGIN(whitespace/indent_namespace)
const char* const kNames[kKinds] = {
    "perlin_whole",    "perlin_tiled",        "perlin_gpu",
    "gaussian_whole",  "gaussian_tiled",      "gaussian_gpu",
    "pixeloe_whole",   "pixeloe_tiled",       "pixeloe_gpu",
    "cpu_gpu_gpu_cpu", "independent_branches"};
// NOLINTEND
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
void check(Status status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
double micros(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start)
      .count();
}
double percentile(std::vector<double> values, double p) {
  if (values.empty())
    return 0;
  std::sort(values.begin(), values.end());
  return values[static_cast<std::size_t>((values.size() - 1) * p)];
}
std::map<std::string, ParameterValue> gaussian_parameters() {
  return {{"sigma_x", 1.},
          {"sigma_y", 1.},
          {"radius_x", std::int64_t{2}},
          {"radius_y", std::int64_t{2}},
          {"x_axis", std::int64_t{1}},
          {"y_axis", std::int64_t{0}},
          {"boundary", std::string("clamp")},
          {"cval", 0.}};
}
std::map<std::string, ParameterValue> pixel_parameters() {
  using I = std::int64_t;
  return {{"pixel_size", I(2)},
          {"thickness", I(1)},
          {"mode", std::string("contrast")},
          {"sharpen_mode", std::string("none")},
          {"sharpen_factor", .5},
          {"do_color_match", true},
          {"do_quant", false},
          {"num_colors", I(32)},
          {"quant_mode", std::string("kmeans")},
          {"dither_mode", std::string("ordered")},
          {"no_post_upscale", false},
          {"weight_mapping", std::string("current")},
          {"weight_normalize", std::string("global")},
          {"colorfix_blur", std::string("exact")},
          {"blur_impl", std::string("lowrank")},
          {"blur_rank", I(1)},
          {"local_stats", std::string("lattice")},
          {"stat_padding", std::string("zero")}};
}
std::string perlin(unsigned mode) {
  return std::string("noise.perlin2002_3d_v1_strict_") + (mode == 2 ? "gpu"
                                                          : mode == 1
                                                              ? "cpu_tiled"
                                                              : "cpu_whole");
}
std::string gaussian(unsigned mode) {
  return std::string("filter.gaussian_baked64_v1_strict_") +
         (mode == 2   ? "gpu"
          : mode == 1 ? "cpu_tiled"
                      : "cpu_whole");
}
struct Request {
  WorkflowDocument document;
  ExecutionBindings bindings;
};
Request request(unsigned kind, unsigned seed, unsigned side,
                const std::string& backend, bool reference,
                const ResourceBudget& root) {
  const bool pixel = kind >= 6 && kind <= 8;
  const bool coordinates = kind < 3 || kind >= 9;
  Request result;
  auto& document = result.document;
  ValueDescriptor descriptor{
      pixel ? ElementType::Float32 : ElementType::Float64,
      {side, side}};
  if (pixel || coordinates)
    descriptor.shape.push_back(3);
  const unsigned count = side * side * (pixel || coordinates ? 3 : 1);
  const unsigned width = pixel ? 4 : 8;
  std::vector<std::uint8_t> values(count * width);
  for (unsigned i = 0; i < count; ++i) {
    if (pixel) {
      const auto number = static_cast<float>((i * 17 + seed * 13) % 256) / 256;
      std::memcpy(values.data() + i * width, &number, width);
    } else {
      const double number =
          static_cast<double>((i * 1709 + 719 + seed * 37) % 131071) / 65536 -
          1;
      std::memcpy(values.data() + i * width, &number, width);
    }
  }
  auto schema =
      scheduler_result::schema(descriptor.element_type, descriptor.shape);
  if (pixel) {
    schema.id = "photospider.image";
    schema.tensors[0].key = "pixels";
    schema.tensors[0].batch_axes = {1, 1};
    schema.tensors[0].layout.spatial = true;
  }
  auto input = scheduler_result::source(
      root, schema, ByteView(values.data(), values.size()), side / 2);
  document.inputs = {scheduler_result::declaration(input)};
  result.bindings.inputs.push_back({"input", std::move(input)});
  if (kind < 3) {
    document.nodes = {
        {1, perlin(reference ? 0 : kind), {WorkflowInputReference{1}}, {}}};
    document.outputs = {{"result", 1, "values"}};
  } else if (kind < 6) {
    document.nodes = {{1,
                       gaussian(reference ? 0 : kind - 3),
                       {WorkflowInputReference{1}},
                       gaussian_parameters()}};
    document.outputs = {{"result", 1, "output"}};
  } else if (pixel) {
    std::string key = "pixeloe.pixelize";
    if (!reference && kind == 7)
      key += "_cpu_tiled";
    if (!reference && kind == 8)
      key += "_" + backend + "_native_fp32";
    document.nodes = {
        {1, key, {WorkflowInputReference{1}}, pixel_parameters()}};
    document.outputs = {{"result", 1, "values"}};
  } else if (kind == 9) {
    document.nodes = {{1, perlin(0), {WorkflowInputReference{1}}, {}},
                      {2,
                       gaussian(reference ? 0 : 2),
                       {WorkflowNodeOutput{1, "values"}},
                       gaussian_parameters()},
                      {3,
                       gaussian(reference ? 0 : 2),
                       {WorkflowNodeOutput{2, "output"}},
                       gaussian_parameters()},
                      {4,
                       gaussian(0),
                       {WorkflowNodeOutput{3, "output"}},
                       gaussian_parameters()}};
    document.outputs = {{"result", 4, "output"}};
  } else {
    document.nodes = {
        {1, perlin(0), {WorkflowInputReference{1}}, {}},
        {2, perlin(reference ? 0 : 1), {WorkflowInputReference{1}}, {}},
        {3, perlin(reference ? 0 : 2), {WorkflowInputReference{1}}, {}},
        {4,
         "benchmark.verify_join",
         {WorkflowNodeOutput{1, "values"}, WorkflowNodeOutput{2, "values"},
          WorkflowNodeOutput{3, "values"}},
         {}}};
    document.outputs = {{"result", 4, "value"}};
  }
  return result;
}
struct Observation {
  std::vector<std::uint8_t> bytes;
  ExecutionDiagnostics diagnostics;
  Status status;
  double construct = 0, compile = 0, execute = 0, total = 0;
  unsigned tiles = 0;
};
Observation run(ExecutionContext& context, Compiler& compiler, unsigned kind,
                unsigned seed, unsigned side, const std::string& backend,
                bool reference) {
  const auto start = Clock::now();
  auto input = request(kind, seed, side, backend, reference,
                       take(context.resource_budget()));
  GraphContext graph(std::move(input.document));
  Observation result;
  result.construct = micros(start);
  PlanningOptions planning;
  planning.execution_mode =
      reference ? ExecutionMode::CpuExact : ExecutionMode::NativeGpu;
  const unsigned tile_width = side / 2;
  planning.tile_width = planning.tile_height = tile_width;
  auto phase = Clock::now();
  auto plan = take(compiler.compile(graph, planning));
  result.compile = micros(phase);
  ExecutionOptions options;
  options.dependencies.maximum_work = options.maximum_dependency_work =
      UINT64_C(1000000000000);
  phase = Clock::now();
  const bool tiled = !reference && (kind == 1 || kind == 4);
  auto seen = take(Footprint::none({side, side}));
  std::vector<std::uint8_t> covered;
  if (tiled) {
    result.bytes.resize(side * side * 8);
    covered.resize(side * side, 0);
    options.result_publication = [&](ValueRef ref, const ResultRef& object) {
      if (ref.node_id != 1)
        return Status::success();
      const auto facts = take(object.descriptor(false));
      const auto added = take(facts.tensor_coverage(0).subtract(seen));
      for (const auto& box : added.boxes()) {
        const auto& y = box.dimensions()[0];
        const auto& x = box.dimensions()[1];
        if (y.extent > tile_width || x.extent > tile_width ||
            y.offset + y.extent > side || x.offset + x.extent > side)
          return Status{ErrorCode::Internal, "invalid mixed benchmark tile"};
        auto window = take(object.acquire_tensor(facts, 0, box));
        for (std::uint64_t row = 0; row < y.extent; ++row)
          for (std::uint64_t col = 0; col < x.extent; ++col) {
            const auto position = (y.offset + row) * side + x.offset + col;
            if (covered[position]++)
              return Status{ErrorCode::Internal,
                            "duplicate mixed benchmark tile"};
            const auto source =
                take(window.row_run({y.offset + row, x.offset + col}));
            std::memcpy(result.bytes.data() + position * 8, source.data, 8);
          }
        ++result.tiles;
      }
      seen = facts.tensor_coverage(0);
      return Status::success();
    };
  }
  auto execution =
      context.execute(plan.plan, std::move(input.bindings), {}, options);
  if (!execution.ok()) {
    result.status = execution.status();
  } else {
    auto output = execution.take_value();
    result.diagnostics = std::move(output.diagnostics);
    const auto& object = output.results.at("result");
    const auto expected_shape =
        kind >= 6 && kind <= 8 ? std::vector<std::uint64_t>{1, 1, side, side, 3}
                               : std::vector<std::uint64_t>{side, side};
    if (object.schema().tensors[0].sample_shape() != expected_shape)
      throw std::runtime_error("unexpected benchmark Result shape");
    if (tiled) {
      if (result.tiles < 2 ||
          std::find(covered.begin(), covered.end(), 0) != covered.end() ||
          result.bytes != scheduler_result::bytes(object))
        throw std::runtime_error("incomplete mixed benchmark tile coverage");
    } else {
      result.bytes = scheduler_result::bytes(object);
    }
  }
  result.execute = micros(phase);
  result.total = micros(start);
  return result;
}
void verify(const Observation& result,
            const std::vector<std::uint8_t>& expected, unsigned kind,
            bool allow_rejection) {
  if (!result.status.ok()) {
    if (allow_rejection && result.status.code == ErrorCode::ResourceExhausted)
      return;
    throw std::runtime_error(std::string(kNames[kind]) + ": " +
                             result.status.message);
  }
  if (!result.diagnostics.fallback_reasons.empty())
    throw std::runtime_error("unexpected CPU fallback");
  if (result.bytes.size() != expected.size())
    throw std::runtime_error("result byte size mismatch");
  if (kind == 8) {
    for (std::size_t i = 0; i < expected.size(); i += 4) {
      float a, b;
      std::memcpy(&a, result.bytes.data() + i, 4);
      std::memcpy(&b, expected.data() + i, 4);
      if (!std::isfinite(a) || !std::isfinite(b) || std::abs(a - b) > 1e-5f)
        throw std::runtime_error("native PixelOE differs from CPU reference");
    }
  } else if (result.bytes != expected) {
    throw std::runtime_error("strict/CPU result bits differ");
  }
  const unsigned native_nodes =
      kind == 9 ? 2 : kind == 2 || kind == 5 || kind == 8 || kind == 10;
  if (result.diagnostics.native_dispatch_count < native_nodes)
    throw std::runtime_error("missing native dispatch");
  if (kind == 7 && (!result.diagnostics.cpu_stage_count ||
                    !result.diagnostics.cpu_tile_callback_count))
    throw std::runtime_error("missing PixelOE tile computation");
  if (kind >= 9) {
    for (unsigned node = 1; node <= 4; ++node) {
      const bool gpu = kind == 9 ? node == 2 || node == 3 : node == 3;
      if (result.diagnostics.selected_backends.at({node, 0}) !=
          (gpu ? Backend::Gpu : Backend::Cpu))
        throw std::runtime_error("mixed DAG selected wrong backend");
    }
  }
}
void lane(const char* name, const CallbackQueueStatistics& before,
          const CallbackQueueStatistics& after) {
  if (after.saturated)
    throw std::runtime_error("scheduler counters saturated");
  std::cout
      << ",\"" << name << "\":{\"accepted\":"
      << after.accepted_callbacks - before.accepted_callbacks
      << ",\"started\":" << after.started_callbacks - before.started_callbacks
      << ",\"submission_ns\":" << after.submission_ns - before.submission_ns
      << ",\"queue_wait_ns\":" << after.queue_wait_ns - before.queue_wait_ns
      << ",\"queue_wait_max_ns_including_warmup\":"
      << after.maximum_queue_wait_ns
      << ",\"queued_peak_including_warmup\":" << after.maximum_queued_callbacks
      << '}';
}
}  // namespace
int main(int argc, char** argv) try {
  copy_profile::initialize();
  if (argc < 3)
    throw std::runtime_error(
        "usage: plugin metal|vulkan [runs=110] [workers=4] [clients=4] "
        "[side=8] [queue=1024] [timing=1] [allow_rejection=0]");
  const std::string backend = argv[2];
  const unsigned runs = argc > 3 ? std::stoul(argv[3]) : 110;
  const unsigned workers = argc > 4 ? std::stoul(argv[4]) : 4;
  const unsigned clients = argc > 5 ? std::stoul(argv[5]) : 4;
  const unsigned side = argc > 6 ? std::stoul(argv[6]) : 8;
  const unsigned queue = argc > 7 ? std::stoul(argv[7]) : 1024;
  const unsigned timing = argc > 8 ? std::stoul(argv[8]) : 1;
  const unsigned rejection = argc > 9 ? std::stoul(argv[9]) : 0;
  if ((backend != "metal" && backend != "vulkan") || runs < kKinds ||
      runs > 100000 || !workers || workers > 64 || !clients || clients > 32 ||
      clients > runs || side < 4 || side > 128 || (side & (side - 1)) ||
      !queue || queue > 65536 || timing > 1 || rejection > 1)
    throw std::runtime_error("mixed benchmark arguments outside bounds");
  auto registry = make_default_operation_registry(false);
  check(registry->load_plugin(argv[1]));
  check(registry->register_operation(scheduler_result::operation(false)));
  check(registry->freeze());
  Compiler compiler(registry);
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = UINT64_C(512) << 20;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Host] = UINT64_C(1024) << 20;
  config.managed_resources->capacity[ResourceKind::Device] = UINT64_C(1024)
                                                             << 20;
  config.managed_resources->capacity[ResourceKind::Shared] = UINT64_C(1024)
                                                             << 20;
  config.managed_resources->capacity[ResourceKind::Metadata] = UINT64_C(64)
                                                               << 20;
  std::array<std::array<std::vector<std::uint8_t>, kSeeds>, kKinds> references;
  {
    ExecutionContext reference(registry, config);
    for (unsigned kind = 0; kind < kKinds; ++kind)
      for (unsigned seed = 0; seed < kSeeds; ++seed) {
        auto result = run(reference, compiler, kind, seed, side, backend, true);
        check(result.status);
        references[kind][seed] = std::move(result.bytes);
      }
  }
  config.cpu_workers = workers;
  config.gpu_enabled = true;
  config.maximum_queued_tasks = queue;
  config.collect_scheduler_timing = timing != 0;
  ExecutionContext context(registry, config);
  if (!context.gpu_enabled())
    throw std::runtime_error("native GPU unavailable");
  struct Sample {
    unsigned kind;
    bool success;
    double construct, compile, execute, total;
    std::uint64_t dispatches, submissions, device_us, transfers, transfer_bytes;
    std::uint64_t stages, tile_callbacks, host_accesses, copied, tiles;
  };
  struct Client {
    std::vector<Sample> samples;
    std::exception_ptr error;
  };
  std::vector<Client> results(clients);
  for (unsigned i = 0; i < clients; ++i)
    results[i].samples.reserve(runs / clients + 1);
  std::mutex mutex;
  std::condition_variable changed;
  bool released = false;
  unsigned ready = 0;
  std::vector<std::thread> threads;
  threads.reserve(clients);
  auto invoke = [&](unsigned id) {
    const unsigned kind = id % kKinds, seed = (id / kKinds) % kSeeds;
    auto result = run(context, compiler, kind, seed, side, backend, false);
    verify(result, references[kind][seed], kind, rejection != 0);
    const auto& d = result.diagnostics;
    return Sample{kind,
                  result.status.ok(),
                  result.construct,
                  result.compile,
                  result.execute,
                  result.total,
                  d.native_dispatch_count,
                  d.native_submission_count,
                  d.native_compute_us,
                  d.transfer_count,
                  d.transfer_bytes,
                  d.cpu_stage_count,
                  d.cpu_tile_callback_count,
                  d.host_access_count,
                  d.result_copy_bytes,
                  result.tiles};
  };
  try {
    for (unsigned client = 0; client < clients; ++client) {
      threads.emplace_back([&, client] {
        auto& output = results[client];
        try {
          for (unsigned kind = 0; kind < kKinds; ++kind)
            invoke(kind);
        } catch (...) {
          output.error = std::current_exception();
        }
        {
          std::unique_lock<std::mutex> lock(mutex);
          ++ready;
          changed.notify_all();
          changed.wait(lock, [&] { return released; });
        }
        if (output.error)
          return;
        try {
          for (unsigned id = client; id < runs; id += clients)
            output.samples.push_back(invoke(id));
        } catch (...) {
          output.error = std::current_exception();
        }
      });
    }
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      released = true;
    }
    changed.notify_all();
    for (auto& thread : threads)
      thread.join();
    throw;
  }
  SchedulerStatistics before;
  ResourceWork prior_work;
  Clock::time_point start;
  {
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [&] { return ready == clients; });
    before = context.scheduler_statistics();
    prior_work = take(context.resource_budget()).statistics().issued;
    allocation_profile::begin();
    copy_profile::begin();
    start = Clock::now();
    released = true;
  }
  changed.notify_all();
  for (auto& thread : threads)
    thread.join();
  const auto allocations = allocation_profile::end();
  const auto copies = copy_profile::end();
  const auto wall = micros(start);
  const auto after = context.scheduler_statistics();
  const auto resources = take(context.resource_budget()).statistics();
  std::array<std::vector<double>, kKinds> construct, compile, execute, latency;
  std::array<unsigned, kKinds> successful{}, exhausted{};
  std::uint64_t dispatches = 0, submissions = 0, device_us = 0, transfers = 0,
                transfer_bytes = 0;
  std::uint64_t stages = 0, tile_callbacks = 0, output_tiles = 0,
                host_accesses = 0, copied = 0;
  unsigned total = 0, success = 0;
  for (const auto& client : results) {
    if (client.error)
      std::rethrow_exception(client.error);
    for (const auto& sample : client.samples) {
      ++total;
      const auto kind = sample.kind;
      const auto& value = sample;
      construct[kind].push_back(value.construct);
      compile[kind].push_back(value.compile);
      execute[kind].push_back(value.execute);
      latency[kind].push_back(value.total);
      if (!value.success) {
        ++exhausted[kind];
        continue;
      }
      ++success;
      ++successful[kind];
      dispatches += value.dispatches;
      submissions += value.submissions;
      device_us += value.device_us;
      transfers += value.transfers;
      transfer_bytes += value.transfer_bytes;
      stages += value.stages;
      tile_callbacks += value.tile_callbacks;
      host_accesses += value.host_accesses;
      copied += value.copied;
      output_tiles += value.tiles;
    }
  }
  if (total != runs)
    throw std::runtime_error("incomplete measured requests");
  std::cout
      << "{\"io_contract\":\"Result\",\"backend\":\"" << backend
      << "\",\"runs\":" << runs << ",\"workers\":" << workers
      << ",\"clients\":" << clients << ",\"side\":" << side
      << ",\"queue_limit\":" << queue
      << ",\"scheduler_timing\":" << (timing ? "true" : "false")
      << ",\"warmup_per_client\":11"
      << ",\"latency_scope\":\"all_attempts_before_verify\","
         "\"execute_scope\":\"execute_plus_output_copy_and_stream_checks\","
         "\"wall_scope\":\"measured_attempts_verify_cleanup_join\","
         "\"diagnostic_counts_scope\":\"successful_requests_only\""
      << ",\"cache\":false,\"new_graphs_and_bindings\":true,"
         "\"reference\":\"CPU_Whole_1worker_bits_PixelOE_GPU_abs_1e-5\""
      << ",\"wall_us\":" << wall << ",\"attempt_qps\":" << runs * 1e6 / wall
      << ",\"success_qps\":" << success * 1e6 / wall
      << ",\"successful\":" << success
      << ",\"successful_request_native_dispatches\":" << dispatches
      << ",\"successful_request_native_submissions\":" << submissions
      << ",\"successful_request_device_us\":" << device_us
      << ",\"transfer_count\":" << transfers
      << ",\"transfer_bytes\":" << transfer_bytes
      << ",\"host_access_count\":" << host_accesses
      << ",\"result_copy_bytes\":" << copied << ",\"cpu_stages\":" << stages
      << ",\"cpu_tile_callbacks\":" << tile_callbacks
      << ",\"stream_output_tiles\":" << output_tiles
      << ",\"context_issued_work\":" << resources.issued.work - prior_work.work
      << ",\"context_issued_stages\":"
      << resources.issued.stages - prior_work.stages
      << ",\"peak_host_including_warmup\":"
      << resources.peak[ResourceKind::Host]
      << ",\"peak_metadata_including_warmup\":"
      << resources.peak[ResourceKind::Metadata]
      << ",\"peak_device_including_warmup\":"
      << resources.peak[ResourceKind::Device];
  lane("cpu_queue", before.cpu, after.cpu);
  lane("gpu_queue", before.gpu, after.gpu);
  allocation_profile::write(std::cout, allocations, runs);
  copy_profile::write(std::cout, copies);
  std::cout << ",\"kinds\":[";
  for (unsigned kind = 0; kind < kKinds; ++kind) {
    if (kind)
      std::cout << ',';
    std::cout << "{\"kind\":\"" << kNames[kind]
              << "\",\"success\":" << successful[kind]
              << ",\"resource_exhausted\":" << exhausted[kind]
              << ",\"construct_p50_us\":" << percentile(construct[kind], .5)
              << ",\"compile_p50_us\":" << percentile(compile[kind], .5)
              << ",\"execute_p50_us\":" << percentile(execute[kind], .5)
              << ",\"p50_us\":" << percentile(latency[kind], .5)
              << ",\"p95_us\":" << percentile(latency[kind], .95)
              << ",\"p99_us\":" << percentile(latency[kind], .99) << '}';
  }
  std::cout << "]}\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
