#pragma once

#include <algorithm>

#include "00-foundation/basic_execution.hpp"

namespace ps::plugin_internal::basic_ops {
template <class T>
void histogram(Kind kind, const OperationInvocation& call,
               MutableValue* output) {
  const auto& input = call.inputs[0];
  field(input);
  const double a = parameter(call, "range_min"),
               b = parameter(call, "range_max");
  require(a < b, ErrorCode::InvalidArgument, "histogram range must increase");
  const auto bins = kind == Kind::Histogram ? integer(call, "bins") : 2;
  auto edge = [&](std::uint64_t index) {
    return interpolate(static_cast<double>(index), 0, static_cast<double>(bins),
                       a, b);
  };
  if (kind == Kind::Histogram) {
    double previous = a;
    for (std::uint64_t j = 1; j <= bins; ++j) {
      if ((j & 255U) == 0)
        poll(call);
      const double next = edge(j);
      require(next > previous, ErrorCode::OperationFailed,
              "histogram bin edges collapse");
      previous = next;
    }
  }
  std::memset(output->data(), 0, output->size());
  each(input.region(), call, [&](auto, const auto& c) {
    const double value = read<T>(input, c);
    std::uint64_t index;
    if (kind == Kind::Outside) {
      if (value >= a && value <= b)
        return;
      index = value < a ? 0 : 1;
    } else {
      if (value < a || value > b)
        return;
      // Compare actual bin edges instead of rounding a normalized quotient.
      // Upper-bound search puts equality in the bin beginning at that edge.
      std::uint64_t low = 0, high = bins;
      while (low + 1 < high) {
        const auto mid = low + (high - low) / 2;
        if (value < edge(mid))
          high = mid;
        else
          low = mid;
      }
      index = low;
    }
    const auto count = load_count(output->data(), index);
    require(count < INT64_MAX, ErrorCode::OperationFailed,
            "histogram count overflow");
    store_count(output->data(), index, count + 1);
  });
}
}  // namespace ps::plugin_internal::basic_ops
