#pragma once

/**
 * @file execution_service_run_state.hpp
 * @brief Private Run, policy, and operation-gate state for ExecutionService.
 *
 * This header is source-private and is included only by ExecutionService
 * implementation translation units. It does not extend the installed ABI.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "compute/execution/execution_service.hpp"
#include "compute/execution/i2_metal_acquisition_deadline.hpp"
#include "compute/execution/resource_demand_estimator.hpp"
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
#include "compute/execution/execution_service_test_probe.hpp"
#endif
#include "execution/device/compute_io_executor.hpp"
#include "execution/device/device_execution_context.hpp"
#include "execution/device/device_executor_registry.hpp"
#include "execution/physical_execution_routes.hpp"
#include "photospider/core/graph_error.hpp"
#include "photospider/host/host.hpp"
#include "policy/policy_registry.hpp"

namespace ps::compute {

namespace execution_service_detail {}
using namespace execution_service_detail;  // NOLINT(build/namespaces)

namespace execution_service_detail {

/**
 * @brief Completes one irreversible shutdown action without recoverable unwind.
 * @tparam Action Join or trusted telemetry operation.
 * @param action Callable completing the already committed shutdown transition.
 * @return Nothing.
 * @throws Nothing; failure terminates because unwinding could destroy still
 * joinable workers or report a recoverable state after admission closed.
 * @note Callers hold no pool, Run, policy, route, or lifecycle lock.
 */
template <typename Action>
void complete_shutdown_action_or_terminate(Action&& action) noexcept {
  try {
    std::forward<Action>(action)();
  } catch (...) {
    std::terminate();
  }
}

/**
 * @brief Joins and accounts every worker transferred out of PoolState.
 *
 * @throws Nothing from construction, completion, and destruction; join or
 * telemetry failure terminates after the irreversible worker transfer.
 * @note The guard is armed immediately after local worker ownership exists.
 * Its destructor prevents any future recoverable validation or refactor from
 * unwinding through a joinable std::thread.
 */
class ShutdownWorkerJoinGuard final {
 public:
  /**
   * @brief Binds local workers to their exact shutdown telemetry generation.
   * @param workers Local worker owners transferred from PoolState.
   * @param registry Stable lifecycle counter/generation authority.
   * @throws Nothing.
   * @note workers and registry must outlive this stack guard.
   */
  ShutdownWorkerJoinGuard(std::vector<std::thread>& workers,
                          RunLifecycleRegistry& registry) noexcept
      : workers_(workers), registry_(registry) {}

  /**
   * @brief Joins and publishes every not-yet-accounted local worker.
   * @return Nothing.
   * @throws Nothing; join or telemetry failure terminates.
   * @note Repeated calls after successful completion are harmless.
   */
  void complete() noexcept {
    if (!armed_) {
      return;
    }
    while (next_worker_ < workers_.size()) {
      std::thread& worker = workers_[next_worker_];
      if (worker.joinable()) {
        complete_shutdown_action_or_terminate([&worker]() { worker.join(); });
      }
      complete_shutdown_action_or_terminate([this]() {
        registry_.publish_physical_retirement(
            ExecutionLifecycleEventKind::WorkerJoined,
            ExecutionLifecycleCategory::None, registry_.shutdown_generation());
      });
      ++next_worker_;
    }
    armed_ = false;
  }

  /**
   * @brief Completes any remaining local worker ownership before unwind.
   * @throws Nothing; join or telemetry failure terminates.
   * @note This is the final fence against std::thread destructor termination.
   */
  ~ShutdownWorkerJoinGuard() noexcept { complete(); }

  /**
   * @brief Prevents duplicating worker-recovery authority.
   * @param other Unused source because construction is forbidden.
   * @throws Nothing; this operation is deleted.
   */
  ShutdownWorkerJoinGuard(const ShutdownWorkerJoinGuard&) = delete;
  /**
   * @brief Prevents assigning duplicate worker-recovery authority.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing; this operation is deleted.
   */
  ShutdownWorkerJoinGuard& operator=(const ShutdownWorkerJoinGuard&) = delete;

 private:
  /** @brief Local worker vector that outlives this guard. */
  std::vector<std::thread>& workers_;
  /** @brief Stable shutdown generation and logical counter authority. */
  RunLifecycleRegistry& registry_;
  /** @brief First worker whose join event is not yet complete. */
  std::size_t next_worker_ = 0U;
  /** @brief True until every local worker is joined and accounted. */
  bool armed_ = true;
};

/**
 * @brief Adapts one started ready submission to stack-bounded device entry.
 *
 * @throws Provider exceptions unchanged from `run()`.
 * @note The worker owns both borrowed objects through synchronous executor
 * return. The adapter creates no queue, completion, grant, or heap owner.
 */
class ReadySubmissionDeviceInvocation final
    : public execution::DeviceExecutorInvocation {
 public:
  /**
   * @brief Binds one worker-owned submission and its existing runtime.
   * @param submission Started submission retained by the worker QueueEntry.
   * @param runtime Matching execution service completion boundary.
   * @param resource_ledger Service-owned device accounting authority.
   * @throws Nothing.
   */
  ReadySubmissionDeviceInvocation(ReadyTaskSubmission& submission,
                                  ExecutionTaskRuntime& runtime,
                                  ResourceLedger& resource_ledger) noexcept
      : submission_(submission),
        runtime_(runtime),
        resource_ledger_(resource_ledger) {}

  /** @copydoc execution::DeviceExecutorInvocation::run */
  void run() override { submission_.execute(runtime_); }

  /** @copydoc execution::DeviceExecutorInvocation::resource_ledger */
  ResourceLedger& resource_ledger() noexcept override {
    return resource_ledger_;
  }

  /** @copydoc execution::DeviceExecutorInvocation::completion_seed */
  std::optional<execution::DeviceCompletionSeed> completion_seed()
      const override {
    const ReadyTaskMetadata& metadata = submission_.metadata();
    return execution::DeviceCompletionSeed(
        metadata.graph_instance_id().value(), metadata.target_node_id(),
        metadata.supersession().key.request_intent(),
        metadata.supersession().generation.value(),
        submission_.identity().run_id().value(),
        submission_.identity().local_task_id().value());
  }

 private:
  /** @brief Worker-owned submission borrowed through executor return. */
  ReadyTaskSubmission& submission_;

  /** @brief Existing Run-scoped task runtime used by provider execution. */
  ExecutionTaskRuntime& runtime_;

  /** @brief Service-owned ledger borrowed through synchronous executor entry.
   */
  ResourceLedger& resource_ledger_;
};

