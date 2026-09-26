#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

#include "01-numeric/exact_predicate.hpp"
#include "01-numeric/sequence_profiles.hpp"
#include "data/exact_numeric.hpp"

namespace ps::plugin_internal::basis_ops {
using data_internal::format_numeric::Natural;
using data_internal::format_numeric::Rational;
using Matrix = std::array<Rational, 9>;
using Triple = std::array<Rational, 3>;
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;

// All geometry is reduced exact rational arithmetic. The existing 8192-bit
// Natural capacity is an explicit resource limit, never a conditioning test.
Rational reduced(Rational x);
Rational number(double x);
Rational add(const Rational& a, const Rational& b);
Rational subtract(const Rational& a, const Rational& b);
Rational multiply(const Rational& a, const Rational& b);
Rational divide(const Rational& a, const Rational& b);
Matrix inverse(const Matrix& a);
Triple white_xyz(const std::array<double, 2>& xy);
Matrix rgb_matrix(const std::array<double, 6>& xy,
                  const std::array<double, 2>& white);
Matrix adaptation(const std::array<double, 2>& source,
                  const std::array<double, 2>& target, unsigned method);
bool identity(const Matrix& m);

struct GeometryError final {};
struct ExactRow final {
  std::array<Natural, 3> numerator;
  std::array<bool, 3> negative{};
  Natural denominator{1};
  std::array<double, 3> approximate{};
  std::array<double, 3> error{};
  bool candidate_available = true;
  explicit ExactRow(const Matrix& matrix, unsigned row);
  // Includes all three terms even for zero coefficients; implicit +0 bias.
  // Original allocation-based algorithm, retained for differential tests.
  std::uint64_t evaluate_reference(const std::array<std::uint64_t, 3>& input,
                                   bool narrow) const;
  std::uint64_t evaluate(const std::array<std::uint64_t, 3>& input,
                         bool narrow) const;
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
inline std::uint64_t bits(double value) {
  std::uint64_t out;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}
inline bool finite_bits(std::uint64_t value, bool narrow) {
  return narrow ? (value & 0x7f800000U) != 0x7f800000U
                : (value & UINT64_C(0x7ff0000000000000)) !=
                      UINT64_C(0x7ff0000000000000);
}
// Internal outward interval endpoint, not a public nextafter replacement:
// exact IEEE value semantics, without libm errno/exception side effects. All
// numeric callbacks already save/restore the caller's floating environment.
// NaNs are not endpoints of accepted certificates; preserve their raw bits.
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
