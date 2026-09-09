#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"

namespace ps::numeric_internal {
// Error-free addition for finite operands whose sum is representable.
struct Split {
  double high;
  double low;
};
inline Split two_sum(double a, double b) {
  const double high = a + b;
  if (!std::isfinite(high))
    return {high, 0};
  const double b_virtual = high - a;
  return {high, (a - (high - b_virtual)) + (b - b_virtual)};
}
// Accumulate a finite term into two components, returning only the magnitude
// of the third component that cannot be retained.
inline double add_split(Split* accumulator, double term) {
  const auto high = two_sum(accumulator->high, term);
  const auto low = two_sum(accumulator->low, high.low);
  const auto merged = two_sum(high.high, low.high);
  const auto tail = two_sum(merged.low, low.low);
  *accumulator = two_sum(merged.high, tail.high);
  return std::abs(tail.low);
}
/** @brief Visits logical row-major samples, independent of storage layout. */
template <class Function>
Status visit(const Value& value, const CancellationToken& cancellation,
             Function function) {
  auto count = value.region().element_count();
  if (!count.ok())
    return count.status();
  const auto& dimensions = value.region().dimensions();
  std::vector<std::uint64_t> coordinate;
  for (const auto d : dimensions)
    coordinate.push_back(d.offset);
  for (std::uint64_t index = 0; index < count.value(); ++index) {
    if ((index & 1023U) == 0 && cancellation.cancelled()) {
      Status status;
      status.code = ErrorCode::Cancelled;
      return status;
    }
    auto status = function(index, coordinate);
    if (!status.ok())
      return status;
    for (std::size_t axis = dimensions.size(); axis; --axis) {
      if (++coordinate[axis - 1] <
          dimensions[axis - 1].offset + dimensions[axis - 1].extent)
        break;
      coordinate[axis - 1] = dimensions[axis - 1].offset;
    }
  }
  return Status::success();
}
/** @brief Reads one already validated coordinate without alignment assumptions.
 */
template <class Number>
Number read(const Value& value, const std::vector<std::uint64_t>& coordinate) {
  Number number;
  std::memcpy(&number,
              value.bytes().data() + value.byte_address(coordinate).value(),
              sizeof(number));
  return number;
}
inline Status numeric_failure(std::uint64_t sample, const char* reason) {
  return Status::failure(
      ErrorCode::OperationFailed,
      std::string(reason) + " at sample " + std::to_string(sample));
}
}  // namespace ps::numeric_internal
