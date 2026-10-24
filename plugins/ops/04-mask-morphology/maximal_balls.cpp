#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "04-mask-morphology/lattice.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::mask {
namespace {
struct DigitalBall final {
  Site center, delta;
  std::int64_t ry = 0, rx = 0;
};
template <class Index>
struct BallExtent final {
  Index y, x;
};
struct BallBounds final {
  std::int64_t ymin, xmin, ymax, xmax;
  std::uint64_t head;
};
struct MaximalBalls final : Program {
  static constexpr std::uint64_t cell_extent = 32;
  Metric metric;
  std::uint64_t height, width, count, padded, cells_x, leaves = 1, workspace;
  ElementType radius_type;
  MaximalBalls(const std::vector<OperationMetadata>& in, const Parameters& p)
      : metric(p), radius_type(output_dtype(p)) {
    const auto& s = image(in[0], 2);
    height = s.sample_shape()[0];
    width = s.sample_shape()[1];
    count = size_product({height, width});
    padded = bytes(static_cast<unsigned __int128>(height + 2) * (width + 2));
    parameter(!text(p, "distance_unit").empty(), "empty distance unit");
    collective = true;
    cells_x = (width + cell_extent - 1) / cell_extent;
    const auto cells = ((height + cell_extent - 1) / cell_extent) * cells_x;
    while (leaves < cells)
      leaves *= 2;
    const auto distance_workspace =
        metric.kind == MetricKind::L2 && metric.isotropic
            ? static_cast<unsigned __int128>(height + 2) *
                      sizeof(DistanceParabola) +
                  static_cast<unsigned __int128>(height) *
                      distance_stripe_width * sizeof(Site)
            : static_cast<unsigned __int128>(count) * sizeof(SiteNode);
    const auto filter_workspace =
        static_cast<unsigned __int128>(count) *
            (count <= UINT32_MAX ? sizeof(std::uint32_t)
                                 : sizeof(std::uint64_t)) *
            4 +
        static_cast<unsigned __int128>(leaves) * 2 * sizeof(BallBounds) +
        256 * sizeof(std::uint64_t);
    workspace = bytes(std::max(distance_workspace, filter_workspace));
    scratch_bytes = bytes(sizeof(Exact) + workspace +
                          static_cast<unsigned __int128>(count) * sizeof(Site) +
                          padded + 64);
    add_output(this,
               spatial_schema(s.descriptor.element_type, {height, width}));
    add_output(this, spatial_schema(radius_type, {height, width}));
  }
  std::int64_t extent(Exact& exact, Site radius, bool vertical,
                      std::uint64_t maximum, Visits* visits) const {
    std::uint64_t lo = 0, hi = maximum;
    while (lo < hi) {
      visits->step();
      const auto mid = lo + (hi - lo + 1) / 2;
      const Site delta{vertical ? static_cast<std::int64_t>(mid) : 0,
                       vertical ? 0 : static_cast<std::int64_t>(mid)};
      if (metric.compare(exact, {}, delta, radius) < 0)
        lo = mid;
      else
        hi = mid - 1;
    }
    return static_cast<std::int64_t>(lo);
  }
  std::int64_t row_extent(Exact& exact, const DigitalBall& ball,
                          std::int64_t dy, Visits* visits) const {
    std::int64_t lo = 0, hi = ball.rx;
    while (lo < hi) {
      visits->step();
      const auto mid = lo + (hi - lo + 1) / 2;
      if (metric.compare(exact, {}, {dy < 0 ? -dy : dy, mid}, ball.delta) < 0)
        lo = mid;
      else
        hi = mid - 1;
    }
    return lo;
  }
  bool contains(Exact& exact, const DigitalBall& outer,
                const DigitalBall& inner, Visits* visits) const {
    visits->step();
    const auto dy = separation(inner.center.y, outer.center.y);
    const auto dx = separation(inner.center.x, outer.center.x);
    if (static_cast<std::uint64_t>(inner.ry) + dy >
            static_cast<std::uint64_t>(outer.ry) ||
        static_cast<std::uint64_t>(inner.rx) + dx >
            static_cast<std::uint64_t>(outer.rx))
      return false;
    if (metric.kind == MetricKind::Linf)
      return true;  // The digital ball is exactly its integer rectangle.
    if (metric.kind == MetricKind::L1 && metric.isotropic)
      return dy + dx + static_cast<std::uint64_t>(inner.ry) <=
             static_cast<std::uint64_t>(outer.ry);
    if (metric.kind == MetricKind::L2 && metric.isotropic) {
      const auto a = metric.integer_key({}, outer.delta);
      const auto b = metric.integer_key({}, inner.delta);
      const auto d = static_cast<unsigned __int128>(dy) * dy +
                     static_cast<unsigned __int128>(dx) * dx;
      // A sufficient continuous-disk inclusion is safe for the open digital
      // balls too. Compare the radical inequality entirely in integers, and
      // bound products before taking this shortcut. Failure still falls back
      // to the exact digital row test below.
      constexpr auto bound = static_cast<unsigned __int128>(1) << 62;
      if (a <= bound && b <= bound && d <= bound && a >= b + d) {
        const auto gap = a - b - d;
        if (gap * gap >= 4 * b * d)
          return true;
      }
    }
    // A lattice row is an integer interval. Symmetry and coordinatewise
    // monotonicity reduce both endpoints of both mirrored rows to their
    // farthest endpoint from the outer center. This tests digital inclusion,
    // including equal digital radii, without rounded or continuous disks.
    for (std::int64_t y = 0; y <= inner.ry; ++y) {
      const auto rx = row_extent(exact, inner, y, visits);
      visits->step();
      const Site farthest{static_cast<std::int64_t>(dy) + y,
                          static_cast<std::int64_t>(dx) + rx};
      if (metric.compare(exact, {}, farthest, outer.delta) >= 0)
        return false;
    }
    return true;
  }
  template <class Index>
  DigitalBall ball_at(std::uint64_t index, const Site* distances,
                      const BallExtent<Index>* extents) const {
    return {{static_cast<std::int64_t>(index / width),
             static_cast<std::int64_t>(index % width)},
            distances[index],
            static_cast<std::int64_t>(extents[index].y),
            static_cast<std::int64_t>(extents[index].x)};
  }
  template <class Index>
  bool search(Exact& exact, const Site* distances,
              const BallExtent<Index>* extents, const Index* next,
              const BallBounds* bounds, std::uint64_t node,
              const DigitalBall& inner, Visits* visits) const {
    visits->step();
    const auto& b = bounds[node];
    if (inner.center.y - inner.ry < b.ymin ||
        inner.center.x - inner.rx < b.xmin ||
        inner.center.y + inner.ry > b.ymax ||
        inner.center.x + inner.rx > b.xmax)
      return false;
    if (node < leaves)
      return search(exact, distances, extents, next, bounds, node * 2, inner,
                    visits) ||
             search(exact, distances, extents, next, bounds, node * 2 + 1,
                    inner, visits);
    for (auto i = b.head; i != std::numeric_limits<Index>::max(); i = next[i])
      if (contains(exact, ball_at(i, distances, extents), inner, visits))
        return true;
    return false;
  }
  template <class Index>
  void insert(const DigitalBall& ball, std::uint64_t index, Index* next,
              BallBounds* bounds, Visits* visits) const {
    auto node =
        leaves +
        static_cast<std::uint64_t>(ball.center.y) / cell_extent * cells_x +
        static_cast<std::uint64_t>(ball.center.x) / cell_extent;
    next[index] = static_cast<Index>(bounds[node].head);
    bounds[node].head = index;
    while (node) {
      visits->step();
      auto& b = bounds[node];
      b.ymin = std::min(b.ymin, ball.center.y - ball.ry);
      b.xmin = std::min(b.xmin, ball.center.x - ball.rx);
      b.ymax = std::max(b.ymax, ball.center.y + ball.ry);
      b.xmax = std::max(b.xmax, ball.center.x + ball.rx);
      node /= 2;
    }
  }
  template <class Index>
  void calculate(const Kernel& kernel,
                 const ps_kernel_invocation_v8& inv) const {
    const Work work{inv, work_limit};
    Visits visits{work};
    const Reader reader{kernel, inv};
    ExactArena arena(inv.scratch[0].allocation, work);
    auto& exact = *arena.p;
    auto* memory = reinterpret_cast<std::uint8_t*>(arena.p + 1);
    auto* distances = reinterpret_cast<Site*>(memory + workspace);
    auto* state = reinterpret_cast<std::uint8_t*>(distances + count);
    const auto stride = width + 2;
    lattice_input(reader, 0, state, height, width, work);
    if (metric.kind == MetricKind::L2 && metric.isotropic) {
      lattice_l2_distances(
          state, height, width, distances,
          reinterpret_cast<DistanceParabola*>(memory), &visits,
          reinterpret_cast<Site*>(reinterpret_cast<DistanceParabola*>(memory) +
                                  height + 2));
    } else {
      auto* nodes = reinterpret_cast<SiteNode*>(memory);
      const auto sites = background_tree(state, height, width, nodes, &visits);
      SiteTree tree{nodes, sites, metric, exact, visits};
      tree.build(0, sites);
      for (std::uint64_t y = 0; y < height; ++y)
        for (std::uint64_t x = 0; x < width; ++x) {
          visits.step();
          if (state[(y + 1) * stride + x + 1])
            distances[y * width + x] = nearest_background(
                tree,
                {static_cast<std::int64_t>(y), static_cast<std::int64_t>(x)},
                height, width);
        }
    }
    // Distance search storage is dead. Reuse it for extents and a spatial
    // hierarchy; radii remain indexed by pixel, without a second output map.
    auto* extents = reinterpret_cast<BallExtent<Index>*>(memory);
    auto* order = reinterpret_cast<Index*>(extents + count);
    auto* next = order + count;
    auto* bounds = reinterpret_cast<BallBounds*>(next + count);
    std::uint64_t candidates = 0;
    for (std::uint64_t y = 0; y < height; ++y)
      for (std::uint64_t x = 0; x < width; ++x) {
        visits.step();
        if (!state[(y + 1) * stride + x + 1])
          continue;
        const auto index = y * width + x;
        order[candidates++] = static_cast<Index>(index);
        if (metric.isotropic) {
          const auto key = metric.integer_key({}, distances[index]);
          auto radius = static_cast<std::uint64_t>(key - 1);
          if (metric.kind == MetricKind::L2) {
            radius =
                static_cast<std::uint64_t>(std::sqrt(static_cast<double>(key)));
            while (static_cast<unsigned __int128>(radius) * radius >= key) {
              visits.step();
              --radius;
            }
            while (static_cast<unsigned __int128>(radius + 1) * (radius + 1) <
                   key) {
              visits.step();
              ++radius;
            }
          }
          extents[index] = {static_cast<Index>(std::min(radius, height - 1)),
                            static_cast<Index>(std::min(radius, width - 1))};
        } else {
          extents[index] = {
              static_cast<Index>(
                  extent(exact, distances[index], true, height - 1, &visits)),
              static_cast<Index>(
                  extent(exact, distances[index], false, width - 1, &visits))};
        }
      }
    // Any strictly larger candidate can witness non-maximality. It need not
    // survive: following strictly increasing radii terminates at a maximal
    // ball, and set inclusion is transitive. Keep removed foreground marked
    // as 2 until all local witnesses have been queried.
    std::uint64_t remaining = 0;
    for (std::uint64_t i = 0; i < candidates; ++i) {
      visits.step();
      const auto index = order[i];
      const auto ball = ball_at(index, distances, extents);
      const auto y = index / width, x = index % width;
      bool included = false;
      for (unsigned direction = 0; direction < 4 && !included; ++direction) {
        if ((direction == 0 && !y) || (direction == 1 && y + 1 == height) ||
            (direction == 2 && !x) || (direction == 3 && x + 1 == width))
          continue;
        const auto neighbor = direction == 0   ? index - width
                              : direction == 1 ? index + width
                              : direction == 2 ? index - 1
                                               : index + 1;
        const auto yy = neighbor / width, xx = neighbor % width;
        if (state[(yy + 1) * stride + xx + 1] &&
            metric.compare(exact, {}, distances[neighbor], ball.delta) > 0)
          included = contains(exact, ball_at(neighbor, distances, extents),
                              ball, &visits);
      }
      if (included)
        state[(y + 1) * stride + x + 1] = 2;
      else
        order[remaining++] = static_cast<Index>(index);
    }
    candidates = remaining;
    if (metric.isotropic && candidates >= 256) {
      // Stable radix passes retain raster order within an exact integer key.
      // The future linked-list storage is still dead during sorting.
      auto* histogram = reinterpret_cast<std::uint64_t*>(bounds + 2 * leaves);
      unsigned __int128 maximum = 0;
      for (std::uint64_t i = 0; i < candidates; ++i) {
        visits.step();
        maximum =
            std::max(maximum, metric.integer_key({}, distances[order[i]]));
      }
      auto* source = order;
      auto* target = next;
      for (unsigned shift = 0; shift < 128 && (maximum >> shift); shift += 8) {
        std::fill_n(histogram, 256, 0);
        for (std::uint64_t i = 0; i < candidates; ++i) {
          visits.step();
          const auto key = metric.integer_key({}, distances[source[i]]);
          ++histogram[255 - static_cast<unsigned>((key >> shift) & 255)];
        }
        std::uint64_t total = 0;
        for (unsigned i = 0; i < 256; ++i) {
          const auto n = histogram[i];
          histogram[i] = total;
          total += n;
        }
        for (std::uint64_t i = 0; i < candidates; ++i) {
          visits.step();
          const auto key = metric.integer_key({}, distances[source[i]]);
          const auto digit = 255 - static_cast<unsigned>((key >> shift) & 255);
          target[histogram[digit]++] = source[i];
        }
        std::swap(source, target);
      }
      if (source != order) {
        for (std::uint64_t i = 0; i < candidates; ++i) {
          visits.step();
          order[i] = source[i];
        }
      }
    } else {
      std::sort(order, order + candidates, [&](auto a, auto b) {
        visits.step();
        const auto cmp = metric.compare(exact, {}, distances[a], distances[b]);
        return cmp > 0 || (!cmp && a < b);
      });
    }
    for (std::uint64_t i = 0; i < 2 * leaves; ++i) {
      visits.step();
      bounds[i] = {INT64_MAX, INT64_MAX, INT64_MIN, INT64_MIN,
                   std::numeric_limits<Index>::max()};
    }
    for (std::uint64_t begin = 0; begin < candidates;) {
      auto end = begin + 1;
      while (end < candidates &&
             !metric.compare(exact, {}, distances[order[begin]],
                             distances[order[end]])) {
        visits.step();
        ++end;
      }
      // Proper containment requires a strictly larger metric radius.
      // Delay insertion until the entire equal-radius batch has been tested.
      for (auto i = begin; i < end; ++i) {
        const auto ball = ball_at(order[i], distances, extents);
        const bool included =
            search(exact, distances, extents, next, bounds, 1, ball, &visits);
        const auto y = static_cast<std::uint64_t>(ball.center.y);
        const auto x = static_cast<std::uint64_t>(ball.center.x);
        state[(y + 1) * stride + x + 1] = !included;
      }
      // No query follows the final equal-radius batch. Its survivors need
      // no index insertion; their output state is already final.
      if (end != candidates) {
        for (auto i = begin; i < end; ++i) {
          const auto ball = ball_at(order[i], distances, extents);
          if (state[(ball.center.y + 1) * stride + ball.center.x + 1])
            insert(ball, order[i], next, bounds, &visits);
        }
      }
      begin = end;
    }
    for (unsigned j = 0; j < inv.output_count; ++j) {
      const bool radius = kernel.writes[j].first == 1;
      lattice_output(inv.outputs[j], work,
                     [&](auto y, auto x, bool narrow) -> std::uint64_t {
                       if (state[(y + 1) * stride + x + 1] != 1)
                         return 0;
                       if (!radius)
                         return convert(1, narrow);
                       return metric.rounded_distance(
                           exact, {}, distances[y * width + x], false, narrow);
                     });
    }
    visits.flush();
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    if (count <= UINT32_MAX)
      calculate<std::uint32_t>(kernel, inv);
    else
      calculate<std::uint64_t>(kernel, inv);
  }
};
}  // namespace
}  // namespace ps::plugin_internal::mask

namespace ps::plugin_internal {
Status register_mask_maximal_balls(OperationRegistry* registry) {
  using namespace mask;  // NOLINT(build/namespaces)
  return registry->register_operation(generation::operation(
      "mask.medial_axis_maximal_balls_strict", 1,
      {{"metric", Type::String},
       {"sy", Type::Float64},
       {"sx", Type::Float64},
       {"distance_unit", Type::String},
       {"output_dtype", Type::String}},
      {{"axis", spatial_schema(ElementType::Float64, {1, 1})},
       {"radius", spatial_schema(ElementType::Float64, {1, 1})}},
      [](const auto& in, const auto& p) {
        return std::make_shared<MaximalBalls>(in, p);
      }));
}
}  // namespace ps::plugin_internal
