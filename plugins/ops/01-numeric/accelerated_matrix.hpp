#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "01-numeric/accelerated_math.hpp"

namespace ps::plugin_internal::numeric_ops {
// Fixed callback-owned storage, admitted as Whole operation workspace. Raw
// words survive conversion for exact replay.
struct MatrixBlock final {
  static constexpr unsigned kRows = 64;
  std::array<std::array<std::uint64_t, 4>, kRows> raw{};
  std::array<std::array<std::uint64_t, 4>, 4> matrix{};
  std::array<std::uint64_t, 4> bias{};
  std::array<double, kRows * 4> x{}, y{};
  std::array<double, 16> coefficients{};
  std::array<double, 4> offsets{};
  std::array<double, kRows * 4> sme_x{};
  std::array<double, 16> sme_coefficients{};
};
// These bounded calls require finite Float32 operands promoted exactly to
// double and a scoped nearest/gradual environment. False selects the fixed
// scalar candidate; neither candidate is publishable before certification.
bool accelerate_matrix_available();
bool accelerate_matrix_candidates(MatrixBlock* block, unsigned rows,
                                  unsigned input_components,
                                  unsigned output_components);
bool sme_matrix_available();
bool sme_matrix_candidates(MatrixBlock* block, unsigned rows,
                           unsigned input_components,
                           unsigned output_components);
inline void scalar_matrix_candidates(MatrixBlock* block, unsigned rows,
                                     unsigned input_components,
                                     unsigned output_components) {
  for (unsigned r = 0; r < rows; ++r)
    for (unsigned o = 0; o < output_components; ++o) {
      double value = 0;
      for (unsigned j = 0; j < input_components; ++j)
        value += block->x[r * input_components + j] *
                 block->coefficients[o * input_components + j];
      block->y[r * output_components + o] = value + block->offsets[o];
    }
}
// Finite binary32 products are exactly representable in binary64. Outward
// additions independently enclose the real dot+bias, without assuming BLAS's
// accumulation order. Unique normal RN32 output gives partition-independent
// bits, including when a different block falls back to ExactDot.
inline std::optional<std::uint64_t> certify_matrix_float32(
    const MatrixBlock& block, unsigned row, unsigned channel,
    unsigned input_components, unsigned output_components) {
  auto enclosure = FastInterval::point(block.offsets[channel]);
  for (unsigned j = 0; j < input_components; ++j) {
    const double product = block.x[row * input_components + j] *
                           block.coefficients[channel * input_components + j];
    enclosure = enclosure + FastInterval::point(product);
  }
  const auto candidate =
      enclosure.accepted(block.y[row * output_components + channel], true);
  if (!candidate || numeric_bits(enclosure.low, true) != *candidate ||
      numeric_bits(enclosure.high, true) != *candidate)
    return {};
  return candidate;
}
}  // namespace ps::plugin_internal::numeric_ops
