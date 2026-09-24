#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/execution/cancellation.hpp"
#include "photospider/execution/resource_allocator.hpp"

namespace ps::data_internal::format_numeric {
struct ExactWorkFailure final {
  Status status;
};
inline thread_local const std::function<Status(std::uint64_t)>*
    exact_work_charge = nullptr;  // NOLINT(whitespace/indent_namespace)
inline thread_local const CancellationToken* exact_work_cancellation = nullptr;
class ExactWorkScope final {
 public:
  ExactWorkScope(const std::function<Status(std::uint64_t)>* charge,
                 const CancellationToken* cancellation) noexcept
      : previous_(exact_work_charge),
        previous_cancellation_(exact_work_cancellation) {
    exact_work_charge = charge;
    exact_work_cancellation = cancellation;
  }
  ~ExactWorkScope() noexcept {
    exact_work_charge = previous_;
    exact_work_cancellation = previous_cancellation_;
  }

 private:
  const std::function<Status(std::uint64_t)>* previous_;
  const CancellationToken* previous_cancellation_;
};
// Exact binary rational arithmetic for one bounded scalar conversion. The
// exponent width of binary64 bounds inputs; intermediate products stay below
// 8192 bits. The caller charges conversion work at the operation boundary.
class Natural final {
 public:
  static constexpr std::size_t maximum_words = 256;
  ResourceVector<std::uint32_t> words;
  static void work(std::uint64_t amount) {
    if (exact_work_cancellation && exact_work_cancellation->cancelled())
      throw ExactWorkFailure{
          Status{ErrorCode::Cancelled, "numeric conversion cancelled"}};
    if (exact_work_charge) {
      auto result = (*exact_work_charge)(amount);
      if (!result.ok())
        throw ExactWorkFailure{result};
      return;
    }
    if (const auto* budget = resource_internal::metadata_budget()) {
      auto result = budget->consume({amount});
      if (!result.ok()) {
        resource_internal::metadata_failure(*budget, result.code);
        throw std::bad_alloc();
      }
    }
  }
  Natural() = default;
  explicit Natural(std::uint64_t n) {
    if (n)
      words.push_back(static_cast<std::uint32_t>(n));
    if (n >> 32)
      words.push_back(static_cast<std::uint32_t>(n >> 32));
  }
  bool zero() const { return words.empty(); }
  void trim() {
    while (!words.empty() && !words.back())
      words.pop_back();
  }
  unsigned bits() const {
    if (zero())
      return 0;
    return static_cast<unsigned>((words.size() - 1) * 32 + 32 -
                                 __builtin_clz(words.back()));
  }
  bool bit(unsigned b) const {
    return b / 32 < words.size() && ((words[b / 32] >> (b % 32)) & 1U);
  }
  unsigned trailing_zeros() const {
    if (zero())
      return 0;
    unsigned count = 0;
    for (const auto word : words) {
      if (word)
        return count + __builtin_ctz(word);
      count += 32;
    }
    return count;
  }
  void shift_right(unsigned amount) {
    if (zero() || !amount)
      return;
    const unsigned whole = amount / 32, bits = amount % 32;
    if (whole >= words.size()) {
      words.clear();
      return;
    }
    for (std::size_t i = 0; i + whole < words.size(); ++i) {
      words[i] = words[i + whole] >> bits;
      if (bits && i + whole + 1 < words.size())
        words[i] |= words[i + whole + 1] << (32 - bits);
    }
    words.resize(words.size() - whole);
    trim();
  }
  int compare(const Natural& b) const {
    if (words.size() != b.words.size())
      return words.size() < b.words.size() ? -1 : 1;
    for (std::size_t i = words.size(); i; --i)
      if (words[i - 1] != b.words[i - 1])
        return words[i - 1] < b.words[i - 1] ? -1 : 1;
    return 0;
  }
  void set_bit(unsigned b) {
    if (b / 32 >= maximum_words)
      throw std::bad_alloc();
    if (words.size() <= b / 32)
      words.resize(b / 32 + 1);
    words[b / 32] |= 1U << (b % 32);
  }
  Natural shift(unsigned b) const {
    if (zero())
      return {};
    if (words.size() + b / 32 + 1 > maximum_words)
      throw std::bad_alloc();
    Natural out;
    out.words.assign(words.size() + b / 32 + 1, 0);
    for (std::size_t i = 0; i < words.size(); ++i) {
      out.words[i + b / 32] |= words[i] << (b % 32);
      if (b % 32)
        out.words[i + b / 32 + 1] |= words[i] >> (32 - b % 32);
    }
    out.trim();
    return out;
  }
  static Natural add(const Natural& a, const Natural& b) {
    if (std::max(a.words.size(), b.words.size()) + 1 > maximum_words)
      throw std::bad_alloc();
    work(std::max(a.words.size(), b.words.size()) + 1);
    Natural out;
    out.words.resize(std::max(a.words.size(), b.words.size()) + 1);
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < out.words.size(); ++i) {
      const std::uint64_t sum = carry + (i < a.words.size() ? a.words[i] : 0) +
                                (i < b.words.size() ? b.words[i] : 0);
      out.words[i] = static_cast<std::uint32_t>(sum);
      carry = sum >> 32;
    }
    out.trim();
    return out;
  }
  static Natural subtract(const Natural& a, const Natural& b) {
    work(a.words.size());
    Natural out;
    out.words.resize(a.words.size());
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < a.words.size(); ++i) {
      const std::uint64_t sub = (i < b.words.size() ? b.words[i] : 0) + borrow;
      out.words[i] = static_cast<std::uint32_t>(
          static_cast<std::uint64_t>(a.words[i]) - sub);
      borrow = a.words[i] < sub;
    }
    out.trim();
    return out;
  }
  static Natural multiply(const Natural& a, const Natural& b) {
    work(std::max<std::size_t>(1, a.words.size() * b.words.size()));
    Natural out;
    if (a.zero() || b.zero())
      return out;
    if (a.words.size() + b.words.size() > maximum_words)
      throw std::bad_alloc();
    out.words.assign(a.words.size() + b.words.size(), 0);
    for (std::size_t i = 0; i < a.words.size(); ++i) {
      std::uint64_t carry = 0;
      for (std::size_t j = 0; j < b.words.size(); ++j) {
        const std::uint64_t product =
            static_cast<std::uint64_t>(a.words[i]) * b.words[j] +
            out.words[i + j] + carry;
        out.words[i + j] = static_cast<std::uint32_t>(product);
        carry = product >> 32;
      }
      out.words[i + b.words.size()] = static_cast<std::uint32_t>(carry);
    }
    out.trim();
    return out;
  }
  static Natural gcd(Natural a, Natural b) {
    if (a.zero())
      return b;
    if (b.zero())
      return a;
    const unsigned common = std::min(a.trailing_zeros(), b.trailing_zeros());
    a.shift_right(a.trailing_zeros());
    do {
      b.shift_right(b.trailing_zeros());
      if (a.compare(b) > 0)
        std::swap(a, b);
      b = subtract(b, a);
    } while (!b.zero());
    return a.shift(common);
  }
  static std::pair<Natural, Natural> divide(const Natural& n,
                                            const Natural& d) {
    if (d.zero())
      throw std::logic_error("zero exact denominator");
    work(std::max<std::size_t>(1, n.bits()) *
         std::max<std::size_t>(1, d.words.size()));
    Natural q, r;
    for (unsigned i = n.bits(); i; --i) {
      r = r.shift(1);
      if (n.bit(i - 1))
        r = add(r, Natural(1));
      if (r.compare(d) >= 0) {
        r = subtract(r, d);
        q.set_bit(i - 1);
      }
    }
    return {std::move(q), std::move(r)};
  }
  std::uint64_t low64() const {
    return (words.empty() ? 0 : words[0]) |
           (words.size() < 2 ? 0 : static_cast<std::uint64_t>(words[1]) << 32);
  }
};

