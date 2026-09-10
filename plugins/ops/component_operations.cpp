#include "plugin/component_operations.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"
#include "numeric_common.hpp"  // NOLINT(build/include_subdir)

namespace ps::plugin_internal {
namespace {
Status stopped() {
  Status status;
  status.code = ErrorCode::Cancelled;
  return status;
}
std::vector<ValueFacet> label_facets() {
  SemanticDescriptor s;
  s.kind = SemanticKind::ScalarField;
  s.channels = {{"component_label", "component_label", "dimensionless"}};
  return {encode_semantic(s).take_value()};
}
template <class T>
T load(const std::uint8_t* bytes, std::uint64_t index) {
  T value;
  std::memcpy(&value, bytes + index * sizeof(T), sizeof(T));
  return value;
}
template <class T>
void store(std::uint8_t* bytes, std::uint64_t index, T value) {
  std::memcpy(bytes + index * sizeof(T), &value, sizeof(T));
}
Result<Value> publish(MutableValue value, const std::vector<ValueFacet>& facets,
                      const CancellationToken& cancellation) {
  if (cancellation.cancelled())
    return Result<Value>(stopped());
  return std::move(value).publish(facets);
}
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
std::uint64_t mix(std::uint64_t value) {
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}
Result<Value> attributes(const OperationInvocation& call, unsigned kind) {
  const auto& input = call.inputs[0];
  const auto count = input.region().element_count().value();
  const auto capacity = std::get<std::int64_t>(call.parameters.at("capacity"));
  std::vector<std::uint64_t> shape =
      kind == 0 ? std::vector<std::uint64_t>{1}
                : std::vector<std::uint64_t>{
                      static_cast<std::uint64_t>(capacity) + 1};
  if (kind == 2)
    shape.push_back(4);
  auto made = MutableValue::allocate({ElementType::Int64, shape},
                                     call.output_region, call.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  // Initialize in bounded chunks so huge static tables remain cancellable.
  const auto bytes = shape[0] * (kind == 2 ? 32 : 8);
  for (std::uint64_t offset = 0; offset < bytes;) {
    if (call.cancellation.cancelled())
      return Result<Value>(stopped());
    const auto chunk = std::min(UINT64_C(8192), bytes - offset);
    std::memset(output.data() + offset, 0, static_cast<std::size_t>(chunk));
    offset += chunk;
  }
  MutableBuffer table;
  std::uint64_t buckets = 0;
  if (kind == 0) {
    // The table is bounded by samples, independent of capacity and sparse IDs.
    if (count > UINT64_MAX / 32)
      return Result<Value>(Status::failure(
          ErrorCode::ResourceExhausted, "component count workspace overflow"));
    buckets = 2;
    while (buckets < count * 2)
      buckets *= 2;
    auto allocated = call.allocator.allocate(buckets * 8);
    if (!allocated.ok())
      return Result<Value>(allocated.status());
    table = allocated.take_value();
    for (std::uint64_t i = 0; i < buckets; ++i) {
      if ((i & 1023U) == 0 && call.cancellation.cancelled())
        return Result<Value>(stopped());
      store<std::int64_t>(table.data(), i, 0);
    }
  }
  std::int64_t distinct = 0;
  auto status = numeric_internal::visit(
      input, call.cancellation, [&](auto i, const auto& coordinate) {
        const auto label =
            numeric_internal::read<std::int64_t>(input, coordinate);
        if (label < 0 || label > capacity)
          return numeric_internal::numeric_failure(
              i, "label outside component capacity");
        if (label == 0)
          return Status::success();
        const auto id = static_cast<std::uint64_t>(label);
        if (kind == 0) {
          auto bucket = mix(id) & (buckets - 1);
          for (std::uint64_t probe = 0;;
               ++probe, bucket = (bucket + 1) & (buckets - 1)) {
            if ((probe & 1023U) == 0 && call.cancellation.cancelled())
              return stopped();
            const auto found = load<std::int64_t>(table.data(), bucket);
            if (found == label)
              break;
            if (!found) {
              store(table.data(), bucket, label);
              ++distinct;
              break;
            }
          }
        } else if (kind == 1) {
          const auto area = load<std::int64_t>(output.data(), id);
          if (area == INT64_MAX)
            return numeric_internal::numeric_failure(i,
                                                     "component area overflow");
          store(output.data(), id, area + 1);
        } else {
          if (coordinate[0] >= static_cast<std::uint64_t>(INT64_MAX) ||
              coordinate[1] >= static_cast<std::uint64_t>(INT64_MAX))
            return numeric_internal::numeric_failure(
                i, "component bbox coordinate overflow");
          const auto x = static_cast<std::int64_t>(coordinate[1]),
                     y = static_cast<std::int64_t>(coordinate[0]);
          auto min_x = load<std::int64_t>(output.data(), id * 4),
               min_y = load<std::int64_t>(output.data(), id * 4 + 1);
          auto max_x = load<std::int64_t>(output.data(), id * 4 + 2),
               max_y = load<std::int64_t>(output.data(), id * 4 + 3);
          if (max_x == 0) {
            min_x = x;
            min_y = y;
          }
          store(output.data(), id * 4, std::min(min_x, x));
          store(output.data(), id * 4 + 1, std::min(min_y, y));
          store(output.data(), id * 4 + 2, std::max(max_x, x + 1));
          store(output.data(), id * 4 + 3, std::max(max_y, y + 1));
        }
        return Status::success();
      });
  if (!status.ok())
    return Result<Value>(status);
  if (kind == 0)
    store(output.data(), 0, distinct);
  return publish(std::move(output), {}, call.cancellation);
}
}  // namespace
Status register_component_operations(OperationRegistry* registry) {
  for (unsigned kind = 0; kind < 5; ++kind) {
    OperationDefinition op;
    const char* keys[] = {"mask.threshold", "mask.components",
                          "component.count", "component.area",
                          "component.bbox"};
    op.key = keys[kind];
    auto& t = op.traits;
    t.input_count = 1;
    t.input_schema.resize(1);
    auto& port = t.input_schema[0];
    port.kind = OperationPortKind::Typed;
    port.rank = 2;
    port.element_type = static_cast<std::uint32_t>(
        kind < 2 ? ElementType::Float32 : ElementType::Int64);
    t.output_element_type =
        kind == 0 ? ElementType::Float32 : ElementType::Int64;
    t.shape_rule = kind < 2 ? OperationShapeRule::PreserveFirstInput
                            : OperationShapeRule::Scalar;
    t.requires_dense_output = true;
    if (kind == 0) {
      port.semantic_kind =
          static_cast<std::uint32_t>(SemanticKind::ScalarField);
      t.output_facets = {encode_semantic(coverage_semantics()).take_value()};
      t.parameter_schema = {{"threshold", OperationParameterType::Float64, true,
                             true, -std::numeric_limits<double>::max(),
                             std::numeric_limits<double>::max()}};
      op.callback = threshold;
    } else {
      port.facets =
          kind == 1
              ? std::vector<ValueFacet>{encode_semantic(coverage_semantics())
                                            .take_value()}
              : label_facets();
      t.parameter_schema = {{"capacity", OperationParameterType::Int64, true,
                             true, 1, 0x1fffffffffffffp0}};
      if (kind == 1) {
        t.output_facets = label_facets();
        t.workspace_input_multiplier = 2;
        op.callback = components;
      } else {
        if (kind == 2)
          t.workspace_input_multiplier = 4;
        if (kind >= 3) {
          t.shape_rule = OperationShapeRule::Axes;
          t.output_axes = {
              {OperationExtentSource::Parameter, 1, "capacity", 0, 0, 1}};
          if (kind == 4)
            t.output_axes.push_back(
                {OperationExtentSource::Constant, 4, {}, 0, 0, 0});
        }
        op.callback = [attribute = kind - 2](const OperationInvocation& call) {
          return attributes(call, attribute);
        };
      }
    }
    if (kind < 2) {
      t.output_semantic_rule = OperationSemanticRule::Establish;
      t.output_schema.kind = OperationPortKind::Typed;
    }
    auto status = registry->register_operation(std::move(op));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
