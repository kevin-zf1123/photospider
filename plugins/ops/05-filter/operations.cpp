#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "05-filter/advanced.hpp"
#include "05-filter/common.hpp"
#include "05-filter/effects.hpp"
#include "05-filter/fields.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::filter {
namespace {
std::uint64_t kernel_extent(const std::vector<OperationMetadata>& in,
                            const Parameters& p, Kind kind, bool y) {
  if (kind == Kind::BoxSum || kind == Kind::BoxMean) {
    const auto value = integer(p, y ? "height" : "width");
    parameter(value > 0, "positive box extent required");
    return static_cast<std::uint64_t>(value);
  }
  if (kind == Kind::Gaussian) {
    const auto radius = integer(p, y ? "radius_y" : "radius_x");
    parameter(radius >= 0 && radius <= (INT64_C(1) << 39) - 1,
              "Gaussian static radius overflow");
    parameter(real(p, y ? "sigma_y" : "sigma_x") != 0 || radius == 0,
              "zero sigma requires zero radius");
    return 2 * static_cast<std::uint64_t>(radius) + 1;
  }
  const bool separable = kind == Kind::Separable || kind == Kind::Correlation;
  const auto port = separable                  ? (y ? 2U : 1U)
                    : kind == Kind::Normalized ? 2U
                                               : 1U;
  const auto& s = tensor(in.at(port));
  require(s.descriptor.element_type == ElementType::Float64 &&
              s.sample_shape().size() == (separable ? 1U : 2U),
          "filter kernel requires Float64 vector/table");
  size_product(s.sample_shape());
  return s.sample_shape()[separable ? 0 : y ? 0 : 1];
}
SchemaTemplate output_schema(const OperationMetadata& input, const Geometry& g,
                             ElementType type) {
  SchemaTemplate s = *input.result_schema;
  s.id = "photospider.tensor";
  s.version = 1;
  auto& t = s.tensors[0];
  t.key = "samples";
  t.descriptor.element_type = type;
  for (unsigned a = 0; a < g.dims.size(); ++a) {
    if (a < t.batch_axes.size()) {
      t.batch_axes[a] = g.dims[a];
    } else {
      t.descriptor.shape[a - t.batch_axes.size()] = g.dims[a];
    }
  }
  check(s.validate());
  return s;
}
}  // namespace

