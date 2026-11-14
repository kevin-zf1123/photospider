#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "cache_build_identity.hpp"  // NOLINT(build/include_subdir)
#include "host/provider_library.hpp"
#include "sdk/execution/provider.hpp"
namespace ps::executor_sdk {
namespace {
template <class F>
ps_kernel_status_v8 checked(F fn) noexcept {
  try {
    auto s = fn();
    return kernel_failure(s);
  } catch (const std::bad_alloc&) {
    return kernel_failure(ErrorCode::ResourceExhausted);
  } catch (...) {
    return kernel_failure(ErrorCode::InvalidArgument);
  }
}
Status invalid() {
  return {ErrorCode::InvalidArgument, "invalid executor data request"};
}
struct Lease {
  std::shared_ptr<const void> module;
  void* object;
  void (*release)(void*);
  ~Lease() { release(object); }
};
ps_kernel_status_v8 read(const void* value, uint32_t member, const uint64_t* at,
                         uint32_t rank, void* bytes, uint32_t n) {
  return checked([&] {
    if (!value || !at || !rank || rank > 8 || !bytes)
      return invalid();
    const auto& r = *static_cast<const ResultRef*>(value);
    auto descriptor = r.descriptor(false);
    if (!descriptor.ok())
      return descriptor.status();
    return r.read_tensor(descriptor.value(), member,
                         std::vector<uint64_t>(at, at + rank), bytes, n);
  });
}
ps_kernel_status_v8 publish(void* raw, uint32_t member,
                            const ps_mapping_region_v1* r, const uint8_t* bytes,
                            uint64_t n) {
  return checked([&] {
    if (!raw || !r || r->rank == 0 || r->rank > 8 || !bytes || n > SIZE_MAX)
      return invalid();
    std::vector<RegionDimension> dims;
    for (unsigned i = 0; i < r->rank; ++i)
      dims.push_back({r->origin[i], r->extent[i]});
    return static_cast<ResultBuilder*>(raw)->publish_tensor(
        member, Region(std::move(dims)), ByteView(bytes, n));
  });
}
ps_kernel_status_v8 seal(void* raw, ps_executor_owner_v1* out) {
  return checked([&] {
    if (!raw || !out)
      return invalid();
    *out = {};
    auto value = static_cast<ResultBuilder*>(raw)->seal();
    if (!value.ok())
      return value.status();
    *out = {new ResultRef(value.take_value()),
            [](void* p) { delete static_cast<ResultRef*>(p); }};
    return Status{};
  });
}
Status start_builder(const Environment& env, const ExecutionPlan& plan,
                     const SchemaTemplate& schema,
                     ps_executor_result_builder_v1* out) {
  auto builder = ResultBuilder::start(env.budget, schema, "executor",
                                      env.config.result_growth_limits(),
                                      plan.tile_extent());
  if (!builder.ok())
    return builder.status();
  *out = {{new ResultBuilder(builder.take_value()),
           [](void* p) { delete static_cast<ResultBuilder*>(p); }},
          publish,
          seal};
  return {};
}
ps_kernel_status_v8 make_result(const void* env, const ps_executor_plan_v1* q,
                                ps_string_v8 name,
                                ps_executor_result_builder_v1* out) {
  return checked([&] {
    if (!out || !env || !q || q->struct_size != sizeof(*q) || !q->handle)
      return invalid();
    *out = {};
    const auto& e = *static_cast<const Environment*>(env);
    const auto& plan = *static_cast<const ExecutionPlan*>(q->handle);
    auto key = host_internal::provider_text(name);
    if (!key.ok())
      return key.status();
    if (!plan.bound_to(*e.operations) || !plan.outputs().count(key.value()))
      return invalid();
    return start_builder(e, plan, *plan.output(key.value()).result_schema, out);
  });
}
ps_kernel_status_v8 make_value_result(const void* env,
                                      const ps_executor_plan_v1* q,
                                      uint32_t value,
                                      ps_executor_result_builder_v1* out) {
  return checked([&] {
    if (!out || !env || !q || q->struct_size != sizeof(*q) || !q->handle)
      return invalid();
    *out = {};
    const auto& e = *static_cast<const Environment*>(env);
    const auto& plan = *static_cast<const ExecutionPlan*>(q->handle);
    const auto& p = plan.program();
    if (!plan.bound_to(*e.operations) || value >= p.values.size())
      return invalid();
    return start_builder(e, plan, *p.schemas[p.values[value].schema], out);
  });
}
}  // namespace
ps_kernel_status_v8 prepare_task(const void*, const ps_executor_plan_v1*,
                                 uint32_t, const ps_native_gpu_service_v2*,
                                 ps_executor_kernel_v1*);
void bind_data_services(ps_executor_host_v1* host) {
  host->query_extension = [](const void*, ps_string_v8 name,
                             ps_string_v8 version, const void** out) {
    if (!out || !name.data || !version.data)
      return kernel_failure(ErrorCode::InvalidArgument);
    *out = nullptr;
    const std::string_view key(name.data, name.size);
    const std::string_view revision(version.data, version.size);
    if (key == PS_EXECUTOR_VALUE_RESULTS_V1 && revision == "1") {
      static const ps_executor_value_results_v1 api{sizeof(api),
                                                    make_value_result};
      *out = &api;
      return ps_kernel_status_v8{};
    }
    if (key != "photospider.physical-sdk" ||
        revision != PHOTOSPIDER_CACHE_BUILD_ID)
      return kernel_failure(ErrorCode::TypeMismatch,
                            "unsupported executor host extension or version");
    static const PhysicalHostServices services{
        [](const void* raw) -> const Environment& {
          return *static_cast<const Environment*>(raw);
        },
        [](const void* raw) -> const ExecutionPlan& {
          return *static_cast<const ExecutionPlan*>(raw);
        },
        [](const void* raw) -> const Submission& {
          return *static_cast<const Submission*>(raw);
        },
        [](void* dest, ExecutionResult&& value) {
          *static_cast<ExecutionResult*>(dest) = std::move(value);
        }};
    *out = &services;
    return ps_kernel_status_v8{};
  };
  host->prepare_kernel = prepare_task;
  host->read_sample = read;
  host->make_result = make_result;
  host->lease = [](const void* ctx, void* object, void (*release)(void*),
                   ps_executor_owner_v1* out) {
    return checked([&] {
      if (!ctx || !release || !out)
        return invalid();
      *out = {new Lease{static_cast<const Environment*>(ctx)->module, object,
                        release},
              [](void* p) { delete static_cast<Lease*>(p); }};
      return Status{};
    });
  };
  host->write_task_timing = [](void* dst, uint64_t cap, uint64_t i,
                               const ps_executor_task_timing_v1* t) {
    if (!dst || !t || i >= cap)
      return;
    static_cast<ExecutionTaskTiming*>(dst)[i] = {
        t->activation,
        t->submission,
        t->source_call,
        t->block,
        t->task,
        t->call,
        static_cast<physical::Backend>(t->backend),
        static_cast<ErrorCode>(t->status),
        t->ready_ns,
        t->submit_ns,
        t->cpu_start_ns,
        t->completion_ns,
        t->consumed_ns,
        t->copy_bytes,
        t->lane_wait_ns,
        t->context_wait_ns,
        t->coordinator_delay_ns,
        t->device};
  };
  host->write_call_timing = [](void* dst, uint64_t cap, uint64_t i,
                               const ps_executor_call_timing_v1* t) {
    if (dst && t && i < cap)
      static_cast<ExecutionCallTiming*>(dst)[i] = {
          t->activation, t->sequence, t->source_call,
          t->retired_ns, t->call,     static_cast<CallOutcome>(t->outcome)};
  };
  host->report_diagnostics = [](void* dest,
                                const ps_executor_diagnostics_v1* d) {
    if (!dest || !d || d->struct_size != sizeof(*d))
      return;
    auto& out = *static_cast<ExecutionDiagnostics*>(dest);
    out.submitted_tasks = d->submitted_tasks;
    out.completed_tasks = d->completed_tasks;
    out.retired_calls = d->retired_calls;
    out.activations = d->activations;
  };
}
}  // namespace ps::executor_sdk
