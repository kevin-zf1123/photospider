#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <utility>

#include "01-numeric/exact_product.hpp"

namespace ps::plugin_internal::numeric_ops {
// Degree <=3 polynomial arithmetic for exact algebraic boundary decisions.
// Initial coefficients in units 2^-1075 have <2105 bits. Ordinary pseudo-
// remainder chains need fewer than 25280 bits, including unbalanced degrees.
// The 40960-bit arena also admits degree-three dyadic evaluation at <=8192
// fractional t bits. All long storage belongs to the admitted continuation.
class ExactPolynomial final {
 public:
  static constexpr unsigned kWords = 640, kSlots = 96;
  using Index = unsigned;
  using Integer = FixedInteger<kWords>;
  struct Number {
    Integer magnitude;
    bool negative = false;
  };
  struct Polynomial {
    std::array<Index, 4> coefficient{};
    int degree = -1;
  };

 private:
  ExactRatioWorkspace<kWords> ratio_;
  std::array<Number, kSlots> numbers_{};
  unsigned used_ = 0;
  Status status_;
  const std::function<Status(std::uint64_t)>* consume_ = nullptr;
  Index allocate() {
    if (!status_.ok())
      return 0;
    status_ = (*consume_)(kWords);
    if (used_ == kSlots)
      capacity();
    if (!status_.ok())
      return 0;
    numbers_[used_] = {};
    return used_++;
  }
  void capacity() {
    status_ = {ErrorCode::ResourceExhausted, "exact polynomial capacity",
               FailureReason::CapacityLimit};
  }
  void copy_polynomial(const Polynomial& source, Polynomial* destination) {
    destination->degree = source.degree;
    for (unsigned i = 0; i < 4; ++i)
      copy(source.coefficient[i], destination->coefficient[i]);
  }
  Polynomial remainder(const Polynomial& a, const Polynomial& b) {
    auto result = polynomial();
    copy_polynomial(a, &result);
    const auto base = mark();
    while (status_.ok() && result.degree >= b.degree && b.degree >= 0) {
      const auto shift = result.degree - b.degree;
      std::array<Index, 4> next{};
      for (int i = 0; i <= result.degree; ++i) {
        next[i] = multiply(b.coefficient[b.degree], result.coefficient[i]);
        if (i >= shift)
          next[i] = add(next[i],
                        multiply(result.coefficient[result.degree],
                                 b.coefficient[i - shift]),
                        true);
      }
      for (int i = 0; i <= result.degree; ++i)
        copy(next[i], result.coefficient[i]);
      trim(&result);
      restore(base);
    }
    normalize_twos(&result);
    return result;
  }
  void normalize_twos(Polynomial* value) {
    unsigned common = kWords * 64;
    for (int i = 0; i <= value->degree; ++i) {
      const auto& words = numbers_[value->coefficient[i]].magnitude.words;
      for (unsigned j = 0; j < kWords; ++j)
        if (words[j]) {
          common = std::min(common, j * 64 + static_cast<unsigned>(
                                                 __builtin_ctzll(words[j])));
          break;
        }
    }
    if (!common || common == kWords * 64)
      return;
    for (int i = 0; i <= value->degree; ++i) {
      if (status_.ok())
        status_ = (*consume_)(kWords);
      if (!status_.ok())
        return;
      auto& words = numbers_[value->coefficient[i]].magnitude.words;
      const auto whole = common / 64, tail = common % 64;
      for (unsigned j = 0; j < kWords; ++j)
        words[j] = j + whole >= kWords
                       ? 0
                       : (words[j + whole] >> tail) |
                             (tail && j + whole + 1 < kWords
                                  ? words[j + whole + 1] << (64 - tail)
                                  : 0);
    }
  }

