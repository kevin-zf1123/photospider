#pragma once

#include <algorithm>
#include <cstdint>

#include "04-mask-morphology/common.hpp"

namespace ps::plugin_internal::mask {
using WideUnsigned = unsigned __int128;
constexpr WideUnsigned kGaussianOne = WideUnsigned(1) << 120;
struct GaussianBound final {
  WideUnsigned low = 0, high = 0;
};
inline WideUnsigned gaussian_product(WideUnsigned a, WideUnsigned b,
                                     bool upper) {
  const auto al = static_cast<std::uint64_t>(a),
             ah = static_cast<std::uint64_t>(a >> 64);
  const auto bl = static_cast<std::uint64_t>(b),
             bh = static_cast<std::uint64_t>(b >> 64);
  const auto p0 = WideUnsigned(al) * bl, p1 = WideUnsigned(al) * bh;
  const auto p2 = WideUnsigned(ah) * bl, p3 = WideUnsigned(ah) * bh;
  const auto t1 = (p0 >> 64) + static_cast<std::uint64_t>(p1) +
                  static_cast<std::uint64_t>(p2);
  const auto t2 =
      (p1 >> 64) + (p2 >> 64) + static_cast<std::uint64_t>(p3) + (t1 >> 64);
  const auto w1 = static_cast<std::uint64_t>(t1);
  const auto w2 = static_cast<std::uint64_t>(t2);
  const auto w3 = static_cast<std::uint64_t>((p3 >> 64) + (t2 >> 64));
  const auto result =
      (WideUnsigned(w3) << 72) | (WideUnsigned(w2) << 8) | (w1 >> 56);
  const bool tail =
      static_cast<std::uint64_t>(p0) || (w1 & UINT64_C(0x00ffffffffffffff));
  return result + (upper && tail);
}
inline GaussianBound gaussian_multiply(GaussianBound a, GaussianBound b) {
  return {gaussian_product(a.low, b.low, false),
          gaussian_product(a.high, b.high, true)};
}
inline GaussianBound gaussian_sample(double value) {
  const auto p = numeric_ops::BinaryParts::decode(bits(value), false);
  if (!p.significand) {
    return {};
  }
  const int shift = p.exponent + 120;
  if (shift >= 0) {
    const auto v = WideUnsigned(p.significand) << shift;
    return {v, v};
  }
  const unsigned right = static_cast<unsigned>(-shift);
  if (right >= 64) {
    return {0, 1};
  }
  const auto lo = p.significand >> right;
  const auto tail = p.significand & ((UINT64_C(1) << right) - 1);
  return {lo, lo + (tail != 0)};
}
inline std::uint64_t gaussian_round(WideUnsigned value, bool narrow) {
  if (!value) {
    return 0;
  }
  const auto high = static_cast<std::uint64_t>(value >> 64);
  const unsigned top =
      high ? 127U - static_cast<unsigned>(__builtin_clzll(high))
           : 63U - static_cast<unsigned>(
                       __builtin_clzll(static_cast<std::uint64_t>(value)));
  const unsigned digits = narrow ? 24 : 53;
  const unsigned shift = top >= digits ? top - digits + 1 : 0;
  auto significand = value >> shift;
  if (shift) {
    const auto rest = value - (significand << shift);
    const auto half = WideUnsigned(1) << (shift - 1);
    significand += rest > half || (rest == half && (significand & 1));
  }
  return convert(std::ldexp(static_cast<double>(significand),
                            static_cast<int>(shift) - 120),
                 narrow);
}
}  // namespace ps::plugin_internal::mask
