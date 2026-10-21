#include <algorithm>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fixtures/native_vulkan_spirv.hpp"
#include "photospider/benchmark/raw_benchmark.hpp"
#include "photospider/photospider.hpp"

// Manual kernel measurement client. This is not a performance threshold test
// and uses no bundled numerical operation or installation-consumer path.
namespace {
using namespace ps;  // NOLINT(build/namespaces)
constexpr std::uint64_t words = 1U << 20;
constexpr std::uint64_t tile_bytes = 1U << 16;
constexpr std::uint64_t payload_limit = 128U << 20;

void check(const Status& status) {
  if (!status.ok())
    throw std::runtime_error(
        std::to_string(static_cast<unsigned>(status.code)) + ": " +
        status.message);
}
template <class T>
T take(Result<T> value) {
  check(value.status());
  return value.take_value();
}
std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
struct ClockSample {
  std::uint64_t steady_before = 0, mach = 0, steady_after = 0;
};
std::array<ClockSample, 4> calibrate() {
  std::array<ClockSample, 4> samples{};
#if defined(__APPLE__)
  static const auto scale = [] {
    mach_timebase_info_data_t info{};
    if (mach_timebase_info(&info) != KERN_SUCCESS || !info.denom)
      throw std::runtime_error("mach clock calibration unavailable");
    return info;
  }();
  for (auto& sample : samples) {
    sample.steady_before = now_ns();
    const auto ticks = mach_absolute_time();
    sample.mach = ticks / scale.denom * scale.numer +
                  ticks % scale.denom * scale.numer / scale.denom;
    sample.steady_after = now_ns();
  }
#endif
  return samples;
}
void quoted(const std::string& value) {
  std::cout << '"';
  for (const auto c : value) {
    if (c == '"' || c == '\\')
      std::cout << '\\';
    if (static_cast<unsigned char>(c) >= 32)
      std::cout << c;
  }
  std::cout << '"';
}
template <class T, std::size_t N>
void array(const std::array<T, N>& values) {
  std::cout << '[';
  for (std::size_t i = 0; i < N; ++i) {
    if (i)
      std::cout << ',';
    std::cout << values[i];
  }
  std::cout << ']';
}
void emit_clocks(const std::string& scenario, unsigned iteration,
                 const char* phase, const std::array<ClockSample, 4>& samples) {
  for (const auto& sample : samples) {
    if (!sample.mach)
      continue;
    std::cout << "{\"kind\":\"clock_calibration\",\"scenario\":";
    quoted(scenario);
    std::cout << ",\"iteration\":" << iteration << ",\"phase\":";
    quoted(phase);
    std::cout << ",\"steady_before\":" << sample.steady_before
              << ",\"mach_ns\":" << sample.mach
              << ",\"steady_after\":" << sample.steady_after << "}\n";
  }
}
SchemaTemplate schema() {
  SchemaTemplate value;
  value.id = "physical.benchmark.bytes";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ElementType::UInt8, {4 * words}};
  value.tensors.push_back(std::move(tensor));
  return value;
}
std::uint32_t transform(std::uint32_t value) {
  const std::uint64_t wide = std::uint64_t{value} * 3 + 7;
  return static_cast<std::uint32_t>(wide ^ (wide >> 32));
}

