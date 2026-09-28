#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include "execution/native_gpu.hpp"
#include "photospider/execution/resources.hpp"
#include "support/test_support.hpp"

inline int native_requested_quota(
    const std::shared_ptr<ps::gpu_internal::Device>& device) {
  using namespace ps;  // NOLINT(build/namespaces)
  constexpr std::uint64_t logical = 16385;
  const auto capacity = device->allocation_capacity(logical).value();
  for (const bool before_conversion : {false, true}) {
    ResourceBudget root(ResourceLimits{});
    auto host = root.allocator().limited_requested(2 * logical);
    auto native = before_conversion ? device->allocator(host)
                                    : device->allocator(root.allocator())
                                          .limited_requested(2 * logical);
    auto copied = native;
    auto first = native.allocate(logical).take_value();
    auto second = copied.allocate(logical).take_value();
    PS_CHECK(root.statistics().live[ResourceKind::Device] == 2 * capacity);
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == 2 * capacity);
    PS_CHECK(!native.allocate(1).ok());
    PS_CHECK(native.owns_allocation(first));
    if (before_conversion)
      PS_CHECK(host.owns_allocation(first));
    auto retained = std::move(first).freeze();
    auto alias = retained;
    retained.reset();
    PS_CHECK(!copied.allocate(1).ok());
    alias.reset();
    PS_CHECK(copied.allocate(logical).ok());
    second = MutableBuffer();
    PS_CHECK(root.statistics().live[ResourceKind::Device] == 0);
    // Actual-capacity and requested-size scopes compose in either order.
    auto actual = native.limited(capacity - 1);
    PS_CHECK(!actual.allocate(logical).ok());
    PS_CHECK(native.allocate(logical).ok());
    auto reversed = device->allocator(root.allocator().limited(capacity - 1))
                        .limited_requested(logical);
    PS_CHECK(!reversed.allocate(logical).ok());
    PS_CHECK(reversed.allocate(1).ok());
    for (auto live : root.statistics().live.values)
      PS_CHECK(live == 0);
  }
  // A failed root admission also returns the requested-size quota.
  ResourceLimits limits;
  limits.capacity[ResourceKind::Device] = capacity;
  ResourceBudget root(limits);
  auto native = device->allocator(root.allocator()).limited_requested(logical);
  auto occupied =
      device->allocator(root.allocator()).allocate(logical).take_value();
  PS_CHECK(!native.allocate(1).ok());
  occupied = MutableBuffer();
  PS_CHECK(native.allocate(logical).ok());
  return 0;
}
