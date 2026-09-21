#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>

#include "01-numeric/accelerated_curve.hpp"
#include "01-numeric/exact_product.hpp"

namespace ps::plugin_internal::numeric_ops {
// All inputs are finite binary64 bits. X/Y are integers in units 2^-1074.
// Differences have <2099 bits; PCHIP slope numerators/denominators have <6300
// bits, and the combined Hermite numerator/denominator have <21000/18900 bits.
// 22528 bits also cover division alignment, 53 quotient bits and midpoint
// doubling. Inverse comparison uses 2^-1075 units: with B=2100,
// slope a/b <2^6303, Hermite N <2^21009 and N-target*D <2^21010.
// Segment-interior comparisons and at most 64 live slots fit this same arena.
// Every temporary is in the caller-owned fixed arena.
class ExactCurve final {
  static constexpr std::size_t kWords = 352, kSlots = 96;
  using Integer = FixedInteger<kWords>;
  struct Signed {
    Integer magnitude;
    bool negative = false;
  };
  struct Rational {
    unsigned numerator, denominator;
  };
  ExactRatioWorkspace<kWords> ratio_;
  std::array<Signed, kSlots> slots_{};
  unsigned used_ = 0;
  Status status_;
  const std::function<Status(std::uint64_t)>* consume_ = nullptr;
  unsigned allocate() {
    if (!status_.ok())
      return 0;
    status_ = (*consume_)(kWords);
    if (used_ == kSlots)
      status_ = {ErrorCode::ResourceExhausted, "curve arithmetic arena",
                 FailureReason::CapacityLimit};
    if (!status_.ok())
      return 0;
    slots_[used_] = {};
    return used_++;
  }
  unsigned integer(unsigned value) {
    auto out = allocate();
    if (status_.ok())
      slots_[out].magnitude.words[0] = value;
    return out;
  }
  unsigned input(std::uint64_t bits, unsigned fraction = 1074) {
    auto out = allocate();
    if (status_.ok()) {
      const auto value = BinaryParts::decode(bits, false);
      slots_[out].magnitude.set(value, fraction);
      slots_[out].negative = value.negative && value.magnitude;
    }
    return out;
  }
  bool zero(unsigned value) const {
    return ExactRatioWorkspace<kWords>::top(slots_[value].magnitude) < 0;
  }
  int sign(unsigned value) const {
    return zero(value) ? 0 : slots_[value].negative ? -1 : 1;
  }
  unsigned add(unsigned a, unsigned b, bool subtract = false) {
    auto out = allocate();
    if (!status_.ok())
      return out;
    const auto& left = slots_[a];
    const auto& right = slots_[b];
    auto& result = slots_[out];
    const bool negative = right.negative != subtract;
    if (left.negative == negative) {
      result = left;
      // The algebraic capacity proof bounds every sum before this operation.
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
    auto out = allocate();
    if (status_.ok()) {
      status_ = multiply_fixed(slots_[a].magnitude, slots_[b].magnitude,
                               &slots_[out].magnitude, *consume_);
      slots_[out].negative =
          slots_[a].negative != slots_[b].negative && !zero(out);
    }
    return out;
  }
  bool collinear(const std::array<unsigned, 3>& h,
                 const std::array<unsigned, 3>& dy, unsigned count) {
    // Equal exact secants make every selected PCHIP slope the same secant.
    // Cross products retain information lost by rounded floating division.
    for (unsigned i = 1; i + 1 < count; ++i) {
      const auto saved = used_;
      const auto a = multiply(dy[0], h[i]), b = multiply(dy[i], h[0]);
      const bool equal =
          status_.ok() && slots_[a].negative == slots_[b].negative &&
          ratio_.compare(slots_[a].magnitude, slots_[b].magnitude) == 0;
      used_ = saved;
      if (!equal)
        return false;
    }
    return true;
  }
  Rational slope(unsigned node, unsigned first, unsigned knots,
                 const std::array<unsigned, 3>& h,
                 const std::array<unsigned, 3>& dy) {
    const auto saved = used_;
    const auto out_a = allocate(), out_b = allocate();
    unsigned a = 0, b = 0;
    if (knots == 2) {
      a = dy[0];
      b = h[0];
    } else if (node && node + 1 != knots) {
      const auto j = node - first;
      if (!sign(dy[j - 1]) || sign(dy[j - 1]) != sign(dy[j])) {
        a = integer(0);
        b = integer(1);
      } else {
        const auto h0 = h[j - 1], h1 = h[j];
        const auto y0 = dy[j - 1], y1 = dy[j];
        const auto w1 = add(add(h1, h1), h0);
        const auto w2 = add(h1, add(h0, h0));
        a = multiply(multiply(multiply(integer(3), add(h0, h1)), y0), y1);
        b = add(multiply(multiply(w1, h0), y1), multiply(multiply(w2, h1), y0));
      }
    } else {
      const unsigned j0 = node ? node - first - 1 : 0;
      const unsigned j1 = node ? j0 - 1 : 1;
      const auto h0 = h[j0], h1 = h[j1], y0 = dy[j0], y1 = dy[j1];
      a = add(multiply(multiply(add(add(h0, h0), h1), y0), h1),
              multiply(multiply(h0, h0), y1), true);
      b = multiply(multiply(h0, h1), add(h0, h1));
      if (sign(a) != sign(y0)) {
        a = integer(0);
        b = integer(1);
      } else if (sign(y0) != sign(y1)) {
        const auto left = multiply(a, h0);
        const auto triple = multiply(integer(3), y0);
        const auto right = multiply(triple, b);
        if (ratio_.compare(slots_[left].magnitude, slots_[right].magnitude) >
            0) {
          a = triple;
          b = h0;
        }
      }
    }
    if (status_.ok()) {
      slots_[out_a] = slots_[a];
      slots_[out_b] = slots_[b];
      if (slots_[out_b].negative) {
        slots_[out_b].negative = false;
        slots_[out_a].negative = !slots_[out_a].negative && !zero(out_a);
      }
    }
    used_ = saved + 2;
    return {out_a, out_b};
  }
  Rational hermite(unsigned h, unsigned d, unsigned u, unsigned y0, unsigned y1,
                   Rational m0, Rational m1) {
    const auto d2 = multiply(d, d), u2 = multiply(u, u);
    const auto first_weight = multiply(u2, add(h, add(d, d)));
    const auto second_weight =
        multiply(d2, add(add(add(h, h), h), add(d, d), true));
    const auto base =
        add(multiply(y0, first_weight), multiply(y1, second_weight));
    const auto denominators = multiply(m0.denominator, m1.denominator);
    const auto left = multiply(
        multiply(multiply(multiply(h, d), u2), m0.numerator), m1.denominator);
    const auto right = multiply(
        multiply(multiply(multiply(h, d2), u), m1.numerator), m0.denominator);
    const auto numerator =
        add(add(multiply(base, denominators), left), right, true);
    const auto denominator =
        multiply(multiply(multiply(h, h), h), denominators);
    return {numerator, denominator};
  }
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
           ((value.significand << (52 - top)) & UINT64_C(0x000fffffffffffff));
  }
  unsigned midpoint(std::uint64_t low, std::uint64_t high, bool narrow) {
    const auto a = BinaryParts::decode(low, narrow),
               b = BinaryParts::decode(high, narrow);
    if (a.infinite || b.infinite) {
      auto finite = input(widen(a.infinite ? high : low, narrow), 1075);
      auto half_ulp = integer(0);
      const unsigned bit = narrow ? 1178 : 2045;
      if (status_.ok()) {
        slots_[half_ulp].magnitude.words[bit / 64] = UINT64_C(1) << (bit % 64);
        slots_[half_ulp].negative = a.infinite;
      }
      return add(finite, half_ulp);
    }
    // The sum in 2^-1074 units is the exact mean in 2^-1075 units,
    // including half-minsubnormal and a 54-bit midpoint significand.
    return add(input(widen(low, narrow)), input(widen(high, narrow)));
  }
  Result<std::uint64_t> finish(unsigned numerator, unsigned denominator,
                               bool narrow, bool negative_zero,
                               int scale = -1074) {
    if (!status_.ok())
      return Result<std::uint64_t>(status_);
    if (zero(numerator) && negative_zero)
      return Result<std::uint64_t>(UINT64_C(1) << (narrow ? 31 : 63));
    ratio_.numerator = slots_[numerator].magnitude;
    ratio_.denominator = slots_[denominator].magnitude;
    ratio_.negative = slots_[numerator].negative;
    return ratio_.round(narrow, *consume_, scale);
  }

