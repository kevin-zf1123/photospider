#include "compute/execution_service.hpp"

#include <atomic>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/scheduler/scheduler.hpp"
#include "scheduler/cpu_work_stealing_scheduler.hpp"

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
                                         Executable executable)
    : metadata_(lease.descriptor(), trace_node_id, is_initial_ready),
      identity_(identity),
      lease_(std::move(lease)),
      executable_(std::move(executable)) {
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
 * @brief Forwards service-owned CPU worker observations to one active Graph.
 *
 * The CPU scheduler borrows this proxy for its complete configured lifetime.
 * One atomic delegate is installed only while `execute_cpu_run()` holds the
 * single-Run gate and is cleared after every in-flight callback settles.
 *
 * @throws Nothing through scheduler worker callbacks.
 * @note Binding is caller-thread serialized. Atomic publication transfers no
 * ownership and never exposes Graph, cache, or scheduler controls.
 */
class ExecutionService::HostContextProxy final : public SchedulerHostContext {
 public:
  /**
   * @brief Binds one active Graph observation target.
   * @param host Borrowed host context valid through the settled Run.
   * @return Nothing.
   * @throws std::logic_error if another target remains bound.
   * @note The service Run gate serializes bind and clear operations.
   */
  void bind(SchedulerHostContext& host) {
    SchedulerHostContext* expected = nullptr;
    if (!delegate_.compare_exchange_strong(expected, &host,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
      throw std::logic_error("ExecutionService host context is already bound.");
    }
  }

  /**
   * @brief Clears the active Graph observation target.
   * @return Nothing.
   * @throws Nothing.
   * @note The settled CPU wait guarantees no worker can enter a later proxy
   * call for the cleared Run.
   */
  void clear() noexcept { delegate_.store(nullptr, std::memory_order_release); }

  /** @copydoc SchedulerHostContext::is_device_available */
  bool is_device_available(Device device) const noexcept override {
    SchedulerHostContext* const delegate =
        delegate_.load(std::memory_order_acquire);
    return delegate != nullptr && delegate->is_device_available(device);
  }

  /** @copydoc SchedulerHostContext::set_task_context */
  void set_task_context(int worker_id, std::uint64_t epoch) noexcept override {
    SchedulerHostContext* const delegate =
        delegate_.load(std::memory_order_acquire);
    if (delegate != nullptr) {
      delegate->set_task_context(worker_id, epoch);
    }
  }

  /** @copydoc SchedulerHostContext::clear_task_context */
  void clear_task_context() noexcept override {
    SchedulerHostContext* const delegate =
        delegate_.load(std::memory_order_acquire);
    if (delegate != nullptr) {
      delegate->clear_task_context();
    }
  }

  /** @copydoc SchedulerHostContext::log_event */
  void log_event(SchedulerTraceAction action, int node_id, int worker_id,
                 std::uint64_t epoch) noexcept override {
    SchedulerHostContext* const delegate =
        delegate_.load(std::memory_order_acquire);
    if (delegate != nullptr) {
      delegate->log_event(action, node_id, worker_id, epoch);
    }
  }

 private:
  /** @brief Borrowed active Graph observation target, or null between Runs. */
  std::atomic<SchedulerHostContext*> delegate_{nullptr};
};

namespace {

/**
 * @brief Clears one service host binding and active Run on every exit.
 *
 * @throws Nothing.
 * @note The scope is created only after both fields have been published and is
 * destroyed after batch submission either fails transactionally or settles.
 */
class ActiveRunScope final {
 public:
  /**
   * @brief Captures cleanup callbacks for one published active Run.
   * @param clear Function that clears service active state.
   * @throws std::bad_alloc if function ownership cannot allocate.
   */
  explicit ActiveRunScope(std::function<void()> clear)
      : clear_(std::move(clear)) {}

  /** @brief Runs non-throwing active-state cleanup. @throws Nothing. */
  ~ActiveRunScope() noexcept {
    try {
      clear_();
    } catch (...) {
      std::terminate();
    }
  }

  /** @brief Prevents duplicate cleanup ownership. */
  ActiveRunScope(const ActiveRunScope& other) = delete;

  /** @brief Prevents replacing cleanup ownership. */
  ActiveRunScope& operator=(const ActiveRunScope& other) = delete;

 private:
  /** @brief Owned cleanup callback invoked exactly once. */
  std::function<void()> clear_;
};

}  // namespace

/** @copydoc ExecutionService::ExecutionService */
ExecutionService::ExecutionService()
    : host_context_(std::make_unique<HostContextProxy>()) {}

/** @copydoc ExecutionService::~ExecutionService */
ExecutionService::~ExecutionService() noexcept {
  if (!cpu_scheduler_) {
    return;
  }
  try {
    if (cpu_scheduler_->is_running()) {
      cpu_scheduler_->shutdown();
    }
  } catch (...) {
  }
  try {
    cpu_scheduler_->detach();
  } catch (...) {
  }
}

/** @copydoc ExecutionService::configure_cpu_scheduler */
void ExecutionService::configure_cpu_scheduler(unsigned int worker_count) {
  if (cpu_scheduler_ && configured_workers_ == worker_count &&
      cpu_scheduler_->is_running()) {
    return;
  }

  if (cpu_scheduler_) {
    if (cpu_scheduler_->is_running()) {
      cpu_scheduler_->shutdown();
    }
    cpu_scheduler_->detach();
    cpu_scheduler_.reset();
    configured_workers_ = 0U;
  }

  auto candidate = std::make_unique<CpuWorkStealingScheduler>(worker_count);
  try {
    candidate->attach(*host_context_);
    candidate->start();
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    try {
      if (candidate->is_running()) {
        candidate->shutdown();
      }
    } catch (...) {
    }
    try {
      candidate->detach();
    } catch (...) {
    }
    std::rethrow_exception(failure);
  }
  cpu_scheduler_ = std::move(candidate);
  configured_workers_ = worker_count;
}

/** @copydoc ExecutionService::active_scheduler */
CpuWorkStealingScheduler& ExecutionService::active_scheduler() {
  if (!cpu_scheduler_ || !active_run_id_.has_value()) {
    throw std::logic_error(
        "ExecutionService runtime operation requires an active Run.");
  }
  return *cpu_scheduler_;
}

/** @copydoc ExecutionService::make_cpu_task */
SchedulerTaskRuntime::Task ExecutionService::make_cpu_task(
    ReadyTaskSubmission submission) {
  auto owned = std::make_shared<ReadyTaskSubmission>(std::move(submission));
  return [this, owned = std::move(owned)]() mutable {
    if (owned->metadata().is_initial_ready()) {
      try {
        log_event(SchedulerTraceAction::AssignInitial,
                  owned->metadata().trace_node_id());
      } catch (...) {
      }
    }
    owned->execute(*this);
  };
}

/** @copydoc ExecutionService::execute_cpu_run */
void ExecutionService::execute_cpu_run(
    SchedulerHostContext& host, unsigned int worker_count,
    std::vector<ReadyTaskSubmission> initial_submissions,
    int total_task_count) {
  if (worker_count == 0U || worker_count > kSchedulerWorkerRequestMax) {
    throw std::invalid_argument(
        "ExecutionService CPU worker count must be in [1,8].");
  }
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

  std::vector<Task> initial_tasks;
  initial_tasks.reserve(initial_submissions.size());
  for (ReadyTaskSubmission& submission : initial_submissions) {
    initial_tasks.push_back(make_cpu_task(std::move(submission)));
  }

  std::lock_guard<std::mutex> lock(run_mutex_);
  configure_cpu_scheduler(worker_count);
  ActiveRunScope active_scope([this]() {
    active_run_id_.reset();
    host_context_->clear();
  });
  host_context_->bind(host);
  active_run_id_ = run_id;

  cpu_scheduler_->submit_initial_tasks(std::move(initial_tasks),
                                       total_task_count);
  wait_for_completion();
}

/** @copydoc ExecutionService::submit_ready_submission */
void ExecutionService::submit_ready_submission(ReadyTaskSubmission submission) {
  if (!active_run_id_.has_value() ||
      submission.metadata().run_id() != *active_run_id_) {
    throw std::invalid_argument(
        "ReadyTaskSubmission does not belong to the active Run.");
  }
  active_scheduler().submit_ready_task_any_thread(
      make_cpu_task(std::move(submission)));
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
  active_scheduler().wait_for_completion();
}

/** @copydoc ExecutionService::set_exception */
void ExecutionService::set_exception(std::exception_ptr error) {
  active_scheduler().set_exception(std::move(error));
}

/** @copydoc ExecutionService::inc_tasks_to_complete */
void ExecutionService::inc_tasks_to_complete(int delta) {
  active_scheduler().inc_tasks_to_complete(delta);
}

/** @copydoc ExecutionService::dec_tasks_to_complete */
void ExecutionService::dec_tasks_to_complete() {
  active_scheduler().dec_tasks_to_complete();
}

/** @copydoc ExecutionService::log_event */
void ExecutionService::log_event(SchedulerTraceAction action, int node_id) {
  active_scheduler().log_event(action, node_id);
}

}  // namespace ps::compute
