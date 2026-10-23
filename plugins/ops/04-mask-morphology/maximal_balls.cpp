#include <algorithm>
#include <cstdint>
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
struct MaximalBalls final : Program {
  Metric metric;
  std::uint64_t height, width, count, padded;
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
    scratch_bytes = bytes(sizeof(Exact) +
                          static_cast<unsigned __int128>(count) *
                              (sizeof(SiteNode) + sizeof(DigitalBall) +
                               sizeof(std::uint64_t) * 2) +
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
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    Visits visits{work};
    const Reader reader{kernel, inv};
    ExactArena arena(inv.scratch[0].allocation, work);
    auto& exact = *arena.p;
    auto* nodes = reinterpret_cast<SiteNode*>(arena.p + 1);
    auto* balls = reinterpret_cast<DigitalBall*>(nodes + count);
    auto* selected = reinterpret_cast<std::uint64_t*>(balls + count);
    auto* retained = selected + count;
    auto* state = reinterpret_cast<std::uint8_t*>(retained + count);
    const auto stride = width + 2;
    lattice_input(reader, 0, state, height, width, work);
    const auto sites = background_tree(state, height, width, nodes, &visits);
    SiteTree tree{nodes, sites, metric, exact, visits};
    tree.build(0, sites);
    std::uint64_t candidates = 0;
    for (std::uint64_t y = 0; y < height; ++y)
      for (std::uint64_t x = 0; x < width; ++x) {
        visits.step();
        if (!state[(y + 1) * stride + x + 1])
          continue;
        const Site center{static_cast<std::int64_t>(y),
                          static_cast<std::int64_t>(x)};
        auto& ball = balls[candidates++];
        ball.center = center;
        ball.delta = nearest_background(tree, center, height, width);
        ball.ry = extent(exact, ball.delta, true, height - 1, &visits);
        ball.rx = extent(exact, ball.delta, false, width - 1, &visits);
      }
    std::sort(balls, balls + candidates,
              [&](const DigitalBall& a, const DigitalBall& b) {
                visits.step();
                const auto cmp = metric.compare(exact, {}, a.delta, b.delta);
                return cmp > 0 || (!cmp && site_less(a.center, b.center));
              });
    std::uint64_t larger = 0, retained_count = 0;
    for (std::uint64_t i = 0; i < candidates; ++i) {
      visits.step();
      auto& candidate = balls[i];
      if (i && metric.compare(exact, {}, candidate.delta, balls[i - 1].delta))
        larger = retained_count;
      bool included = false;
      // Proper inclusion implies strictly greater cardinality, hence a
      // strictly larger metric radius on the translation-invariant lattice.
      // Previously retained balls suffice by transitivity of set inclusion.
      for (std::uint64_t j = 0; j < larger && !included; ++j) {
        visits.step();
        included = contains(exact, balls[retained[j]], candidate, &visits);
      }
      const auto y = static_cast<std::uint64_t>(candidate.center.y);
      const auto x = static_cast<std::uint64_t>(candidate.center.x);
      state[(y + 1) * stride + x + 1] = !included;
      if (!included) {
        selected[y * width + x] = i;
        retained[retained_count++] = i;
      }
    }
    for (unsigned j = 0; j < inv.output_count; ++j) {
      const bool radius = kernel.writes[j].first == 1;
      lattice_output(inv.outputs[j], work,
                     [&](auto y, auto x, bool narrow) -> std::uint64_t {
                       if (!state[(y + 1) * stride + x + 1])
                         return 0;
                       if (!radius)
                         return convert(1, narrow);
                       return metric.rounded_distance(
                           exact, {}, balls[selected[y * width + x]].delta,
                           false, narrow);
                     });
    }
    visits.flush();
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
