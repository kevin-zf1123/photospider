#pragma once

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "photospider/core/resource_allocator.hpp"
#include "photospider/data/region.hpp"

namespace ps::execution_internal {
// Public Regions use ordinary vectors. This private bridge admits their
// capacities before growing the vector or transferring a coordinate owner.
struct AccountedRegions final {
  ResourceLease lease;
  std::vector<Region> boxes;
  std::optional<ResourceBudget> budget;
  AccountedRegions() {
    if (const auto* root = resource_internal::metadata_budget())
      budget = *root;
  }
  Status replace_last(const RegionDimension* dimensions, std::size_t count) {
    const auto bytes = count * sizeof(RegionDimension);
    const auto previous = boxes.back().rank() * sizeof(RegionDimension);
    if (budget) {
      auto status = lease.grow(ResourceCapacity::host(bytes, bytes));
      if (!status.ok())
        return status;
    }
    boxes.back() =
        Region(std::vector<RegionDimension>(dimensions, dimensions + count));
    return budget ? lease.shrink(ResourceCapacity::host(previous, previous))
                  : Status::success();
  }
  Status append(const RegionDimension* dimensions, std::size_t count) {
    if (budget) {
      const auto old = boxes.capacity();
      const auto next =
          boxes.size() == old ? std::max<std::size_t>(1, 2 * old) : old;
      const auto bytes = count * sizeof(RegionDimension) +
                         (next == old ? 0 : next * sizeof(Region));
      const auto capacity = ResourceCapacity::host(bytes, bytes);
      if (lease.valid()) {
        auto status = lease.grow(capacity);
        if (!status.ok())
          return status;
      } else {
        auto admitted = budget->reserve(capacity);
        if (!admitted.ok())
          return admitted.status();
        lease = admitted.take_value();
      }
      if (next != old) {
        boxes.reserve(next);
        auto status = lease.shrink(
            ResourceCapacity::host(old * sizeof(Region), old * sizeof(Region)));
        if (!status.ok())
          return status;
      }
    }
    boxes.emplace_back(
        std::vector<RegionDimension>(dimensions, dimensions + count));
    return Status::success();
  }
};
}  // namespace ps::execution_internal
