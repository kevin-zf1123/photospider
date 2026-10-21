#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "execution/native_gpu.hpp"
#include "photospider/execution/execution.hpp"
#include "support/fixed_native_fixture.hpp"
#include "support/fixed_operation_fixture.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using physical::Domain;
physical::Layout dense(std::uint64_t bytes) {
  physical::Layout result;
  result.shape[0] = bytes;
  return result;
}
int transfer(const std::shared_ptr<gpu_internal::Device>& device,
             const physical::Layout& layout, const void* source,
             std::uint64_t source_bytes, Domain source_domain,
             void* destination, std::uint64_t bytes, Domain destination_domain,
             bool cancelled = false, bool insufficient_staging = false) {
  test::NativeKernel kernel;
  PS_REQUIRE_OK(kernel.prepare_copy(device, layout));
  const auto source_layout = kernel_layout(layout);
  auto destination_layout = source_layout;
  auto stride = TensorSpan::element_size(layout.element_type);
  for (unsigned axis = layout.rank; axis-- > 0;) {
    destination_layout.strides[axis] = static_cast<std::int64_t>(stride);
    stride *= layout.shape[axis];
  }
  const ps_kernel_const_view_v8 input{source,
                                      source_bytes,
                                      layout.byte_offset,
                                      &source_layout,
                                      static_cast<std::uint32_t>(source_domain),
                                      0,
                                      0,
                                      0,
                                      0};
  const ps_kernel_mutable_view_v8 output{
      destination,
      bytes,
      0,
      &destination_layout,
      static_cast<std::uint32_t>(destination_domain),
      0,
      0,
      0,
      0};
  const auto staging_bytes = bytes * ((source_domain == Domain::Host) +
                                      (destination_domain == Domain::Host));
  MutableBuffer scratch;
  ps_kernel_scratch_v8 staging{};
  if (staging_bytes) {
    auto allocation =
        device->allocator(BufferAllocator{}).allocate(staging_bytes);
    PS_REQUIRE_OK(allocation);
    scratch = allocation.take_value();
    staging = {scratch.data(), staging_bytes - insufficient_staging,
               PS_KERNEL_DOMAIN_SHARED_V8, 0};
  }
  ps_kernel_invocation_v8 invocation{};
  invocation.inputs = &input;
  invocation.input_count = 1;
  invocation.outputs = &output;
  invocation.output_count = 1;
  invocation.scratch = staging_bytes ? &staging : nullptr;
  invocation.scratch_count = staging_bytes ? 1 : 0;
  const std::uint64_t object = 1;
  invocation.input_object_ids = &object;
  if (cancelled) {
    invocation.cancellation = {nullptr,
                               [](const void*) -> std::uint32_t { return 1; }};
  }
  const auto status = kernel.run(invocation);
  if (cancelled)
    PS_CHECK(status.code == ErrorCode::Cancelled);
  else if (insufficient_staging)
    PS_CHECK(status.code == ErrorCode::InvalidArgument);
  else
    PS_REQUIRE_OK(status);
  return 0;
}
int affine(const std::shared_ptr<gpu_internal::Device>& device) {
  // Odd sizes and a one-byte allocation subview exercise native base offsets.
  std::vector<std::uint8_t> source(1027);
  for (unsigned i = 0; i < source.size(); ++i)
    source[i] = static_cast<std::uint8_t>(i * 31 + 7);
  auto allocated =
      device->allocator(BufferAllocator{}).allocate(source.size() + 1);
  PS_REQUIRE_OK(allocated);
  auto shared_source = allocated.take_value();
  std::memcpy(shared_source.data() + 1, source.data(), source.size());
  auto native = device->allocate_private(source.size(), {});
  PS_REQUIRE_OK(native);
  auto private_source = native.take_value();
  PS_CHECK(transfer(device, dense(source.size()), source.data(), source.size(),
                    Domain::Host, private_source->handle(), source.size(),
                    Domain::Device) == 0);
  private_source->seal();
  for (unsigned pattern = 0; pattern < 6; ++pattern) {
    physical::Layout layout;
    std::vector<std::uint64_t> offsets;
    if (pattern == 0) {
      layout = dense(1025);
      layout.byte_offset = 1;
      for (unsigned i = 0; i < 1025; ++i)
        offsets.push_back(i + 1);
    } else if (pattern == 1) {
      layout.rank = 2;
      layout.shape = {3, 5};
      layout.strides = {-7, 1};
      layout.byte_offset = 17;
      for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 5; ++column)
          offsets.push_back(17 - 7 * row + column);
    } else if (pattern == 2) {
      layout.rank = 3;
      layout.shape = {3, 2, 65};
      layout.strides = {0, 70, -1};
      layout.byte_offset = 66;
      for (unsigned plane = 0; plane < 3; ++plane)
        for (unsigned row = 0; row < 2; ++row)
          for (unsigned column = 0; column < 65; ++column)
            offsets.push_back(66 + 70 * row - column);
    } else {
      // Unaligned, reversed multi-byte samples preserve their exact bit
      // pattern.
      const auto width = 1U << (pattern - 2);
      layout.element_type = pattern == 3   ? ElementType::UInt16
                            : pattern == 4 ? ElementType::Float32
                                           : ElementType::Float64;
      layout.shape[0] = 3;
      layout.strides[0] = -static_cast<std::int64_t>(width + 1);
      layout.byte_offset = 21;
      for (unsigned sample = 0; sample < 3; ++sample)
        for (unsigned byte = 0; byte < width; ++byte)
          offsets.push_back(21 - (width + 1) * sample + byte);
    }
    std::vector<std::uint8_t> expected;
    for (auto offset : offsets)
      expected.push_back(source[offset]);
    const auto bytes = expected.size();
    for (auto from : {Domain::Host, Domain::Shared, Domain::Device})
      for (auto to : {Domain::Host, Domain::Shared, Domain::Device}) {
        std::vector<std::uint8_t> host_output(bytes, 0xff);
        auto allocated =
            device->allocator(BufferAllocator{}).allocate(bytes + 1);
        PS_REQUIRE_OK(allocated);
        auto shared_output = allocated.take_value();
        auto native_output = device->allocate_private(bytes, {});
        PS_REQUIRE_OK(native_output);
        auto private_output = native_output.take_value();
        const void* input = from == Domain::Host     ? source.data()
                            : from == Domain::Shared ? shared_source.data() + 1
                                                     : private_source->handle();
        void* output = to == Domain::Host     ? host_output.data()
                       : to == Domain::Shared ? shared_output.data() + 1
                                              : private_output->handle();
        PS_CHECK(transfer(device, layout, input, source.size(), from, output,
                          bytes, to) == 0);
        if (to == Domain::Device) {
          private_output->seal();
          PS_CHECK(transfer(device, dense(bytes), output, bytes, Domain::Device,
                            host_output.data(), bytes, Domain::Host) == 0);
        } else if (to == Domain::Shared) {
          std::memcpy(host_output.data(), shared_output.data() + 1, bytes);
        }
        PS_CHECK(host_output == expected);
      }
  }
  std::array<std::uint8_t, 3> output{11, 12, 13};
  const auto original = output;
  PS_CHECK(transfer(device, dense(3), source.data(), source.size(),
                    Domain::Host, output.data(), 3, Domain::Host, true) == 0);
  PS_CHECK(output == original);
  PS_CHECK(transfer(device, dense(3), source.data(), source.size(),
                    Domain::Host, output.data(), 3, Domain::Host, false,
                    true) == 0);
  PS_CHECK(output == original);
  return 0;
}
OperationDefinition operation() {
  auto op = test::fixed_fixture::operation("copy.domains", 1);
  op.traits.supports_cpu = false;
  op.traits.supports_gpu = true;
  op.prepare_kernel = {};
  op.lower = [](const OperationLowering& query) -> Result<OperationProgram> {
    OperationProgramBuilder builder(query);
    if (query.requests[0].empty())
      return builder.finish();
    const auto schema =
        std::make_shared<const SchemaTemplate>(test::fixed_fixture::schema());
    physical::Id previous = physical::none;
    const std::array<Domain, 5> domains{Domain::Device, Domain::Shared,
                                        Domain::Device, Domain::Host,
                                        Domain::Host};
    for (unsigned step = 0; step < domains.size(); ++step) {
      physical::Placement placement;
      placement.backend = physical::Backend::Gpu;
      placement.domain = domains[step];
      auto task = builder.add_copy(placement);
      if (!task.ok())
        return Result<OperationProgram>(task.status());
      auto status = step == 0 ? builder.read_input(task.value(), 0, 0, 0)
                              : builder.read_tile(task.value(), previous);
      if (!status.ok())
        return Result<OperationProgram>(status);
      if (step + 1 == domains.size()) {
        status = builder.write_output(task.value(), 0, 0, 0);
        if (!status.ok())
          return Result<OperationProgram>(status);
      } else {
        auto value = builder.add_temporary(schema);
        if (!value.ok())
          return Result<OperationProgram>(value.status());
        auto tile = builder.write_temporary(task.value(), value.value(), 0, 0);
        if (!tile.ok())
          return Result<OperationProgram>(tile.status());
        previous = tile.value();
      }
    }
    return builder.finish();
  };
  return op;
}
int public_run(std::uint32_t backend) {
  auto registry = std::make_shared<OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(operation()));
  PS_REQUIRE_OK(registry->freeze());
  WorkflowDocument document;
  auto schema =
      std::make_shared<const SchemaTemplate>(test::fixed_fixture::schema());
  WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "source";
  input.result_schema = schema;
  document.inputs = {input};
  document.nodes = {{1, "copy.domains", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"result", 1, "out0"}};
  GraphContext graph(document);
  PlanningOptions options;
  options.execution_mode = ExecutionMode::NativeGpu;
  options.tile_extent = 4;
  auto compiled = Compiler(registry).compile(graph, options);
  PS_REQUIRE_OK(compiled);
  const auto& program = compiled.value().plan->program();
  PS_CHECK(program.tasks.size() == 5);
  PS_CHECK(program.tasks[0].scratch[2] == 4);
  PS_CHECK(program.tasks[1].scratch[2] == 0);
  PS_CHECK(program.tasks[3].scratch[2] == 4);
  PS_CHECK(program.tasks[4].scratch[2] == 8);
  auto bad = program;
  bad.tasks[4].scratch[2] = 7;
  PS_CHECK(physical::finalize_program(&bad).code == ErrorCode::InvalidArgument);
  auto encoded = CompilerArtifact::export_plan(*compiled.value().plan);
  PS_REQUIRE_OK(encoded);
  auto decoded = CompilerArtifact::import_plan(registry, encoded.value());
  PS_REQUIRE_OK(decoded);
  ResourceBudget retained;
  {
    ExecutionContext context(registry);
    retained = context.resource_budget().take_value();
    auto prepared = context.prepare(decoded.value());
    PS_REQUIRE_OK(prepared);
    // Simultaneous Runs must have independent scratch and output ownership.
    std::vector<RunHandle> runs;
    std::vector<std::array<std::uint8_t, 4>> expected;
    std::vector<std::uint64_t> source_objects;
    for (unsigned i = 0; i < 3; ++i) {
      const std::array<std::uint8_t, 4> bytes{
          static_cast<std::uint8_t>(i * 17), static_cast<std::uint8_t>(255 - i),
          128, 37};
      auto started = ResultBuilder::start(retained, *schema, "source");
      PS_REQUIRE_OK(started);
      auto builder = started.take_value();
      PS_REQUIRE_OK(
          builder.publish_tensor(0, Region::whole({4}), {bytes.data(), 4}));
      auto source = builder.seal();
      PS_REQUIRE_OK(source);
      ExecutionOptions trace;
      trace.trace_capacity = 5;
      auto run = context.submit(prepared.value(),
                                {{{"source", source.value()}}}, {}, trace);
      PS_REQUIRE_OK(run);
      runs.push_back(run.take_value());
      expected.push_back(bytes);
      source_objects.push_back(source.value().object_id());
    }
    for (unsigned run = 0; run < runs.size(); ++run) {
      auto result = runs[run].join();
      PS_REQUIRE_OK(result);
      const auto& output = result.value().results.at("result");
      auto facts = output.descriptor();
      PS_REQUIRE_OK(facts);
      for (unsigned i = 0; i < expected[run].size(); ++i) {
        std::uint8_t byte = 0;
        PS_REQUIRE_OK(output.read_tensor(facts.value(), 0, {i}, &byte, 1));
        PS_CHECK(byte == expected[run][i]);
      }
      PS_CHECK(output.object_id() != source_objects[run]);
      std::array<ExecutionTaskTiming, 5> timings;
      PS_CHECK(runs[run].observation().copy_task_timings(timings.data(), 5) ==
               5);
      PS_CHECK(result.value().diagnostics.copy_bytes == 20);
      for (const auto& timing : timings) {
        PS_CHECK(timing.copy_bytes == 4 &&
                 timing.submit_ns <= timing.completion_ns &&
                 timing.completion_ns <= timing.consumed_ns);
        // Vulkan queues may legitimately omit timestamp support. Metal's
        // supported device path exposes command-buffer timing.
        if (backend == PS_NATIVE_METAL_V2)
          PS_CHECK(timing.device.clock_domain ==
                   PS_KERNEL_CLOCK_METAL_MACH_NS_V8);
        if (timing.device.clock_domain == PS_KERNEL_CLOCK_UNAVAILABLE_V8)
          continue;
        PS_CHECK(timing.device.valid_bits > 0 &&
                 timing.device.valid_bits <= 64 &&
                 timing.device.tick_period_ns > 0 &&
                 timing.device.clock_instance != 0);
        if (timing.device.clock_domain == PS_KERNEL_CLOCK_METAL_MACH_NS_V8) {
          PS_CHECK(timing.device.end_tick >= timing.device.begin_tick);
        } else {
          PS_CHECK(timing.device.clock_domain ==
                       PS_KERNEL_CLOCK_VULKAN_TICKS_V8 &&
                   timing.device.valid_bits >= 36);
          const auto mask = timing.device.valid_bits == 64
                                ? UINT64_MAX
                                : (UINT64_C(1) << timing.device.valid_bits) - 1;
          PS_CHECK(!(timing.device.begin_tick & ~mask) &&
                   !(timing.device.end_tick & ~mask));
        }
      }
    }
  }
  for (auto live : retained.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
}  // namespace
int main() {
  auto device = ps::gpu_internal::Device::create();
  if (!device)
    return 77;
  std::cout << device->identity() << '\n';
  PS_CHECK(affine(device) == 0);
  PS_CHECK(public_run(device->backend()) == 0);
  return 0;
}
