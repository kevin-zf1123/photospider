#pragma once

#include <algorithm>
#include <cstdint>

#include "04-mask-morphology/components.hpp"
#include "04-mask-morphology/distance_metric.hpp"

namespace ps::plugin_internal::mask {
// Binary state has a permanent exterior ring. All storage belongs to the
// invocation's admitted scratch, including exact-distance search structures.
inline void lattice_input(const Reader& reader, unsigned port,
                          std::uint8_t* state, std::uint64_t height,
                          std::uint64_t width, const Work& work) {
  const auto stride = width + 2, count = (height + 2) * stride;
  for (std::uint64_t i = 0; i < count;) {
    const auto n = std::min(UINT64_C(256), count - i);
    work.poll(n);
    std::fill_n(state + i, n, 0);
    i += n;
  }
  const bool narrow =
      reader.kernel.groups[reader.kernel.input_base[port]].type ==
      ElementType::Float32;
  component_read_rows(reader, port, 0, height, width, work,
                      [&](const void* data, std::ptrdiff_t step,
                          std::uint64_t y, std::uint64_t x, std::uint64_t n) {
                        auto* out = state + (y + 1) * stride + x + 1;
                        for (std::uint64_t i = 0; i < n; ++i)
                          out[i] = narrow ? load<float>(data, step, i) != 0
                                          : load<double>(data, step, i) != 0;
                      });
}
inline unsigned lattice_neighbors(const std::uint8_t* state, std::uint64_t p,
                                  std::uint64_t stride) {
  return state[p - stride] | (state[p - stride + 1] << 1) |
         (state[p + 1] << 2) | (state[p + stride + 1] << 3) |
         (state[p + stride] << 4) | (state[p + stride - 1] << 5) |
         (state[p - 1] << 6) | (state[p - stride - 1] << 7);
}
inline Site lattice_site(std::uint64_t p, std::uint64_t stride) {
  return {static_cast<std::int64_t>(p / stride) - 1,
          static_cast<std::int64_t>(p % stride) - 1};
}
inline Site distance_delta(Site p, Site q) {
  return {static_cast<std::int64_t>(separation(p.y, q.y)),
          static_cast<std::int64_t>(separation(p.x, q.x))};
}
inline Site nearest_background(const SiteTree& tree, Site p,
                               std::uint64_t height, std::uint64_t width) {
  // For all three coordinate metrics, an outside minimum occurs at one of
  // the four orthogonal projections onto the one-pixel exterior ring.
  NearestSite best{{-1, p.x}, true};
  for (const Site candidate :
       {Site{static_cast<std::int64_t>(height), p.x}, Site{p.y, -1},
        Site{p.y, static_cast<std::int64_t>(width)}}) {
    tree.visits.step();
    const auto order = tree.metric.compare(tree.exact, p, candidate, best.site);
    if (order < 0 || (!order && site_less(candidate, best.site)))
      best.site = candidate;
  }
  tree.search(p, 0, tree.count, &best);
  return distance_delta(p, best.site);
}
inline std::uint64_t background_tree(const std::uint8_t* state,
                                     std::uint64_t height, std::uint64_t width,
                                     SiteNode* nodes, Visits* visits) {
  std::uint64_t count = 0;
  const auto stride = width + 2;
  for (std::uint64_t y = 0; y < height; ++y)
    for (std::uint64_t x = 0; x < width; ++x) {
      visits->step();
      if (!state[(y + 1) * stride + x + 1])
        nodes[count++].point = {static_cast<std::int64_t>(y),
                                static_cast<std::int64_t>(x)};
    }
  return count;
}
// Exact separable L2 transform for equal spacings. Distances stay as integer
// coordinate deltas, so even subnormal spacing cannot change site ordering.
// The admitted line workspace has height+2 entries, including exterior rows.
struct DistanceParabola final {
  std::int64_t row, dx, begin;
};
inline unsigned __int128 lattice_l2_key(Site delta) {
  const auto y = static_cast<unsigned __int128>(delta.y);
  const auto x = static_cast<unsigned __int128>(delta.x);
  return y * y + x * x;
}
inline void lattice_l2_distances(const std::uint8_t* state,
                                 std::uint64_t height, std::uint64_t width,
                                 Site* distances, DistanceParabola* envelope,
                                 Visits* visits) {
  const auto stride = width + 2;
  for (std::uint64_t y = 0; y < height; ++y) {
    std::int64_t left = -1, right = static_cast<std::int64_t>(width);
    for (std::uint64_t x = 0; x < width; ++x) {
      visits->step();
      if (!state[(y + 1) * stride + x + 1])
        left = static_cast<std::int64_t>(x);
      distances[y * width + x] = {0, static_cast<std::int64_t>(x) - left};
    }
    for (auto x = width; x-- > 0;) {
      visits->step();
      if (!state[(y + 1) * stride + x + 1])
        right = static_cast<std::int64_t>(x);
      auto& dx = distances[y * width + x].x;
      dx = std::min(dx, right - static_cast<std::int64_t>(x));
    }
  }
  for (std::uint64_t x = 0; x < width; ++x) {
    std::uint64_t size = 1;
    envelope[0] = {-1, 0, 0};
    for (std::uint64_t y = 0; y <= height; ++y) {
      visits->step();
      const auto row = static_cast<std::int64_t>(y);
      const auto dx = y == height ? 0 : distances[y * width + x].x;
      __int128 begin = 0;
      while (size) {
        visits->step();
        const auto& old = envelope[size - 1];
        const auto numerator = static_cast<__int128>(row) * row +
                               static_cast<__int128>(dx) * dx -
                               static_cast<__int128>(old.row) * old.row -
                               static_cast<__int128>(old.dx) * old.dx;
        const auto denominator = 2 * static_cast<__int128>(row - old.row);
        // First integer query at which the new parabola is strictly better.
        // floor division is explicit for negative intersections.
        begin =
            (numerator >= 0 ? numerator / denominator
                            : -((-numerator + denominator - 1) / denominator)) +
            1;
        if (begin > old.begin)
          break;
        --size;
      }
      if (!size)
        begin = 0;
      if (begin < static_cast<__int128>(height))
        envelope[size++] = {row, dx, static_cast<std::int64_t>(begin)};
    }
    std::uint64_t active = 0;
    for (std::uint64_t y = 0; y < height; ++y) {
      visits->step();
      while (active + 1 < size &&
             envelope[active + 1].begin <= static_cast<std::int64_t>(y)) {
        visits->step();
        ++active;
      }
      const auto& nearest = envelope[active];
      distances[y * width + x] = {
          static_cast<std::int64_t>(
              separation(static_cast<std::int64_t>(y), nearest.row)),
          nearest.dx};
    }
  }
}
template <class Function>
inline void lattice_output(const ps_kernel_mutable_view_v8& out,
                           const Work& work, Function value) {
  const bool narrow =
      out.layout->element_type == static_cast<unsigned>(ElementType::Float32);
  const auto y0 = out.layout->origin[0], x0 = out.layout->origin[1];
  for (std::uint64_t y = y0; y < y0 + out.layout->shape[0]; ++y)
    for (std::uint64_t x = x0; x < x0 + out.layout->shape[1];) {
      const auto n = std::min(UINT64_C(256), x0 + out.layout->shape[1] - x);
      work.poll(n);
      auto* data = static_cast<std::uint8_t*>(out.allocation) +
                   offset(*out.layout, out.byte_offset, {y, x});
      for (std::uint64_t j = 0; j < n; ++j) {
        const auto raw = value(y, x + j, narrow);
        if (narrow)
          save<std::uint32_t>(data, out.layout->strides[1], j,
                              static_cast<std::uint32_t>(raw));
        else
          save<std::uint64_t>(data, out.layout->strides[1], j, raw);
      }
      x += n;
    }
}
}  // namespace ps::plugin_internal::mask
