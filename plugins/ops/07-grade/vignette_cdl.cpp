// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "07-grade/artistic_common.hpp"
#include "07-grade/vignette_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade::artistic {
struct Vignette final : Program {
  bool apply;
  Image image;
  vignette::Geometry geometry;
  std::vector<std::uint64_t> coordinates;
  Vignette(bool application, const std::vector<OperationMetadata>& in,
           const Parameters& p)
      : apply(application) {
    const auto center = reals(p, "center", 2);
    const auto radii = reals(p, "radii", 2);
    geometry.cx = center[0];
    geometry.cy = center[1];
    geometry.rx = radii[0];
    geometry.ry = radii[1];
    const auto turns = std::remainder(real(p, "rotation_pi"), 2.0);
    const auto theta = turns * std::acos(-1.0);
    geometry.cosine = std::abs(turns) == 1    ? -1
                      : turns == 0            ? 1
                      : std::abs(turns) == .5 ? 0
                                              : std::cos(theta);
    geometry.sine = turns == 0 || std::abs(turns) == 1 ? 0
                    : turns == .5                      ? 1
                    : turns == -.5                     ? -1
                                                       : std::sin(theta);
    geometry.inner = real(p, "inner", 0);
    geometry.outer = real(p, "outer", 1);
    geometry.center = real(p, "center_ev", 0);
    geometry.edge = real(p, "edge_ev", -1);
    const auto& t = tensor(in.at(apply ? 1 : 0));
    coordinates = t.sample_shape();
    require(t.descriptor.element_type == ElementType::Float64 &&
                coordinates.size() == 3 && coordinates[2] == 2,
            "vignette coordinates require Float64[H,W,2]");
    if (t.layout.spatial)
      require(t.layout.height_axis == 0 && t.layout.width_axis == 1 &&
                  t.layout.channel_axis == 2,
              "vignette coordinate axes");
    if (apply) {
      image = Image(in[0], p, {}, true);
      require(image.selected.size() == 1 || image.selected.size() == 3,
              "vignette selects linear Gray or RGB");
      const auto& s = image.selection;
      require(
          !s.semantic || (s.interpretation.model ==
                              (image.selected.size() == 1 ? "gray" : "rgb") &&
                          s.interpretation.transfer == "linear"),
          "vignette requires linear Gray/RGB");
      if (s.semantic && image.selected.size() == 1)
        require(s.interpretation.coordinates &&
                    s.interpretation.coordinates->gray_kind == "linear_y",
                "vignette requires linear-Y Gray");
      require(coordinates[0] == image.dims[image.yaxis] &&
                  coordinates[1] == image.dims[image.xaxis],
              "vignette coordinate image extent");
      add_output(this, output_like(in[0]));
    } else {
      auto output = output_like(in[0]);
      output.tensors[0].descriptor.shape[2] = 1;
      add_output(this, std::move(output));
    }
  }
  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (empty(r))
      return {};
    auto out = apply ? image.support(r, false) : std::vector<ReadRegion>{};
    if (geometry.center == geometry.edge)
      return out;
    if (apply && !image.active(r))
      return out;
    const auto& d = r.dimensions();
    out.push_back({apply ? 1U : 0U, 0,
                   Region({d[apply ? image.yaxis : 0],
                           d[apply ? image.xaxis : 1],
                           {0, 2}})});
    return out;
  }
  template <class T>
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader r{k, inv};
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& out = inv.outputs[o];
      work.poll(count(*out.layout) * 96);
      if (!apply) {
        const auto& l = *out.layout;
        Coordinate at{l.origin[0], l.origin[1], 0};
        for (; at[0] < l.origin[0] + l.shape[0]; ++at[0]) {
          for (at[1] = l.origin[1]; at[1] < l.origin[1] + l.shape[1];) {
            cancel(inv);
            std::uint64_t n =
                std::min<std::uint64_t>(1024, l.origin[1] + l.shape[1] - at[1]);
            if (geometry.center == geometry.edge) {
              for (std::uint64_t j = 0; j < n; ++j)
                statistics::save(out, at, 1, j, bits(geometry.center));
            } else {
              const auto x = span(r, 0, at, 1, &n);
              auto q = at;
              q[2] = 1;
              const auto y = span(r, 0, q, 1, &n);
              if (x.stride == 16 && y.stride == 16 && y.data == x.data + 8 &&
                  l.strides[1] == 8 &&
                  aligned(reinterpret_cast<const double*>(x.data)) &&
                  aligned(address<double>(out, at))) {
                geometry.field(reinterpret_cast<const double*>(x.data),
                               address<double>(out, at), n);
              } else {
                for (std::uint64_t j = 0; j < n; ++j)
                  statistics::save(out, at, 1, j,
                                   bits(geometry.ev(x.value(j), y.value(j))));
              }
            }
            at[1] += n;
          }
        }
        continue;
      }
      const bool active = image.active(region(*out.layout));
      std::array<unsigned, 3> selected{};
      for (unsigned c = 0; c < image.selected.size(); ++c)
        selected[c] = image.selected[c];
      const auto slow = [&](Coordinate at, std::uint64_t i) {
        if ((i & 127U) == 0)
          cancel(inv);
        double ev = geometry.center;
        if (active && geometry.center != geometry.edge) {
          auto q = image.spatial_coordinate(at, 0);
          const auto x = r.value(1, 0, q);
          q[2] = 1;
          ev = geometry.ev(x, r.value(1, 0, q));
        }
        for (at[image.axis] = out.layout->origin[image.axis];
             at[image.axis] <
             out.layout->origin[image.axis] + out.layout->shape[image.axis];
             ++at[image.axis]) {
          const auto raw = r.raw(0, 0, at);
          if (image.component(at[image.axis]) < 0 || ev == 0) {
            store(out, at, raw);
          } else {
            T value;
            std::memcpy(&value, &raw, sizeof(T));
            store(out, at, convert(exposed(value, ev), sizeof(T) == 4));
          }
        }
      };
      const bool vertical = out.layout->shape[image.xaxis] == 1;
      image_runs<T>(
          image, r, inv, out,
          [&](const T* src, T* dst, std::uint64_t n, const Coordinate& at) {
            const auto channels = static_cast<unsigned>(image.dims[image.axis]);
            if (geometry.center == geometry.edge) {
              std::memcpy(dst, src, n * channels * sizeof(T));
              if (geometry.center != 0) {
                for (std::uint64_t j = 0; j < n; ++j)
                  for (auto c : image.selected)
                    dst[j * channels + c] =
                        exposed(src[j * channels + c], geometry.center);
              }
              return true;
            }
            const auto q = image.spatial_coordinate(at, 0);
            const auto& v = r.view(1, 0, q);
            const unsigned run = vertical ? 0 : 1;
            if (v.layout->origin[2] || v.layout->shape[2] != 2 ||
                v.layout->strides[2] != 8 || v.layout->strides[run] != 16 ||
                v.layout->origin[run] + v.layout->shape[run] - q[run] < n ||
                !aligned(address<double>(v, q)))
              return false;
            geometry.apply(src, address<double>(v, q), dst, n, channels,
                           selected.data(), image.selected.size());
            return true;
          },
          slow, vertical);
    }
    cancel(inv);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    if (apply && image.type == ElementType::Float32)
      compute<float>(k, i, Work{i, work_limit});
    else
      compute<double>(k, i, Work{i, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& i) const override {
    if (apply && image.type == ElementType::Float32)
      compute<float>(k, i.views, Work{i.views, work_limit, 0, i.work});
    else
      compute<double>(k, i.views, Work{i.views, work_limit, 0, i.work});
  }
};
OperationDefinition vignette_operation(bool apply) {
  auto p = apply ? selectors() : std::vector<OperationParameterSpec>{};
  append(&p, {{"center", P::String, true},
              {"radii", P::String, true},
              {"rotation_pi", P::Float64, false},
              {"inner", P::Float64, false},
              {"outer", P::Float64, false},
              {"center_ev", P::Float64, false},
              {"edge_ev", P::Float64, false}});
  return budgeted_operation(generation::operation(
      apply ? "grade.vignette" : "grade.vignette_ev", apply ? 2 : 1,
      std::move(p),
      {{apply ? "values" : "ev", tensor_schema(ElementType::Float64, {1})}},
      [apply](const auto& in, const auto& p) {
        return std::make_shared<Vignette>(apply, in, p);
      }));
}
}  // namespace ps::plugin_internal::grade::artistic
namespace ps::plugin_internal {
Status register_grade_vignette_cdl(OperationRegistry* registry) {
  for (unsigned kind = 0; kind < 2; ++kind) {
    const auto status = registry->register_operation(
        grade::artistic::vignette_operation(kind != 0));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
