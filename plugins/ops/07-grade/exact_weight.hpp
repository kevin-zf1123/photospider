// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

namespace ps::plugin_internal::grade::statistics {
/** Nonnegative binary64 weights in [0,1], at most 2^40 samples.
 * Units are 2^-1074: 1115 bits suffice, including a possible 2^40 sum.
 * No payload-sized allocation or extended-precision floating assumption.
 */
struct ExactWeight {
  static constexpr unsigned kWords = 18;
  std::array<std::uint64_t, kWords> words{};
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
  // Both operands are disjoint nonnegative portions of at most 2^40 mass.
  void add(const ExactWeight& other) {
    for (unsigned i = 0; i < other.size; ++i) {
      add_word(i, other.words[i]);
    }
  }
  void add(double x) {
    std::uint64_t b;
    std::memcpy(&b, &x, 8);
    const unsigned e = (b >> 52) & 2047U;
    const auto s =
        (b & ((UINT64_C(1) << 52) - 1)) | (e ? UINT64_C(1) << 52 : 0);
    const unsigned shift = e ? e - 1 : 0, i = shift / 64, r = shift % 64;
    if (!s) {
      return;
    }
    // A <=53-bit significand shifted by <=63 occupies at most116 bits.
    // Valid [0,1] weights have i<=15, so both words are always present. A
    // 128-bit wrap emits exactly one carry into the unchanged higher chain.
    using Wide = unsigned __int128;
    const Wide amount = Wide{s} << r;
    const Wide old = Wide{words[i]} | (Wide{words[i + 1]} << 64);
    const Wide sum = old + amount;
    const auto high = static_cast<std::uint64_t>(sum >> 64);
    words[i] = static_cast<std::uint64_t>(sum);
    words[i + 1] = high;
    if (sum < old) {
      add_word(i + 2, 1);
    } else {
      size = std::max(size, i + 1U + static_cast<unsigned>(high != 0));
    }
  }
  std::uint64_t shifted(unsigned shift) const {
    const unsigned i = shift / 64, r = shift % 64;
    auto v = i < size ? words[i] >> r : 0;
    if (r && i + 1 < size) {
      v |= words[i + 1] << (64 - r);
    }
    return v;
  }
  bool below(unsigned shift) const {
    const unsigned i = shift / 64, r = shift % 64;
    for (unsigned j = 0; j < std::min(i, size); ++j) {
      if (words[j]) {
        return true;
      }
    }
    return r && i < size && (words[i] & ((UINT64_C(1) << r) - 1));
  }
  double rounded() const {
    if (!size) {
      return 0;
    }
    const unsigned top =
        64 * (size - 1) + 63 - __builtin_clzll(words[size - 1]);
    const unsigned shift = top > 52 ? top - 52 : 0;
    auto s = shifted(shift);
    if (shift && (shifted(shift - 1) & 1) && (below(shift - 1) || (s & 1))) {
      ++s;
    }
    return std::ldexp(static_cast<double>(s), static_cast<int>(shift) - 1074);
  }
  int compare(const ExactWeight& other) const {
    if (size != other.size) {
      return size < other.size ? -1 : 1;
    }
    for (unsigned i = size; i-- > 0;) {
      if (words[i] != other.words[i]) {
        return words[i] < other.words[i] ? -1 : 1;
      }
    }
    return 0;
  }
  // floor(p * exact mass), plus whether the discarded fractional part is
  // nonzero. Compare the integer cumulative sum with this threshold without
  // rounding p*W.
  std::pair<ExactWeight, bool> threshold(double p) const {
    std::uint64_t b;
    std::memcpy(&b, &p, 8);
    const unsigned e = (b >> 52) & 2047U;
    const auto s =
        (b & ((UINT64_C(1) << 52) - 1)) | (e ? UINT64_C(1) << 52 : 0);
    const unsigned shift = e ? 1075 - e : 1074;
    std::array<std::uint64_t, kWords + 1> product{};
    std::uint64_t carry = 0;
    for (unsigned i = 0; i < size; ++i) {
      const auto v = static_cast<unsigned __int128>(words[i]) * s + carry;
      product[i] = static_cast<std::uint64_t>(v);
      carry = v >> 64;
    }
    product[size] = carry;
    const unsigned offset = shift / 64, r = shift % 64;
    ExactWeight out;
    for (unsigned j = offset; j < product.size() && j - offset < kWords; ++j) {
      auto v = product[j] >> r;
      if (r && j + 1 < product.size()) {
        v |= product[j + 1] << (64 - r);
      }
      out.words[j - offset] = v;
      if (v) {
        out.size = j - offset + 1;
      }
    }
    bool fraction = false;
    for (unsigned j = 0; j < offset; ++j) {
      fraction |= product[j] != 0;
    }
    if (r) {
      fraction |= (product[offset] & ((UINT64_C(1) << r) - 1)) != 0;
    }
    return {out, fraction};
  }
};
}  // namespace ps::plugin_internal::grade::statistics
