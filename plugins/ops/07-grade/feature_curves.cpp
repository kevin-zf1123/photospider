// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "02-format-color/rgb_basis_math.hpp"
#include "04-mask-morphology/common.hpp"
#include "07-grade/feature_curves_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade::feature {
using namespace generation;  // NOLINT(build/namespaces)

inline bool empty(const Region& q) {
  return q.dimensions().empty() ||
         std::any_of(q.dimensions().begin(), q.dimensions().end(),
                     [](const auto& d) { return d.extent == 0; });
}
inline void cancel(const ps_kernel_invocation_v8& inv) {
  if (inv.cancellation.is_cancelled &&
      inv.cancellation.is_cancelled(inv.cancellation.context))
    throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
}
template <class T, class V>
auto address(const V& view, const Coordinate& at) {
  using Byte = std::conditional_t<std::is_same_v<V, ps_kernel_const_view_v8>,
                                  const unsigned char, unsigned char>;
  using Value = std::conditional_t<std::is_same_v<V, ps_kernel_const_view_v8>,
                                   const T, T>;
  return reinterpret_cast<Value*>(static_cast<Byte*>(view.allocation) +
                                  offset(*view.layout, view.byte_offset, at));
}
template <class T>
bool aligned(const T* p) {
  return reinterpret_cast<std::uintptr_t>(p) % alignof(T) == 0;
}
fast_color::Matrix native_matrix(const basis_ops::Matrix& matrix) {
  fast_color::Matrix result{};
  for (unsigned r = 0; r < 3; ++r)
    result[r] = basis_ops::ExactRow(matrix, r).approximate;
  return result;
}

// Preparation only consumes descriptors and static color assertions.
void prepare_rgb(Edit* edit, const color_point::Selection& s,
                 const Parameters& p) {
  const auto& i = s.interpretation;
  if (s.semantic) {
    color_point::typed(
        i.model == "rgb" && i.transfer == "linear" &&
            (i.reference == "scene" || i.reference == "display" ||
             i.reference == "scene_relative" ||
             i.reference == "display_relative"),
        "GRD20 requires relative linear RGB with explicit reference");
    for (const auto& u : s.units)
      color_point::typed(u == "1" || u == "relative",
                         "GRD20 requires relative RGB units");
    color_point::typed(!i.coordinates || i.coordinates->scale.empty() ||
                           i.coordinates->scale == "relative",
                       "GRD20 requires relative coordinate scale");
  }
  format::RgbBasis assertion;
  assertion.preset = i.primaries;
  assertion.primaries_xy = i.primaries_xy;
  assertion.white = i.white;
  const auto basis = p.count("source_basis")
                         ? basis_ops::parse_basis(text(p, "source_basis"))
                         : basis_ops::resolve(assertion);
  color_point::require(
      i.primaries.empty() || basis_ops::preset(i.primaries).xy == basis.xy,
      "GRD20 basis assertion disagrees");
  color_point::require(!i.primaries_xy || *i.primaries_xy == basis.xy,
                       "GRD20 primary assertion disagrees");
  color_point::require(!i.white || *i.white == basis.white,
                       "GRD20 white assertion disagrees");
  auto& b = edit->brightness;
  const auto m = basis_ops::rgb_matrix(basis.xy, basis.white);
  b.forward = native_matrix(m);
  b.inverse = native_matrix(basis_ops::inverse(m));
  const auto a = basis_ops::adaptation(basis.white, {.3127, .3290}, 1);
  b.adaptation = native_matrix(a);
  b.unadaptation = native_matrix(basis_ops::inverse(a));
  b.white = {basis.white[0] / basis.white[1], 1,
             (1 - basis.white[0] - basis.white[1]) / basis.white[1]};
  b.prepare();
}

struct FeatureProgram final : Program {
  unsigned member, axis;
  ElementType type;
  std::vector<std::uint64_t> dims, selected;
  std::uint64_t knots = 0;
  double period = 0;
  bool nonnegative_modulation = false;
  fast_curve::Edge edge = fast_curve::Edge::Clamp;
  Edit edit;

