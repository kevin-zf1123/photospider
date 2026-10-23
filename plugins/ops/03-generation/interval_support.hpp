#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "03-generation/math.hpp"

namespace ps::plugin_internal::generation {
struct OwnedInterval final {
  Directed::Number low, high;
  Interval view() { return {low, high}; }
  Interval view() const {
    return {const_cast<Directed::Number&>(low),
            const_cast<Directed::Number&>(high)};
  }
};
struct IntegrationRetry final {};
class IntervalMath final {
 public:
  Math& m;
  explicit IntervalMath(Math& math) : m(math) {}
  void check_state() {
    check(m.exact.status());
    if (m.d.fault == Directed::Fault::Failure)
      check(m.d.status());
    if (!m.d.active())
      throw IntegrationRetry{};
  }
  OwnedInterval integer(std::int64_t n) {
    OwnedInterval result;
    m.d.integer(result.view(), n);
    return result;
  }
  OwnedInterval rational(const Rational& n) {
    OwnedInterval result;
    m.enclose(result.view(), n);
    return result;
  }
  OwnedInterval range(const Rational& a, const Rational& b) {
    const auto x = rational(a), y = rational(b);
    return {x.low, y.high};
  }
  OwnedInterval add(const OwnedInterval& a, const OwnedInterval& b) {
    OwnedInterval out;
    m.d.add(out.view(), a.view(), b.view());
    return out;
  }
  OwnedInterval sub(const OwnedInterval& a, const OwnedInterval& b) {
    OwnedInterval out;
    m.d.subtract(out.view(), a.view(), b.view());
    return out;
  }
  OwnedInterval mul(const OwnedInterval& a, const OwnedInterval& b) {
    OwnedInterval out;
    m.d.multiply(out.view(), a.view(), b.view());
    return out;
  }
  OwnedInterval div(const OwnedInterval& a, const OwnedInterval& b) {
    OwnedInterval out;
    m.d.divide(out.view(), a.view(), b.view());
    return out;
  }
  OwnedInterval divide(const OwnedInterval& a, std::uint64_t n) {
    OwnedInterval out;
    m.d.divide_small(out.view(), a.view(), n);
    return out;
  }
  OwnedInterval negate(const OwnedInterval& a) {
    OwnedInterval out;
    m.d.negate(out.view(), a.view());
    return out;
  }
  OwnedInterval scale(const OwnedInterval& a, int exponent) {
    OwnedInterval out;
    m.d.scale(out.view(), a.view(), exponent);
    return out;
  }
  OwnedInterval sqrt(const OwnedInterval& a) {
    OwnedInterval out;
    m.sqrt(out.view(), a.view());
    return out;
  }
  OwnedInterval absolute(const OwnedInterval& a) {
    OwnedInterval out;
    if (!a.low.negative)
      return a;
    if (a.high.negative)
      return negate(a);
    auto low = a.low;
    low.negative = false;
    out.high = m.d.compare(low, a.high) > 0 ? low : a.high;
    return out;
  }
  OwnedInterval hull(const OwnedInterval& a, const OwnedInterval& b) {
    return {m.d.compare(a.low, b.low) < 0 ? a.low : b.low,
            m.d.compare(a.high, b.high) > 0 ? a.high : b.high};
  }
  OwnedInterval intersect(const OwnedInterval& a, const OwnedInterval& b) {
    return {m.d.compare(a.low, b.low) > 0 ? a.low : b.low,
            m.d.compare(a.high, b.high) < 0 ? a.high : b.high};
  }
  bool positive(const Directed::Number& n) {
    return !n.negative && Directed::Workspace::top(n.magnitude) >= 0;
  }
  Rational exact_number(const Directed::Number& value) {
    Rational out;
    out.d.words.fill(0);
    out.d.words[m.d.precision / 64] = UINT64_C(1) << (m.d.precision % 64);
    std::copy(value.magnitude.words.begin(), value.magnitude.words.end(),
              out.n.words.begin());
    out.negative = value.negative;
    return out;
  }
  std::uint64_t endpoint(const Directed::Number& value, bool upper) {
    const auto exact = exact_number(value);
    const auto bits_value = m.rounded(exact, false);
    const int order = m.exact.order(m.exact.binary(bits_value), exact);
    if (upper ? order >= 0 : order <= 0)
      return bits_value;
    return bits(std::nextafter(
        number(bits_value), upper ? std::numeric_limits<double>::infinity()
                                  : -std::numeric_limits<double>::infinity()));
  }
  bool rounded(const OwnedInterval& value, bool narrow, std::uint64_t* output) {
    check_state();
    const auto a = m.d.rounded(value.low, narrow);
    const auto b = m.d.rounded(value.high, narrow);
    check_state();
    if (a != b)
      return false;
    *output = a;
    return true;
  }
};
}  // namespace ps::plugin_internal::generation
