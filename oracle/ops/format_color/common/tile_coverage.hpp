#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>

#include "photospider/data/result.hpp"

namespace tile_oracle {
// Independent rectangle arithmetic for tests of complete output coverage.
// No production grid or SDK conversion helper is used by this oracle.
inline ps::Region coverage(const ps::SchemaTemplate& schema,
                           const ps::Region& requested,
                           std::uint64_t extent = 128) {
  const auto& tensor = schema.tensors.at(0);
  const auto shape = tensor.sample_shape();
  if (requested.empty())
    return requested;
  if (schema.tensors.size() != 1 || !schema.fields.empty() ||
      !tensor.layout.spatial)
    return ps::Region::whole(shape);
  auto dimensions = ps::Region::whole(shape).dimensions();
  const auto batch = tensor.batch_axes.size();
  for (std::size_t axis = 0; axis < batch; ++axis)
    dimensions[axis] = requested.dimensions()[axis];
  for (auto axis :
       {batch + tensor.layout.height_axis, batch + tensor.layout.width_axis}) {
    const auto requested_axis = requested.dimensions()[axis];
    const auto first = requested_axis.offset / extent * extent;
    const auto last =
        (requested_axis.offset + requested_axis.extent - 1) / extent * extent;
    const auto end = last + std::min(extent, shape[axis] - last);
    dimensions[axis] = {first, end - first};
  }
  return ps::Region(std::move(dimensions));
}
}  // namespace tile_oracle
