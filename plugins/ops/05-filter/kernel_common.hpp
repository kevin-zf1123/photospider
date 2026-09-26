#pragma once

#include <algorithm>

#include "00-foundation/basic_execution.hpp"

namespace ps::plugin_internal::basic_ops {
template <class T>
void kernel_filter(Kind kind, const OperationInvocation& call,
                   MutableValue* output) {
  const auto& input = call.inputs[0];
  field(input);
  const bool convolution = kind == Kind::Convolve;
  const auto& kernel = call.inputs[1];
  generic(kernel);
  same_type(input, kernel);
  const auto kh = kernel.descriptor().shape[0];
  const auto kw = kernel.descriptor().shape[1];
  const auto ay = integer(call, "anchor_y");
  const auto ax = integer(call, "anchor_x");
  require(ay < kh && ax < kw && kh <= INT64_MAX && kw <= INT64_MAX,
          ErrorCode::InvalidArgument, "anchor outside representable kernel");
  choice(text(call, "boundary"), "zero", "clamp");
  const bool clamp = text(call, "boundary") == "clamp";
  each(kernel.region(), call, [&](auto, const auto& c) { read<T>(kernel, c); });
  const auto& shape = input.descriptor().shape;
  each(call.output_region, call, [&](auto i, const auto& c) {
    double result = 0;
    for (std::uint64_t y = 0; y < kh; ++y) {
      poll(call);
      for (std::uint64_t x = 0; x < kw; ++x) {
        if ((x & 255U) == 0)
          poll(call);
        std::int64_t dy =
            static_cast<std::int64_t>(y) - static_cast<std::int64_t>(ay);
        std::int64_t dx =
            static_cast<std::int64_t>(x) - static_cast<std::int64_t>(ax);
        if (convolution) {
          dy = -dy;
          dx = -dx;
        }
        std::uint64_t sy, sx;
        const bool inside = shifted(c[0], dy, shape[0], clamp, &sy) &&
                            shifted(c[1], dx, shape[1], clamp, &sx);
        const double value = inside ? read<T>(input, {sy, sx}) : 0;
        result =
            finite(result + finite(value * read<T>(call.inputs[1], {y, x})));
      }
    }
    store<T>(output->data(), i, result);
  });
}
}  // namespace ps::plugin_internal::basic_ops
