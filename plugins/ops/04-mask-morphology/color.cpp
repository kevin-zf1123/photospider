#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "04-mask-morphology/color_math.hpp"
#include "04-mask-morphology/fast.hpp"
#include "common/registry.hpp"
#include "photospider/ops/mask.hpp"

namespace ps::plugin_internal::mask {
namespace {
enum class ColorKind { Coordinate, Lab76, Hue, Lab2000, Apply };
struct ColorScratch final {
  Math math;
  std::array<Rational, 4> z;
  std::array<Rational, 3> first, second;
  explicit ColorScratch(const Work& work) : math(work) {}
};
struct Color final : Program {
  ColorKind kind;
  Response response;
  ElementType type;
  std::vector<std::uint64_t> selected;
  double center = 0, minimum_chroma = 0;
  bool include_neutral = false;
  std::array<double, 3> factors{1, 1, 1};
  Color(ColorKind k, const std::vector<OperationMetadata>& in,
        const Parameters& p)
      : kind(k), response(p) {
    const auto& s = image(in[0], 3);
    type = s.descriptor.element_type;
    const auto dims = s.sample_shape();
    selected = channels(text(p, "channels"), dims[0]);
    if (k == ColorKind::Lab76 || k == ColorKind::Lab2000)
      require(selected.size() == 3, "Lab selection requires three coordinates");
    if (k == ColorKind::Hue)
      require(selected.size() == 2, "hue selection requires hue and chroma");
    validate_description(
        in[0], p, selected, k == ColorKind::Apply || k == ColorKind::Lab2000,
        k == ColorKind::Lab76 || k == ColorKind::Lab2000, k == ColorKind::Hue);
    if (k == ColorKind::Hue) {
      center = real(p, "center_turns");
      minimum_chroma = real(p, "minimum_chroma");
      const auto policy = text(p, "neutral_policy");
      parameter(policy == "include" || policy == "exclude",
                "unknown neutral policy");
      include_neutral = policy == "include";
    } else if (k == ColorKind::Apply) {
      require(in[1].result_schema != nullptr,
              "missing grouped-color model schema");
      const auto& model = *in[1].result_schema;
      check(model.validate());
      require(model.tensors.size() == 4 &&
                  model.tensors[1].sample_shape().size() == 1,
              "grouped-color model directory");
      const auto expected = take(ps::mask::color_group_model_schema(
          model.tensors[1].sample_shape()[0], selected.size()));
      require(model.same_schema(expected),
              "grouped-color model schema mismatch");
    } else {
      shape(in[1], {selected.size()});
      require(tensor(in[1]).descriptor.element_type == ElementType::Float64,
              "mask target requires Float64");
      if (k == ColorKind::Coordinate) {
        shape(in[2], {selected.size()});
        require(tensor(in[2]).descriptor.element_type == ElementType::Float64,
                "mask scales require Float64");
      }
      if (k == ColorKind::Lab2000)
        factors = {real(p, "kL"), real(p, "kC"), real(p, "kH")};
    }
    scratch_bytes = sizeof(ColorScratch);
    add_output(this, spatial_schema(type, {dims[1], dims[2]}));
  }
  std::vector<ReadRegion> support(unsigned, const Region& q) const override {
    std::vector<ReadRegion> result;
    for (auto c : selected)
      result.push_back({0, 0, channel_region(c, q)});
    for (unsigned p = 1; p < inputs.size(); ++p)
      for (unsigned f = 0; f < inputs[p].result_schema->tensors.size(); ++f)
        result.push_back(
            {p, f,
             Region::whole(
                 inputs[p].result_schema->tensors[f].sample_shape())});
    return result;
  }
  std::optional<std::uint64_t> fast(const Reader& r, const Coordinate& at,
                                    std::uint64_t count,
                                    const Work& work) const {
    if (kind == ColorKind::Hue || kind == ColorKind::Lab2000)
      return {};
    auto distance = Fast::point(0);
    const auto coordinate = [&](unsigned j) {
      return Fast::point(r.value(0, 0, {selected[j], at[0], at[1]}));
    };
    if (kind == ColorKind::Apply) {
      if (!count)
        return 0;
      std::array<Fast, 4> z;
      for (std::uint64_t g = 0; g < count; ++g) {
        work.poll(1);
        auto current = Fast::point(0);
        for (unsigned j = 0; j < selected.size(); ++j) {
          auto value = coordinate(j) - Fast::point(r.value(1, 2, {g, j}));
          for (unsigned c = 0; c < j; ++c)
            value = value - Fast::point(r.value(1, 3, {g, j, c})) * z[c];
          z[j] = value / Fast::point(r.value(1, 3, {g, j, j}));
          current = current + square(z[j]);
        }
        if (!current.finite())
          return {};
        if (!g)
          distance = current;
        else
          distance = {std::min(distance.low, current.low),
                      std::min(distance.high, current.high)};
      }
    } else {
      for (unsigned j = 0; j < selected.size(); ++j) {
        work.poll(1);
        auto diff = coordinate(j) - Fast::point(r.value(1, 0, {j}));
        if (kind == ColorKind::Coordinate)
          diff = diff / Fast::point(r.value(2, 0, {j}));
        else if (!j)
          diff = diff * Fast::point(100);
        distance = distance + square(diff);
      }
    }
    return radial(distance, response.inner, response.outer, response.smooth,
                  type == ElementType::Float32);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    Work work{inv, work_limit};
    auto* scratch = new (inv.scratch[0].allocation) ColorScratch(work);
    struct Cleanup {
      ColorScratch* p;
      ~Cleanup() { p->~ColorScratch(); }
    } cleanup{scratch};
    auto& m = scratch->math;
    const Reader r{k, inv};
    const auto& out = inv.outputs[0];
    const bool narrow = type == ElementType::Float32;
    const auto count = kind == ColorKind::Apply ? r.raw(1, 0, {0}) : 0;
    samples(out, [&](const Coordinate& at, std::uint64_t) {
      work.poll(1);
      if (auto result = fast(r, at, count, work)) {
        store(out, at, *result);
        return;
      }
      m.exact.reset();
      const auto coord = [&](unsigned j) {
        return r.exact(m.exact, 0, 0, {selected[j], at[0], at[1]});
      };
      std::uint64_t result = 0;
      if (kind == ColorKind::Hue) {
        const auto chroma = r.value(0, 0, {selected[1], at[0], at[1]});
        if (chroma == 0 || chroma < minimum_chroma) {
          result = convert(include_neutral, narrow);
        } else {
          const auto delta = m.sub(coord(0), m.value(center));
          const auto half = m.exact.fraction(1, 2);
          const auto wrapped =
              m.sub(m.exact.modulo(m.add(delta, half), 1), half);
          result = response.rational(m, m.exact.absolute(wrapped), narrow);
        }
      } else if (kind == ColorKind::Lab2000) {
        for (unsigned j = 0; j < 3; ++j) {
          scratch->first[j] = coord(j);
          scratch->second[j] = r.exact(m.exact, 1, 0, {j});
        }
        result = ciede2000(m, scratch->first, scratch->second, factors,
                           response, narrow);
      } else if (kind == ColorKind::Apply) {
        Rational minimum = m.exact.integer(0);
        for (std::uint64_t g = 0; g < count; ++g) {
          work.poll(1);
          auto distance = m.exact.integer(0);
          for (unsigned j = 0; j < selected.size(); ++j) {
            auto value = m.sub(coord(j), r.exact(m.exact, 1, 2, {g, j}));
            for (unsigned c = 0; c < j; ++c)
              value = m.sub(value, m.mul(r.exact(m.exact, 1, 3, {g, j, c}),
                                         scratch->z[c]));
            scratch->z[j] = m.div(value, r.exact(m.exact, 1, 3, {g, j, j}));
            distance = m.add(distance, m.sq(scratch->z[j]));
          }
          if (!g || m.exact.order(distance, minimum) < 0)
            minimum = distance;
        }
        if (count)
          result = response.squared(m, minimum, narrow);
      } else {
        auto distance = m.exact.integer(0);
        for (unsigned j = 0; j < selected.size(); ++j) {
          work.poll(1);
          auto diff = m.sub(coord(j), r.exact(m.exact, 1, 0, {j}));
          if (kind == ColorKind::Coordinate)
            diff = m.div(diff, r.exact(m.exact, 2, 0, {j}));
          else if (!j)
            diff = m.scale(diff, 100);
          distance = m.add(distance, m.sq(diff));
        }
        result = response.squared(m, distance, narrow);
      }
      check(m.exact.status());
      store(out, at, result);
    });
  }
};
std::vector<Parameter> color_parameters(bool description_required = false) {
  return {{"channels", Type::String},
          {"input_description", Type::String, description_required},
          {"inner", Type::Float64},
          {"outer", Type::Float64},
          {"curve", Type::String}};
}
}  // namespace
}  // namespace ps::plugin_internal::mask

