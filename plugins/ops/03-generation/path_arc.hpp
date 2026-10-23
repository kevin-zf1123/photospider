#pragma once

#include <array>
#include <cstdint>

#include "03-generation/curve_integral.hpp"

namespace ps::plugin_internal::generation {
struct ArcLocation final {
  std::int64_t segment = -1;
  Rational low, high;
  bool exact = true;
  bool position_certified = false;
  std::array<std::uint64_t, 2> position{};
};
class ArcTraversal final {
 public:
  CurveIntegral& integral;
  Math& m;
  IntervalMath& im;
  PathCursor path;
  OwnedInterval* lengths;
  OwnedInterval* prefixes;
  OwnedInterval* totals;
  unsigned accuracy;
  ArcTraversal(CurveIntegral& ci, const PathInfo& info,
               OwnedInterval* length_values, OwnedInterval* prefix_values,
               OwnedInterval* total_values)
      : integral(ci),
        m(ci.m),
        im(ci.im),
        path(ci.reader, info, ci.m),
        lengths(length_values),
        prefixes(prefix_values),
        totals(total_values),
        accuracy(m.d.precision / 2) {}
  bool zero(const OwnedInterval& value) {
    return !im.positive(value.high) && !value.high.negative;
  }
  bool point(const OwnedInterval& value) {
    return m.d.compare(value.low, value.high) == 0;
  }
  int compare(const OwnedInterval& a, const OwnedInterval& b) {
    if (m.d.compare(a.high, b.low) < 0)
      return -1;
    if (m.d.compare(a.low, b.high) > 0)
      return 1;
    if (point(a) && point(b) && m.d.compare(a.low, b.low) == 0)
      return 0;
    return 2;
  }
  // Exact analytic lengths are sqrt(square), optionally multiplied by pi.
  // This representation keeps common irrational factors through normalization.
  bool analytic_length(std::uint64_t row, Rational* square, bool* pi,
                       bool* constant_speed = nullptr) {
    m.work.poll();
    *pi = false;
    if (constant_speed) {
      *constant_speed = false;
    }
    const auto curve = path.curve(row);
    if ((curve.kind == CurveKind::Bezier && curve.degree <= 1) ||
        curve.kind == CurveKind::Close) {
      auto& value = integral.scratch.rational_value;
      if (!integral.curves.rational(curve, m.exact.integer(0), true, &value)) {
        return false;
      }
      *square = m.add(m.sq(value.derivative[0]), m.sq(value.derivative[1]));
      if (constant_speed) {
        *constant_speed = true;
      }
      return true;
    }
    integral.initialize(curve);
    if (integral.circle) {
      *pi = curve.kind == CurveKind::FullTurn;
      const auto sweep = *pi ? m.exact.integer(2) : integral.field(7);
      *square = m.mul(integral.circle_squared_radius, m.sq(sweep));
      if (constant_speed) {
        *constant_speed = true;
      }
      return true;
    }
    if (curve.kind == CurveKind::Bezier || curve.kind == CurveKind::Hermite) {
      integral.control_polygon(m.exact.integer(0), m.exact.integer(1));
      if (integral.leaf_straight) {
        const auto& values = integral.scratch.values;
        *square = m.add(m.sq(m.sub(values[0][integral.degree], values[0][0])),
                        m.sq(m.sub(values[1][integral.degree], values[1][0])));
        return true;
      }
    }
    return false;
  }
  // Decide whether (normalized * total + absolute - prefix(stop)) / divisor
  // is rational, and return that exact ratio. Distinct rational square classes
  // are linearly independent; pi and non-pi classes cannot cancel each other.
  // No factorization or unbounded allocation is needed. The common-class path
  // is linear; mixed-class cancellation uses bounded, work-charged scans.
  bool exact_ratio(std::uint64_t s, std::uint64_t stop,
                   const Rational* normalized, const Rational* absolute,
                   const Rational& divisor_square, bool divisor_pi,
                   Rational* result) {
    if (!normalized && !absolute) {
      return false;
    }
    const auto first = path.first(s), end = path.end(s);
    const auto term = [&](std::uint64_t row, Rational* coefficient,
                          Rational* square, bool* pi) {
      if (row == end) {
        *coefficient = absolute ? *absolute : m.exact.integer(0);
        *square = m.exact.integer(1);
        *pi = false;
        return true;
      }
      *coefficient = normalized ? *normalized : m.exact.integer(0);
      if (row < stop) {
        *coefficient = m.sub(*coefficient, m.exact.integer(1));
      }
      if (Exact::zero(*coefficient) || zero(lengths[row])) {
        *coefficient = m.exact.integer(0);
        return true;
      }
      return analytic_length(row, square, pi);
    };
    auto total = m.exact.integer(0);
    for (auto row = first; row <= end; ++row) {
      Rational coefficient, square, scale;
      bool pi = false;
      if (!term(row, &coefficient, &square, &pi)) {
        return false;
      }
      if (Exact::zero(coefficient) || Exact::zero(square)) {
        continue;
      }
      if (pi == divisor_pi &&
          m.rational_sqrt(m.div(square, divisor_square), &scale)) {
        total = m.add(total, m.mul(coefficient, scale));
        continue;
      }
      auto group = m.exact.integer(0);
      for (auto other = first; other <= end; ++other) {
        Rational weight, q;
        bool other_pi = false;
        if (!term(other, &weight, &q, &other_pi)) {
          return false;
        }
        if (!Exact::zero(weight) && !Exact::zero(q) && pi == other_pi &&
            m.rational_sqrt(m.div(q, square), &scale)) {
          group = m.add(group, m.mul(weight, scale));
        }
      }
      check(m.exact.status());
      if (!Exact::zero(group)) {
        return false;
      }
    }
    check(m.exact.status());
    *result = total;
    return true;
  }
  int compare_target(std::uint64_t s, std::uint64_t stop,
                     const OwnedInterval& target, const OwnedInterval& bound,
                     const Rational* normalized = nullptr,
                     const Rational* absolute = nullptr) {
    const auto order = compare(target, bound);
    if (order != 2) {
      return order;
    }
    Rational difference;
    if (exact_ratio(s, stop, normalized, absolute, m.exact.integer(1), false,
                    &difference)) {
      const auto exact_order = m.exact.order(difference, m.exact.integer(0));
      check(m.exact.status());
      return exact_order;
    }
    return 2;
  }
  void index() {
    accuracy = m.d.precision / 2;
    integral.panels_used = 0;
    for (std::uint64_t s = 0; s < path.subpaths(); ++s) {
      auto subtotal = im.integer(0);
      for (auto row = path.first(s); row < path.end(s); ++row) {
        prefixes[row] = subtotal;
        lengths[row] = integral.segment(path.curve(row), m.exact.integer(0),
                                        m.exact.integer(1), m.exact.integer(0),
                                        kIntegralDepth, accuracy);
        subtotal = im.add(subtotal, lengths[row]);
      }
      totals[s] = subtotal;
    }
    im.check_state();
  }
  OwnedInterval partial(std::uint64_t row, const Rational& t) {
    if (Exact::zero(t))
      return im.integer(0);
    if (m.exact.order(t, m.exact.integer(1)) == 0)
      return lengths[row];
    return integral.segment(path.curve(row), m.exact.integer(0), t,
                            m.exact.integer(0), kIntegralDepth, accuracy);
  }
  ArcLocation endpoint(std::int64_t row, unsigned end) {
    ArcLocation location;
    location.segment = row;
    location.low = location.high = m.exact.integer(end);
    return location;
  }
  bool rational_inverse(const Curve& curve, unsigned axis,
                        const Rational& target,
                        const Rational& denominator_bound,
                        ArcLocation* location) {
    auto value =
        m.div(m.add(location->low, location->high), m.exact.integer(2));
    auto previous_n = m.exact.integer(1), before_n = m.exact.integer(0);
    auto previous_d = m.exact.integer(0), before_d = m.exact.integer(1);
    for (std::uint64_t step = 0; step < m.max_refinements; ++step) {
      m.work.poll();
      const auto remainder = m.exact.modulo(value, 1);
      const auto whole = m.sub(value, remainder);
      const auto numerator = m.add(m.mul(whole, previous_n), before_n);
      const auto denominator = m.add(m.mul(whole, previous_d), before_d);
      const auto beyond = m.exact.order(denominator, denominator_bound);
      check(m.exact.status());
      if (beyond > 0) {
        return false;
      }
      const auto candidate = m.div(numerator, denominator);
      if (m.exact.order(candidate, location->low) >= 0 &&
          m.exact.order(candidate, location->high) <= 0) {
        auto& result = integral.scratch.rational_value;
        if (!integral.curves.rational(curve, candidate, false, &result)) {
          throw IntegrationRetry{};
        }
        const auto equal = m.exact.order(result.position[axis], target);
        check(m.exact.status());
        if (!equal) {
          location->low = location->high = candidate;
          location->exact = true;
          return true;
        }
      }
      check(m.exact.status());
      if (Exact::zero(remainder)) {
        return false;
      }
      before_n = previous_n;
      previous_n = numerator;
      before_d = previous_d;
      previous_d = denominator;
      value = m.div(m.exact.integer(1), remainder);
    }
    throw Status{ErrorCode::OperationFailed,
                 "straight rational inverse reconstruction limit",
                 DomainErrorCode{UINT64_C(0x47454e0001)}};
  }
  ArcLocation inverse_straight(std::uint64_t row, const Rational& progress) {
    auto location = endpoint(row, 0);
    location.high = m.exact.integer(1);
    location.exact = false;
    location.position_certified = true;
    const auto curve = path.curve(row);
    auto& value = integral.scratch.rational_value;
    if (!integral.curves.rational(curve, m.exact.integer(0), false, &value)) {
      throw IntegrationRetry{};
    }
    const auto start = value.position;
    if (!integral.curves.rational(curve, m.exact.integer(1), false, &value)) {
      throw IntegrationRetry{};
    }
    const auto finish = value.position;
    const unsigned axis = m.exact.order(start[0], finish[0]) == 0 ? 1 : 0;
    const auto direction = m.sub(finish[axis], start[axis]);
    const auto target = m.lerp(start[axis], finish[axis], progress);
    for (unsigned j = 0; j < 2; ++j) {
      location.position[j] =
          m.rounded(m.lerp(start[j], finish[j], progress), false);
    }
    integral.initialize(curve);
    auto degree = integral.degree;
    while (degree && Exact::zero(integral.scratch.coefficient[axis][degree])) {
      --degree;
    }
    auto denominator_bound = m.exact.integer(1);
    denominator_bound.n = integral.scratch.coefficient[axis][degree].n;
    for (unsigned k = 0; k < degree; ++k) {
      const auto coefficient =
          k ? integral.scratch.coefficient[axis][k]
            : m.sub(integral.scratch.coefficient[axis][0], target);
      auto denominator = m.exact.integer(1);
      denominator.n = coefficient.d;
      denominator_bound = m.mul(denominator_bound, denominator);
    }
    check(m.exact.status());
    m.work.poll(denominator_bound.n.words.size());
    const auto rational_steps =
        2U *
        static_cast<unsigned>(Exact::Workspace::top(denominator_bound.n) + 1);
    bool probed = false;
    // Clearing coefficient denominators bounds every rational root's reduced
    // denominator by denominator_bound. For width < 1 / bound^2, Legendre's
    // theorem puts every such root among the midpoint's continued fractions.
    // Test candidates by exact substitution; only then retain an exact map.
    // The control-polygon proof established monotone collinearity. Solve its
    // polynomial coordinate exactly; the geometric position is independent of
    // rounding the inverse parameter, including exact zero and IEEE midpoints.
    for (std::uint64_t attempt = 0; attempt < m.max_refinements; ++attempt) {
      m.work.poll();
      if (m.rounded(location.low, false) == m.rounded(location.high, false)) {
        // After attempt bisections width is exactly 2^-attempt. The bit bound
        // proves width * denominator_bound^2 < 1 without squaring a wide
        // integer.
        const bool rational_limit = attempt >= rational_steps;
        if (!probed || attempt % 16 == 0 || rational_limit) {
          if (rational_inverse(curve, axis, target, denominator_bound,
                               &location) ||
              rational_limit) {
            return location;
          }
          probed = true;
        }
      }
      const auto middle =
          m.div(m.add(location.low, location.high), m.exact.integer(2));
      if (!integral.curves.rational(curve, middle, false, &value)) {
        throw IntegrationRetry{};
      }
      int order = m.exact.order(value.position[axis], target);
      check(m.exact.status());
      if (!order) {
        location.low = location.high = middle;
        location.exact = true;
        return location;
      }
      if (direction.negative) {
        order = -order;
      }
      if (order < 0) {
        location.low = middle;
      } else {
        location.high = middle;
      }
    }
    throw Status{ErrorCode::OperationFailed,
                 "straight arclength inverse refinement limit",
                 DomainErrorCode{UINT64_C(0x47454e0001)}};
  }
  ArcLocation inverse(std::uint64_t s, std::uint64_t row,
                      const OwnedInterval& target,
                      const Rational* normalized_single = nullptr,
                      const Rational* normalized = nullptr,
                      const Rational* absolute = nullptr) {
    auto location = endpoint(row, 0);
    const auto curve = path.curve(row);
    integral.initialize(curve);
    bool constant_speed =
        (curve.kind == CurveKind::Bezier && curve.degree <= 1) ||
        curve.kind == CurveKind::Close || integral.circle;
    if (curve.kind == CurveKind::Bezier || curve.kind == CurveKind::Hermite) {
      bool linear = true;
      for (unsigned k = 2; k <= integral.degree; ++k) {
        for (unsigned axis = 0; axis < 2; ++axis) {
          linear = linear && Exact::zero(integral.scratch.coefficient[axis][k]);
        }
      }
      constant_speed = constant_speed || linear;
    }
    if (constant_speed && normalized_single &&
        (!absolute || Exact::zero(*absolute))) {
      location.low = location.high = *normalized_single;
      return location;
    }
    if (normalized || absolute) {
      Rational square, parameter;
      bool pi = false;
      if (analytic_length(row, &square, &pi) && !Exact::zero(square) &&
          exact_ratio(s, row, normalized, absolute, square, pi, &parameter)) {
        if (constant_speed) {
          location.low = location.high = parameter;
          return location;
        }
        if (curve.kind == CurveKind::Bezier ||
            curve.kind == CurveKind::Hermite) {
          return inverse_straight(row, parameter);
        }
      }
    }
    if (constant_speed && point(lengths[row]) && point(target)) {
      location.low = location.high =
          m.div(im.exact_number(target.low), im.exact_number(lengths[row].low));
      return location;
    }
    location.high = m.exact.integer(1);
    location.exact = false;
    Rational required;
    required.n.words[0] = 1;
    required.d.words.fill(0);
    required.d.words[(accuracy / 2) / 64] = UINT64_C(1)
                                            << ((accuracy / 2) % 64);
    for (std::uint64_t attempt = 0; attempt < m.max_refinements; ++attempt) {
      m.work.poll();
      std::uint64_t rounded = 0, x = 0, y = 0;
      if (m.exact.order(m.sub(location.high, location.low), required) <= 0 &&
          im.rounded(im.range(location.low, location.high), false, &rounded)) {
        const auto position =
            integral.position_bounds(curve, location.low, location.high);
        if (im.rounded(position[0], false, &x) &&
            im.rounded(position[1], false, &y))
          return location;
      }
      const auto middle =
          m.div(m.add(location.low, location.high), m.exact.integer(2));
      const auto value = partial(row, middle);
      const int order = compare(value, target);
      if (order == 0) {
        location.low = location.high = middle;
        location.exact = true;
        return location;
      }
      if (order == -1) {
        location.low = middle;
        continue;
      }
      if (order == 1) {
        location.high = middle;
        continue;
      }
      // An exact midpoint inverse need not be decided to contract its bracket.
      // Quarter probes give a strict containing bracket, including at cusps.
      const auto left = m.div(m.add(location.low, middle), m.exact.integer(2));
      const auto right =
          m.div(m.add(middle, location.high), m.exact.integer(2));
      const auto before = partial(row, left), after = partial(row, right);
      bool progress = false;
      if (m.d.compare(before.high, target.low) <= 0) {
        location.low = left;
        progress = true;
      }
      if (m.d.compare(after.low, target.high) >= 0) {
        location.high = right;
        progress = true;
      }
      if (!progress)
        throw IntegrationRetry{};
    }
    throw Status{ErrorCode::OperationFailed,
                 "true arclength inverse refinement limit",
                 DomainErrorCode{UINT64_C(0x47454e0001)}};
  }
  ArcLocation locate(std::uint64_t s, const OwnedInterval& target,
                     bool at_start = false, bool at_end = false,
                     const Rational* normalized = nullptr,
                     const Rational* absolute = nullptr) {
    Rational exact_target;
    if (!normalized && !absolute && point(target)) {
      exact_target = im.exact_number(target.low);
      absolute = &exact_target;
    }
    const auto first = path.first(s), end = path.end(s);
    if (first == end)
      return endpoint(-1, 0);
    if (zero(totals[s]))
      return endpoint(first, 0);
    if (at_end)
      return endpoint(end - 1, 1);
    std::uint64_t positives = 0, only = first;
    for (auto row = first; row < end; ++row) {
      if (zero(lengths[row]))
        continue;
      if (!im.positive(lengths[row].low))
        throw IntegrationRetry{};
      ++positives;
      only = row;
    }
    if (at_start) {
      for (auto row = first; row < end; ++row)
        if (!zero(lengths[row]))
          return endpoint(row, 0);
    }
    if (positives == 1 && normalized)
      return inverse(s, only, target, normalized, normalized, absolute);
    for (auto row = first; row < end; ++row) {
      if (zero(lengths[row]))
        continue;
      const auto boundary = im.add(prefixes[row], lengths[row]);
      const int order =
          compare_target(s, row + 1, target, boundary, normalized, absolute);
      if (order == 0) {
        // Exact junction ownership must not be lost in target-prefix intervals.
        for (auto next = row + 1; next < end; ++next) {
          m.work.poll();
          if (!zero(lengths[next])) {
            return endpoint(next, 0);
          }
        }
        return endpoint(end - 1, 1);
      }
      if (order == 1)
        continue;
      if (order == 2)
        throw IntegrationRetry{};
      const auto local = im.sub(target, prefixes[row]);
      if (zero(local))
        return endpoint(row, 0);
      return inverse(s, row, local, nullptr, normalized, absolute);
    }
    throw IntegrationRetry{};
  }
  std::array<std::uint64_t, 3> rounded_location(std::uint64_t s,
                                                const ArcLocation& location) {
    if (location.segment < 0)
      return {path.isolated(s, 0), path.isolated(s, 1), 0};
    if (location.position_certified) {
      return {location.position[0], location.position[1],
              m.rounded(location.low, false)};
    }
    const auto curve = path.curve(location.segment);
    std::array<std::uint64_t, 3> answer{};
    if (location.exact) {
      auto& value = integral.scratch.rational_value;
      if (integral.curves.rational(curve, location.low, false, &value)) {
        for (unsigned axis = 0; axis < 2; ++axis)
          answer[axis] =
              m.rounded(value.position[axis], false, value.copy_position[axis]);
      } else {
        for (unsigned axis = 0; axis < 2; ++axis) {
          OwnedInterval value_bound;
          integral.curves.enclose_component(curve, location.low,
                                            value_bound.view(), axis, false);
          if (!im.rounded(value_bound, false, &answer[axis]))
            throw IntegrationRetry{};
        }
      }
      answer[2] = m.rounded(location.low, false);
    } else {
      const auto position =
          integral.position_bounds(curve, location.low, location.high);
      for (unsigned axis = 0; axis < 2; ++axis)
        if (!im.rounded(position[axis], false, &answer[axis]))
          throw IntegrationRetry{};
      if (!im.rounded(im.range(location.low, location.high), false, &answer[2]))
        throw IntegrationRetry{};
    }
    return answer;
  }
};
}  // namespace ps::plugin_internal::generation
