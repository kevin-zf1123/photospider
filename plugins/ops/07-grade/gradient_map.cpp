// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "02-format-color/rgb_basis_math.hpp"
#include "04-mask-morphology/common.hpp"
#include "07-grade/gradient_map_math.hpp"
#include "common/registry.hpp"
#include "photospider/ops/numeric/color_description.hpp"

namespace ps::plugin_internal::grade::gradient {
using namespace generation;  // NOLINT(build/namespaces)
fast_color::Matrix native_matrix(const basis_ops::Matrix& matrix) {
  fast_color::Matrix result{};
  for (unsigned r = 0; r < 3; ++r)
    result[r] = basis_ops::ExactRow(matrix, r).approximate;
  return result;
}
struct Mapping final : Program {
  bool image;
  unsigned axis = 0;
  ElementType type;
  std::uint64_t knots;
  std::vector<std::uint64_t> dims, output_dims, selected;
  Feature feature;
  Mapping(bool from_image, const std::vector<OperationMetadata>& input,
          const Parameters& p)
      : image(from_image) {
    const auto& source = tensor(input.at(0), true);
    dims = source.sample_shape();
    type = source.descriptor.element_type;
    require(floating(type), "GRD06 image requires Float32/64");
    const auto description =
        take(tensor_description_from_parameter(text(p, "color_description")));
    const auto destination = take(numeric::resolve_color_tuple(description));
    require(destination.interpretation.model == "rgb" && !destination.alpha,
            "GRD06A/B destination requires RGB without alpha");
    const auto policy = text(p, "edge", "clamp");
    require(policy == "clamp" || policy == "reject", "GRD06A/B edge");
    if (!image) {
      auto selectors = p;
      if (!p.count("input_description") && p.count("axis") &&
          !p.count("components"))
        selectors["components"] = std::string("0");
      const auto selection =
          color_point::select(input[0], selectors, {}, false);
      require(selection.axis >= 0 &&
                  selection.channels == std::vector<std::uint64_t>{0},
              "GRD06A requires one explicit feature component");
      axis = selection.axis;
      require(dims[axis] == 1, "GRD06A requires one feature component");
      selected = {0};
    } else {
      const auto selection =
          color_point::select(input[0], p, {"red", "green", "blue"});
      color_point::require(
          selection.axis >= 0 && selection.channels.size() == 3,
          "GRD06B requires a complete RGB group");
      axis = selection.axis;
      selected = selection.channels;
      const auto name = text(p, "feature_source");
      require(name == "max_rgb" || name == "linear_y" || name == "cielab_l" ||
                  name == "oklab_l",
              "GRD06B feature_source");
      feature.mode = name == "max_rgb"    ? 0
                     : name == "linear_y" ? 1
                     : name == "cielab_l" ? 2
                                          : 3;
      const auto& meaning = selection.interpretation;
      if (selection.semantic)
        require(meaning.model == "rgb", "GRD06B source must describe RGB");
      if (feature.mode) {
        if (selection.semantic) {
          const auto& ref = meaning.reference;
          require(meaning.transfer == "linear" &&
                      (ref == "scene" || ref == "display" ||
                       ref == "scene_relative" || ref == "display_relative"),
                  "GRD06B brightness requires relative linear RGB");
          for (const auto& unit : selection.units)
            require(unit == "1" || unit == "relative", "GRD06B RGB units");
          require(!meaning.coordinates || meaning.coordinates->scale.empty() ||
                      meaning.coordinates->scale == "relative",
                  "GRD06B coordinate scale");
        }
        format::RgbBasis assertion;
        assertion.preset = meaning.primaries;
        assertion.primaries_xy = meaning.primaries_xy;
        assertion.white = meaning.white;
        try {
          const auto basis =
              p.count("source_basis")
                  ? basis_ops::parse_basis(text(p, "source_basis"))
                  : basis_ops::resolve(assertion);
          require(meaning.primaries.empty() ||
                      basis_ops::preset(meaning.primaries).xy == basis.xy,
                  "GRD06B basis disagrees with description");
          require(!meaning.primaries_xy || *meaning.primaries_xy == basis.xy,
                  "GRD06B primary assertion");
          require(!meaning.white || *meaning.white == basis.white,
                  "GRD06B white assertion");
          feature.rgb_to_xyz =
              native_matrix(basis_ops::rgb_matrix(basis.xy, basis.white));
          feature.rgb_to_lms = fast_color::multiply(
              fast_color::kXyzToLms,
              fast_color::multiply(native_matrix(basis_ops::adaptation(
                                       basis.white, {.3127, .3290}, 1)),
                                   feature.rgb_to_xyz));
        } catch (const basis_ops::CheckedFailure& f) {
          throw f.status;
        } catch (const basis_ops::GeometryError&) {
          throw Status{ErrorCode::InvalidArgument, "GRD06B invalid RGB basis"};
        }
      }
    }
    if (source.layout.spatial)
      require(source.layout.channel_axis ==
                  std::optional<std::uint32_t>(axis - source.batch_axes.size()),
              "GRD06 component selector must use spatial channel axis");
    const auto& x = tensor(input.at(1), true);
    const auto& y = tensor(input.at(2), true);
    require(x.descriptor.element_type == ElementType::Float64 &&
                x.sample_shape().size() == 1,
            "GRD06 knots require Float64[K]");
    knots = x.sample_shape()[0];
    require(knots >= 2 && knots <= 65536 &&
                floating(y.descriptor.element_type) &&
                y.sample_shape() == std::vector<std::uint64_t>{knots, 3},
            "GRD06 colors require Float32/64[K,3]");
    scratch_bytes = 3 * sizeof(fast_curve::Curve) + 16 * knots * sizeof(double);
    auto output = *input[0].result_schema;
    output.id = "photospider.tensor";
    output.version = 1;
    output.tensors[0].key = "samples";
    output.tensors[0].atomic_trailing_axes = 0;
    if (!image)
      output.tensors[0].descriptor.shape[axis - source.batch_axes.size()] = 3;
    output_dims = output.tensors[0].sample_shape();
    size_product(output_dims);
    add_output(this, std::move(output));
  }
  static bool empty(const Region& r) {
    return r.dimensions().empty() ||
           std::any_of(r.dimensions().begin(), r.dimensions().end(),
                       [](const auto& d) { return !d.extent; });
  }
  bool calculates(const Region& r) const {
    if (empty(r))
      return false;
    if (!image)
      return true;
    const auto d = r.dimensions()[axis];
    return std::any_of(selected.begin(), selected.end(), [&](auto c) {
      return c >= d.offset && c - d.offset < d.extent;
    });
  }
  std::uint64_t task_scratch(unsigned, const Region& r) const override {
    return calculates(r) ? scratch_bytes : 0;
  }
  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (empty(r))
      return {};
    std::vector<ReadRegion> reads;
    if (image)
      reads.push_back({0, 0, r});
    if (!calculates(r))
      return reads;
    for (auto c : selected) {
      auto q = r.dimensions();
      q[axis] = {c, 1};
      reads.push_back({0, 0, Region(std::move(q))});
    }
    reads.push_back({1, 0, Region({{0, knots}})});
    reads.push_back({2, 0, Region({{0, knots}, {0, 3}})});
    return reads;
  }
  static void cancel(const ps_kernel_invocation_v8& inv) {
    if (inv.cancellation.is_cancelled &&
        inv.cancellation.is_cancelled(inv.cancellation.context))
      throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
  }
  fast_curve::Curve* prepare_curves(const mask::Reader& reader, void* data,
                                    const Work& work) const {
    work.poll(knots * 192);
    auto* curves = static_cast<fast_curve::Curve*>(data);
    auto* x = reinterpret_cast<double*>(curves + 3);
    Coordinate at{};
    for (std::uint64_t i = 0; i < knots; ++i) {
      at[0] = i;
      x[i] = reader.value(1, 0, at);
    }
    for (unsigned c = 0; c < 3; ++c) {
      auto* y = x + knots + c * 5 * knots;
      at[1] = c;
      for (std::uint64_t i = 0; i < knots; ++i) {
        at[0] = i;
        y[i] = reader.value(2, 0, at);
      }
      new (curves + c) fast_curve::Curve;
      curves[c].prepare(x, y, knots, false, fast_curve::Edge::Clamp, y + knots);
    }
    return curves;
  }
  template <class T>
  bool contiguous(const mask::Reader& reader,
                  const ps_kernel_invocation_v8& inv,
                  const ps_kernel_mutable_view_v8& output,
                  const fast_curve::Curve* curves) const {
    if (!axis || axis + 1 != dims.size() || output.layout->origin[axis] ||
        output.layout->shape[axis] != output_dims[axis] ||
        output.layout->strides[axis] != sizeof(T) ||
        output.layout->strides[axis - 1] != output_dims[axis] * sizeof(T))
      return false;
    if (image && (selected != std::vector<std::uint64_t>{0, 1, 2} ||
                  (dims[axis] != 3 && dims[axis] != 4)))
      return false;
    const unsigned line = axis - 1;
    Coordinate at{};
    std::uint64_t rows = 1;
    for (unsigned a = 0; a < axis; ++a) {
      at[a] = output.layout->origin[a];
      if (a != line)
        rows *= output.layout->shape[a];
    }
    const auto end = at[line] + output.layout->shape[line];
    for (std::uint64_t r = 0; r < rows; ++r) {
      for (at[line] = output.layout->origin[line]; at[line] < end;) {
        cancel(inv);
        const auto& input = reader.view(0, 0, at);
        if (input.layout->origin[axis] ||
            input.layout->shape[axis] != dims[axis] ||
            input.layout->strides[axis] != sizeof(T) ||
            input.layout->strides[line] != dims[axis] * sizeof(T))
          return false;
        const auto n =
            std::min<std::uint64_t>({4096, end - at[line],
                                     input.layout->origin[line] +
                                         input.layout->shape[line] - at[line]});
        const auto* source = reinterpret_cast<const T*>(
            static_cast<const unsigned char*>(input.allocation) +
            offset(*input.layout, input.byte_offset, at));
        auto* target = reinterpret_cast<T*>(
            static_cast<unsigned char*>(output.allocation) +
            offset(*output.layout, output.byte_offset, at));
        if (reinterpret_cast<std::uintptr_t>(source) % alignof(T) ||
            reinterpret_cast<std::uintptr_t>(target) % alignof(T))
          return false;
        if (!image)
          feature_span(source, target, n, curves);
        else if (dims[axis] == 3)
          image_span<T, 3>(source, target, n, feature, curves);
        else
          image_span<T, 4>(source, target, n, feature, curves);
        at[line] += n;
      }
      for (unsigned a = line; a-- > 0;) {
        if (++at[a] < output.layout->origin[a] + output.layout->shape[a])
          break;
        at[a] = output.layout->origin[a];
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
      std::vector<RegionDimension> region;
      std::uint64_t points = 1;
      Coordinate at{};
      for (unsigned a = 0; a < output.layout->rank; ++a) {
        region.push_back({output.layout->origin[a], output.layout->shape[a]});
        at[a] = output.layout->origin[a];
        if (a != axis)
          points *= output.layout->shape[a];
      }
      if (!points || !output.layout->shape[axis])
        continue;
      const bool arithmetic = calculates(Region(std::move(region)));
      work.poll(points * output.layout->shape[axis] * 128);
      if (arithmetic && !curves)
        curves = prepare_curves(reader, inv.scratch[0].allocation, work);
      if (arithmetic && contiguous<T>(reader, inv, output, curves))
        continue;
      const auto first = output.layout->origin[axis],
                 end = first + output.layout->shape[axis];
      for (std::uint64_t i = 0; i < points; ++i) {
        if (!(i % 1024))
          cancel(inv);
        fast_color::Triple result{};
        if (arithmetic) {
          double q;
          if (image) {
            fast_color::Triple rgb;
            for (unsigned c = 0; c < 3; ++c) {
              at[axis] = selected[c];
              rgb[c] = reader.value(0, 0, at);
            }
            q = feature(rgb);
          } else {
            at[axis] = 0;
            q = reader.value(0, 0, at);
          }
          result = evaluate(curves, q);
        }
        for (auto c = first; c < end; ++c) {
          at[axis] = c;
          const auto found = std::find(selected.begin(), selected.end(), c);
          if (image && found == selected.end())
            store(output, at, reader.raw(0, 0, at));
          else
            store(output, at,
                  convert(result[image ? found - selected.begin() : c],
                          sizeof(T) == 4));
        }
        for (unsigned a = output.layout->rank; a-- > 0;) {
          if (a == axis)
            continue;
          if (++at[a] < output.layout->origin[a] + output.layout->shape[a])
            break;
          at[a] = output.layout->origin[a];
        }
      }
    }
    cancel(inv);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    if (type == ElementType::Float32)
      compute<float>(k, i, Work{i, work_limit});
    else
      compute<double>(k, i, Work{i, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& i) const override {
    if (type == ElementType::Float32)
      compute<float>(k, i.views, Work{i.views, work_limit, 0, i.work});
    else
      compute<double>(k, i.views, Work{i.views, work_limit, 0, i.work});
  }
};
OperationDefinition operation(bool image) {
  using Type = OperationParameterType;
  std::vector<OperationParameterSpec> parameters{
      {"axis", Type::Int64, false},
      {"components", Type::String, false},
      {"input_description", Type::String, false},
      {"group", Type::String, false},
      {"color_description", Type::String, true},
      {"edge", Type::String, false}};
  if (image) {
    parameters.push_back({"source_basis", Type::String, false});
    parameters.push_back({"feature_source", Type::String, true});
  }
  return budgeted_operation(generation::operation(
      image ? "grade.gradient_map_image" : "grade.gradient_map", 3,
      std::move(parameters),
      {{"values", tensor_schema(ElementType::Float64, {1, 3})}},
      [image](const auto& inputs, const auto& p) {
        try {
          return std::make_shared<Mapping>(image, inputs, p);
        } catch (const basis_ops::CheckedFailure& failure) {
          throw failure.status;
        }
      }));
}
}  // namespace ps::plugin_internal::grade::gradient
namespace ps::plugin_internal {
Status register_grade_gradient_map(OperationRegistry* registry) {
  for (bool image : {false, true}) {
    const auto status =
        registry->register_operation(grade::gradient::operation(image));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
