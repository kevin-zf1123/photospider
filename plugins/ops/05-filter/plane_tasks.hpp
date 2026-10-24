#pragma once

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "05-filter/common.hpp"

namespace ps::plugin_internal::filter {
inline std::vector<std::uint64_t> plane_key(const Region& r) {
  std::vector<std::uint64_t> key;
  for (const auto& d : r.dimensions()) {
    key.push_back(d.offset);
    key.push_back(d.extent);
  }
  return key;
}
// Group retained tiles by their complete spatial plane, retaining the frozen
// generation invocation interface and independent output allocations.
template <class State>
OperationDefinition planar_operation(OperationDefinition def) {
  def.lower = [](const OperationLowering& q) -> Result<OperationProgram> {
    try {
      const auto& s = *static_cast<const State*>(q.static_state);
      OperationProgramBuilder builder(q);
      ResourceLimits limits;
      limits.maximum_work = q.limits.maximum_work;
      ResourceBudget budget(limits);
      struct Group {
        Region region;
        std::vector<TileIndex> writes;
      };
      std::map<std::vector<std::uint64_t>, Group> groups;
      const auto grid = take(
          TileGrid::from_schema(*q.outputs[0].result_schema, q.tile_extent, 0));
      for (const auto& member : q.requests[0].members)
        for (auto tile : member.second) {
          check(budget.consume({1}));
          if (s.identity()) {
            const auto task = take(builder.add_copy());
            check(builder.read_input(task, 0, 0, tile));
            check(builder.write_output(task, 0, 0, tile));
          } else {
            const auto region = s.plane_region(take(grid.region(tile)));
            auto& group = groups[plane_key(region)];
            group.region = region;
            group.writes.push_back(tile);
          }
        }
      for (const auto& item : groups) {
        auto bound = s;
        bound.selected = item.second.region;
        const auto task =
            take(builder.add_task("generation", "generation-unchecked-v1", {},
                                  {bound.task_scratch(0, {}), 0, 0}));
        for (const auto& read : bound.support(0, {})) {
          const auto input = take(TileGrid::from_schema(
              *q.inputs[read.port].result_schema, q.tile_extent, read.member));
          for (auto tile :
               take(sdk::tiles_for_region(budget, input, read.region)))
            check(builder.read_input(task, read.port, read.member, tile));
        }
        for (auto tile : item.second.writes)
          check(builder.write_output(task, 0, 0, tile));
      }
      return builder.finish();
    } catch (const Status& e) {
      return Result<OperationProgram>(e);
    }
  };
  struct Owner {
    std::shared_ptr<State> state;
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
      require(!q.writes.empty(), "empty planar invocation");
      auto state =
          std::make_shared<State>(*static_cast<const State*>(q.static_state));
      state->selected =
          state->plane_region(fixed_tensor::view_region(q.writes[0].layout));
      for (const auto& write : q.writes)
        require(plane_key(state->selected) ==
                    plane_key(state->plane_region(
                        fixed_tensor::view_region(write.layout))),
                "planar output group");
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
    } catch (const Status& e) {
      return Result<ps_prepared_kernel_v8>(e);
    }
  };
  return def;
}
}  // namespace ps::plugin_internal::filter
