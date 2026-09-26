#pragma once

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "02-format-color/transfer_program.hpp"
#include "photospider/data/tensor_description.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal::transfer_ops {
using Parameters = std::map<std::string, ParameterValue>;
inline Status invalid(const std::string& message) {
  return {ErrorCode::InvalidArgument, "FMT-09: " + message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
inline Status mismatch(const std::string& message) {
  return {ErrorCode::TypeMismatch, "FMT-09: " + message,
          FailureReason::None,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
inline Status sample_error(const std::string& message,
                           FailureReason reason = FailureReason::InvalidDomain) {
  return {ErrorCode::OperationFailed, "FMT-09: " + message, reason,
          {FailureOrigin::Domain, FailureScope::Unspecified}};
}
inline std::string text(const Parameters& p, const char* key,
                        const char* fallback = "") {
  const auto found = p.find(key);
  return found == p.end() ? fallback : std::get<std::string>(found->second);
}
struct Preparation final {
  CurveProgram program;
  SequenceProfile profile = SequenceProfile::Strict;
  std::optional<std::uint32_t> axis;
  std::vector<std::uint64_t> components;
  bool all = false, semantic = true, materialize = false, narrow = true;
  bool selected(std::uint64_t index) const {
    return all || std::binary_search(components.begin(), components.end(), index);
  }
};
inline Result<TransferDefinition> definition(const Parameters& p) {
  using R = Result<TransferDefinition>;
  const auto curve = text(p, "curve");
  TransferDefinition d;
  bool found = false;
  for (const auto& e : {std::pair{"linear", TransferCurve::Linear},
       {"power_gamma", TransferCurve::PowerGamma}, {"srgb", TransferCurve::Srgb},
       {"bt709", TransferCurve::Bt709}, {"bt2020", TransferCurve::Bt2020},
       {"bt1886", TransferCurve::Bt1886}, {"pq", TransferCurve::Pq},
       {"hlg_oetf", TransferCurve::HlgOetf}, {"acescc", TransferCurve::Acescc},
       {"acescct", TransferCurve::Acescct}}) {
    if (curve == e.first) { d.curve = e.second; found = true; break; }
  }
  if (!found) return R(invalid("unknown or absent curve"));
  if (p.count("gamma")) d.gamma = std::get<double>(p.at("gamma"));
  if (p.count("black_luminance")) d.black_luminance = std::get<double>(p.at("black_luminance"));
  if (p.count("white_luminance")) d.white_luminance = std::get<double>(p.at("white_luminance"));
  if (p.count("coefficient_variant")) {
    const auto v = text(p, "coefficient_variant");
    if (v == "smooth") d.coefficient_variant = Bt2020Coefficients::Smooth;
    else if (v == "rounded_10bit") d.coefficient_variant = Bt2020Coefficients::Rounded10Bit;
    else if (v == "rounded_12bit") d.coefficient_variant = Bt2020Coefficients::Rounded12Bit;
    else return R(invalid("unknown coefficient_variant"));
  }
  auto encoded = encode_transfer_definition(d);
  return encoded.ok() ? R(d) : R(encoded.status());
}
inline Rational endpoint(const TensorEndpoint& e) {
  if (const auto* i = std::get_if<std::int64_t>(&e)) return Rational::integer(*i);
  if (const auto* f = std::get_if<double>(&e)) return Rational::binary(numeric_bits(*f), false);
  const auto& q = std::get<TensorRationalEndpoint>(e);
  Rational r;
  r.n.words.assign(q.numerator.begin(), q.numerator.end()); r.n.trim();
  r.d.words.assign(q.denominator.begin(), q.denominator.end()); r.d.trim();
  r.negative = q.negative;
  return r;
}
inline bool identity_encoding(const std::optional<TensorEncoding>& e) {
  return !e || (endpoint(e->stored[0]).compare(endpoint(e->decoded[0])) == 0 &&
                endpoint(e->stored[1]).compare(endpoint(e->decoded[1])) == 0);
}
// A profile/config label alone is not an analytic transfer assertion. Only an
// explicit complete binding may resolve it; no profile is opened or guessed.
inline Result<TensorInterpretation> analytic(const TensorInterpretation& in) {
  using R = Result<TensorInterpretation>;
  if (!in.profile && !in.configured) {
    if (in.convention != "relative-v1") return R(mismatch("non-native color convention"));
    return R(in);
  }
  if (!in.analytic_binding) return R(mismatch("profile/config requires analytic binding"));
  const auto& b = *in.analytic_binding;
  for (const auto& e : {std::pair{in.model, b.model}, {in.primaries, b.primaries},
                       {in.transfer, b.transfer}, {in.reference, b.reference}})
    if (!e.first.empty() && e.first != e.second)
      return R(invalid("analytic binding conflicts with source assertion"));
  if ((in.white && in.white != b.white) ||
      (in.primaries_xy && in.primaries_xy != b.primaries_xy))
    return R(invalid("analytic binding basis/white conflict"));
  TensorInterpretation result = in;
  result.model = b.model; result.primaries = b.primaries;
  result.transfer = b.transfer; result.reference = b.reference;
  result.white = b.white; result.primaries_xy = b.primaries_xy;
  result.profile.reset(); result.configured.reset(); result.analytic_binding.reset();
  result.convention = "relative-v1";
  return R(std::move(result));
}
inline Status native(const TensorInterpretation& i, const TransferDefinition& d) {
  if (i.model != "rgb" && i.model != "gray") return mismatch("group must be RGB or Gray");
  const auto& r = i.reference;
  const bool relative = r == "scene_relative" || r == "display_relative";
  if (d.curve == TransferCurve::Linear) {
    if (!relative && r != "display_absolute") return mismatch("explicit native reference required");
  } else if (d.curve == TransferCurve::PowerGamma || d.curve == TransferCurve::Srgb) {
    if (!relative) return mismatch("curve requires scene/display-relative input");
  } else if (d.curve == TransferCurve::Pq || d.curve == TransferCurve::Bt1886) {
    if (r != "display_absolute") return mismatch("curve requires absolute display input");
  } else if (r != "scene_relative") return mismatch("curve requires scene-relative input");
  return Status::success();
}
inline Result<OperationPreparation> prepare(const std::vector<OperationMetadata>& inputs,
    const Parameters& p, bool encode, SequenceProfile profile) {
  using R = Result<OperationPreparation>;
  input_internal::Float32Environment environment;
  if (!environment.active()) return R(sample_error("floating environment unavailable"));
  auto available = numeric_ops::sequence_profile_available(profile);
  if (!available.ok()) return R(available);
  const auto& input = inputs[0];
  const auto& shape = input.descriptor.shape;
  const auto type = input.descriptor.element_type;
  if ((type != ElementType::Float32 && type != ElementType::Float64) ||
      shape.empty() || shape.size() > 8) return R(mismatch("requires Float32/64 and rank 1..8"));
  auto state = std::make_shared<Preparation>();
  state->profile = profile; state->narrow = type == ElementType::Float32;
  const auto mode = text(p, "metadata_mode", "respect"), layout = text(p, "layout", "auto");
  if (mode != "respect" && mode != "override" && mode != "raw") return R(invalid("metadata_mode"));
  if (layout != "auto" && layout != "view" && layout != "materialize") return R(invalid("layout"));
  if ((mode == "override") != (p.count("metadata_override") != 0))
    return R(invalid("metadata_override must occur exactly in override mode"));
  state->semantic = mode != "raw"; state->materialize = layout == "materialize";
  std::optional<TensorDescription> description;
  if (mode == "override") {
    auto d = tensor_description_from_parameter(text(p, "metadata_override"));
    if (!d.ok()) return R(d.status());
    description = d.take_value();
  } else {
    for (const auto& f : input.facets) if (f.key == "photospider.tensor-description") {
      auto d = decode_tensor_description(f);
      if (!d.ok() && state->semantic) return R(d.status());
      if (d.ok()) description = d.take_value();
    }
  }
  if (description) {
    auto s = validate_tensor_description(*description, input.descriptor);
    if (!s.ok() && state->semantic) return R(s);
    if (!s.ok()) description.reset();
  }
  TransferDefinition d;
  std::size_t group_index = 0;
  TensorInterpretation effective;
  if (!state->semantic) {
    if (p.count("group") || !p.count("components")) return R(invalid("raw requires components, not group"));
    if (p.count("axis")) {
      const auto a = std::get<std::int64_t>(p.at("axis"));
      if (a < 0 || static_cast<std::uint64_t>(a) >= shape.size()) return R(invalid("raw axis"));
      state->axis = static_cast<std::uint32_t>(a);
    } else if (description) state->axis = description->channel_axis;
    const auto selector = text(p, "components");
    state->all = selector == "all";
    if (!state->all) {
      if (!state->axis || selector.empty()) return R(invalid("index selection requires axis"));
      // Canonical comma-separated decimal indices: bounded, no signs/spaces,
      // duplicates, leading zeroes or empty entries. Ordering is insignificant.
      std::size_t start = 0;
      do {
        auto end = selector.find(',', start);
        if (end == std::string::npos) end = selector.size();
        if (end == start || (end > start + 1 && selector[start] == '0')) return R(invalid("components syntax"));
        std::uint64_t index = 0;
        for (auto pos = start; pos < end; ++pos) {
          if (selector[pos] < '0' || selector[pos] > '9' ||
              index > (UINT64_MAX - (selector[pos] - '0')) / 10)
            return R(invalid("components index"));
          index = index * 10 + (selector[pos] - '0');
        }
        if (index >= shape[*state->axis] || state->components.size() == 64) return R(invalid("components range/capacity"));
        state->components.push_back(index);
        if (end == selector.size()) break;
        start = end + 1;
        if (start == selector.size()) return R(invalid("components trailing comma"));
      } while (true);
      std::sort(state->components.begin(), state->components.end());
      if (std::adjacent_find(state->components.begin(), state->components.end()) != state->components.end())
        return R(invalid("duplicate components"));
    }
    auto parsed = definition(p); if (!parsed.ok()) return R(parsed.status()); d = parsed.take_value();
  } else {
    if (!description || !p.count("group") || p.count("components") || p.count("axis"))
      return R(invalid("semantic mode requires complete metadata and group, not raw selectors"));
    auto& desc = *description;
    bool found = false;
    for (std::size_t i = 0; i < desc.groups.size(); ++i) if (desc.groups[i].name == text(p, "group")) {
      if (found) return R(invalid("ambiguous group"));
      group_index = i; found = true;
    }
    if (!found) return R(invalid("group not found"));
    const auto& g = desc.groups[group_index];
    auto resolved = analytic(g.interpretation); if (!resolved.ok()) return R(resolved.status());
    effective = resolved.take_value();
    if (p.count("curve")) {
      auto parsed = definition(p); if (!parsed.ok()) return R(parsed.status()); d = parsed.take_value();
    } else {
      if (encode || p.count("gamma") || p.count("coefficient_variant") || p.count("black_luminance") || p.count("white_luminance"))
        return R(invalid("explicit curve required for target or parameter assertion"));
      auto parsed = decode_transfer_definition(effective.transfer);
      if (!parsed.ok()) return R(parsed.status()); d = parsed.take_value();
    }
    const auto record = encode_transfer_definition(d);
    if (encode && effective.transfer != "linear")
      return R(mismatch("encoding requires a linear source transfer state"));
    if (!encode && effective.transfer != record.value())
      return R(invalid("source transfer does not match complete assertion"));
    auto status = native(effective, d); if (!status.ok()) return R(status);
    if ((!desc.transfer.empty() && desc.transfer != effective.transfer) ||
        (!desc.reference.empty() && desc.reference != effective.reference))
      return R(invalid("tensor/source transfer or reference assertion conflict"));
    state->axis = desc.channel_axis;
    state->components = g.indices; std::sort(state->components.begin(), state->components.end());
    state->all = !state->axis;
    for (std::size_t j = 0; j < g.indices.size(); ++j) {
      const auto index = g.indices[j];
      auto encoding = g.components[j].encoding;
      const TensorChannelDescription* channel = index < desc.channels.size() ? &desc.channels[index]
          : !state->axis && desc.component ? &*desc.component : nullptr;
      if (!encoding && channel) encoding = channel->encoding;
      if (!encoding) encoding = desc.encoding;
      if (!identity_encoding(encoding)) return R(mismatch("non-native encoding; use FMT-06 first"));
      std::string unit = g.components[j].unit;
      if (unit.empty() && channel) unit = channel->unit;
      if (unit.empty() && g.interpretation.analytic_binding) unit = g.interpretation.analytic_binding->units[j];
      const char* expected = effective.reference == "display_absolute" ? "cd/m2"
          : effective.model == "gray" ? "relative_luminance" : "relative";
      if (unit != expected) return R(mismatch("group must explicitly describe native component units"));
    }
  }
  state->program = make_program(d, encode);
  if (layout == "view" && (!state->program.identity || (input.planar_layout && state->semantic)))
    return R(invalid("ViewUnavailable: requires static identity and legal generic Value owner"));
  OperationOutputSpecialization output;
  output.metadata = input;
  if (state->semantic) {
    auto& desc = *description;
    const auto target = encode ? encode_transfer_definition(d).value() : std::string("linear");
    // Partly intersected overlapping group claims cannot survive as complete
    // descriptions. Fully transformed groups retain basis/white and roles.
    std::vector<TensorColorGroup> groups;
    for (std::size_t i = 0; i < desc.groups.size(); ++i) {
      auto g = desc.groups[i]; unsigned touched = 0;
      for (auto index : g.indices) touched += state->selected(index);
      if (!touched) { groups.push_back(std::move(g)); continue; }
      if (touched != g.indices.size()) continue;
      auto a = analytic(g.interpretation); if (!a.ok()) return R(a.status());
      g.interpretation = a.take_value(); g.interpretation.transfer = target;
      groups.push_back(std::move(g));
    }
    desc.groups = std::move(groups);
    for (std::size_t i = 0; i < desc.channels.size(); ++i) if (state->selected(i) && desc.channels[i].interpretation) {
      auto a = analytic(*desc.channels[i].interpretation); if (!a.ok()) return R(a.status());
      desc.channels[i].interpretation = a.take_value(); desc.channels[i].interpretation->transfer = target;
    }
    if (!state->axis && desc.component && desc.component->interpretation) {
      desc.component->interpretation = effective; desc.component->interpretation->transfer = target;
    }
    // Legacy tensor-wide transfer/profile labels cannot describe mixed groups.
    if (!desc.transfer.empty()) desc.transfer = desc.groups.size() == 1 ? target : "";
    if (desc.profile || desc.configured) {
      desc.profile.reset(); desc.configured.reset(); desc.analytic_binding.reset(); desc.convention = "relative-v1";
    }
    auto checked = validate_tensor_description(desc, input.descriptor); if (!checked.ok()) return R(checked);
    auto facet = encode_tensor_description(desc); if (!facet.ok()) return R(facet.status());
    output.metadata.facets.clear();
    for (const auto& f : input.facets) if (f.key != "photospider.tensor-description") output.metadata.facets.push_back(f);
    output.metadata.facets.push_back(facet.take_value());
  } // raw deliberately retains bytes, never asserts a new transfer or validity.
  if (input.planar_layout) output.metadata.planar_layout->row_pitch_bytes = 0;
  auto all = Footprint::all(shape); if (!all.ok()) return R(all.status());
  std::vector<DependencyMapPiece> pieces;
  auto piece = [&](const Region& region, bool selected) -> Status {
    auto footprint = Footprint::from_regions(shape, {region});
    if (!footprint.ok()) return footprint.status();
    DependencyMappedNeed need; need.port = 0;
    need.roles = static_cast<std::uint32_t>(DependencyRole::Data);
    if (state->semantic && selected) need.roles |= static_cast<std::uint32_t>(DependencyRole::Validation);
    for (std::size_t a = 0; a < shape.size(); ++a) { DependencyAxis axis; axis.observation_axis = static_cast<std::int32_t>(a); need.axes.push_back(axis); }
    pieces.push_back({footprint.take_value(), {std::move(need)}});
    return Status::success();
  };
  if (!state->axis || state->all || !state->semantic) {
    auto s = piece(Region::whole(shape), true); if (!s.ok()) return R(s);
  } else {
    // At most 129 runs, independent of image dimensions/channel-axis position.
    std::vector<std::uint64_t> boundaries{0, shape[*state->axis]};
    for (auto index : state->components) { boundaries.push_back(index); boundaries.push_back(index + 1); }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    for (std::size_t i = 1; i < boundaries.size(); ++i) {
      auto dimensions = Region::whole(shape).dimensions();
      dimensions[*state->axis] = {boundaries[i - 1], boundaries[i] - boundaries[i - 1]};
      auto s = piece(Region(std::move(dimensions)), state->selected(boundaries[i - 1])); if (!s.ok()) return R(s);
    }
  }
  output.static_dependency_pieces = std::move(pieces);
  output.regional_atomic = true;
  output.preserve_output_views = state->program.identity && !state->materialize && !input.planar_layout;
  if (output.preserve_output_views) output.maximum_output_payload_bytes = 0;
  if (!state->semantic && state->program.identity && input.planar_layout) {
    output.data_movement = DataMovementKind::BitwiseMapped;
    output.data_movement_view_policy = layout == "view" ? DataMovementViewPolicy::RequireView
        : state->materialize ? DataMovementViewPolicy::Materialize : DataMovementViewPolicy::Auto;
    output.preserve_output_views = !state->materialize;
    if (output.preserve_output_views) output.maximum_output_payload_bytes = 0;
  }
  OperationPreparation result; result.outputs.push_back(std::move(output)); result.state = std::move(state);
  return R(std::move(result));
}
}  // namespace ps::plugin_internal::transfer_ops