/**
 * @brief Adapts one explicit Host-to-Metal Value upload to executor entry.
 *
 * @throws Native publication and allocation failures unchanged from `run()`.
 * @note The invocation is stack-bounded, borrows the service ledger, and
 * retains the source/seed only through synchronous executor callback return.
 * Native completion ownership moves into the published pending Value.
 */
class HostToMetalValueInvocation final
    : public execution::DeviceExecutorInvocation {
 public:
  /**
   * @brief Captures one exact upload request and its output destination.
   * @param source Ready source Value retained by copy.
   * @param width Positive logical image width.
   * @param height Positive logical image height.
   * @param seed Exact current request/Run lineage.
   * @param capture_deadline Exclusive absolute I2 capture deadline.
   * @param resource_ledger Process ResourceLedger borrowed through entry.
   * @throws Nothing beyond Value copying.
   */
  HostToMetalValueInvocation(
      Value source, std::uint32_t width, std::uint32_t height,
      execution::DeviceCompletionSeed seed,
      std::chrono::steady_clock::time_point capture_deadline,
      ResourceLedger& resource_ledger) noexcept
      : source_(std::move(source)),
        width_(width),
        height_(height),
        seed_(std::move(seed)),
        capture_deadline_(capture_deadline),
        resource_ledger_(resource_ledger) {}

  /** @copydoc execution::DeviceExecutorInvocation::run */
  void run() override {
    execution::MetalExecutionContext& context =
        execution::require_current_metal_execution_context();
    context.publish_float32_host_to_texture(source_, width_, height_);
    published_value_ = context.take_published_value();
    if (!published_value_.valid()) {
      throw std::logic_error(
          "Metal upload returned without a pending resident Value.");
    }
  }

  /** @copydoc execution::DeviceExecutorInvocation::resource_ledger */
  ResourceLedger& resource_ledger() noexcept override {
    return resource_ledger_;
  }

  /** @copydoc execution::DeviceExecutorInvocation::completion_seed */
  std::optional<execution::DeviceCompletionSeed> completion_seed()
      const override {
    return seed_;
  }

  /** @copydoc execution::DeviceExecutorInvocation::execution_deadline */
  std::optional<std::chrono::steady_clock::time_point> execution_deadline()
      const noexcept override {
    return capture_deadline_;
  }

  /**
   * @brief Takes the pending device Value published by `run()`.
   * @return Published Value, or invalid sentinel before/after the sole take.
   * @throws Nothing.
   */
  Value take_published_value() noexcept {
    Value result;
    std::swap(result, published_value_);
    return result;
  }

 private:
  /** @brief Ready host-visible source retained through executor return. */
  Value source_;
  /** @brief Logical image width forwarded to Metal translation. */
  std::uint32_t width_ = 0U;
  /** @brief Logical image height forwarded to Metal translation. */
  std::uint32_t height_ = 0U;
  /** @brief Exact native-completion request and Run lineage. */
  execution::DeviceCompletionSeed seed_;
  /** @brief Unchanged exclusive absolute deadline for every Metal checkpoint.
   */
  std::chrono::steady_clock::time_point capture_deadline_;
  /** @brief Process resource authority borrowed through this invocation. */
  ResourceLedger& resource_ledger_;
  /** @brief Pending device Value taken after synchronous executor return. */
  Value published_value_;
};

/**
 * @brief Type-erased non-inline fence continuation admission.
 *
 * @throws std::bad_alloc while copying the admission callable at construction.
 * @note submit() contains all admission failures and delegates exact Run
 * failure publication to the captured service operation.
 */
class ServiceReadyFenceExecutor final : public ReadyFenceExecutor {
 public:
  /** @brief Nonthrowing service admission callable. */
  using Submitter = std::function<void(Task)>;
  /** @brief Nonthrowing pending-continuation retirement callable. */
  using Releaser = std::function<void()>;

  /**
   * @brief Owns one exact Run-scoped admission callable.
   * @param submitter Nonempty callable that never propagates.
   * @param releaser Nonempty callable that retires one pending continuation.
   * @throws std::invalid_argument for an empty callable.
   * @throws std::bad_alloc when callable ownership cannot allocate.
   */
  ServiceReadyFenceExecutor(Submitter submitter, Releaser releaser)
      : submitter_(std::move(submitter)), releaser_(std::move(releaser)) {
    if (!submitter_ || !releaser_) {
      throw std::invalid_argument(
          "Service fence executor requires admission and release callables.");
    }
  }

  /**
   * @brief Retires the Run's pending continuation ownership.
   * @throws Nothing; the supplied releaser contains synchronization failure.
   * @note ReadyFence retains this executor through callback exit, so
   * destruction cannot precede admitted callback completion.
   */
  ~ServiceReadyFenceExecutor() noexcept override {
    try {
      releaser_();
    } catch (...) {
    }
  }

  /** @copydoc ReadyFenceExecutor::submit */
  void submit(Task task) noexcept override {
    try {
      submitter_(std::move(task));
    } catch (...) {
    }
  }

 private:
  /** @brief Exact Run-scoped service admission operation. */
  Submitter submitter_;
  /** @brief Exact-once Run pending-continuation retirement operation. */
  Releaser releaser_;
};

