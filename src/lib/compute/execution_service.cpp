#include "compute/execution_service.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <limits>
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
         lhs.ready_bytes == rhs.ready_bytes;
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
  return ReadyTaskResourceDemand{owned_bytes, 0U, owned_bytes};
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
   * @param host_context Borrowed Graph observation target.
   * @param total_task_count Positive logical completion count.
   * @param task_resources Uniform adapter declaration for every submission.
   * @param ready_task_bytes Complete service-plus-adapter ready charge.
   * @param execution_task_bytes Complete service-plus-adapter retained charge.
   * @param run_reservation Complete admitted vector transferred into this Run.
   * @throws Nothing.
   */
  RunState(ComputeRunId run_id, SchedulerHostContext& host_context,
           int total_task_count, ReadyTaskResourceDemand task_resources,
           std::uint64_t ready_task_bytes, std::uint64_t execution_task_bytes,
           ResourceLedger::Reservation run_reservation) noexcept
      : id(run_id),
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
   * @throws Nothing after argument evaluation.
   */
  QueueEntry(std::shared_ptr<RunState> run_state,
             ReadyTaskSubmission ready_submission,
             ResourceLedger::Grant grant) noexcept
      : run(std::move(run_state)),
        priority(ready_submission.priority()),
        submission(std::move(ready_submission)),
        ready_grant(std::move(grant)) {}

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
  estimate.add_shared_control_block();
  estimate.add_bytes(static_cast<std::uint64_t>(graph_identity.capacity()));
  estimate.add_bytes(1U);
  return estimate.bytes();
}

/**
 * @brief Calculates mandatory once-per-Run service retained bytes.
 * @return Run state, shared control, registry node, and reservation state.
 * @throws GraphError when checked structural arithmetic overflows.
 * @note Fixed worker-pool infrastructure and ledger root state pre-exist Runs
 * and are not charged again here.
 */
std::uint64_t ExecutionService::service_run_envelope_bytes() {
  RetainedMemoryEstimator estimate("ExecutionService Run envelope");
  estimate.add_objects<RunState>();
  estimate.add_shared_control_block();
  estimate
      .add_objects<std::pair<const std::uint64_t, std::weak_ptr<RunState>>>();
  estimate.add_objects<void*>(3U);
  estimate.add_bytes(ResourceLedger::reservation_state_retained_memory_bytes());
  return estimate.bytes();
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

  RetainedMemoryEstimator shared_estimate(
      "ExecutionService shared Run envelope");
  shared_estimate.add_bytes(service_run_envelope_bytes());
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
  return CpuRunAdmissionEstimate{*complete, ready_bytes_per_task,
                                 execution_bytes_per_task};
}

/**
 * @brief Owns one entry/byte-bounded high/normal service ready store.
 *
 * @throws std::bad_alloc when lane storage grows.
 * @note Every method is called with `PoolState::mutex` held. The store owns no
 * synchronization, Run admission, dependency, or fairness authority.
 */
class ExecutionService::BoundedReadyStore final {
 public:
  /**
   * @brief Fixes the two ready-store limits for the service lifetime.
   * @param entry_limit Maximum stored entries across both lanes.
   * @param byte_limit Maximum accounted bytes across both lanes.
   * @throws Nothing.
   */
  BoundedReadyStore(std::uint64_t entry_limit,
                    std::uint64_t byte_limit) noexcept
      : entry_limit_(entry_limit), byte_limit_(byte_limit) {}

  /**
   * @brief Publishes one fully granted ready entry.
   * @param entry Owned entry with exactly one active ready grant.
   * @return True after publication, false without mutation on a local limit
   * or checked-counter violation.
   * @throws std::invalid_argument when the grant vector is structurally
   * invalid.
   * @throws std::bad_alloc when lane storage cannot grow.
   */
  bool try_push(const std::shared_ptr<QueueEntry>& entry) {
    if (!entry || !entry->ready_grant.has_value() ||
        !entry->ready_grant->active()) {
      throw std::invalid_argument(
          "Bounded ready store requires an active ready grant.");
    }
    const ResourceVector charge = entry->ready_grant->resources();
    if (charge.cpu_slots != 0U || charge.retained_memory_bytes != 0U ||
        charge.scratch_bytes != 0U || charge.ready_entries != 1U ||
        charge.ready_bytes == 0U) {
      throw std::invalid_argument(
          "Bounded ready store received an invalid ready grant.");
    }
    const std::optional<ResourceVector> next = checked_add_resources(
        ResourceVector{0U, 0U, 0U, entry_count_, byte_count_}, charge);
    if (!next.has_value() || next->ready_entries > entry_limit_ ||
        next->ready_bytes > byte_limit_) {
      return false;
    }

    const bool high =
        entry->priority == SchedulerTaskPriority::High ||
        entry->submission.metadata().intent() == ComputeIntent::RealTimeUpdate;
    if (high) {
      high_ready_.push_back(entry);
    } else {
      normal_ready_.push_back(entry);
    }
    entry_count_ = next->ready_entries;
    byte_count_ = next->ready_bytes;
    return true;
  }

  /**
   * @brief Removes the next high-before-normal ready entry.
   * @return Owned entry, or null when both lanes are empty.
   * @throws Nothing; an accounting invariant violation terminates.
   * @note Removal decrements store counters but the returned entry keeps its
   * grant until the worker acquires execution capacity or drops the entry.
   */
  std::shared_ptr<QueueEntry> pop() noexcept {
    std::shared_ptr<QueueEntry> entry;
    if (!high_ready_.empty()) {
      entry = std::move(high_ready_.front());
      high_ready_.pop_front();
    } else if (!normal_ready_.empty()) {
      entry = std::move(normal_ready_.front());
      normal_ready_.pop_front();
    } else {
      return nullptr;
    }
    remove_charge(*entry);
    return entry;
  }

