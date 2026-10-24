#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <new>
#include <optional>
#include <utility>

#include "05-filter/certified.hpp"
#include "05-filter/local_windows.hpp"

namespace ps::plugin_internal::filter {
namespace {
struct BallOps {
  Ball value(Atom a) const { return Ball::point(a.value()); }
  Ball scalar(double a) const { return Ball::point(a); }
  Ball add(Ball a, Ball b) const { return a + b; }
  Ball sub(Ball a, Ball b) const { return a + (-b); }
  Ball mul(Ball a, Ball b) const { return dd_product(a, b); }
  Ball div(Ball a, Ball b) const { return dd_divide(a, b); }
};
// Workspace slots: means[0..4], products[5..24], centered samples[25..29],
// solve[30..49], original center[50..54], rounded coefficient[55..58].
template <unsigned Components = 0, class Ops, class V>
bool coefficients(const LocalFilter& f, const Reader& reader, const Fuel& fuel,
                  const Coordinate& center, Ops& op, V* s, double* result) {
  const auto& g = f.geometry;
  const bool narrow = f.type == ElementType::Float32;
  const unsigned n = Components ? Components : f.components;
  for (unsigned i = 0; i < 64; ++i)
    s[i] = op.scalar(0);
  bool special = false;
  const auto load = [&](unsigned port, Coordinate at) {
    fuel.poll();
    const Atom a{reader.raw(port, 0, at), narrow};
    special |=
        !std::isfinite(a.value()) || (a.zero() && std::signbit(a.value()));
    return op.value(a);
  };
  for (unsigned c = 0; c < n; ++c)
    s[50 + c] = load(1, f.guide_position(center, c));
  s[54] = load(0, center);
  const auto y0 = center[g.ya] > g.ay ? center[g.ya] - g.ay : 0;
  const auto x0 = center[g.xa] > g.ax ? center[g.xa] - g.ax : 0;
  const auto y1 = std::min(g.h - 1, center[g.ya] + g.ay);
  const auto x1 = std::min(g.w - 1, center[g.xa] + g.ax);
  const double count = static_cast<double>((y1 - y0 + 1) * (x1 - x0 + 1));
  auto at = center;
  for (auto y = y0; y <= y1; ++y) {
    at[g.ya] = y;
    for (auto x = x0; x <= x1; ++x) {
      at[g.xa] = x;
      for (unsigned c = 0; c < n; ++c) {
        s[25 + c] = op.mul(op.sub(load(1, f.guide_position(at, c)), s[50 + c]),
                           op.scalar(f.scales[c]));
        s[c] = op.add(s[c], s[25 + c]);
      }
      s[29] = op.sub(load(0, at), s[54]);
      s[4] = op.add(s[4], s[29]);
      for (unsigned c = 0; c < n; ++c) {
        fuel.poll(n + 1);
        for (unsigned d = c; d < n; ++d)
          s[5 + c * 4 + d] =
              op.add(s[5 + c * 4 + d], op.mul(s[25 + c], s[25 + d]));
        s[21 + c] = op.add(s[21 + c], op.mul(s[25 + c], s[29]));
      }
    }
  }
  // Multiply the normal equations by N^2 before solving. Exact centered
  // sums avoid intermediate divisions and certify zero cross moments.
  s[59] = op.scalar(count);
  const auto regularizer = op.mul(op.scalar(f.epsilon), op.mul(s[59], s[59]));
  for (unsigned c = 0; c < n; ++c) {
    for (unsigned d = 0; d < n; ++d) {
      s[30 + c * 4 + d] =
          op.sub(op.mul(s[5 + std::min(c, d) * 4 + std::max(c, d)], s[59]),
                 op.mul(s[c], s[d]));
      if (c == d)
        s[30 + c * 4 + d] = op.add(s[30 + c * 4 + d], regularizer);
    }
    s[46 + c] = op.sub(op.mul(s[21 + c], s[59]), op.mul(s[c], s[4]));
  }
  // SPD elimination has nonzero positive exact pivots; no numerical jitter.
  for (unsigned c = 0; c < n; ++c) {
    for (unsigned d = c + 1; d < n; ++d) {
      fuel.poll(n + 2);
      const auto factor = op.div(s[30 + d * 4 + c], s[30 + c * 4 + c]);
      for (unsigned j = c + 1; j < n; ++j)
        s[30 + d * 4 + j] =
            op.sub(s[30 + d * 4 + j], op.mul(factor, s[30 + c * 4 + j]));
      s[46 + d] = op.sub(s[46 + d], op.mul(factor, s[46 + c]));
    }
  }
  for (unsigned c = n; c-- > 0;) {
    for (unsigned d = c + 1; d < n; ++d)
      s[46 + c] = op.sub(s[46 + c], op.mul(s[30 + c * 4 + d], s[55 + d]));
    s[55 + c] = op.div(s[46 + c], s[30 + c * 4 + c]);
  }
  (void)result;
  return special;
}
template <unsigned Components>
bool fast_coefficients_impl(const LocalFilter& f, const Reader& reader,
                            const Fuel& fuel, const Coordinate& at,
                            double* out) {
  std::array<Ball, 64> s;
  BallOps op;
  if (coefficients<Components>(f, reader, fuel, at, op, s.data(), out))
    return false;
  for (unsigned c = 0; c < f.components; ++c) {
    const auto r = dd_round(s[55 + c], false);
    if (!r)
      return false;
    out[c] = number(*r);
  }
  auto b = s[4] + dd_product(s[54], s[59]);
  for (unsigned c = 0; c < f.components; ++c)
    b = b + (-(s[c] + dd_product(s[50 + c].times(f.scales[c]), s[59]))
                  .times(out[c]));
  const auto r = dd_round(dd_divide(b, s[59]), false);
  if (!r)
    return false;
  out[f.components] = number(*r);
  return true;
}
bool fast_coefficients(const LocalFilter& f, const Reader& reader,
                       const Fuel& fuel, const Coordinate& at, double* out) {
  switch (f.components) {
    case 1:
      return fast_coefficients_impl<1>(f, reader, fuel, at, out);
    case 2:
      return fast_coefficients_impl<2>(f, reader, fuel, at, out);
    case 3:
      return fast_coefficients_impl<3>(f, reader, fuel, at, out);
    default:
      return fast_coefficients_impl<4>(f, reader, fuel, at, out);
  }
}
struct Tagged {
  Rational value;
  std::uint64_t payload = UINT64_C(0x7ff8000000000000);
  bool source_nan = false;
};
static_assert(sizeof(Tagged) <= sizeof(Rational) + 32);
struct TaggedOps {
  generation::Math& m;
  Tagged from(Atom a) const {
    Tagged v;
    v.value = m.exact.binary(a.raw, a.narrow);
    if (a.nan()) {
      v.payload = numeric_ops::converted_nan(
          a.raw, a.narrow ? ElementType::Float32 : ElementType::Float64,
          ElementType::Float64);
      v.source_nan = true;
    }
    return v;
  }
  Tagged scalar(double a) const { return from({bits(a)}); }
  Tagged classified(Rational value, const Tagged& a, const Tagged& b) const {
    Tagged result;
    result.value = std::move(value);
    if (Exact::nan(a.value)) {
      result.payload = a.payload;
      result.source_nan = a.source_nan;
    } else if (Exact::nan(b.value)) {
      result.payload = b.payload;
      result.source_nan = b.source_nan;
    }
    return result;
  }
  Tagged add(const Tagged& a, const Tagged& b) const {
    return classified(m.add(a.value, b.value), a, b);
  }
  Tagged sub(const Tagged& a, const Tagged& b) const {
    return classified(m.sub(a.value, b.value), a, b);
  }
  Tagged mul(const Tagged& a, const Tagged& b) const {
    return classified(m.mul(a.value, b.value), a, b);
  }
  Tagged div(const Tagged& a, const Tagged& b) const {
    return classified(m.div(a.value, b.value), a, b);
  }
  Tagged sum(const Tagged& a, const Tagged& b) const {
    auto result = add(a, b);
    // A reduction's first source NaN takes precedence over invalid arithmetic
    // generated by an earlier term. Binary expressions keep written order.
    if (!a.source_nan && b.source_nan) {
      result.payload = b.payload;
      result.source_nan = true;
    }
    return result;
  }
  std::uint64_t rounded(const Tagged& a, bool narrow) const {
    check(m.exact.status());
    if (Exact::nan(a.value))
      return numeric_ops::converted_nan(
          a.payload, ElementType::Float64,
          narrow ? ElementType::Float32 : ElementType::Float64);
    return m.rounded(a.value, narrow, true);
  }
};
void exact_coefficients(const LocalFilter& f, const Reader& reader,
                        const Fuel& fuel, const Coordinate& center,
                        generation::Math& m, Tagged* s, double* out) {
  const TaggedOps op{m};
  const auto& g = f.geometry;
  const bool narrow = f.type == ElementType::Float32;
  const unsigned n = f.components;
  for (unsigned i = 0; i < 64; ++i)
    s[i] = op.scalar(0);
  const auto y0 = center[g.ya] > g.ay ? center[g.ya] - g.ay : 0;
  const auto x0 = center[g.xa] > g.ax ? center[g.xa] - g.ax : 0;
  const auto y1 = std::min(g.h - 1, center[g.ya] + g.ay);
  const auto x1 = std::min(g.w - 1, center[g.xa] + g.ax);
  const auto count =
      op.scalar(static_cast<double>((y1 - y0 + 1) * (x1 - x0 + 1)));
  auto at = center;
  bool first = true;
  for (auto y = y0; y <= y1; ++y) {
    at[g.ya] = y;
    for (auto x = x0; x <= x1; ++x) {
      at[g.xa] = x;
      for (unsigned c = 0; c < n; ++c) {
        fuel.poll();
        s[25 + c] =
            op.mul(op.from({reader.raw(1, 0, f.guide_position(at, c)), narrow}),
                   op.scalar(f.scales[c]));
        s[c] = first ? s[25 + c] : op.sum(s[c], s[25 + c]);
      }
      fuel.poll();
      s[29] = op.from({reader.raw(0, 0, at), narrow});
      s[4] = first ? s[29] : op.sum(s[4], s[29]);
      for (unsigned c = 0; c < n; ++c) {
        for (unsigned d = 0; d < n; ++d) {
          fuel.poll();
          const auto term = op.mul(s[25 + c], s[25 + d]);
          s[5 + c * 4 + d] = first ? term : op.sum(s[5 + c * 4 + d], term);
        }
        const auto term = op.mul(s[25 + c], s[29]);
        s[21 + c] = first ? term : op.sum(s[21 + c], term);
      }
      first = false;
    }
  }
  for (unsigned c = 0; c < n; ++c)
    s[c] = op.div(s[c], count);
  s[4] = op.div(s[4], count);
  for (unsigned c = 0; c < n; ++c) {
    for (unsigned d = 0; d < n; ++d) {
      s[30 + c * 4 + d] =
          op.sub(op.div(s[5 + c * 4 + d], count), op.mul(s[c], s[d]));
      if (c == d)
        s[30 + c * 4 + d] = op.add(s[30 + c * 4 + d], op.scalar(f.epsilon));
    }
    s[46 + c] = op.sub(op.div(s[21 + c], count), op.mul(s[c], s[4]));
  }
  for (unsigned c = 0; c < n; ++c)
    for (unsigned d = c + 1; d < n; ++d) {
      fuel.poll(n + 2);
      const auto factor = op.div(s[30 + d * 4 + c], s[30 + c * 4 + c]);
      for (unsigned j = c + 1; j < n; ++j)
        s[30 + d * 4 + j] =
            op.sub(s[30 + d * 4 + j], op.mul(factor, s[30 + c * 4 + j]));
      s[46 + d] = op.sub(s[46 + d], op.mul(factor, s[46 + c]));
    }
  for (unsigned c = n; c-- > 0;) {
    for (unsigned d = c + 1; d < n; ++d)
      s[46 + c] = op.sub(s[46 + c], op.mul(s[30 + c * 4 + d], s[55 + d]));
    s[55 + c] = op.div(s[46 + c], s[30 + c * 4 + c]);
  }
  auto dot = op.scalar(0);
  for (unsigned c = 0; c < n; ++c) {
    out[c] = number(op.rounded(s[55 + c], false));
    const auto term = op.mul(op.from({bits(out[c])}), s[c]);
    dot = c ? op.sum(dot, term) : term;
  }
  out[n] = number(op.rounded(op.sub(s[4], dot), false));
}
}  // namespace
void run_guided(const LocalFilter& f, const Kernel& kernel,
                const ps_kernel_invocation_v8& inv, const Work& work) {
  const Fuel fuel{work};
  const Reader reader{kernel, inv};
  generation::MathArena arena(inv.scratch[0].allocation, work);
  auto& m = *arena;
  m.exact.bind(fuel);
  const auto& g = f.geometry;
  const auto& output = inv.outputs[0];
  const bool narrow = f.type == ElementType::Float32;
  auto* exact = reinterpret_cast<Tagged*>(arena.value + 1);
  for (unsigned i = 0; i < 96; ++i)
    new (exact + i) Tagged;
  auto* stage = reinterpret_cast<double*>(exact + 96);
  const auto r = g.support(view_region(output)).at(0);
  const auto yd = r.dimensions()[g.ya], xd = r.dimensions()[g.xa];
  const auto pitch = xd.extent * (f.components + 1);
  auto* columns = reinterpret_cast<Ball*>(stage + yd.extent * pitch);
  static_assert(sizeof(Ball) == 24);
  each_plane(output, g, [&](Coordinate plane) {
    for (std::uint64_t y = 0; y < yd.extent; ++y) {
      plane[g.ya] = yd.offset + y;
      for (std::uint64_t x = 0; x < xd.extent; ++x) {
        plane[g.xa] = xd.offset + x;
        auto* dest = stage + y * pitch + x * (f.components + 1);
        if (!fast_coefficients(f, reader, fuel, plane, dest))
          exact_coefficients(f, reader, fuel, plane, m, exact, dest);
      }
    }
    const auto first_y = output.layout->origin[g.ya];
    const auto first_x = output.layout->origin[g.xa];
    const auto initial_y0 = first_y > g.ay ? first_y - g.ay : 0;
    const auto initial_y1 = std::min(g.h - 1, first_y + g.ay);
    for (std::uint64_t i = 0; i < pitch; ++i) {
      fuel.poll();
      new (columns + i) Ball;
    }
    for (auto yy = initial_y0; yy <= initial_y1; ++yy)
      for (std::uint64_t i = 0; i < pitch; ++i) {
        fuel.poll();
        columns[i] =
            columns[i] + Ball::point(stage[(yy - yd.offset) * pitch + i]);
      }
    for (std::uint64_t y = output.layout->origin[g.ya];
         y < output.layout->origin[g.ya] + output.layout->shape[g.ya]; ++y) {
      plane[g.ya] = y;
      std::array<Ball, 5> sum;
      for (auto xx = first_x > g.ax ? first_x - g.ax : 0;
           xx <= std::min(g.w - 1, first_x + g.ax); ++xx)
        for (unsigned j = 0; j <= f.components; ++j) {
          fuel.poll();
          sum[j] = sum[j] + columns[(xx - xd.offset) * (f.components + 1) + j];
        }
      for (std::uint64_t x = output.layout->origin[g.xa];
           x < output.layout->origin[g.xa] + output.layout->shape[g.xa]; ++x) {
        plane[g.xa] = x;
        const auto y0 = y > g.ay ? y - g.ay : 0;
        const auto x0 = x > g.ax ? x - g.ax : 0;
        const auto y1 = std::min(g.h - 1, y + g.ay);
        const auto x1 = std::min(g.w - 1, x + g.ax);
        const auto count = (y1 - y0 + 1) * (x1 - x0 + 1);
        auto value = sum[f.components];
        std::array<Atom, 4> guide;
        for (unsigned j = 0; j < f.components; ++j) {
          fuel.poll();
          guide[j] = {reader.raw(1, 0, f.guide_position(plane, j)), narrow};
          value = value + sum[j].times(guide[j].value()).times(f.scales[j]);
        }
        auto result = BoundedArithmetic{narrow}.finish(
            BallValue{value, Ball::point(static_cast<double>(count)), true});
        if (!result) {
          const TaggedOps op{m};
          bool first = true;
          for (auto yy = y0; yy <= y1; ++yy)
            for (auto xx = x0; xx <= x1; ++xx) {
              const auto* c = stage + (yy - yd.offset) * pitch +
                              (xx - xd.offset) * (f.components + 1);
              for (unsigned j = 0; j <= f.components; ++j) {
                fuel.poll();
                const auto v = op.from({bits(c[j])});
                exact[j] = first ? v : op.sum(exact[j], v);
              }
              first = false;
            }
          auto dot = op.scalar(0);
          for (unsigned j = 0; j < f.components; ++j) {
            const auto term = op.mul(
                exact[j], op.mul(op.from(guide[j]), op.scalar(f.scales[j])));
            dot = j ? op.sum(dot, term) : term;
          }
          result = op.rounded(op.div(op.add(dot, exact[f.components]),
                                     op.scalar(static_cast<double>(count))),
                              narrow);
        }
        store(output, plane, *result);
        if (x + 1 < output.layout->origin[g.xa] + output.layout->shape[g.xa]) {
          for (unsigned j = 0; j <= f.components; ++j) {
            fuel.poll(2);
            if (x >= g.ax)
              sum[j] =
                  sum[j] +
                  (-columns[(x - g.ax - xd.offset) * (f.components + 1) + j]);
            if (x + g.ax + 1 < g.w)
              sum[j] =
                  sum[j] +
                  columns[(x + g.ax + 1 - xd.offset) * (f.components + 1) + j];
          }
        }
      }
      if (y + 1 < output.layout->origin[g.ya] + output.layout->shape[g.ya]) {
        for (std::uint64_t i = 0; i < pitch; ++i) {
          fuel.poll(2);
          if (y >= g.ay)
            columns[i] =
                columns[i] +
                (-Ball::point(stage[(y - g.ay - yd.offset) * pitch + i]));
          if (y + g.ay + 1 < g.h)
            columns[i] =
                columns[i] +
                Ball::point(stage[(y + g.ay + 1 - yd.offset) * pitch + i]);
        }
      }
    }
  });
  fuel.flush();
}
}  // namespace ps::plugin_internal::filter
