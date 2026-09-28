#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

#include "execution/native_gpu.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

inline int native_atlas_budget(
    const std::shared_ptr<ps::gpu_internal::Device>& device) {
  using namespace ps;  // NOLINT(build/namespaces)
  const ValueDescriptor descriptor{ElementType::Float32, {8}};
  const Region region({{2, 1}});
  auto writer = MutableValue::allocate(descriptor, region, BufferAllocator{})
                    .take_value();
  const std::uint32_t expected = 0x12345678;
  std::memcpy(writer.data(), &expected, sizeof(expected));
  auto value = std::move(writer).publish().take_value();
  auto coverage = Footprint::from_regions({8}, {region}).take_value();
  auto input =
      ValueFragments::create(descriptor, {}, coverage, {value}).take_value();
  auto plan = FragmentAtlasPlan::prepare(input, {4}).take_value();
  const auto payload =
      device->allocation_capacity(plan.payload_allocation_bytes()).take_value();
  const auto directory =
      device->allocation_capacity(plan.directory_allocation_bytes())
          .take_value();
  for (const auto kind :
       {ResourceKind::Device, ResourceKind::Shared, ResourceKind::Payload}) {
    for (const bool reject : {true, false}) {
      ResourceLimits limits;
      limits.capacity[kind] = payload + directory - reject;
      ResourceBudget root(limits);
      auto allocator = device->allocator(root.allocator());
      {
        auto atlas = plan.materialize(input, allocator);
        PS_CHECK(atlas.ok() == !reject);
        if (reject) {
          PS_CHECK(atlas.status().code == ErrorCode::ResourceExhausted);
          // Payload was allocated; the second allocation failed before its
          // capacity was charged, and unwinding released the first owner.
          PS_CHECK(root.statistics().peak[kind] == payload);
          for (auto live : root.statistics().live.values)
            PS_CHECK(live == 0);
        } else {
          PS_CHECK(root.statistics().live[kind] == payload + directory);
          PS_CHECK(device->owns(*atlas.value().payload.storage()));
          PS_CHECK(device->owns(*atlas.value().directory.storage()));
          auto offset = atlas.value().address({2});
          PS_CHECK(offset.ok());
          std::uint32_t actual = 0;
          std::memcpy(&actual,
                      atlas.value().payload.bytes().data() + offset.value(),
                      sizeof(actual));
          PS_CHECK(actual == expected);
        }
      }
      for (auto live : root.statistics().live.values)
        PS_CHECK(live == 0);
      // Reuse the same allocator/root after the failed two-buffer transaction.
      {
        auto again = allocator.allocate(plan.payload_allocation_bytes());
        PS_CHECK(again.ok() && root.statistics().live[kind] == payload);
      }
      for (auto live : root.statistics().live.values)
        PS_CHECK(live == 0);
    }
  }
  return 0;
}
