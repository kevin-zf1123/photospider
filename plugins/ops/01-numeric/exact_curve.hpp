#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "01-numeric/exact_product.hpp"

namespace ps::plugin_internal::numeric_ops {
// All inputs are finite binary64 bits. X/Y are integers in units 2^-1074.
// Differences have <2099 bits; PCHIP slope numerators/denominators have <6300
// bits, and the combined Hermite numerator/denominator have <21000/18900 bits.
// 22528 bits also cover division alignment, 53 quotient bits and midpoint
// doubling. Every temporary is in this continuation-owned fixed arena.
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
  unsigned input(std::uint64_t bits) {
    auto out = allocate();
    if (status_.ok()) {
      const auto value = BinaryParts::decode(bits, false);
      slots_[out].magnitude.set(value, 1074);
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
  Result<std::uint64_t> finish(unsigned numerator, unsigned denominator,
                               bool narrow, bool negative_zero) {
    if (!status_.ok())
      return Result<std::uint64_t>(status_);
    if (zero(numerator) && negative_zero)
      return Result<std::uint64_t>(UINT64_C(1) << (narrow ? 31 : 63));
    ratio_.numerator = slots_[numerator].magnitude;
    ratio_.denominator = slots_[denominator].magnitude;
    ratio_.negative = slots_[numerator].negative;
    return ratio_.round(narrow, *consume_, -1074);
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
      const std::function<Status(std::uint64_t)>& consume) {
    used_ = 0;
    status_ = Status::success();
    consume_ = &consume;
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
    if (!pchip || knots == 2)
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
    const auto d2 = multiply(d, d), u2 = multiply(u, u);
    const auto first_weight = multiply(u2, add(h[j], add(d, d)));
    const auto second_weight =
        multiply(d2, add(add(add(h[j], h[j]), h[j]), add(d, d), true));
    const auto base =
        add(multiply(ys[j], first_weight), multiply(ys[j + 1], second_weight));
    const auto denominators = multiply(m0.denominator, m1.denominator);
    const auto left =
        multiply(multiply(multiply(multiply(h[j], d), u2), m0.numerator),
                 m1.denominator);
    const auto right =
        multiply(multiply(multiply(multiply(h[j], d2), u), m1.numerator),
                 m0.denominator);
    const auto numerator =
        add(add(multiply(base, denominators), left), right, true);
    const auto denominator =
        multiply(multiply(multiply(h[j], h[j]), h[j]), denominators);
    return finish(numerator, denominator, narrow, negative_zero);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