 public:
  explicit ExactCurve(SequenceProfile profile) : ratio_(profile) {}
  // first/count describe exactly the selected y stencil; x/y contain that
  // stencil's raw bits. segment is global. selected>=0 is a direct knot/clamp.
  Result<std::uint64_t> evaluate(
      bool pchip, unsigned knots, unsigned first, unsigned count,
      unsigned segment, int selected, std::uint64_t query,
      const std::array<std::uint64_t, 4>& x,
      const std::array<std::uint64_t, 4>& y, bool narrow,
      const std::function<Status(std::uint64_t)>& consume,
      const std::function<Status()>& strict_fallback = {},
      bool normalized_environment = false) {
    used_ = 0;
    status_ = Status::success();
    consume_ = &consume;
    struct End {
      const std::function<Status(std::uint64_t)>*& callback;
      ~End() { callback = nullptr; }
    } end{consume_};
    if (selected < 0 && narrow && ratio_.profile != SequenceProfile::Strict) {
      auto work = consume(512);
      if (!work.ok())
        return Result<std::uint64_t>(work);
      std::optional<input_internal::Float32Environment> environment;
      if (!normalized_environment)
        environment.emplace();
      if (normalized_environment || environment->active()) {
        auto fast = accelerated_curve(pchip, knots, first, count, segment,
                                      numeric_double(query), x, y);
        if (fast) {
          const auto j = segment - first;
          double candidate = fast->value;
          if (numeric_double(query) >= numeric_double(x[j]) &&
              numeric_double(query) <= numeric_double(x[j + 1]))
            candidate = std::clamp(
                candidate,
                std::min(numeric_double(y[j]), numeric_double(y[j + 1])),
                std::max(numeric_double(y[j]), numeric_double(y[j + 1])));
          // A per-value tolerance does not prove cross-query monotonicity.
          // Publish only a uniquely rounded enclosure, so mixed fast/strict
          // evaluation remains the same monotone mathematical mapping.
          auto bits = fast->bound.accepted(candidate, narrow);
          if (bits && numeric_bits(fast->bound.low, narrow) ==
                          numeric_bits(fast->bound.high, narrow))
            return Result<std::uint64_t>(numeric_bits(fast->bound.low, narrow));
        }
      }
    }
    if (selected < 0 && ratio_.profile != SequenceProfile::Strict &&
        strict_fallback) {
      auto status = strict_fallback();
      if (!status.ok())
        return Result<std::uint64_t>(status);
    }
    if (selected >= 0) {
      const auto value = input(y[0]), one = integer(1);
      return finish(value, one, narrow, y[0] == (UINT64_C(1) << 63));
    }
    std::array<unsigned, 4> xs{}, ys{};
    for (unsigned i = 0; i < count; ++i) {
      xs[i] = input(x[i]);
      ys[i] = input(y[i]);
    }
    const auto q = input(query), j = segment - first;
    std::array<unsigned, 3> h{}, dy{};
    for (unsigned i = 0; i + 1 < count; ++i) {
      h[i] = add(xs[i + 1], xs[i], true);
      dy[i] = add(ys[i + 1], ys[i], true);
    }
    const bool negative_zero =
        y[j] == (UINT64_C(1) << 63) && y[j + 1] == (UINT64_C(1) << 63);
    const auto d = add(q, xs[j], true), u = add(xs[j + 1], q, true);
    if (!pchip || knots == 2 || collinear(h, dy, count))
      return finish(add(multiply(ys[j], u), multiply(ys[j + 1], d)), h[j],
                    narrow, negative_zero);
    if (sign(d) < 0 || sign(u) < 0) {
      const auto endpoint = sign(d) < 0 ? segment : segment + 1;
      const auto m = slope(endpoint, first, knots, h, dy);
      const auto delta = add(q, xs[endpoint - first], true);
      return finish(add(multiply(ys[endpoint - first], m.denominator),
                        multiply(delta, m.numerator)),
                    m.denominator, narrow, negative_zero);
    }
    const auto m0 = slope(segment, first, knots, h, dy);
    const auto m1 = slope(segment + 1, first, knots, h, dy);
    const auto formula = hermite(h[j], d, u, ys[j], ys[j + 1], m0, m1);
    return finish(formula.numerator, formula.denominator, narrow,
                  negative_zero);
  }
  // Invert the original mathematical segment, not its rounded forward map.
  // Topology/finite checks and exact knot selection belong to the adapter.
  // The non-knot target is strictly inside this segment's open y interval.
  Result<std::uint64_t> inverse(
      bool pchip, unsigned knots, unsigned first, unsigned count,
      unsigned segment, int selected, std::uint64_t query,
      const std::array<std::uint64_t, 4>& x,
      const std::array<std::uint64_t, 4>& y, bool narrow,
      const std::function<Status(std::uint64_t)>& consume,
      const std::function<Status()>& strict_fallback = {}) {
    using Answer = Result<std::uint64_t>;
    used_ = 0;
    status_ = Status::success();
    consume_ = &consume;
    struct End {
      const std::function<Status(std::uint64_t)>*& callback;
      SequenceProfile& profile;
      SequenceProfile saved;
      ~End() {
        callback = nullptr;
        profile = saved;
      }
    } end{consume_, ratio_.profile, ratio_.profile};
    if (selected < 0 && narrow && ratio_.profile != SequenceProfile::Strict) {
      input_internal::Float32Environment environment;
      if (environment.active()) {
        const auto j = segment - first;
        double lower = numeric_double(x[j]), upper = numeric_double(x[j + 1]);
        const double target = numeric_double(query);
        const bool increasing = numeric_double(y[j + 1]) > numeric_double(y[j]);
        for (unsigned iteration = 0; iteration < 64; ++iteration) {
          auto work = consume(512);
          if (!work.ok())
            return Answer(work);
          const double middle = lower + (upper - lower) * .5;
          if (!std::isfinite(middle))
            break;
          auto bits = FastInterval{lower, upper}.accepted(middle, narrow);
          if (bits &&
              numeric_bits(lower, narrow) == numeric_bits(upper, narrow))
            return Answer(numeric_bits(lower, narrow));
          auto value = accelerated_curve(pchip, knots, first, count, segment,
                                         middle, x, y);
          if (!value)
            break;
          if (value->bound.high < target) {
            if (increasing)
              lower = middle;
            else
              upper = middle;
          } else if (value->bound.low > target) {
            if (increasing)
              upper = middle;
            else
              lower = middle;
          } else {
            break;
          }
        }
      }
    }
    if (selected < 0 && ratio_.profile != SequenceProfile::Strict &&
        strict_fallback) {
      auto status = strict_fallback();
      if (!status.ok())
        return Result<std::uint64_t>(status);
    }
    if (selected >= 0) {
      const auto value = input(x[0]), one = integer(1);
      return finish(value, one, narrow, x[0] == (UINT64_C(1) << 63));
    }
    // Exact inverse refinement repeatedly compares wide integers. Preserve
    // its scalar lexicographic path after a fast attempt cannot certify x.
    if (pchip)
      ratio_.profile = SequenceProfile::Strict;
    std::array<unsigned, 4> xs{}, ys{};
    for (unsigned i = 0; i < count; ++i) {
      xs[i] = input(x[i], 1075);
      ys[i] = input(y[i], 1075);
    }
    const auto target = input(query, 1075), j = segment - first;
    std::array<unsigned, 3> h{}, dy{};
    for (unsigned i = 0; i + 1 < count; ++i) {
      h[i] = add(xs[i + 1], xs[i], true);
      dy[i] = add(ys[i + 1], ys[i], true);
    }
    if (!pchip || knots == 2 || collinear(h, dy, count)) {
      const auto lower = add(ys[j + 1], target, true),
                 upper = add(target, ys[j], true);
      const auto numerator =
          add(multiply(xs[j], lower), multiply(xs[j + 1], upper));
      if (status_.ok() && slots_[dy[j]].negative)
        slots_[numerator].negative =
            !slots_[numerator].negative && !zero(numerator);
      return finish(numerator, dy[j], narrow, false, -1075);
    }
    const auto m0 = slope(segment, first, knots, h, dy),
               m1 = slope(segment + 1, first, knots, h, dy);
    if (!status_.ok())
      return Answer(status_);
    const auto retained = used_;
    const bool increasing = sign(dy[j]) > 0;
    const auto compare = [&](unsigned q) {
      const auto d = add(q, xs[j], true), u = add(xs[j + 1], q, true);
      if (!status_.ok())
        return 0;
      // Every polynomial comparison lies inside the original segment, the
      // boundary required by the 21010-bit numerator capacity proof.
      if (sign(d) <= 0)
        return -1;
      if (sign(u) <= 0)
        return 1;
      auto formula = hermite(h[j], d, u, ys[j], ys[j + 1], m0, m1);
      const auto difference =
          add(formula.numerator, multiply(target, formula.denominator), true);
      if (!status_.ok())
        return 0;
      return increasing ? sign(difference) : -sign(difference);
    };
    const auto zero_key = UINT64_C(1) << 63;
    const auto infinity =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    const auto sign_bit = UINT64_C(1) << (narrow ? 31 : 63);
    const auto raw = [&](std::uint64_t key) {
      return key < zero_key ? (zero_key - key) | sign_bit : key - zero_key;
    };
    std::uint64_t low = zero_key - infinity, high = zero_key + infinity;
    // Bracket adjacent destination lattice numbers with exact sign tests.
    // Infinity sentinels are not evaluated as real polynomial coordinates.
    while (high - low > 1) {
      auto charged = consume(1);
      if (!charged.ok())
        return Answer(charged);
      const auto middle = low + (high - low) / 2;
      used_ = retained;
      const auto ordering = compare(input(widen(raw(middle), narrow), 1075));
      if (!status_.ok())
        return Answer(status_);
      if (!ordering)
        return Answer(raw(middle));
      if (ordering < 0)
        low = middle;
      else
        high = middle;
    }
    used_ = retained;
    const auto lower = raw(low), upper = raw(high);
    const auto ordering = compare(midpoint(lower, upper, narrow));
    if (!status_.ok())
      return Answer(status_);
    auto result = ordering > 0   ? lower
                  : ordering < 0 ? upper
                  : !(lower & 1) ? lower
                                 : upper;
    if (!(result & (sign_bit - 1)) && low < zero_key)
      result |= sign_bit;
    return Answer(result);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
