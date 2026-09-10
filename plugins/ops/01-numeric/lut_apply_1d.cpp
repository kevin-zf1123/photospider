#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "01-numeric/expression_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace expression_ops;  // NOLINT(build/namespaces)
Result<double> interpolate(const Value& table, double query,
                           const SemanticDescriptor& domain,
                           std::uint64_t sample) {
  using numeric_internal::add_split;
  using numeric_internal::Split;
  using numeric_internal::two_sum;
  const auto failure = [&] {
    return Result<double>(numeric_internal::numeric_failure(
        sample, "LUT interpolation precision cannot be represented"));
  };
  const auto size = table.descriptor().shape[0];
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
  const double a = numeric_internal::read<float>(table, {lower});
  const double b = numeric_internal::read<float>(table, {lower + 1});
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
Result<Value> apply_lut(const OperationInvocation& call,
                        const OperationTraits& traits) {
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Result<Value>(Status::failure(
        ErrorCode::OperationFailed, "LUT numeric environment unavailable"));
  auto inferred = metadata(traits, call);
  if (!inferred.ok())
    return Result<Value>(inferred.status());
  const auto& queries = call.inputs[0];
  const auto& table = call.inputs[1];
  const auto size = table.descriptor().shape[0];
  const auto domain = semantic(table);
  const double end = std::fma(static_cast<double>(size - 1), domain.sample_step,
                              domain.sample_origin);
  const bool clip =
      std::get<std::string>(call.parameters.at("out_of_domain")) == "clip";
  auto made = MutableValue::allocate(inferred.value().descriptor,
                                     call.output_region, call.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  auto status = numeric_internal::visit(
      queries, call.cancellation, [&](auto sample, const auto& coordinate) {
        const double query = numeric_internal::read<float>(queries, coordinate);
        if (!std::isfinite(query))
          return numeric_internal::numeric_failure(sample,
                                                   "nonfinite LUT query");
        if (!clip && (query < domain.sample_origin || query > end))
          return numeric_internal::numeric_failure(
              sample, "LUT query outside sampling domain");
        double result;
        if (query <= domain.sample_origin) {
          result = numeric_internal::read<float>(table, {0});
        } else if (query >= end) {
          result = numeric_internal::read<float>(table, {size - 1});
        } else {
          auto interpolated = interpolate(table, query, domain, sample);
          if (!interpolated.ok())
            return interpolated.status();
          result = interpolated.value();
        }
        if (!std::isfinite(result) ||
            std::abs(result) > std::numeric_limits<float>::max())
          return numeric_internal::numeric_failure(
              sample, "LUT result outside finite Float32");
        const float number = static_cast<float>(result);
        std::memcpy(output.data() + sample * 4, &number, 4);
        return Status::success();
      });
  if (!status.ok())
    return Result<Value>(status);
  if (call.cancellation.cancelled())
    return Result<Value>(stopped());
  return std::move(output).publish();
}
}  // namespace
Status register_lut_apply_1d(OperationRegistry* registry) {
  OperationDefinition lut;
  lut.key = "lut.apply_1d";
  auto& t = lut.traits;
  t.input_count = 2;
  t.input_schema.resize(2);
  for (auto& port : t.input_schema) {
    port.kind = OperationPortKind::Typed;
    port.element_type = static_cast<std::uint32_t>(ElementType::Float32);
  }
  t.input_schema[0].semantic_kind =
      static_cast<std::uint32_t>(SemanticKind::SampledSignal);
  t.input_schema[1].rank = 1;
  t.output_element_type = ElementType::Float32;
  t.shape_rule = OperationShapeRule::PreserveFirstInput;
  t.output_semantic_rule = OperationSemanticRule::ApplyLut1d;
  t.output_semantic_parameter = "out_of_domain";
  t.requires_dense_output = true;
  t.parameter_schema = {
      {"out_of_domain", OperationParameterType::String, true}};
  lut.callback = [traits = t](const OperationInvocation& call) {
    return apply_lut(call, traits);
  };
  return registry->register_operation(std::move(lut));
}
}  // namespace ps::plugin_internal
