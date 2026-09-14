#pragma once

#include <array>
#include <cstdint>

namespace ps::plugin_internal::numeric_ops {
// Raw IEEE classification never executes floating arithmetic, including sNaN.
struct BinaryParts final {
  std::uint64_t bits = 0, magnitude = 0, significand = 0;
  int exponent = 0;
  bool negative = false, nan = false, infinite = false;
  static BinaryParts decode(std::uint64_t raw, bool narrow) {
    BinaryParts value;
    const unsigned fraction = narrow ? 23 : 52;
    const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
    const auto fraction_mask = (UINT64_C(1) << fraction) - 1;
    const auto exponent_mask = narrow ? 255U : 2047U;
    value.bits = raw;
    value.magnitude = raw & (sign - 1);
    value.negative = (raw & sign) != 0;
    const auto field = static_cast<unsigned>((raw >> fraction) & exponent_mask);
    value.significand = raw & fraction_mask;
    value.nan = field == exponent_mask && value.significand;
    value.infinite = field == exponent_mask && !value.significand;
    value.exponent = narrow ? -149 : -1074;
    if (field) {
      value.significand |= UINT64_C(1) << fraction;
      value.exponent += static_cast<int>(field) - 1;
    }
    return value;
  }
  std::uint64_t order_key() const {
    // Binary32 and binary64 are only compared within their own dtype.
    if (!magnitude)
      return UINT64_C(1) << 63;
    return negative ? (UINT64_C(1) << 63) - magnitude
                    : magnitude | (UINT64_C(1) << 63);
  }
};
// Nonnegative values in units of 2^-2148. Two finite binary64 significands
// multiply into at most 106 bits; their product plus an absolute tolerance
// fits below bit 4197. The 4352-bit storage and every loop are fixed/bounded.
struct PredicateInteger final {
  std::array<std::uint64_t, 68> words{};
  void set_product(const BinaryParts& a, const BinaryParts& b) {
    words.fill(0);
    const auto product =
        static_cast<unsigned __int128>(a.significand) * b.significand;
    const unsigned shift =
        static_cast<unsigned>(a.exponent + b.exponent + 2148);
    for (unsigned bit = 0; bit < 106; ++bit)
      if ((product >> bit) & 1)
        words[(shift + bit) / 64] |= UINT64_C(1) << ((shift + bit) % 64);
  }
  // Finite parts only; callers use fractional_bits 1074 or 2148, ensuring
  // every decoded significand has a nonnegative in-capacity shift.
  void set(const BinaryParts& value, unsigned fractional_bits = 2148) {
    words.fill(0);
    const unsigned shift = static_cast<unsigned>(
        value.exponent + static_cast<int>(fractional_bits));
    for (unsigned bit = 0; bit < 53; ++bit)
      if ((value.significand >> bit) & 1)
        words[(shift + bit) / 64] |= UINT64_C(1) << ((shift + bit) % 64);
  }
  void add(const PredicateInteger& other) {
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < words.size(); ++i) {
      const auto sum =
          static_cast<unsigned __int128>(words[i]) + other.words[i] + carry;
      words[i] = static_cast<std::uint64_t>(sum);
      carry = static_cast<std::uint64_t>(sum >> 64);
    }
  }
  // Requires this >= other, guaranteed by magnitude selection for |a-b|.
  void subtract(const PredicateInteger& other) {
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < words.size(); ++i) {
      const auto sub = static_cast<unsigned __int128>(other.words[i]) + borrow;
      const auto value = words[i];
      words[i] = static_cast<std::uint64_t>(
          static_cast<unsigned __int128>(value) - sub);
      borrow = static_cast<unsigned __int128>(value) < sub;
    }
  }
};
}  // namespace ps::plugin_internal::numeric_ops