  FeatureProgram(unsigned kind, const std::vector<OperationMetadata>& inputs,
                 const Parameters& p)
      : member(kind) {
    const auto& source = tensor(inputs.at(0), true);
    type = source.descriptor.element_type;
    dims = source.sample_shape();
    require(floating(type), "GRD20 source requires Float32/64");
    edit.member = member;
    try {
      auto s = color_point::select(inputs[0], p, {}, member >= 2);
      require(s.axis >= 0, "GRD20 requires a component axis");
      axis = static_cast<unsigned>(s.axis);
      selected = s.channels;
      if (member < 2) {
        require(dims[axis] == 1 && selected == std::vector<std::uint64_t>{0},
                "GRD20 feature has one component");
        const auto unit = text(p, "modulation_unit");
        require(unit == "delta_h" || unit == "chroma_scale" ||
                    unit == "saturation_scale" || unit == "delta_lightness" ||
                    unit == "ev",
                "GRD20 modulation_unit");
        nonnegative_modulation =
            unit == "chroma_scale" || unit == "saturation_scale";
        if (member == 0) {
          period = std::get<double>(p.at("period"));
        } else {
          const auto policy = text(p, "edge", "clamp");
          require(policy == "clamp" || policy == "reject", "GRD20 edge");
          edge = policy == "clamp" ? fast_curve::Edge::Clamp
                                   : fast_curve::Edge::Reject;
        }
        const auto& x = tensor(inputs.at(1));
        const auto& y = tensor(inputs.at(2));
        require(x.descriptor.element_type == ElementType::Float64 &&
                    x.sample_shape().size() == 1,
                "GRD20 knots require Float64[K]");
        knots = x.sample_shape()[0];
        require(knots >= 2 && knots <= 65536 &&
                    y.descriptor.element_type == ElementType::Float64 &&
                    y.sample_shape() == x.sample_shape(),
                "GRD20 ordinates require Float64[K], 2<=K<=65536");
        scratch_bytes =
            sizeof(Curve) + 6 * (knots + (member == 0)) * sizeof(double);
      } else {
        std::vector<std::string> roles;
        if (edit.rgb()) {
          roles = {"red", "green", "blue"};
        } else {
          auto model = text(p, "model");
          if (model.empty() && s.semantic)
            model = s.interpretation.model;
          require(!s.semantic || s.interpretation.model == model,
                  "GRD20 model assertion disagrees");
          const bool bounded = model == "hsl" || model == "hsv";
          const bool polar = model == "cielch" || model == "oklch";
          const bool lab = model == "cielab" || model == "oklab";
          require(member == 2   ? bounded || polar
                  : member == 6 ? bounded
                                : lab,
                  "GRD20 incompatible native model");
          if (bounded)
            roles = {"hue", "saturation",
                     model == "hsl" ? "lightness" : "value"};
          else
            roles = polar ? std::vector<std::string>{"l", "c", "h"}
                          : std::vector<std::string>{"l", "a", "b"};
          edit.hue = bounded ? 0 : 2;
          const auto unit = text(p, "modulation_unit");
          require(unit == (member == 2   ? "delta_h"
                           : member == 3 ? "chroma_scale"
                           : member == 4 ? "delta_lightness"
                                         : "saturation_scale"),
                  "GRD20 modulation unit disagrees");
          if (member == 2) {
            const auto h = text(p, "hue_unit");
            require(h == "radian" || h == "pi_multiple", "GRD20 hue_unit");
          }
        }
        s = color_point::select(inputs[0], p, roles);
        selected = s.channels;
        require(selected.size() == 3, "GRD20 selects three color coordinates");
        if (edit.rgb()) {
          prepare_rgb(&edit, s, p);
        } else if (s.semantic) {
          for (unsigned c = 0; c < 3; ++c) {
            const auto& u = s.units[c];
            if (member == 2 && c == edit.hue) {
              require(u.empty() || u == text(p, "hue_unit"),
                      "GRD20 hue units disagree");
            } else if ((member == 3 && c > 0) || (member == 4 && c == 0) ||
                       (member == 6 && c == 1) || (member == 2 && c == 1)) {
              const bool cie = s.interpretation.model == "cielab" &&
                               ((c == 1 && u == "a*") || (c == 2 && u == "b*"));
              const bool chroma =
                  s.interpretation.model == "cielch" && c == 1 && u == "C*";
              require(u.empty() || u == "1" || u == "relative" || cie || chroma,
                      "GRD20 requires native coordinate units");
            }
          }
        }
        const auto& field = tensor(inputs.at(1), true);
        auto expected = dims;
        expected[axis] = 1;
        require(
            field.descriptor.element_type == ElementType::Float64 &&
                field.sample_shape() == expected,
            "GRD20 field requires Float64 with matching non-component axes");
      }
    } catch (const basis_ops::CheckedFailure& f) {
      throw f.status;
    } catch (const basis_ops::GeometryError&) {
      throw Status{ErrorCode::InvalidArgument, "GRD20 invalid RGB basis"};
    }
    auto output = *inputs[0].result_schema;
    output.id = "photospider.tensor";
    output.version = 1;
    output.tensors[0].key = "samples";
    output.tensors[0].atomic_trailing_axes = 0;
    if (member < 2)
      output.tensors[0].descriptor.element_type = ElementType::Float64;
    add_output(this, std::move(output));
  }

