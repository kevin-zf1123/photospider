#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include "05-filter/frequency.hpp"
#include "05-filter/frequency_fast.hpp"
#include "05-filter/frequency_response.hpp"

namespace ps::plugin_internal::frequency {
void sincos_turn(Math& m, Interval sine, Interval cosine,
                 const Rational& turns) {
  auto& e = m.exact;
  auto& d = m.d;
  Directed::Frame frame(d);
  const auto t = e.modulo(turns, 1);
  const unsigned q = e.small_floor(m.add(m.scale(t, 4), e.fraction(1, 2)));
  const auto r = m.sub(t, e.fraction(q, 4));
  auto s = d.interval(), c = d.interval();
  if (m.is_zero(r)) {
    d.integer(s, 0);
    d.integer(c, 1);
  } else {
    auto angle = d.interval(), pi = d.interval();
    m.enclose(angle, m.scale(r, 2));
    d.constant(pi, true);
    d.multiply(angle, angle, pi);
    m.functions.functions.sincos_series(s, c, angle);
  }
  m.functions.functions.orient(sine, cosine, s, c, q & 3, false);
}
std::uint64_t Frequency::window1d(Math& m, std::uint64_t n,
                                  std::uint64_t length) const {
  if (length == 1 || mode == "rectangular" || (mode == "tukey" && alpha == 0))
    return bits(1);
  auto& e = m.exact;
  const auto den = sampling == "periodic" ? length : length - 1;
  const auto t = e.fraction(n, den);
  auto turn = t;
  bool hann = mode == "hann" || alpha == 1;
  if (!hann) {
    const auto a = m.value(alpha), half = m.scale(a, 1);
    if (e.order(m.scale(t, 2), half) >= 0 &&
        e.order(m.scale(m.sub(e.integer(1), t), 2), half) >= 0)
      return bits(1);
    turn = e.order(m.scale(t, 2), a) < 0
               ? m.sub(m.div(t, a), e.fraction(1, 2))
               : m.add(m.div(m.sub(t, e.integer(1)), a), e.fraction(1, 2));
  }
  return m.certify(false, [&](Interval out) {
    auto& d = m.d;
    Directed::Frame frame(d);
    auto s = d.interval(), c = d.interval(), one = d.interval();
    sincos_turn(m, s, c, turn);
    d.integer(one, 1);
    if (hann)
      d.subtract(out, one, c);
    else
      d.add(out, one, c);
    d.scale(out, out, -1);
  });
}
namespace {
Rational coordinate(Math& m, std::uint64_t k, std::uint64_t n, double spacing) {
  const auto signed_k =
      k <= n / 2 ? static_cast<std::int64_t>(k)
                 : static_cast<std::int64_t>(k) - static_cast<std::int64_t>(n);
  return m.div(m.exact.integer(signed_k), m.scale(m.value(spacing), n));
}
void exponential_negative(Math& m, Interval out, const Rational& exponent) {
  auto& d = m.d;
  Directed::Frame frame(d);
  // exp(x)<=2^x for x<=0. Retain an enclosure for tails, not a rounded zero.
  const auto limit =
      m.exact.integer(-static_cast<std::int64_t>(d.precision + 1152));
  if (m.exact.order(exponent, limit) <= 0) {
    d.dyadic(out, 1, -static_cast<int>(d.precision + 1152));
    d.integer(out.low, 0);
    return;
  }
  auto x = d.interval();
  m.enclose(x, exponent);
  // Keep each exponential invocation inside its certified argument interval.
  d.scale(x, x, -4);
  m.functions.exponential(out, x);
  for (unsigned j = 0; j < 4 && d.active(); ++j)
    d.multiply(out, out, out);
}
Rational torus(Math& m, const Rational& f, double center, double spacing) {
  const auto period = m.div(m.exact.integer(1), m.value(spacing));
  const auto u = m.div(m.sub(f, m.value(center)), period);
  auto t = m.exact.modulo(u, 1);
  if (m.exact.order(t, m.exact.fraction(1, 2)) > 0)
    t = m.sub(t, m.exact.integer(1));
  return m.mul(t, period);
}
void lowpass(Math& m, Interval out, const Rational& r2, double cutoff,
             const std::string& mode, std::int64_t order) {
  auto& d = m.d;
  Directed::Frame frame(d);
  const auto u = m.div(r2, m.sq(m.value(cutoff)));
  if (mode == "gaussian") {
    exponential_negative(m, out, m.div(m.exact.negate(u), m.exact.integer(2)));
    return;
  }
  auto x = d.interval(), power = d.interval(), one = d.interval();
  m.enclose(x, u);
  d.integer(power, 1);
  d.integer(one, 1);
  for (auto n = static_cast<std::uint64_t>(order); n && d.active(); n >>= 1) {
    if (n & 1)
      d.multiply(power, power, x);
    if (n > 1)
      d.multiply(x, x, x);
  }
  d.add(power, power, one);
  m.sqrt(power, power);
  d.divide(out, one, power);
}
}  // namespace
std::uint64_t Frequency::response(Math& m, std::uint64_t y,
                                  std::uint64_t x) const {
  const auto h = dims[0], w = dims[1];
  // Conjugate pairs evaluate the same exact representative, including Nyquist.
  const auto ny = (h - y) % h, nx = (w - x) % w;
  if (ny * w + nx < y * w + x) {
    y = ny;
    x = nx;
  }
  auto& e = m.exact;
  const auto fy = coordinate(m, y, h, grid.dy),
             fx = coordinate(m, x, w, grid.dx);
  const auto r2 = m.add(m.sq(fy), m.sq(fx));
  if (kind != Kind::Notch && mode == "ideal") {
    const bool upper = e.order(r2, m.sq(m.value(high))) <= 0;
    const bool inside = kind == Kind::Radial
                            ? upper
                            : upper && e.order(r2, m.sq(m.value(low))) > 0;
    return bits((pass == "high" || pass == "reject" ? !inside : inside) ? 1
                                                                        : 0);
  }
  if (kind != Kind::Notch && m.is_zero(r2))
    return bits(pass == "low" || pass == "reject" ? 1 : 0);
  if (kind == Kind::Notch && depth == 0)
    return bits(1);
  return m.certify(false, [&](Interval out) {
    auto& d = m.d;
    Directed::Frame frame(d);
    auto a = d.interval(), b = d.interval(), one = d.interval();
    d.integer(one, 1);
    if (kind == Kind::Notch) {
      d.integer(out, 1);
      for (const auto& center : centers) {
        const auto cy = m.mul(m.value(center.first), m.value(grid.dy));
        const auto cx = m.mul(m.value(center.second), m.value(grid.dx));
        const bool self = m.is_zero(e.modulo(m.scale(cy, 2), 1)) &&
                          m.is_zero(e.modulo(m.scale(cx, 2), 1));
        for (int sign : {1, -1}) {
          if (sign == -1 && self)
            break;
          const auto yy = torus(m, fy, sign * center.first, grid.dy);
          const auto xx = torus(m, fx, sign * center.second, grid.dx);
          const auto exponent =
              m.div(e.negate(m.add(m.sq(m.div(yy, m.value(sy))),
                                   m.sq(m.div(xx, m.value(sx))))),
                    e.integer(2));
          if (m.is_zero(exponent) && depth == 1) {
            d.integer(out, 0);
            return;
          }
          exponential_negative(m, a, exponent);
          m.enclose(b, m.value(depth));
          d.multiply(a, a, b);
          d.subtract(a, one, a);
          d.multiply(out, out, a);
        }
      }
      return;
    }
    lowpass(m, a, r2, high, mode, order);
    if (kind == Kind::Band) {
      lowpass(m, b, r2, low, mode, order);
      d.subtract(a, a, b);
    }
    if (pass == "high" || pass == "reject")
      d.subtract(out, one, a);
    else
      d.copy(out, a);
  });
}
void Frequency::run_local(const Kernel& k, const ps_kernel_invocation_v8& inv,
                          ps_kernel_work_v9 service) const {
  const Work work{inv, work_limit, 0, service};
  const Fuel fuel{work};
  generation::MathArena arena(inv.scratch[0].allocation, work);
  auto& m = *arena;
  m.exact.bind(fuel);
  m.d.consume = core_internal::WorkConsumer(fuel);
  const Reader reader{k, inv};
  const bool narrow = type == ElementType::Float32;
  auto& e = m.exact;
  double* wx = reinterpret_cast<double*>(arena.value + 1);
  double* wy = wx + (kind == Kind::Window ? dims[1] : 0);
  if (kind == Kind::Window) {
    for (std::uint64_t x = 0; x < dims[1]; ++x) {
      fuel.poll();
      wx[x] = number(window1d(m, x, dims[1]));
    }
    for (std::uint64_t y = 0; y < dims[0]; ++y) {
      fuel.poll();
      wy[y] = number(window1d(m, y, dims[0]));
    }
  }
  if (kind == Kind::Statistics) {
    Ball total{}, squares{};
    for (std::uint64_t y = 0; y < dims[0]; ++y)
      for (std::uint64_t x = 0; x < dims[1]; ++x) {
        fuel.poll(512);
        Coordinate at{};
        at[0] = y;
        at[1] = x;
        const auto v = Ball::point(reader.value(0, 0, at));
        total = total + v;
        squares = squares + dd_product(v, v);
      }
    const double count = static_cast<double>(dims[0] * dims[1]);
    const std::optional<std::uint64_t> candidates[3] = {
        finish_ball(dd_divide(total, {count, 0, 0}), false, profile),
        finish_ball(dd_divide(squares, {count, 0, 0}), false, profile),
        finish_ball(dd_divide(squares.times(count), dd_product(total, total)),
                    false, profile)};
    bool accepted = true;
    for (unsigned i = 0; i < inv.output_count; ++i)
      accepted &= static_cast<bool>(candidates[k.writes[i].first]);
    if (accepted) {
      for (unsigned i = 0; i < inv.output_count; ++i)
        samples(inv.outputs[i], [&](const Coordinate& at, std::uint64_t) {
          store(inv.outputs[i], at, *candidates[k.writes[i].first]);
        });
      check(e.status());
      fuel.flush();
      return;
    }
    auto sum = e.integer(0), square = e.integer(0);
    std::optional<std::uint64_t> nan;
    for (std::uint64_t y = 0; y < dims[0]; ++y)
      for (std::uint64_t x = 0; x < dims[1]; ++x) {
        fuel.poll(3);
        Coordinate at{};
        at[0] = y;
        at[1] = x;
        const auto raw = reader.raw(0, 0, at);
        const Atom atom{raw, false};
        if (atom.nan() && !nan)
          nan = numeric_ops::converted_nan(raw, type, type);
        const auto v = e.binary(raw);
        sum = m.add(sum, v);
        square = m.add(square, m.sq(v));
      }
    const auto n = e.integer(dims[0] * dims[1]);
    for (unsigned i = 0; i < inv.output_count; ++i) {
      const auto port = k.writes[i].first;
      const auto value = port == 0   ? m.div(sum, n)
                         : port == 1 ? m.div(square, n)
                                     : m.div(m.mul(n, square), m.sq(sum));
      check(e.status());
      const auto result = nan ? *nan : m.rounded(value, false);
      samples(inv.outputs[i], [&](const Coordinate& at, std::uint64_t) {
        store(inv.outputs[i], at, result);
      });
    }
    check(e.status());
    fuel.flush();
    return;
  }
  for (unsigned i = 0; i < inv.output_count; ++i) {
    const auto port = k.writes[i].first;
    std::optional<ResponseAxes> axes_cache;
    if (port == 0 &&
        (((kind == Kind::Radial || kind == Kind::Band) && mode == "gaussian") ||
         (kind == Kind::Notch && depth != 0)))
      axes_cache.emplace(*this, m, fuel, inv.outputs[i], arena.value + 1);
    const auto& view = inv.outputs[i];
    auto* output_bytes =
        static_cast<std::uint8_t*>(view.allocation) + view.byte_offset;
    if (axes_cache && kind == Kind::Radial && profile == Profile::Strict &&
        view.layout->rank == 2 && view.layout->strides[1] == 8 &&
        view.layout->strides[0] >=
            static_cast<std::int64_t>(view.layout->shape[1] * 8) &&
        reinterpret_cast<std::uintptr_t>(output_bytes) % alignof(double) == 0) {
      for (std::uint64_t y = 0; y < view.layout->shape[0]; ++y) {
        const auto global_y = y + view.layout->origin[0];
        const auto partner_y = (dims[0] - global_y) % dims[0];
        const bool reverse = partner_y < global_y, self = partner_y == global_y;
        auto* row = output_bytes + y * view.layout->strides[0];
        for (std::uint64_t x = 0; x < view.layout->shape[1]; ++x) {
          fuel.poll(512);
          const auto global_x = x + view.layout->origin[1];
          auto candidate = axes_cache->evaluate_mapped(
              global_y, global_x, reverse || (self && global_x > dims[1] / 2));
          if (!candidate)
            candidate = response_candidate(*this, global_y, global_x, fuel);
          const auto result =
              candidate ? *candidate : response(m, global_y, global_x);
          std::memcpy(row + x * 8, &result, 8);
        }
      }
      continue;
    }
    samples(inv.outputs[i], [&](const Coordinate& at, std::uint64_t) {
      fuel.poll(512);
      std::uint64_t result = 0;
      if (kind == Kind::Window) {
        result = bits(wy[at[0]] * wx[at[1]]);
      } else if (kind == Kind::Radial || kind == Kind::Band ||
                 kind == Kind::Notch) {
        if (port == 0) {
          auto candidate = axes_cache ? axes_cache->evaluate(at[0], at[1])
                                      : std::optional<std::uint64_t>{};
          if (!candidate)
            candidate = response_candidate(*this, at[0], at[1], fuel);
          result = candidate ? *candidate : response(m, at[0], at[1]);
        }
      } else if (kind == Kind::Shift || kind == Kind::Unshift) {
        auto source = at;
        for (auto a : axes)
          source[a] = (source[a] + (kind == Kind::Shift ? (dims[a] + 1) / 2
                                                        : dims[a] / 2)) %
                      dims[a];
        result = reader.raw(port, 0, source);
      } else if (kind == Kind::Project) {
        auto other = at;
        other[grid.ya] = (dims[grid.ya] - at[grid.ya]) % dims[grid.ya];
        other[grid.xa] = (dims[grid.xa] - at[grid.xa]) % dims[grid.xa];
        const bool self = other == at;
        if (self) {
          result = port == 0 ? reader.raw(0, 0, at) : 0;
        } else {
          bool reversed = false;
          for (unsigned a = 0; a < dims.size(); ++a)
            if (at[a] != other[a]) {
              reversed = at[a] > other[a];
              break;
            }
          const auto first = reversed ? other : at,
                     second = reversed ? at : other;
          auto wide =
              Ball::point(reader.value(port, 0, first)) +
              Ball::point((port == 0 ? 1 : -1) * reader.value(port, 0, second));
          wide = wide.times(port == 1 && reversed ? -.5 : .5);
          const auto pair_candidate = finish_ball(wide, narrow, profile);
          if (pair_candidate && !(port == 0 && wide.point() && wide.hi == 0)) {
            const auto candidate = pair_candidate;
            const auto magnitude =
                *candidate &
                (narrow ? UINT64_C(0x7fffffff) : UINT64_C(0x7fffffffffffffff));
            store(inv.outputs[i], at,
                  port == 1 && magnitude == 0 ? 0 : *candidate);
            return;
          }
          ExactArithmetic exact{e, type, {}};
          const auto a = exact.from({reader.raw(port, 0, first), narrow});
          const auto b = exact.from({reader.raw(port, 0, second), narrow});
          auto v = m.div(port == 0 ? m.add(a, b) : m.sub(a, b), e.integer(2));
          check(e.status());
          result =
              exact.first_nan ? *exact.first_nan : m.rounded(v, narrow, true);
          if (port == 1 && reversed)
            result ^=
                narrow ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);
          if (port == 1 &&
              (result & (narrow ? UINT64_C(0x7fffffff)
                                : UINT64_C(0x7fffffffffffffff))) == 0)
            result = 0;
        }
      } else {
        auto coeff = at;
        for (auto a : broadcast)
          coeff[a] = 0;
        if (kind == Kind::Apply) {
          const auto input = Atom{reader.raw(0, 0, at), narrow};
          const auto window = Atom{reader.raw(1, 0, coeff), false};
          if ((input.zero() || window.zero()) && std::isfinite(input.value()) &&
              std::isfinite(window.value())) {
            const bool negative =
                std::signbit(input.value()) != std::signbit(window.value());
            store(inv.outputs[i], at,
                  negative ? (narrow ? UINT64_C(0x80000000)
                                     : UINT64_C(0x8000000000000000))
                           : 0);
            return;
          }
        }
        const auto fr = Ball::point(reader.value(0, 0, at));
        Ball wide;
        if (kind == Kind::Apply) {
          wide = dd_product(fr, Ball::point(reader.value(1, 0, coeff)));
        } else {
          const auto fi = Ball::point(reader.value(1, 0, at));
          const auto hr = Ball::point(reader.value(2, 0, coeff));
          const auto hi = Ball::point(reader.value(3, 0, coeff));
          wide = port == 0 ? dd_product(fr, hr) + (-dd_product(fi, hi))
                           : dd_product(fr, hi) + dd_product(fi, hr);
        }
        const auto point_candidate = finish_ball(wide, narrow, profile);
        if (point_candidate &&
            !(kind == Kind::Multiply && wide.point() && wide.hi == 0)) {
          const auto candidate = point_candidate;
          store(inv.outputs[i], at, *candidate);
          return;
        }
        ExactArithmetic exact{e, type, {}};
        const auto a = exact.from({reader.raw(0, 0, at), narrow});
        if (kind == Kind::Apply) {
          const auto b = exact.from({reader.raw(1, 0, coeff), false});
          const auto value = m.mul(a, b);
          check(e.status());
          result = exact.first_nan ? *exact.first_nan
                                   : m.rounded(value, narrow, true);
        } else {
          const bool response_narrow =
              k.groups[k.input_base[2]].type == ElementType::Float32;
          // Written product order determines first-NaN operand priority.
          const auto first_h = exact.from(
              {reader.raw(port == 0 ? 2 : 3, 0, coeff), response_narrow});
          const auto b = exact.from({reader.raw(1, 0, at), narrow});
          const auto second_h = exact.from(
              {reader.raw(port == 0 ? 3 : 2, 0, coeff), response_narrow});
          const auto& c = port == 0 ? first_h : second_h;
          const auto& d = port == 0 ? second_h : first_h;
          const auto value = port == 0 ? m.sub(m.mul(a, c), m.mul(b, d))
                                       : m.add(m.mul(a, d), m.mul(b, c));
          check(e.status());
          result = exact.first_nan ? *exact.first_nan
                                   : m.rounded(value, narrow, true);
        }
      }
      store(inv.outputs[i], at, result);
    });
  }
  check(e.status());
  fuel.flush();
}
}  // namespace ps::plugin_internal::frequency
