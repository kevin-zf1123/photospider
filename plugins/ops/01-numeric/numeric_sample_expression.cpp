#include <limits>
#include <string>
#include <utility>

#include "01-numeric/expression_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace expression_ops;  // NOLINT(build/namespaces)
Result<Value> sample_expression(const OperationInvocation& call,
                                const OperationTraits& traits) {
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Result<Value>(
        Status::failure(ErrorCode::OperationFailed,
                        "expression numeric environment unavailable"));
  auto inferred = metadata(traits, call);
  if (!inferred.ok())
    return Result<Value>(inferred.status());
  const auto& coefficients = call.inputs[0];
  const auto size = coefficients.descriptor().shape[0];
  auto parsed = expression_internal::parse(
      std::get<std::string>(call.parameters.at("expression")), size);
  if (!parsed.ok())
    return Result<Value>(parsed.status());
  auto made = MutableValue::allocate(inferred.value().descriptor,
                                     call.output_region, call.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  if (call.cancellation.cancelled())
    return Result<Value>(stopped());
  auto workspace = call.allocator.allocate(4096);
  if (!workspace.ok())
    return Result<Value>(workspace.status());
  auto scratch = workspace.take_value();
  for (std::uint64_t i = 0; i < size; ++i) {
    const double coefficient =
        numeric_internal::read<double>(coefficients, {i});
    if (!std::isfinite(coefficient))
      return Result<Value>(numeric_internal::numeric_failure(
          i, "nonfinite expression coefficient"));
    std::memcpy(scratch.data() + i * 8, &coefficient, 8);
  }
  const double start = std::get<double>(call.parameters.at("start"));
  const double step = std::get<double>(call.parameters.at("step"));
  for (std::uint64_t i = 0; i < inferred.value().descriptor.shape[0]; ++i) {
    const double x = std::fma(static_cast<double>(i), step, start);
    auto evaluated =
        expression_internal::evaluate(parsed.value(), x, scratch.data(),
                                      scratch.data() + 2048, call.cancellation);
    if (!evaluated.ok()) {
      auto status = evaluated.status();
      if (status.code == ErrorCode::OperationFailed)
        status.message += " at sample " + std::to_string(i);
      return Result<Value>(status);
    }
    if (!std::isfinite(evaluated.value()) ||
        std::abs(evaluated.value()) > std::numeric_limits<float>::max())
      return Result<Value>(numeric_internal::numeric_failure(
          i, "expression output outside finite Float32"));
    const float number = static_cast<float>(evaluated.value());
    std::memcpy(output.data() + i * 4, &number, 4);
  }
  if (call.cancellation.cancelled())
    return Result<Value>(stopped());
  return std::move(output).publish(inferred.value().facets);
}
}  // namespace
Status register_numeric_sample_expression(OperationRegistry* registry) {
  const double maximum = std::numeric_limits<double>::max();
  OperationDefinition sampler;
  sampler.key = "numeric.sample_expression";
  auto& s = sampler.traits;
  s.input_count = 1;
  s.input_schema.resize(1);
  s.input_schema[0].element_type =
      static_cast<std::uint32_t>(ElementType::Float64);
  s.input_schema[0].rank = 1;
  s.output_element_type = ElementType::Float32;
  s.shape_rule = OperationShapeRule::Axes;
  s.output_axes = {{OperationExtentSource::Parameter, 1, "count", 0, 0, 0}};
  s.output_semantic_rule = OperationSemanticRule::SampleExpression;
  s.output_semantic_parameter = "expression";
  s.output_schema.kind = OperationPortKind::Typed;
  s.output_schema.semantic_kind =
      static_cast<std::uint32_t>(SemanticKind::SampledSignal);
  s.workspace_bytes = 4096;
  s.requires_dense_output = true;
  s.parameter_schema = {
      {"count", OperationParameterType::Int64, true, true, 1, 1048576},
      {"expression", OperationParameterType::String, true},
      {"start", OperationParameterType::Float64, true, true, -maximum, maximum},
      {"step", OperationParameterType::Float64, true, true, 0, maximum}};
  sampler.callback = [traits = s](const OperationInvocation& call) {
    return sample_expression(call, traits);
  };
  return registry->register_operation(std::move(sampler));
}
}  // namespace ps::plugin_internal