  int component(std::uint64_t channel) const {
    const auto found = std::find(selected.begin(), selected.end(), channel);
    return found == selected.end() ? -1
                                   : static_cast<int>(found - selected.begin());
  }
  bool active(std::uint64_t channel) const {
    const int c = component(channel);
    return c >= 0 && (member < 2 || edit.active(static_cast<unsigned>(c)));
  }
  bool calculates(const Region& q) const {
    if (empty(q))
      return false;
    const auto& d = q.dimensions()[axis];
    return std::any_of(selected.begin(), selected.end(), [&](auto c) {
      return c >= d.offset && c - d.offset < d.extent && active(c);
    });
  }
  std::uint64_t task_scratch(unsigned, const Region& q) const override {
    if (!calculates(q))
      return 0;
    std::uint64_t points = 1;
    for (unsigned a = 0; a < q.dimensions().size(); ++a)
      if (a != axis)
        points *= q.dimensions()[a].extent;
    return member < 2 && knots > std::max<std::uint64_t>(64, 2 * points)
               ? 0
               : scratch_bytes;
  }
  std::vector<ReadRegion> support(unsigned, const Region& q) const override {
    if (empty(q))
      return {};
    std::vector<ReadRegion> reads{{0, 0, q}};
    if (!calculates(q))
      return reads;
    if (member < 2) {
      reads.push_back({1, 0, Region({{0, knots}})});
      reads.push_back({2, 0, Region({{0, knots}})});
    } else {
      auto field = q.dimensions();
      field[axis] = {0, 1};
      reads.push_back({1, 0, Region(std::move(field))});
      if (edit.rgb() || member == 2) {
        for (unsigned c = 0; c < 3; ++c) {
          if (!edit.rgb() && c != 1)
            continue;
          const auto& d = q.dimensions()[axis];
          if (selected[c] >= d.offset && selected[c] - d.offset < d.extent)
            continue;
          auto color = q.dimensions();
          color[axis] = {selected[c], 1};
          reads.push_back({0, 0, Region(std::move(color))});
        }
      }
    }
    return reads;
  }
  Curve* prepare_curve(const mask::Reader& reader, void* storage,
                       const Work& work) const {
    work.poll(knots * 64);
    auto* c = new (storage) Curve;
    const auto n = knots + (member == 0);
    auto* x = reinterpret_cast<double*>(c + 1);
    auto* y = x + n;
    Coordinate at{};
    for (std::uint64_t i = 0; i < knots; ++i) {
      at[0] = i;
      x[i] = reader.value(1, 0, at);
      y[i] = reader.value(2, 0, at);
      if (!(i & 1023U))
        cancel(reader.invocation);
    }
    if (member == 0) {
      x[knots] = period;
      y[knots] = y[0];
      c->period = period;
    }
    c->nonnegative = nonnegative_modulation;
    c->prepare(x, y, n, edge, y + n);
    return c;
  }

