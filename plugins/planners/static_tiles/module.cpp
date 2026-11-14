#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "photospider/plugin/planner_module_api.h"
#include "plugins/planners/static_tiles/static_tiles.hpp"
#include "sdk/program/artifact_text.hpp"
namespace {
using namespace ps;                     // NOLINT(build/namespaces)
using namespace ps::compiler_internal;  // NOLINT(build/namespaces)
struct Bytes {
  ps_plan_bytes_v1 data{};
  ~Bytes() {
    if (data.release)
      data.release(data.owner);
  }
};
void check(ps_kernel_status_v8 status) {
  if (status.code)
    throw kernel_status(status);
}
template <class T>
T get(Result<T> r) {
  if (!r.ok())
    throw r.status();
  return r.take_value();
}
struct Selections {
  std::map<std::string, std::size_t> outputs;
  std::map<std::string, TileRequest> requests;
  std::map<std::pair<std::string, uint32_t>, std::vector<TileInterval>> ranges;
};
ps_kernel_status_v8 plan(void*, const ps_planner_host_v1* h) try {
  PlanningOptions options;
  options.tile_extent = h->tile_extent;
  options.execution_mode = static_cast<ExecutionMode>(h->execution_mode);
  options.maximum_records = h->maximum_records;
  options.maximum_tasks = h->maximum_tasks;
  options.maximum_dependency_edges = h->maximum_edges;
  options.maximum_work = h->maximum_work;
  physical::ValidationLimits limits{h->maximum_records, h->maximum_edges,
                                    h->maximum_work};
  const auto byte_limit = physical_transfer_limit(limits);
  auto decode = [&](const Bytes& b, physical::ValidationLimits bounds) {
    return get(decode_physical_tables(
        {b.data.data, static_cast<size_t>(b.data.size)}, h->tile_extent, bounds,
        physical_transfer_limit(bounds)));
  };
  Bytes seed;
  check(h->seed(h->context, &seed.data));
  auto state = decode(seed, limits);
  Selections selected;
  if (h->block_count) {
    check(h->selection(h->context, 0, &selected,
                       [](void* ctx, ps_string_v8 name, uint32_t output,
                          uint32_t m, uint64_t begin, uint64_t end) {
                         try {
                           auto& s = *static_cast<Selections*>(ctx);
                           std::string key(name.data, name.size);
                           s.outputs.emplace(key, output);
                           s.requests.try_emplace(key);
                           if (m != physical::none)
                             s.ranges[{key, m}].push_back({begin, end});
                           return ps_kernel_status_v8{};
                         } catch (...) {
                           return kernel_failure(ErrorCode::ResourceExhausted);
                         }
                       }));
  }
  for (const auto& r : selected.ranges)
    selected.requests.at(r.first.first)
        .members.emplace(r.first.second, get(TileSet::from_intervals(
                                             ResourceBudget{}, r.second.data(),
                                             r.second.size())));
  StaticPlanningInput input{
      state.physical,
      state.bindings,
      selected.outputs,
      selected.requests,
      [&](auto i, bool gpu) {
        uint32_t p, g;
        check(h->traits(h->context, i, &p, &g));
        return gpu ? g : p;
      },
      [&](auto di) {
        uint32_t domain;
        check(h->input_domain(h->context, di, &domain));
        return static_cast<physical::Domain>(domain);
      },
      [&](auto ci, const auto& req, physical::Backend backend) {
        std::vector<ps_tile_request_v8> intervals;
        for (uint32_t i = 0; i < req.size(); ++i)
          for (const auto& m : req[i].members)
            for (auto range : m.second.intervals())
              intervals.push_back({i, m.first, range.begin, range.end});
        Bytes result;
        check(h->lower(h->context, ci, intervals.data(), intervals.size(),
                       static_cast<uint32_t>(backend), &result.data));
        auto local_limits = limits;
        local_limits.maximum_records =
            std::max(limits.maximum_records,
                     physical::ValidationLimits{}.maximum_records);
        auto local = decode(result, local_limits);
        return Result<OperationProgram>(
            OperationProgram{std::move(local.physical),
                             std::move(local.bindings.at(0).outputs)});
      },
      [&](uint64_t n) {
        check(h->charge(h->context, n));
        return true;
      }};
  auto status = expand_static(input, options);
  if (!status.ok())
    throw status;
  auto physical =
      get(encode_physical_tables(state.physical, state.bindings, byte_limit));
  return h->publish(h->context, physical.data(), physical.size());
} catch (const Status& s) {
  return kernel_failure(s);
} catch (const std::bad_alloc&) {
  return kernel_failure(ErrorCode::ResourceExhausted);
} catch (...) {
  return kernel_failure(ErrorCode::Internal);
}
}  // namespace
extern "C" PS_OPERATION_MODULE_EXPORT const ps_planner_module_v1*
ps_planner_module_get_api_v1(void) {
  static const ps_planner_module_v1 module{sizeof(module),
                                           1,
                                           3,
                                           {"static-tiles", 12},
                                           {"2", 1},
                                           {"Fixed physical tile planning", 28},
                                           nullptr,
                                           plan,
                                           [](void*) {}};
  return &module;
}
