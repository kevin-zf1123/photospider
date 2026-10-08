#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>

#include "execution/execution_test_hooks.hpp"
#include "execution/memory_budget.hpp"
#include "execution/native_gpu.hpp"
#include "photospider/execution/resources.hpp"
#include "support/native_allocation_quota.hpp"
#include "support/native_atlas_budget.hpp"
#include "support/native_metadata_budget.hpp"
#include "support/test_support.hpp"

namespace {
ps::CancellationSource* active_cancellation = nullptr;
void cancel_submitted() noexcept {
  active_cancellation->cancel();
}
int shared_capacity(const std::shared_ptr<ps::gpu_internal::Device>& device) {
  using ps::ResourceBudget;
  using ps::ResourceKind;
  using ps::ResourceLimits;
  constexpr std::uint64_t capacity = 32768;
  for (const auto kind : {ResourceKind::Host, ResourceKind::Device,
                          ResourceKind::Shared, ResourceKind::Payload}) {
    ResourceLimits limits;
    limits.capacity[kind] = capacity - 1;
    ResourceBudget root(limits);
    const auto native = device->allocator(root.allocator());
    auto rejected = native.allocate(16385);
    PS_CHECK(!rejected.ok() &&
             rejected.status().code == ps::ErrorCode::ResourceExhausted &&
             rejected.status().reason == ps::FailureReason::CapacityLimit);
    for (const auto value : root.statistics().live.values)
      PS_CHECK(value == 0);
    // A device sublimit applies to device allocations independently of CPU
    // capacity. Failed rounded admission must leave the root reusable.
    auto host = root.allocator().allocate(16).take_value();
    PS_CHECK(root.statistics().live[ResourceKind::Device] == 0 &&
             root.statistics().live[ResourceKind::Shared] == 0);
  }
  for (bool scope_before_native : {false, true}) {
    ResourceLimits limits;
    limits.capacity[ResourceKind::Device] = capacity;
    limits.capacity[ResourceKind::Shared] = capacity;
    ResourceBudget root(limits);
    auto host = root.allocator();
    auto native = scope_before_native
                      ? device->allocator(host.limited(capacity))
                      : device->allocator(host).limited(capacity);
    auto bytes = native.allocate(16385).take_value();
    const auto live = root.statistics().live;
    PS_CHECK(live[ResourceKind::Device] == capacity &&
             live[ResourceKind::Shared] == capacity &&
             live[ResourceKind::Payload] == capacity);
    PS_CHECK(live[ResourceKind::Host] ==
             capacity + sizeof(ps::CpuStorage) +
                 ResourceBudget::lease_metadata_bytes());
    ps::gpu_internal::Invocation invocation(device, {});
    const auto* api = invocation.service();
    std::uint64_t token = 0;
    PS_CHECK(api->buffer(api->context, bytes.data(), bytes.size(), 1, &token) ==
             0);
    bytes = {};
    PS_CHECK(root.statistics().live[ResourceKind::Shared] == capacity);
    PS_CHECK(!native.allocate(1).ok());
    PS_CHECK(api->release(api->context, token) == 0);
    for (const auto value : root.statistics().live.values)
      PS_CHECK(value == 0);
    PS_CHECK(native.allocate(16385).ok());
  }
  // CPU and native allocations converted from one scoped allocator share its
  // aggregate quota, while root dimensions still describe their own storage.
  ResourceBudget root;
  auto host = root.allocator().limited(capacity + 32);
  auto native = device->allocator(host);
  auto cpu = host.allocate(32).take_value();
  auto gpu = native.allocate(16385).take_value();
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == capacity + 32);
  PS_CHECK(root.statistics().live[ResourceKind::Shared] == capacity);
  PS_CHECK(!host.allocate(1).ok());
  gpu = {};
  PS_CHECK(root.statistics().live[ResourceKind::Shared] == 0);
  PS_CHECK(host.allocate(1).ok());
  for (bool on_demand : {false, true}) {
    ResourceLimits limits;
    limits.capacity[ResourceKind::Shared] = capacity;
    auto managed = std::make_shared<ResourceBudget>(limits);
    auto memory = std::make_shared<ps::execution_internal::MemoryBudget>(
        1 << 20, managed);
    auto observation =
        std::make_shared<ps::execution_internal::MemoryObservation>();
    auto reservation =
        memory->reserve(capacity * 4, {}, observation).take_value();
    auto allocator =
        device->allocator(on_demand ? memory->on_demand_allocator(observation)
                                    : reservation->allocator());
    auto buffer = allocator.allocate(16385).take_value();
    const auto before = managed->statistics().live;
    PS_CHECK(before[ResourceKind::Device] == capacity &&
             before[ResourceKind::Shared] == capacity);
    PS_CHECK(!allocator.allocate(1).ok());
    PS_CHECK(managed->statistics().live.values == before.values);
    reservation->seal();
    PS_CHECK(managed->statistics().live[ResourceKind::Payload] == capacity);
    auto retained = std::move(buffer).freeze();
    reservation.reset();
    PS_CHECK(managed->statistics().live[ResourceKind::Shared] == capacity);
    retained.reset();
    PS_CHECK(managed->statistics().live[ResourceKind::Device] == 0 &&
             managed->statistics().live[ResourceKind::Shared] == 0 &&
             managed->statistics().live[ResourceKind::Payload] == 0);
  }
  return 0;
}
int token_lifetime(const std::shared_ptr<ps::gpu_internal::Device>& device) {
  auto allocator = device->allocator(ps::BufferAllocator());
  ps::gpu_internal::Invocation invocation(device, {});
  const auto* api = invocation.service();
  PS_CHECK(api->struct_size == sizeof(*api) &&
           api->abi_version == PS_GPU_ABI_VERSION_1);
  auto storage = allocator.allocate(16).take_value();
  auto owner = std::move(storage).freeze();
  std::weak_ptr<const ps::CpuStorage> weak = owner;
  std::uint64_t token = 0;
  PS_CHECK(api->buffer(api->context, owner->bytes().data(), 16, 0, &token) ==
           0);
  owner.reset();
  PS_CHECK(!weak.expired());
  PS_CHECK(api->release(api->context, token) == 0 && weak.expired());
  auto next = allocator.allocate(16).take_value();
  std::uint64_t old = token;
  for (unsigned i = 0; i < 4096; ++i) {
    PS_CHECK(api->buffer(api->context, next.data(), 16, 1, &token) == 0);
    PS_CHECK(token != old);
    old = token;
    PS_CHECK(api->release(api->context, token) == 0);
  }
  PS_CHECK(api->buffer(api->context, next.data(), 16, 1, &token) == 0);
  PS_CHECK(api->release(api->context, old) != 0);
  PS_CHECK(invocation.status().code == ps::ErrorCode::InvalidArgument);
  // A previous sticky error must not prevent releasing valid live owners.
  PS_CHECK(api->release(api->context, token) == 0);
  ps::gpu_internal::Invocation foreign(device, {});
  const auto* remote = foreign.service();
  int returned = 0;
  std::thread worker([&] {
    returned = remote->buffer(remote->context, next.data(), 16, 1, &token);
  });
  worker.join();
  PS_CHECK(returned != 0 &&
           foreign.status().code == ps::ErrorCode::InvalidArgument);
  return 0;
}
int complete_groups(const std::shared_ptr<ps::gpu_internal::Device>& device) {
  auto storage =
      device->allocator(ps::BufferAllocator()).allocate(16).take_value();
  ps::gpu_internal::Invocation invocation(device, {});
  const auto* api = invocation.service();
  std::uint64_t token = 0;
  PS_CHECK(api->buffer(api->context, storage.data(), 16, 1, &token) == 0);
  const char shader[] =
      "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void reduce(device uint* out [[buffer(0)]], "
      "uint i [[thread_index_in_threadgroup]], uint3 g "
      "[[threadgroup_position_in_grid]]) {"
      "threadgroup uint values[12]; values[i]=i+1; "
      "threadgroup_barrier(mem_flags::mem_threadgroup);"
      "if(i==0){uint sum=0;for(uint "
      "k=0;k<12;++k)sum+=values[k];out[g.y*2+g.x]=sum;}}";
  const ps_gpu_buffer_binding_v1 binding{
      sizeof(ps_gpu_buffer_binding_v1), 0, token, 0, 16, 1};
  ps_gpu_dispatch_v1 command{};
  command.struct_size = sizeof(command);
  command.source = shader;
  command.source_size = sizeof(shader) - 1;
  command.entry = "reduce";
  command.entry_size = 6;
  command.buffers = &binding;
  command.buffer_count = 1;
  command.grid[0] = 7;
  command.grid[1] = 5;
  command.grid[2] = 1;
  command.group[0] = 4;
  command.group[1] = 3;
  command.group[2] = 1;
  PS_CHECK(api->execute(api->context, &command, 1) == 0);
  for (unsigned i = 0; i < 4; ++i) {
    std::uint32_t sum = 0;
    std::memcpy(&sum, storage.data() + 4 * i, 4);
    PS_CHECK(sum == 78);
  }
  // Reuse the slot, then reject a dispatch retaining its old generation.
  PS_CHECK(api->release(api->context, token) == 0);
  std::uint64_t replacement = 0;
  PS_CHECK(api->buffer(api->context, storage.data(), 16, 1, &replacement) == 0);
  PS_CHECK(api->execute(api->context, &command, 1) != 0);
  PS_CHECK(invocation.status().code == ps::ErrorCode::InvalidArgument &&
           invocation.statistics().dispatches == 1);
  PS_CHECK(api->release(api->context, replacement) == 0);
  return 0;
}
}  // namespace

