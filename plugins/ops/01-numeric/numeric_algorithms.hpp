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

namespace ps::plugin_internal::numeric_ops {
using numeric_internal::numeric_failure;
using numeric_internal::read;
using numeric_internal::visit;
inline Status argument(const char* message) {
  return Status::failure(ErrorCode::InvalidArgument, message);
}
inline Result<Value> publish(MutableValue value,
                             const CancellationToken& token) {
  if (token.cancelled()) {
    Status status;
    status.code = ErrorCode::Cancelled;
    return Result<Value>(status);
  }
  return std::move(value).publish();
}
template <class Number>
inline Result<Value> arithmetic(const OperationInvocation& call,
                                unsigned kind) {
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Result<Value>(argument("numeric environment unavailable"));
  double minimum = 0, maximum = 0;
  if (kind == 4) {
    minimum = std::get<double>(call.parameters.at("min"));
    maximum = std::get<double>(call.parameters.at("max"));
    if (minimum > maximum)
      return Result<Value>(argument("clamp min exceeds max"));
  }
  auto made = MutableValue::allocate(call.inputs[0].descriptor(),
                                     call.output_region, call.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  auto status = visit(
      call.inputs[0], call.cancellation,
      [&](auto index, const auto& coordinate) {
        const Number a = read<Number>(call.inputs[0], coordinate);
        const Number b =
            kind == 4 ? Number{} : read<Number>(call.inputs[1], coordinate);
        if (!std::isfinite(a) || !std::isfinite(b))
          return numeric_failure(index, "arithmetic input is nonfinite");
        if (kind == 3 && b == 0)
          return numeric_failure(index, "division by zero");
        Number result;
        if (kind == 4) {
          const double bounded =
              std::clamp(static_cast<double>(a), minimum, maximum);
          if (std::abs(bounded) > std::numeric_limits<Number>::max())
            return numeric_failure(index, "clamp result outside dtype range");
          result = static_cast<Number>(bounded);
        } else {
          result = kind == 0   ? a + b
                   : kind == 1 ? a - b
                   : kind == 2 ? a * b
                               : a / b;
        }
        if (!std::isfinite(result))
          return numeric_failure(index, "arithmetic result is nonfinite");
        std::memcpy(output.data() + index * sizeof(Number), &result,
                    sizeof(Number));
        return Status::success();
      });
  if (!status.ok())
    return Result<Value>(status);
  return publish(std::move(output), call.cancellation);
}

}  // namespace ps::plugin_internal::numeric_ops
