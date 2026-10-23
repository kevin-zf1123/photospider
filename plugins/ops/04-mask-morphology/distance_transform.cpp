#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "04-mask-morphology/distance_metric.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::mask {
namespace {
struct DistanceTransform final : Program {
  Metric metric;
  bool signed_field, truncated, background, exterior, squared;
  double limit = 0;
  std::uint64_t height, width, ry = 0, rx = 0;
  ElementType type;
  DistanceTransform(bool sign, bool local,
                    const std::vector<OperationMetadata>& in,
                    const Parameters& p)
      : metric(p), signed_field(sign), truncated(local) {
    const auto& s = image(in[0], 2);
    height = s.sample_shape()[0];
    width = s.sample_shape()[1];
    type = output_dtype(p);
    const auto mode = text(p, "exterior");
    parameter(mode == "none" || mode == "background", "distance exterior");
    exterior = mode == "background";
    if (!signed_field) {
      const auto feature = text(p, "feature");
      parameter(feature == "foreground" || feature == "background",
                "distance feature");
      background = feature == "background";
      squared = boolean(p, "squared");
      parameter(!squared || metric.kind == MetricKind::L2,
                "squared requires l2");
    } else {
      background = false;
      squared = false;
    }
    if (truncated) {
      limit = real(p, "limit");
      ry = bounded_radius(limit, metric.sy, height + 1);
      rx = bounded_radius(limit, metric.sx, width + 1);
    } else {
      collective = true;
    }
    add_output(this, spatial_schema(type, {height, width}));
    if (truncated) {
      add_output(this, spatial_schema(type, {height, width}));
    } else if (!signed_field) {
      add_output(this, spatial_schema(ElementType::Int64, {2, height, width}));
    }
  }
  Region source_region(const Region& q) const {
    return truncated ? expanded_region(q, height, width, ry, rx)
                     : Region::whole({height, width});
  }
  std::vector<ReadRegion> support(unsigned, const Region& q) const override {
    return {{0, 0, source_region(q)}};
  }
  std::uint64_t task_scratch(unsigned, const Region& q) const override {
    const auto d = source_region(q).dimensions();
    const auto pixels =
        static_cast<unsigned __int128>(d[0].extent) * d[1].extent;
    const auto ring = exterior ? 2 * (d[0].extent + d[1].extent) + 4 : 0;
    return bytes(sizeof(Exact) + (pixels + ring) * sizeof(SiteNode) + pixels +
                 128);
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    Visits visits{work};
    ExactArena arena(inv.scratch[0].allocation, work);
    auto& exact = *arena.p;
    const Reader reader{kernel, inv};
    const auto q =
        truncated ? invocation_region(*inv.outputs[0].layout) : Region{};
    const auto region = source_region(q).dimensions();
    const auto y0 = region[0].offset, x0 = region[1].offset;
    const auto h = region[0].extent, w = region[1].extent;
    const auto pixels = h * w;
    auto* nodes = reinterpret_cast<SiteNode*>(arena.p + 1);
    const auto capacity = pixels + (exterior ? 2 * (h + w) + 4 : 0);
    auto* classification = reinterpret_cast<std::uint8_t*>(nodes + capacity);
    std::uint64_t foreground_count = 0, background_count = 0;
    const auto qdims = truncated ? q.dimensions() : region;
    const Site query_low{static_cast<std::int64_t>(qdims[0].offset),
                         static_cast<std::int64_t>(qdims[1].offset)};
    const Site query_high{
        static_cast<std::int64_t>(qdims[0].offset + qdims[0].extent - 1),
        static_cast<std::int64_t>(qdims[1].offset + qdims[1].extent - 1)};
    // Foreground grows from the front and background from the back.
    for (std::uint64_t y = y0; y < y0 + h; ++y) {
      for (std::uint64_t x = x0; x < x0 + w; ++x) {
        visits.step();
        const Site point{static_cast<std::int64_t>(y),
                         static_cast<std::int64_t>(x)};
        const Site closest{std::clamp(point.y, query_low.y, query_high.y),
                           std::clamp(point.x, query_low.x, query_high.x)};
        if (truncated && !metric.within(exact, point, closest, limit)) {
          continue;
        }
        const bool foreground = reader.value(0, 0, {y, x}) != 0;
        classification[(y - y0) * w + x - x0] = foreground;
        if (signed_field || (foreground != background)) {
          const Site site{static_cast<std::int64_t>(y),
                          static_cast<std::int64_t>(x)};
          if (foreground) {
            nodes[foreground_count++].point = site;
          } else {
            nodes[capacity - ++background_count].point = site;
          }
        }
        visits.step();
      }
    }
    const auto outside = [&](std::int64_t y, std::int64_t x) {
      nodes[capacity - ++background_count].point = {y, x};
      visits.step();
    };
    if (exterior && (signed_field || background)) {
      // Only ring points next to this prepared rectangle can be nearest
      // to its queries; exact limit tests decide whether they participate.
      if (y0 == 0) {
        for (std::int64_t x = static_cast<std::int64_t>(x0) - 1;
             x <= static_cast<std::int64_t>(x0 + w); ++x) {
          outside(-1, x);
        }
      }
      if (y0 + h == height) {
        for (std::int64_t x = static_cast<std::int64_t>(x0) - 1;
             x <= static_cast<std::int64_t>(x0 + w); ++x) {
          outside(height, x);
        }
      }
      if (x0 == 0) {
        for (auto y = y0; y < y0 + h; ++y) {
          outside(y, -1);
        }
      }
      if (x0 + w == width) {
        for (auto y = y0; y < y0 + h; ++y) {
          outside(y, width);
        }
      }
    }
    SiteTree foreground{nodes, foreground_count, metric, exact, visits};
    SiteTree backgrounds{nodes + capacity - background_count, background_count,
                         metric, exact, visits};
    foreground.build(0, foreground_count);
    backgrounds.build(0, background_count);
    const bool narrow = type == ElementType::Float32;
    const auto sign_mask =
        narrow ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);
    std::uint64_t clipped = 0;
    // Numeric limit arithmetic is evaluated only for a selected distance.
    bool needs_distance = false;
    for (const auto& write : kernel.writes) {
      needs_distance |= write.first == 0;
    }
    if (truncated && needs_distance) {
      auto value = exact.binary(bits(limit));
      if (squared) {
        value = exact.multiply(value, value);
      }
      clipped = rounded(exact, value, narrow);
    }
    for (unsigned j = 0; j < inv.output_count; ++j) {
      const auto port = kernel.writes[j].first;
      const auto& output = inv.outputs[j];
      samples(output, [&](const Coordinate& at, std::uint64_t) {
        const auto rank = output.layout->rank;
        const auto y = at[rank - 2], x = at[rank - 1];
        const Site point{static_cast<std::int64_t>(y),
                         static_cast<std::int64_t>(x)};
        const bool negative =
            signed_field && classification[(y - y0) * w + x - x0];
        const auto& tree =
            (signed_field ? negative : background) ? backgrounds : foreground;
        const auto nearest = tree.nearest(point);
        const bool within =
            nearest.found &&
            (!truncated || metric.within(exact, point, nearest.site, limit));
        std::uint64_t result;
        if (port == 1 && !truncated) {
          const auto coordinate = !nearest.found ? INT64_C(-1)
                                  : at[0] == 0   ? nearest.site.y
                                                 : nearest.site.x;
          result = static_cast<std::uint64_t>(coordinate);
        } else if (port == 1) {
          result = convert(within, narrow);
        } else {
          result = within ? metric.rounded_distance(exact, point, nearest.site,
                                                    squared, narrow)
                   : truncated ? clipped
                               : convert(INFINITY, narrow);
          if (negative) {
            result ^= sign_mask;
          }
        }
        store(output, at, result);
        visits.step();
      });
    }
    visits.flush();
  }
};
}  // namespace
}  // namespace ps::plugin_internal::mask