/**
 * @brief Tests a frozen private-route inventory for one selected device.
 * @param execution_type Exact private route id.
 * @param metal_registered Registry capability captured before Run publication.
 * @param device Device frozen with the operation snapshot.
 * @return True only for CPU on every route or registered Metal on
 * `gpu_pipeline`.
 * @throws Nothing.
 * @note The scalar snapshot avoids registry access while service and Run
 * locks are held.
 */
inline bool route_inventory_exposes_device(const std::string& execution_type,
                                           bool metal_registered,
                                           DeviceBackend device) noexcept {
  if (device == DeviceBackend::CPU) {
    return execution::PhysicalExecutionRoutes::is_supported(execution_type);
  }
  return execution_type == "gpu_pipeline" && device == DeviceBackend::Metal &&
         metal_registered;
}

}  // namespace execution_service_detail

/**
 * @brief Owns isolated completion and observation state for one active Run.
 *
 * @throws Nothing from construction after caller-owned values are available.
 * @note The service registry and every queued entry retain shared ownership.
 * The host remains borrowed only until `execute_run()` observes settlement.
 */
struct ExecutionService::RunState final
    : public std::enable_shared_from_this<ExecutionService::RunState> {
  /**
   * @brief Creates one active Run state before queue publication.
   * @param run_id Opaque Run namespace shared by every initial submission.
   * @param graph_identity Stable copied Graph/session fairness identity.
   * @param graph_key Pre-copied key reserved for first Graph policy row.
   * @param qos Explicit immutable service-class, deadline, and weight inputs.
   * @param host_context Borrowed Graph observation target.
   * @param execution_type Private physical route fixed for this Run.
   * @param metal_registered Registry capability captured before publication.
   * @param total_task_count Positive logical completion count.
   * @param task_resources Uniform adapter declaration for every submission.
   * @param ready_task_bytes Complete service-plus-adapter ready charge.
   * @param execution_task_bytes Complete service-plus-adapter retained charge.
   * @param retained_payload_cleanup_lease Optional matching lease used only
   * after physical settlement to clear supplementally admitted payloads.
   * @param run_reservation Complete admitted vector transferred into this Run.
   * @param supplemental_reservations Preallocated inactive retained-only owner
   * slots, or null when no deferred connected payload is possible.
   * @param supplemental_capacity Exact allocated owner-slot count.
   * @throws Nothing.
   */
  RunState(ComputeRunId run_id, std::string graph_identity,
           std::string graph_key, ComputeRunQos qos,
           ExecutionHostContext& host_context, std::string execution_type,
           bool metal_registered, int total_task_count,
           ReadyTaskResourceDemand task_resources,
           std::uint64_t ready_task_bytes, std::uint64_t execution_task_bytes,
           std::optional<ComputeRunLease> retained_payload_cleanup_lease,
           ResourceLedger::Reservation run_reservation,
           std::unique_ptr<PreparedExecutionSharedReservation[]>
               supplemental_reservations,
           std::uint64_t supplemental_capacity) noexcept
      : id(run_id),
        graph(std::move(graph_identity)),
        available_graph_key(std::move(graph_key)),
        policy_qos(std::move(qos)),
        host(&host_context),
        route(std::move(execution_type)),
        route_metal_registered(metal_registered),
        resource_demand(task_resources),
        ready_bytes_per_task(ready_task_bytes),
        execution_retained_bytes_per_task(execution_task_bytes),
        payload_cleanup_lease(std::move(retained_payload_cleanup_lease)),
        reservation(std::move(run_reservation)),
        supplemental_retained_reservations(
            std::move(supplemental_reservations)),
        supplemental_retained_reservation_capacity(supplemental_capacity),
        tasks_to_complete(total_task_count) {}

  /**
   * @brief Tests the immutable route/registry inventory captured for this Run.
   * @param device Device retained by one ready submission.
   * @return True when the frozen inventory exposes the device.
   * @throws Nothing.
   * @note This method performs no registry access and is safe under pool/Run
   * locks during frontier formation and reserved-start revalidation.
   */
  bool exposes_device(DeviceBackend device) const noexcept {
    return route_inventory_exposes_device(route, route_metal_registered,
                                          device);
  }

  /**
   * @brief Tests whether the caller-side Run wait may finish.
   * @return True after successful logical completion, first failure, or
   * accepted cancellation, provided every in-flight callback has drained.
   * @throws Nothing.
   * @note Caller holds `mutex`. Failure and cancellation deliberately make the
   * remaining logical completion count irrelevant, but neither may release
   * the synchronous waiter while a callback is still executing.
   */
  bool settled() const noexcept {
    return in_flight == 0 && pending_fence_continuations == 0 &&
           (cancelled || first_exception != nullptr || tasks_to_complete == 0);
  }

  /** @brief Opaque Run namespace used for route and trace isolation. */
  const ComputeRunId id;

  /** @brief Stable Graph/session identity used only for policy grouping. */
  const std::string graph;

  /**
   * @brief Pre-accounted key moved into a new Graph policy row when needed.
   * @note An existing Graph row leaves this conservative allocation owned by
   * the Run until settlement; admission intentionally accounts it either way.
   */
  std::string available_graph_key;

  /** @brief Explicit QoS inputs used without inferring intent or quality. */
  const ComputeRunQos policy_qos;

  /**
   * @brief Borrowed observation target valid through synchronous settlement.
   */
  ExecutionHostContext* const host;

  /** @brief Immutable private execution route used by every Run callback. */
  const std::string route;

  /** @brief Registry Metal capability frozen before active-Run publication. */
  const bool route_metal_registered;

  /** @brief Nonzero route generation captured when this Run was admitted. */
  const std::uint64_t route_generation = 1U;

  /** @brief Uniform trusted resources required by every logical task. */
  const ReadyTaskResourceDemand resource_demand;

  /** @brief Complete ready-store bytes granted for every logical task. */
  const std::uint64_t ready_bytes_per_task;

  /** @brief Complete retained bytes granted while one task executes. */
  const std::uint64_t execution_retained_bytes_per_task;

  /**
   * @brief Matching lease used only for post-drain payload cleanup.
   * @note Present exactly when the root preallocated supplemental owner slots.
   * The external RunState owns this lease, so it cannot form a Run-plan cycle.
   */
  std::optional<ComputeRunLease> payload_cleanup_lease;

  /**
   * @brief Complete Run vector closed explicitly after synchronous settlement.
   * @note Optional ownership lets `execute_run()` return Host capacity before
   * a worker releases its final non-authoritative `shared_ptr<RunState>`.
   */
  std::optional<ResourceLedger::Reservation> reservation;

  /**
   * @brief Preallocated service owners for deferred retained-memory roots.
   * @note Destruction order releases these roots before the primary root and
   * payload_cleanup_lease. Only the prefix named by the size field is active.
   */
  std::unique_ptr<PreparedExecutionSharedReservation[]>
      supplemental_retained_reservations;

  /** @brief Fixed number of owner slots charged in the initial Run root. */
  const std::uint64_t supplemental_retained_reservation_capacity = 0U;

  /** @brief Active prefix length in supplemental_retained_reservations. */
  std::uint64_t supplemental_retained_reservation_size = 0U;

  /** @brief Guards completion, failure, admission, and in-flight state. */
  mutable std::mutex mutex;

  /** @brief Wakes the one caller waiting for this Run to settle. */
  std::condition_variable settled_cv;

  /** @brief Remaining logical tasks for a successful Run. */
  int tasks_to_complete = 0;

  /** @brief Worker callbacks that have left the service queue but not exited.
   */
  int in_flight = 0;

  /**
   * @brief Fence executors that may still deliver or execute a continuation.
   * @note Settlement waits for zero even after failure or cancellation, keeping
   * the borrowed service/runtime alive through every asynchronous callback.
   */
  int pending_fence_continuations = 0;

  /** @brief Successful starts committed for this Run. */
  std::uint64_t committed_starts = 0U;

  /** @brief Exact first callback exception, or null before failure. */
  std::exception_ptr first_exception;

  /** @brief Whether accepted ComputeRun cancellation closed this admission. */
  bool cancelled = false;

  /** @brief Stable first cancellation reason when `cancelled` is true. */
  std::optional<ComputeRunCancellationReason> cancellation_reason;

  /** @brief Whether dependency release may publish additional ready work. */
  bool accepting = true;

  /**
   * @brief Whether this physical phase owns the active ready-store Run row.
   *
   * @note Multiple provider-free phases may be prepared for one ComputeRun id.
   * Cancellation marks every phase, but only the currently published phase may
   * erase that id from the ready store.
   */
  bool published = false;
};

