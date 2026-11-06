// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "02-format-color/rgb_basis_math.hpp"
#include "04-mask-morphology/common.hpp"
#include "07-grade/curves_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade::curves {
using namespace generation;  // NOLINT(build/namespaces)

fast_color::Matrix native_matrix(const basis_ops::Matrix& matrix) {
  fast_color::Matrix result{};
  for (unsigned r = 0; r < 3; ++r) {
    result[r] = basis_ops::ExactRow(matrix, r).approximate;
  }
  return result;
}
struct Adjustment final : Program {
  unsigned member, axis;
  bool pchip;
  ElementType type;
  std::uint64_t knots, columns;
  fast_curve::Edge edge;
  std::vector<std::uint64_t> dims, selected;
  std::vector<std::pair<std::uint64_t, unsigned>> rows;
  Brightness brightness;

  Adjustment(unsigned kind, const std::vector<OperationMetadata>& input,
             const Parameters& p)
      : member(kind) {
    const auto& source = tensor(input.at(0), true);
    dims = source.sample_shape();
    type = source.descriptor.element_type;
    require(floating(type), "GRD05 image requires Float32/64");
    pchip = member == 1;
    if (member >= 2) {
      const auto method = text(p, "method");
      require(method == "linear" || method == "pchip",
              "GRD05 brightness requires method=linear|pchip");
      pchip = method == "pchip";
    }
    const auto policy = text(p, "edge", "reject");
    require(policy == "reject" || policy == "clamp" ||
                policy == "linear_extrapolate",
            "invalid GRD05 edge");
    edge = policy == "clamp"                ? fast_curve::Edge::Clamp
           : policy == "linear_extrapolate" ? fast_curve::Edge::Linear
                                            : fast_curve::Edge::Reject;
    try {
      color_point::Selection selection;
      if (member >= 2 || p.count("input_description")) {
        selection = color_point::select(
            input[0], p,
            member >= 2 ? std::vector<std::string>{"red", "green", "blue"}
                        : std::vector<std::string>{});
        color_point::require(selection.axis >= 0,
                             "GRD05 component axis required");
        axis = static_cast<unsigned>(selection.axis);
        selected = selection.channels;
      } else {
        require(p.count("axis") && p.count("components"),
                "GRD05 requires axis and components");
        const auto a = integer(p, "axis");
        require(a >= 0 && static_cast<std::uint64_t>(a) <
                              source.descriptor.shape.size(),
                "GRD05 axis outside tensor rank");
        axis = static_cast<unsigned>(a + source.batch_axes.size());
        selected = mask::channels(text(p, "components"), dims[axis]);
        if (source.layout.spatial) {
          require(source.layout.channel_axis == std::optional<std::uint32_t>(a),
                  "GRD05 selector must use spatial channel axis");
        }
      }
      if (member >= 2) {
        const auto& interpretation = selection.interpretation;
        if (selection.semantic) {
          const auto& reference = interpretation.reference;
          color_point::typed(
              interpretation.model == "rgb" &&
                  interpretation.transfer == "linear" &&
                  (reference == "scene" || reference == "display" ||
                   reference == "scene_relative" ||
                   reference == "display_relative"),
              "GRD05 brightness requires relative linear RGB with an explicit "
              "scene/display reference");
          for (const auto& unit : selection.units) {
            color_point::typed(unit == "1" || unit == "relative",
                               "GRD05 brightness requires relative RGB units");
          }
          color_point::typed(
              !interpretation.coordinates ||
                  interpretation.coordinates->scale.empty() ||
                  interpretation.coordinates->scale == "relative",
              "GRD05 brightness requires relative coordinate scale");
        }
        format::RgbBasis assertion;
        assertion.preset = interpretation.primaries;
        assertion.primaries_xy = interpretation.primaries_xy;
        assertion.white = interpretation.white;
        const auto basis = p.count("source_basis")
                               ? basis_ops::parse_basis(text(p, "source_basis"))
                               : basis_ops::resolve(assertion);
        color_point::require(
            interpretation.primaries.empty() ||
                basis_ops::preset(interpretation.primaries).xy == basis.xy,
            "GRD05 basis assertion disagrees");
        color_point::require(!interpretation.primaries_xy ||
                                 *interpretation.primaries_xy == basis.xy,
                             "GRD05 primary assertion disagrees");
        color_point::require(
            !interpretation.white || *interpretation.white == basis.white,
            "GRD05 white assertion disagrees");
        const auto forward = basis_ops::rgb_matrix(basis.xy, basis.white);
        brightness.route = member - 2;
        brightness.forward = native_matrix(forward);
        brightness.inverse = native_matrix(basis_ops::inverse(forward));
        const std::array<double, 2> d65{.3127, .3290};
        const auto adaptation = basis_ops::adaptation(basis.white, d65, 1);
        brightness.adaptation = native_matrix(adaptation);
        brightness.unadaptation = native_matrix(basis_ops::inverse(adaptation));
        brightness.white = {
            basis.white[0] / basis.white[1], 1,
            (1 - basis.white[0] - basis.white[1]) / basis.white[1]};
        brightness.prepare();
      }
    } catch (const basis_ops::CheckedFailure& failure) {
      throw failure.status;
    } catch (const basis_ops::GeometryError&) {
      throw Status{ErrorCode::InvalidArgument, "GRD05 invalid RGB basis"};
    }
    require(!selected.empty(), "GRD05 requires selected components");
    const auto& x = tensor(input.at(1));
    const auto& y = tensor(input.at(2));
    require(x.descriptor.element_type == ElementType::Float64 &&
                x.sample_shape().size() == 1,
            "GRD05 knots require Float64[K]");
    knots = x.sample_shape()[0];
    columns = member >= 2 ? 1 : selected.size();
    require(
        knots >= 2 && knots <= 65536 &&
            y.sample_shape() == std::vector<std::uint64_t>{knots, columns} &&
            floating(y.descriptor.element_type) &&
            (member < 2 || y.descriptor.element_type == ElementType::Float64),
        "GRD05 ordinate shape/dtype");
    for (unsigned i = 0; i < selected.size(); ++i) {
      rows.emplace_back(selected[i], i);
    }
    std::sort(rows.begin(), rows.end());
    scratch_bytes = columns * sizeof(fast_curve::Curve) +
                    (1 + 5 * columns) * knots * sizeof(double);
    auto output = *input[0].result_schema;
    output.id = "photospider.tensor";
    output.version = 1;
    output.tensors[0].key = "samples";
    output.tensors[0].atomic_trailing_axes = 0;
    add_output(this, std::move(output));
  }

