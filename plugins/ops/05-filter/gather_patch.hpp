#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "05-filter/effect_fast.hpp"
#include "05-filter/effects.hpp"
#include "05-filter/local_windows.hpp"

namespace ps::plugin_internal::filter {
inline bool gather_patch(const Effect& e) {
  return e.gather() && e.maximum <= 16;
}
inline std::uint64_t gather_patch_bytes(const Effect& e, const Region& q) {
  const auto pad = 2 * static_cast<std::uint64_t>(e.maximum);
  const auto& d = q.dimensions();
  const auto h = static_cast<unsigned __int128>(d[e.geometry->ya].extent) + pad;
  const auto w = static_cast<unsigned __int128>(d[e.geometry->xa].extent) + pad;
  return bytes(h * w * sizeof(std::uint64_t) + h * (w + 1) * sizeof(double) +
               h);
}
struct DiskRowCache {
  struct Entry {
    std::uint64_t key = 0;
    bool valid = false;
    std::array<std::int8_t, 17> reach{};
  };
  std::array<Entry, 16> entries{};
  const Entry& get(Atom radius, generation::Math& m, const Fuel& fuel) {
    const auto key = radius.raw;
    auto& e = entries[(key ^ (key >> 20) ^ (key >> 48)) & 15];
    if (!e.valid || e.key != key) {
      const auto r = static_cast<std::int64_t>(radius.value());
      for (std::int64_t y = 0; y <= r; ++y) {
        auto x = r;
        while (x > 0 && !disk_contains(radius.value(), y, x, m)) {
          fuel.poll();
          --x;
        }
        fuel.poll();
        e.reach[y] = static_cast<std::int8_t>(x);
      }
      e.key = key;
      e.valid = true;
    }
    return e;
  }
};
inline void run_gather_patch(const Effect& e, const Reader& reader,
                             const ps_kernel_mutable_view_v8& view,
                             generation::MathArena& arena, const Fuel& fuel) {
  const auto& g = *e.geometry;
  const bool narrow = e.type == ElementType::Float32;
  const auto pad = static_cast<std::int64_t>(e.maximum);
  const auto height = view.layout->shape[g.ya] + 2 * pad;
  const auto width = view.layout->shape[g.xa] + 2 * pad;
  const auto y0 = static_cast<std::int64_t>(view.layout->origin[g.ya]) - pad;
  const auto x0 = static_cast<std::int64_t>(view.layout->origin[g.xa]) - pad;
  auto* data = reinterpret_cast<std::uint64_t*>(arena.value + 1);
  auto* prefix = reinterpret_cast<double*>(data + height * width);
  auto* row_exact =
      reinterpret_cast<unsigned char*>(prefix + height * (width + 1));
  auto& m = *arena;
  DiskRowCache cache;
  each_plane(view, g, [&](Coordinate at) {
    for (std::uint64_t iy = 0; iy < height; ++iy) {
      const auto yy = g.map(__int128(y0) + iy, g.h);
      for (std::uint64_t ix = 0; ix < width; ++ix) {
        fuel.poll();
        const auto xx = g.map(__int128(x0) + ix, g.w);
        if (yy < 0 || xx < 0) {
          data[iy * width + ix] = 0;
        } else {
          at[g.ya] = yy;
          at[g.xa] = xx;
          data[iy * width + ix] = reader.raw(0, 0, at);
        }
      }
    }
    for (std::uint64_t iy = 0; iy < height; ++iy) {
      auto* row = prefix + iy * (width + 1);
      row[0] = 0;
      bool exact = true;
      for (std::uint64_t ix = 0; ix < width; ++ix) {
        fuel.poll();
        const double v = Atom{data[iy * width + ix], narrow}.value();
        const auto sum = Ball::two_sum(row[ix], v);
        row[ix + 1] = sum.first;
        exact &= sum.second == 0 && std::isfinite(v) &&
                 std::isfinite(sum.first) && !(v == 0 && std::signbit(v));
      }
      row_exact[iy] = exact;
    }
    for (std::uint64_t iy = 0; iy < view.layout->shape[g.ya]; ++iy) {
      at[g.ya] = view.layout->origin[g.ya] + iy;
      for (std::uint64_t ix = 0; ix < view.layout->shape[g.xa]; ++ix) {
        fuel.poll();
        at[g.xa] = view.layout->origin[g.xa] + ix;
        const auto center = (iy + pad) * width + ix + pad;
        const Atom control{reader.raw(1, 0, e.control_position(at)),
                           e.radius_narrow};
        const auto r = static_cast<std::int64_t>(control.value());
        if (control.value() == 0) {
          store(view, at, data[center]);
          continue;
        }
        const auto* rows =
            e.kind == EffectKind::Disk ? &cache.get(control, m, fuel) : nullptr;
        const auto visit = [&](auto consume) {
          for (std::int64_t dy = -r; dy <= r; ++dy) {
            fuel.poll();
            const auto yy = static_cast<std::int64_t>(at[g.ya]) + dy;
            if (g.boundary == Boundary::Truncate &&
                (yy < 0 || static_cast<std::uint64_t>(yy) >= g.h))
              continue;
            const auto reach = rows ? rows->reach[std::abs(dy)] : r;
            for (std::int64_t dx = -reach; dx <= reach; ++dx) {
              fuel.poll();
              const auto xx = static_cast<std::int64_t>(at[g.xa]) + dx;
              if (g.boundary == Boundary::Truncate &&
                  (xx < 0 || static_cast<std::uint64_t>(xx) >= g.w))
                continue;
              const auto i = static_cast<std::int64_t>(center) +
                             dy * static_cast<std::int64_t>(width) + dx;
              consume(Atom{data[i], narrow});
            }
          }
        };
        double sum = 0;
        bool exact = true;
        std::uint64_t count = 0;
        for (std::int64_t dy = -r; dy <= r && exact; ++dy) {
          fuel.poll();
          const auto yy = static_cast<std::int64_t>(at[g.ya]) + dy;
          if (g.boundary == Boundary::Truncate &&
              (yy < 0 || static_cast<std::uint64_t>(yy) >= g.h))
            continue;
          const auto row_index = static_cast<std::int64_t>(iy) + pad + dy;
          if (!row_exact[row_index]) {
            exact = false;
            break;
          }
          const auto reach = rows ? rows->reach[std::abs(dy)] : r;
          std::int64_t lo = -reach, hi = reach;
          if (g.boundary == Boundary::Truncate) {
            lo = std::max(lo, -static_cast<std::int64_t>(at[g.xa]));
            hi = std::min(hi, static_cast<std::int64_t>(g.w - 1 - at[g.xa]));
          }
          const auto* row = prefix + row_index * (width + 1);
          const auto part =
              Ball::two_sum(row[ix + pad + hi + 1], -row[ix + pad + lo]);
          const auto total = Ball::two_sum(sum, part.first);
          exact &= part.second == 0 && total.second == 0;
          sum = total.first;
          count += hi - lo + 1;
        }
        if (!exact) {
          sum = 0;
          count = 0;
          exact = true;
          visit([&](Atom value) {
            const double v = value.value();
            if (count++) {
              const auto addition = Ball::two_sum(sum, v);
              sum = addition.first;
              exact &= addition.second == 0;
            } else {
              sum = v;
            }
            exact &= std::isfinite(v) && std::isfinite(sum);
          });
        }
        FastArithmetic fast{narrow, e.profile};
        std::optional<std::uint64_t> result;
        if (exact) {
          result = sum == 0
                       ? std::optional<std::uint64_t>(
                             generation::convert(sum, narrow))
                       : fast.quotient(Fast::point(sum), Fast::point(count));
        }
        if (!result) {
          auto total = fast.zero();
          bool first = true;
          visit([&](Atom value) {
            const auto v = fast.from(value);
            total = first ? v : fast.add(total, v);
            first = false;
          });
          result = fast.finish(fast.divide(total, Fast::point(count)));
        }
        if (!result) {
          ExactArithmetic arithmetic{m.exact, e.type, {}};
          auto total = arithmetic.zero();
          bool first = true;
          visit([&](Atom value) {
            const auto v = arithmetic.from(value);
            total = first ? v : arithmetic.add(total, v);
            first = false;
          });
          result = arithmetic.finish(
              arithmetic.divide(total, m.exact.integer(count)));
        }
        store(view, at, *result);
      }
    }
  });
}
}  // namespace ps::plugin_internal::filter