namespace ps::plugin_internal {
Status register_mask_distance_transform(OperationRegistry* registry) {
  using namespace mask;  // NOLINT(build/namespaces)
  try {
    for (unsigned k = 0; k < 4; ++k) {
      const bool sign = k % 2, local = k >= 2;
      const char* names[] = {"nearest_feature", "signed_center_distance",
                             "truncated_nearest_feature",
                             "truncated_signed_distance"};
      std::vector<Parameter> p{{"metric", Type::String},
                               {"sy", Type::Float64},
                               {"sx", Type::Float64},
                               {"output_dtype", Type::String},
                               {"exterior", Type::String}};
      if (!sign) {
        p.push_back({"feature", Type::String});
        p.push_back({"squared", Type::Bool});
      }
      if (local) {
        p.push_back({"limit", Type::Float64});
      }
      std::vector<std::pair<std::string, SchemaTemplate>> outputs{
          {"distance", spatial_schema(ElementType::Float64, {1, 1})}};
      if (local) {
        outputs.emplace_back("within",
                             spatial_schema(ElementType::Float64, {1, 1}));
      } else if (!sign) {
        outputs.emplace_back("nearest",
                             spatial_schema(ElementType::Int64, {2, 1, 1}));
      }
      check(registry->register_operation(generation::operation(
          std::string("mask.") + names[k] + "_strict", 1, std::move(p), outputs,
          [sign, local](const auto& in, const auto& p) {
            return std::make_shared<DistanceTransform>(sign, local, in, p);
          })));
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
