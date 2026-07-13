#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/scheduler/scheduler.hpp"

namespace ps::scheduler_example {

/**
 * @brief Execution policy selected by a repository example plugin.
 *
 * @throws Nothing.
 * @note GPU policy advertises Metal only when the borrowed host context reports
 *       it; task callbacks remain responsible for device-specific operation
 *       execution.
 */
enum class ExamplePolicy : std::uint32_t {
  /** @brief Execute callbacks synchronously on the caller thread. */
  Serial = 0U,
  /** @brief Execute callbacks on a CPU worker pool. */
  CpuWorkers = 1U,
  /** @brief Use a worker pool and advertise an available Metal route. */
  Heterogeneous = 2U,
};

/**
 * @brief SDK-only scheduler used by repository example DSOs.
 *
 * The implementation includes only the installed scheduler SDK and standard
 * library headers. Serial policy runs inline; worker policies own a priority
 * queue, worker threads, completion accounting, and first-exception transport.
 * Task handles remain dispatcher-owned until `wait_for_completion` returns.
 *
 * @throws std::bad_alloc from owned names, queues, workers, callbacks, or
 *         returned diagnostic/device values.
 * @throws std::system_error from valid standard synchronization or thread
 *         operations.
 * @note Lifecycle calls are externally serialized. Host-context calls are
 *       balanced around every entered callback and the borrowed pointer clears
 *       during detach.
 */
class SdkExampleScheduler final : public IScheduler {
 public:
#if defined(PHOTOSPIDER_SCHEDULER_EXAMPLE_TESTING)
  /**
   * @brief Test-only callback invoked before each initial staged queue entry.
   * @param attempt One-based staging attempt within the submitted batch.
   * @return Nothing.
   * @throws Any deterministic test exception selected by the callback.
   * @note Production plugin targets do not define the testing macro and cannot
   *       observe this source-level injection seam.
   */
  using InitialStagingFailureHook = void (*)(std::size_t attempt);

  /**
   * @brief Installs the process-local initial-staging failure test hook.
   * @param hook Callback to invoke, or null to disable injection.
   * @return Nothing.
   * @throws Nothing.
   * @note Tests must restore the previous null state before scheduler teardown.
   */
  static void set_initial_staging_failure_hook_for_testing(
      InitialStagingFailureHook hook) noexcept {
    initial_staging_failure_hook_.store(hook, std::memory_order_release);
  }

  /**
   * @brief Reads completion accounting for deterministic boundary tests.
   * @return Current logical tasks-to-complete count.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note The snapshot does not expose or mutate production scheduler state
   *       when the testing macro is absent.
   */
  int tasks_to_complete_for_testing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_to_complete_;
  }

  /**
   * @brief Reads the cross-epoch borrowed-callback settlement fence.
   * @return Number of entered nonzero-epoch callbacks not yet returned.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note A newer batch cannot reset this count while an older handle still
   *       borrows its dispatcher executor.
   */
  std::size_t borrowed_in_flight_for_testing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return borrowed_in_flight_tasks_;
  }

  /**
   * @brief Evaluates the completion predicate used by the public wait method.
   * @return True only when wait may settle or publish the active exception.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note This deterministic seam proves blocking behavior without timing-only
   *       assertions and is absent from production plugin targets.
   */
  bool completion_ready_for_testing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_requested_ ||
           ((tasks_to_complete_ <= 0 || first_exception_ != nullptr) &&
            borrowed_in_flight_tasks_ == 0U &&
            uncancellable_in_flight_tasks_ == 0U);
  }
