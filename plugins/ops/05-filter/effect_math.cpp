#include <cmath>
#include <optional>

#include "05-filter/effect_fast.hpp"
#include "05-filter/effects.hpp"

namespace ps::plugin_internal::filter {
namespace {
std::optional<std::uint64_t> propagated_nan(Atom a, Atom b) {
  const auto type = a.narrow ? ElementType::Float32 : ElementType::Float64;
  if (a.nan())
    return numeric_ops::converted_nan(a.raw, type, type);
  if (b.nan())
    return numeric_ops::converted_nan(b.raw, type, type);
  return {};
}
Ball absolute(Ball a) {
  return a.hi < 0 || (a.hi == 0 && a.lo < 0) ? -a : a;
}
}  // namespace

std::uint64_t contrast_pixel(Atom a, Atom b, double amount, double scale,
                             generation::Math& m, const Fuel& fuel) {
  if (const auto n = propagated_nan(a, b))
    return *n;
  if (std::isfinite(a.value()) && a.value() == b.value()) {
    if (!a.zero())
      return a.raw;
    return generation::convert(
        a.value() + amount * (scale * (a.value() - b.value())), a.narrow);
  }
  if (const auto value = contrast_float(a, b, amount, scale, fuel))
    return *value;
  const auto difference = Ball::point(a.value()) + (-Ball::point(b.value()));
  auto argument = dd_divide(difference, Ball::point(scale));
  const bool negative =
      argument.hi < 0 || (argument.hi == 0 && argument.lo < 0);
  argument = absolute(argument);
  if (argument.finite()) {
    if (a.narrow && argument.bounds().high < 32) {
      if (const auto exponential =
              float_exp_enclosure(argument.times(-2), fuel)) {
        auto t = dd_divide(Ball{1, 0, 0} + (-*exponential),
                           Ball{1, 0, 0} + *exponential);
        if (negative)
          t = -t;
        if (const auto value = dd_round(
                Ball::point(a.value()) + t.times(scale).times(amount), true))
          return *value;
      }
    }
    Ball t;
    bool bounded = true;
    if (argument.bounds().low >= 32) {
      // 1-tanh(x) < 2*exp(-64) < 2^-91 for x >= 32.
      t = {1, 0, 0x1p-91};
    } else {
      const auto exp = dd_exp_enclosure(argument.times(-2), fuel);
      if (exp)
        t = dd_divide(Ball{1, 0, 0} + (-*exp), Ball{1, 0, 0} + *exp);
      else
        bounded = false;
    }
    if (bounded) {
      if (negative)
        t = -t;
      if (const auto result = dd_round(
              Ball::point(a.value()) + t.times(scale).times(amount), a.narrow))
        return *result;
    }
  }
  auto& e = m.exact;
  const auto av = e.binary(a.raw, a.narrow);
  const auto bv = e.binary(b.raw, b.narrow);
  const auto delta = m.sub(av, bv);
  if (Exact::nan(delta))
    return a.narrow ? UINT64_C(0x7fc00000) : bits(NAN);
  if (Exact::infinity(delta)) {
    return m.rounded(m.add(av, m.mul(m.value(amount),
                                     m.value(delta.negative ? -scale : scale))),
                     a.narrow);
  }
  const auto x = m.div(e.absolute(delta), m.value(scale));
  const auto factor = m.value(delta.negative ? -amount : amount);
  const auto amplitude_value = m.mul(factor, m.value(scale));
  if (e.order(x, m.value(.5)) <= 0) {
    // For 0<x<=1/2, x-x^3/3 <= tanh(x) <= x-x^3/4. Evaluating
    // the linear term exactly keeps tiny cubic residuals and their signs.
    const auto linear = m.add(av, m.mul(m.value(amount), delta));
    const auto cubic = m.mul(amplitude_value, m.mul(m.sq(x), x));
    const auto lower =
        m.rounded(m.sub(linear, m.div(cubic, e.integer(3))), a.narrow, true);
    const auto upper =
        m.rounded(m.sub(linear, m.div(cubic, e.integer(4))), a.narrow, true);
    if (lower == upper)
      return lower;
  }
  if (e.order(x, e.integer(1536)) >= 0) {
    // The true correction to a+amplitude has sign -amplitude and magnitude
    // <2^-2175 (e>2^(11/8)). The limit and all RN32/RN64 midpoints use the
    // 2^-2148 lattice. A same-sided 2^-4096 probe resolves even exact ties
    // and signed underflow without subtracting an indistinguishable tanh.
    auto epsilon = m.value(0x1p-1024);
    epsilon = m.sq(m.sq(epsilon));
    const auto limit = m.add(av, amplitude_value);
    return m.rounded(
        m.add(limit, amplitude_value.negative ? epsilon : e.negate(epsilon)),
        a.narrow, true);
  }
  const bool scaled_exponential = e.order(x, m.value(.5)) > 0;
  const int shift =
      scaled_exponential
          ? 3 * static_cast<int>(std::ceil(number(m.rounded(x, false))))
          : 0;
  const auto limit_value = m.add(av, amplitude_value);
  const auto residual_factor = m.sub(av, amplitude_value);
  const bool negative_limit_residual =
      Exact::zero(limit_value) && residual_factor.negative;
  const auto rounded = m.certify(a.narrow, [&](generation::Interval out) {
    generation::Directed::Frame frame(m.d);
    auto v = m.d.interval(), exponent = m.d.interval();
    auto exponential = m.d.interval(), numerator = m.d.interval();
    auto denominator = m.d.interval(), one = m.d.interval();
    auto amplitude = m.d.interval(), center = m.d.interval();
    m.enclose(exponent, m.scale(x, -2));
    m.d.integer(one, 1);
    if (scaled_exponential) {
      // Normalize exp before multiplying the potentially huge coefficient.
      // The directed engine uses Q_precision; scaling afterwards would lose
      // subnormal exponential information before a large finite product.
      auto logarithm = m.d.interval(), power = m.d.interval();
      m.d.constant(logarithm, false);
      m.d.integer(power, shift);
      m.d.multiply(logarithm, logarithm, power);
      m.d.add(exponent, exponent, logarithm);
      m.functions.exponential(exponential, exponent);
      m.enclose(center, residual_factor);
      m.d.multiply(center, center, exponential);
      m.d.shift(v.low, center.low, -shift, true);
      m.d.shift(v.high, center.high, -shift, false);
      m.enclose(numerator, limit_value);
      m.d.add(numerator, numerator, v);
      m.d.shift(v.low, exponential.low, -shift, true);
      m.d.shift(v.high, exponential.high, -shift, false);
      m.d.add(denominator, one, v);
      m.d.divide(out, numerator, denominator);
      if (negative_limit_residual) {
        // exp is strictly positive. Certify the magnitude because the shared
        // directed rounder canonicalizes an exactly zero endpoint to +0.
        m.d.negate(out, out);
      }
      return;
    }
    m.functions.exponential(exponential, exponent);
    m.d.subtract(numerator, one, exponential);
    m.d.add(denominator, one, exponential);
    m.d.divide(v, numerator, denominator);
    m.enclose(amplitude, m.mul(factor, m.value(scale)));
    m.d.multiply(v, v, amplitude);
    m.enclose(center, av);
    m.d.add(out, center, v);
  });
  return rounded |
         (negative_limit_residual ? UINT64_C(1) << (a.narrow ? 31 : 63) : 0);
}

std::uint64_t remap_pixel(Atom value, Atom guide, double sigma, double alpha,
                          double beta, generation::Math& m, const Fuel& fuel) {
  fuel.poll();
  if (const auto n = propagated_nan(guide, value))
    return *n;
  if (value.value() == guide.value() && std::isfinite(value.value()))
    return guide.zero() ? 0 : guide.raw;
  if (const auto result = dyadic_remap(value, guide, sigma, alpha, beta))
    return *result;
  auto d = Ball::point(value.value()) + (-Ball::point(guide.value()));
  const bool negative = d.hi < 0 || (d.hi == 0 && d.lo < 0);
  auto z = absolute(d);
  if (z.finite()) {
    const auto bounds = z.bounds();
    Ball f;
    bool computed = false;
    if (bounds.low > sigma) {
      f = (z + Ball::point(-sigma)).times(beta) + Ball::point(sigma);
      computed = true;
    } else if (bounds.high <= sigma || (z.point() && z.hi <= sigma)) {
      if (alpha == 1) {
        // Finite nonzero cancellation recovers v; exact zero is +0.
        return value.zero() ? 0 : value.raw;
      }
      if (alpha == 2) {
        f = dd_divide(dd_product(z, z), Ball::point(sigma));
        computed = true;
      }
    }
    if (computed) {
      if (negative)
        f = -f;
      if (const auto result =
              dd_round(Ball::point(guide.value()) + f, value.narrow))
        return *result;
    }
  }
  auto& e = m.exact;
  const auto g = e.binary(guide.raw, guide.narrow);
  const auto v = e.binary(value.raw, value.narrow);
  const auto delta = m.sub(v, g);
  if (Exact::nan(delta))
    return value.narrow ? UINT64_C(0x7fc00000) : bits(NAN);
  const auto magnitude = e.absolute(delta);
  const auto s = m.value(sigma);
  const bool neg = delta.negative;
  auto finish = [&](Rational f) {
    return m.rounded(m.add(g, neg ? e.negate(f) : f), value.narrow);
  };
  if (e.order(magnitude, s) > 0)
    return finish(m.add(s, m.mul(m.value(beta), m.sub(magnitude, s))));
  if (Exact::zero(magnitude))
    return m.rounded(g, value.narrow);
  if (alpha == 1)
    return finish(magnitude);
  if (alpha == 2)
    return finish(m.div(m.sq(magnitude), s));
  const bool zero_guide = Exact::zero(g);
  const auto rounded = m.certify(value.narrow, [&](generation::Interval out) {
    generation::Directed::Frame frame(m.d);
    auto base = m.d.interval(), exponent = m.d.interval();
    auto power = m.d.interval(), amplitude = m.d.interval();
    auto center = m.d.interval();
    m.enclose(base, m.div(magnitude, s));
    m.enclose(exponent, m.value(alpha));
    m.functions.power(power, base, exponent);
    m.enclose(amplitude, neg && !zero_guide ? e.negate(s) : s);
    m.d.multiply(power, power, amplitude);
    if (zero_guide) {
      // Certify the strictly positive magnitude, then restore its known sign.
      // An exactly zero directed endpoint otherwise rounds to canonical +0.
      m.d.copy(out, power);
      return;
    }
    m.enclose(center, g);
    m.d.add(out, center, power);
  });
  return rounded |
         (zero_guide && neg ? UINT64_C(1) << (value.narrow ? 31 : 63) : 0);
}
}  // namespace ps::plugin_internal::filter
