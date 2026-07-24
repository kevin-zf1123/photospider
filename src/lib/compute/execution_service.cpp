#include "compute/execution_service.hpp"

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
#include <thread>
#include <utility>
#include <vector>

#include "compute/resource_demand_estimator.hpp"
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
#include "compute/execution_service_test_probe.hpp"
#endif
#include "execution/physical_execution_routes.hpp"
#include "photospider/core/graph_error.hpp"
#include "photospider/host/host.hpp"
#include "policy/policy_registry.hpp"

namespace ps::compute {

/** @copydoc operator==(const ReadyTaskResourceDemand&, const
 * ReadyTaskResourceDemand&) */
bool operator==(const ReadyTaskResourceDemand& lhs,
                const ReadyTaskResourceDemand& rhs) noexcept {
  return lhs.retained_memory_bytes == rhs.retained_memory_bytes &&
         lhs.scratch_bytes == rhs.scratch_bytes &&
         lhs.ready_bytes == rhs.ready_bytes && lhs.work_units == rhs.work_units;
}

/** @copydoc operator!=(const ReadyTaskResourceDemand&, const
 * ReadyTaskResourceDemand&) */
bool operator!=(const ReadyTaskResourceDemand& lhs,
                const ReadyTaskResourceDemand& rhs) noexcept {
  return !(lhs == rhs);
}

/** @copydoc owned_callback_resource_demand */
ReadyTaskResourceDemand owned_callback_resource_demand(
    std::uint64_t capture_bytes) {
  const std::uint64_t owned_bytes =
      owned_callable_retained_memory_bytes(capture_bytes);
  return ReadyTaskResourceDemand{owned_bytes, 0U, owned_bytes, 1U};
}

/** @copydoc ReadyTaskMetadata::ReadyTaskMetadata */
ReadyTaskMetadata::ReadyTaskMetadata(const ComputeRunDescriptor& descriptor,
                                     int trace_node_id, bool is_initial_ready,
                                     Device device)
    : run_id_(descriptor.id()),
      graph_identity_(descriptor.graph_identity()),
      graph_instance_id_(descriptor.graph_instance_id()),
      revision_(descriptor.revision()),
      target_node_id_(descriptor.target_node_id()),
      intent_(descriptor.intent()),
      quality_(descriptor.quality()),
      qos_(descriptor.qos()),
      trace_node_id_(trace_node_id),
      device_(device),
      is_initial_ready_(is_initial_ready) {
}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ReadyTaskSubmission::ReadyTaskSubmission */
ReadyTaskSubmission::ReadyTaskSubmission(
    ComputeRunLease lease, ComputeRunTaskIdentity identity, int trace_node_id,
    bool is_initial_ready, Executable executable,
    ExecutionTaskPriority priority, ReadyTaskResourceDemand resource_demand,
    Device device)
    : metadata_(lease.descriptor(), trace_node_id, is_initial_ready, device),
      identity_(identity),
      lease_(std::move(lease)),
      executable_(std::move(executable)),
      priority_(priority),
      resource_demand_(resource_demand) {
  if (identity_.run_id() != metadata_.run_id()) {
    throw std::invalid_argument(
        "ReadyTaskSubmission identity does not match its Run lease.");
  }
  if (!executable_) {
    throw std::invalid_argument(
        "ReadyTaskSubmission requires an owned executable.");
  }
}

/** @copydoc ReadyTaskSubmission::default_resource_demand */
ReadyTaskResourceDemand
ReadyTaskSubmission::default_resource_demand() noexcept {
  return {};
}

/** @copydoc ReadyTaskSubmission::execute */
void ReadyTaskSubmission::execute(ExecutionTaskRuntime& task_runtime) {
  try {
    if (lease_.observe_cancellation().has_value()) {
      return;
    }
    executable_(lease_, identity_, task_runtime);
    (void)lease_.observe_cancellation();
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    try {
      (void)lease_.publish_task_failure(identity_, failure);
    } catch (...) {
    }
    std::rethrow_exception(failure);
  }
}

namespace {

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
   * @param telemetry Stable process execution telemetry.
   * @param registry Stable lifecycle counter/generation authority.
   * @throws Nothing.
   * @note workers, telemetry, and registry must outlive this stack guard.
   */
  ShutdownWorkerJoinGuard(std::vector<std::thread>& workers,
                          ExecutionLifecycleTelemetry& telemetry,
                          RunLifecycleRegistry& registry) noexcept
      : workers_(workers), telemetry_(telemetry), registry_(registry) {}

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
        telemetry_.publish(ExecutionLifecycleEventKind::WorkerJoined,
                           ExecutionLifecycleCategory::None, 0U, 0U, 0U,
                           registry_.shutdown_generation(),
                           registry_.counters());
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
  /** @brief Stable physical lifecycle telemetry owner. */
  ExecutionLifecycleTelemetry& telemetry_;
  /** @brief Stable shutdown generation and logical counter authority. */
  RunLifecycleRegistry& registry_;
  /** @brief First worker whose join event is not yet complete. */
  std::size_t next_worker_ = 0U;
  /** @brief True until every local worker is joined and accounted. */
  bool armed_ = true;
};

/**
 * @brief Tests a frozen private-route inventory for one selected device.
 * @param execution_type Exact private route id.
 * @param metal_available Metal capability captured before Run publication.
 * @param device Device frozen with the operation snapshot.
 * @return True only for CPU on every route or available Metal on
 * `gpu_pipeline`.
 * @throws Nothing.
 * @note The scalar snapshot avoids Host virtual calls while service and Run
 * locks are held.
 */
bool route_inventory_exposes_device(const std::string& execution_type,
                                    bool metal_available,
                                    Device device) noexcept {
  if (device == Device::CPU) {
    return execution::PhysicalExecutionRoutes::is_supported(execution_type);
  }
  return execution_type == "gpu_pipeline" && device == Device::GPU_METAL &&
         metal_available;
}

/**
 * @brief Captures whether one Host-aware private route exposes a device.
 * @param host Borrowed process capability observer.
 * @param execution_type Exact private route id.
 * @param device Device requested for the inventory snapshot.
 * @return True when the route and current Host capability expose the device.
 * @throws Nothing.
 * @note Callers invoke this boundary before acquiring service or Run locks.
 */
bool route_exposes_device(const ExecutionHostContext& host,
                          const std::string& execution_type,
                          Device device) noexcept {
  const bool metal_available = execution_type == "gpu_pipeline" &&
                               host.is_device_available(Device::GPU_METAL);
  return route_inventory_exposes_device(execution_type, metal_available,
                                        device);
}

}  // namespace

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
   * @param metal_available Metal capability captured before publication.
   * @param total_task_count Positive logical completion count.
   * @param task_resources Uniform adapter declaration for every submission.
   * @param ready_task_bytes Complete service-plus-adapter ready charge.
   * @param execution_task_bytes Complete service-plus-adapter retained charge.
   * @param run_reservation Complete admitted vector transferred into this Run.
   * @throws Nothing.
   */
  RunState(ComputeRunId run_id, std::string graph_identity,
           std::string graph_key, ComputeRunQos qos,
           ExecutionHostContext& host_context, std::string execution_type,
           bool metal_available, int total_task_count,
           ReadyTaskResourceDemand task_resources,
           std::uint64_t ready_task_bytes, std::uint64_t execution_task_bytes,
           ResourceLedger::Reservation run_reservation) noexcept
      : id(run_id),
        graph(std::move(graph_identity)),
        available_graph_key(std::move(graph_key)),
        policy_qos(std::move(qos)),
        host(&host_context),
        route(std::move(execution_type)),
        route_metal_available(metal_available),
        resource_demand(task_resources),
        ready_bytes_per_task(ready_task_bytes),
        execution_retained_bytes_per_task(execution_task_bytes),
        reservation(std::move(run_reservation)),
        tasks_to_complete(total_task_count) {}

  /**
   * @brief Tests the immutable route/Host inventory captured for this Run.
   * @param device Device retained by one ready submission.
   * @return True when the frozen inventory exposes the device.
   * @throws Nothing.
   * @note This method performs no Host callback and is safe under pool/Run
   * locks during frontier formation and reserved-start revalidation.
   */
  bool exposes_device(Device device) const noexcept {
    return route_inventory_exposes_device(route, route_metal_available, device);
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
    return in_flight == 0 &&
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

  /** @brief Host Metal capability captured before active-Run publication. */
  const bool route_metal_available;

  /** @brief Nonzero route generation captured when this Run was admitted. */
  const std::uint64_t route_generation = 1U;

  /** @brief Uniform trusted resources required by every logical task. */
  const ReadyTaskResourceDemand resource_demand;

  /** @brief Complete ready-store bytes granted for every logical task. */
  const std::uint64_t ready_bytes_per_task;

  /** @brief Complete retained bytes granted while one task executes. */
  const std::uint64_t execution_retained_bytes_per_task;

  /**
   * @brief Complete Run vector closed explicitly after synchronous settlement.
   * @note Optional ownership lets `execute_run()` return Host capacity before
   * a worker releases its final non-authoritative `shared_ptr<RunState>`.
   */
  std::optional<ResourceLedger::Reservation> reservation;

  /** @brief Guards completion, failure, admission, and in-flight state. */
  mutable std::mutex mutex;

  /** @brief Wakes the one caller waiting for this Run to settle. */
  std::condition_variable settled_cv;

  /** @brief Remaining logical tasks for a successful Run. */
  int tasks_to_complete = 0;

  /** @brief Worker callbacks that have left the service queue but not exited.
   */
  int in_flight = 0;

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
 * @brief Calculates the mandatory structural bytes for one submission.
 * @param graph_identity Stable copied metadata string carried by every task.
 * @return Queue entry, shared owner, ready-store handle, and string bytes.
 * @throws GraphError when checked structural arithmetic overflows.
 * @note The returned envelope excludes adapter-declared capture bytes so they
 * can be added exactly once in the relevant lifecycle dimension. Copied string
 * storage is charged by actual capacity plus its null terminator.
 */
std::uint64_t ExecutionService::service_submission_envelope_bytes(
    const std::string& graph_identity) {
  RetainedMemoryEstimator estimate("ExecutionService submission envelope");
  estimate.add_objects<QueueEntry>();
  estimate.add_objects<std::shared_ptr<QueueEntry>>();
  estimate.add_objects<void*>(2U);
  estimate.add_shared_control_block();
  estimate.add_bytes(static_cast<std::uint64_t>(graph_identity.capacity()));
  estimate.add_bytes(1U);
  return estimate.bytes();
}

/** @copydoc ExecutionService::calculate_policy_service_cost */
std::uint64_t ExecutionService::calculate_policy_service_cost(
    ReadyTaskResourceDemand demand, std::uint64_t complete_ready_bytes) {
  if (demand.work_units == 0U) {
    throw std::invalid_argument(
        "ExecutionService policy work units must be positive.");
  }
  if (complete_ready_bytes == 0U) {
    throw GraphError(GraphErrc::ComputeError,
                     "ExecutionService policy ready bytes must be positive.");
  }

  const std::uint64_t quotient = complete_ready_bytes / kPolicyReadyByteQuantum;
  const std::uint64_t remainder =
      complete_ready_bytes % kPolicyReadyByteQuantum;
  if (remainder != 0U &&
      quotient == std::numeric_limits<std::uint64_t>::max()) {
    throw GraphError(GraphErrc::ComputeError,
                     "ExecutionService policy byte quantum overflow.");
  }
  const std::uint64_t byte_quanta = quotient + (remainder != 0U ? 1U : 0U);
  if (byte_quanta >
      std::numeric_limits<std::uint64_t>::max() - demand.work_units) {
    throw GraphError(GraphErrc::ComputeError,
                     "ExecutionService policy service cost overflow.");
  }
  return demand.work_units + byte_quanta;
}

/**
 * @brief Builds one checked service-plus-adapter CPU Run estimate.
 * @param configured_workers Frozen service worker count.
 * @param graph_identity Copied metadata identity shared by logical tasks.
 * @param total_task_count Positive complete task count.
 * @param maximum_parallelism Optional positive Run callback-concurrency cap.
 * @param demand Shared once-per-Run and uniform per-task declarations.
 * @return Complete root vector and reusable child-grant envelopes.
 * @throws std::invalid_argument for a nonpositive task count.
 * @throws GraphError when any addition or multiplication overflows.
 * @note Retained and scratch task bytes scale with maximum callback
 * concurrency: the minimum of fixed workers, logical tasks, and the optional
 * Run cap. Ready entries/bytes scale with every logical task so dependency
 * release cannot exceed the admitted reservation.
 */
ExecutionService::CpuRunAdmissionEstimate
ExecutionService::calculate_cpu_run_admission(
    unsigned int configured_workers, const std::string& graph_identity,
    int total_task_count, std::optional<std::uint32_t> maximum_parallelism,
    CpuRunResourceDemand demand) {
  if (total_task_count <= 0) {
    throw std::invalid_argument(
        "ExecutionService requires a positive total task count.");
  }

  const std::uint64_t service_task_bytes =
      service_submission_envelope_bytes(graph_identity);
  RetainedMemoryEstimator ready_estimate(
      "ExecutionService per-task ready envelope");
  ready_estimate.add_bytes(service_task_bytes);
  ready_estimate.add_bytes(demand.task.ready_bytes);
  const std::uint64_t ready_bytes_per_task = ready_estimate.bytes();

  RetainedMemoryEstimator execution_estimate(
      "ExecutionService per-task execution envelope");
  execution_estimate.add_bytes(service_task_bytes);
  execution_estimate.add_bytes(demand.task.retained_memory_bytes);
  const std::uint64_t execution_bytes_per_task = execution_estimate.bytes();

  std::string policy_graph_identity(graph_identity);
  std::string policy_graph_key(graph_identity);

  RetainedMemoryEstimator shared_estimate(
      "ExecutionService shared Run envelope");
  shared_estimate.add_bytes(
      service_run_envelope_bytes(policy_graph_identity, policy_graph_key));
  shared_estimate.add_bytes(demand.shared_retained_memory_bytes);

  const std::uint64_t logical_task_count =
      static_cast<std::uint64_t>(total_task_count);
  std::uint64_t concurrent_task_count = std::min(
      static_cast<std::uint64_t>(configured_workers), logical_task_count);
  if (maximum_parallelism.has_value()) {
    concurrent_task_count =
        std::min(concurrent_task_count,
                 static_cast<std::uint64_t>(*maximum_parallelism));
  }

  const std::optional<ResourceVector> execution_resources =
      checked_multiply_resources(
          ResourceVector{0U, execution_bytes_per_task,
                         demand.task.scratch_bytes, 0U, 0U},
          concurrent_task_count);
  const std::optional<ResourceVector> ready_resources =
      checked_multiply_resources(
          ResourceVector{0U, 0U, 0U, 1U, ready_bytes_per_task},
          logical_task_count);
  if (!execution_resources.has_value() || !ready_resources.has_value()) {
    throw GraphError(GraphErrc::ComputeError,
                     "ExecutionService Run resource aggregation overflow.");
  }

  const ResourceVector shared_resources{concurrent_task_count,
                                        shared_estimate.bytes(), 0U, 0U, 0U};
  const std::optional<ResourceVector> with_execution =
      checked_add_resources(shared_resources, *execution_resources);
  const std::optional<ResourceVector> complete =
      with_execution.has_value()
          ? checked_add_resources(*with_execution, *ready_resources)
          : std::nullopt;
  if (!complete.has_value()) {
    throw GraphError(GraphErrc::ComputeError,
                     "ExecutionService Run resource aggregation overflow.");
  }
  return CpuRunAdmissionEstimate{
      *complete,
      ready_bytes_per_task,
      execution_bytes_per_task,
      std::move(policy_graph_identity),
      std::move(policy_graph_key),
  };
}

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
namespace {

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
 * @brief Returns the unique test-product probe state.
 * @return Process-lifetime allocation-free storage.
 * @throws Nothing.
 */
ReservedStartProbeState& reserved_start_probe_state() noexcept {
  static ReservedStartProbeState state;
  return state;
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
bool record_reserved_start_attempt_for_testing(
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

}  // namespace
#endif

/**
 * @brief Owns one policy-aware entry/byte-bounded service ready store.
 *
 * @throws std::bad_alloc when entry or policy-row storage grows.
 * @note Every method is called with `PoolState::mutex` held. The store owns
 * ready values and ordering history but no synchronization, Run admission,
 * dependency, resource-token, worker, completion, or lifecycle authority.
 */
class ExecutionService::BoundedReadyStore final {
 private:
  struct PolicyGraphState;
  struct PolicyRunState;

 public:
  /**
   * @brief Immutable ABI snapshot prepared while the ready store is locked.
   *
   * @throws std::bad_alloc when candidate storage cannot allocate.
   * @note The value contains copied scalar descriptors only. It retains no
   * ready entry, Run, grant, route, or mutation authority across a callback.
   */
  struct PolicySnapshot final {
    /** @brief Host-selected QoS class. */
    ComputeRunQosClass service_class = ComputeRunQosClass::Throughput;

    /** @brief Nonzero generation unique to this immutable snapshot. */
    std::uint64_t snapshot_generation = 0U;

    /** @brief Nonzero sequence unique to this callback attempt. */
    std::uint64_t selection_sequence = 0U;

    /** @brief Exact plugin-visible admissible frontier. */
    std::vector<ps_policy_candidate_v1> candidates;

    /** @brief Whether the bounded ABI callback may consume this value. */
    bool plugin_eligible = false;
  };

  /**
   * @brief Pinned current entry plus Host-authored commit values.
   *
   * @throws Nothing for move/copy operations after the shared pin exists.
   * @note `entry` keeps a purged object alive but does not keep it visible;
   * commit revalidates exact identity and store ownership under the lock.
   */
  struct SelectionPin final {
    /** @brief Exact selected object, or null when no candidate is startable. */
    std::shared_ptr<QueueEntry> entry;

    /** @brief Selected class after current Host arbitration. */
    ComputeRunQosClass service_class = ComputeRunQosClass::Throughput;

    /** @brief Projected Graph charge committed only on successful start. */
    std::uint64_t graph_score = 0U;

    /** @brief Projected normalized Run charge committed only on start. */
    std::uint64_t run_score = 0U;

    /** @brief Captured nonreused candidate identity. */
    std::uint64_t candidate_id = 0U;

    /** @brief Captured nonreused entry version. */
    std::uint64_t entry_version = 0U;

    /** @brief Captured stable enqueue sequence. */
    std::uint64_t enqueue_sequence = 0U;

    /** @brief Captured immutable private route generation. */
    std::uint64_t route_generation = 0U;
  };

  /** @brief Outcome of one allocation-free reserved-start transaction. */
  enum class StartResult {
    /** @brief Entry/grants/fairness/in-flight ownership committed atomically.
     */
    Started,
    /** @brief The pin no longer denotes a current admissible entry. */
    Obsolete,
    /** @brief The admitted root could not mint the execution child grant. */
    GrantUnavailable,
    /** @brief A nonreused counter reached its terminal value. */
    IdentityExhausted,
  };

  /**
   * @brief Fixes aggregate ready-store limits for the service lifetime.
   * @param entry_limit Maximum stored entries across both service classes.
   * @param byte_limit Maximum accounted bytes across both service classes.
   * @param telemetry Stable physical-counter owner outliving this store.
   * @throws Nothing.
   */
  BoundedReadyStore(std::uint64_t entry_limit, std::uint64_t byte_limit,
                    ExecutionLifecycleTelemetry& telemetry) noexcept
      : entry_limit_(entry_limit),
        byte_limit_(byte_limit),
        telemetry_(telemetry) {}

  /**
   * @brief Returns conservative per-Run policy-map structural ownership.
   * @return One Run row, one conservatively charged Graph row, and linkage.
   * @throws GraphError when checked structural arithmetic overflows.
   * @note Graph rows are shared in storage but charged once per Run so
   * admission never depends on another Run retaining the allocation.
   */
  static std::uint64_t run_policy_envelope_bytes() {
    RetainedMemoryEstimator estimate("ExecutionService policy Run envelope");
    estimate.add_objects<std::pair<const std::uint64_t, PolicyRunState>>();
    estimate.add_objects<void*>(3U);
    estimate.add_objects<std::pair<const std::string, PolicyGraphState>>();
    estimate.add_objects<void*>(3U);
    return estimate.bytes();
  }

  /**
   * @brief Returns the stateless strategy for one explicit QoS class.
   * @param service_class Explicit immutable Run class.
   * @return Borrowed strategy owned for the store lifetime.
   * @throws Nothing; an invalid enum terminates as a trusted invariant breach.
   */
  const BuiltinPolicy& policy_for(
      ComputeRunQosClass service_class) const noexcept {
    switch (service_class) {
      case ComputeRunQosClass::Interactive:
        return interactive_policy_;
      case ComputeRunQosClass::Throughput:
        return throughput_policy_;
    }
    std::terminate();
  }

  /**
   * @brief Publishes one fully granted ready entry through policy accounting.
   * @param entry Owned entry with one active ready grant and checked cost.
   * @return True after publication, false without mutation on a local limit
   * or checked-counter/sequence violation.
   * @throws std::invalid_argument when identity, class, cost, or grant shape is
   * structurally invalid.
   * @throws std::bad_alloc when entry or policy-row ownership cannot allocate.
   * @note Successful publication assigns fresh nonreused identities and clears
   * every prior transient worker-cycle grant-block mark before visibility.
   */
  bool try_push(const std::shared_ptr<QueueEntry>& entry) {
    if (!entry) {
      throw std::invalid_argument(
          "Bounded ready store requires one owned entry.");
    }
    validate_entry(*entry);
    const ResourceVector charge = entry->ready_grant->resources();
    const std::optional<ResourceVector> next = checked_add_resources(
        ResourceVector{0U, 0U, 0U, entry_count_, byte_count_}, charge);
    if (!next.has_value() || next->ready_entries > entry_limit_ ||
        next->ready_bytes > byte_limit_ ||
        next_enqueue_sequence_ == std::numeric_limits<std::uint64_t>::max() ||
        next_candidate_id_ == std::numeric_limits<std::uint64_t>::max() ||
        next_entry_version_ == std::numeric_limits<std::uint64_t>::max()) {
      return false;
    }

    bool graph_inserted = false;
    auto graph_it = graph_states_.find(entry->run->graph);
    if (graph_it == graph_states_.end()) {
      if (next_graph_id_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
      }
      auto inserted = graph_states_.try_emplace(
          std::move(entry->run->available_graph_key), next_graph_id_ + 1U);
      graph_it = inserted.first;
      graph_inserted = inserted.second;
      if (!graph_inserted) {
        std::terminate();
      }
      ++next_graph_id_;
    }

    bool run_inserted = false;
    auto run_it = run_states_.end();
    try {
      run_it = run_states_.find(entry->run->id.value());
      if (run_it == run_states_.end()) {
        auto inserted = run_states_.try_emplace(
            entry->run->id.value(), entry->run.get(), &graph_it->second);
        run_it = inserted.first;
        run_inserted = inserted.second;
        if (!run_inserted) {
          std::terminate();
        }
        ++graph_it->second.active_runs;
      }
    } catch (...) {
      if (graph_inserted) {
        graph_states_.erase(graph_it);
      }
      throw;
    }

    PolicyRunState& run_state = run_it->second;
    if (run_state.run != entry->run.get() ||
        run_state.graph != &graph_it->second) {
      std::terminate();
    }

    try {
      entries_.push_back(entry);
    } catch (...) {
      if (run_inserted) {
        --graph_it->second.active_runs;
        run_states_.erase(run_it);
      }
      if (graph_inserted) {
        graph_states_.erase(graph_it);
      }
      throw;
    }

    entry->store_position = std::prev(entries_.end());
    entry->store_owned = true;
    entry->high_lane = entry->priority == ExecutionTaskPriority::High;
    entry->candidate_id = ++next_candidate_id_;
    entry->entry_version = ++next_entry_version_;
    entry->grant_blocked_worker_mask = 0U;
    entry->enqueued_class_dispatch_count =
        class_dispatch_count(entry->run->policy_qos.service_class);
    entry->enqueue_sequence = ++next_enqueue_sequence_;
    link_entry(run_state, *entry);
    entry_count_ = next->ready_entries;
    byte_count_ = next->ready_bytes;
    telemetry_.increment_physical_counter(
        ExecutionLifecyclePhysicalCounter::ReadyEntry);
    return true;
  }

  /**
   * @brief Builds one bounded immutable plugin snapshot from current state.
   * @param worker_id Worker attempting a start.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used only for startability checks.
   * @return Current class/frontier and fresh callback generations; a frontier
   * larger than the ABI bound returns `plugin_eligible == false` and no copied
   * candidate array so the caller uses the full-state built-in path.
   * @throws std::bad_alloc when temporary or result storage cannot allocate.
   * @throws GraphError when a nonreused callback identity is exhausted.
   * @note Caller holds the service/store mutex. No pointer or authority enters
   * the returned ABI records.
   */
  PolicySnapshot make_policy_snapshot(
      int worker_id, execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) {
    PolicySnapshot snapshot;
    const std::optional<ComputeRunQosClass> selected_class =
        choose_class(worker_id, lane, routes);
    if (!selected_class.has_value()) {
      return snapshot;
    }
    snapshot.service_class = *selected_class;
    std::vector<CandidateRecord> frontier =
        build_frontier(*selected_class, worker_id, lane, routes);
    if (frontier.empty()) {
      return snapshot;
    }
    if (frontier.size() > policy::kPolicyCandidateCountMax) {
      return snapshot;
    }
    if (next_snapshot_generation_ ==
            std::numeric_limits<std::uint64_t>::max() ||
        next_selection_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
      throw GraphError(GraphErrc::ComputeError,
                       "ExecutionService policy identity exhausted.");
    }
    snapshot.candidates.reserve(frontier.size());
    for (const CandidateRecord& candidate : frontier) {
      snapshot.candidates.push_back(candidate.abi);
    }
    snapshot.snapshot_generation = ++next_snapshot_generation_;
    snapshot.selection_sequence = ++next_selection_sequence_;
    snapshot.plugin_eligible = true;
    return snapshot;
  }

  /**
   * @brief Recomputes current Host state and pins one returned candidate.
   * @param candidate_id Nonzero identity validated against the original call.
   * @param service_class Original Host-selected class.
   * @param worker_id Worker attempting the reserved start.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used only for startability checks.
   * @return Exact current pin, or an empty value when Host state made the
   * decision obsolete.
   * @throws std::bad_alloc when recomputation storage cannot allocate.
   * @note Caller holds the service/store mutex. Revalidation does not advance
   * callback generations or mutate fairness, burst, ready, Run, or grants.
   */
  SelectionPin resolve_current(
      std::uint64_t candidate_id, ComputeRunQosClass service_class,
      int worker_id, execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) {
    const std::optional<ComputeRunQosClass> current_class =
        choose_class(worker_id, lane, routes);
    if (!current_class.has_value() || *current_class != service_class) {
      return {};
    }
    std::vector<CandidateRecord> frontier =
        build_frontier(service_class, worker_id, lane, routes);
    const auto found =
        std::find_if(frontier.begin(), frontier.end(),
                     [candidate_id](const auto& candidate) {
                       return candidate.abi.candidate_id == candidate_id;
                     });
    return found == frontier.end() ? SelectionPin{} : pin_from(*found);
  }

  /**
   * @brief Chooses the deterministic built-in directly from full Host state.
   * @param worker_id Worker attempting a start.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used only for startability checks.
   * @return Current exact pin or empty when no route is startable.
   * @throws Nothing.
   * @note This allocation-free path is used for sticky faults, oversized ABI
   * frontiers, and Host snapshot-allocation failure. The selected minimum is
   * identical to choosing from the reduced admissible frontier.
   */
  SelectionPin select_builtin_current(
      int worker_id, execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) noexcept {
    const std::optional<ComputeRunQosClass> selected_class =
        choose_class(worker_id, lane, routes);
    if (!selected_class.has_value()) {
      return {};
    }
    const SelectedEntry selected =
        select_from_class(policy_for(*selected_class), worker_id, lane, routes);
    if (selected.entry == nullptr || selected.run_state == nullptr) {
      return {};
    }
    return pin_from(selected);
  }

  /**
   * @brief Commits one Host-owned reserved start without partial mutation.
   * @param pin Exact current object and projected fairness values.
   * @param worker_id Worker that will enter the callback after return.
   * @param lane Fixed physical lane owned by the calling worker.
   * @param routes Host-owned route state updated by the no-throw commit.
   * @return Started, obsolete, unavailable grant, or identity exhaustion.
   * @throws std::system_error only while staging the reservation child grant;
   * exceptional exits precede every ready/fairness/in-flight mutation.
   * @note Caller holds the service/store mutex. This method locks the Run and
   * then its reservation through `try_grant`, preserving the frozen order.
   */
  StartResult commit_start(SelectionPin& pin, int worker_id,
                           execution::PhysicalExecutionLane lane,
                           execution::PhysicalExecutionRoutes& routes) {
    if (!pin.entry || !pin.entry->run) {
      return StartResult::Obsolete;
    }
    const bool throughput_ready =
        has_startable(ComputeRunQosClass::Throughput, worker_id, lane, routes);
    const auto run_found = run_states_.find(pin.entry->run->id.value());
    if (run_found == run_states_.end()) {
      return StartResult::Obsolete;
    }
    PolicyRunState& run_state = run_found->second;
    if (run_state.run != pin.entry->run.get() || run_state.graph == nullptr ||
        !pin_matches(pin, run_state, worker_id, lane, routes)) {
      return StartResult::Obsolete;
    }

    std::lock_guard<std::mutex> run_lock(pin.entry->run->mutex);
    RunState& run = *pin.entry->run;
    if (!route_startable(run, *pin.entry, worker_id, lane, routes) ||
        run.route_generation != pin.route_generation ||
        class_dispatch_count(pin.service_class) ==
            std::numeric_limits<std::uint64_t>::max() ||
        run.committed_starts == std::numeric_limits<std::uint64_t>::max() ||
        (pin.service_class == ComputeRunQosClass::Interactive &&
         throughput_ready &&
         consecutive_interactive_ ==
             std::numeric_limits<std::uint64_t>::max())) {
      return class_dispatch_count(pin.service_class) ==
                         std::numeric_limits<std::uint64_t>::max() ||
                     run.committed_starts ==
                         std::numeric_limits<std::uint64_t>::max()
                 ? StartResult::IdentityExhausted
                 : StartResult::Obsolete;
    }

    const ResourceVector execution_resources{
        1U,
        run.execution_retained_bytes_per_task,
        run.resource_demand.scratch_bytes,
        0U,
        0U,
    };
    if (!run.reservation.has_value()) {
      std::terminate();
    }
    std::optional<ResourceLedger::Grant> staged_grant =
        run.reservation->try_grant(execution_resources);
    if (!staged_grant.has_value()) {
      return StartResult::GrantUnavailable;
    }
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
    if (record_reserved_start_attempt_for_testing(
            pin.candidate_id, pin.entry_version, pin.route_generation,
            execution_resources)) {
      return StartResult::Obsolete;
    }
#endif
    const Device device = pin.entry->submission.metadata().device();
    if (!routes.commit_start(run.route, device)) {
      return StartResult::Obsolete;
    }

    pin.entry->execution_grant.emplace(std::move(*staged_grant));
    remove_entry(*pin.entry, run_state);
    pin.entry->ready_grant.reset();
    run_state.charged_service = pin.run_score;
    run_state.graph->charged_service_for(pin.service_class) = pin.graph_score;
    ++class_dispatch_count(pin.service_class);
    ++run.committed_starts;
    ++run.in_flight;
    if (pin.service_class == ComputeRunQosClass::Interactive &&
        throughput_ready) {
      ++consecutive_interactive_;
    } else {
      consecutive_interactive_ = 0U;
    }
    return StartResult::Started;
  }

  /**
   * @brief Tests whether one worker has any currently startable route.
   * @param worker_id Worker attempting selection.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used only for startability checks.
   * @return True when class arbitration can choose at least one entry.
   * @throws Nothing.
   */
  bool has_startable_work(
      int worker_id, execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) noexcept {
    return choose_class(worker_id, lane, routes).has_value();
  }

  /**
   * @brief Hides one exact grant-exhausted pin from one worker selection cycle.
   * @param pin Candidate/version/enqueue identity returned by current
   * selection.
   * @param worker_id CPU or GPU worker whose next selection must skip the pin.
   * @return True only when the identical store-owned entry was marked.
   * @throws Nothing.
   * @note Caller holds the service/store mutex. The mark is allocation-free,
   * preserves ready ownership and fairness state, and cannot transfer to a
   * replacement entry because all nonreused identities are revalidated.
   */
  bool mark_grant_blocked(const SelectionPin& pin, int worker_id) noexcept {
    if (!pin.entry || !pin.entry->store_owned ||
        pin.entry->candidate_id != pin.candidate_id ||
        pin.entry->entry_version != pin.entry_version ||
        pin.entry->enqueue_sequence != pin.enqueue_sequence ||
        *pin.entry->store_position != pin.entry) {
      return false;
    }
    pin.entry->grant_blocked_worker_mask |= worker_mask(worker_id);
    return true;
  }

  /**
   * @brief Restores every transiently blocked candidate for one worker.
   * @param worker_id CPU or GPU worker starting a fresh selection cycle.
   * @return Nothing.
   * @throws Nothing.
   * @note Caller holds the service/store mutex. Other workers' independent
   * block marks remain unchanged.
   */
  void clear_grant_blocked(int worker_id) noexcept {
    const std::uint16_t mask = worker_mask(worker_id);
    for (const std::shared_ptr<QueueEntry>& entry : entries_) {
      entry->grant_blocked_worker_mask &= static_cast<std::uint16_t>(~mask);
    }
  }

  /**
   * @brief Purges every queued entry belonging to one Run.
   * @param run Matching retained Run state.
   * @return Number of removed entries; the empty policy row remains active.
   * @throws Nothing; an accounting invariant violation terminates.
   */
  std::size_t erase_run(const std::shared_ptr<RunState>& run) noexcept {
    const auto found = run_states_.find(run->id.value());
    if (found == run_states_.end()) {
      return 0U;
    }
    PolicyRunState& state = found->second;
    if (state.run != run.get()) {
      std::terminate();
    }
    std::size_t removed = 0U;
    while (state.high.head != nullptr) {
      remove_entry(*state.high.head, state);
      ++removed;
    }
    while (state.normal.head != nullptr) {
      remove_entry(*state.normal.head, state);
      ++removed;
    }
    return removed;
  }

  /**
   * @brief Retires one settled Run's empty policy state.
   * @param run Matching settled Run removed from the active service index.
   * @return Nothing.
   * @throws Nothing; queued entries or identity mismatch terminate.
   * @note The shared Graph row is removed only after its last active Run.
   */
  void retire_run(const std::shared_ptr<RunState>& run) noexcept {
    const auto found = run_states_.find(run->id.value());
    if (found == run_states_.end()) {
      return;
    }
    PolicyRunState& state = found->second;
    if (state.run != run.get() || state.high.head != nullptr ||
        state.normal.head != nullptr || state.graph == nullptr ||
        state.graph->active_runs == 0U) {
      std::terminate();
    }
    PolicyGraphState* graph = state.graph;
    run_states_.erase(found);
    --graph->active_runs;
    if (graph->active_runs == 0U) {
      const auto graph_found = graph_states_.find(run->graph);
      if (graph_found == graph_states_.end() || &graph_found->second != graph) {
        std::terminate();
      }
      graph_states_.erase(graph_found);
    }
  }

  /**
   * @brief Tests exact physical policy-state ownership for one Run.
   * @param run Matching retained RunState.
   * @return True only when the policy row retains the same raw state.
   * @throws Nothing.
   * @note This is a physical ready-store predicate, not lifecycle admission or
   * close authority.
   */
  bool owns_run(const std::shared_ptr<RunState>& run) const noexcept {
    const auto found = run_states_.find(run->id.value());
    return found != run_states_.end() && found->second.run == run.get();
  }

  /**
   * @brief Reports whether any physical policy row uses one Run identity.
   * @param run_id Exact nonzero Run identity.
   * @return True while initial/dependent publication or settlement retains the
   * policy row.
   * @throws Nothing.
   * @note Used only to reject duplicate physical execution of one globally
   * unique Run; it does not replace RunLifecycleRegistry.
   */
  bool contains_run_id(ComputeRunId run_id) const noexcept {
    return run_states_.find(run_id.value()) != run_states_.end();
  }

  /**
   * @brief Returns current physical policy Run rows.
   * @return Exact row count.
   * @throws Nothing.
   */
  std::uint64_t run_count() const noexcept {
    return static_cast<std::uint64_t>(run_states_.size());
  }

  /**
   * @brief Drops all entries and policy rows during final service teardown.
   * @return Nothing.
   * @throws Nothing; owner destruction is noexcept.
   */
  void clear() noexcept {
    for (std::uint64_t count = 0U; count < entry_count_; ++count) {
      telemetry_.decrement_physical_counter(
          ExecutionLifecyclePhysicalCounter::ReadyEntry);
    }
    entries_.clear();
    run_states_.clear();
    graph_states_.clear();
    entry_count_ = 0U;
    byte_count_ = 0U;
    consecutive_interactive_ = 0U;
  }

  /**
   * @brief Reports whether no ready entry is stored.
   * @return True even if active empty Run policy rows remain.
   * @throws Nothing.
   */
  bool empty() const noexcept { return entry_count_ == 0U; }

  /**
   * @brief Returns current entries across both service classes.
   * @return Exact stored entry count.
   * @throws Nothing.
   */
  std::uint64_t entry_count() const noexcept { return entry_count_; }

  /**
   * @brief Returns current accounted bytes across both service classes.
   * @return Exact stored byte count.
   * @throws Nothing.
   */
  std::uint64_t byte_count() const noexcept { return byte_count_; }

 private:
  /**
   * @brief Returns the fixed bit assigned to one configured physical worker.
   * @param worker_id CPU id in `[0,7]` or the GPU id in `[1,8]`.
   * @return Nonzero bit in the 16-bit per-entry transient mask.
   * @throws Nothing; an out-of-domain trusted id terminates.
   */
  static std::uint16_t worker_mask(int worker_id) noexcept {
    if (worker_id < 0 ||
        worker_id > static_cast<int>(kExecutionWorkerRequestMax)) {
      std::terminate();
    }
    return static_cast<std::uint16_t>(1U << worker_id);
  }

  /**
   * @brief Tests whether one entry is hidden in the current worker cycle.
   * @param entry Current store-owned ready entry.
   * @param worker_id Worker attempting route selection.
   * @return True only after that worker observed grant exhaustion for `entry`.
   * @throws Nothing; invalid internal worker ids terminate.
   */
  static bool is_grant_blocked(const QueueEntry& entry,
                               int worker_id) noexcept {
    return (entry.grant_blocked_worker_mask & worker_mask(worker_id)) != 0U;
  }

  /**
   * @brief Intrusive same-Run FIFO endpoints for one priority hint.
   * @throws Nothing for value construction.
   */
  struct LaneEndpoints final {
    /** @brief Oldest entry in this Run/lane, or null. */
    QueueEntry* head = nullptr;

    /** @brief Newest entry in this Run/lane, or null. */
    QueueEntry* tail = nullptr;
  };

  /**
   * @brief Shared fairness accounting for one stable Graph identity.
   * @throws Nothing for value construction.
   */
  struct PolicyGraphState final {
    /**
     * @brief Creates one Graph fairness row with a nonreused opaque id.
     * @param graph_identity Nonzero service-lifetime Graph identity.
     * @throws Nothing.
     */
    explicit PolicyGraphState(std::uint64_t graph_identity) noexcept
        : graph_id(graph_identity) {}

    /**
     * @brief Returns the class-local raw service accumulator.
     * @param service_class Class already chosen by inter-class arbitration.
     * @return Mutable raw Graph service for only that class.
     * @throws Nothing; an invalid trusted enum terminates.
     * @note Interactive and Throughput history never share this scalar.
     */
    std::uint64_t& charged_service_for(
        ComputeRunQosClass service_class) noexcept {
      switch (service_class) {
        case ComputeRunQosClass::Interactive:
          return interactive_charged_service;
        case ComputeRunQosClass::Throughput:
          return throughput_charged_service;
      }
      std::terminate();
    }

    /** @brief Raw work/byte service charged to Interactive selections. */
    std::uint64_t interactive_charged_service = 0U;

    /** @brief Raw work/byte service charged to Throughput selections. */
    std::uint64_t throughput_charged_service = 0U;

    /** @brief Nonzero opaque identity exposed only as policy snapshot data. */
    const std::uint64_t graph_id;

    /** @brief Active Run policy rows currently sharing this Graph. */
    std::uint64_t active_runs = 0U;
  };

  /**
   * @brief Persistent policy accounting and ready lanes for one active Run.
   * @throws Nothing for scalar construction.
   * @note This row outlives temporary ready emptiness until Run settlement.
   */
  struct PolicyRunState final {
    /**
     * @brief Binds one active Run to its stable Graph policy row.
     * @param active_run Borrowed Run retained by active/entry ownership.
     * @param graph_state Stable map-owned Graph row.
     * @throws Nothing.
     */
    PolicyRunState(RunState* active_run, PolicyGraphState* graph_state) noexcept
        : run(active_run), graph(graph_state) {}

    /** @brief Borrowed active Run; never retained beyond service settlement. */
    RunState* run = nullptr;

    /** @brief Stable shared Graph accounting row. */
    PolicyGraphState* graph = nullptr;

    /**
     * @brief Weight-normalized service charged within this Run's fixed class.
     */
    std::uint64_t charged_service = 0U;

    /** @brief Same-Run high-hint FIFO. */
    LaneEndpoints high;

    /** @brief Same-Run normal-hint FIFO. */
    LaneEndpoints normal;
  };

 public:
  /**
   * @brief Complete detached initial-publication storage for one Run.
   *
   * @throws Nothing from movement/destruction after staging allocations finish.
   * @note Map/list nodes, ready grants, queue entries, and nonreused identities
   * are all owned here before lifecycle installation. No node is visible in the
   * live store until try_publish_prepared_batch().
   */
  struct PreparedBatch final {
    /** @brief Matching Run retained independently from staged entries. */
    std::shared_ptr<RunState> run;
    /** @brief Detached queue-list nodes in initial publication order. */
    std::list<std::shared_ptr<QueueEntry>> entries;
    /** @brief Detached candidate Graph policy row. */
    std::map<std::string, PolicyGraphState> graph_states;
    /** @brief Detached exact Run policy row. */
    std::map<std::uint64_t, PolicyRunState> run_states;
    /** @brief Aggregate ready-entry/byte charge for the initial set. */
    ResourceVector ready_charge;
  };

  /**
   * @brief Allocates every store node for one unpublished initial batch.
   * @param run Matching prepared Run state.
   * @param entries Fully granted queue entries.
   * @return Detached batch with reserved nonreused publication identities.
   * @throws std::invalid_argument for empty/mismatched entries.
   * @throws std::overflow_error when store identities are exhausted.
   * @throws GraphError when aggregate ready charge overflows.
   * @throws std::bad_alloc when detached map/list nodes cannot allocate.
   * @note Caller holds the pool mutex. Identity gaps after staging failure are
   * intentional and preserve non-reuse without publishing any store state.
   */
  PreparedBatch prepare_initial_batch(
      const std::shared_ptr<RunState>& run,
      const std::vector<std::shared_ptr<QueueEntry>>& entries) {
    if (!run || entries.empty()) {
      throw std::invalid_argument(
          "ExecutionService prepared batch requires Run and entries.");
    }
    const std::uint64_t count = static_cast<std::uint64_t>(entries.size());
    if (count >
            std::numeric_limits<std::uint64_t>::max() - next_candidate_id_ ||
        count >
            std::numeric_limits<std::uint64_t>::max() - next_entry_version_ ||
        count > std::numeric_limits<std::uint64_t>::max() -
                    next_enqueue_sequence_ ||
        next_graph_id_ == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error(
          "ExecutionService ready publication identity space is exhausted.");
    }

    PreparedBatch batch;
    batch.run = run;
    batch.graph_states.try_emplace(std::move(run->available_graph_key),
                                   ++next_graph_id_);
    PolicyGraphState* staged_graph = &batch.graph_states.begin()->second;
    batch.run_states.try_emplace(run->id.value(), run.get(), staged_graph);
    for (const std::shared_ptr<QueueEntry>& entry : entries) {
      if (!entry || entry->run.get() != run.get()) {
        throw std::invalid_argument(
            "ExecutionService prepared batch mixes Run ownership.");
      }
      validate_entry(*entry);
      const std::optional<ResourceVector> next_charge = checked_add_resources(
          batch.ready_charge, entry->ready_grant->resources());
      if (!next_charge.has_value()) {
        throw GraphError(
            GraphErrc::ComputeError,
            "ExecutionService prepared ready charge exceeds representation.");
      }
      batch.ready_charge = *next_charge;
      entry->candidate_id = ++next_candidate_id_;
      entry->entry_version = ++next_entry_version_;
      entry->enqueue_sequence = ++next_enqueue_sequence_;
      batch.entries.push_back(entry);
    }
    return batch;
  }

  /**
   * @brief Atomically links one detached initial batch into the live store.
   * @param batch Active detached batch from prepare_initial_batch().
   * @return True after publication, false when current bounded capacity rejects
   * the complete batch without mutation.
   * @throws std::invalid_argument for inactive/foreign/duplicate Run state.
   * @throws std::system_error only if standard container internals violate
   * their allocator/comparator contract.
   * @note Caller holds the pool and matching Run mutexes. All allocating nodes
   * already exist. Graph-row insertion rollback preserves an empty live store
   * if the Run node cannot link.
   */
  bool try_publish_prepared_batch(PreparedBatch& batch) {
    if (!batch.run || batch.entries.empty() ||
        batch.graph_states.size() != 1U || batch.run_states.size() != 1U ||
        batch.run_states.begin()->first != batch.run->id.value()) {
      throw std::invalid_argument(
          "ExecutionService prepared batch is inactive or malformed.");
    }
    if (contains_run_id(batch.run->id)) {
      throw std::invalid_argument(
          "ExecutionService prepared Run id is already active.");
    }
    const std::optional<ResourceVector> next = checked_add_resources(
        ResourceVector{0U, 0U, 0U, entry_count_, byte_count_},
        batch.ready_charge);
    if (!next.has_value() || next->ready_entries > entry_limit_ ||
        next->ready_bytes > byte_limit_) {
      return false;
    }

    bool graph_inserted = false;
    auto graph_it = graph_states_.find(batch.run->graph);
    if (graph_it == graph_states_.end()) {
      graph_states_.merge(batch.graph_states);
      graph_it = graph_states_.find(batch.run->graph);
      if (graph_it == graph_states_.end()) {
        std::terminate();
      }
      graph_inserted = true;
    }

    batch.run_states.begin()->second.graph = &graph_it->second;
    try {
      run_states_.merge(batch.run_states);
    } catch (...) {
      if (graph_inserted) {
        auto graph_node = graph_states_.extract(graph_it);
        batch.graph_states.insert(std::move(graph_node));
      }
      throw;
    }
    const auto run_it = run_states_.find(batch.run->id.value());
    if (run_it == run_states_.end() || !batch.run_states.empty()) {
      std::terminate();
    }
    ++graph_it->second.active_runs;

    const auto first = batch.entries.begin();
    entries_.splice(entries_.end(), batch.entries);
    for (auto entry_it = first; entry_it != entries_.end(); ++entry_it) {
      QueueEntry& entry = **entry_it;
      entry.store_position = entry_it;
      entry.store_owned = true;
      entry.high_lane = entry.priority == ExecutionTaskPriority::High;
      entry.grant_blocked_worker_mask = 0U;
      entry.enqueued_class_dispatch_count =
          class_dispatch_count(entry.run->policy_qos.service_class);
      link_entry(run_it->second, entry);
      telemetry_.increment_physical_counter(
          ExecutionLifecyclePhysicalCounter::ReadyEntry);
    }
    entry_count_ = next->ready_entries;
    byte_count_ = next->ready_bytes;
    batch.run.reset();
    return true;
  }

 private:
  /**
   * @brief Complete mutable selection retained only during one pop call.
   * @throws Nothing for value construction.
   */
  struct SelectedEntry final {
    /** @brief Chosen persistent Run policy row. */
    PolicyRunState* run_state = nullptr;

    /** @brief Chosen store-owned entry. */
    QueueEntry* entry = nullptr;

    /** @brief Graph charged service after this selection. */
    std::uint64_t graph_score = 0U;

    /** @brief Run normalized service after this selection. */
    std::uint64_t run_score = 0U;

    /** @brief Whether dispatch-count aging selected this entry. */
    bool aged = false;
  };

  /**
   * @brief Host-private candidate owner used only while forming a frontier.
   * @throws Nothing after the shared pin and ABI scalar record exist.
   * @note The ABI field is safe to copy to foreign code; pointer fields never
   * leave trusted Host control or survive the current locked operation.
   */
  struct CandidateRecord final {
    /** @brief Matching persistent Run policy row. */
    PolicyRunState* run_state = nullptr;

    /** @brief Exact store-owned entry retained during local computation. */
    std::shared_ptr<QueueEntry> entry;

    /** @brief Complete authority-free ABI descriptor. */
    ps_policy_candidate_v1 abi{};

    /** @brief Raw positive cost used by the Graph admissibility band. */
    std::uint64_t raw_cost = 0U;

    /** @brief Normalized positive cost used by the Run admissibility band. */
    std::uint64_t normalized_cost = 0U;
  };

  /**
   * @brief Adds an ordering-only counter without unsigned wraparound.
   * @param lhs Existing charged service.
   * @param rhs Positive candidate cost.
   * @return Exact sum or the representable ceiling.
   * @throws Nothing.
   * @note Saturation affects ordering only; aged selection preserves progress.
   */
  static std::uint64_t saturating_add(std::uint64_t lhs,
                                      std::uint64_t rhs) noexcept {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
      return std::numeric_limits<std::uint64_t>::max();
    }
    return lhs + rhs;
  }

  /**
   * @brief Converts one optional steady-clock deadline into ABI nanoseconds.
   * @param deadline Optional absolute monotonic time point.
   * @return Finite nanoseconds when present, otherwise the ABI sentinel.
   * @throws Nothing; negative times clamp to zero and large values clamp below
   * the reserved no-deadline sentinel.
   */
  static std::uint64_t deadline_nanoseconds(
      const std::optional<std::chrono::steady_clock::time_point>&
          deadline) noexcept {
    if (!deadline.has_value()) {
      return PS_POLICY_NO_DEADLINE_NS;
    }
    const auto raw = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         deadline->time_since_epoch())
                         .count();
    if (raw <= 0) {
      return 0U;
    }
    return std::min<std::uint64_t>(static_cast<std::uint64_t>(raw),
                                   PS_POLICY_NO_DEADLINE_NS - 1U);
  }

  /**
   * @brief Tests immutable and Run-local conditions for one physical start.
   * @param run Run whose mutex is held by the caller.
   * @param entry Ready entry whose selected device fixes the physical lane.
   * @param worker_id Stable worker id.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used only for startability checks.
   * @return True only for a live, uncancelled, class/route-capable Run.
   * @throws Nothing.
   */
  static bool route_startable(
      const RunState& run, const QueueEntry& entry, int worker_id,
      execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) noexcept {
    if (!run.accepting || run.cancelled || run.first_exception != nullptr) {
      return false;
    }
    if (run.policy_qos.maximum_parallelism.has_value() &&
        static_cast<std::uint64_t>(run.in_flight) >=
            *run.policy_qos.maximum_parallelism) {
      return false;
    }
    const Device device = entry.submission.metadata().device();
    if (!run.exposes_device(device)) {
      return false;
    }
    return routes.can_start(run.route, device, lane, worker_id,
                            static_cast<std::uint64_t>(run.in_flight));
  }

  /**
   * @brief Tests whether one class currently contains startable ready work.
   * @param service_class Class already independent from intent and route.
   * @param worker_id Worker attempting a start.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used only for startability checks.
   * @return True when at least one live Run exposes one lane head.
   * @throws Nothing.
   */
  bool has_startable(
      ComputeRunQosClass service_class, int worker_id,
      execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) noexcept {
    for (auto& row : run_states_) {
      PolicyRunState& state = row.second;
      if (state.run->policy_qos.service_class != service_class) {
        continue;
      }
      std::lock_guard<std::mutex> run_lock(state.run->mutex);
      if (candidate_entry_for_lane(state, worker_id, lane, routes) != nullptr) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Applies the fixed three-to-one inter-class arbitration rule.
   * @param worker_id Worker attempting a start.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used only for startability checks.
   * @return Selected class, or null when this worker has no startable route.
   * @throws Nothing.
   */
  std::optional<ComputeRunQosClass> choose_class(
      int worker_id, execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) noexcept {
    const bool interactive_ready =
        has_startable(ComputeRunQosClass::Interactive, worker_id, lane, routes);
    const bool throughput_ready =
        has_startable(ComputeRunQosClass::Throughput, worker_id, lane, routes);
    if (!interactive_ready && !throughput_ready) {
      return std::nullopt;
    }
    if (interactive_ready &&
        (!throughput_ready ||
         consecutive_interactive_ < kInteractiveBurstLimit)) {
      return ComputeRunQosClass::Interactive;
    }
    return ComputeRunQosClass::Throughput;
  }

  /**
   * @brief Creates one trusted ABI record from a current lane head.
   * @param state Matching Run/Graph fairness row.
   * @param entry Current store-owned lane head.
   * @return Complete local candidate with shared pin and scalar ABI bytes.
   * @throws Nothing after copying the existing shared owner.
   */
  CandidateRecord make_candidate(PolicyRunState& state,
                                 QueueEntry& entry) const noexcept {
    const ComputeRunQosClass service_class =
        state.run->policy_qos.service_class;
    const std::uint64_t dispatch_count = class_dispatch_count(service_class);
    const std::uint64_t age =
        dispatch_count >= entry.enqueued_class_dispatch_count
            ? dispatch_count - entry.enqueued_class_dispatch_count
            : 0U;
    const std::uint64_t normalized =
        policy_for(service_class)
            .normalized_cost(entry.policy_service_cost,
                             state.run->policy_qos.weight);
    ps_policy_candidate_v1 abi{};
    abi.struct_size = sizeof(abi);
    abi.struct_kind = PS_POLICY_STRUCT_CANDIDATE;
    abi.candidate_id = entry.candidate_id;
    abi.graph_id = state.graph->graph_id;
    abi.run_id = state.run->id.value();
    abi.deadline_ns = deadline_nanoseconds(state.run->policy_qos.deadline);
    abi.weight = state.run->policy_qos.weight;
    abi.work_units = entry.submission.resource_demand().work_units;
    abi.ready_bytes = entry.ready_grant->resources().ready_bytes;
    abi.graph_service_score =
        saturating_add(state.graph->charged_service_for(service_class),
                       entry.policy_service_cost);
    abi.run_service_score = saturating_add(state.charged_service, normalized);
    abi.dispatch_age = age;
    abi.enqueue_sequence = entry.enqueue_sequence;
    if (entry.high_lane) {
      abi.flags |= PS_POLICY_CANDIDATE_FLAG_HIGH_PRIORITY_HINT;
    }
    if (state.run->policy_qos.deadline.has_value()) {
      abi.flags |= PS_POLICY_CANDIDATE_FLAG_DEADLINE_PRESENT;
    }
    return CandidateRecord{&state, *entry.store_position, abi,
                           entry.policy_service_cost, normalized};
  }

  /**
   * @brief Reduces current base candidates to the exact admissible frontier.
   * @param service_class Class chosen before aging.
   * @param worker_id Worker attempting a start.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used only for startability checks.
   * @return At most one current candidate per live Run after age/deadline and
   * one-quantum Graph/Run bands.
   * @throws std::bad_alloc when temporary ownership cannot allocate.
   * @note Caller holds the service/store mutex. Each Run mutex is held only
   * while copying its current startability facts.
   */
  std::vector<CandidateRecord> build_frontier(
      ComputeRunQosClass service_class, int worker_id,
      execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) {
    std::vector<CandidateRecord> candidates;
    candidates.reserve(run_states_.size());
    for (auto& row : run_states_) {
      PolicyRunState& state = row.second;
      if (state.run->policy_qos.service_class != service_class) {
        continue;
      }
      std::lock_guard<std::mutex> run_lock(state.run->mutex);
      QueueEntry* entry =
          candidate_entry_for_lane(state, worker_id, lane, routes);
      if (entry != nullptr) {
        candidates.push_back(make_candidate(state, *entry));
      }
    }
    if (candidates.empty()) {
      return candidates;
    }

    const auto erase_unless = [&candidates](const auto& predicate) {
      candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                      [&predicate](const auto& candidate) {
                                        return !predicate(candidate);
                                      }),
                       candidates.end());
    };
    const std::uint64_t maximum_age =
        std::max_element(candidates.begin(), candidates.end(),
                         [](const auto& lhs, const auto& rhs) {
                           return lhs.abi.dispatch_age < rhs.abi.dispatch_age;
                         })
            ->abi.dispatch_age;
    if (maximum_age >= kPolicyAgingDispatches) {
      erase_unless([maximum_age](const auto& candidate) {
        return candidate.abi.dispatch_age == maximum_age;
      });
    } else if (service_class == ComputeRunQosClass::Interactive) {
      std::uint64_t minimum_deadline = PS_POLICY_NO_DEADLINE_NS;
      for (const CandidateRecord& candidate : candidates) {
        if ((candidate.abi.flags & PS_POLICY_CANDIDATE_FLAG_DEADLINE_PRESENT) !=
            0U) {
          minimum_deadline =
              std::min(minimum_deadline, candidate.abi.deadline_ns);
        }
      }
      if (minimum_deadline != PS_POLICY_NO_DEADLINE_NS) {
        erase_unless([minimum_deadline](const auto& candidate) {
          return candidate.abi.deadline_ns == minimum_deadline;
        });
      }
    }

    const bool graph_scores_saturated = std::all_of(
        candidates.begin(), candidates.end(), [](const auto& value) {
          return value.abi.graph_service_score ==
                 std::numeric_limits<std::uint64_t>::max();
        });
    if (graph_scores_saturated) {
      const auto oldest = std::min_element(
          candidates.begin(), candidates.end(),
          [](const auto& lhs, const auto& rhs) {
            return lhs.abi.enqueue_sequence < rhs.abi.enqueue_sequence;
          });
      const std::uint64_t graph_id = oldest->abi.graph_id;
      erase_unless([graph_id](const auto& candidate) {
        return candidate.abi.graph_id == graph_id;
      });
    } else {
      std::uint64_t minimum_score = std::numeric_limits<std::uint64_t>::max();
      std::uint64_t maximum_cost = 0U;
      for (const CandidateRecord& candidate : candidates) {
        minimum_score =
            std::min(minimum_score, candidate.abi.graph_service_score);
        maximum_cost = std::max(maximum_cost, candidate.raw_cost);
      }
      const std::uint64_t limit = saturating_add(minimum_score, maximum_cost);
      erase_unless([limit](const auto& candidate) {
        return candidate.abi.graph_service_score <= limit;
      });
    }

    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> run_bands;
    for (const CandidateRecord& candidate : candidates) {
      auto [found, inserted] = run_bands.try_emplace(
          candidate.abi.graph_id, candidate.abi.run_service_score,
          candidate.normalized_cost);
      if (!inserted) {
        found->second.first =
            std::min(found->second.first, candidate.abi.run_service_score);
        found->second.second =
            std::max(found->second.second, candidate.normalized_cost);
      }
    }
    erase_unless([&run_bands](const auto& candidate) {
      const auto found = run_bands.find(candidate.abi.graph_id);
      if (found == run_bands.end()) {
        return false;
      }
      const std::uint64_t limit =
          saturating_add(found->second.first, found->second.second);
      return candidate.abi.run_service_score <= limit;
    });
    return candidates;
  }

  /**
   * @brief Converts one private candidate into an exact nonmutating pin.
   * @param candidate Current frontier member.
   * @return Complete shared pin and projected charges.
   * @throws Nothing.
   */
  static SelectionPin pin_from(const CandidateRecord& candidate) noexcept {
    return SelectionPin{candidate.entry,
                        candidate.entry->run->policy_qos.service_class,
                        candidate.abi.graph_service_score,
                        candidate.abi.run_service_score,
                        candidate.abi.candidate_id,
                        candidate.entry->entry_version,
                        candidate.abi.enqueue_sequence,
                        candidate.entry->run->route_generation};
  }

  /**
   * @brief Converts one allocation-free built-in choice into a shared pin.
   * @param selected Current store-owned selection.
   * @return Complete shared pin.
   * @throws Nothing.
   */
  static SelectionPin pin_from(const SelectedEntry& selected) noexcept {
    std::shared_ptr<QueueEntry> entry = *selected.entry->store_position;
    return SelectionPin{entry,
                        entry->run->policy_qos.service_class,
                        selected.graph_score,
                        selected.run_score,
                        entry->candidate_id,
                        entry->entry_version,
                        entry->enqueue_sequence,
                        entry->run->route_generation};
  }

  /**
   * @brief Revalidates exact object, nonreused identities, lane head, and row.
   * @param pin Candidate retained after policy return.
   * @param state Matching current Run policy row.
   * @param worker_id Stable worker id.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used for startability checks.
   * @return True only while the identical visible object remains eligible.
   * @throws Nothing.
   */
  bool pin_matches(
      const SelectionPin& pin, PolicyRunState& state, int worker_id,
      execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) const noexcept {
    return pin.entry->store_owned &&
           pin.entry->candidate_id == pin.candidate_id &&
           pin.entry->entry_version == pin.entry_version &&
           pin.entry->enqueue_sequence == pin.enqueue_sequence &&
           pin.entry->run->route_generation == pin.route_generation &&
           pin.entry->run.get() == state.run &&
           candidate_entry_for_lane(state, worker_id, lane, routes) ==
               pin.entry.get() &&
           *pin.entry->store_position == pin.entry;
  }

  /**
   * @brief Returns the successful-start counter for one policy class.
   * @param service_class Explicit class selected before aging.
   * @return Mutable class-local counter.
   * @throws Nothing; an invalid trusted enum terminates.
   */
  std::uint64_t& class_dispatch_count(
      ComputeRunQosClass service_class) noexcept {
    switch (service_class) {
      case ComputeRunQosClass::Interactive:
        return interactive_dispatch_count_;
      case ComputeRunQosClass::Throughput:
        return throughput_dispatch_count_;
    }
    std::terminate();
  }

  /**
   * @brief Returns the successful-start counter for one policy class.
   * @param service_class Explicit class selected before aging.
   * @return Immutable class-local counter.
   * @throws Nothing; an invalid trusted enum terminates.
   */
  const std::uint64_t& class_dispatch_count(
      ComputeRunQosClass service_class) const noexcept {
    switch (service_class) {
      case ComputeRunQosClass::Interactive:
        return interactive_dispatch_count_;
      case ComputeRunQosClass::Throughput:
        return throughput_dispatch_count_;
    }
    std::terminate();
  }

  /**
   * @brief Validates one entry before any store or policy mutation.
   * @param entry Candidate owned by its staging caller.
   * @return Nothing.
   * @throws std::invalid_argument for malformed ownership, grant, identity,
   * class, or checked-cost state.
   */
  void validate_entry(const QueueEntry& entry) const {
    if (!entry.run || !entry.ready_grant.has_value() ||
        !entry.ready_grant->active() || entry.store_owned ||
        entry.policy_service_cost == 0U ||
        entry.submission.metadata().run_id() != entry.run->id ||
        entry.submission.metadata().graph_identity() != entry.run->graph ||
        entry.submission.metadata().qos().service_class !=
            entry.run->policy_qos.service_class) {
      throw std::invalid_argument(
          "Bounded ready store received invalid policy entry ownership.");
    }
    const ResourceVector charge = entry.ready_grant->resources();
    if (charge.cpu_slots != 0U || charge.retained_memory_bytes != 0U ||
        charge.scratch_bytes != 0U || charge.ready_entries != 1U ||
        charge.ready_bytes == 0U) {
      throw std::invalid_argument(
          "Bounded ready store received an invalid ready grant.");
    }
  }

  /**
   * @brief Links one published entry to its same-Run priority FIFO.
   * @param state Matching persistent Run policy row.
   * @param entry Newly list-owned entry with no intrusive neighbors.
   * @return Nothing.
   * @throws Nothing; invalid linkage terminates.
   */
  static void link_entry(PolicyRunState& state, QueueEntry& entry) noexcept {
    if (entry.run_previous != nullptr || entry.run_next != nullptr) {
      std::terminate();
    }
    LaneEndpoints& lane = entry.high_lane ? state.high : state.normal;
    entry.run_previous = lane.tail;
    if (lane.tail != nullptr) {
      lane.tail->run_next = &entry;
    } else {
      lane.head = &entry;
    }
    lane.tail = &entry;
  }

  /**
   * @brief Returns the same-Run entry eligible for global policy ranking.
   * @param state Active Run state.
   * @return Oldest aged lane head, otherwise the high-hint head when present.
   * @throws Nothing.
   * @note Aging overrides the same-Run priority hint so a stream of newly
   * released high work cannot indefinitely hide an older normal candidate.
   */
  QueueEntry* candidate_entry(PolicyRunState& state) const noexcept {
    if (state.high.head == nullptr) {
      return state.normal.head;
    }
    if (state.normal.head == nullptr) {
      return state.high.head;
    }

    const bool high_aged = is_aged(*state.high.head);
    const bool normal_aged = is_aged(*state.normal.head);
    if (high_aged != normal_aged) {
      return normal_aged ? state.normal.head : state.high.head;
    }
    if (high_aged) {
      return state.normal.head->enqueue_sequence <
                     state.high.head->enqueue_sequence
                 ? state.normal.head
                 : state.high.head;
    }
    return state.high.head;
  }

  /**
   * @brief Returns the first entry in one priority FIFO usable by a worker.
   * @param state Active Run policy row whose Run mutex is held.
   * @param head Oldest entry in one intrusive priority FIFO.
   * @param worker_id Stable worker id.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used for startability checks.
   * @return First startable entry, or null when this FIFO belongs to another
   * physical lane.
   * @throws Nothing.
   * @note Skipping an opposite-device head does not mutate FIFO order; each
   * physical lane independently observes its oldest compatible entry.
   */
  QueueEntry* first_startable_entry(
      PolicyRunState& state, QueueEntry* head, int worker_id,
      execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) const noexcept {
    for (QueueEntry* entry = head; entry != nullptr; entry = entry->run_next) {
      if (!is_grant_blocked(*entry, worker_id) &&
          route_startable(*state.run, *entry, worker_id, lane, routes)) {
        return entry;
      }
    }
    return nullptr;
  }

  /**
   * @brief Returns the same-Run candidate for one fixed physical lane.
   * @param state Active Run policy row whose Run mutex is held.
   * @param worker_id Stable worker id.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used for startability checks.
   * @return Oldest compatible aged head, otherwise compatible high work first.
   * @throws Nothing.
   * @note Device filtering happens before the existing high/normal aging rule;
   * opposite-lane entries remain published for their owning worker.
   */
  QueueEntry* candidate_entry_for_lane(
      PolicyRunState& state, int worker_id,
      execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) const noexcept {
    QueueEntry* high =
        first_startable_entry(state, state.high.head, worker_id, lane, routes);
    QueueEntry* normal = first_startable_entry(state, state.normal.head,
                                               worker_id, lane, routes);
    if (high == nullptr) {
      return normal;
    }
    if (normal == nullptr) {
      return high;
    }
    const bool high_aged = is_aged(*high);
    const bool normal_aged = is_aged(*normal);
    if (high_aged != normal_aged) {
      return normal_aged ? normal : high;
    }
    if (high_aged) {
      return normal->enqueue_sequence < high->enqueue_sequence ? normal : high;
    }
    return high;
  }

  /**
   * @brief Tests one entry's deterministic dispatch-count age.
   * @param entry Published entry.
   * @return True after at least eight later successful dispatches.
   * @throws Nothing.
   */
  bool is_aged(const QueueEntry& entry) const noexcept {
    const std::uint64_t dispatch_count =
        class_dispatch_count(entry.run->policy_qos.service_class);
    return dispatch_count >= entry.enqueued_class_dispatch_count &&
           dispatch_count - entry.enqueued_class_dispatch_count >=
               kPolicyAgingDispatches;
  }

  /**
   * @brief Reports whether one explicit service class has ready work.
   * @param service_class QoS class to inspect.
   * @return True when any matching active Run has an eligible entry.
   * @throws Nothing.
   */
  bool has_ready(ComputeRunQosClass service_class) noexcept {
    for (auto& row : run_states_) {
      PolicyRunState& state = row.second;
      if (state.run->policy_qos.service_class == service_class &&
          candidate_entry(state) != nullptr) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Selects one entry within a policy's explicit service class.
   * @param policy Stateless built-in ranking strategy.
   * @param worker_id Stable worker id.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used for startability checks.
   * @return Complete chosen row/entry and next accounting scores.
   * @throws Nothing.
   * @note Aged candidates precede ordinary comparison only within the class
   * already selected by inter-class arbitration.
   */
  SelectedEntry select_from_class(
      const BuiltinPolicy& policy, int worker_id,
      execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) noexcept {
    SelectedEntry best;
    BuiltinPolicy::Candidate best_snapshot;
    std::uint64_t maximum_age = 0U;
    std::optional<std::chrono::steady_clock::time_point> minimum_deadline;
    bool any_deadline = false;

    for (auto& row : run_states_) {
      PolicyRunState& state = row.second;
      if (state.run->policy_qos.service_class != policy.service_class()) {
        continue;
      }
      std::lock_guard<std::mutex> run_lock(state.run->mutex);
      QueueEntry* entry =
          candidate_entry_for_lane(state, worker_id, lane, routes);
      if (entry == nullptr) {
        continue;
      }
      const std::uint64_t dispatch_count =
          class_dispatch_count(policy.service_class());
      const std::uint64_t age =
          dispatch_count >= entry->enqueued_class_dispatch_count
              ? dispatch_count - entry->enqueued_class_dispatch_count
              : 0U;
      maximum_age = std::max(maximum_age, age);
      if (state.run->policy_qos.deadline.has_value()) {
        any_deadline = true;
        if (!minimum_deadline.has_value() ||
            *state.run->policy_qos.deadline < *minimum_deadline) {
          minimum_deadline = state.run->policy_qos.deadline;
        }
      }
    }

    for (auto& row : run_states_) {
      PolicyRunState& state = row.second;
      if (state.run->policy_qos.service_class != policy.service_class()) {
        continue;
      }
      std::lock_guard<std::mutex> run_lock(state.run->mutex);
      QueueEntry* entry =
          candidate_entry_for_lane(state, worker_id, lane, routes);
      if (entry == nullptr) {
        continue;
      }
      const std::uint64_t graph_score = saturating_add(
          state.graph->charged_service_for(policy.service_class()),
          entry->policy_service_cost);
      const std::uint64_t run_score =
          saturating_add(state.charged_service,
                         policy.normalized_cost(entry->policy_service_cost,
                                                state.run->policy_qos.weight));
      const std::uint64_t dispatch_count =
          class_dispatch_count(policy.service_class());
      const std::uint64_t age =
          dispatch_count >= entry->enqueued_class_dispatch_count
              ? dispatch_count - entry->enqueued_class_dispatch_count
              : 0U;
      const bool aged =
          maximum_age >= kPolicyAgingDispatches && age == maximum_age;
      if (maximum_age >= kPolicyAgingDispatches && !aged) {
        continue;
      }
      if (maximum_age < kPolicyAgingDispatches &&
          policy.service_class() == ComputeRunQosClass::Interactive &&
          any_deadline && state.run->policy_qos.deadline != minimum_deadline) {
        continue;
      }
      const BuiltinPolicy::Candidate snapshot{
          state.run->policy_qos.deadline,
          graph_score,
          run_score,
          entry->enqueue_sequence,
      };

      bool replace = best.entry == nullptr;
      if (!replace) {
        replace = policy.precedes(snapshot, best_snapshot);
      }
      if (replace) {
        best = SelectedEntry{&state, entry, graph_score, run_score, aged};
        best_snapshot = snapshot;
      }
    }
    return best;
  }

  /**
   * @brief Unlinks and destroys one store list node with exact accounting.
   * @param entry Store-owned entry selected or purged.
   * @param state Matching Run policy row retained after removal.
   * @return Nothing.
   * @throws Nothing; invalid linkage/accounting terminates.
   */
  void remove_entry(QueueEntry& entry, PolicyRunState& state) noexcept {
    if (!entry.store_owned || entry.run.get() != state.run ||
        !entry.ready_grant.has_value() || !entry.ready_grant->active()) {
      std::terminate();
    }
    LaneEndpoints& lane = entry.high_lane ? state.high : state.normal;
    if (entry.run_previous != nullptr) {
      entry.run_previous->run_next = entry.run_next;
    } else if (lane.head == &entry) {
      lane.head = entry.run_next;
    } else {
      std::terminate();
    }
    if (entry.run_next != nullptr) {
      entry.run_next->run_previous = entry.run_previous;
    } else if (lane.tail == &entry) {
      lane.tail = entry.run_previous;
    } else {
      std::terminate();
    }

    const ResourceVector charge = entry.ready_grant->resources();
    if (charge.ready_entries != 1U || charge.ready_entries > entry_count_ ||
        charge.ready_bytes > byte_count_) {
      std::terminate();
    }
    entry_count_ -= charge.ready_entries;
    byte_count_ -= charge.ready_bytes;
    entry.store_owned = false;
    entry.run_previous = nullptr;
    entry.run_next = nullptr;
    entries_.erase(entry.store_position);
    telemetry_.decrement_physical_counter(
        ExecutionLifecyclePhysicalCounter::ReadyEntry);
  }

  /** @brief Immutable maximum entries across all classes and lanes. */
  const std::uint64_t entry_limit_;

  /** @brief Immutable maximum accounted bytes across all classes and lanes. */
  const std::uint64_t byte_limit_;

  /** @brief Stable non-owning physical lifecycle counter owner. */
  ExecutionLifecycleTelemetry& telemetry_;

  /** @brief Store-owned list node for every currently ready value. */
  std::list<std::shared_ptr<QueueEntry>> entries_;

  /** @brief Persistent active-Run policy rows keyed by opaque Run id. */
  std::map<std::uint64_t, PolicyRunState> run_states_;

  /** @brief Shared Graph fairness rows keyed by stable Graph identity. */
  std::map<std::string, PolicyGraphState> graph_states_;

  /** @brief Stateless deadline-aware interactive built-in strategy. */
  InteractiveBuiltinPolicy interactive_policy_;

  /** @brief Stateless weighted throughput built-in strategy. */
  ThroughputBuiltinPolicy throughput_policy_;

  /** @brief Current entries across all classes and lanes. */
  std::uint64_t entry_count_ = 0U;

  /** @brief Current exact ready-grant bytes across all entries. */
  std::uint64_t byte_count_ = 0U;

  /** @brief Successful Interactive starts used only for class-local aging. */
  std::uint64_t interactive_dispatch_count_ = 0U;

  /** @brief Successful Throughput starts used only for class-local aging. */
  std::uint64_t throughput_dispatch_count_ = 0U;

  /** @brief Stable publication sequence assigned without reuse. */
  std::uint64_t next_enqueue_sequence_ = 0U;

  /** @brief Stable candidate identity assigned without reuse. */
  std::uint64_t next_candidate_id_ = 0U;

  /** @brief Stable private entry version assigned without reuse. */
  std::uint64_t next_entry_version_ = 0U;

  /** @brief Stable opaque Graph identity assigned without reuse. */
  std::uint64_t next_graph_id_ = 0U;

  /** @brief Stable immutable snapshot generation assigned without reuse. */
  std::uint64_t next_snapshot_generation_ = 0U;

  /** @brief Stable callback-attempt sequence assigned without reuse. */
  std::uint64_t next_selection_sequence_ = 0U;

  /** @brief Current interactive burst while throughput remains ready. */
  std::uint64_t consecutive_interactive_ = 0U;
};

/**
 * @brief Complete unpublished physical batch behind PreparedExecutionRun.
 *
 * @throws Nothing from destruction after successful construction.
 * @note Field order makes cancellation registration retire before detached
 * entries, then RunState/reservation. The owning service outlives every
 * admission candidate and prepared batch.
 */
struct PreparedExecutionRunState final {
  /** @brief Exact service that created and may publish this state. */
  ExecutionService* owner = nullptr;
  /** @brief Matching physical Run and root reservation owner. */
  std::shared_ptr<ExecutionService::RunState> run;
  /** @brief Fully detached ready-store map/list publication nodes. */
  ExecutionService::BoundedReadyStore::PreparedBatch batch;
  /** @brief Cancellation cleanup active through publication and settlement. */
  ComputeRunCancellationRegistration cancellation_registration;
};

/**
 * @brief Complete retained-only root behind one shared phase reservation.
 *
 * @throws Nothing from destruction after successful construction.
 * @note Declaration order releases the reservation before its Run lease. The
 * owning service outlives every prepared shared reservation.
 */
struct PreparedExecutionSharedReservationState final {
  /**
   * @brief Captures one exact Run lease and ledger root.
   * @param active_run_lease Matching Run settlement owner.
   * @param active_reservation Retained-only physical authority.
   * @throws Nothing.
   * @note Declaration order makes reservation release precede lease release.
   */
  PreparedExecutionSharedReservationState(
      ComputeRunLease active_run_lease,
      ResourceLedger::Reservation active_reservation) noexcept
      : run_lease(std::move(active_run_lease)),
        reservation(std::move(active_reservation)) {}

  /** @brief Matching Run retained through physical settlement. */
  ComputeRunLease run_lease;
  /** @brief Retained-only ledger root released before run_lease. */
  ResourceLedger::Reservation reservation;
};

/** @copydoc PreparedExecutionRun::PreparedExecutionRun */
PreparedExecutionRun::PreparedExecutionRun() noexcept = default;

/** @copydoc PreparedExecutionRun::PreparedExecutionRun */
PreparedExecutionRun::PreparedExecutionRun(
    std::unique_ptr<PreparedExecutionRunState> state) noexcept
    : state_(std::move(state)) {}

/** @copydoc PreparedExecutionRun::PreparedExecutionRun */
PreparedExecutionRun::PreparedExecutionRun(
    PreparedExecutionRun&& other) noexcept = default;  // NOLINT

/** @copydoc PreparedExecutionRun::operator= */
PreparedExecutionRun& PreparedExecutionRun::operator=(
    PreparedExecutionRun&& other) noexcept {
  if (this != &other) {
    if (state_) {
      std::terminate();
    }
    state_ = std::move(other.state_);
  }
  return *this;
}

/** @copydoc PreparedExecutionRun::~PreparedExecutionRun */
PreparedExecutionRun::~PreparedExecutionRun() noexcept = default;

/** @copydoc PreparedExecutionSharedReservation::
 * PreparedExecutionSharedReservation */
PreparedExecutionSharedReservation::
    PreparedExecutionSharedReservation() noexcept = default;

/** @copydoc PreparedExecutionSharedReservation::
 * PreparedExecutionSharedReservation */
PreparedExecutionSharedReservation::PreparedExecutionSharedReservation(
    std::unique_ptr<PreparedExecutionSharedReservationState> state) noexcept
    : state_(std::move(state)) {}

/** @copydoc PreparedExecutionSharedReservation::
 * PreparedExecutionSharedReservation */
PreparedExecutionSharedReservation::PreparedExecutionSharedReservation(
    PreparedExecutionSharedReservation&& other) noexcept = default;  // NOLINT

/** @copydoc PreparedExecutionSharedReservation::operator= */
PreparedExecutionSharedReservation&
PreparedExecutionSharedReservation::operator=(
    PreparedExecutionSharedReservation&& other) noexcept {
  if (this != &other) {
    if (state_) {
      std::terminate();
    }
    state_ = std::move(other.state_);
  }
  return *this;
}

/** @copydoc PreparedExecutionSharedReservation::
 * ~PreparedExecutionSharedReservation */
PreparedExecutionSharedReservation::
    ~PreparedExecutionSharedReservation() noexcept = default;

/** @copydoc ExecutionService::service_run_envelope_bytes */
std::uint64_t ExecutionService::service_run_envelope_bytes(
    const std::string& graph_identity, const std::string& graph_key) {
  RetainedMemoryEstimator estimate("ExecutionService Run envelope");
  estimate.add_objects<RunState>();
  estimate.add_shared_control_block();
  estimate.add_bytes(BoundedReadyStore::run_policy_envelope_bytes());
  estimate.add_bytes(static_cast<std::uint64_t>(graph_identity.capacity()));
  estimate.add_bytes(1U);
  estimate.add_bytes(static_cast<std::uint64_t>(graph_key.capacity()));
  estimate.add_bytes(1U);
  estimate.add_bytes(ResourceLedger::reservation_state_retained_memory_bytes());
  estimate.add_bytes(
      ComputeRunLease::cancellation_notification_retained_memory_bytes(
          static_cast<std::uint64_t>(sizeof(ExecutionService*)) +
          static_cast<std::uint64_t>(sizeof(std::weak_ptr<RunState>))));
  return estimate.bytes();
}

namespace {

/**
 * @brief Low-frequency fallback for grant releases outside service callbacks.
 *
 * @note Ordinary enqueue, completion, cancellation, policy replacement, and
 * shutdown transitions wake workers through a notification epoch. This bound
 * prevents an unobservable external child-grant release from stranding work
 * while keeping an exhausted candidate out of a busy retry loop.
 */
constexpr std::chrono::milliseconds kGrantRetryBackoff{50};

/**
 * @brief Resolves the bounded process execution-worker request once.
 * @param requested Zero for automatic resolution or an exact value in `[1,8]`.
 * @param detected Platform hardware concurrency, possibly zero.
 * @return Exact positive worker count in `[1,8]`.
 * @throws std::invalid_argument when `requested` exceeds the public bound.
 */
unsigned int resolve_execution_worker_count(unsigned int requested,
                                            unsigned int detected) {
  if (requested > kExecutionWorkerRequestMax) {
    throw std::invalid_argument(
        "ExecutionService CPU worker count must be in [0,8].");
  }
  if (requested != 0U) {
    return requested;
  }
  return std::min(kExecutionWorkerRequestMax, std::max(1U, detected));
}

/**
 * @brief Validates and subtracts protected interactive admission headroom.
 * @param limits Immutable five-dimensional ledger capacity.
 * @param headroom Component-wise capacity reserved from Throughput Runs.
 * @return General-capacity ceiling available to Throughput Runs.
 * @throws std::invalid_argument when any headroom dimension exceeds its limit.
 * @note The returned vector is policy configuration, not resource authority.
 */
ResourceVector calculate_general_capacity(const ResourceVector& limits,
                                          const ResourceVector& headroom) {
  if (!resources_fit(headroom, limits)) {
    throw std::invalid_argument(
        "ExecutionService interactive headroom exceeds resource limits.");
  }
  return ResourceVector{
      limits.cpu_slots - headroom.cpu_slots,
      limits.retained_memory_bytes - headroom.retained_memory_bytes,
      limits.scratch_bytes - headroom.scratch_bytes,
      limits.ready_entries - headroom.ready_entries,
      limits.ready_bytes - headroom.ready_bytes,
  };
}

/**
 * @brief Tracks built-in Throughput root reservations against general quota.
 *
 * @throws std::system_error when its transaction mutex cannot be locked.
 * @note This accounting owns no physical capacity. `ResourceLedger` remains
 * the sole authority and retains this observer until the matching root vector
 * is physically returned after both parent and child-grant ownership end.
 */
class ThroughputReservationAccount final
    : public ResourceLedger::ReservationReleaseObserver {
 public:
  /**
   * @brief Fixes the policy-only ceiling for the service lifetime.
   * @param capacity Complete `limits - interactive_headroom` vector.
   * @throws Nothing.
   */
  explicit ThroughputReservationAccount(ResourceVector capacity) noexcept
      : capacity_(capacity) {}

  /** @copydoc
   * ResourceLedger::ReservationReleaseObserver::release_transaction_mutex */
  std::mutex& release_transaction_mutex() noexcept override { return mutex_; }

  /** @copydoc
   * ResourceLedger::ReservationReleaseObserver::on_reservation_released */
  void on_reservation_released(
      const ResourceVector& released) noexcept override {
    if (!resources_fit(released, reserved_)) {
      std::terminate();
    }
    reserved_ = ResourceVector{
        reserved_.cpu_slots - released.cpu_slots,
        reserved_.retained_memory_bytes - released.retained_memory_bytes,
        reserved_.scratch_bytes - released.scratch_bytes,
        reserved_.ready_entries - released.ready_entries,
        reserved_.ready_bytes - released.ready_bytes,
    };
  }

  /**
   * @brief Computes one prospective Throughput charge without mutation.
   * @param resources Complete candidate Run vector.
   * @return Next class-owned total, or null on overflow/quota exhaustion.
   * @throws Nothing.
   * @note Caller holds `release_transaction_mutex()`.
   */
  std::optional<ResourceVector> checked_charge(
      const ResourceVector& resources) const noexcept {
    const std::optional<ResourceVector> after =
        checked_add_resources(reserved_, resources);
    if (!after.has_value() || !resources_fit(*after, capacity_)) {
      return std::nullopt;
    }
    return after;
  }

  /**
   * @brief Commits a prevalidated charge after ledger reservation succeeds.
   * @param charged Exact value returned by `checked_charge()`.
   * @return Nothing.
   * @throws Nothing; a non-monotonic value terminates.
   * @note Caller holds `release_transaction_mutex()` and the ledger has already
   * committed the matching root reservation with this object as observer.
   */
  void commit_charge(const ResourceVector& charged) noexcept {
    if (!resources_fit(reserved_, charged) ||
        !resources_fit(charged, capacity_)) {
      std::terminate();
    }
    reserved_ = charged;
  }

  /**
   * @brief Copies current Throughput-owned root commitments for tests.
   * @return Exact class-owned vector; no authority is minted.
   * @throws std::system_error when transaction locking fails.
   */
  ResourceVector snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reserved_;
  }

 private:
  /** @brief Serializes Throughput check/commit with exact root release. */
  mutable std::mutex mutex_;

  /** @brief Immutable policy quota excluding configured headroom. */
  const ResourceVector capacity_;

  /** @brief Active built-in Throughput root vectors only. */
  ResourceVector reserved_;
};

}  // namespace

/**
 * @brief Owns all fixed-pool, bounded-store, registry, and ledger state.
 *
 * @throws std::bad_alloc from container growth and worker creation staging.
 * @note One mutex defines queue-to-Run lock order: pool mutex is acquired
 * before a Run mutex whenever both are needed.
 */
class ExecutionService::PoolState final {
 public:
  /**
   * @brief Creates one unconfigured execution domain with immutable limits.
   * @param resource_limits Complete ledger and ready-store limits.
   * @param telemetry Stable physical counter owner.
   * @param policy_observer Stable non-owning service callback/binding observer.
   * @throws std::invalid_argument when interactive headroom exceeds a limit.
   * @throws std::bad_alloc when ledger state cannot allocate.
   */
  PoolState(ExecutionResourceLimits resource_limits,
            ExecutionLifecycleTelemetry& telemetry,
            policy::PolicyLifecycleObserver policy_observer)
      : registry(policy::PolicyRegistry::process_instance()),
        interactive_binding(registry.create_binding(
            "interactive", PolicyClass::Interactive, 1U, policy_observer)),
        throughput_binding(registry.create_binding(
            "throughput", PolicyClass::Throughput, 1U, policy_observer)),
        throughput_reservations(std::make_shared<ThroughputReservationAccount>(
            calculate_general_capacity(resource_limits.resource_vector(),
                                       resource_limits.interactive_headroom))),
        ledger(resource_limits.resource_vector()),
        ready_store(resource_limits.ready_entries, resource_limits.ready_bytes,
                    telemetry) {
    interactive_binding->mark_service_published();
    throughput_binding->mark_service_published();
  }

  /**
   * @brief Applies one built-in policy ceiling before ordinary ledger commit.
   * @param resources Complete checked root vector requested by one Run/owner.
   * @param service_class Explicit policy class; no intent inference occurs.
   * @param settlement_observer Non-owning exact Run settlement callback.
   * @return Ledger-minted reservation, or null without mutation when either
   * policy ceiling or authoritative capacity is unavailable.
   * @throws std::bad_alloc or std::system_error from ledger admission.
   * @note Caller holds `mutex`. Throughput check, ledger commit, and class
   * charge are serialized by the account transaction mutex; exact root release
   * takes the same lock before returning capacity and debiting the class. The
   * policy mints no token; `ledger` remains the sole physical authority.
   */
  std::optional<ResourceLedger::Reservation> try_reserve_for_policy(
      const ResourceVector& resources, ComputeRunQosClass service_class,
      ResourceLedger::ReservationSettlementObserver settlement_observer) {
    if (service_class == ComputeRunQosClass::Interactive) {
      return ledger.try_reserve(resources, nullptr, settlement_observer);
    }

    std::lock_guard<std::mutex> transaction_lock(
        throughput_reservations->release_transaction_mutex());
    const std::optional<ResourceVector> after =
        throughput_reservations->checked_charge(resources);
    if (!after.has_value()) {
      return std::nullopt;
    }
    std::optional<ResourceLedger::Reservation> reservation = ledger.try_reserve(
        resources, throughput_reservations, settlement_observer);
    if (!reservation.has_value()) {
      return std::nullopt;
    }
    throughput_reservations->commit_charge(*after);
    return reservation;
  }

  /**
   * @brief Publishes one worker-relevant state transition under `mutex`.
   * @return Nothing.
   * @throws Nothing.
   * @note Every caller holds `mutex` and subsequently notifies `ready_cv`.
   * Unsigned wrap remains safe because grant-blocked waits also use a bounded
   * fallback; observing exactly one full 64-bit lap cannot strand a worker.
   */
  void advance_worker_notification_epoch() noexcept {
    ++worker_notification_epoch;
  }

  /**
   * @brief Copies the current immutable binding for one explicit class.
   * @param service_class Run QoS class already chosen by Host arbitration.
   * @return Shared invocation/context/DSO lease.
   * @throws Nothing; caller holds `mutex` and invalid enums terminate.
   */
  std::shared_ptr<policy::PolicyBinding> binding_for(
      ComputeRunQosClass service_class) const noexcept {
    switch (service_class) {
      case ComputeRunQosClass::Interactive:
        return interactive_binding;
      case ComputeRunQosClass::Throughput:
        return throughput_binding;
    }
    std::terminate();
  }

  /** @brief Process registry shared by all execution-service instances. */
  policy::PolicyRegistry& registry;

  /** @brief Serializes preparation/publication of policy replacements only. */
  mutable std::mutex policy_mutation_mutex;

  /** @brief Current Interactive context/generation/DSO lease. */
  std::shared_ptr<policy::PolicyBinding> interactive_binding;

  /** @brief Current Throughput context/generation/DSO lease. */
  std::shared_ptr<policy::PolicyBinding> throughput_binding;

  /**
   * @brief Non-authoritative quota for active built-in Throughput reservations.
   */
  const std::shared_ptr<ThroughputReservationAccount> throughput_reservations;

  /** @brief Serializes fixed configuration, queues, and active Run registry. */
  mutable std::mutex mutex;

  /** @brief Wakes fixed workers when ready work or shutdown is published. */
  std::condition_variable ready_cv;

  /** @brief Monotonic worker-relevant publication/completion generation. */
  std::uint64_t worker_notification_epoch = 0U;

  /** @brief Sole host-authoritative resource mint for this service. */
  ResourceLedger ledger;

  /** @brief Sole policy-aware entry/byte-bounded ready-store owner. */
  BoundedReadyStore ready_store;

  /** @brief Private route discovery plus allocation-free start/finish state. */
  execution::PhysicalExecutionRoutes physical_routes;

  /** @brief Fixed CPU workers followed by one GPU-pipeline worker. */
  std::vector<std::thread> workers;

  /** @brief Frozen worker count, or zero before complete configuration. */
  unsigned int configured_workers = 0U;

  /** @brief True after explicit shutdown requests worker-loop exit. */
  bool stopping = false;

  /** @brief True while one control thread owns physical shutdown progress. */
  bool shutdown_in_progress = false;

  /** @brief True after routes, workers, bindings, and telemetry stop. */
  bool shutdown_complete = false;

  /** @brief Wakes repeated shutdown callers after the owner completes. */
  std::condition_variable shutdown_cv;
};

/** @brief Current service-worker Run context, null outside callbacks. */
thread_local ExecutionService::RunState* ExecutionService::tls_run_state_ =
    nullptr;

/** @brief Exact service owning the entered callback, or null outside one. */
thread_local ExecutionService* ExecutionService::tls_service_ = nullptr;

/** @brief Current service worker id, or -1 outside callbacks. */
thread_local int ExecutionService::tls_worker_id_ = -1;

#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
namespace testing {

/** @copydoc arm_reserved_start_rollback_probe_for_testing */
void arm_reserved_start_rollback_probe_for_testing() noexcept {
  ReservedStartProbeState& state = reserved_start_probe_state();
  state.armed.store(false, std::memory_order_release);
  state.calls.store(0U, std::memory_order_relaxed);
  for (ReservedStartProbeAttempt& attempt : state.attempts) {
    attempt.candidate_id.store(0U, std::memory_order_relaxed);
    attempt.entry_version.store(0U, std::memory_order_relaxed);
    attempt.route_generation.store(0U, std::memory_order_relaxed);
    attempt.cpu_slots.store(0U, std::memory_order_relaxed);
    attempt.retained_memory_bytes.store(0U, std::memory_order_relaxed);
    attempt.scratch_bytes.store(0U, std::memory_order_relaxed);
    attempt.ready_entries.store(0U, std::memory_order_relaxed);
    attempt.ready_bytes.store(0U, std::memory_order_relaxed);
  }
  state.armed.store(true, std::memory_order_release);
}

/** @copydoc reserved_start_rollback_probe_snapshot_for_testing */
ReservedStartRollbackProbeSnapshot
reserved_start_rollback_probe_snapshot_for_testing() noexcept {
  ReservedStartProbeState& state = reserved_start_probe_state();
  ReservedStartRollbackProbeSnapshot snapshot;
  snapshot.calls = state.calls.load(std::memory_order_acquire);
  for (std::size_t index = 0U; index < 2U; ++index) {
    const ReservedStartProbeAttempt& attempt = state.attempts[index];
    const std::uint64_t ready_bytes =
        attempt.ready_bytes.load(std::memory_order_acquire);
    snapshot.candidate_ids[index] =
        attempt.candidate_id.load(std::memory_order_relaxed);
    snapshot.entry_versions[index] =
        attempt.entry_version.load(std::memory_order_relaxed);
    snapshot.route_generations[index] =
        attempt.route_generation.load(std::memory_order_relaxed);
    snapshot.resources[index] = ResourceVector{
        attempt.cpu_slots.load(std::memory_order_relaxed),
        attempt.retained_memory_bytes.load(std::memory_order_relaxed),
        attempt.scratch_bytes.load(std::memory_order_relaxed),
        attempt.ready_entries.load(std::memory_order_relaxed),
        ready_bytes,
    };
  }
  return snapshot;
}

/** @copydoc disarm_reserved_start_rollback_probe_for_testing */
void disarm_reserved_start_rollback_probe_for_testing() noexcept {
  reserved_start_probe_state().armed.store(false, std::memory_order_release);
}

}  // namespace testing
#endif

/** @copydoc ExecutionService::default_resource_limits */
ExecutionResourceLimits ExecutionService::default_resource_limits() noexcept {
  constexpr std::uint64_t kOneMebibyte = 1024U * 1024U;
  return ExecutionResourceLimits{
      32U,
      1024U * kOneMebibyte,
      512U * kOneMebibyte,
      65536U,
      256U * kOneMebibyte,
      ResourceVector{1U, 64U * kOneMebibyte, 32U * kOneMebibyte, 1024U,
                     16U * kOneMebibyte},
  };
}

/** @copydoc ExecutionService::ExecutionService */
ExecutionService::ExecutionService()
    : ExecutionService(default_resource_limits()) {}

/** @copydoc ExecutionService::ExecutionService */
ExecutionService::ExecutionService(ExecutionResourceLimits resource_limits)
    : lifecycle_telemetry_(std::make_unique<ExecutionLifecycleTelemetry>()),
      lifecycle_registry_(
          std::make_unique<RunLifecycleRegistry>(*lifecycle_telemetry_)),
      pool_(std::make_unique<PoolState>(resource_limits, *lifecycle_telemetry_,
                                        policy_lifecycle_observer())) {
  const ExecutionLifecycleCounters counters = lifecycle_registry_->counters();
  lifecycle_telemetry_->publish(ExecutionLifecycleEventKind::ServiceStarted,
                                ExecutionLifecycleCategory::None, 0U, 0U, 0U,
                                0U, counters);
}

/** @copydoc ExecutionService::ExecutionService */
ExecutionService::ExecutionService(unsigned int worker_count)
    : ExecutionService(worker_count, default_resource_limits()) {}

/** @copydoc ExecutionService::ExecutionService */
ExecutionService::ExecutionService(unsigned int worker_count,
                                   ExecutionResourceLimits resource_limits)
    : ExecutionService(resource_limits) {
  configure_worker_count(worker_count);
}

/** @copydoc ExecutionService::~ExecutionService */
ExecutionService::~ExecutionService() noexcept {
  if (!pool_) {
    return;
  }
  try {
    shutdown();
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ExecutionService::policy_lifecycle_observer */
policy::PolicyLifecycleObserver
ExecutionService::policy_lifecycle_observer() noexcept {
  return policy::PolicyLifecycleObserver{
      this, &ExecutionService::observe_policy_invocation_entered,
      &ExecutionService::observe_policy_invocation_returned,
      &ExecutionService::observe_policy_binding_published,
      &ExecutionService::observe_policy_binding_retired};
}

/** @copydoc ExecutionService::observe_policy_invocation_entered */
void ExecutionService::observe_policy_invocation_entered(
    void* context) noexcept {
  if (context == nullptr) {
    std::terminate();
  }
  auto* service = static_cast<ExecutionService*>(context);
  service->lifecycle_telemetry_->increment_physical_counter(
      ExecutionLifecyclePhysicalCounter::LivePolicyInvocation);
}

/** @copydoc ExecutionService::observe_policy_invocation_returned */
void ExecutionService::observe_policy_invocation_returned(
    void* context) noexcept {
  if (context == nullptr) {
    std::terminate();
  }
  auto* service = static_cast<ExecutionService*>(context);
  service->lifecycle_telemetry_->decrement_physical_counter(
      ExecutionLifecyclePhysicalCounter::LivePolicyInvocation);
}

/** @copydoc ExecutionService::observe_policy_binding_published */
void ExecutionService::observe_policy_binding_published(
    void* context) noexcept {
  if (context == nullptr) {
    std::terminate();
  }
  auto* service = static_cast<ExecutionService*>(context);
  service->lifecycle_telemetry_->increment_physical_counter(
      ExecutionLifecyclePhysicalCounter::LivePolicyBinding);
}

/** @copydoc ExecutionService::observe_policy_binding_retired */
void ExecutionService::observe_policy_binding_retired(
    void* context, std::uint64_t generation, bool destroy_failed) noexcept {
  if (context == nullptr) {
    std::terminate();
  }
  auto* service = static_cast<ExecutionService*>(context);
  service->lifecycle_telemetry_->decrement_physical_counter(
      ExecutionLifecyclePhysicalCounter::LivePolicyBinding);
  try {
    service->lifecycle_telemetry_->publish(
        ExecutionLifecycleEventKind::BindingRetired,
        destroy_failed ? ExecutionLifecycleCategory::FailureOther
                       : ExecutionLifecycleCategory::None,
        0U, 0U, 0U, generation, service->lifecycle_registry_->counters());
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ExecutionService::configure_worker_count */
void ExecutionService::configure_worker_count(unsigned int worker_count) {
  if (worker_count > kExecutionWorkerRequestMax) {
    throw std::invalid_argument(
        "ExecutionService CPU worker count must be in [0,8].");
  }

  std::unique_lock<std::mutex> lock(pool_->mutex);
  if (pool_->configured_workers != 0U) {
    if (worker_count == 0U || worker_count == pool_->configured_workers) {
      return;
    }
    throw std::invalid_argument(
        "ExecutionService CPU worker count is already fixed.");
  }
  if (pool_->stopping) {
    throw std::logic_error("ExecutionService is stopping.");
  }

  const unsigned int resolved_workers = resolve_execution_worker_count(
      worker_count, std::thread::hardware_concurrency());
  const ResourceLedger::Snapshot resources = pool_->ledger.snapshot();
  if (static_cast<std::uint64_t>(resolved_workers) >
      resources.limits.cpu_slots) {
    throw std::invalid_argument(
        "ExecutionService worker count exceeds configured CPU capacity.");
  }

  std::vector<std::thread> staged_workers;
  staged_workers.reserve(static_cast<std::size_t>(resolved_workers) + 1U);
  pool_->configured_workers = resolved_workers;
  try {
    for (unsigned int index = 0; index < resolved_workers; ++index) {
      staged_workers.emplace_back(&ExecutionService::worker_loop, this,
                                  static_cast<int>(index),
                                  execution::PhysicalExecutionLane::Cpu);
    }
    staged_workers.emplace_back(&ExecutionService::worker_loop, this,
                                static_cast<int>(resolved_workers),
                                execution::PhysicalExecutionLane::Gpu);
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    pool_->stopping = true;
    pool_->advance_worker_notification_epoch();
    lock.unlock();
    pool_->ready_cv.notify_all();
    for (std::thread& worker : staged_workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    lock.lock();
    pool_->stopping = false;
    pool_->configured_workers = 0U;
    lock.unlock();
    std::rethrow_exception(failure);
  }

  pool_->workers.swap(staged_workers);
}

/** @copydoc ExecutionService::worker_count */
unsigned int ExecutionService::worker_count() const {
  std::lock_guard<std::mutex> lock(pool_->mutex);
  return pool_->configured_workers;
}

/** @copydoc ExecutionService::is_configured */
bool ExecutionService::is_configured() const {
  std::lock_guard<std::mutex> lock(pool_->mutex);
  return pool_->configured_workers != 0U && !pool_->stopping &&
         pool_->workers.size() ==
             static_cast<std::size_t>(pool_->configured_workers) + 1U;
}

/** @copydoc ExecutionService::get_stats */
std::string ExecutionService::get_stats() const {
  std::lock_guard<std::mutex> lock(pool_->mutex);
  const ResourceLedger::Snapshot resources = pool_->ledger.snapshot();
  std::ostringstream stream;
  stream << "Workers: " << pool_->configured_workers << ", GPU lanes: 1"
         << ", Active runs: " << pool_->ready_store.run_count()
         << ", Ready tasks: " << pool_->ready_store.entry_count()
         << ", Ready bytes: " << pool_->ready_store.byte_count()
         << ", Reserved CPU: " << resources.reserved.cpu_slots;
  return stream.str();
}

/** @copydoc ExecutionService::policy_available_types */
std::vector<std::string> ExecutionService::policy_available_types() const {
  return pool_->registry.available_types();
}

/** @copydoc ExecutionService::policy_description */
std::string ExecutionService::policy_description(
    const std::string& type_name) const {
  return pool_->registry.description(type_name);
}

/** @copydoc ExecutionService::policy_scan */
std::size_t ExecutionService::policy_scan(
    const std::vector<std::string>& directories) {
  return pool_->registry.scan(directories);
}

/** @copydoc ExecutionService::policy_load */
void ExecutionService::policy_load(const std::string& path) {
  pool_->registry.load(path);
}

/** @copydoc ExecutionService::policy_loaded_plugins */
std::vector<std::string> ExecutionService::policy_loaded_plugins() const {
  return pool_->registry.loaded_plugins();
}

/** @copydoc ExecutionService::configure_policy_defaults */
void ExecutionService::configure_policy_defaults(
    const HostPolicyConfig& config) {
  policy::PolicyRegistry::assert_mutation_allowed("configure_policy_defaults");
  std::lock_guard<std::mutex> mutation_lock(pool_->policy_mutation_mutex);
  std::uint64_t interactive_generation = 0U;
  std::uint64_t throughput_generation = 0U;
  {
    std::lock_guard<std::mutex> pool_lock(pool_->mutex);
    interactive_generation = pool_->interactive_binding->generation();
    throughput_generation = pool_->throughput_binding->generation();
  }
  if (interactive_generation == std::numeric_limits<std::uint64_t>::max() ||
      throughput_generation == std::numeric_limits<std::uint64_t>::max()) {
    throw GraphError(GraphErrc::ComputeError,
                     "ExecutionService policy generation exhausted.");
  }

  std::shared_ptr<policy::PolicyBinding> interactive =
      pool_->registry.create_binding(
          config.interactive_type, PolicyClass::Interactive,
          interactive_generation + 1U, policy_lifecycle_observer());
  std::shared_ptr<policy::PolicyBinding> throughput =
      pool_->registry.create_binding(
          config.throughput_type, PolicyClass::Throughput,
          throughput_generation + 1U, policy_lifecycle_observer());
  {
    std::lock_guard<std::mutex> pool_lock(pool_->mutex);
    if (pool_->interactive_binding->generation() != interactive_generation ||
        pool_->throughput_binding->generation() != throughput_generation) {
      throw GraphError(GraphErrc::ComputeError,
                       "ExecutionService policy replacement raced state.");
    }
    pool_->interactive_binding.swap(interactive);
    pool_->throughput_binding.swap(throughput);
    pool_->interactive_binding->mark_service_published();
    pool_->throughput_binding->mark_service_published();
    pool_->advance_worker_notification_epoch();
  }
  pool_->ready_cv.notify_all();
}

/** @copydoc ExecutionService::policy_info */
PolicyInfoSnapshot ExecutionService::policy_info(
    PolicyClass policy_class) const {
  std::shared_ptr<policy::PolicyBinding> binding;
  {
    std::lock_guard<std::mutex> lock(pool_->mutex);
    switch (policy_class) {
      case PolicyClass::Interactive:
        binding = pool_->interactive_binding;
        break;
      case PolicyClass::Throughput:
        binding = pool_->throughput_binding;
        break;
      default:
        throw GraphError(GraphErrc::InvalidParameter, "Unknown policy class.");
    }
  }
  return PolicyInfoSnapshot{policy_class, binding->type_name(),
                            binding->generation(), binding->fault()};
}

/** @copydoc ExecutionService::replace_policy */
void ExecutionService::replace_policy(PolicyClass policy_class,
                                      const std::string& type) {
  policy::PolicyRegistry::assert_mutation_allowed("replace_policy");
  std::lock_guard<std::mutex> mutation_lock(pool_->policy_mutation_mutex);
  std::shared_ptr<policy::PolicyBinding> current;
  {
    std::lock_guard<std::mutex> pool_lock(pool_->mutex);
    switch (policy_class) {
      case PolicyClass::Interactive:
        current = pool_->interactive_binding;
        break;
      case PolicyClass::Throughput:
        current = pool_->throughput_binding;
        break;
      default:
        throw GraphError(GraphErrc::InvalidParameter, "Unknown policy class.");
    }
  }
  if (current->generation() == std::numeric_limits<std::uint64_t>::max()) {
    throw GraphError(GraphErrc::ComputeError,
                     "ExecutionService policy generation exhausted.");
  }
  std::shared_ptr<policy::PolicyBinding> candidate =
      pool_->registry.create_binding(type, policy_class,
                                     current->generation() + 1U,
                                     policy_lifecycle_observer());
  {
    std::lock_guard<std::mutex> pool_lock(pool_->mutex);
    std::shared_ptr<policy::PolicyBinding>& published =
        policy_class == PolicyClass::Interactive ? pool_->interactive_binding
                                                 : pool_->throughput_binding;
    if (published.get() != current.get()) {
      throw GraphError(GraphErrc::ComputeError,
                       "ExecutionService policy replacement raced state.");
    }
    published.swap(candidate);
    published->mark_service_published();
    pool_->advance_worker_notification_epoch();
  }
  pool_->ready_cv.notify_all();
}

/** @copydoc ExecutionService::available_execution_types */
std::vector<std::string> ExecutionService::available_execution_types() {
  return execution::PhysicalExecutionRoutes::available_types();
}

/** @copydoc ExecutionService::execution_description */
std::string ExecutionService::execution_description(
    const std::string& type_name) {
  return execution::PhysicalExecutionRoutes::description(type_name);
}

/** @copydoc ExecutionService::is_execution_type */
bool ExecutionService::is_execution_type(
    const std::string& type_name) noexcept {
  return execution::PhysicalExecutionRoutes::is_supported(type_name);
}

/** @copydoc ExecutionService::resource_snapshot */
ResourceLedger::Snapshot ExecutionService::resource_snapshot() const {
  return pool_->ledger.snapshot();
}

/** @copydoc ExecutionService::register_graph_lifecycle */
void ExecutionService::register_graph_lifecycle(
    std::shared_ptr<GraphLifetimeAnchor> anchor) {
  lifecycle_registry_->register_graph(std::move(anchor));
}

/** @copydoc ExecutionService::rollback_graph_lifecycle_registration */
void ExecutionService::rollback_graph_lifecycle_registration(
    GraphInstanceId graph_instance_id) {
  lifecycle_registry_->rollback_graph_registration(graph_instance_id);
}

/** @copydoc ExecutionService::begin_graph_admission */
RunLifecycleAdmissionCandidate ExecutionService::begin_graph_admission(
    GraphInstanceId graph_instance_id) {
  return lifecycle_registry_->begin_graph_admission(graph_instance_id);
}

/** @copydoc ExecutionService::commit_graph_admission */
RunLifecycleAdmissionHandle ExecutionService::commit_graph_admission(
    RunLifecycleAdmissionCandidate candidate, ComputeRunLease run_lease,
    std::shared_ptr<ComputeRequestCancellationSource> cancellation) {
  return lifecycle_registry_->commit_standalone(
      std::move(candidate), std::move(run_lease), std::move(cancellation));
}

/** @copydoc ExecutionService::commit_graph_admission_group */
RunLifecycleAdmissionHandle ExecutionService::commit_graph_admission_group(
    RunLifecycleAdmissionCandidate candidate, RunGroupId run_group_id,
    ComputeRunLease hp_lease, ComputeRunLease rt_lease,
    std::shared_ptr<ComputeRequestCancellationSource> cancellation,
    std::shared_ptr<DirtySiblingCommitGate> sibling_commit_gate) {
  return lifecycle_registry_->commit_realtime_group(
      std::move(candidate), run_group_id, std::move(hp_lease),
      std::move(rt_lease), std::move(cancellation),
      std::move(sibling_commit_gate));
}

/** @copydoc ExecutionService::finalize_graph_admission */
void ExecutionService::finalize_graph_admission(
    RunLifecycleAdmissionHandle& handle) noexcept {
  try {
    lifecycle_registry_->finalize_admission(handle);
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ExecutionService::permits_visible_commit */
bool ExecutionService::permits_visible_commit(GraphInstanceId graph_instance_id,
                                              ComputeRunId run_id) const {
  return lifecycle_registry_->permits_visible_commit(graph_instance_id, run_id);
}

/** @copydoc ExecutionService::begin_graph_close_lifecycle */
std::uint64_t ExecutionService::begin_graph_close_lifecycle(
    GraphInstanceId graph_instance_id, ComputeRunCancellationReason reason) {
  return lifecycle_registry_->begin_graph_close(graph_instance_id, reason);
}

/** @copydoc ExecutionService::finish_graph_close_lifecycle */
void ExecutionService::finish_graph_close_lifecycle(
    GraphInstanceId graph_instance_id) {
  lifecycle_registry_->finish_graph_close(graph_instance_id);
}

/** @copydoc ExecutionService::close_graph_lifecycle */
void ExecutionService::close_graph_lifecycle(
    GraphInstanceId graph_instance_id, ComputeRunCancellationReason reason) {
  lifecycle_registry_->close_graph(graph_instance_id, reason);
}

/** @copydoc ExecutionService::validate_shutdown_caller */
void ExecutionService::validate_shutdown_caller() const {
  if (tls_service_ == this ||
      policy::PolicyRegistry::callback_active_on_current_thread(this)) {
    throw std::logic_error(
        "ExecutionService shutdown cannot run from its worker or policy "
        "callback.");
  }
}

/** @copydoc ExecutionService::begin_shutdown */
std::uint64_t ExecutionService::begin_shutdown() {
  validate_shutdown_caller();
  return lifecycle_registry_->begin_service_shutdown();
}

/** @copydoc ExecutionService::shutdown */
void ExecutionService::shutdown() {
  validate_shutdown_caller();
  (void)begin_shutdown();

  {
    std::unique_lock<std::mutex> lock(pool_->mutex);
    if (pool_->shutdown_complete) {
      return;
    }
    if (pool_->shutdown_in_progress) {
      pool_->shutdown_cv.wait(
          lock, [this]() { return !pool_->shutdown_in_progress; });
      if (pool_->shutdown_complete) {
        return;
      }
    }
    pool_->shutdown_in_progress = true;
  }

  try {
    lifecycle_registry_->wait_until_empty();
    const ResourceLedger::Snapshot before_stop = pool_->ledger.snapshot();
    if (before_stop.reserved != ResourceVector{}) {
      throw std::logic_error(
          "ExecutionService registry settled before resource ledger zero.");
    }

    std::vector<std::thread> workers;
    std::shared_ptr<policy::PolicyBinding> interactive_binding;
    std::shared_ptr<policy::PolicyBinding> throughput_binding;
    ShutdownWorkerJoinGuard worker_join_guard(workers, *lifecycle_telemetry_,
                                              *lifecycle_registry_);
    {
      std::lock_guard<std::mutex> lock(pool_->mutex);
      pool_->stopping = true;
      pool_->physical_routes.begin_shutdown();
      if (!pool_->ready_store.empty()) {
        throw std::logic_error(
            "ExecutionService registry settled with ready entries.");
      }
      pool_->ready_store.clear();
      pool_->advance_worker_notification_epoch();
      workers.swap(pool_->workers);
      interactive_binding = std::move(pool_->interactive_binding);
      throughput_binding = std::move(pool_->throughput_binding);
    }
    pool_->ready_cv.notify_all();

    worker_join_guard.complete();

    interactive_binding.reset();
    throughput_binding.reset();

    {
      std::lock_guard<std::mutex> lock(pool_->mutex);
      if (!pool_->physical_routes.drained()) {
        throw std::logic_error(
            "ExecutionService routes did not drain before worker join.");
      }
      pool_->configured_workers = 0U;
    }
    if (pool_->ledger.snapshot().reserved != ResourceVector{}) {
      throw std::logic_error(
          "ExecutionService resource ledger changed after worker join.");
    }
    if (!lifecycle_telemetry_->physical_counters_zero()) {
      throw std::logic_error(
          "ExecutionService physical lifecycle counters did not retire.");
    }

    ExecutionLifecycleCounters final_counters;
    (void)lifecycle_registry_->mark_service_stopped(final_counters);
    {
      std::lock_guard<std::mutex> lock(pool_->mutex);
      pool_->shutdown_complete = true;
      pool_->shutdown_in_progress = false;
    }
    pool_->shutdown_cv.notify_all();
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(pool_->mutex);
      pool_->shutdown_in_progress = false;
    }
    pool_->shutdown_cv.notify_all();
    throw;
  }
}

/** @copydoc ExecutionService::lifecycle_snapshot */
ExecutionLifecyclePage ExecutionService::lifecycle_snapshot(
    std::uint64_t after_cursor, std::uint32_t limit) const {
  return lifecycle_telemetry_->snapshot(after_cursor, limit);
}

/** @copydoc ExecutionService::throughput_reservation_snapshot_for_testing */
ResourceVector ExecutionService::throughput_reservation_snapshot_for_testing()
    const {
  return pool_->throughput_reservations->snapshot();
}

/** @copydoc ExecutionService::estimate_cpu_run_resources */
ResourceVector ExecutionService::estimate_cpu_run_resources(
    const ReadyTaskSubmission& representative, int total_task_count,
    CpuRunResourceDemand run_resource_demand) const {
  unsigned int configured_workers = 0U;
  {
    std::lock_guard<std::mutex> lock(pool_->mutex);
    if (pool_->configured_workers == 0U || pool_->workers.empty()) {
      throw std::logic_error(
          "ExecutionService worker count is not configured.");
    }
    if (pool_->stopping) {
      throw std::logic_error("ExecutionService is stopping.");
    }
    configured_workers = pool_->configured_workers;
  }
  return calculate_cpu_run_admission(
             configured_workers, representative.metadata().graph_identity(),
             total_task_count,
             representative.metadata().qos().maximum_parallelism,
             run_resource_demand)
      .resources;
}

/** @copydoc ExecutionService::prepare_run */
PreparedExecutionRun ExecutionService::prepare_run(
    ExecutionHostContext& host, const std::string& execution_type,
    std::vector<ReadyTaskSubmission> initial_submissions, int total_task_count,
    CpuRunResourceDemand run_resource_demand) {
  if (!is_execution_type(execution_type)) {
    throw std::invalid_argument(
        "ExecutionService requires a known private execution route.");
  }
  if (total_task_count <= 0 || initial_submissions.empty()) {
    throw std::invalid_argument(
        "ExecutionService requires a nonempty active Run batch.");
  }
  if (initial_submissions.size() > static_cast<std::size_t>(total_task_count)) {
    throw std::invalid_argument(
        "ExecutionService initial ready count exceeds total task count.");
  }
  const bool route_metal_available =
      route_exposes_device(host, execution_type, Device::GPU_METAL);
  const ComputeRunId run_id = initial_submissions.front().metadata().run_id();
  ComputeRunLease service_lease = initial_submissions.front().lease_;
  for (const ReadyTaskSubmission& submission : initial_submissions) {
    if (submission.metadata().run_id() != run_id) {
      throw std::invalid_argument(
          "ExecutionService initial batch mixes multiple Runs.");
    }
    if (submission.resource_demand() != run_resource_demand.task) {
      throw std::invalid_argument(
          "ExecutionService initial batch resource declaration mismatch.");
    }
    if (!route_inventory_exposes_device(execution_type, route_metal_available,
                                        submission.metadata().device())) {
      throw std::invalid_argument(
          "ExecutionService submission device is unavailable on its route.");
    }
  }

  unsigned int configured_workers = 0U;
  {
    std::lock_guard<std::mutex> lock(pool_->mutex);
    if (pool_->configured_workers == 0U || pool_->workers.empty()) {
      throw std::logic_error(
          "ExecutionService worker count is not configured.");
    }
    if (pool_->stopping) {
      throw std::logic_error("ExecutionService is stopping.");
    }
    configured_workers = pool_->configured_workers;
  }

  CpuRunAdmissionEstimate admission = calculate_cpu_run_admission(
      configured_workers,
      initial_submissions.front().metadata().graph_identity(), total_task_count,
      initial_submissions.front().metadata().qos().maximum_parallelism,
      run_resource_demand);
  std::optional<ResourceLedger::Reservation> reservation;
  const ResourceLedger::ReservationSettlementObserver settlement_observer =
      service_lease.begin_resource_settlement_observation(
          *lifecycle_telemetry_);
  {
    try {
      std::lock_guard<std::mutex> lock(pool_->mutex);
      if (pool_->stopping) {
        throw std::logic_error("ExecutionService is stopping.");
      }
      reservation = pool_->try_reserve_for_policy(
          admission.resources,
          initial_submissions.front().metadata().qos().service_class,
          settlement_observer);
    } catch (...) {
      service_lease.cancel_resource_settlement_observation();
      throw;
    }
  }
  if (!reservation.has_value()) {
    service_lease.cancel_resource_settlement_observation();
    throw GraphError(
        GraphErrc::ComputeError,
        "ExecutionService policy/ledger cannot admit the complete Run.");
  }
  service_lease.commit_resource_settlement_observation();

  auto run = std::make_shared<RunState>(
      run_id, std::move(admission.policy_graph_identity),
      std::move(admission.policy_graph_key),
      initial_submissions.front().metadata().qos(), host, execution_type,
      route_metal_available, total_task_count, run_resource_demand.task,
      admission.ready_bytes_per_task,
      admission.execution_retained_bytes_per_task, std::move(*reservation));

  const std::weak_ptr<RunState> weak_run(run);
  ComputeRunCancellationRegistration cancellation_registration =
      service_lease.register_cancellation_notification(
          [this, weak_run](ComputeRunCancellationReason reason) noexcept {
            if (const std::shared_ptr<RunState> accepted_run =
                    weak_run.lock()) {
              cancel_run(accepted_run, reason);
            }
          });

  std::vector<std::shared_ptr<QueueEntry>> staged_entries;
  staged_entries.reserve(initial_submissions.size());
  for (ReadyTaskSubmission& submission : initial_submissions) {
    staged_entries.push_back(make_queue_entry(run, std::move(submission)));
  }
  const std::size_t staged_submission_size = initial_submissions.size();
  const std::size_t staged_submission_capacity = initial_submissions.capacity();
  release_initial_submission_storage(initial_submissions);
  observe_initial_submission_storage(
      admission.resources, staged_submission_size, staged_submission_capacity,
      initial_submissions);

  BoundedReadyStore::PreparedBatch batch;
  {
    std::lock_guard<std::mutex> pool_lock(pool_->mutex);
    if (pool_->configured_workers == 0U || pool_->workers.empty()) {
      throw std::logic_error(
          "ExecutionService worker count is not configured.");
    }
    if (pool_->stopping) {
      throw std::logic_error("ExecutionService is stopping.");
    }
    if (pool_->ready_store.contains_run_id(run_id)) {
      throw std::logic_error("ExecutionService Run id is already active.");
    }
    batch = pool_->ready_store.prepare_initial_batch(run, staged_entries);
  }

  auto state = std::make_unique<PreparedExecutionRunState>();
  state->owner = this;
  state->run = std::move(run);
  state->batch = std::move(batch);
  state->cancellation_registration = std::move(cancellation_registration);
  return PreparedExecutionRun(std::move(state));
}

/** @copydoc ExecutionService::prepare_shared_reservation */
PreparedExecutionSharedReservation ExecutionService::prepare_shared_reservation(
    const ComputeRunLease& run_lease, std::uint64_t retained_memory_bytes) {
  if (retained_memory_bytes == 0U) {
    throw std::invalid_argument(
        "ExecutionService shared reservation requires retained memory.");
  }
  ComputeRunLease service_lease(run_lease);
  {
    std::lock_guard<std::mutex> lock(pool_->mutex);
    if (pool_->configured_workers == 0U || pool_->workers.empty()) {
      throw std::logic_error(
          "ExecutionService worker count is not configured.");
    }
    if (pool_->stopping) {
      throw std::logic_error("ExecutionService is stopping.");
    }
  }

  RetainedMemoryEstimator complete_demand(
      "ExecutionService shared reservation envelope");
  complete_demand.add_bytes(retained_memory_bytes);
  complete_demand.add_objects<PreparedExecutionSharedReservationState>();
  complete_demand.add_bytes(
      ResourceLedger::reservation_state_retained_memory_bytes());
  const ResourceVector resources{0U, complete_demand.bytes(), 0U, 0U, 0U};
  const ResourceLedger::ReservationSettlementObserver settlement_observer =
      service_lease.begin_resource_settlement_observation(
          *lifecycle_telemetry_);
  std::optional<ResourceLedger::Reservation> reservation;
  {
    try {
      std::lock_guard<std::mutex> lock(pool_->mutex);
      if (pool_->stopping) {
        throw std::logic_error("ExecutionService is stopping.");
      }
      reservation = pool_->try_reserve_for_policy(
          resources, run_lease.descriptor().qos().service_class,
          settlement_observer);
    } catch (...) {
      service_lease.cancel_resource_settlement_observation();
      throw;
    }
  }
  if (!reservation.has_value()) {
    service_lease.cancel_resource_settlement_observation();
    throw GraphError(
        GraphErrc::ComputeError,
        "ExecutionService policy/ledger cannot admit shared Run ownership.");
  }
  service_lease.commit_resource_settlement_observation();

  auto state = std::make_unique<PreparedExecutionSharedReservationState>(
      std::move(service_lease), std::move(*reservation));
  return PreparedExecutionSharedReservation(std::move(state));
}

/** @copydoc ExecutionService::execute_prepared_run */
void ExecutionService::execute_prepared_run(PreparedExecutionRun prepared) {
  if (!prepared.state_ || prepared.state_->owner != this) {
    throw std::invalid_argument(
        "ExecutionService requires one active local prepared Run.");
  }
  std::unique_ptr<PreparedExecutionRunState> state = std::move(prepared.state_);
  const std::shared_ptr<RunState> run = state->run;

  {
    std::lock_guard<std::mutex> pool_lock(pool_->mutex);
    if (pool_->configured_workers == 0U || pool_->workers.empty()) {
      throw std::logic_error(
          "ExecutionService worker count is not configured.");
    }
    if (pool_->stopping) {
      throw std::logic_error("ExecutionService is stopping.");
    }
    std::lock_guard<std::mutex> run_lock(run->mutex);
    if (run->cancelled) {
      run->reservation.reset();
      return;
    }
    if (!pool_->ready_store.try_publish_prepared_batch(state->batch)) {
      throw GraphError(
          GraphErrc::ComputeError,
          "ExecutionService bounded ready store rejected initial work.");
    }
    run->published = true;
    pool_->advance_worker_notification_epoch();
  }
  pool_->ready_cv.notify_all();

  std::exception_ptr failure;
  {
    std::unique_lock<std::mutex> lock(run->mutex);
    run->settled_cv.wait(lock, [&run]() { return run->settled(); });
    run->accepting = false;
    failure = run->first_exception;
  }

  {
    std::lock_guard<std::mutex> pool_lock(pool_->mutex);
    std::lock_guard<std::mutex> run_lock(run->mutex);
    pool_->ready_store.retire_run(run);
    run->published = false;
    run->reservation.reset();
  }

  if (failure) {
    std::rethrow_exception(failure);
  }
}

/** @copydoc ExecutionService::execute_run */
void ExecutionService::execute_run(
    ExecutionHostContext& host, const std::string& execution_type,
    std::vector<ReadyTaskSubmission> initial_submissions, int total_task_count,
    CpuRunResourceDemand run_resource_demand) {
  if (!initial_submissions.empty() &&
      initial_submissions.front().lease_.observe_cancellation().has_value()) {
    return;
  }
  execute_prepared_run(prepare_run(host, execution_type,
                                   std::move(initial_submissions),
                                   total_task_count, run_resource_demand));
}

/** @copydoc ExecutionService::release_initial_submission_storage */
void ExecutionService::release_initial_submission_storage(
    std::vector<ReadyTaskSubmission>& submissions) noexcept {
  std::vector<ReadyTaskSubmission> released_storage;
  released_storage.swap(submissions);
}

/** @copydoc ExecutionService::observe_initial_submission_storage */
void ExecutionService::observe_initial_submission_storage(
    const ResourceVector& admitted_resources, std::size_t staged_size,
    std::size_t staged_capacity,
    const std::vector<ReadyTaskSubmission>& submissions) const noexcept {
  if (initial_submission_storage_observer_ == nullptr) {
    return;
  }
  initial_submission_storage_observer_(
      initial_submission_storage_observer_context_, admitted_resources,
      staged_size, staged_capacity, submissions.size(), submissions.capacity());
}

/** @copydoc ExecutionService::make_queue_entry */
std::shared_ptr<ExecutionService::QueueEntry>
ExecutionService::make_queue_entry(const std::shared_ptr<RunState>& run,
                                   ReadyTaskSubmission submission) {
  if (submission.metadata().run_id() != run->id) {
    throw std::invalid_argument(
        "ReadyTaskSubmission does not belong to its routed Run.");
  }
  if (submission.resource_demand() != run->resource_demand) {
    throw GraphError(
        GraphErrc::ComputeError,
        "ReadyTaskSubmission resource declaration differs from Run admission.");
  }
  if (!run->exposes_device(submission.metadata().device())) {
    throw std::invalid_argument(
        "ReadyTaskSubmission device is unavailable on its routed Run.");
  }
  const ResourceVector ready_resources{0U, 0U, 0U, 1U,
                                       run->ready_bytes_per_task};
  if (!run->reservation.has_value()) {
    throw std::logic_error(
        "ReadyTaskSubmission Run reservation is already closed.");
  }
  std::optional<ResourceLedger::Grant> ready_grant =
      run->reservation->try_grant(ready_resources);
  if (!ready_grant.has_value()) {
    throw GraphError(
        GraphErrc::ComputeError,
        "Run reservation cannot grant uniformly estimated ready work.");
  }
  const std::uint64_t policy_service_cost = calculate_policy_service_cost(
      submission.resource_demand(), run->ready_bytes_per_task);
  return std::make_shared<QueueEntry>(
      run, std::move(submission), std::move(*ready_grant), policy_service_cost);
}

/** @copydoc ExecutionService::enqueue_submission */
void ExecutionService::enqueue_submission(const std::shared_ptr<RunState>& run,
                                          ReadyTaskSubmission submission) {
  const std::optional<ComputeRunCancellationReason> cancellation =
      submission.lease_.observe_cancellation();
  if (cancellation.has_value()) {
    cancel_run(run, *cancellation);
    return;
  }
  std::shared_ptr<QueueEntry> entry =
      make_queue_entry(run, std::move(submission));

  {
    std::lock_guard<std::mutex> pool_lock(pool_->mutex);
    if (pool_->stopping) {
      throw std::logic_error("ExecutionService is stopping.");
    }
    if (!pool_->ready_store.owns_run(run)) {
      throw std::logic_error(
          "ExecutionService Run no longer accepts ready work.");
    }
    std::lock_guard<std::mutex> run_lock(run->mutex);
    if (run->cancelled) {
      return;
    }
    if (!run->accepting || run->first_exception) {
      throw std::logic_error(
          "ExecutionService Run no longer accepts ready work.");
    }

    if (!pool_->ready_store.try_push(entry)) {
      throw GraphError(
          GraphErrc::ComputeError,
          "ExecutionService bounded ready store rejected dependent work.");
    }
    pool_->advance_worker_notification_epoch();
  }
  pool_->ready_cv.notify_all();
}

/** @copydoc ExecutionService::submit_ready_submission */
void ExecutionService::submit_ready_submission(ReadyTaskSubmission submission) {
  const ComputeRunId run_id = submission.metadata().run_id();
  if (tls_run_state_ == nullptr || tls_run_state_->id != run_id) {
    throw std::invalid_argument(
        "Dependent ready publication requires the matching worker Run.");
  }
  std::shared_ptr<RunState> run = tls_run_state_->shared_from_this();
  enqueue_submission(run, std::move(submission));
}

/** @copydoc ExecutionService::available_devices */
std::vector<Device> ExecutionService::available_devices() const {
  return {Device::CPU};
}

/** @copydoc ExecutionService::available_devices(const ExecutionHostContext&,
 * const std::string&) */
std::vector<Device> ExecutionService::available_devices(
    const ExecutionHostContext& host, const std::string& execution_type) const {
  if (execution_type == "gpu_pipeline") {
    if (route_exposes_device(host, execution_type, Device::GPU_METAL)) {
      return {Device::GPU_METAL, Device::CPU};
    }
    return {Device::CPU};
  }
  if (execution_type == "cpu" || execution_type == "serial_debug") {
    return {Device::CPU};
  }
  throw GraphError(GraphErrc::NotFound,
                   "Unknown private execution route: " + execution_type);
}

/** @copydoc ExecutionService::submit_initial_task_handles */
void ExecutionService::submit_initial_task_handles(
    std::vector<ExecutionTaskHandle>&& handles, int total_task_count,
    ExecutionTaskPriority priority) {
  (void)handles;
  (void)total_task_count;
  (void)priority;
  throw std::logic_error(
      "ExecutionService rejects borrowed initial task handles.");
}

/** @copydoc ExecutionService::submit_ready_task_handles_from_worker */
void ExecutionService::submit_ready_task_handles_from_worker(
    std::vector<ExecutionTaskHandle>&& handles,
    ExecutionTaskPriority priority) {
  (void)handles;
  (void)priority;
  throw std::logic_error(
      "ExecutionService rejects borrowed ready task handles.");
}

/** @copydoc ExecutionService::submit_ready_task_any_thread */
void ExecutionService::submit_ready_task_any_thread(
    Task&& task, ExecutionTaskPriority priority,
    std::optional<std::uint64_t> epoch) {
  (void)task;
  (void)priority;
  (void)epoch;
  throw std::logic_error("ExecutionService rejects anonymous ready callbacks.");
}

/** @copydoc ExecutionService::wait_for_completion */
void ExecutionService::wait_for_completion() {
  throw std::logic_error(
      "ExecutionService requires an explicit Run-scoped completion wait.");
}

/** @copydoc ExecutionService::current_worker_run */
ExecutionService::RunState& ExecutionService::current_worker_run() {
  if (tls_run_state_ == nullptr || tls_worker_id_ < 0) {
    throw std::logic_error(
        "ExecutionService runtime operation requires a worker Run.");
  }
  return *tls_run_state_;
}

/** @copydoc ExecutionService::set_exception */
void ExecutionService::set_exception(std::exception_ptr error) {
  if (!error) {
    return;
  }
  RunState& current = current_worker_run();
  fail_run(current.shared_from_this(), std::move(error));
}

/** @copydoc ExecutionService::inc_tasks_to_complete */
void ExecutionService::inc_tasks_to_complete(int delta) {
  if (delta <= 0) {
    throw std::invalid_argument(
        "ExecutionService completion increment must be positive.");
  }
  RunState& run = current_worker_run();
  std::lock_guard<std::mutex> lock(run.mutex);
  if (run.cancelled || run.first_exception) {
    return;
  }
  if (run.tasks_to_complete > std::numeric_limits<int>::max() - delta) {
    throw std::overflow_error("ExecutionService completion count overflow.");
  }
  run.tasks_to_complete += delta;
}

/** @copydoc ExecutionService::dec_tasks_to_complete */
void ExecutionService::dec_tasks_to_complete() {
  RunState& run = current_worker_run();
  std::lock_guard<std::mutex> lock(run.mutex);
  if (run.cancelled || run.first_exception) {
    return;
  }
  if (run.tasks_to_complete <= 0) {
    throw std::logic_error("ExecutionService completion count underflow.");
  }
  --run.tasks_to_complete;
  if (run.tasks_to_complete == 0) {
    run.settled_cv.notify_all();
  }
}

/** @copydoc ExecutionService::log_event */
void ExecutionService::log_event(ExecutionTraceAction action, int node_id) {
  RunState& run = current_worker_run();
  run.host->log_event(action, node_id, tls_worker_id_, run.id.value());
}

/** @copydoc ExecutionService::fail_run */
void ExecutionService::fail_run(const std::shared_ptr<RunState>& run,
                                std::exception_ptr failure) noexcept {
  if (!failure) {
    return;
  }
  try {
    {
      std::lock_guard<std::mutex> pool_lock(pool_->mutex);
      std::lock_guard<std::mutex> run_lock(run->mutex);
      if (!run->cancelled && !run->first_exception) {
        run->first_exception = std::move(failure);
      }
      if (!run->cancelled) {
        run->accepting = false;
      }
      if (run->published) {
        (void)pool_->ready_store.erase_run(run);
      }
      pool_->advance_worker_notification_epoch();
    }
    pool_->ready_cv.notify_all();
    run->settled_cv.notify_all();
  } catch (...) {
  }
}

/** @copydoc ExecutionService::cancel_run */
void ExecutionService::cancel_run(
    const std::shared_ptr<RunState>& run,
    ComputeRunCancellationReason reason) noexcept {
  try {
    {
      std::lock_guard<std::mutex> pool_lock(pool_->mutex);
      std::lock_guard<std::mutex> run_lock(run->mutex);
      if (!run->cancelled) {
        run->cancelled = true;
        run->cancellation_reason = reason;
      }
      run->first_exception = nullptr;
      run->accepting = false;
      if (run->published) {
        (void)pool_->ready_store.erase_run(run);
      }
      pool_->advance_worker_notification_epoch();
    }
    pool_->ready_cv.notify_all();
    run->settled_cv.notify_all();
  } catch (...) {
  }
}

/** @copydoc ExecutionService::retire_worker_entry */
void ExecutionService::retire_worker_entry(
    std::shared_ptr<QueueEntry>& entry,
    const std::shared_ptr<RunState>& run) noexcept {
  try {
    const Device device = entry->submission.metadata().device();
    {
      std::lock_guard<std::mutex> pool_lock(pool_->mutex);
      std::lock_guard<std::mutex> run_lock(run->mutex);
      if (run->in_flight <= 0) {
        std::terminate();
      }
      entry->execution_grant.reset();
      if (!pool_->physical_routes.finish(run->route, device)) {
        std::terminate();
      }
      pool_->advance_worker_notification_epoch();
    }

    entry.reset();
    if (worker_entry_retirement_observer_ != nullptr) {
      worker_entry_retirement_observer_(
          worker_entry_retirement_observer_context_, run->id);
    }

    bool settled = false;
    {
      std::lock_guard<std::mutex> pool_lock(pool_->mutex);
      std::lock_guard<std::mutex> run_lock(run->mutex);
      --run->in_flight;
      settled = run->settled();
    }
    if (settled) {
      run->settled_cv.notify_all();
    }
    pool_->ready_cv.notify_all();
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ExecutionService::worker_loop */
void ExecutionService::worker_loop(
    int worker_id, execution::PhysicalExecutionLane lane) noexcept {
  for (;;) {
    std::shared_ptr<QueueEntry> entry;
    std::exception_ptr selection_failure;
    std::shared_ptr<RunState> failed_run;
    unsigned int obsolete_retries = 0U;
    bool force_builtin = false;
    bool grant_blocked = false;
    std::uint64_t grant_blocked_epoch = 0U;

    while (!entry && !selection_failure) {
      BoundedReadyStore::PolicySnapshot snapshot;
      std::shared_ptr<policy::PolicyBinding> binding;
      {
        std::unique_lock<std::mutex> lock(pool_->mutex);
        if (grant_blocked && !pool_->ready_store.has_startable_work(
                                 worker_id, lane, pool_->physical_routes)) {
          const std::uint64_t observed_epoch = grant_blocked_epoch;
          if (!pool_->stopping &&
              pool_->worker_notification_epoch == observed_epoch) {
            (void)pool_->ready_cv.wait_for(
                lock, kGrantRetryBackoff, [this, observed_epoch]() {
                  return pool_->stopping ||
                         pool_->worker_notification_epoch != observed_epoch;
                });
          }
          if (pool_->stopping) {
            return;
          }
          pool_->ready_store.clear_grant_blocked(worker_id);
          grant_blocked = false;
          obsolete_retries = 0U;
          force_builtin = false;
          failed_run.reset();
          continue;
        }
        pool_->ready_cv.wait(lock, [this, worker_id, lane]() {
          return pool_->stopping ||
                 pool_->ready_store.has_startable_work(worker_id, lane,
                                                       pool_->physical_routes);
        });
        if (pool_->stopping) {
          return;
        }

        if (!force_builtin) {
          try {
            snapshot = pool_->ready_store.make_policy_snapshot(
                worker_id, lane, pool_->physical_routes);
          } catch (const std::bad_alloc&) {
            force_builtin = true;
          } catch (...) {
            selection_failure = std::current_exception();
          }
          if (!selection_failure && !snapshot.plugin_eligible) {
            force_builtin = true;
          }
          if (!force_builtin && !selection_failure) {
            binding = pool_->binding_for(snapshot.service_class);
          }
        }

        if (force_builtin && !selection_failure) {
          BoundedReadyStore::SelectionPin pin =
              pool_->ready_store.select_builtin_current(worker_id, lane,
                                                        pool_->physical_routes);
          if (!pin.entry) {
            force_builtin = false;
            continue;
          }
          failed_run = pin.entry->run;
          try {
            const BoundedReadyStore::StartResult result =
                pool_->ready_store.commit_start(pin, worker_id, lane,
                                                pool_->physical_routes);
            if (result == BoundedReadyStore::StartResult::Started) {
              entry = std::move(pin.entry);
            } else if (result ==
                       BoundedReadyStore::StartResult::GrantUnavailable) {
              if (pool_->ready_store.mark_grant_blocked(pin, worker_id)) {
                if (!grant_blocked) {
                  grant_blocked_epoch = pool_->worker_notification_epoch;
                }
                grant_blocked = true;
                obsolete_retries = 0U;
                failed_run.reset();
              }
            } else if (result ==
                       BoundedReadyStore::StartResult::IdentityExhausted) {
              selection_failure = std::make_exception_ptr(
                  GraphError(GraphErrc::ComputeError,
                             "ExecutionService start identity exhausted."));
            }
          } catch (...) {
            selection_failure = std::current_exception();
          }
        }
      }
      if (entry || selection_failure || force_builtin) {
        continue;
      }

      bool binding_faulted = false;
      try {
        binding_faulted = binding->fault().has_value();
      } catch (const std::bad_alloc&) {
        force_builtin = true;
        continue;
      } catch (...) {
        selection_failure = std::current_exception();
        continue;
      }
      if (binding_faulted) {
        force_builtin = true;
        continue;
      }

      policy::PolicyInvocationResult decision;
      try {
        decision =
            binding->select(snapshot.candidates, snapshot.snapshot_generation,
                            snapshot.selection_sequence);
      } catch (const std::bad_alloc&) {
        force_builtin = true;
        continue;
      } catch (...) {
        selection_failure = std::current_exception();
        continue;
      }

      if (decision.kind ==
          policy::PolicyInvocationResult::Kind::InvalidPluginDecision) {
        try {
          if (!decision.fault.has_value()) {
            throw GraphError(GraphErrc::ComputeError,
                             "Policy violation omitted its fault snapshot.");
          }
          (void)binding->publish_first_fault(std::move(*decision.fault));
          force_builtin = true;
        } catch (...) {
          selection_failure = std::current_exception();
        }
        continue;
      }
      if (decision.kind ==
          policy::PolicyInvocationResult::Kind::BuiltinViolation) {
        std::lock_guard<std::mutex> lock(pool_->mutex);
        BoundedReadyStore::SelectionPin pin;
        try {
          pin = pool_->ready_store.resolve_current(
              snapshot.candidates.front().candidate_id, snapshot.service_class,
              worker_id, lane, pool_->physical_routes);
        } catch (...) {
        }
        failed_run = pin.entry ? pin.entry->run : nullptr;
        selection_failure = std::make_exception_ptr(GraphError(
            GraphErrc::ComputeError,
            "Trusted built-in policy returned an invalid decision."));
        continue;
      }

      BoundedReadyStore::SelectionPin pin;
      bool obsolete = false;
      {
        std::lock_guard<std::mutex> lock(pool_->mutex);
        const std::shared_ptr<policy::PolicyBinding> current =
            pool_->binding_for(snapshot.service_class);
        if (current.get() != binding.get() ||
            current->generation() != binding->generation()) {
          obsolete = true;
        } else {
          try {
            pin = pool_->ready_store.resolve_current(
                decision.candidate_id, snapshot.service_class, worker_id, lane,
                pool_->physical_routes);
          } catch (const std::bad_alloc&) {
            obsolete = true;
          } catch (...) {
            selection_failure = std::current_exception();
          }
          if (!selection_failure && !pin.entry) {
            obsolete = true;
          }
        }

        if (!obsolete && !selection_failure) {
          failed_run = pin.entry->run;
          try {
            const BoundedReadyStore::StartResult result =
                pool_->ready_store.commit_start(pin, worker_id, lane,
                                                pool_->physical_routes);
            if (result == BoundedReadyStore::StartResult::Started) {
              entry = std::move(pin.entry);
            } else if (result ==
                       BoundedReadyStore::StartResult::GrantUnavailable) {
              if (pool_->ready_store.mark_grant_blocked(pin, worker_id)) {
                if (!grant_blocked) {
                  grant_blocked_epoch = pool_->worker_notification_epoch;
                }
                grant_blocked = true;
                obsolete_retries = 0U;
                failed_run.reset();
              } else {
                obsolete = true;
              }
            } else if (result ==
                       BoundedReadyStore::StartResult::IdentityExhausted) {
              selection_failure = std::make_exception_ptr(
                  GraphError(GraphErrc::ComputeError,
                             "ExecutionService start identity exhausted."));
            } else {
              obsolete = true;
            }
          } catch (...) {
            selection_failure = std::current_exception();
          }
        }
      }
      if (obsolete && !selection_failure && !entry) {
        if (obsolete_retries < 2U) {
          ++obsolete_retries;
        } else {
          force_builtin = true;
        }
      }
    }

    if (grant_blocked) {
      std::lock_guard<std::mutex> lock(pool_->mutex);
      pool_->ready_store.clear_grant_blocked(worker_id);
    }

    if (selection_failure) {
      if (failed_run) {
        fail_run(failed_run, selection_failure);
      }
      continue;
    }
    if (!entry) {
      continue;
    }

    const std::shared_ptr<RunState> run = entry->run;
    try {
      const std::optional<ComputeRunCancellationReason> cancellation =
          entry->submission.lease_.observe_cancellation();
      if (cancellation.has_value()) {
        cancel_run(run, *cancellation);
      }
    } catch (...) {
      fail_run(run, std::current_exception());
    }
    bool skip_callback = false;
    {
      std::lock_guard<std::mutex> lock(run->mutex);
      if (run->cancelled || run->first_exception) {
        skip_callback = true;
      }
    }
    if (skip_callback) {
      retire_worker_entry(entry, run);
      continue;
    }

    tls_service_ = this;
    tls_run_state_ = run.get();
    tls_worker_id_ = worker_id;
    run->host->set_task_context(worker_id, run->id.value());
    lifecycle_telemetry_->increment_physical_counter(
        ExecutionLifecyclePhysicalCounter::EnteredCallback);
    try {
      if (entry->submission.metadata().is_initial_ready()) {
        log_event(ExecutionTraceAction::AssignInitial,
                  entry->submission.metadata().trace_node_id());
      }
      entry->submission.execute(*this);
      const std::optional<ComputeRunCancellationReason> cancellation =
          entry->submission.lease_.observe_cancellation();
      if (cancellation.has_value()) {
        cancel_run(run, *cancellation);
      }
    } catch (...) {
      fail_run(run, std::current_exception());
    }
    lifecycle_telemetry_->decrement_physical_counter(
        ExecutionLifecyclePhysicalCounter::EnteredCallback);
    run->host->clear_task_context();
    tls_worker_id_ = -1;
    tls_run_state_ = nullptr;
    tls_service_ = nullptr;
    retire_worker_entry(entry, run);
  }
}

}  // namespace ps::compute