#endif

  /**
   * @brief Configures one detached, stopped example scheduler.
   * @param type_name Stable scheduler type returned by `name()`.
   * @param policy Serial, CPU-worker, or heterogeneous policy.
   * @param requested_workers Worker count; zero uses hardware concurrency.
   * @throws std::bad_alloc if the type-name copy fails.
   * @note Construction does not attach or start the scheduler. Worker resources
   *       are created only by `start()`.
   */
  SdkExampleScheduler(std::string type_name, ExamplePolicy policy,
                      std::uint32_t requested_workers)
      : type_name_(std::move(type_name)), policy_(policy) {
    const unsigned int hardware = std::thread::hardware_concurrency();
    worker_count_ =
        requested_workers == 0U ? std::max(1U, hardware) : requested_workers;
  }

  /**
   * @brief Stops all owned workers while suppressing destructor exceptions.
   * @throws Nothing.
   * @note The host normally performs explicit shutdown and detach first; this
   *       fallback only prevents worker lifetime from escaping direct teardown.
   */
  ~SdkExampleScheduler() noexcept override {
    if (is_running()) {
      try {
        shutdown();
      } catch (...) {
      }
    }
  }

  /**
   * @brief Prevents copying worker, queue, and borrowed-host ownership.
   * @param other Scheduler whose ownership cannot be duplicated.
   * @throws Nothing because the operation is deleted.
   */
  SdkExampleScheduler(const SdkExampleScheduler& other) = delete;

  /**
   * @brief Prevents copy assignment of worker and batch state.
   * @param other Scheduler whose ownership cannot replace this instance.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  SdkExampleScheduler& operator=(const SdkExampleScheduler& other) = delete;

  /**
   * @brief Borrows the host capability and attribution context.
   * @param host Host-owned context that outlives shutdown and detach.
   * @return Nothing.
   * @throws Nothing.
   * @note Lifecycle calls are externally serialized, so publication needs no
   *       internal lock and detach clears the exact borrowed pointer.
   */
  void attach(SchedulerHostContext& host) noexcept override { host_ = &host; }

  /**
   * @brief Clears the borrowed host context after workers have stopped.
   * @return Nothing.
   * @throws Nothing.
   * @note The host calls detach after shutdown; no queued or running callback
   *       may retain the pointer after this method returns.
   */
  void detach() noexcept override { host_ = nullptr; }

  /**
   * @brief Starts the selected inline or worker-pool execution policy.
   * @return Nothing.
   * @throws std::bad_alloc if worker-vector staging cannot allocate.
   * @throws std::system_error if thread creation, locking, or joining fails.
   * @note Serial start only publishes lifecycle state. Worker start stages all
   *       threads and joins any partial set before propagating a construction
   *       failure; a repeated call while running is a no-op.
   */
  void start() override {
    if (running_.load(std::memory_order_acquire)) {
      return;
    }
    if (policy_ == ExamplePolicy::Serial) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = false;
      }
      running_.store(true, std::memory_order_release);
      return;
    }

    std::vector<std::thread> staged;
    staged.reserve(worker_count_);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_requested_ = false;
    }
    try {
      for (std::uint32_t index = 0; index < worker_count_; ++index) {
        staged.emplace_back(
            [this, index]() { worker_loop(static_cast<int>(index)); });
      }
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
      }
      work_cv_.notify_all();
      for (std::thread& worker : staged) {
        if (worker.joinable()) {
          worker.join();
        }
      }
      throw;
    }
    workers_.swap(staged);
    running_.store(true, std::memory_order_release);
  }

  /**
   * @brief Stops admission, discards queued work, and joins every worker.
   * @return Nothing.
   * @throws std::system_error if valid synchronization or thread joining fails.
   * @note Stop state and queue clearing publish under `mutex_` before both wait
   *       domains are notified. The object remains attached and may restart.
   */
  void shutdown() override {
    running_.store(false, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_requested_ = true;
      high_queue_.clear();
      normal_queue_.clear();
    }
    work_cv_.notify_all();
    completion_cv_.notify_all();
    for (std::thread& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
  }

  /**
   * @brief Copies the stable scheduler type name supplied at construction.
   * @return Owned type name independent of plugin string storage.
   * @throws std::bad_alloc if the returned string copy cannot allocate.
   * @note The name is immutable for the complete scheduler lifetime.
   */
  std::string name() const override { return type_name_; }

  /**
   * @brief Builds a point-in-time lifecycle, queue, and completion diagnostic.
   * @return Owned human-readable statistics string.
   * @throws std::bad_alloc if stream or returned string storage cannot
   * allocate.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note The text is observational and must not be parsed for control flow.
   */
  std::string get_stats() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream output;
    output << "SdkExampleScheduler{type=" << type_name_ << ", running="
           << (running_.load(std::memory_order_acquire) ? "true" : "false")
           << ", queued=" << high_queue_.size() + normal_queue_.size()
           << ", completed=" << completed_tasks_.load(std::memory_order_relaxed)
           << "}";
    return output.str();
  }

  /**
   * @brief Reads the externally visible lifecycle publication.
   * @return True only after start publishes success and before shutdown begins.
   * @throws Nothing.
   * @note Acquire ordering observes the worker-set publication performed by
   *       successful start.
   */
  bool is_running() const noexcept override {
    return running_.load(std::memory_order_acquire);
  }

  /**
   * @brief Reports devices routable by this example policy and host.
   * @return Metal then CPU for an available heterogeneous policy, otherwise
   *         CPU only.
   * @throws std::bad_alloc if result-vector allocation fails.
   * @note The scheduler asks only the public host capability; it receives no
   *       native Metal handle or runtime owner.
   */
  std::vector<Device> available_devices() const override {
    if (policy_ == ExamplePolicy::Heterogeneous && host_ != nullptr &&
        host_->is_device_available(Device::GPU_METAL)) {
      return {Device::GPU_METAL, Device::CPU};
    }
    return {Device::CPU};
  }

  /**
   * @brief Atomically replaces the active batch with valid borrowed handles.
   * @param handles Dispatcher-owned handles borrowed through settlement.
   * @param total_task_count Nonnegative logical completion count; when at least
   *        one handle is valid, this must cover every valid submitted handle.
   * @param priority Queue priority applied to every valid handle.
   * @return Nothing.
   * @throws std::logic_error if the scheduler is not running.
   * @throws std::invalid_argument if the completion count is negative or
   *         smaller than the number of valid handles.
   * @throws std::bad_alloc if queue staging cannot allocate.
   * @note Validation and staging finish before the no-throw locked publication,
   *       so every failure preserves the prior epoch, queues, exception, and
   *       completion state. Valid handles remain borrowed until matching
   *       completion or exception settlement permits `wait_for_completion()`
   *       to return.
   */
  void submit_initial_task_handles(std::vector<TaskHandle>&& handles,
                                   int total_task_count,
                                   SchedulerTaskPriority priority) override {
    if (!is_running()) {
      throw std::logic_error("scheduler is not running");
    }
    if (total_task_count < 0) {
      throw std::invalid_argument(
          "initial task completion count must be nonnegative");
    }

    const std::size_t valid_handle_count =
        static_cast<std::size_t>(std::count_if(
            handles.begin(), handles.end(), [](const TaskHandle& handle) {
              return static_cast<bool>(handle);
            }));
    if (valid_handle_count > static_cast<std::size_t>(total_task_count)) {
      throw std::invalid_argument(
          "initial task completion count is smaller than valid handle count");
    }

    std::lock_guard<std::mutex> submission_lock(initial_submission_mutex_);
    const std::uint64_t staged_epoch =
        next_epoch(active_epoch_.load(std::memory_order_acquire));
    std::deque<WorkItem> staged_high;
    std::deque<WorkItem> staged_normal;
    std::deque<WorkItem>& staged_queue =
        priority == SchedulerTaskPriority::High ? staged_high : staged_normal;
    std::size_t staged_count = 0U;
    for (const TaskHandle& handle : handles) {
      if (!handle) {
        continue;
      }
#if defined(PHOTOSPIDER_SCHEDULER_EXAMPLE_TESTING)
      invoke_initial_staging_failure_hook(staged_count + 1U);
#endif
      if (policy_ != ExamplePolicy::Serial) {
        staged_queue.emplace_back(handle, staged_epoch);
      }
      ++staged_count;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      high_queue_.swap(staged_high);
      normal_queue_.swap(staged_normal);
      tasks_to_complete_ = staged_count == 0U ? 0 : total_task_count;
      first_exception_ = nullptr;
      active_epoch_.store(staged_epoch, std::memory_order_release);
    }

    if (policy_ == ExamplePolicy::Serial) {
      run_handles_inline(handles, staged_epoch);
    } else {
      work_cv_.notify_all();
      completion_cv_.notify_all();
    }
  }

  /**
   * @brief Publishes one dependency-released borrowed-handle batch.
   * @param handles Dispatcher-owned handles released by the current worker.
   * @param priority Queue priority applied to every valid handle.
   * @return Nothing.
   * @throws std::bad_alloc if worker-policy queue growth cannot allocate.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note Serial policy executes the batch inline under the current callback
   *       epoch. Worker policy publishes all valid handles atomically and
   *       ignores calls outside this scheduler or from a stale/failed epoch.
   */
  void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      SchedulerTaskPriority priority) override {
    if (policy_ == ExamplePolicy::Serial) {
      if (tls_owner_ != this || tls_epoch_ == 0U) {
        return;
      }
      run_handles_inline(handles, tls_epoch_);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const std::uint64_t active_epoch =
          active_epoch_.load(std::memory_order_acquire);
      if (first_exception_ != nullptr || tls_owner_ != this ||
          tls_epoch_ == 0U || tls_epoch_ != active_epoch) {
        return;
      }
      std::deque<WorkItem>& queue = queue_for(priority);
      const std::size_t original_size = queue.size();
      try {
        for (const TaskHandle& handle : handles) {
          if (handle) {
            queue.emplace_back(handle, active_epoch);
          }
        }
      } catch (...) {
        while (queue.size() > original_size) {
          queue.pop_back();
        }
        throw;
      }
    }
    work_cv_.notify_all();
  }
  /**
   * @brief Submits one owned callback from an arbitrary caller thread.
   * @param task Callback state transferred to the scheduler; empty work is
   *        ignored.
   * @param priority Requested queue priority.
   * @param epoch Optional batch epoch; absence selects the active epoch and
   *        zero denotes uncancellable compatibility work.
   * @return Nothing.
   * @throws std::bad_alloc if callback queue publication cannot allocate.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note Serial policy executes inline. Worker policy rejects stale or failed
   *       epochs before moving the callback into its selected queue.
   */
  void submit_ready_task_any_thread(
      Task&& task, SchedulerTaskPriority priority,
      std::optional<std::uint64_t> epoch) override {
    if (!task) {
      return;
    }
    const std::uint64_t selected_epoch =
        epoch.value_or(active_epoch_.load(std::memory_order_acquire));
    if (policy_ == ExamplePolicy::Serial) {
      run_callback_inline(std::move(task), selected_epoch);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const std::uint64_t active_epoch =
          active_epoch_.load(std::memory_order_acquire);
      if (first_exception_ != nullptr ||
          !is_epoch_current(selected_epoch, active_epoch)) {
        return;
      }
      queue_for(priority).emplace_back(std::move(task), selected_epoch);
    }
    work_cv_.notify_one();
  }

  /**
   * @brief Waits until the active count and all entered callbacks settle.
   * @return Nothing.
   * @throws The exact first callback exception after every borrowed and
   *         uncancellable callback has returned.
   * @throws std::system_error if condition-variable waiting or locking fails.
   * @note Shutdown also releases the wait. Exception state remains readable so
   *       repeated waits keep reporting the active batch failure until a new
   *       initial batch replaces it.
   */
  void wait_for_completion() override {
    std::unique_lock<std::mutex> lock(mutex_);
    completion_cv_.wait(lock, [this]() {
      return stop_requested_ ||
             (tasks_to_complete_ <= 0 && borrowed_in_flight_tasks_ == 0U &&
              uncancellable_in_flight_tasks_ == 0U) ||
             (first_exception_ != nullptr && borrowed_in_flight_tasks_ == 0U &&
              uncancellable_in_flight_tasks_ == 0U);
    });
    if (first_exception_ != nullptr) {
      const std::exception_ptr error = first_exception_;
      lock.unlock();
      std::rethrow_exception(error);
    }
  }

  /**
   * @brief Publishes the first exact exception for the current call epoch.
   * @param error Exception identity to retain; null is ignored.
   * @return Nothing.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note A stale nonzero epoch cannot clear queues or replace active batch
   *       state. The first accepted publisher discards all queued work.
   */
  void set_exception(std::exception_ptr error) override {
    if (error == nullptr) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const std::uint64_t publisher_epoch = current_call_epoch();
      const std::uint64_t active_epoch =
          active_epoch_.load(std::memory_order_acquire);
      if (is_epoch_current(publisher_epoch, active_epoch) &&
          first_exception_ == nullptr) {
        first_exception_ = std::move(error);
        high_queue_.clear();
        normal_queue_.clear();
      }
    }
    completion_cv_.notify_all();
  }

  /**
   * @brief Adds positive dynamically discovered work to completion accounting.
   * @param delta Positive number of logical tasks; nonpositive values are
   *        ignored.
   * @return Nothing.
   * @throws std::overflow_error if applying a current-epoch delta would exceed
   *         `INT_MAX`.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note Epoch validation and overflow detection occur while holding the state
   *       mutex. Stale nonzero calls and rejected additions leave the counter
   *       unchanged.
   */
  void inc_tasks_to_complete(int delta) override {
    if (delta <= 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_epoch_current(current_call_epoch(),
                         active_epoch_.load(std::memory_order_acquire))) {
      if (tasks_to_complete_ > std::numeric_limits<int>::max() - delta) {
        throw std::overflow_error("scheduler task completion counter overflow");
      }
      tasks_to_complete_ += delta;
    }
  }

  /**
   * @brief Retires one logical task for the current call epoch.
   * @return Nothing.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note Zero is a hard floor. Stale nonzero calls and decrements at zero are
   *       no-ops; waiters are notified after every attempt.
   */
  void dec_tasks_to_complete() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (is_epoch_current(current_call_epoch(),
                           active_epoch_.load(std::memory_order_acquire)) &&
          tasks_to_complete_ > 0) {
        --tasks_to_complete_;
      }
    }
    completion_cv_.notify_all();
  }

  /**
   * @brief Publishes one trace action through the borrowed host context.
   * @param action Stable scheduler trace action.
   * @param node_id Associated graph node id, or -1 when unavailable.
   * @return Nothing.
   * @throws Nothing under the host-context contract.
   * @note Worker and epoch identity come from this scheduler's TLS context;
   *       calls while detached are ignored.
   */
  void log_event(SchedulerTraceAction action, int node_id) noexcept override {
    if (host_ != nullptr) {
      host_->log_event(action, node_id,
                       tls_owner_ == this ? tls_worker_id_ : -1,
                       current_call_epoch());
    }
  }

 private:
  /**
   * @brief Move-only queue entry containing a task handle or callback.
   *
   * @throws Nothing while moving entries; callback construction follows the
   *         `SchedulerTaskRuntime::Task` move contract.
   * @note Exactly one of `handle` and `callback` is active. The scheduler owns
   *       callback state but only borrows a handle executor.
   */
  struct WorkItem {
    /**
     * @brief Creates one borrowed-handle queue entry.
     * @param value Task handle to copy.
     * @param value_epoch Batch epoch.
     * @throws Nothing.
     * @note The handle continues to borrow its dispatcher-owned executor.
     */
    WorkItem(TaskHandle value, std::uint64_t value_epoch)
        : handle(value), epoch(value_epoch) {}

    /**
     * @brief Creates one owned-callback queue entry.
     * @param value Callback state to move.
     * @param value_epoch Batch epoch.
     * @throws Nothing when the callback move is non-throwing.
     * @note Callback ownership transfers completely into this queue entry.
     */
    WorkItem(Task&& value, std::uint64_t value_epoch)
        : callback(std::move(value)), epoch(value_epoch) {}

    /**
     * @brief Moves one queue entry without changing borrowed ownership.
     * @param other Queue entry whose active work state transfers here.
     * @throws Nothing.
     * @note A moved borrowed handle still refers to the same host executor.
     */
    WorkItem(WorkItem&& other) noexcept = default;

    /**
     * @brief Move-assigns one queue entry without copying callback state.
     * @param other Queue entry replacing the current work state.
     * @return This queue entry after ownership transfer.
     * @throws Nothing.
     * @note Any previously owned callback follows its normal move-assignment
     *       release semantics.
     */
    WorkItem& operator=(WorkItem&& other) noexcept = default;

    /**
     * @brief Prevents copying callback ownership between queue entries.
     * @param other Queue entry whose ownership cannot be duplicated.
     * @throws Nothing because the operation is deleted.
     */
    WorkItem(const WorkItem& other) = delete;

    /**
     * @brief Prevents copy assignment of callback ownership.
     * @param other Queue entry whose ownership cannot replace this entry.
     * @return No value because the operation is deleted.
     * @throws Nothing because the operation is deleted.
     */
    WorkItem& operator=(const WorkItem& other) = delete;

    /** @brief Borrowed handle, empty for callback work. */
    TaskHandle handle;
    /** @brief Owned callback, empty for handle work. */
    Task callback;
    /** @brief Scheduler batch epoch associated with this entry. */
    std::uint64_t epoch = 0U;
  };

  /**
   * @brief Publishes and restores one callback's scheduler/host task context.
   *
   * @throws Nothing.
   * @note The borrowed host remains alive until shutdown and detach. Nested
   *       scheduler entry restores the prior scheduler context after clearing
   *       the current host context.
   */
  class TaskContextScope final {
   public:
    /**
     * @brief Publishes scheduler, worker, and epoch identity.
     * @param owner Scheduler entering the callback.
     * @param host Borrowed host, or null while detached.
     * @param worker_id Scheduler worker id.
     * @param epoch Task epoch.
     * @throws Nothing.
     * @note Any previously active nested context is cleared before the new
     *       identity is published.
     */
    TaskContextScope(SdkExampleScheduler* owner, SchedulerHostContext* host,
                     int worker_id, std::uint64_t epoch) noexcept
        : owner_(owner),
          host_(host),
          previous_owner_(tls_owner_),
          previous_host_(tls_owner_ == nullptr ? nullptr : tls_owner_->host_),
          previous_worker_id_(tls_worker_id_),
          previous_epoch_(tls_epoch_) {
      if (previous_host_ != nullptr) {
        previous_host_->clear_task_context();
      }
      tls_owner_ = owner_;
      tls_worker_id_ = worker_id;
      tls_epoch_ = epoch;
      if (host_ != nullptr) {
        host_->set_task_context(worker_id, epoch);
      }
    }

    /**
     * @brief Clears current host context and restores nested caller identity.
     * @throws Nothing.
     * @note Restoration is balanced even when callback execution captured an
     *       exception.
     */
    ~TaskContextScope() noexcept {
      if (host_ != nullptr) {
        host_->clear_task_context();
      }
      tls_owner_ = previous_owner_;
      tls_worker_id_ = previous_worker_id_;
      tls_epoch_ = previous_epoch_;
      if (previous_host_ != nullptr) {
        previous_host_->set_task_context(previous_worker_id_, previous_epoch_);
      }
    }

    /**
     * @brief Prevents duplicating one balanced context scope.
     * @param other Active scope whose restoration duty cannot be copied.
     * @throws Nothing because the operation is deleted.
     */
    TaskContextScope(const TaskContextScope& other) = delete;

    /**
     * @brief Prevents assignment of one balanced context scope.
     * @param other Active scope whose restoration duty cannot replace this one.
     * @return No value because the operation is deleted.
     * @throws Nothing because the operation is deleted.
     */
    TaskContextScope& operator=(const TaskContextScope& other) = delete;

   private:
    /** @brief Scheduler whose callback context is active. */
    SdkExampleScheduler* owner_;
    /** @brief Borrowed host context, or null. */
    SchedulerHostContext* host_;
    /** @brief Scheduler context active before this callback. */
    SdkExampleScheduler* previous_owner_;
    /** @brief Host context active before this callback, or null. */
    SchedulerHostContext* previous_host_;
    /** @brief Worker id active before this callback. */
    int previous_worker_id_;
    /** @brief Epoch active before this callback. */
    std::uint64_t previous_epoch_;
  };

  /**
   * @brief Tests whether work survives epoch cancellation.
   * @param candidate Work epoch; zero denotes uncancellable work.
   * @param active Currently published batch epoch.
   * @return True for epoch zero or an exact active-epoch match.
   * @throws Nothing.
   */
  static bool is_epoch_current(std::uint64_t candidate,
                               std::uint64_t active) noexcept {
    return candidate == 0U || candidate == active;
  }

  /**
   * @brief Advances a nonzero batch epoch with deterministic wrap handling.
   * @param current Currently published epoch.
   * @return Next nonzero epoch, wrapping the maximum value to one.
   * @throws Nothing.
   */
  static std::uint64_t next_epoch(std::uint64_t current) noexcept {
    return current == std::numeric_limits<std::uint64_t>::max() ? 1U
                                                                : current + 1U;
  }

  /**
   * @brief Resolves this scheduler's epoch on the calling thread.
   * @return Current callback epoch, or zero outside this scheduler.
   * @throws Nothing.
   */
  std::uint64_t current_call_epoch() const noexcept {
    return tls_owner_ == this ? tls_epoch_ : 0U;
  }

