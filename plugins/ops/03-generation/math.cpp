#include "03-generation/math.hpp"

#include <algorithm>

namespace ps::plugin_internal::generation {
namespace {
template <class Integer>
void shift_small(Integer* value, unsigned bits, unsigned words) {
  std::uint64_t carry = 0;
  for (unsigned i = 0; i < words; ++i) {
    const auto next = value->words[i] >> (64 - bits);
    value->words[i] = (value->words[i] << bits) | carry;
    carry = next;
  }
}
bool integer_sqrt_exact(const Exact::Integer& source, Exact::Integer* output,
                        const Work& work) {
  using Workspace = Exact::Workspace;
  const int highest = Workspace::top(source);
  output->words.fill(0);
  if (highest < 0) {
    return true;
  }
  // A square has an even binary valuation and one of these residues mod 64.
  constexpr std::uint64_t residues = UINT64_C(0x0202021202030213);
  if (!((residues >> (source.words[0] & 63)) & 1)) {
    return false;
  }
  unsigned lowest = 0;
  while (!source.words[lowest / 64]) {
    work.poll();
    lowest += 64;
  }
  lowest += static_cast<unsigned>(__builtin_ctzll(source.words[lowest / 64]));
  if (lowest & 1) {
    return false;
  }
  if (lowest == static_cast<unsigned>(highest)) {
    output->words[lowest / 128] = UINT64_C(1) << ((lowest / 2) % 64);
    return true;
  }
  Exact::Integer root, remainder, trial;
  const unsigned pairs = static_cast<unsigned>(highest / 2 + 1);
  const unsigned words = (pairs + 65) / 64;
  const auto checkpoint = [&](unsigned amount) { work.poll(amount); };
  for (int pair = static_cast<int>(pairs) - 1; pair >= 0; --pair) {
    checkpoint(4 * words + 4);
    shift_small(&remainder, 2, words);
    const auto bit = static_cast<unsigned>(2 * pair);
    remainder.words[0] |= (source.words[bit / 64] >> (bit % 64)) & 3;
    shift_small(&root, 1, words);
    std::copy_n(root.words.begin(), words, trial.words.begin());
    shift_small(&trial, 1, words);
    trial.words[0] |= 1;
    checkpoint(words);
    int order = 0;
    for (unsigned j = words; j; --j) {
      if (remainder.words[j - 1] != trial.words[j - 1]) {
        order = remainder.words[j - 1] < trial.words[j - 1] ? -1 : 1;
        break;
      }
    }
    checkpoint(2 * words);
    if (order >= 0) {
      std::uint64_t borrow = 0;
      for (unsigned j = 0; j < words; ++j) {
        const auto sub =
            static_cast<unsigned __int128>(trial.words[j]) + borrow;
        const auto old = remainder.words[j];
        remainder.words[j] = static_cast<std::uint64_t>(
            static_cast<unsigned __int128>(old) - sub);
        borrow = static_cast<unsigned __int128>(old) < sub;
      }
      for (unsigned j = 0; j < words; ++j) {
        if (++root.words[j]) {
          break;
        }
      }
    }
  }
  if (Workspace::top(remainder) >= 0) {
    return false;
  }
  *output = root;
  return true;
}
void root_endpoint(Directed& math, Directed::Number& output,
                   const Directed::Number& input, bool upper) {
  Directed::Integer source, root, remainder, trial;
  math.shift_unsigned(source, input.magnitude, math.precision);
  if (!math.active()) {
    return;
  }
  const int highest = Directed::Workspace::top(source);
  const unsigned pairs =
      highest < 0 ? 0 : static_cast<unsigned>(highest / 2 + 1);
  // After j pairs, root needs j bits; the unreduced remainder needs j+2.
  // Keep the full source, but never scan the unused high half for each bit.
  const unsigned words = (pairs + 65) / 64;
  const auto checkpoint = [&](unsigned amount) {
    if (!math.work(amount))
      check(math.status());
  };
  for (int pair = static_cast<int>(pairs) - 1; pair >= 0; --pair) {
    checkpoint(4 * words + 4);
    shift_small(&remainder, 2, words);
    const auto bit = static_cast<unsigned>(2 * pair);
    remainder.words[0] |= (source.words[bit / 64] >> (bit % 64)) & 3;
    shift_small(&root, 1, words);
    std::copy_n(root.words.begin(), words, trial.words.begin());
    shift_small(&trial, 1, words);
    trial.words[0] |= 1;
    checkpoint(words);
    int order = 0;
    for (unsigned j = words; j; --j) {
      if (remainder.words[j - 1] != trial.words[j - 1]) {
        order = remainder.words[j - 1] < trial.words[j - 1] ? -1 : 1;
        break;
      }
    }
    checkpoint(2 * words);
    if (order >= 0) {
      std::uint64_t borrow = 0;
      for (unsigned j = 0; j < words; ++j) {
        const auto sub =
            static_cast<unsigned __int128>(trial.words[j]) + borrow;
        const auto old = remainder.words[j];
        remainder.words[j] = static_cast<std::uint64_t>(
            static_cast<unsigned __int128>(old) - sub);
        borrow = static_cast<unsigned __int128>(old) < sub;
      }
      for (unsigned j = 0; j < words; ++j) {
        if (++root.words[j]) {
          break;
        }
      }
    }
  }
  output.magnitude = root;
  output.negative = false;
  if (upper && Directed::Workspace::top(remainder) >= 0) {
    math.increment(output.magnitude);
  }
}
}  // namespace
void Math::enclose(Interval output, const Rational& value) {
  check(exact.status());
  if (!d.active())
    return;
  const auto kind = classify(value);
  if (kind == Exact::Classification::Zero) {
    d.integer(output, 0);
    return;
  }
  if (kind != Exact::Classification::FiniteNonzero) {
    d.retry();
    return;
  }
  const int numerator_top = kind.numerator_top;
  const int denominator_top = kind.denominator_top;
  // Common path coefficients are integers or small rational ratios. Import
  // their signed numerator directly in Q_precision units, then use one-limb
  // directed division. This avoids normalized full-width long division while
  // retaining exact floor/ceiling bounds, including gradual underflow.
  if (denominator_top < 64 && numerator_top + static_cast<int>(d.precision) <
                                  static_cast<int>(64 * Directed::kWords)) {
    work.poll(Directed::kWords);
    Directed::Frame frame(d);
    auto& source = d.number();
    std::copy_n(value.n.words.begin(), Directed::kWords,
                source.magnitude.words.begin());
    source.negative = value.negative;
    d.shift(output.low, source, static_cast<int>(d.precision), true);
    d.copy(output.high, output.low);
    if (value.d.words[0] != 1)
      d.divide_small(output, output, value.d.words[0]);
    return;
  }
  // Powers of two need only a signed directed shift; large binary exponents
  // therefore never enter the rational divider either.
  bool dyadic = denominator_top >= 0;
  for (unsigned j = 0; j <= static_cast<unsigned>(denominator_top) / 64; ++j) {
    work.poll();
    const auto expected = j == static_cast<unsigned>(denominator_top) / 64
                              ? UINT64_C(1) << (denominator_top % 64)
                              : 0;
    dyadic &= value.d.words[j] == expected;
  }
  const int shift = static_cast<int>(d.precision) - denominator_top;
  if (dyadic && numerator_top < static_cast<int>(64 * Directed::kWords) &&
      numerator_top + shift < static_cast<int>(64 * Directed::kWords)) {
    work.poll(Directed::kWords);
    Directed::Frame frame(d);
    auto& source = d.number();
    std::copy_n(value.n.words.begin(), Directed::kWords,
                source.magnitude.words.begin());
    source.negative = value.negative;
    d.shift(output.low, source, shift, true);
    d.shift(output.high, source, shift, false);
    return;
  }
  const int exponent = exact.normalized(d, output, value, &kind);
  check(exact.status());
  d.scale(output, output, exponent);
  if (value.negative)
    d.negate(output, output);
}
void Math::sqrt(Interval output, Interval input) {
  if (!d.active()) {
    return;
  }
  Directed::Frame frame(d);
  auto result = d.interval();
  // The mathematical radicand is nonnegative. Clip outward negative noise.
  if (input.low.negative) {
    d.clear(result.low);
  } else {
    root_endpoint(d, result.low, input.low, false);
  }
  if (input.high.negative) {
    d.retry();
    return;
  }
  root_endpoint(d, result.high, input.high, true);
  d.copy(output, result);
}
void Math::sincos(Interval sine, Interval cosine, Interval input) {
  if (!d.active()) {
    return;
  }
  Directed::Frame frame(d);
  auto half_pi = d.interval(), quotient = d.interval(), half = d.interval();
  d.constant(half_pi, true);
  d.scale(half_pi, half_pi, -1);
  d.divide(quotient, input, half_pi);
  d.dyadic(half, 1, -1);
  d.add(quotient, quotient, half);
  auto& lower = d.number();
  d.shift(lower, quotient.low, -static_cast<int>(d.precision), true);
  // Any integer quadrant is valid if the reduced interval remains in the
  // certified series domain. Requiring a unique nearest quadrant would never
  // converge at an exact odd multiple of pi/4.
  if (!d.active()) {
    return;
  }
  const unsigned raw = static_cast<unsigned>(lower.magnitude.words[0] & 3);
  const unsigned quadrant = lower.negative ? (4 - raw) & 3 : raw;
  auto multiple = d.interval(), reduced = d.interval(), s = d.interval(),
       c = d.interval(), bound = d.interval();
  d.shift(multiple.low, lower, static_cast<int>(d.precision), true);
  d.copy(multiple.high, multiple.low);
  d.multiply(multiple, multiple, half_pi);
  d.subtract(reduced, input, multiple);
  d.integer(bound, 1);
  if (d.compare(reduced.high, bound.high) > 0) {
    d.retry();
    return;
  }
  d.negate(bound, bound);
  if (d.compare(reduced.low, bound.low) < 0) {
    d.retry();
    return;
  }
  functions.functions.sincos_series(s, c, reduced);
  functions.functions.orient(sine, cosine, s, c, quadrant, false);
}
void Math::atan_positive(Interval output, Interval y, Interval x, int order) {
  if (!d.active()) {
    return;
  }
  Directed::Frame frame(d);
  if (!order) {
    d.constant(output, true);
    d.scale(output, output, -2);
    return;
  }
  auto ratio = d.interval(), angle = d.interval(), one = d.interval();
  d.divide(ratio, order < 0 ? y : x, order < 0 ? x : y);
  if (!d.active()) {
    return;
  }
  d.integer(one, 1);
  if (ratio.low.negative) {
    d.clear(ratio.low);
  }
  if (d.compare(ratio.high, one.high) > 0) {
    d.copy(ratio.high, one.high);
  }
  functions.functions.arctangent(angle, ratio);
  if (order > 0) {
    auto half_pi = d.interval();
    d.constant(half_pi, true);
    d.scale(half_pi, half_pi, -1);
    d.subtract(output, half_pi, angle);
  } else {
    d.copy(output, angle);
  }
}
void Math::atan2(Interval output, const Rational& y, const Rational& x) {
  if (!d.active()) {
    return;
  }
  Directed::Frame frame(d);
  const bool yz = is_zero(y), xz = is_zero(x);
  if (yz && !x.negative) {
    d.integer(output, 0);
    return;
  }
  if (yz || xz) {
    d.constant(output, true);
    if (xz) {
      d.scale(output, output, -1);
    }
    if (!yz && y.negative) {
      d.negate(output, output);
    }
    return;
  }
  auto yy = d.interval(), xx = d.interval(), angle = d.interval();
  const auto ay = exact.absolute(y), ax = exact.absolute(x);
  enclose(yy, ay);
  enclose(xx, ax);
  atan_positive(angle, yy, xx, exact.order(ay, ax));
  if (x.negative) {
    auto pi = d.interval();
    d.constant(pi, true);
    d.subtract(angle, pi, angle);
  }
  if (y.negative) {
    d.negate(angle, angle);
  }
  d.copy(output, angle);
}
void Math::asin(Interval output, const Rational& input) {
  Directed::Frame frame(d);
  const auto x = exact.absolute(input);
  const auto square = sq(x), rest = sub(exact.integer(1), square);
  auto xx = d.interval(), yy = d.interval(), root = d.interval();
  enclose(xx, x);
  enclose(yy, rest);
  sqrt(root, yy);
  if (is_zero(x)) {
    d.integer(output, 0);
  } else if (is_zero(rest)) {
    d.constant(output, true);
    d.scale(output, output, -1);
  } else {
    atan_positive(output, xx, root, exact.order(square, rest));
  }
  if (input.negative) {
    d.negate(output, output);
  }
}
bool Math::rational_sqrt(const Rational& value, Rational* root) {
  check(exact.status());
  work.poll(Exact::kWords);
  const auto kind = classify(value);
  if (kind == Exact::Classification::Infinity ||
      kind == Exact::Classification::NaN ||
      (value.negative && kind != Exact::Classification::Zero)) {
    return false;
  }
  Rational result;
  if (!integer_sqrt_exact(value.n, &result.n, work) ||
      !integer_sqrt_exact(value.d, &result.d, work)) {
    return false;
  }
  *root = result;
  return true;
}
std::uint64_t Math::affine_sqrt(const Rational& bias, const Rational& factor,
                                const Rational& radicand, bool narrow) {
  if (is_zero(factor) || is_zero(radicand)) {
    return rounded(bias, narrow);
  }
  if (is_zero(bias)) {
    const auto result = rounded_sqrt(mul(sq(factor), radicand), narrow);
    return result | (factor.negative ? UINT64_C(1) << (narrow ? 31 : 63) : 0);
  }
  // Rational roots include exact cancellation and every possible IEEE midpoint.
  // Evaluate them in the exact field before interval rounding loses
  // correlation.
  Rational root;
  if (rational_sqrt(radicand, &root)) {
    return rounded(add(bias, mul(factor, root)), narrow);
  }
  return certify(narrow, [&](Interval out) {
    auto b = d.interval(), f = d.interval(), r = d.interval();
    enclose(b, bias);
    enclose(f, factor);
    enclose(r, radicand);
    sqrt(r, r);
    d.multiply(r, r, f);
    d.add(out, b, r);
  });
}
std::uint64_t Math::tangent(const Rational& component,
                            const Rational& squared_norm, bool narrow) {
  if (is_zero(component) || is_zero(squared_norm)) {
    return 0;
  }
  auto value = rounded_sqrt(div(sq(component), squared_norm), narrow);
  if (component.negative) {
    value |= UINT64_C(1) << (narrow ? 31 : 63);
  }
  return value;
}
}  // namespace ps::plugin_internal::generation
