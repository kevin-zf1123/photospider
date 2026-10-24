#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

#include "05-filter/field_wide.hpp"

namespace ps::plugin_internal::filter {
// A double-double enclosure of an exact expression. The radius includes every
// discarded term; unsuccessful certificates use the existing rational arena.
inline Ball dd_product(Ball a, Ball b) {
  return FieldWideArithmetic::product(a, b);
}
inline Ball dd_divide(Ball a, Ball b) {
  if (!a.finite() || !b.finite())
    return Ball::bad();
  auto bounds = b.bounds();
  if (bounds.high < 0) {
    a = -a;
    b = -b;
    bounds = b.bounds();
  }
  if (!(bounds.low > 0))
    return Ball::bad();
  const double q = a.hi / b.hi;
  if (!std::isfinite(q))
    return Ball::bad();
  const auto remainder = a + (-b.times(q));
  const double low = (remainder.hi + remainder.lo) / b.hi;
  if (!std::isfinite(low))
    return Ball::bad();
  const auto residual = remainder + (-b.times(low));
  if (residual.point() && residual.hi == 0)
    return {q, low, 0};
  const auto r = residual.bounds();
  const double radius = numeric_ops::numeric_up(
      std::max(std::abs(r.low), std::abs(r.high)) / bounds.low);
  return {q, low, radius};
}
inline std::optional<std::uint64_t> dd_round(Ball a, bool narrow) {
  if (a.finite() && a.point()) {
    // Preserve only a proved exact value; arithmetic cancellation is +0.
    if (a.hi == 0)
      return 0;
    return generation::convert(a.hi, narrow);
  }
  return BoundedArithmetic{narrow}.finish(BallValue{a});
}
inline std::optional<std::uint64_t> dd_exp_negative(Ball x, const Fuel& fuel) {
  if (!x.finite())
    return {};
  const auto bounds = x.bounds();
  if (bounds.high <= -746)
    return UINT64_C(0);
  if (bounds.low < -700 || bounds.high > 0)
    return {};
  // ln(2) is enclosed by these two binary64 components and 2^-108.
  constexpr double ln2_hi = 0x1.62e42fefa39efp-1;
  constexpr double ln2_lo = 0x1.abc9e3b39803fp-56;
  const auto k = static_cast<int>(std::nearbyint(x.hi / ln2_hi));
  const Ball ln2{ln2_hi, ln2_lo, 0x1p-108};
  const auto r = x + (-ln2.times(k));
  const auto rb = r.bounds();
  if (rb.low < -.36 || rb.high > .36)
    return {};
  // Each coefficient encloses the exact rational 1/n!; constants are
  // binary64 high/low components plus an outward-rounded exact residual.
  static constexpr Ball coefficients[25] = {
      {0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0},
      {0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0},
      {0x1.0000000000000p-1, 0x0.0p+0, 0x0.0p+0},
      {0x1.5555555555555p-3, 0x1.5555555555555p-57, 0x1.5555555555556p-111},
      {0x1.5555555555555p-5, 0x1.5555555555555p-59, 0x1.5555555555556p-113},
      {0x1.1111111111111p-7, 0x1.1111111111111p-63, 0x1.1111111111112p-119},
      {0x1.6c16c16c16c17p-10, -0x1.f49f49f49f49fp-65, 0x1.27d27d27d27d3p-119},
      {0x1.a01a01a01a01ap-13, 0x1.a01a01a01a01ap-73, 0x1.a01a01a01a01bp-133},
      {0x1.a01a01a01a01ap-16, 0x1.a01a01a01a01ap-76, 0x1.a01a01a01a01bp-136},
      {0x1.71de3a556c734p-19, -0x1.c154f8ddc6c00p-73, 0x1.71de3a556c734p-127},
      {0x1.27e4fb7789f5cp-22, 0x1.cbbc05b4fa99ap-76, 0x1.c6d278883e8f5p-132},
      {0x1.ae64567f544e4p-26, -0x1.c062e06d1f209p-80, 0x1.c7880adcbc46ep-136},
      {0x1.1eed8eff8d898p-29, -0x1.2aec959e14c06p-83, 0x1.2fb0073dd2d9fp-139},
      {0x1.6124613a86d09p-33, 0x1.f28e0cc748ebep-87, 0x1.7b2c4c8a840bcp-141},
      {0x1.93974a8c07c9dp-37, 0x1.05d6f8a2efd1fp-92, 0x1.3aa3346236a5ep-147},
      {0x1.ae7f3e733b81fp-41, 0x1.1d8656b0ee8cbp-97, 0x1.6e142a138f825p-157},
      {0x1.ae7f3e733b81fp-45, 0x1.1d8656b0ee8cbp-101, 0x1.6e142a138f825p-161},
      {0x1.952c77030ad4ap-49, 0x1.ac981465ddc6cp-103, 0x1.588b72e53bc5fp-165},
      {0x1.6827863b97d97p-53, 0x1.eec01221a8b0bp-107, 0x1.568798662118bp-161},
      {0x1.2f49b46814157p-57, 0x1.2650f61dbdcb4p-112, 0x1.69502917cbf3bp-166},
      {0x1.e542ba4020225p-62, 0x1.ea72b4afe3c2fp-120, 0x1.44020dfd65c8dp-174},
      {0x1.71b8ef6dcf572p-66, -0x1.d043ae40c4647p-120, 0x1.486121e81d5fep-176},
      {0x1.0ce396db7f853p-70, -0x1.aebcdbd20331cp-124, 0x1.38a88578b4d75p-178},
      {0x1.761b41316381ap-75, -0x1.3423c7d91404fp-130, 0x1.e6135bfc1194ap-185},
      {0x1.f2cf01972f578p-80, -0x1.9ada5fcc1ab14p-135, 0x1.440ce7fd610dcp-189},
  };
  auto sum = coefficients[24];
  for (int n = 23; n >= 0; --n) {
    fuel.poll(3);
    sum = dd_product(sum, r) + coefficients[n];
  }
  // exp(.36)*.36^25/25! < 2^-118.
  sum.error = Ball::up(sum.error + 0x1p-118);
  const double scale = std::ldexp(1.0, k);
  return dd_round(sum.times(scale), false);
}
inline std::uint64_t exact_exp_negative(generation::Math& m,
                                        const Rational& x) {
  auto& e = m.exact;
  if (Exact::nan(x))
    return bits(NAN);
  if (Exact::infinity(x))
    return x.negative ? 0 : bits(INFINITY);
  if (Exact::zero(x))
    return bits(1);
  if (e.order(x, e.integer(-746)) <= 0) {
    check(e.status());
    return 0;
  }
  return m.certify(false, [&](generation::Interval out) {
    generation::Directed::Frame frame(m.d);
    auto argument = m.d.interval();
    m.enclose(argument, x);
    m.functions.exponential(out, argument);
  });
}
}  // namespace ps::plugin_internal::filter
