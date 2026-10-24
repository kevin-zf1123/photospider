#pragma once

#include "01-numeric/exact_ratio.hpp"
#include "04-mask-morphology/common.hpp"
#include "04-mask-morphology/fast.hpp"

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
  template <class Consumer>
  std::uint64_t finish(numeric_ops::ExactRatioWorkspace<Words>& ratio,
                       bool narrow, const Consumer& work) const {
    if (narrow) {
      work.poll(128);
      const auto n_top =
          numeric_ops::ExactRatioWorkspace<Words>::top(numerator);
      const auto d_top =
          numeric_ops::ExactRatioWorkspace<Words>::top(denominator);
      if (n_top < 0)
        return 0;
      const int exponent = n_top - d_top + (Words == 12 ? -149 : -1074);
      if (exponent >= -149 && exponent <= 128) {
        const auto normalized = [](const Integer& value, int highest) {
          std::uint64_t head;
          if (highest <= 52) {
            head = value.words[0] << (52 - highest);
          } else {
            const auto shift = static_cast<unsigned>(highest - 52);
            const auto word = shift / 64, tail = shift % 64;
            head = value.words[word] >> tail;
            if (tail && word + 1 < Words)
              head |= value.words[word + 1] << (64 - tail);
          }
          const auto low = std::ldexp(static_cast<double>(head), -52);
          return Fast{low, highest > 52 ? numeric_ops::numeric_up(low) : low};
        };
        // The leading 53 integer bits and one outward ULP enclose every
        // omitted bit. Accept only identical correctly rounded endpoints;
        // exact midpoint, extreme exponent and Float64 cases retain division.
        auto value =
            normalized(numerator, n_top) / normalized(denominator, d_top);
        value = {numeric_ops::numeric_down(std::ldexp(value.low, exponent)),
                 numeric_ops::numeric_up(std::ldexp(value.high, exponent))};
        if (negative)
          value = -value;
        if (auto result = certified(value, true))
          return *result;
      }
    }
    ratio.numerator = numerator;
    ratio.denominator = denominator;
    ratio.negative = negative;
    return take(ratio.round(narrow, work, Words == 12 ? -149 : -1074));
  }
};
}  // namespace ps::plugin_internal::mask
