#include <cmath>
#include <limits>
#include <utility>

#include "03-generation/math.hpp"

namespace ps::plugin_internal::generation {
namespace {
// Signed quadrant primitive for a unit disk. All clipping and branch choices
// use exact rationals; only the analytic sqrt/asin primitive needs refinement.
void disk_primitive(Math& m, Interval output, Rational x, Rational y) {
  auto& e = m.exact;
  auto& d = m.d;
  Directed::Frame frame(d);
  const bool negative = x.negative != y.negative;
  x = m.min(e.absolute(x), e.integer(1));
  y = m.min(e.absolute(y), e.integer(1));
  if (Exact::zero(x) || Exact::zero(y)) {
    d.integer(output, 0);
    return;
  }
  const auto x2 = m.sq(x), y2 = m.sq(y);
  if (e.order(m.add(x2, y2), e.integer(1)) <= 0) {
    m.enclose(output, m.mul(x, y));
  } else {
    auto sx = d.interval(), sy = d.interval(), a = d.interval(),
         b = d.interval(), t = d.interval(), pi = d.interval();
    m.enclose(sx, m.sub(e.integer(1), x2));
    m.sqrt(sx, sx);
    m.enclose(t, x);
    d.multiply(sx, sx, t);
    m.enclose(sy, m.sub(e.integer(1), y2));
    m.sqrt(sy, sy);
    m.enclose(t, y);
    d.multiply(sy, sy, t);
    m.asin(a, x);
    m.asin(b, y);
    d.add(a, a, b);
    d.add(a, a, sx);
    d.add(a, a, sy);
    d.constant(pi, true);
    d.scale(pi, pi, -1);
    d.subtract(a, a, pi);
    d.scale(output, a, -1);
  }
  if (negative) {
    d.negate(output, output);
  }
}
}  // namespace
std::uint64_t ellipse_coverage(Math& m, const std::array<Rational, 2>& pixel,
                               const std::array<Rational, 2>& center,
                               const std::array<Rational, 2>& radii,
                               bool narrow) {
  auto& e = m.exact;
  std::array<Rational, 2> lower, upper, near, far;
  for (unsigned a = 0; a < 2; ++a) {
    lower[a] = m.div(m.sub(pixel[a], center[a]), radii[a]);
    upper[a] = m.div(m.sub(m.add(pixel[a], e.integer(1)), center[a]), radii[a]);
    far[a] = m.max(e.absolute(lower[a]), e.absolute(upper[a]));
    near[a] = e.order(lower[a], e.integer(0)) <= 0 &&
                      e.order(upper[a], e.integer(0)) >= 0
                  ? e.integer(0)
                  : m.min(e.absolute(lower[a]), e.absolute(upper[a]));
  }
  if (e.order(m.add(m.sq(far[0]), m.sq(far[1])), e.integer(1)) <= 0) {
    return convert(1, narrow);
  }
  if (e.order(m.add(m.sq(near[0]), m.sq(near[1])), e.integer(1)) >= 0) {
    return 0;
  }
  const auto scale = m.mul(radii[0], radii[1]);
  return m.certify(narrow, [&](Interval result) {
    auto term = m.d.interval(), area = m.d.interval(), factor = m.d.interval(),
         one = m.d.interval();
    m.d.integer(area, 0);
    for (unsigned corner = 0; corner < 4; ++corner) {
      disk_primitive(m, term, (corner & 1) ? upper[0] : lower[0],
                     (corner & 2) ? upper[1] : lower[1]);
      if (corner == 0 || corner == 3) {
        m.d.add(area, area, term);
      } else {
        m.d.subtract(area, area, term);
      }
    }
    m.enclose(factor, scale);
    m.d.multiply(result, area, factor);
    m.d.integer(one, 1);
    if (result.low.negative) {
      m.d.clear(result.low);
    }
    if (m.d.compare(result.high, one.high) > 0) {
      m.d.copy(result.high, one.high);
    }
  });
}
std::uint64_t ellipse_distance(Math& m, const std::array<Rational, 2>& point,
                               const std::array<Rational, 2>& radii,
                               bool narrow) {
  auto& e = m.exact;
  Rational a = radii[0], b = radii[1], x = e.absolute(point[0]),
           y = e.absolute(point[1]);
  if (e.order(a, b) < 0) {
    std::swap(a, b);
    std::swap(x, y);
  }
  const auto a2 = m.sq(a), b2 = m.sq(b), x2 = m.sq(x), y2 = m.sq(y);
  const auto implicit = m.add(m.div(x2, a2), m.div(y2, b2));
  const int side = e.order(implicit, e.integer(1));
  if (!side) {
    return 0;
  }
  if (e.order(a, b) == 0) {
    return m.affine_sqrt(e.negate(a), e.integer(1), m.add(x2, y2), narrow);
  }
  if (Exact::zero(x)) {
    return m.rounded(m.sub(y, b), narrow);
  }
  const auto signed_root = [&](const Rational& square) {
    const auto value = m.rounded_sqrt(square, narrow);
    return value | (side < 0 ? UINT64_C(1) << (narrow ? 31 : 63) : 0);
  };
  if (Exact::zero(y)) {
    const auto delta = m.sub(a2, b2);
    if (e.order(m.mul(a, x), delta) >= 0) {
      return m.rounded(m.sub(x, a), narrow);
    }
    return signed_root(m.mul(b2, m.sub(e.integer(1), m.div(x2, delta))));
  }
  const auto ax = m.mul(a, x), by = m.mul(b, y);
  auto lower = side < 0 ? m.sub(by, b2) : e.integer(0);
  auto upper = side < 0 ? e.integer(0) : m.add(ax, by);
  const auto stationary = [&](const Rational& lambda) {
    return m.sub(m.add(m.sq(m.div(ax, m.add(a2, lambda))),
                       m.sq(m.div(by, m.add(b2, lambda)))),
                 e.integer(1));
  };
  const auto squared_distance = [&](const Rational& lambda) {
    const auto dx = m.div(m.mul(lambda, x), m.add(a2, lambda));
    const auto dy = m.div(m.mul(lambda, y), m.add(b2, lambda));
    return m.add(m.sq(dx), m.sq(dy));
  };
  // A floating solve proposes a short bracket only. Both ends are admitted
  // by exact predicates before replacing the mathematically proved bracket.
  // Nonfinite/ill-conditioned proposals simply retain the original bracket.
  const auto approximate = [&](const Rational& value) {
    return number(m.rounded(value, false));
  };
  const double aa = approximate(a2), bb = approximate(b2), xx = approximate(ax),
               yy = approximate(by);
  double lo = approximate(lower), hi = approximate(upper);
  if (std::isfinite(aa) && std::isfinite(bb) && std::isfinite(xx) &&
      std::isfinite(yy) && std::isfinite(lo) && std::isfinite(hi) && aa > 0 &&
      bb > 0) {
    for (unsigned i = 0; i < 64; ++i) {
      if (!(i & 7)) {
        m.work.poll(8);
      }
      const double candidate = lo + (hi - lo) * .5;
      if (candidate == lo || candidate == hi) {
        break;
      }
      const double value =
          std::hypot(xx / (aa + candidate), yy / (bb + candidate));
      if (std::isnan(value)) {
        break;
      }
      if (value > 1) {
        lo = candidate;
      } else {
        hi = candidate;
      }
    }
    const double infinity = std::numeric_limits<double>::infinity();
    for (unsigned i = 0; i < 8; ++i) {
      lo = std::nextafter(lo, -infinity);
      hi = std::nextafter(hi, infinity);
    }
    for (const double candidate : {lo, hi}) {
      if (!std::isfinite(candidate)) {
        continue;
      }
      const auto value = m.value(candidate);
      if (e.order(value, lower) <= 0 || e.order(value, upper) >= 0) {
        continue;
      }
      const int order = e.order(stationary(value), e.integer(0));
      check(e.status());
      if (!order) {
        return signed_root(squared_distance(value));
      }
      if (order > 0) {
        lower = value;
      } else {
        upper = value;
      }
    }
  }
  // In the first quadrant, the unique root above -b^2 is the global
  // minimizer. Positive y excludes the singular minor-axis endpoint.
  for (std::uint64_t iteration = 0; iteration < m.max_refinements;
       ++iteration) {
    m.work.poll();
    const auto middle = m.div(m.add(lower, upper), e.integer(2));
    const int order = e.order(stationary(middle), e.integer(0));
    check(e.status());
    if (!order) {
      return signed_root(squared_distance(middle));
    }
    if (order > 0) {
      lower = middle;
    } else {
      upper = middle;
    }
    if (e.order(lower, e.negate(b2)) == 0) {
      continue;
    }
    const auto first = signed_root(squared_distance(lower));
    const auto last = signed_root(squared_distance(upper));
    if (first == last) {
      return first;
    }
  }
  throw Status{ErrorCode::OperationFailed,
               "ellipse distance refinement did not converge",
               DomainErrorCode{UINT64_C(0x47454e0001)}};
}
}  // namespace ps::plugin_internal::generation
