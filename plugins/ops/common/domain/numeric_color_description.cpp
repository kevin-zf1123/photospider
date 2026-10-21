#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/domain/model_coordinates.hpp"
#include "common/domain/rgb_geometry_codec.hpp"
#include "common/floating_environment.hpp"
#include "photospider/core/status_helpers.hpp"
#include "photospider/ops/format/transfer.hpp"
#include "photospider/ops/numeric/color_description.hpp"

namespace ps::numeric {
namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw core_internal::invalid_schema_domain(message);
}
template <class T>
T checked(Result<T> result) {
  if (!result.ok())
    throw result.status();
  return result.take_value();
}
const std::vector<std::string>& roles(const std::string& model) {
  static const std::map<std::string, std::vector<std::string>> table{
      {"rgb", {"red", "green", "blue"}},
      {"xyz", {"x", "y", "z"}},
      {"cielab", {"l", "a", "b"}},
      {"cielch", {"l", "c", "h"}},
      {"oklab", {"l", "a", "b"}},
      {"oklch", {"l", "c", "h"}},
      {"hsl", {"hue", "saturation", "lightness"}},
      {"ycbcr", {"y", "cb", "cr"}},
      {"cmyk", {"cyan", "magenta", "yellow", "black"}}};
  const auto found = table.find(model);
  require(found != table.end(), "unsupported numeric color model");
  return found->second;
}
TensorInterpretation global(const TensorDescription& d) {
  return {d.model,       d.primaries,  d.transfer,         d.reference,
          d.association, d.white,      d.primaries_xy,     d.profile,
          d.convention,  d.configured, d.analytic_binding, d.coordinates};
}
void merge(TensorInterpretation* a, const TensorInterpretation& b) {
  const bool asserts = !b.model.empty() || b.profile || b.configured;
  require(!asserts || a->convention == b.convention,
          "conflicting numeric color convention");
  for (auto pair : {std::make_pair(&a->model, &b.model),
                    std::make_pair(&a->primaries, &b.primaries),
                    std::make_pair(&a->transfer, &b.transfer),
                    std::make_pair(&a->reference, &b.reference),
                    std::make_pair(&a->association, &b.association)}) {
    require(pair.first->empty() || pair.second->empty() ||
                *pair.first == *pair.second,
            "conflicting numeric color interpretation");
    if (pair.first->empty())
      *pair.first = *pair.second;
  }
  const auto optional = [](auto* destination, const auto& source) {
    require(!*destination || !source || **destination == *source,
            "conflicting numeric color configuration");
    if (!*destination)
      *destination = source;
  };
  optional(&a->white, b.white);
  optional(&a->primaries_xy, b.primaries_xy);
  optional(&a->profile, b.profile);
  optional(&a->configured, b.configured);
  optional(&a->analytic_binding, b.analytic_binding);
  require(data_internal::overlay_model_coordinates(&a->coordinates,
                                                   b.coordinates, true),
          "conflicting numeric model coordinates");
}
bool unitless(const std::string& unit) {
  return unit.empty() || unit == "1" || unit == "relative";
}
void metadata(TensorColorGroup* group) {
  auto& i = group->interpretation;
  const auto& expected = roles(i.model);
  require(!i.configured && !i.analytic_binding,
          "numeric colors require direct native coordinates");
  require(i.association.empty() || i.association == "straight",
          "numeric colors require straight coordinates");
  const auto coordinates = i.coordinates.value_or(TensorModelCoordinates{});
  require(coordinates.scale.empty() || coordinates.scale == "relative",
          "numeric colors require relative coordinates");
  require(coordinates.gray_kind.empty(),
          "gray interpretation is inapplicable to numeric color tuples");
  require(i.model == "ycbcr" || !coordinates.ncl_coefficients,
          "NCL coefficients require YCbCr");
  const bool cmyk = i.model == "cmyk";
  const bool rgb = i.model == "rgb" || i.model == "hsl" || i.model == "ycbcr";
  if (cmyk) {
    require(i.convention == "icc-native" && i.profile &&
                i.profile->byte_length && i.reference == "profile",
            "CMYK requires icc-native, profile reference and identity");
    require(!i.white && i.primaries.empty() && !i.primaries_xy &&
                i.transfer.empty(),
            "CMYK forbids native RGB/white assertions");
  } else {
    require(i.convention == "relative-v1" && !i.profile &&
                (i.reference == "scene" || i.reference == "display"),
            "native numeric colors require relative-v1 and scene/display");
    if (rgb) {
      namespace basis = plugin_internal::basis_ops;
      const format::RgbBasis definition{i.primaries, i.primaries_xy, i.white};
      (void)checked(format::rgb_basis_parameter(definition));
      const auto resolved = basis::resolve(definition);
      i.white = resolved.white;
      i.primaries_xy = resolved.xy;
      const auto transfer = checked(decode_transfer_definition(i.transfer));
      require(transfer.curve == TransferCurve::Linear ||
                  transfer.curve == TransferCurve::Srgb ||
                  transfer.curve == TransferCurve::PowerGamma,
              "unsupported numeric color transfer");
      require(i.model != "ycbcr" || transfer.curve != TransferCurve::Linear,
              "YCbCr requires encoded underlying RGB");
    } else {
      require(i.primaries.empty() && !i.primaries_xy && i.transfer.empty(),
              "non-RGB model forbids RGB basis/transfer assertions");
      require(i.white.has_value(), "numeric color requires reference white");
      (void)checked(format::xyz_white_parameter(*i.white));
    }
    if (i.model == "xyz")
      require(coordinates.scale == "relative",
              "XYZ requires explicit relative scale and white Y=1");
    if (i.model == "oklab" || i.model == "oklch")
      require(*i.white == std::array<double, 2>{.3127, .3290},
              "OKLab coordinates require D65");
    if (i.model == "ycbcr") {
      require(coordinates.ncl_coefficients.has_value(),
              "YCbCr requires explicit Kr,Kb");
      // Kr>0, Kb>0 and exact Kr+Kb<1 have the same simplex admission as xy.
      (void)checked(format::xyz_white_parameter(*coordinates.ncl_coefficients));
    }
  }
  for (std::size_t c = 0; c < expected.size(); ++c) {
    const auto& component = group->components[c];
    require(component.role == expected[c],
            "numeric color components require canonical model order");
    const auto& unit = component.unit;
    const bool hue = component.role == "h" || component.role == "hue";
    const bool opponent = (i.model == "cielab" && ((c == 1 && unit == "a*") ||
                                                   (c == 2 && unit == "b*"))) ||
                          (i.model == "cielch" && c == 1 && unit == "C*");
    require(
        hue ? unit == "radian" || unit == "pi_multiple" || unit == "rational_pi"
            : unitless(unit) || opponent,
        "numeric color unit requires explicit native coordinates");
  }
  i.association = "straight";
}
}  // namespace

