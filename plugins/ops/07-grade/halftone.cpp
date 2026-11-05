// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "07-grade/artistic_common.hpp"
#include "07-grade/halftone_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade::artistic {
struct Halftone final : Program {
  unsigned kind, axis = 2, yaxis = 0, xaxis = 1, layers = 0;
  ElementType type = ElementType::Float64;
  std::int64_t origin_x = 0, origin_y = 0;
  Image image;
  std::vector<std::uint64_t> dims;
  std::array<halftone::Screen, 4> screens;
  Halftone(unsigned member, const std::vector<OperationMetadata>& in,
           const Parameters& p)
      : kind(member) {
    const auto& source = tensor(in.at(0));
    type = source.descriptor.element_type;
    dims = source.sample_shape();
    require(floating(type), "halftone requires Float32/64");
    origin_x = integer(p, "origin_x");
    origin_y = integer(p, "origin_y");
    auto output = output_like(in[0]);
    if (kind < 2) {
      require(dims.size() == 2, "halftone coverage requires T[H,W]");
      if (source.layout.spatial)
        require(source.layout.height_axis == 0 &&
                    source.layout.width_axis == 1 &&
                    !source.layout.channel_axis,
                "halftone coverage axes");
    } else if (kind == 2) {
      image =
          Image(in[0], p, {"cyan", "magenta", "yellow", "black"}, true, false);
      require(image.selection.semantic
                  ? image.selection.interpretation.model == "cmyk"
                  : text(p, "model") == "cmyk",
              "CMYK screen requires explicit CMYK interpretation");
      for (const auto& unit : image.selection.units)
        require(unit.empty() || unit == "1" || unit == "relative",
                "CMYK screen requires normalized ink units");
      axis = image.axis;
      yaxis = image.yaxis;
      xaxis = image.xaxis;
    } else {
      const auto a = integer(p, "axis", 2);
      require(dims.size() == 3 && a >= 0 && a < 3, "ink image component axis");
      axis = a;
      unsigned k = 0;
      for (unsigned d = 0; d < 3; ++d)
        if (d != axis)
          (k++ == 0 ? yaxis : xaxis) = d;
      require(dims[axis] <= 64, "halftone colorize requires 1..64 inks");
      layers = dims[axis];
      if (source.layout.spatial)
        require(source.layout.height_axis == yaxis &&
                    source.layout.width_axis == xaxis &&
                    source.layout.channel_axis == axis,
                "ink image spatial axes");
      typed(in.at(1), ElementType::Float64, {layers, 3});
      typed(in.at(2), ElementType::Float64, {3});
      output.tensors[0].descriptor.shape[axis] = 3;
      auto destination = in[0];
      destination.result_schema =
          std::make_shared<const SchemaTemplate>(output);
      auto q = p;
      q.erase("axis");
      q["input_description"] = text(p, "output_description");
      try {
        const auto s =
            color_point::select(destination, q, {"red", "green", "blue"});
        require(s.axis == static_cast<int>(axis) &&
                    s.channels == std::vector<std::uint64_t>{0, 1, 2} &&
                    s.interpretation.model == "rgb" &&
                    s.interpretation.transfer == "linear",
                "colorize destination requires complete linear RGB");
        format::RgbBasis assertion;
        assertion.preset = s.interpretation.primaries;
        assertion.primaries_xy = s.interpretation.primaries_xy;
        assertion.white = s.interpretation.white;
        basis_ops::resolve(assertion);
      } catch (const basis_ops::CheckedFailure& e) {
        throw e.status;
      } catch (const basis_ops::GeometryError&) {
        throw Status{ErrorCode::InvalidArgument,
                     "colorize destination RGB basis"};
      }
    }
    if (kind < 3) {
      const auto samples = integer(p, "samples_per_axis", 4);
      require(samples >= 1 && samples <= 16, "halftone samples_per_axis");
      const auto bases =
          reals(p, kind == 2 ? "bases" : "basis", kind == 2 ? 16 : 4);
      const auto origins =
          reals(p, kind == 2 ? "origins" : "origin", kind == 2 ? 8 : 2);
      const auto types =
          kind == 2 ? tokens(text(p, "screen"))
                    : std::vector<std::string>{kind == 0 ? "dot" : "line"};
      require(types.size() == (kind == 2 ? 4 : 1), "CMYK screen count");
      const bool edge = clamping(p, "reject");
      for (unsigned c = 0; c < types.size(); ++c) {
        require(types[c] == "dot" || types[c] == "line",
                "halftone screen type");
        auto& s = screens[c];
        s.samples = samples;
        s.dot = types[c] == "dot";
        s.clamp = edge;
        s.ox = origins[2 * c];
        s.oy = origins[2 * c + 1];
        s.basis(bases.data() + 4 * c);
        s.prepare();
      }
    }
    check(output.validate());
    add_output(this, std::move(output));
  }
  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (empty(r))
      return {};
    if (kind < 3)
      return {{0, 0, r}};
    auto q = r.dimensions();
    q[axis] = {0, layers};
    return {{0, 0, Region(std::move(q))},
            {1, 0, whole({layers, 3})},
            {2, 0, whole({3})}};
  }
  template <class T, unsigned Kind>
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    constexpr unsigned kind = Kind;
    const mask::Reader reader{k, inv};
    std::array<double, 64 * 3> colors{};
    std::array<double, 3> background{};
    if (kind == 3 && inv.output_count) {
      work.poll(layers * 3 + 3);
      for (unsigned c = 0; c < 3; ++c) {
        background[c] = reader.value(2, 0, Coordinate{c});
        for (unsigned j = 0; j < layers; ++j)
          colors[j * 3 + c] = reader.value(1, 0, Coordinate{j, c});
      }
    }
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& out = inv.outputs[o];
      work.poll(mask::bytes(
          static_cast<unsigned __int128>(count(*out.layout)) *
          (kind == 3 ? layers * 8
                     : screens[0].samples * screens[0].samples * 64)));
      if (kind < 2) {
        const auto& l = *out.layout;
        for (std::uint64_t y = l.origin[0]; y < l.origin[0] + l.shape[0]; ++y) {
          Coordinate at{y, l.origin[1]};
          while (at[1] < l.origin[1] + l.shape[1]) {
            cancel(inv);
            std::uint64_t n =
                std::min<std::uint64_t>(1024, l.origin[1] + l.shape[1] - at[1]);
            const auto src = span(reader, 0, at, 1, &n);
            const __int128 x = static_cast<__int128>(origin_x) + at[1];
            const __int128 yy = static_cast<__int128>(origin_y) + y;
            if (src.stride == sizeof(T) && l.strides[1] == sizeof(T) &&
                aligned(reinterpret_cast<const T*>(src.data)) &&
                aligned(address<T>(out, at))) {
              screens[0].row(reinterpret_cast<const T*>(src.data),
                             address<T>(out, at), n, x, yy);
            } else {
              for (std::uint64_t j = 0; j < n; ++j)
                statistics::save(
                    out, at, 1, j,
                    convert(screens[0].apply(src.value(j), x + j, yy),
                            sizeof(T) == 4));
            }
            at[1] += n;
          }
        }
      } else if (kind == 2) {
        image_runs<T>(
            image, reader, inv, out,
            [&](const T* src, T* dst, std::uint64_t n, const Coordinate& q) {
              const auto channels = dims[axis];
              std::memcpy(dst, src, n * channels * sizeof(T));
              for (unsigned c = 0; c < 4; ++c)
                screens[c].row(src + image.selected[c], dst + image.selected[c],
                               n, static_cast<__int128>(origin_x) + q[xaxis],
                               static_cast<__int128>(origin_y) + q[yaxis],
                               channels);
              return true;
            },
            [&](Coordinate q, std::uint64_t j) {
              if ((j & 127U) == 0)
                cancel(inv);
              for (q[axis] = out.layout->origin[axis];
                   q[axis] < out.layout->origin[axis] + out.layout->shape[axis];
                   ++q[axis]) {
                const auto c = image.component(q[axis]);
                store(out, q,
                      c < 0
                          ? reader.raw(0, 0, q)
                          : convert(
                                screens[c].apply(
                                    reader.value(0, 0, q),
                                    static_cast<__int128>(origin_x) + q[xaxis],
                                    static_cast<__int128>(origin_y) + q[yaxis]),
                                sizeof(T) == 4));
              }
            });
      } else {
        const auto& l = *out.layout;
        if (axis == 2 && l.origin[2] == 0 && l.shape[2] == 3 &&
            l.strides[2] == sizeof(T) && l.strides[1] == 3 * sizeof(T)) {
          bool direct = true;
          for (const auto& group : k.groups)
            if (group.port == 0) {
              for (const auto& slot : group.slots) {
                const auto& v = inv.inputs[slot.second];
                direct &= v.layout->origin[2] == 0 &&
                          v.layout->shape[2] == layers &&
                          v.layout->strides[2] == sizeof(T) &&
                          v.layout->strides[1] == layers * sizeof(T);
              }
            }
          if (direct) {
            for (std::uint64_t y = l.origin[0]; y < l.origin[0] + l.shape[0];
                 ++y) {
              Coordinate q{y, l.origin[1], 0};
              while (q[1] < l.origin[1] + l.shape[1]) {
                cancel(inv);
                const auto& v = reader.view(0, 0, q);
                const auto n = std::min<std::uint64_t>(
                    {1024, l.origin[1] + l.shape[1] - q[1],
                     v.layout->origin[1] + v.layout->shape[1] - q[1]});
                if (!aligned(address<T>(v, q)) ||
                    !aligned(address<T>(out, q))) {
                  direct = false;
                  break;
                }
                halftone::colorize(address<T>(v, q), address<T>(out, q), n,
                                   layers, colors.data(), background.data());
                q[1] += n;
              }
              if (!direct)
                break;
            }
            if (direct)
              continue;
          }
        }
        pixels(out, axis, [&](Coordinate q, std::uint64_t j) {
          if ((j & 127U) == 0)
            cancel(inv);
          auto value = background;
          auto src = q;
          for (unsigned layer = 0; layer < layers; ++layer) {
            src[axis] = layer;
            const double a = reader.value(0, 0, src);
            if (a == 0)
              continue;
            for (unsigned c = 0; c < 3; ++c)
              value[c] = a == 1
                             ? colors[3 * layer + c]
                             : (1 - a) * value[c] + a * colors[3 * layer + c];
          }
          for (q[axis] = out.layout->origin[axis];
               q[axis] < out.layout->origin[axis] + out.layout->shape[axis];
               ++q[axis])
            store(out, q, convert(value[q[axis]], sizeof(T) == 4));
        });
      }
    }
    cancel(inv);
  }
  template <class T>
  void dispatch(const Kernel& k, const ps_kernel_invocation_v8& inv,
                const Work& work) const {
    if (kind < 2)
      compute<T, 0>(k, inv, work);
    else if (kind == 2)
      compute<T, 2>(k, inv, work);
    else
      compute<T, 3>(k, inv, work);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    if (type == ElementType::Float32)
      dispatch<float>(k, i, Work{i, work_limit});
    else
      dispatch<double>(k, i, Work{i, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& i) const override {
    if (type == ElementType::Float32)
      dispatch<float>(k, i.views, Work{i.views, work_limit, 0, i.work});
    else
      dispatch<double>(k, i.views, Work{i.views, work_limit, 0, i.work});
  }
};
OperationDefinition halftone_operation(unsigned kind) {
  constexpr const char* names[]{"grade.halftone_dot", "grade.halftone_line",
                                "grade.halftone_cmyk",
                                "grade.halftone_colorize"};
  auto p = kind == 2 ? selectors() : std::vector<OperationParameterSpec>{};
  if (kind < 3) {
    append(&p, {{"samples_per_axis", P::Int64, false},
                {"edge", P::String, false},
                {"origin_x", P::Int64, false},
                {"origin_y", P::Int64, false}});
    if (kind == 2)
      append(&p, {{"bases", P::String, true},
                  {"origins", P::String, true},
                  {"screen", P::String, true}});
    else
      append(&p, {{"basis", P::String, true}, {"origin", P::String, true}});
  } else {
    append(&p, {{"axis", P::Int64, false},
                {"output_description", P::String, true},
                {"group", P::String, true}});
  }
  return budgeted_operation(generation::operation(
      names[kind], kind == 3 ? 3 : 1, std::move(p),
      {{kind < 2 ? "ink" : "values", tensor_schema(ElementType::Float64, {1})}},
      [kind](const auto& in, const auto& p) {
        return std::make_shared<Halftone>(kind, in, p);
      }));
}
}  // namespace ps::plugin_internal::grade::artistic
namespace ps::plugin_internal {
Status register_grade_halftone(OperationRegistry* registry) {
  for (unsigned k = 0; k < 4; ++k) {
    const auto status =
        registry->register_operation(grade::artistic::halftone_operation(k));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