/**
 * @brief Move-owned service queue entry paired with matching Run state.
 *
 * @throws Nothing while moved after caller allocation succeeds.
 * @note Queue storage owns the complete submission and therefore its Run lease.
 */
struct ExecutionService::QueueEntry final {
  /**
   * @brief Transfers one ready submission into service queue ownership.
   * @param run_state Matching active Run retained through callback exit.
   * @param ready_submission Dependency-ready owned work.
   * @param grant Exact ready-entry/byte authority for this store value.
   * @param service_cost Checked ordering-only work/byte charge.
   * @throws Nothing after argument evaluation.
   */
  QueueEntry(std::shared_ptr<RunState> run_state,
             ReadyTaskSubmission ready_submission, ResourceLedger::Grant grant,
             std::uint64_t service_cost) noexcept
      : run(std::move(run_state)),
        priority(ready_submission.priority()),
        submission(std::move(ready_submission)),
        ready_grant(std::move(grant)),
        policy_service_cost(service_cost) {}

  /** @brief Matching active Run state. */
  std::shared_ptr<RunState> run;

  /** @brief Queue selection hint captured before submission movement. */
  ExecutionTaskPriority priority = ExecutionTaskPriority::Normal;

  /** @brief Complete owned callback, identity, metadata, and lease. */
  ReadyTaskSubmission submission;

  /** @brief Ready-store authority released after worker removal or purge. */
  std::optional<ResourceLedger::Grant> ready_grant;

  /** @brief CPU/memory/scratch authority held across callback execution. */
  std::optional<ResourceLedger::Grant> execution_grant;

  /** @brief Whether reserved start acquired operation-gate ownership. */
  bool operation_gate_started = false;

  /**
   * @brief Real evidence-startability/grant facts bound to the physical start.
   * @note The worker publishes this immutable observation only after releasing
   * the pool, Run-state, and terminal-arbiter mutexes.
   */
  std::optional<ComputeRunServiceStartObservation> service_start_observation;

  /**
   * @brief Coordinate committed with the physical route start, when observed.
   * @note The worker publishes this immutable fact only after releasing the
   * service pool, service Run-state, and Run terminal-arbiter mutexes.
   */
  std::optional<ComputeRunObservationCoordinate> service_start_coordinate;

  /** @brief Checked work plus ready-byte quanta used only for ordering. */
  const std::uint64_t policy_service_cost;

  /** @brief Nonreused service-lifetime identity assigned at publication. */
  std::uint64_t candidate_id = 0U;

  /** @brief Nonreused private version protecting remove/reinsert ABA. */
  std::uint64_t entry_version = 0U;

  /**
   * @brief Workers that transiently observed execution-grant exhaustion.
   *
   * @note The service mutex protects this cycle-local mask. A set bit changes
   * only candidate visibility for that worker; it never removes ready
   * ownership or advances policy/fairness accounting.
   */
  std::uint16_t grant_blocked_worker_mask = 0U;

  /** @brief Stable successful-dispatch count observed at publication. */
  std::uint64_t enqueued_class_dispatch_count = 0U;

  /** @brief Stable total enqueue order used as the final policy tie break. */
  std::uint64_t enqueue_sequence = 0U;

  /** @brief Previous same-Run entry in this priority lane, or null. */
  QueueEntry* run_previous = nullptr;

  /** @brief Next same-Run entry in this priority lane, or null. */
  QueueEntry* run_next = nullptr;

