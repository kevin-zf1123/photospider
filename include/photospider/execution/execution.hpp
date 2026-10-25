#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "photospider/compiler/plan.hpp"
#include "photospider/core/cancellation.hpp"
#include "photospider/core/resources.hpp"
#include "photospider/data/result.hpp"
#include "photospider/plugin/fixed_invocation_api.h"

namespace ps {
namespace execution_internal {
struct ContextState;
struct PreparedState;
struct RunState;
struct DiagnosticsState;
}  // namespace execution_internal
/** @brief Fixed local resources. CPU workers are always available. */
struct ExecutionContextConfig final {
  std::uint32_t cpu_workers = 0;
  std::uint32_t maximum_queued_tasks = 1024;
  /** @brief Shared context credit for accepted asynchronous GPU submissions.
   * Zero disables GPU issue. Credit returns only after terminal completion.
   */
  std::uint32_t maximum_gpu_submissions = 4;
  std::uint32_t maximum_runs = 1024;
  std::uint64_t maximum_live_bytes = 256U * 1024U * 1024U;
  std::optional<ResourceLimits> managed_resources;
};
struct ExecutionBinding final {
  std::string name;
  ResultRef result;
};
/** @brief Exact-name immutable input snapshot, retained through Run drain.
 * Duplicate/missing/extra names fail preflight. Payload must already cover the
 * compiled tile views and match their physical layout; adaptation requires
 * explicit compiled COPY tasks. No sample validation or hidden copy occurs.
 */
struct ExecutionBindings final {
  std::vector<ExecutionBinding> inputs;
};

enum class CallOutcome : std::uint8_t { Succeeded, Failed, Elided };
/** @brief Owning call export, independent of final RET ownership.
 * Values are keyed by physical value ID. A callback may copy ResultRef owners
 * beyond Run/context lifetime. Failed and elided calls have no values.
 */
struct CallRecord final {
  std::uint64_t sequence = 0, activation = 0, source_call = 0;
  physical::Id call = physical::none, failed_task = physical::none;
  CallOutcome outcome = CallOutcome::Elided;
  Status status;
  std::map<physical::Id, ResultRef, std::less<physical::Id>,
           ResourceAllocator<std::pair<const physical::Id, ResultRef>>>
      values;
};
struct ExecutionOptions final {
  /** @brief Zero uses context CPU/GPU grants. A positive value bounds this
   * Run's lanes separately for each backend.
   */
  std::uint32_t maximum_parallelism = 0;
  /** @brief Every block entry consumes one activation, including empty loops.
   */
  std::uint64_t maximum_block_activations = 1048576;
  /** @brief Run-local coordinator plus opted-in v9 arithmetic/limb work.
   * Shared Context ResourceLimits also bound cumulative work across Runs.
   */
  std::uint64_t maximum_work = 268435456;
  /** @brief Run-local v9 refinement attempts. Zero permits no refinements.
   * Legacy v8 kernels do not expose arithmetic/refinement admission.
   */
  std::uint64_t maximum_refinement_stages = UINT64_MAX;
  /** @brief Maximum retained task records and call records, separately.
   * Zero disables detailed tracing and native timestamp queries. Both arrays
   * are admitted and allocated before starting the Run. Excess records are
   * counted and discarded, including across loops; callbacks never allocate.
   */
  std::uint32_t trace_capacity = 0;
  /** @brief Ordered per-Run delivery on a separate serial lane.
   * Throws close the lane, discard queued exports and abort the Run after
   * accepted work drains. join waits for delivery settlement. A callback must
   * not join its own Run or destroy its ExecutionContext.
   */
  std::function<void(const CallRecord&)> on_call_retired;
};
/** @brief Last failed block admission, captured before any task in it issues.
 * capacity describes an atomic budget rejection when available. Allocator or
 * backend failures can have no exhausted dimension. Earlier blocks remain
 * observable; this record never implies rollback of their effects.
 */
struct BlockAdmissionFailure final {
  physical::Id block = physical::none;
  std::uint64_t activation = 0;
  ErrorCode code = ErrorCode::Ok;
  ResourceAdmissionFailure capacity;
};
/** @brief Per-Run output and scratch backing in physical Domain index order.
 * Host, Device and Shared are disjoint here; native bytes use actual capacity.
 * Reserved includes unallocated and already freed block-protected capacity.
 * Live counts committed backing, including retained exports. Excludes borrowed
 * inputs, code/driver memory and host metadata, whose budget is separate.
 */
struct ExecutionMemoryStatistics final {
  std::array<std::uint64_t, 3> reserved_bytes_by_domain{},
      live_bytes_by_domain{};
  std::array<std::uint64_t, 3> peak_reserved_bytes_by_domain{},
      peak_live_bytes_by_domain{};
};
/** @brief One consumed task completion, including rejected submissions.
 * Host times are nanoseconds from this Run's submission epoch on steady_clock.
 * ready_ns begins actual issue eligibility, including effect/retirement gates.
 * submit_ns precedes worker enqueue or GPU submit. cpu_start_ns is CPU-only;
 * completion_ns is the terminal notification, consumed_ns the coordinator's
 * observation. GPU device timing has its own explicitly named clock domain.
 * copy_bytes counts logical bytes of a successful COPY, not staging traffic.
 * Records are ordered by coordinator consumption, not submission or device
 * execution. Unsubmitted/skipped tasks have no task record.
 */
struct ExecutionTaskTiming final {
  std::uint64_t activation = 0, submission = 0, source_call = 0;
  physical::Id block = physical::none, task = physical::none,
               call = physical::none;
  physical::Backend backend = physical::Backend::Cpu;
  ErrorCode status = ErrorCode::Ok;
  std::uint64_t ready_ns = 0, submit_ns = 0, cpu_start_ns = 0;
  std::uint64_t completion_ns = 0, consumed_ns = 0, copy_bytes = 0;
  /** @brief Disjoint ready-to-submit intervals, classified at coordinator
   * safe points. Lane includes its exclusive preallocated scratch; Context
   * includes fair admission and CPU FIFO/GPU credit. Remaining time is host
   * coordinator work or wakeup. These are scheduler observations, not device
   * utilization. A full-block reservation has no dynamic scratch-capacity wait.
   */
  std::uint64_t lane_wait_ns = 0, context_wait_ns = 0, coordinator_delay_ns = 0;
  ps_kernel_device_timing_v8 device{};
};
/** @brief IR-ordered retirement time, excluding observer delivery.
 * Join with task records by activation/call. Elided calls have no tasks.
 * Aborted/suppressed calls have no retirement record.
 */
struct ExecutionCallTiming final {
  std::uint64_t activation = 0, sequence = 0, source_call = 0, retired_ns = 0;
  physical::Id call = physical::none;
  CallOutcome outcome = CallOutcome::Elided;
};
/** @brief Per-Run submission-slot statistics, indexed by Backend.
 * Inflight starts at issue and ends at coordinator consumption, including
 * inline completions and locally rejected GPU submissions. CPU slots include
 * queue wait. These are host scheduler occupancy, not device utilization.
 * inflight_ns sums submit-to-consumption intervals and may overlap.
 */
struct ExecutionBackendStatistics final {
  std::uint64_t submitted = 0, completed = 0, rejected = 0, inflight_ns = 0;
  std::uint32_t inflight = 0, peak_inflight = 0;
};
/** @brief First task-coordinator abort and subsequent task drain.
 * Host times use the task trace's Run-relative steady clock. Start records
 * coordinator abort observation (or exceptional frame unwinding), not the
 * caller's cancellation request. Completion means every issued task's terminal
 * event has been consumed; observer delivery is excluded. A false finished
 * flag denotes an ongoing drain. Ordinary call-failure cuts are not Run aborts.
 */
struct ExecutionDrainTiming final {
  physical::Id block = physical::none;
  std::uint64_t activation = 0, started_ns = 0, completed_ns = 0;
  std::uint32_t inflight_at_abort = 0;
  bool finished = false;
};
struct ExecutionDiagnostics final {
  /** @brief Trace origin as nanoseconds from steady_clock::time_since_epoch.
   * Add Run-relative host times to correlate observations in this process.
   * This is not a wall clock or device timestamp. Cross-domain comparisons
   * require an explicit clock calibration with its uncertainty.
   */
  std::uint64_t host_epoch_ns = 0;
  ExecutionMemoryStatistics memory;
  /** @brief New managed metadata owned by this Run, including diagnostics.
   * Shared Context/PreparedPlan and pre-existing input owners keep their prior
   * attribution. Export and diagnostic owners remain charged after Run/context
   * destruction; cross-thread release updates the original observation.
   */
  ResourceMetadataStatistics metadata;
  std::array<ExecutionBackendStatistics, 2> backends{};
  std::optional<ExecutionDrainTiming> abort_drain;
  std::uint64_t activations = 0, submitted_tasks = 0, completed_tasks = 0;
  std::uint64_t retired_calls = 0;
  /** @brief Admitted coordinator/v9 work and v9 refinement stages in this Run.
   * Legacy v8 numerical work is not observable. Failed admissions add nothing.
   */
  ResourceWork issued_work;
  std::uint32_t peak_active_tasks = 0;
  /** @brief Successful output-tile allocations, including later rollback.
   * Excludes scratch, commands, borrowed inputs and Result envelope aliases.
   */
  std::uint64_t allocation_count = 0, block_admission_failures = 0;
  std::optional<BlockAdmissionFailure> last_block_admission_failure;
  /** @brief Coordinator monotonic nanoseconds spent at an unsettled head.
   * Completed waits exclude observer delivery. Abort-truncated waits are
   * reported separately. current_head_wait_ns is the latest progress sample.
   */
  std::uint64_t retirement_head_wait_ns = 0, peak_retirement_head_wait_ns = 0;
  std::uint64_t unfinished_head_wait_ns = 0, current_head_wait_ns = 0;
  /** @brief Summed host intervals; parallel tasks can overlap.
   * CPU queue wait ends at invoke, CPU invoke ends at completion notification.
   * Completion delay ends when the coordinator consumes the notification.
   * COPY bytes include only successful logical transfers, once per task.
   */
  std::uint64_t ready_wait_ns = 0, cpu_queue_wait_ns = 0, cpu_invoke_ns = 0;
  std::uint64_t lane_wait_ns = 0, context_wait_ns = 0, coordinator_delay_ns = 0;
  /** @brief Always zero with atomic full-block output/scratch admission;
   * unavailable capacity rejects the block before any task becomes eligible.
   */
  std::uint64_t scratch_capacity_wait_ns = 0;
  /** @brief Completed observer callbacks: queue time from enqueue to callback,
   * and callback duration. Independent of task completion and retirement.
   * Includes throwing callbacks; discarded records contribute no callback.
   */
  std::uint64_t observer_queue_wait_ns = 0, observer_invoke_ns = 0;
  std::uint64_t observer_deliveries = 0;
  std::uint64_t completion_delay_ns = 0, copy_bytes = 0;
  std::uint64_t coordinator_waits = 0, coordinator_wakeups = 0;
  std::uint64_t task_timing_count = 0, call_timing_count = 0;
  std::uint64_t dropped_task_timings = 0, dropped_call_timings = 0;
};
/** @brief Independent Run diagnostic owner, with no Run or Result ownership.
 * snapshot is a synchronized allocation-free copy at coordinator safe points.
 * It remains readable after failure and after Run/context destruction.
 * Default observations return zero counters. Copies share only diagnostics.
 */
class PHOTOSPIDER_API ExecutionObservation final {
 public:
  ExecutionObservation() = default;
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
  friend class RunHandle;
  std::shared_ptr<execution_internal::DiagnosticsState> state_;
};
struct ExecutionResult final {
  ResourceMap<ResultRef> results;
  ExecutionDiagnostics diagnostics;
};
/** @brief Immutable plan, direct kernels and module/resource leases.
 * Preparation performs no payload execution. Copies share preparation and
 * support independent concurrent Runs in their originating context.
 */
class PHOTOSPIDER_API PreparedPlan final {
 public:
  PreparedPlan() = default;
  bool valid() const noexcept { return state_ != nullptr; }
  const ExecutionPlan& plan() const;

