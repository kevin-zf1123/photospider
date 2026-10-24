#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "05-filter/effects.hpp"
#include "05-filter/local_laplacian_one.hpp"
#include "05-filter/local_windows.hpp"
#include "05-filter/plane_tasks.hpp"
#include "05-filter/pyramid_math.hpp"

namespace ps::plugin_internal::filter {
namespace {
struct LocalLaplacian final : Program {
  Profile profile;
  ElementType type;
  Geometry geometry;
  Region selected;
  unsigned levels;
  double sigma, alpha, beta;
  std::vector<std::uint64_t> heights, widths, offsets;
  std::uint64_t total = 0;
  static Geometry make_geometry(const OperationMetadata& input,
                                const Parameters& p) {
    auto args = p;
    args["anchor_y"] = INT64_C(2);
    args["anchor_x"] = INT64_C(2);
    args["boundary"] = std::string("reflect_half");
    args["cval"] = 0.0;
    return Geometry(tensor(input, true), args, 5, 5, Kind::BoxSum);
  }
  LocalLaplacian(Profile prof, const std::vector<OperationMetadata>& in,
                 const Parameters& p)
      : profile(prof),
        type(tensor(in[0], true).descriptor.element_type),
        geometry(make_geometry(in[0], p)),
        levels(0),
        sigma(real(p, "edge_threshold")),
        alpha(real(p, "detail_exponent")),
        beta(real(p, "edge_slope")) {
    check(numeric_ops::sequence_profile_available(profile));
    const auto count = integer(p, "levels");
    parameter(count >= 0 && count <= 40, "pyramid level representation");
    levels = static_cast<unsigned>(count);
    auto h = geometry.h, w = geometry.w;
    for (unsigned l = 0; l <= levels; ++l) {
      heights.push_back(h);
      widths.push_back(w);
      offsets.push_back(total);
      total = generation::checked_add(total, size_product({h, w}));
      if (l != levels) {
        parameter(h != 1 || w != 1, "extra local Laplacian level");
        h = (h + 1) / 2;
        w = (w + 1) / 2;
      }
    }
    collective = true;
    const auto pixels = static_cast<unsigned __int128>(geometry.h) * geometry.w;
    scratch_bytes =
        bytes(sizeof(generation::Math) + 128 +
              (3 * static_cast<unsigned __int128>(total) + 2 * pixels) *
                  generation::width(type) +
              static_cast<unsigned __int128>(total) * 8);
    add_output(this, field_schema(in[0], type));
  }
  bool identity() const { return levels == 0 || (alpha == 1 && beta == 1); }
  Region plane_region(const Region& q) const {
    auto d = q.dimensions();
    if (levels == 1) {
      const auto start = (d[geometry.ya].offset / 256) * 256;
      const auto end = std::min(
          geometry.h,
          std::max(start + 256, d[geometry.ya].offset + d[geometry.ya].extent));
      d[geometry.ya] = {start, end - start};
    } else {
      d[geometry.ya] = {0, geometry.h};
    }
    d[geometry.xa] = {0, geometry.w};
    return Region(std::move(d));
  }
  std::uint64_t task_scratch(unsigned, const Region&) const override {
    return levels == 1 ? one_level_scratch(geometry, selected, type)
                       : scratch_bytes;
  }
  std::vector<ReadRegion> support(unsigned, const Region&) const override {
    auto d = selected.dimensions();
    d[geometry.ya] = {0, geometry.h};
    d[geometry.xa] = {0, geometry.w};
    return {{0, 0, Region(std::move(d))}};
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    const Fuel fuel{work};
    const Reader reader{kernel, inv};
    generation::MathArena arena(inv.scratch[0].allocation, work);
    auto& m = *arena;
    m.exact.bind(fuel);
    const bool narrow = type == ElementType::Float32;
    if (levels == 1) {
      if (narrow)
        local_laplacian_one<std::uint32_t>(geometry, selected, sigma, alpha,
                                           beta, reader, inv, arena, fuel);
      else
        local_laplacian_one<std::uint64_t>(geometry, selected, sigma, alpha,
                                           beta, reader, inv, arena, fuel);
      fuel.flush();
      return;
    }
    const auto& g = geometry;
    const auto pixels = g.h * g.w;
    const auto execute = [&](auto raw_type) {
      using Raw = decltype(raw_type);
      auto* stamps = reinterpret_cast<std::uint64_t*>(arena.value + 1);
      auto* original = reinterpret_cast<Raw*>(stamps + total);
      auto* details = original + total;
      auto* memo = details + total;
      auto* first = memo + total;
      auto* second = first + pixels;
      each_plane(inv.outputs[0], g, [&](Coordinate plane) {
        for (std::uint64_t i = 0; i < total;) {
          const auto count = std::min<std::uint64_t>(512, total - i);
          fuel.poll(count);
          std::fill_n(stamps + i, count, UINT64_C(0));
          i += count;
        }
        for (std::uint64_t y = 0; y < g.h; ++y) {
          plane[g.ya] = y;
          for (std::uint64_t x = 0; x < g.w; ++x) {
            fuel.poll();
            plane[g.xa] = x;
            original[y * g.w + x] = reader.raw(0, 0, plane);
          }
        }
        for (unsigned l = 1; l <= levels; ++l)
          for (std::uint64_t y = 0; y < heights[l]; ++y)
            for (std::uint64_t x = 0; x < widths[l]; ++x)
              original[offsets[l] + y * widths[l] + x] =
                  pyramid_math::pyramid_pixel(original + offsets[l - 1],
                                              heights[l - 1], widths[l - 1], y,
                                              x, false, narrow, m, fuel);
        std::uint64_t epoch = 0;
        Atom guide{};
        const auto gaussian = [&](auto&& self, unsigned l, std::uint64_t y,
                                  std::uint64_t x) -> std::uint64_t {
          fuel.poll();
          const auto index = offsets[l] + y * widths[l] + x;
          if (stamps[index] == epoch)
            return memo[index];
          std::uint64_t value;
          if (l == 0) {
            value = remap_pixel({original[index], narrow}, guide, sigma, alpha,
                                beta, m, fuel);
          } else {
            const auto width = widths[l - 1];
            value = pyramid_math::pyramid_sample(
                [&](std::uint64_t i) {
                  return self(self, l - 1, i / width, i % width);
                },
                heights[l - 1], width, y, x, false, narrow, m, fuel);
          }
          memo[index] = value;
          stamps[index] = epoch;
          return value;
        };
        for (unsigned l = 0; l < levels; ++l)
          for (std::uint64_t y = 0; y < heights[l]; ++y)
            for (std::uint64_t x = 0; x < widths[l]; ++x) {
              ++epoch;
              const auto index = offsets[l] + y * widths[l] + x;
              guide = {original[index], narrow};
              const auto center = gaussian(gaussian, l, y, x);
              const auto cw = widths[l + 1];
              const auto expanded = pyramid_math::pyramid_sample(
                  [&](std::uint64_t i) {
                    return gaussian(gaussian, l + 1, i / cw, i % cw);
                  },
                  heights[l + 1], cw, y, x, true, narrow, m, fuel);
              details[index] =
                  pyramid_math::combine(center, expanded, true, narrow);
            }
        auto* current = first;
        auto* next = second;
        const auto base_count = heights[levels] * widths[levels];
        for (std::uint64_t i = 0; i < base_count; ++i) {
          fuel.poll();
          current[i] = original[offsets[levels] + i];
        }
        for (unsigned l = levels; l-- > 0;) {
          const auto width = widths[l];
          for (std::uint64_t y = 0; y < heights[l]; ++y)
            for (std::uint64_t x = 0; x < width; ++x) {
              const auto expanded = pyramid_math::pyramid_pixel(
                  current, heights[l + 1], widths[l + 1], y, x, true, narrow, m,
                  fuel);
              next[y * width + x] = pyramid_math::combine(
                  details[offsets[l] + y * width + x], expanded, false, narrow);
            }
          std::swap(current, next);
        }
        for (unsigned o = 0; o < inv.output_count; ++o) {
          const auto& view = inv.outputs[o];
          for (std::uint64_t y = view.layout->origin[g.ya];
               y < view.layout->origin[g.ya] + view.layout->shape[g.ya]; ++y) {
            plane[g.ya] = y;
            for (std::uint64_t x = view.layout->origin[g.xa];
                 x < view.layout->origin[g.xa] + view.layout->shape[g.xa];
                 ++x) {
              fuel.poll();
              plane[g.xa] = x;
              store(view, plane, current[y * g.w + x]);
            }
          }
        }
      });
    };
    if (narrow)
      execute(std::uint32_t{});
    else
      execute(std::uint64_t{});
    fuel.flush();
  }
};
}  // namespace

OperationDefinition make_local_laplacian(const std::string& key,
                                         Profile profile) {
  auto def = generation::operation(
      key, 1,
      {{"y_axis", Type::Int64},
       {"x_axis", Type::Int64},
       {"levels", Type::Int64},
       {"edge_threshold", Type::Float64},
       {"detail_exponent", Type::Float64},
       {"edge_slope", Type::Float64}},
      {{"output", tensor_schema(ElementType::Float64, {1, 1})}},
      [profile](const auto& in, const auto& p) {
        return std::make_shared<LocalLaplacian>(profile, in, p);
      });
  return planar_operation<LocalLaplacian>(std::move(def));
}
}  // namespace ps::plugin_internal::filter
