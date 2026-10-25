#include <algorithm>
#include <map>
#include <memory>
#include <numeric>
#include <tuple>
#include <utility>
#include <vector>

#include "05-filter/frequency.hpp"
#include "05-filter/frequency_direct.hpp"

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
  // Exact rational components avoid certifying a zero residual by refinement.
  if (!imaginary && (6 * k) % n == 0) {
    const int cosine[6] = {2, 1, -1, -2, -1, 1};
    return {cosine[6 * k / n] * .5, 0, 0};
  }
  if (imaginary && (12 * k) % n == 0) {
    const auto phase = (12 * k / n) % 12;
    if (phase == 1 || phase == 5 || phase == 7 || phase == 11)
      return {(phase < 6 ? .5 : -.5) * (inverse ? 1 : -1), 0, 0};
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
void root_table(Math& math, Complex* output, std::uint64_t n,
                std::uint64_t count, bool inverse, const Fuel& fuel) {
  if (!count)
    return;
  if (n <= 4 || 12 % n == 0) {
    for (std::uint64_t j = 0; j < count; ++j) {
      fuel.poll();
      new (output + j) Complex{root_component(math, j, n, false, inverse),
                               root_component(math, j, n, true, inverse)};
    }
    return;
  }
  const Complex step{root_component(math, 1 % n, n, false, inverse),
                     root_component(math, 1 % n, n, true, inverse)};
  Complex current{};
  for (std::uint64_t j = 0; j < count; ++j) {
    fuel.poll(512);
    if (j % 32 == 0 || (12 * j) % n == 0)
      current = {root_component(math, j, n, false, inverse),
                 root_component(math, j, n, true, inverse)};
    else
      current = product(current, step);
    new (output + j) Complex{current};
  }
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
// Child transforms occupy j+r*k. One admitted line buffer combines them
// into natural order, without any rounded intermediate transform outputs.
void mixed_line(Complex* data, std::uint64_t n, std::uint64_t stride,
                const Complex* roots, std::uint64_t order, Complex* temporary,
                const Fuel& fuel) {
  if (n == 1)
    return;
  std::uint64_t radix = n;
  for (auto candidate : {2U, 3U, 5U})
    if (n % candidate == 0) {
      radix = candidate;
      break;
    }
  const auto m = n / radix;
  for (std::uint64_t j = 0; j < radix; ++j) {
    fuel.poll();
    mixed_line(data + j * stride, m, stride * radix, roots, order, temporary,
               fuel);
  }
  for (std::uint64_t k = 0; k < m; ++k)
    for (std::uint64_t l = 0; l < radix; ++l) {
      Complex sum{{0, 0, 0}, {0, 0, 0}};
      for (std::uint64_t j = 0; j < radix; ++j) {
        fuel.poll(512);
        const auto phase =
            static_cast<std::uint64_t>(static_cast<unsigned __int128>(j) *
                                       (k + m * l) * (order / n) % order);
        sum = add(sum, product(data[(j + radix * k) * stride], roots[phase]));
      }
      temporary[k + m * l] = sum;
    }
  for (std::uint64_t i = 0; i < n; ++i) {
    fuel.poll();
    data[i * stride] = temporary[i];
  }
}
// Both transform axes reuse this admitted workspace, never root_order.
void bluestein_axis(Math& math, Complex* data, std::uint64_t n,
                    std::uint64_t stride, std::uint64_t lines,
                    std::uint64_t line_step, bool inverse, Complex* workspace,
                    const Fuel& fuel) {
  const auto size = bluestein_size(n);
  auto* roots = workspace;
  auto* chirp = roots + size;
  auto* kernel = chirp + n;
  auto* line = kernel + size;
  root_table(math, roots, size, size / 2, false, fuel);
  for (std::uint64_t j = 0; j < size; ++j) {
    fuel.poll();
    new (kernel + j) Complex{{0, 0, 0}, {0, 0, 0}};
    new (line + j) Complex{{0, 0, 0}, {0, 0, 0}};
  }
  const Complex delta{root_component(math, 1, n, false, inverse),
                      root_component(math, 1, n, true, inverse)};
  Complex current{}, ratio{};
  for (std::uint64_t j = 0; j < n; ++j) {
    fuel.poll(1024);
    const auto phase = static_cast<std::uint64_t>(
        static_cast<unsigned __int128>(j) * j % (2 * n));
    if (j % 32 == 0) {
      const auto next = (2 * j + 1) % (2 * n);
      current = {root_component(math, phase, 2 * n, false, inverse),
                 root_component(math, phase, 2 * n, true, inverse)};
      ratio = {root_component(math, next, 2 * n, false, inverse),
               root_component(math, next, 2 * n, true, inverse)};
    } else if ((12 * phase) % (2 * n) == 0) {
      current = {root_component(math, phase, 2 * n, false, inverse),
                 root_component(math, phase, 2 * n, true, inverse)};
    }
    new (chirp + j) Complex{current};
    kernel[j] = {current.re, -current.im};
    if (j)
      kernel[size - j] = kernel[j];
    current = product(current, ratio);
    ratio = product(ratio, delta);
  }
  fft_line(kernel, size, 1, roots, size, fuel);
  for (std::uint64_t l = 0; l < lines; ++l) {
    auto* input = data + l * line_step;
    for (std::uint64_t j = 0; j < size; ++j) {
      fuel.poll(512);
      line[j] = j < n ? product(input[j * stride], chirp[j])
                      : Complex{{0, 0, 0}, {0, 0, 0}};
    }
    fft_line(line, size, 1, roots, size, fuel);
    for (std::uint64_t j = 0; j < size; ++j) {
      fuel.poll(512);
      line[j] = product(line[j], kernel[j]);
      line[j].im = -line[j].im;
    }
    fft_line(line, size, 1, roots, size, fuel);
    const auto scale = 1.0 / static_cast<double>(size);
    for (std::uint64_t j = 0; j < n; ++j) {
      fuel.poll(512);
      input[j * stride] = product(
          {line[j].re.times(scale), (-line[j].im).times(scale)}, chirp[j]);
    }
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
      auto result = builder.finish();
      if (!result.ok())
        return result;
      auto program = result.take_value();
      for (auto& kernel : program.program.kernels)
        kernel.abi_version = 9;
      check(physical::finalize_program(&program.program, q.limits));
      return Result<OperationProgram>(std::move(program));
    } catch (const Status& s) {
      return Result<OperationProgram>(s);
    }
  };
  struct BudgetOwner {
    std::shared_ptr<Frequency> state;
    ps_prepared_kernel_v9 inner{};
    ~BudgetOwner() {
      if (inner.release)
        inner.release(inner.context);
    }
  };
  const auto prepare_budgeted = def.prepare_kernel_v9;
  def.prepare_kernel_v9 =
      [prepare_budgeted](const auto& desc, const auto& placement,
                         const KernelPreparation& q, const auto& resources,
                         const auto* native) -> Result<ps_prepared_kernel_v9> {
    try {
      require(!q.writes.empty(), "empty budgeted transform invocation");
      auto owner = std::make_unique<BudgetOwner>();
      owner->state = std::make_shared<Frequency>(
          *static_cast<const Frequency*>(q.static_state));
      owner->state->selected = owner->state->plane_region(
          fixed_tensor::view_region(q.writes[0].layout));
      for (const auto& w : q.writes)
        require(key_of(owner->state->selected) ==
                    key_of(owner->state->plane_region(
                        fixed_tensor::view_region(w.layout))),
                "budgeted transform batch group");
      auto mapped = q;
      mapped.static_state = owner->state.get();
      owner->inner =
          take(prepare_budgeted(desc, placement, mapped, resources, native));
      auto kernel = owner->inner;
      kernel.context = owner.release();
      kernel.invoke_cpu = [](const ps_kernel_invocation_v9* inv,
                             ps_kernel_status_v8* out) {
        const auto& owner =
            *static_cast<const BudgetOwner*>(inv->views.prepared_context);
        auto mapped = *inv;
        mapped.views.prepared_context = owner.inner.context;
        owner.inner.invoke_cpu(&mapped, out);
      };
      kernel.release = [](const void* p) {
        delete static_cast<const BudgetOwner*>(p);
      };
      return Result<ps_prepared_kernel_v9>(kernel);
    } catch (const Status& failure) {
      return Result<ps_prepared_kernel_v9>(failure);
    }
  };
  return def;
}
void Frequency::run_transform(const Kernel& k,
                              const ps_kernel_invocation_v8& inv,
                              ps_kernel_work_v9 service) const {
  const Work work{inv, work_limit, 0, service};
  const Fuel fuel{work};
  generation::MathArena arena(inv.scratch[0].allocation, work);
  auto& m = *arena;
  m.exact.bind(fuel);
  m.d.consume = core_internal::WorkConsumer(fuel);
  const Reader reader{k, inv};
  const auto h = grid.shape[grid.ya], w = grid.shape[grid.xa],
             root_size = std::max(h, w);
  auto* data = reinterpret_cast<Complex*>(arena.value + 1);
  auto* roots = data + h * w;
  auto* axis_roots = roots + root_size;
  auto* temporary = axis_roots + h + w;
  for (std::uint64_t j = 0; j < root_size; ++j) {
    fuel.poll();
    new (temporary + j) Complex{};
  }
  auto* blue = temporary + root_size;
  const bool radix2 = power2(h) && power2(w);
  const bool fft = true;
  if (radix2) {
    root_table(m, roots, root_size, root_size / 2, inverse(), fuel);
  }
  if (!radix2) {
    for (auto axis :
         {std::make_pair(h, axis_roots), std::make_pair(w, axis_roots + h)})
      if (!bluestein_size(axis.first))
        root_table(m, axis.second, axis.first, axis.first, inverse(), fuel);
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
    scale = dd_divide({1, 0, 0}, {static_cast<double>(h * w), 0, 0});
  }
  Coordinate batch{};
  std::uint64_t batches = 1;
  for (unsigned a = 0; a < dims.size(); ++a) {
    batch[a] = selected.dimensions()[a].offset;
    if (a != grid.ya && a != grid.xa)
      batches *= selected.dimensions()[a].extent;
  }
  for (std::uint64_t b = 0; b < batches; ++b) {
    bool positive_zero_witness[2] = {false, false};
    bool positive_sine_witness = false;
    const auto period_y = h > 1 && h % 2 == 0 ? 2U : 1U;
    const auto period_x = w > 1 && w % 2 == 0 ? 2U : 1U;
    std::array<double, 8> periodic_seed{};
    bool periodic = kind != Kind::Irfft;
    bool packed_y_periodic = kind == Kind::Irfft;
    bool boundary_equal = true, boundary_opposite = true;
    bool boundary_antiperiodic = h % 2 == 0;
    bool real_periodic = kind == Kind::Irfft;
    bool imaginary_x_antiperiodic = kind == Kind::Irfft && w % 2 == 0;
    std::array<Ball, 4> real_template{};
    bool imaginary_y_zero[2] = {false, false};
    bool residual_period_checked = false;
    std::uint64_t residual_py = h, residual_px = w;
    const auto logical_value = [&](std::uint64_t y, std::uint64_t x) {
      fuel.poll(4);
      auto at = batch;
      bool conjugate = false;
      if (kind == Kind::Irfft && x > w / 2) {
        y = (h - y) % h;
        x = w - x;
        conjugate = true;
      }
      at[grid.ya] = y;
      at[grid.xa] = x;
      const auto re = reader.value(0, 0, at);
      const auto im = real_input() ? 0 : reader.value(1, 0, at);
      return std::make_pair(re, conjugate ? -im : im);
    };
    const auto find_residual_period = [&] {
      if (residual_period_checked)
        return;
      residual_period_checked = true;
      for (unsigned axis = 0; axis < 2; ++axis) {
        const auto extent = axis == 0 ? h : w;
        for (auto candidate : {3U, 5U, 7U}) {
          fuel.poll();
          if (candidate >= extent || extent % candidate)
            continue;
          bool equal = true;
          for (std::uint64_t y = 0; y < h && equal; ++y)
            for (std::uint64_t x = 0; x < w && equal; ++x) {
              const auto a = logical_value(y, x);
              const auto b = logical_value(axis == 0 ? y % candidate : y,
                                           axis == 1 ? x % candidate : x);
              equal = std::isfinite(a.first) && std::isfinite(a.second) &&
                      std::isfinite(b.first) && std::isfinite(b.second) &&
                      a == b;
            }
          if (equal) {
            (axis == 0 ? residual_py : residual_px) = candidate;
            break;
          }
        }
      }
    };
    if (periodic) {
      for (unsigned y = 0; y < period_y; ++y)
        for (unsigned x = 0; x < period_x; ++x) {
          fuel.poll(4);
          auto at = batch;
          at[grid.ya] = y;
          at[grid.xa] = x;
          periodic_seed[2 * (y * 2 + x)] = reader.value(0, 0, at);
          periodic_seed[2 * (y * 2 + x) + 1] =
              real_input() ? 0 : reader.value(1, 0, at);
        }
    }
    if (fft) {
      auto origin = batch;
      origin[grid.ya] = origin[grid.xa] = 0;
      for (unsigned component = 0; component < (real_input() ? 1U : 2U);
           ++component) {
        const Atom value{reader.raw(component, 0, origin),
                         type == ElementType::Float32};
        // The origin coefficient for this component is exactly +1. A finite
        // source other than -0 excludes an all-negative-zero reduction.
        positive_zero_witness[component] =
            std::isfinite(value.value()) &&
            value.raw != (type == ElementType::Float32
                              ? UINT64_C(0x80000000)
                              : UINT64_C(0x8000000000000000));
      }
      if (kind == Kind::Rfft && w > 2) {
        auto witness = origin;
        witness[grid.xa] = w - 1;
        const Atom value{reader.raw(0, 0, witness),
                         type == ElementType::Float32};
        // sin(2*pi*kx/W) is strictly positive for every nonboundary packed
        // column. At y=0 this witness is independent of ky.
        positive_sine_witness =
            std::isfinite(value.value()) &&
            value.raw != (type == ElementType::Float32
                              ? UINT64_C(0x80000000)
                              : UINT64_C(0x8000000000000000));
      }
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
          fuel.poll(4);
          const auto seed = 2 * ((y % period_y) * 2 + x % period_x);
          periodic &= std::isfinite(re) && std::isfinite(im) &&
                      re == periodic_seed[seed] &&
                      im == periodic_seed[seed + 1];
          const auto imaginary = conjugate ? -im : im;
          if (kind == Kind::Irfft) {
            packed_y_periodic &= std::isfinite(re) && std::isfinite(imaginary);
            if (packed_y_periodic && y >= period_y) {
              const auto& seed_value = data[(y % period_y) * w + x];
              packed_y_periodic &= seed_value.re.point() &&
                                   seed_value.re.finite() &&
                                   re == seed_value.re.hi;
              if (x != 0 && !(w % 2 == 0 && x == w / 2))
                packed_y_periodic &= seed_value.im.point() &&
                                     seed_value.im.finite() &&
                                     imaginary == seed_value.im.hi;
            }
          }
          if (kind == Kind::Irfft) {
            if (y < period_y && x < period_x)
              real_template[y * 2 + x] = Ball::point(re);
            const auto& original =
                real_template[(y % period_y) * 2 + x % period_x];
            real_periodic &= original.finite() && original.point() &&
                             std::isfinite(re) && re == original.hi;
          }
          new (data + y * w + x)
              Complex{Ball::point(re), Ball::point(imaginary)};
        }
      if (kind == Kind::Irfft && packed_y_periodic) {
        for (std::uint64_t y = 0; y < h; ++y) {
          fuel.poll(4);
          const auto& a = data[y * w].im;
          const auto b = w % 2 == 0 ? data[y * w + w / 2].im : Ball{0, 0, 0};
          boundary_equal &= a.point() && a.finite() && b.point() &&
                            b.finite() && a.hi == b.hi;
          boundary_opposite &= a.point() && a.finite() && b.point() &&
                               b.finite() && a.hi == -b.hi;
          if (boundary_antiperiodic) {
            const auto& partner_a = data[((y + h / 2) % h) * w].im;
            const auto partner_b = w % 2 == 0
                                       ? data[((y + h / 2) % h) * w + w / 2].im
                                       : Ball{0, 0, 0};
            boundary_antiperiodic &=
                a.point() && a.finite() && b.point() && b.finite() &&
                partner_a.point() && partner_a.finite() && partner_b.point() &&
                partner_b.finite() && partner_a.hi == -a.hi &&
                partner_b.hi == -b.hi;
          }
        }
      }
      if (packed_y_periodic) {
        for (unsigned frequency = 0; frequency < period_y; ++frequency) {
          imaginary_y_zero[frequency] = true;
          for (std::uint64_t x = 1; x < w; ++x) {
            if (w % 2 == 0 && x == w / 2)
              continue;
            Ball total{};
            for (unsigned y = 0; y < period_y; ++y) {
              fuel.poll(4);
              total = total +
                      data[y * w + x].im.times((y * frequency) & 1 ? -1 : 1);
            }
            imaginary_y_zero[frequency] &=
                total.finite() && total.point() && total.hi == 0;
          }
        }
      }
      if (imaginary_x_antiperiodic) {
        for (std::uint64_t y = 0; y < h && imaginary_x_antiperiodic; ++y)
          for (std::uint64_t x = 1; x < w / 2; ++x) {
            fuel.poll(4);
            const auto& a = data[y * w + x].im;
            const auto& b = data[y * w + x + w / 2].im;
            imaginary_x_antiperiodic &= a.finite() && a.point() && b.finite() &&
                                        b.point() && a.hi == -b.hi;
          }
      }
      if (bluestein_size(w)) {
        bluestein_axis(m, data, w, 1, h, w, inverse(), blue, fuel);
      } else if (w > 1) {
        for (std::uint64_t y = 0; y < h; ++y)
          if (power2(w))
            fft_line(data + y * w, w, 1, radix2 ? roots : axis_roots + h,
                     radix2 ? root_size : w, fuel);
          else
            mixed_line(data + y * w, w, 1, axis_roots + h, w, temporary, fuel);
      }
      if (bluestein_size(h)) {
        bluestein_axis(m, data, h, w, w, 1, inverse(), blue, fuel);
      } else if (h > 1) {
        if (root_size < 1024) {
          for (std::uint64_t x = 0; x < w; ++x) {
            if (power2(h))
              fft_line(data + x, h, w, radix2 ? roots : axis_roots,
                       radix2 ? root_size : h, fuel);
            else
              mixed_line(data + x, h, w, axis_roots, h, temporary, fuel);
          }
        } else {
          auto* column = radix2 ? axis_roots : roots;
          for (std::uint64_t x = 0; x < w; ++x) {
            for (std::uint64_t y = 0; y < h; ++y) {
              fuel.poll(2);
              new (column + y) Complex{data[y * w + x]};
            }
            if (power2(h))
              fft_line(column, h, 1, radix2 ? roots : axis_roots,
                       radix2 ? root_size : h, fuel);
            else
              mixed_line(column, h, 1, axis_roots, h, temporary, fuel);
            for (std::uint64_t y = 0; y < h; ++y) {
              fuel.poll(2);
              data[y * w + x] = column[y];
            }
          }
        }
      }
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
        bool exact_zero = false;
        if (fft) {
          // Outside the y-periodic support, packed interior columns vanish.
          // The only remaining imaginary boundary columns cancel pointwise
          // when their coefficients 1 and (-1)^kx have opposite amplitudes.
          const bool packed_excluded =
              packed_y_periodic && source[grid.ya] % (h / period_y) != 0 &&
              ((boundary_antiperiodic && source[grid.ya] % 2 == 0) ||
               (source[grid.xa] % 2 == 0 && boundary_opposite) ||
               (source[grid.xa] % 2 != 0 && boundary_equal));
          bool real_zero = real_periodic;
          if (real_zero && source[grid.ya] % (h / period_y) == 0 &&
              source[grid.xa] % (w / period_x) == 0) {
            Ball total{};
            for (unsigned y = 0; y < period_y; ++y)
              for (unsigned x = 0; x < period_x; ++x) {
                fuel.poll(4);
                const auto sign = (y * (source[grid.ya] / (h / period_y)) +
                                   x * (source[grid.xa] / (w / period_x))) &
                                  1;
                total = total + real_template[y * 2 + x].times(sign ? -1 : 1);
              }
            real_zero = total.finite() && total.point() && total.hi == 0;
          }
          const bool y_self =
              source[grid.ya] == 0 || (h % 2 == 0 && source[grid.ya] == h / 2);
          const bool x_self =
              source[grid.xa] == 0 || (w % 2 == 0 && source[grid.xa] == w / 2);
          const bool packed_real_zero =
              real_zero && y_self &&
              (x_self ||
               (packed_y_periodic &&
                imaginary_y_zero[source[grid.ya] == 0 ? 0 : 1]) ||
               (imaginary_x_antiperiodic && source[grid.xa] % 2 == 0));
          const bool excluded =
              packed_real_zero || packed_excluded ||
              (periodic && (source[grid.ya] % (h / period_y) != 0 ||
                            source[grid.xa] % (w / period_x) != 0));
          const auto value = excluded
                                 ? Complex{{0, 0, 0}, {0, 0, 0}}
                                 : data[source[grid.ya] * w + source[grid.xa]];
          const auto v = dd_product(component ? value.im : value.re, scale);
          result = profile == Profile::Strict
                       ? dd_round(v, type == ElementType::Float32)
                       : FastArithmetic{type == ElementType::Float32, profile}
                             .finish(v.bounds());
          if (v.point() && v.hi == 0) {
            const bool boundary_imag =
                kind == Kind::Rfft && component &&
                (source[grid.xa] == 0 ||
                 (w % 2 == 0 && source[grid.xa] == w / 2));
            exact_zero =
                boundary_imag || (kind == Kind::Rfft && component
                                      ? positive_sine_witness
                                      : positive_zero_witness[component]);
            if (exact_zero)
              result = 0;
            else
              result.reset();
          }
        }
        if (!result) {
          const auto value = data[source[grid.ya] * w + source[grid.xa]];
          const auto enclosure = (component ? value.im : value.re).bounds();
          if (!std::isfinite(enclosure.low) || !std::isfinite(enclosure.high) ||
              (enclosure.low <= 0 && enclosure.high >= 0)) {
            find_residual_period();
            const bool outside = source[grid.ya] % (h / residual_py) != 0 ||
                                 source[grid.xa] % (w / residual_px) != 0;
            const bool boundary_imag =
                kind == Kind::Rfft && component &&
                (source[grid.xa] == 0 ||
                 (w % 2 == 0 && source[grid.xa] == w / 2));
            const bool positive =
                boundary_imag || (kind == Kind::Rfft && component
                                      ? positive_sine_witness
                                      : positive_zero_witness[component]);
            if (outside && positive) {
              result = 0;
              exact_zero = true;
            }
          }
          if (!result)
            result = trace::direct(*this, m, reader, source, component, fuel);
        }
        if (conjugate) {
          const auto magnitude = *result & (type == ElementType::Float32
                                                ? UINT64_C(0x7fffffff)
                                                : UINT64_C(0x7fffffffffffffff));
          if (magnitude == 0 && !exact_zero) {
            // Exact zero must stay +0, but underflowed nonzero keeps its sign.
            // Evaluate this coordinate directly when rounded bits cannot tell.
            result = trace::direct(*this, m, reader, at, component, fuel);
          } else if (!exact_zero) {
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
  check(m.exact.status());
  fuel.flush();
}
}  // namespace ps::plugin_internal::frequency
