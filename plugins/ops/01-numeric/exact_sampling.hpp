#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>

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
