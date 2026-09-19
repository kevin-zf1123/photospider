#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>

#include "01-numeric/lowpass_kernel.hpp"
#include "photospider/execution/resource_allocator.hpp"

namespace ps::plugin_internal::numeric_ops {
// A polynomial in u^2 plus a uniform absolute error on |u|<=1. Coefficients
// use the active directed Q precision and separately admitted vector storage.
struct LowpassStoredInterval {
  DirectedInterval::Number low, high;
  DirectedInterval::Interval view() { return {low, high}; }
};
struct LowpassPolynomial {
  ResourceVector<LowpassStoredInterval> coefficients;
  LowpassStoredInterval error;
};
// Global Taylor baseline for continuous moments. Every truncation carries a
// geometric majorant, so a finite order never silently becomes the kernel.
// Large parameters may require more precision/order than the caller's budget.
class LowpassPolynomialBuilder {
  using Interval = DirectedInterval::Interval;
  using Frame = DirectedInterval::Frame;
  DirectedLowpassKernel& kernel_;
  DirectedInterval& m_;
  void bound(Interval result, Interval value) {
    m_.integer(result, 0);
    m_.copy(result.high,
            m_.compare_unsigned(value.low.magnitude, value.high.magnitude) > 0
                ? value.low
                : value.high);
    result.high.negative = false;
  }
  // kind: 0=sinc(Au), 1=cos(Au), 2=exp(-A*u^2). Argument is A^2
  // for trig or A for exp. |tail| <=2*|next coefficient| once all following
  // coefficient ratios are <=1/2, uniformly over |u|<=1.
  LowpassPolynomial series(Interval argument, unsigned order, unsigned kind) {
    Frame frame(m_);
    LowpassPolynomial result;
    result.coefficients.resize(order + 1);
    auto term = m_.interval(), negative = m_.interval(), ratio = m_.interval(),
         half = m_.interval();
    m_.integer(term, 1);
    m_.negate(negative, argument);
    m_.integer(half, 1);
    m_.scale(half, half, -1);
    m_.copy(result.coefficients[0].view(), term);
    const auto divisor = [kind](std::uint64_t n) {
      return kind == 2   ? n
             : kind == 1 ? (2 * n - 1) * (2 * n)
                         : (2 * n) * (2 * n + 1);
    };
    for (unsigned n = 1; n <= order + 1; ++n) {
      m_.multiply(term, term, negative);
      m_.divide_small(term, term, divisor(n));
      if (n <= order)
        m_.copy(result.coefficients[n].view(), term);
    }
    m_.divide_small(ratio, argument, divisor(order + 2));
    if (m_.compare(ratio.high, half.low) > 0)
      throw DirectedInterval::Unresolved{};
    bound(result.error.view(), term);
    m_.scale(result.error.view(), result.error.view(), 1);
    return result;
  }
  LowpassPolynomial bessel(Interval b, unsigned order) {
    Frame frame(m_);
    LowpassPolynomial result;
    result.coefficients.resize(order + 1);
    for (auto& coefficient : result.coefficients)
      m_.integer(coefficient.view(), 0);
    auto term = m_.interval(), coefficient = m_.interval(),
         factor = m_.interval(), ratio = m_.interval(), half = m_.interval();
    m_.integer(term, 1);
    m_.integer(half, 1);
    m_.scale(half, half, -1);
    for (unsigned n = 0; n <= order; ++n) {
      // Expand the finite exact polynomial (1-u^2)^n; coefficients are
      // enclosed with directed rounding, and the omitted I0 tail is separate.
      m_.copy(coefficient, term);
      for (unsigned k = 0; k <= n; ++k) {
        auto destination = result.coefficients[k].view();
        m_.add(destination, destination, coefficient);
        if (k < n) {
          m_.integer(factor, -static_cast<std::int64_t>(n - k));
          m_.multiply(coefficient, coefficient, factor);
          m_.divide_small(coefficient, coefficient, k + 1);
        }
      }
      m_.multiply(term, term, b);
      m_.divide_small(term, term, static_cast<std::uint64_t>(n + 1) * (n + 1));
    }
    m_.divide_small(ratio, b,
                    static_cast<std::uint64_t>(order + 2) * (order + 2));
    if (m_.compare(ratio.high, half.low) > 0)
      throw DirectedInterval::Unresolved{};
    bound(result.error.view(), term);
    m_.scale(result.error.view(), result.error.view(), 1);
    return result;
  }
  LowpassPolynomial product(LowpassPolynomial a, LowpassPolynomial b,
                            Interval true_b_bound) {
    Frame frame(m_);
    LowpassPolynomial result;
    result.coefficients.resize(a.coefficients.size() + b.coefficients.size() -
                               1);
    for (auto& value : result.coefficients)
      m_.integer(value.view(), 0);
    auto term = m_.interval(), error = m_.interval();
    for (unsigned i = 0; i < a.coefficients.size(); ++i)
      for (unsigned j = 0; j < b.coefficients.size(); ++j) {
        m_.multiply(term, a.coefficients[i].view(), b.coefficients[j].view());
        auto target = result.coefficients[i + j].view();
        m_.add(target, target, term);
      }
    // |sinc|<=1. Product error <= Ea*|b| + Eb + Ea*Eb.
    m_.multiply(error, a.error.view(), true_b_bound);
    m_.add(error, error, b.error.view());
    m_.multiply(term, a.error.view(), b.error.view());
    m_.add(result.error.view(), error, term);
    return result;
  }