  bool calculates(const Region& region) const {
    if (region.dimensions().empty()) {
      return false;
    }
    for (const auto& d : region.dimensions()) {
      if (!d.extent) {
        return false;
      }
    }
    const auto c = region.dimensions()[axis];
    return std::any_of(selected.begin(), selected.end(), [&](auto s) {
      return s >= c.offset && s - c.offset < c.extent;
    });
  }
  std::uint64_t task_scratch(unsigned, const Region& region) const override {
    return calculates(region) ? scratch_bytes : 0;
  }
  std::vector<ReadRegion> support(unsigned,
                                  const Region& region) const override {
    if (region.dimensions().empty()) {
      return {};
    }
    for (const auto& d : region.dimensions()) {
      if (!d.extent) {
        return {};
      }
    }
    std::vector<ReadRegion> reads{{0, 0, region}};
    if (!calculates(region)) {
      return reads;
    }
    if (member >= 2) {
      for (auto c : selected) {
        auto shape = region.dimensions();
        shape[axis] = {c, 1};
        reads.push_back({0, 0, Region(std::move(shape))});
      }
    }
    reads.push_back({1, 0, Region({{0, knots}})});
    reads.push_back({2, 0, Region({{0, knots}, {0, columns}})});
    return reads;
  }
  int row(std::uint64_t c) const {
    const auto found = std::lower_bound(
        rows.begin(), rows.end(), c,
        [](const auto& item, auto value) { return item.first < value; });
    return found == rows.end() || found->first != c ? -1 : found->second;
  }
  fast_curve::Curve* prepare_curves(const mask::Reader& reader, void* data,
                                    const Work& work) const {
    work.poll(knots * columns * 64);
    auto* curves = static_cast<fast_curve::Curve*>(data);
    auto* x = reinterpret_cast<double*>(curves + columns);
    Coordinate at{};
    for (std::uint64_t i = 0; i < knots; ++i) {
      at[0] = i;
      x[i] = reader.value(1, 0, at);
    }
    for (std::uint64_t c = 0; c < columns; ++c) {
      auto* y = x + knots + c * 5 * knots;
      at[1] = c;
      for (std::uint64_t i = 0; i < knots; ++i) {
        at[0] = i;
        y[i] = reader.value(2, 0, at);
      }
      new (curves + c) fast_curve::Curve;
      curves[c].prepare(x, y, knots, pchip, edge, y + knots);
    }
    return curves;
  }
  static void poll(const ps_kernel_invocation_v8& inv) {
    if (inv.cancellation.is_cancelled &&
        inv.cancellation.is_cancelled(inv.cancellation.context)) {
      throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
    }
  }

