#include <algorithm>
#include <memory>
#include <string>

#include "sdk/execution/provider.hpp"
#include "sdk/program/artifact_text.hpp"
namespace ps::executor_sdk {
namespace {
ps_kernel_status_v8 invalid() {
  return kernel_failure(ErrorCode::InvalidArgument);
}
ps_string_v8 str(const std::string& s) {
  return {s.data(), static_cast<uint32_t>(s.size())};
}
}  // namespace
void bind_plan_services(ps_executor_plan_v1* view) {
  view->task_info = [](const void* raw, uint32_t id, ps_executor_task_v1* out) {
    const auto& p = static_cast<const ExecutionPlan*>(raw)->program();
    if (!out || id >= p.tasks.size())
      return invalid();
    const auto& t = p.tasks[id];
    const auto& place = p.placements[t.placement];
    *out = {};
    out->kind = static_cast<uint32_t>(t.kind);
    out->call = t.call;
    if (t.kernel != physical::none) {
      const auto& k = p.kernels[t.kernel];
      out->abi = k.abi_version;
      out->symbol = str(k.symbol);
      out->contract = str(k.contract);
    }
    out->backend = static_cast<uint32_t>(place.backend);
    out->domain = static_cast<uint32_t>(place.domain);
    out->profile = str(place.profile);
    out->queue = str(place.queue);
    out->read_count = t.reads.size();
    out->writes = t.writes.data();
    out->write_count = t.writes.size();
    out->dependencies = t.predecessors.data();
    out->dependency_count = t.predecessors.size();
    out->schema_reads = t.schema_reads.data();
    out->schema_read_count = t.schema_reads.size();
    out->parameters = t.parameters.data();
    out->parameter_bytes = t.parameters.size();
    std::copy_n(t.scratch.data(), 3, out->scratch);
    return ps_kernel_status_v8{};
  };
  view->read = [](const void* raw, uint32_t id, uint32_t slot, uint32_t* port,
                  uint32_t* tile, ps_kernel_layout_v8* layout) {
    const auto& p = static_cast<const ExecutionPlan*>(raw)->program();
    if (!port || !tile || !layout || id >= p.tasks.size() ||
        slot >= p.tasks[id].reads.size())
      return invalid();
    const auto& r = p.tasks[id].reads[slot];
    *port = r.port;
    *tile = r.tile;
    *layout = kernel_layout(p.layouts[r.layout]);
    return ps_kernel_status_v8{};
  };
  view->tile = [](const void* raw, uint32_t id, ps_executor_tile_v1* out) {
    const auto& p = static_cast<const ExecutionPlan*>(raw)->program();
    if (!out || id >= p.tiles.size())
      return invalid();
    const auto& t = p.tiles[id];
    const auto& l = p.layouts[t.layout];
    *out = {t.value,       t.member,     static_cast<uint32_t>(t.domain),
            t.producer,    t.tile_index, t.bytes,
            l.byte_offset, t.alignment,  kernel_layout(l)};
    return ps_kernel_status_v8{};
  };
  view->tables = [](const void* raw, ps_string_v8* bytes,
                    ps_executor_owner_v1* out) {
    if (!bytes || !out)
      return invalid();
    try {
      const auto& p = *static_cast<const ExecutionPlan*>(raw);
      auto result = compiler_internal::encode_physical_tables(
          p.program(), p.calls(), UINT32_MAX);
      if (!result.ok())
        return kernel_failure(result.status());
      auto owner = std::make_unique<std::string>(result.take_value());
      *bytes = str(*owner);
      *out = {owner.release(),
              [](void* s) { delete static_cast<std::string*>(s); }};
      return ps_kernel_status_v8{};
    } catch (...) {
      return kernel_failure(ErrorCode::ResourceExhausted);
    }
  };
}
ps_kernel_status_v8 prepare_task(const void* raw,
                                 const ps_executor_plan_v1* plan, uint32_t id,
                                 const ps_native_gpu_service_v2* native,
                                 ps_executor_kernel_v1* out) {
  if (!raw || !plan || !out || plan->struct_size != sizeof(*plan) ||
      !plan->handle)
    return invalid();
  *out = {};
  try {
    const auto& env = *static_cast<const Environment*>(raw);
    const auto& p = *static_cast<const ExecutionPlan*>(plan->handle);
    if (!p.bound_to(*env.operations) || !p.current())
      return kernel_failure(ErrorCode::Stale);
    if (id >= p.program().tasks.size())
      return invalid();
    const auto& task = p.program().tasks[id];
    if (task.kind == physical::TaskKind::Copy || task.call >= p.calls().size())
      return invalid();
    const auto& op = p.calls()[task.call];
    auto prepared = op.prepared->prepare_kernel(p.program(), id, op.outputs,
                                                native, env.module, env.budget);
    if (!prepared.ok())
      return kernel_failure(prepared.status());
    auto k = std::make_unique<PreparedKernel>(prepared.take_value());
    out->invoke_cpu = [](void* raw, ps_kernel_invocation_v8* i) {
      if (!raw || !i)
        return invalid();
      auto& k = *static_cast<PreparedKernel*>(raw);
      k.bind(i);
      return k.invoke(*i);
    };
    out->invoke_cpu_v9 = [](void* raw, ps_kernel_invocation_v9* i) {
      if (!raw || !i)
        return invalid();
      auto& k = *static_cast<PreparedKernel*>(raw);
      k.bind(i);
      return k.invoke(*i);
    };
    out->submit_gpu = [](void* raw, ps_kernel_invocation_v8* i,
                         const ps_kernel_completion_v8* c) {
      ps_kernel_submission_v8 failure{};
      if (!raw || !i || !c) {
        failure.status = invalid();
        return failure;
      }
      auto& k = *static_cast<PreparedKernel*>(raw);
      k.bind(i);
      return k.submit(*i, *c);
    };
    out->owner = {k.release(),
                  [](void* raw) { delete static_cast<PreparedKernel*>(raw); }};
    return {};
  } catch (...) {
    return kernel_failure(ErrorCode::ResourceExhausted);
  }
}
}  // namespace ps::executor_sdk
