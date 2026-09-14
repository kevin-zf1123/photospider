#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "01-numeric/comparison_profiles.hpp"
#include "01-numeric/exact_predicate.hpp"
#include "photospider/core/status.hpp"

namespace ps::plugin_internal::numeric_ops {
// Caller-owned, fixed-capacity exact rational scratch. The numerator is in
// units 2^-2148, denominator in units 2^-1074; result therefore has scale
// 2^-1074. Four raw finite binary64 products require at most 4198 bits.
struct RatioWorkspace final {
  PredicateInteger numerator, denominator, term, shifted, candidate;
  std::array<std::int64_t, 4> greater{}, less{};
  bool negative = false;
  SequenceProfile profile;
  explicit RatioWorkspace(SequenceProfile selected) : profile(selected) {}
  int compare(const PredicateInteger& a, const PredicateInteger& b) {
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
  static int top(const PredicateInteger& value) {
    for (std::size_t i = value.words.size(); i; --i)
      if (value.words[i - 1])
        return static_cast<int>((i - 1) * 64 + 63 -
                                __builtin_clzll(value.words[i - 1]));
    return -1;
  }
  // Distinct input/output. Reject a nonzero bit shifted beyond capacity.
  static bool shift(const PredicateInteger& source, unsigned bits,
                    PredicateInteger* output) {
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
      bool narrow, const std::function<Status(std::uint64_t)>& consume) {
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
    const unsigned fraction = narrow ? 23 : 52, minimum = narrow ? 925 : 0;
    int ratio_top = n_top - d_top;
    if (ratio_top >= 0) {
      if (!shift(denominator, static_cast<unsigned>(ratio_top), &candidate))
        return capacity();
      if (compare(numerator, candidate) < 0)
        --ratio_top;
    }
    unsigned quantum = minimum;
    if (ratio_top > static_cast<int>(minimum + fraction))
      quantum = static_cast<unsigned>(ratio_top) - fraction;
    if (!shift(denominator, quantum, &shifted))
      return capacity();
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
}  // namespace ps::plugin_internal::numeric_ops
