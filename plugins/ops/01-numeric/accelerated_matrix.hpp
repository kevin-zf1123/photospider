#pragma once

#include <algorithm>
#include <array>
#include <cmath>
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
// Finite binary32 products are exact in binary64. For <=4 additions, let
// u=2^-53, gamma=4u/(1-4u), T=b+sum(p), S=abs(b)+sum(abs(p)). The independent
// rounded sums satisfy abs(sum-T)<=gamma*S and (1-gamma)*S<=magnitude<=
// (1+gamma)*S. radius=8u*magnitude is exact. Endpoint rounding adds at most
// u*(1+gamma)*(1+8u)*S; 8u*(1-gamma) exceeds this plus gamma. Thus even the
// rounded endpoints enclose T, without nextafter or assumptions about BLAS.
// Nonzero sums are multiples of 2^-298; radius/endpoints are multiples of
// 2^-348, and S<2^259, so none of these operations approach binary64 range
// limits. This proof is specific to finite Float32 sources and the scoped
// nearest/gradual environment, not a replacement for general FastInterval.
inline std::optional<std::uint64_t> certify_matrix_float32(
    const MatrixBlock& block, unsigned row, unsigned channel,
    unsigned input_components, unsigned output_components) {
  double sum = block.offsets[channel];
  double magnitude = std::abs(sum);
  for (unsigned j = 0; j < input_components; ++j) {
    const double product = block.x[row * input_components + j] *
                           block.coefficients[channel * input_components + j];
    sum += product;
    magnitude += std::abs(product);
  }
  const double radius = magnitude * 0x1p-50;
  const double low = sum - radius, high = sum + radius;
  // Retain the normal finite real-range admission; endpoints that straddle
  // zero or a rounding boundary cannot agree below and use raw-word ExactDot.
  if (std::min(std::abs(low), std::abs(high)) < 0x1p-126 ||
      std::max(std::abs(low), std::abs(high)) > 0x1.fffffep127)
    return {};
  const auto candidate =
      numeric_bits(block.y[row * output_components + channel], true);
  const auto exponent = candidate & UINT64_C(0x7f800000);
  if (!exponent || exponent == UINT64_C(0x7f800000) ||
      numeric_bits(low, true) != candidate ||
      numeric_bits(high, true) != candidate)
    return {};
  return candidate;
}
}  // namespace ps::plugin_internal::numeric_ops
