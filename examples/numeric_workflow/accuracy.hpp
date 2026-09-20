#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#include "photospider/core/numeric_diagnostics.hpp"

inline bool numeric_accuracy(std::uint64_t actual, std::uint64_t reference,
                             ps::CpuNumericProfile profile) {
  if (actual == reference)
    return true;
  if (profile == ps::CpuNumericProfile::Strict)
    return false;
  double a = 0, r = 0;
  std::memcpy(&a, &actual, 8);
  std::memcpy(&r, &reference, 8);
  if (!std::isfinite(a) || !std::isfinite(r) || std::abs(r) < 0x1p-126 ||
      std::abs(r) > 0x1.fffffep127)
    return false;
  return std::abs(a - r) <= std::ldexp(1.0, std::ilogb(std::abs(r)) - 21);
}
