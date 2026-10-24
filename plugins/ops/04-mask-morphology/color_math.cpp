#include "04-mask-morphology/color_math.hpp"

namespace ps::plugin_internal::mask {
namespace {
int quadratic_order(Math& m, const Rational& a, const Rational& b,
                    const Rational& v, const Rational& target) {
  const auto z = m.sub(target, a);
  if (Exact::zero(b) || Exact::zero(v))
    return m.exact.order(a, target);
  if (b.negative) {
    if (!z.negative || Exact::zero(z))
      return -1;
    return -m.exact.order(m.mul(m.sq(b), v), m.sq(z));
  }
  if (z.negative && !Exact::zero(z))
    return 1;
  return m.exact.order(m.mul(m.sq(b), v), m.sq(z));
}
void unit_interval(Directed& d, Interval v) {
  Directed::Frame frame(d);
  auto zero = d.interval(), one = d.interval();
  d.integer(zero, 0);
  d.integer(one, 1);
  if (d.compare(v.low, zero.low) < 0)
    d.copy(v.low, zero.low);
  if (d.compare(v.high, one.high) > 0)
    d.copy(v.high, one.high);
}
void fraction(Directed& d, Interval out, std::int64_t n, std::int64_t den = 1) {
  Directed::Frame frame(d);
  auto divisor = d.interval();
  d.integer(out, n);
  d.integer(divisor, den);
  d.divide(out, out, divisor);
}
void times(Directed& d, Interval out, Interval v, std::int64_t n,
           std::int64_t den = 1) {
  Directed::Frame frame(d);
  auto factor = d.interval();
  fraction(d, factor, n, den);
  d.multiply(out, v, factor);
}
void seventh(Directed& d, Interval out, Interval value) {
  Directed::Frame frame(d);
  auto square = d.interval(), fourth = d.interval(), base = d.interval();
  d.copy(base, value);
  d.multiply(square, base, base);
  d.multiply(fourth, square, square);
  d.multiply(out, fourth, square);
  d.multiply(out, out, base);
}
void cosine_degrees(Math& m, Interval out, Interval angle) {
  auto& d = m.d;
  Directed::Frame frame(d);
  auto radians = d.interval(), sine = d.interval(), pi = d.interval();
  d.constant(pi, true);
  times(d, radians, angle, 1, 180);
  d.multiply(radians, radians, pi);
  m.sincos(sine, out, radians);
}
void sine_degrees(Math& m, Interval out, Interval angle) {
  auto& d = m.d;
  Directed::Frame frame(d);
  auto radians = d.interval(), cosine = d.interval(), pi = d.interval();
  d.constant(pi, true);
  times(d, radians, angle, 1, 180);
  d.multiply(radians, radians, pi);
  m.sincos(out, cosine, radians);
}
// x=(1+G)*a. Its sign and all collinearity predicates are inherited from a.
// The half-angle identity avoids an uncertain |x| versus |y| comparison.
void hue_degrees(Math& m, Interval out, Interval x, Interval norm,
                 const Rational& a, const Rational& b) {
  auto& d = m.d;
  Directed::Frame frame(d);
  if (Exact::zero(b)) {
    d.integer(out, a.negative && !Exact::zero(a) ? 180 : 0);
    return;
  }
  if (Exact::zero(a)) {
    d.integer(out, b.negative ? 270 : 90);
    return;
  }
  auto xx = d.interval(), yy = d.interval(), angle = d.interval();
  auto denominator = d.interval(), pi = d.interval();
  if (a.negative)
    d.negate(xx, x);
  else
    d.copy(xx, x);
  m.enclose(yy, m.exact.absolute(b));
  d.add(denominator, norm, xx);
  d.divide(angle, yy, denominator);
  // The exact half-angle ratio belongs to [0,1].
  unit_interval(d, angle);
  m.functions.functions.arctangent(angle, angle);
  d.scale(angle, angle, 1);
  d.constant(pi, true);
  if (a.negative)
    d.subtract(angle, pi, angle);
  if (b.negative) {
    auto full = d.interval();
    d.scale(full, pi, 1);
    d.subtract(angle, full, angle);
  }
  d.divide(angle, angle, pi);
  times(d, out, angle, 180);
}
int hue_order(Math& m, const Rational& a1, const Rational& b1,
              const Rational& a2, const Rational& b2, const Rational& cross) {
  const bool lower1 = b1.negative && !Exact::zero(b1);
  const bool lower2 = b2.negative && !Exact::zero(b2);
  if (lower1 != lower2)
    return lower1 ? 1 : -1;
  if (!Exact::zero(cross))
    return cross.negative ? 1 : -1;
  if (Exact::zero(b1) && Exact::zero(b2) && a1.negative != a2.negative)
    return a1.negative ? 1 : -1;
  (void)m;
  return 0;
}
}  // namespace

std::uint64_t coverage_quadratic(Math& m, const Rational& a, const Rational& b,
                                 const Rational& v, bool narrow) {
  if (Exact::zero(b) || Exact::zero(v))
    return m.rounded(a, narrow);
  for (unsigned precision = 128; precision <= 4096; precision *= 2) {
    m.work.poll();
    auto& d = m.d;
    d.reset();
    d.precision = precision;
    auto result = d.interval(), aa = d.interval(), bb = d.interval(),
         vv = d.interval();
    m.enclose(aa, a);
    m.enclose(bb, b);
    m.enclose(vv, v);
    m.sqrt(vv, vv);
    d.multiply(vv, vv, bb);
    d.add(result, aa, vv);
    unit_interval(d, result);
    check(m.exact.status());
    if (d.fault == Directed::Fault::Failure)
      check(d.status());
    if (!d.active())
      continue;
    const auto lo = d.rounded(result.low, narrow);
    const auto hi = d.rounded(result.high, narrow);
    if (!d.active())
      continue;
    if (lo == hi)
      return lo;
    if (hi == lo + 1) {
      const auto midpoint =
          m.div(m.add(m.exact.binary(lo, narrow), m.exact.binary(hi, narrow)),
                m.exact.integer(2));
      const auto order = quadratic_order(m, a, b, v, midpoint);
      check(m.exact.status());
      return order < 0 ? lo : order > 0 ? hi : (lo & 1) ? hi : lo;
    }
  }
  throw Status{ErrorCode::OperationFailed, "mask radical rounding unresolved",
               DomainErrorCode{UINT64_C(0x4d41534b0002)}};
}
std::uint64_t Response::rational(Math& m, const Rational& distance,
                                 bool narrow) const {
  const auto i = m.value(inner), o = m.value(outer);
  if (m.exact.order(distance, i) <= 0)
    return convert(1, narrow);
  if (inner == outer || m.exact.order(distance, o) >= 0)
    return 0;
  const auto t = m.div(m.sub(distance, i), m.sub(o, i));
  return m.rounded(m.sub(m.exact.integer(1), curve01(m.exact, t, smooth)),
                   narrow);
}
std::uint64_t Response::squared(Math& m, const Rational& v, bool narrow) const {
  const auto i = m.value(inner), o = m.value(outer);
  if (m.exact.order(v, m.sq(i)) <= 0)
    return convert(1, narrow);
  if (inner == outer || m.exact.order(v, m.sq(o)) >= 0)
    return 0;
  const auto w = m.sub(o, i);
  if (!smooth)
    return coverage_quadratic(m, m.div(o, w), m.div(m.exact.integer(-1), w), v,
                              narrow);
  const auto w2 = m.sq(w), w3 = m.mul(w2, w), i2 = m.sq(i);
  const auto a = m.sub(
      m.sub(m.exact.integer(1), m.div(m.scale(m.add(v, i2), 3), w2)),
      m.div(m.scale(m.add(m.scale(m.mul(i, v), 3), m.mul(i, i2)), 2), w3));
  const auto b = m.add(m.div(m.scale(i, 6), w2),
                       m.div(m.scale(m.add(v, m.scale(i2, 3)), 2), w3));
  return coverage_quadratic(m, a, b, v, narrow);
}
std::uint64_t Response::quadratic(Math& m, const Rational& a, const Rational& b,
                                  const Rational& v, bool narrow) const {
  const auto i = m.value(inner), o = m.value(outer);
  if (quadratic_order(m, a, b, v, i) <= 0)
    return convert(1, narrow);
  if (inner == outer || quadratic_order(m, a, b, v, o) >= 0)
    return 0;
  const auto w = m.sub(o, i), u = m.div(m.sub(a, i), w), z = m.div(b, w);
  if (!smooth)
    return coverage_quadratic(m, m.sub(m.exact.integer(1), u),
                              m.exact.negate(z), v, narrow);
  const auto u2 = m.sq(u), z2v = m.mul(m.sq(z), v);
  const auto bias = m.add(m.sub(m.exact.integer(1), m.scale(m.add(u2, z2v), 3)),
                          m.scale(m.mul(u, m.add(u2, m.scale(z2v, 3))), 2));
  const auto factor = m.sub(m.scale(m.mul(z, m.add(m.scale(u2, 3), z2v)), 2),
                            m.scale(m.mul(u, z), 6));
  return coverage_quadratic(m, bias, factor, v, narrow);
}
void Response::interval(Math& m, Interval out, Interval distance) const {
  auto& d = m.d;
  Directed::Frame frame(d);
  auto i = d.interval(), o = d.interval(), t = d.interval(), one = d.interval();
  m.enclose(i, m.value(inner));
  m.enclose(o, m.value(outer));
  d.integer(one, 1);
  if (d.compare(distance.high, i.low) <= 0) {
    d.integer(out, 1);
    return;
  }
  if (d.compare(distance.low, o.high) >= 0 &&
      (inner != outer || d.compare(distance.low, i.high) > 0)) {
    d.integer(out, 0);
    return;
  }
  if (inner == outer) {
    d.retry();
    return;
  }
  d.subtract(t, distance, i);
  d.subtract(o, o, i);
  d.divide(t, t, o);
  unit_interval(d, t);
  if (smooth) {
    auto square = d.interval(), factor = d.interval();
    d.multiply(square, t, t);
    d.scale(factor, t, 1);
    d.integer(i, 3);
    d.subtract(factor, i, factor);
    d.multiply(t, square, factor);
  }
  d.subtract(out, one, t);
  unit_interval(d, out);
}

std::uint64_t ciede2000(Math& m, const std::array<Rational, 3>& p,
                        const std::array<Rational, 3>& q,
                        const std::array<double, 3>& factors,
                        const Response& response, bool narrow) {
  bool equal = true;
  for (unsigned j = 0; j < 3; ++j)
    equal &= m.exact.order(p[j], q[j]) == 0;
  if (equal)
    return convert(1, narrow);
  const auto l1 = m.scale(p[0], 100), l2 = m.scale(q[0], 100);
  const auto c1sq = m.add(m.sq(p[1]), m.sq(p[2]));
  const auto c2sq = m.add(m.sq(q[1]), m.sq(q[2]));
  const bool neutral = Exact::zero(c1sq) || Exact::zero(c2sq);
  if (m.exact.order(p[1], q[1]) == 0 && m.exact.order(p[2], q[2]) == 0) {
    const auto lightness =
        m.div(m.exact.absolute(m.sub(l1, l2)), m.value(factors[0]));
    const auto shift =
        m.sub(m.div(m.add(l1, l2), m.exact.integer(2)), m.exact.integer(50));
    const auto v = m.add(m.exact.integer(20), m.sq(shift));
    const auto c = m.mul(m.exact.fraction(3, 200), m.sq(shift));
    const auto denominator = m.sub(v, m.sq(c));
    if (Exact::zero(denominator))
      return response.rational(m, m.div(lightness, m.exact.integer(2)), narrow);
    return response.quadratic(
        m, m.div(m.mul(lightness, v), denominator),
        m.div(m.exact.negate(m.mul(lightness, c)), denominator), v, narrow);
  }
  // On the b axis G does not change chroma; zero hue difference removes all
  // trigonometric terms. Preserve exact rational threshold equalities.
  if (Exact::zero(p[1]) && Exact::zero(q[1]) &&
      (neutral || p[2].negative == q[2].negative) &&
      (m.exact.order(l1, l2) == 0 ||
       m.exact.order(m.add(l1, l2), m.exact.integer(100)) == 0)) {
    const auto a = m.exact.absolute(p[2]), b = m.exact.absolute(q[2]);
    const auto scale =
        m.add(m.exact.integer(1), m.mul(m.exact.fraction(9, 400), m.add(a, b)));
    const auto dc = m.div(m.sub(b, a), m.mul(m.value(factors[1]), scale));
    const auto dl = m.div(m.sub(l2, l1), m.value(factors[0]));
    return response.squared(m, m.add(m.sq(dc), m.sq(dl)), narrow);
  }
  const auto cross = m.sub(m.mul(p[1], q[2]), m.mul(p[2], q[1]));
  const auto dot = m.add(m.mul(p[1], q[1]), m.mul(p[2], q[2]));
  const auto order = hue_order(m, p[1], p[2], q[1], q[2], cross);
  const bool small =
      Exact::zero(cross) || (order <= 0 ? !cross.negative : cross.negative);
  // For an angular gap above pi, the angle sum belongs to (pi,3*pi).
  // Its sine has the sign of this exact expression; the shared a-axis scale
  // is positive. This also resolves an exact sum of 360 degrees.
  const auto sine_sum = m.add(m.mul(p[1], q[2]), m.mul(q[1], p[2]));
  const bool zero_hue = neutral || (Exact::zero(cross) && !dot.negative);
  return m.certify(narrow, [&](Interval out) {
    auto& d = m.d;
    auto cp1 = d.interval(), cp2 = d.interval(), ap1 = d.interval(),
         ap2 = d.interval();
    auto h1 = d.interval(), h2 = d.interval(), dh = d.interval(),
         hm = d.interval();
    {
      Directed::Frame frame(d);
      auto c1 = d.interval(), c2 = d.interval(), g = d.interval(),
           v = d.interval(), denominator = d.interval(), one = d.interval(),
           b = d.interval();
      m.enclose(c1, c1sq);
      m.sqrt(c1, c1);
      m.enclose(c2, c2sq);
      m.sqrt(c2, c2);
      d.add(g, c1, c2);
      d.scale(g, g, -1);
      seventh(d, g, g);
      d.integer(v, INT64_C(6103515625));  // 25^7
      d.add(denominator, g, v);
      d.divide(g, g, denominator);
      m.sqrt(g, g);
      d.integer(one, 1);
      d.subtract(g, one, g);
      d.scale(g, g, -1);
      d.add(g, one, g);
      m.enclose(ap1, p[1]);
      d.multiply(ap1, ap1, g);
      m.enclose(ap2, q[1]);
      d.multiply(ap2, ap2, g);
      d.multiply(cp1, ap1, ap1);
      m.enclose(b, m.sq(p[2]));
      d.add(cp1, cp1, b);
      m.sqrt(cp1, cp1);
      d.multiply(cp2, ap2, ap2);
      m.enclose(b, m.sq(q[2]));
      d.add(cp2, cp2, b);
      m.sqrt(cp2, cp2);
    }
    if (!zero_hue) {
      hue_degrees(m, h1, ap1, cp1, p[1], p[2]);
      hue_degrees(m, h2, ap2, cp2, q[1], q[2]);
      Directed::Frame frame(d);
      auto full = d.interval(), hs = d.interval();
      d.integer(full, 360);
      d.add(hs, h1, h2);
      if (neutral) {
        d.integer(dh, 0);
        d.copy(hm, hs);
      } else {
        if (Exact::zero(cross)) {
          d.integer(dh, dot.negative ? (order <= 0 ? 180 : -180) : 0);
        } else {
          d.subtract(dh, h2, h1);
          if (!small) {
            if (order <= 0)
              d.subtract(dh, dh, full);
            else
              d.add(dh, dh, full);
          }
        }
        if (small) {
          d.scale(hm, hs, -1);
        } else {
          if (sine_sum.negative && !Exact::zero(sine_sum))
            d.add(hm, hs, full);
          else
            d.subtract(hm, hs, full);
          d.scale(hm, hm, -1);
        }
      }
    }
    auto dl = d.interval(), dc = d.interval(), dh_big = d.interval(),
         lm = d.interval(), cm = d.interval(), sl = d.interval(),
         sc = d.interval(), sh = d.interval(), rt = d.interval();
    m.enclose(dl, m.sub(l2, l1));
    d.subtract(dc, cp2, cp1);
    if (zero_hue) {
      d.integer(dh_big, 0);
    } else {
      Directed::Frame frame(d);
      auto angle = d.interval(), sine = d.interval();
      d.scale(angle, dh, -1);
      sine_degrees(m, sine, angle);
      d.multiply(dh_big, cp1, cp2);
      m.sqrt(dh_big, dh_big);
      d.scale(dh_big, dh_big, 1);
      d.multiply(dh_big, dh_big, sine);
    }
    m.enclose(lm, m.div(m.add(l1, l2), m.exact.integer(2)));
    d.add(cm, cp1, cp2);
    d.scale(cm, cm, -1);
    {
      Directed::Frame frame(d);
      auto v = d.interval(), temp = d.interval(), t = d.interval(),
           angle = d.interval();
      d.integer(v, 50);
      d.subtract(v, lm, v);
      d.multiply(v, v, v);
      d.integer(temp, 20);
      d.add(temp, v, temp);
      m.sqrt(temp, temp);
      d.divide(v, v, temp);
      times(d, v, v, 15, 1000);
      d.integer(sl, 1);
      d.add(sl, sl, v);
      times(d, sc, cm, 45, 1000);
      d.integer(v, 1);
      d.add(sc, sc, v);
      // Exact zero hue difference removes both the hue square and rotation
      // cross term. Their hue angles, trigonometric scales and exponential
      // are unnecessary even when the other two terms are nonzero.
      if (!zero_hue) {
        d.integer(t, 1);
        d.integer(temp, 30);
        d.subtract(angle, hm, temp);
        cosine_degrees(m, v, angle);
        times(d, v, v, 17, 100);
        d.subtract(t, t, v);
        d.scale(angle, hm, 1);
        cosine_degrees(m, v, angle);
        times(d, v, v, 24, 100);
        d.add(t, t, v);
        times(d, angle, hm, 3);
        d.integer(temp, 6);
        d.add(angle, angle, temp);
        cosine_degrees(m, v, angle);
        times(d, v, v, 32, 100);
        d.add(t, t, v);
        d.scale(angle, hm, 2);
        d.integer(temp, 63);
        d.subtract(angle, angle, temp);
        cosine_degrees(m, v, angle);
        times(d, v, v, 20, 100);
        d.subtract(t, t, v);
        d.multiply(sh, cm, t);
        times(d, sh, sh, 15, 1000);
        d.integer(v, 1);
        d.add(sh, sh, v);
        d.integer(temp, 275);
        d.subtract(angle, hm, temp);
        times(d, angle, angle, 1, 25);
        d.multiply(angle, angle, angle);
        d.negate(angle, angle);
        m.functions.exponential(angle, angle);
        times(d, angle, angle, 60);
        sine_degrees(m, rt, angle);
        seventh(d, v, cm);
        d.integer(temp, INT64_C(6103515625));
        d.add(temp, temp, v);
        d.divide(v, v, temp);
        m.sqrt(v, v);
        d.scale(v, v, 1);
        d.multiply(rt, rt, v);
        d.negate(rt, rt);
      }
    }
    {
      Directed::Frame frame(d);
      auto factor = d.interval(), distance = d.interval(), temp = d.interval();
      m.enclose(factor, m.value(factors[0]));
      d.multiply(sl, sl, factor);
      d.divide(dl, dl, sl);
      m.enclose(factor, m.value(factors[1]));
      d.multiply(sc, sc, factor);
      d.divide(dc, dc, sc);
      if (!zero_hue) {
        m.enclose(factor, m.value(factors[2]));
        d.multiply(sh, sh, factor);
        d.divide(dh_big, dh_big, sh);
      }
      d.multiply(distance, dl, dl);
      d.multiply(temp, dc, dc);
      d.add(distance, distance, temp);
      if (!zero_hue) {
        d.multiply(temp, dh_big, dh_big);
        d.add(distance, distance, temp);
        d.multiply(temp, dc, dh_big);
        d.multiply(temp, temp, rt);
        d.add(distance, distance, temp);
      }
      m.sqrt(distance, distance);
      response.interval(m, out, distance);
    }
  });
}
}  // namespace ps::plugin_internal::mask
