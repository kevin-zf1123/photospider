#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "execution/native_gpu.hpp"
#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/status_check.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
int preparation(const std::shared_ptr<gpu_internal::Device>& device,
                bool padded = false) {
  auto service = device->preparation_service();
  ps_kernel_view_spec_v8 output{};
  output.domain = PS_KERNEL_DOMAIN_SHARED_V8;
  output.bytes = 16;
  output.layout.rank = 1;
  output.layout.element_type = 1;
  output.layout.shape[0] = 16;
  output.layout.strides[0] = 1;
  ps_kernel_preparation_v8 query{};
  query.struct_size = sizeof(query);
  query.placement.backend = PS_BACKEND_GPU_V8;
  query.placement.domain = PS_KERNEL_DOMAIN_SHARED_V8;
  query.writes = &output;
  query.write_count = 1;
  constexpr char source[] =
      "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void kept(device uchar* o [[buffer(0)]],"
      "uint i [[thread_position_in_grid]]) {o[i]=uchar(i+9);}";
  const ps_native_binding_v2 binding{0, PS_NATIVE_OUTPUT_V2, 0, 0, 16};
  ps_native_program_v2 program{};
  program.struct_size = sizeof(program);
  program.source = {source, sizeof(source) - 1};
  program.entry = {"kept", 4};
  program.bindings = &binding;
  program.binding_count = 1;
  program.grid[0] = 16;
  program.grid[1] = program.grid[2] = 1;
  ps_prepared_kernel_v8 raw{};
  raw.struct_size = 123;
  auto status = service.prepare(service.context, nullptr, &query, &raw);
  PS_CHECK(status.code == PS_KERNEL_INVALID_ARGUMENT_V8 && !raw.struct_size);
  for (unsigned mode = 0; !padded && mode < 7; ++mode) {
    auto bad = program;
    auto view = output;
    auto bad_query = query;
    bad_query.writes = &view;
    auto bad_binding = binding;
    bad.bindings = &bad_binding;
    if (mode == 0)
      bad_binding.bytes = 17;
    if (mode == 1)
      bad.constant_bytes = 4097;
    if (mode == 2)
      bad_binding.source = PS_NATIVE_INPUT_V2;
    if (mode == 3)
      bad.grid[0] = 0;
    if (mode == 4)
      bad.group[0] = 1;
    if (mode == 5)
      view.domain = PS_KERNEL_DOMAIN_HOST_V8;
    if (mode == 6)
      bad.source = {"\xff", 1};
    raw.struct_size = 123;
    status = service.prepare(service.context, &bad, &bad_query, &raw);
    PS_CHECK(status.code == PS_KERNEL_INVALID_ARGUMENT_V8 && !raw.struct_size &&
             !raw.context && !raw.release);
  }
  constexpr char padded_source[] =
      "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void padded(device uchar* o [[buffer(0)]],"
      "device uint* s [[buffer(1)]],constant uchar* c [[buffer(2)]],"
      "uint3 p [[thread_position_in_grid]]) {"
      "if(p.x>=3 || p.y>=2 || p.z>=2) return;"
      "uint i=p.x+3*(p.y+2*p.z);s[i]=i+c[4095];o[i]=uchar(s[i]);}";
  const std::array<ps_native_binding_v2, 2> padded_bindings{{
      {0, PS_NATIVE_OUTPUT_V2, 0, 0, 12},
      {1, PS_NATIVE_SCRATCH_V2, PS_KERNEL_DOMAIN_SHARED_V8, 0, 48},
  }};
  std::array<std::uint8_t, 4096> constants{};
  constants.back() = 9;
  if (padded) {
    output.bytes = output.layout.shape[0] = 12;
    query.scratch[PS_KERNEL_DOMAIN_SHARED_V8] = 48;
    program.source = {padded_source, sizeof(padded_source) - 1};
    program.entry = {"padded", 6};
    program.bindings = padded_bindings.data();
    program.binding_count = 2;
    program.constants = constants.data();
    program.constant_bytes = constants.size();
    program.constant_index = 2;
    program.grid[0] = 3;
    program.grid[1] = program.grid[2] = 2;
    program.group[0] = program.group[1] = program.group[2] = 2;
  }
  PS_REQUIRE_OK(
      kernel_status(service.prepare(service.context, &program, &query, &raw)));
  constants.fill(0);
  struct Guard {
    ps_prepared_kernel_v8 kernel;
    ~Guard() { kernel.release(kernel.context); }
  } guard{raw};
  device->clear_pipeline_cache();
  ResourceBudget budget;
  auto allocated = device->allocator(budget.allocator()).allocate(output.bytes);
  PS_REQUIRE_OK(allocated);
  auto storage = allocated.take_value();
  ps_kernel_mutable_view_v8 view{};
  view.allocation = storage.data();
  view.bytes = output.bytes;
  view.domain = PS_KERNEL_DOMAIN_SHARED_V8;
  view.layout = &output.layout;
  ps_kernel_invocation_v8 invocation{};
  invocation.struct_size = sizeof(invocation);
  invocation.abi_version = 8;
  invocation.prepared_context = raw.context;
  invocation.outputs = &view;
  invocation.output_count = 1;
  MutableBuffer scratch;
  ps_kernel_scratch_v8 scratch_view{};
  if (padded) {
    auto allocated_scratch = device->allocator(budget.allocator()).allocate(48);
    PS_REQUIRE_OK(allocated_scratch);
    scratch = allocated_scratch.take_value();
    scratch_view.allocation = scratch.data();
    scratch_view.bytes = 48;
    scratch_view.domain = PS_KERNEL_DOMAIN_SHARED_V8;
    invocation.scratch = &scratch_view;
    invocation.scratch_count = 1;
  }
  struct Completion {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false, key_ok = false;
    ps_kernel_status_v8 status{};
  } completed;
  ps_kernel_completion_v8 completion{
      &completed,
      {1, 2, 3},
      [](void* context, ps_kernel_completion_key_v8 key,
         const ps_kernel_status_v8* status) {
        auto& value = *static_cast<Completion*>(context);
        std::lock_guard<std::mutex> lock(value.mutex);
        value.status = *status;
        value.key_ok =
            key.run == 1 && key.activation == 2 && key.submission == 3;
        value.done = true;
        value.changed.notify_all();
      },
      nullptr};
  const auto accepted = raw.submit_gpu(&invocation, &completion);
  PS_CHECK(accepted.accepted == 1 && !accepted.status.code);
  {
    std::unique_lock<std::mutex> lock(completed.mutex);
    completed.changed.wait(lock, [&] { return completed.done; });
  }
  PS_CHECK(completed.key_ok);
  PS_REQUIRE_OK(kernel_status(completed.status));
  for (unsigned i = 0; i < output.bytes; ++i)
    PS_CHECK(storage.data()[i] == i + 9);
  return 0;
}
int check(const ResultRef& result, unsigned bias, bool external = false) {
  auto descriptor = result.descriptor();
  PS_REQUIRE_OK(descriptor);
  for (unsigned i = 0; i < 129; ++i) {
    std::uint8_t byte = 0;
    PS_REQUIRE_OK(result.read_tensor(descriptor.value(), 0, {i}, &byte, 1));
    PS_CHECK(byte == static_cast<std::uint8_t>((external ? 3 * i : i) + bias));
  }
  return 0;
}
SchemaTemplate schema() {
  auto result = multi_result::schema(ElementType::UInt8, {129});
  result.id = "native.tiles";
  result.tensors[0].key = "samples";
  return result;
}
ExecutionBindings binding(const ResourceBudget& root, unsigned bias) {
  auto builder = multi_result::take(
      ResultBuilder::start(root, schema(), "native.input", {}, 32));
  for (unsigned begin = 0; begin < 129; begin += 32) {
    const auto count = std::min(32U, 129 - begin);
    std::vector<std::uint8_t> bytes(count);
    for (unsigned i = 0; i < count; ++i)
      bytes[i] = static_cast<std::uint8_t>(3 * (begin + i) + bias);
    multi_result::check(builder.publish_tensor(0, Region({{begin, count}}),
                                               {bytes.data(), bytes.size()}));
  }
  return {{{"source", multi_result::take(builder.seal())}}};
}
}  // namespace

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto device = gpu_internal::Device::create();
  if (!device || device->backend() != PS_NATIVE_METAL_V2) {
    std::cout << "Fixed Metal backend unavailable\n";
    return 77;
  }
  std::cout << device->identity() << '\n';
  PS_CHECK(preparation(device) == 0);
  PS_CHECK(preparation(device, true) == 0);
  auto registry = std::make_shared<OperationRegistry>();
  PS_REQUIRE_OK(registry->load_plugin(PS_NATIVE_KERNEL_FIXTURE));
  PS_REQUIRE_OK(registry->freeze());
  WorkflowDocument document;
  document.nodes = {
      {1, "native.fill", {}, {}},
      {2, "native.add", {WorkflowNodeOutput{1, "value"}}, {}},
      {3, "native.download", {WorkflowNodeOutput{2, "value"}}, {}},
      {4, "native.fill", {}, {}}};
  document.outputs = {{"result", 3, "value"}, {"other", 4, "value"}};
  GraphContext graph(document);
  PlanningOptions options;
  options.execution_mode = ExecutionMode::NativeGpu;
  options.tile_extent = 32;
  auto compiled = Compiler(registry).compile(graph, options);
  PS_REQUIRE_OK(compiled);
  auto text = CompilerArtifact::export_plan(compiled.value().plan.value());
  PS_REQUIRE_OK(text);
  auto imported = CompilerArtifact::import_plan(registry, text.value());
  PS_REQUIRE_OK(imported);
  ExecutionContextConfig config;
  config.cpu_workers = 2;
  config.maximum_gpu_submissions = 2;
  ExecutionContext context(registry, config);
  auto prepared = context.prepare(imported.value());
  PS_REQUIRE_OK(prepared);
  ExecutionOptions trace;
  trace.trace_capacity = 20;
  auto first = context.submit(prepared.value(), {}, {}, trace);
  auto second = context.submit(prepared.value(), {}, {}, trace);
  PS_REQUIRE_OK(first);
  PS_REQUIRE_OK(second);
  for (const auto& run : {first.value(), second.value()}) {
    auto result = run.join();
    PS_REQUIRE_OK(result);
    PS_CHECK(check(result.value().results.at("result"), 43) == 0);
    PS_CHECK(check(result.value().results.at("other"), 41) == 0);
    PS_CHECK(result.value().diagnostics.completed_tasks == 20);
    PS_CHECK(result.value().diagnostics.retired_calls == 4);
    std::array<ExecutionTaskTiming, 20> timings;
    PS_CHECK(run.observation().copy_task_timings(timings.data(), 20) == 20);
    unsigned gpu_timings = 0, cpu_timings = 0;
    for (const auto& timing : timings) {
      if (timing.backend == physical::Backend::Gpu) {
        ++gpu_timings;
        PS_CHECK(timing.device.clock_domain ==
                     PS_KERNEL_CLOCK_METAL_MACH_NS_V8 &&
                 timing.device.clock_instance == 1 &&
                 timing.device.tick_period_ns == 1.0 &&
                 timing.device.end_tick >= timing.device.begin_tick);
      } else {
        ++cpu_timings;
        PS_CHECK(timing.device.clock_domain == PS_KERNEL_CLOCK_UNAVAILABLE_V8 &&
                 timing.copy_bytes > 0 &&
                 timing.submit_ns <= timing.cpu_start_ns &&
                 timing.cpu_start_ns <= timing.completion_ns);
      }
      PS_CHECK(timing.submit_ns <= timing.completion_ns &&
               timing.completion_ns <= timing.consumed_ns);
    }
    PS_CHECK(gpu_timings == 15 && cpu_timings == 5);
  }
  ResourceBudget selections;
  auto last = TileSet::from_indices(selections, {4});
  PS_REQUIRE_OK(last);
  auto selected = imported.value().select_outputs(
      {{"result", TileRequest{{{0, last.value()}}}}});
  PS_REQUIRE_OK(selected);
  auto partial = multi_result::run(context, selected.value());
  PS_REQUIRE_OK(partial);
  PS_CHECK(partial.value().diagnostics.completed_tasks == 3);
  const auto value = partial.value().results.at("result");
  auto facts = value.descriptor();
  PS_REQUIRE_OK(facts);
  std::uint8_t byte = 0;
  PS_REQUIRE_OK(value.read_tensor(facts.value(), 0, {128}, &byte, 1));
  PS_CHECK(byte == 171);
  PS_CHECK(!value.read_tensor(facts.value(), 0, {127}, &byte, 1).ok());
  auto empty = imported.value().select_outputs({{"result", TileRequest{}}});
  PS_REQUIRE_OK(empty);
  auto empty_result = multi_result::run(context, empty.value());
  PS_REQUIRE_OK(empty_result);
  PS_CHECK(empty_result.value().diagnostics.completed_tasks == 0);
  auto empty_descriptor =
      empty_result.value().results.at("result").descriptor();
  PS_REQUIRE_OK(empty_descriptor);
  PS_CHECK(empty_descriptor.value().tensor_coverage(0).empty());
  CancellationSource cancellation;
  cancellation.cancel();
  auto cancelled = context.submit(prepared.value(), {}, cancellation.token());
  PS_REQUIRE_OK(cancelled);
  PS_CHECK(cancelled.value().join().status().code == ErrorCode::Cancelled);

  // Every domain crossing is a physical COPY, including CPU -> GPU -> CPU.
  WorkflowDocument external;
  external.inputs = {multi_result::declaration(1, "source", schema())};
  external.nodes = {
      {1, "native.upload", {WorkflowInputReference{1}}, {}},
      {2, "native.add", {WorkflowNodeOutput{1, "value"}}, {}},
      {3, "native.download", {WorkflowNodeOutput{2, "value"}}, {}}};
  external.outputs = {{"result", 3, "value"}};
  GraphContext external_graph(external);
  auto chain = Compiler(registry).compile(external_graph, options);
  PS_REQUIRE_OK(chain);
  unsigned copies = 0;
  for (const auto& task : chain.value().plan->program().tasks)
    copies += task.kind == physical::TaskKind::Copy;
  PS_CHECK(copies == 10);
  auto chain_prepared = context.prepare(chain.value().plan.value());
  PS_REQUIRE_OK(chain_prepared);
  auto root = context.resource_budget();
  PS_REQUIRE_OK(root);
  auto seven = context.submit(chain_prepared.value(), binding(root.value(), 7));
  auto nineteen =
      context.submit(chain_prepared.value(), binding(root.value(), 19));
  PS_REQUIRE_OK(seven);
  PS_REQUIRE_OK(nineteen);
  auto seven_result = seven.value().join();
  auto nineteen_result = nineteen.value().join();
  PS_REQUIRE_OK(seven_result);
  PS_REQUIRE_OK(nineteen_result);
  PS_CHECK(check(nineteen_result.value().results.at("result"), 21, true) == 0);
  PS_CHECK(check(seven_result.value().results.at("result"), 9, true) == 0);
  PS_CHECK(seven_result.value().diagnostics.completed_tasks == 15 &&
           nineteen_result.value().diagnostics.completed_tasks == 15);
  return 0;
}
