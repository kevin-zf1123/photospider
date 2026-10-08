#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "00-foundation/numeric_common.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "plugin/expression.hpp"
#include "plugin/operation_semantics.hpp"
#include "plugin/port_validation.hpp"

namespace ps::plugin_internal::expression_result {
using namespace numeric_ops;  // NOLINT(build/namespaces)
template <class Number>
Number read(MathTensorReader& reader, const std::vector<std::uint64_t>& at) {
  auto bits = reader.bits(at);
  Number number;
  std::memcpy(&number, &bits, sizeof(number));
  return number;
}
inline Result<double> interpolate(MathTensorReader& table, std::uint64_t size,
                                  double query,
                                  const SemanticDescriptor& domain,
                                  std::uint64_t sample) {
  using numeric_internal::add_split;
  using numeric_internal::Split;
  using numeric_internal::two_sum;
  const auto failure = [&] {
    return Result<double>(numeric_internal::numeric_failure(
        sample, "LUT interpolation precision cannot be represented"));
  };
  int exponent;
  const double step = std::frexp(domain.sample_step, &exponent);
  auto delta = two_sum(query, -domain.sample_origin);
  delta.high = std::scalbn(delta.high, -exponent);
  delta.low = std::scalbn(delta.low, -exponent);
  const double position = delta.high / step;
  // Indices must be exactly representable before multiplying by the step.
  if (!std::isfinite(position) || position < 0 || position >= 0x1p53)
    return failure();
  auto lower = std::min(static_cast<std::uint64_t>(position), size - 2);
  Split left{}, right{};
  double lost = 0;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    const double product = static_cast<double>(lower) * step;
    left = two_sum(delta.high, -product);
    lost = add_split(&left, delta.low);
    lost +=
        add_split(&left, -std::fma(static_cast<double>(lower), step, -product));
    right = {step, 0};
    lost += add_split(&right, -left.high);
    lost += add_split(&right, -left.low);
    const bool before = left.high < 0 || (left.high == 0 && left.low < 0);
    const bool after = right.high < 0 || (right.high == 0 && right.low < 0);
    if (!before && !after)
      break;
    if (attempt || (before && lower == 0) || (after && lower >= size - 2))
      return failure();
    if (before)
      --lower;
    else
      ++lower;
  }
  const double a = read<float>(table, {lower});
  const double b = read<float>(table, {lower + 1});
  if (a == b)
    return Result<double>(a);
  // Sum the weighted numerator before division, retaining product residuals.
  // This preserves cancellation, including a mathematically exact zero.
  const double ar = a * right.high, bl = b * left.high;
  Split numerator = two_sum(ar, bl);
  double error = lost * (std::abs(a) + std::abs(b));
  error += add_split(&numerator, std::fma(a, right.high, -ar));
  error += add_split(&numerator, std::fma(b, left.high, -bl));
  for (const auto& term :
       {std::pair<double, double>{a, right.low}, {b, left.low}}) {
    const double product = term.first * term.second;
    error += add_split(&numerator, product);
    error += add_split(&numerator, std::fma(term.first, term.second, -product));
  }
  const double high = numerator.high / step;
  const double remainder =
      std::fma(-high, step, numerator.high) + numerator.low;
  const double result = high + remainder / step;
  if (!std::isfinite(result) ||
      std::abs(result) > std::numeric_limits<float>::max())
    return failure();
  const float rounded = static_cast<float>(result);
  const double ulp = std::max(
      static_cast<double>(std::numeric_limits<float>::denorm_min()),
      std::abs(static_cast<double>(rounded) - std::nextafter(rounded, 0.F)));
  if (error / step > ulp / 8)
    return failure();
  return Result<double>(result);
}

