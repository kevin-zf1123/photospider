#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "01-numeric/exact_ratio.hpp"
#include "01-numeric/numeric_nan.hpp"
#include "photospider/data/value.hpp"

namespace ps::plugin_internal::numeric_ops {
enum class AggregateKind { Sum, Minimum, Maximum };
// Caller-owned bounded scratch. At most 2^40+1 finite binary64 terms, measured
// in 2^-1074 units, have every prefix magnitude <2^2139. The 4352-bit ratio
// workspace covers that bound and its final RN conversion. Integer prefixes
// need at most 104 bits. No intermediate overflow or floating arithmetic
// occurs. finish is destructive; reset before starting another aggregate.
struct ExactAggregate final {
  RatioWorkspace ratio;
  std::array<std::uint64_t, 4> left{}, right{};
  std::array<std::int64_t, 4> greater{}, less{};
  AggregateKind kind;
  ElementType type;
  std::uint64_t selected = 0, first_nan = 0;
  bool initialized = false, has_nan = false, positive_inf = false,
       negative_inf = false, all_negative_zero = true;
  ExactAggregate(SequenceProfile profile, AggregateKind operation,
                 ElementType dtype)
      : ratio(profile), kind(operation), type(dtype) {}
  void reset() {
    ratio.numerator.words.fill(0);
    ratio.negative = false;
    initialized = has_nan = positive_inf = negative_inf = false;
    all_negative_zero = true;
    selected = first_nan = 0;
  }
  Status add(std::uint64_t bits,
             const std::function<Status(std::uint64_t)>& consume) {
    auto charged = consume(512);
    if (!charged.ok())
      return charged;
    const bool narrow = type == ElementType::Float32;
    const bool floating = narrow || type == ElementType::Float64;
    const auto parts =
        floating ? BinaryParts::decode(bits, narrow) : BinaryParts{};
    if (!floating)
      all_negative_zero = false;
    if (floating) {
      all_negative_zero &= parts.negative && !parts.magnitude;
      if (parts.nan) {
        if (!has_nan)
          first_nan = bits;
        has_nan = true;
        return Status::success();
      }
      if (parts.infinite) {
        negative_inf |= parts.negative;
        positive_inf |= !parts.negative;
      }
    }
    if (kind == AggregateKind::Sum) {
      if (floating) {
        if (!parts.infinite) {
          ratio.term.set(parts, 1074);
          ratio.add_term(parts.negative);
        }
      } else {
        const bool negative = type == ElementType::Int64 && (bits >> 63);
        ratio.term.words.fill(0);
        ratio.term.words[0] = negative ? UINT64_C(0) - bits : bits;
        ratio.add_term(negative);
      }
      return Status::success();
    }
    if (!initialized) {
      selected = bits;
      initialized = true;
      return Status::success();
    }
    if (floating && !parts.magnitude &&
        !BinaryParts::decode(selected, narrow).magnitude) {
      selected =
          kind == AggregateKind::Minimum ? selected | bits : selected & bits;
      return Status::success();
    }
    const auto key = [&](std::uint64_t raw) {
      return floating ? BinaryParts::decode(raw, narrow).order_key()
             : type == ElementType::Int64 ? raw ^ (UINT64_C(1) << 63)
                                          : raw;
    };
    left[0] = key(bits);
    right[0] = key(selected);
    compare_keys(left.data(), right.data(), greater.data(), less.data(),
                 ratio.profile);
    if (kind == AggregateKind::Minimum ? less[0] : greater[0])
      selected = bits;
    return Status::success();
  }
  Result<std::uint64_t> finish(
      const std::function<Status(std::uint64_t)>& consume) {
    return finish_as(type, 1, consume);
  }
  // Caller validates destination domain and positive divisor. Numeric sum and
  // mean may change float width or widen/narrow integer sums; min/max preserve
  // their source dtype. Source integers enter a floating mean without casts.
  Result<std::uint64_t> finish_as(
      ElementType destination, std::uint64_t divisor,
      const std::function<Status(std::uint64_t)>& consume) {
    using Answer = Result<std::uint64_t>;
    auto charged = consume(128);
    if (!charged.ok())
      return Answer(charged);
    const bool narrow = destination == ElementType::Float32;
    const bool floating = narrow || destination == ElementType::Float64;
    if (has_nan)
      return Answer(converted_nan(first_nan, type, destination));
    if (kind != AggregateKind::Sum)
      return Answer(selected);
    if (!floating) {
      const auto magnitude = ratio.numerator.words[0];
      const auto limit = destination == ElementType::UInt8 ? UINT64_C(255)
                         : ratio.negative                  ? UINT64_C(1) << 63
                                          : UINT64_C(0x7fffffffffffffff);
      if (RatioWorkspace::top(ratio.numerator) > 63 || magnitude > limit ||
          (destination == ElementType::UInt8 && ratio.negative && magnitude))
        return Answer(Status{ErrorCode::OperationFailed,
                             "integer aggregate overflow",
                             FailureReason::ArithmeticOverflow});
      return Answer(ratio.negative ? UINT64_C(0) - magnitude : magnitude);
    }
    const auto infinity =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
    if (positive_inf && negative_inf)
      return Answer(infinity | (UINT64_C(1) << (narrow ? 22 : 51)));
    if (positive_inf || negative_inf)
      return Answer(infinity | (negative_inf ? sign : 0));
    if (RatioWorkspace::top(ratio.numerator) < 0)
      return Answer(all_negative_zero ? sign : 0);
    ratio.denominator.words.fill(0);
    ratio.denominator.words[0] = divisor;
    return ratio.round(
        narrow, consume,
        type == ElementType::Float32 || type == ElementType::Float64 ? -1074
                                                                     : 0);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
