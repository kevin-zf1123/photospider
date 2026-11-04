// Copyright 2026 Photospider Authors
#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "07-grade/statistical_common.hpp"

namespace ps::plugin_internal::grade::statistics {
/** Request-local specialization for collective operator helpers. All copies
 * contain descriptors only. The fixed callback owns its immutable specialized
 * program until release; no Run payload or cross-Run state is retained.
 */
template <class State>
OperationDefinition collective_operation(OperationDefinition definition) {
  const auto lower = definition.lower;
  definition.lower =
      [lower](const OperationLowering& q) -> Result<OperationProgram> {
    try {
      auto state = *static_cast<const State*>(q.static_state);
      std::vector<std::pair<unsigned, Region>> regions;
      for (unsigned p = 0; p < q.requests.size(); ++p) {
        for (const auto& m : q.requests[p].members) {
          const auto grid = take(TileGrid::from_schema(
              *q.outputs[p].result_schema, q.tile_extent, m.first));
          for (auto tile : m.second) {
            regions.emplace_back(p, take(grid.region(tile)));
          }
        }
      }
      state.configure(regions);
      auto child = q;
      child.static_state = &state;
      return lower(child);
    } catch (const Status& s) {
      return Result<OperationProgram>(s);
    }
  };
  const auto prepare = definition.prepare_kernel_v9;
  definition.prepare_kernel_v9 =
      [prepare](const auto& kernel, const auto& placement,
                const KernelPreparation& q, const ResourceSet& resources,
                const ps_native_gpu_service_v2* native)
      -> Result<ps_prepared_kernel_v9> {
    struct Owner {
      std::shared_ptr<State> state;
      ps_prepared_kernel_v9 ready{};
      ~Owner() {
        if (ready.release) {
          ready.release(ready.context);
        }
      }
    };
    try {
      auto owner = std::make_unique<Owner>();
      owner->state =
          std::make_shared<State>(*static_cast<const State*>(q.static_state));
      std::vector<std::pair<unsigned, Region>> regions;
      for (const auto& w : q.writes) {
        regions.emplace_back(w.port, fixed_tensor::view_region(w.layout));
      }
      owner->state->configure(regions);
      auto child = q;
      child.static_state = owner->state.get();
      owner->ready = take(prepare(kernel, placement, child, resources, native));
      auto out = owner->ready;
      out.context = owner.release();
      out.release = [](const void* p) { delete static_cast<const Owner*>(p); };
      out.invoke_cpu = [](const ps_kernel_invocation_v9* i,
                          ps_kernel_status_v8* s) {
        const auto& owner =
            *static_cast<const Owner*>(i->views.prepared_context);
        auto child = *i;
        child.views.prepared_context = owner.ready.context;
        owner.ready.invoke_cpu(&child, s);
      };
      return Result<ps_prepared_kernel_v9>(out);
    } catch (const Status& s) {
      return Result<ps_prepared_kernel_v9>(s);
    }
  };
  return definition;
}
}  // namespace ps::plugin_internal::grade::statistics
