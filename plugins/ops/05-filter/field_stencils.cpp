#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "05-filter/field_wide.hpp"
#include "05-filter/field_windows.hpp"
#include "05-filter/fields.hpp"

namespace ps::plugin_internal::filter {
namespace {
template <class A>
auto constant(A& a, double x) {
  return a.from(Atom{bits(x)});
}
template <class A, class V>
auto subtract(A& a, const V& x, const V& y) {
  return a.add(x, a.multiply(y, constant(a, -1)));
}
template <class A, class V>
std::optional<std::uint64_t> finish_ratio(A& a, const V& n, const V& d) {
  if constexpr (std::is_same_v<A, FastArithmetic>) {
    if (a.profile != Profile::Strict)
      return a.finish(a.divide(n, d));
    if (auto r = a.quotient(n, d))
      return r;
  }
  return a.finish(a.divide(n, d));
}
template <class A>
auto count_value(A& a, std::uint64_t n) {
  if constexpr (std::is_same_v<A, ExactArithmetic>) {
    return a.e.integer(n);
  } else {
    const auto x = static_cast<double>(n);
    if constexpr (std::is_same_v<A, FastArithmetic>) {
      return static_cast<std::uint64_t>(x) == n
                 ? Fast::point(x)
                 : Fast{numeric_ops::numeric_down(x),
                        numeric_ops::numeric_up(x)};
    } else {
      return static_cast<std::uint64_t>(x) == n ? a.from(Atom{bits(x)})
                                                : BallValue{Ball::bad()};
    }
  }
}
template <class A>
std::optional<std::uint64_t> stencil(const Fields& f, const Reader& reader,
                                     const Fuel& fuel, const Coordinate& at,
                                     unsigned port, A& m) {
  const auto& g = *f.geometry;
  const bool narrow = f.type == ElementType::Float32;
  const auto input = [&](unsigned p, std::int64_t y, std::int64_t x) {
    fuel.poll();
    const auto my = g.map(__int128(at[g.ya]) + y, g.h);
    const auto mx = g.map(__int128(at[g.xa]) + x, g.w);
    if (my < 0 || mx < 0)
      return m.from(Atom{bits(g.cval)});
    auto pos = at;
    pos[g.ya] = my;
    pos[g.xa] = mx;
    return m.from(Atom{reader.raw(p, 0, pos), narrow});
  };
  const auto c = [&](double x) { return constant(m, x); };
  const auto sub = [&](const auto& a, const auto& b) {
    return subtract(m, a, b);
  };
  const auto square = [&](const auto& a) { return m.multiply(a, a); };
  if (f.kind == FieldKind::Central || f.kind == FieldKind::Sobel ||
      f.kind == FieldKind::Scharr) {
    auto sum = m.zero();
    bool first = true;
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        const int d = port == 0 ? x : y, s = port == 0 ? y : x;
        if (!d || (f.kind == FieldKind::Central && s))
          continue;
        const int smooth = f.kind == FieldKind::Central ? 1
                           : f.kind == FieldKind::Sobel ? (s ? 1 : 2)
                                                        : (s ? 3 : 10);
        auto value = input(0, y, x);
        auto term = m.multiply(c(d * smooth), value);
        sum = first ? term : m.add(sum, term);
        first = false;
      }
    }
    const auto normalization = f.mode == "raw"                ? 1
                               : f.kind == FieldKind::Central ? 2
                               : f.kind == FieldKind::Sobel   ? 8
                                                              : 32;
    const auto denominator =
        m.multiply(c(port == 0 ? f.dx : f.dy), c(normalization));
    if constexpr (std::is_same_v<A, FastArithmetic>) {
      if (auto r = m.quotient(sum, denominator))
        return r;
    }
    return m.finish(m.divide(sum, denominator));
  }
  if (f.kind == FieldKind::Hessian) {
    if (port == 1) {
      auto se = input(0, 1, 1), sw = input(0, 1, -1);
      auto ne = input(0, -1, 1), nw = input(0, -1, -1);
      return finish_ratio(m, m.add(sub(sub(se, sw), ne), nw),
                          m.multiply(m.multiply(c(4), c(f.dx)), c(f.dy)));
    }
    const bool x = port == 0;
    auto e = input(0, x ? 0 : 1, x ? 1 : 0);
    auto center = input(0, 0, 0);
    auto w = input(0, x ? 0 : -1, x ? -1 : 0);
    auto sum = m.add(sub(e, m.multiply(c(2), center)), w);
    return finish_ratio(m, sum, square(c(x ? f.dx : f.dy)));
  }
  if (f.kind == FieldKind::Laplacian4) {
    auto east = input(0, 0, 1), west = input(0, 0, -1);
    auto center = input(0, 0, 0);
    auto xx = sub(m.add(east, west), m.multiply(c(2), center));
    auto north = input(0, -1, 0), south = input(0, 1, 0);
    auto yy = sub(m.add(north, south), m.multiply(c(2), center));
    return m.finish(m.multiply(
        c(f.scale),
        m.add(m.divide(xx, square(c(f.dx))), m.divide(yy, square(c(f.dy))))));
  }
  if (f.kind == FieldKind::Laplacian8) {
    auto n = input(0, -1, 0), s = input(0, 1, 0);
    auto e = input(0, 0, 1), w = input(0, 0, -1);
    auto cross = m.multiply(c(4), m.add(m.add(m.add(n, s), e), w));
    auto ne = input(0, -1, 1), nw = input(0, -1, -1);
    auto se = input(0, 1, 1), sw = input(0, 1, -1);
    auto corners = m.add(m.add(m.add(ne, nw), se), sw);
    auto center = input(0, 0, 0);
    auto sum = sub(m.add(cross, corners), m.multiply(c(20), center));
    return finish_ratio(m, m.multiply(c(f.scale), sum),
                        m.multiply(c(6), square(c(f.dx))));
  }
  auto sx = m.zero(), sy = m.zero(), product = m.zero();
  bool first = true;
  std::uint64_t count = 0;
  for (std::uint64_t j = 0; j < g.kh; ++j) {
    for (std::uint64_t i = 0; i < g.kw; ++i) {
      fuel.poll();
      bool outside = false;
      const auto pos = g.position(at, j, i, &outside);
      if (outside && g.boundary == Boundary::Truncate)
        continue;
      const unsigned xport =
          f.kind == FieldKind::Structure && port == 2 ? 1 : 0;
      auto x =
          outside ? c(g.cval) : m.from(Atom{reader.raw(xport, 0, pos), narrow});
      auto y = x;
      const bool paired = f.kind == FieldKind::Covariance ||
                          (f.kind == FieldKind::Structure && port == 1);
      if (paired) {
        fuel.poll();
        y = outside ? c(g.cval) : m.from(Atom{reader.raw(1, 0, pos), narrow});
      }
      if (f.kind != FieldKind::Structure) {
        sx = first ? x : m.add(sx, x);
        if (f.kind == FieldKind::Covariance)
          sy = first ? y : m.add(sy, y);
      }
      if (f.kind != FieldKind::Moments || port == 1) {
        auto xy = m.multiply(x, y);
        product = first ? xy : m.add(product, xy);
      }
      first = false;
      ++count;
    }
  }
  auto n = count_value(m, count);
  if (f.kind == FieldKind::Structure)
    return finish_ratio(m, product, n);
  if (f.kind == FieldKind::Moments && port == 0) {
    if constexpr (std::is_same_v<A, FastArithmetic>) {
      if (auto r = m.quotient(sx, n))
        return r;
    }
    return m.finish(m.divide(sx, n));
  }
  auto numerator = sub(m.multiply(n, product),
                       m.multiply(sx, f.kind == FieldKind::Moments ? sx : sy));
  auto denominator = m.multiply(n, count_value(m, count - f.ddof));
  return finish_ratio(m, numerator, denominator);
}
struct SmallStencil {
  struct Tap {
    int y, x, weight;
  };
  Tap taps[9]{};
  unsigned count = 0;
  Fast divisor = Fast::point(1);
  bool enabled = false;
  SmallStencil(const Fields& f, unsigned port) {
    enabled = f.type == ElementType::Float32 &&
              (f.kind == FieldKind::Central || f.kind == FieldKind::Sobel ||
               f.kind == FieldKind::Scharr);
    if (!enabled)
      return;
    FastArithmetic fast{true, f.profile};
    const auto c = [](double x) { return Fast::point(x); };
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        const auto d = port == 0 ? x : y, smooth = port == 0 ? y : x;
        if (!d || (f.kind == FieldKind::Central && smooth))
          continue;
        const auto value =
            d * (f.kind == FieldKind::Central ? 1
                 : f.kind == FieldKind::Sobel ? (smooth ? 1 : 2)
                                              : (smooth ? 3 : 10));
        taps[count++] = {y, x, value};
      }
    }
    const auto n = f.mode == "raw"                ? 1
                   : f.kind == FieldKind::Central ? 2
                   : f.kind == FieldKind::Sobel   ? 8
                                                  : 32;
    divisor = fast.multiply(c(n), c(port == 0 ? f.dx : f.dy));
  }
  std::optional<std::uint64_t> run(const Fields& f, const Reader& reader,
                                   const Fuel& fuel,
                                   const Coordinate& at) const {
    if (!enabled)
      return {};
    const auto& g = *f.geometry;
    double sum = 0;
    bool exact = true;
    for (unsigned i = 0; i < count; ++i) {
      fuel.poll(8);
      auto pos = at;
      pos[g.ya] = g.map(__int128(at[g.ya]) + taps[i].y, g.h);
      pos[g.xa] = g.map(__int128(at[g.xa]) + taps[i].x, g.w);
      const double value = Atom{reader.raw(0, 0, pos), true}.value();
      // All values are multiples of 2^-41 and total magnitude is <2^12.
      // Thus each integer product and partial sum is exact in binary64.
      exact = exact && (value == 0 ||
                        (std::abs(value) >= 0x1p-18 && std::abs(value) <= 8));
      sum += taps[i].weight * value;
    }
    if (!exact || sum == 0)
      return {};
    FastArithmetic fast{true, f.profile};
    auto value = Fast::point(sum);
    return finish_ratio(fast, value, divisor);
  }
};
}  // namespace
void run_stencil(const Fields& f, const Kernel& kernel,
                 const ps_kernel_invocation_v8& inv, const Work& work) {
  const Fuel fuel{work};
  const Reader reader{kernel, inv};
  generation::MathArena arena(inv.scratch[0].allocation, work);
  arena->exact.bind(fuel);
  arena->d.consume = core_internal::WorkConsumer(fuel);
  const auto port = kernel.writes[0].first;
  const SmallStencil quick(f, port);
  const auto fallback = [&](const Coordinate& at) {
    auto result = quick.run(f, reader, fuel, at);
    if (result)
      return result;
    FastArithmetic fast{f.type == ElementType::Float32, f.profile};
    result = stencil(f, reader, fuel, at, port, fast);
    if (!result) {
      FieldWideArithmetic wide{f.type == ElementType::Float32};
      result = stencil(f, reader, fuel, at, port, wide);
    }
    if (!result) {
      ExactArithmetic exact{arena->exact, f.type, {}};
      result = stencil(f, reader, fuel, at, port, exact);
    }
    return result;
  };
  if (f.window_rows()) {
    field_window_rows(f, inv.outputs[0], reader, fuel,
                      reinterpret_cast<Fast*>(arena.value + 1), port, fallback);
  } else {
    samples(inv.outputs[0], [&](const Coordinate& at, std::uint64_t) {
      store(inv.outputs[0], at, *fallback(at));
    });
  }
  fuel.flush();
}
}  // namespace ps::plugin_internal::filter
