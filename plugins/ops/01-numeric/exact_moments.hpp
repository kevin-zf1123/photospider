#pragma once

#include <cstdint>
#include <functional>

#include "01-numeric/exact_aggregate.hpp"
#include "01-numeric/exact_root.hpp"

namespace ps::plugin_internal::numeric_ops {
// T=Sum((x*2^1074)^2). The sum accumulator uses integer units for integer
// inputs, then aligns once at finalization. For N<=2^40, |S|/T/(N*T,S*S) need
// at most 2138/4236/4276 bits for binary64. N*(N-ddof) is <=2^80 (81 bits).
// Direct root midpoint products need at most 4280 bits; all fit 4352 bits.
// The caller validates source/destination, 1<=N<=2^40 and ddof<N before reads,
// then adds exactly N values. finish is destructive; reset before another
// group.
struct ExactMoments final {
  using Integer = RatioWorkspace::Integer;
  ExactAggregate sum;
  Integer squares, aligned_sum;
  ExactMoments(SequenceProfile profile, ElementType source)
      : sum(profile, AggregateKind::Sum, source) {}
  void reset() {
    sum.reset();
    squares.words.fill(0);
    aligned_sum.words.fill(0);
  }
  Status add(std::uint64_t bits,
             const std::function<Status(std::uint64_t)>& consume) {
    auto status = sum.add(bits, consume);
    if (!status.ok())
      return status;
    status = consume(256);
    if (!status.ok())
      return status;
    const bool floating =
        sum.type == ElementType::Float32 || sum.type == ElementType::Float64;
    if (floating) {
      const auto parts =
          BinaryParts::decode(bits, sum.type == ElementType::Float32);
      if (parts.nan || parts.infinite)
        return Status::success();
      sum.ratio.term.set_product(parts, parts);
      squares.add(sum.ratio.term);
    } else {
      const auto magnitude = sum.type == ElementType::Int64 && (bits >> 63)
                                 ? UINT64_C(0) - bits
                                 : bits;
      integer_coefficient(static_cast<unsigned __int128>(magnitude) * magnitude,
                          &sum.ratio.term);
      if (!RatioWorkspace::shift(sum.ratio.term, 2148, &sum.ratio.shifted))
        return Status{ErrorCode::ResourceExhausted, "integer square capacity",
                      FailureReason::CapacityLimit};
      squares.add(sum.ratio.shifted);
    }
    return Status::success();
  }
  Result<std::uint64_t> finish(
      ElementType destination, std::uint64_t count, std::uint64_t ddof,
      bool deviation, const std::function<Status(std::uint64_t)>& consume) {
    using Answer = Result<std::uint64_t>;
    auto status = consume(1024);
    if (!status.ok())
      return Answer(status);
    const bool narrow = destination == ElementType::Float32;
    if (sum.has_nan)
      return Answer(converted_nan(sum.first_nan, sum.type, destination));
    if (sum.positive_inf || sum.negative_inf)
      return Answer(narrow ? UINT64_C(0x7fc00000)
                           : UINT64_C(0x7ff8000000000000));
    if (!count || ddof >= count)
      return Answer(Status{ErrorCode::InvalidArgument,
                           "invalid moment degrees of freedom"});
    auto& ratio = sum.ratio;
    if (sum.type == ElementType::Float32 || sum.type == ElementType::Float64)
      aligned_sum = ratio.numerator;
    else if (!RatioWorkspace::shift(ratio.numerator, 1074, &aligned_sum))
      return Answer(Status{ErrorCode::ResourceExhausted,
                           "integer sum alignment capacity",
                           FailureReason::CapacityLimit});
    status = multiply_fixed(aligned_sum, aligned_sum, &ratio.term, consume);
    if (!status.ok())
      return Answer(status);
    integer_coefficient(count, &ratio.candidate);
    status =
        multiply_fixed(squares, ratio.candidate, &ratio.numerator, consume);
    if (!status.ok())
      return Answer(status);
    if (ratio.compare(ratio.numerator, ratio.term) < 0)
      return Answer(
          Status{ErrorCode::Internal, "exact moment numerator is negative"});
    ratio.numerator.subtract(ratio.term);
    ratio.negative = false;
    integer_coefficient(static_cast<unsigned __int128>(count) * (count - ddof),
                        &ratio.denominator);
    return deviation ? round_sqrt_ratio(&ratio, narrow, -2148, consume)
                     : ratio.round(narrow, consume, -2148);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
