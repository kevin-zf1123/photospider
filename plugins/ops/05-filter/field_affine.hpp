#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

#include "05-filter/arithmetic.hpp"

namespace ps::plugin_internal::filter {
// Exact fixed-width fallback for RN32(a + gain*b), used when the RN64 FMA
// enclosure straddles a binary32 midpoint. Exponent spans that do not fit the
// admitted 128-bit expression retain the general rational path.
inline std::optional<std::uint64_t> affine32(Atom a, Atom b, double gain) {
  using U = unsigned __int128;
  if (!a.narrow || !b.narrow)
    return {};
  struct Number {
    U mantissa;
    int exponent;
    bool negative;
  };
  const auto decode = [](std::uint64_t raw, bool narrow) {
    const unsigned fraction = narrow ? 23 : 52, ebits = narrow ? 8 : 11;
    const auto e = (raw >> fraction) & ((UINT64_C(1) << ebits) - 1);
    const auto m = raw & ((UINT64_C(1) << fraction) - 1);
    return Number{U(m | (e ? UINT64_C(1) << fraction : 0)),
                  static_cast<int>(e ? e : 1) - (narrow ? 127 : 1023) -
                      static_cast<int>(fraction),
                  (raw >> (fraction + ebits)) != 0};
  };
  const auto bit_count = [](U n) {
    if (const auto hi = static_cast<std::uint64_t>(n >> 64))
      return 128 - __builtin_clzll(hi);
    const auto lo = static_cast<std::uint64_t>(n);
    return lo ? 64 - __builtin_clzll(lo) : 0;
  };
  auto aa = decode(a.raw, true);
  const auto bb = decode(b.raw, true), gg = decode(bits(gain), false);
  Number product{bb.mantissa * gg.mantissa, bb.exponent + gg.exponent,
                 bb.negative != gg.negative};
  if (!aa.mantissa)
    aa.exponent = product.exponent;
  if (!product.mantissa)
    product.exponent = aa.exponent;
  const auto exponent = std::min(aa.exponent, product.exponent);
  const auto as = aa.exponent - exponent, ps = product.exponent - exponent;
  if (as >= 127 || ps >= 127 || bit_count(aa.mantissa) + as > 126 ||
      bit_count(product.mantissa) + ps > 126)
    return {};
  const auto av = aa.mantissa << as, pv = product.mantissa << ps;
  const bool negative = aa.negative == product.negative ? aa.negative
                        : av >= pv                      ? aa.negative
                                                        : product.negative;
  const U n = aa.negative == product.negative ? av + pv
              : av >= pv                      ? av - pv
                                              : pv - av;
  if (!n)
    return {};
  const auto sign = negative ? UINT64_C(0x80000000) : 0;
  const int top = bit_count(n) - 1;
  int e = top + exponent;
  if (e > 127)
    return sign | UINT64_C(0x7f800000);
  const auto shift = std::max(e, -126) - 23 - exponent;
  U significand = 0;
  if (shift <= 0) {
    significand = n << -shift;
  } else if (shift < 128) {
    significand = n >> shift;
    const auto remainder = n & ((U(1) << shift) - 1);
    const auto half = U(1) << (shift - 1);
    if (remainder > half || (remainder == half && (significand & 1)))
      ++significand;
  }
  if (!significand)
    return sign;
  if (e < -126) {
    if (significand < (U(1) << 23))
      return sign | static_cast<std::uint64_t>(significand);
    e = -126;
  }
  if (significand >= (U(1) << 24)) {
    significand >>= 1;
    ++e;
  }
  if (e > 127)
    return sign | UINT64_C(0x7f800000);
  return sign | (static_cast<std::uint64_t>(e + 127) << 23) |
         (static_cast<std::uint64_t>(significand) & UINT64_C(0x7fffff));
}
}  // namespace ps::plugin_internal::filter