  /** @brief True while linked in the same-Run high-priority lane. */
  bool high_lane = false;

  /** @brief Iterator into the store-owned entry list while published. */
  std::list<std::shared_ptr<QueueEntry>>::iterator store_position;

  /** @brief True while `store_position` and intrusive lane links are valid. */
  bool store_owned = false;
};

/**
 * @brief Exact-once rendezvous between provider return and fence readiness.
 *
 * @throws std::system_error when synchronization fails and std::bad_alloc when
 * a callback is parked.
 * @note The gate retains the Run and lease until either no continuation was
 * registered or the continuation reaches ordinary ready-store ownership. It
 * owns no resource grant, native allocation, or worker.
 */
struct ExecutionService::FenceContinuationGate final {
  /**
   * @brief Captures exact continuation routing before fence registration.
   * @param active_run Matching service Run.
   * @param active_lease Matching non-forgeable Run lease.
   * @param active_identity Exact Run/local task identity.
   * @param active_trace_node_id Planned node for diagnostics.
   * @throws Nothing after argument evaluation.
   */
  FenceContinuationGate(std::shared_ptr<RunState> active_run,
                        ComputeRunLease active_lease,
                        ComputeRunTaskIdentity active_identity,
                        int active_trace_node_id) noexcept
      : run(std::move(active_run)),
        lease(std::move(active_lease)),
        identity(active_identity),
        trace_node_id(active_trace_node_id) {}

  /** @brief Protects retirement, submission, and parked callback state. */
  std::mutex mutex;
  /** @brief Matching active Run retained through continuation admission. */
  std::shared_ptr<RunState> run;
  /** @brief Matching Run lease copied into the eventual ready submission. */
  ComputeRunLease lease;
  /** @brief Exact Run/local task identity resumed by the callback. */
  ComputeRunTaskIdentity identity;
  /** @brief Planned node used for continuation trace metadata. */
  int trace_node_id = -1;
  /** @brief True after the original QueueEntry fully retired. */
  bool original_retired = false;
  /** @brief True after ReadyFence delivered its callback exactly once. */
  bool callback_delivered = false;
  /** @brief Callback parked until original_retired becomes true. */
  std::optional<ReadyFenceExecutor::Task> parked_task;
};

/**
 * @brief Complete checked service admission calculation for one CPU Run.
 *
 * @throws Nothing for value movement after successful calculation.
 */
struct ExecutionService::CpuRunAdmissionEstimate final {
  /** @brief Complete root vector reserved before any service allocation. */
  ResourceVector resources;

  /** @brief Uniform service-plus-adapter bytes for one queued submission. */
  std::uint64_t ready_bytes_per_task = 0U;

  /** @brief Uniform service-plus-adapter bytes for one executing submission. */
  std::uint64_t execution_retained_bytes_per_task = 0U;

  /** @brief Actual copied Graph identity retained by the Run policy row. */
  std::string policy_graph_identity;

  /** @brief Actual copied key available for a new Graph policy row. */
  std::string policy_graph_key;
};

/**
 * @brief Authority-free comparison strategy for one explicit QoS class.
 *
 * @throws Nothing from policy comparison or scalar normalization.
 * @note Implementations retain no ready entry, worker, Run, Graph, grant,
 * reservation, executor, completion route, or lifecycle state.
 */
class ExecutionService::BuiltinPolicy {
 public:
  /**
   * @brief Immutable candidate values supplied by the owning ready store.
   * @throws Nothing for value construction and copying.
   */
  struct Candidate final {
    /** @brief Optional absolute monotonic deadline from explicit Run QoS. */
    std::optional<std::chrono::steady_clock::time_point> deadline;

    /** @brief Graph service after selecting this candidate. */
    std::uint64_t graph_score = 0U;

    /** @brief Weight-normalized Run service after this candidate. */
    std::uint64_t run_score = 0U;

    /** @brief Stable final tie break assigned by the store. */
    std::uint64_t enqueue_sequence = 0U;
  };

  /**
   * @brief Releases one stateless built-in strategy through the private base.
   * @throws Nothing.
   * @note The virtual lifetime ends with the owning ready store. Policies
   * retain no entries, Runs, workers, synchronization, or resource authority,
   * so destruction performs no cleanup beyond ordinary object teardown.
   */
  virtual ~BuiltinPolicy() noexcept = default;

  /**
   * @brief Returns the explicit QoS class implemented by this strategy.
   * @return Interactive or Throughput.
   * @throws Nothing.
   */
  virtual ComputeRunQosClass service_class() const noexcept = 0;

  /**
   * @brief Reports whether the class may consume protected headroom.
   * @return True only for the interactive built-in strategy.
   * @throws Nothing.
   */
  virtual bool may_consume_headroom() const noexcept = 0;

  /**
   * @brief Orders two non-aged candidates from this service class.
   * @param lhs Candidate under consideration.
   * @param rhs Current best candidate.
   * @return True when lhs must precede rhs.
   * @throws Nothing.
   */
  virtual bool precedes(const Candidate& lhs,
                        const Candidate& rhs) const noexcept = 0;

  /**
   * @brief Normalizes one positive checked cost by a positive Run weight.
   * @param service_cost Raw work plus ready-byte-quanta cost.
   * @param weight Positive immutable QoS weight.
   * @return Ceiling division, always at least one.
   * @throws Nothing; zero inputs indicate a service invariant violation and
   * terminate.
   */
  std::uint64_t normalized_cost(std::uint64_t service_cost,
                                std::uint32_t weight) const noexcept {
    if (service_cost == 0U || weight == 0U) {
      std::terminate();
    }
    const std::uint64_t divisor = static_cast<std::uint64_t>(weight);
    return service_cost / divisor + (service_cost % divisor == 0U ? 0U : 1U);
  }
};

/**
 * @brief Deadline-aware interactive ordering strategy.
 *
 * @throws Nothing.
 * @note Explicit service class, not compute intent or output quality, selects
 * this policy.
 */
