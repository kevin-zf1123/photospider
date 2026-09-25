#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

#include "01-numeric/directed_interval.hpp"
#include "01-numeric/exact_root.hpp"

namespace ps::plugin_internal::model_ops {
// A bounded exact extended-rational field. IEEE inputs are imported as their
// exact binary values. No floating intermediate, including a branch predicate,
// is used here. All arithmetic is caller-owned and every long loop is charged.
// A zero denominator represents infinity (numerator=1) or generated NaN (0).
// Source NaN payload selection is performed before entering this field.
class RationalMath final {
 public:
  static constexpr unsigned kWords = 256;
  using Integer = numeric_ops::FixedInteger<kWords>;
  using Workspace = numeric_ops::ExactRatioWorkspace<kWords>;
  using Work = std::function<Status(std::uint64_t)>;
  struct Rational final {
    Integer n, d;
    bool negative = false;
    Rational() { d.words[0] = 1; }
  };

 private:
  Workspace rounding_;
  const Work* consume_ = nullptr;
  static int top(const Integer& v) { return Workspace::top(v); }
  static unsigned trailing(const Integer& v) {
    for (unsigned i = 0; i < kWords; ++i)
      if (v.words[i])
        return i * 64 + static_cast<unsigned>(__builtin_ctzll(v.words[i]));
    return kWords * 64;
  }
  static bool one(const Integer& v) { return v.words[0] == 1 && top(v) == 0; }
  static bool power_two(const Integer& v) {
    const int highest = top(v);
    return highest >= 0 && static_cast<unsigned>(highest) == trailing(v);
  }
  int compare(const Integer& a, const Integer& b) const {
    const int highest = std::max(top(a), top(b));
    const unsigned count = static_cast<unsigned>((highest + 64) / 64);
    work(count + 1);
    for (unsigned j = count; j; --j)
      if (a.words[j - 1] != b.words[j - 1])
        return a.words[j - 1] < b.words[j - 1] ? -1 : 1;
    return 0;
  }
  void right(Integer* v, unsigned bits) const {
    work(kWords);
    if (!bits)
      return;
    const unsigned whole = bits / 64, tail = bits % 64;
    for (unsigned i = 0; i < kWords; ++i) {
      const auto j = i + whole;
      v->words[i] = j >= kWords ? 0 : v->words[j] >> tail;
      if (tail && j + 1 < kWords)
        v->words[i] |= v->words[j + 1] << (64 - tail);
    }
  }
  Integer left(const Integer& v, unsigned bits) const {
    work(kWords);
    Integer result;
    if (!Workspace::shift(v, bits, &result))
      capacity();
    return result;
  }
  Integer product(const Integer& a, const Integer& b) const {
    Integer result;
    const auto status = numeric_ops::multiply_fixed(a, b, &result, *consume_);
    if (!status.ok())
      throw status;
    return result;
  }
  Integer sum(Integer a, const Integer& b) const {
    work(kWords);
    if (std::max(top(a), top(b)) + 1 >= static_cast<int>(kWords * 64))
      capacity();
    a.add(b);
    return a;
  }
  Integer difference(Integer a, const Integer& b) const {
    work(kWords);
    a.subtract(b);
    return a;
  }
  std::pair<Integer, Integer> divmod(const Integer& a, const Integer& b) const {
    if (top(b) < 0)
      capacity();
    Integer quotient, remainder = a;
    if (one(b))
      return {a, quotient};
    const int bits = top(a) - top(b);
    if (bits < 0)
      return {quotient, remainder};
    auto shifted = left(b, static_cast<unsigned>(bits));
    const auto words = static_cast<unsigned>((top(a) + 64) / 64);
    for (int bit = bits; bit >= 0; --bit) {
      work(3 * words + 1);
      int order = 0;
      for (unsigned j = words; j; --j)
        if (remainder.words[j - 1] != shifted.words[j - 1]) {
          order = remainder.words[j - 1] < shifted.words[j - 1] ? -1 : 1;
          break;
        }
      if (order >= 0) {
        std::uint64_t borrow = 0;
        for (unsigned j = 0; j < words; ++j) {
          const auto subtract =
              static_cast<unsigned __int128>(shifted.words[j]) + borrow;
          const auto old = remainder.words[j];
          remainder.words[j] = static_cast<std::uint64_t>(
              static_cast<unsigned __int128>(old) - subtract);
          borrow = static_cast<unsigned __int128>(old) < subtract;
        }
        quotient.words[static_cast<unsigned>(bit) / 64] |= UINT64_C(1)
                                                           << (bit % 64);
      }
      for (unsigned j = 0; j < words; ++j)
        shifted.words[j] = (shifted.words[j] >> 1) |
                           (j + 1 < words ? shifted.words[j + 1] << 63 : 0);
    }
    return {quotient, remainder};
  }
  // GCD-local active lengths avoid scanning/copying all 256 limbs at every
  // binary-Euclid step. Limbs at or above each length stay zero throughout.
  void right_active(Integer* value, unsigned bits, unsigned* count) const {
    work(kWords);  // Retain the former conservative proof-fuel accounting.
    if (!bits)
      return;
    const unsigned whole = bits / 64, tail = bits % 64;
    const unsigned remaining = *count - whole;
    for (unsigned i = 0; i < remaining; ++i) {
      const unsigned j = i + whole;
      value->words[i] = value->words[j] >> tail;
      if (tail && j + 1 < *count)
        value->words[i] |= value->words[j + 1] << (64 - tail);
    }
    std::fill(value->words.begin() + remaining, value->words.begin() + *count,
              0);
    *count = remaining;
    while (*count && !value->words[*count - 1])
      --*count;
  }
  void subtract_active(Integer* a, const Integer& b, unsigned* count) const {
    work(kWords);
    std::uint64_t borrow = 0;
    for (unsigned i = 0; i < *count; ++i) {
      const auto sub = static_cast<unsigned __int128>(b.words[i]) + borrow;
      const auto old = a->words[i];
      a->words[i] =
          static_cast<std::uint64_t>(static_cast<unsigned __int128>(old) - sub);
      borrow = static_cast<unsigned __int128>(old) < sub;
    }
    while (*count && !a->words[*count - 1])
      --*count;
  }
  Integer gcd(Integer a, Integer b) const {
    unsigned a_count = static_cast<unsigned>((top(a) + 64) / 64);
    unsigned b_count = static_cast<unsigned>((top(b) + 64) / 64);
    if (!a_count)
      return b;
    if (!b_count)
      return a;
    const unsigned a_zeros = trailing(a), b_zeros = trailing(b);
    const unsigned common = std::min(a_zeros, b_zeros);
    right_active(&a, a_zeros, &a_count);
    right_active(&b, b_zeros, &b_count);
    // Binary Euclid retains bounded progress and a fuel/cancellation check
    // for every iteration. No floating estimate participates in this loop.
    while (true) {
      work(std::max(a_count, b_count) + 1);
      int order = a_count < b_count ? -1 : a_count > b_count ? 1 : 0;
      if (!order) {
        for (unsigned j = a_count; j; --j)
          if (a.words[j - 1] != b.words[j - 1]) {
            order = a.words[j - 1] < b.words[j - 1] ? -1 : 1;
            break;
          }
      }
      if (!order)
        return left(a, common);
      if (order > 0) {
        subtract_active(&a, b, &a_count);
        right_active(&a, trailing(a), &a_count);
      } else {
        subtract_active(&b, a, &b_count);
        right_active(&b, trailing(b), &b_count);
      }
    }
  }
  Integer quotient(const Integer& a, const Integer& b) const {
    if (one(b))
      return a;
    if (power_two(b)) {
      auto result = a;
      right(&result, trailing(b));
      return result;
    }
    auto result = divmod(a, b);
    if (top(result.second) >= 0)
      throw Status{ErrorCode::Internal, "FMT-11 non-exact internal division"};
    return std::move(result.first);
  }
  void reduce(Rational* v) const {
    if (!finite(*v))
      return;
    if (zero(*v)) {
      v->d.words.fill(0);
      v->d.words[0] = 1;
      return;
    }
    if (one(v->d))
      return;
    if (power_two(v->d)) {
      const unsigned bits = std::min(trailing(v->n), trailing(v->d));
      right(&v->n, bits);
      right(&v->d, bits);
      return;
    }
    const auto common = gcd(v->n, v->d);
    v->n = quotient(v->n, common);
    v->d = quotient(v->d, common);
  }
  std::optional<Integer> integer_cube_root(const Integer& value) const {
    if (top(value) < 0)
      return Integer{};
    const auto zeros = trailing(value);
    if (zeros % 3)
      return {};
    auto odd = value;
    right(&odd, zeros);
    // Cubes modulo nine are 0, 1 or 8. This cheap exact test avoids most
    // bit-by-bit perfect-root probes for ordinary nonalgebraic inputs.
    unsigned residue = 0;
    for (unsigned j = static_cast<unsigned>((top(odd) + 64) / 64); j; --j)
      residue = static_cast<unsigned>(
          ((static_cast<unsigned __int128>(residue) << 64) + odd.words[j - 1]) %
          9);
    if (residue != 0 && residue != 1 && residue != 8)
      return {};
    Integer root;
    for (int bit = top(odd) / 3; bit >= 0; --bit) {
      work(1);
      auto candidate = root;
      candidate.words[static_cast<unsigned>(bit) / 64] |= UINT64_C(1)
                                                          << (bit % 64);
      const auto square = product(candidate, candidate);
      const auto cube = product(square, candidate);
      if (compare(cube, odd) <= 0)
        root = std::move(candidate);
    }
    if (compare(product(product(root, root), root), odd))
      return {};
    return left(root, zeros / 3);
  }

