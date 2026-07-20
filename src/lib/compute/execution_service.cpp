#include "compute/execution_service.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
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
#include <unordered_map>
#include <utility>
#include <vector>

#include "compute/resource_demand_estimator.hpp"
#include "photospider/core/graph_error.hpp"
#include "photospider/scheduler/scheduler.hpp"
#include "scheduler/scheduler_worker_limits.hpp"

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
                                     int trace_node_id, bool is_initial_ready)
    : run_id_(descriptor.id()),
      graph_identity_(descriptor.graph_identity()),
      revision_(descriptor.revision()),
      target_node_id_(descriptor.target_node_id()),
      intent_(descriptor.intent()),
      quality_(descriptor.quality()),
      qos_(descriptor.qos()),
      trace_node_id_(trace_node_id),
      is_initial_ready_(is_initial_ready) {
}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ReadyTaskSubmission::ReadyTaskSubmission */
ReadyTaskSubmission::ReadyTaskSubmission(
    ComputeRunLease lease, ComputeRunTaskIdentity identity, int trace_node_id,
    bool is_initial_ready, Executable executable,
    SchedulerTaskPriority priority, ReadyTaskResourceDemand resource_demand)
    : metadata_(lease.descriptor(), trace_node_id, is_initial_ready),
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
void ReadyTaskSubmission::execute(SchedulerTaskRuntime& task_runtime) {
  try {
    executable_(lease_, identity_, task_runtime);
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    try {
      (void)lease_.publish_task_failure(identity_, failure);
    } catch (...) {
    }
    std::rethrow_exception(failure);
  }
}