Result<TensorColorGroup> resolve_color_tuple(const TensorDescription& d) {
  using Answer = Result<TensorColorGroup>;
  try {
    plugin_internal::FloatingEnvironment environment;
    require(environment.active(),
            "numeric color floating environment unavailable");
    require(d.channel_axis && *d.channel_axis == 0 && d.groups.size() == 1 &&
                !d.component && d.axes.empty() && !d.encoding && !d.sampling,
            "numeric color description must describe one unpacked tuple");
    const auto& source = d.groups.front();
    const auto& expected = roles(source.interpretation.model);
    const auto count = expected.size() + static_cast<bool>(source.alpha);
    require(source.indices.size() == expected.size() &&
                source.components.size() == expected.size() &&
                (!source.alpha || *source.alpha == expected.size()) &&
                (d.channels.empty() || d.channels.size() == count),
            "numeric color tuple has incomplete or unrelated channels");
    const auto valid = validate_tensor_description(
        d, {ElementType::Float64, {static_cast<std::uint64_t>(count)}});
    if (!valid.ok())
      return Answer(valid);
    auto group = source;
    merge(&group.interpretation, global(d));
    for (std::size_t c = 0; c < expected.size(); ++c) {
      require(group.indices[c] == c && !group.components[c].encoding &&
                  !group.components[c].sampling,
              "numeric color requires contiguous unpacked model order");
      if (!d.channels.empty()) {
        const auto& channel = d.channels[c];
        require(!channel.encoding && !channel.sampling,
                "numeric color channel requires native sampling/encoding");
        if (channel.interpretation)
          merge(&group.interpretation, *channel.interpretation);
        if (group.components[c].unit.empty())
          group.components[c].unit = channel.unit;
      }
    }
    if (source.alpha) {
      require(!d.channels.empty(), "alpha requires an explicit tuple channel");
      const auto& alpha = d.channels[*source.alpha];
      require(alpha.role == "alpha" && unitless(alpha.unit) &&
                  !alpha.encoding && !alpha.sampling && !alpha.interpretation,
              "alpha must be an independent native tuple channel");
    }
    metadata(&group);
    return Answer(std::move(group));
  } catch (const Status& status) {
    return Answer(status);
  } catch (const plugin_internal::basis_ops::CheckedFailure& failure) {
    return Answer(failure.status);
  }
}

