#include "03-generation/math.hpp"

#include <algorithm>

namespace ps::plugin_internal::generation {
namespace {
void shift_small(Directed::Integer* value, unsigned bits, unsigned words) {
  std::uint64_t carry = 0;
  for (unsigned i = 0; i < words; ++i) {
    const auto next = value->words[i] >> (64 - bits);
    value->words[i] = (value->words[i] << bits) | carry;
    carry = next;
  }
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
  for (int pair = static_cast<int>(pairs) - 1; pair >= 0; --pair) {
    if ((pairs - static_cast<unsigned>(pair) - 1) % 32 == 0) {
      const auto batch = std::min(32U, static_cast<unsigned>(pair) + 1);
      if (!math.work(batch * (7 * words + 4))) {
        return;
      }
    }
    shift_small(&remainder, 2, words);
    const auto bit = static_cast<unsigned>(2 * pair);
    remainder.words[0] |= (source.words[bit / 64] >> (bit % 64)) & 3;
    shift_small(&root, 1, words);
    std::copy_n(root.words.begin(), words, trial.words.begin());
    shift_small(&trial, 1, words);
    trial.words[0] |= 1;
    int order = 0;
    for (unsigned j = words; j; --j) {
      if (remainder.words[j - 1] != trial.words[j - 1]) {
        order = remainder.words[j - 1] < trial.words[j - 1] ? -1 : 1;
        break;
      }
    }
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
  const bool yz = Exact::zero(y), xz = Exact::zero(x);
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
  if (Exact::zero(x)) {
    d.integer(output, 0);
  } else if (Exact::zero(rest)) {
    d.constant(output, true);
    d.scale(output, output, -1);
  } else {
    atan_positive(output, xx, root, exact.order(square, rest));
  }
  if (input.negative) {
    d.negate(output, output);
  }
}
std::uint64_t Math::affine_sqrt(const Rational& bias, const Rational& factor,
                                const Rational& radicand, bool narrow) {
  if (Exact::zero(factor) || Exact::zero(radicand)) {
    return rounded(bias, narrow);
  }
  if (Exact::zero(bias)) {
    const auto result = rounded_sqrt(mul(sq(factor), radicand), narrow);
    return result | (factor.negative ? UINT64_C(1) << (narrow ? 31 : 63) : 0);
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
  if (Exact::zero(component) || Exact::zero(squared_norm)) {
    return 0;
  }
  auto value = rounded_sqrt(div(sq(component), squared_norm), narrow);
  if (component.negative) {
    value |= UINT64_C(1) << (narrow ? 31 : 63);
  }
  return value;
}
}  // namespace ps::plugin_internal::generation
