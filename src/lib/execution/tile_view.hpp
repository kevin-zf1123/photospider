#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "core/resource_observation.hpp"
#include "photospider/compiler/physical_program.hpp"
#include "photospider/data/tensor_span.hpp"

namespace ps::execution_internal {
// A temporary public-data view of a validated physical layout. Binding and
// frame export use one at a time under the Run's shared scratch reservation.
struct TileView final {
  static constexpr std::uint64_t metadata_bytes =
      8 *
      (sizeof(RegionDimension) + sizeof(std::int64_t) + sizeof(std::uint64_t));

  TileView(const ResourceLease& metadata, const physical::Layout& source,
           bool include_layout, std::uint64_t base_offset = 0)
      : allocation(metadata) {
    std::vector<RegionDimension> dimensions;
    dimensions.reserve(source.rank);
    allocation.commit(dimensions.capacity() * sizeof(RegionDimension));
    for (unsigned axis = 0; axis < source.rank; ++axis)
      dimensions.push_back({source.origin[axis], source.shape[axis]});
    region = Region(std::move(dimensions));
    if (include_layout) {
      layout.byte_offset = base_offset + source.byte_offset;
      layout.byte_strides.assign(source.strides.begin(),
                                 source.strides.begin() + source.rank);
      allocation.commit(layout.byte_strides.capacity() * sizeof(std::int64_t));
      layout.origin.assign(source.origin.begin(),
                           source.origin.begin() + source.rank);
      allocation.commit(layout.origin.capacity() * sizeof(std::uint64_t));
    }
  }

  core_internal::MetadataAllocation allocation;
  Region region;
  StridedLayout layout;
};
}  // namespace ps::execution_internal
