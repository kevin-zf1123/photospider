#include "03-generation/path_math.hpp"

#include <algorithm>
#include <utility>

namespace ps::plugin_internal::generation {
Curve core_curve(const Reader& reader, Math& m, std::uint64_t row) {
  Curve curve;
  curve.core = true;
  curve.pooled = true;
  curve.field = 2;
  curve.row = row;
  const auto verb = reader.bits(0, 0, {row});
  const auto begin = static_cast<std::uint64_t>(reader.index(0, 1, {row}));
  curve.control[0] = begin - 1;
  if (verb != 5) {
    curve.degree = static_cast<unsigned>(verb - 1);
    for (unsigned j = 0; j < curve.degree; ++j) {
      curve.control[j + 1] = begin + j;
    }
    return curve;
  }
  curve.degree = 1;
  const auto count = static_cast<std::uint64_t>(reader.index(0, 5, {2}));
  std::uint64_t low = 0, high = count;
  while (low + 1 < high) {
    m.work.poll();
    const auto middle = low + (high - low) / 2;
    if (static_cast<std::uint64_t>(reader.index(0, 3, {middle})) <= row) {
      low = middle;
    } else {
      high = middle;
    }
  }
  const auto move = static_cast<std::uint64_t>(reader.index(0, 3, {low}));
  curve.control[1] = static_cast<std::uint64_t>(reader.index(0, 1, {move}));
  return curve;
}
Curve primitive_curve(const Reader& reader, std::uint64_t row) {
  Curve curve;
  curve.pooled = true;
  const auto kind = reader.index(0, 0, {row, 0});
  curve.row = static_cast<std::uint64_t>(reader.index(0, 0, {row, 1}));
  if (kind == 1) {
    curve.kind = CurveKind::Bezier;
    curve.field = 5;
    curve.degree = static_cast<unsigned>(reader.index(0, 4, {curve.row}));
  } else if (kind == 2 || kind == 3) {
    curve.kind = kind == 2 ? CurveKind::Arc : CurveKind::FullTurn;
    curve.field = 6;
  } else if (kind == 4) {
    curve.kind = CurveKind::Hermite;
    curve.field = 7;
  } else if (kind == 5) {
    curve.kind = CurveKind::Spline;
    curve.field = 8;
  } else {
    curve.kind = CurveKind::Close;
    curve.control[0] = row;
  }
  return curve;
}
Rational CurveMath::control(const Curve& curve, unsigned point,
                            unsigned axis) const {
  return curve.core
             ? m.input(reader, 0, curve.field, {curve.control[point], axis})
             : m.input(reader, 0, curve.field, {curve.row, point, axis});
}
bool CurveMath::compact_polynomial(const Curve& curve, const Rational& input,
                                   bool derivative, CurveValue* output) {
  using Short = model_ops::RationalMathStorage<32>;
  using Value = Short::Rational;
  // Input classification selects arithmetic capacity only. It is not payload
  // validation. Failed compact arithmetic retains its charged work on fallback.
  for (unsigned i = Short::kWords; i < Exact::kWords; ++i) {
    if (input.n.words[i] || input.d.words[i]) {
      return false;
    }
  }
  auto& e = scratch.compact;
  e.reset();
  Value t;
  std::copy_n(input.n.words.begin(), Short::kWords, t.n.words.begin());
  std::copy_n(input.d.words.begin(), Short::kWords, t.d.words.begin());
  t.negative = input.negative;
  const auto scale = [&](const Value& a, std::int64_t b) {
    return e.multiply(a, e.integer(b));
  };
  const auto lerp = [&](const Value& a, const Value& b) {
    return e.add(a, e.multiply(t, e.subtract(b, a)));
  };
  const auto point = [&](unsigned j, unsigned axis) {
    const Coordinate at = curve.core ? Coordinate{curve.control[j], axis}
                                     : Coordinate{curve.row, j, axis};
    return e.binary(
        reader.bits(0, curve.field, at),
        reader.kernel.groups[reader.kernel.input_base[0] + curve.field].type ==
            ElementType::Float32);
  };
  std::array<Value, 2> result;
  std::array<bool, 2> copy{};
  const int endpoint = Short::zero(t)                  ? 0
                       : e.order(t, e.integer(1)) == 0 ? 1
                                                       : -1;
  if (curve.kind == CurveKind::Bezier) {
    for (unsigned axis = 0; axis < 2; ++axis) {
      std::array<Value, 4> p;
      bool negative_zero = true;
      for (unsigned j = 0; j <= curve.degree; ++j) {
        p[j] = point(j, axis);
        negative_zero &= Short::zero(p[j]) && p[j].negative;
      }
      if (derivative) {
        for (unsigned j = 0; j < curve.degree; ++j) {
          p[j] = scale(e.subtract(p[j + 1], p[j]), curve.degree);
        }
        for (unsigned level = curve.degree; level > 1; --level) {
          for (unsigned j = 0; j + 1 < level; ++j) {
            p[j] = lerp(p[j], p[j + 1]);
          }
        }
        result[axis] = p[0];
      } else if (endpoint >= 0) {
        result[axis] = p[endpoint ? curve.degree : 0];
        copy[axis] = true;
      } else {
        for (unsigned level = curve.degree; level; --level) {
          for (unsigned j = 0; j < level; ++j) {
            p[j] = lerp(p[j], p[j + 1]);
          }
        }
        result[axis] = p[0];
        if (negative_zero) {
          result[axis].negative = true;
          copy[axis] = true;
        }
      }
    }
  } else if (endpoint >= 0) {
    for (unsigned axis = 0; axis < 2; ++axis) {
      result[axis] = point(endpoint + (derivative ? 2 : 0), axis);
      copy[axis] = true;
    }
  } else {
    const auto t2 = e.multiply(t, t);
    std::array<Value, 4> coefficients;
    if (derivative) {
      coefficients = {
          e.subtract(scale(t2, 6), scale(t, 6)),
          e.add(scale(t2, -6), scale(t, 6)),
          e.add(e.subtract(scale(t2, 3), scale(t, 4)), e.integer(1)),
          e.subtract(scale(t2, 3), scale(t, 2))};
    } else {
      const auto t3 = e.multiply(t2, t);
      coefficients = {
          e.add(e.subtract(scale(t3, 2), scale(t2, 3)), e.integer(1)),
          e.add(scale(t3, -2), scale(t2, 3)),
          e.add(e.subtract(t3, scale(t2, 2)), t), e.subtract(t3, t2)};
    }
    for (unsigned axis = 0; axis < 2; ++axis) {
      result[axis] = e.integer(0);
      for (unsigned j = 0; j < 4; ++j) {
        result[axis] =
            e.add(result[axis], e.multiply(coefficients[j], point(j, axis)));
      }
    }
  }
  const auto status = e.status();
  if (status.code == ErrorCode::ResourceExhausted &&
      status.reason == FailureReason::CapacityLimit) {
    return false;
  }
  check(status);
  for (unsigned axis = 0; axis < 2; ++axis) {
    auto& target =
        derivative ? output->derivative[axis] : output->position[axis];
    target = m.exact.integer(0);
    std::copy(result[axis].n.words.begin(), result[axis].n.words.end(),
              target.n.words.begin());
    std::copy(result[axis].d.words.begin(), result[axis].d.words.end(),
              target.d.words.begin());
    target.negative = result[axis].negative;
    (derivative ? output->copy_derivative : output->copy_position)[axis] =
        copy[axis];
  }
  return true;
}
void CurveMath::bezier(const Curve& curve, const Rational& t, bool derivative,
                       CurveValue* output) {
  auto& e = m.exact;
  for (unsigned axis = 0; axis < 2; ++axis) {
    std::array<Rational, 4> p;
    bool negative_zero = true;
    for (unsigned j = 0; j <= curve.degree; ++j) {
      p[j] = control(curve, j, axis);
      negative_zero &= Exact::zero(p[j]) && p[j].negative;
    }
    if (derivative) {
      std::array<Rational, 3> differences;
      for (unsigned j = 0; j < curve.degree; ++j) {
        differences[j] = m.scale(m.sub(p[j + 1], p[j]), curve.degree);
      }
      for (unsigned level = curve.degree; level > 1; --level) {
        for (unsigned j = 0; j + 1 < level; ++j) {
          differences[j] = m.lerp(differences[j], differences[j + 1], t);
        }
      }
      output->derivative[axis] = differences[0];
    }
    if (Exact::zero(t)) {
      output->position[axis] = p[0];
      output->copy_position[axis] = true;
    } else if (e.order(t, e.integer(1)) == 0) {
      output->position[axis] = p[curve.degree];
      output->copy_position[axis] = true;
    } else {
      for (unsigned level = curve.degree; level; --level) {
        for (unsigned j = 0; j < level; ++j) {
          p[j] = m.lerp(p[j], p[j + 1], t);
        }
      }
      output->position[axis] = p[0];
      if (negative_zero) {
        output->position[axis].negative = true;
        output->copy_position[axis] = true;
      }
    }
  }
}
void CurveMath::hermite(const Curve& curve, const Rational& t, bool derivative,
                        CurveValue* output) {
  auto& e = m.exact;
  const auto t2 = m.sq(t), t3 = m.mul(t2, t);
  const std::array<Rational, 4> h{
      m.add(m.sub(m.scale(t3, 2), m.scale(t2, 3)), e.integer(1)),
      m.add(m.scale(t3, -2), m.scale(t2, 3)),
      m.add(m.sub(t3, m.scale(t2, 2)), t), m.sub(t3, t2)};
  std::array<Rational, 4> dh;
  if (derivative) {
    dh = {m.sub(m.scale(t2, 6), m.scale(t, 6)),
          m.add(m.scale(t2, -6), m.scale(t, 6)),
          m.add(m.sub(m.scale(t2, 3), m.scale(t, 4)), e.integer(1)),
          m.sub(m.scale(t2, 3), m.scale(t, 2))};
  }
  const int endpoint = Exact::zero(t)                  ? 0
                       : e.order(t, e.integer(1)) == 0 ? 1
                                                       : -1;
  for (unsigned axis = 0; axis < 2; ++axis) {
    if (endpoint >= 0) {
      output->position[axis] = control(curve, endpoint, axis);
      output->copy_position[axis] = true;
      if (derivative) {
        output->derivative[axis] = control(curve, endpoint + 2, axis);
        output->copy_derivative[axis] = true;
      }
      continue;
    }
    auto position = e.integer(0), tangent = e.integer(0);
    for (unsigned j = 0; j < 4; ++j) {
      const auto p = control(curve, j, axis);
      position = m.add(position, m.mul(h[j], p));
      if (derivative) {
        tangent = m.add(tangent, m.mul(dh[j], p));
      }
    }
    output->position[axis] = position;
    output->derivative[axis] = tangent;
  }
}
Rational CurveMath::knot(const Curve& curve, std::uint64_t row) const {
  return m.input(reader, curve.pooled ? 0 : 2, curve.pooled ? 10 : 0, {row});
}
void CurveMath::basis(const Curve& curve, std::uint64_t begin,
                      std::uint64_t span, unsigned degree, const Rational& tau,
                      std::array<Rational, 17>* output) {
  auto& e = m.exact;
  (*output)[0] = e.integer(1);
  for (unsigned j = 1; j <= degree; ++j) {
    m.work.poll();
    scratch.left[j] = m.sub(tau, knot(curve, begin + span + 1 - j));
    scratch.right[j] = m.sub(knot(curve, begin + span + j), tau);
    auto saved = e.integer(0);
    for (unsigned r = 0; r < j; ++r) {
      const auto denominator = m.add(scratch.right[r + 1], scratch.left[j - r]);
      const auto temp = Exact::zero(denominator)
                            ? e.integer(0)
                            : m.div((*output)[r], denominator);
      (*output)[r] = m.add(saved, m.mul(scratch.right[r + 1], temp));
      saved = m.mul(scratch.left[j - r], temp);
    }
    (*output)[j] = saved;
  }
}
void CurveMath::spline(const Curve& curve, const Rational& t, bool derivative,
                       CurveValue* output) {
  auto& e = m.exact;
  const auto record = [&](unsigned column) {
    return static_cast<std::uint64_t>(
        reader.index(0, curve.field, {curve.row, column}));
  };
  const unsigned degree = static_cast<unsigned>(record(0));
  const auto cb = record(1), n = record(2), kb = record(3);
  // Primitive column 4 is knot_count; only standalone records carry a flag.
  const bool weighted = curve.pooled || record(4) != 0;
  const auto start = knot(curve, kb + degree), end = knot(curve, kb + n),
             length = m.sub(end, start);
  const auto tau = m.lerp(start, end, t);
  const bool final = e.order(t, e.integer(1)) == 0;
  std::uint64_t low = degree, high = n;
  while (low < high) {
    m.work.poll();
    const auto middle = low + (high - low) / 2;
    const int order = e.order(knot(curve, kb + middle), tau);
    if (order < 0 || (!final && order == 0)) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  const auto span = low - 1;
  basis(curve, kb, span, degree, tau, &scratch.basis);
  if (derivative && degree) {
    basis(curve, kb, span, degree - 1, tau, &scratch.lower);
  }
  std::array<Rational, 3> sum{e.integer(0), e.integer(0), e.integer(0)};
  std::array<Rational, 3> change{e.integer(0), e.integer(0), e.integer(0)};
  for (unsigned j = 0; j <= degree; ++j) {
    m.work.poll();
    const auto i = span - degree + j;
    const auto weight = weighted ? m.input(reader, curve.pooled ? 0 : 3,
                                           curve.pooled ? 11 : 0, {cb + i})
                                 : e.integer(1);
    const auto coefficient = m.mul(scratch.basis[j], weight);
    auto differential = e.integer(0);
    if (derivative && degree) {
      auto a = e.integer(0), b = e.integer(0);
      const auto da = m.sub(knot(curve, kb + i + degree), knot(curve, kb + i));
      const auto db =
          m.sub(knot(curve, kb + i + degree + 1), knot(curve, kb + i + 1));
      if (j && !Exact::zero(da)) {
        a = m.div(scratch.lower[j - 1], da);
      }
      if (j < degree && !Exact::zero(db)) {
        b = m.div(scratch.lower[j], db);
      }
      differential = m.mul(m.scale(m.sub(a, b), degree), weight);
    }
    for (unsigned axis = 0; axis < 3; ++axis) {
      const auto p = axis == 2 ? e.integer(1)
                               : m.input(reader, curve.pooled ? 0 : 1,
                                         curve.pooled ? 9 : 0, {cb + i, axis});
      sum[axis] = m.add(sum[axis], m.mul(coefficient, p));
      if (derivative) {
        change[axis] = m.add(change[axis], m.mul(differential, p));
      }
    }
  }
  for (unsigned axis = 0; axis < 2; ++axis) {
    output->position[axis] = m.div(sum[axis], sum[2]);
    if (derivative) {
      output->derivative[axis] = m.mul(
          length,
          m.div(m.sub(m.mul(change[axis], sum[2]), m.mul(sum[axis], change[2])),
                m.sq(sum[2])));
    }
  }
}
std::pair<Curve, Curve> CurveMath::closure(const Curve& curve) const {
  const auto first =
      static_cast<std::uint64_t>(reader.index(0, 1, {curve.row}));
  return {primitive_curve(reader, curve.control[0] - 1),
          primitive_curve(reader, first)};
}
bool CurveMath::endpoints_equal(const Curve& last, const Curve& first) {
  struct Expression {
    std::array<Rational, 2> center, cosine, sine;
    Rational angle;
  };
  const auto expression = [&](const Curve& curve, bool end) {
    Expression value;
    const auto zero = m.exact.integer(0);
    value.cosine = {zero, zero};
    value.sine = {zero, zero};
    value.angle = zero;
    if (curve.kind != CurveKind::Arc && curve.kind != CurveKind::FullTurn) {
      CurveValue point;
      rational(curve, m.exact.integer(end ? 1 : 0), false, &point);
      value.center = point.position;
      return value;
    }
    value.angle = m.input(reader, 0, curve.field, {curve.row, 6});
    if (end && curve.kind == CurveKind::Arc) {
      value.angle =
          m.add(value.angle, m.input(reader, 0, curve.field, {curve.row, 7}));
    }
    for (unsigned axis = 0; axis < 2; ++axis) {
      value.center[axis] = m.input(reader, 0, curve.field, {curve.row, axis});
      value.cosine[axis] =
          m.input(reader, 0, curve.field, {curve.row, axis + 2});
      value.sine[axis] = m.input(reader, 0, curve.field, {curve.row, axis + 4});
    }
    if (value.angle.negative && !Exact::zero(value.angle)) {
      value.angle = m.exact.negate(value.angle);
      for (auto& component : value.sine) {
        component = m.exact.negate(component);
      }
    }
    if (Exact::zero(value.angle)) {
      for (unsigned axis = 0; axis < 2; ++axis) {
        value.center[axis] = m.add(value.center[axis], value.cosine[axis]);
      }
      value.cosine = {zero, zero};
      value.sine = {zero, zero};
    }
    if (Exact::zero(value.cosine[0]) && Exact::zero(value.cosine[1]) &&
        Exact::zero(value.sine[0]) && Exact::zero(value.sine[1])) {
      value.angle = zero;
    }
    return value;
  };
  const auto a = expression(last, true), b = expression(first, false);
  if (m.exact.order(a.angle, b.angle)) {
    return false;
  }
  for (unsigned axis = 0; axis < 2; ++axis) {
    if (m.exact.order(a.center[axis], b.center[axis]) ||
        m.exact.order(a.cosine[axis], b.cosine[axis]) ||
        m.exact.order(a.sine[axis], b.sine[axis])) {
      return false;
    }
  }
  check(m.exact.status());
  return true;
}
bool CurveMath::arc_landmark(const Curve& curve, const Rational& input,
                             int* sine, int* cosine) {
  const auto theta = m.input(reader, 0, curve.field, {curve.row, 6});
  const auto sweep = m.input(reader, 0, curve.field, {curve.row, 7});
  if (curve.kind == CurveKind::Arc) {
    if (!Exact::zero(m.add(theta, m.mul(input, sweep)))) {
      return false;
    }
    *sine = 0;
    *cosine = 1;
    return true;
  }
  if (!Exact::zero(theta)) {
    return false;
  }
  const auto turns = m.scale(input, 4);
  const auto q = m.exact.small_floor(turns);
  if (m.exact.order(turns, m.exact.integer(q)) != 0) {
    return false;
  }
  const int s[] = {0, 1, 0, -1}, c[] = {1, 0, -1, 0};
  const unsigned quadrant = sweep.negative ? (4 - q) & 3 : q & 3;
  *sine = s[quadrant];
  *cosine = c[quadrant];
  return true;
}
bool CurveMath::rational(const Curve& curve, const Rational& t, bool derivative,
                         CurveValue* output) {
  if (curve.kind == CurveKind::Bezier || curve.kind == CurveKind::Hermite) {
    if (!compact_polynomial(curve, t, derivative, output)) {
      if (curve.kind == CurveKind::Bezier) {
        bezier(curve, t, derivative, output);
      } else {
        hermite(curve, t, derivative, output);
      }
    }
    return true;
  }
  if (curve.kind == CurveKind::Spline) {
    spline(curve, t, derivative, output);
    return true;
  }
  if (curve.kind == CurveKind::Close) {
    const auto endpoints = closure(curve);
    CurveValue end, start;
    if (!rational(endpoints.first, m.exact.integer(1), false, &end) ||
        !rational(endpoints.second, m.exact.integer(0), false, &start)) {
      return false;
    }
    for (unsigned axis = 0; axis < 2; ++axis) {
      output->position[axis] =
          m.lerp(end.position[axis], start.position[axis], t);
      if (derivative) {
        output->derivative[axis] =
            m.sub(start.position[axis], end.position[axis]);
      }
    }
    return true;
  }
  int sine, cosine;
  if (!arc_landmark(curve, t, &sine, &cosine) ||
      (derivative && curve.kind == CurveKind::FullTurn)) {
    return false;
  }
  const auto sweep = m.input(reader, 0, curve.field, {curve.row, 7});
  for (unsigned axis = 0; axis < 2; ++axis) {
    const auto c = m.input(reader, 0, curve.field, {curve.row, axis});
    const auto u = m.input(reader, 0, curve.field, {curve.row, axis + 2});
    const auto v = m.input(reader, 0, curve.field, {curve.row, axis + 4});
    output->position[axis] =
        m.add(c, m.add(m.scale(u, cosine), m.scale(v, sine)));
    if (derivative) {
      output->derivative[axis] =
          m.mul(sweep, m.add(m.scale(u, -sine), m.scale(v, cosine)));
    }
  }
  return true;
}
void CurveMath::arc_vector(const Curve& curve, const Rational& input,
                           Interval output, unsigned axis, bool derivative,
                           bool unit_speed) {
  auto& d = m.d;
  Directed::Frame frame(d);
  auto sine = d.interval(), cosine = d.interval(), angle = d.interval(),
       sweep = d.interval(), temp = d.interval(), x = d.interval(),
       y = d.interval();
  const auto initial = m.input(reader, 0, curve.field, {curve.row, 6});
  const auto extent = m.input(reader, 0, curve.field, {curve.row, 7});
  const auto t = curve.kind == CurveKind::FullTurn &&
                         m.exact.order(input, m.exact.integer(1)) == 0
                     ? m.exact.integer(0)
                     : input;
  int s, c;
  if (arc_landmark(curve, t, &s, &c)) {
    d.integer(sine, s);
    d.integer(cosine, c);
  } else {
    if (curve.kind == CurveKind::FullTurn) {
      d.constant(sweep, true);
      d.scale(sweep, sweep, 1);
      if (extent.negative) {
        d.negate(sweep, sweep);
      }
      m.enclose(temp, t);
      d.multiply(angle, temp, sweep);
      m.enclose(temp, initial);
      d.add(angle, angle, temp);
    } else {
      m.enclose(angle, m.add(initial, m.mul(t, extent)));
    }
    m.sincos(sine, cosine, angle);
  }
  m.enclose(x, m.input(reader, 0, curve.field, {curve.row, axis + 2}));
  m.enclose(y, m.input(reader, 0, curve.field, {curve.row, axis + 4}));
  d.multiply(x, x, derivative ? sine : cosine);
  d.multiply(y, y, derivative ? cosine : sine);
  if (derivative) {
    d.negate(x, x);
  }
  d.add(output, x, y);
  if (!derivative) {
    m.enclose(temp, m.input(reader, 0, curve.field, {curve.row, axis}));
    d.add(output, output, temp);
  } else if (unit_speed) {
    if (extent.negative) {
      d.negate(output, output);
    }
  } else {
    if (curve.kind == CurveKind::FullTurn) {
      d.constant(sweep, true);
      d.scale(sweep, sweep, 1);
      if (extent.negative) {
        d.negate(sweep, sweep);
      }
    } else {
      m.enclose(sweep, extent);
    }
    d.multiply(output, output, sweep);
  }
}
void CurveMath::component(const Curve& curve, const Rational& t,
                          Interval output, unsigned axis, bool derivative) {
  if (curve.kind == CurveKind::Arc || curve.kind == CurveKind::FullTurn) {
    arc_vector(curve, t, output, axis, derivative);
    return;
  }
  CurveValue value;
  if (rational(curve, t, derivative, &value)) {
    m.enclose(output,
              derivative ? value.derivative[axis] : value.position[axis]);
    return;
  }
  Directed::Frame frame(m.d);
  const auto endpoints = closure(curve);
  auto start = m.d.interval(), end = m.d.interval(), factor = m.d.interval();
  component(endpoints.first, m.exact.integer(1), end, axis, false);
  component(endpoints.second, m.exact.integer(0), start, axis, false);
  if (endpoints_equal(endpoints.first, endpoints.second)) {
    if (derivative) {
      m.d.integer(output, 0);
    } else {
      m.d.copy(output, start);
    }
    return;
  }
  m.d.subtract(start, start, end);
  if (derivative) {
    m.d.copy(output, start);
  } else {
    m.enclose(factor, t);
    m.d.multiply(start, start, factor);
    m.d.add(output, end, start);
  }
}
bool CurveMath::valid(const Curve& curve, const Rational& t) {
  if (curve.kind == CurveKind::Arc || curve.kind == CurveKind::FullTurn) {
    return true;
  }
  CurveValue value;
  if (rational(curve, t, true, &value)) {
    return !Exact::zero(value.derivative[0]) ||
           !Exact::zero(value.derivative[1]);
  }
  const auto endpoints = closure(curve);
  if (endpoints_equal(endpoints.first, endpoints.second)) {
    return false;
  }
  m.certify(false, [&](Interval out) {
    auto x = m.d.interval(), y = m.d.interval();
    component(curve, t, x, 0, true);
    component(curve, t, y, 1, true);
    const auto nonzero = [&](Interval v) {
      return (!v.low.negative && m.d.top(v.low) >= 0) ||
             (v.high.negative && m.d.top(v.high) >= 0);
    };
    if (nonzero(x) || nonzero(y)) {
      m.d.integer(out, 1);
    } else {
      m.d.retry();
    }
  });
  return true;
}
std::uint64_t CurveMath::sample(const Curve& curve, const Rational& t,
                                unsigned output, unsigned axis, bool narrow) {
  CurveValue value;
  if (rational(curve, t, output != 0, &value)) {
    if (output == 0) {
      return m.rounded(value.position[axis], narrow, value.copy_position[axis]);
    }
    if (output == 1) {
      return m.rounded(value.derivative[axis], narrow,
                       value.copy_derivative[axis]);
    }
    if (output == 3) {
      return !Exact::zero(value.derivative[0]) ||
             !Exact::zero(value.derivative[1]);
    }
    return m.tangent(
        value.derivative[axis],
        m.add(m.sq(value.derivative[0]), m.sq(value.derivative[1])), narrow);
  }
  if (output == 3) {
    return valid(curve, t);
  }
  if (output == 2 && !valid(curve, t)) {
    return 0;
  }
  // Algebraic quadrant cases give exact derivative zeros and an exact rational
  // tangent direction even for a FullTurn's irrational 2*pi speed.
  if (curve.kind == CurveKind::Arc || curve.kind == CurveKind::FullTurn) {
    int sine, cosine;
    if (arc_landmark(curve, t, &sine, &cosine) && output != 0) {
      std::array<Rational, 2> q;
      const auto sweep = m.input(reader, 0, curve.field, {curve.row, 7});
      for (unsigned a = 0; a < 2; ++a) {
        q[a] = m.add(
            m.scale(m.input(reader, 0, curve.field, {curve.row, a + 2}), -sine),
            m.scale(m.input(reader, 0, curve.field, {curve.row, a + 4}),
                    cosine));
      }
      if (Exact::zero(q[axis])) {
        return 0;
      }
      if (output == 2) {
        return m.tangent(sweep.negative ? m.exact.negate(q[axis]) : q[axis],
                         m.add(m.sq(q[0]), m.sq(q[1])), narrow);
      }
    }
  }
  return m.certify(narrow, [&](Interval result) {
    if (output != 2) {
      component(curve, t, result, axis, output == 1);
      return;
    }
    auto x = m.d.interval(), y = m.d.interval(), norm = m.d.interval(),
         temp = m.d.interval();
    if (curve.kind == CurveKind::Arc || curve.kind == CurveKind::FullTurn) {
      arc_vector(curve, t, x, 0, true, true);
      arc_vector(curve, t, y, 1, true, true);
    } else {
      component(curve, t, x, 0, true);
      component(curve, t, y, 1, true);
    }
    m.d.multiply(norm, x, x);
    m.d.multiply(temp, y, y);
    m.d.add(norm, norm, temp);
    m.sqrt(norm, norm);
    m.d.divide(result, axis ? y : x, norm);
  });
}
}  // namespace ps::plugin_internal::generation
