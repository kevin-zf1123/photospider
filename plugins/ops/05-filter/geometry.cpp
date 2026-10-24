#include <algorithm>
#include <utility>
#include <vector>

#include "05-filter/common.hpp"

namespace ps::plugin_internal::filter {
Geometry::Geometry(const ResultTensorSpec& input, const Parameters& p,
                   std::uint64_t rows, std::uint64_t columns, Kind kind)
    : kh(rows), kw(columns), cval(real(p, "cval")), dims(input.sample_shape()) {
  const auto y = integer(p, "y_axis"), x = integer(p, "x_axis");
  require(floating(input.descriptor.element_type) && dims.size() >= 2 &&
              dims.size() <= 8,
          "filter input requires Float32/64 rank 2..8");
  parameter(y >= 0 && x >= 0 && y != x &&
                static_cast<std::uint64_t>(std::max(y, x)) < dims.size(),
            "filter spatial axes");
  ya = static_cast<unsigned>(y);
  xa = static_cast<unsigned>(x);
  h = dims[ya];
  w = dims[xa];
  size_product(dims);
  if (kind == Kind::Convolve || kind == Kind::Correlate ||
      kind == Kind::Normalized || kind == Kind::Color) {
    size_product({kh, kw});
  }
  const auto anchor_y =
      kind == Kind::Gaussian ? (kh - 1) / 2 : integer(p, "anchor_y");
  const auto anchor_x =
      kind == Kind::Gaussian ? (kw - 1) / 2 : integer(p, "anchor_x");
  parameter(anchor_y < kh && anchor_x < kw, "filter anchor outside kernel");
  ay = anchor_y;
  ax = anchor_x;
  correlation = kind == Kind::Correlate || kind == Kind::Correlation ||
                kind == Kind::BoxSum || kind == Kind::BoxMean;
  const auto b = text(p, "boundary");
  parameter(b == "constant" || b == "clamp" || b == "wrap" ||
                b == "reflect_half" || b == "reflect_whole" ||
                (b == "truncate" &&
                 (kind == Kind::Normalized || kind == Kind::BoxMean)),
            "filter boundary");
  boundary = b == "constant"        ? Boundary::Constant
             : b == "clamp"         ? Boundary::Clamp
             : b == "wrap"          ? Boundary::Wrap
             : b == "reflect_half"  ? Boundary::Half
             : b == "reflect_whole" ? Boundary::Whole
                                    : Boundary::Truncate;
  const bool shape_parameter =
      kind == Kind::Convolve || kind == Kind::Correlate ||
      kind == Kind::Separable || kind == Kind::Correlation ||
      kind == Kind::Normalized;
  const auto mode = shape_parameter ? text(p, "shape") : "same";
  parameter(mode == "same" || (kind != Kind::Normalized &&
                               (mode == "full" || mode == "valid")),
            "filter shape");
  parameter(mode == "same" || boundary == Boundary::Constant,
            "full/valid require constant boundary");
  // cval=0 for full/valid is a numerical caller premise.
  if (mode == "full") {
    dims[ya] = generation::checked_add(h, kh - 1);
    dims[xa] = generation::checked_add(w, kw - 1);
    oy = correlation
             ? static_cast<std::int64_t>(ay) - static_cast<std::int64_t>(kh - 1)
             : -static_cast<std::int64_t>(ay);
    ox = correlation
             ? static_cast<std::int64_t>(ax) - static_cast<std::int64_t>(kw - 1)
             : -static_cast<std::int64_t>(ax);
  } else if (mode == "valid") {
    parameter(h >= kh && w >= kw, "empty valid filter output");
    dims[ya] = h - kh + 1;
    dims[xa] = w - kw + 1;
    oy = correlation ? ay : kh - 1 - ay;
    ox = correlation ? ax : kw - 1 - ax;
  }
  size_product(dims);
}

std::int64_t Geometry::map(__int128 value, std::uint64_t extent) const {
  if (value >= 0 && value < extent) {
    return static_cast<std::int64_t>(value);
  }
  if (boundary == Boundary::Constant || boundary == Boundary::Truncate) {
    return -1;
  }
  if (boundary == Boundary::Clamp) {
    return value < 0 ? 0 : static_cast<std::int64_t>(extent - 1);
  }
  if (extent == 1) {
    return 0;
  }
  const __int128 period = boundary == Boundary::Wrap   ? extent
                          : boundary == Boundary::Half ? 2 * extent
                                                       : 2 * extent - 2;
  auto t = value % period;
  if (t < 0) {
    t += period;
  }
  if (boundary == Boundary::Wrap || t < extent) {
    return static_cast<std::int64_t>(t);
  }
  return static_cast<std::int64_t>(period -
                                   (boundary == Boundary::Half ? 1 : 0) - t);
}

std::vector<Region> Geometry::support(const Region& q) const {
  const auto axis = [&](unsigned a, std::uint64_t extent, std::uint64_t count,
                        std::uint64_t anchor, std::int64_t origin) {
    std::vector<RegionDimension> result;
    const auto d = q.dimensions()[a];
    const __int128 low =
        __int128(d.offset) + origin +
        (correlation ? -__int128(anchor) : __int128(anchor) - count + 1);
    const __int128 high =
        __int128(d.offset) + d.extent - 1 + origin +
        (correlation ? __int128(count) - 1 - anchor : __int128(anchor));
    if (boundary == Boundary::Constant || boundary == Boundary::Truncate) {
      const auto first = std::max(__int128(0), low);
      const auto last = std::min(__int128(extent - 1), high);
      if (first <= last) {
        result.push_back({static_cast<std::uint64_t>(first),
                          static_cast<std::uint64_t>(last - first + 1)});
      }
      return result;
    }
    if (boundary == Boundary::Clamp) {
      const auto first = map(low, extent), last = map(high, extent);
      result.push_back({static_cast<std::uint64_t>(first),
                        static_cast<std::uint64_t>(last - first + 1)});
      return result;
    }
    if (boundary == Boundary::Wrap) {
      if (high - low + 1 >= extent) {
        return std::vector<RegionDimension>{{0, extent}};
      }
      const auto first = static_cast<std::uint64_t>(map(low, extent));
      const auto length = static_cast<std::uint64_t>(high - low + 1);
      if (first + length <= extent) {
        result.push_back({first, length});
      } else {
        result.push_back({first, extent - first});
        result.push_back({0, length - (extent - first)});
      }
      return result;
    }
    const __int128 step =
        boundary == Boundary::Half ? extent : std::max(UINT64_C(1), extent - 1);
    if (high - low + 1 >= 2 * step) {
      return std::vector<RegionDimension>{{0, extent}};
    }
    auto first = std::min(map(low, extent), map(high, extent));
    auto last = std::max(map(low, extent), map(high, extent));
    auto block = low / step;
    if (low % step < 0) {
      --block;
    }
    for (auto edge = (block + 1) * step; edge <= high; edge += step) {
      first = std::min(first, map(edge, extent));
      last = std::max(last, map(edge, extent));
      first = std::min(first, map(edge - 1, extent));
      last = std::max(last, map(edge - 1, extent));
    }
    result.push_back({static_cast<std::uint64_t>(first),
                      static_cast<std::uint64_t>(last - first + 1)});
    return result;
  };
  std::vector<Region> result;
  for (auto y : axis(ya, h, kh, ay, oy)) {
    for (auto x : axis(xa, w, kw, ax, ox)) {
      auto dimensions = q.dimensions();
      dimensions[ya] = y;
      dimensions[xa] = x;
      result.emplace_back(std::move(dimensions));
    }
  }
  return result;
}

Coordinate Geometry::position(const Coordinate& at, std::uint64_t j,
                              std::uint64_t i, bool* outside) const {
  const auto y = map(__int128(at[ya]) + oy +
                         (correlation ? __int128(j) - ay : __int128(ay) - j),
                     h);
  const auto x = map(__int128(at[xa]) + ox +
                         (correlation ? __int128(i) - ax : __int128(ax) - i),
                     w);
  *outside = y < 0 || x < 0;
  auto result = at;
  if (!*outside) {
    result[ya] = y;
    result[xa] = x;
  }
  return result;
}
}  // namespace ps::plugin_internal::filter
