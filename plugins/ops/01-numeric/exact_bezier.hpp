#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "01-numeric/exact_polynomial.hpp"

namespace ps::plugin_internal::numeric_ops {
// Certified inverse of a monotone quadratic/cubic x polynomial. The root
// bracket is dyadic, and interval Horner encloses the complete y image.
// Common-root decisions resolve exact zero and output midpoint boundaries.
// Inputs are finite, RN64-reconstructed absolute controls; q is interior.
class ExactBezier final {
  ExactPolynomial math_;
  struct End {
    ExactPolynomial& math;
    ~End() { math.end(); }
  };
  using Index = ExactPolynomial::Index;
  using Polynomial = ExactPolynomial::Polynomial;
  Result<std::uint64_t> rounded(Index numerator, int degree, unsigned precision,
                                bool narrow) {
    const auto denominator =
        math_.shift(math_.integer(1), degree > 0 ? degree * precision : 0);
    return math_.round(numerator, denominator, narrow);
  }
  Index boundary(std::uint64_t low, std::uint64_t high, bool narrow) {
    const auto a = BinaryParts::decode(low, narrow);
    const auto b = BinaryParts::decode(high, narrow);
    if (!a.infinite && !b.infinite)
      return math_.add(math_.binary(widen(low, narrow), 1074),
                       math_.binary(widen(high, narrow), 1074));
    const auto maximum =
        narrow ? UINT64_C(0x7f7fffff) : UINT64_C(0x7fefffffffffffff);
    auto result =
        math_.add(math_.binary(widen(maximum, narrow)),
                  math_.shift(math_.integer(1), narrow ? 1178 : 2045));
    return a.negative ? math_.add(math_.integer(0), result, true) : result;
  }

