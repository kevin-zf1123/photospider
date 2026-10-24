#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "05-filter/field_wide.hpp"
#include "05-filter/fields.hpp"

namespace ps::plugin_internal::filter {
std::uint64_t log_coefficient(generation::Math& m, double sigma,
                              std::uint64_t y, std::uint64_t x,
                              bool normalized) {
  const auto s2 = m.sq(m.value(sigma));
  const auto radius2 =
      m.add(m.sq(m.exact.integer(y)), m.sq(m.exact.integer(x)));
  const auto numerator = m.sub(radius2, m.scale(s2, 2));
  if (Exact::zero(numerator))
    return 0;
  auto exponent = m.div(radius2, m.scale(s2, 2));
  // All legal binary64 sigmas and admitted radii bound the prefactor by 2^6528.
  // exp(-16384)*2^6528 is below half the least binary64 subnormal.
  if (m.exact.order(exponent, m.exact.integer(16384)) >= 0) {
    check(m.exact.status());
    return numerator.negative ? UINT64_C(0x8000000000000000) : 0;
  }
  exponent.negative = !Exact::zero(exponent);
  auto factor = m.div(numerator, m.scale(m.mul(m.sq(s2), s2), 2));
  if (normalized)
    factor = m.mul(factor, s2);
  return m.certify(false, [&](generation::Interval out) {
    auto a = m.d.interval(), exponential = m.d.interval();
    auto coefficient = m.d.interval(), pi = m.d.interval();
    m.enclose(a, exponent);
    m.functions.exponential(exponential, a);
    m.enclose(coefficient, factor);
    m.d.constant(pi, true);
    m.d.multiply(out, coefficient, exponential);
    m.d.divide(out, out, pi);
  });
}

namespace {
template <class Arithmetic>
std::optional<std::uint64_t> log_sample(const Fields& f, const Reader& reader,
                                        const Fuel& fuel, const Coordinate& at,
                                        const double* taps, Arithmetic& math) {
  const auto& g = *f.geometry;
  const bool narrow = f.type == ElementType::Float32;
  fuel.poll();
  const Atom center{reader.raw(0, 0, at), narrow};
  auto sum = math.zero();
  bool first = true;
  for (std::uint64_t j = 0; j < g.kh; ++j) {
    for (std::uint64_t i = 0; i < g.kw; ++i) {
      fuel.poll();
      const auto coefficient = taps[j * g.kw + i];
      if ((j == f.radius && i == f.radius) || coefficient == 0)
        continue;
      bool outside = false;
      const auto pos = g.position(at, j, i, &outside);
      auto sample =
          outside ? Atom{bits(g.cval)} : Atom{reader.raw(0, 0, pos), narrow};
      auto value = math.from(sample), c = math.from(center);
      const auto difference =
          math.add(value, math.multiply(math.from({bits(-1)}), c));
      auto term = math.multiply(math.from({bits(coefficient)}), difference);
      sum = first ? term : math.add(sum, term);
      first = false;
    }
  }
  return math.finish(sum);
}
}  // namespace

void run_log(const Fields& f, const Kernel& kernel,
             const ps_kernel_invocation_v8& inv, const Work& work) {
  const Fuel fuel{work};
  const Reader reader{kernel, inv};
  generation::MathArena arena(inv.scratch[0].allocation, work);
  auto& m = *arena;
  m.exact.bind(fuel);
  m.d.consume = core_internal::WorkConsumer(fuel);
  const auto side = 2 * f.radius + 1;
  auto* taps = reinterpret_cast<double*>(arena.value + 1);
  for (std::uint64_t j = 0; j < side; ++j) {
    for (std::uint64_t i = 0; i < side; ++i) {
      fuel.poll();
      const auto y = j > f.radius ? j - f.radius : f.radius - j;
      const auto x = i > f.radius ? i - f.radius : f.radius - i;
      const auto a = std::max(y, x), b = std::min(y, x);
      const auto canonical = (f.radius - a) * side + f.radius - b;
      taps[j * side + i] =
          canonical < j * side + i
              ? taps[canonical]
              : number(log_coefficient(m, f.sigma, a, b, f.normalized));
    }
  }
  if (f.kind == FieldKind::LogKernel) {
    if (kernel.writes[0].first == 1) {
      ExactArithmetic exact{m.exact, ElementType::Float64, {}};
      auto sum = exact.zero();
      for (std::uint64_t i = 0; i < side * side; ++i) {
        fuel.poll();
        const auto term = exact.from({bits(taps[i])});
        sum = i ? exact.add(sum, term) : term;
      }
      store(inv.outputs[0], {0}, *exact.finish(sum));
    } else {
      samples(inv.outputs[0], [&](const Coordinate& at, std::uint64_t) {
        fuel.poll();
        store(inv.outputs[0], at, bits(taps[at[0] * side + at[1]]));
      });
    }
  } else {
    samples(inv.outputs[0], [&](const Coordinate& at, std::uint64_t) {
      FastArithmetic fast{f.type == ElementType::Float32, f.profile};
      auto result = log_sample(f, reader, fuel, at, taps, fast);
      if (!result) {
        FieldWideArithmetic wide{f.type == ElementType::Float32};
        result = log_sample(f, reader, fuel, at, taps, wide);
      }
      if (!result) {
        ExactArithmetic exact{m.exact, f.type, {}};
        result = log_sample(f, reader, fuel, at, taps, exact);
      }
      store(inv.outputs[0], at, *result);
    });
  }
  fuel.flush();
}

void run_staged(const Fields& f, const Kernel& kernel,
                const ps_kernel_invocation_v8& inv, const Work& work) {
  const auto& output = inv.outputs[0];
  std::vector<RegionDimension> dimensions;
  auto layout = *output.layout;
  std::uint64_t stride = generation::width(f.type);
  for (unsigned a = layout.rank; a-- > 0;) {
    layout.strides[a] = static_cast<std::int64_t>(stride);
    stride *= layout.shape[a];
  }
  for (unsigned a = 0; a < layout.rank; ++a)
    dimensions.push_back({layout.origin[a], layout.shape[a]});
  const auto prefix = f.staging_scratch(Region(dimensions));
  auto* data = static_cast<std::uint8_t*>(inv.scratch[0].allocation) + prefix;
  auto stage_output = output;
  stage_output.layout = &layout;
  stage_output.allocation = data;
  stage_output.byte_offset = 0;
  stage_output.bytes = stride;
  auto stage = inv;
  auto scratch = inv.scratch[0];
  scratch.bytes = prefix;
  stage.scratch = &scratch;
  stage.outputs = &stage_output;
  f.small->run_with_work(kernel, stage, work);
  if (f.large) {
    stage_output.allocation = data + stride;
    f.large->run_with_work(kernel, stage, work);
  }
  const Fuel fuel{work};
  const Reader reader{kernel, inv};
  generation::MathArena arena(inv.scratch[0].allocation, work);
  auto& m = *arena;
  m.exact.bind(fuel);
  m.d.consume = core_internal::WorkConsumer(fuel);
  const bool narrow = f.type == ElementType::Float32;
  samples(output, [&](const Coordinate& at, std::uint64_t) {
    fuel.poll(3);
    const auto offset = generation::offset(layout, 0, at);
    const auto load = [&](const void* p) {
      std::uint64_t raw = 0;
      std::memcpy(&raw, p, generation::width(f.type));
      return Atom{raw, narrow};
    };
    const auto small = load(data + offset);
    const auto base = f.large ? small : Atom{reader.raw(0, 0, at), narrow};
    const auto other = f.large ? load(data + stride + offset) : small;
    std::optional<std::uint64_t> result;
    FastArithmetic fast{narrow, f.profile};
    if (f.large) {
      result = fast.finish(fast.multiply(
          Fast::point(f.gain), fast.add(fast.from(base), -fast.from(other))));
      if (!result) {
        ExactArithmetic exact{m.exact, f.type, {}};
        auto a = exact.from(base), b = exact.from(other);
        result = exact.finish(m.mul(m.value(f.gain), m.sub(a, b)));
      }
    } else {
      std::optional<std::uint64_t> difference;
      if (std::isfinite(base.value()) && std::isfinite(other.value())) {
        // Both operands already have the destination dtype. The hardware
        // subtraction implements the declared S-stage directly.
        if (narrow) {
          const float a = static_cast<float>(base.value());
          const float b = static_cast<float>(other.value());
          difference = generation::convert(static_cast<double>(a - b), true);
        } else {
          difference = bits(base.value() - other.value());
        }
      }
      if (!difference) {
        ExactArithmetic exact{m.exact, f.type, {}};
        auto a = exact.from(base), b = exact.from(other);
        difference = exact.finish(m.sub(a, b));
      }
      const Atom detail{*difference, narrow};
      if (detail.nan()) {
        result = numeric_ops::converted_nan(detail.raw, f.type, f.type);
      } else if (!(std::abs(detail.value()) > f.threshold)) {
        result = base.raw;
      } else {
        result = fused_field(base, detail, f.gain, narrow, f.profile, fuel);
        if (!result) {
          ExactArithmetic exact{m.exact, f.type, {}};
          auto a = exact.from(base), d = exact.from(detail);
          result = exact.finish(m.add(a, m.mul(m.value(f.gain), d)));
        }
      }
    }
    store(output, at, *result);
  });
  fuel.flush();
}
}  // namespace ps::plugin_internal::filter
