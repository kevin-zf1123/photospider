#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "photospider/data/footprint.hpp"
#include "photospider/execution/resource_allocator.hpp"

namespace ps::neighborhood_internal {
inline Result<Footprint> expand(const ResourceBudget& budget,
                                const Footprint& centers,
                                const ResourceVector<std::uint64_t>& radii,
                                bool periodic, const FootprintLimits& limits) {
  using Answer = Result<Footprint>;
  if (!centers.valid() || radii.size() != centers.shape().size())
    return Answer(
        Status{ErrorCode::InvalidArgument, "invalid neighborhood domain"});
  ResourceAllocationScope scope(budget);
  auto remaining = limits.maximum_work;
  const auto charge = [&](std::uint64_t amount) {
    if (limits.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (amount > remaining)
      return Status{ErrorCode::ResourceExhausted, "neighborhood work limit"};
    remaining -= amount;
    return limits.consume_work ? limits.consume_work(amount)
                               : budget.consume({amount});
  };
  const auto rank = radii.size();
  auto setup = charge(rank + 1);
  if (!setup.ok())
    return Answer(setup);
  using Box = std::array<RegionDimension, 8>;
  ResourceVector<Box> candidates{ResourceAllocator<Box>(budget)};
  for (const auto& box : centers.boxes()) {
    auto status = charge(rank);
    if (!status.ok())
      return Answer(status);
    std::array<std::array<RegionDimension, 2>, 8> spans{};
    std::array<unsigned, 8> counts{};
    std::uint64_t variants = 1;
    for (std::size_t axis = 0; axis < rank; ++axis) {
      const auto d = box.dimensions()[axis];
      const auto n = centers.shape()[axis], radius = radii[axis];
      counts[axis] = 1;
      if (!periodic) {
        const auto first = d.offset > radius ? d.offset - radius : 0;
        const auto end = d.offset + d.extent;
        const auto last = radius >= n - end ? n : end + radius;
        spans[axis][0] = {first, last - first};
      } else if (static_cast<unsigned __int128>(d.extent) +
                     2 * static_cast<unsigned __int128>(radius) >=
                 n) {
        spans[axis][0] = {0, n};
      } else {
        const auto first = static_cast<std::uint64_t>(
            (static_cast<unsigned __int128>(d.offset) + n - radius % n) % n);
        const auto length = d.extent + 2 * radius;
        const auto head = std::min(length, n - first);
        spans[axis][0] = {first, head};
        if (head != length) {
          spans[axis][1] = {0, length - head};
          counts[axis] = 2;
          variants *= 2;
        }
      }
    }
    if (variants > limits.maximum_boxes ||
        candidates.size() > limits.maximum_boxes - variants)
      return Answer(
          Status{ErrorCode::ResourceExhausted, "neighborhood rectangle limit"});
    status = charge(variants * (rank + 1));
    if (!status.ok())
      return Answer(status);
    for (std::uint64_t index = 0; index < variants; ++index) {
      if (candidates.size() == candidates.capacity()) {
        status = charge(candidates.size() * 8 + 1);
        if (!status.ok())
          return Answer(status);
      }
      auto choice = index;
      Box candidate{};
      for (std::size_t axis = 0; axis < rank; ++axis) {
        candidate[axis] = spans[axis][choice % counts[axis]];
        choice /= counts[axis];
      }
      candidates.push_back(candidate);
    }
  }
  auto status = charge(candidates.size() * (rank + 1));
  if (!status.ok())
    return Answer(status);
  const auto bytes =
      candidates.size() * (sizeof(Region) + rank * sizeof(RegionDimension)) +
      rank * 16;
  auto bridge = budget.reserve(ResourceCapacity::host(bytes, bytes));
  if (!bridge.ok())
    return Answer(bridge.status());
  std::vector<Region> boxes;
  boxes.reserve(candidates.size());
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    if (!(index % 64)) {
      auto active = charge(0);
      if (!active.ok())
        return Answer(active);
    }
    const auto& candidate = candidates[index];
    boxes.emplace_back(std::vector<RegionDimension>(candidate.begin(),
                                                    candidate.begin() + rank));
  }
  auto normalized = limits;
  normalized.maximum_work = remaining;
  normalized.consume_work = charge;
  return Footprint::from_regions(centers.shape(), boxes, normalized);
}
}  // namespace ps::neighborhood_internal
