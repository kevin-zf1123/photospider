#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "photospider/data/region.hpp"

namespace ps::footprint_internal {

// Canonical axis intervals form contiguous groups. Narrow a group on each
// axis; no allocation and no tensor-domain flattening is required.
template <class Coordinate, class RegionAt, class Tick>
std::size_t containing(std::size_t count, std::size_t rank, Coordinate at,
                       RegionAt region, Tick tick) {
  std::size_t first = 0, last = count;
  for (std::size_t axis = 0; axis < rank; ++axis) {
    auto lo = first, hi = last;
    while (lo < hi) {
      tick();
      const auto mid = lo + (hi - lo) / 2;
      if (region(mid).dimensions()[axis].offset <= at[axis])
        lo = mid + 1;
      else
        hi = mid;
    }
    const auto upper = lo;
    if (upper == first)
      return count;
    const auto d = region(upper - 1).dimensions()[axis];
    if (at[axis] - d.offset >= d.extent)
      return count;
    lo = first;
    hi = upper;
    while (lo < hi) {
      tick();
      const auto mid = lo + (hi - lo) / 2;
      if (region(mid).dimensions()[axis].offset < d.offset)
        lo = mid + 1;
      else
        hi = mid;
    }
    first = lo;
    last = upper;
  }
  return first == last ? count : first;
}

template <class Coordinate>
std::size_t containing(const std::vector<Region>& boxes, Coordinate at,
                       std::size_t rank) {
  return containing(
      boxes.size(), rank, at,
      [&](auto index) -> const Region& { return boxes[index]; }, [] {});
}

}  // namespace ps::footprint_internal