  /**
   * @brief Purges every queued entry belonging to one Run.
   * @param run Matching retained Run state.
   * @return Number of removed entries.
   * @throws Nothing; an accounting invariant violation terminates.
   */
  std::size_t erase_run(const std::shared_ptr<RunState>& run) noexcept {
    std::size_t removed = 0U;
    const auto erase_lane =
        [this, &run,
         &removed](std::deque<std::shared_ptr<QueueEntry>>* lane) noexcept {
          for (auto it = lane->begin(); it != lane->end();) {
            if (*it && (*it)->run.get() == run.get()) {
              remove_charge(**it);
              it = lane->erase(it);
              ++removed;
            } else {
              ++it;
            }
          }
        };
    erase_lane(&high_ready_);
    erase_lane(&normal_ready_);
    return removed;
  }

  /**
   * @brief Drops all queued entries during final service teardown.
   * @return Nothing.
   * @throws Nothing; owner destruction is noexcept.
   */
  void clear() noexcept {
    high_ready_.clear();
    normal_ready_.clear();
    entry_count_ = 0U;
    byte_count_ = 0U;
  }

  /**
   * @brief Reports whether both ready lanes are empty.
   * @return True when no entry is stored.
   * @throws Nothing.
   */
  bool empty() const noexcept { return entry_count_ == 0U; }

  /**
   * @brief Returns current entries across both lanes.
   * @return Exact stored entry count.
   * @throws Nothing.
   */
  std::uint64_t entry_count() const noexcept { return entry_count_; }

  /**
   * @brief Returns current accounted bytes across both lanes.
   * @return Exact stored byte count.
   * @throws Nothing.
   */
  std::uint64_t byte_count() const noexcept { return byte_count_; }

 private:
  /**
   * @brief Removes one entry's exact ready grant charge from local counters.
   * @param entry Entry being popped or purged.
   * @return Nothing.
   * @throws Nothing; an invalid stored grant terminates.
   */
  void remove_charge(const QueueEntry& entry) noexcept {
    if (!entry.ready_grant.has_value() || !entry.ready_grant->active()) {
      std::terminate();
    }
    const ResourceVector charge = entry.ready_grant->resources();
    if (charge.ready_entries != 1U || charge.ready_entries > entry_count_ ||
        charge.ready_bytes > byte_count_) {
      std::terminate();
    }
    entry_count_ -= charge.ready_entries;
    byte_count_ -= charge.ready_bytes;
  }

  /** @brief Immutable maximum entries across both priority lanes. */
  const std::uint64_t entry_limit_;

  /** @brief Immutable maximum accounted bytes across both lanes. */
  const std::uint64_t byte_limit_;

  /** @brief Latency-hint FIFO used by high-priority and RT work. */
  std::deque<std::shared_ptr<QueueEntry>> high_ready_;

  /** @brief Throughput FIFO used by normal HP work. */
  std::deque<std::shared_ptr<QueueEntry>> normal_ready_;

  /** @brief Current entries across both deques. */
  std::uint64_t entry_count_ = 0U;

  /** @brief Current accounted bytes across both deques. */
  std::uint64_t byte_count_ = 0U;
};

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
   * @throws std::bad_alloc when ledger state cannot allocate.
   */
  explicit PoolState(ExecutionResourceLimits resource_limits)
      : ledger(resource_limits.resource_vector()),
        ready_store(resource_limits.ready_entries,
                    resource_limits.ready_bytes) {}

  /** @brief Serializes fixed configuration, queues, and active Run registry. */
  mutable std::mutex mutex;

  /** @brief Wakes fixed workers when ready work or shutdown is published. */
  std::condition_variable ready_cv;

  /** @brief Sole host-authoritative resource mint for this service. */
  ResourceLedger ledger;

  /** @brief Sole entry/byte-bounded ready-store owner. */
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
      32U,    1024U * kOneMebibyte, 512U * kOneMebibyte,
      65536U, 256U * kOneMebibyte,
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
  return pool_->ledger.try_reserve(
      ResourceVector{static_cast<std::uint64_t>(worker_slots)});
}

/** @copydoc ExecutionService::try_reserve_legacy_scheduler_worker_pair */
std::optional<ResourceLedger::ReservationPair>
ExecutionService::try_reserve_legacy_scheduler_worker_pair(
    unsigned int hp_worker_slots, unsigned int rt_worker_slots) {
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

  const CpuRunAdmissionEstimate admission = calculate_cpu_run_admission(
      configured_workers,
      initial_submissions.front().metadata().graph_identity(), total_task_count,
      run_resource_demand);
  std::optional<ResourceLedger::Reservation> reservation =
      pool_->ledger.try_reserve(admission.resources);
  if (!reservation.has_value()) {
    throw GraphError(
        GraphErrc::ComputeError,
        "ExecutionService resource ledger cannot admit the complete Run.");
  }

  auto run = std::make_shared<RunState>(
      run_id, host, total_task_count, run_resource_demand.task,
      admission.ready_bytes_per_task,
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
  return std::make_shared<QueueEntry>(run, std::move(submission),
                                      std::move(*ready_grant));
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