 public:
  explicit ExactBezier(SequenceProfile profile) : math_(profile) {}
  // Finite input only, preserving signed zero and avoiding host floating work.
  static std::uint64_t widen(std::uint64_t bits, bool narrow) {
    if (!narrow)
      return bits;
    const auto value = BinaryParts::decode(bits, true);
    const auto sign = (bits >> 31) << 63;
    if (!value.magnitude)
      return sign;
    const auto top = 63 - __builtin_clzll(value.significand);
    return sign |
           (static_cast<std::uint64_t>(value.exponent + top + 1023) << 52) |
           ((value.significand << (52 - top)) & UINT64_C(0xfffffffffffff));
  }
  Result<bool> monotone(const std::array<std::uint64_t, 4>& x, unsigned degree,
                        const std::function<Status(std::uint64_t)>& consume) {
    math_.begin(consume);
    End end{math_};
    std::array<Index, 4> p{};
    for (unsigned i = 0; i <= degree; ++i)
      p[i] = math_.binary(x[i]);
    const auto u = math_.add(p[1], p[0], true);
    const auto v = math_.add(p[2], p[1], true);
    bool result = math_.sign(u) >= 0;
    if (degree == 2) {
      result = result && math_.sign(v) >= 0;
    } else {
      const auto w = math_.add(p[3], p[2], true);
      result = result && math_.sign(w) >= 0;
      const auto a = math_.add(math_.add(u, math_.shift(v, 1), true), w);
      const auto b = math_.shift(math_.add(v, u, true), 1);
      if (result && math_.sign(a) > 0 && math_.sign(b) < 0 &&
          math_.compare_magnitude(b, math_.shift(a, 1)) < 0) {
        const auto minimum = math_.add(math_.shift(math_.multiply(a, u), 2),
                                       math_.multiply(b, b), true);
        result = math_.sign(minimum) >= 0;
      }
    }
    return math_.status().ok() ? Result<bool>(result)
                               : Result<bool>(math_.status());
  }
  Result<std::uint64_t> inverse(
      const std::array<std::uint64_t, 4>& x,
      const std::array<std::uint64_t, 4>& y, unsigned degree,
      std::uint64_t query, bool narrow,
      const std::function<Status(std::uint64_t)>& consume,
      const std::function<Status()>& root_call = {}) {
    using Answer = Result<std::uint64_t>;
    math_.begin(consume);
    End end{math_};
    auto f = math_.bernstein(x, degree), ordinate = math_.bernstein(y, degree);
    const auto coefficients = math_.mark();
    math_.copy(math_.add(f.coefficient[0], math_.binary(query), true),
               f.coefficient[0]);
    math_.restore(coefficients);
    math_.trim(&f);
    const auto lower = math_.integer(0);
    const auto base =
        math_.mark();  // Nine persistent slots; GCD needs 24 more.
    if (ordinate.degree <= 0)
      return rounded(ordinate.coefficient[0], 0, 0, narrow);
    bool zero_checked = false, boundary_checked = false;
    std::uint64_t previous_boundary = 0;
    for (unsigned precision = 1; precision <= 8192; ++precision) {
      auto middle = math_.add(math_.shift(lower, 1), math_.integer(1));
      if (!math_.status().ok())
        return Answer(math_.status());
      if (root_call) {
        auto admitted = root_call();
        if (!admitted.ok())
          return Answer(admitted);
      }
      const auto value = math_.evaluate(f, middle, precision);
      if (!math_.status().ok())
        return Answer(math_.status());
      if (!math_.sign(value)) {
        auto exact = math_.evaluate(ordinate, middle, precision);
        return rounded(exact, ordinate.degree, precision, narrow);
      }
      math_.copy(math_.sign(value) < 0 ? middle : math_.shift(lower, 1), lower);
      math_.restore(base);
      if (precision % 8)
        continue;
      const auto enclosure = math_.enclose(ordinate, lower, precision);
      const bool crosses_zero =
          math_.sign(enclosure.first) <= 0 && math_.sign(enclosure.second) >= 0;
      auto low = rounded(enclosure.first, ordinate.degree, precision, narrow);
      if (!low.ok())
        return low;
      auto high = rounded(enclosure.second, ordinate.degree, precision, narrow);
      if (!high.ok())
        return high;
      math_.restore(base);
      if (low.value() == high.value())
        return low;
      if (crosses_zero && !zero_checked) {
        zero_checked = true;
        const bool exact_zero = math_.shares_unit_root(f, ordinate);
        if (!math_.status().ok())
          return Answer(math_.status());
        if (exact_zero)
          return Answer(UINT64_C(0));
      }
      const auto low_key = BinaryParts::decode(low.value(), narrow).order_key();
      const auto high_key =
          BinaryParts::decode(high.value(), narrow).order_key();
      if (high_key - low_key == 1 &&
          (!boundary_checked || previous_boundary != low.value())) {
        boundary_checked = true;
        previous_boundary = low.value();
        auto target = boundary(low.value(), high.value(), narrow);
        const bool negative_boundary = math_.sign(target) < 0;
        auto g = math_.polynomial();
        g.degree = ordinate.degree;
        for (unsigned i = 0; i < 4; ++i)
          math_.copy(ordinate.coefficient[i], g.coefficient[i]);
        math_.copy(math_.add(g.coefficient[0], target, true), g.coefficient[0]);
        const bool tie = math_.shares_unit_root(f, g);
        if (!math_.status().ok())
          return Answer(math_.status());
        math_.restore(base);
        if (tie) {
          auto bits = (low.value() & 1) ? high.value() : low.value();
          const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
          // A coarse enclosure may round through zero from the opposite side.
          // The proved nonzero midpoint supplies the underflow zero sign.
          if (!(bits & (sign - 1)))
            bits = negative_boundary ? sign : 0;
          return Answer(bits);
        }
      }
    }
    return Answer(Status{ErrorCode::ResourceExhausted,
                         "Bezier inverse refinement capacity",
                         FailureReason::CapacityLimit});
  }
};
}  // namespace ps::plugin_internal::numeric_ops
