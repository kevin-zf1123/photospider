#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "05-filter/field_affine.hpp"
#include "05-filter/field_wide.hpp"
#include "05-filter/fields.hpp"

namespace ps::plugin_internal::filter {
namespace {
Fast root_bounds(Fast x) {
  if (!x.finite() || x.high < 0)
    return {-INFINITY, INFINITY};
  if (x.low == 0 && x.high == 0)
    return Fast::point(0);
  return {x.low <= 0 ? 0 : numeric_ops::numeric_down(std::sqrt(x.low)),
          numeric_ops::numeric_up(std::sqrt(x.high))};
}
Fast pi_bounds() {
  constexpr double pi = 0x1.921fb54442d18p+1;
  return {numeric_ops::numeric_down(pi), numeric_ops::numeric_up(pi)};
}
std::optional<std::uint64_t> quick_angle(Atom y, Atom x, bool turn, bool eigen,
                                         const FastArithmetic& fast) {
  if (!numeric_ops::accelerated_math_domain(11, y.value(), x.value()))
    return {};
  double angle;
  const double yy = y.value(), xx = x.value();
  photospider_sleef_evaluate(11, &yy, &xx, &angle, 1);
  auto result = numeric_ops::accelerated_math_enclosure(angle);
  if (eigen) {
    result = result * Fast::point(.5);
    if (yy < 0)
      result = result + pi_bounds();
  } else if (turn) {
    result = result / (pi_bounds() * Fast::point(2));
  }
  return fast.finish(result);
}
}  // namespace

std::optional<std::uint64_t> fused_field(Atom base, Atom detail, double gain,
                                         bool narrow, Profile profile,
                                         const Fuel& fuel) {
  const double a = base.value(), b = detail.value();
  if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(gain))
    return {};
  const double value = std::fma(gain, b, a);
  if (!narrow)
    return bits(value);
  if (!std::isfinite(value) || value == 0)
    return generation::convert(value, true);
  FastArithmetic fast{true, profile};
  auto result = fast.finish(
      {numeric_ops::numeric_down(value), numeric_ops::numeric_up(value)});
  if (!result) {
    fuel.poll(128);
    result = affine32(base, detail, gain);
  }
  if (!result) {
    FieldWideArithmetic wide{true};
    result = wide.finish(wide.add(
        wide.from(base),
        wide.multiply(wide.from(detail), wide.from(Atom{bits(gain)}))));
  }
  return result;
}

std::uint64_t field_angle(generation::Math& m, Rational y, Rational x,
                          bool narrow, bool turn, bool eigen) {
  auto& e = m.exact;
  if (Exact::nan(y) || Exact::nan(x))
    return m.rounded(e.invalid(), narrow);
  const bool yz = Exact::zero(y), xz = Exact::zero(x);
  if (yz && xz)
    return 0;
  std::optional<Rational> turns;
  if (Exact::infinity(y) && Exact::infinity(x))
    turns = e.fraction(x.negative ? 3 : 1, 8);
  else if (Exact::infinity(y) || xz)
    turns = e.fraction(1, 4);
  else if (yz || Exact::infinity(x))
    turns = e.fraction(x.negative ? 1 : 0, 2);
  else if (e.order(e.absolute(y), e.absolute(x)) == 0)
    turns = e.fraction(x.negative ? 3 : 1, 8);
  check(e.status());
  if (turns) {
    // The negative x axis is +pi even for a negative zero y.
    turns->negative = y.negative && !(yz && x.negative);
    if (eigen) {
      *turns = m.div(*turns, e.integer(2));
      if (turns->negative && !Exact::zero(*turns))
        *turns = m.add(*turns, e.fraction(1, 2));
      if (Exact::zero(*turns))
        turns->negative = false;
    }
    if (turn)
      return m.rounded(*turns, narrow, true);
    if (Exact::zero(*turns))
      return m.rounded(*turns, narrow, true);
    return m.certify(narrow, [&](generation::Interval out) {
      auto factor = m.d.interval(), pi = m.d.interval();
      m.enclose(factor, m.scale(*turns, 2));
      m.d.constant(pi, true);
      m.d.multiply(out, factor, pi);
    });
  }
  return m.certify(narrow, [&](generation::Interval out) {
    m.atan2(out, y, x);
    if (eigen) {
      m.d.scale(out, out, -1);
      if (y.negative) {
        auto pi = m.d.interval();
        m.d.constant(pi, true);
        m.d.add(out, out, pi);
      }
    } else if (turn) {
      auto two_pi = m.d.interval();
      m.d.constant(two_pi, true);
      m.d.scale(two_pi, two_pi, 1);
      m.d.divide(out, out, two_pi);
    }
  });
}

