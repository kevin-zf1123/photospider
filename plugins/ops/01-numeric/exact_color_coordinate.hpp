#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>

#include "01-numeric/directed_interval.hpp"

namespace ps::plugin_internal::numeric_ops {
// One complete linear coordinate, including an optional symbolic pi factor.
// Float64 source integers use units 2^-1074. Weighted floating numerators need
// <4200 bits; weighted Int64 p/q numerators/denominators need <2230 bits. The
// 9216-bit arena also covers IEEE rounding alignment. Pi import/division at
// <=4096 fractional bits fits the separate 12288-bit directed workspace.
class ExactColorCoordinate final {
  static constexpr std::size_t kWords = 144, kSlots = 24;
  using Integer = FixedInteger<kWords>;
  struct Signed {
    Integer magnitude;
    bool negative = false;
  };
  std::array<Signed, kSlots> slots_{};
  ExactRatioWorkspace<kWords> ratio_;
  DirectedInterval math_;
  unsigned used_ = 0;
  const std::function<Status(std::uint64_t)>* consume_ = nullptr;
  void work(std::uint64_t count) const {
    auto status = (*consume_)(count);
    if (!status.ok())
      throw status;
  }
  unsigned allocate() {
    work(kWords);
    if (used_ == kSlots)
      DirectedInterval::capacity();
    slots_[used_] = {};
    return used_++;
  }
  bool zero(unsigned index) const {
    return ExactRatioWorkspace<kWords>::top(slots_[index].magnitude) < 0;
  }
  unsigned integer(std::uint64_t value, bool negative = false) {
    const auto result = allocate();
    slots_[result].magnitude.words[0] = value;
    slots_[result].negative = negative && value;
    return result;
  }
  unsigned signed_integer(std::int64_t value) {
    const auto raw = static_cast<std::uint64_t>(value);
    return integer(value < 0 ? UINT64_C(0) - raw : raw, value < 0);
  }
  unsigned binary(std::uint64_t bits) {
    const auto result = allocate();
    const auto value = BinaryParts::decode(bits, false);
    slots_[result].magnitude.set(value, 1074);
    slots_[result].negative = value.negative && value.magnitude;
    return result;
  }
  unsigned add(unsigned a, unsigned b, bool subtract = false) {
    const auto out = allocate();
    const auto& left = slots_[a];
    const auto& right = slots_[b];
    const bool negative = right.negative != subtract;
    auto& result = slots_[out];
    work(kWords * 3);
    if (left.negative == negative) {
      result = left;
      result.magnitude.add(right.magnitude);
    } else if (ratio_.compare(left.magnitude, right.magnitude) >= 0) {
      result = left;
      result.magnitude.subtract(right.magnitude);
    } else {
      result = right;
      result.negative = negative;
      result.magnitude.subtract(left.magnitude);
    }
    if (zero(out))
      result.negative = false;
    return out;
  }
  unsigned multiply(unsigned a, unsigned b) {
    const auto out = allocate();
    auto status = multiply_fixed(slots_[a].magnitude, slots_[b].magnitude,
                                 &slots_[out].magnitude, *consume_);
    if (!status.ok())
      throw status;
    slots_[out].negative =
        slots_[a].negative != slots_[b].negative && !zero(out);
    return out;
  }
  void import(DirectedInterval::Interval output, unsigned value) {
    math_.work(kWords + DirectedInterval::kWords);
    std::copy(slots_[value].magnitude.words.begin(),
              slots_[value].magnitude.words.end(),
              output.low.magnitude.words.begin());
    // Both integers have the same implicit Q_precision scale, which cancels
    // in divide. Keep magnitudes positive to restore signed underflow exactly.
    math_.copy(output.high, output.low);
  }
  Result<std::uint64_t> finish(unsigned numerator, unsigned denominator,
                               int scale, int pi_power, bool narrow,
                               bool negative_zero) {
    const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
    if (zero(numerator))
      return Result<std::uint64_t>(negative_zero ? sign : 0);
    if (!pi_power) {
      work(kWords * 2);
      ratio_.numerator = slots_[numerator].magnitude;
      ratio_.denominator = slots_[denominator].magnitude;
      ratio_.negative = slots_[numerator].negative;
      return ratio_.round(narrow, *consume_, scale);
    }
    for (unsigned precision = 128; precision <= 4096; precision *= 2) {
      work(1);
      math_.precision = precision;
      DirectedInterval::Frame frame(math_);
      auto n = math_.interval(), d = math_.interval(), ratio = math_.interval();
      auto pi = math_.interval(), converted = math_.interval();
      import(n, numerator);
      import(d, denominator);
      math_.divide(ratio, n, d);
      math_.constant(pi, true);
      if (pi_power > 0)
        math_.multiply(converted, ratio, pi);
      else
        math_.divide(converted, ratio, pi);
      math_.scale(converted, converted, scale);
      const auto low = math_.rounded(converted.low, narrow);
      const auto high = math_.rounded(converted.high, narrow);
      if (low == high)
        return Result<std::uint64_t>(low |
                                     (slots_[numerator].negative ? sign : 0));
    }
    return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted,
                                        "color hue pi rounding precision limit",
                                        FailureReason::CapacityLimit});
  }

 public:
  explicit ExactColorCoordinate(SequenceProfile profile)
      : ratio_(profile), math_(profile) {}
  // c is exact-widened binary64 when rational=false; otherwise p/q are original
  // Int64 values with positive q. direct means selected OR complete stored-row
  // identity, never merely one equal component. pi_power is -1,0,+1.
  Result<std::uint64_t> evaluate(
      const std::array<std::uint64_t, 2>& stops, std::uint64_t query,
      const std::array<std::uint64_t, 2>& c,
      const std::array<std::int64_t, 2>& p,
      const std::array<std::int64_t, 2>& q, bool rational, bool direct,
      int pi_power, bool narrow,
      const std::function<Status(std::uint64_t)>& consume) {
    used_ = 0;
    math_.used = 0;
    consume_ = &consume;
    math_.consume = &consume;
    struct End {
      ExactColorCoordinate& self;
      ~End() {
        self.consume_ = nullptr;
        self.math_.consume = nullptr;
      }
    } end{*this};
    try {
      unsigned numerator, denominator;
      if (direct) {
        numerator = rational ? signed_integer(p[0]) : binary(c[0]);
        denominator = integer(rational ? static_cast<std::uint64_t>(q[0]) : 1);
      } else {
        const auto s0 = binary(stops[0]), s1 = binary(stops[1]),
                   x = binary(query);
        const auto u = add(s1, x, true), v = add(x, s0, true), h = add(u, v);
        if (rational) {
          const auto p0 = signed_integer(p[0]), p1 = signed_integer(p[1]);
          const auto q0 = integer(static_cast<std::uint64_t>(q[0]));
          const auto q1 = integer(static_cast<std::uint64_t>(q[1]));
          numerator =
              add(multiply(multiply(u, p0), q1), multiply(multiply(v, p1), q0));
          denominator = multiply(multiply(h, q0), q1);
        } else {
          const auto c0 = binary(c[0]), c1 = binary(c[1]);
          numerator = add(multiply(u, c0), multiply(v, c1));
          denominator = h;
        }
      }
      return finish(numerator, denominator, rational ? 0 : -1074, pi_power,
                    narrow, direct && !rational && c[0] == (UINT64_C(1) << 63));
    } catch (const Status& status) {
      return Result<std::uint64_t>(status);
    } catch (const DirectedInterval::Unresolved&) {
      return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted,
                                          "color hue unresolved interval",
                                          FailureReason::CapacityLimit});
    }
  }
};
}  // namespace ps::plugin_internal::numeric_ops
