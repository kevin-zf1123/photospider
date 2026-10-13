#include "01-numeric/expression_metadata.hpp"

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "01-numeric/expression.hpp"
#include "common/floating_environment.hpp"
#include "photospider/data/semantic.hpp"

namespace ps::plugin_internal::expression_result {
namespace {
Status invalid_argument(const char* message) {
  return Status::failure(ErrorCode::InvalidArgument, message);
}
using Facets = Result<std::vector<ValueFacet>>;
Status mismatch(const char* message) {
  return Status::failure(ErrorCode::TypeMismatch, message);
}

const ValueFacet* typed_facet(const TensorMetadata& metadata) {
  for (const auto& facet : metadata.facets)
    if (facet.key == "photospider.image" || facet.key == "photospider.semantic")
      return &facet;
  return nullptr;
}
Result<SemanticDescriptor> typed(const TensorMetadata& metadata) {
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
}  // namespace
Result<std::vector<ValueFacet>> infer_expression_facets(
    bool lut, const std::vector<TensorMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    const ValueDescriptor& output) {
  if (inputs.empty())
    return Facets(mismatch("semantic source input absent"));
  const auto& input = inputs[0];
  if (!lut) {
    FloatingEnvironment environment;
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
    const auto expression = parameters.find("expression");
    const auto start = parameters.find("start"), step = parameters.find("step");
    if (expression == parameters.end() ||
        !std::holds_alternative<std::string>(expression->second) ||
        start == parameters.end() ||
        !std::holds_alternative<double>(start->second) ||
        step == parameters.end() ||
        !std::holds_alternative<double>(step->second))
      return Facets(invalid_argument("missing expression/domain parameters"));
    const double origin = std::get<double>(start->second),
                 delta = std::get<double>(step->second);
    const double end =
        std::fma(static_cast<double>(output.shape[0] - 1), delta, origin);
    if (!std::isfinite(origin) || !std::isfinite(delta) || delta <= 0 ||
        !std::isfinite(end) || (output.shape[0] > 1 && end <= origin))
      return Facets(invalid_argument(
          "expression sampling endpoint is not representable"));
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
  FloatingEnvironment environment;
  if (!environment.active())
    return Facets(Status::failure(ErrorCode::OperationFailed,
                                  "LUT metadata environment unavailable"));
  if (inputs.size() != 2 ||
      input.descriptor.element_type != ElementType::Float32 ||
      output.element_type != ElementType::Float32 ||
      output.shape != input.descriptor.shape ||
      inputs[1].descriptor.element_type != ElementType::Float32 ||
      inputs[1].descriptor.shape.size() != 1 ||
      inputs[1].descriptor.shape[0] < 2)
    return Facets(
        mismatch("LUT requires Float32 query and at least two table samples"));
  auto query = typed(input), table = typed(inputs[1]);
  if (!query.ok())
    return Facets(query.status());
  if (!table.ok())
    return Facets(table.status());
  const auto& s = table.value();
  if (query.value().kind != SemanticKind::SampledSignal ||
      (s.kind != SemanticKind::SampledSignal && s.kind != SemanticKind::Lut) ||
      s.channels.size() != 1 || query.value().unit != s.sample_axis_unit)
    return Facets(mismatch("LUT query samples must use the table axis unit"));
  const auto policy = parameters.find("out_of_domain");
  if (policy == parameters.end() ||
      !std::holds_alternative<std::string>(policy->second) ||
      (std::get<std::string>(policy->second) != "reject" &&
       std::get<std::string>(policy->second) != "clip"))
    return Facets(
        invalid_argument("LUT out-of-domain policy must be reject or clip"));
  const double end =
      std::fma(static_cast<double>(inputs[1].descriptor.shape[0] - 1),
               s.sample_step, s.sample_origin);
  if (!std::isfinite(end) || end <= s.sample_origin)
    return Facets(mismatch("LUT sampling endpoint is not representable"));
  return Facets(std::vector<ValueFacet>{});
}
}  // namespace ps::plugin_internal::expression_result
