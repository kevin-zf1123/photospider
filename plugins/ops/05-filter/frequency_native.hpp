// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "05-filter/common.hpp"

namespace ps::plugin_internal::frequency {
using namespace filter;  // NOLINT(build/namespaces)
inline ResultTensorSpec resized(ResultTensorSpec t,
                                const std::vector<std::uint64_t>& shape) {
  for (unsigned a = 0; a < shape.size(); ++a) {
    if (a < t.batch_axes.size()) {
      t.batch_axes[a] = shape[a];
    } else {
      t.descriptor.shape[a - t.batch_axes.size()] = shape[a];
    }
  }
  return t;
}
struct Native : Program {
  unsigned ya = 0, xa = 1;
  ElementType type = ElementType::Float64;
  std::vector<std::uint64_t> dims;
  Region selected;
  std::set<unsigned> members;
  int copy_source(unsigned) const { return -1; }
  void axes(const Parameters& p, const ResultTensorSpec& t) {
    dims = t.sample_shape();
    size_product(dims);
    type = t.descriptor.element_type;
    const auto y = integer(p, "y_axis"), x = integer(p, "x_axis");
    parameter(dims.size() >= 2 && y >= 0 && x >= 0 && y != x &&
                  static_cast<std::uint64_t>(std::max(y, x)) < dims.size(),
              "frequency spatial axes");
    require(floating(type), "frequency requires Float32/Float64");
    ya = y;
    xa = x;
    collective = true;
  }
  Region plane_region(const Region& q) const {
    auto d = q.dimensions();
    d[ya] = {0, dims[ya]};
    d[xa] = {0, dims[xa]};
    return Region(std::move(d));
  }
  template <class Function>
  void planes(Function function) const {
    Coordinate at{};
    std::uint64_t count = 1;
    for (unsigned a = 0; a < dims.size(); ++a) {
      at[a] = selected.dimensions()[a].offset;
      if (a != ya && a != xa) {
        count *= selected.dimensions()[a].extent;
      }
    }
    for (std::uint64_t i = 0; i < count; ++i) {
      function(at);
      for (unsigned a = dims.size(); a-- > 0;) {
        if (a == ya || a == xa) {
          continue;
        }
        if (++at[a] <
            selected.dimensions()[a].offset + selected.dimensions()[a].extent) {
          break;
        }
        at[a] = selected.dimensions()[a].offset;
      }
    }
  }
  template <class Scalar>
  void load(const Reader& reader, unsigned port, unsigned member, Coordinate at,
            Scalar* data, std::uint64_t h, std::uint64_t w,
            const Work& work) const {
    for (std::uint64_t y = 0; y < h; ++y) {
      at[ya] = y;
      for (std::uint64_t x = 0; x < w;) {
        at[xa] = x;
        const auto& v = reader.view(port, member, at);
        const auto n = std::min<std::uint64_t>(
            512,
            std::min(w - x, v.layout->origin[xa] + v.layout->shape[xa] - x));
        check(work(n));
        const auto* base = static_cast<const unsigned char*>(v.allocation) +
                           generation::offset(*v.layout, v.byte_offset, at);
        if (v.layout->strides[xa] == sizeof(Scalar)) {
          std::memcpy(data + y * w + x, base, n * sizeof(Scalar));
        } else {
          for (std::uint64_t j = 0; j < n; ++j) {
            data[y * w + x + j] =
                mask::load<Scalar>(base, v.layout->strides[xa], j);
          }
        }
        x += n;
      }
    }
  }
  template <class Scalar>
  void publish(const Kernel& k, const ps_kernel_invocation_v8& inv,
               unsigned member, Coordinate at, const Scalar* data,
               std::uint64_t w, const Work& work, std::uint64_t y0 = 0,
               std::uint64_t x0 = 0) const {
    for (unsigned o = 0; o < inv.output_count; ++o) {
      if (k.writes[o].second != member) {
        continue;
      }
      const auto& v = inv.outputs[o];
      bool inside = true;
      for (unsigned a = 0; a < dims.size(); ++a) {
        if (a != ya && a != xa) {
          inside &= at[a] >= v.layout->origin[a] &&
                    at[a] - v.layout->origin[a] < v.layout->shape[a];
        }
      }
      if (!inside) {
        continue;
      }
      for (auto y = v.layout->origin[ya];
           y < v.layout->origin[ya] + v.layout->shape[ya]; ++y) {
        at[ya] = y;
        for (std::uint64_t x = 0; x < v.layout->shape[xa];) {
          const auto n = std::min<std::uint64_t>(512, v.layout->shape[xa] - x);
          check(work(n));
          at[xa] = v.layout->origin[xa] + x;
          auto* base = static_cast<unsigned char*>(v.allocation) +
                       generation::offset(*v.layout, v.byte_offset, at);
          if (v.layout->strides[xa] == sizeof(Scalar)) {
            std::memcpy(base, data + (y - y0) * w + at[xa] - x0,
                        n * sizeof(Scalar));
          } else {
            for (std::uint64_t j = 0; j < n; ++j) {
              mask::save(base, v.layout->strides[xa], j,
                         data[(y - y0) * w + at[xa] - x0 + j]);
            }
          }
          x += n;
        }
      }
    }
  }
};
// Compile only selected members, grouped by non-spatial tile coordinates. Each
// invocation owns its scratch and every exported tile has a separate owner.
template <class State>
OperationDefinition native_tasks(OperationDefinition def) {
  using Address = std::tuple<unsigned, unsigned, TileIndex>;
  def.lower = [](const OperationLowering& q) -> Result<OperationProgram> {
    try {
      const auto& s = *static_cast<const State*>(q.static_state);
      OperationProgramBuilder b(q);
      ResourceLimits limits;
      limits.maximum_work = q.limits.maximum_work;
      ResourceBudget budget(limits);
      struct Group {
        Region region;
        std::vector<Address> writes;
      };
      std::map<std::vector<std::uint64_t>, Group> groups;
      for (unsigned p = 0; p < q.requests.size(); ++p) {
        for (const auto& m : q.requests[p].members) {
          const auto grid = take(TileGrid::from_schema(
              *q.outputs[p].result_schema, q.tile_extent, m.first));
          for (auto tile : m.second) {
            check(budget.consume({1}));
            if (s.copy_source(m.first) >= 0) {
              const auto task = take(b.add_copy());
              check(b.read_input(task, 0, s.copy_source(m.first), tile));
              check(b.write_output(task, p, m.first, tile));
              continue;
            }
            const auto r = s.plane_region(take(grid.region(tile)));
            std::vector<std::uint64_t> key;
            for (auto d : r.dimensions()) {
              key.push_back(d.offset);
              key.push_back(d.extent);
            }
            auto& group = groups[key];
            group.region = r;
            group.writes.emplace_back(p, m.first, tile);
          }
        }
      }
      for (const auto& item : groups) {
        auto bound = s;
        bound.selected = item.second.region;
        for (const auto& w : item.second.writes) {
          bound.members.insert(std::get<1>(w));
        }
        const auto task =
            take(b.add_task("generation", "generation-unchecked-v1", {},
                            {bound.task_scratch(0, {}), 0, 0}));
        for (const auto& read : bound.support(0, {})) {
          const auto grid = take(TileGrid::from_schema(
              *q.inputs[read.port].result_schema, q.tile_extent, read.member));
          for (auto tile :
               take(sdk::tiles_for_region(budget, grid, read.region))) {
            check(b.read_input(task, read.port, read.member, tile));
          }
        }
        for (const auto& w : item.second.writes) {
          check(b.write_output(task, std::get<0>(w), std::get<1>(w),
                               std::get<2>(w)));
        }
      }
      return b.finish();
    } catch (const Status& e) {
      return Result<OperationProgram>(e);
    }
  };
  def = generation::budgeted_operation(std::move(def));
  struct Owner {
    std::shared_ptr<State> state;
    ps_prepared_kernel_v9 inner{};
    ~Owner() {
      if (inner.release) {
        inner.release(inner.context);
      }
    }
  };
  const auto prepare = def.prepare_kernel_v9;
  def.prepare_kernel_v9 =
      [prepare](const auto& desc, const auto& placement,
                const KernelPreparation& q, const auto& resources,
                const auto* native) -> Result<ps_prepared_kernel_v9> {
    try {
      require(!q.writes.empty(), "empty frequency invocation");
      auto owner = std::make_unique<Owner>();
      owner->state =
          std::make_shared<State>(*static_cast<const State*>(q.static_state));
      auto& state = *owner->state;
      state.selected =
          state.plane_region(fixed_tensor::view_region(q.writes[0].layout));
      for (const auto& w : q.writes) {
        state.members.insert(w.member);
      }
      state.initialize();
      auto mapped = q;
      mapped.static_state = &state;
      owner->inner = take(prepare(desc, placement, mapped, resources, native));
      auto result = owner->inner;
      result.context = owner.release();
      result.invoke_cpu = [](const ps_kernel_invocation_v9* inv,
                             ps_kernel_status_v8* out) {
        const auto& owner =
            *static_cast<const Owner*>(inv->views.prepared_context);
        auto mapped = *inv;
        mapped.views.prepared_context = owner.inner.context;
        owner.inner.invoke_cpu(&mapped, out);
      };
      result.release = [](const void* p) {
        delete static_cast<const Owner*>(p);
      };
      return Result<ps_prepared_kernel_v9>(result);
    } catch (const Status& e) {
      return Result<ps_prepared_kernel_v9>(e);
    }
  };
  return def;
}
OperationDefinition make_convolution(const std::string&, unsigned);
OperationDefinition make_dct(const std::string&, bool);
OperationDefinition make_wavelet(const std::string&, unsigned, bool);
OperationDefinition make_wavelet_directory(const std::string&, bool);
}  // namespace ps::plugin_internal::frequency
