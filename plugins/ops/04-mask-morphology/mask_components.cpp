#include <utility>
#include <vector>

#include "04-mask-morphology/component_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace component_ops;  // NOLINT(build/namespaces)
Result<Value> components(const OperationInvocation& call) {
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Result<Value>(
        Status::failure(ErrorCode::OperationFailed,
                        "components numeric environment unavailable"));
  const auto& input = call.inputs[0];
  const auto& shape = input.descriptor().shape;
  const auto width = shape[1], count = input.region().element_count().value();
  auto made = MutableValue::allocate({ElementType::Int64, shape},
                                     call.output_region, call.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  if (call.cancellation.cancelled())
    return Result<Value>(stopped());
  auto allocated = call.allocator.allocate(
      count * 8);  // Dense Int64 output already proves this product.
  if (!allocated.ok())
    return Result<Value>(allocated.status());
  auto queue = allocated.take_value();
  auto status = numeric_internal::visit(
      input, call.cancellation, [&](auto i, const auto& coordinate) {
        const float value = numeric_internal::read<float>(input, coordinate);
        if (value != 0 && value != 1)
          return numeric_internal::numeric_failure(
              i, "components require binary 0/1 coverage");
        store<std::int64_t>(output.data(), i, value == 0 ? 0 : -1);
        return Status::success();
      });
  if (!status.ok())
    return Result<Value>(status);
  const auto capacity = std::get<std::int64_t>(call.parameters.at("capacity"));
  std::int64_t label = 0;
  for (std::uint64_t seed = 0; seed < count; ++seed) {
    if ((seed & 1023U) == 0 && call.cancellation.cancelled())
      return Result<Value>(stopped());
    if (load<std::int64_t>(output.data(), seed) != -1)
      continue;
    if (label == capacity)
      return Result<Value>(numeric_internal::numeric_failure(
          seed, "component capacity exceeded"));
    ++label;
    std::uint64_t head = 0, tail = 1;
    store(output.data(), seed, label);
    store(queue.data(), 0, seed);
    while (head < tail) {
      if ((head & 1023U) == 0 && call.cancellation.cancelled())
        return Result<Value>(stopped());
      const auto pixel = load<std::uint64_t>(queue.data(), head++);
      const auto x = pixel % width;
      auto enqueue = [&](std::uint64_t neighbour) {
        if (load<std::int64_t>(output.data(), neighbour) == -1) {
          store(output.data(), neighbour, label);
          store(queue.data(), tail++, neighbour);
        }
      };
      if (pixel >= width)
        enqueue(pixel - width);
      if (x > 0)
        enqueue(pixel - 1);
      if (x + 1 < width)
        enqueue(pixel + 1);
      if (pixel < count - width)
        enqueue(pixel + width);
    }
  }
  return publish(std::move(output), label_facets(), call.cancellation);
}
}  // namespace
Status register_mask_components(OperationRegistry* registry) {
  OperationDefinition op;
  op.key = "mask.components";
  auto& t = op.traits;
  t.input_count = 1;
  t.input_schema.resize(1);
  auto& port = t.input_schema[0];
  port.kind = OperationPortKind::Typed;
  port.rank = 2;
  port.element_type = static_cast<std::uint32_t>(ElementType::Float32);
  t.outputs[0].output_element_type = ElementType::Int64;
  t.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  t.outputs[0].requires_dense_output = true;

  port.facets =
      true ? std::vector<ValueFacet>{encode_semantic(coverage_semantics())
                                         .take_value()}
           : label_facets();
  t.parameter_schema = {{"capacity", OperationParameterType::Int64, true, true,
                         1, 0x1fffffffffffffp0}};

  t.outputs[0].output_facets = label_facets();
  t.workspace_input_multiplier = 2;
  op.callback = components;

  t.outputs[0].output_semantic_rule = OperationSemanticRule::Establish;
  t.outputs[0].output_schema.kind = OperationPortKind::Typed;

  return registry->register_operation(std::move(op));
}
}  // namespace ps::plugin_internal
