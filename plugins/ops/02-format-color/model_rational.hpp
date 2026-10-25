#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
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
// Arithmetic records the first operational failure and stops consuming work.
// Results are usable only while active(); reset starts a new evaluation.
template <unsigned Words = 256>
class RationalMathStorage final {
 public:
  static constexpr unsigned kWords = Words;
  static_assert(Words >= 4 && Words % 4 == 0);
  using Integer = numeric_ops::FixedInteger<kWords>;
  using Workspace = numeric_ops::ExactRatioWorkspace<kWords>;
  using Work = core_internal::WorkConsumer;
  struct Classification final {
    enum Kind { FiniteNonzero, Zero, Infinity, NaN };
    Kind kind;
    int numerator_top, denominator_top;
    bool operator==(Kind other) const { return kind == other; }
    bool operator!=(Kind other) const { return kind != other; }
  };
  struct Rational final {
    Integer n, d;
    bool negative = false;
    Rational() { d.words[0] = 1; }
  };

 private:
  Workspace rounding_;
  Work consume_;
  bool meter_classification_ = false;
  std::optional<Classification> operand_class(const Rational& value) const {
    if (!active())
      return {};
    if (meter_classification_)
      return classify(value);
    const int dt = top(value.d), nt = top(value.n);
    return Classification{
        dt >= 0
            ? (nt >= 0 ? Classification::FiniteNonzero : Classification::Zero)
            : (nt >= 0 ? Classification::Infinity : Classification::NaN),
        nt, dt};
  }
  mutable ErrorCode failure_code_ = ErrorCode::Ok;
  mutable FailureReason failure_reason_ = FailureReason::None;
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
  std::optional<int> scan_top(const Integer& value) const {
    if (!active() || (meter_classification_ && !work(kWords)))
      return {};
    return top(value);
  }
  std::optional<unsigned> scan_trailing(const Integer& value) const {
    if (!active() || (meter_classification_ && !work(kWords)))
      return {};
    return trailing(value);
  }
  int compare(const Integer& a, const Integer& b) const {
    const auto at = scan_top(a);
    if (!at)
      return 0;
    const auto bt = scan_top(b);
    if (!bt)
      return 0;
    const int highest = std::max(*at, *bt);
    const unsigned count = static_cast<unsigned>((highest + 64) / 64);
    if (!work(count + 1))
      return 0;
    for (unsigned j = count; j; --j)
      if (a.words[j - 1] != b.words[j - 1])
        return a.words[j - 1] < b.words[j - 1] ? -1 : 1;
    return 0;
  }
  void right(Integer* v, unsigned bits) const {
    if (!work(kWords))
      return;
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
    Integer result;
    if (!active())
      return result;
    if (meter_classification_) {
      try {
        if (!Workspace::shift_bounded(v, bits, &result, consume_))
          capacity();
      } catch (const Status& failure) {
        fail(failure.code, failure.reason);
      }
    } else {
      if (!work(kWords))
        return result;
      if (!Workspace::shift(v, bits, &result))
        capacity();
    }
    return result;
  }
  Integer product(const Integer& a, const Integer& b) const {
    Integer result;
    if (!active())
      return result;
    const auto status = numeric_ops::multiply_fixed(a, b, &result, consume_, 0,
                                                    meter_classification_);
    if (!status.ok())
      fail(status.code, status.reason);
    return result;
  }
  Integer sum(Integer a, const Integer& b) const {
    const auto at = scan_top(a);
    if (!at)
      return {};
    const auto bt = scan_top(b);
    if (!bt)
      return {};
    if (!work(kWords))
      return {};
    if (std::max(*at, *bt) + 1 >= static_cast<int>(kWords * 64)) {
      capacity();
      return {};
    }
    a.add(b);
    return a;
  }
  Integer difference(Integer a, const Integer& b) const {
    if (!work(kWords))
      return {};
    a.subtract(b);
    return a;
  }
  std::pair<Integer, Integer> divmod(const Integer& a, const Integer& b) const {
    const auto bt = scan_top(b);
    if (!bt)
      return {};
    if (*bt < 0) {
      capacity();
      return {};
    }
    Integer quotient, remainder = a;
    if (b.words[0] == 1 && *bt == 0)
      return {a, quotient};
    const auto at = scan_top(a);
    if (!at)
      return {};
    const int bits = *at - *bt;
    if (bits < 0)
      return {quotient, remainder};
    auto shifted = left(b, static_cast<unsigned>(bits));
    const auto words = static_cast<unsigned>((*at + 64) / 64);
    for (int bit = bits; bit >= 0; --bit) {
      if (!work((meter_classification_ ? words : 3 * words) + 1))
        return {};
      int order = 0;
      for (unsigned j = words; j; --j)
        if (remainder.words[j - 1] != shifted.words[j - 1]) {
          order = remainder.words[j - 1] < shifted.words[j - 1] ? -1 : 1;
          break;
        }
      if (order >= 0) {
        if (meter_classification_ && !work(words))
          return {};
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
      if (meter_classification_ && !work(words))
        return {};
      for (unsigned j = 0; j < words; ++j)
        shifted.words[j] = (shifted.words[j] >> 1) |
                           (j + 1 < words ? shifted.words[j + 1] << 63 : 0);
    }
    return {quotient, remainder};
  }
  // GCD-local active lengths avoid scanning/copying all 256 limbs at every
  // binary-Euclid step. Limbs at or above each length stay zero throughout.
  void right_active(Integer* value, unsigned bits, unsigned* count) const {
    if (!work(kWords))
      return;
    if (!bits)
      return;
    const unsigned whole = bits / 64, tail = bits % 64;
    if (whole >= *count) {
      value->words.fill(0);
      *count = 0;
      return;
    }
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
    if (!work(kWords))
      return;
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
    const auto at = scan_top(a);
    if (!at)
      return {};
    const auto bt = scan_top(b);
    if (!bt)
      return {};
    unsigned a_count = static_cast<unsigned>((*at + 64) / 64);
    unsigned b_count = static_cast<unsigned>((*bt + 64) / 64);
    if (!a_count)
      return b;
    if (!b_count)
      return a;
    const auto az = scan_trailing(a);
    if (!az)
      return {};
    const auto bz = scan_trailing(b);
    if (!bz)
      return {};
    const unsigned a_zeros = *az, b_zeros = *bz;
    const unsigned common = std::min(a_zeros, b_zeros);
    // A power-of-two operand makes the GCD a power of two immediately.
    // Dyadic arithmetic must not walk every bit of an odd numerator.
    if (*at == static_cast<int>(a_zeros) || *bt == static_cast<int>(b_zeros)) {
      Integer result;
      result.words[common / 64] = UINT64_C(1) << (common % 64);
      if (!work(1))
        return {};
      return result;
    }
    if (a_count == 1 && b_count == 1) {
      auto x = a.words[0], y = b.words[0];
      while (y) {
        if (!work(1))
          return {};
        const auto remainder = x % y;
        x = y;
        y = remainder;
      }
      Integer result;
      result.words[0] = x;
      return result;
    }
    right_active(&a, a_zeros, &a_count);
    right_active(&b, b_zeros, &b_count);
    // Binary Euclid retains bounded progress and a fuel/cancellation check
    // for every iteration. No floating estimate participates in this loop.
    while (true) {
      if (!work(std::max(a_count, b_count) + 1))
        return {};
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
        const auto zeros = scan_trailing(a);
        if (!zeros)
          return {};
        right_active(&a, *zeros, &a_count);
      } else {
        subtract_active(&b, a, &b_count);
        const auto zeros = scan_trailing(b);
        if (!zeros)
          return {};
        right_active(&b, *zeros, &b_count);
      }
    }
  }
  Integer quotient(const Integer& a, const Integer& b) const {
    const auto bt = scan_top(b);
    if (!bt)
      return {};
    if (b.words[0] == 1 && *bt == 0)
      return a;
    const auto zeros = scan_trailing(b);
    if (!zeros)
      return {};
    if (*bt >= 0 && static_cast<unsigned>(*bt) == *zeros) {
      auto result = a;
      right(&result, *zeros);
      return result;
    }
    auto result = divmod(a, b);
    const auto remainder_top = scan_top(result.second);
    if (!remainder_top)
      return {};
    if (*remainder_top >= 0)
      fail(ErrorCode::Internal);
    return std::move(result.first);
  }
  void reduce(Rational* v) const {
    const auto kind = operand_class(*v);
    if (!kind || *kind == Classification::Infinity ||
        *kind == Classification::NaN)
      return;
    if (*kind == Classification::Zero) {
      if (meter_classification_ && !work(kWords))
        return;
      v->d.words.fill(0);
      v->d.words[0] = 1;
      return;
    }
    if (v->d.words[0] == 1 && kind->denominator_top == 0)
      return;
    if (meter_classification_ && !work(kWords))
      return;
    const unsigned denominator_zeros = trailing(v->d);
    if (static_cast<unsigned>(kind->denominator_top) == denominator_zeros) {
      if (meter_classification_ && !work(kWords))
        return;
      const unsigned bits = std::min(trailing(v->n), denominator_zeros);
      right(&v->n, bits);
      right(&v->d, bits);
      return;
    }
    const auto common = gcd(v->n, v->d);
    v->n = quotient(v->n, common);
    v->d = quotient(v->d, common);
  }
  std::optional<Integer> integer_cube_root(const Integer& value) const {
    if (!active())
      return {};
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
      if (!work(1))
        return {};
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
  explicit RationalMathStorage(numeric_ops::SequenceProfile profile)
      : rounding_(profile) {}
  void bind(Work work) { consume_ = std::move(work); }
  // Explicit instance opt-in leaves legacy consumers' work units unchanged.
  void meter_classification(bool enabled) { meter_classification_ = enabled; }
  // Each classification scan is precharged separately. A denied scan is not a
  // numerical classification and must not be used as a Boolean predicate.
  std::optional<Classification> classify(const Rational& value) const {
    if (!work(kWords))
      return {};
    const int dt = top(value.d);
    if (!work(kWords))
      return {};
    const int nt = top(value.n);
    return Classification{
        dt >= 0
            ? (nt >= 0 ? Classification::FiniteNonzero : Classification::Zero)
            : (nt >= 0 ? Classification::Infinity : Classification::NaN),
        nt, dt};
  }
  // Uses this instance's explicit metering mode and returns reusable tops.
  std::optional<Classification> classification(const Rational& value) const {
    return operand_class(value);
  }
  void reset() {
    failure_code_ = ErrorCode::Ok;
    failure_reason_ = FailureReason::None;
  }
  bool active() const { return failure_code_ == ErrorCode::Ok; }
  Status status() const { return {failure_code_, {}, failure_reason_}; }
  void fail(ErrorCode code, FailureReason reason = FailureReason::None) const {
    if (active()) {
      failure_code_ = code;
      failure_reason_ = reason;
    }
  }
  bool work(std::uint64_t amount) const {
    if (!active())
      return false;
    const auto& status = consume_(amount);
    if (!status.ok())
      fail(status.code, status.reason);
    return active();
  }
  void capacity() const {
    fail(ErrorCode::ResourceExhausted, FailureReason::CapacityLimit);
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
    if (!active())
      return {};
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
      if (!work(1))
        return {};
      if (text[offset] == '.') {
        fractional = true;
        continue;
      }
      if (text[offset] < '0' || text[offset] > '9') {
        fail(ErrorCode::Internal);
        return {};
      }
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
    const auto ak = operand_class(a);
    if (!ak)
      return {};
    const auto bk = operand_class(b);
    if (!bk)
      return {};
    if (*ak == Classification::NaN || *bk == Classification::NaN)
      return invalid();
    if (*ak == Classification::Infinity || *bk == Classification::Infinity) {
      if (*ak == Classification::Infinity && *bk == Classification::Infinity &&
          a.negative != b.negative)
        return invalid();
      return *ak == Classification::Infinity ? a : b;
    }
    if (*ak == Classification::Zero && *bk == Classification::Zero) {
      auto result = integer(0);
      result.negative = a.negative && b.negative;
      return result;
    }
    if (*ak == Classification::Zero)
      return b;
    if (*bk == Classification::Zero)
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
    const auto ak = operand_class(a);
    if (!ak)
      return {};
    const auto bk = operand_class(b);
    if (!bk)
      return {};
    if (*ak == Classification::NaN || *bk == Classification::NaN ||
        (*ak == Classification::Zero && *bk == Classification::Infinity) ||
        (*bk == Classification::Zero && *ak == Classification::Infinity))
      return invalid();
    if (*ak == Classification::Infinity || *bk == Classification::Infinity)
      return inf(a.negative != b.negative);
    Rational result;
    result.negative = a.negative != b.negative;
    if (*ak == Classification::Zero || *bk == Classification::Zero)
      return result;
    const auto g1 = gcd(a.n, b.d), g2 = gcd(b.n, a.d);
    result.n = product(quotient(a.n, g1), quotient(b.n, g2));
    result.d = product(quotient(a.d, g2), quotient(b.d, g1));
    return result;
  }
  Rational divide(const Rational& a, const Rational& b) const {
    const auto ak = operand_class(a);
    if (!ak)
      return {};
    const auto bk = operand_class(b);
    if (!bk)
      return {};
    if (*ak == Classification::NaN || *bk == Classification::NaN ||
        (*ak == Classification::Zero && *bk == Classification::Zero) ||
        (*ak == Classification::Infinity && *bk == Classification::Infinity))
      return invalid();
    if (*bk == Classification::Zero || *ak == Classification::Infinity)
      return inf(a.negative != b.negative);
    if (*bk == Classification::Infinity) {
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
    const auto ak = operand_class(a);
    if (!ak)
      return 0;
    const auto bk = operand_class(b);
    if (!bk)
      return 0;
    if (a.negative != b.negative &&
        !(*ak == Classification::Zero && *bk == Classification::Zero))
      return a.negative ? -1 : 1;
    if (*ak == Classification::Infinity || *bk == Classification::Infinity) {
      const int result =
          (*ak == Classification::Infinity) == (*bk == Classification::Infinity)
              ? 0
          : *ak == Classification::Infinity ? 1
                                            : -1;
      return a.negative ? -result : result;
    }
    const auto common = gcd(a.d, b.d);
    const int result = compare(product(a.n, quotient(b.d, common)),
                               product(b.n, quotient(a.d, common)));
    return a.negative ? -result : result;
  }
  Rational modulo(const Rational& a, unsigned period) const {
    const auto kind = operand_class(a);
    if (!kind)
      return {};
    if (*kind == Classification::Infinity || *kind == Classification::NaN)
      return invalid();
    const auto divisor = product(a.d, integer(period).n);
    auto remainder = divmod(a.n, divisor).second;
    if (a.negative) {
      const auto highest = scan_top(remainder);
      if (!highest)
        return {};
      if (*highest >= 0)
        remainder = difference(divisor, remainder);
    }
    Rational result;
    result.n = std::move(remainder);
    result.d = a.d;
    reduce(&result);
    return result;
  }
  unsigned small_floor(const Rational& a) const {
    const auto kind = operand_class(a);
    if (!kind)
      return 0;
    if (*kind == Classification::Infinity || *kind == Classification::NaN ||
        a.negative) {
      capacity();
      return 0;
    }
    const auto result = divmod(a.n, a.d).first;
    const auto highest = scan_top(result);
    if (!highest)
      return 0;
    if (*highest > 31) {
      capacity();
      return 0;
    }
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
  std::uint64_t rounded(const Rational& a, bool narrow,
                        bool preserve_zero = true) {
    const auto kind = operand_class(a);
    if (!kind)
      return 0;
    const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
    if (*kind == Classification::NaN)
      return narrow ? UINT64_C(0x7fc00000) : UINT64_C(0x7ff8000000000000);
    if (*kind == Classification::Infinity)
      return (narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000)) |
             (a.negative ? sign : 0);
    if (*kind == Classification::Zero)
      return preserve_zero && a.negative ? sign : 0;
    if (meter_classification_ && !work(kWords))
      return 0;
    rounding_.numerator = a.n;
    if (meter_classification_ && !work(kWords))
      return 0;
    rounding_.denominator = a.d;
    rounding_.negative = a.negative;
    auto result = [&]() -> Result<std::uint64_t> {
      if (!meter_classification_)
        return rounding_.round(narrow, consume_, 0);
      try {
        return rounding_.round(narrow, consume_, 0, false, true);
      } catch (const Status& failure) {
        return Result<std::uint64_t>(failure);
      }
    }();
    if (!result.ok()) {
      fail(result.status().code, result.status().reason);
      return 0;
    }
    return result.value();
  }
  std::uint64_t rounded_sqrt(const Rational& a, bool narrow) {
    const auto kind = operand_class(a);
    if (!kind)
      return 0;
    if (*kind == Classification::NaN ||
        (a.negative && *kind != Classification::Zero))
      return narrow ? UINT64_C(0x7fc00000) : UINT64_C(0x7ff8000000000000);
    if (*kind == Classification::Infinity)
      return narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    if (*kind == Classification::Zero)
      return 0;
    if (meter_classification_ && !work(kWords))
      return 0;
    rounding_.numerator = a.n;
    if (meter_classification_ && !work(kWords))
      return 0;
    rounding_.denominator = a.d;
    rounding_.negative = false;
    auto result = [&]() -> Result<std::uint64_t> {
      if (!meter_classification_)
        return numeric_ops::round_sqrt_ratio(&rounding_, narrow, 0, consume_);
      try {
        return numeric_ops::round_sqrt_ratio(&rounding_, narrow, 0, consume_,
                                             true);
      } catch (const Status& failure) {
        return Result<std::uint64_t>(failure);
      }
    }();
    if (!result.ok()) {
      fail(result.status().code, result.status().reason);
      return 0;
    }
    return result.value();
  }
  // Import a normalized positive ratio and return its separate binary scale.
  // Normalization prevents tiny finite inputs from disappearing at low working
  // precision and prevents large rational limbs from overflowing interval math.
  int normalized(numeric_ops::CheckedDirectedInterval& math,
                 numeric_ops::CheckedDirectedInterval::Interval out,
                 const Rational& a,
                 const Classification* known = nullptr) const {
    if (!active())
      return 0;
    static_assert(kWords >= numeric_ops::CheckedDirectedInterval::kWords);
    using Frame = numeric_ops::CheckedDirectedInterval::Frame;
    Frame frame(math);
    auto n = math.interval(), d = math.interval();
    const auto import = [&](auto output, const Integer& value, int highest) {
      if (meter_classification_ && !work(kWords))
        return;
      auto shifted = value;
      const int bits = static_cast<int>(math.precision) - highest;
      bool discarded = false;
      if (bits < 0) {
        const auto zeros = scan_trailing(value);
        if (!zeros)
          return;
        discarded = *zeros < static_cast<unsigned>(-bits);
        right(&shifted, static_cast<unsigned>(-bits));
      } else {
        shifted = left(value, static_cast<unsigned>(bits));
      }
      if (!active())
        return;
      math.clear(output.low);
      if (meter_classification_ &&
          !work(kWords - numeric_ops::CheckedDirectedInterval::kWords))
        return;
      for (unsigned j = numeric_ops::CheckedDirectedInterval::kWords;
           j < kWords; ++j)
        if (shifted.words[j]) {
          capacity();
          return;
        }
      if (meter_classification_ &&
          !work(numeric_ops::CheckedDirectedInterval::kWords))
        return;
      std::copy_n(shifted.words.begin(),
                  numeric_ops::CheckedDirectedInterval::kWords,
                  output.low.magnitude.words.begin());
      math.copy(output.high, output.low);
      if (discarded)
        math.increment(output.high.magnitude);
    };
    const auto kind =
        known ? std::optional<Classification>(*known) : operand_class(a);
    if (!kind)
      return 0;
    const int nt = kind->numerator_top, dt = kind->denominator_top;
    if (nt < 0 || dt < 0) {
      capacity();
      return 0;
    }
    import(n, a.n, nt);
    if (!active())
      return 0;
    import(d, a.d, dt);
    if (!active())
      return 0;
    math.divide(out, n, d);
    return nt - dt;
  }
  void enclose(numeric_ops::CheckedDirectedInterval& math,
               numeric_ops::CheckedDirectedInterval::Interval out,
               const Rational& a) const {
    const auto kind = operand_class(a);
    if (!kind)
      return;
    if (*kind == Classification::Infinity || *kind == Classification::NaN) {
      math.retry();
      return;
    }
    if (*kind == Classification::Zero) {
      math.integer(out, 0);
      return;
    }
    const int exponent = normalized(math, out, a, &*kind);
    if (!active())
      return;
    math.scale(out, out, exponent);
    if (a.negative)
      math.negate(out, out);
  }
};
using RationalMath = RationalMathStorage<>;
}  // namespace ps::plugin_internal::model_ops
