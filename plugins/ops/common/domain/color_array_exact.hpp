#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace ps::color_internal {
// Metadata arithmetic only. Binary64 coordinates become integers in units
// 2^-1074 (<2098 bits). Homogeneous 1-x-y needs <2100 bits; six signed triple
// products need <6303 bits. 8192 bits cover every finite input, including
// near-singular matrices, without tolerance or floating environment changes.
// All storage is bounded stack memory; no sample arithmetic uses this class.
struct Integer final {
  std::array<std::uint32_t, 256> words{};
  unsigned used = 0;
  bool negative = false;

  void trim() {
    while (used && !words[used - 1])
      --used;
    if (!used)
      negative = false;
  }
  static Integer binary(double value) {
    std::uint64_t bits;
    std::memcpy(&bits, &value, 8);
    const unsigned exponent = (bits >> 52) & 2047;
    const std::uint64_t mantissa =
        (bits & 0xfffffffffffffULL) | (exponent ? 1ULL << 52 : 0);
    const unsigned shift = exponent ? exponent - 1 : 0;
    Integer result;
    for (unsigned i = 0; i < 53; ++i)
      if ((mantissa >> i) & 1)
        result.words[(shift + i) / 32] |= 1U << ((shift + i) % 32);
    result.used = (shift + 53 + 31) / 32;
    result.negative = bits >> 63;
    result.trim();
    return result;
  }
};
inline int compare(const Integer& a, const Integer& b) {
  if (a.used != b.used)
    return a.used > b.used ? 1 : -1;
  for (unsigned i = a.used; i; --i)
    if (a.words[i - 1] != b.words[i - 1])
      return a.words[i - 1] > b.words[i - 1] ? 1 : -1;
  return 0;
}
inline Integer add(const Integer& a, const Integer& b, bool subtract = false) {
  Integer out;
  const bool sign = b.negative != subtract;
  if (a.negative == sign) {
    std::uint64_t carry = 0;
    out.used = std::max(a.used, b.used);
    for (unsigned i = 0; i < out.used; ++i) {
      const auto sum =
          static_cast<std::uint64_t>(a.words[i]) + b.words[i] + carry;
      out.words[i] = static_cast<std::uint32_t>(sum);
      carry = sum >> 32;
    }
    if (carry)
      out.words[out.used++] = static_cast<std::uint32_t>(carry);
    out.negative = a.negative;
  } else {
    const bool larger = compare(a, b) >= 0;
    const auto& x = larger ? a : b;
    const auto& y = larger ? b : a;
    std::uint64_t borrow = 0;
    out.used = x.used;
    for (unsigned i = 0; i < out.used; ++i) {
      const auto sub = static_cast<std::uint64_t>(y.words[i]) + borrow;
      out.words[i] = static_cast<std::uint32_t>(x.words[i] - sub);
      borrow = x.words[i] < sub;
    }
    out.negative = larger ? a.negative : sign;
  }
  out.trim();
  return out;
}
inline Integer multiply(const Integer& a, const Integer& b) {
  Integer out;
  out.used = a.used + b.used;
  for (unsigned i = 0; i < a.used; ++i) {
    std::uint64_t carry = 0;
    for (unsigned j = 0; j < b.used; ++j) {
      const auto sum = static_cast<std::uint64_t>(a.words[i]) * b.words[j] +
                       out.words[i + j] + carry;
      out.words[i + j] = static_cast<std::uint32_t>(sum);
      carry = sum >> 32;
    }
    out.words[i + b.used] = static_cast<std::uint32_t>(carry);
  }
  out.negative = a.negative != b.negative;
  out.trim();
  return out;
}
using Column = std::array<Integer, 3>;
inline Column homogeneous(double x, double y) {
  auto a = Integer::binary(x), b = Integer::binary(y);
  return {a, b, add(add(Integer::binary(1), a, true), b, true)};
}
inline bool nonsingular(const std::array<Column, 3>& m) {
  Integer determinant;
  for (unsigned i = 0; i < 3; ++i) {
    auto forward =
        multiply(m[0][i], multiply(m[1][(i + 1) % 3], m[2][(i + 2) % 3]));
    auto backward =
        multiply(m[0][i], multiply(m[1][(i + 2) % 3], m[2][(i + 1) % 3]));
    determinant = add(add(determinant, forward), backward, true);
  }
  return determinant.used != 0;
}
inline bool positive_sum_below_one(double x, double y) {
  auto a = Integer::binary(x), b = Integer::binary(y);
  return a.used && b.used && !a.negative && !b.negative &&
         compare(add(a, b), Integer::binary(1)) < 0;
}
inline bool valid_basis(const std::array<double, 6>& p,
                        const std::array<double, 2>& white) {
  std::array<Column, 3> basis{homogeneous(p[0], p[1]), homogeneous(p[2], p[3]),
                              homogeneous(p[4], p[5])};
  if (!nonsingular(basis))
    return false;
  const auto w = homogeneous(white[0], white[1]);
  // Cramer's numerators must all be nonzero. White's common positive y
  // denominator cannot change nonsingularity; negative scales remain legal.
  for (unsigned i = 0; i < 3; ++i) {
    auto replaced = basis;
    replaced[i] = w;
    if (!nonsingular(replaced))
      return false;
  }
  return true;
}
}  // namespace ps::color_internal