 public:
  explicit ExactPolynomial(SequenceProfile profile) : ratio_(profile) {}
  // Borrow work only within one synchronous arithmetic entry. Call end before
  // retiring the phase; no work callback is owned by the immutable program.
  void begin(const std::function<Status(std::uint64_t)>& consume) {
    used_ = 0;
    status_ = Status::success();
    consume_ = &consume;
  }
  void end() { consume_ = nullptr; }
  const Status& status() const { return status_; }
  unsigned mark() const { return used_; }
  void restore(unsigned mark) { used_ = mark; }
  int sign(Index value) const {
    if (ExactRatioWorkspace<kWords>::top(numbers_[value].magnitude) < 0)
      return 0;
    return numbers_[value].negative ? -1 : 1;
  }
  int compare_magnitude(Index a, Index b) {
    return ratio_.compare(numbers_[a].magnitude, numbers_[b].magnitude);
  }
  Index integer(std::uint64_t value) {
    auto result = allocate();
    if (status_.ok())
      numbers_[result].magnitude.words[0] = value;
    return result;
  }
  Index binary(std::uint64_t bits, unsigned fraction = 1075) {
    auto result = allocate();
    if (status_.ok()) {
      const auto value = BinaryParts::decode(bits, false);
      numbers_[result].magnitude.set(value, fraction);
      numbers_[result].negative = value.magnitude && value.negative;
    }
    return result;
  }
  void copy(Index source, Index destination) {
    if (status_.ok())
      numbers_[destination] = numbers_[source];
  }
  Index shift(Index source, unsigned bits) {
    auto result = allocate();
    if (status_.ok()) {
      if (!ExactRatioWorkspace<kWords>::shift(numbers_[source].magnitude, bits,
                                              &numbers_[result].magnitude))
        capacity();
      numbers_[result].negative = numbers_[source].negative;
    }
    return result;
  }
  Index add(Index a, Index b, bool subtract = false) {
    auto result = allocate();
    if (!status_.ok())
      return result;
    auto& out = numbers_[result];
    const auto& left = numbers_[a];
    const auto& right = numbers_[b];
    const bool negative = right.negative != subtract;
    if (left.negative == negative) {
      if (std::max(ExactRatioWorkspace<kWords>::top(left.magnitude),
                   ExactRatioWorkspace<kWords>::top(right.magnitude)) +
              1 >=
          static_cast<int>(kWords * 64)) {
        capacity();
        return result;
      }
      out = left;
      out.magnitude.add(right.magnitude);
    } else if (compare_magnitude(a, b) >= 0) {
      out = left;
      out.magnitude.subtract(right.magnitude);
    } else {
      out = right;
      out.negative = negative;
      out.magnitude.subtract(left.magnitude);
    }
    if (!sign(result))
      out.negative = false;
    return result;
  }
  Index multiply(Index a, Index b) {
    auto result = allocate();
    if (status_.ok()) {
      status_ = multiply_fixed(numbers_[a].magnitude, numbers_[b].magnitude,
                               &numbers_[result].magnitude, *consume_);
      numbers_[result].negative =
          numbers_[a].negative != numbers_[b].negative && sign(result);
    }
    return result;
  }
  Polynomial polynomial() {
    Polynomial result;
    for (auto& coefficient : result.coefficient)
      coefficient = integer(0);
    return result;
  }
  void trim(Polynomial* value) const {
    while (value->degree >= 0 && !sign(value->coefficient[value->degree]))
      --value->degree;
  }
  Polynomial bernstein(const std::array<std::uint64_t, 4>& controls,
                       unsigned degree) {
    auto result = polynomial();
    result.degree = degree;
    const auto saved = mark();
    std::array<Index, 4> p{};
    for (unsigned i = 0; i <= degree; ++i)
      p[i] = binary(controls[i]);
    const auto base = mark();
    copy(p[0], result.coefficient[0]);
    copy(multiply(integer(degree), add(p[1], p[0], true)),
         result.coefficient[1]);
    restore(base);
    auto second = add(add(p[0], shift(p[1], 1), true), p[2]);
    copy(degree == 3 ? multiply(integer(3), second) : second,
         result.coefficient[2]);
    restore(base);
    if (degree == 3) {
      auto third = add(add(p[3], p[0], true),
                       multiply(integer(3), add(p[1], p[2], true)));
      copy(third, result.coefficient[3]);
    }
    restore(saved);
    trim(&result);
    return result;
  }
  // Exact numerator, common denominator 2^(degree*precision); coefficients
  // retain their 2^-1075 units. The zero polynomial uses denominator one.
  Index evaluate(const Polynomial& polynomial, Index t, unsigned precision) {
    if (polynomial.degree < 0)
      return integer(0);
    auto result = polynomial.coefficient[polynomial.degree];
    for (int i = polynomial.degree - 1; i >= 0; --i)
      result =
          add(multiply(result, t), shift(polynomial.coefficient[i],
                                         (polynomial.degree - i) * precision));
    return result;
  }
  std::pair<Index, Index> enclose(const Polynomial& polynomial, Index lower,
                                  unsigned precision) {
    if (polynomial.degree < 0) {
      auto zero = integer(0);
      return {zero, zero};
    }
    const auto upper = add(lower, integer(1));
    auto low = polynomial.coefficient[polynomial.degree], high = low;
    for (int i = polynomial.degree - 1; i >= 0; --i) {
      auto c =
          shift(polynomial.coefficient[i], (polynomial.degree - i) * precision);
      low = add(multiply(low, sign(low) < 0 ? upper : lower), c);
      high = add(multiply(high, sign(high) < 0 ? lower : upper), c);
    }
    return {low, high};
  }
  // f has exactly one root inside (0,1), and neither endpoint is a root.
  // Decide whether g vanishes at that root; a resultant alone is insufficient.
  bool shares_unit_root(const Polynomial& f, const Polynomial& g) {
    const auto saved = mark();
    auto a = polynomial(), b = polynomial();
    copy_polynomial(f, &a);
    copy_polynomial(g, &b);
    trim(&a);
    trim(&b);
    normalize_twos(&a);
    normalize_twos(&b);
    const auto base = mark();
    while (status_.ok() && b.degree > 0) {
      auto r = remainder(a, b);
      copy_polynomial(b, &a);
      copy_polynomial(r, &b);
      restore(base);
    }
    bool result = false;
    if (status_.ok() && b.degree < 0) {
      if (a.degree == 3) {
        result = true;
      } else if (a.degree > 0) {
        const auto at_zero = a.coefficient[0];
        auto at_one = a.coefficient[0];
        for (int i = 1; i <= a.degree; ++i)
          at_one = add(at_one, a.coefficient[i]);
        result = sign(at_zero) * sign(at_one) < 0;
        if (!result && a.degree == 2) {
          const auto quadratic = a.coefficient[2], linear = a.coefficient[1];
          const auto twice = shift(quadratic, 1);
          if (sign(at_zero) == sign(quadratic) &&
              sign(at_one) == sign(quadratic) &&
              sign(linear) == -sign(quadratic) &&
              compare_magnitude(linear, twice) < 0) {
            const auto discriminant =
                add(multiply(linear, linear),
                    shift(multiply(quadratic, at_zero), 2), true);
            result = sign(discriminant) >= 0;
          }
        }
      }
    }
    restore(saved);
    return status_.ok() && result;
  }
  Result<std::uint64_t> round(Index numerator, Index denominator, bool narrow,
                              int scale = -1075) {
    if (!status_.ok())
      return Result<std::uint64_t>(status_);
    ratio_.numerator = numbers_[numerator].magnitude;
    ratio_.denominator = numbers_[denominator].magnitude;
    ratio_.negative = numbers_[numerator].negative;
    return ratio_.round(narrow, *consume_, scale);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