 private:
  friend class ExecutionContext;
  std::shared_ptr<const execution_internal::PreparedState> state_;
};
/** @brief Shared mutable Run owner with explicit cancellation and drain.
 * Destroying a handle releases that reference; accepted work keeps its own
 * lifetime until completion. Context destruction cancels and drains all Runs.
 */
class PHOTOSPIDER_API RunHandle final {
 public:
  RunHandle() = default;
  bool valid() const noexcept { return state_ != nullptr; }
  /** @brief Idempotent stop request; false once terminal outcome is latched. */
  bool request_cancel() const noexcept;
  /** @brief Retain diagnostics without retaining this Run or its Results. */
  ExecutionObservation observation() const noexcept;
  /** @brief Wait for task quiescence and observer settlement.
   * Concurrent joins return shared immutable Result owners. Cancellation wins
   * over stale source state and operational failure before terminal latch.
   * Retained failure text is bounded to 256 bytes; structured detail is kept.
   * If copying a terminal failure's diagnostic exhausts memory, the returned
   * status preserves its code, reason and detail with an empty message.
   * Default handles and joining from this Run's observer fail InvalidArgument.
   */
  Result<ExecutionResult> join() const;

 private:
  friend class ExecutionContext;
  std::shared_ptr<execution_internal::RunState> state_;
};
/** @brief Owner of bounded worker resources and the frozen operation registry.
 * Direct methods must not race destruction. Destruction rejects submissions,
 * requests cancellation and waits for every accepted Run and observer to drain.
 * Returned Result owners retain their storage and accounting independently.
 */
class PHOTOSPIDER_API ExecutionContext final {
 public:
  explicit ExecutionContext(std::shared_ptr<OperationRegistry> operations,
                            ExecutionContextConfig config = {});
  ~ExecutionContext() noexcept;
  ExecutionContext(const ExecutionContext&) = delete;
  ExecutionContext& operator=(const ExecutionContext&) = delete;
  /** @brief Bind direct kernels to the exact compiled placement.
   * Stale/foreign/default Plans fail Stale. Unsupported placements fail
   * BackendUnavailable. The immutable Plan body is shared; new host tables,
   * bridge metadata and kernel wrappers are admitted under this context root.
   * Capacity/allocation exhaustion returns ResourceExhausted with partial
   * preparation released. No lowering, topology change or fallback occurs.
   */
  Result<PreparedPlan> prepare(const ExecutionPlan& plan) const;
  /** @brief Preflight immutable bindings then start one independent Run.
   * Context/run limits or metadata/payload admission may fail with
   * ResourceExhausted. Callback exceptions never escape the Run boundary.
   */
  Result<RunHandle> submit(const PreparedPlan& plan,
                           ExecutionBindings bindings = {},
                           const CancellationToken& cancellation = {},
                           ExecutionOptions options = {});
  Result<ResourceBudget> resource_budget() const;
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
