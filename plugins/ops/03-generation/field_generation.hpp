#pragma once

#include "00-foundation/basic_execution.hpp"

namespace ps::plugin_internal::basic_ops {
template <class T>
void generator(Kind kind, const OperationInvocation& call,
               MutableValue* output) {
  choice(text(call, "dtype"), "float32", "float64");
  const double value = kind == Kind::Constant ? parameter(call, "value") : 0;
  if (kind == Kind::Coordinate) {
    choice(text(call, "axis"), "x", "y");
    choice(text(call, "space"), "pixel", "normalized");
  }
  each(call.output_region, call, [&](auto i, const auto& c) {
    double number = value;
    if (kind == Kind::Coordinate) {
      const auto axis = text(call, "axis") == "x" ? 1 : 0;
      require(c[axis] < (std::uint64_t{1} << 52), ErrorCode::OperationFailed,
              "pixel center cannot be represented");
      number = static_cast<double>(c[axis]) + .5;
      if (text(call, "space") == "normalized")
        number /= integer(call, axis == 1 ? "width" : "height");
    }
    store<T>(output->data(), i, number);
  });
}
}  // namespace ps::plugin_internal::basic_ops