  template <class T, unsigned C>
  bool contiguous(const mask::Reader& reader,
                  const ps_kernel_invocation_v8& inv,
                  const ps_kernel_mutable_view_v8& output,
                  const fast_curve::Curve* curves) const {
    if (axis + 1 != dims.size() || axis == 0 ||
        output.layout->origin[axis] != 0 || output.layout->shape[axis] != C ||
        output.layout->strides[axis] != sizeof(T) ||
        output.layout->strides[axis - 1] != C * sizeof(T)) {
      return false;
    }
    if (selected.size() != C && !(C == 4 && selected.size() == 3)) {
      return false;
    }
    for (unsigned c = 0; c < selected.size(); ++c) {
      if (selected[c] != c) {
        return false;
      }
    }
    const unsigned line = axis - 1;
    Coordinate at{};
    std::uint64_t rows_count = 1;
    for (unsigned j = 0; j < axis; ++j) {
      at[j] = output.layout->origin[j];
      if (j != line) {
        rows_count *= output.layout->shape[j];
      }
    }
    const auto end = output.layout->origin[line] + output.layout->shape[line];
    for (std::uint64_t r = 0; r < rows_count; ++r) {
      for (at[line] = output.layout->origin[line]; at[line] < end;) {
        poll(inv);
        const auto& input = reader.view(0, 0, at);
        if (input.layout->origin[axis] != 0 || input.layout->shape[axis] != C ||
            input.layout->strides[axis] != sizeof(T) ||
            input.layout->strides[line] != C * sizeof(T)) {
          return false;
        }
        const auto n =
            std::min<std::uint64_t>({4096, end - at[line],
                                     input.layout->origin[line] +
                                         input.layout->shape[line] - at[line]});
        const auto* source = reinterpret_cast<const T*>(
            static_cast<const unsigned char*>(input.allocation) +
            generation::offset(*input.layout, input.byte_offset, at));
        auto* target = reinterpret_cast<T*>(
            static_cast<unsigned char*>(output.allocation) +
            generation::offset(*output.layout, output.byte_offset, at));
        if (reinterpret_cast<std::uintptr_t>(source) % alignof(T) ||
            reinterpret_cast<std::uintptr_t>(target) % alignof(T)) {
          return false;
        }
        if (member >= 2) {
          if constexpr (C >= 3) {
            brightness.span<T, C>(source, target, n, curves[0]);
          }
        } else {
          component_span<T, C>(source, target, n, curves, selected.size());
        }
        at[line] += n;
      }
      for (unsigned j = line; j-- > 0;) {
        if (++at[j] < output.layout->origin[j] + output.layout->shape[j]) {
          break;
        }
        at[j] = output.layout->origin[j];
      }
    }
    return true;
  }

