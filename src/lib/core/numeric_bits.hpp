#pragma once

#include <cstdint>
#include <cstring>

namespace ps::core_internal {
// Bitwise binary64 inspection does not raise floating-point exceptions for
// signaling NaNs and does not depend on the caller's floating-point mode.
inline std::uint64_t binary64_bits(double value) noexcept {
  std::uint64_t result;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}
// Numeric ordering for finite binary64 values; both signed zeros share a key.
inline std::uint64_t binary64_order_key(std::uint64_t bits) noexcept {
  constexpr std::uint64_t sign = UINT64_C(1) << 63;
  const auto magnitude = bits & (sign - 1);
  return bits & sign ? sign - magnitude : sign | magnitude;
}
inline bool finite_binary64(double value) noexcept {
  constexpr std::uint64_t exponent = UINT64_C(0x7ff0000000000000);
  return (binary64_bits(value) & exponent) != exponent;
}
}  // namespace ps::core_internal
