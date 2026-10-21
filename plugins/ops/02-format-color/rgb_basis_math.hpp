#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

#include "01-numeric/exact_product.hpp"
#include "01-numeric/sequence_profiles.hpp"
#include "common/domain/rgb_geometry.hpp"

namespace ps::plugin_internal::basis_ops {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
// Static numerators/denominator are bounded by Natural's 8192 bits. A finite
// binary64 sample in units 2^-1074 has <2098 bits, so the absolute sum of three
// products has <10292 bits. Division alignment and rounding fit in 10496 bits.
using RowInteger = numeric_ops::FixedInteger<164>;
struct RowWorkspace final {
  numeric_ops::ExactRatioWorkspace<164> ratio{SequenceProfile::Strict};
  RowInteger input;
};
struct ExactRow final {
  std::array<RowInteger, 3> numerator;
  std::array<bool, 3> negative{}, nonzero{};
  RowInteger denominator;
  std::array<double, 3> approximate{};
  std::array<double, 3> error{};
  bool candidate_available = true;
  explicit ExactRow(const Matrix& matrix, unsigned row);
  // Includes all three terms even for zero coefficients; implicit +0 bias.
  // All mutable integers belong to declared invocation scratch.
  Result<std::uint64_t> evaluate(
      const std::array<std::uint64_t, 3>& input, bool narrow,
      RowWorkspace* workspace,
      const core_internal::WorkConsumer& consume) const;
};

constexpr unsigned block_size = 64;
struct CandidateBlock final {
  std::array<std::array<double, block_size>, 3> input{};
  std::array<double, block_size> value{}, magnitude{};
};
// No contraction/reassociation: SIMD candidates have exactly the scalar DAG.
void candidates(CandidateBlock* block, unsigned count, const ExactRow& row,
                SequenceProfile profile);
// FP32: certifies RN32, hence even strict uses this path. FP64 accelerated:
// certifies the NUM bound inside the normal FP32 range. FP64 strict is exact.
// Returning false requests exact evaluation; it never masks a resource error.
bool certify(const ExactRow& row, const CandidateBlock& block, unsigned lane,
             bool narrow, SequenceProfile profile, std::uint64_t* result);
inline double outward(double value, bool upward) {
  static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559,
                "FMT-10 requires IEEE binary64");
  constexpr auto sign = UINT64_C(0x8000000000000000);
  constexpr auto infinity = UINT64_C(0x7ff0000000000000);
  auto raw = bits(value);
  const auto magnitude = raw & ~sign;
  if (magnitude > infinity)
    return value;
  if (!magnitude)
    raw = (upward ? 0 : sign) | 1;
  else if (magnitude == infinity && upward == !(raw & sign))
    return value;
  else if (upward == !(raw & sign))
    ++raw;
  else
    --raw;
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}
}  // namespace ps::plugin_internal::basis_ops
