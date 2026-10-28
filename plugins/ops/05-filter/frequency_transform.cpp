#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "05-filter/frequency.hpp"
#include "05-filter/frequency_fft_execution.hpp"
#include "05-filter/frequency_fft_io.hpp"
#include "photospider/core/resource_allocator.hpp"

namespace ps::plugin_internal::frequency {
template <class Scalar>
struct TypedTransformTables {
  TransformCache<Scalar> base;
#if PS_FRQ_PACKETS
  typename PlannedTransform<Scalar>::Cache plans;
  FixedTransform::Cache fixed;
#endif
};
struct TransformTables {
  TypedTransformTables<float> single;
  TypedTransformTables<double> double_precision;
};
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
// A row cursor resolves a fixed input tile and affine base only when crossing
// its x extent. Scalar loads remain alignment-safe and honor signed strides.
template <class Scalar>
struct TransformRow {
  const Reader& reader;
  Coordinate at;
  unsigned port, axis;
  std::uint64_t begin = 0, end = 0;
  const unsigned char* base = nullptr;
  std::ptrdiff_t stride = 0;
  Scalar value(std::uint64_t x) {
    if (x < begin || x >= end) {
      at[axis] = x;
      const auto& view = reader.view(port, 0, at);
      base = static_cast<const unsigned char*>(view.allocation) +
             generation::offset(*view.layout, view.byte_offset, at);
      begin = x;
      end = view.layout->origin[axis] + view.layout->shape[axis];
      stride = view.layout->strides[axis];
    }
    return mask::load<Scalar>(base, stride, x - begin);
  }
};
#if PS_FRQ_PACKETS
struct TransformSource {
  const unsigned char* base = nullptr;
  std::ptrdiff_t row_stride = 0;
};
template <class Scalar>
TransformSource transform_source(const Frequency& state, const Reader& reader,
                                 unsigned port, const Coordinate& batch,
                                 std::uint64_t height, std::uint64_t width) {
  auto at = batch;
  at[state.grid.ya] = 0;
  at[state.grid.xa] = 0;
  const auto& view = reader.view(port, 0, at);
  const auto& layout = *view.layout;
  if (layout.origin[state.grid.ya] != 0 || layout.origin[state.grid.xa] != 0 ||
      layout.shape[state.grid.ya] < height ||
      layout.shape[state.grid.xa] < width ||
      layout.strides[state.grid.xa] != sizeof(Scalar))
    return {};
  return {static_cast<const unsigned char*>(view.allocation) +
              generation::offset(layout, view.byte_offset, at),
          layout.strides[state.grid.ya]};
}
#endif
template <class Scalar>
void load_transform(const Frequency& state, const Reader& reader,
                    const Coordinate& batch, TransformComplex<Scalar>* data,
                    const Work& work) {
  using Complex = TransformComplex<Scalar>;
  const auto h = state.grid.shape[state.grid.ya];
  const auto w = state.grid.shape[state.grid.xa];
  const auto width = state.kind == Kind::Irfft ? w / 2 + 1 : w;
  const auto pitch = state.working_width();
  if (state.compact_real() && state.real_input()) {
#if PS_FRQ_PACKETS
    const auto direct =
        transform_packets_available()
            ? transform_source<Scalar>(state, reader, 0, batch, h, w)
            : TransformSource{};
#if defined(__x86_64__)
    if (direct.base && h == 1 && w / 2 >= 32768) {
      transform_linear(
          w / 2, 64, work,
          [&](std::uint64_t begin, std::uint64_t count, const Work& local) {
            const TransformBlock load{local, 64 * count};
            TransformIO<Scalar>::pack_real(
                data + begin, direct.base + begin * sizeof(Complex), count,
                load);
          });
      const TransformBlock last{work, 4};
      last.poll(4);
      new (data + w / 2) Complex{};
      return;
    }
#endif
#endif
    transform_rows(
        h, 32 * w + 4, work,
        [&](std::uint64_t begin, std::uint64_t count, const Work& local) {
          const TransformBlock load{local, count * (32 * w + 4)};
#if PS_FRQ_PACKETS
          if (direct.base) {
            for (auto y = begin; y < begin + count; ++y) {
              TransformIO<Scalar>::pack_real(
                  data + y * pitch,
                  direct.base +
                      static_cast<std::ptrdiff_t>(y) * direct.row_stride,
                  w / 2, load);
              load.poll(4);
              new (data + y * pitch + w / 2) Complex{};
            }
            return;
          }
#endif
          // Reader caches tile lookups; each task owns its mutable cache.
          const Reader source{reader.kernel, reader.invocation};
          for (auto y = begin; y < begin + count; ++y) {
            auto at = batch;
            at[state.grid.ya] = y;
            TransformRow<Scalar> re{source, at, 0, state.grid.xa};
            for (std::uint64_t x = 0; x < w / 2; ++x) {
              load.poll(64);
              new (data + y * pitch + x)
                  Complex{re.value(2 * x), re.value(2 * x + 1)};
            }
            load.poll(4);
            new (data + y * pitch + w / 2) Complex{};
          }
        });
    return;
  }
#if PS_FRQ_PACKETS
  TransformSource real, imag;
  if (transform_packets_available()) {
    real = transform_source<Scalar>(state, reader, 0, batch, h, width);
    if (!state.real_input())
      imag = transform_source<Scalar>(state, reader, 1, batch, h, width);
  }
  const bool direct = real.base && (state.real_input() || imag.base);
#if defined(__x86_64__)
  if (direct && pitch == width && h * width >= 32768 &&
      (h == 1 ||
       (width == 1 &&
        real.row_stride == static_cast<std::ptrdiff_t>(sizeof(Scalar)) &&
        (!imag.base ||
         imag.row_stride == static_cast<std::ptrdiff_t>(sizeof(Scalar)))))) {
    transform_linear(
        h * width, 32, work,
        [&](std::uint64_t begin, std::uint64_t count, const Work& local) {
          const TransformBlock load{local, 32 * count};
          TransformIO<Scalar>::pack(
              data + begin, real.base + begin * sizeof(Scalar),
              imag.base ? imag.base + begin * sizeof(Scalar) : nullptr, count,
              load);
        });
    return;
  }
#endif
#endif
  transform_rows(
      h, 32 * width, work,
      [&](std::uint64_t begin, std::uint64_t count, const Work& local) {
        const TransformBlock load{local, 32 * count * width};
#if PS_FRQ_PACKETS
        if (direct) {
          if (width == 1 && pitch == 1 &&
              real.row_stride == static_cast<std::ptrdiff_t>(sizeof(Scalar)) &&
              (!imag.base || imag.row_stride ==
                                 static_cast<std::ptrdiff_t>(sizeof(Scalar)))) {
            TransformIO<Scalar>::pack(
                data + begin, real.base + begin * sizeof(Scalar),
                imag.base ? imag.base + begin * sizeof(Scalar) : nullptr, count,
                load);
            return;
          }
          for (auto y = begin; y < begin + count; ++y)
            TransformIO<Scalar>::pack(
                data + y * pitch,
                real.base + static_cast<std::ptrdiff_t>(y) * real.row_stride,
                imag.base ? imag.base +
                                static_cast<std::ptrdiff_t>(y) * imag.row_stride
                          : nullptr,
                width, load);
          return;
        }
#endif
        const Reader source{reader.kernel, reader.invocation};
        for (auto y = begin; y < begin + count; ++y) {
          auto at = batch;
          at[state.grid.ya] = y;
          TransformRow<Scalar> re{source, at, 0, state.grid.xa};
          TransformRow<Scalar> im{source, at, 1, state.grid.xa};
          for (std::uint64_t x = 0; x < width; ++x) {
            load.poll(32);
            new (data + y * pitch + x)
                Complex{re.value(x), state.real_input() ? 0 : im.value(x)};
          }
        }
      });
  if (state.kind == Kind::Irfft && !state.compact_real()) {
    // The first phase has joined: mirrored reads only address its half plane.
    transform_rows(
        h, 8 * (w - width), work,
        [&](std::uint64_t begin, std::uint64_t count, const Work& local) {
          const TransformBlock reflect{local, 8 * count * (w - width)};
          for (auto y = begin; y < begin + count; ++y)
            for (std::uint64_t x = width; x < w; ++x) {
              reflect.poll(8);
              new (data + y * pitch + x)
                  Complex{TransformEngine<Scalar>::conjugate(
                      data[((h - y) % h) * pitch + w - x])};
            }
        });
  }
}
template <class Scalar>
void store_transform(const Frequency& state,
                     const ps_kernel_mutable_view_v8& view, unsigned component,
                     const Coordinate& batch,
                     const TransformComplex<Scalar>* data, Scalar scale,
                     const Work& work) {
  const auto h = state.grid.shape[state.grid.ya];
  const auto w = state.grid.shape[state.grid.xa];
  const auto pitch = state.working_width();
  const bool unpack = state.kind == Kind::Irfft && state.compact_real();
  const auto y0 = view.layout->origin[state.grid.ya];
  const auto x0 = view.layout->origin[state.grid.xa];
  const auto height = view.layout->shape[state.grid.ya];
  const auto width = view.layout->shape[state.grid.xa];
  auto at = batch;
  at[state.grid.ya] = y0;
  at[state.grid.xa] = x0;
  auto* first = static_cast<unsigned char*>(view.allocation) +
                generation::offset(*view.layout, view.byte_offset, at);
  const auto row_stride = view.layout->strides[state.grid.ya];
#if PS_FRQ_PACKETS
  const bool direct = transform_packets_available() &&
                      view.layout->strides[state.grid.xa] == sizeof(Scalar);
#endif
  transform_rows(
      height, 64 * width, work,
      [&](std::uint64_t begin, std::uint64_t count, const Work& local) {
        const TransformBlock output{local, 64 * count * width};
#if PS_FRQ_PACKETS
        if (direct && width == 1 && pitch == 1 && !unpack &&
            row_stride == static_cast<std::ptrdiff_t>(sizeof(Scalar))) {
          auto* destination = first + begin * sizeof(Scalar);
          if (state.kind == Kind::Rfft) {
            if (component)
              TransformIO<Scalar>::template real_column_component<true>(
                  destination, data, h, y0 + begin, count, scale, output);
            else
              TransformIO<Scalar>::template real_column_component<false>(
                  destination, data, h, y0 + begin, count, scale, output);
            return;
          }
          const auto* source = data + y0 + begin;
          if (component)
            TransformIO<Scalar>::template component<true>(destination, source,
                                                          count, scale, output);
          else
            TransformIO<Scalar>::template component<false>(
                destination, source, count, scale, output);
          return;
        }
#endif
        for (auto y = y0 + begin; y < y0 + begin + count; ++y) {
          auto* base = first + static_cast<std::ptrdiff_t>(y - y0) * row_stride;
          const auto scalar = [&](std::uint64_t x) {
            output.poll(64);
            auto value = data[y * pitch + (unpack ? x / 2 : x)];
            if (unpack)
              value.re = x % 2 ? value.im : value.re;
            if (state.kind == Kind::Rfft &&
                (x == 0 || (w % 2 == 0 && x == w / 2))) {
              const auto partner = (h - y) % h;
              if (partner < y)
                value = TransformEngine<Scalar>::conjugate(
                    data[partner * pitch + x]);
              if (partner == y)
                value.im = 0;
            }
            mask::save(
                base, view.layout->strides[state.grid.xa], x - x0,
                static_cast<Scalar>((component ? value.im : value.re) * scale));
          };
#if PS_FRQ_PACKETS
          if (direct) {
            if (unpack) {
              const auto* source = reinterpret_cast<const unsigned char*>(
                                       data + y * pitch + x0 / 2) +
                                   (x0 % 2) * sizeof(Scalar);
              TransformIO<Scalar>::unpack_real(base, source, width, scale,
                                               output);
            } else {
              auto begin_x = x0, end_x = x0 + width;
              if (state.kind == Kind::Rfft) {
                if (begin_x == 0) {
                  scalar(0);
                  ++begin_x;
                }
                if (w % 2 == 0 && end_x > w / 2)
                  end_x = w / 2;
              }
              if (begin_x < end_x) {
                auto* target = base + (begin_x - x0) * sizeof(Scalar);
                const auto* source = data + y * pitch + begin_x;
                if (component)
                  TransformIO<Scalar>::template component<true>(
                      target, source, end_x - begin_x, scale, output);
                else
                  TransformIO<Scalar>::template component<false>(
                      target, source, end_x - begin_x, scale, output);
              }
              if (state.kind == Kind::Rfft && w % 2 == 0 && x0 <= w / 2 &&
                  x0 + width > w / 2)
                scalar(w / 2);
            }
            continue;
          }
#endif
          for (std::uint64_t x = x0; x < x0 + width; ++x)
            scalar(x);
        }
      });
}
#if PS_FRQ_PACKETS
template <class Scalar>
bool store_transform_pair(const Frequency& state, const Kernel& kernel,
                          const ps_kernel_invocation_v8& inv,
                          const Coordinate& batch,
                          const TransformComplex<Scalar>* data, Scalar scale,
                          const Work& work) {
  if ((state.kind != Kind::Fft && state.kind != Kind::Ifft) ||
      inv.output_count != 2 || !transform_packets_available())
    return false;
  unsigned real_index = 0;
  if (kernel.writes[0].first == 1 && kernel.writes[1].first == 0)
    real_index = 1;
  else if (kernel.writes[0].first != 0 || kernel.writes[1].first != 1)
    return false;
  const auto& re = inv.outputs[real_index];
  const auto& im = inv.outputs[1 - real_index];
  const auto ya = state.grid.ya, xa = state.grid.xa;
  if (re.layout->strides[xa] != sizeof(Scalar) ||
      im.layout->strides[xa] != sizeof(Scalar))
    return false;
  for (unsigned a = 0; a < state.dims.size(); ++a) {
    if (re.layout->origin[a] != im.layout->origin[a] ||
        re.layout->shape[a] != im.layout->shape[a])
      return false;
    if (a != ya && a != xa &&
        (batch[a] < re.layout->origin[a] ||
         batch[a] - re.layout->origin[a] >= re.layout->shape[a]))
      return true;
  }
  const auto y0 = re.layout->origin[ya], x0 = re.layout->origin[xa];
  const auto height = re.layout->shape[ya], width = re.layout->shape[xa];
  auto at = batch;
  at[ya] = y0;
  at[xa] = x0;
  auto* real = static_cast<unsigned char*>(re.allocation) +
               generation::offset(*re.layout, re.byte_offset, at);
  auto* imag = static_cast<unsigned char*>(im.allocation) +
               generation::offset(*im.layout, im.byte_offset, at);
  const auto pitch = state.working_width();
  constexpr auto sample_cost = TransformIO<Scalar>::paired_sample_cost;
  constexpr auto row_cost = TransformIO<Scalar>::paired_row_cost;
  const auto cost_per_row = sample_cost * width + row_cost;
  transform_rows(
      height, cost_per_row, work,
      [&](std::uint64_t begin, std::uint64_t count, const Work& local) {
        const TransformBlock output{local, count * cost_per_row};
        if (width == 1 && pitch == 1 &&
            re.layout->strides[ya] ==
                static_cast<std::ptrdiff_t>(sizeof(Scalar)) &&
            im.layout->strides[ya] ==
                static_cast<std::ptrdiff_t>(sizeof(Scalar))) {
          for (std::uint64_t j = 0; j < count; j += 32) {
            const auto lanes = std::min<std::uint64_t>(32, count - j);
            output.poll(row_cost * lanes);
            TransformIO<Scalar>::components(real + (begin + j) * sizeof(Scalar),
                                            imag + (begin + j) * sizeof(Scalar),
                                            data + y0 + begin + j, lanes, scale,
                                            output);
          }
          return;
        }
        for (auto y = begin; y < begin + count; ++y) {
          output.poll(row_cost);
          TransformIO<Scalar>::components(
              real + static_cast<std::ptrdiff_t>(y) * re.layout->strides[ya],
              imag + static_cast<std::ptrdiff_t>(y) * im.layout->strides[ya],
              data + (y0 + y) * pitch + x0, width, scale, output);
        }
      });
  return true;
}
#endif
template <class Scalar>
void run_transform(const Frequency& state, const Kernel& k,
                   const ps_kernel_invocation_v8& inv,
                   ps_kernel_work_v9 service,
                   TypedTransformTables<Scalar>& prepared) {
  using Engine = TransformEngine<Scalar>;
  using Complex = TransformComplex<Scalar>;
  auto& tables = prepared.base;
  const Work work{inv, state.work_limit, 0, service};
  const Reader reader{k, inv};
  const auto h = state.grid.shape[state.grid.ya],
             w = state.grid.shape[state.grid.xa];
  const auto pitch = state.working_width(), row = state.row_length();
  const auto maximum = std::max(h, row), lanes = state.column_count();
  const auto blue_count =
      std::max(convolution_scratch(h), convolution_scratch(row));
  auto* data = static_cast<Complex*>(inv.scratch[0].allocation);
  auto* temporary = data + h * pitch;
  auto* columns = temporary + maximum;
  auto* blue = columns + state.column_scratch();
  require(reinterpret_cast<unsigned char*>(blue + blue_count) -
                  static_cast<unsigned char*>(inv.scratch[0].allocation) <=
              static_cast<std::ptrdiff_t>(inv.scratch[0].bytes),
          "floating transform scratch capacity");
#if PS_FRQ_PACKETS
  std::optional<TransformExecution<Scalar>> execution;
  if (transform_packets_available()) {
    execution.emplace(h, pitch, row, state.inverse(), prepared.plans, tables,
                      prepared.fixed, work);
    const auto needed =
        h * pitch +
        transform_worker_stride(h, pitch, row, std::is_same_v<Scalar, double>) *
            transform_worker_capacity(h, pitch, row);
    require(needed <= inv.scratch[0].bytes / sizeof(Complex),
            "parallel transform scratch capacity");
  }
  const bool planned = execution.has_value();
#else
  constexpr bool planned = false;
#endif
  if (!planned) {
    const TransformBlock initialize{work, 4 * maximum};
    for (std::uint64_t i = 0; i < maximum; ++i) {
      initialize.poll(4);
      new (temporary + i) Complex{};
    }
  }
  check(work(16));
  const Scalar scale = static_cast<Scalar>(
      state.grid.norm == "ortho" ? 1.0 / std::sqrt(static_cast<double>(h * w))
      : state.inverse()          ? 1.0 / static_cast<double>(h * w)
                                 : 1.0);
  Coordinate batch{};
  std::uint64_t batches = 1;
  for (unsigned a = 0; a < state.dims.size(); ++a) {
    batch[a] = state.selected.dimensions()[a].offset;
    if (a != state.grid.ya && a != state.grid.xa)
      batches *= state.selected.dimensions()[a].extent;
  }
  for (std::uint64_t b = 0; b < batches; ++b) {
    load_transform<Scalar>(state, reader, batch, data, work);
#if PS_FRQ_PACKETS
    if (execution) {
      if (state.compact_real() && state.kind == Kind::Irfft) {
        execution->columns(data, temporary, work);
        execution->real(data, w, tables, work);
        execution->rows(data, temporary, work);
      } else {
        execution->rows(data, temporary, work);
        if (state.compact_real())
          execution->real(data, w, tables, work);
        execution->columns(data, temporary, work);
      }
    } else {
#endif
      if (state.compact_real() && state.kind == Kind::Irfft) {
        Engine::columns(data, h, pitch, lanes, true, temporary, columns, blue,
                        tables, work);
        Engine::real(data, h, w, true, tables, work);
        Engine::axis(data, row, 1, h, pitch, true, temporary, blue, tables,
                     work);
      } else {
        Engine::axis(data, row, 1, h, pitch, state.inverse(), temporary, blue,
                     tables, work);
        if (state.compact_real())
          Engine::real(data, h, w, false, tables, work);
        Engine::columns(data, h, pitch, lanes, state.inverse(), temporary,
                        columns, blue, tables, work);
      }
#if PS_FRQ_PACKETS
    }
#endif
    check(work(0));
    bool paired = false;
#if PS_FRQ_PACKETS
    paired = store_transform_pair(state, k, inv, batch, data, scale, work);
#endif
    for (unsigned i = 0; !paired && i < inv.output_count; ++i) {
      const auto& view = inv.outputs[i];
      bool contains = true;
      for (unsigned a = 0; a < state.dims.size(); ++a)
        if (a != state.grid.ya && a != state.grid.xa)
          contains &= batch[a] >= view.layout->origin[a] &&
                      batch[a] - view.layout->origin[a] < view.layout->shape[a];
      if (!contains)
        continue;
      store_transform<Scalar>(state, view, k.writes[i].first, batch, data,
                              scale, work);
    }
    check(work(0));
    for (unsigned a = static_cast<unsigned>(state.dims.size()); a-- > 0;) {
      if (a == state.grid.ya || a == state.grid.xa)
        continue;
      if (++batch[a] < state.selected.dimensions()[a].offset +
                           state.selected.dimensions()[a].extent)
        break;
      batch[a] = state.selected.dimensions()[a].offset;
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
  if (type == ElementType::Float32)
    frequency::run_transform<float>(*this, k, inv, service, tables->single);
  else
    frequency::run_transform<double>(*this, k, inv, service,
                                     tables->double_precision);
}
}  // namespace ps::plugin_internal::frequency
