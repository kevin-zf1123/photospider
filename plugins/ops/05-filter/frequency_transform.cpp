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
#include "05-filter/frequency_fft.hpp"
#include "photospider/core/resource_allocator.hpp"

namespace ps::plugin_internal::frequency {
struct TransformTables {
  TransformCache<float> single;
  TransformCache<double> double_precision;
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
    const TransformBlock load{work, 32 * h * w + 4 * h};
    for (std::uint64_t y = 0; y < h; ++y) {
      auto at = batch;
      at[state.grid.ya] = y;
      TransformRow<Scalar> re{reader, at, 0, state.grid.xa};
      for (std::uint64_t x = 0; x < w / 2; ++x) {
        load.poll(64);
        new (data + y * pitch + x)
            Complex{re.value(2 * x), re.value(2 * x + 1)};
      }
      load.poll(4);
      new (data + y * pitch + w / 2) Complex{};
    }
    return;
  }
  {
    const TransformBlock load{work, 32 * h * width};
    for (std::uint64_t y = 0; y < h; ++y) {
      auto at = batch;
      at[state.grid.ya] = y;
      TransformRow<Scalar> re{reader, at, 0, state.grid.xa};
      TransformRow<Scalar> im{reader, at, 1, state.grid.xa};
      for (std::uint64_t x = 0; x < width; ++x) {
        load.poll(32);
        new (data + y * pitch + x)
            Complex{re.value(x), state.real_input() ? 0 : im.value(x)};
      }
    }
  }
  if (state.kind == Kind::Irfft && !state.compact_real()) {
    // Complete from the loaded half plane, without repeating input traversal.
    const TransformBlock reflect{work, 8 * h * (w - width)};
    for (std::uint64_t y = 0; y < h; ++y)
      for (std::uint64_t x = width; x < w; ++x) {
        reflect.poll(8);
        new (data + y * w + x) Complex{TransformEngine<Scalar>::conjugate(
            data[((h - y) % h) * w + w - x])};
      }
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
  const TransformBlock output{work, 64 * height * width};
  for (std::uint64_t y = y0; y < y0 + height; ++y) {
    auto at = batch;
    at[state.grid.ya] = y;
    at[state.grid.xa] = x0;
    auto* base = static_cast<unsigned char*>(view.allocation) +
                 generation::offset(*view.layout, view.byte_offset, at);
    for (std::uint64_t x = x0; x < x0 + width; ++x) {
      output.poll(64);
      auto value = data[y * pitch + (unpack ? x / 2 : x)];
      if (unpack)
        value.re = x % 2 ? value.im : value.re;
      if (state.kind == Kind::Rfft && (x == 0 || (w % 2 == 0 && x == w / 2))) {
        const auto partner = (h - y) % h;
        if (partner < y)
          value = TransformEngine<Scalar>::conjugate(data[partner * pitch + x]);
        if (partner == y)
          value.im = 0;
      }
      mask::save(
          base, view.layout->strides[state.grid.xa], x - x0,
          static_cast<Scalar>((component ? value.im : value.re) * scale));
    }
  }
}
template <class Scalar>
void run_transform(const Frequency& state, const Kernel& k,
                   const ps_kernel_invocation_v8& inv,
                   ps_kernel_work_v9 service, TransformCache<Scalar>& tables) {
  using Engine = TransformEngine<Scalar>;
  using Complex = TransformComplex<Scalar>;
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
  {
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
    if (state.compact_real() && state.kind == Kind::Irfft) {
      Engine::columns(data, h, pitch, lanes, true, temporary, columns, blue,
                      tables, work);
      Engine::real(data, h, w, true, tables, work);
      Engine::axis(data, row, 1, h, pitch, true, temporary, blue, tables, work);
    } else {
      Engine::axis(data, row, 1, h, pitch, state.inverse(), temporary, blue,
                   tables, work);
      if (state.compact_real())
        Engine::real(data, h, w, false, tables, work);
      Engine::columns(data, h, pitch, lanes, state.inverse(), temporary,
                      columns, blue, tables, work);
    }
    check(work(0));
    for (unsigned i = 0; i < inv.output_count; ++i) {
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