  template <class T>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    fast_curve::Curve* curves = nullptr;
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& output = inv.outputs[o];
      std::vector<RegionDimension> dimensions;
      std::uint64_t count = 1;
      for (unsigned j = 0; j < output.layout->rank; ++j) {
        dimensions.push_back(
            {output.layout->origin[j], output.layout->shape[j]});
        count *= output.layout->shape[j];
      }
      if (!count) {
        continue;
      }
      const bool arithmetic = calculates(Region(std::move(dimensions)));
      work.poll(count * (member >= 2 ? 256 : 48));
      if (arithmetic && !curves) {
        curves = prepare_curves(reader, inv.scratch[0].allocation, work);
      }
      if (arithmetic) {
        if (dims[axis] == 1 && contiguous<T, 1>(reader, inv, output, curves)) {
          continue;
        }
        if (dims[axis] == 3 && contiguous<T, 3>(reader, inv, output, curves)) {
          continue;
        }
        if (dims[axis] == 4 && contiguous<T, 4>(reader, inv, output, curves)) {
          continue;
        }
      }
      Coordinate at{};
      std::uint64_t points = 1;
      for (unsigned j = 0; j < output.layout->rank; ++j) {
        at[j] = output.layout->origin[j];
        if (j != axis) {
          points *= output.layout->shape[j];
        }
      }
      const auto first = output.layout->origin[axis];
      const auto end = first + output.layout->shape[axis];
      for (std::uint64_t i = 0; i < points; ++i) {
        if (!(i % 256)) {
          poll(inv);
        }
        fast_color::Triple result{};
        bool changed = false;
        if (arithmetic && member >= 2) {
          fast_color::Triple rgb{};
          for (unsigned c = 0; c < 3; ++c) {
            at[axis] = selected[c];
            rgb[c] = reader.value(0, 0, at);
          }
          result = brightness.apply(rgb, curves[0], &changed);
        }
        for (auto c = first; c < end; ++c) {
          at[axis] = c;
          const int selected_row = row(c);
          std::uint64_t value;
          if (selected_row < 0 || (member >= 2 && !changed)) {
            value = reader.raw(0, 0, at);
          } else if (member >= 2) {
            value = generation::convert(result[selected_row], sizeof(T) == 4);
          } else {
            value = generation::convert(
                curves[selected_row].evaluate(reader.value(0, 0, at)),
                sizeof(T) == 4);
          }
          store(output, at, value);
        }
        for (unsigned j = output.layout->rank; j-- > 0;) {
          if (j == axis) {
            continue;
          }
          if (++at[j] < output.layout->origin[j] + output.layout->shape[j]) {
            break;
          }
          at[j] = output.layout->origin[j];
        }
      }
    }
    poll(inv);
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    if (type == ElementType::Float32) {
      compute<float>(kernel, inv, Work{inv, work_limit});
    } else {
      compute<double>(kernel, inv, Work{inv, work_limit});
    }
  }
  void run_v9(const Kernel& kernel,
              const ps_kernel_invocation_v9& inv) const override {
    if (type == ElementType::Float32) {
      compute<float>(kernel, inv.views,
                     Work{inv.views, work_limit, 0, inv.work});
    } else {
      compute<double>(kernel, inv.views,
                      Work{inv.views, work_limit, 0, inv.work});
    }
  }
};

OperationDefinition operation(unsigned member) {
  static constexpr const char* names[]{
      "grade.curves_linear", "grade.curves_pchip",    "grade.curves_luminance",
      "grade.curves_xyz_y",  "grade.curves_cielab_l", "grade.curves_oklab_l"};
  using Type = OperationParameterType;
  std::vector<OperationParameterSpec> parameters{
      {"axis", Type::Int64, false},
      {"components", Type::String, false},
      {"input_description", Type::String, false},
      {"group", Type::String, false},
      {"edge", Type::String, false}};
  if (member >= 2) {
    parameters.push_back({"source_basis", Type::String, false});
    parameters.push_back({"method", Type::String, true});
  }
  auto definition = budgeted_operation(generation::operation(
      names[member], 3, std::move(parameters),
      {{"values", tensor_schema(ElementType::Float64, {1})}},
      [member](const auto& inputs, const auto& parameters) {
        return std::make_shared<Adjustment>(member, inputs, parameters);
      }));
  definition.reusable_tiles = true;
  return definition;
}
}  // namespace ps::plugin_internal::grade::curves
namespace ps::plugin_internal {
Status register_grade_curves(OperationRegistry* registry) {
  for (unsigned member = 0; member < 6; ++member) {
    const auto status =
        registry->register_operation(grade::curves::operation(member));
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
