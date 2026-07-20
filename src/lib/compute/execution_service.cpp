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

#include "photospider/core/graph_error.hpp"
#include "photospider/scheduler/scheduler.hpp"
#include "scheduler/scheduler_worker_budget.hpp"

namespace ps::compute {

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
ReadyTaskSubmission::ReadyTaskSubmission(ComputeRunLease lease,
                                         ComputeRunTaskIdentity identity,
                                         int trace_node_id,
                                         bool is_initial_ready,
                                         Executable executable,
                                         SchedulerTaskPriority priority)
    : metadata_(lease.descriptor(), trace_node_id, is_initial_ready),
      identity_(identity),
      lease_(std::move(lease)),
      executable_(std::move(executable)),
      priority_(priority) {
  if (identity_.run_id() != metadata_.run_id()) {
    throw std::invalid_argument(
        "ReadyTaskSubmission identity does not match its Run lease.");
  }
  if (!executable_) {
    throw std::invalid_argument(
        "ReadyTaskSubmission requires an owned executable.");
  }
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
   * @throws Nothing.
   */
  RunState(ComputeRunId run_id, SchedulerHostContext& host_context,
           int total_task_count) noexcept
      : id(run_id), host(&host_context), tasks_to_complete(total_task_count) {}

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

  /** @brief Guards completion, failure, admission, and in-flight state. */
  mutable std::mutex mutex;

  /** @brief Wakes the one caller waiting for this Run to settle. */
  std::condition_variable settled_cv;

  /** @brief Remaining logical tasks for a successful Run. */
  int tasks_to_complete = 0;

  /** @brief Worker callbacks that have left the process queue but not exited.
   */
  int in_flight = 0;

  /** @brief Exact first callback exception, or null before failure. */
  std::exception_ptr first_exception;

  /** @brief Whether dependency release may publish additional ready work. */
  bool accepting = true;
};

/**
 * @brief Move-owned process queue entry paired with matching Run state.
 *
 * @throws Nothing while moved after caller allocation succeeds.
 * @note Queue storage owns the complete submission and therefore its Run lease.
 */
struct ExecutionService::QueueEntry final {
  /**
   * @brief Transfers one ready submission into process queue ownership.
   * @param run_state Matching active Run retained through callback exit.
   * @param ready_submission Dependency-ready owned work.
   * @throws Nothing after argument evaluation.
   */
  QueueEntry(std::shared_ptr<RunState> run_state,
             ReadyTaskSubmission ready_submission) noexcept
      : run(std::move(run_state)),
        priority(ready_submission.priority()),
        submission(std::move(ready_submission)) {}

  /** @brief Matching active Run state. */
  std::shared_ptr<RunState> run;

  /** @brief Queue selection hint captured before submission movement. */
  SchedulerTaskPriority priority = SchedulerTaskPriority::Normal;

  /** @brief Complete owned callback, identity, metadata, and lease. */
  ReadyTaskSubmission submission;
};

/**
 * @brief Owns all fixed-pool implementation details and reservation state.
 *
 * @throws std::bad_alloc from container growth and worker creation staging.
 * @note One mutex defines queue-to-Run lock order: pool mutex is acquired
 * before a Run mutex whenever both are needed.
 */
class ExecutionService::PoolState final {
 public:
  /** @brief Serializes fixed configuration, queues, and active Run registry. */
  mutable std::mutex mutex;

  /** @brief Wakes fixed workers when ready work or shutdown is published. */
  std::condition_variable ready_cv;

  /** @brief Latency-hint FIFO used by RT and explicit high-priority work. */
  std::deque<std::shared_ptr<QueueEntry>> high_ready;

  /** @brief Throughput FIFO used by normal HP work. */
  std::deque<std::shared_ptr<QueueEntry>> normal_ready;

  /** @brief Active Run states keyed by non-reused numeric Run id. */
  std::unordered_map<uint64_t, std::weak_ptr<RunState>> active_runs;

  /** @brief Fixed service-owned worker threads. */
  std::vector<std::thread> workers;