 public:
  explicit RationalMath(numeric_ops::SequenceProfile profile)
      : rounding_(profile) {}
  void bind(const Work& work) { consume_ = &work; }
  void work(std::uint64_t amount) const {
    const auto status = (*consume_)(amount);
    if (!status.ok())
      throw status;
  }
  [[noreturn]] static void capacity() {
    throw Status{ErrorCode::ResourceExhausted, "FMT-11 exact rational capacity",
                 FailureReason::CapacityLimit};
  }
  static bool finite(const Rational& a) { return top(a.d) >= 0; }
  static bool zero(const Rational& a) { return finite(a) && top(a.n) < 0; }
  static bool nan(const Rational& a) { return !finite(a) && top(a.n) < 0; }
  static bool infinity(const Rational& a) {
    return !finite(a) && top(a.n) >= 0;
  }
  Rational integer(std::int64_t value) const {
    Rational result;
    const auto raw = static_cast<std::uint64_t>(value);
    result.n.words[0] = value < 0 ? UINT64_C(0) - raw : raw;
    result.negative = value < 0;
    return result;
  }
  Rational invalid() const {
    auto result = integer(0);
    result.d.words[0] = 0;
    return result;
  }
  Rational inf(bool negative) const {
    auto result = integer(1);
    result.d.words[0] = 0;
    result.negative = negative;
    return result;
  }
  Rational binary(std::uint64_t bits, bool narrow = false) const {
    const auto parts = numeric_ops::BinaryParts::decode(bits, narrow);
    if (parts.nan)
      return invalid();
    if (parts.infinite)
      return inf(parts.negative);
    auto result = integer(0);
    result.negative = parts.negative;
    result.n.words[0] = parts.significand;
    if (parts.exponent >= 0)
      result.n = left(result.n, static_cast<unsigned>(parts.exponent));
    else
      result.d = left(result.d, static_cast<unsigned>(-parts.exponent));
    reduce(&result);
    return result;
  }
  Rational fraction(std::int64_t numerator, std::int64_t denominator) const {
    return divide(integer(numerator), integer(denominator));
  }
  Rational decimal(std::string_view text) const {
    Rational result;
    bool fractional = false;
    std::size_t offset = 0;
    if (!text.empty() && text[0] == '-') {
      result.negative = true;
      ++offset;
    }
    const auto ten = integer(10);
    for (; offset < text.size(); ++offset) {
      work(1);
      if (text[offset] == '.') {
        fractional = true;
        continue;
      }
      if (text[offset] < '0' || text[offset] > '9')
        throw Status{ErrorCode::Internal, "FMT-11 invalid fixed coefficient"};
      result.n = sum(product(result.n, ten.n), integer(text[offset] - '0').n);
      if (fractional)
        result.d = product(result.d, ten.n);
    }
    reduce(&result);
    return result;
  }
  Rational negate(Rational a) const {
    a.negative = !a.negative;
    return a;
  }
  Rational absolute(Rational a) const {
    a.negative = false;
    return a;
  }
  Rational add(const Rational& a, const Rational& b) const {
    if (nan(a) || nan(b))
      return invalid();
    if (infinity(a) || infinity(b)) {
      if (infinity(a) && infinity(b) && a.negative != b.negative)
        return invalid();
      return infinity(a) ? a : b;
    }
    if (zero(a) && zero(b)) {
      auto result = integer(0);
      result.negative = a.negative && b.negative;
      return result;
    }
    if (zero(a))
      return b;
    if (zero(b))
      return a;
    const auto common = gcd(a.d, b.d);
    const auto ad = quotient(a.d, common), bd = quotient(b.d, common);
    const auto an = product(a.n, bd), bn = product(b.n, ad);
    Rational result;
    result.d = product(ad, b.d);
    if (a.negative == b.negative) {
      result.n = sum(an, bn);
      result.negative = a.negative;
    } else {
      const int order = compare(an, bn);
      result.n = order >= 0 ? difference(an, bn) : difference(bn, an);
      result.negative = order > 0 ? a.negative : order < 0 && b.negative;
    }
    reduce(&result);
    return result;
  }
  Rational subtract(const Rational& a, const Rational& b) const {
    return add(a, negate(b));
  }
  Rational multiply(const Rational& a, const Rational& b) const {
    if (nan(a) || nan(b) || (zero(a) && infinity(b)) ||
        (zero(b) && infinity(a)))
      return invalid();
    if (infinity(a) || infinity(b))
      return inf(a.negative != b.negative);
    Rational result;
    result.negative = a.negative != b.negative;
    if (zero(a) || zero(b))
      return result;
    const auto g1 = gcd(a.n, b.d), g2 = gcd(b.n, a.d);
    result.n = product(quotient(a.n, g1), quotient(b.n, g2));
    result.d = product(quotient(a.d, g2), quotient(b.d, g1));
    return result;
  }
  Rational divide(const Rational& a, const Rational& b) const {
    if (nan(a) || nan(b) || (zero(a) && zero(b)) ||
        (infinity(a) && infinity(b)))
      return invalid();
    if (zero(b) || infinity(a))
      return inf(a.negative != b.negative);
    if (infinity(b)) {
      auto result = integer(0);
      result.negative = a.negative != b.negative;
      return result;
    }
    Rational inverse;
    inverse.n = b.d;
    inverse.d = b.n;
    inverse.negative = b.negative;
    return multiply(a, inverse);
  }
  Rational cube(const Rational& a) const { return multiply(multiply(a, a), a); }
  // NaNs are unordered. Every caller handles them before asking for ordering.
  int order(const Rational& a, const Rational& b) const {
    if (a.negative != b.negative && !(zero(a) && zero(b)))
      return a.negative ? -1 : 1;
    if (infinity(a) || infinity(b)) {
      const int result = infinity(a) == infinity(b) ? 0 : infinity(a) ? 1 : -1;
      return a.negative ? -result : result;
    }
    const auto common = gcd(a.d, b.d);
    const int result = compare(product(a.n, quotient(b.d, common)),
                               product(b.n, quotient(a.d, common)));
    return a.negative ? -result : result;
  }
  Rational modulo(const Rational& a, unsigned period) const {
    if (!finite(a))
      return invalid();
    const auto divisor = product(a.d, integer(period).n);
    auto remainder = divmod(a.n, divisor).second;
    if (a.negative && top(remainder) >= 0)
      remainder = difference(divisor, remainder);
    Rational result;
    result.n = std::move(remainder);
    result.d = a.d;
    reduce(&result);
    return result;
  }
  unsigned small_floor(const Rational& a) const {
    if (!finite(a) || a.negative)
      capacity();
    const auto result = divmod(a.n, a.d).first;
    if (top(result) > 31)
      capacity();
    return static_cast<unsigned>(result.words[0]);
  }
  std::optional<Rational> perfect_cube_root(const Rational& a) const {
    if (!finite(a) || zero(a))
      return a;
    const auto n = integer_cube_root(a.n);
    if (!n)
      return {};
    const auto d = integer_cube_root(a.d);
    if (!d)
      return {};
    Rational result;
    result.n = *n;
    result.d = *d;
    result.negative = a.negative;
    return result;
  }
  std::uint64_t rounded(const Rational& a, bool narrow) {
    const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
    if (nan(a))
      return narrow ? UINT64_C(0x7fc00000) : UINT64_C(0x7ff8000000000000);
    if (infinity(a))
      return (narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000)) |
             (a.negative ? sign : 0);
    if (zero(a))
      return a.negative ? sign : 0;
    rounding_.numerator = a.n;
    rounding_.denominator = a.d;
    rounding_.negative = a.negative;
    auto result = rounding_.round(narrow, *consume_, 0);
    if (!result.ok())
      throw result.status();
    return result.value();
  }
  std::uint64_t rounded_sqrt(const Rational& a, bool narrow) {
    if (nan(a) || (a.negative && !zero(a)))
      return rounded(invalid(), narrow);
    if (infinity(a) || zero(a))
      return rounded(absolute(a), narrow);
    rounding_.numerator = a.n;
    rounding_.denominator = a.d;
    rounding_.negative = false;
    auto result =
        numeric_ops::round_sqrt_ratio(&rounding_, narrow, 0, *consume_);
    if (!result.ok())
      throw result.status();
    return result.value();
  }
  // Import a normalized positive ratio and return its separate binary scale.
  // Normalization prevents tiny finite inputs from disappearing at low working
  // precision and prevents large rational limbs from overflowing interval math.
  int normalized(numeric_ops::DirectedInterval& math,
                 numeric_ops::DirectedInterval::Interval out,
                 const Rational& a) const {
    using Frame = numeric_ops::DirectedInterval::Frame;
    Frame frame(math);
    auto n = math.interval(), d = math.interval();
    const auto import = [&](auto output, const Integer& value, int highest) {
      auto shifted = value;
      const int bits = static_cast<int>(math.precision) - highest;
      bool discarded = false;
      if (bits < 0) {
        discarded = trailing(value) < static_cast<unsigned>(-bits);
        right(&shifted, static_cast<unsigned>(-bits));
      } else {
        shifted = left(value, static_cast<unsigned>(bits));
      }
      math.clear(output.low);
      for (unsigned j = numeric_ops::DirectedInterval::kWords; j < kWords; ++j)
        if (shifted.words[j])
          capacity();
      std::copy_n(shifted.words.begin(), numeric_ops::DirectedInterval::kWords,
                  output.low.magnitude.words.begin());
      math.copy(output.high, output.low);
      if (discarded)
        math.increment(output.high.magnitude);
    };
    const int nt = top(a.n), dt = top(a.d);
    if (nt < 0 || dt < 0)
      capacity();
    import(n, a.n, nt);
    import(d, a.d, dt);
    math.divide(out, n, d);
    return nt - dt;
  }
  void enclose(numeric_ops::DirectedInterval& math,
               numeric_ops::DirectedInterval::Interval out,
               const Rational& a) const {
    if (!finite(a))
      throw numeric_ops::DirectedInterval::Unresolved{};
    if (zero(a)) {
      math.integer(out, 0);
      return;
    }
    const int exponent = normalized(math, out, a);
    math.scale(out, out, exponent);
    if (a.negative)
      math.negate(out, out);
  }
};
}  // namespace ps::plugin_internal::model_ops