 public:
  explicit LowpassPolynomialBuilder(DirectedLowpassKernel& kernel)
      : kernel_(kernel), m_(kernel.functions.math) {}
  LowpassPolynomial build(const LowpassParameters& p, unsigned order) {
    if (!order || order > 512)
      DirectedInterval::capacity();
    Frame frame(m_);
    auto radius = m_.interval(), parameter = m_.interval(),
         argument = m_.interval(), pi = m_.interval(), bound = m_.interval();
    m_.raw(radius, p.radius, false);
    if (p.kernel == LowpassKernel::Gaussian) {
      kernel_.binary_ratio(argument, p.radius, p.sigma);
      m_.multiply(argument, argument, argument);
      m_.scale(argument, argument, -1);
      return series(argument, order, 2);
    }
    m_.constant(pi, true);
    m_.raw(parameter, p.cutoff, false);
    m_.multiply(argument, parameter, radius);
    m_.multiply(argument, argument, pi);
    m_.scale(argument, argument, 1);
    m_.multiply(argument, argument, argument);
    auto sinc = series(argument, order, 0);
    LowpassPolynomial window;
    m_.integer(bound, 1);
    if (p.kernel == LowpassKernel::Kaiser) {
      m_.raw(parameter, p.beta, false);
      m_.multiply(parameter, parameter, parameter);
      m_.scale(parameter, parameter, -2);
      window = bessel(parameter, order);
      kernel_.bessel_series(bound, parameter);
    } else {
      m_.multiply(argument, pi, pi);
      window = series(argument, order, 1);
      auto factor = m_.interval();
      m_.integer(factor, p.kernel == LowpassKernel::Hann      ? 25
                         : p.kernel == LowpassKernel::Hamming ? 23
                                                              : 25);
      m_.divide_small(factor, factor, 50);
      for (auto& value : window.coefficients)
        m_.multiply(value.view(), value.view(), factor);
      m_.multiply(window.error.view(), window.error.view(), factor);
      m_.integer(factor, p.kernel == LowpassKernel::Hann      ? 25
                         : p.kernel == LowpassKernel::Hamming ? 27
                                                              : 21);
      m_.divide_small(factor, factor, 50);
      m_.add(window.coefficients[0].view(), window.coefficients[0].view(),
             factor);
      if (p.kernel == LowpassKernel::Blackman) {
        m_.scale(argument, argument, 2);
        auto second = series(argument, order, 1);
        m_.integer(factor, 2);
        m_.divide_small(factor, factor, 25);
        for (unsigned i = 0; i < window.coefficients.size(); ++i) {
          m_.multiply(second.coefficients[i].view(),
                      second.coefficients[i].view(), factor);
          m_.add(window.coefficients[i].view(), window.coefficients[i].view(),
                 second.coefficients[i].view());
        }
        m_.multiply(second.error.view(), second.error.view(), factor);
        m_.add(window.error.view(), window.error.view(), second.error.view());
      }
    }
    return product(std::move(sinc), std::move(window), bound);
  }
  // Exact polynomial moments (with outward arithmetic) for A+B*u on [lo,hi].
  // Caller adds (hi-lo)*max(|endpoint samples|)*polynomial.error to the result.
  void integral(Interval output, LowpassPolynomial& polynomial, Interval lo,
                Interval hi, Interval a, Interval b) {
    Frame frame(m_);
    auto lp = m_.interval(), hp = m_.interval(), ls = m_.interval(),
         hs = m_.interval(), first = m_.interval(), second = m_.interval(),
         term = m_.interval();
    m_.copy(lp, lo);
    m_.copy(hp, hi);
    m_.multiply(ls, lo, lo);
    m_.multiply(hs, hi, hi);
    m_.integer(output, 0);
    for (unsigned n = 0; n < polynomial.coefficients.size(); ++n) {
      m_.subtract(first, hp, lp);
      m_.divide_small(first, first, 2 * n + 1);
      m_.multiply(first, first, a);
      m_.multiply(second, hp, hi);
      m_.multiply(term, lp, lo);
      m_.subtract(second, second, term);
      m_.divide_small(second, second, 2 * n + 2);
      m_.multiply(second, second, b);
      m_.add(first, first, second);
      m_.multiply(term, first, polynomial.coefficients[n].view());
      m_.add(output, output, term);
      m_.multiply(lp, lp, ls);
      m_.multiply(hp, hp, hs);
    }
  }
};
}  // namespace ps::plugin_internal::numeric_ops