struct Rational final {
  Natural n{0}, d{1};
  bool negative = false;
  bool negative_zero = false;
  static Rational integer(std::int64_t value) {
    Rational r;
    r.negative = value < 0;
    r.n = Natural(r.negative ? static_cast<std::uint64_t>(0) -
                                   static_cast<std::uint64_t>(value)
                             : static_cast<std::uint64_t>(value));
    return r;
  }
  static Rational binary(std::uint64_t bits, bool binary32) {
    Rational r;
    const unsigned fraction = binary32 ? 23 : 52;
    const unsigned exponent_bits = binary32 ? 8 : 11;
    const std::uint64_t mask = (static_cast<std::uint64_t>(1) << fraction) - 1;
    const auto exponent =
        (bits >> fraction) &
        ((static_cast<std::uint64_t>(1) << exponent_bits) - 1);
    r.negative = (bits >> (fraction + exponent_bits)) != 0;
    std::uint64_t mantissa = bits & mask;
    r.negative_zero = r.negative && !exponent && !mantissa;
    if (exponent)
      mantissa |= static_cast<std::uint64_t>(1) << fraction;
    const int shift = static_cast<int>(exponent ? exponent : 1) -
                      (binary32 ? 127 : 1023) - static_cast<int>(fraction);
    r.n = Natural(mantissa);
    if (shift >= 0)
      r.n = r.n.shift(static_cast<unsigned>(shift));
    else
      r.d = r.d.shift(static_cast<unsigned>(-shift));
    return r;
  }
  static Rational add(const Rational& a, const Rational& b) {
    Rational out;
    const auto x = Natural::multiply(a.n, b.d);
    const auto y = Natural::multiply(b.n, a.d);
    out.d = Natural::multiply(a.d, b.d);
    if (a.negative == b.negative) {
      out.n = Natural::add(x, y);
      out.negative = a.negative;
    } else if (x.compare(y) >= 0) {
      out.n = Natural::subtract(x, y);
      out.negative = a.negative;
    } else {
      out.n = Natural::subtract(y, x);
      out.negative = b.negative;
    }
    if (out.n.zero())
      out.negative = false;
    return out;
  }
  static Rational subtract(const Rational& a, Rational b) {
    b.negative = !b.negative;
    return add(a, b);
  }
  static Rational multiply(const Rational& a, const Rational& b) {
    return {Natural::multiply(a.n, b.n), Natural::multiply(a.d, b.d),
            a.negative != b.negative, false};
  }
  static Rational divide(const Rational& a, const Rational& b) {
    return {Natural::multiply(a.n, b.d), Natural::multiply(a.d, b.n),
            a.negative != b.negative, false};
  }
  int compare(const Rational& b) const {
    if (n.zero() && b.n.zero())
      return 0;
    if (n.zero())
      return b.negative ? 1 : -1;
    if (b.n.zero())
      return negative ? -1 : 1;
    if (negative != b.negative && !n.zero() && !b.n.zero())
      return negative ? -1 : 1;
    const int c = Natural::multiply(n, b.d).compare(Natural::multiply(b.n, d));
    return negative ? -c : c;
  }
  Natural rounded_magnitude() const {
    auto qr = Natural::divide(n, d);
    const int relation = Natural::add(qr.second, qr.second).compare(d);
    if (relation > 0 || (relation == 0 && qr.first.bit(0)))
      qr.first = Natural::add(qr.first, Natural(1));
    return qr.first;
  }
  std::uint64_t floating_bits(bool binary32) const {
    const unsigned fraction = binary32 ? 23 : 52;
    const int bias = binary32 ? 127 : 1023;
    const int minimum = 1 - bias;
    const std::uint64_t sign =
        static_cast<std::uint64_t>(negative || (n.zero() && negative_zero))
        << (fraction + (binary32 ? 8 : 11));
    if (n.zero())
      return sign;
    int exponent = static_cast<int>(n.bits()) - static_cast<int>(d.bits());
    const bool high =
        exponent >= 0
            ? n.compare(d.shift(static_cast<unsigned>(exponent))) >= 0
            : n.shift(static_cast<unsigned>(-exponent)).compare(d) >= 0;
    if (!high)
      --exponent;
    const int scale = std::max(exponent, minimum) - static_cast<int>(fraction);
    Rational scaled = *this;
    if (scale >= 0)
      scaled.d = scaled.d.shift(static_cast<unsigned>(scale));
    else
      scaled.n = scaled.n.shift(static_cast<unsigned>(-scale));
    Natural rounded = scaled.rounded_magnitude();
    if (rounded.bits() > fraction + 1) {
      rounded = Natural::divide(rounded, Natural(2)).first;
      ++exponent;
    }
    if (exponent < minimum && rounded.bits() > fraction)
      exponent = minimum;
    if (exponent + bias >= (binary32 ? 255 : 2047))
      return sign |
             (static_cast<std::uint64_t>(binary32 ? 255 : 2047) << fraction);
    if (exponent < minimum && rounded.bits() <= fraction)
      return sign | rounded.low64();
    return sign | (static_cast<std::uint64_t>(exponent + bias) << fraction) |
           (rounded.low64() &
            ((static_cast<std::uint64_t>(1) << fraction) - 1));
  }
};
}  // namespace ps::data_internal::format_numeric
