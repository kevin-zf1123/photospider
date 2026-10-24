#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "02-format-color/color_point.hpp"
#include "05-filter/effects.hpp"
#include "05-filter/local_windows.hpp"
#include "05-filter/smaa_math.hpp"
#include "05-filter/smaa_resources/AreaTex.h"
#include "05-filter/smaa_resources/SearchTex.h"

namespace ps::plugin_internal::filter {
namespace smaa {
namespace {
constexpr auto unorm = [] {
  std::array<float, 256> values{};
  for (unsigned i = 0; i < values.size(); ++i)
    values[i] = static_cast<float>(i) / 255.0F;
  return values;
}();
}  // namespace
float area_texel(std::int64_t y, std::int64_t x, unsigned channel) {
  y = std::clamp<std::int64_t>(y, 0, AREATEX_HEIGHT - 1);
  x = std::clamp<std::int64_t>(x, 0, AREATEX_WIDTH - 1);
  const auto i = (y * AREATEX_WIDTH + x) * 2 + channel;
  return unorm[areaTexBytes[i]];
}
float search_texel(std::int64_t y, std::int64_t x) {
  y = std::clamp<std::int64_t>(y, 0, SEARCHTEX_HEIGHT - 1);
  x = std::clamp<std::int64_t>(x, 0, SEARCHTEX_WIDTH - 1);
  return unorm[searchTexBytes[y * SEARCHTEX_WIDTH + x]];
}
}  // namespace smaa
namespace {
struct Rectangle {
  std::int64_t y0, x0, y1, x1;
  std::uint64_t width() const { return x1 - x0; }
  std::uint64_t size() const { return (y1 - y0) * width(); }
  std::uint64_t index(std::int64_t y, std::int64_t x) const {
    return (y - y0) * width() + x - x0;
  }
};
struct Smaa final : Program {
  Geometry geometry;
  color_point::Selection color;
  bool external;
  static Geometry make_geometry(const OperationMetadata& in,
                                const Parameters& p) {
    auto a = p;
    a["anchor_y"] = INT64_C(14);
    a["anchor_x"] = INT64_C(14);
    a["boundary"] = std::string("clamp");
    a["cval"] = 0.0;
    return Geometry(tensor(in, true), a, 29, 29, Kind::BoxMean);
  }
  Smaa(const std::vector<OperationMetadata>& in, const Parameters& p)
      : geometry(make_geometry(in[0], p)), external(false) {
    parameter(text(p, "profile") == "smaa_1x_low_luma_v1",
              "native SMAA profile identity");
    parameter(text(p, "resources") == "iryoku-smaa-71c806a8",
              "native SMAA resource identity");
    const auto guide = text(p, "guide");
    parameter(guide == "internal_luma" || guide == "external_luma",
              "native SMAA guide mode");
    external = guide == "external_luma";
    require(in.size() == (external ? 2U : 1U), "native SMAA input arity");
    require(tensor(in[0], true).descriptor.element_type == ElementType::Float32,
            "native SMAA v1 requires Float32");
    try {
      color = color_point::select(in[0], p);
      require(color.semantic && color.description.has_value(),
              "native SMAA semantic image");
      require(color.interpretation.model == "rgb" ||
                  color.interpretation.model == "gray",
              "native SMAA RGB/Gray");
      if (color.interpretation.model == "rgb") {
        color = color_point::select(in[0], p, {"red", "green", "blue"});
        require(color.interpretation.primaries == "srgb",
                "native SMAA sRGB primaries");
      } else {
        require(color.channels.size() == 1, "native SMAA Gray component");
      }
      require(color.interpretation.transfer == "srgb",
              "native SMAA gamma encoded sRGB transfer");
      require(color.interpretation.association.empty(),
              "native SMAA opaque group association");
      for (const auto& group : color.description->groups)
        if (group.name == text(p, "group"))
          require(!group.alpha, "native SMAA opaque group alpha");
    } catch (const basis_ops::CheckedFailure& e) {
      throw e.status;
    }
    require(color.axis < 0 || (color.axis != static_cast<int>(geometry.ya) &&
                               color.axis != static_cast<int>(geometry.xa)),
            "native SMAA color/spatial axes");
    if (external) {
      const auto& guide_input = tensor(in[1], true);
      auto shape = geometry.dims;
      if (color.axis >= 0)
        shape[color.axis] = 1;
      require(guide_input.descriptor.element_type == ElementType::Float32 &&
                  guide_input.sample_shape() == shape,
              "native SMAA scalar spatial guide");
    }
    add_output(this, field_schema(in[0], ElementType::Float32));
  }
  Rectangle rect(const Region& q, std::int64_t pad, bool extra = false) const {
    const auto& d = q.dimensions();
    const auto y = static_cast<std::int64_t>(d[geometry.ya].offset);
    const auto x = static_cast<std::int64_t>(d[geometry.xa].offset);
    return {std::max<std::int64_t>(0, y - pad),
            std::max<std::int64_t>(0, x - pad),
            std::min<std::int64_t>(geometry.h,
                                   y + d[geometry.ya].extent + pad + extra),
            std::min<std::int64_t>(geometry.w,
                                   x + d[geometry.xa].extent + pad + extra)};
  }
  std::uint64_t task_scratch(unsigned, const Region& q) const override {
    return bytes((static_cast<unsigned __int128>(rect(q, 14).size()) +
                  2 * static_cast<unsigned __int128>(rect(q, 12).size()) +
                  4 * static_cast<unsigned __int128>(rect(q, 0, true).size())) *
                 sizeof(float));
  }
  std::vector<ReadRegion> support(unsigned, const Region& q) const override {
    std::vector<ReadRegion> result{{0, 0, q}};
    for (const auto& r : geometry.support(q)) {
      auto d = r.dimensions();
      if (color.axis >= 0)
        d[color.axis] = {0, external ? 1 : geometry.dims[color.axis]};
      result.push_back({external ? 1U : 0U, 0, Region(std::move(d))});
    }
    if (external) {
      // The final blend samples adjacent source pixels independently of guide.
      auto d = q.dimensions();
      const auto r = rect(q, 1);
      d[geometry.ya] = {static_cast<std::uint64_t>(r.y0),
                        static_cast<std::uint64_t>(r.y1 - r.y0)};
      d[geometry.xa] = {static_cast<std::uint64_t>(r.x0),
                        static_cast<std::uint64_t>(r.x1 - r.x0)};
      result.push_back({0, 0, Region(std::move(d))});
    }
    return result;
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    const Fuel fuel{work};
    const Reader reader{kernel, inv};
    const auto& g = geometry;
    const auto& view = inv.outputs[0];
    const auto q = view_region(view);
    const auto gr = rect(q, 14), er = rect(q, 12), wr = rect(q, 0, true);
    auto* guide_data = static_cast<float*>(inv.scratch[0].allocation);
    auto* edge_data = reinterpret_cast<smaa::Pair*>(guide_data + gr.size());
    auto* weight_data = reinterpret_cast<smaa::Quad*>(edge_data + er.size());
    const auto clamp_y = [&](auto y) {
      return std::clamp<std::int64_t>(y, 0, g.h - 1);
    };
    const auto clamp_x = [&](auto x) {
      return std::clamp<std::int64_t>(x, 0, g.w - 1);
    };
    each_plane(view, g, [&](Coordinate at) {
      if (color.axis >= 0 && at[color.axis] != view.layout->origin[color.axis])
        return;
      const auto read_color = [&](std::int64_t y, std::int64_t x,
                                  std::uint64_t c) {
        fuel.poll();
        auto p = at;
        p[g.ya] = clamp_y(y);
        p[g.xa] = clamp_x(x);
        if (color.axis >= 0)
          p[color.axis] = c;
        return static_cast<float>(Atom{reader.raw(0, 0, p), true}.value());
      };
      for (auto y = gr.y0; y < gr.y1; ++y)
        for (auto x = gr.x0; x < gr.x1; ++x) {
          fuel.poll();
          float value;
          if (external) {
            auto p = at;
            p[g.ya] = y;
            p[g.xa] = x;
            if (color.axis >= 0)
              p[color.axis] = 0;
            value = static_cast<float>(Atom{reader.raw(1, 0, p), true}.value());
          } else if (color.channels.size() == 1) {
            value = read_color(y, x, color.channels[0]);
          } else {
            const float r = read_color(y, x, color.channels[0]) * 0.2126F;
            const float green = read_color(y, x, color.channels[1]) * 0.7152F;
            const float b = read_color(y, x, color.channels[2]) * 0.0722F;
            value = (r + green) + b;
          }
          guide_data[gr.index(y, x)] = value;
        }
      const auto guide = [&](auto y, auto x) {
        fuel.poll();
        return guide_data[gr.index(clamp_y(y), clamp_x(x))];
      };
      for (auto y = er.y0; y < er.y1; ++y) {
        auto x = er.x0;
        for (; x < er.x1 && x < 2; ++x)
          edge_data[er.index(y, x)] = smaa::edges(guide, y, x);
#if defined(__aarch64__)
        if (y >= 2 && y + 1 < static_cast<std::int64_t>(g.h)) {
          const auto stride = gr.width();
          for (; x + 4 <= er.x1 && x + 4 < static_cast<std::int64_t>(g.w);
               x += 4) {
            fuel.poll(28);
            const auto* p = guide_data + gr.index(y, x);
            const auto c = vld1q_f32(p), l = vld1q_f32(p - 1);
            const auto t = vld1q_f32(p - stride);
            const auto dx = vabdq_f32(c, l), dy = vabdq_f32(c, t);
            auto contrast = vmaxq_f32(dx, dy);
            contrast = vmaxq_f32(contrast, vabdq_f32(c, vld1q_f32(p + 1)));
            contrast = vmaxq_f32(contrast, vabdq_f32(c, vld1q_f32(p + stride)));
            contrast = vmaxq_f32(contrast, vabdq_f32(l, vld1q_f32(p - 2)));
            contrast =
                vmaxq_f32(contrast, vabdq_f32(t, vld1q_f32(p - 2 * stride)));
            const auto threshold = vdupq_n_f32(.15F);
            const auto west = vandq_u32(vcgeq_f32(dx, threshold),
                                        vcgeq_f32(vaddq_f32(dx, dx), contrast));
            const auto north =
                vandq_u32(vcgeq_f32(dy, threshold),
                          vcgeq_f32(vaddq_f32(dy, dy), contrast));
            const float32x4x2_t pair = {
                {vcvtq_f32_u32(vshrq_n_u32(west, 31)),
                 vcvtq_f32_u32(vshrq_n_u32(north, 31))}};
            static_assert(sizeof(smaa::Pair) == 2 * sizeof(float));
            vst2q_f32(edge_data[er.index(y, x)].data(), pair);
          }
        }
#endif
        for (; x < er.x1; ++x)
          edge_data[er.index(y, x)] = smaa::edges(guide, y, x);
      }
      const auto edge = [&](auto y, auto x) {
        fuel.poll();
        return edge_data[er.index(clamp_y(y), clamp_x(x))];
      };
      for (auto y = wr.y0; y < wr.y1; ++y)
        for (auto x = wr.x0; x < wr.x1; ++x)
          weight_data[wr.index(y, x)] = smaa::weights(edge, y, x);
      const auto weight = [&](auto y, auto x) {
        fuel.poll();
        return weight_data[wr.index(clamp_y(y), clamp_x(x))];
      };
      const auto qr = rect(q, 0);
      const auto begin = color.axis >= 0 ? view.layout->origin[color.axis] : 0;
      const auto count = color.axis >= 0 ? view.layout->shape[color.axis] : 1;
      for (auto y = qr.y0; y < qr.y1; ++y)
        for (auto x = qr.x0; x < qr.x1; ++x) {
          const auto blend = smaa::blend(weight, y, x);
          at[g.ya] = y;
          at[g.xa] = x;
          for (auto c = begin; c < begin + count; ++c) {
            fuel.poll();
            if (color.axis >= 0)
              at[color.axis] = c;
            const bool selected =
                std::find(color.channels.begin(), color.channels.end(), c) !=
                color.channels.end();
            const auto value =
                !selected || !blend.active
                    ? reader.raw(0, 0, at)
                    : generation::convert(smaa::neighborhood(
                                              [&](auto yy, auto xx) {
                                                return read_color(yy, xx, c);
                                              },
                                              y, x, blend),
                                          true);
            store(view, at, value);
          }
        }
    });
    fuel.flush();
  }
};
}  // namespace
OperationDefinition make_smaa() {
  auto def = generation::operation(
      "filter.smaa_1x_native_strict", 1,
      {{"y_axis", Type::Int64},
       {"x_axis", Type::Int64},
       {"input_description", Type::String},
       {"group", Type::String},
       {"profile", Type::String},
       {"resources", Type::String},
       {"guide", Type::String}},
      {{"output", tensor_schema(ElementType::Float32, {1, 1})}},
      [](const auto& in, const auto& p) {
        return std::make_shared<Smaa>(in, p);
      },
      true);
  def.traits.repeated_maximum = 2;
  return def;
}
}  // namespace ps::plugin_internal::filter