Result<TensorDescription> color_tuple_description(
    TensorInterpretation interpretation, const std::vector<std::string>& units,
    bool with_alpha) {
  using Answer = Result<TensorDescription>;
  try {
    const auto& expected = roles(interpretation.model);
    require(units.size() == expected.size(),
            "numeric color unit count mismatch");
    TensorDescription out;
    out.channel_axis = 0;
    TensorColorGroup group;
    group.name = "color";
    group.interpretation = std::move(interpretation);
    for (std::size_t c = 0; c < expected.size(); ++c) {
      TensorChannelDescription component;
      component.role = expected[c];
      component.unit = units[c];
      group.indices.push_back(c);
      group.components.push_back(component);
      if (with_alpha)
        out.channels.push_back(std::move(component));
    }
    if (with_alpha) {
      group.alpha = expected.size();
      TensorChannelDescription alpha;
      alpha.role = "alpha";
      alpha.unit = "1";
      out.channels.push_back(std::move(alpha));
    }
    out.groups.push_back(std::move(group));
    auto resolved = resolve_color_tuple(out);
    if (!resolved.ok())
      return Answer(resolved.status());
    return Answer(std::move(out));
  } catch (const Status& status) {
    return Answer(status);
  }
}

Status validate_color_tuple_shape(const TensorDescription& description,
                                  const TensorDescriptor& descriptor,
                                  bool rational_hue) {
  auto group = resolve_color_tuple(description);
  if (!group.ok())
    return group.status();
  bool rational = false;
  for (const auto& component : group.value().components)
    rational |= component.unit == "rational_pi";
  const auto mismatch = [] {
    return Status{ErrorCode::TypeMismatch,
                  "sampled color shape/dtype disagrees with tuple",
                  FailureReason::None,
                  {FailureOrigin::Schema, FailureScope::Unspecified}};
  };
  if (rational != rational_hue || (rational && group.value().alpha))
    return core_internal::invalid_schema_domain(
        "rational hue requires the matching split-port ramp");
  const auto count = rational ? 2U
                              : group.value().components.size() +
                                    static_cast<bool>(group.value().alpha);
  if ((descriptor.element_type != ElementType::Float32 &&
       descriptor.element_type != ElementType::Float64) ||
      descriptor.shape.size() < 2 || descriptor.shape.size() > 8 ||
      descriptor.shape.back() != count)
    return mismatch();
  std::uint64_t product = 1;
  for (const auto extent : descriptor.shape) {
    if (!extent || extent > (UINT64_C(1) << 40) / product)
      return mismatch();
    product *= extent;
  }
  return Status::success();
}
}  // namespace ps::numeric