int main() {
  using ps::BufferAllocator;
  using ps::CpuStorage;
  using ps::ErrorCode;
  using ps::gpu_internal::allocation_capacity;
  using ps::gpu_internal::Device;
  using ps::gpu_internal::Invocation;
  PS_CHECK(allocation_capacity(16385) == 32768);
  PS_CHECK(allocation_capacity(UINT64_MAX) == 0);
  auto device = Device::create();
  if (!device) {
    std::cout << "Metal unavailable: native hardware tests skipped\n";
    return 77;
  }
  const char metadata_shader[] =
      "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void metadata(device uint* out [[buffer(0)]]) {out[0]=7;}";
  ps_gpu_dispatch_v1 metadata_command{};
  metadata_command.struct_size = sizeof(metadata_command);
  metadata_command.source = metadata_shader;
  metadata_command.source_size = sizeof(metadata_shader) - 1;
  metadata_command.entry = "metadata";
  metadata_command.entry_size = 8;
  metadata_command.grid[0] = metadata_command.grid[1] =
      metadata_command.grid[2] = 1;
  PS_CHECK(native_metadata_testing::check(metadata_command) == 0);
  PS_CHECK(token_lifetime(device) == 0);
  PS_CHECK(shared_capacity(device) == 0);
  PS_CHECK(native_requested_quota(device) == 0);
  PS_CHECK(native_atlas_budget(device) == 0);
  PS_CHECK(complete_groups(device) == 0);
  auto allocator = device->allocator(BufferAllocator());
  auto input = allocator.allocate(16).take_value();
  const float data[4] = {0, .25F, .5F, 1};
  std::memcpy(input.data(), data, sizeof(data));
  auto frozen = std::move(input).freeze();
  auto output = allocator.allocate(16).take_value();
  PS_CHECK(device->owns(*frozen));
  PS_CHECK(!device->view(frozen->bytes().data(), 16, true).ok());
  PS_CHECK(!device->view(reinterpret_cast<const std::uint8_t*>(data), 16, false)
                .ok());
  std::shared_ptr<const CpuStorage> retained;
  {
    Invocation invocation(device, {});
    const auto* api = invocation.service();
    std::uint64_t source = 0, destination = 0;
    PS_CHECK(
        api->buffer(api->context, frozen->bytes().data(), 16, 0, &source) == 0);
    PS_CHECK(api->buffer(api->context, output.data(), 16, 1, &destination) ==
             0);
    const char shader[] =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void scale(device const float* a [[buffer(0)]], "
        "device float* b [[buffer(1)]], uint i [[thread_position_in_grid]])"
        "{b[i]=a[i]*.5f;}";
    ps_gpu_buffer_binding_v1 buffers[] = {
        {sizeof(ps_gpu_buffer_binding_v1), 0, source, 0, 16, 0},
        {sizeof(ps_gpu_buffer_binding_v1), 1, destination, 0, 16, 1}};
    ps_gpu_dispatch_v1 command{};
    command.struct_size = sizeof(command);
    command.source = shader;
    command.source_size = sizeof(shader) - 1;
    command.entry = "scale";
    command.entry_size = 5;
    command.buffers = buffers;
    command.buffer_count = 2;
    command.grid[0] = 4;
    command.grid[1] = command.grid[2] = 1;
    PS_CHECK(api->execute(api->context, &command, 1) == 0);
    PS_CHECK(invocation.statistics().dispatches == 1);
    float actual[4];
    std::memcpy(actual, output.data(), sizeof(actual));
    for (int i = 0; i < 4; ++i)
      PS_CHECK(actual[i] == data[i] * .5F);
    // This shader supplies no pragma: the host must disable contraction.
    auto rounding = allocator.allocate(16).take_value();
    const float operands[4] = {0x1.000002p0F, 1, 1, 1};
    std::memcpy(rounding.data(), operands, sizeof(operands));
    std::uint64_t rounding_token = 0;
    PS_CHECK(api->buffer(api->context, rounding.data(), 16, 0,
                         &rounding_token) == 0);
    const char arithmetic[] =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void scale(device const float* a [[buffer(0)]], "
        "device float* b [[buffer(1)]], uint i [[thread_position_in_grid]])"
        "{b[i]=a[i]*0x1.fffffcp-1f-1.0f;}";
    buffers[0].token = rounding_token;
    command.source = arithmetic;
    command.source_size = sizeof(arithmetic) - 1;
    PS_CHECK(api->execute(api->context, &command, 1) == 0);
    float rounded = 1;
    std::memcpy(&rounded, output.data(), 4);
    PS_CHECK(rounded == 0);  // FMA would produce -2^-46.
    buffers[0].token = source;
    command.source = shader;
    command.source_size = sizeof(shader) - 1;
    PS_CHECK(api->execute(api->context, &command, 1) == 0);
    retained = std::move(output).freeze();
    buffers[0].byte_size = 17;
    PS_CHECK(api->execute(api->context, &command, 1) != 0);
    PS_CHECK(invocation.status().code == ErrorCode::InvalidArgument);
    PS_CHECK(invocation.statistics().dispatches == 3);
  }
  {
    auto storage = allocator.allocate(16).take_value();
    std::memset(storage.data(), 0, 16);
    ps::CancellationSource cancellation;
    Invocation invocation(device, cancellation.token());
    const auto* api = invocation.service();
    std::uint64_t token = 0;
    PS_CHECK(api->buffer(api->context, storage.data(), 16, 1, &token) == 0);
    const char shader[] =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void work(device uint* out [[buffer(0)]], "
        "uint i [[thread_position_in_grid]]) {uint v=i;"
        "for(uint j=0;j<100000;++j)v=1664525u*v+1013904223u;out[i]=v;}";
    const ps_gpu_buffer_binding_v1 binding{
        sizeof(ps_gpu_buffer_binding_v1), 0, token, 0, 16, 1};
    ps_gpu_dispatch_v1 command{};
    command.struct_size = sizeof(command);
    command.source = shader;
    command.source_size = sizeof(shader) - 1;
    command.entry = "work";
    command.entry_size = 4;
    command.buffers = &binding;
    command.buffer_count = 1;
    command.grid[0] = 4;
    command.grid[1] = command.grid[2] = 1;
    ps::execution_testing::ExecutionTestHooks hooks;
    hooks.native_submitted = cancel_submitted;
    active_cancellation = &cancellation;
    ps::execution_testing::install_execution_test_hooks(&hooks);
    const auto status = api->execute(api->context, &command, 1);
    ps::execution_testing::install_execution_test_hooks(nullptr);
    active_cancellation = nullptr;
    PS_CHECK(status != 0 && invocation.status().code == ErrorCode::Cancelled);
    PS_CHECK(invocation.statistics().dispatches == 1 &&
             invocation.statistics().submissions == 1);
    // Cancellation must drain the command: all writes are visible on return.
    for (std::uint32_t i = 0; i < 4; ++i) {
      std::uint32_t expected = i, actual = 0;
      for (unsigned j = 0; j < 100000; ++j)
        expected = 1664525U * expected + 1013904223U;
      std::memcpy(&actual, storage.data() + i * 4, 4);
      PS_CHECK(actual == expected);
    }
  }
  allocator = BufferAllocator();
  device.reset();
  float last = 0;
  std::memcpy(&last, retained->bytes().data() + 12, 4);
  PS_CHECK(last == .5F);
  std::cout << "Metal native allocation, dispatch, bounds and retained "
               "lifetime passed\n";
  return 0;
}