  /** @brief One transitional process-budget share for the complete pool. */
  std::optional<SchedulerWorkerBudget::Reservation> reservation;

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

/** @copydoc ExecutionService::ExecutionService */
ExecutionService::ExecutionService() : pool_(std::make_unique<PoolState>()) {}

/** @copydoc ExecutionService::ExecutionService */
ExecutionService::ExecutionService(unsigned int worker_count)
    : ExecutionService() {
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
      pool_->high_ready.clear();
      pool_->normal_ready.clear();
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
      pool_->reservation.reset();
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
  std::optional<SchedulerWorkerBudget::Reservation> reservation =
      SchedulerWorkerBudget::process().try_reserve(resolved_workers);
  if (!reservation.has_value()) {
    throw GraphError(GraphErrc::ComputeError,
                     "process scheduler worker budget cannot admit the fixed "
                     "ExecutionService CPU pool");
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
  pool_->reservation.emplace(std::move(*reservation));
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
  std::ostringstream stream;
  stream << "Workers: " << pool_->configured_workers
         << ", Active runs: " << pool_->active_runs.size() << ", Ready tasks: "
         << (pool_->high_ready.size() + pool_->normal_ready.size());
  return stream.str();
}

/** @copydoc ExecutionService::execute_cpu_run */
void ExecutionService::execute_cpu_run(
    SchedulerHostContext& host,
    std::vector<ReadyTaskSubmission> initial_submissions,
    int total_task_count) {
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
  }

  auto run = std::make_shared<RunState>(run_id, host, total_task_count);
  std::vector<std::shared_ptr<QueueEntry>> staged_entries;
  staged_entries.reserve(initial_submissions.size());
  for (ReadyTaskSubmission& submission : initial_submissions) {
    staged_entries.push_back(
        std::make_shared<QueueEntry>(run, std::move(submission)));
  }

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
    std::size_t high_insertions = 0;
    std::size_t normal_insertions = 0;
    try {
      for (const std::shared_ptr<QueueEntry>& entry : staged_entries) {
        const bool high = entry->priority == SchedulerTaskPriority::High ||
                          entry->submission.metadata().intent() ==
                              ComputeIntent::RealTimeUpdate;
        if (high) {
          pool_->high_ready.push_back(entry);
          ++high_insertions;
        } else {
          pool_->normal_ready.push_back(entry);
          ++normal_insertions;
        }
      }
    } catch (...) {
      while (high_insertions-- > 0U) {
        pool_->high_ready.pop_back();
      }
      while (normal_insertions-- > 0U) {
        pool_->normal_ready.pop_back();
      }
      pool_->active_runs.erase(key);
      throw;
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

/** @copydoc ExecutionService::enqueue_submission */
void ExecutionService::enqueue_submission(const std::shared_ptr<RunState>& run,
                                          ReadyTaskSubmission submission) {
  if (submission.metadata().run_id() != run->id) {
    throw std::invalid_argument(
        "ReadyTaskSubmission does not belong to its routed Run.");
  }
  auto entry = std::make_shared<QueueEntry>(run, std::move(submission));

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

    const bool high =
        entry->priority == SchedulerTaskPriority::High ||
        entry->submission.metadata().intent() == ComputeIntent::RealTimeUpdate;
    if (high) {
      pool_->high_ready.push_back(std::move(entry));
    } else {
      pool_->normal_ready.push_back(std::move(entry));
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
      const auto belongs_to_run =
          [&run](const std::shared_ptr<QueueEntry>& entry) {
            return entry && entry->run.get() == run.get();
          };
      pool_->high_ready.erase(
          std::remove_if(pool_->high_ready.begin(), pool_->high_ready.end(),
                         belongs_to_run),
          pool_->high_ready.end());
      pool_->normal_ready.erase(
          std::remove_if(pool_->normal_ready.begin(), pool_->normal_ready.end(),
                         belongs_to_run),
          pool_->normal_ready.end());
    }
    run->settled_cv.notify_all();
  } catch (...) {
  }
}

/** @copydoc ExecutionService::worker_loop */
void ExecutionService::worker_loop(int worker_id) noexcept {
  for (;;) {
    std::shared_ptr<QueueEntry> entry;
    {
      std::unique_lock<std::mutex> lock(pool_->mutex);
      pool_->ready_cv.wait(lock, [this]() {
        return pool_->stopping || !pool_->high_ready.empty() ||
               !pool_->normal_ready.empty();
      });
      if (pool_->stopping) {
        return;
      }
      if (!pool_->high_ready.empty()) {
        entry = std::move(pool_->high_ready.front());
        pool_->high_ready.pop_front();
      } else {
        entry = std::move(pool_->normal_ready.front());
        pool_->normal_ready.pop_front();
      }
      std::lock_guard<std::mutex> run_lock(entry->run->mutex);
      if (!entry->run->accepting || entry->run->first_exception) {
        entry.reset();
      } else {
        ++entry->run->in_flight;
      }
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
