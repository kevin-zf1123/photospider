#pragma once

#include <utility>
#include <vector>

#include "05-filter/advanced.hpp"

namespace ps::plugin_internal::filter {
template <class Function>
void each_plane(const ps_kernel_mutable_view_v8& view, const Geometry& g,
                Function function) {
  Coordinate at{};
  std::uint64_t count = 1;
  for (unsigned a = 0; a < view.layout->rank; ++a) {
    at[a] = view.layout->origin[a];
    if (a != g.ya && a != g.xa)
      count *= view.layout->shape[a];
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    function(at);
    for (unsigned a = view.layout->rank; a-- > 0;) {
      if (a == g.ya || a == g.xa)
        continue;
      if (++at[a] < view.layout->origin[a] + view.layout->shape[a])
        break;
      at[a] = view.layout->origin[a];
    }
  }
}
inline Region view_region(const ps_kernel_mutable_view_v8& view) {
  std::vector<RegionDimension> d;
  for (unsigned a = 0; a < view.layout->rank; ++a)
    d.push_back({view.layout->origin[a], view.layout->shape[a]});
  return Region(std::move(d));
}
}  // namespace ps::plugin_internal::filter