Filter::Filter(Kind operation, Profile selected,
               const std::vector<OperationMetadata>& in, const Parameters& p)
    : kind(operation),
      profile(selected),
      type(tensor(in.at(0), true).descriptor.element_type),
      geometry(tensor(in.at(0), true), p, kernel_extent(in, p, kind, true),
               kernel_extent(in, p, kind, false), kind) {
  check(numeric_ops::sequence_profile_available(profile));
  normalization = kind == Kind::Gaussian ? "sum"
                  : box() ? (kind == Kind::BoxMean ? "sum" : "none")
                  : kind == Kind::Color || kind == Kind::Normalized
                      ? "sum"
                      : text(p, "normalization");
  parameter(normalization == "none" || normalization == "sum" ||
                normalization == "l1",
            "filter normalization");
  bias = real(p, "bias");
  if (kind == Kind::Gaussian) {
    sx = real(p, "sigma_x");
    sy = real(p, "sigma_y");
  }
  if (kind == Kind::Normalized) {
    const auto& m = tensor(in[1], true);
    const auto& image = tensor(in[0], true);
    require(m.descriptor.element_type == type, "mask dtype mismatch");
    auto md = m.sample_shape(), id = image.sample_shape();
    require(md.size() == id.size(), "mask rank mismatch");
    if (p.count("component_axis")) {
      const auto a = integer(p, "component_axis");
      parameter(a >= 0 && static_cast<std::size_t>(a) < id.size() &&
                    a != geometry.ya && a != geometry.xa,
                "mask component axis");
      component_axis = static_cast<int>(a);
      broadcast = md[a] == 1 && id[a] != 1;
      if (broadcast) {
        md[a] = id[a];
      }
    }
    require(md == id, "mask shape/broadcast mismatch");
    empty = text(p, "empty");
    parameter(empty == "copy_center" || empty == "zero" || empty == "error",
              "normalized filter empty policy");
  }
  if (kind == Kind::Color) {
    try {
      const auto s = color_point::select(in[0], p);
      require(
          s.interpretation.model == "rgb" || s.interpretation.model == "gray",
          "color blur requires linear RGB or Gray");
      require(s.interpretation.transfer == "linear",
              "color blur requires linear transfer");
      color_axis = s.axis;
      require(color_axis < 0 || (color_axis != static_cast<int>(geometry.ya) &&
                                 color_axis != static_cast<int>(geometry.xa)),
              "color component axis cannot be spatial");
      color_channels = s.channels;
      require(
          color_channels.size() == (s.interpretation.model == "rgb" ? 3U : 1U),
          "color blur component count");
      for (const auto& group : s.description->groups) {
        if (group.name == text(p, "group")) {
          alpha = group.alpha;
        }
      }
    } catch (const basis_ops::CheckedFailure& failure) {
      throw failure.status;
    }
  }
  add_output(this, output_schema(in[0], geometry, type));
  if (kind == Kind::Normalized) {
    add_output(this, output_schema(in[0], geometry, ElementType::UInt8));
  }
}
bool Filter::separable() const {
  return kind == Kind::Separable || kind == Kind::Correlation ||
         kind == Kind::Gaussian;
}
bool Filter::box() const {
  return kind == Kind::BoxSum || kind == Kind::BoxMean;
}
bool Filter::selected(const Coordinate& at) const {
  return kind != Kind::Color || color_axis < 0 || alpha_sample(at) ||
         std::find(color_channels.begin(), color_channels.end(),
                   at[color_axis]) != color_channels.end();
}
bool Filter::alpha_sample(const Coordinate& at) const {
  return kind == Kind::Color && alpha && at[color_axis] == *alpha;
}

std::vector<ReadRegion> Filter::support(unsigned port, const Region& q) const {
  std::vector<ReadRegion> result;
  const auto append = [&](unsigned input, const Region& region) {
    for (auto s : geometry.support(region)) {
      if (input == 1 && broadcast) {
        auto d = s.dimensions();
        d[component_axis] = {0, 1};
        s = Region(std::move(d));
      }
      result.push_back({input, 0, std::move(s)});
    }
  };
  if (kind == Kind::Color && color_axis >= 0) {
    // Bit-copy AOV support is local. Selected colors add only their own halo
    // and the associated coverage plane, including alpha-only requests.
    result.push_back({0, 0, q});
    const auto d = q.dimensions()[color_axis];
    bool transformed = false;
    for (auto c : color_channels) {
      if (c < d.offset || c >= d.offset + d.extent) {
        continue;
      }
      auto region = q.dimensions();
      region[color_axis] = {c, 1};
      append(0, Region(region));
      transformed = true;
      if (alpha) {
        region[color_axis] = {*alpha, 1};
        append(0, Region(region));
      }
    }
    if (alpha && *alpha >= d.offset && *alpha < d.offset + d.extent) {
      auto region = q.dimensions();
      region[color_axis] = {*alpha, 1};
      append(0, Region(region));
      transformed = true;
    }
    if (!transformed) {
      return result;
    }
  } else if (!(kind == Kind::Normalized && port == 1)) {
    append(0, q);
  }
  if (kind == Kind::Normalized) {
    append(1, q);
    if (port == 0 && empty == "copy_center") {
      result.push_back({0, 0, q});
    }
  }
  if (!box() && kind != Kind::Gaussian) {
    const auto first = kind == Kind::Normalized ? 2U : 1U;
    result.push_back(
        {first, 0, Region::whole(tensor(inputs[first]).sample_shape())});
    if (separable()) {
      result.push_back({2, 0, Region::whole(tensor(inputs[2]).sample_shape())});
    }
  }
  return result;
}
std::uint64_t Filter::task_scratch(unsigned, const Region& region) const {
  const auto coefficients =
      box()         ? 0
      : separable() ? static_cast<unsigned __int128>(geometry.kh) + geometry.kw
                    : static_cast<unsigned __int128>(geometry.kh) * geometry.kw;
  const auto height =
      static_cast<unsigned __int128>(region.dimensions()[geometry.ya].extent) +
      geometry.kh - 1;
  const auto width = region.dimensions()[geometry.xa].extent;
  const auto rows =
      height *
      (separable() || box()
           ? width
           : static_cast<unsigned __int128>(width) + geometry.kw - 1) *
      sizeof(Fast);
  const auto line =
      separable() || box()
          ? (static_cast<unsigned __int128>(width) + geometry.kw - 1) *
                sizeof(double)
          : 0;
  return bytes(sizeof(generation::Math) + coefficients * sizeof(double) + rows +
               line + 128);
}