OperationDefinition operation(std::string name, physical::Backend backend,
                              physical::Domain output, physical::Domain input,
                              bool reads, bool copy = false) {
  OperationDefinition op;
  op.key = std::move(name);
  op.traits.input_count = reads ? 1 : 0;
  op.traits.supports_cpu = backend == physical::Backend::Cpu;
  op.traits.supports_gpu = backend == physical::Backend::Gpu;
  OperationPortConstraint port;
  port.result_schema_id = schema().id;
  port.result_schema_version = 1;
  if (reads)
    op.traits.input_schema = {port};
  op.traits.outputs[0].key = "value";
  op.traits.outputs[0].output_schema = port;
  op.traits.outputs[0].result_schema = schema();
  op.lower = [=](const OperationLowering& query) {
    OperationProgramBuilder builder(query);
    if (query.requests[0].empty())
      return builder.finish();
    physical::Placement placement;
    placement.backend = backend;
    placement.domain = output;
    placement.profile = backend == physical::Backend::Cpu ? "cpu" : "native";
    for (const auto tile : query.requests[0].members.at(0)) {
      auto created = copy ? builder.add_copy(placement)
                          : builder.add_task(reads ? "transform" : "fill",
                                             "uint32-v1", placement);
      if (!created.ok())
        return Result<OperationProgram>(created.status());
      if (reads) {
        auto status = builder.read_input(created.value(), 0, 0, tile, input);
        if (!status.ok())
          return Result<OperationProgram>(status);
      }
      auto status = builder.write_output(created.value(), 0, 0, tile);
      if (!status.ok())
        return Result<OperationProgram>(status);
    }
    return builder.finish();
  };
  if (copy)
    return op;
  op.prepare_kernel = [=](const physical::KernelDesc& kernel,
                          const physical::Placement& placement,
                          const KernelPreparation& query, const ResourceSet&,
                          const ps_native_gpu_service_v2* service)
      -> Result<ps_prepared_kernel_v8> {
    if (kernel.contract != "uint32-v1" || query.writes.size() != 1 ||
        query.reads.size() != (reads ? 1U : 0U) || query.writes[0].bytes % 4)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "benchmark kernel contract"});
    if (backend == physical::Backend::Cpu) {
      ps_prepared_kernel_v8 raw{};
      raw.struct_size = sizeof(raw);
      raw.abi_version = 8;
      raw.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                          ps_kernel_status_v8* status) {
        const auto& output = invocation->outputs[0];
        auto* destination =
            static_cast<std::uint8_t*>(output.allocation) + output.byte_offset;
        const auto count = output.layout->shape[0] / 4;
        for (std::uint64_t i = 0; i < count; ++i) {
          if (!(i % 1024) && invocation->cancellation.is_cancelled &&
              invocation->cancellation.is_cancelled(
                  invocation->cancellation.context)) {
            *status = kernel_failure(ErrorCode::Cancelled);
            return;
          }
          std::uint32_t value =
              static_cast<std::uint32_t>(output.layout->origin[0] / 4 + i + 7);
          if (invocation->input_count) {
            const auto& input = invocation->inputs[0];
            std::memcpy(&value,
                        static_cast<const std::uint8_t*>(input.allocation) +
                            input.byte_offset + i * 4,
                        4);
            value = transform(value);
          }
          std::memcpy(destination + i * 4, &value, 4);
        }
        *status = {};
      };
      return Result<ps_prepared_kernel_v8>(raw);
    }
    if (!service)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "native benchmark requires GPU"});
    static constexpr char source[] =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct P {uint w,h,d,m,a;};"
        "kernel void fill(device uint* o [[buffer(1)]],"
        "constant P& p [[buffer(2)]],uint i [[thread_position_in_grid]]) {"
        "if(i<p.w) o[i]=i+p.a;}"
        "kernel void transform(device const uint* a [[buffer(0)]],"
        "device uint* o [[buffer(1)]],constant P& p [[buffer(2)]],"
        "uint i [[thread_position_in_grid]]) {"
        "if(i<p.w){ulong w=ulong(a[i])*p.m+p.a;o[i]=uint(w^(w>>32));}}";
    std::array<ps_kernel_view_spec_v8, 1> in{}, out{};
    const auto convert = [](const KernelView& value) {
      ps_kernel_view_spec_v8 result{};
      result.domain = static_cast<std::uint32_t>(value.domain);
      result.bytes = value.bytes;
      result.layout = kernel_layout(value.layout);
      return result;
    };
    out[0] = convert(query.writes[0]);
    if (reads)
      in[0] = convert(query.reads[0]);
    ps_kernel_preparation_v8 preparation{};
    preparation.struct_size = sizeof(preparation);
    preparation.placement.backend = PS_BACKEND_GPU_V8;
    preparation.placement.domain = static_cast<std::uint32_t>(placement.domain);
    preparation.reads = reads ? in.data() : nullptr;
    preparation.read_count = reads ? 1 : 0;
    preparation.writes = out.data();
    preparation.write_count = 1;
    const auto width = static_cast<std::uint32_t>(out[0].bytes / 4);
    const std::array<std::uint32_t, 5> parameters{
        width, 1, 1, 3,
        reads ? 7U
              : static_cast<std::uint32_t>(out[0].layout.origin[0] / 4 + 7)};
    const std::array<ps_native_binding_v2, 2> bindings{{
        {reads ? 0U : 1U, reads ? PS_NATIVE_INPUT_V2 : PS_NATIVE_OUTPUT_V2, 0,
         0, out[0].bytes},
        {1, PS_NATIVE_OUTPUT_V2, 0, 0, out[0].bytes},
    }};
    ps_native_program_v2 program{};
    program.struct_size = sizeof(program);
    program.source = {source, sizeof(source) - 1};
    program.entry =
        reads ? ps_string_v8{"transform", 9} : ps_string_v8{"fill", 4};
    if (service->backend == PS_NATIVE_VULKAN_V2) {
      program.code_format = PS_NATIVE_SPIRV_V2;
      program.source =
          reads
              ? ps_string_v8{reinterpret_cast<const char*>(kNativeVulkanSpirv),
                             sizeof(kNativeVulkanSpirv)}
              : ps_string_v8{
                    reinterpret_cast<const char*>(kNativeVulkanFillSpirv),
                    sizeof(kNativeVulkanFillSpirv)};
    }
    program.bindings = bindings.data();
    program.binding_count = reads ? 2 : 1;
    program.constants = parameters.data();
    program.constant_bytes = sizeof(parameters);
    program.constant_index = 2;
    program.grid[0] = width;
    program.grid[1] = program.grid[2] = 1;
    ps_prepared_kernel_v8 raw{};
    auto status = kernel_status(
        service->prepare(service->context, &program, &preparation, &raw));
    return status.ok() ? Result<ps_prepared_kernel_v8>(raw)
                       : Result<ps_prepared_kernel_v8>(status);
  };
  return op;
}
std::shared_ptr<OperationRegistry> registry() {
  auto result = std::make_shared<OperationRegistry>();
  using physical::Backend;
  using physical::Domain;
  check(result->register_operation(operation(
      "gpu.fill", Backend::Gpu, Domain::Shared, Domain::Shared, false)));
  check(result->register_operation(operation(
      "gpu.transform", Backend::Gpu, Domain::Shared, Domain::Shared, true)));
  check(result->register_operation(operation(
      "gpu.device", Backend::Gpu, Domain::Device, Domain::Device, true)));
  check(result->register_operation(
      operation("cpu.fill", Backend::Cpu, Domain::Host, Domain::Host, false)));
  check(result->register_operation(operation(
      "cpu.transform", Backend::Cpu, Domain::Host, Domain::Host, true)));
  check(result->register_operation(operation(
      "copy.upload", Backend::Gpu, Domain::Device, Domain::Host, true, true)));
  check(result->register_operation(operation("copy.download", Backend::Gpu,
                                             Domain::Host, Domain::Device, true,
                                             true)));
  check(result->freeze());
  return result;
}
IRProgram program(const std::string& scenario) {
  auto shared_schema = std::make_shared<const SchemaTemplate>(schema());
  const bool independent = scenario == "independent";
  const bool split = scenario == "split" || scenario == "split_tight";
  const bool copy = scenario == "copy" || scenario == "cancel";
  const unsigned calls = scenario == "tiles" || scenario == "tiles_serial" ? 1
                         : copy                                            ? 5
                                                                           : 4;
  const std::array<std::string, 5> copy_keys{"cpu.fill", "copy.upload",
                                             "gpu.device", "copy.download",
                                             "cpu.transform"};
  IRProgram ir;
  ir.entry = 1;
  if (!split)
    ir.blocks.push_back({1, {}, {}, IRReturn{}});
  for (unsigned i = 0; i < calls; ++i) {
    SemanticNode node;
    node.id = i + 1;
    node.operation = copy                ? copy_keys[i]
                     : !i || independent ? "gpu.fill"
                                         : "gpu.transform";
    if (i && !independent)
      node.inputs = {split ? 100U + i : 10U + i - 1};
    node.outputs = {{"value", shared_schema, 10U + i}};
    ir.calls.push_back(std::move(node));
    if (split) {
      IRBlock block;
      block.id = i + 1;
      if (i)
        block.parameters = {{100U + i, shared_schema}};
      block.calls = {i + 1};
      if (i + 1 == calls)
        block.terminator = IRReturn{{10U + i}};
      else
        block.terminator = IRJump{{i + 2, {10U + i}}};
      ir.blocks.push_back(std::move(block));
    } else {
      ir.blocks[0].calls.push_back(i + 1);
    }
    if (independent || i + 1 == calls)
      ir.returns.push_back({"result" + std::to_string(i), shared_schema});
  }
  if (!split) {
    IRReturn returned;
    if (independent) {
      for (unsigned i = 0; i < calls; ++i)
        returned.values.push_back(10U + i);
    } else {
      returned.values = {10U + calls - 1};
    }
    ir.blocks[0].terminator = std::move(returned);
  }
  return ir;
}
void verify(const ExecutionResult& result, unsigned transformations) {
  for (const auto& entry : result.results) {
    const auto facts = take(entry.second.descriptor());
    auto window =
        take(entry.second.acquire_tensor(facts, 0, Region::whole({4 * words})));
    for (std::uint64_t first = 0; first < 4 * words;) {
      const auto row = take(window.row_run({first}));
      if (!row.bytes || row.sample_stride_bytes != 1 || row.bytes % 4)
        throw std::runtime_error("unexpected benchmark result layout");
      for (std::uint64_t offset = 0; offset < row.bytes; offset += 4) {
        std::uint32_t actual;
        std::memcpy(&actual, row.data + offset, 4);
        auto expected = static_cast<std::uint32_t>((first + offset) / 4 + 7);
        for (unsigned step = 0; step < transformations; ++step)
          expected = transform(expected);
        if (actual != expected)
          throw std::runtime_error("benchmark bitwise result mismatch");
      }
      first += row.bytes;
    }
  }
}
void emit(const std::string& scenario, unsigned iteration,
          std::uint64_t elapsed, const RawBenchmarkSample& sample,
          const physical::Program& plan) {
  const auto& d = sample.execution;
  std::cout << "{\"kind\":\"sample\",\"scenario\":";
  quoted(scenario);
  std::cout << ",\"iteration\":" << iteration << ",\"run_ns\":" << elapsed
            << ",\"host_epoch_ns\":" << d.host_epoch_ns
            << ",\"status\":" << static_cast<unsigned>(sample.outcome)
            << ",\"activations\":" << d.activations
            << ",\"submitted\":" << d.submitted_tasks
            << ",\"completed\":" << d.completed_tasks
            << ",\"allocations\":" << d.allocation_count
            << ",\"admission_failures\":" << d.block_admission_failures
            << ",\"packet_size\":1,\"copy_bytes\":" << d.copy_bytes
            << ",\"head_wait_ns\":" << d.retirement_head_wait_ns
            << ",\"lane_wait_ns\":" << d.lane_wait_ns
            << ",\"context_wait_ns\":" << d.context_wait_ns
            << ",\"coordinator_delay_ns\":" << d.coordinator_delay_ns
            << ",\"completion_delay_ns\":" << d.completion_delay_ns
            << ",\"cpu_queue_wait_ns\":" << d.cpu_queue_wait_ns
            << ",\"coordinator_waits\":" << d.coordinator_waits
            << ",\"coordinator_wakeups\":" << d.coordinator_wakeups
            << ",\"cpu_peak_inflight\":" << d.backends[0].peak_inflight
            << ",\"gpu_peak_inflight\":" << d.backends[1].peak_inflight
            << ",\"gpu_inflight_ns\":" << d.backends[1].inflight_ns
            << ",\"metadata_peak_reserved\":" << d.metadata.peak_reserved_bytes
            << ",\"metadata_peak_allocated\":"
            << d.metadata.peak_allocated_bytes << ",\"peak_reserved\":";
  array(d.memory.peak_reserved_bytes_by_domain);
  std::cout << ",\"peak_live\":";
  array(d.memory.peak_live_bytes_by_domain);
  std::cout << ",\"dropped_tasks\":" << d.dropped_task_timings
            << ",\"dropped_calls\":" << d.dropped_call_timings;
  if (d.abort_drain)
    std::cout << ",\"drain_started_ns\":" << d.abort_drain->started_ns
              << ",\"drain_completed_ns\":" << d.abort_drain->completed_ns
              << ",\"drain_inflight\":" << d.abort_drain->inflight_at_abort
              << ",\"drain_finished\":" << d.abort_drain->finished;
  std::cout << "}\n";
  for (const auto& task : sample.task_timings) {
    std::cout << "{\"kind\":\"task\",\"scenario\":";
    quoted(scenario);
    std::cout << ",\"iteration\":" << iteration
              << ",\"activation\":" << task.activation
              << ",\"submission\":" << task.submission
              << ",\"task\":" << task.task << ",\"call\":" << task.call
              << ",\"backend\":" << static_cast<unsigned>(task.backend)
              << ",\"ready_ns\":" << task.ready_ns
              << ",\"submit_ns\":" << task.submit_ns
              << ",\"cpu_start_ns\":" << task.cpu_start_ns
              << ",\"completion_ns\":" << task.completion_ns
              << ",\"consumed_ns\":" << task.consumed_ns
              << ",\"copy_bytes\":" << task.copy_bytes
              << ",\"clock_domain\":" << task.device.clock_domain
              << ",\"clock_instance\":" << task.device.clock_instance
              << ",\"valid_bits\":" << task.device.valid_bits
              << ",\"begin_tick\":" << task.device.begin_tick
              << ",\"end_tick\":" << task.device.end_tick
              << ",\"tick_period_ns\":" << task.device.tick_period_ns
              << ",\"predecessors\":[";
    bool comma = false;
    for (auto id : plan.tasks[task.task].predecessors) {
      if (comma)
        std::cout << ',';
      std::cout << id;
      comma = true;
    }
    std::cout << "]}\n";
  }
  for (const auto& call : sample.call_timings) {
    std::cout << "{\"kind\":\"call\",\"scenario\":";
    quoted(scenario);
    std::cout << ",\"iteration\":" << iteration
              << ",\"activation\":" << call.activation
              << ",\"call\":" << call.call
              << ",\"retired_ns\":" << call.retired_ns << "}\n";
  }
}
void measure(const std::string& scenario, unsigned iterations, bool warmup) {
  auto operations = registry();
  Compiler compiler(operations);
  auto ir = take(compiler.bind_ir(program(scenario)));
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::NativeGpu;
  planning.tile_extent = tile_bytes;
  auto plan = take(compiler.plan(ir, planning));
  ExecutionContextConfig config;
  config.cpu_workers = 4;
  config.maximum_queued_tasks = 64;
  config.maximum_runs = 8;
  config.maximum_gpu_submissions = scenario == "tiles_serial" ? 1 : 4;
  config.maximum_live_bytes =
      scenario == "coalesced_tight" || scenario == "split_tight"
          ? 12U << 20
          : payload_limit;
  ExecutionContext context(operations, config);
  const auto root = take(context.resource_budget());
  const auto prepare_started = now_ns();
  auto prepared = take(context.prepare(plan));
  const auto prepare_ns = now_ns() - prepare_started;
  const auto prepared_payload = root.statistics().live[ResourceKind::Payload];
  std::cout << "{\"kind\":\"configuration\",\"scenario\":";
  quoted(scenario);
  std::cout << ",\"device\":";
  quoted(context.native_device_description());
  std::cout << ",\"shape_bytes\":" << words * 4
            << ",\"tile_extent_bytes\":" << tile_bytes
            << ",\"cpu_workers\":4,\"queue_slots\":64,\"gpu_credits\":"
            << config.maximum_gpu_submissions
            << ",\"payload_limit\":" << config.maximum_live_bytes
            << ",\"trace_capacity\":1024,\"blocks\":"
            << plan.program().blocks.size()
            << ",\"tasks\":" << plan.program().tasks.size()
            << ",\"calls\":" << plan.program().calls.size()
            << ",\"prepared_payload\":" << prepared_payload
            << ",\"prepare_ns\":" << prepare_ns << "}\n";
  for (physical::Id i = 0; i < plan.program().blocks.size(); ++i) {
    const auto& block = plan.program().blocks[i];
    std::cout << "{\"kind\":\"block\",\"scenario\":";
    quoted(scenario);
    std::cout << ",\"block\":" << i << ",\"calls\":" << block.calls.size()
              << ",\"tasks\":" << block.tasks.size() << ",\"outputs\":";
    array(block.reservation.outputs);
    std::cout << ",\"cpu_scratch\":";
    array(block.reservation.scratch[0]);
    std::cout << ",\"gpu_scratch\":";
    array(block.reservation.scratch[1]);
    std::cout << "}\n";
  }
  const unsigned total = iterations + (warmup ? 1U : 0U);
  for (unsigned iteration = 0; iteration < total; ++iteration) {
    {
      ExecutionOptions options;
      options.trace_capacity = 1024;
      const auto before_clocks = calibrate();
      const auto started = now_ns();
      auto run = take(context.submit(prepared, {}, {}, options));
      if (scenario == "cancel") {
        const auto until = now_ns() + 5000000000ULL;
        while (!run.observation().snapshot().backends[1].inflight &&
               now_ns() < until)
          std::this_thread::yield();
        if (!run.request_cancel())
          throw std::runtime_error(
              "cancel benchmark did not intercept the Run");
      }
      auto observation = run.observation();
      auto result = run.join();
      const auto elapsed = now_ns() - started;
      const auto after_clocks = calibrate();
      RawBenchmarkSample sample;
      sample.outcome = result.status().code;
      sample.execution = observation.snapshot();
      sample.task_timings.resize(sample.execution.task_timing_count);
      sample.call_timings.resize(sample.execution.call_timing_count);
      sample.task_timings.resize(observation.copy_task_timings(
          sample.task_timings.data(), sample.task_timings.size()));
      sample.call_timings.resize(observation.copy_call_timings(
          sample.call_timings.data(), sample.call_timings.size()));
      if (sample.execution.dropped_task_timings ||
          sample.execution.dropped_call_timings)
        throw std::runtime_error("benchmark trace capacity was insufficient");
      if (scenario == "coalesced_tight") {
        if (result.status().code != ErrorCode::ResourceExhausted ||
            sample.execution.submitted_tasks)
          throw std::runtime_error("coalesced capacity rejection mismatch");
      } else if (scenario == "cancel") {
        if (result.status().code != ErrorCode::Cancelled ||
            !sample.execution.abort_drain ||
            !sample.execution.abort_drain->finished ||
            sample.execution.completed_tasks !=
                sample.execution.submitted_tasks)
          throw std::runtime_error("cancel benchmark failed to drain");
      } else {
        check(result.status());
        const auto transforms = scenario == "copy" ? 2
                                : scenario == "tiles" ||
                                        scenario == "tiles_serial" ||
                                        scenario == "independent"
                                    ? 0
                                    : 3;
        verify(result.value(), transforms);
      }
      if (!warmup || iteration) {
        const auto index = iteration - (warmup ? 1U : 0U);
        emit(scenario, index, elapsed, sample, plan.program());
        emit_clocks(scenario, index, "before", before_clocks);
        emit_clocks(scenario, index, "after", after_clocks);
      }
    }
    if (root.statistics().live[ResourceKind::Payload] != prepared_payload)
      throw std::runtime_error("benchmark retained payload after Run release");
  }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "all";
    const unsigned iterations = argc > 2 ? std::stoul(argv[2]) : 5;
    if (!iterations || iterations > 100)
      throw std::runtime_error("iterations must be in [1,100]");
    const bool warmup = argc <= 3 || std::string(argv[3]) != "cold";
    const std::array<std::string, 9> scenarios{
        "tiles", "tiles_serial",    "independent", "dependent", "copy",
        "split", "coalesced_tight", "split_tight", "cancel"};
    bool matched = false;
    for (const auto& scenario : scenarios)
      if (selected == "all" || selected == scenario) {
        matched = true;
        measure(scenario, iterations, warmup);
      }
    if (!matched)
      throw std::runtime_error("unknown benchmark scenario");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
