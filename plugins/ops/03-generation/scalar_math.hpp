#pragma once

#include "03-generation/math.hpp"

namespace ps::plugin_internal::generation {
// Polynomial-only arena: omits transcendental interval storage. The caller
// chooses a limb tier before arithmetic, using a bound on expression size.
template <unsigned Words>
struct ScalarMath final {
  using E = model_ops::RationalMathStorage<Words>;
  using R = typename E::Rational;
  E exact{numeric_ops::SequenceProfile::Strict};
  explicit ScalarMath(const Work& work) { exact.bind(work); }
  R input(const Reader& reader, unsigned port, const Coordinate& at) {
    return exact.binary(
        reader.bits(port, 0, at),
        reader.kernel.groups[reader.kernel.input_base[port]].type ==
            ElementType::Float32);
  }
  R add(const R& a, const R& b) { return exact.add(a, b); }
  R sub(const R& a, const R& b) { return exact.subtract(a, b); }
  R mul(const R& a, const R& b) { return exact.multiply(a, b); }
  R div(const R& a, const R& b) { return exact.divide(a, b); }
  R sq(const R& a) { return mul(a, a); }
  R center(std::int64_t origin, std::uint64_t index) {
    return add(add(exact.integer(origin), exact.integer(index)),
               exact.fraction(1, 2));
  }
  std::uint64_t rounded(R value, bool narrow, bool copy = false) {
    if (!copy && E::zero(value))
      value.negative = false;
    const auto result = exact.rounded(value, narrow);
    check(exact.status());
    return result;
  }
};
}  // namespace ps::plugin_internal::generation