class ExecutionService::InteractiveBuiltinPolicy final
    : public ExecutionService::BuiltinPolicy {
 public:
  /** @copydoc BuiltinPolicy::service_class */
  ComputeRunQosClass service_class() const noexcept override {
    return ComputeRunQosClass::Interactive;
  }

  /** @copydoc BuiltinPolicy::may_consume_headroom */
  bool may_consume_headroom() const noexcept override { return true; }

  /** @copydoc BuiltinPolicy::precedes */
  bool precedes(const Candidate& lhs,
                const Candidate& rhs) const noexcept override {
    if (lhs.deadline.has_value() != rhs.deadline.has_value()) {
      return lhs.deadline.has_value();
    }
    if (lhs.deadline.has_value() && lhs.deadline != rhs.deadline) {
      return *lhs.deadline < *rhs.deadline;
    }
    if (lhs.graph_score != rhs.graph_score) {
      return lhs.graph_score < rhs.graph_score;
    }
    if (lhs.run_score != rhs.run_score) {
      return lhs.run_score < rhs.run_score;
    }
    return lhs.enqueue_sequence < rhs.enqueue_sequence;
  }
};

/**
 * @brief Deterministic weighted throughput ordering strategy.
 *
 * @throws Nothing.
 * @note This policy is confined to general capacity and cannot consume the
 * composition-root interactive headroom.
 */
class ExecutionService::ThroughputBuiltinPolicy final
    : public ExecutionService::BuiltinPolicy {
 public:
  /** @copydoc BuiltinPolicy::service_class */
  ComputeRunQosClass service_class() const noexcept override {
    return ComputeRunQosClass::Throughput;
  }

  /** @copydoc BuiltinPolicy::may_consume_headroom */
  bool may_consume_headroom() const noexcept override { return false; }

  /** @copydoc BuiltinPolicy::precedes */
  bool precedes(const Candidate& lhs,
                const Candidate& rhs) const noexcept override {
    if (lhs.graph_score != rhs.graph_score) {
      return lhs.graph_score < rhs.graph_score;
    }
    if (lhs.run_score != rhs.run_score) {
      return lhs.run_score < rhs.run_score;
    }
    return lhs.enqueue_sequence < rhs.enqueue_sequence;
  }
};

