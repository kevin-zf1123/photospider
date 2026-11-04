// Copyright 2026 Photospider Authors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

#include "07-grade/statistical_common.hpp"

namespace ps::plugin_internal::grade::chromatic {
/** Exact nonnegative sum of binary64 squares in units of 2^-2148.
 * Two finite squares need at most 4197 bits; 66 limbs suffice. This bounded
 * fallback is used only near chroma endpoints or in very narrow transitions.
 */
struct SquaredChroma {
  std::array<std::uint64_t, 66> words{};
  unsigned size = 0;
  void add_word(unsigned i, std::uint64_t v) {
    if (!v) {
      return;
    }
    do {
      const auto old = words[i];
      words[i] += v;
      v = words[i] < old;
      ++i;
    } while (v);
    size = std::max(size, i);
  }
  void add_square(double x) {
    std::uint64_t b;
    std::memcpy(&b, &x, 8);
    const unsigned e = (b >> 52) & 2047U;
    const auto s =
        (b & ((UINT64_C(1) << 52) - 1)) | (e ? UINT64_C(1) << 52 : 0);
    const auto square = static_cast<unsigned __int128>(s) * s;
    const unsigned shift = e ? 2 * (e - 1) : 0, i = shift / 64, r = shift % 64;
    const auto low = static_cast<std::uint64_t>(square),
               high = static_cast<std::uint64_t>(square >> 64);
    add_word(i, low << r);
    add_word(i + 1, (high << r) | (r ? low >> (64 - r) : 0));
    if (r) {
      add_word(i + 2, high >> (64 - r));
    }
  }
  int compare(const SquaredChroma& b) const {
    if (size != b.size) {
      return size < b.size ? -1 : 1;
    }
    for (unsigned i = size; i-- > 0;) {
      if (words[i] != b.words[i]) {
        return words[i] < b.words[i] ? -1 : 1;
      }
    }
    return 0;
  }
  SquaredChroma subtract(const SquaredChroma& b) const {
    SquaredChroma out;
    std::uint64_t borrow = 0;
    for (unsigned i = 0; i < size; ++i) {
      const auto x = words[i], y = b.words[i];
      out.words[i] = x - y - borrow;
      borrow = x < y || (borrow && x == y);
      if (out.words[i]) {
        out.size = i + 1;
      }
    }
    return out;
  }
  std::pair<double, int> leading() const {
    const unsigned top =
        64 * (size - 1) + 63 - __builtin_clzll(words[size - 1]);
    const unsigned shift = top > 52 ? top - 52 : 0, i = shift / 64,
                   r = shift % 64;
    auto s = words[i] >> r;
    if (r && i + 1 < size) {
      s |= words[i + 1] << (64 - r);
    }
    return {static_cast<double>(s), static_cast<int>(shift)};
  }
};
[[gnu::noinline, gnu::cold]] inline double precise_neutral_weight(
    double a, double b, double inner, double outer) {
  const double width = outer - inner;
  SquaredChroma value, low, high;
  value.add_square(a);
  value.add_square(b);
  low.add_square(inner);
  high.add_square(outer);
  if (value.compare(low) <= 0) {
    return 1;
  }
  if (value.compare(high) >= 0) {
    return 0;
  }
  const auto numerator = value.subtract(low).leading();
  // Rationalize sqrt(value)-inner. Keep the numerator in integer units and
  // split the positive denominator into significands/exponents, avoiding both
  // cancellation and 0*infinity for arbitrarily separated finite scales.
  int h_exponent, width_exponent;
  std::frexp(std::max(std::abs(a), std::abs(b)), &h_exponent);
  const double h_significand =
      std::hypot(std::ldexp(a, -h_exponent), std::ldexp(b, -h_exponent));
  const double sum_significand = h_significand + std::ldexp(inner, -h_exponent);
  const double width_significand = std::frexp(width, &width_exponent);
  const double t =
      std::ldexp(numerator.first / sum_significand / width_significand,
                 numerator.second - 2148 - h_exponent - width_exponent);
  return 1.0 - statistics::smooth(t);
}
// Keep the ordinary pixel path small; the exact fallback owns its bounded
// integer workspace only when an endpoint/transition actually requires it.
inline double neutral_chroma_weight(double a, double b, double h, double inner,
                                    double outer) {
  const double error = std::max(0x1p-48 * std::max(h, outer), 0x1p-1070);
  if (h < inner - error) {
    return 1.0;
  }
  if (h > outer + error) {
    return 0.0;
  }
  const double width = outer - inner;
  if (width > .001 * outer && h > inner + error && h < outer - error) {
    return 1.0 - statistics::smooth((h - inner) / width);
  }
  return precise_neutral_weight(a, b, inner, outer);
}
}  // namespace ps::plugin_internal::grade::chromatic
