#pragma once

#include <cstddef>
#include <cstdint>

#include "01-numeric/exact_product.hpp"
#include "01-numeric/numeric_nan.hpp"

namespace ps::plugin_internal::filter_ops {
// kx/ky are nonnegative finite baked64 coefficients <=1. Samples may be any
// binary32/64 value. At most UINT64_MAX contributing taps are admitted. In
// units of 2^-1074, |N| < A*2^4246 < 2^4310 and D <= A*2^2148 < 2^2212.
// General ratio rounding aligns operands no higher than max(Ntop,Dtop+948),
// including the binary32 subnormal divisor shift (925) and quotient (23).
// with one extra remainder bit. Thus 68 words suffice, including division.
struct GaussianExact final {
  using Integer = numeric_ops::FixedInteger<68>;
  numeric_ops::ExactRatioWorkspace<68> ratio{
      numeric_ops::SequenceProfile::Strict};
  Integer x, y, value;
  std::uint64_t first_nan = 0;
  unsigned infinities = 0;
  bool all_negative_zero = true, narrow = false;
  std::uint64_t remaining = 0;

  // Baked weights need <=18 limbs in 2^-1074 units, their product <=34,
  // and any finite binary64 sample <=33. Each axis sum also fits 18 limbs
  // when the two-dimensional tap count is <=UINT64_MAX. multiply_fixed bills
  // 4*68 + a_words*(16*b_words+1); general RN64 division bills <=14080.
  // The bound includes zero/special shortcuts and never refunds unused work.
  static Result<std::uint64_t> work_bound(std::uint64_t nx, std::uint64_t ny) {
    using Answer = Result<std::uint64_t>;
    if (!nx || !ny || nx > UINT64_MAX / ny || nx > UINT64_MAX - ny)
      return Answer(
          Status{ErrorCode::ResourceExhausted, "Gaussian work overflow"});
    constexpr std::uint64_t fixed = 19827, per_weight = 204, per_tap = 24140;
    if (nx + ny > (UINT64_MAX - fixed) / per_weight)
      return Answer(
          Status{ErrorCode::ResourceExhausted, "Gaussian work overflow"});
    const auto base = fixed + (nx + ny) * per_weight;
    if (nx * ny > (UINT64_MAX - base) / per_tap)
      return Answer(
          Status{ErrorCode::ResourceExhausted, "Gaussian work overflow"});
    return Answer(base + nx * ny * per_tap);
  }

  Status begin(const std::uint64_t* kx, std::uint64_t nx,
               const std::uint64_t* ky, std::uint64_t ny, bool output_narrow,
               const execution_internal::WorkConsumer& consume) {
    if (!nx || !ny || nx > UINT64_MAX / ny)
      return {ErrorCode::ResourceExhausted, "Gaussian tap product overflow"};
    const auto status = consume(68 * 4);
    if (!status.ok())
      return status;
    ratio.numerator.words.fill(0);
    ratio.negative = false;
    x.words.fill(0);
    y.words.fill(0);
    first_nan = infinities = 0;
    all_negative_zero = true;
    narrow = output_narrow;
    remaining = nx * ny;
    for (unsigned axis = 0; axis < 2; ++axis) {
      auto& sum = axis ? y : x;
      const auto* weights = axis ? ky : kx;
      const auto count = axis ? ny : nx;
      for (std::uint64_t i = 0; i < count; ++i) {
        const auto status = consume(68 * 3);
        if (!status.ok())
          return status;
        const auto weight = numeric_ops::BinaryParts::decode(weights[i], false);
        if (weight.negative || weight.nan || weight.infinite ||
            weights[i] > UINT64_C(0x3ff0000000000000))
          return {ErrorCode::InvalidArgument,
                  "invalid baked64 Gaussian weight"};
        value.set(weight, 1074);
        sum.add(value);
      }
    }
    if (decltype(ratio)::top(x) < 0 || decltype(ratio)::top(y) < 0)
      return {ErrorCode::InvalidArgument, "zero Gaussian normalizer"};
    return numeric_ops::multiply_fixed(x, y, &ratio.denominator, consume, 32);
  }
  // Call in logical kernel row-major order, including repeated boundary taps.
  // The caller must still perform every authorized read after a NaN/Inf.
  Status add(std::uint64_t kx, std::uint64_t ky, std::uint64_t raw,
             bool input_narrow,
             const execution_internal::WorkConsumer& consume) {
    if (!remaining)
      return {ErrorCode::Internal, "Gaussian tap count exceeded"};
    --remaining;
    auto status = consume(68 * 3);
    if (!status.ok())
      return status;
    if (!kx || !ky)
      return Status::success();
    const auto sample = numeric_ops::BinaryParts::decode(raw, input_narrow);
    if (sample.nan && !first_nan)
      first_nan = numeric_ops::converted_nan(
          raw, input_narrow ? ElementType::Float32 : ElementType::Float64,
          narrow ? ElementType::Float32 : ElementType::Float64);
    if (sample.infinite)
      infinities |= sample.negative ? 2 : 1;
    all_negative_zero =
        all_negative_zero && sample.negative && !sample.magnitude;
    if (sample.nan || sample.infinite || first_nan || infinities)
      return Status::success();
    // Three IEEE significands have at most 159 bits. Multiply at bit zero,
    // then place the exact product in the existing 2^-3222 accumulator.
    // This avoids multiplying the thousands of zero bits below each operand.
    const auto wx = numeric_ops::BinaryParts::decode(kx, false);
    const auto wy = numeric_ops::BinaryParts::decode(ky, false);
    status = consume(12);
    if (!status.ok())
      return status;
    const auto weight =
        static_cast<unsigned __int128>(wx.significand) * wy.significand;
    const auto low =
        static_cast<unsigned __int128>(static_cast<std::uint64_t>(weight)) *
        sample.significand;
    const auto high = (weight >> 64) * sample.significand + (low >> 64);
    const std::array<std::uint64_t, 3> compact{
        static_cast<std::uint64_t>(low), static_cast<std::uint64_t>(high),
        static_cast<std::uint64_t>(high >> 64)};
    const auto shift = static_cast<unsigned>(wx.exponent + wy.exponent +
                                             sample.exponent + 3222);
    const auto whole = shift / 64, tail = shift % 64;
    ratio.term.words.fill(0);
    for (unsigned i = 0; i < compact.size(); ++i) {
      ratio.term.words[whole + i] |= compact[i] << tail;
      if (tail)
        ratio.term.words[whole + i + 1] |= compact[i] >> (64 - tail);
    }
    status = consume(68 * 3);
    if (!status.ok())
      return status;
    ratio.add_term(sample.negative);
    return Status::success();
  }
  Result<std::uint64_t> finish(
      const execution_internal::WorkConsumer& consume) {
    using Answer = Result<std::uint64_t>;
    const auto status = consume(1);
    if (!status.ok())
      return Answer(status);
    if (first_nan)
      return Answer(first_nan);
    const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
    const auto inf =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    const auto quiet = UINT64_C(1) << (narrow ? 22 : 51);
    if (infinities)
      return Answer(infinities == 3 ? inf | quiet
                                    : inf | (infinities == 2 ? sign : 0));
    if (all_negative_zero)
      return Answer(sign);
    return ratio.round(narrow, consume, -1074);
  }
};
}  // namespace ps::plugin_internal::filter_ops
