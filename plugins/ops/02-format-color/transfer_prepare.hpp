#pragma once

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "02-format-color/transfer_program.hpp"
#include "common/tensor_program.hpp"
#include "photospider/ops/tensor_description.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal::transfer_ops {
using Parameters = std::map<std::string, ParameterValue>;
inline Status invalid(const std::string& message) {
  return {ErrorCode::InvalidArgument,
          "FMT-09: " + message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
inline Status mismatch(const std::string& message) {
  return {ErrorCode::TypeMismatch,
          "FMT-09: " + message,
          FailureReason::None,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
inline Status sample_error(
    const std::string& message,
    FailureReason reason = FailureReason::InvalidDomain) {
  return {ErrorCode::OperationFailed,
          "FMT-09: " + message,
          reason,
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
  std::optional<std::uint32_t> axis, sample_axis;
  std::string layout;
  std::shared_ptr<const SchemaTemplate> output_schema;

  std::vector<std::uint64_t> components;
  bool all = false, semantic = true, narrow = true;
  bool selected(std::uint64_t index) const {
    return all ||
           std::binary_search(components.begin(), components.end(), index);
  }
};
inline Result<TransferDefinition> definition(const Parameters& p) {
  using R = Result<TransferDefinition>;
  const auto curve = text(p, "curve");
  TransferDefinition d;
  bool found = false;
  for (const auto& e : {std::pair{"linear", TransferCurve::Linear},
                        {"power_gamma", TransferCurve::PowerGamma},
                        {"srgb", TransferCurve::Srgb},
                        {"bt709", TransferCurve::Bt709},
                        {"bt2020", TransferCurve::Bt2020},
                        {"bt1886", TransferCurve::Bt1886},
                        {"pq", TransferCurve::Pq},
                        {"hlg_oetf", TransferCurve::HlgOetf},
                        {"acescc", TransferCurve::Acescc},
                        {"acescct", TransferCurve::Acescct}}) {
    if (curve == e.first) {
      d.curve = e.second;
      found = true;
      break;
    }
  }
  if (!found) {
    return R(invalid("unknown or absent curve"));
  }
  if (p.count("gamma")) {
    d.gamma = std::get<double>(p.at("gamma"));
  }
  if (p.count("black_luminance")) {
    d.black_luminance = std::get<double>(p.at("black_luminance"));
  }
  if (p.count("white_luminance")) {
    d.white_luminance = std::get<double>(p.at("white_luminance"));
  }
  if (p.count("coefficient_variant")) {
    const auto v = text(p, "coefficient_variant");
    if (v == "smooth") {
      d.coefficient_variant = Bt2020Coefficients::Smooth;
    } else if (v == "rounded_10bit") {
      d.coefficient_variant = Bt2020Coefficients::Rounded10Bit;
    } else if (v == "rounded_12bit") {
      d.coefficient_variant = Bt2020Coefficients::Rounded12Bit;
    } else {
      return R(invalid("unknown coefficient_variant"));
    }
  }
  auto encoded = encode_transfer_definition(d);
  return encoded.ok() ? R(d) : R(encoded.status());
}
inline Rational endpoint(const TensorEndpoint& e) {
  if (const auto* i = std::get_if<std::int64_t>(&e)) {
    return Rational::integer(*i);
  }
  if (const auto* f = std::get_if<double>(&e)) {
    return Rational::binary(numeric_bits(*f), false);
  }
  const auto& q = std::get<TensorRationalEndpoint>(e);
  Rational r;
  r.n.words.assign(q.numerator.begin(), q.numerator.end());
  r.n.trim();
  r.d.words.assign(q.denominator.begin(), q.denominator.end());
  r.d.trim();
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
    if (in.convention != "relative-v1") {
      return R(mismatch("non-native color convention"));
    }
    return R(in);
  }
  if (!in.analytic_binding) {
    return R(mismatch("profile/config requires analytic binding"));
  }
  const auto& b = *in.analytic_binding;
  for (const auto& e : {std::pair{in.model, b.model},
                        {in.primaries, b.primaries},
                        {in.transfer, b.transfer},
                        {in.reference, b.reference}}) {
    if (!e.first.empty() && e.first != e.second) {
      return R(invalid("analytic binding conflicts with source assertion"));
    }
  }
  if ((in.white && in.white != b.white) ||
      (in.primaries_xy && in.primaries_xy != b.primaries_xy)) {
    return R(invalid("analytic binding basis/white conflict"));
  }
  TensorInterpretation result = in;
  result.model = b.model;
  result.primaries = b.primaries;
  result.transfer = b.transfer;
  result.reference = b.reference;
  result.white = b.white;
  result.primaries_xy = b.primaries_xy;
  result.profile.reset();
  result.configured.reset();
  result.analytic_binding.reset();
  result.convention = "relative-v1";
  return R(std::move(result));
}
inline Status native(const TensorInterpretation& i,
                     const TransferDefinition& d) {
  if (i.model != "rgb" && i.model != "gray") {
    return mismatch("group must be RGB or Gray");
  }
  auto r = i.reference;
  const auto scale = i.coordinates ? i.coordinates->scale : std::string{};
  if (r == "scene" || r == "display") {
    if (scale != "relative" && scale != "absolute") {
      return mismatch(
          "native scene/display reference requires explicit coordinate scale");
    }
    if (r == "scene" && scale == "absolute") {
      return mismatch(
          "absolute transfer coordinates require display reference");
    }
    r = scale == "absolute" ? "display_absolute" : r + "_relative";
  }
  if (r == "absolute_display_cd_m2") {
    r = "display_absolute";
  }
  const bool relative = r == "scene_relative" || r == "display_relative";
  if (!scale.empty() &&
      scale != (r == "display_absolute" ? "absolute" : "relative")) {
    return invalid("reference and coordinate scale conflict");
  }
  if (d.curve == TransferCurve::Linear) {
    if (!relative && r != "display_absolute") {
      return mismatch("explicit native reference required");
    }
  } else if (d.curve == TransferCurve::PowerGamma ||
             d.curve == TransferCurve::Srgb) {
    if (!relative) {
      return mismatch("curve requires scene/display-relative input");
    }
  } else if (d.curve == TransferCurve::Pq || d.curve == TransferCurve::Bt1886) {
    if (r != "display_absolute") {
      return mismatch("curve requires absolute display input");
    }
  } else if (r != "scene_relative") {
    return mismatch("curve requires scene-relative input");
  }
  return Status::success();
}
inline Result<OperationPreparation> prepare(
    const std::vector<OperationMetadata>& inputs, const Parameters& p,
    bool encode, SequenceProfile profile) {
  using R = Result<OperationPreparation>;
  plugin_internal::FloatingEnvironment environment;
  if (!environment.active()) {
    return R(sample_error("floating environment unavailable"));
  }
  auto available = numeric_ops::sequence_profile_available(profile);
  if (!available.ok()) {
    return R(available);
  }
  if (inputs.size() != 1) {
    return R(mismatch("requires one input"));
  }
  auto checked_input = tensor_ops::check_tensor(inputs[0]);
  if (!checked_input.ok()) {
    return R(checked_input);
  }
  const auto& input = inputs[0].result_schema->tensors[0];
  const auto full_shape = input.sample_shape();
  auto count = input.sample_count();
  if (!count.ok()) {
    return R(count.status());
  }
  if (!count.value() || count.value() > (UINT64_C(1) << 40)) {
    return R(mismatch("sample count exceeds 2^40"));
  }
  const auto& shape = input.descriptor.shape;
  const auto type = input.descriptor.element_type;
  if ((type != ElementType::Float32 && type != ElementType::Float64) ||
      shape.empty() || full_shape.size() > 8) {
    return R(mismatch("requires Float32/64 and rank 1..8"));
  }
  auto state = std::make_shared<Preparation>();
  state->profile = profile;
  state->narrow = type == ElementType::Float32;
  const auto layout = text(p, "layout", "auto");
  if (layout != "auto" && layout != "view" && layout != "materialize") {
    return R(invalid("layout"));
  }
  state->semantic = p.count("input_description") != 0;
  state->layout = layout;
  std::optional<TensorDescription> description;
  if (state->semantic) {
    auto d = tensor_description_from_parameter(text(p, "input_description"));
    if (!d.ok()) {
      return R(d.status());
    }
    description = d.take_value();
  }
  if (description) {
    auto s = validate_tensor_description(*description, input.descriptor);
    if (!s.ok() && state->semantic) {
      return R(s);
    }
    if (!s.ok()) {
      description.reset();
    }
  }
  TransferDefinition d;
  TensorInterpretation effective;
  if (!state->semantic) {
    if (p.count("group") || !p.count("components")) {
      return R(invalid("raw requires components, not group"));
    }
    if (p.count("axis")) {
      const auto a = std::get<std::int64_t>(p.at("axis"));
      if (a < 0 || static_cast<std::uint64_t>(a) >= shape.size()) {
        return R(invalid("raw axis"));
      }
      state->axis = static_cast<std::uint32_t>(a);
    } else if (description) {
      state->axis = description->channel_axis;
    }
    const auto selector = text(p, "components");
    state->all = selector == "all";
    if (!state->all) {
      if (!state->axis || selector.empty()) {
        return R(invalid("index selection requires axis"));
      }
      // Canonical comma-separated decimal indices: bounded, no signs/spaces,
      // duplicates, leading zeroes or empty entries. Ordering is insignificant.
      std::size_t start = 0;
      do {
        auto end = selector.find(',', start);
        if (end == std::string::npos) {
          end = selector.size();
        }
        if (end == start || (end > start + 1 && selector[start] == '0')) {
          return R(invalid("components syntax"));
        }
        std::uint64_t index = 0;
        for (auto pos = start; pos < end; ++pos) {
          if (selector[pos] < '0' || selector[pos] > '9' ||
              index > (UINT64_MAX - (selector[pos] - '0')) / 10) {
            return R(invalid("components index"));
          }
          index = index * 10 + (selector[pos] - '0');
        }
        if (index >= shape[*state->axis] || state->components.size() == 64) {
          return R(invalid("components range/capacity"));
        }
        state->components.push_back(index);
        if (end == selector.size()) {
          break;
        }
        start = end + 1;
        if (start == selector.size()) {
          return R(invalid("components trailing comma"));
        }
      } while (true);
      std::sort(state->components.begin(), state->components.end());
      if (std::adjacent_find(state->components.begin(),
                             state->components.end()) !=
          state->components.end()) {
        return R(invalid("duplicate components"));
      }
    }
    auto parsed = definition(p);
    if (!parsed.ok()) {
      return R(parsed.status());
    }
    d = parsed.take_value();
  } else {
    auto selected = [&]() -> Result<color_point::Selection> {
      try {
        return Result<color_point::Selection>(
            color_point::select(inputs[0], p));
      } catch (const basis_ops::CheckedFailure& failure) {
        return Result<color_point::Selection>(failure.status);
      }
    }();
    if (!selected.ok()) {
      return R(selected.status());
    }
    effective = selected.value().interpretation;
    if (p.count("curve")) {
      auto parsed = definition(p);
      if (!parsed.ok()) {
        return R(parsed.status());
      }
      d = parsed.take_value();
    } else {
      if (encode || p.count("gamma") || p.count("coefficient_variant") ||
          p.count("black_luminance") || p.count("white_luminance")) {
        return R(invalid(
            "explicit curve required for target or parameter assertion"));
      }
      auto parsed = decode_transfer_definition(effective.transfer);
      if (!parsed.ok()) {
        return R(parsed.status());
      }
      d = parsed.take_value();
    }
    const auto record = encode_transfer_definition(d);
    if (encode && effective.transfer != "linear") {
      return R(mismatch("encoding requires a linear source transfer state"));
    }
    if (!encode && effective.transfer != record.value()) {
      return R(invalid("source transfer does not match complete assertion"));
    }
    auto status = native(effective, d);
    if (!status.ok()) {
      return R(status);
    }
    if (effective.model == "gray") {
      const auto expected = encode || d.curve == TransferCurve::Linear
                                ? "linear_y"
                                : "encoded_luma";
      if (!effective.coordinates ||
          effective.coordinates->gray_kind != expected) {
        return R(
            mismatch("Gray transfer requires explicit linear_y/encoded_luma "
                     "coordinates"));
      }
    }
    state->axis = description->channel_axis;
    state->components = selected.value().channels;
    std::sort(state->components.begin(), state->components.end());
    state->all = !state->axis;
    const bool absolute =
        effective.reference == "display_absolute" ||
        effective.reference == "absolute_display_cd_m2" ||
        (effective.coordinates && effective.coordinates->scale == "absolute");
    for (const auto& unit : selected.value().units) {
      const bool valid_unit =
          absolute
              ? (unit == "cd/m2" || unit == "cd/m^2")
              : (unit == "relative" || unit == "1" ||
                 (effective.model == "gray" && unit == "relative_luminance"));
      if (!valid_unit) {
        return R(
            mismatch("group must explicitly describe native component units"));
      }
    }
  }
  state->program = make_program(d, encode);
  if (layout == "view" && !state->program.identity) {
    return R(invalid("ViewUnavailable: requires static identity"));
  }
  if (state->axis) {
    state->sample_axis =
        static_cast<std::uint32_t>(input.batch_axes.size() + *state->axis);
  }
  OperationOutputSpecialization output;
  auto schema = *inputs[0].result_schema;
  auto& output_tensor = schema.tensors[0];
  output_tensor.atomic_trailing_axes = 0;
  output.metadata.result_schema =
      std::make_shared<const SchemaTemplate>(std::move(schema));
  state->output_schema = output.metadata.result_schema;
  OperationPreparation result;
  result.outputs.push_back(std::move(output));
  result.state = std::move(state);
  return R(std::move(result));
}
}  // namespace ps::plugin_internal::transfer_ops
