#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "execution/native_gpu.hpp"
#include "photospider/core/resource_allocator.hpp"
#include "support/fixed_native_fixture.hpp"
#include "support/native_allocation_quota.hpp"
#include "support/native_atlas_budget.hpp"
#include "support/status_check.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using gpu_internal::Device;
struct Program {
  std::string source;
  ps_kernel_view_spec_v8 output{}, input{};
  ps_kernel_preparation_v8 query{};
  ps_native_binding_v2 bindings[2]{};
  ps_native_program_v2 program{};
  Program(std::string code, std::uint64_t bytes = 16, bool reads = false,
          ElementType type = ElementType::UInt8)
      : source(std::move(code)) {
    output.domain = PS_KERNEL_DOMAIN_SHARED_V8;
    output.bytes = bytes;
    output.layout.rank = 1;
    output.layout.element_type = static_cast<std::uint32_t>(type);
    const unsigned width = type == ElementType::Float32 ? 4 : 1;
    output.layout.shape[0] = bytes / width;
    output.layout.strides[0] = width;
    input = output;
    query.struct_size = sizeof(query);
    query.placement.backend = PS_BACKEND_GPU_V8;
    query.placement.domain = PS_KERNEL_DOMAIN_SHARED_V8;
    query.writes = &output;
    query.write_count = 1;
    query.reads = reads ? &input : nullptr;
    query.read_count = reads ? 1 : 0;
    bindings[0] = {0, reads ? PS_NATIVE_INPUT_V2 : PS_NATIVE_OUTPUT_V2, 0, 0,
                   bytes};
    bindings[1] = {1, PS_NATIVE_OUTPUT_V2, 0, 0, bytes};
    program.struct_size = sizeof(program);
    program.source = {source.data(), static_cast<std::uint32_t>(source.size())};
    program.entry = {"run", 3};
    program.bindings = bindings;
    program.binding_count = reads ? 2 : 1;
    program.grid[0] = bytes / 4;
    program.grid[1] = program.grid[2] = 1;
  }
  Status run(const test::NativeKernel& kernel, MutableBuffer& storage,
             const std::shared_ptr<const CpuStorage>& source = {},
             const CancellationToken& token = {},
             const std::function<void()>& accepted = {}) const {
    ps_kernel_mutable_view_v8 write{};
    write.allocation = storage.data();
    write.bytes = storage.size();
    write.domain = PS_KERNEL_DOMAIN_SHARED_V8;
    write.layout = &output.layout;
    ps_kernel_const_view_v8 read{};
    if (source) {
      read.allocation = source->bytes().data();
      read.bytes = source->bytes().size();
      read.domain = PS_KERNEL_DOMAIN_SHARED_V8;
      read.layout = &input.layout;
    }
    ps_kernel_invocation_v8 invocation{};
    invocation.outputs = &write;
    invocation.output_count = 1;
    invocation.inputs = source ? &read : nullptr;
    invocation.input_count = source ? 1 : 0;
    const std::uint64_t source_identity = 1;
    invocation.input_object_ids = source ? &source_identity : nullptr;
    invocation.cancellation = {
        &token, [](const void* raw) -> std::uint32_t {
          return static_cast<const CancellationToken*>(raw)->cancelled();
        }};
    return kernel.run(invocation, accepted);
  }
};
ResourceLease fill(const ResourceBudget& root, ResourceKind kind) {
  const auto available = root.available_capacity();
  ResourceCapacity capacity;
  if (kind == ResourceKind::Entries)
    capacity[kind] = available[kind];
  else
    capacity = ResourceCapacity::host(
        available[kind] - ResourceBudget::lease_metadata_bytes(),
        kind == ResourceKind::Metadata
            ? available[kind] - ResourceBudget::lease_metadata_bytes()
            : 0);
  return root.reserve(capacity).take_value();
}
int metadata() {
  const std::string shader =
      "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void run(device uint* o [[buffer(0)]]) {o[0]=7;}\n";
  // A cache key larger than a fixed context makes enough metadata reclaimable
  // to admit a new prepared owner when the root is otherwise full.
  Program large(shader + "/*" + std::string(16384, 'x') + "*/", 4);
  Program small(shader, 4);
  for (auto kind :
       {ResourceKind::Host, ResourceKind::Metadata, ResourceKind::Entries}) {
    auto root = std::make_shared<ResourceBudget>();
    ResourceBudget foreign;
    ErrorCode sticky = ErrorCode::Ok;
    ResourceAllocationScope scope(*root, &sticky);
    auto device = Device::create(root);
    PS_CHECK(device);
    auto allocator = device->allocator(BufferAllocator{});
    auto allocation = allocator.allocate(4);
    PS_REQUIRE_OK(allocation);
    auto bytes = allocation.take_value();
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
    device->clear_pipeline_cache();
    PS_CHECK(root->statistics().live.values == initial.values);
    // Exercise both a cache miss and a cache hit under pressure. The former
    // reclaims before building a new key; the latter before context allocation.
    for (bool cache_hit : {false, true}) {
      {
        test::NativeKernel warm;
        PS_REQUIRE_OK(warm.prepare(device, large.program, large.query));
      }
      const auto cached = root->statistics().live;
      PS_CHECK(cached[ResourceKind::Metadata] >
               initial[ResourceKind::Metadata]);
      auto held = fill(*root, kind);
      {
        auto& selected = cache_hit ? large : small;
        test::NativeKernel next;
        PS_REQUIRE_OK(next.prepare(device, selected.program, selected.query));
        PS_REQUIRE_OK(selected.run(next, bytes));
        PS_CHECK(sticky == ErrorCode::Ok);
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data(), 4);
        PS_CHECK(value == 7);
      }
      held = {};
      device->clear_pipeline_cache();
      PS_CHECK(root->statistics().live.values == initial.values);
    }
    // With no reclaimable key, preparation fails finitely and transfers no
    // owner.
    auto held = fill(*root, kind);
    const auto full = root->statistics().live;
    test::NativeKernel rejected;
    auto failure = rejected.prepare(device, small.program, small.query);
    PS_CHECK(failure.code == ErrorCode::ResourceExhausted && rejected.empty());
    PS_CHECK(sticky == ErrorCode::ResourceExhausted);
    PS_CHECK(root->statistics().live.values == full.values);
    held = {};
    sticky = ErrorCode::Ok;
    {
      test::NativeKernel recovered;
      PS_REQUIRE_OK(recovered.prepare(device, small.program, small.query));
      PS_REQUIRE_OK(small.run(recovered, bytes, {}, {}, [&] {
        std::thread cleaner([&] { device->clear_pipeline_cache(); });
        cleaner.join();
      }));
    }
    bytes = {};
    device->collect_expired_allocations();
    device->clear_pipeline_cache();
    for (auto n : root->statistics().live.values)
      PS_CHECK(n == 0);
  }
  ResourceBudget foreign;
  {
    ResourceAllocationScope scope(foreign);
    auto unmanaged = Device::create();
    test::NativeKernel kernel;
    PS_REQUIRE_OK(kernel.prepare(unmanaged, small.program, small.query));
    auto bytes =
        unmanaged->allocator(BufferAllocator{}).allocate(4).take_value();
    PS_REQUIRE_OK(small.run(kernel, bytes));
    for (auto n : foreign.statistics().live.values)
      PS_CHECK(n == 0);
  }
  return 0;
}
int shared_capacity(const std::shared_ptr<Device>& device) {
  constexpr std::uint64_t capacity = 32768;
  for (auto kind : {ResourceKind::Host, ResourceKind::Device,
                    ResourceKind::Shared, ResourceKind::Payload}) {
    ResourceLimits limits;
    limits.capacity[kind] = capacity - 1;
    ResourceBudget root(limits);
    auto rejected = device->allocator(root.allocator()).allocate(16385);
    PS_CHECK(rejected.status().code == ErrorCode::ResourceExhausted);
    for (auto n : root.statistics().live.values)
      PS_CHECK(n == 0);
  }
  for (bool before : {false, true}) {
    ResourceBudget root;
    auto host = root.allocator();
    auto native = before ? device->allocator(host.limited(capacity))
                         : device->allocator(host).limited(capacity);
    const auto baseline = root.statistics();
    PS_CHECK(baseline.live[ResourceKind::Metadata] > 0);
    PS_CHECK(baseline.live[ResourceKind::Host] ==
             baseline.live[ResourceKind::Metadata]);
    auto allocation = native.allocate(16385);
    PS_REQUIRE_OK(allocation);
    auto bytes = allocation.take_value();
    const auto live = root.statistics().live;
    PS_CHECK(live[ResourceKind::Device] == capacity &&
             live[ResourceKind::Shared] == capacity &&
             live[ResourceKind::Payload] == capacity);
    PS_CHECK(live[ResourceKind::Host] ==
             capacity + live[ResourceKind::Metadata]);
    PS_CHECK(live[ResourceKind::Metadata] >
             baseline.live[ResourceKind::Metadata] + sizeof(CpuStorage) +
                 ResourceBudget::lease_metadata_bytes());
    auto view = device->view(bytes.data(), bytes.size(), true);
    PS_REQUIRE_OK(view);
    bytes = {};
    const auto retained = root.statistics();
    PS_CHECK(!native.allocate(1).ok());
    PS_CHECK(root.statistics().live.values == retained.live.values);
    PS_CHECK(root.statistics().allocated_metadata_bytes ==
             retained.allocated_metadata_bytes);
    view = Result<gpu_internal::BufferView>(gpu_internal::BufferView{});
    PS_CHECK(root.statistics().live.values == baseline.live.values);
    PS_CHECK(root.statistics().allocated_metadata_bytes ==
             baseline.allocated_metadata_bytes);
    PS_CHECK(native.allocate(16385).ok());
    PS_CHECK(root.statistics().live.values == baseline.live.values);
    PS_CHECK(root.statistics().allocated_metadata_bytes ==
             baseline.allocated_metadata_bytes);
    native = BufferAllocator{};
    for (auto n : root.statistics().live.values)
      PS_CHECK(n == 0);
    PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
  }
  ResourceBudget root;
  auto host = root.allocator().limited(capacity + 32);
  auto cpu = host.allocate(32).take_value();
  auto native = device->allocator(host);
  auto gpu = native.allocate(16385).take_value();
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == capacity + 32);
  PS_CHECK(!host.allocate(1).ok());
  gpu = {};
  PS_CHECK(host.allocate(1).ok());
  return 0;
}
int shaders(const std::shared_ptr<Device>& device) {
  auto allocator = device->allocator(BufferAllocator{});
  auto bytes = allocator.allocate(16).take_value();
  Program reduction(
      "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void run(device uint* out [[buffer(0)]],"
      "uint i [[thread_index_in_threadgroup]],uint3 g "
      "[[threadgroup_position_in_grid]]) {"
      "threadgroup uint "
      "v[12];v[i]=i+1;threadgroup_barrier(mem_flags::mem_threadgroup);"
      "if(i==0){uint sum=0;for(uint "
      "k=0;k<12;++k)sum+=v[k];out[g.y*2+g.x]=sum;}}");
  reduction.program.grid[0] = 7;
  reduction.program.grid[1] = 5;
  reduction.program.group[0] = 4;
  reduction.program.group[1] = 3;
  reduction.program.group[2] = 1;
  test::NativeKernel kernel;
  PS_REQUIRE_OK(kernel.prepare(device, reduction.program, reduction.query));
  PS_REQUIRE_OK(reduction.run(kernel, bytes));
  for (unsigned i = 0; i < 4; ++i) {
    std::uint32_t sum = 0;
    std::memcpy(&sum, bytes.data() + 4 * i, 4);
    PS_CHECK(sum == 78);
  }
  Program arithmetic(
      "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void run(device const float* a [[buffer(0)]],device float* b "
      "[[buffer(1)]],"
      "uint i [[thread_position_in_grid]]) {b[i]=a[i]*0x1.fffffcp-1f-1.0f;}",
      16, true, ElementType::Float32);
  auto input = allocator.allocate(16).take_value();
  const float operands[4] = {0x1.000002p0F, 1, 1, 1};
  std::memcpy(input.data(), operands, 16);
  auto frozen = std::move(input).freeze();
  PS_CHECK(device->owns(*frozen));
  PS_CHECK(!device->view(frozen->bytes().data(), 16, true).ok());
  PS_CHECK(
      !device->view(reinterpret_cast<const std::uint8_t*>(operands), 16, false)
           .ok());
  PS_REQUIRE_OK(kernel.prepare(device, arithmetic.program, arithmetic.query));
  PS_REQUIRE_OK(arithmetic.run(kernel, bytes, frozen));
  float rounded = 1;
  std::memcpy(&rounded, bytes.data(), 4);
  PS_CHECK(rounded == 0);  // FMA would produce -2^-46.
  Program work(
      "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void run(device uint* o [[buffer(0)]],uint i "
      "[[thread_position_in_grid]]) {"
      "uint v=i;for(uint j=0;j<100000;++j)v=1664525u*v+1013904223u;o[i]=v;}");
  PS_REQUIRE_OK(kernel.prepare(device, work.program, work.query));
  CancellationSource stop;
  PS_REQUIRE_OK(
      work.run(kernel, bytes, {}, stop.token(), [&] { stop.cancel(); }));
  PS_CHECK(stop.token().cancelled());
  for (std::uint32_t i = 0; i < 4; ++i) {
    std::uint32_t expected = i, actual = 0;
    for (unsigned j = 0; j < 100000; ++j)
      expected = 1664525U * expected + 1013904223U;
    std::memcpy(&actual, bytes.data() + 4 * i, 4);
    PS_CHECK(actual == expected);
  }
  PS_CHECK(work.run(kernel, bytes, {}, stop.token()).code ==
           ErrorCode::Cancelled);
  return 0;
}
}  // namespace
int main() {
  using gpu_internal::allocation_capacity;
  PS_CHECK(allocation_capacity(0) == 0 && allocation_capacity(1) == 1);
  PS_CHECK(allocation_capacity(16384) == 16384 &&
           allocation_capacity(16385) == 32768);
  PS_CHECK(allocation_capacity(INT64_MAX - UINT64_C(16384)) ==
           INT64_MAX - UINT64_C(16383));
  PS_CHECK(allocation_capacity(INT64_MAX - UINT64_C(16383)) == 0 &&
           allocation_capacity(UINT64_MAX) == 0);
  auto device = Device::create();
  if (!device || device->backend() != PS_NATIVE_METAL_V2) {
    std::cout << "Metal unavailable\n";
    return 77;
  }
  PS_CHECK(metadata() == 0);
  PS_CHECK(shared_capacity(device) == 0);
  PS_CHECK(native_requested_quota(device) == 0);
  PS_CHECK(native_atlas_budget(device) == 0);
  PS_CHECK(shaders(device) == 0);
  return 0;
}
