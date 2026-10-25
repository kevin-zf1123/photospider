#include <algorithm>
#include <map>
#include <memory>
#include <numeric>
#include <tuple>
#include <utility>
#include <vector>

#include "05-filter/frequency.hpp"

namespace ps::plugin_internal::frequency {
namespace {
using Address = std::tuple<unsigned, unsigned, TileIndex>;
std::vector<std::uint64_t> key_of(const Region& r) {
  std::vector<std::uint64_t> key;
  for (const auto& d : r.dimensions()) {
    key.push_back(d.offset);
    key.push_back(d.extent);
  }
  return key;
}
bool power2(std::uint64_t n) {
  return (n & (n - 1)) == 0;
}
int mobius(std::uint64_t n, const Fuel& fuel) {
  int sign = 1;
  for (std::uint64_t p = 2; p <= n / p; ++p) {
    fuel.poll();
    if (n % p)
      continue;
    n /= p;
    sign = -sign;
    if (n % p == 0)
      return 0;
  }
  return n > 1 ? -sign : sign;
}
void cyclotomic(Math& m, Rational* phi, std::uint64_t n, const Fuel& fuel) {
  auto& e = m.exact;
  for (std::uint64_t j = 0; j <= n; ++j) {
    fuel.poll(Exact::kWords * 2);
    new (phi + j) Rational{};
  }
  phi[0] = e.integer(1);
  if (power2(n)) {
    phi[n / 2] = e.integer(1);
    return;
  }
  // Product (1-x^d)^mu(n/d), truncated as an exact formal power series.
  for (std::uint64_t d = 1; d <= n; ++d) {
    fuel.poll();
    if (n % d)
      continue;
    const int mu = mobius(n / d, fuel);
    if (mu > 0) {
      for (std::uint64_t j = n + 1; j-- > d;)
        phi[j] = m.sub(phi[j], phi[j - d]);
    }
    if (mu < 0) {
      for (std::uint64_t j = d; j <= n; ++j)
        phi[j] = m.add(phi[j], phi[j - d]);
    }
  }
  check(e.status());
}
int jacobi(std::uint64_t a, std::uint64_t n, const Fuel& fuel) {
  int result = 1;
  a %= n;
  while (a) {
    fuel.poll();
    while ((a & 1) == 0) {
      a >>= 1;
      if (n % 8 == 3 || n % 8 == 5)
        result = -result;
    }
    std::swap(a, n);
    if (a % 4 == 3 && n % 4 == 3)
      result = -result;
    a %= n;
  }
  return n == 1 ? result : 0;
}
// Primitive real quadratic character chi_D(a)=(D/a), D>0 fundamental.
// Its Gauss sum is the positive sqrt(D), not an independently rounded root.
int quadratic_character(std::uint64_t discriminant, std::uint64_t a,
                        const Fuel& fuel) {
  if (!a)
    return discriminant == 1 ? 1 : 0;
  int result = 1;
  while ((a & 1) == 0) {
    if ((discriminant & 1) == 0)
      return 0;
    if (discriminant % 8 == 3 || discriminant % 8 == 5)
      result = -result;
    a >>= 1;
  }
  return result * jacobi(discriminant, a, fuel);
}
void reduce_polynomial(Math& m, Rational* poly, const Rational* phi,
                       std::uint64_t n, std::uint64_t degree,
                       const Fuel& fuel) {
  for (std::uint64_t j = n; j-- > degree;) {
    fuel.poll(Exact::kWords);
    if (Exact::zero(poly[j]))
      continue;
    const auto c = poly[j];
    for (std::uint64_t k = 0; k <= degree; ++k) {
      fuel.poll(Exact::kWords);
      if (!Exact::zero(phi[k]))
        poly[j - degree + k] = m.sub(poly[j - degree + k], m.mul(c, phi[k]));
    }
  }
}
std::uint64_t direct(const Frequency& s, Math& m, const Reader& reader,
                     Coordinate at, unsigned component, Rational* poly,
                     Rational* phi, Rational* normalized, const Fuel& fuel) {
  auto& e = m.exact;
  const auto h = s.grid.shape[s.grid.ya], w = s.grid.shape[s.grid.xa];
  const auto n = s.root_order, ky = at[s.grid.ya], kx = at[s.grid.xa];
  for (std::uint64_t j = 0; j < n; ++j) {
    fuel.poll(Exact::kWords * 2);
    new (poly + j) Rational{};
  }
  std::optional<std::uint64_t> first_nan;
  bool all_zero = true, all_negative_zero = true, any_term = false;
  // Exceptional arithmetic is reduced in original logical sample order. The
  // polynomial is used only for finite terms; zero trigonometric factors are
  // structurally excluded before source arithmetic.
  auto exceptional = e.integer(0);
  const auto append = [&](const Rational& value, std::uint64_t phase) {
    poly[phase] = m.add(poly[phase], value);
    const auto mirror = (n - phase) % n;
    poly[mirror] = m.add(poly[mirror], value);
  };
  const bool y_first = s.grid.ya < s.grid.xa;
  for (std::uint64_t i = 0; i < h * w; ++i) {
    fuel.poll(4);
    const auto y = y_first ? i / w : i % h, x = y_first ? i % w : i / h;
    auto source = at;
    source[s.grid.ya] = y;
    source[s.grid.xa] = x;
    bool conjugated = false;
    if (s.kind == Kind::Irfft && x > w / 2) {
      source[s.grid.ya] = (h - y) % h;
      source[s.grid.xa] = w - x;
      conjugated = true;
    }
    const auto angle = static_cast<std::uint64_t>(
        (static_cast<unsigned __int128>(ky) * y * (n / h) +
         static_cast<unsigned __int128>(kx) * x * (n / w)) %
        n);
    const auto phase = s.inverse() ? angle : (n - angle) % n;
    for (unsigned input = 0; input < (s.real_input() ? 1U : 2U); ++input) {
      // Both components are real parts of powers of the same primitive root.
      auto p =
          (phase + (input ? n / 4 : 0) + (component ? 3 * (n / 4) : 0)) % n;
      if (p == n / 4 || p == 3 * (n / 4))
        continue;
      const bool narrow = s.type == ElementType::Float32;
      auto raw = reader.raw(input, 0, source);
      if (input && conjugated)
        raw ^= narrow ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);
      Atom a{raw, narrow};
      if (a.nan() && !first_nan)
        first_nan = numeric_ops::converted_nan(raw, s.type, s.type);
      auto value = e.binary(raw, narrow);
      any_term = true;
      all_zero &= Exact::zero(value);
      if (Exact::zero(value))
        all_negative_zero &= value.negative != (p > n / 4 && p < 3 * (n / 4));
      if (!Exact::finite(value)) {
        if (p > n / 4 && p < 3 * (n / 4))
          value = e.negate(value);
        exceptional = m.add(exceptional, value);
      } else {
        append(value, p);
      }
    }
  }
  if (first_nan)
    return *first_nan;
  if (all_zero && s.kind == Kind::Rfft && component == 1 &&
      (kx == 0 || (w % 2 == 0 && kx == w / 2)))
    return 0;
  if (all_zero)
    return any_term && all_negative_zero
               ? (s.type == ElementType::Float32 ? UINT64_C(0x80000000)
                                                 : UINT64_C(0x8000000000000000))
               : 0;
  if (!Exact::finite(exceptional))
    return m.rounded(exceptional, s.type == ElementType::Float32);
  std::uint64_t degree = n;
  while (degree && Exact::zero(phi[degree])) {
    fuel.poll(Exact::kWords);
    --degree;
  }
  reduce_polynomial(m, poly, phi, n, degree, fuel);
  auto scale = e.fraction(1, 2);
  if (s.grid.norm == "ortho") {
    if (s.norm_squarefree == 1) {
      scale = m.div(scale, e.integer(s.norm_squarefactor));
    } else {
      for (std::uint64_t j = 0; j < n; ++j) {
        fuel.poll(Exact::kWords * 2);
        new (normalized + j) Rational{};
      }
      // Multiply by the exact Gauss sum sqrt(D) in Q[zeta_n]/Phi_n.
      // 1/sqrt(HW) = sqrt(D)/(q*d) for D=d, or sqrt(D)/(2*q*d)
      // for D=4d. All algebraic dependencies survive until final RN.
      for (std::uint64_t a = 0; a < s.norm_discriminant; ++a) {
        fuel.poll();
        const int sign = quadratic_character(s.norm_discriminant, a, fuel);
        if (!sign)
          continue;
        const auto shift = a * (n / s.norm_discriminant);
        for (std::uint64_t j = 0; j < degree; ++j) {
          fuel.poll(Exact::kWords);
          if (Exact::zero(poly[j]))
            continue;
          const auto k = (j + shift) % n;
          normalized[k] = sign > 0 ? m.add(normalized[k], poly[j])
                                   : m.sub(normalized[k], poly[j]);
        }
      }
      reduce_polynomial(m, normalized, phi, n, degree, fuel);
      poly = normalized;
      scale = m.div(
          scale, e.integer(s.norm_squarefactor * s.norm_squarefree *
                           (s.norm_discriminant == s.norm_squarefree ? 1 : 2)));
    }
  }
  if (s.inverse() && s.grid.norm == "backward")
    scale = m.div(scale, e.integer(h * w));
  bool rational = true;
  for (std::uint64_t j = 1; j < degree; ++j) {
    fuel.poll(Exact::kWords);
    rational &= Exact::zero(poly[j]);
  }
  if (rational) {
    const auto value = m.mul(poly[0], scale);
    return m.rounded(value, s.type == ElementType::Float32);
  }
  return m.certify(s.type == ElementType::Float32, [&](Interval out) {
    auto& d = m.d;
    Directed::Frame frame(d);
    d.integer(out, 0);
    for (std::uint64_t j = 0; j < degree && d.active(); ++j) {
      fuel.poll(Exact::kWords);
      if (Exact::zero(poly[j]))
        continue;
      Directed::Frame term(d);
      auto a = d.interval(), b = d.interval(), c = d.interval();
      sincos_turn(m, b, c, e.fraction(j, n));
      m.enclose(a, poly[j]);
      d.multiply(a, a, c);
      d.add(out, out, a);
    }
    auto factor = d.interval();
    m.enclose(factor, scale);
    d.multiply(out, out, factor);
  });
}
struct Complex {
  Ball re, im;
};
Complex add(Complex a, Complex b) {
  return {a.re + b.re, a.im + b.im};
}
Complex minus(Complex a, Complex b) {
  return {a.re + (-b.re), a.im + (-b.im)};
}
Complex product(Complex a, Complex b) {
  return {dd_product(a.re, b.re) + (-dd_product(a.im, b.im)),
          dd_product(a.re, b.im) + dd_product(a.im, b.re)};
}
Ball root_component(Math& m, std::uint64_t k, std::uint64_t n, bool imaginary,
                    bool inverse) {
  if ((4 * k) % n == 0) {
    const auto q = ((4 * k / n) * (inverse ? 1 : 3)) & 3;
    const int c[4] = {1, 0, -1, 0}, s[4] = {0, 1, 0, -1};
    return {static_cast<double>(imaginary ? s[q] : c[q]), 0, 0};
  }
  const auto turns = m.exact.fraction(
      inverse ? static_cast<std::int64_t>(k) : -static_cast<std::int64_t>(k),
      n);
  const auto expression = [&](Interval out, double subtract) {
    auto& d = m.d;
    Directed::Frame frame(d);
    auto a = d.interval(), b = d.interval(), c = d.interval();
    sincos_turn(m, a, b, turns);
    m.enclose(c, m.value(subtract));
    d.subtract(out, imaginary ? a : b, c);
  };
  const auto hi =
      number(m.certify(false, [&](Interval out) { expression(out, 0); }));
  const auto lo =
      number(m.certify(false, [&](Interval out) { expression(out, hi); }));
  return {hi, lo, numeric_ops::numeric_up(std::abs(lo) * 0x1p-52 + 0x1p-1074)};
}
void fft_line(Complex* data, std::uint64_t n, std::uint64_t stride,
              const Complex* roots, std::uint64_t root_size, const Fuel& fuel) {
  for (std::uint64_t i = 1, j = 0; i < n; ++i) {
    fuel.poll();
    auto bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j)
      std::swap(data[i * stride], data[j * stride]);
  }
  for (std::uint64_t length = 2; length <= n; length *= 2)
    for (std::uint64_t i = 0; i < n; i += length)
      for (std::uint64_t j = 0; j < length / 2; ++j) {
        fuel.poll(512);
        const auto u = data[(i + j) * stride];
        const auto v = product(data[(i + j + length / 2) * stride],
                               roots[j * (root_size / length)]);
        data[(i + j) * stride] = add(u, v);
        data[(i + j + length / 2) * stride] = minus(u, v);
      }
}
}  // namespace

