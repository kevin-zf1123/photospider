#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>

#include "01-numeric/accelerated_math.hpp"
#include "01-numeric/comparison_profiles.hpp"
#include "01-numeric/exact_predicate.hpp"
#include "photospider/core/status.hpp"

namespace ps::plugin_internal::numeric_ops {
// Caller-owned, fixed-capacity exact rational scratch. round() computes
// RN((numerator / denominator) * 2^scale). The default scale -1074
// matches products in units 2^-2148 divided by a width in units 2^-1074.
// Callers prove capacity for their formula and all shifted division operands.
template <std::size_t Words>
struct ExactRatioWorkspace final {
  using Integer = FixedInteger<Words>;
  Integer numerator, denominator, term, shifted, candidate;
  std::array<std::int64_t, 4> greater{}, less{};
  bool negative = false;
  SequenceProfile profile;
  explicit ExactRatioWorkspace(SequenceProfile selected) : profile(selected) {}
  int compare(const Integer& a, const Integer& b) {
    for (std::size_t end = a.words.size(); end; end -= 4) {
      compare_keys(a.words.data() + end - 4, b.words.data() + end - 4,
                   greater.data(), less.data(), profile);
      for (unsigned i = 4; i; --i) {
        if (greater[i - 1])
          return 1;
        if (less[i - 1])
          return -1;
      }
    }
    return 0;
  }
  static int top(const Integer& value) {
    for (std::size_t i = value.words.size(); i; --i)
      if (value.words[i - 1])
        return static_cast<int>((i - 1) * 64 + 63 -
                                __builtin_clzll(value.words[i - 1]));
    return -1;
  }
  // Distinct input/output. Reject a nonzero bit shifted beyond capacity.
  static bool shift(const Integer& source, unsigned bits, Integer* output) {
    output->words.fill(0);
    const auto whole = bits / 64, tail = bits % 64;
    for (std::size_t i = 0; i < source.words.size(); ++i) {
      const auto value = source.words[i];
      if (!value)
        continue;
      if (i + whole >= output->words.size())
        return false;
      output->words[i + whole] |= value << tail;
      if (tail && (value >> (64 - tail))) {
        if (i + whole + 1 >= output->words.size())
          return false;
        output->words[i + whole + 1] |= value >> (64 - tail);
      }
    }
    return true;
  }
  void add_term(bool sign) {
    if (sign == negative) {
      numerator.add(term);
      return;
    }
    const auto order = compare(numerator, term);
    if (order >= 0) {
      numerator.subtract(term);
    } else {
      term.subtract(numerator);
      numerator = term;
      negative = sign;
    }
    if (!order)
      negative = false;
  }
  void product_term(const BinaryParts& a, const BinaryParts& b,
                    bool subtract = false) {
    term.set_product(a, b);
    add_term((a.negative != b.negative) != subtract);
  }
  Result<std::uint64_t> round(
      bool narrow, const std::function<Status(std::uint64_t)>& consume,
      int scale = -1074, bool approximate = false) {
    using Answer = Result<std::uint64_t>;
    const auto capacity = [] {
      return Answer(Status{ErrorCode::ResourceExhausted,
                           "exact ratio scratch capacity",
                           FailureReason::CapacityLimit});
    };
    auto work = consume(512);
    if (!work.ok())
      return Answer(work);
    const auto n_top = top(numerator), d_top = top(denominator);
    if (d_top < 0)
      return Answer(
          Status{ErrorCode::InvalidArgument, "zero exact denominator"});
    if (n_top < 0)
      return Answer(UINT64_C(0));
    // A power-of-two denominator needs only bit extraction and ties-to-even.
    // This is also the common finish path for exact sums, dots and prefixes.
    bool dyadic = true;
    for (std::size_t i = 0; i < Words; ++i)
      if (denominator.words[i] != (i == static_cast<unsigned>(d_top) / 64
                                       ? UINT64_C(1) << (d_top % 64)
                                       : 0)) {
        dyadic = false;
        break;
      }
    if (dyadic) {
      const unsigned fraction = narrow ? 23 : 52;
      const int minimum = narrow ? -149 : -1074;
      const int binary_scale = scale - d_top;
      int quantum =
          std::max(minimum, n_top + binary_scale - static_cast<int>(fraction));
      const int shift_bits = quantum - binary_scale;
      const auto bit = [&](int index) {
        return index >= 0 && index < static_cast<int>(Words * 64) &&
               ((numerator.words[index / 64] >> (index % 64)) & 1);
      };
      std::uint64_t significand = 0;
      for (unsigned i = 0; i <= fraction; ++i)
        if (bit(shift_bits + static_cast<int>(i)))
          significand |= UINT64_C(1) << i;
      bool sticky = false;
      if (shift_bits > 1) {
        const auto bits = static_cast<unsigned>(shift_bits - 1);
        const auto whole = std::min<std::size_t>(bits / 64, Words);
        for (std::size_t i = 0; i < whole; ++i)
          sticky |= numerator.words[i] != 0;
        if (whole < Words && bits % 64)
          sticky |= (numerator.words[whole] &
                     ((UINT64_C(1) << (bits % 64)) - 1)) != 0;
      }
      if (bit(shift_bits - 1) && (sticky || (significand & 1)))
        ++significand;
      if (significand == (UINT64_C(1) << (fraction + 1))) {
        significand >>= 1;
        ++quantum;
      }
      std::uint64_t exponent = 0;
      if (significand >= (UINT64_C(1) << fraction)) {
        exponent = quantum - minimum + 1;
        significand -= UINT64_C(1) << fraction;
      }
      const auto maximum = narrow ? 255U : 2047U;
      if (exponent >= maximum) {
        exponent = maximum;
        significand = 0;
      }
      return Answer(
          (static_cast<std::uint64_t>(negative) << (narrow ? 31 : 63)) |
          (exponent << fraction) | significand);
    }
    // Bound the integer ratio using its leading 53 bits. Discarded low bits
    // widen the endpoint, so cancellation in constructing numerator remains
    // exact. No original Float64 operand is narrowed to Float32.
    if (approximate && profile != SequenceProfile::Strict) {
      input_internal::Float32Environment environment;
      const int exponent = n_top - d_top + scale;
      if (environment.active() && exponent >= -127 && exponent <= 128) {
        const auto normalized = [](const Integer& integer, int highest) {
          std::uint64_t head = 0;
          for (unsigned i = 0; i < 53; ++i) {
            const int index = highest - static_cast<int>(i);
            if (index >= 0 && ((integer.words[index / 64] >> (index % 64)) & 1))
              head |= UINT64_C(1) << (52 - i);
          }
          const double low = std::ldexp(static_cast<double>(head), -52);
          return FastInterval{low, highest > 52 ? numeric_up(low) : low};
        };
        auto value =
            normalized(numerator, n_top) / normalized(denominator, d_top);
        value = {numeric_down(std::ldexp(value.low, exponent)),
                 numeric_up(std::ldexp(value.high, exponent))};
        if (negative)
          value = -value;
        auto fast =
            value.accepted(value.low + (value.high - value.low) * .5, narrow);
        if (fast)
          return Answer(*fast);
      }
    }
    const unsigned fraction = narrow ? 23 : 52;
    const int minimum = narrow ? -149 : -1074;
    int ratio_top = n_top - d_top;
    if (ratio_top >= 0) {
      if (!shift(denominator, static_cast<unsigned>(ratio_top), &candidate))
        return capacity();
      if (compare(numerator, candidate) < 0)
        --ratio_top;
    } else {
      if (!shift(numerator, static_cast<unsigned>(-ratio_top), &candidate))
        return capacity();
      if (compare(candidate, denominator) < 0)
        --ratio_top;
    }
    int quantum =
        std::max(minimum, ratio_top + scale - static_cast<int>(fraction));
    const auto alignment = quantum - scale;
    if (alignment >= 0) {
      if (!shift(denominator, static_cast<unsigned>(alignment), &shifted))
        return capacity();
    } else {
      if (!shift(numerator, static_cast<unsigned>(-alignment), &candidate))
        return capacity();
      numerator = candidate;
      shifted = denominator;
    }
    std::uint64_t significand = 0;
    for (unsigned i = fraction + 1; i; --i) {
      work = consume(256);
      if (!work.ok())
        return Answer(work);
      if (!shift(shifted, i - 1, &candidate))
        return capacity();
      if (compare(numerator, candidate) >= 0) {
        numerator.subtract(candidate);
        significand |= UINT64_C(1) << (i - 1);
      }
    }
    if (!shift(numerator, 1, &candidate))
      return capacity();
    const auto remainder_order = compare(candidate, shifted);
    if (remainder_order > 0 || (!remainder_order && (significand & 1)))
      ++significand;
    if (significand == (UINT64_C(1) << (fraction + 1))) {
      significand >>= 1;
      ++quantum;
    }
    const auto implicit = UINT64_C(1) << fraction;
    std::uint64_t exponent = 0;
    if (significand >= implicit) {
      exponent = quantum - minimum + 1;
      significand -= implicit;
    }
    const auto maximum = narrow ? 255U : 2047U;
    if (exponent >= maximum) {
      exponent = maximum;
      significand = 0;
    }
    return Answer((static_cast<std::uint64_t>(negative) << (narrow ? 31 : 63)) |
                  (exponent << fraction) | significand);
  }
};
using RatioWorkspace = ExactRatioWorkspace<68>;
}  // namespace ps::plugin_internal::numeric_ops