  template <class T, unsigned C>
  bool packed(const mask::Reader& reader, const ps_kernel_invocation_v8& inv,
              const ps_kernel_mutable_view_v8& out, const Curve* curve) const {
    const auto& l = *out.layout;
    if (!axis || axis + 1 != dims.size() || dims[axis] != C || l.origin[axis] ||
        l.shape[axis] != C)
      return false;
    for (unsigned c = 0; c < selected.size(); ++c)
      if (selected[c] != c)
        return false;
    using Output = std::conditional_t<C == 1, double, T>;
    const auto line = axis - 1;
    if (l.strides[axis] != sizeof(Output) ||
        l.strides[line] != C * sizeof(Output))
      return false;
    Coordinate at{};
    std::uint64_t rows = 1;
    for (unsigned a = 0; a < axis; ++a) {
      at[a] = l.origin[a];
      if (a != line)
        rows *= l.shape[a];
    }
    // A complete contiguous retained tile can span multiple logical rows.
    // Prove all source/field coverage before taking this flattened path.
    const auto& whole = reader.view(0, 0, at);
    const auto covers = [&](const ps_kernel_layout_v8& layout,
                            std::uint64_t bytes, bool field) {
      for (unsigned a = l.rank; a-- > 0;) {
        const auto extent = field && a == axis ? 1 : l.shape[a];
        if (at[a] < layout.origin[a] ||
            at[a] - layout.origin[a] > layout.shape[a] ||
            extent > layout.shape[a] - (at[a] - layout.origin[a]) ||
            (extent > 1 && layout.strides[a] != bytes))
          return false;
        bytes *= extent;
      }
      return true;
    };
    const double* all_fields = nullptr;
    bool dense_field = true;
    if constexpr (C != 1) {
      const auto& control = reader.view(1, 0, at);
      all_fields = address<double>(control, at);
      dense_field = aligned(all_fields) && covers(*control.layout, 8, true);
    }
    const auto* all_source = address<T>(whole, at);
    auto* all_target = address<Output>(out, at);
    if (aligned(all_source) && aligned(all_target) && dense_field &&
        covers(*whole.layout, sizeof(T), false) &&
        covers(l, sizeof(Output), false)) {
      const auto count = rows * l.shape[line];
      for (std::uint64_t i = 0; i < count;) {
        cancel(inv);
        const auto n = std::min<std::uint64_t>(4096, count - i);
        if constexpr (C == 1)
          curve->span(all_source + i, all_target + i, n);
        else
          edit.span<T, C>(all_source + C * i, all_fields + i,
                          all_target + C * i, n);
        i += n;
      }
      return true;
    }
    const auto end = l.origin[line] + l.shape[line];
    for (std::uint64_t row = 0; row < rows; ++row) {
      for (at[line] = l.origin[line]; at[line] < end;) {
        cancel(inv);
        const auto& input = reader.view(0, 0, at);
        const auto& s = *input.layout;
        if (s.origin[axis] || s.shape[axis] != C ||
            s.strides[axis] != sizeof(T) || s.strides[line] != C * sizeof(T))
          return false;
        const auto* x = address<T>(input, at);
        auto* y = address<Output>(out, at);
        if (!aligned(x) || !aligned(y))
          return false;
        auto n = std::min<std::uint64_t>(
            {4096, end - at[line], s.origin[line] + s.shape[line] - at[line]});
        if constexpr (C == 1) {
          curve->span(x, y, n);
        } else {
          const auto& control = reader.view(1, 0, at);
          const auto& f = *control.layout;
          const auto* field = address<double>(control, at);
          if (f.strides[line] != sizeof(double) || !aligned(field))
            return false;
          n = std::min(n, f.origin[line] + f.shape[line] - at[line]);
          edit.span<T, C>(x, field, y, n);
        }
        at[line] += n;
      }
      for (unsigned a = line; a-- > 0;) {
        if (++at[a] < l.origin[a] + l.shape[a])
          break;
        at[a] = l.origin[a];
      }
    }
    return true;
  }
  template <class T>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    Curve* curve = nullptr;
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& out = inv.outputs[o];
      const auto& l = *out.layout;
      std::vector<RegionDimension> region;
      Coordinate at{};
      std::uint64_t count = 1, points = 1;
      for (unsigned a = 0; a < l.rank; ++a) {
        region.push_back({l.origin[a], l.shape[a]});
        at[a] = l.origin[a];
        count *= l.shape[a];
        if (a != axis)
          points *= l.shape[a];
      }
      if (!count)
        continue;
      const bool arithmetic = calculates(Region(std::move(region)));
      work.poll(count * (arithmetic ? 256 : 1));
      const bool sparse =
          member < 2 && knots > std::max<std::uint64_t>(64, 2 * points);
      if (member < 2 && !sparse && !curve)
        curve = prepare_curve(reader, inv.scratch[0].allocation, work);
      if (arithmetic && !sparse &&
          ((member < 2 && packed<T, 1>(reader, inv, out, curve)) ||
           (member >= 2 && dims[axis] == 3 &&
            packed<T, 3>(reader, inv, out, nullptr)) ||
           (member >= 2 && dims[axis] == 4 &&
            packed<T, 4>(reader, inv, out, nullptr))))
        continue;
      const auto first = l.origin[axis], end = first + l.shape[axis];
      const auto interval = std::max<std::uint64_t>(1, 4096 / l.shape[axis]);
      for (std::uint64_t i = 0; i < points; ++i) {
        if (!(i % interval))
          cancel(inv);
        auto q = at;
        q[axis] = 0;
        const double field =
            member >= 2 && arithmetic ? reader.value(1, 0, q) : 0;
        fast_color::Triple result{};
        bool copy = false;
        double gate = 1;
        if (arithmetic && member >= 2) {
          if (edit.rgb()) {
            fast_color::Triple x;
            for (unsigned c = 0; c < 3; ++c) {
              q[axis] = selected[c];
              x[c] = reader.value(0, 0, q);
            }
            result = edit.replace(x, field, &copy);
          } else {
            if (member == 2) {
              q[axis] = selected[1];
              gate = reader.value(0, 0, q);
            }
            copy = edit.copies(field, gate);
          }
        }
        for (auto c = first; c < end; ++c) {
          if (c != first && !((c - first) & 4095U))
            cancel(inv);
          at[axis] = c;
          std::uint64_t value;
          if (member < 2) {
            const double query = reader.value(0, 0, at);
            const auto table = [&](unsigned port, std::size_t index) {
              Coordinate coordinate{};
              coordinate[0] = index;
              return reader.value(port, 0, coordinate);
            };
            const double result = sparse
                                      ? sparse_value(
                                            query, knots, period,
                                            [&](auto i) { return table(1, i); },
                                            [&](auto i) { return table(2, i); },
                                            nonnegative_modulation)
                                      : curve->evaluate(query);
            value = convert(result, false);
          } else if (!active(c) || copy) {
            value = reader.raw(0, 0, at);
          } else {
            value = convert(
                edit.rgb() ? result[component(c)]
                           : edit.coordinate(reader.value(0, 0, at), field),
                sizeof(T) == 4);
          }
          store(out, at, value);
        }
        for (unsigned a = l.rank; a-- > 0;) {
          if (a == axis)
            continue;
          if (++at[a] < l.origin[a] + l.shape[a])
            break;
          at[a] = l.origin[a];
        }
      }
    }
    cancel(inv);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    if (type == ElementType::Float32)
      compute<float>(k, inv, Work{inv, work_limit});
    else
      compute<double>(k, inv, Work{inv, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& inv) const override {
    const Work work{inv.views, work_limit, 0, inv.work};
    if (type == ElementType::Float32)
      compute<float>(k, inv.views, work);
    else
      compute<double>(k, inv.views, work);
  }
};

