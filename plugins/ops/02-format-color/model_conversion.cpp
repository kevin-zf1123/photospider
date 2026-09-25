#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_publication.hpp"
#include "02-format-color/model_math.hpp"
#include "02-format-color/model_simd.hpp"
#include "photospider/data/tensor_description.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "photospider/format/channel.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace model_ops;  // NOLINT(build/namespaces)
using numeric_ops::SequenceProfile;
using Params = std::map<std::string, ParameterValue>;
enum class Layout { Auto, View, Materialize };
struct Slot final {
  int component = -1;  // -1: bypass copy, otherwise target model component.
  std::uint64_t source = 0;
};
struct Preparation final {
  MathConfig math;
  std::shared_ptr<const Constants> constants;
  std::optional<std::uint32_t> source_axis;
  std::optional<std::uint32_t> output_axis;
  std::array<std::uint64_t, 3> components{};
  std::vector<Slot> slots;
  std::vector<std::uint64_t> source_shape;
  Layout layout = Layout::Auto;
};
Status invalid(const std::string& text) {
  return {ErrorCode::InvalidArgument,
          text,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
Status mismatch(const std::string& text) {
  return {ErrorCode::TypeMismatch,
          text,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
std::string text(const Params& p, const char* key, const char* fallback = "") {
  const auto i = p.find(key);
  return i == p.end() ? fallback : std::get<std::string>(i->second);
}
void require(bool condition, const std::string& error) {
  if (!condition)
    throw invalid(error);
}
bool single(Kind kind) {
  return kind == Kind::GrayToColor || kind == Kind::Threshold ||
         kind == Kind::BinaryToGray;
}
std::vector<std::string> roles(const std::string& model) {
  if (model == "xyz")
    return {"x", "y", "z"};
  if (model == "cielab" || model == "oklab")
    return {"l", "a", "b"};
  if (model == "cielch" || model == "oklch")
    return {"l", "c", "h"};
  if (model == "rgb")
    return {"red", "green", "blue"};
  if (model == "hsl")
    return {"hue", "saturation", "lightness"};
  if (model == "hsv")
    return {"hue", "saturation", "value"};
  if (model == "ycbcr")
    return {"y", "cb", "cr"};
  if (model == "xyy")
    return {"x", "y", "luminance"};
  return {"gray"};
}
GrayKind gray_kind(const std::string& kind) {
  if (kind == "linear_y")
    return GrayKind::LinearY;
  if (kind == "encoded_luma")
    return GrayKind::EncodedLuma;
  if (kind == "cielab_l")
    return GrayKind::CielabL;
  if (kind == "oklab_l")
    return GrayKind::OklabL;
  throw invalid(
      "gray_kind must explicitly identify one of four native Gray meanings");
}
const char* gray_name(GrayKind kind) {
  static constexpr const char* names[]{"linear_y", "encoded_luma", "cielab_l",
                                       "oklab_l"};
  return names[static_cast<unsigned>(kind)];
}
GrayKind gray_from_model(const std::string& model) {
  if (model == "xyz")
    return GrayKind::LinearY;
  if (model == "ycbcr")
    return GrayKind::EncodedLuma;
  if (model == "cielab")
    return GrayKind::CielabL;
  if (model == "oklab")
    return GrayKind::OklabL;
  throw mismatch("color_to_gray requires XYZ, NCL YCbCr, CIELAB or OKLab");
}
std::vector<std::uint64_t> indices(const std::string& value) {
  std::vector<std::uint64_t> result;
  const char* first = value.data();
  const char* end = first + value.size();
  while (first < end) {
    std::uint64_t n = 0;
    const auto parsed = std::from_chars(first, end, n);
    require(parsed.ec == std::errc{} && parsed.ptr != first,
            "components must be comma-separated unsigned indices");
    result.push_back(n);
    if (parsed.ptr == end)
      break;
    require(*parsed.ptr == ',' && parsed.ptr + 1 < end, "malformed components");
    first = parsed.ptr + 1;
  }
  return result;
}
std::optional<TensorDescription> description_of(
    const OperationMetadata& input) {
  for (const auto& f : input.facets) {
    if (f.key != "photospider.tensor-description")
      continue;
    auto decoded = decode_tensor_description(f);
    if (!decoded.ok())
      throw decoded.status();
    auto status =
        validate_tensor_description(decoded.value(), input.descriptor);
    if (!status.ok())
      throw status;
    return decoded.take_value();
  }
  return {};
}
TensorInterpretation top_interpretation(const TensorDescription& d) {
  return {d.model,       d.primaries,  d.transfer,         d.reference,
          d.association, d.white,      d.primaries_xy,     d.profile,
          d.convention,  d.configured, d.analytic_binding, d.coordinates};
}
void clear_top(TensorDescription& d) {
  d.model.clear();
  d.primaries.clear();
  d.transfer.clear();
  d.reference.clear();
  d.association.clear();
  d.white.reset();
  d.primaries_xy.reset();
  d.profile.reset();
  d.configured.reset();
  d.analytic_binding.reset();
  d.coordinates.reset();
  d.convention = "relative-v1";
}
bool described(const TensorInterpretation& i) {
  return !i.model.empty() || !i.primaries.empty() || !i.transfer.empty() ||
         !i.reference.empty() || !i.association.empty() || i.white ||
         i.primaries_xy || i.profile || i.configured || i.analytic_binding ||
         i.coordinates;
}
TensorChannelDescription bypass_description(const TensorDescription& source,
                                            std::uint64_t slot) {
  TensorChannelDescription result = source.channels.empty()
                                        ? TensorChannelDescription{}
                                        : source.channels[slot];
  auto effective = top_interpretation(source);
  if (result.interpretation) {
    const auto& local = *result.interpretation;
    const auto inherit = [](auto& to, const auto& from) {
      if (!from.empty())
        to = from;
    };
    inherit(effective.model, local.model);
    inherit(effective.primaries, local.primaries);
    inherit(effective.transfer, local.transfer);
    inherit(effective.reference, local.reference);
    inherit(effective.association, local.association);
    effective.convention = local.convention;
    if (local.white)
      effective.white = local.white;
    if (local.primaries_xy)
      effective.primaries_xy = local.primaries_xy;
    if (local.profile)
      effective.profile = local.profile;
    if (local.configured)
      effective.configured = local.configured;
    if (local.analytic_binding)
      effective.analytic_binding = local.analytic_binding;
    if (local.coordinates)
      effective.coordinates = local.coordinates;
  }
  if (described(effective))
    result.interpretation = std::move(effective);
  if (!result.encoding)
    result.encoding = source.encoding;
  if (!result.sampling)
    result.sampling = source.sampling;
  return result;
}
void valid_pair(const std::array<double, 2>& pair, const char* name) {
  require(std::isfinite(pair[0]) && std::isfinite(pair[1]),
          std::string(name) + " must be finite");
  const RationalMath::Work work = [](std::uint64_t) {
    return Status::success();
  };
  RationalMath exact(SequenceProfile::Strict);
  exact.bind(work);
  const auto a = exact.binary(numeric_ops::numeric_bits(pair[0]));
  const auto b = exact.binary(numeric_ops::numeric_bits(pair[1]));
  require(exact.order(a, exact.integer(0)) > 0 &&
              exact.order(b, exact.integer(0)) > 0 &&
              exact.order(exact.add(a, b), exact.integer(1)) < 0,
          std::string(name) +
              " requires positive entries whose exact sum is below one");
}
std::array<double, 2> parameter_pair(const Params& p, const char* x,
                                     const char* y) {
  require(p.count(x) && p.count(y),
          std::string(x) + " and " + y + " are required together");
  return {std::get<double>(p.at(x)), std::get<double>(p.at(y))};
}
std::optional<std::array<double, 2>> ncl_parameter(const Params& p) {
  const auto preset = text(p, "ncl_matrix");
  if (p.count("ncl_matrix")) {
    require(!preset.empty(), "ncl_matrix must not be empty");
    require(!p.count("kr") && !p.count("kb"),
            "NCL preset and explicit coefficients conflict");
    if (preset == "bt601")
      return std::array<double, 2>{.299, .114};
    if (preset == "bt709")
      return std::array<double, 2>{.2126, .0722};
    if (preset == "bt2020_ncl")
      return std::array<double, 2>{.2627, .0593};
    throw invalid("ncl_matrix must be bt601, bt709 or bt2020_ncl");
  }
  if (p.count("kr") || p.count("kb")) {
    auto pair = parameter_pair(p, "kr", "kb");
    valid_pair(pair, "NCL coefficients");
    return pair;
  }
  return {};
}
std::uint64_t typed_constant(const std::string& literal, bool narrow) {
  const std::string prefix = narrow ? "f32:" : "f64:";
  require(
      literal.substr(0, 4) == prefix && literal.size() == (narrow ? 12u : 20u),
      "typed constants use f32:xxxxxxxx or f64:xxxxxxxxxxxxxxxx matching the "
      "input dtype");
  std::uint64_t bits = 0;
  const auto parsed = std::from_chars(
      literal.data() + 4, literal.data() + literal.size(), bits, 16);
  require(
      parsed.ec == std::errc{} && parsed.ptr == literal.data() + literal.size(),
      "invalid typed constant hexadecimal bits");
  const auto value = numeric_ops::BinaryParts::decode(bits, narrow);
  require(!value.nan && !value.infinite,
          "black_value/white_value must be finite");
  return bits;
}
void legal_parameters(Kind kind, const Params& p, bool raw) {
  std::set<std::string> allowed{"metadata_mode",
                                "metadata_override",
                                "group",
                                "axis",
                                "components",
                                "axis_free",
                                "layout",
                                "algorithm",
                                "expected_source_dtype",
                                "expected_source_shape",
                                "expected_source_tensor",
                                "expected_source_layout"};
  const auto add = [&](std::initializer_list<const char*> keys) {
    for (const auto* k : keys)
      allowed.insert(k);
  };
  if (kind == Kind::XyzToLab || kind == Kind::LabToXyz)
    add({"white_x", "white_y"});
  if (kind == Kind::LabToLch || kind == Kind::OklabToOklch ||
      kind == Kind::RgbToHsl || kind == Kind::RgbToHsv)
    add({"output_hue_unit"});
  if (kind == Kind::LchToLab || kind == Kind::OklchToOklab ||
      kind == Kind::HslToRgb || kind == Kind::HsvToRgb)
    add({"input_hue_unit"});
  if (kind == Kind::RgbToYcbcr || kind == Kind::YcbcrToRgb)
    add({"ncl_matrix", "kr", "kb"});
  if (kind == Kind::ColorToGray || kind == Kind::GrayToColor)
    add({"gray_kind"});
  if (kind == Kind::GrayToColor)
    add({"gray_white_x", "gray_white_y", "output_axis"});
  if (kind == Kind::Threshold)
    add({"threshold"});
  if (kind == Kind::BinaryToGray)
    add({"black_value", "white_value"});
  for (const auto& kv : p)
    require(allowed.count(kv.first) != 0,
            "inapplicable FMT-11 parameter: " + kv.first);
  if (raw)
    require(!p.count("group"), "raw forbids semantic group selection");
  if (!raw && (p.count("white_x") || p.count("white_y") ||
               p.count("gray_white_x") || p.count("gray_white_y")))
    throw invalid(
        "semantic white comes from the effective description, not raw "
        "parameters");
}
void native_metadata(const TensorInterpretation& interpretation,
                     const std::vector<TensorChannelDescription>& components,
                     const TensorDescription& description, Preparation& out,
                     const Params& p) {
  const auto kind = out.math.kind;
  if (interpretation.model != source_model(kind, out.math.gray) ||
      interpretation.convention != "relative-v1" || interpretation.profile ||
      interpretation.configured ||
      (!interpretation.association.empty() &&
       interpretation.association != "straight") ||
      (description.association == "premultiplied"))
    throw mismatch(
        "FMT-11 requires the stated native model in canonical straight "
        "relative-v1 coordinates");
  if (interpretation.reference != "scene" &&
      interpretation.reference != "display")
    throw mismatch("FMT-11 requires an explicit scene/display reference");
  if (description.encoding)
    throw mismatch(
        "native model conversion requires explicit prior code decoding");
  for (std::size_t c = 0; c < components.size(); ++c) {
    if (components[c].encoding ||
        (!description.channels.empty() && out.source_axis &&
         description.channels[out.components[c]].encoding))
      throw mismatch(
          "selected color components must be native, not encoded codes");
  }
  const auto coords =
      interpretation.coordinates.value_or(TensorModelCoordinates{});
  const auto model = interpretation.model;
  const bool xyz_like = model == "xyz" || model == "xyy" ||
                        ((model == "gray" || model == "black_white") &&
                         out.math.gray == GrayKind::LinearY);
  if (xyz_like && coords.scale != "relative" && coords.scale != "absolute")
    throw mismatch(
        "XYZ/xyY/linear-Y Gray requires explicit relative or absolute scale");
  if ((kind == Kind::XyzToLab || kind == Kind::XyzToOklab) &&
      coords.scale != "relative")
    throw mismatch(
        "CIELAB/OKLab conversion requires relative XYZ with white Y=1");
  const bool white_needed = xyz_like || model == "cielab" ||
                            model == "cielch" || model == "oklab" ||
                            model == "oklch" ||
                            ((model == "gray" || model == "black_white") &&
                             out.math.gray != GrayKind::EncodedLuma);
  if (white_needed) {
    require(interpretation.white.has_value(),
            "native model description requires reference-white xy");
    valid_pair(*interpretation.white, "reference white");
    out.math.white = *interpretation.white;
  }
  if (kind == Kind::XyzToOklab || kind == Kind::OklabToXyz ||
      model == "oklab" || model == "oklch" ||
      ((model == "gray" || model == "black_white") &&
       out.math.gray == GrayKind::OklabL))
    require(interpretation.white &&
                *interpretation.white == std::array<double, 2>{.3127, .3290},
            "OKLab uses the fixed binary64 D65 white, without adaptation");
  const bool encoded =
      model == "ycbcr" || ((model == "gray" || model == "black_white") &&
                           out.math.gray == GrayKind::EncodedLuma);
  const bool rgb_basis =
      model == "rgb" || model == "hsl" || model == "hsv" || encoded;
  if (rgb_basis) {
    if (interpretation.transfer.empty() ||
        (interpretation.primaries.empty() &&
         (!interpretation.primaries_xy || !interpretation.white)))
      throw mismatch(
          "RGB-derived coordinates require underlying RGB basis and transfer");
    if ((encoded || kind == Kind::RgbToYcbcr) &&
        interpretation.transfer == "linear")
      throw mismatch("NCL YCbCr acts on encoded RGB, not implicit linear RGB");
  }
  if (encoded) {
    require(coords.ncl_coefficients.has_value(),
            "NCL source must retain explicit Kr,Kb");
    valid_pair(*coords.ncl_coefficients, "NCL coefficients");
    out.math.ncl = *coords.ncl_coefficients;
  }
  const bool inverse_hue = kind == Kind::LchToLab ||
                           kind == Kind::OklchToOklab ||
                           kind == Kind::HslToRgb || kind == Kind::HsvToRgb;
  if (inverse_hue) {
    const auto hue_slot =
        (kind == Kind::LchToLab || kind == Kind::OklchToOklab) ? 2u : 0u;
    auto unit = components[hue_slot].unit;
    if (unit.empty() && !description.channels.empty())
      unit = description.channels[out.components[hue_slot]].unit;
    require(unit == "radian" || unit == "pi_multiple",
            "source hue must declare radian or pi_multiple");
    if (p.count("input_hue_unit"))
      require(text(p, "input_hue_unit") == unit,
              "input_hue_unit assertion disagrees with source");
    out.math.input_pi = unit == "pi_multiple";
  }
  // Empty unit strings inherit the explicit native model/convention. Explicit
  // code-domain/lightness-100 or degree assertions cannot silently override it.
  for (unsigned i = 0; i < components.size(); ++i) {
    auto unit = components[i].unit;
    if (unit.empty() && !description.channels.empty() && out.source_axis)
      unit = description.channels[out.components[i]].unit;
    if (unit.empty())
      continue;
    const bool hue =
        inverse_hue && i == ((model == "cielch" || model == "oklch") ? 2u : 0u);
    if (hue)
      continue;
    const bool absolute_component = xyz_like && model != "black_white" &&
                                    coords.scale == "absolute" &&
                                    (model != "xyy" || i == 2);
    if (absolute_component) {
      require(unit == "cd/m2" || unit == "cd/m^2",
              "absolute coordinate unit must be cd/m2");
    } else {
      const auto role = components[i].role;
      const bool opponent =
          (model == "cielab" &&
           ((role == "a" && unit == "a*") || (role == "b" && unit == "b*"))) ||
          (model == "cielch" && role == "c" && unit == "C*");
      require(
          unit == "1" || unit == "relative" || opponent,
          "non-native or wrong-role component unit requires explicit decoding");
    }
  }
}

Result<OperationPreparation> prepare(
    Kind kind, SequenceProfile profile,
    const std::vector<OperationMetadata>& inputs, const Params& p) {
  using Answer = Result<OperationPreparation>;
  try {
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    const auto& input = inputs[0];
    const auto& shape = input.descriptor.shape;
    if ((input.descriptor.element_type != ElementType::Float32 &&
         input.descriptor.element_type != ElementType::Float64) ||
        shape.empty() || shape.size() > 8 || input.result_schema)
      return Answer(
          mismatch("FMT-11 requires a rank 1..8 Float32/Float64 Value"));
    auto selected = std::make_shared<Preparation>();
    auto& out = *selected;
    out.source_shape = shape;
    out.math.kind = kind;
    out.math.profile = profile;
    out.math.narrow = input.descriptor.element_type == ElementType::Float32;
    const auto mode = text(p, "metadata_mode", "respect");
    require(mode == "respect" || mode == "override" || mode == "raw",
            "invalid metadata_mode");
    const bool raw = mode == "raw";
    out.math.semantic = !raw;
    legal_parameters(kind, p, raw);
    require((mode == "override") == (p.count("metadata_override") != 0),
            "metadata_override must occur exactly in override mode");
    auto description = description_of(input);
    if (mode == "override") {
      auto decoded =
          tensor_description_from_parameter(text(p, "metadata_override"));
      if (!decoded.ok())
        throw decoded.status();
      auto valid =
          validate_tensor_description(decoded.value(), input.descriptor);
      if (!valid.ok())
        throw valid;
      description = decoded.take_value();
    }
    if (!raw)
      require(description.has_value(),
              "semantic FMT-11 requires an explicit description or override");
    const auto algorithm = text(p, "algorithm", "auto");
    require(algorithm == "auto" || algorithm == "scalar" ||
                algorithm == "reference",
            "invalid algorithm");
    out.math.algorithm = algorithm == "auto"     ? Algorithm::Auto
                         : algorithm == "scalar" ? Algorithm::Scalar
                                                 : Algorithm::Reference;
    const auto layout = text(p, "layout", "auto");
    require(layout == "auto" || layout == "view" || layout == "materialize",
            "invalid layout");
    out.layout = layout == "view"          ? Layout::View
                 : layout == "materialize" ? Layout::Materialize
                                           : Layout::Auto;
    require(kind == Kind::ColorToGray || out.layout != Layout::View,
            "arithmetic FMT-11 members reject forced view even for bypass "
            "requests");
    const bool axis_free =
        p.count("axis_free") && std::get<bool>(p.at("axis_free"));
    require(!p.count("axis_free") || single(kind),
            "axis_free is only valid for scalar Gray/BlackWhite sources");
    if (axis_free) {
      require(!p.count("axis") && !p.count("components"),
              "axis_free forbids channel selectors");
      if (!raw)
        require(!description->channel_axis,
                "axis_free contradicts semantic channel_axis");
    } else if (p.count("axis")) {
      const auto axis = std::get<std::int64_t>(p.at("axis"));
      require(axis >= 0 && static_cast<std::uint64_t>(axis) < shape.size(),
              "axis outside source rank");
      out.source_axis = static_cast<std::uint32_t>(axis);
      if (!raw)
        require(description->channel_axis == out.source_axis,
                "axis assertion disagrees with source metadata");
    } else if (!raw && description->channel_axis) {
      out.source_axis = description->channel_axis;
    } else {
      throw invalid("source requires an explicit raw axis or axis_free=true");
    }
    const unsigned input_count = single(kind) ? 1 : 3;
    const TensorColorGroup* group = nullptr;
    TensorInterpretation interpretation;
    std::vector<TensorChannelDescription> source_components;
    if (!raw) {
      if (axis_free) {
        require(!p.count("group") && description->component.has_value(),
                "axis-free semantic source needs a described component, not "
                "group selection");
        source_components = {*description->component};
        interpretation = description->component->interpretation.value_or(
            top_interpretation(*description));
      } else {
        for (const auto& g : description->groups) {
          const bool match =
              p.count("group") ? g.name == text(p, "group")
                               : (kind == Kind::ColorToGray
                                      ? (g.interpretation.model == "xyz" ||
                                         g.interpretation.model == "ycbcr" ||
                                         g.interpretation.model == "cielab" ||
                                         g.interpretation.model == "oklab")
                                      : g.interpretation.model ==
                                            source_model(kind, out.math.gray));
          if (!match)
            continue;
          require(group == nullptr, "semantic source group is ambiguous");
          group = &g;
        }
        require(group != nullptr && group->indices.size() == input_count,
                "a complete uniquely selected source group is required");
        interpretation = group->interpretation;
        if (kind == Kind::ColorToGray)
          gray_from_model(interpretation.model);
        else
          require(interpretation.model == source_model(kind, out.math.gray),
                  "selected source group has the wrong native model");
        const auto expected_roles = roles(interpretation.model);
        require(expected_roles.size() == input_count,
                "source model arity mismatch");
        source_components.resize(input_count);
        for (unsigned c = 0; c < input_count; ++c) {
          auto found =
              std::find_if(group->components.begin(), group->components.end(),
                           [&](const auto& value) {
                             return value.role == expected_roles[c];
                           });
          require(found != group->components.end(),
                  "source group role missing");
          const auto j =
              static_cast<std::size_t>(found - group->components.begin());
          out.components[c] = group->indices[j];
          source_components[c] = *found;
        }
      }
      if (kind == Kind::ColorToGray)
        out.math.gray = gray_from_model(interpretation.model);
      if (single(kind)) {
        require(interpretation.coordinates.has_value(),
                "Gray/BlackWhite must retain its Gray origin");
        out.math.gray = gray_kind(interpretation.coordinates->gray_kind);
      }
      if (p.count("gray_kind"))
        require(gray_kind(text(p, "gray_kind")) == out.math.gray,
                "gray_kind assertion disagrees with source");
      if (p.count("components")) {
        const auto asserted = indices(text(p, "components"));
        require(asserted.size() == input_count,
                "component assertion arity mismatch");
        for (unsigned c = 0; c < input_count; ++c)
          require(asserted[c] == out.components[c],
                  "component assertion disagrees with model role order");
      }
      native_metadata(interpretation, source_components, *description, out, p);
    } else {
      if (!axis_free) {
        require(p.count("components"), "raw components are required");
        const auto values = indices(text(p, "components"));
        require(values.size() == input_count, "raw component arity mismatch");
        std::set<std::uint64_t> unique;
        for (unsigned c = 0; c < input_count; ++c) {
          require(values[c] < shape[*out.source_axis] &&
                      unique.insert(values[c]).second,
                  "raw components must be distinct in-range indices");
          out.components[c] = values[c];
        }
      }
      if (kind == Kind::ColorToGray || kind == Kind::GrayToColor)
        out.math.gray = gray_kind(text(p, "gray_kind"));
      if (kind == Kind::XyzToLab || kind == Kind::LabToXyz) {
        out.math.white = parameter_pair(p, "white_x", "white_y");
        valid_pair(out.math.white, "raw reference white");
      }
      if (kind == Kind::GrayToColor && out.math.gray == GrayKind::LinearY) {
        out.math.white = parameter_pair(p, "gray_white_x", "gray_white_y");
        valid_pair(out.math.white, "raw Gray white");
      }
      if (kind == Kind::LchToLab || kind == Kind::OklchToOklab ||
          kind == Kind::HslToRgb || kind == Kind::HsvToRgb) {
        const auto unit = text(p, "input_hue_unit");
        require(unit == "radian" || unit == "pi_multiple",
                "raw input_hue_unit is required");
        out.math.input_pi = unit == "pi_multiple";
      }
    }
    if (kind == Kind::GrayToColor && out.math.gray != GrayKind::LinearY)
      require(!p.count("gray_white_x") && !p.count("gray_white_y"),
              "gray_white only applies to linear_y");
    if (kind == Kind::LabToLch || kind == Kind::OklabToOklch ||
        kind == Kind::RgbToHsl || kind == Kind::RgbToHsv) {
      const auto unit = text(p, "output_hue_unit");
      require(unit == "radian" || unit == "pi_multiple",
              "output_hue_unit is required");
      out.math.output_pi = unit == "pi_multiple";
    }
    if (kind == Kind::RgbToYcbcr || kind == Kind::YcbcrToRgb) {
      const auto pair = ncl_parameter(p);
      if (raw || kind == Kind::RgbToYcbcr)
        require(pair.has_value(), "NCL matrix is required");
      if (!raw && kind == Kind::YcbcrToRgb && pair)
        require(*pair == out.math.ncl,
                "NCL matrix assertion disagrees with source");
      if (pair)
        out.math.ncl = *pair;
      valid_pair(out.math.ncl, "NCL coefficients");
    }
    if (kind == Kind::Threshold) {
      require(p.count("threshold"), "threshold is required");
      out.math.threshold = std::get<double>(p.at("threshold"));
      require(std::isfinite(out.math.threshold),
              "threshold must be finite Float64");
    }
    if (kind == Kind::BinaryToGray) {
      out.math.levels = {
          typed_constant(text(p, "black_value"), out.math.narrow),
          typed_constant(text(p, "white_value"), out.math.narrow)};
    }
    // Optional edge assertions are checked against the effective, call-local
    // source metadata, including override; they never assert payload validity.
    if (p.count("expected_source_dtype"))
      require(std::get<std::int64_t>(p.at("expected_source_dtype")) ==
                  static_cast<std::int64_t>(input.descriptor.element_type),
              "source dtype assertion mismatch");
    if (p.count("expected_source_shape")) {
      std::string encoded;
      for (const auto n : shape)
        encoded += (encoded.empty() ? "" : ",") + std::to_string(n);
      require(text(p, "expected_source_shape") == encoded,
              "source shape assertion mismatch");
    }
    if (p.count("expected_source_tensor")) {
      std::string encoded = "none";
      if (description) {
        auto result = tensor_description_parameter(*description);
        if (!result.ok())
          throw result.status();
        encoded = result.take_value();
      }
      require(text(p, "expected_source_tensor") == encoded,
              "effective source description assertion mismatch");
    }
    if (p.count("expected_source_layout"))
      require(text(p, "expected_source_layout") ==
                  format::detail::layout_assertion(input.planar_layout),
              "physical source layout assertion mismatch");

    OperationOutputSpecialization output;
    output.metadata = input;
    output.metadata.atomic_trailing_axes = 0;
    out.output_axis = out.source_axis;
    const auto source_channels = out.source_axis ? shape[*out.source_axis] : 1;
    require(source_channels <= 65536,
            "FMT-11 channel mapping exceeds bounded 65536 slots");
    std::vector<std::int64_t> remap(source_channels, -1);
    if (kind == Kind::ColorToGray) {
      const auto first =
          *std::min_element(out.components.begin(), out.components.end());
      for (std::uint64_t i = 0; i < source_channels; ++i) {
        const bool edited =
            std::find(out.components.begin(), out.components.end(), i) !=
            out.components.end();
        if (i == first)
          out.slots.push_back({0, 0});
        if (edited)
          continue;
        remap[i] = static_cast<std::int64_t>(out.slots.size());
        out.slots.push_back({-1, i});
      }
      output.metadata.descriptor.shape[*out.output_axis] = source_channels - 2;
    } else if (kind == Kind::GrayToColor) {
      if (!out.source_axis) {
        require(shape.size() < 8 && p.count("output_axis"),
                "axis-free Gray expansion requires output_axis and rank room");
        const auto axis = std::get<std::int64_t>(p.at("output_axis"));
        require(axis >= 0 && static_cast<std::uint64_t>(axis) <= shape.size(),
                "output_axis outside insertion range");
        out.output_axis = static_cast<std::uint32_t>(axis);
        output.metadata.descriptor.shape.insert(
            output.metadata.descriptor.shape.begin() + axis, 3);
      } else {
        require(!p.count("output_axis"),
                "output_axis only applies to axis-free Gray expansion");
        require(source_channels <= UINT64_MAX - 2,
                "Gray expansion channel overflow");
        output.metadata.descriptor.shape[*out.output_axis] =
            source_channels + 2;
      }
      for (std::uint64_t i = 0; i < source_channels; ++i) {
        if (i == out.components[0]) {
          for (int c = 0; c < 3; ++c)
            out.slots.push_back({c, 0});
        } else {
          remap[i] = static_cast<std::int64_t>(out.slots.size());
          out.slots.push_back({-1, i});
        }
      }
    } else {
      require(!p.count("output_axis"), "output_axis is inapplicable");
      for (std::uint64_t i = 0; i < source_channels; ++i) {
        int component = -1;
        for (unsigned c = 0; c < input_count; ++c)
          if (out.components[c] == i)
            component = static_cast<int>(c);
        out.slots.push_back({component, i});
        remap[i] = static_cast<std::int64_t>(i);
      }
    }
    if (description) {
      TensorDescription result = *description;
      const bool structural =
          kind == Kind::ColorToGray || kind == Kind::GrayToColor;
      if (structural || !raw) {
        std::vector<TensorColorGroup> groups;
        for (const auto& g : description->groups) {
          if (!raw && group == &g)
            continue;
          bool conflict = false;
          for (const auto n : g.indices)
            for (unsigned c = 0; c < input_count; ++c)
              conflict = conflict || n == out.components[c];
          if (g.alpha) {
            for (unsigned c = 0; c < input_count; ++c)
              conflict = conflict || *g.alpha == out.components[c];
          }
          if (conflict) {
            if (!raw)
              throw invalid(
                  "another group refers to an edited/deleted color component");
            if (structural)
              continue;
          }
          auto mapped = g;
          if (structural) {
            for (auto& n : mapped.indices)
              n = static_cast<std::uint64_t>(remap[n]);
            if (mapped.alpha)
              mapped.alpha = static_cast<std::uint64_t>(remap[*mapped.alpha]);
          }
          groups.push_back(std::move(mapped));
        }
        result.groups = std::move(groups);
        if (!description->channels.empty() || !raw ||
            described(top_interpretation(*description)) ||
            description->encoding || description->sampling) {
          result.channels.assign(out.slots.size(), {});
          for (std::size_t i = 0; i < out.slots.size(); ++i)
            if (out.slots[i].component < 0)
              result.channels[i] =
                  bypass_description(*description, out.slots[i].source);
        }
        if (structural) {
          clear_top(result);
          result.encoding.reset();
          result.component.reset();
        }
        result.channel_axis = out.output_axis;
        if (!out.source_axis && out.output_axis && !result.axes.empty())
          result.axes.insert(result.axes.begin() + *out.output_axis,
                             TensorAxisDescription{"channel", "", 0, 1});
      }
      if (!raw) {
        auto target = interpretation;
        target.model = target_model(kind, out.math.gray);
        target.analytic_binding.reset();
        target.profile.reset();
        target.configured.reset();
        target.convention = "relative-v1";
        target.association = "straight";
        if (!target.coordinates)
          target.coordinates.emplace();
        if (kind == Kind::LabToXyz || kind == Kind::OklabToXyz)
          target.coordinates->scale = "relative";
        if (kind == Kind::RgbToYcbcr)
          target.coordinates->ncl_coefficients = out.math.ncl;
        if (kind == Kind::ColorToGray || kind == Kind::Threshold ||
            kind == Kind::BinaryToGray)
          target.coordinates->gray_kind = gray_name(out.math.gray);
        if (kind == Kind::GrayToColor)
          target.coordinates->gray_kind.clear();
        const auto target_roles = roles(target.model);
        std::vector<TensorChannelDescription> converted;
        for (unsigned c = 0; c < target_roles.size(); ++c) {
          TensorChannelDescription item;
          item.role = target_roles[c];
          item.name = target_roles[c];
          item.unit = "1";
          const unsigned original =
              kind == Kind::ColorToGray
                  ? ((out.math.gray == GrayKind::LinearY ||
                      out.math.gray == GrayKind::EncodedLuma)
                         ? (out.math.gray == GrayKind::LinearY ? 1u : 0u)
                         : 0u)
              : single(kind) ? 0u
                             : c;
          item.sampling = source_components[original].sampling;
          if (!item.sampling && out.source_axis &&
              !description->channels.empty())
            item.sampling =
                description->channels[out.components[original]].sampling;
          if (!item.sampling)
            item.sampling = description->sampling;
          const bool hue =
              (target.model == "cielch" || target.model == "oklch")
                  ? c == 2
                  : (target.model == "hsl" || target.model == "hsv") && c == 0;
          if (hue)
            item.unit = out.math.output_pi ? "pi_multiple" : "radian";
          if ((target.model == "cielab" || target.model == "cielch") && c > 0 &&
              !hue)
            item.unit = target.model == "cielch" ? "C*" : c == 1 ? "a*" : "b*";
          if (target.coordinates->scale == "absolute" &&
              (target.model == "xyz" || (target.model == "xyy" && c == 2) ||
               (target.model == "gray" && out.math.gray == GrayKind::LinearY)))
            item.unit = "cd/m2";
          converted.push_back(std::move(item));
        }
        if (out.output_axis) {
          TensorColorGroup target_group;
          target_group.name = group ? group->name : "color";
          target_group.interpretation = target;
          if (group && group->alpha)
            target_group.alpha =
                static_cast<std::uint64_t>(remap[*group->alpha]);
          for (unsigned c = 0; c < converted.size(); ++c)
            for (std::size_t i = 0; i < out.slots.size(); ++i)
              if (out.slots[i].component == static_cast<int>(c)) {
                target_group.indices.push_back(i);
                target_group.components.push_back(converted[c]);
                result.channels[i] = converted[c];
                result.channels[i].interpretation = target;
              }
          result.groups.push_back(std::move(target_group));
          clear_top(result);
        } else {
          result.channels.clear();
          result.groups.clear();
          result.component = converted[0];
          result.component->interpretation = target;
          clear_top(result);
        }
      }
      auto valid =
          validate_tensor_description(result, output.metadata.descriptor);
      if (!valid.ok())
        throw valid;
      auto encoded = encode_tensor_description(result);
      if (!encoded.ok())
        throw encoded.status();
      auto& facets = output.metadata.facets;
      facets.erase(std::remove_if(facets.begin(), facets.end(),
                                  [](const auto& f) {
                                    return f.key ==
                                           "photospider.tensor-description";
                                  }),
                   facets.end());
      facets.push_back(encoded.take_value());
    }
    if (input.planar_layout) {
      auto image = *input.planar_layout;
      require(image.channel_axis == out.source_axis,
              "FMT-11 image selectors must use the actual planar channel axis");
      if (!out.source_axis && out.output_axis) {
        if (image.height_axis >= *out.output_axis)
          ++image.height_axis;
        if (image.width_axis >= *out.output_axis)
          ++image.width_axis;
      }
      image.channel_axis = out.output_axis;
      image.groups.clear();  // Physical groups are optional; semantic groups
                             // live in the facet.
      output.metadata.planar_layout = image;
      require(out.layout != Layout::View,
              "Q validated planar aliases are not supported by the current "
              "host; use auto/materialize or generic fragments");
    }
    // Piecewise channel-affine relation shared by demand and dirty propagation.
    std::vector<DependencyMapPiece> mappings;
    mappings.reserve(out.slots.size());
    for (std::size_t slot = 0; slot < out.slots.size(); ++slot) {
      std::vector<RegionDimension> dimensions;
      for (const auto n : output.metadata.descriptor.shape)
        dimensions.push_back({0, n});
      if (out.output_axis)
        dimensions[*out.output_axis] = {slot, 1};
      auto observation = Footprint::from_regions(
          output.metadata.descriptor.shape, {Region(std::move(dimensions))});
      if (!observation.ok())
        throw observation.status();
      DependencyMapPiece piece{observation.take_value(), {}};
      const auto& selected_slot = out.slots[slot];
      unsigned mask =
          selected_slot.component < 0
              ? 1
              : support(kind, static_cast<unsigned>(selected_slot.component),
                        out.math.gray);
      for (unsigned c = 0; c < 3; ++c) {
        if (!(mask & (1u << c)))
          continue;
        DependencyMappedNeed need;
        need.port = 0;
        need.roles = static_cast<std::uint32_t>(DependencyRole::Data);
        for (unsigned axis = 0; axis < shape.size(); ++axis) {
          DependencyAxis mapped;
          if (out.source_axis && axis == *out.source_axis) {
            mapped.observation_axis = -1;
            mapped.fixed = {selected_slot.component < 0 ? selected_slot.source
                                                        : out.components[c],
                            1};
          } else {
            mapped.observation_axis = static_cast<std::int32_t>(
                axis +
                (!out.source_axis && out.output_axis && axis >= *out.output_axis
                     ? 1
                     : 0));
          }
          need.axes.push_back(mapped);
        }
        piece.inputs.push_back(std::move(need));
      }
      mappings.push_back(std::move(piece));
    }
    output.static_dependency_pieces = std::move(mappings);
    output.regional_atomic = true;
    output.preserve_output_views =
        kind == Kind::ColorToGray && out.layout != Layout::Materialize;
    if (out.layout == Layout::View)
      output.maximum_output_payload_bytes = 0;
    auto constants = prepare_constants(out.math);
    if (!constants.ok())
      throw constants.status();
    out.constants = constants.take_value();
    OperationPreparation result;
    result.outputs.push_back(std::move(output));
    result.state = std::move(selected);
    return Answer(std::move(result));
  } catch (const Status& status) {
    return Answer(status);
  }
}

void source_coordinate(const Preparation& p,
                       const std::vector<std::uint64_t>& at,
                       std::vector<std::uint64_t>& source) {
  for (unsigned axis = 0; axis < source.size(); ++axis)
    source[axis] =
        at[axis +
           (!p.source_axis && p.output_axis && axis >= *p.output_axis ? 1 : 0)];
}
void advance(std::vector<std::uint64_t>& at, const Region& box) {
  for (std::size_t axis = at.size(); axis-- > 0;) {
    const auto& dim = box.dimensions()[axis];
    if (++at[axis] < dim.offset + dim.extent)
      break;
    at[axis] = dim.offset;
  }
}
Status located(Status status, const std::vector<std::uint64_t>& at,
               int component) {
  if (status.detail.origin == FailureOrigin::Domain) {
    status.message += " at [";
    for (std::size_t i = 0; i < at.size(); ++i)
      status.message += (i ? "," : "") + std::to_string(at[i]);
    status.message += "] model component " + std::to_string(component);
  }
  return status;
}
struct Scratch final {
  ModelMath math;
  static constexpr std::size_t kBatch = 64;
  std::array<std::array<double, kBatch>, 3> input{};
  std::array<double, kBatch> candidates{};
  std::array<std::array<std::uint64_t, 3>, kBatch> bits{};
  explicit Scratch(const MathConfig& config, const Constants& constants,
                   const RationalMath::Work& work)
      : math(config, constants, work) {}
};
Result<ValueFragments> generic_views(const DependencyPhase& phase,
                                     const Preparation& p, ModelMath& math) {
  using Answer = Result<ValueFragments>;
  const auto& descriptor = phase.query.output.descriptor;
  const auto& facets = phase.query.output.facets;
  const auto width = Value::element_size(descriptor.element_type);
  const auto& fragments = phase.inputs[0].fragments();
  const auto boxes = phase.query.outputs.boxes().size();
  if (p.slots.size() && fragments.size() > UINT64_MAX / p.slots.size())
    return Answer(invalid("Q view metadata count overflow"));
  const auto pieces = p.slots.size() * fragments.size();
  if (pieces && boxes > UINT64_MAX / pieces)
    return Answer(invalid("Q view metadata count overflow"));
  std::uint64_t facet_bytes = 0;
  for (const auto& f : facets)
    facet_bytes += f.payload.size() + f.key.size() + sizeof(f);
  numeric_ops::ArrayPublication publication(
      boxes * pieces, descriptor.shape.size(), facet_bytes);
  std::vector<Value> values;
  for (const auto& box : phase.query.outputs.boxes()) {
    const auto channels = box.dimensions()[*p.output_axis];
    for (std::uint64_t channel = channels.offset;
         channel < channels.offset + channels.extent; ++channel) {
      const auto& slot = p.slots[channel];
      const auto index =
          slot.component < 0
              ? slot.source
              : p.components[static_cast<unsigned>(
                    copied_component(p.math.kind, 0, p.math.gray))];
      for (const auto& fragment : fragments) {
        auto status = phase.consume_work(descriptor.shape.size() + 1);
        if (!status.ok())
          return Answer(status);
        const auto input_channel =
            fragment.region().dimensions()[*p.source_axis];
        if (index < input_channel.offset ||
            index - input_channel.offset >= input_channel.extent)
          continue;
        auto dimensions = box.dimensions();
        dimensions[*p.output_axis] = {channel, 1};
        bool empty = false;
        for (unsigned axis = 0; axis < dimensions.size(); ++axis) {
          if (axis == *p.output_axis)
            continue;
          const auto a = dimensions[axis];
          const auto b = fragment.region().dimensions()[axis];
          const auto start = std::max(a.offset, b.offset);
          const auto end = std::min(a.offset + a.extent, b.offset + b.extent);
          if (start >= end) {
            empty = true;
            break;
          }
          dimensions[axis] = {start, end - start};
        }
        if (empty)
          continue;
        const Region region(dimensions);
        std::vector<std::uint64_t> at, source;
        for (const auto d : dimensions)
          at.push_back(d.offset);
        source = at;
        source[*p.source_axis] = index;
        auto address = fragment.byte_address(source);
        if (!address.ok())
          return Answer(address.status());
        const auto origin = at;
        auto strides = fragment.layout().byte_strides;
        strides[*p.output_axis] = 0;  // One channel per owner-preserving view.
        if (slot.component >= 0 && p.math.semantic) {
          auto count = region.element_count();
          if (!count.ok())
            return Answer(count.status());
          for (std::uint64_t i = 0; i < count.value(); ++i) {
            source = at;
            source[*p.source_axis] = index;
            std::array<std::uint64_t, 3> bits{};
            auto read = phase.read(0, source,
                                   &bits[static_cast<unsigned>(copied_component(
                                       p.math.kind, 0, p.math.gray))],
                                   width);
            if (!read.ok())
              return Answer(read);
            auto checked = math.evaluate(bits, 0);
            if (!checked.ok())
              return Answer(located(checked.status(), at, 0));
            advance(at, region);
          }
        }
        auto view = Value::from_storage(
            descriptor, region, {address.value(), std::move(strides), origin},
            fragment.storage(), facets, phase.query.resources);
        if (!view.ok())
          return Answer(view.status());
        auto retained = publication.retain(view.take_value());
        if (!retained.ok())
          return Answer(retained.status());
        values.push_back(retained.take_value());
      }
    }
  }
  return publication.finish(descriptor, phase.query.outputs, values.data(),
                            values.size(), phase.sets, facets,
                            phase.query.resources);
}
Result<ValueFragments> generic(const DependencyPhase& phase,
                               const Preparation& p) {
  using Answer = Result<ValueFragments>;
  auto status = phase.consume_work(1);
  if (!status.ok())
    return Answer(status);
  input_internal::Float32Environment environment;
  MathConfig config = p.math;
  if (!environment.active())
    config.algorithm = Algorithm::Reference;
  const RationalMath::Work work = [&](std::uint64_t n) {
    return phase.consume_work(n);
  };
  auto storage = phase.allocator.allocate(sizeof(Scratch));
  if (!storage.ok())
    return Answer(storage.status());
  auto memory = storage.take_value();
  std::unique_ptr<Scratch, void (*)(Scratch*)> scratch(
      new (memory.data()) Scratch(config, *p.constants, work),
      [](Scratch* s) { s->~Scratch(); });
  if (p.math.kind == Kind::ColorToGray && p.layout != Layout::Materialize)
    return generic_views(phase, p, scratch->math);
  const auto& descriptor = phase.query.output.descriptor;
  const auto& facets = phase.query.output.facets;
  const auto width = Value::element_size(descriptor.element_type);
  std::uint64_t facet_bytes = 0;
  for (const auto& f : facets)
    facet_bytes += f.payload.size() + f.key.size() + sizeof(f);
  numeric_ops::ArrayPublication publication(
      phase.query.outputs.boxes().size(), descriptor.shape.size(), facet_bytes);
  std::vector<Value> values;
  for (const auto& box : phase.query.outputs.boxes()) {
    auto allocated = MutableValue::allocate(descriptor, box, phase.allocator);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto writer = allocated.take_value();
    auto count = box.element_count();
    if (!count.ok())
      return Answer(count.status());
    std::vector<std::uint64_t> at, source(p.source_shape.size());
    for (const auto dim : box.dimensions())
      at.push_back(dim.offset);
    for (std::uint64_t i = 0; i < count.value(); ++i) {
      status = phase.consume_work(1);
      if (!status.ok())
        return Answer(status);
      const auto& slot = p.slots[p.output_axis ? at[*p.output_axis] : 0];
      source_coordinate(p, at, source);
      std::uint64_t result = 0;
      if (slot.component < 0) {
        if (p.source_axis)
          source[*p.source_axis] = slot.source;
        status = phase.read(0, source, &result, width);
        if (!status.ok())
          return Answer(status);
      } else {
        const auto component = static_cast<unsigned>(slot.component);
        const auto mask = support(p.math.kind, component, p.math.gray);
        std::array<std::uint64_t, 3> bits{};
        for (unsigned c = 0; c < 3; ++c) {
          if (!(mask & (1u << c)))
            continue;
          if (p.source_axis)
            source[*p.source_axis] = p.components[c];
          status = phase.read(0, source, &bits[c], width);
          if (!status.ok())
            return Answer(status);
        }
        auto computed = scratch->math.evaluate(bits, component);
        if (!computed.ok())
          return Answer(located(computed.status(), at, slot.component));
        result = computed.value();
      }
      std::memcpy(writer.data() + i * width, &result, width);
      advance(at, box);
    }
    status = phase.consume_work(1);
    if (!status.ok())
      return Answer(status);
    auto value = std::move(writer).publish(facets, phase.query.resources);
    if (!value.ok())
      return Answer(value.status());
    auto retained = publication.retain(value.take_value());
    if (!retained.ok())
      return Answer(retained.status());
    values.push_back(retained.take_value());
  }
  return publication.finish(descriptor, phase.query.outputs, values.data(),
                            values.size(), phase.sets, facets,
                            phase.query.resources);
}
struct State final {
  const Preparation* prepared;
  bool requested = false;
  explicit State(const Preparation* p) : prepared(p) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    if (!requested) {
      requested = true;
      DependencyNeedBatch batch;
      batch.static_mapping = true;
      return Result<DependencyPoll>(std::move(batch));
    }
    auto answer = generic(phase, *prepared);
    return answer.ok() ? Result<DependencyPoll>(answer.take_value())
                       : Result<DependencyPoll>(answer.status());
  }
};
Status planar(const PlanarOperationInvocation& call) {
  const auto& p = *static_cast<const Preparation*>(call.prepared->state());
  const auto* budget = resource_internal::metadata_budget();
  const RationalMath::Work work = [&](std::uint64_t n) {
    if (call.cancellation.cancelled())
      return Status{ErrorCode::Cancelled,
                    "FMT-11 cancelled",
                    FailureReason::Cancelled,
                    {FailureOrigin::Cancellation, FailureScope::Run}};
    return budget ? budget->consume({n}) : Status::success();
  };
  auto status = work(1);
  if (!status.ok())
    return status;
  input_internal::Float32Environment environment;
  MathConfig config = p.math;
  if (!environment.active())
    config.algorithm = Algorithm::Reference;
  auto storage = call.allocator.allocate(sizeof(Scratch));
  if (!storage.ok())
    return storage.status();
  auto memory = storage.take_value();
  std::unique_ptr<Scratch, void (*)(Scratch*)> scratch(
      new (memory.data()) Scratch(config, *p.constants, work),
      [](Scratch* s) { s->~Scratch(); });
  const auto width =
      Value::element_size(call.output_metadata.descriptor.element_type);
  const auto& layout = *call.output_metadata.planar_layout;
  const auto y = call.output_region.dimensions()[layout.height_axis];
  const auto x = call.output_region.dimensions()[layout.width_axis];
  const auto channels = p.output_axis
                            ? call.output_region.dimensions()[*p.output_axis]
                            : RegionDimension{0, 1};
  std::vector<std::uint64_t> at, source(p.source_shape.size());
  for (const auto dim : call.output_region.dimensions())
    at.push_back(dim.offset);
  for (std::uint64_t channel = channels.offset;
       channel < channels.offset + channels.extent; ++channel) {
    if (p.output_axis)
      at[*p.output_axis] = channel;
    const auto& slot = p.slots[channel];
    const auto component = static_cast<unsigned>(std::max(0, slot.component));
    const auto mask =
        slot.component < 0 ? 1 : support(config.kind, component, config.gray);
    const bool candidate =
        slot.component >= 0 &&
        (config.kind == Kind::RgbToYcbcr || config.kind == Kind::YcbcrToRgb) &&
        config.profile != SequenceProfile::Strict &&
        config.algorithm != Algorithm::Reference;
    std::array<double, 3> coefficients{};
    if (candidate) {
      for (unsigned c = 0; c < 3; ++c) {
        const auto bound = p.constants->first_fast[component][c];
        coefficients[c] = bound.low + (bound.high - bound.low) * .5;
      }
    }
    for (std::uint64_t row = y.offset; row < y.offset + y.extent; ++row) {
      at[layout.height_axis] = row;
      for (std::uint64_t column = x.offset; column < x.offset + x.extent;) {
        status = work(1);
        if (!status.ok())
          return status;
        at[layout.width_axis] = column;
        source_coordinate(p, at, source);
        auto output = call.output.row_run(at);
        if (!output.ok())
          return output.status();
        std::uint64_t span =
            std::min(output.value().samples, x.offset + x.extent - column);
        std::array<const std::uint8_t*, 3> pointers{};
        for (unsigned c = 0; c < 3; ++c) {
          if (!(mask & (1u << c)))
            continue;
          if (p.source_axis)
            source[*p.source_axis] =
                slot.component < 0 ? slot.source : p.components[c];
          auto input = call.inputs[0].row_run(source);
          if (!input.ok())
            return input.status();
          span = std::min(span, input.value().samples);
          pointers[c] = input.value().data;
        }
        if (span == 0)
          return {ErrorCode::Internal, "FMT-11 empty row span"};
        for (std::uint64_t offset = 0; offset < span;) {
          const auto count =
              std::min<std::uint64_t>(Scratch::kBatch, span - offset);
          status = work(count);
          if (!status.ok())
            return status;
          if (slot.component < 0) {
            std::memcpy(output.value().data + offset * width,
                        pointers[0] + offset * width, count * width);
            offset += count;
            continue;
          }
          for (std::size_t lane = 0; lane < count; ++lane) {
            scratch->bits[lane] = {};
            for (unsigned c = 0; c < 3; ++c) {
              if (!(mask & (1u << c)))
                continue;
              std::memcpy(&scratch->bits[lane][c],
                          pointers[c] + (offset + lane) * width, width);
              if (candidate) {
                const auto classified = numeric_ops::BinaryParts::decode(
                    scratch->bits[lane][c], config.narrow);
                // Classification precedes every FP conversion, including sNaN.
                scratch->input[c][lane] =
                    classified.nan || classified.infinite
                        ? 0
                        : numeric_ops::numeric_double(scratch->bits[lane][c],
                                                      config.narrow);
              }
            }
          }
          if (candidate) {
            std::array<const double*, 3> inputs{};
            for (unsigned c = 0; c < 3; ++c)
              inputs[c] = scratch->input[c].data();
            dot_candidates(inputs, coefficients, mask,
                           scratch->candidates.data(), count, config.profile,
                           config.algorithm == Algorithm::Auto);
          }
          for (std::size_t lane = 0; lane < count; ++lane) {
            auto computed = scratch->math.evaluate(
                scratch->bits[lane], component,
                candidate ? std::optional<double>(scratch->candidates[lane])
                          : std::nullopt);
            if (!computed.ok()) {
              at[layout.width_axis] = column + offset + lane;
              return located(computed.status(), at, slot.component);
            }
            const auto bits = computed.value();
            std::memcpy(output.value().data + (offset + lane) * width, &bits,
                        width);
          }
          offset += count;
        }
        column += span;
      }
    }
  }
  return work(1);
}
OperationDefinition definition(Kind kind, SequenceProfile profile,
                               const char* suffix) {
  OperationDefinition def;
  def.key = std::string(operation_name(kind)) + suffix;
  auto& traits = def.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.cacheable = false;
  traits.planar_storage_capable = true;
  traits.requires_metadata_specialization = true;
  traits.workspace_bytes = sizeof(Scratch);
  // All statics are optional in the schema so preparation can supply defaults
  // and diagnose member-specific applicability, including Empty requests.
  const auto add = [&](const char* key, OperationParameterType type) {
    traits.parameter_schema.push_back({key, type, false});
  };
  for (const auto* key :
       {"algorithm", "black_value", "components", "expected_source_layout",
        "expected_source_shape", "expected_source_tensor", "gray_kind", "group",
        "input_hue_unit", "layout", "metadata_mode", "metadata_override",
        "ncl_matrix", "output_hue_unit", "white_value"})
    add(key, OperationParameterType::String);
  for (const auto* key : {"axis", "expected_source_dtype", "output_axis"})
    add(key, OperationParameterType::Int64);
  add("axis_free", OperationParameterType::Bool);
  for (const auto* key : {"gray_white_x", "gray_white_y", "kb", "kr",
                          "threshold", "white_x", "white_y"})
    add(key, OperationParameterType::Float64);
  std::sort(traits.parameter_schema.begin(), traits.parameter_schema.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(State);
  output.maximum_dependency_stages = 2;
  def.prepare_static = [kind, profile](const auto& inputs, const auto& params) {
    return prepare(kind, profile, inputs, params);
  };
  def.start_dependency = [](const DependencyQuery& query,
                            const BufferAllocator& allocator) {
    return DependencyContinuation::make<State>(
        allocator, static_cast<const Preparation*>(query.prepared->state()));
  };
  def.planar_callback = planar;
  return def;
}
}  // namespace
Status register_model_conversions(OperationRegistry* registry) {
  const std::array<std::pair<SequenceProfile, const char*>, 3> profiles{
      {{SequenceProfile::Strict, "_strict"},
       {SequenceProfile::AppleSilicon, "_accelerated_apple_silicon"},
       {SequenceProfile::X86Avx2, "_accelerated_x86_64"}}};
  for (unsigned member = 0; member < 20; ++member)
    for (const auto& profile : profiles) {
      auto status = registry->register_operation(
          definition(static_cast<Kind>(member), profile.first, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
