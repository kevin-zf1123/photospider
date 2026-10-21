#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "execution/execution_test_hooks.hpp"
#include "execution/native_gpu.hpp"
#include "fixtures/native_vulkan_spirv.hpp"
#include "photospider/photospider.hpp"
#include "support/fixed_native_fixture.hpp"
#include "support/fixed_operation_fixture.hpp"
#include "support/native_allocation_quota.hpp"
#include "support/native_atlas_budget.hpp"
#include "support/status_check.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using gpu_internal::Device;
struct Parameters {
  std::uint32_t width, height, depth, multiplier, increment;
};
std::uint32_t expected_value(std::uint32_t value, const Parameters& p) {
  const std::uint64_t wide = std::uint64_t{value} * p.multiplier + p.increment;
  return static_cast<std::uint32_t>(wide ^ (wide >> 32));
}
struct Program {
  Parameters parameters;
  ps_kernel_view_spec_v8 input{}, output{};
  ps_kernel_preparation_v8 query{};
  std::array<ps_native_binding_v2, 2> bindings{};
  ps_native_program_v2 program{};
  explicit Program(Parameters p) : parameters(p) {
    const auto bytes = std::uint64_t{p.width} * p.height * p.depth * 4;
    input.domain = PS_KERNEL_DOMAIN_SHARED_V8;
    input.bytes = bytes;
    input.layout.rank = 1;
    input.layout.element_type = static_cast<std::uint32_t>(ElementType::UInt8);
    input.layout.shape[0] = bytes;
    input.layout.strides[0] = 1;
    output = input;
    query.struct_size = sizeof(query);
    query.placement.backend = PS_BACKEND_GPU_V8;
    query.placement.domain = PS_KERNEL_DOMAIN_SHARED_V8;
    query.reads = &input;
    query.read_count = 1;
    query.writes = &output;
    query.write_count = 1;
    bindings[0] = {0, PS_NATIVE_INPUT_V2, 0, 0, bytes};
    bindings[1] = {1, PS_NATIVE_OUTPUT_V2, 0, 0, bytes};
    program.struct_size = sizeof(program);
    program.code_format = PS_NATIVE_SPIRV_V2;
    program.source = {reinterpret_cast<const char*>(kNativeVulkanSpirv),
                      sizeof(kNativeVulkanSpirv)};
    program.entry = {"transform", 9};
    program.bindings = bindings.data();
    program.binding_count = 2;
    program.constants = &parameters;
    program.constant_bytes = sizeof(parameters);
    program.constant_index = 2;
    program.grid[0] = p.width;
    program.grid[1] = p.height;
    program.grid[2] = p.depth;
  }
  Status run(const test::NativeKernel& kernel, const CpuStorage& source,
             MutableBuffer& output_buffer, std::uint64_t offset = 0,
             const CancellationToken& token = {},
             const std::function<void()>& accepted = {}) const {
    ps_kernel_const_view_v8 read{};
    read.allocation = source.bytes().data();
    read.bytes = input.bytes;
    read.domain = input.domain;
    read.layout = &input.layout;
    ps_kernel_mutable_view_v8 write{};
    write.allocation = output_buffer.data() + offset;
    write.bytes = output.bytes;
    write.domain = output.domain;
    write.layout = &output.layout;
    const std::uint64_t identity = 1;
    ps_kernel_invocation_v8 invocation{};
    invocation.inputs = &read;
    invocation.input_count = 1;
    invocation.input_object_ids = &identity;
    invocation.outputs = &write;
    invocation.output_count = 1;
    invocation.cancellation = {
        &token, [](const void* raw) -> std::uint32_t {
          return static_cast<const CancellationToken*>(raw)->cancelled();
        }};
    return kernel.run(invocation, accepted);
  }
};
int capacity(const std::shared_ptr<Device>& device) {
  const auto queried = device->allocation_capacity(4);
  PS_CHECK(queried.ok() && queried.value() >= 4);
  const auto bytes = queried.value();
  const auto metadata =
      sizeof(CpuStorage) + ResourceBudget::lease_metadata_bytes();
  for (const auto kind : {ResourceKind::Host, ResourceKind::Device,
                          ResourceKind::Shared, ResourceKind::Payload}) {
    ResourceLimits limits;
    const auto exact_limit =
        bytes + (kind == ResourceKind::Host ? metadata : 0);
    limits.capacity[kind] = exact_limit - 1;
    ResourceBudget rejected(limits);
    auto failure = device->allocator(rejected.allocator()).allocate(4);
    PS_CHECK(!failure.ok() &&
             failure.status().code == ErrorCode::ResourceExhausted);
    for (auto live : rejected.statistics().live.values)
      PS_CHECK(live == 0);
    limits.capacity[kind] = exact_limit;
    ResourceBudget exact(limits);
    {
      auto value = device->allocator(exact.allocator()).allocate(4);
      PS_CHECK(value.ok() && value.value().size() == 4);
      for (const auto charged : {ResourceKind::Host, ResourceKind::Device,
                                 ResourceKind::Shared, ResourceKind::Payload})
        PS_CHECK(exact.statistics().live[charged] ==
                 bytes + (charged == ResourceKind::Host ? metadata : 0));
    }
    for (auto live : exact.statistics().live.values)
      PS_CHECK(live == 0);
  }
  PS_CHECK(!device->allocation_capacity(UINT64_MAX).ok());
  return 0;
}
bool memory_freed = false;
void observe_free() noexcept {
  memory_freed = true;
}
int allocation_failure(const std::shared_ptr<Device>& device) {
  for (std::uint32_t point : {1, 2, 3}) {
    memory_freed = false;
    bool retired = false, ordered = false;
    struct Lease {
      bool& retired;
      bool& ordered;
      ~Lease() {
        retired = true;
        ordered = memory_freed;
      }
    };
    BufferAllocator host([&](std::uint64_t) {
      return Result<std::shared_ptr<void>>(
          std::shared_ptr<void>(new Lease{retired, ordered}));
    });
    host = host.limited_requested(4);
    execution_testing::ExecutionTestHooks hooks;
    hooks.native_allocation_failure = point;
    hooks.native_memory_freed = observe_free;
    execution_testing::install_execution_test_hooks(&hooks);
    auto failed = device->allocator(host).allocate(4);
    execution_testing::install_execution_test_hooks(nullptr);
    PS_CHECK(!failed.ok() &&
             failed.status().code == ErrorCode::ResourceExhausted);
    PS_CHECK(retired && ordered && memory_freed);
    PS_CHECK(device->allocator(host).allocate(4).ok());
  }
  execution_testing::ExecutionTestHooks hooks;
  hooks.native_allocation_limit = 1;
  execution_testing::install_execution_test_hooks(&hooks);
  auto first = device->allocator(BufferAllocator()).allocate(4);
  const bool first_ok = first.ok();
  auto blocked = device->allocator(BufferAllocator()).allocate(4);
  first = Result<MutableBuffer>(MutableBuffer());
  auto reused = device->allocator(BufferAllocator()).allocate(4);
  execution_testing::install_execution_test_hooks(nullptr);
  PS_CHECK(first_ok && !blocked.ok() &&
           blocked.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(reused.ok());
  return 0;
}
int compute(const std::shared_ptr<Device>& device) {
  Program program({17, 9, 3, 0x1234567U, 71});
  auto allocator = device->allocator(BufferAllocator{});
  auto input = allocator.allocate(program.input.bytes).take_value();
  for (std::uint32_t i = 0; i < program.input.bytes / 4; ++i) {
    const auto value = 0xf0000000U + i;
    std::memcpy(input.data() + i * 4, &value, 4);
  }
  auto frozen = std::move(input).freeze();
  auto middle = allocator.allocate(program.output.bytes).take_value();
  auto output = allocator.allocate(program.output.bytes).take_value();
  test::NativeKernel first, second;
  PS_REQUIRE_OK(first.prepare(device, program.program, program.query));
  program.program.group[0] = 8;
  program.program.group[1] = 4;
  program.program.group[2] = 2;
  PS_REQUIRE_OK(second.prepare(device, program.program, program.query));
  PS_REQUIRE_OK(program.run(first, *frozen, middle));
  auto intermediate = std::move(middle).freeze();
  PS_REQUIRE_OK(program.run(second, *intermediate, output));
  for (std::uint32_t i = 0; i < program.output.bytes / 4; ++i) {
    std::uint32_t value = 0;
    std::memcpy(&value, output.data() + i * 4, 4);
    PS_CHECK(value ==
             expected_value(expected_value(0xf0000000U + i, program.parameters),
                            program.parameters));
  }
  // A prepared pipeline and immutable constants are shared by concurrent runs.
  auto parallel = allocator.allocate(program.output.bytes).take_value();
  auto work = std::async(std::launch::async,
                         [&] { return program.run(first, *frozen, parallel); });
  device->clear_pipeline_cache();
  PS_REQUIRE_OK(program.run(first, *frozen, output));
  PS_REQUIRE_OK(work.get());
  PS_CHECK(std::memcmp(output.data(), parallel.data(), output.size()) == 0);
  CancellationSource stop;
  PS_REQUIRE_OK(program.run(first, *frozen, output, 0, stop.token(),
                            [&] { stop.cancel(); }));
  PS_CHECK(program.run(first, *frozen, output, 0, stop.token()).code ==
           ErrorCode::Cancelled);
  for (std::uint32_t i = 0; i < program.output.bytes / 4; ++i) {
    std::uint32_t value = 0;
    std::memcpy(&value, output.data() + i * 4, 4);
    PS_CHECK(value == expected_value(0xf0000000U + i, program.parameters));
  }
  return 0;
}
int validation(const std::shared_ptr<Device>& device) {
  for (unsigned mode = 0; mode < 9; ++mode) {
    Program program({1, 1, 1, 3, 7});
    if (mode == 0)
      program.bindings[0].offset = 1;
    if (mode == 1)
      program.bindings[0].bytes = 5;
    if (mode == 2)
      program.program.group[0] = 8;
    if (mode == 3)
      program.program.code_format = PS_NATIVE_MSL_V2;
    if (mode == 4)
      program.program.source.size = 7;
    if (mode == 5)
      program.bindings[1].source = PS_NATIVE_INPUT_V2;
    if (mode == 6)
      program.program.grid[0] = UINT64_MAX;
    if (mode == 7)
      program.program.code_format = 99;
    if (mode == 8)
      program.program.constant_bytes = 4097;
    test::NativeKernel kernel;
    auto status = kernel.prepare(device, program.program, program.query);
    PS_CHECK(status.code == (mode == 3 ? ErrorCode::BackendUnavailable
                                       : ErrorCode::InvalidArgument));
    PS_CHECK(kernel.empty());
  }
  const auto offset = device->minimum_buffer_offset_alignment();
  auto allocator = device->allocator(BufferAllocator{});
  auto input = allocator.allocate(4).take_value();
  auto source = std::move(input).freeze();
  auto output =
      allocator.allocate(std::max<std::uint64_t>(256, offset + 4)).take_value();
  Program program({1, 1, 1, 3, 7});
  test::NativeKernel kernel;
  PS_REQUIRE_OK(kernel.prepare(device, program.program, program.query));
  PS_REQUIRE_OK(program.run(kernel, *source, output, offset));
  for (std::size_t i = 0; i < output.size(); i += 4) {
    std::uint32_t value = 0;
    std::memcpy(&value, output.data() + i, 4);
    PS_CHECK(value == (i == offset ? 7 : 0));
  }
  return 0;
}
int numeric_probe(const std::shared_ptr<Device>& device) {
  Program program({7, 1, 1, 0, 0});
  program.program.source = {
      reinterpret_cast<const char*>(kNativeVulkanNumericProbeSpirv),
      sizeof(kNativeVulkanNumericProbeSpirv)};
  program.program.entry = {"numeric_probe", 13};
  program.program.grid[0] = 1;
  program.program.constants = nullptr;
  program.program.constant_bytes = 0;
  auto allocator = device->allocator(BufferAllocator{});
  auto input = allocator.allocate(28).take_value();
  std::array<std::uint32_t, 7> values{1, 0x00800000};
  std::memcpy(input.data(), values.data(), sizeof(values));
  auto source = std::move(input).freeze();
  auto output = allocator.allocate(28).take_value();
  test::NativeKernel kernel;
  PS_REQUIRE_OK(kernel.prepare(device, program.program, program.query));
  PS_REQUIRE_OK(program.run(kernel, *source, output));
  std::memcpy(values.data(), output.data(), sizeof(values));
  PS_CHECK(values[0] == 1 && values[1] == 0x00800000 && values[2] == 1);
  std::cout << "fp32_probe_hex copy=" << std::hex << values[2]
            << " normal_half=" << values[3] << " tiny_add=" << values[4]
            << " sqrt_normal=" << values[5] << " sqrt_tiny=" << values[6]
            << std::dec << '\n';
  return 0;
}
int constants_budget() {
  Program program({1, 1, 1, 3, 7});
  auto probe = Device::create();
  const auto bytes = probe->allocation_capacity(sizeof(Parameters)).value();
  for (auto kind :
       {ResourceKind::Device, ResourceKind::Shared, ResourceKind::Payload}) {
    for (bool reject : {false, true}) {
      ResourceLimits limits;
      limits.capacity[kind] = bytes - reject;
      auto budget = std::make_shared<ResourceBudget>(limits);
      auto device = Device::create(budget);
      {
        test::NativeKernel kernel;
        auto result = kernel.prepare(device, program.program, program.query);
        PS_CHECK(result.ok() == !reject);
        if (reject)
          PS_CHECK(result.code == ErrorCode::ResourceExhausted &&
                   kernel.empty());
        PS_CHECK(budget->statistics().live[kind] == (reject ? 0 : bytes));
      }
      device->collect_expired_allocations();
      device->clear_pipeline_cache();
      for (auto live : budget->statistics().live.values)
        PS_CHECK(live == 0);
    }
  }
  return 0;
}
int metadata_budget() {
  Program small({1, 1, 1, 3, 7}), large({1, 1, 1, 3, 7});
  std::vector<std::uint32_t> words(std::begin(kNativeVulkanSpirv),
                                   std::end(kNativeVulkanSpirv));
  std::size_t at = 5;
  while (at < words.size() && (words[at] & 65535) != 248)
    at += words[at] >> 16;
  PS_CHECK(at < words.size());
  at += words[at] >> 16;
  words.insert(words.begin() + at, 4096, 0x00010000);  // OpNop in a block.
  large.program.source = {reinterpret_cast<const char*>(words.data()),
                          static_cast<std::uint32_t>(words.size() * 4)};
  for (auto kind :
       {ResourceKind::Host, ResourceKind::Metadata, ResourceKind::Entries}) {
    auto root = std::make_shared<ResourceBudget>();
    ResourceBudget foreign;
    ErrorCode sticky = ErrorCode::Ok;
    ResourceAllocationScope scope(*root, &sticky);
    auto device = Device::create(root);
    auto input = device->allocator(BufferAllocator{}).allocate(4).take_value();
    std::memset(input.data(), 0, 4);
    auto source = std::move(input).freeze();
    auto output = device->allocator(BufferAllocator{}).allocate(4).take_value();
    const auto initial = root->statistics().live;
    {
      ResourceAllocationScope other(foreign);
      test::NativeKernel owner;
      PS_REQUIRE_OK(owner.prepare(device, small.program, small.query));
      PS_CHECK(root->statistics().live[ResourceKind::Metadata] >
               initial[ResourceKind::Metadata]);
      for (auto n : foreign.statistics().live.values)
        PS_CHECK(n == 0);
    }
    device->collect_expired_allocations();
    device->clear_pipeline_cache();
    PS_CHECK(root->statistics().live.values == initial.values);
    const auto fill = [&](bool recoverable) {
      const auto available = root->available_capacity();
      ResourceCapacity capacity;
      if (kind == ResourceKind::Entries) {
        // Constant owner/map and fixed context need three entries; the next
        // submission must reclaim cached entries before admitting its node.
        capacity[kind] = available[kind] - (recoverable ? 3 : 0);
      } else {
        capacity = ResourceCapacity::host(
            available[kind] - ResourceBudget::lease_metadata_bytes(),
            kind == ResourceKind::Metadata
                ? available[kind] - ResourceBudget::lease_metadata_bytes()
                : 0);
      }
      return root->reserve(capacity).take_value();
    };
    for (bool hit : {false, true}) {
      {
        test::NativeKernel warm;
        PS_REQUIRE_OK(warm.prepare(device, large.program, large.query));
      }
      device->collect_expired_allocations();
      auto held = fill(true);
      {
        auto& selected = hit ? large : small;
        test::NativeKernel next;
        PS_REQUIRE_OK(next.prepare(device, selected.program, selected.query));
        PS_REQUIRE_OK(selected.run(next, *source, output));
        PS_CHECK(sticky == ErrorCode::Ok);
        std::uint32_t value = 0;
        std::memcpy(&value, output.data(), 4);
        PS_CHECK(value == 7);
      }
      held = {};
      device->collect_expired_allocations();
      device->clear_pipeline_cache();
      PS_CHECK(root->statistics().live.values == initial.values);
    }
    auto held = fill(false);
    const auto full = root->statistics().live;
    test::NativeKernel denied;
    PS_CHECK(denied.prepare(device, small.program, small.query).code ==
             ErrorCode::ResourceExhausted);
    PS_CHECK(denied.empty() && sticky == ErrorCode::ResourceExhausted &&
             root->statistics().live.values == full.values);
    held = {};
    sticky = ErrorCode::Ok;
    {
      test::NativeKernel recovered;
      PS_REQUIRE_OK(recovered.prepare(device, small.program, small.query));
      PS_REQUIRE_OK(small.run(recovered, *source, output));
    }
    source.reset();
    output = {};
    device->collect_expired_allocations();
    device->clear_pipeline_cache();
    for (auto live : root->statistics().live.values)
      PS_CHECK(live == 0);
  }
  ResourceBudget foreign;
  {
    ResourceAllocationScope scope(foreign);
    auto device = Device::create();
    test::NativeKernel kernel;
    PS_REQUIRE_OK(kernel.prepare(device, small.program, small.query));
    for (auto live : foreign.statistics().live.values)
      PS_CHECK(live == 0);
  }
  return 0;
}
int completion_releases_device() {
  struct State {
    std::shared_ptr<Device> device = Device::create();
    ps_prepared_kernel_v8 kernel{};
    std::mutex mutex;
    std::condition_variable changed;
    bool returned = false, done = false, key_ok = false;
    ps_kernel_status_v8 status{};
    ~State() {
      if (kernel.release)
        kernel.release(kernel.context);
    }
  } state;
  std::weak_ptr<Device> weak = state.device;
  Program program({1, 1, 1, 3, 7});
  auto service = state.device->preparation_service();
  PS_REQUIRE_OK(kernel_status(service.prepare(service.context, &program.program,
                                              &program.query, &state.kernel)));
  auto input =
      state.device->allocator(BufferAllocator{}).allocate(4).take_value();
  auto source = std::move(input).freeze();
  auto output =
      state.device->allocator(BufferAllocator{}).allocate(4).take_value();
  ps_kernel_const_view_v8 read{};
  read.allocation = source->bytes().data();
  read.bytes = 4;
  read.domain = PS_KERNEL_DOMAIN_SHARED_V8;
  read.layout = &program.input.layout;
  ps_kernel_mutable_view_v8 write{};
  write.allocation = output.data();
  write.bytes = 4;
  write.domain = PS_KERNEL_DOMAIN_SHARED_V8;
  write.layout = &program.output.layout;
  const std::uint64_t identity = 1;
  ps_kernel_invocation_v8 invocation{};
  invocation.struct_size = sizeof(invocation);
  invocation.abi_version = 8;
  invocation.prepared_context = state.kernel.context;
  invocation.inputs = &read;
  invocation.input_count = 1;
  invocation.input_object_ids = &identity;
  invocation.outputs = &write;
  invocation.output_count = 1;
  const ps_kernel_completion_v8 completion{
      &state,
      {3, 5, 7},
      [](void* raw, ps_kernel_completion_key_v8 key,
         const ps_kernel_status_v8* status) {
        auto& value = *static_cast<State*>(raw);
        std::unique_lock<std::mutex> lock(value.mutex);
        value.changed.wait(lock, [&] { return value.returned; });
        value.status = *status;
        value.key_ok =
            key.run == 3 && key.activation == 5 && key.submission == 7;
        auto kernel = value.kernel;
        value.kernel = {};
        lock.unlock();
        kernel.release(kernel.context);
        // No submit-stack Device reference remains. Destruction must stop the
        // completion queue on its own worker without self-joining.
        value.device.reset();
        lock.lock();
        value.done = true;
        value.changed.notify_all();
      },
      nullptr};
  const auto accepted = state.kernel.submit_gpu(&invocation, &completion);
  PS_CHECK(accepted.accepted == 1);
  {
    std::unique_lock<std::mutex> lock(state.mutex);
    state.returned = true;
    state.changed.notify_all();
    state.changed.wait(lock, [&] { return state.done; });
  }
  PS_REQUIRE_OK(kernel_status(state.status));
  PS_CHECK(state.key_ok && weak.expired());
  std::uint32_t value = 0;
  std::memcpy(&value, output.data(), 4);
  PS_CHECK(value == 7);
  return 0;
}
OperationDefinition operation(bool copy) {
  auto result =
      test::fixed_fixture::operation(copy ? "upload" : "transform", 1);
  result.traits.supports_cpu = false;
  result.traits.supports_gpu = true;
  result.lower = [copy](const OperationLowering& query) {
    OperationProgramBuilder builder(query);
    if (query.requests[0].empty())
      return builder.finish();
    physical::Placement placement;
    placement.backend = copy ? physical::Backend::Cpu : physical::Backend::Gpu;
    placement.domain = physical::Domain::Shared;
    auto task = copy ? builder.add_copy(placement)
                     : builder.add_task("transform", "u32-v1", placement);
    if (!task.ok())
      return Result<OperationProgram>(task.status());
    auto status = builder.read_input(
        task.value(), 0, 0, 0,
        copy ? physical::Domain::Host : physical::Domain::Shared);
    if (status.ok())
      status = builder.write_output(task.value(), 0, 0, 0);
    return status.ok() ? builder.finish() : Result<OperationProgram>(status);
  };
  result.prepare_kernel = [copy](const auto& kernel, const auto& placement,
                                 const KernelPreparation& query,
                                 const ResourceSet&,
                                 const ps_native_gpu_service_v2* service)
      -> Result<ps_prepared_kernel_v8> {
    if (copy || !service || service->backend != PS_NATIVE_VULKAN_V2)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "Vulkan required"});
    if (kernel.symbol != "transform" || kernel.contract != "u32-v1" ||
        placement.backend != physical::Backend::Gpu ||
        !query.task_parameters.empty() ||
        query.scratch != std::array<std::uint64_t, 3>{} ||
        query.reads.size() != 1 || query.writes.size() != 1)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::InvalidArgument, "Vulkan fixture signature"});
    Program program({1, 1, 1, 3, 7});
    const auto valid = [](const auto& view) {
      return view.domain == physical::Domain::Shared && view.bytes == 4 &&
             view.layout.rank == 1 && view.layout.shape[0] == 4 &&
             view.layout.element_type == ElementType::UInt8 &&
             !view.layout.byte_offset && !view.layout.origin[0] &&
             view.layout.strides[0] == 1;
    };
    if (!valid(query.reads[0]) || !valid(query.writes[0]))
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::InvalidArgument, "Vulkan fixture views"});
    program.input.layout = kernel_layout(query.reads[0].layout);
    program.output.layout = kernel_layout(query.writes[0].layout);
    ps_prepared_kernel_v8 raw{};
    const auto status = kernel_status(service->prepare(
        service->context, &program.program, &program.query, &raw));
    return status.ok() ? Result<ps_prepared_kernel_v8>(raw)
                       : Result<ps_prepared_kernel_v8>(status);
  };
  if (copy)
    result.prepare_kernel = {};
  return result;
}
int workflow(const std::shared_ptr<Device>& probe) {
  auto registry = std::make_shared<OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(operation(true)));
  PS_REQUIRE_OK(registry->register_operation(operation(false)));
  PS_REQUIRE_OK(registry->freeze());
  WorkflowDocument document;
  WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "source";
  input.result_schema =
      std::make_shared<const SchemaTemplate>(test::fixed_fixture::schema());
  document.inputs = {input};
  document.nodes = {{1, "upload", {WorkflowInputReference{1}}, {}},
                    {2, "transform", {WorkflowNodeOutput{1, "out0"}}, {}},
                    {3, "transform", {WorkflowNodeOutput{2, "out0"}}, {}}};
  document.outputs = {{"result", 3, "out0"}};
  GraphContext graph(document);
  PlanningOptions options;
  options.execution_mode = ExecutionMode::NativeGpu;
  options.tile_extent = 4;
  auto compiled = Compiler(registry).compile(graph, options);
  PS_REQUIRE_OK(compiled);
  const auto payload = probe->allocation_capacity(4).value();
  const auto constants = probe->allocation_capacity(sizeof(Parameters)).value();
  ResourceBudget inputs;
  auto builder = ResultBuilder::start(inputs, test::fixed_fixture::schema(),
                                      "input", {}, 4)
                     .take_value();
  const std::uint32_t zero = 0;
  PS_REQUIRE_OK(builder.publish_tensor(
      0, Region::whole({4}),
      {reinterpret_cast<const std::uint8_t*>(&zero), 4}));
  auto binding = builder.seal().take_value();
  for (auto kind : {ResourceKind::Device, ResourceKind::Shared}) {
    for (bool reject : {false, true}) {
      ResourceBudget root;
      ResultRef retained;
      {
        ExecutionContextConfig config;
        config.managed_resources = ResourceLimits{};
        config.managed_resources->capacity[kind] =
            3 * payload + 2 * constants - reject;
        ExecutionContext context(registry, config);
        root = context.resource_budget().take_value();
        auto prepared = context.prepare(compiled.value().plan.value());
        PS_REQUIRE_OK(prepared);
        ExecutionOptions trace;
        trace.trace_capacity = 3;
        auto run = context.submit(prepared.value(), {{{"source", binding}}}, {},
                                  trace);
        PS_REQUIRE_OK(run);
        auto result = run.value().join();
        PS_CHECK(result.ok() == !reject);
        if (reject) {
          PS_CHECK(result.status().code == ErrorCode::ResourceExhausted);
        } else {
          PS_CHECK(result.value().diagnostics.completed_tasks == 3);
          std::array<ExecutionTaskTiming, 3> timings;
          PS_CHECK(run.value().observation().copy_task_timings(
                       timings.data(), timings.size()) == 3);
          for (const auto& timing : timings) {
            PS_CHECK(timing.submit_ns <= timing.completion_ns &&
                     timing.completion_ns <= timing.consumed_ns);
            const auto& device = timing.device;
            if (device.clock_domain == PS_KERNEL_CLOCK_UNAVAILABLE_V8)
              continue;  // Queue timestamps are an optional capability.
            PS_CHECK(timing.backend == physical::Backend::Gpu &&
                     device.clock_domain == PS_KERNEL_CLOCK_VULKAN_TICKS_V8 &&
                     device.valid_bits >= 36 && device.valid_bits <= 64 &&
                     device.tick_period_ns > 0 && device.clock_instance);
            const auto mask = device.valid_bits == 64
                                  ? UINT64_MAX
                                  : (UINT64_C(1) << device.valid_bits) - 1;
            PS_CHECK(!(device.begin_tick & ~mask) &&
                     !(device.end_tick & ~mask));
            std::cout << "Vulkan task " << timing.task
                      << " timestamp bits=" << device.valid_bits
                      << " period_ns=" << device.tick_period_ns
                      << " modulo_ticks="
                      << ((device.end_tick - device.begin_tick) & mask) << '\n';
          }
          retained = result.value().results.at("result");
        }
      }
      PS_CHECK(root.statistics().live[kind] == (reject ? 0 : payload));
      if (!reject) {
        auto descriptor = retained.descriptor().take_value();
        std::uint32_t bits = 0;
        for (unsigned i = 0; i < 4; ++i) {
          std::uint8_t byte = 0;
          PS_REQUIRE_OK(retained.read_tensor(descriptor, 0, {i}, &byte, 1));
          bits |= std::uint32_t{byte} << (8 * i);
        }
        PS_CHECK(bits == 28);
      }
    }
  }
  return 0;
}
}  // namespace
int main() {
  auto device = Device::create();
  if (!device || device->backend() != PS_NATIVE_VULKAN_V2) {
    std::cout << "Vulkan unavailable: native hardware tests skipped\n";
    return 77;
  }
  std::cout << device->identity() << '\n';
  PS_CHECK(capacity(device) == 0);
  PS_CHECK(native_requested_quota(device) == 0);
  PS_CHECK(native_atlas_budget(device) == 0);
  PS_CHECK(allocation_failure(device) == 0);
  PS_CHECK(compute(device) == 0);
  PS_CHECK(validation(device) == 0);
  PS_CHECK(numeric_probe(device) == 0);
  PS_CHECK(constants_budget() == 0);
  PS_CHECK(metadata_budget() == 0);
  PS_CHECK(completion_releases_device() == 0);
  PS_CHECK(workflow(device) == 0);
  auto buffer = device->allocator(BufferAllocator{}).allocate(4).take_value();
  const std::uint32_t bits = 0x1234abcd;
  std::memcpy(buffer.data(), &bits, 4);
  auto retained = std::move(buffer).freeze();
  device.reset();
  std::uint32_t actual = 0;
  std::memcpy(&actual, retained->bytes().data(), 4);
  PS_CHECK(actual == bits);
  return 0;
}