OperationDefinition make_filter(const std::string& key, Kind kind,
                                Profile profile) {
  std::vector<Parameter> p{{"y_axis", Type::Int64},
                           {"x_axis", Type::Int64},
                           {"boundary", Type::String},
                           {"cval", Type::Float64}};
  if (kind == Kind::Gaussian) {
    generation::append(&p, {{"sigma_x", Type::Float64},
                            {"sigma_y", Type::Float64},
                            {"radius_x", Type::Int64},
                            {"radius_y", Type::Int64}});
  } else {
    generation::append(&p,
                       {{"anchor_y", Type::Int64}, {"anchor_x", Type::Int64}});
  }
  unsigned ports = 2;
  if (kind == Kind::BoxSum || kind == Kind::BoxMean) {
    ports = 1;
    generation::append(&p, {{"height", Type::Int64}, {"width", Type::Int64}});
  } else if (kind == Kind::Gaussian) {
    ports = 1;
  } else if (kind == Kind::Normalized) {
    ports = 3;
    generation::append(&p, {{"shape", Type::String},
                            {"empty", Type::String},
                            {"component_axis", Type::Int64, false}});
  } else if (kind == Kind::Color) {
    generation::append(
        &p, {{"input_description", Type::String}, {"group", Type::String}});
  } else {
    if (kind == Kind::Separable || kind == Kind::Correlation) {
      ports = 3;
    }
    generation::append(&p, {{"shape", Type::String},
                            {"normalization", Type::String},
                            {"bias", Type::Float64}});
  }
  std::vector<std::pair<std::string, SchemaTemplate>> outputs{
      {"output", tensor_schema(ElementType::Float64, {1, 1})}};
  if (kind == Kind::Normalized) {
    outputs.emplace_back("valid", tensor_schema(ElementType::UInt8, {1, 1}));
  }
  return generation::operation(key, ports, std::move(p), outputs,
                               [kind, profile](const auto& in, const auto& p) {
                                 return std::make_shared<Filter>(kind, profile,
                                                                 in, p);
                               });
}
}  // namespace ps::plugin_internal::filter

