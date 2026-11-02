// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "02-format-color/rgb_basis_math.hpp"
#include "04-mask-morphology/common.hpp"
#include "07-grade/finishing_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade::finishing {
using namespace generation;  // NOLINT(build/namespaces)
inline Matrix native_matrix(const basis_ops::Matrix& m) {
  Matrix result{};
  for (unsigned r = 0; r < 3; ++r)
    result[r] = basis_ops::ExactRow(m, r).approximate;
  return result;
}
inline basis_ops::ResolvedBasis resolve_basis(const color_point::Selection& s,
                                              const Parameters& p,
                                              const char* name) {
  const auto& i = s.interpretation;
  format::RgbBasis assertion;
  assertion.preset = i.primaries;
  assertion.primaries_xy = i.primaries_xy;
  assertion.white = i.white;
  const auto basis = p.count(name) ? basis_ops::parse_basis(text(p, name))
                                   : basis_ops::resolve(assertion);
  color_point::require(
      i.primaries.empty() || basis_ops::preset(i.primaries).xy == basis.xy,
      "GRD basis assertion disagrees");
  color_point::require(!i.primaries_xy || *i.primaries_xy == basis.xy,
                       "GRD primary assertion disagrees");
  color_point::require(!i.white || *i.white == basis.white,
                       "GRD white assertion disagrees");
  return basis;
}
inline unsigned adaptation_method(const Parameters& p, bool required) {
  const auto method = text(p, "adaptation", required ? "" : "bradford");
  const std::array<std::string, 4> methods{"xyz_scaling", "bradford", "cat02",
                                           "cat16"};
  const auto found = std::find(methods.begin(), methods.end(), method);
  require(found != methods.end(), "GRD invalid adaptation method");
  return static_cast<unsigned>(found - methods.begin());
}
inline bool empty(const Region& r) {
  return r.dimensions().empty() ||
         std::any_of(r.dimensions().begin(), r.dimensions().end(),
                     [](auto d) { return d.extent == 0; });
}
inline void cancel(const ps_kernel_invocation_v8& inv) {
  if (inv.cancellation.is_cancelled &&
      inv.cancellation.is_cancelled(inv.cancellation.context))
    throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
}
struct Adjustment final : Program {
  Transform transform;
  unsigned axis = 0;
  ElementType type;
  bool normalize = false;
  std::vector<std::uint64_t> dims, selected;
  Adjustment(unsigned kind, const std::vector<OperationMetadata>& input,
             const Parameters& p) {
    transform.kind = kind;
    const auto& source = tensor(input.at(0), true);
    type = source.descriptor.element_type;
    dims = source.sample_shape();
    require(floating(type), "GRD11-13 require Float32/64");
    try {
      color_point::Selection s;
      if (kind == 7 && !p.count("input_description")) {
        require(p.count("axis") && p.count("components"),
                "GRD13 requires component selection");
        const auto a = integer(p, "axis");
        require(a >= 0 && static_cast<std::uint64_t>(a) <
                              source.descriptor.shape.size(),
                "GRD13 invalid component axis");
        axis = static_cast<unsigned>(a + source.batch_axes.size());
        selected = mask::channels(text(p, "components"), dims[axis]);
        if (source.layout.spatial)
          require(source.layout.channel_axis == std::optional<std::uint32_t>(a),
                  "GRD13 spatial component axis mismatch");
      } else {
        s = color_point::select(
            input[0], p,
            kind == 7 ? std::vector<std::string>{}
                      : std::vector<std::string>{"red", "green", "blue"});
        require(s.axis >= 0, "GRD11-13 require a component axis");
        axis = static_cast<unsigned>(s.axis);
        selected = s.channels;
        if (kind != 7 && s.semantic) {
          const auto& i = s.interpretation;
          require(i.model == "rgb", "GRD11-12 require RGB");
          if (kind != 3) {
            const bool relative = i.reference == "scene" ||
                                  i.reference == "display" ||
                                  i.reference == "scene_relative" ||
                                  i.reference == "display_relative";
            const bool absolute = i.reference == "scene_absolute" ||
                                  i.reference == "display_absolute";
            require(
                i.transfer == "linear" && (relative || (kind <= 2 && absolute)),
                "GRD requires an explicit linear RGB reference");
            if (kind >= 4) {
              for (const auto& u : s.units)
                require(u == "1" || u == "relative",
                        "GRD12 requires relative units");
              require(!i.coordinates || i.coordinates->scale.empty() ||
                          i.coordinates->scale == "relative",
                      "GRD12 requires relative scale");
            }
          }
        }
      }
      if (kind == 1 || kind == 2 || (kind >= 4 && kind <= 6)) {
        const auto basis = resolve_basis(s, p, "source_basis");
        const auto forward = basis_ops::rgb_matrix(basis.xy, basis.white);
        transform.forward = native_matrix(forward);
        transform.inverse = native_matrix(basis_ops::inverse(forward));
        transform.white = white_xyz(basis.white);
        if (kind <= 2) {
          const auto method = adaptation_method(p, kind == 1);
          Triple ws, wt;
          auto dest = basis;
          if (kind == 1) {
            require(p.count("source_white") && p.count("target_white") &&
                        (p.count("destination_basis") ||
                         p.count("output_description")),
                    "GRD11B requires whites and destination RGB basis");
            ws = white_xyz(basis_ops::parse_white(text(p, "source_white")));
            wt = white_xyz(basis_ops::parse_white(text(p, "target_white")));
          } else {
            require(p.count("source_temperature"),
                    "GRD11C requires source_temperature");
            ws = planck_white(real(p, "source_temperature"),
                              real(p, "source_tint", 0));
            wt =
                p.count("target_white")
                    ? white_xyz(basis_ops::parse_white(text(p, "target_white")))
                    : transform.white;
          }
          if (kind == 1 && p.count("output_description")) {
            auto destination = input[0];
            auto q = p;
            q.erase("axis");
            q.erase("components");
            q["input_description"] = text(p, "output_description");
            const auto ds =
                color_point::select(destination, q, {"red", "green", "blue"});
            require(
                ds.axis == static_cast<int>(axis) && ds.channels == selected,
                "GRD11B destination selectors must match source");
            require(
                ds.interpretation.model == "rgb" &&
                    ds.interpretation.transfer == "linear" &&
                    ds.interpretation.reference == s.interpretation.reference,
                "GRD11B destination requires same linear reference");
            for (unsigned c = 0; c < 3; ++c) {
              const auto canonical = [](const std::string& unit) {
                return unit == "relative" ? std::string("1") : unit;
              };
              require(canonical(ds.units[c]) == canonical(s.units[c]),
                      "GRD11B destination requires the same coordinate units");
            }
            dest = resolve_basis(ds, p, "destination_basis");
          } else if (kind == 1) {
            dest = basis_ops::parse_basis(text(p, "destination_basis"));
          }
          const auto destination_inverse = native_matrix(
              basis_ops::inverse(basis_ops::rgb_matrix(dest.xy, dest.white)));
          transform.identity =
              basis.xy == dest.xy && basis.white == dest.white && ws == wt;
          transform.matrix = fast_color::multiply(
              destination_inverse,
              fast_color::multiply(adaptation(ws, wt, method),
                                   transform.forward));
        } else if (kind == 6) {
          const auto d65 = white_xyz({.3127, .3290});
          const auto a = adaptation(transform.white, d65, 1);
          transform.to_lms =
              fast_color::multiply(fast_color::kXyzToLms,
                                   fast_color::multiply(a, transform.forward));
          transform.from_lms = fast_color::multiply(
              transform.inverse,
              fast_color::multiply(fast_color::inverse(a),
                                   fast_color::inverse(fast_color::kXyzToLms)));
          const auto lab_inverse = fast_color::inverse(fast_color::kLmsToOklab);
          for (unsigned c = 0; c < 3; ++c)
            transform.neutral_roots[c] = lab_inverse[c][0];
        }
      }
    } catch (const basis_ops::CheckedFailure& failure) {
      throw failure.status;
    } catch (const basis_ops::GeometryError&) {
      throw Status{ErrorCode::InvalidArgument, "GRD invalid RGB geometry"};
    }
    require(!selected.empty(), "GRD requires selected components");
    if (kind == 0 || kind == 3) {
      const auto& control = tensor(input.at(1));
      require(control.descriptor.element_type == ElementType::Float64 &&
                  control.sample_shape() == std::vector<std::uint64_t>{3},
              "GRD gains/weights require Float64[3]");
      if (kind == 3) {
        const auto& bias = tensor(input.at(2));
        require(bias.descriptor.element_type == ElementType::Float64 &&
                    bias.sample_shape() == std::vector<std::uint64_t>{1},
                "GRD bias requires Float64[1]");
        normalize = color_point::boolean(p, "normalize");
      }
    }
    if (kind == 7) {
      transform.levels = static_cast<double>(integer(p, "levels", 8) - 1);
      const auto edge = text(p, "edge", "clamp");
      require(edge == "clamp" || edge == "reject", "GRD13 invalid edge");
      transform.clamp = edge == "clamp";
    }
    auto output = *input[0].result_schema;
    output.id = "photospider.tensor";
    output.version = 1;
    auto& t = output.tensors[0];
    t.key = "samples";
    t.atomic_trailing_axes = 0;
    if (kind == 3)
      t.descriptor.shape[axis - t.batch_axes.size()] = 1;
    check(output.validate());
    add_output(this, std::move(output));
  }
  int component(std::uint64_t c) const {
    const auto found = std::find(selected.begin(), selected.end(), c);
    return found == selected.end() ? -1
                                   : static_cast<int>(found - selected.begin());
  }
  bool calculates(const Region& r) const {
    if (empty(r))
      return false;
    if (transform.kind == 3)
      return true;
    const auto d = r.dimensions()[axis];
    return std::any_of(selected.begin(), selected.end(), [&](auto c) {
      return c >= d.offset && c - d.offset < d.extent;
    });
  }
  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (empty(r))
      return {};
    const auto kind = transform.kind;
    std::vector<ReadRegion> result;
    if (kind != 3)
      result.push_back({0, 0, r});
    if (!calculates(r) || transform.identity)
      return result;
    if (kind != 0 && kind != 7) {
      for (auto c : selected) {
        auto region = r.dimensions();
        region[axis] = {c, 1};
        result.push_back({0, 0, Region(std::move(region))});
      }
    }
    if (kind == 0 || kind == 3)
      result.push_back({1, 0, Region({{0, 3}})});
    if (kind == 3)
      result.push_back({2, 0, Region({{0, 1}})});
    return result;
  }
  template <class T, class V>
  static auto address(const V& v, const Coordinate& at) {
    using Byte = std::conditional_t<std::is_same_v<V, ps_kernel_const_view_v8>,
                                    const unsigned char, unsigned char>;
    using Pointer =
        std::conditional_t<std::is_same_v<V, ps_kernel_const_view_v8>, const T*,
                           T*>;
    return reinterpret_cast<Pointer>(static_cast<Byte*>(v.allocation) +
                                     offset(*v.layout, v.byte_offset, at));
  }
  template <class T, unsigned C>
  bool packed(const mask::Reader& reader, const ps_kernel_invocation_v8& inv,
              const ps_kernel_mutable_view_v8& out, const Transform& op) const {
    const auto& l = *out.layout;
    if (axis == 0 || axis + 1 != l.rank || l.origin[axis] != 0 ||
        l.shape[axis] != (op.kind == 3 ? 1 : C) || selected.size() != 3)
      return false;
    Coordinate at{};
    for (unsigned a = 0; a < l.rank; ++a)
      at[a] = l.origin[a];
    std::array<unsigned, 3> selection{};
    for (unsigned c = 0; c < 3; ++c)
      selection[c] = static_cast<unsigned>(selected[c]);
    const unsigned run = axis - 1;
    std::uint64_t rows = 1;
    for (unsigned a = 0; a < run; ++a)
      rows *= l.shape[a];
    // Process complete tuples only; fragmented channel tiles use the general
    // path. Prove this before writing anything.
    for (std::uint64_t row = 0; row < rows; ++row) {
      for (at[run] = l.origin[run]; at[run] < l.origin[run] + l.shape[run];) {
        const auto& v = reader.view(0, 0, at);
        if (v.layout->origin[axis] != 0 || v.layout->shape[axis] != C ||
            v.layout->strides[axis] != sizeof(T) ||
            v.layout->strides[run] != C * sizeof(T) ||
            l.strides[axis] != sizeof(T) ||
            l.strides[run] != (op.kind == 3 ? 1 : C) * sizeof(T) ||
            reinterpret_cast<std::uintptr_t>(address<T>(v, at)) % alignof(T) ||
            reinterpret_cast<std::uintptr_t>(address<T>(out, at)) % alignof(T))
          return false;
        at[run] = std::min(l.origin[run] + l.shape[run],
                           v.layout->origin[run] + v.layout->shape[run]);
      }
      for (unsigned a = run; a-- > 0;) {
        if (++at[a] < l.origin[a] + l.shape[a])
          break;
        at[a] = l.origin[a];
      }
    }
    for (unsigned a = 0; a < l.rank; ++a)
      at[a] = l.origin[a];
    for (std::uint64_t row = 0; row < rows; ++row) {
      for (at[run] = l.origin[run]; at[run] < l.origin[run] + l.shape[run];) {
        cancel(inv);
        const auto& v = reader.view(0, 0, at);
        const auto n = std::min<std::uint64_t>(
            {4096, l.origin[run] + l.shape[run] - at[run],
             v.layout->origin[run] + v.layout->shape[run] - at[run]});
        op.span<T, C>(address<T>(v, at), address<T>(out, at), n, selection);
        at[run] += n;
      }
      for (unsigned a = run; a-- > 0;) {
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
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& out = inv.outputs[o];
      std::uint64_t count = 1;
      std::vector<RegionDimension> rd;
      for (unsigned a = 0; a < out.layout->rank; ++a) {
        count *= out.layout->shape[a];
        rd.push_back({out.layout->origin[a], out.layout->shape[a]});
      }
      if (!count)
        continue;
      work.poll(count * 64);
      auto op = transform;
      if (calculates(Region(rd)) && (op.kind == 0 || op.kind == 3)) {
        Coordinate q{};
        for (unsigned c = 0; c < 3; ++c) {
          q[0] = c;
          (op.kind == 0 ? op.gains : op.weights)[c] = reader.value(1, 0, q);
        }
        if (op.kind == 3) {
          op.bias = reader.value(2, 0, Coordinate{});
          op.divisor =
              normalize ? (op.weights[0] + op.weights[1]) + op.weights[2] : 1;
        }
      }
      if ((dims[axis] == 3 && packed<T, 3>(reader, inv, out, op)) ||
          (dims[axis] == 4 && packed<T, 4>(reader, inv, out, op)))
        continue;
      samples(out, [&](const Coordinate& at, std::uint64_t i) {
        if ((i & 255U) == 0)
          cancel(inv);
        const auto c = component(at[axis]);
        if (op.kind != 3 &&
            (c < 0 || op.identity || (op.kind == 0 && op.gains[c] == 1))) {
          store(out, at, reader.raw(0, 0, at));
        } else if (op.kind == 7) {
          store(out, at,
                convert(op.posterize(static_cast<T>(reader.value(0, 0, at))),
                        sizeof(T) == 4));
        } else if (op.kind == 0) {
          store(out, at,
                convert(reader.value(0, 0, at) * op.gains[c], sizeof(T) == 4));
        } else {
          Triple rgb{};
          auto q = at;
          for (unsigned j = 0; j < 3; ++j) {
            q[axis] = selected[j];
            rgb[j] = reader.value(0, 0, q);
          }
          const auto result = op.apply(rgb);
          store(out, at, convert(result[op.kind == 3 ? 0 : c], sizeof(T) == 4));
        }
      });
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
    if (type == ElementType::Float32)
      compute<float>(k, inv.views, Work{inv.views, work_limit, 0, inv.work});
    else
      compute<double>(k, inv.views, Work{inv.views, work_limit, 0, inv.work});
  }
};
OperationDefinition operation(unsigned kind) {
  using Type = OperationParameterType;
  static constexpr const char* names[] = {"white_balance_gains",
                                          "white_balance_adapt",
                                          "white_balance_temperature_tint",
                                          "monochrome_weights",
                                          "monochrome_linear_y",
                                          "monochrome_cielab",
                                          "monochrome_oklab",
                                          "posterize"};
  std::vector<OperationParameterSpec> p{
      {"axis", Type::Int64, false},
      {"components", Type::String, false},
      {"input_description", Type::String, false},
      {"group", Type::String, false}};
  if (kind == 1 || kind == 2 || (kind >= 4 && kind <= 6))
    p.push_back({"source_basis", Type::String, false});
  if (kind == 1 || kind == 2) {
    p.push_back({"target_white", Type::String, kind == 1});
    p.push_back({"adaptation", Type::String, kind == 1});
  }
  if (kind == 1) {
    p.push_back({"source_white", Type::String, true});
    p.push_back({"destination_basis", Type::String, false});
    p.push_back({"output_description", Type::String, false});
  }
  if (kind == 2) {
    p.push_back({"source_temperature", Type::Float64, true});
    p.push_back({"source_tint", Type::Float64, false});
  }
  if (kind == 3)
    p.push_back({"normalize", Type::Bool, false});
  if (kind == 7) {
    p.push_back({"levels", Type::Int64, false});
    p.push_back({"edge", Type::String, false});
  }
  return budgeted_operation(generation::operation(
      std::string("grade.") + names[kind],
      kind == 0   ? 2
      : kind == 3 ? 3
                  : 1,
      std::move(p), {{"values", tensor_schema(ElementType::Float64, {1})}},
      [kind](const auto& inputs, const auto& params) {
        return std::make_shared<Adjustment>(kind, inputs, params);
      }));
}
}  // namespace ps::plugin_internal::grade::finishing
namespace ps::plugin_internal {
Status register_grade_finishing(OperationRegistry* registry) {
  for (unsigned kind = 0; kind < 8; ++kind) {
    const auto status =
        registry->register_operation(grade::finishing::operation(kind));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
