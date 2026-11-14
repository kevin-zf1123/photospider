#pragma once
#include <memory>
#include <string>

#include "photospider/execution/execution.hpp"
#include "photospider/plugin/executor_module_api.h"
namespace ps {
namespace execution_internal {
struct DiagnosticsState;
struct PreparedState;
struct RunState;
struct ContextState;
}  // namespace execution_internal
class PhysicalExecutionObservation final {
 public:
  PhysicalExecutionObservation() = default;
  bool valid() const noexcept { return state_ != nullptr; }
  ExecutionDiagnostics snapshot() const noexcept;
  /** @brief Copy a retained trace prefix into caller storage, without
   * allocation. Returns the number copied, bounded by capacity. Null returns
   * zero. Each copy is synchronized; separate calls need not see the same
   * progress point. Trace owners never retain Results and remain usable after
   * Run destruction.
   */
  std::size_t copy_task_timings(ExecutionTaskTiming* target,
                                std::size_t capacity) const noexcept;
  std::size_t copy_call_timings(ExecutionCallTiming* target,
                                std::size_t capacity) const noexcept;

 private:
  friend class PhysicalRunHandle;
  std::shared_ptr<execution_internal::DiagnosticsState> state_;
};
/** @brief Immutable plan, direct kernels and module/resource leases.
 * Preparation performs no payload execution. Copies share preparation and
 * support independent concurrent Runs in their originating context.
 */
class PhysicalPreparedPlan final {
 public:
  PhysicalPreparedPlan() = default;
  bool valid() const noexcept { return state_ != nullptr; }
  const ExecutionPlan& plan() const;

 private:
  friend class PhysicalExecutionContext;
  std::shared_ptr<const execution_internal::PreparedState> state_;
};
/** @brief Shared mutable Run owner with explicit cancellation and drain.
 * Destroying a handle releases that reference; accepted work keeps its own
 * lifetime until completion. Context destruction cancels and drains all Runs.
 */
class PhysicalRunHandle final {
 public:
  PhysicalRunHandle() = default;
  bool valid() const noexcept { return state_ != nullptr; }
  /** @brief Idempotent stop request; false once terminal outcome is latched. */
  bool request_cancel() const noexcept;
  /** @brief Retain diagnostics without retaining this Run or its Results. */
  PhysicalExecutionObservation observation() const noexcept;
  /** @brief Wait for tasks and observers, then copy the terminal outcome.
   * Failure is written directly into the bounded C records while holding the
   * Run lock. Success transfers shared Results into the caller's output. */
  ps_kernel_status_v8 join(ExecutionResult& output,
                           ps_executor_failure_v1& detail) const;

 private:
  friend class PhysicalExecutionContext;
  std::shared_ptr<execution_internal::RunState> state_;
};
/** @brief Owner of bounded worker resources and the frozen operation registry.
 * Direct methods must not race destruction. Destruction rejects submissions,
 * requests cancellation and waits for every accepted Run and observer to drain.
 * From an external thread it also joins all coordinators, including temporary
 * coordinators for overlapping Runs, after they release their execution owners.
 * A provider releasing the last Context owner during coordinator cleanup may
 * trigger destruction on a coordinator thread. That path stops and detaches
 * coordinators to avoid self/cross-Context joins; their remaining owners retire
 * as cleanup returns. Observer callbacks must still not destroy their Context.
 * Caller-retained Run handles, PreparedPlans, Results and observations retain
 * their documented resources independently; destroying the Context does not
 * revoke these owners or promise an immediate decrease in process RSS.
 */
class PhysicalExecutionContext final {
 public:
  explicit PhysicalExecutionContext(
      std::shared_ptr<OperationRegistry> operations,
      ExecutionContextConfig config, ResourceBudget budget);
  ~PhysicalExecutionContext() noexcept;
  PhysicalExecutionContext(const PhysicalExecutionContext&) = delete;
  PhysicalExecutionContext& operator=(const PhysicalExecutionContext&) = delete;
  /** @brief Bind direct kernels to the exact compiled placement.
   * Stale/foreign/default Plans fail Stale. Unsupported placements fail
   * BackendUnavailable. The immutable Plan body is shared; new host tables,
   * bridge metadata and kernel wrappers are admitted under this context root.
   * Capacity/allocation exhaustion returns ResourceExhausted with partial
   * preparation released. No lowering, topology change or fallback occurs.
   */
  Result<PhysicalPreparedPlan> prepare(const ExecutionPlan& plan) const;
  /** @brief Preflight immutable bindings then start one independent Run.
   * Context/run limits or metadata/payload admission may fail with
   * ResourceExhausted. Callback exceptions never escape the Run boundary.
   */
  Result<PhysicalRunHandle> submit(const PhysicalPreparedPlan& plan,
                                   ExecutionBindings bindings = {},
                                   const CancellationToken& cancellation = {},
                                   ExecutionOptions options = {});
  Result<ResourceBudget> resource_budget() const;
  /** Release idle cache references before retrying a rejected admission.
   * Thread-safe; returns true if any entries were removed. Running readers
   * keep their owners. No Run is retried or cancelled by this method.
   */
  bool release_cached_tiles() const;
  std::uint32_t cpu_workers() const noexcept;
  /** @brief Human-readable native device and driver description for
   * diagnostics. Empty until successful native device creation during prepare.
   * This query creates no device and submits no work. The owned string is safe
   * to retain independently; its format is not a persistent identity or parsing
   * contract. Concurrent calls with prepare/submit are synchronized;
   * destruction must not race this method. Allocation failure throws
   * std::bad_alloc.
   */
  std::string native_device_description() const;

 private:
  std::shared_ptr<execution_internal::ContextState> state_;
};
}  // namespace ps
