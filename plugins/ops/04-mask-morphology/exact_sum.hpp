#pragma once

#include "01-numeric/exact_ratio.hpp"
#include "04-mask-morphology/common.hpp"

namespace ps::plugin_internal::mask {
template <std::size_t Words>
void set_coefficient(numeric_ops::FixedInteger<Words>* out,
                     unsigned __int128 coefficient, unsigned shift) {
  out->words.fill(0);
  const auto index = shift / 64, tail = shift % 64;
  const auto low = static_cast<std::uint64_t>(coefficient);
  const auto high = static_cast<std::uint64_t>(coefficient >> 64);
  out->words[index] = low << tail;
  if (index + 1 < Words)
    out->words[index + 1] = tail ? (low >> (64 - tail)) | (high << tail) : high;
  if (tail && index + 2 < Words)
    out->words[index + 2] = high >> (64 - tail);
}
template <unsigned Words>
struct WeightedSum final {
  using Integer = numeric_ops::FixedInteger<Words>;
  Integer numerator, denominator;
  bool negative = false;
  void signed_add(Integer value, bool sign) {
    if (sign == negative) {
      numerator.add(value);
      return;
    }
    int order = 0;
    for (unsigned j = Words; j; --j) {
      if (numerator.words[j - 1] != value.words[j - 1]) {
        order = numerator.words[j - 1] < value.words[j - 1] ? -1 : 1;
        break;
      }
    }
    if (order >= 0) {
      numerator.subtract(value);
    } else {
      value.subtract(numerator);
      numerator = value;
      negative = sign;
    }
    if (!order)
      negative = false;
  }
  void merge(const WeightedSum& b, bool subtract) {
    signed_add(b.numerator, b.negative != subtract);
    if (subtract)
      denominator.subtract(b.denominator);
    else
      denominator.add(b.denominator);
  }
  void sample(std::uint64_t x, std::uint64_t w, bool narrow, bool subtract) {
    constexpr int minimum = Words == 12 ? -149 : -1074;
    const auto a = numeric_ops::BinaryParts::decode(x, narrow);
    const auto b = numeric_ops::BinaryParts::decode(w, narrow);
    Integer term;
    set_coefficient(
        &term, static_cast<unsigned __int128>(a.significand) * b.significand,
        static_cast<unsigned>(a.exponent + b.exponent - 2 * minimum));
    signed_add(term, (a.negative != b.negative) != subtract);
    set_coefficient(&term, b.significand,
                    static_cast<unsigned>(b.exponent - minimum));
    if (subtract)
      denominator.subtract(term);
    else
      denominator.add(term);
  }
  bool empty() const {
    return numeric_ops::ExactRatioWorkspace<Words>::top(denominator) < 0;
  }
  std::uint64_t finish(numeric_ops::ExactRatioWorkspace<Words>& ratio,
                       bool narrow, const Work& work) const {
    ratio.numerator = numerator;
    ratio.denominator = denominator;
    ratio.negative = negative;
    return take(ratio.round(narrow, work, Words == 12 ? -149 : -1074));
  }
};
}  // namespace ps::plugin_internal::mask