namespace ps::plugin_internal {
Status register_mask_color(OperationRegistry* registry) {
  using namespace mask;  // NOLINT(build/namespaces)
  const auto add = [&](const char* name, ColorKind kind, unsigned ports,
                       std::vector<Parameter> params) {
    auto op = definition(name, ports, std::move(params),
                         [kind](const auto& in, const auto& p) {
                           return std::make_shared<Color>(kind, in, p);
                         });
    if (kind == ColorKind::Apply) {
      op.traits.input_schema[1].element_type_mask = 0;
      op.traits.input_schema[1].result_schema_id =
          "photospider.ops.mask.color_groups";
      op.traits.input_schema[1].result_schema_version = 1;
    }
    return registry->register_operation(std::move(op));
  };
  for (auto kind : {ColorKind::Coordinate, ColorKind::Lab76, ColorKind::Hue,
                    ColorKind::Lab2000, ColorKind::Apply}) {
    auto p = color_parameters(kind == ColorKind::Apply ||
                              kind == ColorKind::Lab2000);
    const char* name = "coordinate_range";
    unsigned ports = 2;
    switch (kind) {
      case ColorKind::Coordinate:
        ports = 3;
        break;
      case ColorKind::Lab76:
        name = "lab76_range";
        break;
      case ColorKind::Hue:
        name = "hue_range";
        ports = 1;
        p.insert(p.end(), {{"center_turns", Type::Float64},
                           {"minimum_chroma", Type::Float64},
                           {"neutral_policy", Type::String}});
        break;
      case ColorKind::Lab2000:
        name = "lab2000_range";
        p.insert(p.end(), {{"kL", Type::Float64},
                           {"kC", Type::Float64},
                           {"kH", Type::Float64}});
        break;
      case ColorKind::Apply:
        name = "apply_color_groups";
        break;
    }
    auto status = add(name, kind, ports, std::move(p));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
