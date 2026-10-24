#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "05-filter/fields.hpp"

namespace ps::plugin_internal::filter {
// Sliding entries enclose exact sums/products; they never represent a rounded
// tensor stage. Final rounding is certified independently for every output.
template <class Fallback>
void field_window_rows(const Fields& f, const ps_kernel_mutable_view_v8& output,
                       const Reader& reader, const Fuel& fuel, Fast* scratch,
                       unsigned port, Fallback fallback) {
  const auto& g = *f.geometry;
  const auto& layout = *output.layout;
  const auto height = layout.shape[g.ya], width = layout.shape[g.xa];
  const auto rows = height + g.kh - 1, columns = width + g.kw - 1;
  const bool structure = f.kind == FieldKind::Structure;
  const bool covariance = f.kind == FieldKind::Covariance;
  const bool mean = f.kind == FieldKind::Moments && port == 0;
  const unsigned components = structure || mean ? 1 : covariance ? 3 : 2;
  auto* line = scratch + rows * width * components;
  const auto low_y = static_cast<std::int64_t>(layout.origin[g.ya]) -
                     static_cast<std::int64_t>(g.ay);
  const auto low_x = static_cast<std::int64_t>(layout.origin[g.xa]) -
                     static_cast<std::int64_t>(g.ax);
  const bool truncate = g.boundary == Boundary::Truncate;
  const bool narrow = f.type == ElementType::Float32;
  // Reassociation changes enclosure widths; strict final certification keeps
  // the result invariant across tile boundaries and sliding reset points.
  FastArithmetic fast{narrow, Profile::Strict};
  const auto count_axis = [](std::int64_t first, std::uint64_t count,
                             std::uint64_t bound) {
    const auto low = std::max(INT64_C(0), first);
    const auto high = std::min(static_cast<std::int64_t>(bound),
                               first + static_cast<std::int64_t>(count));
    return static_cast<std::uint64_t>(std::max(INT64_C(0), high - low));
  };
  Coordinate plane{};
  std::uint64_t planes = 1;
  for (unsigned a = 0; a < layout.rank; ++a) {
    plane[a] = layout.origin[a];
    if (a != g.ya && a != g.xa)
      planes *= layout.shape[a];
  }
  for (std::uint64_t p = 0; p < planes; ++p) {
    for (std::uint64_t r = 0; r < rows; ++r) {
      const auto my = g.map(__int128(low_y) + r, g.h);
      for (std::uint64_t x = 0; x < columns; ++x) {
        fuel.poll(8);
        const auto mx = g.map(__int128(low_x) + x, g.w);
        const bool outside = my < 0 || mx < 0;
        auto at = plane;
        at[g.ya] = outside ? 0 : my;
        at[g.xa] = outside ? 0 : mx;
        const auto read = [&](unsigned input) {
          return outside ? Fast::point(0)
                         : fast.from(Atom{reader.raw(input, 0, at), narrow});
        };
        const auto a = read(structure && port == 2 ? 1 : 0);
        const auto b = covariance || (structure && port == 1) ? read(1) : a;
        auto* cell = line + x * components;
        if (structure) {
          cell[0] = fast.multiply(a, b);
        } else {
          cell[0] = a;
          if (covariance)
            cell[1] = b;
          if (!mean)
            cell[components - 1] = fast.multiply(a, b);
        }
      }
      std::array<Fast, 3> sum{};
      for (std::uint64_t x = 0; x < width; ++x) {
        if (!x) {
          for (unsigned c = 0; c < components; ++c)
            sum[c] = line[c];
          for (std::uint64_t i = 1; i < g.kw; ++i) {
            fuel.poll(components * 4);
            for (unsigned c = 0; c < components; ++c)
              sum[c] = fast.add(sum[c], line[i * components + c]);
          }
        } else {
          fuel.poll(components * 8);
          for (unsigned c = 0; c < components; ++c) {
            sum[c] = fast.add(sum[c], -line[(x - 1) * components + c]);
            sum[c] = fast.add(sum[c], line[(x + g.kw - 1) * components + c]);
          }
        }
        for (unsigned c = 0; c < components; ++c)
          scratch[(r * width + x) * components + c] = sum[c];
      }
    }
    for (std::uint64_t x = 0; x < width; ++x) {
      std::array<Fast, 3> sum{};
      for (std::uint64_t y = 0; y < height; ++y) {
        if (!y) {
          for (unsigned c = 0; c < components; ++c)
            sum[c] = scratch[x * components + c];
          for (std::uint64_t j = 1; j < g.kh; ++j) {
            fuel.poll(components * 4);
            for (unsigned c = 0; c < components; ++c)
              sum[c] =
                  fast.add(sum[c], scratch[(j * width + x) * components + c]);
          }
        } else {
          fuel.poll(components * 8);
          for (unsigned c = 0; c < components; ++c) {
            sum[c] = fast.add(sum[c],
                              -scratch[((y - 1) * width + x) * components + c]);
            sum[c] = fast.add(
                sum[c], scratch[((y + g.kh - 1) * width + x) * components + c]);
          }
        }
        const auto count = truncate ? count_axis(low_y + y, g.kh, g.h) *
                                          count_axis(low_x + x, g.kw, g.w)
                                    : g.kh * g.kw;
        const auto n = Fast::point(static_cast<double>(count));
        auto numerator = sum[0], denominator = n;
        if (!structure && !mean) {
          fuel.poll(20);
          numerator =
              fast.add(fast.multiply(n, sum[components - 1]),
                       -fast.multiply(sum[0], covariance ? sum[1] : sum[0]));
          denominator = fast.multiply(
              n, Fast::point(static_cast<double>(count - f.ddof)));
        }
        auto result = fast.quotient(numerator, denominator);
        if (!result) {
          const auto value = fast.divide(numerator, denominator);
          // Reassociated exact zeros must use the written expression's signs.
          if (value.nonzero())
            result = fast.finish(value);
        }
        auto at = plane;
        at[g.ya] += y;
        at[g.xa] += x;
        if (!result)
          result = fallback(at);
        store(output, at, *result);
      }
    }
    for (unsigned a = layout.rank; a-- > 0;) {
      if (a == g.ya || a == g.xa)
        continue;
      if (++plane[a] < layout.origin[a] + layout.shape[a])
        break;
      plane[a] = layout.origin[a];
    }
  }
}
}  // namespace ps::plugin_internal::filter
