#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>

#include "data/exact_numeric.hpp"

namespace ps::plugin_internal::basis_ops {
// Per-row bounded automatic scratch, not a cache or an unaccounted heap.
// Only [0, size) is initialized. Copying never reads the unused tail. The
// operation already declares 64 KiB of workspace; a row needs < 12 KiB.
// Static geometry stays in the existing allocator-aware Natural type.
class RowUnsigned final {
 public:
  static constexpr unsigned capacity =
      data_internal::format_numeric::Natural::maximum_words;
  std::array<std::uint32_t, capacity> words;
  unsigned size = 0;

  RowUnsigned() = default;
  explicit RowUnsigned(const data_internal::format_numeric::Natural& n)
      : size(static_cast<unsigned>(n.words.size())) {
    if (size > capacity)
      throw std::bad_alloc();
    std::copy(n.words.begin(), n.words.end(), words.begin());
  }
  RowUnsigned(const RowUnsigned& n) : size(n.size) {
    std::copy_n(n.words.begin(), size, words.begin());
  }
  RowUnsigned& operator=(const RowUnsigned& n) {
    if (this != &n) {
      size = n.size;
      std::copy_n(n.words.begin(), size, words.begin());
    }
    return *this;
  }
  bool zero() const { return size == 0; }
  void trim() {
    while (size && !words[size - 1])
      --size;
  }
  unsigned bits() const {
    return size ? (size - 1) * 32 + 32 - __builtin_clz(words[size - 1]) : 0;
  }
  unsigned trailing_zeros() const {
    for (unsigned i = 0; i < size; ++i)
      if (words[i])
        return i * 32 + __builtin_ctz(words[i]);
    return 0;
  }
  int compare(const RowUnsigned& b) const {
    if (size != b.size)
      return size < b.size ? -1 : 1;
    for (unsigned i = size; i; --i)
      if (words[i - 1] != b.words[i - 1])
        return words[i - 1] < b.words[i - 1] ? -1 : 1;
    return 0;
  }
  void shift_right(unsigned amount) {
    if (!size || !amount)
      return;
    const unsigned whole = amount / 32, bit = amount % 32;
    if (whole >= size) {
      size = 0;
      return;
    }
    for (unsigned i = 0; i + whole < size; ++i) {
      words[i] = words[i + whole] >> bit;
      if (bit && i + whole + 1 < size)
        words[i] |= words[i + whole + 1] << (32 - bit);
    }
    size -= whole;
    trim();
  }
  void shift_left(unsigned amount) {
    if (!size || !amount)
      return;
    const unsigned whole = amount / 32, bit = amount % 32;
    const unsigned end = size + whole + 1;
    if (end > capacity)
      throw std::bad_alloc();
    for (unsigned i = end; i-- > 0;) {
      std::uint32_t word = 0;
      if (i >= whole && i - whole < size)
        word = words[i - whole] << bit;
      if (bit && i > whole && i - whole - 1 < size)
        word |= words[i - whole - 1] >> (32 - bit);
      words[i] = word;
    }
    size = end;
    trim();
  }
  void multiply(std::uint64_t factor) {
    using data_internal::format_numeric::Natural;
    const unsigned factor_words = factor >> 32 ? 2 : factor ? 1 : 0;
    Natural::work(std::max(1U, size * factor_words));
    if (!size || !factor) {
      size = 0;
      return;
    }
    if (size + factor_words > capacity)
      throw std::bad_alloc();
    std::uint64_t carry = 0;
    for (unsigned i = 0; i < size; ++i) {
      const __uint128_t product =
          static_cast<__uint128_t>(words[i]) * factor + carry;
      words[i] = static_cast<std::uint32_t>(product);
      carry = static_cast<std::uint64_t>(product >> 32);
    }
    while (carry) {
      words[size++] = static_cast<std::uint32_t>(carry);
      carry >>= 32;
    }
  }
  void add(const RowUnsigned& b) {
    using data_internal::format_numeric::Natural;
    const unsigned end = std::max(size, b.size);
    if (end + 1 > capacity)
      throw std::bad_alloc();
    Natural::work(end + 1);
    std::uint64_t carry = 0;
    for (unsigned i = 0; i < end; ++i) {
      const std::uint64_t sum =
          carry + (i < size ? words[i] : 0) + (i < b.size ? b.words[i] : 0);
      words[i] = static_cast<std::uint32_t>(sum);
      carry = sum >> 32;
    }
    size = end;
    if (carry)
      words[size++] = static_cast<std::uint32_t>(carry);
  }
  // Caller proves *this >= b. No allocation or vector-resize in division.
  void subtract(const RowUnsigned& b) {
    data_internal::format_numeric::Natural::work(size);
    std::uint64_t borrow = 0;
    for (unsigned i = 0; i < size; ++i) {
      const std::uint64_t sub = (i < b.size ? b.words[i] : 0) + borrow;
      const auto word = words[i];
      words[i] = static_cast<std::uint32_t>(word - sub);
      borrow = word < sub;
    }
    trim();
  }
};
}  // namespace ps::plugin_internal::basis_ops
