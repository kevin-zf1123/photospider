#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>

#include "01-numeric/accelerated_math.hpp"
#include "01-numeric/sequence_profiles.hpp"

namespace ps::plugin_internal::numeric_ops {
// Shared finite-input endpoint interpolation, RN64 control reconstruction and
// direct output conversion. All scratch is part of the caller's continuation.
struct ExactSampling final {
  ExactSequence first, second;
  std::array<std::uint64_t, 68> products{};
  SequenceProfile profile;
  explicit ExactSampling(SequenceProfile selected) : profile(selected) {}
  Result<std::uint64_t> weighted(
      std::uint64_t a, std::uint64_t b, std::uint32_t wa, std::uint32_t wb,
      std::uint32_t divisor, bool narrow, bool subtract,
      const std::function<Status(std::uint64_t)>& consume) {
    auto charged = consume(8192);
    if (!charged.ok())
      return Result<std::uint64_t>(charged);
    double x = 0, y = 0;
    std::memcpy(&x, &a, 8);
    std::memcpy(&y, &b, 8);
    input_internal::Float32Environment environment;
    if (environment.active()) {
      const double left = x * wa, right = (subtract ? -y : y) * wb;
      const double sum = left + right;
      const double z = sum - left;
      const double residual = (left - (sum - z)) + (right - z);
      // Products and sum must be exact; only the final division may round.
      // Exclude tiny products whose FMA residual could itself underflow.
      if (std::isfinite(sum) && (x == 0 || std::abs(x) >= 0x1p-900) &&
          (y == 0 || std::abs(y) >= 0x1p-900) &&
          std::fma(x, static_cast<double>(wa), -left) == 0 &&
          std::fma(subtract ? -y : y, static_cast<double>(wb), -right) == 0 &&
          residual == 0 && divisor && !narrow) {
        const double value = sum / divisor;
        const bool negative_zero = !subtract && (a >> 63) && (!wb || (b >> 63));
        return Result<std::uint64_t>(
            value == 0 ? (static_cast<std::uint64_t>(
                              sum == 0 ? negative_zero : std::signbit(sum))
                          << 63)
                       : numeric_bits(value));
      }
      if (narrow && wa == 1 && wb == 0 && divisor == 1 && (!subtract || x != 0))
        return Result<std::uint64_t>(numeric_bits(x, true));
    }
    first.set(x);
    second.set(y);
    sequence_multiply(&first, wa, profile, products.data());
    sequence_multiply(&second, wb, profile, products.data());
    if (subtract)
      second.negative = !second.negative;
    first.add(second);
    const bool negative_zero = !subtract && (a >> 63) && (!wb || (b >> 63));
    return Result<std::uint64_t>(
        first.rounded_bits(divisor, narrow, negative_zero));
  }
  Result<std::uint64_t> coordinate(
      std::uint32_t index, std::uint32_t count,
      const std::array<std::uint64_t, 2>& endpoints,
      const std::function<Status(std::uint64_t)>& consume) {
    if (!index)
      return Result<std::uint64_t>(endpoints[0]);
    if (index + 1 == count)
      return Result<std::uint64_t>(endpoints[1]);
    return weighted(endpoints[0], endpoints[1], count - 1 - index, index,
                    count - 1, false, false, consume);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
