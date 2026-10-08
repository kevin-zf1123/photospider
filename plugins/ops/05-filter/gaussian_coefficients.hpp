#pragma once

#include <cstdint>

#include "01-numeric/directed_functions.hpp"

namespace ps::plugin_internal::filter_ops {
// The owner admits this complete arena before construction. Coefficients are
// generated at runtime, under the supplied work/cancellation consumer.
struct GaussianCoefficients final {
  numeric_ops::DirectedFunctions functions;
  GaussianCoefficients() : functions(numeric_ops::SequenceProfile::Strict) {}
  explicit GaussianCoefficients(
      const core_internal::WorkConsumer& initialization)
      : functions(numeric_ops::SequenceProfile::Strict, &initialization) {}
  using Wide = unsigned __int128;
  static unsigned bits(Wide value) {
    const auto high = static_cast<std::uint64_t>(value >> 64);
    return high ? 128U - __builtin_clzll(high)
                : 64U - __builtin_clzll(static_cast<std::uint64_t>(value));
  }
  // Nonzero dyadics. Equal top exponents bound the alignment to 127 bits,
  // so the shorter significand can be shifted without losing a high bit.
  static int compare(Wide a, int ae, Wide b, int be) {
    const auto ab = bits(a), bb = bits(b);
    const int atop = ae + static_cast<int>(ab),
              btop = be + static_cast<int>(bb);
    if (atop != btop)
      return atop < btop ? -1 : 1;
    if (ab < bb)
      a <<= bb - ab;
    else
      b <<= ab - bb;
    return a < b ? -1 : a > b ? 1 : 0;
  }
  Result<std::uint64_t> coefficient(
      std::uint64_t sigma_bits, std::uint64_t offset,
      const core_internal::WorkConsumer& consume) {
    using Answer = Result<std::uint64_t>;
    using Math = numeric_ops::DirectedInterval;
    const auto sigma = numeric_ops::BinaryParts::decode(sigma_bits, false);
    if (sigma.nan || sigma.infinite || (sigma.negative && sigma.magnitude) ||
        (!sigma.magnitude && offset) || offset > INT64_MAX)
      return Answer(Status{ErrorCode::InvalidArgument,
                           "invalid Gaussian sigma or offset"});
    auto& math = functions.math;
    math.product_checkpoint_words = 32;
    math.consume = consume;
    struct Reset final {
      Math& math;
      ~Reset() {
        math.consume = nullptr;
        math.used = 0;
      }
    } reset{math};
    try {
      consume.check(64);
      if (!offset)
        return Answer(UINT64_C(0x3ff0000000000000));
      const Wide numerator = static_cast<Wide>(offset) * offset;
      const Wide square =
          static_cast<Wide>(sigma.significand) * sigma.significand;
      // u = j^2/(2*sigma^2). u>=1024 certifies RN64(exp(-u))=+0;
      // u<=2^-54 gives exp(-u)>1-u>=1-2^-54, hence RN64=1.
      // These exact comparisons also prevent extreme sigma from exhausting
      // an interval arena just to establish its obvious destination range.
      if (compare(numerator, 0, square, 2 * sigma.exponent + 11) >= 0)
        return Answer(UINT64_C(0));
      if (compare(numerator, 0, square, 2 * sigma.exponent - 53) <= 0)
        return Answer(UINT64_C(0x3ff0000000000000));
      for (unsigned precision = 128; precision <= 4096; precision *= 2) {
        math.used = 0;
        math.precision = precision;
        try {
          Math::Frame frame(math);
          auto j = math.interval(), s = math.interval(),
               ratio = math.interval(), exponent = math.interval(),
               result = math.interval();
          math.unsigned_integer(j, numerator);
          math.unsigned_integer(s, square);
          math.divide(ratio, j, s);
          math.scale(exponent, ratio, -2 * sigma.exponent - 1);
          math.negate(exponent, exponent);
          functions.exponential(result, exponent);
          const auto low = math.rounded(result.low, false);
          const auto high = math.rounded(result.high, false);
          if (low == high)
            return Answer(low);
        } catch (const Math::Unresolved&) {
          // Retry the complete expression, retaining all issued work.
        } catch (const numeric_ops::DirectedFunctions::RangeResult& range) {
          if (!range.overflow)
            return Answer(UINT64_C(0));
          return Answer(Status{ErrorCode::Internal,
                               "positive Gaussian exponent enclosure"});
        }
      }
      return Answer(Status{ErrorCode::ResourceExhausted,
                           "Gaussian coefficient refinement limit",
                           FailureReason::CapacityLimit});
    } catch (const Status& status) {
      return Answer(status);
    }
  }
};
}  // namespace ps::plugin_internal::filter_ops
