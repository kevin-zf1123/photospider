#pragma once
#include <array>
#include <cstdint>
#include <cstring>

#include "common/domain/exact_numeric.hpp"
#include "photospider/ops/export.hpp"
namespace ps::plugin_internal::basis_ops {
using data_internal::format_numeric::Natural;
using data_internal::format_numeric::Rational;
using Matrix = std::array<Rational, 9>;
using Triple = std::array<Rational, 3>;

// All geometry is reduced exact rational arithmetic. The existing 8192-bit
// Natural capacity is an explicit resource limit, never a conditioning test.
PHOTOSPIDER_OPS_SDK_API Rational reduced(Rational x);
PHOTOSPIDER_OPS_SDK_API Rational number(double x);
PHOTOSPIDER_OPS_SDK_API Rational add(const Rational& a, const Rational& b);
PHOTOSPIDER_OPS_SDK_API Rational subtract(const Rational& a, const Rational& b);
PHOTOSPIDER_OPS_SDK_API Rational multiply(const Rational& a, const Rational& b);
PHOTOSPIDER_OPS_SDK_API Rational divide(const Rational& a, const Rational& b);
PHOTOSPIDER_OPS_SDK_API Matrix inverse(const Matrix& a);
PHOTOSPIDER_OPS_SDK_API Triple white_xyz(const std::array<double, 2>& xy);
PHOTOSPIDER_OPS_SDK_API Matrix rgb_matrix(const std::array<double, 6>& xy,
                                          const std::array<double, 2>& white);
PHOTOSPIDER_OPS_SDK_API Matrix adaptation(const std::array<double, 2>& source,
                                          const std::array<double, 2>& target,
                                          unsigned method);
PHOTOSPIDER_OPS_SDK_API bool identity(const Matrix& m);

struct PHOTOSPIDER_OPS_SDK_API GeometryError final {};
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
}  // namespace ps::plugin_internal::basis_ops
