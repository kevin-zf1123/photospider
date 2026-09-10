#include <limits>
#include <utility>

#include "04-mask-morphology/component_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace component_ops;  // NOLINT(build/namespaces)
Result<Value> threshold(const OperationInvocation& call) {
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Result<Value>(
        Status::failure(ErrorCode::OperationFailed,
                        "threshold numeric environment unavailable"));
  auto descriptor = call.inputs[0].descriptor();
  auto made =
      MutableValue::allocate(descriptor, call.output_region, call.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  const double threshold = std::get<double>(call.parameters.at("threshold"));
  auto status = numeric_internal::visit(
      call.inputs[0], call.cancellation, [&](auto i, const auto& coordinate) {
        const double number =
            numeric_internal::read<float>(call.inputs[0], coordinate);
        if (!std::isfinite(number))
          return numeric_internal::numeric_failure(i,
                                                   "nonfinite threshold input");
        store(output.data(), i, number >= threshold ? 1.F : 0.F);
        return Status::success();
      });
  if (!status.ok())
    return Result<Value>(status);
  return publish(std::move(output),
                 {encode_semantic(coverage_semantics()).take_value()},
                 call.cancellation);
}
}  // namespace
Status register_mask_threshold(OperationRegistry* registry) {
  constexpr unsigned kind = 0;
  OperationDefinition op;
  op.key = "mask.threshold";
  auto& t = op.traits;
  t.input_count = 1;
  t.input_schema.resize(1);
  auto& port = t.input_schema[0];
  port.kind = OperationPortKind::Typed;
  port.rank = 2;
  port.element_type = static_cast<std::uint32_t>(kind < 2 ? ElementType::Float32
                                                          : ElementType::Int64);
  t.output_element_type = kind == 0 ? ElementType::Float32 : ElementType::Int64;
  t.shape_rule = kind < 2 ? OperationShapeRule::PreserveFirstInput
                          : OperationShapeRule::Scalar;
  t.requires_dense_output = true;

  port.semantic_kind = static_cast<std::uint32_t>(SemanticKind::ScalarField);
  t.output_facets = {encode_semantic(coverage_semantics()).take_value()};
  t.parameter_schema = {{"threshold", OperationParameterType::Float64, true,
                         true, -std::numeric_limits<double>::max(),
                         std::numeric_limits<double>::max()}};
  op.callback = threshold;

  t.output_semantic_rule = OperationSemanticRule::Establish;
  t.output_schema.kind = OperationPortKind::Typed;

  return registry->register_operation(std::move(op));
}
}  // namespace ps::plugin_internal
