#include "photospider/format/rgb_basis.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_publication.hpp"
#include "02-format-color/rgb_basis_math.hpp"
#include "data/input_validation.hpp"
#include "photospider/data/region_runs.hpp"
#include "photospider/execution/data_movement.hpp"
#include "photospider/numeric/workflow_authoring.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal::basis_ops {
namespace {
using Parameters = std::map<std::string, ParameterValue>;
using data_internal::format_numeric::ExactWorkFailure;
using data_internal::format_numeric::ExactWorkScope;
struct CheckedFailure final {
  Status status;
};
Status invalid(const std::string& message) {
  return {ErrorCode::InvalidArgument,
          "FMT-10: " + message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
Status mismatch(const std::string& message) {
  return {ErrorCode::TypeMismatch,
          "FMT-10: " + message,
          FailureReason::None,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
void require(bool condition, const std::string& message) {
  if (!condition)
    throw CheckedFailure{invalid(message)};
}
void typed(bool condition, const std::string& message) {
  if (!condition)
    throw CheckedFailure{mismatch(message)};
}
template <class T>
T checked(Result<T> result) {
  if (!result.ok())
    throw CheckedFailure{result.status()};
  return result.take_value();
}
void checked(Status result) {
  if (!result.ok())
    throw CheckedFailure{std::move(result)};
}
std::string text(const Parameters& params, const char* key,
                 const char* default_value = "") {
  const auto it = params.find(key);
  return it == params.end() ? default_value : std::get<std::string>(it->second);
}
struct ResolvedBasis final {
  std::string preset;
  std::array<double, 6> xy;
  std::array<double, 2> white;
};
ResolvedBasis preset(const std::string& key) {
  if (key == "srgb_rec709")
    return {key, {.64, .33, .30, .60, .15, .06}, {.3127, .3290}};
  if (key == "display_p3")
    return {key, {.68, .32, .265, .69, .15, .06}, {.3127, .3290}};
  if (key == "rec2020")
    return {key, {.708, .292, .170, .797, .131, .046}, {.3127, .3290}};
  if (key == "adobe_rgb_1998")
    return {key, {.64, .33, .21, .71, .15, .06}, {.3127, .3290}};
  if (key == "prophoto_rgb")
    return {key,
            {.734699, .265301, .159597, .840403, .036598, .000105},
            {.3457, .3585}};
  if (key == "aces_ap0")
    return {key, {.73470, .26530, 0, 1, .00010, -.077}, {.32168, .33767}};
  if (key == "aces_ap1")
    return {key, {.713, .293, .165, .830, .128, .044}, {.32168, .33767}};
  throw CheckedFailure{invalid("unknown basis preset")};
}
ResolvedBasis resolve(const format::RgbBasis& basis) {
  ResolvedBasis out;
  if (!basis.preset.empty()) {
    out = preset(basis.preset);
    require(!basis.primaries_xy || *basis.primaries_xy == out.xy,
            "preset/custom primaries disagree");
    require(!basis.white || *basis.white == out.white,
            "preset/custom white disagree");
  } else {
    require(basis.primaries_xy && basis.white,
            "custom basis requires all xy coordinates");
    out = {"", *basis.primaries_xy, *basis.white};
  }
  for (auto& value : out.xy) {
    require(finite_bits(bits(value), false), "nonfinite primary xy");
    if (value == 0)
      value = 0;  // Metadata zero canonicalization.
  }
  for (auto& value : out.white) {
    require(finite_bits(bits(value), false), "nonfinite white xy");
    if (value == 0)
      value = 0;
  }
  return out;
}
std::string hex_word(double x) {
  auto raw = bits(x);
  if (!(raw & UINT64_C(0x7fffffffffffffff)))
    raw = 0;
  static constexpr char digits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (unsigned i = 0; i < 16; ++i)
    out[i] = digits[(raw >> ((15 - i) * 4)) & 15];
  return out;
}
std::vector<double> parse_words(const std::string& value,
                                const std::string& prefix, unsigned count) {
  require(value.size() == prefix.size() + count * 17 - 1 &&
              value.compare(0, prefix.size(), prefix) == 0,
          "malformed geometry codec");
  std::vector<double> out;
  for (unsigned j = 0; j < count; ++j) {
    if (j)
      require(value[prefix.size() + j * 17 - 1] == ',',
              "malformed xy separator");
    std::uint64_t raw = 0;
    for (unsigned i = 0; i < 16; ++i) {
      const char c = value[prefix.size() + j * 17 + i];
      require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'),
              "noncanonical xy hex");
      raw =
          (raw << 4) | static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    require(finite_bits(raw, false) && raw != UINT64_C(0x8000000000000000),
            "xy must be finite and zero-canonical");
    double x;
    std::memcpy(&x, &raw, 8);
    out.push_back(x);
  }
  return out;
}
std::string basis_codec(const ResolvedBasis& basis) {
  if (!basis.preset.empty())
    return basis.preset;
  std::string out = "basis-v1:";
  for (unsigned j = 0; j < 8; ++j) {
    if (j)
      out += ',';
    out += hex_word(j < 6 ? basis.xy[j] : basis.white[j - 6]);
  }
  return out;
}
ResolvedBasis parse_basis(const std::string& value) {
  if (value.compare(0, 9, "basis-v1:"))
    return preset(value);
  const auto words = parse_words(value, "basis-v1:", 8);
  ResolvedBasis out;
  std::copy_n(words.begin(), 6, out.xy.begin());
  std::copy_n(words.begin() + 6, 2, out.white.begin());
  return out;
}
std::array<double, 2> parse_white(const std::string& value) {
  if (value == "d65")
    return {.3127, .3290};
  if (value == "d50")
    return {.3457, .3585};
  if (value == "aces")
    return {.32168, .33767};
  const auto words = parse_words(value, "xy-v1:", 2);
  return {words[0], words[1]};
}
std::string white_codec(const std::array<double, 2>& white) {
  return "xy-v1:" + hex_word(white[0]) + ',' + hex_word(white[1]);
}
unsigned method_index(const std::string& method) {
  if (method == "xyz_scaling")
    return 0;
  if (method == "bradford")
    return 1;
  if (method == "cat02")
    return 2;
  if (method == "cat16")
    return 3;
  throw CheckedFailure{
      invalid("method must be xyz_scaling, bradford, cat02 or cat16")};
}
std::array<std::uint64_t, 3> components(const std::string& value) {
  std::array<std::uint64_t, 3> out{};
  std::size_t at = 0;
  for (unsigned j = 0; j < 3; ++j) {
    const auto end = value.find(',', at);
    require((j == 2) == (end == std::string::npos),
            "components require exactly three indices");
    const auto token =
        value.substr(at, end == std::string::npos ? end : end - at);
    const auto parsed =
        std::from_chars(token.data(), token.data() + token.size(), out[j]);
    require(!token.empty() && parsed.ec == std::errc{} &&
                parsed.ptr == token.data() + token.size() &&
                token == std::to_string(out[j]),
            "noncanonical component index");
    at = end == std::string::npos ? value.size() : end + 1;
  }
  require(out[0] != out[1] && out[0] != out[2] && out[1] != out[2],
          "repeated component index");
  return out;
}
TensorInterpretation global_interpretation(const TensorDescription& d) {
  TensorInterpretation out;
  out.model = d.model;
  out.primaries = d.primaries;
  out.transfer = d.transfer;
  out.reference = d.reference;
  out.association = d.association;
  out.white = d.white;
  out.primaries_xy = d.primaries_xy;
  out.profile = d.profile;
  out.convention = d.convention;
  out.configured = d.configured;
  out.analytic_binding = d.analytic_binding;
  return out;
}
bool described(const TensorInterpretation& value) {
  return !value.model.empty() || !value.primaries.empty() ||
         !value.transfer.empty() || !value.reference.empty() ||
         !value.association.empty() || value.white || value.primaries_xy ||
         value.profile || value.configured || value.analytic_binding;
}
void overlay(TensorInterpretation* target, const TensorInterpretation& source) {
  // A default-constructed, absent global/channel record makes no convention
  // assertion. Explicit records must agree, including opaque resource identity.
  const bool had_description = described(*target);
  if (described(source)) {
    typed(!had_description || target->convention == source.convention,
          "conflicting coordinate conventions");
    if (!had_description)
      target->convention = source.convention;
  }
  typed(!target->profile || !source.profile ||
            *target->profile == *source.profile,
        "conflicting profile identity");
  typed(!target->configured || !source.configured ||
            *target->configured == *source.configured,
        "conflicting configuration identity");
  typed(!target->analytic_binding || !source.analytic_binding ||
            *target->analytic_binding == *source.analytic_binding,
        "conflicting analytic binding");
  for (auto pair :
       {std::make_pair(&target->model, &source.model),
        std::make_pair(&target->primaries, &source.primaries),
        std::make_pair(&target->transfer, &source.transfer),
        std::make_pair(&target->reference, &source.reference),
        std::make_pair(&target->association, &source.association)}) {
    typed(pair.first->empty() || pair.second->empty() ||
              *pair.first == *pair.second,
          "conflicting effective interpretation");
    if (pair.first->empty())
      *pair.first = *pair.second;
  }
  typed(!target->white || !source.white || *target->white == *source.white,
        "conflicting effective white");
  typed(!target->primaries_xy || !source.primaries_xy ||
            *target->primaries_xy == *source.primaries_xy,
        "conflicting effective primaries");
  if (!target->white)
    target->white = source.white;
  if (!target->primaries_xy)
    target->primaries_xy = source.primaries_xy;
  if (!target->profile)
    target->profile = source.profile;
  if (!target->configured)
    target->configured = source.configured;
  if (!target->analytic_binding)
    target->analytic_binding = source.analytic_binding;
}
Rational endpoint(const TensorEndpoint& value) {
  if (const auto* v = std::get_if<std::int64_t>(&value))
    return Rational::integer(*v);
  if (const auto* v = std::get_if<double>(&value))
    return number(*v);
  const auto& v = std::get<TensorRationalEndpoint>(value);
  Rational out;
  out.n.words.assign(v.numerator.begin(), v.numerator.end());
  out.d.words.assign(v.denominator.begin(), v.denominator.end());
  out.n.trim();
  out.d.trim();
  out.negative = v.negative;
  return out;
}
void native_encoding(const std::optional<TensorEncoding>& value) {
  if (!value)
    return;
  typed(
      endpoint(value->stored[0]).compare(endpoint(value->decoded[0])) == 0 &&
          endpoint(value->stored[1]).compare(endpoint(value->decoded[1])) == 0,
      "semantic samples must use native numeric encoding; decode explicitly");
}
struct Preparation final {
  std::array<ExactRow, 3> rows;
  std::array<std::uint64_t, 3> selected{};
  unsigned axis = 0;
  bool narrow = false, semantic = true, is_identity = false,
       materialize = false;
  SequenceProfile profile = SequenceProfile::Strict;
  std::optional<ResolvedBasis> source_basis;
  std::array<double, 2> source_white{};
  Preparation(const Matrix& matrix)
      : rows{{ExactRow(matrix, 0), ExactRow(matrix, 1), ExactRow(matrix, 2)}} {
    is_identity = identity(matrix);
  }
  int row(std::uint64_t channel) const {
    for (unsigned i = 0; i < 3; ++i)
      if (channel == selected[i])
        return static_cast<int>(i);
    return -1;
  }
};

OperationPreparation prepare_impl(const std::vector<OperationMetadata>& inputs,
                                  const Parameters& params, unsigned member,
                                  SequenceProfile profile) {
  input_internal::Float32Environment environment;
  typed(environment.active(), "floating environment unavailable");
  checked(numeric_ops::sequence_profile_available(profile));
  require(inputs.size() == 1, "one input required");
  const auto& input = inputs[0];
  const auto& shape = input.descriptor.shape;
  typed(!input.result_schema && !shape.empty() && shape.size() <= 8 &&
            (input.descriptor.element_type == ElementType::Float32 ||
             input.descriptor.element_type == ElementType::Float64),
        "requires Float32/64 rank 1..8");
  std::uint64_t count = 1;
  for (const auto extent : shape) {
    typed(extent && extent <= (UINT64_C(1) << 40) / count,
          "tensor exceeds 2^40 samples");
    count *= extent;
  }
  const auto mode = text(params, "metadata_mode", "respect");
  require(mode == "respect" || mode == "override" || mode == "raw",
          "unknown metadata mode");
  require((mode == "override") ==
              static_cast<bool>(params.count("metadata_override")),
          "override payload/mode mismatch");
  const bool semantic = mode != "raw";
  const auto layout = text(params, "layout", "auto");
  require(layout == "auto" || layout == "view" || layout == "materialize",
          "unknown layout");
  std::optional<TensorDescription> desc;
  for (const auto& facet : input.facets) {
    // Do not revive retired typed Image/ColorArray-v1 conversions in raw mode.
    typed(facet.key != "photospider.image" &&
              facet.key != "photospider.color-array" &&
              facet.key != "photospider.semantic",
          "legacy typed facets need explicit generic/planar migration");
    if (facet.key == "photospider.tensor-description")
      desc = checked(decode_tensor_description(facet));
  }
  if (mode == "override")
    desc = checked(
        tensor_description_from_parameter(text(params, "metadata_override")));
  if (desc)
    checked(validate_tensor_description(*desc, input.descriptor));
  std::array<std::uint64_t, 3> selected{};
  unsigned axis = 0, group_index = 0;
  TensorInterpretation effective;
  std::array<std::string, 3> selected_units;
  const std::array<std::string, 3> rgb_roles{{"red", "green", "blue"}},
      xyz_roles{{"x", "y", "z"}};
  const auto& source_roles = member == 0 ? rgb_roles : xyz_roles;
  if (!semantic) {
    require(!params.count("group") && params.count("components"),
            "raw needs components and forbids group");
    selected = components(text(params, "components"));
    if (params.count("axis")) {
      const auto a = std::get<std::int64_t>(params.at("axis"));
      require(a >= 0 && static_cast<std::uint64_t>(a) < shape.size(),
              "axis outside rank");
      axis = static_cast<unsigned>(a);
    } else {
      require(desc && desc->channel_axis,
              "raw needs explicit or described axis");
      axis = *desc->channel_axis;
    }
  } else {
    require(!params.count("components") && !params.count("axis"),
            "semantic group forbids raw selectors");
    require(desc && desc->channel_axis && params.count("group"),
            "semantic input needs v4 channel axis and explicit group");
    axis = *desc->channel_axis;
    bool found = false;
    for (unsigned i = 0; i < desc->groups.size(); ++i)
      if (desc->groups[i].name == text(params, "group")) {
        require(!found, "ambiguous group");
        found = true;
        group_index = i;
      }
    require(found, "selected group is absent");
    auto& group = desc->groups[group_index];
    effective = group.interpretation;
    overlay(&effective, global_interpretation(*desc));
    typed(effective.model == (member == 0 ? "rgb" : "xyz") &&
              group.indices.size() == 3,
          "selected group has wrong model");
    native_encoding(desc->encoding);
    // Resolve every selected channel before consuming a binding. In particular,
    // never index an inherited binding before checking its complete role order.
    for (unsigned j = 0; j < 3; ++j) {
      const auto it = std::find_if(
          group.components.begin(), group.components.end(),
          [&](const auto& c) { return c.role == source_roles[j]; });
      require(it != group.components.end(), "incomplete color roles");
      const auto k = static_cast<unsigned>(it - group.components.begin());
      selected[j] = group.indices[k];
      native_encoding(it->encoding);
      selected_units[j] = it->unit;
      if (selected[j] < desc->channels.size()) {
        const auto& channel = desc->channels[selected[j]];
        native_encoding(channel.encoding);
        if (selected_units[j].empty())
          selected_units[j] = channel.unit;
        if (channel.interpretation)
          overlay(&effective, *channel.interpretation);
      }
    }
    typed(effective.association.empty() || effective.association == "straight",
          "premultiplied semantic boundary is unsupported");
    if (effective.profile || effective.configured) {
      typed(effective.analytic_binding.has_value(),
            "profile/configured source needs explicit analytic binding");
      const auto& b = *effective.analytic_binding;
      typed(b.model == effective.model && b.roles.size() == 3 &&
                b.units.size() == 3,
            "analytic binding model/order mismatch");
      for (unsigned k = 0; k < 3; ++k)
        typed(b.roles[k] == group.components[k].role,
              "analytic binding role order mismatch");
      TensorInterpretation analytic;
      analytic.model = b.model;
      analytic.primaries = b.primaries;
      analytic.transfer = b.transfer;
      analytic.reference = b.reference;
      analytic.white = b.white;
      analytic.primaries_xy = b.primaries_xy;
      analytic.convention = b.convention;
      // The explicit caller binding, not the ICC/OCIO label, asserts analytic
      // native coordinates. No profile engine, PCS white guess or transform
      // runs.
      effective.convention = b.convention;
      overlay(&effective, analytic);
    }
    typed(effective.convention == "relative-v1",
          "unsupported coordinate convention");
    std::string unit;
    for (unsigned j = 0; j < 3; ++j) {
      auto& current = selected_units[j];
      if (effective.analytic_binding) {
        const auto k = static_cast<unsigned>(
            std::find(group.indices.begin(), group.indices.end(), selected[j]) -
            group.indices.begin());
        const auto& declared = effective.analytic_binding->units[k];
        typed(current.empty() || current == declared,
              "analytic binding unit mismatch");
        if (current.empty())
          current = declared;
      }
      typed(current == "1" || current == "cd/m2",
            "native component unit must be 1 or cd/m2");
      typed(unit.empty() || unit == current, "component units disagree");
      unit = current;
    }
    const auto& ref = effective.reference;
    typed(ref == "scene_relative" || ref == "display_relative" ||
              ref == "absolute_display_cd_m2" || ref == "scene" ||
              ref == "display",
          "explicit scene/display reference required");
    typed(unit == "1" ? ref != "absolute_display_cd_m2"
                      : (ref == "absolute_display_cd_m2" || ref == "display"),
          "unit/reference combination is incoherent");
    typed(
        member == 0
            ? effective.transfer == "linear"
            : (effective.transfer.empty() || effective.transfer == "linear") &&
                  effective.primaries.empty() && !effective.primaries_xy,
        "requires native linear RGB or coherent XYZ");
    for (unsigned i = 0; i < desc->groups.size(); ++i)
      if (i != group_index)
        for (auto c : desc->groups[i].indices)
          require(
              std::find(selected.begin(), selected.end(), c) == selected.end(),
              "selected group overlaps another group");
  }
  require(axis < shape.size(), "axis outside rank");
  for (auto c : selected)
    require(c < shape[axis], "component outside channel extent");
  if (input.planar_layout)
    typed(input.planar_layout->channel_axis &&
              *input.planar_layout->channel_axis == axis,
          "selected axis must be structural image channel axis");
  Matrix matrix;
  std::optional<ResolvedBasis> source, target;
  std::array<double, 2> source_white{}, target_white{};
  if (member == 0) {
    if (semantic) {
      source = resolve(
          {effective.primaries, effective.primaries_xy, effective.white});
      if (params.count("source_basis")) {
        const auto assertion = parse_basis(text(params, "source_basis"));
        require(assertion.xy == source->xy && assertion.white == source->white,
                "source basis assertion disagrees");
      }
    } else {
      require(params.count("source_basis"), "raw A requires source_basis");
      source = parse_basis(text(params, "source_basis"));
    }
    source_white = source->white;
    matrix = rgb_matrix(source->xy, source->white);
    target_white = source_white;
  } else if (member == 1) {
    require(params.count("target_basis"), "B requires target_basis");
    target = parse_basis(text(params, "target_basis"));
    target_white = target->white;
    if (params.count("target_white"))
      require(parse_white(text(params, "target_white")) == target_white,
              "duplicate target white disagrees");
    if (semantic) {
      typed(effective.white.has_value(), "XYZ source white is required");
      source_white = *effective.white;
      (void)white_xyz(source_white);
      if (params.count("source_white"))
        require(parse_white(text(params, "source_white")) == source_white,
                "source white assertion disagrees");
      const auto policy = text(params, "white_handling", "require_match");
      require(policy == "require_match" || policy == "preserve_xyz",
              "B never adapts white");
      typed(policy == "preserve_xyz" || source_white == target_white,
            "source/target whites differ under require_match");
    } else {
      require(!params.count("white_handling") && !params.count("source_white"),
              "raw B forbids semantic white assertions/policy");
    }
    matrix = inverse(rgb_matrix(target->xy, target->white));
  } else {
    require(params.count("target_white") && params.count("method"),
            "C requires target_white and method");
    target_white = parse_white(text(params, "target_white"));
    if (semantic) {
      typed(effective.white.has_value(), "XYZ source white is required");
      source_white = *effective.white;
      if (params.count("source_white"))
        require(parse_white(text(params, "source_white")) == source_white,
                "source white assertion disagrees");
    } else {
      require(params.count("source_white"), "raw C requires source_white");
      source_white = parse_white(text(params, "source_white"));
    }
    matrix = adaptation(source_white, target_white,
                        method_index(text(params, "method")));
  }
  auto state = std::make_shared<Preparation>(matrix);
  state->axis = axis;
  state->selected = selected;
  state->semantic = semantic;
  state->narrow = input.descriptor.element_type == ElementType::Float32;
  state->materialize = layout == "materialize";
  state->profile = profile;
  state->source_basis = source;
  state->source_white = source_white;
  require(layout != "view" || state->is_identity,
          "ViewUnavailable: nonidentity transform");
  OperationOutputSpecialization output;
  output.metadata = input;
  output.metadata.atomic_trailing_axes = 0;
  if (semantic) {
    auto& group = desc->groups[group_index];
    TensorInterpretation updated = effective;
    updated.model = member == 1 ? "rgb" : "xyz";
    updated.white = target_white;
    updated.profile.reset();
    updated.configured.reset();
    updated.analytic_binding.reset();
    updated.primaries = target ? target->preset : "";
    updated.primaries_xy =
        target ? std::optional<std::array<double, 6>>(target->xy)
               : std::nullopt;
    updated.transfer = member == 1 ? "linear" : "";
    const auto& target_roles = member == 1 ? rgb_roles : xyz_roles;
    const auto global = global_interpretation(*desc);
    // Preserve inherited descriptions of unrelated groups before clearing
    // global RGB-only fields that can no longer describe a mixed-model tensor.
    for (auto& other : desc->groups)
      overlay(&other.interpretation, global);
    group.interpretation = updated;
    for (unsigned j = 0; j < 3; ++j) {
      const auto it =
          std::find(group.indices.begin(), group.indices.end(), selected[j]);
      const auto k = static_cast<unsigned>(it - group.indices.begin());
      group.components[k].role = target_roles[j];
      // Units previously supplied solely by a dropped binding must survive.
      group.components[k].unit = selected_units[j];
      if (selected[j] < desc->channels.size()) {
        auto& channel = desc->channels[selected[j]];
        channel.role = target_roles[j];
        channel.unit = selected_units[j];
        if (channel.interpretation)
          channel.interpretation = updated;
      }
    }
    if (desc->groups.size() == 1) {
      desc->model = updated.model;
      desc->primaries = updated.primaries;
      desc->transfer = updated.transfer;
      desc->reference = updated.reference;
      desc->association = updated.association;
      desc->white = updated.white;
      desc->primaries_xy = updated.primaries_xy;
    } else {
      desc->model.clear();
      desc->primaries.clear();
      desc->transfer.clear();
      desc->reference.clear();
      desc->association.clear();
      desc->white.reset();
      desc->primaries_xy.reset();
    }
    desc->profile.reset();
    desc->configured.reset();
    desc->analytic_binding.reset();
    desc->convention = "relative-v1";
    checked(validate_tensor_description(*desc, input.descriptor));
    output.metadata.facets.clear();
    for (const auto& f : input.facets)
      if (f.key != "photospider.tensor-description")
        output.metadata.facets.push_back(f);
    output.metadata.facets.push_back(checked(encode_tensor_description(*desc)));
  }
  if (input.planar_layout)
    output.metadata.planar_layout->row_pitch_bytes = 0;
  std::vector<std::uint64_t> boundaries{0, shape[axis]};
  for (auto c : selected) {
    boundaries.push_back(c);
    boundaries.push_back(c + 1);
  }
  std::sort(boundaries.begin(), boundaries.end());
  boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                   boundaries.end());
  std::vector<DependencyMapPiece> pieces;
  for (unsigned i = 1; i < boundaries.size(); ++i) {
    auto dims = Region::whole(shape).dimensions();
    dims[axis] = {boundaries[i - 1], boundaries[i] - boundaries[i - 1]};
    auto coverage = checked(Footprint::from_regions(shape, {Region(dims)}));
    const bool color = state->row(boundaries[i - 1]) >= 0;
    std::vector<DependencyMappedNeed> needs;
    const unsigned terms = color && !state->is_identity ? 3 : 1;
    for (unsigned j = 0; j < terms; ++j) {
      DependencyMappedNeed need;
      need.roles = static_cast<std::uint32_t>(DependencyRole::Data);
      if (semantic && color)
        need.roles |= static_cast<std::uint32_t>(DependencyRole::Validation);
      for (unsigned a = 0; a < shape.size(); ++a)
        need.axes.push_back(
            terms == 3 && a == axis
                ? DependencyAxis{-1, {selected[j], 1}, 0}
                : DependencyAxis{static_cast<std::int32_t>(a), {0, 1}, 0});
      needs.push_back(std::move(need));
    }
    DependencyMappedNeed descriptor;
    descriptor.roles = static_cast<std::uint32_t>(DependencyRole::Descriptor);
    descriptor.tags.push_back({1, 0});
    needs.push_back(std::move(descriptor));
    pieces.push_back({std::move(coverage), std::move(needs)});
  }
  output.static_dependency_pieces = std::move(pieces);
  output.regional_atomic = true;
  output.preserve_output_views =
      state->is_identity && !state->materialize && !input.planar_layout;
  if (output.preserve_output_views)
    output.maximum_output_payload_bytes = 0;
  if (state->is_identity && input.planar_layout) {
    output.data_movement = DataMovementKind::BitwiseMapped;
    output.data_movement_view_policy =
        layout == "view"     ? DataMovementViewPolicy::RequireView
        : state->materialize ? DataMovementViewPolicy::Materialize
                             : DataMovementViewPolicy::Auto;
  }
  OperationPreparation result;
  result.outputs.push_back(std::move(output));
  result.state = std::move(state);
  return result;
}
Result<OperationPreparation> prepare(
    const std::vector<OperationMetadata>& inputs, const Parameters& params,
    unsigned member, SequenceProfile profile) {
  try {
    return Result<OperationPreparation>(
        prepare_impl(inputs, params, member, profile));
  } catch (const CheckedFailure& f) {
    return Result<OperationPreparation>(f.status);
  } catch (const GeometryError&) {
    return Result<OperationPreparation>(invalid(
        "invalid white, singular basis, or nonpositive adaptation response"));
  } catch (const ExactWorkFailure& f) {
    return Result<OperationPreparation>(f.status);
  }
}
Status sample_failure(const char* message, FailureReason reason,
                      const std::vector<std::uint64_t>& at) {
  AtomKey atom;
  atom.rank = static_cast<std::uint32_t>(at.size());
  std::copy(at.begin(), at.end(), atom.coordinate.begin());
  return {ErrorCode::OperationFailed,
          std::string("FMT-10: ") + message,
          reason,
          {FailureOrigin::Domain, FailureScope::Atom, atom}};
}
NumericDiagnostics diagnostics(const Preparation& state) {
  NumericDiagnostics out;
  out.profile = state.profile == SequenceProfile::Strict
                    ? CpuNumericProfile::Strict
                : state.profile == SequenceProfile::AppleSilicon
                    ? CpuNumericProfile::AppleSiliconNeon
                    : CpuNumericProfile::X86Avx2;
  const char* name =
      "photospider.fmt10/"
      "2;exact-rational;outward-bits;row-runs;no-fast-math;rn;fp-contract=off;"
#if defined(__APPLE__) && defined(__aarch64__)
      "Darwin/arm64;"
#elif defined(__FreeBSD__)
      "FreeBSD;"
#elif defined(__linux__) && defined(__x86_64__)
      "Linux/x86_64;"
#else
      "portable;"
#endif
#ifdef PHOTOSPIDER_FMT10_EXACT_ONLY
      "exact-only;"
#elif defined(PHOTOSPIDER_FMT10_NO_SIMD)
      "scalar;"
#else
      "simd;"
#endif
      __clang_version__;
  std::strncpy(out.implementation.data(), name, out.implementation.size() - 1);
  return out;
}
struct Workspace final {
  CandidateBlock candidate;
  std::array<std::array<std::uint64_t, block_size>, 3> raw{};
  std::array<std::uint64_t, block_size> result{}, packed_offsets{};
  std::array<bool, block_size> finite{};
};
void fallback(NumericDiagnostics* d, NumericFallbackReason reason) {
  ++d->strict_fallbacks;
  ++d->fallback_reasons[static_cast<unsigned>(reason)];
}
Status evaluate(
    Workspace* work, unsigned count, int row, const Preparation& state,
    NumericDiagnostics* report,
    const std::function<std::vector<std::uint64_t>(unsigned)>& coordinate) {
  const bool arithmetic = row >= 0 && !state.is_identity;
  bool use_candidate = false;
#ifndef PHOTOSPIDER_FMT10_EXACT_ONLY
  use_candidate = arithmetic && state.rows[row].candidate_available &&
                  (state.narrow || state.profile != SequenceProfile::Strict);
#endif
  if (arithmetic) {
    std::fill_n(work->finite.begin(), count, true);
    for (unsigned j = 0; j < 3; ++j)
      for (unsigned i = 0; i < count; ++i) {
        const bool finite = finite_bits(work->raw[j][i], state.narrow);
        work->finite[i] = work->finite[i] && finite;
        // Strict FP64/EXACT do not need a floating candidate or conversion.
        // Nonfinite classification stays integer-only, including sNaNs.
        if (!use_candidate)
          continue;
        if (!finite) {
          work->candidate.input[j][i] = 0;
        } else if (state.narrow) {
          const auto raw = static_cast<std::uint32_t>(work->raw[j][i]);
          float value;
          std::memcpy(&value, &raw, sizeof(value));
          work->candidate.input[j][i] = value;
        } else {
          std::memcpy(&work->candidate.input[j][i], &work->raw[j][i], 8);
        }
      }
    if (use_candidate)
      candidates(&work->candidate, count, state.rows[row], state.profile);
  }
  for (unsigned i = 0; i < count; ++i) {
    if (!arithmetic) {
      ++report->copied_elements;
      if (row >= 0 && state.semantic &&
          !finite_bits(work->raw[0][i], state.narrow))
        return sample_failure("nonfinite identity input",
                              FailureReason::InvalidDomain, coordinate(i));
      work->result[i] = work->raw[0][i];
      continue;
    }
    ++report->evaluated_values;
    const bool finite = work->finite[i];
    if (state.semantic && !finite)
      return sample_failure("nonfinite consumed color triple",
                            FailureReason::InvalidDomain, coordinate(i));
    if (!finite || !use_candidate ||
        !certify(state.rows[row], work->candidate, i, state.narrow,
                 state.profile, &work->result[i])) {
      if (state.profile != SequenceProfile::Strict)
        fallback(report, finite
                             ? NumericFallbackReason::RoundingUnresolved
                             : NumericFallbackReason::SpecialValueProtection);
      work->result[i] = state.rows[row].evaluate(
          {work->raw[0][i], work->raw[1][i], work->raw[2][i]}, state.narrow);
    }
    if (state.semantic && !finite_bits(work->result[i], state.narrow))
      return sample_failure("requested result overflow",
                            FailureReason::ArithmeticOverflow, coordinate(i));
  }
  return Status::success();
}
bool contains(const Region& region, const std::vector<std::uint64_t>& at) {
  for (unsigned a = 0; a < at.size(); ++a) {
    const auto d = region.dimensions()[a];
    if (at[a] < d.offset || at[a] - d.offset >= d.extent)
      return false;
  }
  return true;
}
struct Cursor final {
  const DependencyPhase& phase;
  std::array<const Value*, 3> previous{};
  Status read(const std::vector<std::uint64_t>& at, unsigned term,
              std::size_t width, std::uint64_t* out) {
    auto charged = phase.consume_work(at.size() + 1);
    if (!charged.ok())
      return charged;
    const Value* source = previous[term];
    if (!source || !contains(source->region(), at)) {
      charged = phase.consume_work(phase.inputs[0].fragments().size() *
                                   (at.size() + 1));
      if (!charged.ok())
        return charged;
      source = nullptr;
      for (const auto& fragment : phase.inputs[0].fragments())
        if (contains(fragment.region(), at)) {
          source = &fragment;
          break;
        }
      if (!source)
        return {ErrorCode::NotFound, "FMT-10 required sample is missing"};
      previous[term] = source;
    }
    auto address = source->byte_address(at);
    if (!address.ok())
      return address.status();
    *out = 0;
    std::memcpy(out, source->bytes().data() + address.value(), width);
    return Status::success();
  }
};
Result<ValueFragments> publish(const DependencyPhase& phase,
                               const Preparation& state, Workspace* work,
                               NumericDiagnostics* report) {
  using Answer = Result<ValueFragments>;
  const auto& descriptor = phase.query.output.descriptor;
  const auto& facets = phase.query.output.facets;
  const auto width = state.narrow ? 4U : 8U;
  std::uint64_t facet_bytes = 0;
  for (const auto& f : facets)
    facet_bytes += f.payload.size() + f.key.size();
  numeric_ops::ArrayPublication publication(
      phase.query.outputs.boxes().size() +
          (state.is_identity ? phase.inputs[0].fragments().size() : 0),
      descriptor.shape.size(), facet_bytes);
  ResourceVector<Value> values;
  Cursor cursor{phase, {}};
  std::vector<std::uint64_t> at;
  for (const auto& box : phase.query.outputs.boxes()) {
    if (state.is_identity && !state.materialize) {
      for (const auto& fragment : phase.inputs[0].fragments()) {
        std::vector<RegionDimension> overlap;
        bool intersects = true;
        for (unsigned a = 0; a < box.rank(); ++a) {
          const auto p = box.dimensions()[a],
                     q = fragment.region().dimensions()[a];
          const auto begin = std::max(p.offset, q.offset),
                     end = std::min(p.offset + p.extent, q.offset + q.extent);
          if (end <= begin) {
            intersects = false;
            break;
          }
          overlap.push_back({begin, end - begin});
        }
        if (!intersects)
          continue;
        Region region(std::move(overlap));
        auto selected =
            Footprint::from_regions(descriptor.shape, {region}, phase.sets);
        if (!selected.ok())
          return Answer(selected.status());
        auto status = selected.value().visit(
            [&](const auto& coordinate) {
              auto charged = phase.consume_work(coordinate.size() + 1);
              if (!charged.ok())
                return charged;
              if (state.semantic && state.row(coordinate[state.axis]) >= 0) {
                std::uint64_t raw;
                auto read = cursor.read(coordinate, 0, width, &raw);
                if (!read.ok())
                  return read;
                if (!finite_bits(raw, state.narrow))
                  return sample_failure("nonfinite identity input",
                                        FailureReason::InvalidDomain,
                                        coordinate);
              }
              ++report->view_elements;
              return Status::success();
            },
            phase.sets.maximum_work, phase.query.cancellation);
        if (!status.ok())
          return Answer(status);
        auto view = fragment.view(region);
        if (!view.ok())
          return Answer(view.status());
        auto mapped = Value::from_storage(
            descriptor, region, view.value().layout(), view.value().storage(),
            facets, phase.query.resources);
        if (!mapped.ok())
          return Answer(mapped.status());
        auto retained = publication.retain(mapped.take_value());
        if (!retained.ok())
          return Answer(retained.status());
        values.push_back(retained.take_value());
      }
      continue;
    }
    auto allocation = MutableValue::allocate(descriptor, box, phase.allocator);
    if (!allocation.ok())
      return Answer(allocation.status());
    auto writer = allocation.take_value();
    auto count = box.element_count();
    if (!count.ok())
      return Answer(count.status());
    // Walk one output component at a time, independently of logical channel
    // axis order. HWC generic tensors now fill SIMD batches too; the previous
    // contiguous-output walk would degenerate to one lane when C was last.
    const auto& channels = box.dimensions()[state.axis];
    const auto spatial_count = count.value() / channels.extent;
    for (auto channel = channels.offset;
         channel < channels.offset + channels.extent; ++channel) {
      auto plane_dimensions = box.dimensions();
      plane_dimensions[state.axis] = {channel, 1};
      const Region plane(std::move(plane_dimensions));
      const int row = state.row(channel);
      const unsigned terms = row >= 0 && !state.is_identity ? 3 : 1;
      for (std::uint64_t offset = 0; offset < spatial_count;) {
        if (phase.query.cancellation.cancelled())
          return Answer(Status{ErrorCode::Cancelled, "FMT-10 cancelled"});
        const auto n = static_cast<unsigned>(
            std::min<std::uint64_t>(block_size, spatial_count - offset));
        auto charged = phase.consume_work(n * 64);
        if (!charged.ok())
          return Answer(charged);
        for (unsigned lane = 0; lane < n; ++lane) {
          region_run_coordinate(plane, offset + lane, &at);
          std::uint64_t packed = 0;
          for (unsigned axis = 0; axis < at.size(); ++axis)
            packed = packed * box.dimensions()[axis].extent + at[axis] -
                     box.dimensions()[axis].offset;
          work->packed_offsets[lane] = packed;
          for (unsigned j = 0; j < terms; ++j) {
            at[state.axis] = terms == 3 ? state.selected[j] : channel;
            auto read = cursor.read(at, j, width, &work->raw[j][lane]);
            if (!read.ok())
              return Answer(read);
          }
        }
        auto status = evaluate(work, n, row, state, report, [&](unsigned lane) {
          std::vector<std::uint64_t> coordinate;
          region_run_coordinate(plane, offset + lane, &coordinate);
          return coordinate;
        });
        if (!status.ok())
          return Answer(status);
        for (unsigned lane = 0; lane < n; ++lane)
          std::memcpy(writer.data() + work->packed_offsets[lane] * width,
                      &work->result[lane], width);
        offset += n;
      }
    }
    auto value = std::move(writer).publish(facets, phase.query.resources);
    if (!value.ok())
      return Answer(value.status());
    auto retained = publication.retain(value.take_value());
    if (!retained.ok())
      return Answer(retained.status());
    values.push_back(retained.take_value());
  }
  if (phase.query.cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, "FMT-10 cancelled"});
  return publication.finish(descriptor, phase.query.outputs, values.data(),
                            values.size(), phase.sets, facets,
                            phase.query.resources);
}
struct Continuation final {
  const Preparation* state;
  bool requested = false;
  Workspace workspace;
  explicit Continuation(const Preparation* p) : state(p) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    if (!requested) {
      requested = true;
      DependencyNeedBatch batch;
      batch.static_mapping = true;
      return Result<DependencyPoll>(std::move(batch));
    }
    input_internal::Float32Environment environment;
    if (!environment.active())
      return Result<DependencyPoll>(
          mismatch("floating environment unavailable"));
    ExactWorkScope scope(&phase.consume_work, &phase.query.cancellation);
    auto report = diagnostics(*state);
    auto result = [&]() -> Result<ValueFragments> {
      try {
        return publish(phase, *state, &workspace, &report);
      } catch (const ExactWorkFailure& f) {
        return Result<ValueFragments>(f.status);
      } catch (const std::bad_alloc&) {
        return Result<ValueFragments>(
            Status{ErrorCode::ResourceExhausted,
                   "FMT-10 exact capacity/allocation exhausted",
                   FailureReason::CapacityLimit});
      }
    }();
    if (phase.report_numeric) {
      auto status = phase.report_numeric(report);
      if (!status.ok())
        return Result<DependencyPoll>(status);
    }
    return result.ok() ? Result<DependencyPoll>(result.take_value())
                       : Result<DependencyPoll>(result.status());
  }
};
template <bool Narrow>
Status planar_impl(const PlanarOperationInvocation& call) {
  input_internal::Float32Environment environment;
  if (!environment.active())
    return mismatch("floating environment unavailable");
  if (!call.prepared || !call.prepared->state())
    return {ErrorCode::Internal, "FMT-10 preparation absent"};
  const auto& state = *static_cast<const Preparation*>(call.prepared->state());
  const auto& layout = *call.output_metadata.planar_layout;
  const auto& dimensions = call.output_region.dimensions();
  const auto charge = [&](std::uint64_t amount) {
    if (call.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, "FMT-10 cancelled"};
    if (const auto* budget = resource_internal::metadata_budget())
      return budget->consume({amount});
    return Status::success();
  };
  const std::function<Status(std::uint64_t)> consume = charge;
  ExactWorkScope scope(&consume, &call.cancellation);
  // Accounted payload scratch rather than a hidden per-tile heap/cache.
  auto scratch = call.allocator.allocate(sizeof(Workspace));
  if (!scratch.ok())
    return scratch.status();
  auto scratch_buffer = scratch.take_value();
  auto* work = new (scratch_buffer.data()) Workspace();
  auto report = diagnostics(state);
  std::vector<std::uint64_t> at(dimensions.size());
  for (unsigned a = 0; a < at.size(); ++a)
    at[a] = dimensions[a].offset;
  constexpr auto width = Narrow ? 4U : 8U;
  try {
    for (std::uint64_t channel = dimensions[state.axis].offset;
         channel <
         dimensions[state.axis].offset + dimensions[state.axis].extent;
         ++channel) {
      const int row = state.row(channel);
      const unsigned terms = row >= 0 && !state.is_identity ? 3 : 1;
      for (std::uint64_t y = dimensions[layout.height_axis].offset;
           y < dimensions[layout.height_axis].offset +
                   dimensions[layout.height_axis].extent;
           ++y) {
        at[layout.height_axis] = y;
        for (std::uint64_t x = dimensions[layout.width_axis].offset;
             x < dimensions[layout.width_axis].offset +
                     dimensions[layout.width_axis].extent;) {
          at[layout.width_axis] = x;
          at[state.axis] = channel;
          auto target = call.output.row_run(at);
          if (!target.ok())
            return target.status();
          std::array<PlanarRowRun, 3> source;
          auto available = target.value().samples;
          for (unsigned j = 0; j < terms; ++j) {
            at[state.axis] = terms == 3 ? state.selected[j] : channel;
            auto run = call.inputs[0].row_run(at);
            if (!run.ok())
              return run.status();
            source[j] = run.take_value();
            available = std::min(available, source[j].samples);
          }
          // Keep the validated row-run pointers across 64-lane math blocks.
          // Reacquire only at an actual tile/coverage boundary, not every
          // block.
          for (std::uint64_t offset = 0; offset < available;) {
            const auto n = static_cast<unsigned>(
                std::min<std::uint64_t>(available - offset, block_size));
            auto status = charge(n * (64 + terms));
            if (!status.ok())
              return status;
            if (row < 0) {
              // Bypass components are a bitwise transfer, never arithmetic.
              std::memcpy(target.value().data + (offset * width),
                          source[0].data + (offset * width), n * width);
              report.copied_elements += n;
            } else {
              for (unsigned j = 0; j < terms; ++j)
                for (unsigned lane = 0; lane < n; ++lane) {
                  work->raw[j][lane] = 0;
                  std::memcpy(&work->raw[j][lane],
                              source[j].data + (offset + lane) * width, width);
                }
              at[state.axis] = channel;
              status =
                  evaluate(work, n, row, state, &report, [&](unsigned lane) {
                    auto coordinate = at;
                    coordinate[layout.width_axis] = x + offset + lane;
                    return coordinate;
                  });
              if (!status.ok())
                return status;
              for (unsigned lane = 0; lane < n; ++lane)
                std::memcpy(target.value().data + (offset + lane) * width,
                            &work->result[lane], width);
            }
            offset += n;
          }
          x += available;
        }
      }
    }
  } catch (const ExactWorkFailure& f) {
    return f.status;
  } catch (const std::bad_alloc&) {
    return {ErrorCode::ResourceExhausted,
            "FMT-10 exact capacity/allocation exhausted",
            FailureReason::CapacityLimit};
  }
  return charge(0);
}
Status planar(const PlanarOperationInvocation& call) {
  if (!call.prepared || !call.prepared->state())
    return {ErrorCode::Internal, "FMT-10 preparation absent"};
  const auto& state = *static_cast<const Preparation*>(call.prepared->state());
  return state.narrow ? planar_impl<true>(call) : planar_impl<false>(call);
}
template <bool Narrow>
Status validate_mapped_impl(const PlanarMappedValidationInvocation& call) {
  const auto& state = *static_cast<const Preparation*>(call.prepared->state());
  if (!state.is_identity || !state.semantic || call.input_port != 0)
    return Status{ErrorCode::Internal, "FMT-10 mapped validation invariant"};
  const auto& dims = call.input.region().dimensions();
  const auto& config = call.input.config();
  std::vector<std::uint64_t> at(dims.size());
  for (unsigned axis = 0; axis < at.size(); ++axis)
    at[axis] = dims[axis].offset;
  constexpr auto width = Narrow ? 4U : 8U;
  for (auto channel = dims[state.axis].offset;
       channel < dims[state.axis].offset + dims[state.axis].extent; ++channel) {
    if (state.row(channel) < 0)
      continue;
    at[state.axis] = channel;
    for (auto y = dims[config.height_axis].offset;
         y < dims[config.height_axis].offset + dims[config.height_axis].extent;
         ++y) {
      at[config.height_axis] = y;
      for (auto x = dims[config.width_axis].offset;
           x <
           dims[config.width_axis].offset + dims[config.width_axis].extent;) {
        if (call.cancellation.cancelled())
          return Status{ErrorCode::Cancelled,
                        "FMT-10 view validation cancelled"};
        at[config.width_axis] = x;
        auto run = call.input.row_run(at);
        if (!run.ok())
          return run.status();
        const auto available = run.value().samples;
        for (std::uint64_t offset = 0; offset < available;) {
          if (call.cancellation.cancelled())
            return Status{ErrorCode::Cancelled,
                          "FMT-10 view validation cancelled"};
          const auto count =
              std::min<std::uint64_t>(available - offset, block_size);
          if (const auto* budget = resource_internal::metadata_budget()) {
            auto status = budget->consume({count * (dims.size() + 1)});
            if (!status.ok())
              return status;
          }
          for (std::uint64_t lane = 0; lane < count; ++lane) {
            std::uint64_t raw = 0;
            std::memcpy(&raw, run.value().data + (offset + lane) * width,
                        width);
            if (!finite_bits(raw, Narrow)) {
              at[config.width_axis] = x + offset + lane;
              return sample_failure("nonfinite identity input",
                                    FailureReason::InvalidDomain, at);
            }
          }
          offset += count;
        }
        x += available;
      }
    }
  }
  return Status::success();
}
Status validate_mapped(const PlanarMappedValidationInvocation& call) {
  const auto& state = *static_cast<const Preparation*>(call.prepared->state());
  return state.narrow ? validate_mapped_impl<true>(call)
                      : validate_mapped_impl<false>(call);
}
OperationDefinition definition(unsigned member, const std::string& suffix,
                               SequenceProfile profile) {
  const std::array<const char*, 3> names{
      {"rgb_to_xyz", "xyz_to_rgb", "adapt_xyz_white"}};
  OperationDefinition op;
  op.key = std::string("color.") + names[member] + '_' + suffix;
  auto& traits = op.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.requires_metadata_specialization = true;
  traits.cacheable = false;
  traits.planar_storage_capable = true;
  traits.workspace_bytes = sizeof(Workspace) + 65536;
  for (const auto* name :
       {"metadata_mode", "group", "components", "metadata_override", "layout"})
    traits.parameter_schema.push_back(
        {name, OperationParameterType::String, false});
  traits.parameter_schema.push_back(
      {"axis", OperationParameterType::Int64, false});
  if (member == 0) {
    traits.parameter_schema.push_back(
        {"source_basis", OperationParameterType::String, false});
  } else {
    traits.parameter_schema.push_back(
        {"source_white", OperationParameterType::String, false});
    traits.parameter_schema.push_back(
        {"target_white", OperationParameterType::String, member == 2});
    if (member == 1) {
      traits.parameter_schema.push_back(
          {"target_basis", OperationParameterType::String, true});
      traits.parameter_schema.push_back(
          {"white_handling", OperationParameterType::String, false});
    } else {
      traits.parameter_schema.push_back(
          {"method", OperationParameterType::String, true});
    }
  }
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(Continuation);
  output.maximum_dependency_stages = 2;
  op.prepare_static = [member, profile](const auto& inputs,
                                        const auto& params) {
    return prepare(inputs, params, member, profile);
  };
  op.start_dependency = [](const DependencyQuery& query,
                           const BufferAllocator& allocator) {
    return DependencyContinuation::make<Continuation>(
        allocator, static_cast<const Preparation*>(query.prepared->state()));
  };
  op.planar_callback = planar;
  op.validate_planar_mapped = validate_mapped;
  return op;
}

// Resolve real graph metadata without trusting a supplied RGB interpretation.
// An explicit DFS stack bounds storage by reachable nodes, not C++ call depth.
OperationMetadata edge_metadata(const WorkflowDocument& document,
                                const WorkflowInput& edge,
                                const OperationRegistry& registry) {
  std::map<std::uint64_t, const WorkflowNode*> nodes;
  std::map<std::uint64_t, OperationMetadata> inputs;
  for (const auto& declaration : document.inputs) {
    auto canonical = declaration;
    checked(input_internal::validate_declaration(&canonical));
    OperationMetadata m{canonical.descriptor, canonical.facets};
    m.planar_layout = canonical.planar_layout;
    require(inputs.emplace(canonical.id, std::move(m)).second,
            "duplicate graph input id");
  }
  for (const auto& n : document.nodes)
    require(n.id && nodes.emplace(n.id, &n).second,
            "duplicate/zero graph node id");
  std::map<std::uint64_t, std::map<std::string, OperationMetadata>> outputs;
  std::map<std::uint64_t, unsigned> status;
  const auto resolve_edge =
      [&](const WorkflowInput& source) -> OperationMetadata {
    if (const auto* ref = std::get_if<WorkflowInputReference>(&source)) {
      const auto it = inputs.find(ref->input_id);
      require(it != inputs.end(), "input edge declaration is absent");
      return it->second;
    }
    const auto& ref = std::get<WorkflowNodeOutput>(source);
    const auto it = outputs.find(ref.source_node);
    require(it != outputs.end() && it->second.count(ref.source_port),
            "node/port is absent");
    return it->second.at(ref.source_port);
  };
  if (std::holds_alternative<WorkflowInputReference>(edge))
    return resolve_edge(edge);
  struct Frame final {
    std::uint64_t id;
    std::size_t next = 0;
  };
  std::vector<Frame> stack;
  const auto push = [&](std::uint64_t id) {
    require(nodes.count(id), "producer node is absent");
    require(status[id] != 1, "cycle in source graph");
    status[id] = 1;
    stack.push_back({id, 0});
  };
  push(std::get<WorkflowNodeOutput>(edge).source_node);
  while (!stack.empty()) {
    auto& frame = stack.back();
    const auto& n = *nodes.at(frame.id);
    bool descended = false;
    while (frame.next < n.inputs.size()) {
      const auto& dependency = n.inputs[frame.next++];
      if (const auto* ref = std::get_if<WorkflowNodeOutput>(&dependency)) {
        if (status[ref->source_node] != 2) {
          push(ref->source_node);
          descended = true;
          break;
        }
      }
    }
    if (descended)
      continue;
    std::vector<OperationMetadata> source;
    for (const auto& ref : n.inputs)
      source.push_back(resolve_edge(ref));
    const auto prepared =
        checked(registry.prepare_operation(n.operation, source, n.parameters));
    auto inferred = checked(
        infer_operation_outputs(prepared->traits(), source, n.parameters));
    for (unsigned i = 0; i < inferred.size(); ++i)
      outputs[n.id].emplace(prepared->traits().outputs[i].key,
                            std::move(inferred[i]));
    status[n.id] = 2;
    stack.pop_back();
  }
  return resolve_edge(edge);
}
Parameters parameters(const format::RgbBasisOptions& options) {
  require(options.profile == "strict" ||
              options.profile == "accelerated_apple_silicon" ||
              options.profile == "accelerated_x86_64",
          "unknown CPU profile");
  Parameters out{{"metadata_mode", options.metadata_mode},
                 {"layout", options.layout}};
  if (!options.group.empty())
    out.emplace("group", options.group);
  if (options.components) {
    const auto& c = *options.components;
    out.emplace("components", std::to_string(c[0]) + ',' +
                                  std::to_string(c[1]) + ',' +
                                  std::to_string(c[2]));
  }
  if (options.axis)
    out.emplace("axis", static_cast<std::int64_t>(*options.axis));
  if (options.source_basis)
    out.emplace("source_basis", basis_codec(resolve(*options.source_basis)));
  if (options.target_basis)
    out.emplace("target_basis", basis_codec(resolve(*options.target_basis)));
  if (options.source_white) {
    (void)white_xyz(*options.source_white);
    out.emplace("source_white", white_codec(*options.source_white));
  }
  if (options.target_white) {
    (void)white_xyz(*options.target_white);
    out.emplace("target_white", white_codec(*options.target_white));
  }
  if (options.method)
    out.emplace("method", *options.method);
  if (options.white_handling)
    out.emplace("white_handling", *options.white_handling);
  if (options.metadata_override)
    out.emplace(
        "metadata_override",
        checked(tensor_description_parameter(*options.metadata_override)));
  return out;
}
struct Appended final {
  WorkflowNodeOutput edge;
  OperationMetadata metadata;
};
Appended append(WorkflowDocument* document, WorkflowInput input,
                const OperationMetadata& source, unsigned member,
                const format::RgbBasisOptions& options,
                const OperationRegistry& registry) {
  const std::array<const char*, 3> names{
      {"rgb_to_xyz", "xyz_to_rgb", "adapt_xyz_white"}};
  auto params = parameters(options);
  const auto key =
      std::string("color.") + names[member] + '_' + options.profile;
  auto prepared = checked(registry.prepare_operation(key, {source}, params));
  auto inferred =
      checked(infer_operation_outputs(prepared->traits(), {source}, params));
  const auto id =
      checked(numeric::available_workflow_node_ids(*document, 1, {input}))[0];
  document->nodes.push_back({id, key, {std::move(input)}, std::move(params)});
  return {{id, "values"}, std::move(inferred[0])};
}
Result<WorkflowNodeOutput> author(WorkflowDocument& document,
                                  WorkflowInput input,
                                  const format::RgbBasisOptions& options,
                                  std::shared_ptr<OperationRegistry> registry,
                                  unsigned member) {
  using Answer = Result<WorkflowNodeOutput>;
  try {
    input_internal::Float32Environment environment;
    typed(environment.active(), "floating environment unavailable");
    if (!registry)
      registry = make_default_operation_registry();
    const auto source = edge_metadata(document, input, *registry);
    WorkflowDocument staged = document;
    if (member < 3) {
      const auto out =
          append(&staged, input, source, member, options, *registry);
      document = std::move(staged);
      return Answer(out.edge);
    }
    require(options.target_basis.has_value(), "D requires target_basis");
    require(!options.source_white, "D source white belongs in source_basis");
    const auto target = resolve(*options.target_basis);
    (void)rgb_matrix(target.xy, target.white);
    if (options.target_white)
      require(*options.target_white == target.white,
              "D duplicate target white disagrees");
    const auto policy = options.white_handling.value_or("require_match");
    require(policy == "require_match" || policy == "preserve_xyz" ||
                policy == "adapt",
            "unknown D white policy");
    require((policy == "adapt") == options.method.has_value(),
            "D method is required exactly for adapt");
    auto a = options;
    a.target_basis.reset();
    a.target_white.reset();
    a.white_handling.reset();
    a.method.reset();
    // A caller-owned registry can legally bind this key to another operation.
    // Its opaque state has no internal type guarantee (and can be null).
    // Resolve the helper's source contract using our own preparation instead;
    // only the result of this direct, private call has Preparation provenance.
    const auto builtin_a =
        prepare_impl({source}, parameters(a), 0, SequenceProfile::Strict);
    const auto& prepared_a =
        *static_cast<const Preparation*>(builtin_a.state.get());
    auto first = append(&staged, input, source, 0, a, *registry);
    typed(policy != "require_match" || prepared_a.source_white == target.white,
          "D require_match rejects unequal whites");
    auto current = first;
    if (policy == "adapt") {
      auto c = options;
      c.metadata_mode = options.metadata_mode == "raw" ? "raw" : "respect";
      c.metadata_override.reset();
      c.source_basis.reset();
      c.target_basis.reset();
      c.white_handling.reset();
      c.target_white = target.white;
      if (c.metadata_mode == "raw")
        c.source_white = prepared_a.source_white;
      current =
          append(&staged, current.edge, current.metadata, 2, c, *registry);
    }
    auto b = options;
    b.metadata_mode = options.metadata_mode == "raw" ? "raw" : "respect";
    b.metadata_override.reset();
    b.source_basis.reset();
    b.source_white.reset();
    b.target_white.reset();
    b.method.reset();
    if (b.metadata_mode == "raw")
      b.white_handling.reset();
    else
      b.white_handling =
          policy == "preserve_xyz" ? "preserve_xyz" : "require_match";
    current = append(&staged, current.edge, current.metadata, 1, b, *registry);
    document = std::move(staged);
    return Answer(current.edge);
  } catch (const CheckedFailure& f) {
    return Answer(f.status);
  } catch (const GeometryError&) {
    return Answer(
        invalid("invalid/singular geometry or nonpositive white response"));
  } catch (const ExactWorkFailure& f) {
    return Answer(f.status);
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted,
                         "FMT-10 authoring capacity exhausted",
                         FailureReason::CapacityLimit});
  }
}
}  // namespace
Status register_members(OperationRegistry* registry) {
  for (const auto& p :
       {std::make_pair("strict", SequenceProfile::Strict),
        std::make_pair("accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (unsigned member = 0; member < 3; ++member) {
      auto status =
          registry->register_operation(definition(member, p.first, p.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal::basis_ops

namespace ps::plugin_internal {
Status register_rgb_basis(OperationRegistry* registry) {
  return basis_ops::register_members(registry);
}
}  // namespace ps::plugin_internal

namespace ps::format {
Result<std::string> rgb_basis_parameter(const RgbBasis& basis) {
  using namespace plugin_internal::basis_ops;  // NOLINT(build/namespaces)
  try {
    input_internal::Float32Environment environment;
    typed(environment.active(), "floating environment unavailable");
    const auto resolved = resolve(basis);
    (void)rgb_matrix(resolved.xy, resolved.white);
    return Result<std::string>(basis_codec(resolved));
  } catch (const CheckedFailure& f) {
    return Result<std::string>(f.status);
  } catch (const GeometryError&) {
    return Result<std::string>(invalid("invalid/singular RGB geometry"));
  } catch (const ExactWorkFailure& f) {
    return Result<std::string>(f.status);
  } catch (const std::bad_alloc&) {
    return Result<std::string>(Status{ErrorCode::ResourceExhausted,
                                      "FMT-10 geometry capacity exhausted",
                                      FailureReason::CapacityLimit});
  }
}
Result<std::string> xyz_white_parameter(const std::array<double, 2>& white) {
  using namespace plugin_internal::basis_ops;  // NOLINT(build/namespaces)
  try {
    (void)white_xyz(white);
    return Result<std::string>(white_codec(white));
  } catch (const CheckedFailure& f) {
    return Result<std::string>(f.status);
  } catch (const GeometryError&) {
    return Result<std::string>(invalid("invalid white xy"));
  } catch (const ExactWorkFailure& f) {
    return Result<std::string>(f.status);
  } catch (const std::bad_alloc&) {
    return Result<std::string>(Status{ErrorCode::ResourceExhausted,
                                      "FMT-10 geometry capacity exhausted",
                                      FailureReason::CapacityLimit});
  }
}
Result<WorkflowNodeOutput> rgb_to_xyz(WorkflowDocument& d, WorkflowInput input,
                                      const RgbBasisOptions& o,
                                      std::shared_ptr<OperationRegistry> r) {
  return plugin_internal::basis_ops::author(d, std::move(input), o,
                                            std::move(r), 0);
}
Result<WorkflowNodeOutput> xyz_to_rgb(WorkflowDocument& d, WorkflowInput input,
                                      const RgbBasisOptions& o,
                                      std::shared_ptr<OperationRegistry> r) {
  return plugin_internal::basis_ops::author(d, std::move(input), o,
                                            std::move(r), 1);
}
Result<WorkflowNodeOutput> adapt_xyz_white(
    WorkflowDocument& d, WorkflowInput input, const RgbBasisOptions& o,
    std::shared_ptr<OperationRegistry> r) {
  return plugin_internal::basis_ops::author(d, std::move(input), o,
                                            std::move(r), 2);
}
Result<WorkflowNodeOutput> convert_linear_rgb(
    WorkflowDocument& d, WorkflowInput input, const RgbBasisOptions& o,
    std::shared_ptr<OperationRegistry> r) {
  return plugin_internal::basis_ops::author(d, std::move(input), o,
                                            std::move(r), 3);
}
}  // namespace ps::format
