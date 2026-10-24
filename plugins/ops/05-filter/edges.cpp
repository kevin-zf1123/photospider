#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "05-filter/local_windows.hpp"

namespace ps::plugin_internal::filter {
namespace {
Geometry edge_geometry(const OperationMetadata& in, const Parameters& p,
                       bool byte) {
  auto t = tensor(in, true);
  require(byte ? t.descriptor.element_type == ElementType::UInt8
               : floating(t.descriptor.element_type),
          "edge input dtype");
  t.descriptor.element_type = ElementType::Float64;
  auto args = p;
  args["anchor_y"] = static_cast<std::int64_t>(0);
  args["anchor_x"] = static_cast<std::int64_t>(0);
  args["boundary"] = std::string("truncate");
  return Geometry(t, args, 1, 1, Kind::BoxMean);
}
struct Hysteresis final : Program {
  Geometry geometry;
  std::int64_t connectivity;
  Region selected;
  Hysteresis(const std::vector<OperationMetadata>& in, const Parameters& p)
      : geometry(edge_geometry(in[0], p, true)),
        connectivity(integer(p, "connectivity")) {
    const auto& b = tensor(in[1], true);
    require(b.descriptor.element_type == ElementType::UInt8 &&
                b.sample_shape() == geometry.dims,
            "edge mask shape/dtype");
    parameter(connectivity == 4 || connectivity == 8, "edge connectivity");
    collective = true;
    scratch_bytes =
        bytes(static_cast<unsigned __int128>(geometry.h) * geometry.w *
                  (geometry.h * geometry.w <= UINT32_MAX ? 5 : 9) +
              64);
    add_output(this, field_schema(in[0], ElementType::UInt8));
  }
  Region plane_region(const Region& q) const {
    auto d = q.dimensions();
    d[geometry.ya] = {0, geometry.h};
    d[geometry.xa] = {0, geometry.w};
    return Region(std::move(d));
  }
  std::vector<ReadRegion> support(unsigned, const Region&) const override {
    return {{0, 0, selected}, {1, 0, selected}};
  }
  template <class Index>
  void run_queue(const Kernel& kernel,
                 const ps_kernel_invocation_v8& inv) const {
    const Work work{inv, work_limit};
    const Fuel fuel{work};
    const Reader reader{kernel, inv};
    const auto& g = geometry;
    const auto count = g.h * g.w;
    auto* queue = static_cast<Index*>(inv.scratch[0].allocation);
    auto* state = reinterpret_cast<std::uint8_t*>(queue + count);
    each_plane(inv.outputs[0], g, [&](Coordinate plane) {
      std::uint64_t head = 0, tail = 0;
      for (std::uint64_t y = 0; y < g.h; ++y) {
        plane[g.ya] = y;
        for (std::uint64_t x = 0; x < g.w;) {
          plane[g.xa] = x;
          const auto& weak = reader.view(0, 0, plane);
          const auto& strong = reader.view(1, 0, plane);
          const auto end = std::min(
              weak.layout->origin[g.xa] + weak.layout->shape[g.xa],
              strong.layout->origin[g.xa] + strong.layout->shape[g.xa]);
          const auto* wp =
              static_cast<const std::uint8_t*>(weak.allocation) +
              generation::offset(*weak.layout, weak.byte_offset, plane);
          const auto* sp =
              static_cast<const std::uint8_t*>(strong.allocation) +
              generation::offset(*strong.layout, strong.byte_offset, plane);
          if (weak.layout->strides[g.xa] == 1 &&
              strong.layout->strides[g.xa] == 1) {
            while (end - x >= 8) {
              fuel.poll(16);
              std::uint64_t seeds;
              std::memcpy(&seeds, sp, sizeof(seeds));
              if (!seeds) {
                std::memcpy(state + y * g.w + x, wp, 8);
              } else {
                for (unsigned k = 0; k < 8; ++k) {
                  const auto i = y * g.w + x + k;
                  state[i] = sp[k] ? 2 : wp[k] ? 1 : 0;
                  if (sp[k])
                    queue[tail++] = static_cast<Index>(i);
                }
              }
              x += 8;
              wp += 8;
              sp += 8;
            }
          }
          for (; x < end; ++x, wp += weak.layout->strides[g.xa],
                          sp += strong.layout->strides[g.xa]) {
            fuel.poll(2);
            const auto i = y * g.w + x;
            state[i] = *wp ? 1 : 0;
            if (*sp) {
              state[i] = 2;
              queue[tail++] = static_cast<Index>(i);
            }
          }
        }
      }
      while (head < tail) {
        const std::uint64_t i = queue[head++], y = i / g.w, x = i % g.w;
        fuel.poll(connectivity);
        const auto visit = [&](std::uint64_t j) {
          if (state[j] == 1) {
            state[j] = 2;
            queue[tail++] = static_cast<Index>(j);
          }
        };
        if (x)
          visit(i - 1);
        if (x + 1 < g.w)
          visit(i + 1);
        if (y) {
          visit(i - g.w);
          if (connectivity == 8) {
            if (x)
              visit(i - g.w - 1);
            if (x + 1 < g.w)
              visit(i - g.w + 1);
          }
        }
        if (y + 1 < g.h) {
          visit(i + g.w);
          if (connectivity == 8) {
            if (x)
              visit(i + g.w - 1);
            if (x + 1 < g.w)
              visit(i + g.w + 1);
          }
        }
      }
      for (unsigned o = 0; o < inv.output_count; ++o) {
        const auto& v = inv.outputs[o];
        for (std::uint64_t y = v.layout->origin[g.ya];
             y < v.layout->origin[g.ya] + v.layout->shape[g.ya]; ++y) {
          plane[g.ya] = y;
          for (std::uint64_t x = v.layout->origin[g.xa];
               x < v.layout->origin[g.xa] + v.layout->shape[g.xa]; ++x) {
            fuel.poll();
            plane[g.xa] = x;
            store(v, plane, state[y * g.w + x] == 2 ? 1 : 0);
          }
        }
      }
    });
    fuel.flush();
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    if (geometry.h * geometry.w <= UINT32_MAX)
      run_queue<std::uint32_t>(kernel, inv);
    else
      run_queue<std::uint64_t>(kernel, inv);
  }
};
std::vector<std::uint64_t> region_key(const Region& r) {
  std::vector<std::uint64_t> key;
  for (const auto& d : r.dimensions()) {
    key.push_back(d.offset);
    key.push_back(d.extent);
  }
  return key;
}
Result<OperationProgram> lower_edges(const OperationLowering& q) {
  try {
    const auto& s = *static_cast<const Hysteresis*>(q.static_state);
    OperationProgramBuilder b(q);
    ResourceLimits limits;
    limits.maximum_work = q.limits.maximum_work;
    ResourceBudget budget(limits);
    const auto grid =
        take(TileGrid::from_schema(*q.outputs[0].result_schema, q.tile_extent));
    std::map<std::vector<std::uint64_t>,
             std::pair<Region, std::vector<TileIndex>>>
        groups;
    for (const auto& m : q.requests[0].members)
      for (auto tile : m.second) {
        check(budget.consume({1}));
        const auto region = s.plane_region(take(grid.region(tile)));
        auto& group = groups[region_key(region)];
        group.first = region;
        group.second.push_back(tile);
      }
    for (const auto& entry : groups) {
      const auto task = take(b.add_task("generation", "generation-unchecked-v1",
                                        {}, {s.scratch_bytes, 0, 0}));
      for (unsigned p = 0; p < 2; ++p) {
        const auto in = take(
            TileGrid::from_schema(*q.inputs[p].result_schema, q.tile_extent));
        for (auto tile :
             take(sdk::tiles_for_region(budget, in, entry.second.first)))
          check(b.read_input(task, p, 0, tile));
      }
      for (auto tile : entry.second.second)
        check(b.write_output(task, 0, 0, tile));
    }
    return b.finish();
  } catch (const Status& e) {
    return Result<OperationProgram>(e);
  }
}
struct BoundKernel {
  std::shared_ptr<Hysteresis> state;
  ps_prepared_kernel_v8 inner{};
  ~BoundKernel() {
    if (inner.release)
      inner.release(inner.context);
  }
};
struct Nms final : Program {
  Geometry geometry;
  ElementType type;
  double low, high;
  Nms(const std::vector<OperationMetadata>& in, const Parameters& p)
      : geometry(edge_geometry(in[0], p, false)),
        type(tensor(in[0], true).descriptor.element_type),
        low(real(p, "low")),
        high(real(p, "high")) {
    for (unsigned i = 1; i < 3; ++i)
      require(tensor(in[i], true).sample_shape() == geometry.dims &&
                  tensor(in[i], true).descriptor.element_type == type,
              "NMS input shape/dtype");
    add_output(this, field_schema(in[0], ElementType::UInt8));
    add_output(this, field_schema(in[0], ElementType::UInt8));
  }
  std::vector<ReadRegion> support(unsigned, const Region& q) const override {
    auto g = geometry;
    g.kh = g.kw = 3;
    g.ay = g.ax = 1;
    std::vector<ReadRegion> result{{0, 0, q}, {1, 0, q}};
    for (const auto& r : g.support(q))
      result.push_back({2, 0, r});
    return result;
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    const Fuel fuel{work};
    const Reader r{kernel, inv};
    const auto& g = geometry;
    const double threshold = kernel.writes[0].first ? high : low;
    samples(inv.outputs[0], [&](Coordinate at, std::uint64_t) {
      fuel.poll(3);
      const double gx = r.value(0, 0, at), gy = r.value(1, 0, at),
                   value = r.value(2, 0, at);
      int dx = 1, dy = 0;
      if (std::isnan(gx) || std::isnan(gy) || (gx == 0 && gy == 0)) {
        store(inv.outputs[0], at, 0);
        return;
      }
      if (std::abs(gx) >= 2 * std::abs(gy)) {
        dx = 1;
        dy = 0;
      } else if (std::abs(gy) >= 2 * std::abs(gx)) {
        dx = 0;
        dy = 1;
      } else {
        dy = 1;
        dx = std::signbit(gx) == std::signbit(gy) ? 1 : -1;
      }
      const auto neighbor = [&](int sign) -> double {
        fuel.poll();
        const auto y = static_cast<std::int64_t>(at[g.ya]) + sign * dy;
        const auto x = static_cast<std::int64_t>(at[g.xa]) + sign * dx;
        if (y < 0 || x < 0 || static_cast<std::uint64_t>(y) >= g.h ||
            static_cast<std::uint64_t>(x) >= g.w)
          return INFINITY;
        auto p = at;
        p[g.ya] = y;
        p[g.xa] = x;
        return r.value(2, 0, p);
      };
      const auto positive = neighbor(1), negative = neighbor(-1);
      store(
          inv.outputs[0], at,
          value >= positive && value > negative && value >= threshold ? 1 : 0);
    });
    fuel.flush();
  }
};
}  // namespace
OperationDefinition make_hysteresis() {
  auto result = generation::operation(
      "filter.hysteresis_edges_strict", 2,
      {{"y_axis", Type::Int64},
       {"x_axis", Type::Int64},
       {"connectivity", Type::Int64}},
      {{"edges", tensor_schema(ElementType::UInt8, {1, 1})}},
      [](const auto& in, const auto& p) {
        return std::make_shared<Hysteresis>(in, p);
      });
  result.lower = lower_edges;
  const auto prepare = result.prepare_kernel;
  result.prepare_kernel =
      [prepare](const auto& desc, const auto& placement,
                const KernelPreparation& query, const auto& resources,
                const auto* native) -> Result<ps_prepared_kernel_v8> {
    try {
      require(!query.writes.empty(), "empty edge invocation");
      const auto& original =
          *static_cast<const Hysteresis*>(query.static_state);
      auto state = std::make_shared<Hysteresis>(original);
      state->selected = state->plane_region(
          fixed_tensor::view_region(query.writes[0].layout));
      for (const auto& write : query.writes)
        require(region_key(state->selected) ==
                    region_key(state->plane_region(
                        fixed_tensor::view_region(write.layout))),
                "edge plane group mismatch");
      auto mapped = query;
      mapped.static_state = state.get();
      auto owner = std::make_unique<BoundKernel>();
      owner->state = std::move(state);
      owner->inner = take(prepare(desc, placement, mapped, resources, native));
      ps_prepared_kernel_v8 prepared = owner->inner;
      prepared.context = owner.release();
      prepared.invoke_cpu = [](const ps_kernel_invocation_v8* inv,
                               ps_kernel_status_v8* out) {
        const auto& owner =
            *static_cast<const BoundKernel*>(inv->prepared_context);
        auto mapped = *inv;
        mapped.prepared_context = owner.inner.context;
        owner.inner.invoke_cpu(&mapped, out);
      };
      prepared.release = [](const void* context) {
        delete static_cast<const BoundKernel*>(context);
      };
      return Result<ps_prepared_kernel_v8>(prepared);
    } catch (const Status& e) {
      return Result<ps_prepared_kernel_v8>(e);
    }
  };
  return result;
}
OperationDefinition make_nms() {
  return generation::operation(
      "filter.canny_nms_quantized4_strict", 3,
      {{"y_axis", Type::Int64},
       {"x_axis", Type::Int64},
       {"low", Type::Float64},
       {"high", Type::Float64}},
      {{"weak", tensor_schema(ElementType::UInt8, {1, 1})},
       {"strong", tensor_schema(ElementType::UInt8, {1, 1})}},
      [](const auto& in, const auto& p) {
        return std::make_shared<Nms>(in, p);
      });
}
}  // namespace ps::plugin_internal::filter
