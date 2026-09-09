#include "plugin/operation_semantics.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"
#include "plugin/expression.hpp"

namespace ps::contract_internal {
namespace {
using Facets = Result<std::vector<ValueFacet>>;
Status mismatch(const char* message) {
  return Status::failure(ErrorCode::TypeMismatch, message);
}
Status invalid(const char* message) {
  return Status::failure(ErrorCode::InvalidArgument, message);
}
const ValueFacet* typed_facet(const OperationMetadata& metadata) {
  for (const auto& facet : metadata.facets)
    if (facet.key == "photospider.image" || facet.key == "photospider.semantic")
      return &facet;
  return nullptr;
}
Result<SemanticDescriptor> typed(const OperationMetadata& metadata) {
  if (const auto* facet = typed_facet(metadata))
    return decode_semantic(*facet);
  return Result<SemanticDescriptor>(
      mismatch("channel/color operation requires typed semantics"));
}
Facets encode(const SemanticDescriptor& s) {
  auto facet = encode_semantic(s);
  return facet.ok() ? Facets(std::vector<ValueFacet>{facet.take_value()})
                    : Facets(facet.status());
}
bool channel_array(SemanticKind kind) {
  return kind == SemanticKind::Image || kind == SemanticKind::VectorField ||
         kind == SemanticKind::ComplexField;
}
}  // namespace
Facets infer_transformed_facets(
    const OperationTraits& t, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    const ValueDescriptor& output) {
  const auto rule = t.output_semantic_rule;
  if (rule == OperationSemanticRule::MergeChannelsParameter) {
    const auto found = parameters.find(t.output_semantic_parameter);
    if (found == parameters.end() ||
        !std::holds_alternative<std::string>(found->second))
      return Facets(invalid("missing channel target semantic parameter"));
    auto target = semantic_from_parameter(std::get<std::string>(found->second));
    if (!target.ok())
      return Facets(target.status());
    const auto& s = target.value();
    if (!channel_array(s.kind) || s.channels.size() != inputs.size())
      return Facets(mismatch("target must describe every merged HWC channel"));
    auto valid = validate_semantic_descriptor(s, output);
    if (!valid.ok())
      return Facets(valid);
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      if (inputs[i].descriptor.shape.size() != 2)
        return Facets(mismatch("merge inputs must be HW"));
      if (const auto* facet = typed_facet(inputs[i])) {
        auto source = decode_semantic(*facet);
        if (!source.ok())
          return Facets(source.status());
        const auto& f = source.value();
        if ((f.kind != SemanticKind::ScalarField &&
             f.kind != SemanticKind::Mask) ||
            f.channels.size() != 1 ||
            (f.kind == SemanticKind::Mask &&
             f.channels[0].role != "coverage") ||
            f.channels[0].role != s.channels[i].role ||
            f.channels[0].unit != s.channels[i].unit)
          return Facets(
              mismatch("merged channel role/unit contradicts target"));
      } else if (!inputs[i].facets.empty()) {
        return Facets(mismatch(
            "generic merge inputs must have no opaque interpretation"));
      }
    }
    return encode(s);
  }
  if (t.output_semantic_input >= inputs.size())
    return Facets(mismatch("semantic source input absent"));
  const auto& input = inputs[t.output_semantic_input];
  if (rule == OperationSemanticRule::SampleExpression) {
    input_internal::Float32Environment environment;
    if (!environment.active())
      return Facets(
          Status::failure(ErrorCode::OperationFailed,
                          "expression metadata environment unavailable"));
    if (input.descriptor.element_type != ElementType::Float64 ||
        input.descriptor.shape.size() != 1 || input.descriptor.shape[0] < 1 ||
        input.descriptor.shape[0] > 256 || !input.facets.empty() ||
        output.element_type != ElementType::Float32 ||
        output.shape.size() != 1 || output.shape[0] < 1 ||
        output.shape[0] > 1048576)
      return Facets(
          mismatch("expression coefficient/output descriptor outside limits"));
    const auto expression = parameters.find(t.output_semantic_parameter);
    const auto start = parameters.find("start"), step = parameters.find("step");
    if (expression == parameters.end() ||
        !std::holds_alternative<std::string>(expression->second) ||
        start == parameters.end() ||
        !std::holds_alternative<double>(start->second) ||
        step == parameters.end() ||
        !std::holds_alternative<double>(step->second))
      return Facets(invalid("missing expression/domain parameters"));
    const double origin = std::get<double>(start->second),
                 delta = std::get<double>(step->second);
    const double end =
        std::fma(static_cast<double>(output.shape[0] - 1), delta, origin);
    if (!std::isfinite(origin) || !std::isfinite(delta) || delta <= 0 ||
        !std::isfinite(end) || (output.shape[0] > 1 && end <= origin))
      return Facets(
          invalid("expression sampling endpoint is not representable"));
    auto parsed = expression_internal::parse(
        std::get<std::string>(expression->second), input.descriptor.shape[0]);
    if (!parsed.ok())
      return Facets(parsed.status());
    SemanticDescriptor result;
    result.kind = SemanticKind::SampledSignal;
    result.channels = {{"value", "value", "dimensionless"}};
    result.sample_origin = origin;
    result.sample_step = delta;
    result.sample_axis_unit = "dimensionless";
    return encode(result);
  }
  if (rule == OperationSemanticRule::ApplyLut1d) {
    input_internal::Float32Environment environment;
    if (!environment.active())
      return Facets(Status::failure(ErrorCode::OperationFailed,
                                    "LUT metadata environment unavailable"));
    if (inputs.size() != 2 || t.output_semantic_input ||
        input.descriptor.element_type != ElementType::Float32 ||
        output.element_type != ElementType::Float32 ||
        output.shape != input.descriptor.shape ||
        inputs[1].descriptor.element_type != ElementType::Float32 ||
        inputs[1].descriptor.shape.size() != 1 ||
        inputs[1].descriptor.shape[0] < 2)
      return Facets(mismatch(
          "LUT requires Float32 query and at least two table samples"));
    auto query = typed(input), table = typed(inputs[1]);
    if (!query.ok())
      return Facets(query.status());
    if (!table.ok())
      return Facets(table.status());
    const auto& s = table.value();
    if (query.value().kind != SemanticKind::SampledSignal ||
        (s.kind != SemanticKind::SampledSignal &&
         s.kind != SemanticKind::Lut) ||
        s.channels.size() != 1 || query.value().unit != s.sample_axis_unit)
      return Facets(mismatch("LUT query samples must use the table axis unit"));
    const auto policy = parameters.find(t.output_semantic_parameter);
    if (policy == parameters.end() ||
        !std::holds_alternative<std::string>(policy->second) ||
        (std::get<std::string>(policy->second) != "reject" &&
         std::get<std::string>(policy->second) != "clip"))
      return Facets(invalid("LUT out-of-domain policy must be reject or clip"));
    const double end =
        std::fma(static_cast<double>(inputs[1].descriptor.shape[0] - 1),
                 s.sample_step, s.sample_origin);
    if (!std::isfinite(end) || end <= s.sample_origin)
      return Facets(mismatch("LUT sampling endpoint is not representable"));
    return Facets(std::vector<ValueFacet>{});
  }
  if (rule == OperationSemanticRule::SwizzleChannels) {
    if (input.descriptor.shape.size() != 3)
      return Facets(mismatch("swizzle requires HWC"));
    const auto found = parameters.find(t.output_semantic_parameter);
    if (found == parameters.end() ||
        !std::holds_alternative<std::string>(found->second))
      return Facets(invalid("missing channel selection parameter"));
    auto indices =
        channel_indices_from_parameter(std::get<std::string>(found->second));
    if (!indices.ok())
      return Facets(indices.status());
    for (auto index : indices.value())
      if (index >= input.descriptor.shape[2])
        return Facets(invalid("swizzle channel index outside input"));
    if (!typed_facet(input))
      return Facets(std::vector<ValueFacet>{});
    auto source = typed(input);
    if (!source.ok())
      return Facets(source.status());
    auto s = source.take_value();
    if (!channel_array(s.kind))
      return Facets(mismatch("swizzle semantic kind is not HWC"));
    const auto original = s.channels;
    s.channels.clear();
    for (auto index : indices.value())
      s.channels.push_back(original[index]);
    if (s.kind == SemanticKind::Image && s.channels.size() == 3 &&
        std::none_of(s.channels.begin(), s.channels.end(),
                     [](const auto& c) { return c.role == "coverage"; })) {
      if (s.association == "coverage_premultiplied")
        return Facets(std::vector<ValueFacet>{});
      s.association = "none";
    }
    auto encoded = encode(s);
    // Repeated/incomplete roles cannot claim an image/vector/complex profile.
    if (!encoded.ok() || !validate_semantic_descriptor(s, output).ok())
      return Facets(std::vector<ValueFacet>{});
    return encoded;
  }
  auto source = typed(input);
  if (!source.ok())
    return Facets(source.status());
  auto s = source.take_value();
  if (rule == OperationSemanticRule::ExtractChannel) {
    if (!channel_array(s.kind) || input.descriptor.shape.size() != 3)
      return Facets(mismatch("extract requires typed HWC channels"));
    const auto found = parameters.find(t.output_semantic_parameter);
    if (found == parameters.end() ||
        !std::holds_alternative<std::int64_t>(found->second))
      return Facets(invalid("missing channel extraction index"));
    const auto index = std::get<std::int64_t>(found->second);
    if (index < 0 || static_cast<std::uint64_t>(index) >= s.channels.size())
      return Facets(invalid("extraction index outside input"));
    SemanticDescriptor result;
    result.channels = {s.channels[index]};
    result.unit = result.channels[0].unit;
    if (s.kind == SemanticKind::Image && result.channels[0].role == "coverage")
      return encode(coverage_semantics());
    result.kind = SemanticKind::ScalarField;
    return encode(result);
  }
  if (s.kind != SemanticKind::Image)
    return Facets(mismatch("color/alpha operation requires image semantics"));
  if (rule == OperationSemanticRule::AssociateAlpha ||
      rule == OperationSemanticRule::UnassociateAlpha) {
    const bool associate = rule == OperationSemanticRule::AssociateAlpha;
    if (s.model != "rgb" ||
        s.association != (associate ? "straight" : "coverage_premultiplied"))
      return Facets(
          mismatch("alpha operation requires matching RGB association"));
    s.association = associate ? "coverage_premultiplied" : "straight";
    return encode(s);
  }
  if (s.association == "coverage_premultiplied")
    return Facets(mismatch("color conversion requires explicit unassociation"));
  const char* required = rule == OperationSemanticRule::RgbToXyz   ? "rgb"
                         : rule == OperationSemanticRule::LabToXyz ? "lab"
                                                                   : "xyz";
  if (s.model != required)
    return Facets(mismatch("color model contradicts conversion"));
  if ((rule == OperationSemanticRule::RgbToXyz ||
       rule == OperationSemanticRule::XyzToRgb) &&
      s.white != rgba_semantics().white)
    return Facets(
        mismatch("linear sRGB/XYZ conversion requires canonical D65 white"));
  const bool rgb = rule == OperationSemanticRule::XyzToRgb;
  const bool lab = rule == OperationSemanticRule::XyzToLab;
  if (!rgb && !lab && rule != OperationSemanticRule::RgbToXyz &&
      rule != OperationSemanticRule::LabToXyz)
    return Facets(invalid("unknown color semantic transformation"));
  const SemanticChannel alpha{"A", "coverage", "dimensionless"};
  s.model = rgb ? "rgb" : lab ? "lab" : "xyz";
  s.primaries = rgb ? "srgb" : "";
  s.transfer = lab ? "identity" : "linear";
  s.unit = lab ? "lab" : "relative";
  s.channels =
      rgb   ? std::vector<SemanticChannel>{{"R", "red", "relative"},
                                           {"G", "green", "relative"},
                                           {"B", "blue", "relative"}}
      : lab ? std::vector<SemanticChannel>{{"L", "lightness", "lab_lightness"},
                                           {"a", "a", "lab_opponent"},
                                           {"b", "b", "lab_opponent"}}
            : std::vector<SemanticChannel>{{"X", "x", "relative"},
                                           {"Y", "y", "relative"},
                                           {"Z", "z", "relative"}};
  if (s.association != "none")
    s.channels.push_back(alpha);
  return encode(s);
}
}  // namespace ps::contract_internal
