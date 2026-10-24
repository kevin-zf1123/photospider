#include <algorithm>
#include <cmath>
#include <cstdint>

#include "05-filter/advanced.hpp"
#include "05-filter/field_wide.hpp"

namespace ps::plugin_internal::filter {
namespace {
struct Ranked {
  Atom atom;
  std::uint64_t order;
};
struct RankPosition {
  std::uint64_t lower, upper, nearest;
};
RankPosition position(std::uint64_t count, double q) {
  if (q == 1)
    return {count - 1, count - 1, count - 1};
  const auto raw = bits(q);
  const unsigned exponent = static_cast<unsigned>((raw >> 52) & 2047);
  const auto mantissa =
      (raw & UINT64_C(0x000fffffffffffff)) | (exponent ? UINT64_C(1) << 52 : 0);
  const unsigned shift = exponent ? 1075 - exponent : 1074;
  const unsigned __int128 numerator =
      static_cast<unsigned __int128>(count - 1) * mantissa;
  const auto lower =
      shift >= 128 ? 0 : static_cast<std::uint64_t>(numerator >> shift);
  const auto fraction =
      shift >= 128
          ? numerator
          : numerator & ((static_cast<unsigned __int128>(1) << shift) - 1);
  const auto upper = lower + (fraction != 0);
  auto nearest = lower;
  if (shift < 128) {
    const auto half = static_cast<unsigned __int128>(1) << (shift - 1);
    nearest += fraction > half || (fraction == half && (lower & 1));
  }
  return {lower, upper, nearest};
}
}  // namespace
void run_rank(const LocalFilter& f, const Kernel& kernel,
              const ps_kernel_invocation_v8& inv, const Work& work) {
  const Fuel fuel{work};
  const Reader reader{kernel, inv};
  generation::MathArena arena(inv.scratch[0].allocation, work);
  auto& m = *arena;
  m.exact.bind(fuel);
  const auto& g = f.geometry;
  const bool narrow = f.type == ElementType::Float32;
  const auto area = g.kh * g.kw;
  auto* ranked = reinterpret_cast<Ranked*>(arena.value + 1);
  auto* offsets =
      reinterpret_cast<std::array<std::uint64_t, 2>*>(ranked + area);
  std::uint64_t retained = 0;
  for (std::uint64_t y = 0; y < g.kh; ++y)
    for (std::uint64_t x = 0; x < g.kw; ++x) {
      fuel.poll();
      if (reader.raw(1, 0, {y, x}))
        offsets[retained++] = {y, x};
    }
  const auto less = [&](const Ranked& a, const Ranked& b) {
    fuel.poll();
    const double x = a.atom.value(), y = b.atom.value();
    return x < y || (x == y && a.order < b.order);
  };
  samples(inv.outputs[0], [&](const Coordinate& at, std::uint64_t) {
    std::uint64_t count = 0;
    std::optional<std::uint64_t> nan;
    for (std::uint64_t tap = 0; tap < retained; ++tap) {
      fuel.poll();
      const auto y = offsets[tap][0], x = offsets[tap][1];
      bool outside;
      const auto pos = g.position(at, y, x, &outside);
      if (outside && g.boundary == Boundary::Truncate)
        continue;
      fuel.poll();
      const Atom value =
          outside ? Atom{bits(g.cval)} : Atom{reader.raw(0, 0, pos), narrow};
      if (value.nan() && !nan)
        nan = numeric_ops::converted_nan(
            value.raw,
            value.narrow ? ElementType::Float32 : ElementType::Float64, f.type);
      ranked[count] = {value, count};
      ++count;
    }
    if (nan) {
      store(inv.outputs[0], at, *nan);
      return;
    }
    if (!count) {
      if (f.empty == "error")
        throw Status{ErrorCode::OperationFailed, "rank filter empty support"};
      fuel.poll();
      store(inv.outputs[0], at, reader.raw(0, 0, at));
      return;
    }
    std::sort(ranked, ranked + count, less);
    const auto p = position(count, f.quantile);
    auto lo = p.lower, hi = p.upper;
    if (f.policy == "lower")
      hi = lo;
    else if (f.policy == "higher" || f.policy == "upper")
      lo = hi;
    else if (f.policy == "nearest_even")
      lo = hi = p.nearest;
    const auto a = ranked[lo].atom, b = ranked[hi].atom;
    if (lo == hi) {
      store(
          inv.outputs[0], at,
          a.narrow == narrow ? a.raw : generation::convert(a.value(), narrow));
      return;
    }
    const double aa = a.value(), bb = b.value();
    if (std::isinf(aa) || std::isinf(bb)) {
      const double v = std::isinf(aa) && std::isinf(bb) &&
                               std::signbit(aa) != std::signbit(bb)
                           ? NAN
                       : std::isinf(aa) ? aa
                                        : bb;
      store(inv.outputs[0], at, generation::convert(v, narrow));
      return;
    }
    FieldWideArithmetic wide{narrow};
    const bool midpoint = f.policy == "midpoint" || f.policy == "mean";
    const auto weight =
        midpoint
            ? wide.from(Atom{bits(.5)})
            : wide.add(wide.multiply(
                           wide.from({bits(static_cast<double>(count - 1))}),
                           wide.from({bits(f.quantile)})),
                       wide.from({bits(-static_cast<double>(lo))}));
    const auto value = wide.add(
        wide.multiply(
            wide.from(a),
            wide.add(wide.one(), wide.multiply(wide.from({bits(-1)}), weight))),
        wide.multiply(wide.from(b), weight));
    auto result = wide.finish(value);
    if (!result) {
      ExactArithmetic e{m.exact, f.type, {}};
      const auto av = e.from(a), bv = e.from(b);
      const auto t =
          midpoint
              ? m.exact.fraction(1, 2)
              : m.sub(m.mul(m.exact.integer(count - 1), m.value(f.quantile)),
                      m.exact.integer(lo));
      result = e.finish(
          m.add(m.mul(m.sub(m.exact.integer(1), t), av), m.mul(t, bv)));
    }
    store(inv.outputs[0], at, *result);
  });
  fuel.flush();
}
}  // namespace ps::plugin_internal::filter
