#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "05-filter/effects.hpp"
#include "05-filter/local_windows.hpp"
#include "05-filter/pyramid_math.hpp"

namespace ps::plugin_internal::filter {
struct OneLevelRegion {
  std::uint64_t first, end, coarse_first, coarse_end;
};
inline OneLevelRegion one_level_region(const Geometry& g, const Region& q) {
  const auto first = q.dimensions().empty() ? 0 : q.dimensions()[g.ya].offset;
  const auto end =
      q.dimensions().empty() ? g.h : first + q.dimensions()[g.ya].extent;
  return {first > 8 ? first - 8 : 0, std::min(g.h, end + 8),
          first / 2 > 2 ? first / 2 - 2 : 0,
          std::min((g.h + 1) / 2, (end + 1) / 2 + 2)};
}
inline std::uint64_t one_level_scratch(const Geometry& g, const Region& q,
                                       ElementType type) {
  const auto r = one_level_region(g, q);
  return bytes(sizeof(generation::Math) + 128 +
               (static_cast<unsigned __int128>(r.end - r.first) * g.w +
                static_cast<unsigned __int128>(r.coarse_end - r.coarse_first) *
                    ((g.w + 1) / 2)) *
                   generation::width(type));
}
// At L=1, reconstruction at p depends only on the remapped detail at p and
// the original coarsest Gaussian plane. Keep the published complete-plane
// dependency, but evaluate its necessary rows and distribute output stripes.
// All remap/reduce/detail/expand/reconstruction RN sites remain unchanged.
template <class Raw>
void local_laplacian_one(const Geometry& g, const Region& selected,
                         double sigma, double alpha, double beta,
                         const Reader& reader,
                         const ps_kernel_invocation_v8& inv,
                         generation::MathArena& arena, const Fuel& fuel) {
  const bool narrow = sizeof(Raw) == 4;
  auto& m = *arena;
  const auto r = one_level_region(g, selected);
  const auto ch = (g.h + 1) / 2, cw = (g.w + 1) / 2;
  auto* source = reinterpret_cast<Raw*>(arena.value + 1);
  auto* coarse = source + (r.end - r.first) * g.w;
  each_plane(inv.outputs[0], g, [&](Coordinate plane) {
    for (std::uint64_t y = r.first; y < r.end; ++y) {
      plane[g.ya] = y;
      for (std::uint64_t x = 0; x < g.w; ++x) {
        fuel.poll();
        plane[g.xa] = x;
        source[(y - r.first) * g.w + x] = reader.raw(0, 0, plane);
      }
    }
    const auto original = [&](std::uint64_t i) -> std::uint64_t {
      return source[i - r.first * g.w];
    };
    for (std::uint64_t y = r.coarse_first; y < r.coarse_end; ++y)
      for (std::uint64_t x = 0; x < cw; ++x)
        coarse[(y - r.coarse_first) * cw + x] = pyramid_math::pyramid_sample(
            original, g.h, g.w, y, x, false, narrow, m, fuel);
    const auto base = [&](std::uint64_t i) -> std::uint64_t {
      return coarse[i - r.coarse_first * cw];
    };
    std::array<Raw, 169> mapped{};
    std::array<Raw, 25> gaussian{};
    std::array<std::uint64_t, 169> mapped_epoch{};
    std::array<std::uint64_t, 25> gaussian_epoch{};
    fuel.poll(388);
    std::uint64_t epoch = 0;
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& view = inv.outputs[o];
      const auto y_end = view.layout->origin[g.ya] + view.layout->shape[g.ya];
      const auto x_end = view.layout->origin[g.xa] + view.layout->shape[g.xa];
      for (std::uint64_t y = view.layout->origin[g.ya]; y < y_end; ++y) {
        plane[g.ya] = y;
        for (std::uint64_t x = view.layout->origin[g.xa]; x < x_end; ++x) {
          ++epoch;
          const Atom guide{original(y * g.w + x), narrow};
          const auto remapped = [&](std::uint64_t i) -> std::uint64_t {
            fuel.poll();
            const auto yy = i / g.w, xx = i % g.w;
            const auto slot = (static_cast<std::int64_t>(yy) -
                               static_cast<std::int64_t>(y) + 6) *
                                  13 +
                              static_cast<std::int64_t>(xx) -
                              static_cast<std::int64_t>(x) + 6;
            if (mapped_epoch[slot] != epoch) {
              mapped[slot] = remap_pixel({original(i), narrow}, guide, sigma,
                                         alpha, beta, m, fuel);
              mapped_epoch[slot] = epoch;
            }
            return mapped[slot];
          };
          const auto reduced = [&](std::uint64_t i) -> std::uint64_t {
            fuel.poll();
            const auto yy = i / cw, xx = i % cw;
            const auto slot = (static_cast<std::int64_t>(yy) -
                               static_cast<std::int64_t>(y / 2) + 2) *
                                  5 +
                              static_cast<std::int64_t>(xx) -
                              static_cast<std::int64_t>(x / 2) + 2;
            if (gaussian_epoch[slot] != epoch) {
              gaussian[slot] = pyramid_math::pyramid_sample(
                  remapped, g.h, g.w, yy, xx, false, narrow, m, fuel);
              gaussian_epoch[slot] = epoch;
            }
            return gaussian[slot];
          };
          const auto expanded = pyramid_math::pyramid_sample(
              reduced, ch, cw, y, x, true, narrow, m, fuel);
          const auto detail = pyramid_math::combine(remapped(y * g.w + x),
                                                    expanded, true, narrow);
          const auto original_expanded = pyramid_math::pyramid_sample(
              base, ch, cw, y, x, true, narrow, m, fuel);
          const auto value =
              pyramid_math::combine(detail, original_expanded, false, narrow);
          fuel.poll();
          plane[g.xa] = x;
          store(view, plane, value);
        }
      }
    }
  });
}
}  // namespace ps::plugin_internal::filter
