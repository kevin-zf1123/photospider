// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "01-numeric/fast_curve.hpp"
#include "01-numeric/fast_sampling.hpp"

namespace ps::plugin_internal::grade::gradient_channels {
// The published table dtype is a stage boundary, including when the table is
// held as widened binary64 scratch for subsequent vector lookup.
template <class Output, class Poll>
void bake_column(const fast_curve::Curve& curve, double start, double end,
                 std::uint64_t length, double* table, std::size_t stride,
                 Poll poll) {
  constexpr std::size_t batch = 256;
  alignas(64) double grid[batch];
  alignas(64) Output values[batch];
  for (std::uint64_t base = 0; base < length; base += batch) {
    poll();
    const auto count = std::min<std::uint64_t>(batch, length - base);
    if (length == 1)
      grid[0] = start;
    else
      fast_sampling::coordinates(start, end, length, base, grid, count);
    fast_curve::span_multi(&curve, 1, grid, values, count);
    for (std::size_t j = 0; j < count; ++j)
      table[(base + j) * stride] = values[j];
  }
}
}  // namespace ps::plugin_internal::grade::gradient_channels
