#pragma once

#include <cstdint>

#include "01-numeric/exact_root.hpp"
#include "02-format-color/transfer_program.hpp"

namespace ps::plugin_internal::transfer_ops {
// A degree-two polynomial with <=64-bit coefficients at scale 2^-2148 has
// <4262 magnitude bits for finite binary64 x. The 4352-bit ratio also holds
// shifted division operands and the squared midpoint used by final sqrt.
class AlgebraicMath final {
  using Integer = numeric_ops::FixedInteger<68>;
  using Ratio = numeric_ops::ExactRatioWorkspace<68>;
  Ratio ratio_{SequenceProfile::Strict};
  Integer input_, product_, coefficient_;

 public:
  Result<std::uint64_t> evaluate(const Program& program, double x, bool narrow,
                                 const core_internal::WorkConsumer& consume) {
    using Answer = Result<std::uint64_t>;
    auto status = consume(1);
    if (!status.ok())
      return Answer(status);
    const auto& polynomial = program.algebraic;
    const auto parts = numeric_ops::BinaryParts::decode(numeric_bits(x), false);
    // The only nonfinite algebraic operand admitted by branch selection is
    // ACEScc's constant floor; its polynomial has no nonconstant coefficients.
    input_.words.fill(0);
    if (!parts.nan && !parts.infinite)
      input_.set(parts, 1074);
    ratio_.numerator.words.fill(0);
    ratio_.negative = false;
    for (unsigned degree = 0; degree < 3; ++degree) {
      const auto coefficient = polynomial.numerator[degree];
      if (!coefficient || (degree && !parts.significand))
        continue;
      coefficient_.words.fill(0);
      coefficient_.words[0] = coefficient;
      if (!degree) {
        Ratio::shift(coefficient_, 2148, &ratio_.term);
      } else {
        if (degree == 2) {
          status =
              numeric_ops::multiply_fixed(input_, input_, &product_, consume);
          if (!status.ok())
            return Answer(status);
        }
        status =
            numeric_ops::multiply_fixed(degree == 2 ? product_ : input_,
                                        coefficient_, &ratio_.term, consume);
        if (!status.ok())
          return Answer(status);
        if (degree == 1) {
          Ratio::shift(ratio_.term, 1074, &ratio_.shifted);
          ratio_.term = ratio_.shifted;
        }
      }
      ratio_.add_term(polynomial.negative[degree] !=
                      (degree == 1 && parts.negative));
    }
    ratio_.denominator.words.fill(0);
    ratio_.denominator.words[0] = polynomial.denominator;
    if (program.final_sqrt) {
      if (ratio_.negative && Ratio::top(ratio_.numerator) >= 0)
        return Answer(narrow ? UINT64_C(0x7fc00000)
                             : UINT64_C(0x7ff8000000000000));
      ratio_.negative = false;
      return numeric_ops::round_sqrt_ratio(&ratio_, narrow, -2148, consume);
    }
    return ratio_.round(narrow, consume, -2148);
  }
};
}  // namespace ps::plugin_internal::transfer_ops
