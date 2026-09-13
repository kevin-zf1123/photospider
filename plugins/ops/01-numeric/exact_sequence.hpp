#pragma once

#include <array>
#include <cstdint>
#include <cstring>

namespace ps::plugin_internal::numeric_ops {
/**
 * Exact signed sum of weighted binary64 inputs, in units of 2^-1074.
 * 68 base-2^32 limbs cover 2098 binary64 bits plus a 20-bit count and carry.
 * Storage is inline, so a containing host continuation owns all retained limbs.
 * Every loop has a fixed bound, independent of exponent gaps or sample count.
 */
struct ExactSequence final {
  std::array<std::uint32_t, 68> words{};
  bool negative = false;

  void set_integer(std::int64_t value) {
    words.fill(0);
    negative = value < 0;
    auto magnitude = static_cast<std::uint64_t>(value);
    if (negative)
      magnitude = UINT64_C(0) - magnitude;
    words[0] = static_cast<std::uint32_t>(magnitude);
    words[1] = static_cast<std::uint32_t>(magnitude >> 32);
  }

  void set(double value) {
    words.fill(0);
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    negative = (bits >> 63) != 0;
    const auto exponent = static_cast<unsigned>((bits >> 52) & 2047);
    auto significand = bits & UINT64_C(0xfffffffffffff);
    if (exponent)
      significand |= UINT64_C(1) << 52;
    const unsigned shift = exponent ? exponent - 1 : 0;
    for (unsigned bit = 0; bit < 53; ++bit)
      if ((significand >> bit) & 1)
        words[(shift + bit) / 32] |= UINT32_C(1) << ((shift + bit) % 32);
  }

  void multiply(std::uint32_t factor) {
    std::uint64_t carry = 0;
    for (auto& word : words) {
      const auto product = static_cast<std::uint64_t>(word) * factor + carry;
      word = static_cast<std::uint32_t>(product);
      carry = product >> 32;
    }
  }

  void add(const ExactSequence& other) {
    if (negative == other.negative) {
      std::uint64_t carry = 0;
      for (unsigned i = 0; i < words.size(); ++i) {
        const auto sum =
            static_cast<std::uint64_t>(words[i]) + other.words[i] + carry;
        words[i] = static_cast<std::uint32_t>(sum);
        carry = sum >> 32;
      }
      return;
    }
    int comparison = 0;
    for (unsigned i = words.size(); i && !comparison; --i)
      if (words[i - 1] != other.words[i - 1])
        comparison = words[i - 1] > other.words[i - 1] ? 1 : -1;
    std::uint64_t borrow = 0;
    for (unsigned i = 0; i < words.size(); ++i) {
      const auto a = comparison >= 0 ? words[i] : other.words[i];
      const auto b = comparison >= 0 ? other.words[i] : words[i];
      const auto subtrahend = static_cast<std::uint64_t>(b) + borrow;
      words[i] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(a) -
                                            subtrahend);
      borrow = static_cast<std::uint64_t>(a) < subtrahend;
    }
    if (comparison < 0)
      negative = other.negative;
    if (comparison == 0)
      negative = false;
  }

  bool bit(unsigned index) const {
    return ((words[index / 32] >> (index % 32)) & 1) != 0;
  }

  /** Destructively divides by a positive count and rounds directly to IEEE
   * bits. No floating operation or ambient rounding mode participates.
   * Callers reject exponent-all-ones when their output is finite-only.
   */
  std::uint64_t rounded_bits(std::uint32_t divisor, bool binary32,
                             bool negative_exact_zero = false) {
    std::uint64_t remainder = 0;
    for (unsigned i = words.size(); i; --i) {
      const auto dividend = (remainder << 32) | words[i - 1];
      words[i - 1] = static_cast<std::uint32_t>(dividend / divisor);
      remainder = dividend % divisor;
    }
    int top = -1;
    for (unsigned i = words.size(); i && top < 0; --i)
      if (words[i - 1]) {
        unsigned word = words[i - 1];
        unsigned high = 0;
        while (word >>= 1)
          ++high;
        top = static_cast<int>((i - 1) * 32 + high);
      }
    const unsigned fraction = binary32 ? 23 : 52;
    const unsigned minimum_shift = binary32 ? 925 : 0;
    unsigned shift = minimum_shift;
    if (top > static_cast<int>(shift + fraction))
      shift = static_cast<unsigned>(top) - fraction;
    std::uint64_t significand = 0;
    for (unsigned i = 0; i <= fraction; ++i)
      if (bit(shift + i))
        significand |= UINT64_C(1) << i;
    bool increment = false;
    if (shift) {
      bool sticky = remainder != 0;
      for (unsigned i = 0; i + 1 < shift; ++i)
        sticky = sticky || bit(i);
      increment = bit(shift - 1) && (sticky || (significand & 1));
    } else {
      increment = remainder * 2 > divisor ||
                  (remainder * 2 == divisor && (significand & 1));
    }
    significand += increment;
    if (significand == (UINT64_C(1) << (fraction + 1))) {
      significand >>= 1;
      ++shift;
    }
    const auto implicit = UINT64_C(1) << fraction;
    std::uint64_t exponent = 0;
    if (significand >= implicit) {
      exponent = shift - minimum_shift + 1;
      significand -= implicit;
    }
    const std::uint64_t max_exponent = binary32 ? 255 : 2047;
    if (exponent >= max_exponent) {
      exponent = max_exponent;
      significand = 0;
    }
    const bool sign =
        top < 0 && remainder == 0 ? negative_exact_zero : negative;
    return (static_cast<std::uint64_t>(sign) << (binary32 ? 31 : 63)) |
           (exponent << fraction) | significand;
  }
};
}  // namespace ps::plugin_internal::numeric_ops
