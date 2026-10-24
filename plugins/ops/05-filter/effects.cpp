#include "05-filter/effects.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "05-filter/effect_fast.hpp"
#include "05-filter/gather_patch.hpp"

namespace ps::plugin_internal::filter {
namespace {
Ball magnitude(Ball value) {
  return value.hi < 0 || (value.hi == 0 && value.lo < 0) ? -value : value;
}
template <class Visit>
void gather_taps(const Effect& effect, const Coordinate& at, double radius,
                 generation::Math& m, const Fuel& fuel, Visit visit) {
  const auto& g = *effect.geometry;
  const auto r = static_cast<std::int64_t>(radius);
  for (std::int64_t dy = -r; dy <= r; ++dy) {
    fuel.poll();
    const auto y = g.map(__int128(at[g.ya]) + dy, g.h);
    if (y < 0)
      continue;
    auto p = at;
    p[g.ya] = y;
    for (std::int64_t dx = -r; dx <= r; ++dx) {
      fuel.poll();
      if (effect.kind == EffectKind::Disk && !disk_contains(radius, dy, dx, m))
        continue;
      const auto x = g.map(__int128(at[g.xa]) + dx, g.w);
      if (x < 0)
        continue;
      p[g.xa] = x;
      visit(p);
    }
  }
}
std::uint64_t gather_pixel(const Effect& effect, const Reader& reader,
                           const Coordinate& at, generation::Math& m,
                           const Fuel& fuel) {
  const bool narrow = effect.type == ElementType::Float32;
  fuel.poll();
  const Atom control{reader.raw(1, 0, effect.control_position(at)),
                     effect.radius_narrow};
  const double radius = control.value();
  if (radius == 0)
    return reader.raw(0, 0, at);
  FastArithmetic fast{narrow, effect.profile};
  auto sum = fast.zero();
  std::uint64_t count = 0;
  gather_taps(effect, at, radius, m, fuel, [&](const Coordinate& p) {
    const auto value = fast.from({reader.raw(0, 0, p), narrow});
    sum = count++ ? fast.add(sum, value) : value;
  });
  if (const auto value = fast.quotient(sum, Fast::point(count)))
    return *value;
  if (const auto value = fast.finish(fast.divide(sum, Fast::point(count))))
    return *value;
  ExactArithmetic exact{m.exact, effect.type, {}};
  auto total = exact.zero();
  count = 0;
  gather_taps(effect, at, radius, m, fuel, [&](const Coordinate& p) {
    const auto value = exact.from({reader.raw(0, 0, p), narrow});
    total = count++ ? exact.add(total, value) : value;
  });
  return *exact.finish(exact.divide(total, m.exact.integer(count)));
}
std::uint64_t directional_pixel(const Effect& effect, const Reader& reader,
                                const Coordinate& at, generation::Math& m,
                                const Fuel& fuel) {
  const auto& g = *effect.geometry;
  const bool narrow = effect.type == ElementType::Float32;
  auto position = [&](int y, int x, bool control) {
    auto p = control ? effect.control_position(at) : at;
    p[g.ya] = g.map(__int128(at[g.ya]) + y, g.h);
    p[g.xa] = g.map(__int128(at[g.xa]) + x, g.w);
    return p;
  };
  fuel.poll(5);
  // The guide center belongs to the prescribed control cross.
  reader.raw(1, 0, effect.control_position(at));
  const Atom north{reader.raw(1, 0, position(-1, 0, true)), narrow};
  const Atom south{reader.raw(1, 0, position(1, 0, true)), narrow};
  const Atom east{reader.raw(1, 0, position(0, 1, true)), narrow};
  const Atom west{reader.raw(1, 0, position(0, -1, true)), narrow};
  bool active, vertical;
  const auto dx =
      ExactDouble::point(east.value()).add(ExactDouble::point(-west.value()));
  const auto dy =
      ExactDouble::point(south.value()).add(ExactDouble::point(-north.value()));
  if (narrow && dx.exact && dy.exact) {
    const double x = std::abs(dx.value), y = std::abs(dy.value);
    active = std::max(x, y) > effect.threshold;
    vertical = x >= y;
  } else {
    const auto ex =
        magnitude(Ball::point(east.value()) + (-Ball::point(west.value())));
    const auto ey =
        magnitude(Ball::point(south.value()) + (-Ball::point(north.value())));
    const auto xb = ex.bounds(), yb = ey.bounds();
    if (ex.point() && ey.point()) {
      active = std::max(ex.hi, ey.hi) > effect.threshold;
      vertical = ex.hi >= ey.hi;
    } else if (ex.finite() && ey.finite() &&
               (std::max(xb.high, yb.high) <= effect.threshold ||
                ((std::max(xb.low, yb.low) > effect.threshold) &&
                 (xb.low >= yb.high || xb.high < yb.low)))) {
      active = std::max(xb.low, yb.low) > effect.threshold;
      vertical = xb.low >= yb.high;
    } else {
      auto& e = m.exact;
      const auto x = e.absolute(
          m.sub(e.binary(east.raw, narrow), e.binary(west.raw, narrow)));
      const auto y = e.absolute(
          m.sub(e.binary(south.raw, narrow), e.binary(north.raw, narrow)));
      if (Exact::nan(x) || Exact::nan(y)) {
        const auto type = narrow ? ElementType::Float32 : ElementType::Float64;
        // max(ex, ey) consumes ex first, including an invalid NaN generated
        // by E-W. abs clears the selected subtraction NaN's sign.
        const auto first = Exact::nan(x) ? east : south;
        const auto second = Exact::nan(x) ? west : north;
        for (const auto value : {first, second})
          if (value.nan())
            return numeric_ops::converted_nan(value.raw, type, type) &
                   (narrow ? UINT64_C(0x7fffffff)
                           : UINT64_C(0x7fffffffffffffff));
        return narrow ? UINT64_C(0x7fc00000) : bits(NAN);
      } else {
        vertical = e.order(x, y) >= 0;
        active = e.order(vertical ? x : y, m.value(effect.threshold)) > 0;
      }
      check(e.status());
    }
  }
  const Atom center{reader.raw(0, 0, at), narrow};
  if (!active)
    return center.raw;
  fuel.poll(3);
  const Atom u{
      reader.raw(0, 0, position(vertical ? -1 : 0, vertical ? 0 : -1, false)),
      narrow};
  const Atom v{
      reader.raw(0, 0, position(vertical ? 1 : 0, vertical ? 0 : 1, false)),
      narrow};
  if (center.zero() && u.zero() && v.zero()) {
    const bool negative = std::signbit(center.value()) &&
                          std::signbit(u.value()) && std::signbit(v.value());
    return negative
               ? (narrow ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000))
               : 0;
  }
  if (narrow) {
    const auto weight =
        ExactDouble::point(1).add(ExactDouble::point(-effect.blend));
    const auto center_term =
        weight.multiply(ExactDouble::point(center.value()));
    const auto neighbors = ExactDouble::point(u.value())
                               .add(ExactDouble::point(v.value()))
                               .multiply(ExactDouble::point(effect.blend))
                               .multiply(ExactDouble::point(.5));
    const auto result = center_term.add(neighbors);
    if (result.exact)
      return result.value == 0 ? 0 : generation::convert(result.value, true);
  }
  const auto exact_weight = Ball{1, 0, 0} + Ball::point(-effect.blend);
  const auto result = dd_product(exact_weight, Ball::point(center.value())) +
                      (Ball::point(u.value()) + Ball::point(v.value()))
                          .times(effect.blend)
                          .times(.5);
  if (const auto value = dd_round(result, narrow))
    return *value;
  ExactArithmetic e{m.exact, effect.type, {}};
  const auto c = e.from(center), a = e.from(u), b = e.from(v);
  const auto weight = m.value(effect.blend);
  return *e.finish(
      m.add(m.mul(m.sub(e.one(), weight), c),
            m.div(m.mul(weight, m.add(a, b)), m.exact.integer(2))));
}
}  // namespace
void Effect::run(const Kernel& kernel,
                 const ps_kernel_invocation_v8& inv) const {
  const Work work{inv, work_limit};
  const Fuel fuel{work};
  const Reader reader{kernel, inv};
  const bool narrow = type == ElementType::Float32;
  for (unsigned o = 0; o < inv.output_count; ++o) {
    const bool valid = gather() && kernel.writes[o].first == 1;
    if (valid || identity()) {
      samples(inv.outputs[o], [&](const Coordinate& at, std::uint64_t) {
        fuel.poll();
        store(inv.outputs[o], at, valid ? 1 : reader.raw(0, 0, at));
      });
      continue;
    }
    generation::MathArena arena(inv.scratch[0].allocation, work);
    auto& m = *arena;
    m.exact.bind(fuel);
    if (gather_patch(*this)) {
      run_gather_patch(*this, reader, inv.outputs[o], arena, fuel);
      continue;
    }
    samples(inv.outputs[o], [&](const Coordinate& at, std::uint64_t) {
      fuel.poll();
      std::uint64_t result;
      if (kind == EffectKind::Contrast)
        result = contrast_pixel({reader.raw(0, 0, at), narrow},
                                {reader.raw(1, 0, at), narrow}, amount, scale,
                                m, fuel);
      else if (gather())
        result = gather_pixel(*this, reader, at, m, fuel);
      else
        result = directional_pixel(*this, reader, at, m, fuel);
      store(inv.outputs[o], at, result);
    });
  }
  fuel.flush();
}
}  // namespace ps::plugin_internal::filter