void run_pointwise(const Fields& f, const Kernel& kernel,
                   const ps_kernel_invocation_v8& inv, const Work& work) {
  const Fuel fuel{work};
  const Reader reader{kernel, inv};
  generation::MathArena arena(inv.scratch[0].allocation, work);
  auto& m = *arena;
  m.exact.bind(fuel);
  m.d.consume = core_internal::WorkConsumer(fuel);
  const bool narrow = f.type == ElementType::Float32;
  const unsigned port = kernel.writes[0].first;
  FastArithmetic fast{narrow, f.profile};
  samples(inv.outputs[0], [&](const Coordinate& at, std::uint64_t) {
    const auto load = [&](unsigned p) {
      fuel.poll();
      return Atom{reader.raw(p, 0, at), narrow};
    };
    Atom a = load(0), b = load(1), c{};
    if (f.kind == FieldKind::Eigen)
      c = load(2);
    if ((f.kind == FieldKind::Orientation && port == 1) ||
        (f.kind == FieldKind::Eigen && port == 3)) {
      const bool valid = f.kind == FieldKind::Orientation
                             ? !a.nan() && !b.nan() && !(a.zero() && b.zero())
                             : !a.nan() && !b.nan() && !c.nan() &&
                                   !(a.value() == c.value() && b.zero());
      store(inv.outputs[0], at, valid ? 1 : 0);
      return;
    }
    std::optional<std::uint64_t> result;
    ExactArithmetic exact{m.exact, f.type, {}};
    if (f.kind == FieldKind::Detail) {
      result = fused_field(a, b, f.gain, narrow, f.profile, fuel);
      if (!result) {
        const auto aa = exact.from(a), bb = exact.from(b);
        result = exact.finish(m.add(aa, m.mul(m.value(f.gain), bb)));
      }
    } else if (f.kind == FieldKind::Magnitude) {
      if (a.zero() && b.zero()) {
        store(inv.outputs[0], at, 0);
        return;
      }
      auto aa = fast.absolute(fast.from(a)), bb = fast.absolute(fast.from(b));
      if (std::isfinite(a.value()) && std::isfinite(b.value())) {
        const auto value = f.mode == "l1"
                               ? fast.add(aa, bb)
                               : root_bounds(fast.add(fast.multiply(aa, aa),
                                                      fast.multiply(bb, bb)));
        result = fast.finish(value);
      }
      if (!result) {
        auto x = exact.from(a), y = exact.from(b);
        if (f.mode == "l1") {
          result =
              exact.finish(m.add(m.exact.absolute(x), m.exact.absolute(y)));
        } else {
          auto squared = m.add(m.sq(x), m.sq(y));
          result = exact.first_nan ? *exact.first_nan
                                   : m.rounded_sqrt(squared, narrow);
        }
      }
    } else if (f.kind == FieldKind::Orientation) {
      // atan2's written operand order is gy, gx.
      result = quick_angle(b, a, f.mode == "turn", false, fast);
      if (!result) {
        auto yy = exact.from(b), xx = exact.from(a);
        result = exact.first_nan
                     ? *exact.first_nan
                     : field_angle(m, yy, xx, narrow, f.mode == "turn", false);
      }
    } else {
      if (port <= 1) {
        const auto aa = fast.from(a), bb = fast.from(b), cc = fast.from(c);
        if (aa.finite() && bb.finite() && cc.finite()) {
          const auto difference = aa - cc;
          const auto d = root_bounds(
              fast.add(fast.multiply(difference, difference),
                       fast.multiply(Fast::point(4), fast.multiply(bb, bb))));
          auto value = (port == 0 ? fast.add(fast.add(aa, cc), d)
                                  : fast.add(fast.add(aa, cc), -d)) *
                       Fast::point(.5);
          result = fast.finish(value);
        }
        if (!result) {
          const auto aa = exact.from(a), cc = exact.from(c), bb = exact.from(b);
          auto bias = m.div(m.add(aa, cc), m.exact.integer(2));
          auto radicand = m.add(m.sq(m.sub(aa, cc)), m.scale(m.sq(bb), 4));
          if (exact.first_nan) {
            result = exact.first_nan;
          } else if (!Exact::finite(bias) || !Exact::finite(radicand)) {
            auto root = m.exact.binary(m.rounded_sqrt(radicand, false));
            result = exact.finish(m.add(
                bias,
                m.div(m.scale(root, port == 0 ? 1 : -1), m.exact.integer(2))));
          } else {
            result =
                m.affine_sqrt(bias, m.exact.fraction(port == 0 ? 1 : -1, 2),
                              radicand, narrow);
          }
        }
      } else {
        if (!a.nan() && !b.nan() && !c.nan() && a.value() == c.value() &&
            b.zero()) {
          store(inv.outputs[0], at, 0);
          return;
        }
        if (std::isfinite(a.value()) && std::isfinite(b.value()) &&
            std::isfinite(c.value())) {
          const double xx = a.value() - c.value(), yy = 2 * b.value();
          const auto difference = fast.add(fast.from(a), -fast.from(c));
          if (difference.low == difference.high && std::isfinite(yy))
            result = quick_angle({bits(yy)}, {bits(xx)}, false, true, fast);
        }
        if (!result) {
          auto bb = exact.from(b), aa = exact.from(a), cc = exact.from(c);
          result = exact.first_nan
                       ? *exact.first_nan
                       : field_angle(m, m.scale(bb, 2), m.sub(aa, cc), narrow,
                                     false, true);
        }
      }
    }
    store(inv.outputs[0], at, *result);
  });
  fuel.flush();
}
}  // namespace ps::plugin_internal::filter
