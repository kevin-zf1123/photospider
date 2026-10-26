#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

#include "05-filter/frequency.hpp"
#include "05-filter/frequency_work.hpp"
#include "photospider/core/resource_allocator.hpp"

namespace ps::plugin_internal::frequency {
struct TransformComplex {
  double re = 0, im = 0;
};
static_assert(sizeof(TransformComplex) == 2 * sizeof(double));
// Prepared owners capture the Context allocator. Published dimension-derived
// tables are immutable; neither payloads nor execution ledgers are retained.
struct TransformTables {
  using Roots =
      std::vector<TransformComplex, ResourceAllocator<TransformComplex>>;
  using Key = std::pair<std::uint64_t, bool>;
  using Entry = std::pair<const Key, Roots>;
  std::mutex mutex;
  ResourceAllocator<TransformComplex> allocator;
  std::map<Key, Roots, std::less<Key>, ResourceAllocator<Entry>> roots;
};
namespace {
using Complex = TransformComplex;
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
Complex add(Complex a, Complex b) {
  return {a.re + b.re, a.im + b.im};
}
Complex minus(Complex a, Complex b) {
  return {a.re - b.re, a.im - b.im};
}
Complex product(Complex a, Complex b) {
  return {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}
Complex conjugate(Complex a) {
  return {a.re, -a.im};
}
Complex root(std::uint64_t k, std::uint64_t n, bool inverse) {
  k %= n;
  if ((4 * k) % n == 0) {
    const int c[4] = {1, 0, -1, 0}, s[4] = {0, 1, 0, -1};
    const auto q = 4 * k / n;
    return {static_cast<double>(c[q]),
            static_cast<double>(inverse ? s[q] : -s[q])};
  }
  // Reduce to the nearest quarter turn before calling the native libm.
  const auto q = (4 * k + n / 2) / n;
  const auto numerator =
      static_cast<std::int64_t>(4 * k) - static_cast<std::int64_t>(q * n);
  constexpr double half_pi = 0x1.921fb54442d18p+0;
  const auto angle = half_pi * (static_cast<double>(numerator) / n);
  const auto c = std::cos(angle), s = std::sin(angle);
  Complex value;
  switch (q & 3) {
    case 0:
      value = {c, s};
      break;
    case 1:
      value = {-s, c};
      break;
    case 2:
      value = {-c, -s};
      break;
    default:
      value = {s, -c};
      break;
  }
  return inverse ? value : conjugate(value);
}
const TransformTables::Roots& root_table(TransformTables& tables,
                                         std::uint64_t n, bool inverse,
                                         const Work& work) {
  check(work(1));
  const auto key = std::make_pair(n, inverse);
  {
    std::lock_guard<std::mutex> lock(tables.mutex);
    const auto found = tables.roots.find(key);
    if (found != tables.roots.end())
      return found->second;
  }
  TransformTables::Roots values(tables.allocator);
  const TransformBlock build{work, 128 * n};
  values.reserve(n);
  for (std::uint64_t i = 0; i < n; ++i) {
    build.poll(128);
    values.push_back(root(i, n, inverse));
  }
  check(work(0));
  std::lock_guard<std::mutex> lock(tables.mutex);
  return tables.roots.emplace(key, std::move(values)).first->second;
}
void fft_line(Complex* data, std::uint64_t n, std::uint64_t stride,
              const TransformTables::Roots& roots, bool inverse,
              const Work& work) {
  unsigned levels = 0;
  for (auto v = n; v > 1; v >>= 1)
    ++levels;
  // Bit reversal costs at most 2*levels+8 per visit. Radix-two and radix-four
  // costs include all scalar arithmetic, addressing and loop control.
  const TransformBlock fuel{work, n * (2 * levels + 8) +
                                      (levels & 1 ? n / 2 * 32 : 0) +
                                      (levels / 2) * (n / 4) * 128};
  for (std::uint64_t i = 0, j = 0; i < n; ++i) {
    fuel.poll(2 * levels + 8);
    if (i < j)
      std::swap(data[i * stride], data[j * stride]);
    if (i + 1 != n) {
      auto bit = n / 2;
      while (j & bit) {
        j ^= bit;
        bit >>= 1;
      }
      j ^= bit;
    }
  }
  std::uint64_t length = 1;
  if (levels & 1) {
    for (std::uint64_t i = 0; i < n; i += 2) {
      fuel.poll(32);
      const auto a = data[i * stride], b = data[(i + 1) * stride];
      data[i * stride] = add(a, b);
      data[(i + 1) * stride] = minus(a, b);
    }
    length = 2;
  }
  for (; length < n; length *= 4) {
    const auto size = 4 * length;
    for (std::uint64_t i = 0; i < n; i += size)
      for (std::uint64_t j = 0; j < length; ++j) {
        fuel.poll(128);
        const auto phase = j * (n / size);
        const auto twiddle = [&](Complex v, std::uint64_t p) {
          return p ? product(v, roots[p]) : v;
        };
        const auto a = data[(i + j) * stride];
        // Binary bit reversal orders the two middle radix-four inputs 2,1.
        const auto c = twiddle(data[(i + j + length) * stride], 2 * phase);
        const auto b = twiddle(data[(i + j + 2 * length) * stride], phase);
        const auto d = twiddle(data[(i + j + 3 * length) * stride], 3 * phase);
        const auto ac = add(a, c), bd = add(b, d);
        const auto amc = minus(a, c), bmd = minus(b, d);
        const Complex rotated =
            inverse ? Complex{-bmd.im, bmd.re} : Complex{bmd.im, -bmd.re};
        data[(i + j) * stride] = add(ac, bd);
        data[(i + j + length) * stride] = add(amc, rotated);
        data[(i + j + 2 * length) * stride] = minus(ac, bd);
        data[(i + j + 3 * length) * stride] = minus(amc, rotated);
      }
  }
  check(work(0));
}
std::uint64_t radix_of(std::uint64_t n) {
  for (auto r : {2U, 3U, 5U})
    if (n % r == 0)
      return r;
  return n;
}
std::uint64_t mixed_cost(std::uint64_t n) {
  if (n == 1)
    return 0;
  const auto r = radix_of(n), m = n / r;
  return r * mixed_cost(m) + m * r * (32 + 32 * r) + 4 * n;
}
void mixed_body(Complex* data, std::uint64_t n, std::uint64_t stride,
                const TransformTables::Roots& roots, Complex* temporary,
                const TransformBlock& fuel) {
  if (n == 1)
    return;
  const auto r = radix_of(n), m = n / r;
  for (std::uint64_t j = 0; j < r; ++j)
    mixed_body(data + j * stride, m, stride * r, roots, temporary, fuel);
  const auto order = roots.size();
  for (std::uint64_t k = 0; k < m; ++k) {
    Complex v[31];
    for (std::uint64_t j = 0; j < r; ++j) {
      fuel.poll(32);
      const auto phase = j * k * (order / n);
      v[j] = phase ? product(data[(j + r * k) * stride], roots[phase])
                   : data[(j + r * k) * stride];
    }
    for (std::uint64_t q = 0; q < r; ++q) {
      Complex sum{};
      for (std::uint64_t j = 0; j < r; ++j) {
        fuel.poll(32);
        const auto phase = (q * j * (order / r)) % order;
        sum = add(sum, phase ? product(v[j], roots[phase]) : v[j]);
      }
      temporary[k + q * m] = sum;
    }
  }
  for (std::uint64_t i = 0; i < n; ++i) {
    fuel.poll(4);
    data[i * stride] = temporary[i];
  }
}
void transform_axis(Complex* data, std::uint64_t n, std::uint64_t stride,
                    std::uint64_t lines, std::uint64_t step, bool inverse,
                    Complex* temporary, Complex* blue, TransformTables& tables,
                    const Work& work) {
  const auto size = bluestein_size(n);
  if (!size) {
    const auto& roots = root_table(tables, n, inverse, work);
    for (std::uint64_t l = 0; l < lines; ++l) {
      if (power2(n)) {
        fft_line(data + l * step, n, stride, roots, inverse, work);
      } else {
        const TransformBlock fuel{work, mixed_cost(n)};
        mixed_body(data + l * step, n, stride, roots, temporary, fuel);
        check(work(0));
      }
    }
    return;
  }
  // Arbitrary lengths use a power-of-two convolution; no direct quadratic
  // transform or value-dependent numerical fallback is used.
  const auto& roots = root_table(tables, size, false, work);
  auto* chirp = blue;
  auto* kernel = chirp + n;
  auto* line = kernel + size;
  {
    const TransformBlock fill{work, 4 * size + 160 * n};
    for (std::uint64_t i = 0; i < size; ++i) {
      fill.poll(4);
      new (kernel + i) Complex{};
    }
    for (std::uint64_t i = 0; i < n; ++i) {
      fill.poll(160);
      const auto phase = static_cast<std::uint64_t>(
          static_cast<unsigned __int128>(i) * i % (2 * n));
      new (chirp + i) Complex{root(phase, 2 * n, inverse)};
      kernel[i] = conjugate(chirp[i]);
      if (i)
        kernel[size - i] = kernel[i];
    }
  }
  fft_line(kernel, size, 1, roots, false, work);
  for (std::uint64_t l = 0; l < lines; ++l) {
    auto* input = data + l * step;
    {
      const TransformBlock fill{work, 32 * size};
      for (std::uint64_t i = 0; i < size; ++i) {
        fill.poll(32);
        new (line + i)
            Complex{i < n ? product(input[i * stride], chirp[i]) : Complex{}};
      }
    }
    fft_line(line, size, 1, roots, false, work);
    {
      const TransformBlock multiply{work, 32 * size};
      for (std::uint64_t i = 0; i < size; ++i) {
        multiply.poll(32);
        line[i] = conjugate(product(line[i], kernel[i]));
      }
    }
    fft_line(line, size, 1, roots, false, work);
    {
      const TransformBlock scatter{work, 32 * n};
      const auto scale = 1.0 / size;
      for (std::uint64_t i = 0; i < n; ++i) {
        scatter.poll(32);
        input[i * stride] =
            product({line[i].re * scale, -line[i].im * scale}, chirp[i]);
      }
    }
  }
  check(work(0));
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
      require(desc.symbol == "generation" &&
                  desc.contract == "generation-unchecked-v1",
              "unsupported frequency transform kernel");
      require(!q.writes.empty(), "empty budgeted transform invocation");
      auto owner = std::make_unique<BudgetOwner>();
      owner->state = std::make_shared<Frequency>(
          *static_cast<const Frequency*>(q.static_state));
      owner->state->tables = std::allocate_shared<TransformTables>(
          ResourceAllocator<TransformTables>{});
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
  const Reader reader{k, inv};
  const auto h = grid.shape[grid.ya], w = grid.shape[grid.xa];
  const auto maximum = std::max(h, w), lanes = column_count();
  const auto blue_size = std::max(bluestein_size(h), bluestein_size(w));
  auto* data = static_cast<Complex*>(inv.scratch[0].allocation);
  auto* temporary = data + h * w;
  auto* columns = temporary + maximum;
  auto* blue = columns + h * lanes;
  const auto blue_count = blue_size ? maximum + 2 * blue_size : 0;
  require(reinterpret_cast<unsigned char*>(blue + blue_count) -
                  static_cast<unsigned char*>(inv.scratch[0].allocation) <=
              static_cast<std::ptrdiff_t>(inv.scratch[0].bytes),
          "floating transform scratch capacity");
  {
    const TransformBlock initialize{work, 4 * maximum};
    for (std::uint64_t i = 0; i < maximum; ++i) {
      initialize.poll(4);
      new (temporary + i) Complex{};
    }
  }
  check(work(16));
  const auto scale = grid.norm == "ortho"
                         ? 1.0 / std::sqrt(static_cast<double>(h * w))
                     : inverse() ? 1.0 / static_cast<double>(h * w)
                                 : 1.0;
  Coordinate batch{};
  std::uint64_t batches = 1;
  for (unsigned a = 0; a < dims.size(); ++a) {
    batch[a] = selected.dimensions()[a].offset;
    if (a != grid.ya && a != grid.xa)
      batches *= selected.dimensions()[a].extent;
  }
  for (std::uint64_t b = 0; b < batches; ++b) {
    {
      const TransformBlock load{work, 32 * h * w};
      for (std::uint64_t y = 0; y < h; ++y)
        for (std::uint64_t x = 0; x < w; ++x) {
          load.poll(32);
          auto at = batch;
          const bool reflected = kind == Kind::Irfft && x > w / 2;
          at[grid.ya] = reflected ? (h - y) % h : y;
          at[grid.xa] = reflected ? w - x : x;
          const auto re = reader.value(0, 0, at);
          const auto im = real_input() ? 0 : reader.value(1, 0, at);
          new (data + y * w + x) Complex{re, reflected ? -im : im};
        }
    }
    transform_axis(data, w, 1, h, w, inverse(), temporary, blue, *tables, work);
    if (lanes) {
      for (std::uint64_t x0 = 0; x0 < w; x0 += lanes) {
        const auto count = std::min(lanes, w - x0);
        {
          const TransformBlock gather{work, 4 * h * count};
          for (std::uint64_t y = 0; y < h; ++y)
            for (std::uint64_t x = 0; x < count; ++x) {
              gather.poll(4);
              new (columns + x * h + y) Complex{data[y * w + x0 + x]};
            }
        }
        transform_axis(columns, h, 1, count, h, inverse(), temporary, blue,
                       *tables, work);
        const TransformBlock scatter{work, 4 * h * count};
        for (std::uint64_t y = 0; y < h; ++y)
          for (std::uint64_t x = 0; x < count; ++x) {
            scatter.poll(4);
            data[y * w + x0 + x] = columns[x * h + y];
          }
      }
    } else {
      transform_axis(data, h, w, w, 1, inverse(), temporary, blue, *tables,
                     work);
    }
    check(work(0));
    for (unsigned i = 0; i < inv.output_count; ++i) {
      const auto& view = inv.outputs[i];
      bool contains = true;
      for (unsigned a = 0; a < dims.size(); ++a)
        if (a != grid.ya && a != grid.xa)
          contains &= batch[a] >= view.layout->origin[a] &&
                      batch[a] - view.layout->origin[a] < view.layout->shape[a];
      if (!contains)
        continue;
      const auto count =
          view.layout->shape[grid.ya] * view.layout->shape[grid.xa];
      const TransformBlock output{work, 64 * count};
      const auto component = k.writes[i].first;
      for (std::uint64_t y = view.layout->origin[grid.ya];
           y < view.layout->origin[grid.ya] + view.layout->shape[grid.ya]; ++y)
        for (std::uint64_t x = view.layout->origin[grid.xa];
             x < view.layout->origin[grid.xa] + view.layout->shape[grid.xa];
             ++x) {
          output.poll(64);
          auto value = data[y * w + x];
          if (kind == Kind::Rfft && (x == 0 || (w % 2 == 0 && x == w / 2))) {
            const auto partner = (h - y) % h;
            // Construct a conjugate boundary pair from one representative;
            // this is output packing, not a numerical validation pass.
            if (partner < y)
              value = conjugate(data[partner * w + x]);
            if (partner == y)
              value.im = 0;
          }
          auto at = batch;
          at[grid.ya] = y;
          at[grid.xa] = x;
          store(view, at,
                generation::convert((component ? value.im : value.re) * scale,
                                    type == ElementType::Float32));
        }
    }
    check(work(0));
    for (unsigned a = static_cast<unsigned>(dims.size()); a-- > 0;) {
      if (a == grid.ya || a == grid.xa)
        continue;
      if (++batch[a] <
          selected.dimensions()[a].offset + selected.dimensions()[a].extent)
        break;
      batch[a] = selected.dimensions()[a].offset;
    }
  }
}
}  // namespace ps::plugin_internal::frequency
