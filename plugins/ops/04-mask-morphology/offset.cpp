#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "04-mask-morphology/distance_metric.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::mask {
namespace {
struct DiscreteOffset final : Program {
  Metric metric;
  double radius;
  bool erode;
  std::uint64_t height, width, ry, rx;
  ElementType type;
  DiscreteOffset(const std::vector<OperationMetadata>& in, const Parameters& p)
      : metric(p),
        radius(std::abs(real(p, "radius"))),
        erode(real(p, "radius") < 0) {
    const auto& s = image(in[0], 2);
    height = s.sample_shape()[0];
    width = s.sample_shape()[1];
    type = s.descriptor.element_type;
    ry = bounded_radius(radius, metric.sy, height + 1);
    rx = bounded_radius(radius, metric.sx, width + 1);
    add_output(this, spatial_schema(type, {height, width}));
  }
  Region source(const Region& q) const {
    return expanded_region(q, height, width, ry, rx);
  }
  std::vector<ReadRegion> support(unsigned, const Region& q) const override {
    return {{0, 0, source(q)}};
  }
  std::uint64_t task_scratch(unsigned, const Region& q) const override {
    const auto d = source(q).dimensions();
    return bytes(sizeof(Exact) + static_cast<unsigned __int128>(d[0].extent) *
                                     d[1].extent * sizeof(SiteNode));
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    Visits visits{work};
    const Reader reader{kernel, inv};
    const auto& output = inv.outputs[0];
    if (radius == 0) {
      samples(output, [&](const Coordinate& at, std::uint64_t) {
        store(output, at, reader.raw(0, 0, at));
        visits.step();
      });
      visits.flush();
      return;
    }
    ExactArena arena(inv.scratch[0].allocation, work);
    auto& exact = *arena.p;
    auto* nodes = reinterpret_cast<SiteNode*>(arena.p + 1);
    const auto q = invocation_region(*output.layout);
    const auto d = source(q).dimensions(), qd = q.dimensions();
    const Site low{static_cast<std::int64_t>(qd[0].offset),
                   static_cast<std::int64_t>(qd[1].offset)};
    const Site high{static_cast<std::int64_t>(qd[0].offset + qd[0].extent - 1),
                    static_cast<std::int64_t>(qd[1].offset + qd[1].extent - 1)};
    std::uint64_t count = 0;
    for (auto y = d[0].offset; y < d[0].offset + d[0].extent; ++y) {
      for (auto x = d[1].offset; x < d[1].offset + d[1].extent; ++x) {
        const Site p{static_cast<std::int64_t>(y),
                     static_cast<std::int64_t>(x)};
        const Site closest{std::clamp(p.y, low.y, high.y),
                           std::clamp(p.x, low.x, high.x)};
        // A rectangular tile closure does not make its corner samples inputs.
        if (metric.within(exact, closest, p, radius) &&
            ((reader.value(0, 0, {y, x}) != 0) != erode)) {
          nodes[count++].point = p;
        }
        visits.step();
      }
    }
    SiteTree tree{nodes, count, metric, exact, visits};
    tree.build(0, count);
    samples(output, [&](const Coordinate& at, std::uint64_t) {
      const Site p{static_cast<std::int64_t>(at[0]),
                   static_cast<std::int64_t>(at[1])};
      bool hit = false;
      if (erode) {
        const Site outside[]{{-1, p.x},
                             {static_cast<std::int64_t>(height), p.x},
                             {p.y, -1},
                             {p.y, static_cast<std::int64_t>(width)}};
        for (auto site : outside) {
          if (metric.within(exact, p, site, radius)) {
            hit = true;
            break;
          }
        }
      }
      if (!hit) {
        const auto nearest = tree.nearest(p);
        hit = nearest.found && metric.within(exact, p, nearest.site, radius);
      }
      store(output, at,
            convert(erode ? !hit : hit, type == ElementType::Float32));
      visits.step();
    });
    visits.flush();
  }
};
}  // namespace
}  // namespace ps::plugin_internal::mask

namespace ps::plugin_internal {
Status register_mask_offset(OperationRegistry* registry) {
  using namespace mask;  // NOLINT(build/namespaces)
  try {
    check(registry->register_operation(
        definition("offset_discrete", 1,
                   {{"radius", Type::Float64},
                    {"metric", Type::String},
                    {"sy", Type::Float64},
                    {"sx", Type::Float64}},
                   [](const auto& in, const auto& p) {
                     return std::make_shared<DiscreteOffset>(in, p);
                   })));
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
