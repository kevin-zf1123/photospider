#include "sdk/execution/provider.hpp"

#include <memory>
#include <mutex>
#include <utility>

#include "cache_build_identity.hpp"  // NOLINT(build/include_subdir)
#include "plugins/executors/physical/physical_context.hpp"
namespace ps::executor_sdk {
namespace {
template <class T>
T& ref(void* p) {
  return *static_cast<T*>(p);
}
template <class T>
ps_executor_owner_v1 own(T value) {
  return {new T(std::move(value)), [](void* p) { delete static_cast<T*>(p); }};
}
template <class F>
ps_kernel_status_v8 guard(F f) noexcept {
  try {
    auto status = f();
    return kernel_failure(status);
  } catch (const std::bad_alloc&) {
    return kernel_failure(ErrorCode::ResourceExhausted);
  } catch (...) {
    return kernel_failure(ErrorCode::OperationFailed);
  }
}
struct PhysicalOwner {
  const PhysicalHostServices* host;
  std::unique_ptr<PhysicalExecutionContext> context;
  PhysicalExecutionContext* operator->() const { return context.get(); }
  void reset() { context.reset(); }
};
struct PhysicalRun {
  const PhysicalHostServices* host = nullptr;
  PhysicalRunHandle handle;
  // The host asks for its observation after submit can already issue work.
  // Reserve that owner before issue, including the low-memory failure path.
  std::mutex observation_mutex;
  std::unique_ptr<PhysicalExecutionObservation> first_observation =
      std::make_unique<PhysicalExecutionObservation>();
};
ps_kernel_status_v8 create(void*, const ps_executor_host_v1* h,
                           ps_executor_owner_v1* out) {
  return guard([&] {
    if (!h || h->struct_size != sizeof(*h) || !h->query_extension)
      return Status{ErrorCode::TypeMismatch,
                    "physical executor requires matching Photospider SDK"};
    const void* extension = nullptr;
    constexpr char name[] = "photospider.physical-sdk";
    constexpr char build[] = PHOTOSPIDER_CACHE_BUILD_ID;
    auto status = kernel_status(
        h->query_extension(h->handle, {name, sizeof(name) - 1},
                           {build, sizeof(build) - 1}, &extension));
    if (!status.ok())
      return status;
    if (!extension)
      return Status{ErrorCode::TypeMismatch, "missing physical SDK services"};
    auto* host = static_cast<const PhysicalHostServices*>(extension);
    const auto& e = host->environment(h->handle);
    *out = own(PhysicalOwner{host, std::make_unique<PhysicalExecutionContext>(
                                       e.operations, e.config, e.budget)});
    return Status{};
  });
}
ps_kernel_status_v8 prepare(void* c, const ps_executor_plan_v1* plan,
                            ps_executor_owner_v1* out) {
  return guard([&] {
    auto result = ref<PhysicalOwner>(c)->prepare(
        ref<PhysicalOwner>(c).host->plan(plan->handle));
    if (result.ok())
      *out = own(result.take_value());
    return result.status();
  });
}
ps_kernel_status_v8 submit(void* c, void* prep,
                           const ps_executor_submit_v2* submit,
                           ps_executor_owner_v1* out) {
  return guard([&] {
    const auto& s = ref<PhysicalOwner>(c).host->submission(submit->handle);
    // Allocate the external Run owner before issuing asynchronous work.
    auto run = std::make_unique<PhysicalRun>();
    auto result = ref<PhysicalOwner>(c)->submit(
        ref<PhysicalPreparedPlan>(prep), s.bindings, s.cancellation, s.options);
    if (result.ok()) {
      run->host = ref<PhysicalOwner>(c).host;
      run->handle = result.take_value();
      *out = {run.release(),
              [](void* p) { delete static_cast<PhysicalRun*>(p); }};
    }
    return result.status();
  });
}
ps_kernel_status_v8 join(void* r, const ps_executor_results_v1* sink) try {
  auto& run = ref<PhysicalRun>(r);
  ExecutionResult result;
  auto status = run.handle.join(result, *sink->failure);
  if (!status.code)
    run.host->result(sink->result_owner, std::move(result));
  return status;
} catch (const std::bad_alloc&) {
  return kernel_failure(ErrorCode::ResourceExhausted);
} catch (...) {
  return kernel_failure(ErrorCode::OperationFailed);
}
}  // namespace
const ps_executor_module_v2& physical_api() {
  static const ps_executor_module_v2 api{
      sizeof(api),
      PS_EXECUTOR_MODULE_ABI_VERSION_2,
      5,
      3,
      7,
      (1U << 8) | (1U << 9),
      {"physical", 8},
      nullptr,
      create,
      [](void* c) { ref<PhysicalOwner>(c).reset(); },
      prepare,
      submit,
      [](void* r) -> uint32_t {
        return ref<PhysicalRun>(r).handle.request_cancel();
      },
      join,
      [](void* r, ps_executor_owner_v1* out) {
        return guard([&] {
          auto& run = ref<PhysicalRun>(r);
          std::unique_ptr<PhysicalExecutionObservation> owner;
          {
            std::lock_guard<std::mutex> lock(run.observation_mutex);
            owner = std::move(run.first_observation);
          }
          if (!owner)
            owner = std::make_unique<PhysicalExecutionObservation>();
          *owner = run.handle.observation();
          *out = {owner.release(), [](void* p) {
                    delete static_cast<PhysicalExecutionObservation*>(p);
                  }};
          return Status{};
        });
      },
      [](void* r, void* out) {
        *static_cast<ExecutionDiagnostics*>(out) =
            ref<PhysicalExecutionObservation>(r).snapshot();
      },
      [](void* r, uint32_t kind, void* out, uint64_t n) -> uint64_t {
        return kind ? ref<PhysicalExecutionObservation>(r).copy_call_timings(
                          static_cast<ExecutionCallTiming*>(out), n)
                    : ref<PhysicalExecutionObservation>(r).copy_task_timings(
                          static_cast<ExecutionTaskTiming*>(out), n);
      },
      [](void* c) -> uint32_t {
        return ref<PhysicalOwner>(c)->release_cached_tiles();
      },
      [](void* c) -> uint32_t { return ref<PhysicalOwner>(c)->cpu_workers(); },
      [](void* c, void* ctx, void (*write)(void*, ps_string_v8)) {
        return guard([&] {
          auto s = ref<PhysicalOwner>(c)->native_device_description();
          write(ctx, {s.data(), static_cast<uint32_t>(s.size())});
          return Status{};
        });
      },
      [](void*) {}};
  return api;
}
}  // namespace ps::executor_sdk
