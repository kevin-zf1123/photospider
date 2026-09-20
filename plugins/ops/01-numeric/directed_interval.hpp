#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "01-numeric/exact_product.hpp"
#include "01-numeric/math_constants.hpp"

namespace ps::plugin_internal::numeric_ops {
// Fixed-capacity signed dyadic enclosures. The full pool is owned/admitted as
// part of its operation continuation. No arithmetic limb allocates on the heap.
// Inputs and every intermediate are checked against the 12288-bit capacity.
struct DirectedInterval final {
  static constexpr std::size_t kWords = 192, kSlots = 128;
  using Integer = FixedInteger<kWords>;
  using Workspace = ExactRatioWorkspace<kWords>;
  struct Unresolved {};
  struct Number {
    Integer magnitude;
    bool negative = false;
  };
  struct Interval {
    Number& low;
    Number& high;
  };
  std::array<Number, kSlots> pool;
  Workspace rounding;
  std::size_t used = 0;
  unsigned precision = 128;
  const std::function<Status(std::uint64_t)>* consume = nullptr;
  explicit DirectedInterval(SequenceProfile profile) : rounding(profile) {}
  struct Frame {
    DirectedInterval& context;
    std::size_t saved;
    explicit Frame(DirectedInterval& owner)
        : context(owner), saved(owner.used) {}
    ~Frame() { context.used = saved; }
  };
  void work(std::uint64_t amount) const {
    const auto status = (*consume)(amount);
    if (!status.ok())
      throw status;
  }
  [[noreturn]] static void capacity() {
    throw Status{ErrorCode::ResourceExhausted,
                 "directed math precision/state capacity",
                 FailureReason::CapacityLimit};
  }
  Number& number() {
    if (used == pool.size())
      capacity();
    auto& result = pool[used++];
    clear(result);
    return result;
  }
  Interval interval() {
    auto& low = number();
    auto& high = number();
    return {low, high};
  }
  void clear(Number& value) const {
    work(kWords);
    value.magnitude.words.fill(0);
    value.negative = false;
  }
  void copy(Number& output, const Number& value) const {
    work(kWords);
    output = value;
  }
  void copy(Interval output, Interval value) const {
    copy(output.low, value.low);
    copy(output.high, value.high);
  }
  int top(const Number& value) const { return Workspace::top(value.magnitude); }
  void normalize(Number& value) const {
    if (top(value) < 0)
      value.negative = false;
  }
  int compare_unsigned(const Integer& a, const Integer& b) {
    work(kWords);
    return rounding.compare(a, b);
  }
  int compare(const Number& a, const Number& b) {
    if (a.negative != b.negative)
      return a.negative ? -1 : 1;
    const auto order = compare_unsigned(a.magnitude, b.magnitude);
    return a.negative ? -order : order;
  }
  void increment(Integer& value) const {
    work(kWords);
    for (auto& word : value.words)
      if (++word)
        return;
    capacity();
  }
  // Source and destination must be distinct; signed wrappers arrange this.
  void shift_unsigned(Integer& output, const Integer& value, int shift) const {
    work(kWords * 2);
    if (shift >= 0) {
      if (!Workspace::shift(value, static_cast<unsigned>(shift), &output))
        capacity();
      return;
    }
    const auto bits = static_cast<unsigned>(-shift), whole = bits / 64,
               tail = bits % 64;
    output.words.fill(0);
    for (std::size_t j = whole; j < kWords; ++j) {
      output.words[j - whole] |= value.words[j] >> tail;
      if (tail && j > whole)
        output.words[j - whole - 1] |= value.words[j] << (64 - tail);
    }
  }
  bool discarded(const Integer& value, unsigned bits) const {
    const auto whole = std::min<std::size_t>(bits / 64, kWords);
    const auto tail = bits % 64;
    for (std::size_t j = 0; j < whole; ++j)
      if (value.words[j])
        return true;
    return whole < kWords && tail &&
           (value.words[whole] & ((UINT64_C(1) << tail) - 1));
  }
  // Distinct source/destination. lower selects floor, otherwise ceiling.
  void shift(Number& output, const Number& value, int bits, bool lower) const {
    shift_unsigned(output.magnitude, value.magnitude, bits);
    output.negative = value.negative;
    if (bits < 0 && discarded(value.magnitude, static_cast<unsigned>(-bits)) &&
        (lower == value.negative))
      increment(output.magnitude);
    normalize(output);
  }
  void integer(Number& output, std::int64_t value) const {
    clear(output);
    const auto raw = static_cast<std::uint64_t>(value);
    const auto magnitude = value < 0 ? UINT64_C(0) - raw : raw;
    output.magnitude.words[precision / 64] = magnitude << (precision % 64);
    if (precision % 64)
      output.magnitude.words[precision / 64 + 1] =
          magnitude >> (64 - precision % 64);
    output.negative = value < 0;
  }
  void integer(Interval output, std::int64_t value) const {
    integer(output.low, value);
    copy(output.high, output.low);
  }
  void unsigned_integer(Interval output, unsigned __int128 value) {
    Frame frame(*this);
    auto& source = number();
    source.magnitude.words[0] = static_cast<std::uint64_t>(value);
    source.magnitude.words[1] = static_cast<std::uint64_t>(value >> 64);
    shift(output.low, source, static_cast<int>(precision), true);
    copy(output.high, output.low);
  }
  void dyadic(Interval output, std::uint64_t significand, int exponent,
              bool negative = false) {
    Frame frame(*this);
    auto& source = number();
    source.magnitude.words[0] = significand;
    source.negative = negative;
    shift(output.low, source, static_cast<int>(precision) + exponent, true);
    shift(output.high, source, static_cast<int>(precision) + exponent, false);
  }
  void raw(Interval output, std::uint64_t bits, bool narrow) {
    const auto parts = BinaryParts::decode(bits, narrow);
    dyadic(output, parts.significand, parts.exponent, parts.negative);
  }
  void widen(Interval value, std::uint64_t units) {
    Frame frame(*this);
    auto& error = number();
    error.magnitude.words[0] = units;
    error.negative = true;
    add(value.low, value.low, error);
    error.negative = false;
    add(value.high, value.high, error);
  }
  void constant(Interval output, bool pi) {
    if (precision < 128 || precision > 4096)
      capacity();
    Frame frame(*this);
    auto& source = number();
    const auto& words = pi ? pi_floor_4096 : ln2_floor_4096;
    std::copy(words.begin(), words.end(), source.magnitude.words.begin());
    shift_unsigned(output.low.magnitude, source.magnitude,
                   static_cast<int>(precision) - 4096);
    output.low.negative = false;
    copy(output.high, output.low);
    increment(output.high.magnitude);
  }
  // Alias-safe signed integer addition, exact in the shared Q_precision units.
  void add(Number& output, const Number& a, const Number& b) {
    Frame frame(*this);
    auto& temporary = number();
    if (a.negative == b.negative) {
      if (std::max(top(a), top(b)) + 1 >= static_cast<int>(kWords * 64))
        capacity();
      copy(temporary, a);
      work(kWords);
      temporary.magnitude.add(b.magnitude);
    } else {
      const auto order = compare_unsigned(a.magnitude, b.magnitude);
      copy(temporary, order >= 0 ? a : b);
      work(kWords);
      temporary.magnitude.subtract(order >= 0 ? b.magnitude : a.magnitude);
    }
    normalize(temporary);
    copy(output, temporary);
  }
  void negate(Interval output, Interval value) {
    Frame frame(*this);
    auto temp = interval();
    copy(temp.low, value.high);
    copy(temp.high, value.low);
    if (top(temp.low) >= 0)
      temp.low.negative = !temp.low.negative;
    if (top(temp.high) >= 0)
      temp.high.negative = !temp.high.negative;
    copy(output, temp);
  }
  void add(Interval output, Interval a, Interval b) {
    Frame frame(*this);
    auto temp = interval();
    add(temp.low, a.low, b.low);
    add(temp.high, a.high, b.high);
    copy(output, temp);
  }
  void subtract(Interval output, Interval a, Interval b) {
    Frame frame(*this);
    auto opposite = interval();
    negate(opposite, b);
    add(output, a, opposite);
  }
  void multiply(Number& output, const Number& a, const Number& b, bool lower) {
    Frame frame(*this);
    auto& product = number();
    auto status =
        multiply_fixed(a.magnitude, b.magnitude, &product.magnitude, *consume);
    if (!status.ok())
      throw status;
    product.negative = a.negative != b.negative;
    shift(output, product, -static_cast<int>(precision), lower);
  }
  void multiply(Interval output, Interval a, Interval b) {
    Frame frame(*this);
    auto result = interval();
    auto& candidate = number();
    bool first = true;
    for (const auto* left : {&a.low, &a.high})
      for (const auto* right : {&b.low, &b.high}) {
        multiply(candidate, *left, *right, true);
        if (first || compare(candidate, result.low) < 0)
          copy(result.low, candidate);
        multiply(candidate, *left, *right, false);
        if (first || compare(candidate, result.high) > 0)
          copy(result.high, candidate);
        first = false;
      }
    copy(output, result);
  }
  // All outputs/scratch are distinct from both immutable inputs.
  void divide_unsigned(Integer& quotient, Integer& remainder,
                       const Integer& numerator, const Integer& denominator) {
    Frame frame(*this);
    auto& shifted = number();
    if (Workspace::top(denominator) < 0)
      capacity();
    work(2 * kWords);
    quotient.words.fill(0);
    remainder = numerator;
    const int bits = Workspace::top(numerator) - Workspace::top(denominator);
    if (bits < 0)
      return;
    shift_unsigned(shifted.magnitude, denominator, bits);
    const auto words =
        static_cast<std::size_t>((Workspace::top(numerator) + 64) / 64);
    const auto blocks = (words + 3) / 4 * 4;
    for (int bit = bits; bit >= 0; --bit) {
      work(2 * words + blocks);
      int order = 0;
      // All words above the original numerator are zero throughout division.
      // Restrict compare/subtract/shift to that proved active capacity.
      for (std::size_t end = blocks; end && !order; end -= 4) {
        compare_keys(remainder.words.data() + end - 4,
                     shifted.magnitude.words.data() + end - 4,
                     rounding.greater.data(), rounding.less.data(),
                     rounding.profile);
        for (unsigned j = 4; j; --j) {
          if (rounding.greater[j - 1]) {
            order = 1;
            break;
          }
          if (rounding.less[j - 1]) {
            order = -1;
            break;
          }
        }
      }
      if (order >= 0) {
        std::uint64_t borrow = 0;
        for (std::size_t j = 0; j < words; ++j) {
          const auto subtract =
              static_cast<unsigned __int128>(shifted.magnitude.words[j]) +
              borrow;
          const auto original = remainder.words[j];
          remainder.words[j] = static_cast<std::uint64_t>(
              static_cast<unsigned __int128>(original) - subtract);
          borrow = static_cast<unsigned __int128>(original) < subtract;
        }
        quotient.words[static_cast<unsigned>(bit) / 64] |= UINT64_C(1)
                                                           << (bit % 64);
      }
      for (std::size_t j = 0; j < words; ++j)
        shifted.magnitude.words[j] =
            (shifted.magnitude.words[j] >> 1) |
            (j + 1 < words ? shifted.magnitude.words[j + 1] << 63 : 0);
    }
  }

