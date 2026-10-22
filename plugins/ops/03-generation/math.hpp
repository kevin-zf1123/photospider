#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <utility>

#include "01-numeric/directed_color_functions.hpp"
#include "02-format-color/model_rational.hpp"
#include "03-generation/runtime.hpp"

namespace ps::plugin_internal::generation {
using Exact = model_ops::RationalMathStorage<384>;
using Rational = Exact::Rational;
using Directed = numeric_ops::CheckedDirectedInterval;
using Interval = Directed::Interval;

// The complete arithmetic arena is admitted as Host scratch before execution.
// Every integer operation and refinement uses the invocation's work consumer.
class Math final {
 public:
  Exact exact{numeric_ops::SequenceProfile::Strict};
  numeric_ops::DirectedColorFunctionsStorage<Directed> functions{
      numeric_ops::SequenceProfile::Strict};
  Directed& d = functions.functions.math;
  const Work& work;
  std::uint64_t max_refinements;
  explicit Math(const Work& consumer, std::uint64_t refinements = 4096)
      : work(consumer), max_refinements(refinements) {
    exact.bind(consumer);
    d.consume = core_internal::WorkConsumer(consumer);
  }
  Rational value(double v) const { return exact.binary(bits(v)); }
  Rational input(const Reader& reader, unsigned port, unsigned field,
                 const Coordinate& at) const {
    const auto type =
        reader.kernel.groups[reader.kernel.input_base[port] + field].type;
    return exact.binary(reader.bits(port, field, at),
                        type == ElementType::Float32);
  }
  Rational add(const Rational& a, const Rational& b) const {
    return exact.add(a, b);
  }
  Rational sub(const Rational& a, const Rational& b) const {
    return exact.subtract(a, b);
  }
  Rational mul(const Rational& a, const Rational& b) const {
    return exact.multiply(a, b);
  }
  Rational div(const Rational& a, const Rational& b) const {
    return exact.divide(a, b);
  }
  Rational sq(const Rational& a) const { return mul(a, a); }
  Rational scale(const Rational& a, std::int64_t b) const {
    return mul(a, exact.integer(b));
  }
  Rational lerp(const Rational& a, const Rational& b, const Rational& t) const {
    return add(a, mul(t, sub(b, a)));
  }
  Rational min(const Rational& a, const Rational& b) const {
    return exact.order(a, b) <= 0 ? a : b;
  }
  Rational max(const Rational& a, const Rational& b) const {
    return exact.order(a, b) >= 0 ? a : b;
  }
  Rational positive(const Rational& a) const {
    return max(a, exact.integer(0));
  }
  Rational center(std::int64_t origin, std::uint64_t index) const {
    return add(add(exact.integer(origin), exact.integer(index)),
               exact.fraction(1, 2));
  }
  Rational fade(const Rational& x) const {
    return mul(mul(sq(x), x), add(mul(x, sub(scale(x, 6), exact.integer(15))),
                                  exact.integer(10)));
  }
  std::uint64_t rounded(const Rational& value, bool narrow,
                        bool preserve_zero = false) {
    auto v = value;
    if (!preserve_zero && Exact::zero(v)) {
      v.negative = false;
    }
    const auto result = exact.rounded(v, narrow);
    check(exact.status());
    return result;
  }
  std::uint64_t rounded_sqrt(const Rational& value, bool narrow) {
    const auto result = exact.rounded_sqrt(value, narrow);
    check(exact.status());
    return result;
  }
  void enclose(Interval output, const Rational& value) {
    exact.enclose(d, output, value);
  }
  void sqrt(Interval output, Interval input);
  void sincos(Interval sine, Interval cosine, Interval input);
  void atan2(Interval output, const Rational& y, const Rational& x);
  // atan(y/x) for a certified first-quadrant pair. Caller supplies the exact
  // ordering to avoid a nonconvergent comparison at x==y.
  void atan_positive(Interval output, Interval y, Interval x, int y_order_x);
  void asin(Interval output, const Rational& x);
  template <class Expression>
  std::uint64_t certify(bool narrow, Expression expression) {
    for (unsigned precision = 128, attempt = 0;
         precision <= 4096 && attempt < max_refinements;
         precision *= 2, ++attempt) {
      work.poll();
      d.reset();
      d.precision = precision;
      auto result = d.interval();
      expression(result);
      check(exact.status());
      if (d.fault == Directed::Fault::Failure) {
        check(d.status());
      }
      if (!d.active()) {
        continue;
      }
      const auto lower = d.rounded(result.low, narrow);
      const auto upper = d.rounded(result.high, narrow);
      if (!d.active()) {
        if (d.fault == Directed::Fault::Failure) {
          check(d.status());
        }
        continue;
      }
      if (lower == upper) {
        return lower;
      }
    }
    throw Status{ErrorCode::OperationFailed,
                 "generation refinement did not certify rounding",
                 DomainErrorCode{UINT64_C(0x47454e0001)}};
  }
  std::uint64_t affine_sqrt(const Rational& bias, const Rational& factor,
                            const Rational& radicand, bool narrow);
  std::uint64_t tangent(const Rational& component, const Rational& squared_norm,
                        bool narrow);
};

// Destroy the arena even if a bounded-work or cancellation failure unwinds.
struct MathArena final {
  Math* value;
  MathArena(void* storage, const Work& work, std::uint64_t refinements = 4096)
      : value(new (storage) Math(work, refinements)) {}
  ~MathArena() { value->~Math(); }
  Math& operator*() const { return *value; }
  Math* operator->() const { return value; }
};
}  // namespace ps::plugin_internal::generation