#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
namespace execution_service_detail {

/**
 * @brief Allocation-free storage for one test-product start attempt.
 * @throws Nothing for atomic initialization and access.
 */
struct ReservedStartProbeAttempt final {
  /** @brief Ready-entry candidate identity. */
  std::atomic<std::uint64_t> candidate_id{0U};
  /** @brief Nonreused ready-entry version. */
  std::atomic<std::uint64_t> entry_version{0U};
  /** @brief Immutable copied route generation. */
  std::atomic<std::uint64_t> route_generation{0U};
  /** @brief Staged CPU execution slots. */
  std::atomic<std::uint64_t> cpu_slots{0U};
  /** @brief Staged retained-memory bytes. */
  std::atomic<std::uint64_t> retained_memory_bytes{0U};
  /** @brief Staged scratch bytes. */
  std::atomic<std::uint64_t> scratch_bytes{0U};
  /** @brief Staged ready-entry units. */
  std::atomic<std::uint64_t> ready_entries{0U};
  /** @brief Staged ready bytes. */
  std::atomic<std::uint64_t> ready_bytes{0U};
};

/**
 * @brief Process-local state compiled only into the non-installed test product.
 * @throws Nothing for atomic access.
 * @note GoogleTest executes the owning case with one isolated service; no
 * production object stores or references this state.
 */
struct ReservedStartProbeState final {
  /** @brief Enables the single deterministic first-attempt rollback. */
  std::atomic_bool armed{false};
  /** @brief Total attempts observed while armed. */
  std::atomic<std::uint64_t> calls{0U};
  /** @brief First two fixed-size observations. */
  ReservedStartProbeAttempt attempts[2];
};

/**
 * @brief Process-local post-coordinate route-commit failure test state.
 * @throws Nothing for atomic initialization and access.
 * @note The state is compiled only into the internal test product and owns no
 * service, Run, route, reservation, or callback authority.
 */
struct RouteCommitFailureProbeState final {
  /** @brief True while one route-commit rejection remains armed. */
  std::atomic_bool armed{false};
  /** @brief True after the armed rejection has been consumed. */
  std::atomic_bool triggered{false};
};

/**
 * @brief Process-local start-arbitration observer state for test products.
 * @throws Nothing for atomic initialization and access.
 * @note The callback may coordinate a bounded fixture but owns no service,
 * Run, route, gate, grant, or cancellation authority.
 */
struct ServiceStartArbitrationProbeState final {
  /** @brief Borrowed opaque fixture context. */
  std::atomic<void*> context{nullptr};
  /** @brief Optional allocation-free checkpoint callback. */
  std::atomic<testing::ServiceStartArbitrationObserver> observer{nullptr};
};

/**
 * @brief Process-local operation-admission observer state for test products.
 * @throws Nothing for atomic initialization and access.
 * @note The callback publishes fixture state only. It never owns service,
 * Run, gate, resource, ready-entry, or provider authority.
 */
struct OperationAdmissionWaitProbeState final {
  /** @brief Borrowed opaque fixture context. */
  std::atomic<void*> context{nullptr};
  /** @brief Optional allocation-free notification callback. */
  std::atomic<testing::OperationAdmissionWaitObserver> observer{nullptr};
};

/**
 * @brief Process-local retained operation-string observer state for tests.
 * @throws Nothing for atomic initialization and access.
 * @note The callback only records actual owner capacity and estimator totals.
 * It owns no string, estimator, gate, resource, Run, or service state.
 */
struct RetainedOperationStringChargeProbeState final {
  /** @brief Borrowed opaque fixture context. */
  std::atomic<void*> context{nullptr};
  /** @brief Optional allocation-free observation callback. */
  std::atomic<testing::RetainedOperationStringChargeObserver> observer{nullptr};
};

/**
 * @brief Returns the unique test-product probe state.
 * @return Process-lifetime allocation-free storage.
 * @throws Nothing.
 */
inline ReservedStartProbeState& reserved_start_probe_state() noexcept {
  static ReservedStartProbeState state;
  return state;
}

/**
 * @brief Returns the unique test-product route-commit failure state.
 * @return Process-lifetime allocation-free storage.
 * @throws Nothing.
 */
inline RouteCommitFailureProbeState&
route_commit_failure_probe_state() noexcept {
  static RouteCommitFailureProbeState state;
  return state;
}

/**
 * @brief Returns the unique test-product start-arbitration observer state.
 * @return Process-lifetime allocation-free observer storage.
 * @throws Nothing.
 */
inline ServiceStartArbitrationProbeState&
service_start_arbitration_probe_state() noexcept {
  static ServiceStartArbitrationProbeState state;
  return state;
}

/**
 * @brief Returns the unique test-product admission observer state.
 * @return Process-lifetime allocation-free observer storage.
 * @throws Nothing.
 */
inline OperationAdmissionWaitProbeState&
operation_admission_wait_probe_state() noexcept {
  static OperationAdmissionWaitProbeState state;
  return state;
}

/**
 * @brief Returns the unique test-product retained-string observer state.
 * @return Process-lifetime allocation-free observer storage.
 * @throws Nothing.
 */
inline RetainedOperationStringChargeProbeState&
retained_operation_string_charge_probe_state() noexcept {
  static RetainedOperationStringChargeProbeState state;
  return state;
}

/**
 * @brief Notifies one denied exact-identity or exclusive-key gate start.
 * @param constraints Exact operation declaration rejected by can_start().
 * @return Nothing.
 * @throws Nothing.
 * @note The caller holds the pool mutex. The borrowed callback therefore may
 * only publish atomic fixture state and notify non-service synchronization.
 */
inline void notify_operation_admission_wait_for_testing(
    const OperationExecutionConstraints& constraints) noexcept {
  OperationAdmissionWaitProbeState& state =
      operation_admission_wait_probe_state();
  const testing::OperationAdmissionWaitObserver observer =
      state.observer.load(std::memory_order_acquire);
  if (observer != nullptr) {
    observer(state.context.load(std::memory_order_relaxed),
             constraints.implementation_identity);
  }
}

/**
 * @brief Records a staged child grant and rolls back the first armed attempt.
 * @param candidate_id Current ready-entry identity.
 * @param entry_version Current nonreused ready-entry version.
 * @param route_generation Immutable route generation.
 * @param resources Exact staged execution child grant.
 * @return True only for the first attempt after arming.
 * @throws Nothing.
 * @note This function is absent from the production translation unit. It uses
 * atomics only and cannot call service code while pool and Run locks are held.
 */
inline bool record_reserved_start_attempt_for_testing(
    std::uint64_t candidate_id, std::uint64_t entry_version,
    std::uint64_t route_generation, const ResourceVector& resources) noexcept {
  ReservedStartProbeState& state = reserved_start_probe_state();
  if (!state.armed.load(std::memory_order_acquire)) {
    return false;
  }
  const std::uint64_t index =
      state.calls.fetch_add(1U, std::memory_order_acq_rel);
  if (index < 2U) {
    ReservedStartProbeAttempt& attempt = state.attempts[index];
    attempt.candidate_id.store(candidate_id, std::memory_order_relaxed);
    attempt.entry_version.store(entry_version, std::memory_order_relaxed);
    attempt.route_generation.store(route_generation, std::memory_order_relaxed);
    attempt.cpu_slots.store(resources.cpu_slots, std::memory_order_relaxed);
    attempt.retained_memory_bytes.store(resources.retained_memory_bytes,
                                        std::memory_order_relaxed);
    attempt.scratch_bytes.store(resources.scratch_bytes,
                                std::memory_order_relaxed);
    attempt.ready_entries.store(resources.ready_entries,
                                std::memory_order_relaxed);
    attempt.ready_bytes.store(resources.ready_bytes, std::memory_order_release);
  }
  return index == 0U;
}

/**
 * @brief Consumes one armed rejection after coordinate reservation.
 * @return True exactly once between arm and disarm operations.
 * @throws Nothing.
 * @note The caller holds the Run terminal arbiter and pool/RunState locks; this
 * helper performs only finite lock-free atomic operations.
 */
inline bool consume_route_commit_failure_for_testing() noexcept {
  RouteCommitFailureProbeState& state = route_commit_failure_probe_state();
  if (!state.armed.exchange(false, std::memory_order_acq_rel)) {
    return false;
  }
  state.triggered.store(true, std::memory_order_release);
  return true;
}

/**
 * @brief Notifies one deterministic start-arbitration checkpoint.
 * @param point Exact checkpoint reached by the service worker.
 * @return Nothing.
 * @throws Nothing.
 * @note The callback is test-product-only and must not re-enter service code
 * while the documented production locks remain held.
 */
inline void notify_service_start_arbitration_for_testing(
    testing::ServiceStartArbitrationPoint point) noexcept {
  ServiceStartArbitrationProbeState& state =
      service_start_arbitration_probe_state();
  const testing::ServiceStartArbitrationObserver observer =
      state.observer.load(std::memory_order_acquire);
  if (observer != nullptr) {
    observer(state.context.load(std::memory_order_relaxed), point);
  }
}

}  // namespace execution_service_detail
#endif

/**
 * @brief Tracks exact-identity parallelism and cross-identity exclusive keys.
 *
 * @throws std::bad_alloc when a first active identity or key allocates map
 * storage.
 * @note Every method is called with `PoolState::mutex` held. The gate owns no
 * callback, Run, ready entry, resource grant, key string, or synchronization
 * primitive. A nonempty key row borrows the stable constraint string from the
 * active QueueEntry or `OperationExecutionLeaseState` and is erased before
 * that exact owner retires. Direct acquisition never borrows the caller's
 * input constraints after the lease-state copy has been constructed.
 */