OperationDefinition transform_tasks(OperationDefinition def) {
  def.lower = [](const OperationLowering& q) -> Result<OperationProgram> {
    try {
      const auto& s = *static_cast<const Frequency*>(q.static_state);
      OperationProgramBuilder builder(q);
      ResourceLimits limits;
      limits.maximum_work = q.limits.maximum_work;
      ResourceBudget budget(limits);
      struct Group {
        Region region;
        std::vector<Address> writes;
      };
      std::map<std::vector<std::uint64_t>, Group> groups;
      for (unsigned p = 0; p < q.requests.size(); ++p) {
        const auto grid = take(TileGrid::from_schema(
            *q.outputs[p].result_schema, q.tile_extent, 0));
        for (const auto& member : q.requests[p].members)
          for (auto tile : member.second) {
            check(budget.consume({1}));
            const auto region = s.plane_region(take(grid.region(tile)));
            auto& group = groups[key_of(region)];
            group.region = region;
            group.writes.emplace_back(p, member.first, tile);
          }
      }
      for (const auto& item : groups) {
        auto bound = s;
        bound.selected = item.second.region;
        const auto task =
            take(builder.add_task("generation", "generation-unchecked-v1", {},
                                  {bound.task_scratch(0, {}), 0, 0}));
        for (const auto& read : bound.support(0, {})) {
          const auto grid = take(TileGrid::from_schema(
              *q.inputs[read.port].result_schema, q.tile_extent, read.member));
          for (auto tile :
               take(sdk::tiles_for_region(budget, grid, read.region)))
            check(builder.read_input(task, read.port, read.member, tile));
        }
        for (const auto& a : item.second.writes)
          check(builder.write_output(task, std::get<0>(a), std::get<1>(a),
                                     std::get<2>(a)));
      }
      return builder.finish();
    } catch (const Status& s) {
      return Result<OperationProgram>(s);
    }
  };
  struct Owner {
    std::shared_ptr<Frequency> state;
    ps_prepared_kernel_v8 inner{};
    ~Owner() {
      if (inner.release)
        inner.release(inner.context);
    }
  };
  const auto prepare = def.prepare_kernel;
  def.prepare_kernel =
      [prepare](const auto& desc, const auto& placement,
                const KernelPreparation& q, const auto& resources,
                const auto* native) -> Result<ps_prepared_kernel_v8> {
    try {
      require(!q.writes.empty(), "empty transform invocation");
      auto state = std::make_shared<Frequency>(
          *static_cast<const Frequency*>(q.static_state));
      state->selected =
          state->plane_region(fixed_tensor::view_region(q.writes[0].layout));
      for (const auto& w : q.writes)
        require(key_of(state->selected) ==
                    key_of(state->plane_region(
                        fixed_tensor::view_region(w.layout))),
                "transform batch group");
      auto mapped = q;
      mapped.static_state = state.get();
      auto owner = std::make_unique<Owner>();
      owner->state = std::move(state);
      owner->inner = take(prepare(desc, placement, mapped, resources, native));
      auto kernel = owner->inner;
      kernel.context = owner.release();
      kernel.invoke_cpu = [](const ps_kernel_invocation_v8* inv,
                             ps_kernel_status_v8* out) {
        const auto& owner = *static_cast<const Owner*>(inv->prepared_context);
        auto mapped = *inv;
        mapped.prepared_context = owner.inner.context;
        owner.inner.invoke_cpu(&mapped, out);
      };
      kernel.release = [](const void* p) {
        delete static_cast<const Owner*>(p);
      };
      return Result<ps_prepared_kernel_v8>(kernel);
    } catch (const Status& s) {
      return Result<ps_prepared_kernel_v8>(s);
    }
  };
  return def;
}
void Frequency::run_transform(const Kernel& k,
                              const ps_kernel_invocation_v8& inv) const {
  const Work work{inv, work_limit};
  const Fuel fuel{work};
  generation::MathArena arena(inv.scratch[0].allocation, work);
  auto& m = *arena;
  m.exact.bind(fuel);
  m.d.consume = core_internal::WorkConsumer(fuel);
  const Reader reader{k, inv};
  const auto h = grid.shape[grid.ya], w = grid.shape[grid.xa], n = root_order,
             root_size = std::max(h, w);
  auto* data = reinterpret_cast<Complex*>(arena.value + 1);
  auto* roots = data + h * w;
  auto* poly = reinterpret_cast<Rational*>(roots + root_size);
  auto* phi = poly + n;
  auto* normalized = phi + n + 1;
  bool phi_ready = false;
  const bool fft = power2(h) && power2(w);
  if (fft) {
    for (std::uint64_t j = 0; j < root_size / 2; ++j) {
      fuel.poll();
      new (roots + j) Complex{root_component(m, j, root_size, false, inverse()),
                              root_component(m, j, root_size, true, inverse())};
    }
  }
  Ball scale{1, 0, 0};
  if (grid.norm == "ortho") {
    const auto value = m.exact.fraction(1, h * w);
    const auto hi = number(m.rounded_sqrt(value, false));
    const auto lo = number(m.certify(false, [&](Interval out) {
      Directed::Frame f(m.d);
      auto v = m.d.interval();
      m.enclose(v, value);
      m.sqrt(out, v);
      m.enclose(v, m.value(hi));
      m.d.subtract(out, out, v);
    }));
    scale = {hi, lo,
             numeric_ops::numeric_up(std::abs(lo) * 0x1p-52 + 0x1p-1074)};
  } else if (inverse()) {
    scale = {1.0 / static_cast<double>(h * w), 0, 0};
  }
  Coordinate batch{};
  std::uint64_t batches = 1;
  for (unsigned a = 0; a < dims.size(); ++a) {
    batch[a] = selected.dimensions()[a].offset;
    if (a != grid.ya && a != grid.xa)
      batches *= selected.dimensions()[a].extent;
  }
  for (std::uint64_t b = 0; b < batches; ++b) {
    if (fft) {
      for (std::uint64_t y = 0; y < h; ++y)
        for (std::uint64_t x = 0; x < w; ++x) {
          fuel.poll(2);
          auto at = batch;
          at[grid.ya] = y;
          at[grid.xa] = x;
          bool conjugate = false;
          if (kind == Kind::Irfft && x > w / 2) {
            at[grid.ya] = (h - y) % h;
            at[grid.xa] = w - x;
            conjugate = true;
          }
          const auto re = reader.value(0, 0, at),
                     im = real_input() ? 0 : reader.value(1, 0, at);
          new (data + y * w + x)
              Complex{Ball::point(re), Ball::point(conjugate ? -im : im)};
        }
      for (std::uint64_t y = 0; y < h; ++y)
        fft_line(data + y * w, w, 1, roots, root_size, fuel);
      for (std::uint64_t x = 0; x < w; ++x)
        fft_line(data + x, h, w, roots, root_size, fuel);
    }
    for (unsigned i = 0; i < inv.output_count; ++i) {
      const auto component = k.writes[i].first;
      const auto& view = inv.outputs[i];
      bool contains_batch = true;
      for (unsigned a = 0; a < dims.size(); ++a) {
        fuel.poll();
        if (a != grid.ya && a != grid.xa)
          contains_batch &=
              batch[a] >= view.layout->origin[a] &&
              batch[a] - view.layout->origin[a] < view.layout->shape[a];
      }
      if (!contains_batch)
        continue;
      const auto output_sample = [&](const Coordinate& at) {
        fuel.poll(512);
        auto source = at;
        bool conjugate = false;
        if (kind == Kind::Rfft &&
            (at[grid.xa] == 0 || (w % 2 == 0 && at[grid.xa] == w / 2))) {
          const auto y = at[grid.ya], partner = (h - y) % h;
          if (component && partner == y) {
            store(inv.outputs[i], at, 0);
            return;
          }
          if (partner < y) {
            source[grid.ya] = partner;
            conjugate = component == 1;
          }
        }
        std::optional<std::uint64_t> result;
        if (fft) {
          const auto value = data[source[grid.ya] * w + source[grid.xa]];
          const auto v = dd_product(component ? value.im : value.re, scale);
          result = profile == Profile::Strict
                       ? dd_round(v, type == ElementType::Float32)
                       : FastArithmetic{type == ElementType::Float32, profile}
                             .finish(v.bounds());
          if (v.point() && v.hi == 0)
            result.reset();
        }
        if (!result) {
          if (!phi_ready) {
            cyclotomic(m, phi, n, fuel);
            phi_ready = true;
          }
          result = direct(*this, m, reader, source, component, poly, phi,
                          normalized, fuel);
        }
        if (conjugate) {
          const auto magnitude = *result & (type == ElementType::Float32
                                                ? UINT64_C(0x7fffffff)
                                                : UINT64_C(0x7fffffffffffffff));
          if (magnitude == 0) {
            if (!phi_ready) {
              cyclotomic(m, phi, n, fuel);
              phi_ready = true;
            }
            // Exact zero must stay +0, but underflowed nonzero keeps its sign.
            // Evaluate this coordinate directly when rounded bits cannot tell.
            result = direct(*this, m, reader, at, component, poly, phi,
                            normalized, fuel);
          } else {
            *result ^= type == ElementType::Float32
                           ? UINT64_C(0x80000000)
                           : UINT64_C(0x8000000000000000);
          }
        }
        store(inv.outputs[i], at, *result);
      };
      for (std::uint64_t y = view.layout->origin[grid.ya];
           y < view.layout->origin[grid.ya] + view.layout->shape[grid.ya]; ++y)
        for (std::uint64_t x = view.layout->origin[grid.xa];
             x < view.layout->origin[grid.xa] + view.layout->shape[grid.xa];
             ++x) {
          auto at = batch;
          at[grid.ya] = y;
          at[grid.xa] = x;
          output_sample(at);
        }
    }
    for (unsigned a = static_cast<unsigned>(dims.size()); a-- > 0;) {
      if (a == grid.ya || a == grid.xa)
        continue;
      if (++batch[a] <
          selected.dimensions()[a].offset + selected.dimensions()[a].extent)
        break;
      batch[a] = selected.dimensions()[a].offset;
    }
  }
  fuel.flush();
}
}  // namespace ps::plugin_internal::frequency