struct Prepared final {
  expression_internal::Expression expression;
  SemanticDescriptor domain;
};
struct Kernel final {
  bool lut;
  explicit Kernel(bool lookup) : lut(lookup) {}
  static std::uint32_t support_roles() { return 5; }
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    input_internal::Float32Environment environment;
    if (!environment.active())
      return {ErrorCode::OperationFailed,
              "expression numeric environment unavailable"};
    const auto* prepared =
        static_cast<const Prepared*>(phase.query.prepared->state());
    MathTensorReader first(phase.tensors->at({0, 0}), phase.query.cancellation);
    const auto shape =
        phase.query.output.result_schema->tensors[0].sample_shape();
    const auto count =
        math_take(phase.query.output.result_schema->tensors[0].sample_count());
    MathTensorWriter writer(writers[0]);
    std::vector<std::uint64_t> at(shape.size());
    if (!lut) {
      auto storage = math_take(phase.resources.allocator().allocate(4096));
      const auto size =
          phase.query.inputs[0].result_schema->tensors[0].sample_shape()[0];
      for (std::uint64_t i = 0; i < size; ++i) {
        math_require(phase.consume_work(1));
        const double coefficient = read<double>(first, {i});
        if (!std::isfinite(coefficient))
          return numeric_internal::numeric_failure(
              i, "nonfinite expression coefficient");
        std::memcpy(storage.data() + i * 8, &coefficient, 8);
      }
      const double start = std::get<double>(phase.query.parameters.at("start"));
      const double step = std::get<double>(phase.query.parameters.at("step"));
      for (std::uint64_t i = 0; i < count; ++i) {
        math_require(phase.consume_work(prepared->expression.nodes.size() + 1));
        const double x = std::fma(static_cast<double>(i), step, start);
        auto value = expression_internal::evaluate(
            prepared->expression, x, storage.data(), storage.data() + 2048,
            phase.query.cancellation);
        if (!value.ok()) {
          auto status = value.status();
          if (status.code == ErrorCode::OperationFailed)
            status.message += " at sample " + std::to_string(i);
          return status;
        }
        if (!std::isfinite(value.value()) ||
            std::abs(value.value()) > std::numeric_limits<float>::max())
          return numeric_internal::numeric_failure(
              i, "expression output outside finite Float32");
        const float number = static_cast<float>(value.value());
        std::memcpy(writer.address({i}), &number, 4);
      }
    } else {
      MathTensorReader table(phase.tensors->at({1, 0}),
                             phase.query.cancellation);
      const auto size =
          phase.query.inputs[1].result_schema->tensors[0].sample_shape()[0];
      const auto& domain = prepared->domain;
      const double end = std::fma(static_cast<double>(size - 1),
                                  domain.sample_step, domain.sample_origin);
      const bool clip =
          std::get<std::string>(phase.query.parameters.at("out_of_domain")) ==
          "clip";
      for (std::uint64_t i = 0; i < count; ++i) {
        math_require(phase.consume_work(1));
        const double query = read<float>(first, at);
        if (!std::isfinite(query))
          return numeric_internal::numeric_failure(i, "nonfinite LUT query");
        if (!clip && (query < domain.sample_origin || query > end))
          return numeric_internal::numeric_failure(
              i, "LUT query outside sampling domain");
        const double value =
            query <= domain.sample_origin ? read<float>(table, {0})
            : query >= end
                ? read<float>(table, {size - 1})
                : math_take(interpolate(table, size, query, domain, i));
        if (!std::isfinite(value) ||
            std::abs(value) > std::numeric_limits<float>::max())
          return numeric_internal::numeric_failure(
              i, "LUT result outside finite Float32");
        const float number = static_cast<float>(value);
        std::memcpy(writer.address(at), &number, 4);
        math_next(at, shape);
      }
    }
    return phase.consume_work(0);
  }
};
using Program = WholeTensorProgram<Kernel>;
inline OperationDefinition operation(const char* key, bool lut) {
  OperationDefinition definition;
  definition.key = key;
  auto& traits = definition.traits;
  traits.input_count = lut ? 2 : 1;
  traits.requires_metadata_specialization = true;
  traits.input_schema.resize(traits.input_count);
  for (auto& input : traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.element_type = static_cast<std::uint32_t>(lut ? ElementType::Float32
                                                        : ElementType::Float64);
  }
  set_whole_tensor_output(traits, ElementType::Float32, sizeof(Program));
  traits.outputs[0].key = "value";
  const double maximum = std::numeric_limits<double>::max();
  if (lut) {
    traits.parameter_schema = {
        {"out_of_domain", OperationParameterType::String, true}};
  } else {
    traits.parameter_schema = {
        {"count", OperationParameterType::Int64, true, true, 1, 1048576},
        {"expression", OperationParameterType::String, true},
        {"start", OperationParameterType::Float64, true, true, -maximum,
         maximum},
        {"step", OperationParameterType::Float64, true, true, 0, maximum}};
  }
  definition.prepare_static =
      [lut](const auto& inputs,
            const auto& params) -> Result<OperationPreparation> {
    try {
      input_internal::Float32Environment environment;
      if (!environment.active())
        return Result<OperationPreparation>(
            Status{ErrorCode::OperationFailed,
                   "numeric metadata environment unavailable"});
      std::vector<OperationMetadata> metadata;
      for (const auto& input : inputs) {
        if (!input.result_schema || !input.result_schema->fields.empty() ||
            input.result_schema->tensors.size() != 1)
          return Result<OperationPreparation>(
              Status{ErrorCode::TypeMismatch,
                     "expression requires one tensor and no fields"});
        const auto& member = input.result_schema->tensors[0];
        metadata.push_back({member.descriptor, member.facets});
      }
      if ((!lut && !inputs[0].result_schema->tensors[0].batch_axes.empty()) ||
          (lut && !inputs[1].result_schema->tensors[0].batch_axes.empty()))
        return Result<OperationPreparation>(Status{
            ErrorCode::TypeMismatch, "coefficient/table input is unbatched"});
      auto schema = numeric_tensor_schema(
          ElementType::Float32,
          lut ? metadata[0].descriptor.shape
              : std::vector<std::uint64_t>{static_cast<std::uint64_t>(
                    std::get<std::int64_t>(params.at("count")))});
      OperationTraits transform;
      transform.outputs[0].output_semantic_rule =
          lut ? OperationSemanticRule::ApplyLut1d
              : OperationSemanticRule::SampleExpression;
      transform.outputs[0].output_semantic_parameter =
          lut ? "out_of_domain" : "expression";
      schema.tensors[0].facets =
          math_take(contract_internal::infer_transformed_facets(
              transform, metadata, params, schema.tensors[0].descriptor));
      auto state = std::make_shared<Prepared>();
      if (lut) {
        schema.tensors[0].batch_axes =
            inputs[0].result_schema->tensors[0].batch_axes;
        for (const auto& facet : metadata[1].facets)
          if (facet.key == "photospider.semantic")
            state->domain = math_take(decode_semantic(facet));
      } else {
        state->expression = math_take(expression_internal::parse(
            std::get<std::string>(params.at("expression")),
            metadata[0].descriptor.shape[0]));
      }
      OperationPreparation prepared;
      prepared.state = std::move(state);
      prepared.outputs.resize(1);
      prepared.outputs[0].metadata.result_schema =
          std::make_shared<const SchemaTemplate>(std::move(schema));
      return Result<OperationPreparation>(std::move(prepared));
    } catch (const Status& status) {
      return Result<OperationPreparation>(status);
    }
  };
  definition.start_result = [lut](const ResultProgramQuery&,
                                  const BufferAllocator& allocator) {
    return ResultContinuation::make<Program>(allocator, lut);
  };
  return definition;
}
}  // namespace ps::plugin_internal::expression_result