OperationDefinition operation(unsigned member) {
  static constexpr const char* names[] = {
      "grade.feature_curve_periodic", "grade.feature_curve_linear",
      "grade.apply_hue_delta",        "grade.apply_chroma_scale",
      "grade.apply_lightness_delta",  "grade.reconstruct_luminance",
      "grade.apply_saturation_scale", "grade.set_xyz_y",
      "grade.set_cielab_lightness",   "grade.set_oklab_lightness"};
  using Type = OperationParameterType;
  std::vector<OperationParameterSpec> p{
      {"axis", Type::Int64, false},
      {"components", Type::String, false},
      {"input_description", Type::String, false},
      {"group", Type::String, false}};
  if (member < 2 || member == 2 || member == 3 || member == 4 || member == 6)
    p.push_back({"modulation_unit", Type::String, true});
  if (member == 0)
    p.push_back({"period", Type::Float64, true});
  if (member == 1)
    p.push_back({"edge", Type::String, false});
  if (member == 2 || member == 3 || member == 4 || member == 6)
    p.push_back({"model", Type::String, false});
  if (member == 2)
    p.push_back({"hue_unit", Type::String, true});
  if (member == 5 || member >= 7)
    p.push_back({"source_basis", Type::String, false});
  auto definition = budgeted_operation(generation::operation(
      names[member], member < 2 ? 3 : 2, std::move(p),
      {{member < 2 ? "modulation" : "values",
        tensor_schema(ElementType::Float64, {1})}},
      [member](const auto& inputs, const auto& p) {
        return std::make_shared<FeatureProgram>(member, inputs, p);
      }));
  definition.reusable_tiles = true;
  return definition;
}
}  // namespace ps::plugin_internal::grade::feature

namespace ps::plugin_internal {
Status register_grade_feature_curves(OperationRegistry* registry) {
  for (unsigned member = 0; member < 10; ++member) {
    const auto s =
        registry->register_operation(grade::feature::operation(member));
    if (!s.ok())
      return s;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