#if defined(PHOTOSPIDER_SCHEDULER_EXAMPLE_TESTING)
  /**
   * @brief Invokes the optional deterministic staging-failure hook.
   * @param attempt One-based initial staging attempt.
   * @return Nothing.
   * @throws The installed test exception unchanged.
   */
  static void invoke_initial_staging_failure_hook(std::size_t attempt) {
    const InitialStagingFailureHook hook =
        initial_staging_failure_hook_.load(std::memory_order_acquire);
    if (hook != nullptr) {
      hook(attempt);
    }
  }
#endif

  /**
   * @brief Selects the queue for one priority while `mutex_` is held.
   * @param priority Requested scheduler priority.
   * @return Mutable selected queue.
   * @throws Nothing.
   * @note The caller must hold `mutex_` for the complete reference use.
   */
  std::deque<WorkItem>& queue_for(SchedulerTaskPriority priority) noexcept {
    return priority == SchedulerTaskPriority::High ? high_queue_
                                                   : normal_queue_;
  }

  /**
   * @brief Claims one callback for execution when its epoch remains current.
   * @param epoch Queue-entry epoch.
   * @return True when execution may enter; false when stale or failed.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note Epoch-zero work increments a separate uncancellable in-flight count.
   */
  bool try_begin_work(std::uint64_t epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t active_epoch =
        active_epoch_.load(std::memory_order_acquire);
    if (first_exception_ != nullptr || !is_epoch_current(epoch, active_epoch)) {
      return false;
    }
    if (epoch == 0U) {
      ++uncancellable_in_flight_tasks_;
    } else {
      ++borrowed_in_flight_tasks_;
    }
    return true;
  }

  /**
   * @brief Retires one entered callback and conditionally publishes its error.
   * @param epoch Epoch captured when execution entered.
   * @param error Callback exception, or null after success.
   * @return Nothing.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note Every entered nonzero callback retires the cross-epoch borrowed
   *       executor fence exactly once. A stale callback cannot publish its
   *       exception or completion count into the newer active batch.
   */
  void finish_work(std::uint64_t epoch, std::exception_ptr error) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const std::uint64_t active_epoch =
          active_epoch_.load(std::memory_order_acquire);
      if (epoch == 0U) {
        if (uncancellable_in_flight_tasks_ > 0U) {
          --uncancellable_in_flight_tasks_;
        }
      } else if (borrowed_in_flight_tasks_ > 0U) {
        --borrowed_in_flight_tasks_;
      }
      if (error != nullptr && is_epoch_current(epoch, active_epoch) &&
          first_exception_ == nullptr) {
        first_exception_ = std::move(error);
        high_queue_.clear();
        normal_queue_.clear();
      }
    }
    completion_cv_.notify_all();
  }

  /**
   * @brief Executes a serial handle batch in input order.
   * @param handles Borrowed task handles.
   * @param epoch Active batch epoch.
   * @return Nothing.
   * @throws Nothing directly; task exceptions enter first-exception state.
   */
  void run_handles_inline(const std::vector<TaskHandle>& handles,
                          std::uint64_t epoch) {
    for (const TaskHandle& handle : handles) {
      if (!handle || !try_begin_work(epoch)) {
        continue;
      }
      std::exception_ptr error;
      try {
        TaskContextScope context(this, host_, -1, epoch);
        try {
          handle.run();
        } catch (...) {
          error = std::current_exception();
        }
      } catch (...) {
        error = std::current_exception();
      }
      if (error == nullptr) {
        completed_tasks_.fetch_add(1, std::memory_order_relaxed);
      }
      finish_work(epoch, std::move(error));
    }
    completion_cv_.notify_all();
  }

  /**
   * @brief Executes one serial callback with balanced host task context.
   * @param task Callback state owned by this call.
   * @param epoch Task epoch.
   * @return Nothing.
   * @throws Nothing directly; callback exceptions enter first-exception state.
   */
  void run_callback_inline(Task&& task, std::uint64_t epoch) {
    if (!try_begin_work(epoch)) {
      return;
    }
    std::exception_ptr error;
    try {
      TaskContextScope context(this, host_, -1, epoch);
      try {
        task();
      } catch (...) {
        error = std::current_exception();
      }
    } catch (...) {
      error = std::current_exception();
    }
    if (error == nullptr) {
      completed_tasks_.fetch_add(1, std::memory_order_relaxed);
    }
    finish_work(epoch, std::move(error));
  }

  /**
   * @brief Runs one worker until shutdown drains or discards queued work.
   * @param worker_id Stable worker id.
   * @return Nothing.
   * @throws Nothing; task exceptions enter scheduler exception transport.
   */
  void worker_loop(int worker_id) noexcept {
    for (;;) {
      std::optional<WorkItem> item;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        work_cv_.wait(lock, [this]() {
          return stop_requested_ || !high_queue_.empty() ||
                 !normal_queue_.empty();
        });
        if (stop_requested_ && high_queue_.empty() && normal_queue_.empty()) {
          break;
        }
        std::deque<WorkItem>& queue =
            high_queue_.empty() ? normal_queue_ : high_queue_;
        item.emplace(std::move(queue.front()));
        queue.pop_front();
      }

      if (!try_begin_work(item->epoch)) {
        completion_cv_.notify_all();
        continue;
      }
      std::exception_ptr error;
      try {
        TaskContextScope context(this, host_, worker_id, item->epoch);
        try {
          if (item->handle) {
            item->handle.run();
          } else if (item->callback) {
            item->callback();
          }
        } catch (...) {
          error = std::current_exception();
        }
      } catch (...) {
        error = std::current_exception();
      }
      if (error == nullptr) {
        completed_tasks_.fetch_add(1, std::memory_order_relaxed);
      }
      finish_work(item->epoch, std::move(error));
    }
  }

  /** @brief Stable scheduler type name. */
  std::string type_name_;
  /** @brief Serial, CPU-worker, or heterogeneous policy. */
  ExamplePolicy policy_;
  /** @brief Configured worker count for non-serial policies. */
  std::uint32_t worker_count_ = 1U;
  /** @brief Borrowed host context from attach until detach. */
  SchedulerHostContext* host_ = nullptr;
  /** @brief Public lifecycle state. */
  std::atomic<bool> running_{false};
  /** @brief Queue, exception, epoch, and completion-state mutex. */
  mutable std::mutex mutex_;
  /** @brief Serializes allocation-before-publication initial transactions. */
  std::mutex initial_submission_mutex_;
  /** @brief Wakes workers after queue publication or shutdown. */
  std::condition_variable work_cv_;
  /** @brief Wakes the completion waiter. */
  std::condition_variable completion_cv_;
  /** @brief Scheduler-owned worker threads. */
  std::vector<std::thread> workers_;
  /** @brief High-priority ready work. */
  std::deque<WorkItem> high_queue_;
  /** @brief Normal-priority ready work. */
  std::deque<WorkItem> normal_queue_;
  /** @brief Requests worker exit after queues are cleared. */
  bool stop_requested_ = false;
  /** @brief Logical tasks still expected to complete. */
  int tasks_to_complete_ = 0;
  /** @brief Entered nonzero-epoch callbacks borrowing any batch executor. */
  std::size_t borrowed_in_flight_tasks_ = 0U;
  /** @brief Epoch-zero callbacks executing independently of batch changes. */
  std::size_t uncancellable_in_flight_tasks_ = 0U;
  /** @brief First exception captured for the active batch. */
  std::exception_ptr first_exception_;
  /** @brief Monotonically increasing batch epoch. */
  std::atomic<std::uint64_t> active_epoch_{0U};
  /** @brief Completed callback count for diagnostics. */
  std::atomic<std::uint64_t> completed_tasks_{0U};

#if defined(PHOTOSPIDER_SCHEDULER_EXAMPLE_TESTING)
  /** @brief Process-local deterministic initial-staging failure callback. */
  inline static std::atomic<InitialStagingFailureHook>
      initial_staging_failure_hook_{nullptr};
#endif

  /** @brief Scheduler owning the current thread-local task context. */
  inline static thread_local SdkExampleScheduler* tls_owner_ = nullptr;
  /** @brief Worker id for the current scheduler thread. */
  inline static thread_local int tls_worker_id_ = -1;
  /** @brief Task epoch for the current scheduler thread. */
  inline static thread_local std::uint64_t tls_epoch_ = 0U;
};

}  // namespace ps::scheduler_example
