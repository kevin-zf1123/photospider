#pragma once

#include "00-foundation/basic_execution.hpp"

namespace ps::plugin_internal::basic_ops {
template <class T>
void smooth(Kind kind, const OperationInvocation& call, MutableValue* output) {
  const auto& input = call.inputs[0];
  field(input);
  const auto radius = static_cast<std::int64_t>(integer(call, "radius"));
  const double sigma = kind == Kind::Gaussian ? parameter(call, "sigma") : 1;
  if (kind == Kind::Gaussian && sigma == 0) {
    each(call.output_region, call, [&](auto i, const auto& c) {
      store<T>(output->data(), i, read<T>(input, c));
    });
    return;
  }
  // Fixed maximum weight storage plus Float64 horizontal intermediates.
  const auto& in_dims = call.input_demands[0].dimensions();
  const auto& out_dims = call.output_region.dimensions();
  const auto rows = in_dims[0].extent, cols = out_dims[1].extent;
  require(
      rows <= (std::numeric_limits<std::uint64_t>::max() - 129 * 8) / 8 / cols,
      ErrorCode::ResourceExhausted, "smoothing scratch overflow");
  auto scratch = take(call.allocator.allocate(129 * 8 + rows * cols * 8));
  auto get = [&](std::uint64_t j) {
    double value;
    std::memcpy(&value, scratch.data() + j * 8, 8);
    return value;
  };
  auto put = [&](std::uint64_t j, double value) {
    finite(value);
    std::memcpy(scratch.data() + j * 8, &value, 8);
  };
  double sum = 0;
  for (std::int64_t d = -radius; d <= radius; ++d) {
    // For tiny sigma, distance/sigma may overflow; its weight is exactly zero.
    const double scaled = static_cast<double>(d) / sigma;
    const double weight =
        kind == Kind::Box ? 1 : std::exp(-.5 * scaled * scaled);
    put(d + radius, weight);
    sum += weight;
  }
  for (std::int64_t d = 0; d <= 2 * radius; ++d)
    put(d, get(d) / sum);
  const auto& shape = input.descriptor().shape;
  for (std::uint64_t y = 0; y < rows; ++y) {
    poll(call);
    for (std::uint64_t x = 0; x < cols; ++x) {
      if ((x & 255U) == 0)
        poll(call);
      double total = 0;
      for (std::int64_t d = -radius; d <= radius; ++d) {
        std::uint64_t sx;
        shifted(out_dims[1].offset + x, d, shape[1], true, &sx);
        const double value = read<T>(input, {in_dims[0].offset + y, sx});
        total = finite(total + value * get(d + radius));
      }
      put(129 + y * cols + x, total);
    }
  }
  each(call.output_region, call, [&](auto i, const auto& c) {
    double total = 0;
    for (std::int64_t d = -radius; d <= radius; ++d) {
      std::uint64_t sy;
      shifted(c[0], d, shape[0], true, &sy);
      total = finite(total + get(129 + (sy - in_dims[0].offset) * cols + c[1] -
                                 out_dims[1].offset) *
                                 get(d + radius));
    }
    // Nonnegative normalized kernels must not manufacture invalid coverage.
    if (!input.facets().empty() &&
        input.facets()[0].payload ==
            take(encode_semantic(coverage_semantics())).payload)
      total = std::clamp(total, 0.0, 1.0);
    store<T>(output->data(), i, total);
  });
}
}  // namespace ps::plugin_internal::basic_ops
