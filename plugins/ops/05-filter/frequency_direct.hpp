#pragma once

#include <array>
#include <numeric>
#include <utility>

#include "05-filter/frequency.hpp"

namespace ps::plugin_internal::frequency::trace {
struct Ramanujan {
  std::uint64_t order, degree;
  std::array<std::uint64_t, 64> primes{};
  unsigned count = 0;
  Ramanujan(std::uint64_t n, const Fuel& fuel) : order(n), degree(n) {
    auto remaining = n;
    for (std::uint64_t p = 2; p <= remaining / p; ++p) {
      fuel.poll();
      if (remaining % p)
        continue;
      primes[count++] = p;
      degree = degree / p * (p - 1);
      do {
        fuel.poll();
        remaining /= p;
      } while (remaining % p == 0);
    }
    if (remaining > 1) {
      primes[count++] = remaining;
      degree = degree / remaining * (remaining - 1);
    }
  }
  std::int64_t operator()(std::uint64_t phase, const Fuel& fuel) const {
    fuel.poll(128);
    auto quotient = order / std::gcd(order, phase);
    std::uint64_t denominator = 1;
    int sign = 1;
    for (unsigned j = 0; j < count; ++j) {
      fuel.poll();
      const auto p = primes[j];
      if (quotient % p)
        continue;
      quotient /= p;
      if (quotient % p == 0)
        return 0;
      sign = -sign;
      denominator *= p - 1;
    }
    return sign * static_cast<std::int64_t>(degree / denominator);
  }
};
inline int jacobi(std::uint64_t a, std::uint64_t n, const Fuel& fuel) {
  int result = 1;
  a %= n;
  while (a) {
    fuel.poll();
    while ((a & 1) == 0) {
      fuel.poll();
      a >>= 1;
      if (n % 8 == 3 || n % 8 == 5)
        result = -result;
    }
    std::swap(a, n);
    if (a % 4 == 3 && n % 4 == 3)
      result = -result;
    a %= n;
  }
  return n == 1 ? result : 0;
}
inline int character(std::uint64_t discriminant, std::uint64_t a,
                     const Fuel& fuel) {
  if (!a)
    return discriminant == 1 ? 1 : 0;
  int result = 1;
  while ((a & 1) == 0) {
    fuel.poll();
    if ((discriminant & 1) == 0)
      return 0;
    if (discriminant % 8 == 3 || discriminant % 8 == 5)
      result = -result;
    a >>= 1;
  }
  return result * jacobi(discriminant, a, fuel);
}
// Stream the exact real-cosine terms in original logical sample/port order.
// Structural zero factors are excluded before reading exceptional operands.
template <class Function>
void terms(const Frequency& s, const Reader& reader, Coordinate at,
           unsigned component, std::uint64_t n, const Fuel& fuel,
           Function function) {
  const auto h = s.grid.shape[s.grid.ya], w = s.grid.shape[s.grid.xa];
  const auto ky = at[s.grid.ya], kx = at[s.grid.xa];
  const auto gy = std::gcd(h, ky), gx = std::gcd(w, kx);
  const auto step_y = static_cast<std::uint64_t>(
      static_cast<unsigned __int128>(ky / gy) * (n / (h / gy)) % n);
  const auto step_x = static_cast<std::uint64_t>(
      static_cast<unsigned __int128>(kx / gx) * (n / (w / gx)) % n);
  const bool y_first = s.grid.ya < s.grid.xa;
  for (std::uint64_t i = 0; i < h * w; ++i) {
    fuel.poll(4);
    const auto y = y_first ? i / w : i % h, x = y_first ? i % w : i / h;
    auto source = at;
    source[s.grid.ya] = y;
    source[s.grid.xa] = x;
    bool conjugated = false;
    if (s.kind == Kind::Irfft && x > w / 2) {
      source[s.grid.ya] = (h - y) % h;
      source[s.grid.xa] = w - x;
      conjugated = true;
    }
    const auto angle = static_cast<std::uint64_t>(
        (static_cast<unsigned __int128>(step_y) * y +
         static_cast<unsigned __int128>(step_x) * x) %
        n);
    const auto phase = s.inverse() ? angle : (n - angle) % n;
    for (unsigned input = 0; input < (s.real_input() ? 1U : 2U); ++input) {
      fuel.poll();
      const auto p =
          (phase + (input ? n / 4 : 0) + (component ? 3 * (n / 4) : 0)) % n;
      if (p == n / 4 || p == 3 * (n / 4))
        continue;
      const bool narrow = s.type == ElementType::Float32;
      auto raw = reader.raw(input, 0, source);
      if (input && conjugated)
        raw ^= narrow ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);
      function(i * 2 + input, p, Atom{raw, narrow});
    }
  }
}
inline std::uint64_t direct(const Frequency& s, Math& m, const Reader& reader,
                            Coordinate at, unsigned component,
                            const Fuel& fuel) {
  check(m.work.stage());
  auto& e = m.exact;
  const auto h = s.grid.shape[s.grid.ya], w = s.grid.shape[s.grid.xa];
  const auto ky = at[s.grid.ya], kx = at[s.grid.xa];
  auto n =
      std::lcm(std::lcm(h / std::gcd(h, ky), w / std::gcd(w, kx)), UINT64_C(4));
  if (s.grid.norm == "ortho")
    n = std::lcm(n, s.norm_discriminant);
  const bool narrow = s.type == ElementType::Float32;
  std::optional<std::uint64_t> first_nan;
  bool all_zero = true, all_negative_zero = true, any = false;
  auto exceptional = e.integer(0);
  terms(s, reader, at, component, n, fuel, [&](auto, auto p, Atom atom) {
    if (atom.nan() && !first_nan)
      first_nan = numeric_ops::converted_nan(atom.raw, s.type, s.type);
    any = true;
    all_zero &= atom.zero();
    if (atom.zero())
      all_negative_zero &= static_cast<bool>(atom.raw >> (narrow ? 31 : 63)) !=
                           (p > n / 4 && p < 3 * (n / 4));
    if (!std::isfinite(atom.value())) {
      auto value = e.binary(atom.raw, narrow);
      if (p > n / 4 && p < 3 * (n / 4))
        value = e.negate(value);
      exceptional = m.add(exceptional, value);
      check(e.status());
    }
  });
  check(e.status());
  if (first_nan)
    return *first_nan;
  if (all_zero && s.kind == Kind::Rfft && component &&
      (kx == 0 || (w % 2 == 0 && kx == w / 2)))
    return 0;
  if (all_zero)
    return any && all_negative_zero
               ? (narrow ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000))
               : 0;
  fuel.poll(2 * Exact::kWords);
  if (!Exact::finite(exceptional))
    return m.rounded(exceptional, narrow);
  auto scale = e.integer(1);
  if (s.inverse() && s.grid.norm == "backward")
    scale = e.fraction(1, h * w);
  // Rational trace invariants are evaluated only after interval rounding is
  // unresolved. No polynomial storage depends on the primitive root order.
  bool trace_ready = false;
  Rational trace_value, trace_square;
  std::uint64_t degree = 0;
  const auto prepare_trace = [&] {
    if (trace_ready)
      return;
    check(m.work.stage());
    Ramanujan roots(n, fuel);
    degree = roots.degree;
    auto u = e.integer(0), v = e.integer(0);
    auto ld = UINT64_C(1), l0 = n / s.norm_discriminant;
    if (s.grid.norm == "ortho") {
      for (;;) {
        fuel.poll(128);
        const auto g = std::gcd(l0, s.norm_discriminant);
        if (g == 1)
          break;
        ld *= g;
        l0 /= g;
      }
    } else {
      l0 = 1;
    }
    Ramanujan independent(l0, fuel);
    terms(s, reader, at, component, n, fuel, [&](auto i, auto p, Atom a) {
      if (a.zero())
        return;
      const auto x = e.binary(a.raw, narrow);
      std::int64_t trace_coefficient = roots(p, fuel);
      if (s.grid.norm == "ortho") {
        trace_coefficient = 0;
        if (p % ld == 0) {
          const auto factor = static_cast<__int128>(s.norm_discriminant) * ld *
                              character(s.norm_discriminant, l0, fuel) *
                              character(s.norm_discriminant, p / ld, fuel) *
                              independent(p / ld, fuel);
          trace_coefficient = static_cast<std::int64_t>(factor);
        }
      }
      v = m.add(v, m.mul(x, e.integer(trace_coefficient)));
      terms(s, reader, at, component, n, fuel, [&](auto j, auto q, Atom b) {
        if (j < i || b.zero())
          return;
        const auto c = roots((p + q) % n, fuel) + roots((p + n - q) % n, fuel);
        if (!c)
          return;
        auto term = m.mul(m.mul(x, e.binary(b.raw, narrow)), e.integer(c));
        if (i == j)
          term = m.div(term, e.integer(2));
        u = m.add(u, term);
      });
    });
    if (s.grid.norm == "ortho") {
      trace_square = m.div(u, e.integer(h * w));
      const auto t = s.norm_discriminant == s.norm_squarefree ? 1U : 2U;
      trace_value =
          m.div(v, e.integer(s.norm_squarefactor * s.norm_squarefree * t));
    } else {
      trace_square = m.mul(u, m.sq(scale));
      trace_value = m.mul(v, scale);
    }
    check(e.status());
    trace_ready = true;
  };
  const auto equal = [&](const Rational& q) {
    prepare_trace();
    const auto delta =
        m.add(m.sub(trace_square, m.scale(m.mul(q, trace_value), 2)),
              m.mul(e.integer(degree), m.sq(q)));
    check(e.status());
    fuel.poll(2 * Exact::kWords);
    return Exact::zero(delta);
  };
  for (unsigned precision = 128, attempt = 0;
       precision <= 4096 && attempt < m.max_refinements;
       precision *= 2, ++attempt) {
    check(m.work.stage());
    auto& d = m.d;
    d.reset();
    d.precision = precision;
    const auto output = d.interval();
    d.integer(output, 0);
    terms(s, reader, at, component, n, fuel, [&](auto, auto p, Atom a) {
      if (a.zero() || !d.active())
        return;
      Directed::Frame frame(d);
      auto sine = d.interval(), cosine = d.interval(), value = d.interval();
      sincos_turn(m, sine, cosine, e.fraction(p, n));
      m.enclose(value, e.binary(a.raw, narrow));
      d.multiply(value, value, cosine);
      d.add(output, output, value);
    });
    auto factor = d.interval();
    if (s.grid.norm == "ortho") {
      m.enclose(factor, e.fraction(1, h * w));
      m.sqrt(factor, factor);
    } else {
      m.enclose(factor, scale);
    }
    d.multiply(output, output, factor);
    check(e.status());
    if (d.fault == Directed::Fault::Failure)
      check(d.status());
    if (!d.active())
      continue;
    const auto lower = d.rounded(output.low, narrow);
    const auto upper = d.rounded(output.high, narrow);
    if (d.fault == Directed::Fault::Failure)
      check(d.status());
    if (!d.active())
      continue;
    if (lower == upper)
      return lower;
    if (equal(e.integer(0)))
      return 0;
    if (std::isfinite(Atom{lower, narrow}.value()) &&
        std::isfinite(Atom{upper, narrow}.value())) {
      const auto a = e.binary(lower, narrow), b = e.binary(upper, narrow);
      const auto midpoint = m.div(m.add(a, b), e.integer(2));
      if (equal(midpoint))
        return m.rounded(midpoint, narrow);
    } else {
      // Infinity is an RN result, not the exact overflow boundary. The tie is
      // maxfinite plus half its highest-binade ULP, for either sign.
      const auto maximum = e.binary(
          narrow ? UINT64_C(0x7f7fffff) : UINT64_C(0x7fefffffffffffff), narrow);
      const auto threshold =
          m.add(maximum, m.value(std::ldexp(1.0, narrow ? 103 : 970)));
      if (equal(threshold))
        return m.rounded(threshold, narrow);
      const auto negative = e.negate(threshold);
      if (equal(negative))
        return m.rounded(negative, narrow);
    }
  }
  throw Status{ErrorCode::OperationFailed,
               "frequency refinement did not certify rounding"};
}
}  // namespace ps::plugin_internal::frequency::trace
