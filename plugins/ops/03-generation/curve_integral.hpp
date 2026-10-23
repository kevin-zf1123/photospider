#pragma once

#include <array>
#include <cstdint>

#include "03-generation/interval_support.hpp"
#include "03-generation/path_support.hpp"

namespace ps::plugin_internal::generation {
inline constexpr unsigned kIntegralOrder = 16, kIntegralDepth = 512;
using IntegralJet = std::array<OwnedInterval, kIntegralOrder + 2>;
struct IntegralPanel final {
  Rational a, b, tolerance;
  unsigned depth = 0;
};
struct CurveIntegralScratch final {
  EvaluationScratch evaluation;
  std::array<std::array<Rational, 17>, 3> coefficient{}, values{};
  std::array<Rational, 17> polynomial{};
  std::array<IntegralJet, 3> homogeneous{}, position{};
  std::array<IntegralJet, 2> velocity{};
  IntegralJet squared{}, point_speed{}, range_speed{};
  std::array<IntegralPanel, kIntegralDepth + 2> panels;
  CurveValue rational_value;
  explicit CurveIntegralScratch(const Work& work) : evaluation(work) {}
};

// Interval Taylor integration, with a separate rigorous remainder over each
// complete panel. No agreement between ordinary quadrature estimates is used.
class CurveIntegral final {
 public:
  const Reader& reader;
  CurveIntegralScratch& scratch;
  Math& m;
  IntervalMath im;
  CurveMath curves;
  Curve curve;
  unsigned degree = 0;
  Rational span_a, span_b, weight_min, weight_max, circle_squared_radius;
  bool homogeneous = false, circle = false, leaf_straight = false;
  std::uint64_t panels_used = 0, panel_limit;
  explicit CurveIntegral(const Reader& source, CurveIntegralScratch& workspace,
                         std::uint64_t max_panels = 1048576)
      : reader(source),
        scratch(workspace),
        m(workspace.evaluation.math),
        im(m),
        curves(source, workspace.evaluation),
        panel_limit(max_panels) {}
  Rational field(unsigned column) const {
    return m.input(reader, 0, curve.field, {curve.row, column});
  }
  Rational control(unsigned j, unsigned axis) const {
    return curve.core
               ? m.input(reader, 0, curve.field, {curve.control[j], axis})
               : m.input(reader, 0, curve.field, {curve.row, j, axis});
  }
  void reset_coefficients() {
    for (auto& axis : scratch.coefficient)
      for (auto& value : axis)
        value = Rational{};
  }
  void initialize(const Curve& selected) {
    curve = selected;
    span_a = m.exact.integer(0);
    span_b = m.exact.integer(1);
    homogeneous = false;
    circle = false;
    reset_coefficients();
    if (curve.kind == CurveKind::Arc || curve.kind == CurveKind::FullTurn) {
      const auto u2 = m.add(m.sq(field(2)), m.sq(field(3)));
      const auto v2 = m.add(m.sq(field(4)), m.sq(field(5)));
      const auto uv =
          m.add(m.mul(field(2), field(4)), m.mul(field(3), field(5)));
      circle = Exact::zero(uv) && m.exact.order(u2, v2) == 0;
      circle_squared_radius = u2;
    }
    if (curve.kind == CurveKind::Arc || curve.kind == CurveKind::FullTurn ||
        curve.kind == CurveKind::Close)
      return;
    if (curve.kind == CurveKind::Bezier) {
      degree = curve.degree;
      for (unsigned axis = 0; axis < 2; ++axis) {
        auto& values = scratch.values[axis];
        for (unsigned j = 0; j <= degree; ++j)
          values[j] = control(j, axis);
        unsigned choose = 1;
        for (unsigned k = 0; k <= degree; ++k) {
          scratch.coefficient[axis][k] = m.scale(values[0], choose);
          for (unsigned j = 0; j < degree - k; ++j)
            values[j] = m.sub(values[j + 1], values[j]);
          if (k < degree)
            choose = choose * (degree - k) / (k + 1);
        }
      }
    } else if (curve.kind == CurveKind::Hermite) {
      degree = 3;
      for (unsigned axis = 0; axis < 2; ++axis) {
        const auto a = control(0, axis), b = control(1, axis);
        const auto da = control(2, axis), db = control(3, axis);
        auto& c = scratch.coefficient[axis];
        c[0] = a;
        c[1] = da;
        c[2] = m.sub(m.scale(m.sub(b, a), 3), m.add(m.scale(da, 2), db));
        c[3] = m.add(m.scale(m.sub(a, b), 2), m.add(da, db));
      }
    }
  }
  void initialize_spline(const Curve& selected, std::uint64_t span) {
    initialize(selected);
    homogeneous = true;
    const auto record = [&](unsigned c) {
      return static_cast<std::uint64_t>(
          reader.index(0, curve.field, {curve.row, c}));
    };
    degree = record(0);
    const auto cb = record(1), n = record(2), kb = record(3);
    const auto knot = [&](std::uint64_t j) {
      return m.input(reader, 0, 10, {kb + j});
    };
    const auto start = knot(degree), end = knot(n);
    const auto ka = knot(span), kb_value = knot(span + 1);
    span_a = m.div(m.sub(ka, start), m.sub(end, start));
    span_b = m.div(m.sub(kb_value, start), m.sub(end, start));
    weight_min = weight_max = m.input(reader, 0, 11, {cb + span - degree});
    for (unsigned j = 1; j <= degree; ++j) {
      const auto weight = m.input(reader, 0, 11, {cb + span - degree + j});
      weight_min = m.min(weight_min, weight);
      weight_max = m.max(weight_max, weight);
    }
    // Interpolate the homogeneous B-spline polynomials on this fixed knot
    // span. Evaluation at its endpoints uses this span's one-sided polynomial.
    auto& basis = scratch.evaluation.basis;
    auto& left = scratch.evaluation.left;
    auto& right = scratch.evaluation.right;
    for (unsigned sample = 0; sample <= degree; ++sample) {
      m.work.poll();
      const auto u =
          degree ? m.exact.fraction(sample, degree) : m.exact.integer(0);
      const auto tau = m.lerp(ka, kb_value, u);
      basis[0] = m.exact.integer(1);
      for (unsigned j = 1; j <= degree; ++j) {
        left[j] = m.sub(tau, knot(span + 1 - j));
        right[j] = m.sub(knot(span + j), tau);
        auto saved = m.exact.integer(0);
        for (unsigned r = 0; r < j; ++r) {
          const auto den = m.add(right[r + 1], left[j - r]);
          const auto value =
              Exact::zero(den) ? m.exact.integer(0) : m.div(basis[r], den);
          basis[r] = m.add(saved, m.mul(right[r + 1], value));
          saved = m.mul(left[j - r], value);
        }
        basis[j] = saved;
      }
      for (unsigned axis = 0; axis < 3; ++axis) {
        auto sum = m.exact.integer(0);
        for (unsigned j = 0; j <= degree; ++j) {
          const auto row = cb + span - degree + j;
          const auto weight = m.input(reader, 0, 11, {row});
          const auto value = axis == 2 ? m.exact.integer(1)
                                       : m.input(reader, 0, 9, {row, axis});
          sum = m.add(sum, m.mul(m.mul(basis[j], weight), value));
        }
        scratch.values[axis][sample] = sum;
      }
    }
    for (unsigned axis = 0; axis < 3; ++axis) {
      auto& values = scratch.values[axis];
      auto& poly = scratch.polynomial;
      for (auto& v : poly)
        v = Rational{};
      poly[0] = m.exact.integer(1);
      auto factor = m.exact.integer(1);
      for (unsigned k = 0; k <= degree; ++k) {
        const auto value = m.mul(values[0], factor);
        for (unsigned j = 0; j <= k; ++j)
          scratch.coefficient[axis][j] =
              m.add(scratch.coefficient[axis][j], m.mul(value, poly[j]));
        for (unsigned j = 0; j < degree - k; ++j)
          values[j] = m.sub(values[j + 1], values[j]);
        if (k < degree) {
          const auto root = m.exact.fraction(k, degree);
          for (int j = static_cast<int>(k) + 1; j >= 0; --j)
            poly[j] = m.sub(
                j ? poly[j - 1] : Rational{},
                j <= static_cast<int>(k) ? m.mul(root, poly[j]) : Rational{});
          factor = m.mul(factor, m.exact.fraction(degree, k + 1));
        }
      }
    }
  }
  void polynomial_jets(const Rational& a, const Rational& b) {
    const auto u = im.range(a, b);
    for (unsigned axis = 0; axis < (homogeneous ? 3U : 2U); ++axis) {
      auto& jet =
          homogeneous ? scratch.homogeneous[axis] : scratch.position[axis];
      for (unsigned k = 0; k < jet.size(); ++k) {
        jet[k] = im.integer(0);
        if (k > degree)
          continue;
        std::uint64_t choose = 1;
        for (unsigned j = 1; j <= k; ++j)
          choose = choose * (degree + 1 - j) / j;
        for (int power = degree; power >= static_cast<int>(k); --power) {
          jet[k] = im.add(
              im.mul(jet[k], u),
              im.rational(m.scale(scratch.coefficient[axis][power], choose)));
          if (power > static_cast<int>(k))
            choose = choose * (power - k) / power;
        }
      }
    }
    if (homogeneous) {
      auto& denominator = scratch.homogeneous[2];
      denominator[0] =
          im.intersect(denominator[0], im.range(weight_min, weight_max));
      for (unsigned axis = 0; axis < 2; ++axis) {
        auto& out = scratch.position[axis];
        for (unsigned k = 0; k < out.size(); ++k) {
          auto value = scratch.homogeneous[axis][k];
          for (unsigned j = 1; j <= k; ++j)
            value = im.sub(value, im.mul(denominator[j], out[k - j]));
          out[k] = im.div(value, denominator[0]);
        }
      }
    }
  }
  void arc_jets(const Rational& a, const Rational& b) {
    const auto middle = m.div(m.add(a, b), m.exact.integer(2));
    auto sweep = im.rational(field(7));
    if (curve.kind == CurveKind::FullTurn) {
      m.d.constant(sweep.view(), true);
      sweep = im.scale(sweep, 1);
      if (field(7).negative)
        sweep = im.negate(sweep);
    }
    const auto angle =
        im.add(im.rational(field(6)), im.mul(im.rational(middle), sweep));
    OwnedInterval sine, cosine;
    m.sincos(sine.view(), cosine.view(), angle.view());
    im.check_state();
    const auto radius =
        im.mul(im.absolute(sweep),
               im.rational(m.div(m.sub(b, a), m.exact.integer(2))));
    const auto one = im.integer(1), minus = im.integer(-1);
    const OwnedInterval error{im.negate(radius).low, radius.high};
    sine = im.intersect(im.add(sine, error), {minus.low, one.high});
    cosine = im.intersect(im.add(cosine, error), {minus.low, one.high});
    auto factor = im.integer(1);
    for (unsigned k = 0; k < scratch.position[0].size(); ++k) {
      const auto c = k % 4 == 0   ? cosine
                     : k % 4 == 1 ? im.negate(sine)
                     : k % 4 == 2 ? im.negate(cosine)
                                  : sine;
      const auto s = k % 4 == 0   ? sine
                     : k % 4 == 1 ? cosine
                     : k % 4 == 2 ? im.negate(sine)
                                  : im.negate(cosine);
      for (unsigned axis = 0; axis < 2; ++axis) {
        auto value =
            im.mul(factor, im.add(im.mul(im.rational(field(axis + 2)), c),
                                  im.mul(im.rational(field(axis + 4)), s)));
        if (!k)
          value = im.add(value, im.rational(field(axis)));
        scratch.position[axis][k] = value;
      }
      factor = im.divide(im.mul(factor, sweep), k + 1);
    }
  }
  bool speed_jet(const Rational& a, const Rational& b, IntegralJet* output) {
    if (curve.kind == CurveKind::Arc || curve.kind == CurveKind::FullTurn) {
      arc_jets(a, b);
    } else if (curve.kind == CurveKind::Close) {
      for (unsigned axis = 0; axis < 2; ++axis) {
        for (auto& v : scratch.position[axis])
          v = im.integer(0);
        curves.enclose_component(curve, m.exact.integer(0),
                                 scratch.position[axis][1].view(), axis, true);
      }
    } else {
      polynomial_jets(a, b);
    }
    for (unsigned axis = 0; axis < 2; ++axis)
      for (unsigned k = 0; k <= kIntegralOrder; ++k)
        scratch.velocity[axis][k] =
            im.mul(scratch.position[axis][k + 1], im.integer(k + 1));
    for (unsigned k = 0; k <= kIntegralOrder; ++k) {
      auto& square = scratch.squared[k];
      square = im.integer(0);
      for (unsigned axis = 0; axis < 2; ++axis)
        for (unsigned j = 0; j <= k; ++j)
          square = im.add(square, im.mul(scratch.velocity[axis][j],
                                         scratch.velocity[axis][k - j]));
    }
    (*output)[0] = im.sqrt(scratch.squared[0]);
    im.check_state();
    if (!im.positive((*output)[0].low))
      return false;
    const auto den = im.scale((*output)[0], 1);
    for (unsigned k = 1; k <= kIntegralOrder; ++k) {
      auto value = scratch.squared[k];
      for (unsigned j = 1; j < k; ++j)
        value = im.sub(value, im.mul((*output)[j], (*output)[k - j]));
      (*output)[k] = im.div(value, den);
    }
    im.check_state();
    return true;
  }
  OwnedInterval panel(const Rational& a, const Rational& b) {
    const auto width = im.rational(m.sub(b, a));
    if (circle) {
      auto sweep = im.absolute(im.rational(field(7)));
      if (curve.kind == CurveKind::FullTurn) {
        m.d.constant(sweep.view(), true);
        sweep = im.scale(sweep, 1);
      }
      return im.mul(width,
                    im.mul(sweep, im.sqrt(im.rational(circle_squared_radius))));
    }
    if (curve.kind == CurveKind::Bezier || curve.kind == CurveKind::Hermite) {
      const auto control_bound = control_polygon(a, b);
      if (leaf_straight)
        return control_bound;
    }
    const bool smooth = speed_jet(a, b, &scratch.range_speed);
    auto basic = im.mul(width, scratch.range_speed[0]);
    if (!smooth)
      return basic;
    const auto middle = m.div(m.add(a, b), m.exact.integer(2));
    if (!speed_jet(middle, middle, &scratch.point_speed))
      return basic;
    const auto h = im.scale(width, -1);
    auto power = h;
    auto sum = im.integer(0);
    for (unsigned k = 0; k < kIntegralOrder; ++k) {
      if (!(k & 1))
        sum = im.add(
            sum, im.divide(im.scale(im.mul(scratch.point_speed[k], power), 1),
                           k + 1));
      power = im.mul(power, h);
    }
    // Taylor's order-16 coefficient is enclosed over [a,b], not only at m.
    const auto error = im.divide(
        im.scale(
            im.mul(im.absolute(scratch.range_speed[kIntegralOrder]), power), 1),
        kIntegralOrder + 1);
    const OwnedInterval remainder{im.negate(error).low, error.high};
    const auto result = im.intersect(basic, im.add(sum, remainder));
    im.check_state();
    return result;
  }
  OwnedInterval control_polygon(const Rational& a, const Rational& b) {
    const auto h = m.sub(b, a);
    for (unsigned axis = 0; axis < 2; ++axis) {
      auto power = m.exact.integer(1);
      for (unsigned k = 0; k <= degree; ++k) {
        auto value = m.exact.integer(0);
        unsigned choose = 1;
        for (unsigned j = 1; j <= k; ++j)
          choose = choose * (degree + 1 - j) / j;
        for (int j = degree; j >= static_cast<int>(k); --j) {
          value = m.add(m.mul(value, a),
                        m.scale(scratch.coefficient[axis][j], choose));
          if (j > static_cast<int>(k))
            choose = choose * (j - k) / j;
        }
        scratch.polynomial[k] = m.mul(value, power);
        power = m.mul(power, h);
      }
      for (unsigned j = 0; j <= degree; ++j) {
        auto value = m.exact.integer(0);
        unsigned choose_j = 1, choose_d = 1;
        for (unsigned k = 0; k <= j; ++k) {
          value = m.add(value, m.mul(scratch.polynomial[k],
                                     m.exact.fraction(choose_j, choose_d)));
          if (k < j) {
            choose_j = choose_j * (j - k) / (k + 1);
            choose_d = choose_d * (degree - k) / (k + 1);
          }
        }
        scratch.values[axis][j] = value;
      }
    }
    const auto squared = [&](unsigned a_index, unsigned b_index) {
      return m.add(
          m.sq(m.sub(scratch.values[0][a_index], scratch.values[0][b_index])),
          m.sq(m.sub(scratch.values[1][a_index], scratch.values[1][b_index])));
    };
    const auto chord = im.sqrt(im.rational(squared(0, degree)));
    auto perimeter = im.integer(0);
    leaf_straight = true;
    const auto dx = m.sub(scratch.values[0][degree], scratch.values[0][0]);
    const auto dy = m.sub(scratch.values[1][degree], scratch.values[1][0]);
    for (unsigned j = 1; j <= degree; ++j) {
      const auto x = m.sub(scratch.values[0][j], scratch.values[0][j - 1]);
      const auto y = m.sub(scratch.values[1][j], scratch.values[1][j - 1]);
      const auto cross = m.sub(m.mul(dx, y), m.mul(dy, x));
      const auto dot = m.add(m.mul(dx, x), m.mul(dy, y));
      if (!Exact::zero(cross) || (dot.negative && !Exact::zero(dot)) ||
          (Exact::zero(dx) && Exact::zero(dy) &&
           (!Exact::zero(x) || !Exact::zero(y))))
        leaf_straight = false;
      perimeter = im.add(perimeter, im.sqrt(im.rational(squared(j - 1, j))));
    }
    return leaf_straight ? chord : OwnedInterval{chord.low, perimeter.high};
  }
  template <class Accept>
  OwnedInterval integrate_piece(const Rational& a, const Rational& b,
                                const Rational& tolerance, unsigned depth_limit,
                                Accept accept,
                                bool use_control_polygon = false) {
    OwnedInterval total = im.integer(0);
    unsigned pending = 1;
    scratch.panels[0] = {a, b, tolerance, 0};
    while (pending) {
      // Keep the active panel in a different slot while pushing both children.
      auto& active = scratch.panels[kIntegralDepth + 1];
      active = scratch.panels[--pending];
      if (++panels_used > panel_limit)
        throw Status{ErrorCode::ResourceExhausted,
                     "arclength integration panel budget",
                     FailureReason::WorkLimit};
      m.work.poll();
      const auto value = use_control_polygon
                             ? control_polygon(active.a, active.b)
                             : panel(active.a, active.b);
      const auto error = im.sub(value, value);
      const auto allowed = im.rational(active.tolerance);
      im.check_state();
      if (m.d.compare(error.high, allowed.low) <= 0) {
        total = im.add(total, value);
        accept(active.a, active.b, value);
      } else {
        if (active.depth >= depth_limit || active.depth >= kIntegralDepth)
          throw Status{ErrorCode::OperationFailed,
                       "arclength integration depth limit",
                       DomainErrorCode{UINT64_C(0x47454e0001)}};
        const auto middle =
            m.div(m.add(active.a, active.b), m.exact.integer(2));
        const auto half = m.div(active.tolerance, m.exact.integer(2));
        scratch.panels[pending++] = {middle, active.b, half, active.depth + 1};
        scratch.panels[pending++] = {active.a, middle, half, active.depth + 1};
      }
    }
    return total;
  }
  std::array<OwnedInterval, 2> position_bounds(const Curve& selected,
                                               const Rational& a,
                                               const Rational& b) {
    std::array<OwnedInterval, 2> answer;
    if (m.exact.order(a, b) == 0 || selected.kind == CurveKind::Close) {
      for (unsigned axis = 0; axis < 2; ++axis) {
        OwnedInterval first, last;
        curves.enclose_component(selected, a, first.view(), axis, false);
        curves.enclose_component(selected, b, last.view(), axis, false);
        answer[axis] = im.hull(first, last);
      }
      return answer;
    }
    if (selected.kind != CurveKind::Spline) {
      initialize(selected);
      if (selected.kind == CurveKind::Arc ||
          selected.kind == CurveKind::FullTurn)
        arc_jets(a, b);
      else
        polynomial_jets(a, b);
      for (unsigned axis = 0; axis < 2; ++axis)
        answer[axis] = scratch.position[axis][0];
      return answer;
    }
    bool first = true;
    const auto d = reader.index(0, selected.field, {selected.row, 0});
    const auto n = reader.index(0, selected.field, {selected.row, 2});
    const auto kb = reader.index(0, selected.field, {selected.row, 3});
    const auto start =
        m.input(reader, 0, 10, {static_cast<std::uint64_t>(kb + d)});
    const auto end =
        m.input(reader, 0, 10, {static_cast<std::uint64_t>(kb + n)});
    for (auto span = d; span < n; ++span) {
      const auto ka =
          m.input(reader, 0, 10, {static_cast<std::uint64_t>(kb + span)});
      const auto next =
          m.input(reader, 0, 10, {static_cast<std::uint64_t>(kb + span + 1)});
      if (m.exact.order(ka, next) == 0)
        continue;
      const auto ta = m.div(m.sub(ka, start), m.sub(end, start));
      const auto tb = m.div(m.sub(next, start), m.sub(end, start));
      const auto low = m.max(a, ta), high = m.min(b, tb);
      if (m.exact.order(low, high) > 0)
        continue;
      initialize_spline(selected, span);
      polynomial_jets(m.div(m.sub(low, ta), m.sub(tb, ta)),
                      m.div(m.sub(high, ta), m.sub(tb, ta)));
      for (unsigned axis = 0; axis < 2; ++axis)
        answer[axis] = first ? scratch.position[axis][0]
                             : im.hull(answer[axis], scratch.position[axis][0]);
      first = false;
    }
    return answer;
  }
  Rational relative_tolerance(const Rational& a, const Rational& b,
                              unsigned accuracy) {
    const auto rough = panel(a, b);
    im.check_state();
    Rational factor;
    factor.n.words[0] = 1;
    factor.d.words.fill(0);
    factor.d.words[accuracy / 64] = UINT64_C(1) << (accuracy % 64);
    return m.mul(m.max(im.exact_number(rough.high), m.exact.integer(1)),
                 factor);
  }
  OwnedInterval segment(const Curve& selected, const Rational& a,
                        const Rational& b, const Rational& tolerance,
                        unsigned depth_limit = kIntegralDepth,
                        unsigned relative_bits = 0) {
    const auto ignore = [](const Rational&, const Rational&,
                           const OwnedInterval&) {};
    if (selected.kind != CurveKind::Spline) {
      initialize(selected);
      return integrate_piece(
          a, b,
          relative_bits ? relative_tolerance(a, b, relative_bits) : tolerance,
          depth_limit, ignore);
    }
    auto total = im.integer(0);
    const auto degree_value =
        reader.index(0, selected.field, {selected.row, 0});
    const auto n = reader.index(0, selected.field, {selected.row, 2});
    const auto kb = reader.index(0, selected.field, {selected.row, 3});
    const auto start =
        m.input(reader, 0, 10, {static_cast<std::uint64_t>(kb + degree_value)});
    const auto end =
        m.input(reader, 0, 10, {static_cast<std::uint64_t>(kb + n)});
    for (auto span = degree_value; span < n; ++span) {
      const auto ka =
          m.input(reader, 0, 10, {static_cast<std::uint64_t>(kb + span)});
      const auto next =
          m.input(reader, 0, 10, {static_cast<std::uint64_t>(kb + span + 1)});
      if (m.exact.order(ka, next) == 0)
        continue;
      const auto ta = m.div(m.sub(ka, start), m.sub(end, start));
      const auto tb = m.div(m.sub(next, start), m.sub(end, start));
      const auto low = m.max(a, ta), high = m.min(b, tb);
      if (m.exact.order(low, high) >= 0)
        continue;
      initialize_spline(selected, span);
      const auto length = m.sub(tb, ta);
      const auto ua = m.div(m.sub(low, ta), length),
                 ub = m.div(m.sub(high, ta), length);
      const auto eps =
          relative_bits
              ? relative_tolerance(ua, ub, relative_bits)
              : m.mul(tolerance, m.div(m.sub(high, low), m.sub(b, a)));
      total = im.add(total, integrate_piece(ua, ub, eps, depth_limit, ignore));
    }
    return total;
  }
};
}  // namespace ps::plugin_internal::generation
