#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "04-mask-morphology/fast.hpp"

namespace ps::plugin_internal::mask {
struct Visits final {
  const Work& work;
  std::uint64_t pending = 0;
  void step() {
    if (++pending == 256) {
      flush();
    }
  }
  void flush() {
    work.poll(pending);
    pending = 0;
  }
};
inline void distance_description(const Parameters& p) {
  parameter(!text(p, "distance_unit").empty(), "empty distance unit");
  const auto basis = text(p, "distance_basis");
  parameter(basis == "signed_center" || basis == "continuous_sdf" ||
                basis == "level_set",
            "unknown signed distance basis");
}
inline Region invocation_region(const ps_kernel_layout_v8& layout) {
  std::vector<RegionDimension> dimensions;
  for (unsigned axis = 0; axis < layout.rank; ++axis) {
    dimensions.push_back({layout.origin[axis], layout.shape[axis]});
  }
  return Region(std::move(dimensions));
}
inline Region expanded_region(const Region& q, std::uint64_t height,
                              std::uint64_t width, std::uint64_t ry,
                              std::uint64_t rx) {
  const auto dims = spatial_region(q).dimensions();
  const auto expand = [](RegionDimension d, std::uint64_t n, std::uint64_t r) {
    const auto begin = d.offset > r ? d.offset - r : 0;
    const auto end = d.offset + d.extent;
    return RegionDimension{begin, end + std::min(r, n - end) - begin};
  };
  return Region({expand(dims[0], height, ry), expand(dims[1], width, rx)});
}
inline std::uint64_t bounded_radius(double radius, double spacing,
                                    std::uint64_t bound) {
  // Only the intersection with the finite canvas is materialized. Exact
  // comparisons also cover quotients outside the machine floating range.
  Exact e(numeric_ops::SequenceProfile::Strict);
  const auto consume = [](std::uint64_t) { return Status::success(); };
  e.bind(consume);
  const auto r = e.binary(bits(radius)), s = e.binary(bits(spacing));
  std::uint64_t lo = 0, hi = bound;
  while (lo < hi) {
    const auto mid = lo + (hi - lo + 1) / 2;
    if (e.order(e.multiply(e.integer(mid), s), r) <= 0) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  check(e.status());
  return lo;
}
template <class T, class Function>
void unary_rows(const Reader& reader, const ps_kernel_mutable_view_v8& out,
                const Work& work, Function function) {
  const auto y0 = out.layout->origin[0], x0 = out.layout->origin[1];
  const auto height = out.layout->shape[0], width = out.layout->shape[1];
  for (std::uint64_t y = y0; y < y0 + height; ++y) {
    for (std::uint64_t x = x0; x < x0 + width;) {
      const Coordinate at{y, x};
      const auto& source = reader.view(0, 0, at);
      const auto n =
          std::min({UINT64_C(256), x0 + width - x,
                    source.layout->origin[1] + source.layout->shape[1] - x});
      work.poll(n);
      const auto* input = static_cast<const std::uint8_t*>(source.allocation) +
                          offset(*source.layout, source.byte_offset, at);
      auto* output = static_cast<std::uint8_t*>(out.allocation) +
                     offset(*out.layout, out.byte_offset, at);
      function(input, source.layout->strides[1], output, out.layout->strides[1],
               n);
      x += n;
    }
  }
}
}  // namespace ps::plugin_internal::mask
