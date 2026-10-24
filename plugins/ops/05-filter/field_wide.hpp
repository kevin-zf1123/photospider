#pragma once

#include "05-filter/arithmetic.hpp"

namespace ps::plugin_internal::filter {
struct FieldWideArithmetic : WideArithmetic {
  explicit FieldWideArithmetic(bool narrow) : WideArithmetic(narrow) {}
  static Ball product(Ball a, Ball b) {
    if (!a.finite() || !b.finite())
      return Ball::bad();
    if (b.point())
      return a.times(b.hi);
    if (a.point())
      return b.times(a.hi);
    auto result = a.times(b.hi) + a.times(b.lo);
    const auto size =
        Ball::up(Ball::up(std::abs(a.hi) + std::abs(a.lo)) + a.error);
    const auto propagated =
        b.error != 0 && size != 0 ? numeric_ops::numeric_up(size * b.error) : 0;
    result.error = Ball::up(result.error + propagated);
    return result;
  }
  BallValue add(BallValue a, BallValue b) const {
    if (!a.divided && !b.divided)
      return {a.numerator + b.numerator};
    return {product(a.numerator, b.denominator) +
                product(b.numerator, a.denominator),
            product(a.denominator, b.denominator), true};
  }
  BallValue multiply(BallValue a, BallValue b) const {
    return {product(a.numerator, b.numerator),
            product(a.denominator, b.denominator), a.divided || b.divided};
  }
  BallValue divide(BallValue a, BallValue b) const {
    return {product(a.numerator, b.denominator),
            product(a.denominator, b.numerator), true};
  }
};
}  // namespace ps::plugin_internal::filter
