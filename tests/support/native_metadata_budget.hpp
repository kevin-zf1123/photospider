#pragma once

#include <cstring>
#include <memory>
#include <thread>

#include "execution/execution_test_hooks.hpp"
#include "execution/native_gpu.hpp"
#include "photospider/core/resource_allocator.hpp"
#include "support/test_support.hpp"

namespace native_metadata_testing {
inline ps::gpu_internal::Device* submitted_device = nullptr;
inline void clear_submitted() noexcept {
  std::thread worker([] { submitted_device->clear_pipeline_cache(); });
  worker.join();
}
// Reserve the remaining tested dimension, including the reservation's own
// lease overhead. Cache eviction must create room for the next allocation.
inline ps::ResourceLease fill(const ps::ResourceBudget& root,
                              ps::ResourceKind kind) {
  using ps::ResourceKind;
  const auto available = root.available_capacity();
  ps::ResourceCapacity capacity;
  if (kind == ResourceKind::Entries) {
    capacity[kind] = available[kind];
  } else {
    capacity = ps::ResourceCapacity::host(
        available[kind] - ps::ResourceBudget::lease_metadata_bytes(),
        kind == ResourceKind::Metadata
            ? available[kind] - ps::ResourceBudget::lease_metadata_bytes()
            : 0);
  }
  return root.reserve(capacity).take_value();
}
inline int check(ps_gpu_dispatch_v1 dispatch) {
  using namespace ps;  // NOLINT(build/namespaces)
  using gpu_internal::Device;
  using gpu_internal::Invocation;
  for (auto kind :
       {ResourceKind::Host, ResourceKind::Metadata, ResourceKind::Entries}) {
    auto root = std::make_shared<ResourceBudget>();
    ResourceBudget foreign;
    ErrorCode sticky = ErrorCode::Ok;
    ResourceAllocationScope scope(*root, &sticky);
    auto device = Device::create(root);
    PS_CHECK(device);
    auto allocator = device->allocator(BufferAllocator());
    auto bytes = allocator.allocate(4).take_value();
    PS_CHECK(root->statistics().live[ResourceKind::Metadata] > 0);
    const auto owner_live = root->statistics().live;
    {
      ResourceAllocationScope other(foreign);
      auto temporary = allocator.allocate(4).take_value();
      Invocation invocation(device, {});
      std::uint64_t token = 0;
      const auto* api = invocation.service();
      PS_CHECK(api->buffer(api->context, bytes.data(), 4, 1, &token) == 0);
      PS_CHECK(root->statistics().live[ResourceKind::Metadata] >
               owner_live[ResourceKind::Metadata]);
      for (auto n : foreign.statistics().live.values)
        PS_CHECK(n == 0);
    }
    device->collect_expired_allocations();
    PS_CHECK(root->statistics().live.values == owner_live.values);
    for (bool grow_views : {false, true}) {
      Invocation invocation(device, {});
      const auto* api = invocation.service();
      std::uint64_t token = 0;
      PS_CHECK(api->buffer(api->context, bytes.data(), 4, 1, &token) == 0);
      ps_gpu_buffer_binding_v1 binding{sizeof(binding), 0, token, 0, 4, 1};
      dispatch.buffers = &binding;
      dispatch.buffer_count = 1;
      device->clear_pipeline_cache();
      const auto before = root->statistics().live;
      PS_CHECK(api->execute(api->context, &dispatch, 1) == 0);
      const auto cached = root->statistics().live;
      PS_CHECK(cached[ResourceKind::Metadata] > before[ResourceKind::Metadata]);
      // A cache hit borrows the command bytes and retains the cached owner.
      PS_CHECK(api->execute(api->context, &dispatch, 1) == 0);
      PS_CHECK(root->statistics().live.values == cached.values);
      auto held = fill(*root, kind);
      if (grow_views) {
        // The first token stays live, forcing vector growth for the second.
        std::uint64_t next = 0;
        PS_CHECK(api->buffer(api->context, bytes.data(), 4, 1, &next) == 0);
        PS_CHECK(api->release(api->context, next) == 0);
      } else {
        auto next = allocator.allocate(4);
        PS_CHECK(next.ok());
      }
      PS_CHECK(sticky == ErrorCode::Ok && invocation.status().ok());
      held = {};
      device->collect_expired_allocations();
      device->clear_pipeline_cache();
      // With every reclaimable entry gone, failure is finite and sticky.
      auto full = fill(*root, kind);
      auto failed = allocator.allocate(4);
      PS_CHECK(!failed.ok() &&
               failed.status().code == ErrorCode::ResourceExhausted);
      PS_CHECK(sticky == ErrorCode::ResourceExhausted);
      sticky = ErrorCode::Ok;
      const auto full_live = root->statistics().live;
      {
        Invocation rejected(device, {});
        const auto* rejected_api = rejected.service();
        std::uint64_t rejected_token = 0;
        PS_CHECK(rejected_api->buffer(rejected_api->context, bytes.data(), 4, 1,
                                      &rejected_token) != 0);
        PS_CHECK(rejected.status().code == ErrorCode::ResourceExhausted);
        PS_CHECK(sticky == ErrorCode::ResourceExhausted);
      }
      PS_CHECK(root->statistics().live.values == full_live.values);
      sticky = ErrorCode::Ok;
      const auto dispatched = invocation.statistics().dispatches;
      PS_CHECK(api->execute(api->context, &dispatch, 1) != 0);
      PS_CHECK(invocation.status().code == ErrorCode::ResourceExhausted);
      PS_CHECK(sticky == ErrorCode::ResourceExhausted);
      PS_CHECK(invocation.statistics().dispatches == dispatched);
      PS_CHECK(root->statistics().live.values == full_live.values);
      full = {};
      sticky = ErrorCode::Ok;
    }
    // Clear from another thread while the queue is held and work submitted.
    {
      Invocation invocation(device, {});
      const auto* api = invocation.service();
      std::uint64_t token = 0;
      PS_CHECK(api->buffer(api->context, bytes.data(), 4, 1, &token) == 0);
      ps_gpu_buffer_binding_v1 binding{sizeof(binding), 0, token, 0, 4, 1};
      dispatch.buffers = &binding;
      std::memset(bytes.data(), 0, 4);
      execution_testing::ExecutionTestHooks hooks;
      hooks.native_submitted = clear_submitted;
      submitted_device = device.get();
      execution_testing::install_execution_test_hooks(&hooks);
      const auto status = api->execute(api->context, &dispatch, 1);
      execution_testing::install_execution_test_hooks(nullptr);
      submitted_device = nullptr;
      PS_CHECK(status == 0);
      std::uint32_t result = 0;
      std::memcpy(&result, bytes.data(), 4);
      PS_CHECK(result == 7);
    }
    bytes = {};
    device->collect_expired_allocations();
    for (auto n : root->statistics().live.values)
      PS_CHECK(n == 0);
  }
  // An explicitly unmanaged device stays unmanaged even inside a TLS root.
  ResourceBudget foreign;
  {
    ResourceAllocationScope scope(foreign);
    auto unmanaged = Device::create();
    auto bytes =
        unmanaged->allocator(BufferAllocator()).allocate(4).take_value();
    Invocation invocation(unmanaged, {});
    std::uint64_t token = 0;
    const auto* api = invocation.service();
    PS_CHECK(api->buffer(api->context, bytes.data(), 4, 1, &token) == 0);
    for (auto n : foreign.statistics().live.values)
      PS_CHECK(n == 0);
  }
  return 0;
}
}  // namespace native_metadata_testing
