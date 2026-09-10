#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "00-foundation/numeric_common.hpp"
#include "data/input_validation.hpp"

namespace ps::plugin_internal::component_ops {
inline Status stopped() {
  Status status;
  status.code = ErrorCode::Cancelled;
  return status;
}
inline std::vector<ValueFacet> label_facets() {
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
inline Result<Value> publish(MutableValue value,
                             const std::vector<ValueFacet>& facets,
                             const CancellationToken& cancellation) {
  if (cancellation.cancelled())
    return Result<Value>(stopped());
  return std::move(value).publish(facets);
}

inline std::uint64_t mix(std::uint64_t value) {
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}
inline Result<Value> attributes(const OperationInvocation& call,
                                unsigned kind) {
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

}  // namespace ps::plugin_internal::component_ops