/**
 * @brief Owns isolated completion and observation state for one active Run.
 *
 * @throws Nothing from construction after caller-owned values are available.
 * @note The service registry and every queued entry retain shared ownership.
 * The host remains borrowed only until `execute_cpu_run()` observes settlement.
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
   * @param total_task_count Positive logical completion count.
   * @param task_resources Uniform adapter declaration for every submission.
   * @param ready_task_bytes Complete service-plus-adapter ready charge.
   * @param execution_task_bytes Complete service-plus-adapter retained charge.
   * @param run_reservation Complete admitted vector transferred into this Run.
   * @throws Nothing.
   */
  RunState(ComputeRunId run_id, std::string graph_identity,
           std::string graph_key, ComputeRunQos qos,
           SchedulerHostContext& host_context, int total_task_count,
           ReadyTaskResourceDemand task_resources,
           std::uint64_t ready_task_bytes, std::uint64_t execution_task_bytes,
           ResourceLedger::Reservation run_reservation) noexcept
      : id(run_id),
        graph(std::move(graph_identity)),
        available_graph_key(std::move(graph_key)),
        policy_qos(std::move(qos)),
        host(&host_context),
        resource_demand(task_resources),
        ready_bytes_per_task(ready_task_bytes),
        execution_retained_bytes_per_task(execution_task_bytes),
        reservation(std::move(run_reservation)),
        tasks_to_complete(total_task_count) {}

  /**
   * @brief Tests whether the caller-side Run wait may finish.
   * @return True after successful logical completion and callback drainage, or
   * after failure and callback drainage.
   * @throws Nothing.
   * @note Caller holds `mutex`.
   */
  bool settled() const noexcept {
    return in_flight == 0 &&
           (first_exception != nullptr || tasks_to_complete == 0);
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
  SchedulerHostContext* const host;

  /** @brief Uniform trusted resources required by every logical task. */
  const ReadyTaskResourceDemand resource_demand;

  /** @brief Complete ready-store bytes granted for every logical task. */
  const std::uint64_t ready_bytes_per_task;

  /** @brief Complete retained bytes granted while one task executes. */
  const std::uint64_t execution_retained_bytes_per_task;

  /**
   * @brief Complete Run vector retained until all queued/executing grants end.
   */
  ResourceLedger::Reservation reservation;

  /** @brief Guards completion, failure, admission, and in-flight state. */
  mutable std::mutex mutex;

  /** @brief Wakes the one caller waiting for this Run to settle. */
  std::condition_variable settled_cv;

  /** @brief Remaining logical tasks for a successful Run. */
  int tasks_to_complete = 0;

  /** @brief Worker callbacks that have left the service queue but not exited.
   */
  int in_flight = 0;

  /** @brief Exact first callback exception, or null before failure. */
  std::exception_ptr first_exception;

  /** @brief Whether dependency release may publish additional ready work. */
  bool accepting = true;
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
  SchedulerTaskPriority priority = SchedulerTaskPriority::Normal;

  /** @brief Complete owned callback, identity, metadata, and lease. */
  ReadyTaskSubmission submission;

  /** @brief Ready-store authority released after worker removal or purge. */
  std::optional<ResourceLedger::Grant> ready_grant;

  /** @brief CPU/memory/scratch authority held across callback execution. */
  std::optional<ResourceLedger::Grant> execution_grant;

  /** @brief Checked work plus ready-byte quanta used only for ordering. */
  const std::uint64_t policy_service_cost;

  /** @brief Stable successful-dispatch count observed at publication. */
  std::uint64_t enqueued_dispatch_count = 0U;

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
 * @param demand Shared once-per-Run and uniform per-task declarations.
 * @return Complete root vector and reusable child-grant envelopes.
 * @throws std::invalid_argument for a nonpositive task count.
 * @throws GraphError when any addition or multiplication overflows.
 * @note Retained and scratch task bytes scale with maximum callback
 * concurrency, not batch size. Ready entries/bytes scale with every logical
 * task so dependency release cannot exceed the admitted reservation.
 */
ExecutionService::CpuRunAdmissionEstimate
ExecutionService::calculate_cpu_run_admission(unsigned int configured_workers,
                                              const std::string& graph_identity,
                                              int total_task_count,
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
  const std::uint64_t concurrent_task_count = std::min(
      static_cast<std::uint64_t>(configured_workers), logical_task_count);

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
class ExecutionService::SchedulerPolicy {
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
  virtual ~SchedulerPolicy() noexcept = default;

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
class ExecutionService::InteractiveSchedulerPolicy final
    : public ExecutionService::SchedulerPolicy {
 public:
  /** @copydoc SchedulerPolicy::service_class */
  ComputeRunQosClass service_class() const noexcept override {
    return ComputeRunQosClass::Interactive;
  }

  /** @copydoc SchedulerPolicy::may_consume_headroom */
  bool may_consume_headroom() const noexcept override { return true; }

  /** @copydoc SchedulerPolicy::precedes */
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
class ExecutionService::ThroughputSchedulerPolicy final
    : public ExecutionService::SchedulerPolicy {
 public:
  /** @copydoc SchedulerPolicy::service_class */
  ComputeRunQosClass service_class() const noexcept override {
    return ComputeRunQosClass::Throughput;
  }

  /** @copydoc SchedulerPolicy::may_consume_headroom */
  bool may_consume_headroom() const noexcept override { return false; }

  /** @copydoc SchedulerPolicy::precedes */
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

/**
 * @brief Owns one policy-aware entry/byte-bounded service ready store.
 *
 * @throws std::bad_alloc when entry or policy-row storage grows.
 * @note Every method is called with `PoolState::mutex` held. The store owns
 * ready values and ordering history but no synchronization, Run admission,
 * dependency, resource-token, worker, completion, or lifecycle authority.
 */
class ExecutionService::BoundedReadyStore final {
 public:
  /**
   * @brief Fixes aggregate ready-store limits for the service lifetime.
   * @param entry_limit Maximum stored entries across both service classes.
   * @param byte_limit Maximum accounted bytes across both service classes.
   * @throws Nothing.
   */
  BoundedReadyStore(std::uint64_t entry_limit,
                    std::uint64_t byte_limit) noexcept
      : entry_limit_(entry_limit), byte_limit_(byte_limit) {}

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
  const SchedulerPolicy& policy_for(
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
        next_enqueue_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
      return false;
    }

    bool graph_inserted = false;
    auto graph_it = graph_states_.find(entry->run->graph);
    if (graph_it == graph_states_.end()) {
      auto inserted =
          graph_states_.try_emplace(std::move(entry->run->available_graph_key));
      graph_it = inserted.first;
      graph_inserted = inserted.second;
      if (!graph_inserted) {
        std::terminate();
      }
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
    entry->high_lane = entry->priority == SchedulerTaskPriority::High;
    entry->enqueued_dispatch_count = dispatch_count_;
    entry->enqueue_sequence = ++next_enqueue_sequence_;
    link_entry(run_state, *entry);
    entry_count_ = next->ready_entries;
    byte_count_ = next->ready_bytes;
    return true;
  }

  /**
   * @brief Removes the next policy-selected ready entry.
   * @return Owned entry, or null when no ready value is stored.
   * @throws Nothing; an accounting or policy invariant violation terminates.
   * @note Removal decrements store counters but the returned entry keeps its
   * ready grant until the worker acquires execution capacity or drops it.
   * Inter-class three-to-one arbitration completes before the selected
   * policy applies class-local aging, deadline, and fair-score ordering.
   */
  std::shared_ptr<QueueEntry> pop() noexcept {
    if (entry_count_ == 0U) {
      return nullptr;
    }

    const bool interactive_ready = has_ready(ComputeRunQosClass::Interactive);
    const bool throughput_ready = has_ready(ComputeRunQosClass::Throughput);
    if (!interactive_ready && !throughput_ready) {
      std::terminate();
    }

    ComputeRunQosClass selected_class = ComputeRunQosClass::Throughput;
    if (interactive_ready && throughput_ready) {
      if (consecutive_interactive_ < kInteractiveBurstLimit) {
        selected_class = ComputeRunQosClass::Interactive;
      }
    } else if (interactive_ready) {
      selected_class = ComputeRunQosClass::Interactive;
    }

    SelectedEntry selected = select_from_class(policy_for(selected_class));
    if (selected.entry == nullptr || selected.run_state == nullptr) {
      std::terminate();
    }
    selected.run_state->charged_service = selected.run_score;
    selected.run_state->graph->charged_service = selected.graph_score;

    std::shared_ptr<QueueEntry> entry = *selected.entry->store_position;
    remove_entry(*selected.entry, *selected.run_state);
    if (dispatch_count_ != std::numeric_limits<std::uint64_t>::max()) {
      ++dispatch_count_;
    }
    if (selected_class == ComputeRunQosClass::Interactive && throughput_ready) {
      if (consecutive_interactive_ !=
          std::numeric_limits<std::uint64_t>::max()) {
        ++consecutive_interactive_;
      }
    } else {
      consecutive_interactive_ = 0U;
    }
    return entry;
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
   * @brief Drops all entries and policy rows during final service teardown.
   * @return Nothing.
   * @throws Nothing; owner destruction is noexcept.
   */
  void clear() noexcept {
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
    /** @brief Raw work/byte service charged to this Graph. */
    std::uint64_t charged_service = 0U;

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

    /** @brief Weight-normalized service charged to this Run. */
    std::uint64_t charged_service = 0U;

    /** @brief Same-Run high-hint FIFO. */
    LaneEndpoints high;

    /** @brief Same-Run normal-hint FIFO. */
    LaneEndpoints normal;
  };

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
   * @brief Tests one entry's deterministic dispatch-count age.
   * @param entry Published entry.
   * @return True after at least eight later successful dispatches.
   * @throws Nothing.
   */
  bool is_aged(const QueueEntry& entry) const noexcept {
    return dispatch_count_ >= entry.enqueued_dispatch_count &&
           dispatch_count_ - entry.enqueued_dispatch_count >=
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
   * @return Complete chosen row/entry and next accounting scores.
   * @throws Nothing.
   * @note Aged candidates precede ordinary comparison only within the class
   * already selected by inter-class arbitration.
   */
  SelectedEntry select_from_class(const SchedulerPolicy& policy) noexcept {
    SelectedEntry best;
    SchedulerPolicy::Candidate best_snapshot;
    for (auto& row : run_states_) {
      PolicyRunState& state = row.second;
      if (state.run->policy_qos.service_class != policy.service_class()) {
        continue;
      }
      QueueEntry* entry = candidate_entry(state);
      if (entry == nullptr) {
        continue;
      }
      const std::uint64_t graph_score = saturating_add(
          state.graph->charged_service, entry->policy_service_cost);
      const std::uint64_t run_score =
          saturating_add(state.charged_service,
                         policy.normalized_cost(entry->policy_service_cost,
                                                state.run->policy_qos.weight));
      const bool aged = is_aged(*entry);
      const SchedulerPolicy::Candidate snapshot{
          state.run->policy_qos.deadline,
          graph_score,
          run_score,
          entry->enqueue_sequence,
      };

      bool replace = best.entry == nullptr;
      if (!replace && aged != best.aged) {
        replace = aged;
      } else if (!replace && aged && best.aged) {
        replace = entry->enqueue_sequence < best.entry->enqueue_sequence;
      } else if (!replace && !aged && !best.aged) {
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
  }

  /** @brief Immutable maximum entries across all classes and lanes. */
  const std::uint64_t entry_limit_;

  /** @brief Immutable maximum accounted bytes across all classes and lanes. */
  const std::uint64_t byte_limit_;

  /** @brief Store-owned list node for every currently ready value. */
  std::list<std::shared_ptr<QueueEntry>> entries_;

  /** @brief Persistent active-Run policy rows keyed by opaque Run id. */
  std::map<std::uint64_t, PolicyRunState> run_states_;

  /** @brief Shared Graph fairness rows keyed by stable Graph identity. */
  std::map<std::string, PolicyGraphState> graph_states_;

  /** @brief Stateless deadline-aware interactive built-in strategy. */
  InteractiveSchedulerPolicy interactive_policy_;

  /** @brief Stateless weighted throughput built-in strategy. */
  ThroughputSchedulerPolicy throughput_policy_;

  /** @brief Current entries across all classes and lanes. */
  std::uint64_t entry_count_ = 0U;

  /** @brief Current exact ready-grant bytes across all entries. */
  std::uint64_t byte_count_ = 0U;

  /** @brief Successful policy selections since service construction. */
  std::uint64_t dispatch_count_ = 0U;

  /** @brief Stable publication sequence assigned without reuse. */
  std::uint64_t next_enqueue_sequence_ = 0U;

  /** @brief Current interactive burst while throughput remains ready. */
  std::uint64_t consecutive_interactive_ = 0U;
};

/** @copydoc ExecutionService::service_run_envelope_bytes */
std::uint64_t ExecutionService::service_run_envelope_bytes(
    const std::string& graph_identity, const std::string& graph_key) {
  RetainedMemoryEstimator estimate("ExecutionService Run envelope");
  estimate.add_objects<RunState>();
  estimate.add_shared_control_block();
  estimate
      .add_objects<std::pair<const std::uint64_t, std::weak_ptr<RunState>>>();
  estimate.add_objects<void*>(3U);
  estimate.add_bytes(BoundedReadyStore::run_policy_envelope_bytes());
  estimate.add_bytes(static_cast<std::uint64_t>(graph_identity.capacity()));
  estimate.add_bytes(1U);
  estimate.add_bytes(static_cast<std::uint64_t>(graph_key.capacity()));
  estimate.add_bytes(1U);
  estimate.add_bytes(ResourceLedger::reservation_state_retained_memory_bytes());
  return estimate.bytes();
}

namespace {

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
   * @throws std::invalid_argument when interactive headroom exceeds a limit.
   * @throws std::bad_alloc when ledger state cannot allocate.
   */
  explicit PoolState(ExecutionResourceLimits resource_limits)
      : resource_limits(resource_limits.resource_vector()),
        interactive_headroom(resource_limits.interactive_headroom),
        general_capacity(
            calculate_general_capacity(resource_limits.resource_vector(),
                                       resource_limits.interactive_headroom)),
        ledger(resource_limits.resource_vector()),
        ready_store(resource_limits.ready_entries,
                    resource_limits.ready_bytes) {}

  /**
   * @brief Applies one built-in policy ceiling before ordinary ledger commit.
   * @param resources Complete checked root vector requested by one Run/owner.
   * @param service_class Explicit policy class; no intent inference occurs.
   * @return Ledger-minted reservation, or null without mutation when either
   * policy ceiling or authoritative capacity is unavailable.
   * @throws std::bad_alloc or std::system_error from ledger admission.
   * @note Caller holds `mutex`. The policy mints no token; `ledger` performs
   * the final atomic capacity validation and constructs the sole authority.
   */
  std::optional<ResourceLedger::Reservation> try_reserve_for_policy(
      const ResourceVector& resources, ComputeRunQosClass service_class) {
    const SchedulerPolicy& policy = ready_store.policy_for(service_class);
    if (!policy.may_consume_headroom()) {
      const ResourceLedger::Snapshot snapshot = ledger.snapshot();
      if (snapshot.limits != resource_limits) {
        std::terminate();
      }
      const std::optional<ResourceVector> after =
          checked_add_resources(snapshot.reserved, resources);
      if (!after.has_value() || !resources_fit(*after, general_capacity)) {
        return std::nullopt;
      }
    }
    return ledger.try_reserve(resources);
  }

  /** @brief Immutable complete capacity configured for the sole ledger. */
  const ResourceVector resource_limits;

  /** @brief Immutable subset protected from Throughput Run admission. */
  const ResourceVector interactive_headroom;

  /** @brief Immutable ceiling for Throughput Runs. */
  const ResourceVector general_capacity;

  /** @brief Serializes fixed configuration, queues, and active Run registry. */
  mutable std::mutex mutex;

  /** @brief Wakes fixed workers when ready work or shutdown is published. */
  std::condition_variable ready_cv;

  /** @brief Sole host-authoritative resource mint for this service. */
  ResourceLedger ledger;

  /** @brief Sole policy-aware entry/byte-bounded ready-store owner. */
  BoundedReadyStore ready_store;

  /** @brief Active Run states keyed by non-reused numeric Run id. */
  std::unordered_map<uint64_t, std::weak_ptr<RunState>> active_runs;

  /** @brief Fixed service-owned worker threads. */
  std::vector<std::thread> workers;

  /** @brief Frozen worker count, or zero before complete configuration. */
  unsigned int configured_workers = 0U;

  /** @brief True after destructor requests worker-loop exit. */
  bool stopping = false;
};

/** @brief Current service-worker Run context, null outside callbacks. */
thread_local ExecutionService::RunState* ExecutionService::tls_run_state_ =
    nullptr;

/** @brief Current service worker id, or -1 outside callbacks. */
thread_local int ExecutionService::tls_worker_id_ = -1;

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
    : pool_(std::make_unique<PoolState>(resource_limits)) {}

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

  std::vector<std::thread> workers;
  try {
    {
      std::lock_guard<std::mutex> lock(pool_->mutex);
      pool_->stopping = true;
      pool_->ready_store.clear();
      workers.swap(pool_->workers);
    }
    pool_->ready_cv.notify_all();
    for (std::thread& worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    {
      std::lock_guard<std::mutex> lock(pool_->mutex);
      pool_->active_runs.clear();
      pool_->configured_workers = 0U;
    }
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ExecutionService::configure_worker_count */
void ExecutionService::configure_worker_count(unsigned int worker_count) {
  if (worker_count > kSchedulerWorkerRequestMax) {
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

  const unsigned int resolved_workers = resolve_scheduler_worker_count(
      worker_count, std::thread::hardware_concurrency());
  const ResourceLedger::Snapshot resources = pool_->ledger.snapshot();
  if (static_cast<std::uint64_t>(resolved_workers) >
      resources.limits.cpu_slots) {
    throw std::invalid_argument(
        "ExecutionService worker count exceeds configured CPU capacity.");
  }

  std::vector<std::thread> staged_workers;
  staged_workers.reserve(resolved_workers);
  pool_->configured_workers = resolved_workers;
  try {
    for (unsigned int index = 0; index < resolved_workers; ++index) {
      staged_workers.emplace_back(&ExecutionService::worker_loop, this,
                                  static_cast<int>(index));
    }
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    pool_->stopping = true;
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
         pool_->workers.size() == pool_->configured_workers;
}

/** @copydoc ExecutionService::get_stats */
std::string ExecutionService::get_stats() const {
  std::lock_guard<std::mutex> lock(pool_->mutex);
  const ResourceLedger::Snapshot resources = pool_->ledger.snapshot();
  std::ostringstream stream;
  stream << "Workers: " << pool_->configured_workers
         << ", Active runs: " << pool_->active_runs.size()
         << ", Ready tasks: " << pool_->ready_store.entry_count()
         << ", Ready bytes: " << pool_->ready_store.byte_count()
         << ", Reserved CPU: " << resources.reserved.cpu_slots;
  return stream.str();
}

/** @copydoc ExecutionService::try_reserve_legacy_scheduler_workers */
std::optional<ResourceLedger::Reservation>
ExecutionService::try_reserve_legacy_scheduler_workers(
    unsigned int worker_slots) {
  std::lock_guard<std::mutex> lock(pool_->mutex);
  return pool_->ledger.try_reserve(
      ResourceVector{static_cast<std::uint64_t>(worker_slots)});
}

/** @copydoc ExecutionService::try_reserve_legacy_scheduler_worker_pair */
std::optional<ResourceLedger::ReservationPair>
ExecutionService::try_reserve_legacy_scheduler_worker_pair(
    unsigned int hp_worker_slots, unsigned int rt_worker_slots) {
  std::lock_guard<std::mutex> lock(pool_->mutex);
  return pool_->ledger.try_reserve_pair(
      ResourceVector{static_cast<std::uint64_t>(hp_worker_slots)},
      ResourceVector{static_cast<std::uint64_t>(rt_worker_slots)});
}

/** @copydoc ExecutionService::resource_snapshot */
ResourceLedger::Snapshot ExecutionService::resource_snapshot() const {
  return pool_->ledger.snapshot();
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
  return calculate_cpu_run_admission(configured_workers,
                                     representative.metadata().graph_identity(),
                                     total_task_count, run_resource_demand)
      .resources;
}

/** @copydoc ExecutionService::execute_cpu_run */
void ExecutionService::execute_cpu_run(
    SchedulerHostContext& host,
    std::vector<ReadyTaskSubmission> initial_submissions, int total_task_count,
    CpuRunResourceDemand run_resource_demand) {
  if (total_task_count <= 0 || initial_submissions.empty()) {
    throw std::invalid_argument(
        "ExecutionService requires a nonempty active Run batch.");
  }
  if (initial_submissions.size() > static_cast<std::size_t>(total_task_count)) {
    throw std::invalid_argument(
        "ExecutionService initial ready count exceeds total task count.");
  }
  const ComputeRunId run_id = initial_submissions.front().metadata().run_id();
  for (const ReadyTaskSubmission& submission : initial_submissions) {
    if (submission.metadata().run_id() != run_id) {
      throw std::invalid_argument(
          "ExecutionService initial batch mixes multiple Runs.");
    }
    if (submission.resource_demand() != run_resource_demand.task) {
      throw std::invalid_argument(
          "ExecutionService initial batch resource declaration mismatch.");
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
      run_resource_demand);
  std::optional<ResourceLedger::Reservation> reservation;
  {
    std::lock_guard<std::mutex> lock(pool_->mutex);
    if (pool_->stopping) {
      throw std::logic_error("ExecutionService is stopping.");
    }
    reservation = pool_->try_reserve_for_policy(
        admission.resources,
        initial_submissions.front().metadata().qos().service_class);
  }
  if (!reservation.has_value()) {
    throw GraphError(
        GraphErrc::ComputeError,
        "ExecutionService policy/ledger cannot admit the complete Run.");
  }

  auto run = std::make_shared<RunState>(
      run_id, std::move(admission.policy_graph_identity),
      std::move(admission.policy_graph_key),
      initial_submissions.front().metadata().qos(), host, total_task_count,
      run_resource_demand.task, admission.ready_bytes_per_task,
      admission.execution_retained_bytes_per_task, std::move(*reservation));

  {
    std::vector<std::shared_ptr<QueueEntry>> staged_entries;
    staged_entries.reserve(initial_submissions.size());
    for (ReadyTaskSubmission& submission : initial_submissions) {
      staged_entries.push_back(make_queue_entry(run, std::move(submission)));
    }
    const std::size_t staged_submission_size = initial_submissions.size();
    const std::size_t staged_submission_capacity =
        initial_submissions.capacity();
    release_initial_submission_storage(initial_submissions);
    observe_initial_submission_storage(
        admission.resources, staged_submission_size, staged_submission_capacity,
        initial_submissions);

    {
      std::lock_guard<std::mutex> lock(pool_->mutex);
      if (pool_->configured_workers == 0U || pool_->workers.empty()) {
        throw std::logic_error(
            "ExecutionService worker count is not configured.");
      }
      if (pool_->stopping) {
        throw std::logic_error("ExecutionService is stopping.");
      }
      const uint64_t key = run_id.value();
      const auto existing = pool_->active_runs.find(key);
      if (existing != pool_->active_runs.end() && !existing->second.expired()) {
        throw std::logic_error("ExecutionService Run id is already active.");
      }

      pool_->active_runs.insert_or_assign(key, run);
      try {
        for (const std::shared_ptr<QueueEntry>& entry : staged_entries) {
          if (!pool_->ready_store.try_push(entry)) {
            throw GraphError(
                GraphErrc::ComputeError,
                "ExecutionService bounded ready store rejected initial work.");
          }
        }
      } catch (...) {
        (void)pool_->ready_store.erase_run(run);
        pool_->active_runs.erase(key);
        pool_->ready_store.retire_run(run);
        throw;
      }
    }
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
    std::lock_guard<std::mutex> lock(pool_->mutex);
    const auto current = pool_->active_runs.find(run_id.value());
    if (current != pool_->active_runs.end()) {
      const std::shared_ptr<RunState> published = current->second.lock();
      if (!published || published.get() == run.get()) {
        pool_->active_runs.erase(current);
      }
    }
    pool_->ready_store.retire_run(run);
  }

  if (failure) {
    std::rethrow_exception(failure);
  }
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
  const ResourceVector ready_resources{0U, 0U, 0U, 1U,
                                       run->ready_bytes_per_task};
  std::optional<ResourceLedger::Grant> ready_grant =
      run->reservation.try_grant(ready_resources);
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
  std::shared_ptr<QueueEntry> entry =
      make_queue_entry(run, std::move(submission));

  {
    std::lock_guard<std::mutex> pool_lock(pool_->mutex);
    if (pool_->stopping) {
      throw std::logic_error("ExecutionService is stopping.");
    }
    const auto active = pool_->active_runs.find(run->id.value());
    if (active == pool_->active_runs.end() ||
        active->second.lock().get() != run.get()) {
      throw std::logic_error(
          "ExecutionService Run no longer accepts ready work.");
    }
    std::lock_guard<std::mutex> run_lock(run->mutex);
    if (!run->accepting || run->first_exception) {
      throw std::logic_error(
          "ExecutionService Run no longer accepts ready work.");
    }

    if (!pool_->ready_store.try_push(entry)) {
      throw GraphError(
          GraphErrc::ComputeError,
          "ExecutionService bounded ready store rejected dependent work.");
    }
  }
  pool_->ready_cv.notify_one();
}

/** @copydoc ExecutionService::find_active_run */
std::shared_ptr<ExecutionService::RunState> ExecutionService::find_active_run(
    ComputeRunId run_id) {
  std::lock_guard<std::mutex> lock(pool_->mutex);
  const auto active = pool_->active_runs.find(run_id.value());
  if (active == pool_->active_runs.end()) {
    throw std::invalid_argument("ReadyTaskSubmission names no active Run.");
  }
  std::shared_ptr<RunState> run = active->second.lock();
  if (!run || run->id != run_id) {
    throw std::invalid_argument("ReadyTaskSubmission names no active Run.");
  }
  return run;
}

/** @copydoc ExecutionService::submit_ready_submission */
void ExecutionService::submit_ready_submission(ReadyTaskSubmission submission) {
  const ComputeRunId run_id = submission.metadata().run_id();
  std::shared_ptr<RunState> run;
  if (tls_run_state_ != nullptr && tls_run_state_->id == run_id) {
    run = tls_run_state_->shared_from_this();
  } else {
    run = find_active_run(run_id);
  }
  enqueue_submission(run, std::move(submission));
}

/** @copydoc ExecutionService::available_devices */
std::vector<Device> ExecutionService::available_devices() const {
  return {Device::CPU};
}

/** @copydoc ExecutionService::submit_initial_task_handles */
void ExecutionService::submit_initial_task_handles(
    std::vector<TaskHandle>&& handles, int total_task_count,
    SchedulerTaskPriority priority) {
  (void)handles;
  (void)total_task_count;
  (void)priority;
  throw std::logic_error(
      "ExecutionService rejects borrowed initial task handles.");
}

/** @copydoc ExecutionService::submit_ready_task_handles_from_worker */
void ExecutionService::submit_ready_task_handles_from_worker(
    std::vector<TaskHandle>&& handles, SchedulerTaskPriority priority) {
  (void)handles;
  (void)priority;
  throw std::logic_error(
      "ExecutionService rejects borrowed ready task handles.");
}

/** @copydoc ExecutionService::submit_ready_task_any_thread */
void ExecutionService::submit_ready_task_any_thread(
    Task&& task, SchedulerTaskPriority priority,
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
  if (run.first_exception) {
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
  if (run.first_exception) {
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
void ExecutionService::log_event(SchedulerTraceAction action, int node_id) {
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
      if (!run->first_exception) {
        run->first_exception = std::move(failure);
      }
      run->accepting = false;
      (void)pool_->ready_store.erase_run(run);
    }
    run->settled_cv.notify_all();
  } catch (...) {
  }
}

/** @copydoc ExecutionService::worker_loop */
void ExecutionService::worker_loop(int worker_id) noexcept {
  for (;;) {
    std::shared_ptr<QueueEntry> entry;
    std::exception_ptr grant_failure;
    {
      std::unique_lock<std::mutex> lock(pool_->mutex);
      pool_->ready_cv.wait(lock, [this]() {
        return pool_->stopping || !pool_->ready_store.empty();
      });
      if (pool_->stopping) {
        return;
      }
      entry = pool_->ready_store.pop();
      if (!entry) {
        std::terminate();
      }
      std::lock_guard<std::mutex> run_lock(entry->run->mutex);
      if (!entry->run->accepting || entry->run->first_exception) {
        entry.reset();
      } else {
        try {
          const ResourceVector execution_resources{
              1U,
              entry->run->execution_retained_bytes_per_task,
              entry->run->resource_demand.scratch_bytes,
              0U,
              0U,
          };
          std::optional<ResourceLedger::Grant> execution_grant =
              entry->run->reservation.try_grant(execution_resources);
          if (!execution_grant.has_value()) {
            throw GraphError(
                GraphErrc::ComputeError,
                "Run reservation cannot grant worker execution resources.");
          }
          entry->execution_grant.emplace(std::move(*execution_grant));
          entry->ready_grant.reset();
          ++entry->run->in_flight;
        } catch (...) {
          grant_failure = std::current_exception();
        }
      }
    }
    if (grant_failure) {
      fail_run(entry->run, grant_failure);
      entry.reset();
      continue;
    }
    if (!entry) {
      continue;
    }

    const std::shared_ptr<RunState> run = entry->run;
    tls_run_state_ = run.get();
    tls_worker_id_ = worker_id;
    run->host->set_task_context(worker_id, run->id.value());
    try {
      if (entry->submission.metadata().is_initial_ready()) {
        log_event(SchedulerTraceAction::AssignInitial,
                  entry->submission.metadata().trace_node_id());
      }
      entry->submission.execute(*this);
    } catch (...) {
      fail_run(run, std::current_exception());
    }
    run->host->clear_task_context();
    tls_worker_id_ = -1;
    tls_run_state_ = nullptr;
    entry->execution_grant.reset();

    {
      std::lock_guard<std::mutex> lock(run->mutex);
      if (run->in_flight <= 0) {
        std::terminate();
      }
      --run->in_flight;
      if (run->settled()) {
        run->settled_cv.notify_all();
      }
    }
  }
}

}  // namespace ps::compute