  void divide(Number& output, const Number& a, const Number& b, bool lower) {
    Frame frame(*this);
    auto& scaled = number();
    auto& quotient = number();
    auto& remainder = number();
    shift_unsigned(scaled.magnitude, a.magnitude, static_cast<int>(precision));
    divide_unsigned(quotient.magnitude, remainder.magnitude, scaled.magnitude,
                    b.magnitude);
    quotient.negative = a.negative != b.negative;
    if (top(remainder) >= 0 && lower == quotient.negative)
      increment(quotient.magnitude);
    normalize(quotient);
    copy(output, quotient);
  }
  bool includes_zero(Interval value) const {
    return (value.low.negative || top(value.low) < 0) && !value.high.negative;
  }
  void divide(Interval output, Interval a, Interval b) {
    if (includes_zero(b))
      throw Unresolved{};
    Frame frame(*this);
    auto result = interval();
    auto& candidate = number();
    bool first = true;
    for (const auto* left : {&a.low, &a.high})
      for (const auto* right : {&b.low, &b.high}) {
        divide(candidate, *left, *right, true);
        if (first || compare(candidate, result.low) < 0)
          copy(result.low, candidate);
        divide(candidate, *left, *right, false);
        if (first || compare(candidate, result.high) > 0)
          copy(result.high, candidate);
        first = false;
      }
    copy(output, result);
  }
  void divide_small(Number& output, const Number& value, std::uint64_t divisor,
                    bool lower) const {
    work(kWords * 4);
    std::uint64_t remainder = 0;
    for (std::size_t j = kWords; j; --j) {
      const auto numerator = (static_cast<unsigned __int128>(remainder) << 64) |
                             value.magnitude.words[j - 1];
      output.magnitude.words[j - 1] =
          static_cast<std::uint64_t>(numerator / divisor);
      remainder = static_cast<std::uint64_t>(numerator % divisor);
    }
    output.negative = value.negative;
    if (remainder && lower == value.negative)
      increment(output.magnitude);
    normalize(output);
  }
  void divide_small(Interval output, Interval value, std::uint64_t divisor) {
    Frame frame(*this);
    auto result = interval();
    divide_small(result.low, value.low, divisor, true);
    divide_small(result.high, value.high, divisor, false);
    copy(output, result);
  }
  void scale(Interval output, Interval value, int bits) {
    Frame frame(*this);
    auto result = interval();
    shift(result.low, value.low, bits, true);
    shift(result.high, value.high, bits, false);
    copy(output, result);
  }
  std::uint64_t rounded(const Number& value, bool narrow) {
    work(2 * kWords);
    rounding.numerator = value.magnitude;
    rounding.denominator.words.fill(0);
    rounding.denominator.words[0] = 1;
    rounding.negative = value.negative;
    auto result =
        rounding.round(narrow, *consume, -static_cast<int>(precision));
    if (!result.ok())
      throw result.status();
    return result.value();
  }
};
}  // namespace ps::plugin_internal::numeric_ops
