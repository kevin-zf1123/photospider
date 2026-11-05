// Copyright 2026 Photospider contributors
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "07-grade/artistic_common.hpp"
#include "07-grade/warper_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade::artistic {
struct Warper final : Program {
  Image image, effect;
  unsigned kind, nu, nv;
  unsigned first, second;
  Warper(unsigned member, const std::vector<OperationMetadata>& in,
         const Parameters& p)
      : kind(member) {
    image = Image(in[0], p,
                  kind == 0 ? std::vector<std::string>{"l", "a", "b"}
                            : std::vector<std::string>{"l", "c", "h"});
    auto selection = image.selection;
    if (kind != 2) {
      selection = statistics::select(in.at(0), p, false, kind != 0);
    } else {
      const auto model = text(
          p, "model",
          selection.semantic ? selection.interpretation.model.c_str() : "");
      require(
          (model == "cielch" || model == "oklch") &&
              (!selection.semantic || selection.interpretation.model == model),
          "CL warper requires LCh");
      for (unsigned c = 0; c < 2; ++c) {
        const auto& unit = selection.units[c];
        require(unit.empty() || unit == "1" || unit == "relative" ||
                    (c == 1 && model == "cielch" && unit == "C*"),
                "CL native coordinate units");
      }
      if (p.count("hue_unit")) {
        const auto unit = text(p, "hue_unit");
        require((unit == "radian" || unit == "pi_multiple") &&
                    (!selection.semantic || unit == selection.units[2]),
                "CL hue unit assertion");
      }
    }
    if (kind == 1)
      require(text(p, "hue_unit", selection.units[2].c_str()) == "pi_multiple",
              "hue warper requires pi_multiple hue");
    clamping(p);
    enum_parameter(p, "topology", "allow", "positive_jacobian");
    if (kind == 0)
      enum_parameter(p, "boundary", "free", "pinned_zero");
    first = image.selected[kind == 0 ? 1 : kind == 1 ? 2 : 1];
    second = image.selected[kind == 0 ? 2 : kind == 1 ? 1 : 0];
    effect = image;
    effect.selected = {first, second};
    const auto& u = tensor(in.at(1));
    const auto& v = tensor(in.at(2));
    require(u.descriptor.element_type == ElementType::Float64 &&
                v.descriptor.element_type == ElementType::Float64 &&
                u.sample_shape().size() == 1 && v.sample_shape().size() == 1,
            "color warper axes require Float64 vectors");
    require(u.sample_shape()[0] >= 2 && u.sample_shape()[0] <= 256 &&
                v.sample_shape()[0] >= 2 && v.sample_shape()[0] <= 256,
            "color warper axis extents");
    nu = static_cast<unsigned>(u.sample_shape()[0]);
    nv = static_cast<unsigned>(v.sample_shape()[0]);
    typed(in.at(3), ElementType::Float64, {nu, nv, 2});
    add_output(this, output_like(in[0]));
  }
  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    auto out = effect.support(r, true);
    if (effect.active(r)) {
      out.push_back({1, 0, whole({nu})});
      out.push_back({2, 0, whole({nv})});
      out.push_back({3, 0, whole({nu, nv, 2})});
    }
    return out;
  }
  template <class T>
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{k, inv};
    std::array<unsigned, 3> selected{};
    for (unsigned c = 0; c < 3; ++c)
      selected[c] = image.selected[c];
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& out = inv.outputs[o];
      const bool active = effect.active(region(*out.layout));
      work.poll(count(*out.layout) * 160 + (active ? nu + nv : 0));
      std::array<double, 256> u{}, v{};
      warper::Mesh mesh{kind, nu, nv, u.data(), v.data()};
      const double* table = nullptr;
      std::array<const double*, 256> rows{};
      bool row_table = active;
      if (active) {
        for (unsigned j = 0; j < nu; ++j)
          u[j] = reader.value(1, 0, Coordinate{j});
        for (unsigned j = 0; j < nv; ++j)
          v[j] = reader.value(2, 0, Coordinate{j});
        const auto& t = reader.view(3, 0, {});
        if (!t.layout->origin[0] && !t.layout->origin[1] &&
            !t.layout->origin[2] && t.layout->shape[0] == nu &&
            t.layout->shape[1] == nv && t.layout->shape[2] == 2 &&
            t.layout->strides[0] == nv * 16 && t.layout->strides[1] == 16 &&
            t.layout->strides[2] == 8 &&
            aligned(address<double>(t, Coordinate{})))
          table = address<double>(t, Coordinate{});
        if (!table) {
          // Raw rank-three tables may arrive as one view per leading index.
          // Borrow complete packed rows for this invocation without copying
          // dynamic payload or retaining pointers across executions.
          for (unsigned a = 0; a < nu; ++a) {
            if ((a & 63U) == 0)
              cancel(inv);
            const Coordinate at{a, 0, 0};
            const auto& row = reader.view(3, 0, at);
            const auto& l = *row.layout;
            if (l.origin[1] || l.shape[1] != nv || l.origin[2] ||
                l.shape[2] != 2 || l.strides[1] != 16 || l.strides[2] != 8 ||
                !aligned(address<double>(row, at))) {
              row_table = false;
              break;
            }
            rows[a] = address<double>(row, at);
          }
        }
      }
      const auto execute = [&](auto read) {
        image_runs<T>(
            image, reader, inv, out,
            [&](const T* src, T* dst, std::uint64_t n, const Coordinate&) {
              mesh.span(src, dst, n, image.dims[image.axis], selected.data(),
                        read);
              return true;
            },
            [&](Coordinate at, std::uint64_t j) {
              if ((j & 127U) == 0)
                cancel(inv);
              std::array<double, 2> values{}, source{};
              if (active) {
                auto q = at;
                q[image.axis] = first;
                source[0] = reader.value(0, 0, q);
                q[image.axis] = second;
                source[1] = reader.value(0, 0, q);
                values = mesh.apply(source[0], source[1], read);
              }
              for (at[image.axis] = out.layout->origin[image.axis];
                   at[image.axis] < out.layout->origin[image.axis] +
                                        out.layout->shape[image.axis];
                   ++at[image.axis]) {
                const int c = at[image.axis] == first    ? 0
                              : at[image.axis] == second ? 1
                                                         : -1;
                store(out, at,
                      c < 0 || values[c] == source[c]
                          ? reader.raw(0, 0, at)
                          : convert(values[c], sizeof(T) == 4));
              }
            },
            true);
      };
      if (table) {
        execute([&](unsigned a, unsigned b, unsigned c) {
          return table[(a * nv + b) * 2 + c];
        });
      } else if (row_table) {
        execute([&](unsigned a, unsigned b, unsigned c) {
          return rows[a][b * 2 + c];
        });
      } else {
        execute([&](unsigned a, unsigned b, unsigned c) {
          return reader.value(3, 0, Coordinate{a, b, c});
        });
      }
    }
    cancel(inv);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    if (image.type == ElementType::Float32)
      compute<float>(k, i, Work{i, work_limit});
    else
      compute<double>(k, i, Work{i, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& i) const override {
    if (image.type == ElementType::Float32)
      compute<float>(k, i.views, Work{i.views, work_limit, 0, i.work});
    else
      compute<double>(k, i.views, Work{i.views, work_limit, 0, i.work});
  }
};
OperationDefinition warper_operation(unsigned kind) {
  constexpr const char* names[]{"grade.color_warper_ab",
                                "grade.color_warper_hc",
                                "grade.color_warper_cl"};
  auto p = selectors();
  append(&p, {{"edge", P::String, false}, {"topology", P::String, false}});
  if (kind == 0)
    p.push_back({"boundary", P::String, false});
  return budgeted_operation(generation::operation(
      names[kind], 4, std::move(p),
      {{"values", tensor_schema(ElementType::Float64, {1})}},
      [kind](const auto& in, const auto& p) {
        return std::make_shared<Warper>(kind, in, p);
      }));
}
}  // namespace ps::plugin_internal::grade::artistic
namespace ps::plugin_internal {
Status register_grade_warper(OperationRegistry* registry) {
  for (unsigned k = 0; k < 3; ++k) {
    auto status =
        registry->register_operation(grade::artistic::warper_operation(k));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