class ExecutionService::OperationStartGate final {
 public:
  /**
   * @brief Tests whether one operation may start without changing ownership.
   * @param constraints Exact constraints copied into a ready submission.
   * @return True when identity capacity and exclusive-key ownership are free.
   * @throws Nothing.
   */
  bool can_start(
      const OperationExecutionConstraints& constraints) const noexcept {
    if (constraints.implementation_identity != 0U) {
      const auto identity =
          identities_.find(constraints.implementation_identity);
      if (identity != identities_.end()) {
        const std::uint64_t limit = effective_limit(constraints);
        if (limit != 0U && identity->second.active >= limit) {
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
          notify_operation_admission_wait_for_testing(constraints);
#endif
          return false;
        }
      }
    }
    if (!constraints.exclusive_key.empty()) {
      const auto key =
          exclusive_keys_.find(std::string_view(constraints.exclusive_key));
      if (key != exclusive_keys_.end() && key->second != 0U) {
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
        notify_operation_admission_wait_for_testing(constraints);
#endif
        return false;
      }
    }
    return true;
  }

  /**
   * @brief Commits one operation start after a successful can_start() check.
   * @param constraints Exact identity/cap/key declaration.
   * @return True when ownership was committed, false when no longer startable.
   * @throws std::logic_error when one active identity presents inconsistent
   * constraints.
   * @throws std::overflow_error when an active counter cannot advance.
   * @throws std::bad_alloc when first-use map storage cannot allocate.
   * @note All allocating map nodes are staged before any active counter
   * changes. A failed key insertion erases a newly staged identity row.
   */
  bool try_start(const OperationExecutionConstraints& constraints) {
    if (!can_start(constraints)) {
      return false;
    }

    bool identity_inserted = false;
    auto identity = identities_.end();
    if (constraints.implementation_identity != 0U) {
      auto inserted = identities_.try_emplace(
          constraints.implementation_identity,
          IdentityState{0U, constraints.reentrant,
                        constraints.maximum_parallelism});
      identity = inserted.first;
      identity_inserted = inserted.second;
      if (!identity_inserted &&
          (identity->second.reentrant != constraints.reentrant ||
           identity->second.maximum_parallelism !=
               constraints.maximum_parallelism)) {
        throw std::logic_error(
            "Active operation identity has inconsistent constraints.");
      }
    }

    auto key = exclusive_keys_.end();
    try {
      if (!constraints.exclusive_key.empty()) {
        key = exclusive_keys_
                  .try_emplace(std::string_view(constraints.exclusive_key), 0U)
                  .first;
      }
    } catch (...) {
      if (identity_inserted) {
        identities_.erase(identity);
      }
      throw;
    }

    if ((identity != identities_.end() &&
         identity->second.active ==
             std::numeric_limits<std::uint64_t>::max()) ||
        (key != exclusive_keys_.end() &&
         key->second == std::numeric_limits<std::uint64_t>::max())) {
      if (identity_inserted) {
        identities_.erase(identity);
      }
      if (key != exclusive_keys_.end() && key->second == 0U) {
        exclusive_keys_.erase(key);
      }
      throw std::overflow_error("Operation start-gate counter exhausted.");
    }

    if (identity != identities_.end()) {
      ++identity->second.active;
    }
    if (key != exclusive_keys_.end()) {
      ++key->second;
    }
    return true;
  }

  /**
   * @brief Releases one previously committed operation start.
   * @param constraints Exact declaration passed to try_start().
   * @return Nothing.
   * @throws Nothing; inconsistent ownership terminates.
   */
  void finish(const OperationExecutionConstraints& constraints) noexcept {
    if (constraints.implementation_identity != 0U) {
      const auto identity =
          identities_.find(constraints.implementation_identity);
      if (identity == identities_.end() || identity->second.active == 0U ||
          identity->second.reentrant != constraints.reentrant ||
          identity->second.maximum_parallelism !=
              constraints.maximum_parallelism) {
        std::terminate();
      }
      --identity->second.active;
      if (identity->second.active == 0U) {
        identities_.erase(identity);
      }
    }
    if (!constraints.exclusive_key.empty()) {
      const auto key =
          exclusive_keys_.find(std::string_view(constraints.exclusive_key));
      if (key == exclusive_keys_.end() || key->second != 1U) {
        std::terminate();
      }
      exclusive_keys_.erase(key);
    }
  }

  /**
   * @brief Reports whether no operation start remains owned.
   * @return True when identity and exclusive-key tables are empty.
   * @throws Nothing.
   */
  bool empty() const noexcept {
    return identities_.empty() && exclusive_keys_.empty();
  }

 private:
  /**
   * @brief Active declaration retained for one exact implementation identity.
   * @throws Nothing for scalar value operations.
   */
  struct IdentityState final {
    /** @brief Currently executing callbacks. */
    std::uint64_t active = 0U;
    /** @brief Frozen reentrancy declaration for this identity. */
    bool reentrant = true;
    /** @brief Frozen positive cap, or zero for unbounded. */
    std::uint32_t maximum_parallelism = 0U;
  };

  /**
   * @brief Resolves the effective callback cap for one declaration.
   * @param constraints Exact operation constraints.
   * @return One for non-reentrant callbacks, the positive explicit cap, or zero
   * for no identity-specific cap.
   * @throws Nothing.
   */
  static std::uint64_t effective_limit(
      const OperationExecutionConstraints& constraints) noexcept {
    if (!constraints.reentrant) {
      return 1U;
    }
    return constraints.maximum_parallelism;
  }

  /** @brief Active callbacks keyed by exact registry identity. */
  std::map<std::uint64_t, IdentityState> identities_;

  /**
   * @brief Active exclusion ownership indexed by borrowed stable key views.
   * @note Each view is erased by finish() before its QueueEntry/direct-lease
   * constraint owner can be destroyed.
   */
  std::map<std::string_view, std::uint64_t> exclusive_keys_;
};

}  // namespace ps::compute
