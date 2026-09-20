#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "01-numeric/lowpass_kernel.hpp"

namespace ps::plugin_internal::numeric_ops {
// Exact dyadic tap classification, independent of coefficient enclosures.
inline int uniform_tap_sign(const LowpassParameters& p, unsigned radius,
                            unsigned offset) {
  if (!offset || p.kernel == LowpassKernel::Gaussian)
    return 1;
  if (offset == radius &&
      (p.kernel == LowpassKernel::Hann || p.kernel == LowpassKernel::Blackman))
    return 0;
  const auto cutoff = BinaryParts::decode(p.cutoff, false);
  const auto product =
      static_cast<unsigned __int128>(cutoff.significand) * offset;
  const auto fraction = static_cast<unsigned>(-cutoff.exponent - 1);
  // cutoff<1/2 gives exponent<=-54, so no left shift is required.
  if (fraction >= 128)
    return 1;
  const auto integer = product >> fraction;
  const auto remainder =
      product & ((static_cast<unsigned __int128>(1) << fraction) - 1);
  if (!remainder)
    return 0;
  return (integer & 1) ? -1 : 1;
}
struct UniformLowpassMath {
  DirectedLowpassKernel kernel;
  using Frame = DirectedInterval::Frame;
  Result<std::uint64_t> evaluate(
      const LowpassParameters& parameters, unsigned radius,
      const std::uint64_t* samples, bool narrow,
      const std::function<Status(std::uint64_t)>& consume) {
    using Answer = Result<std::uint64_t>;
    auto& m = kernel.functions.math;
    m.consume = &consume;
    struct End {
      DirectedInterval& m;
      ~End() {
        m.consume = nullptr;
        m.used = 0;
      }
    } end{m};
    const auto signbit = UINT64_C(1) << (narrow ? 31 : 63);
    const auto inf =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    const auto quiet = UINT64_C(1) << (narrow ? 22 : 51);
    std::optional<std::uint64_t> first_nan;
    unsigned infinities = 0;
    bool constant = true;
    const auto center = samples[radius];
    try {
      for (unsigned i = 0; i <= 2 * radius; ++i) {
        m.work(1);
        const auto distance = i < radius ? radius - i : i - radius;
        const auto sign = uniform_tap_sign(parameters, radius, distance);
        if (!sign)
          continue;
        const auto value = BinaryParts::decode(samples[i], narrow);
        constant = constant && value.bits == center;
        if (value.nan && !first_nan)
          first_nan = value.bits | quiet;
        if (value.infinite)
          infinities |= (value.negative != (sign < 0)) ? 2 : 1;
      }
      std::optional<std::uint64_t> shortcut;
      if (first_nan) {
        shortcut = first_nan;
      } else if (infinities) {
        shortcut = infinities == 3 ? inf | quiet
                                   : inf | (infinities == 2 ? signbit : 0);
      } else if (constant) {
        shortcut = center;
      } else {
        // In exact binary64 units, paired taps can prove the entire result
        // equals center independently of the even kernel: a+b=2*center.
        m.used = 0;
        m.precision = 1074;
        Frame frame(m);
        auto target = m.interval(), a = m.interval(), b = m.interval(),
             sum = m.interval();
        m.raw(target, center, narrow);
        m.scale(target, target, 1);
        bool paired = true;
        for (unsigned j = 1; j <= radius && paired; ++j) {
          if (!uniform_tap_sign(parameters, radius, j))
            continue;
          m.raw(a, samples[radius - j], narrow);
          m.raw(b, samples[radius + j], narrow);
          m.add(sum, a, b);
          paired = m.compare(sum.low, target.low) == 0 &&
                   m.compare(sum.high, target.high) == 0;
        }
        if (paired)
          shortcut = center & (signbit - 1) ? center : UINT64_C(0);
      }
      for (unsigned precision = 128; precision <= 4096; precision *= 2) {
        m.used = 0;
        m.precision = precision;
        try {
          Frame frame(m);
          auto numerator = m.interval(), denominator = m.interval(),
               coefficient = m.interval(), source = m.interval(),
               temp = m.interval(), u = m.interval(), quotient = m.interval();
          m.integer(numerator, 0);
          m.integer(denominator, 0);
          for (unsigned j = 0; j <= radius; ++j) {
            if (!uniform_tap_sign(parameters, radius, j))
              continue;
            m.integer(u, j);
            m.divide_small(u, u, radius);
            kernel.coefficient(coefficient, u, parameters);
            if (j)
              m.scale(temp, coefficient, 1);
            else
              m.copy(temp, coefficient);
            m.add(denominator, denominator, temp);
            if (!shortcut) {
              m.raw(source, samples[radius + j], narrow);
              if (j) {
                m.raw(temp, samples[radius - j], narrow);
                m.add(source, source, temp);
              }
              m.multiply(temp, coefficient, source);
              m.add(numerator, numerator, temp);
            }
          }
          if (denominator.high.negative || m.top(denominator.high) < 0)
            return Answer(Status{ErrorCode::InvalidArgument,
                                 "nonpositive lowpass normalizer",
                                 FailureReason::InvalidDomain});
          if (denominator.low.negative || m.top(denominator.low) < 0)
            continue;
          if (shortcut)
            return Answer(*shortcut);
          m.divide(quotient, numerator, denominator);
          const auto low = m.rounded(quotient.low, narrow),
                     high = m.rounded(quotient.high, narrow);
          if (low == high)
            return Answer(low);
        } catch (const DirectedInterval::Unresolved&) {
          // Retry complete coefficients and sum at higher precision.
        }
      }
      return Answer(Status{ErrorCode::ResourceExhausted,
                           "uniform lowpass rounding could not be certified",
                           FailureReason::CapacityLimit});
    } catch (const Status& status) {
      return Answer(status);
    }
  }
};
}  // namespace ps::plugin_internal::numeric_ops