namespace ps::plugin_internal {
Status register_filters(OperationRegistry* registry) {
  using namespace filter;  // NOLINT(build/namespaces)
  try {
    for (auto profile :
         {Profile::Strict, Profile::AppleSilicon, Profile::X86Avx2}) {
      const auto suffix = profile == Profile::Strict ? "_strict"
                          : profile == Profile::AppleSilicon
                              ? "_accelerated_apple_silicon"
                              : "_accelerated_x86_64";
      for (auto entry :
           {std::make_pair("convolve2d", Kind::Convolve),
            std::make_pair("correlate2d", Kind::Correlate),
            std::make_pair("normalized_convolution", Kind::Normalized),
            std::make_pair("separable_convolution", Kind::Separable),
            std::make_pair("separable_correlation", Kind::Correlation),
            std::make_pair("box_sum", Kind::BoxSum),
            std::make_pair("box_mean", Kind::BoxMean),
            std::make_pair("gaussian_filter", Kind::Gaussian)}) {
        check(registry->register_operation(
            make_filter(std::string("filter.") + entry.first + suffix,
                        entry.second, profile)));
      }
      for (auto entry :
           {std::make_pair("central_gradient", FieldKind::Central),
            std::make_pair("sobel_gradient", FieldKind::Sobel),
            std::make_pair("scharr_gradient", FieldKind::Scharr),
            std::make_pair("gradient_magnitude", FieldKind::Magnitude),
            std::make_pair("gradient_orientation", FieldKind::Orientation),
            std::make_pair("laplacian4", FieldKind::Laplacian4),
            std::make_pair("laplacian8_isotropic", FieldKind::Laplacian8),
            std::make_pair("log_kernel", FieldKind::LogKernel),
            std::make_pair("log_filter", FieldKind::LogFilter),
            std::make_pair("difference_of_gaussians", FieldKind::Difference),
            std::make_pair("hessian2d", FieldKind::Hessian),
            std::make_pair("structure_tensor2d", FieldKind::Structure),
            std::make_pair("symmetric_eigen2d", FieldKind::Eigen),
            std::make_pair("unsharp_mask", FieldKind::Unsharp),
            std::make_pair("detail_gain", FieldKind::Detail),
            std::make_pair("local_mean_variance", FieldKind::Moments),
            std::make_pair("local_covariance", FieldKind::Covariance)}) {
        check(registry->register_operation(
            make_field(std::string("filter.") + entry.first + suffix,
                       entry.second, profile)));
      }
      for (auto entry : {std::make_pair("median", LocalKind::Median),
                         std::make_pair("percentile", LocalKind::Percentile),
                         std::make_pair("bilateral", LocalKind::Bilateral),
                         std::make_pair("joint_bilateral", LocalKind::Joint),
                         std::make_pair("guided_scalar", LocalKind::Guided),
                         std::make_pair("guided_vector", LocalKind::Vector)}) {
        check(registry->register_operation(
            make_local(std::string("filter.") + entry.first + suffix,
                       entry.second, profile)));
      }
      for (auto entry :
           {std::make_pair("local_contrast_tanh", EffectKind::Contrast),
            std::make_pair("variable_box_gather", EffectKind::Box),
            std::make_pair("variable_disk_gather", EffectKind::Disk),
            std::make_pair("directional_post_aa_v1", EffectKind::Directional)})
        check(registry->register_operation(
            make_effect(std::string("filter.") + entry.first + suffix,
                        entry.second, profile)));
      check(registry->register_operation(make_local_laplacian(
          std::string("filter.local_laplacian_reference") + suffix, profile)));
      check(registry->register_operation(make_gabor_kernel(
          std::string("filter.gabor_kernel") + suffix, profile)));
      check(registry->register_operation(make_gabor_response(
          std::string("filter.gabor_response") + suffix, profile)));
      for (auto entry : {std::make_pair("gaussian_pyramid", 0),
                         std::make_pair("laplacian_pyramid", 1),
                         std::make_pair("reconstruct_laplacian", 2)})
        check(registry->register_operation(
            make_pyramid(std::string("filter.") + entry.first + suffix,
                         entry.second, profile)));
      check(registry->register_operation(make_coefficients(
          std::string("filter.gaussian_coefficients") + suffix, profile)));
    }
    check(registry->register_operation(make_smaa()));
    check(registry->register_operation(make_hysteresis()));
    check(registry->register_operation(make_nms()));
    check(registry->register_operation(make_band()));
    check(registry->register_operation(make_pyramid_assembly()));
    check(registry->register_operation(make_filter(
        "filter.positive_color_blur_strict", Kind::Color, Profile::Strict)));
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
