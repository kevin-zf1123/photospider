#include <array>
#include <cmath>
#include <cstdint>
#include <new>
#include <optional>

#include "05-filter/advanced.hpp"
#include "05-filter/certified.hpp"

namespace ps::plugin_internal::filter {
namespace {
constexpr std::size_t kRangeCacheSize = kBilateralRangeCacheEntries;
struct RangeEntry {
  std::array<std::uint64_t, 8> key;
  std::uint64_t weight;
  bool valid;
};
static_assert(sizeof(RangeEntry) <= 80);
std::uint64_t mix_key(std::uint64_t x) {
  x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
  return x ^ (x >> 31);
}
std::uint64_t spatial_weight(generation::Math& m, const LocalFilter& f,
                             std::int64_t y, std::int64_t x) {
  if (!x && !y)
    return bits(1);
  auto a = m.div(m.sq(m.exact.integer(y)), m.scale(m.sq(m.value(f.sy)), 2));
  auto b = m.div(m.sq(m.exact.integer(x)), m.scale(m.sq(m.value(f.sx)), 2));
  auto value = m.add(a, b);
  value.negative = !Exact::zero(value);
  return exact_exp_negative(m, value);
}
}  // namespace
void run_bilateral(const LocalFilter& f, const Kernel& kernel,
                   const ps_kernel_invocation_v8& inv, const Work& work) {
  const Fuel fuel{work};
  const Reader reader{kernel, inv};
  generation::MathArena arena(inv.scratch[0].allocation, work);
  auto& m = *arena;
  m.exact.bind(fuel);
  m.d.consume = core_internal::WorkConsumer(fuel);
  const auto& g = f.geometry;
  const bool narrow = f.type == ElementType::Float32;
  const unsigned guide_port = f.kind == LocalKind::Bilateral ? 0 : 1;
  const auto area = g.kh * g.kw;
  auto* spatial = reinterpret_cast<double*>(arena.value + 1);
  auto* range = spatial + area;
  auto* values = reinterpret_cast<Atom*>(range + area);
  auto* cache = reinterpret_cast<RangeEntry*>(values + area);
  for (std::size_t i = 0; i < kRangeCacheSize; ++i) {
    fuel.poll();
    new (cache + i) RangeEntry;
    cache[i].valid = false;
  }
  for (std::uint64_t y = 0; y < g.kh; ++y)
    for (std::uint64_t x = 0; x < g.kw; ++x) {
      fuel.poll();
      spatial[y * g.kw + x] =
          number(spatial_weight(m, f, static_cast<std::int64_t>(y) - g.ay,
                                static_cast<std::int64_t>(x) - g.ax));
    }
  samples(inv.outputs[0], [&](const Coordinate& at, std::uint64_t) {
    std::array<Atom, 4> center;
    for (unsigned c = 0; c < f.components; ++c) {
      fuel.poll();
      center[c] = {reader.raw(guide_port, 0, f.guide_position(at, c)), narrow};
    }
    std::optional<std::uint64_t> first_nan;
    const auto observe = [&](Atom a) {
      if (a.nan() && !first_nan)
        first_nan = numeric_ops::converted_nan(
            a.raw, a.narrow ? ElementType::Float32 : ElementType::Float64,
            f.type);
    };
    for (std::uint64_t y = 0; y < g.kh; ++y) {
      for (std::uint64_t x = 0; x < g.kw; ++x) {
        fuel.poll();
        const auto i = y * g.kw + x;
        range[i] = 0;
        if (spatial[i] == 0)
          continue;
        bool outside;
        const auto pos = g.position(at, y, x, &outside);
        if (outside)
          continue;
        std::array<Atom, 4> other;
        Ball distance;
        bool zero = true, finite = true;
        std::array<std::uint64_t, 8> key{};
        std::uint64_t hash = 0;
        for (unsigned c = 0; c < f.components; ++c) {
          fuel.poll();
          other[c] = {reader.raw(guide_port, 0, f.guide_position(pos, c)),
                      narrow};
          observe(other[c]);
          observe(center[c]);
          finite &= std::isfinite(other[c].value()) &&
                    std::isfinite(center[c].value());
          auto delta = Ball::two_sum(other[c].value(), -center[c].value());
          finite &= std::isfinite(delta.first) && std::isfinite(delta.second);
          if (delta.first < 0 || (delta.first == 0 && delta.second < 0)) {
            delta.first = -delta.first;
            delta.second = -delta.second;
          }
          key[2 * c] = bits(delta.first == 0 ? 0 : delta.first);
          key[2 * c + 1] = bits(delta.second == 0 ? 0 : delta.second);
          hash = mix_key(hash ^ key[2 * c] ^
                         ((key[2 * c + 1] << 31) | (key[2 * c + 1] >> 33)));
          zero &= std::isfinite(other[c].value()) &&
                  other[c].value() == center[c].value();
        }
        auto& entry = cache[hash & (kRangeCacheSize - 1)];
        const bool hit = finite && entry.valid &&
                         std::equal(key.begin(), key.begin() + 2 * f.components,
                                    entry.key.begin());
        std::optional<std::uint64_t> wr;
        if (zero) {
          wr = bits(1);
        } else if (hit) {
          wr = entry.weight;
        } else {
          for (unsigned c = 0; c < f.components; ++c) {
            const auto diff = (Ball::point(other[c].value()) +
                               (-Ball::point(center[c].value())))
                                  .times(f.scales[c]);
            distance = distance + dd_product(diff, diff);
          }
          wr = dd_exp_negative(
              -dd_divide(
                  distance,
                  dd_product(Ball::point(f.sr), Ball::point(f.sr)).times(2)),
              fuel);
        }
        if (!wr) {
          auto d = m.exact.integer(0);
          for (unsigned c = 0; c < f.components; ++c) {
            const auto a = m.exact.binary(other[c].raw, narrow);
            const auto b = m.exact.binary(center[c].raw, narrow);
            d = m.add(d, m.sq(m.mul(m.sub(a, b), m.value(f.scales[c]))));
          }
          auto exponent = m.div(d, m.scale(m.sq(m.value(f.sr)), 2));
          exponent.negative = !Exact::zero(exponent);
          wr = exact_exp_negative(m, exponent);
        }
        if (finite && !zero && !hit) {
          fuel.poll(9);
          std::copy_n(key.begin(), 2 * f.components, entry.key.begin());
          entry.weight = *wr;
          entry.valid = true;
        }
        range[i] = number(*wr);
        if (range[i] > 0) {
          fuel.poll();
          values[i] = {reader.raw(0, 0, pos), narrow};
          observe(values[i]);
        }
      }
    }
    Ball numerator, denominator;
    bool negative_zero = true;
    for (std::uint64_t i = 0; i < area; ++i) {
      fuel.poll();
      if (spatial[i] == 0 || range[i] == 0)
        continue;
      const auto w = dd_product(Ball::point(spatial[i]), Ball::point(range[i]));
      denominator = denominator + w;
      if (range[i] > 0) {
        numerator = numerator + w.times(values[i].value());
        negative_zero &= values[i].zero() && std::signbit(values[i].value());
      } else {
        numerator = Ball::bad();
        negative_zero = false;
      }
    }
    auto result = BoundedArithmetic{narrow}.finish(
        BallValue{numerator, denominator, true});
    if (first_nan)
      result = first_nan;
    if (!result) {
      ExactArithmetic e{m.exact, f.type, {}};
      auto sum = e.zero(), den = e.zero();
      bool first = true;
      for (std::uint64_t i = 0; i < area; ++i) {
        fuel.poll();
        if (spatial[i] == 0 || range[i] == 0)
          continue;
        const auto w = m.mul(m.value(spatial[i]), m.value(range[i]));
        const auto term = range[i] > 0
                              ? m.mul(w, m.exact.binary(values[i].raw, narrow))
                              : m.exact.invalid();
        sum = first ? term : m.add(sum, term);
        den = first ? w : m.add(den, w);
        first = false;
      }
      result = e.finish(m.div(sum, den));
    }
    if (negative_zero)
      result = narrow ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);
    store(inv.outputs[0], at, *result);
  });
  fuel.flush();
}
}  // namespace ps::plugin_internal::filter
