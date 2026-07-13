// Photospider kernel: single-threaded built-in scheduler implementation.

#include "scheduler/serial_debug_scheduler.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ps {
namespace {

/** @brief Serial scheduler owning the current inline callback context. */
thread_local const SerialDebugScheduler* tls_serial_owner = nullptr;
/** @brief Host context currently published by the serial scheduler. */
thread_local SchedulerHostContext* tls_serial_host = nullptr;
/** @brief Worker id currently published by the serial scheduler. */
thread_local int tls_serial_worker_id = -1;
/** @brief Epoch currently published by the serial scheduler. */
thread_local std::uint64_t tls_serial_epoch = 0U;

/**
 * @brief Validates one initial serial batch before state publication.
 * @param valid_item_count Number of nonempty callbacks or handles.
 * @param total_task_count Caller-supplied logical completion count.
 * @return Nothing.
 * @throws std::invalid_argument if the count is negative or cannot cover every
 *         valid initial item.
 * @note The helper observes no scheduler state, so rejection cannot advance an
 *       epoch, clear an exception, or alter completion accounting.
 */
void validate_initial_count(std::size_t valid_item_count,
                            int total_task_count) {
  if (total_task_count < 0) {
    throw std::invalid_argument(
        "serial scheduler initial completion count must be nonnegative");
  }
  if (valid_item_count > static_cast<std::size_t>(total_task_count)) {
    throw std::invalid_argument(
        "serial scheduler initial completion count is smaller than valid "
        "item count");
  }
}

/**
 * @brief Balances and restores one serial host task-context publication.
 *
 * @throws Nothing. Construction and destruction call only noexcept host
 *         context operations.
 * @note The context is borrowed from an attached scheduler. Construction and
 *       destruction are non-throwing because the public host methods are
 *       non-throwing.
 */
class TaskContextScope final {
 public:
  /**
   * @brief Publishes one worker/epoch identity when a context is available.
   * @param owner Serial scheduler entering the callback.
   * @param host Borrowed context, or null while detached.
   * @param worker_id Scheduler worker id.
   * @param epoch Active task epoch.
   * @throws Nothing.
   * @note A nested serial callback clears the prior host publication before
   *       publishing its own identity.
   */
  TaskContextScope(const SerialDebugScheduler* owner,
                   SchedulerHostContext* host, int worker_id,
                   std::uint64_t epoch) noexcept
      : host_(host),
        previous_owner_(tls_serial_owner),
        previous_host_(tls_serial_host),
        previous_worker_id_(tls_serial_worker_id),
        previous_epoch_(tls_serial_epoch) {
    if (previous_host_ != nullptr) {
      previous_host_->clear_task_context();
    }
    tls_serial_owner = owner;
    tls_serial_host = host_;
    tls_serial_worker_id = worker_id;
    tls_serial_epoch = epoch;
    if (host_ != nullptr) {
      host_->set_task_context(worker_id, epoch);
    }
  }

  /**
   * @brief Clears current identity and restores a nested caller's identity.
   * @throws Nothing.
   * @note Host set/clear calls remain balanced on normal and exception exits.
   */
  ~TaskContextScope() noexcept {
    if (host_ != nullptr) {
      host_->clear_task_context();
    }
    tls_serial_owner = previous_owner_;
    tls_serial_host = previous_host_;
    tls_serial_worker_id = previous_worker_id_;
    tls_serial_epoch = previous_epoch_;
    if (previous_host_ != nullptr) {
      previous_host_->set_task_context(previous_worker_id_, previous_epoch_);
    }
  }

  /**
   * @brief Prevents duplicating a balanced task-context scope.
   * @param other Scope whose restoration duty cannot be copied.
   * @throws Nothing because the operation is deleted.
   * @note Each entered callback owns exactly one restoration operation.
   */
  TaskContextScope(const TaskContextScope& other) = delete;

  /**
   * @brief Prevents assigning a balanced task-context scope.
   * @param other Scope whose restoration duty cannot replace this one.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   * @note Assignment could otherwise lose the prior TLS identity.
   */
  TaskContextScope& operator=(const TaskContextScope& other) = delete;

 private:
  /** @brief Borrowed host context, or null. */
  SchedulerHostContext* host_;
  /** @brief Serial scheduler active before this scope. */
  const SerialDebugScheduler* previous_owner_;
  /** @brief Host context active before this scope, or null. */
  SchedulerHostContext* previous_host_;
  /** @brief Worker id active before this scope. */
  int previous_worker_id_;
  /** @brief Epoch active before this scope. */
  std::uint64_t previous_epoch_;
};

}  // namespace

/** @copydoc SerialDebugScheduler::~SerialDebugScheduler */
SerialDebugScheduler::~SerialDebugScheduler() noexcept {
  if (is_running()) {
    shutdown();
  }
}

/** @copydoc SerialDebugScheduler::attach */
void SerialDebugScheduler::attach(SchedulerHostContext& host) noexcept {
  host_context_ = &host;
}

/** @copydoc SerialDebugScheduler::detach */
void SerialDebugScheduler::detach() noexcept {
  host_context_ = nullptr;
}

/** @copydoc SerialDebugScheduler::start */
void SerialDebugScheduler::start() noexcept {
  running_.store(true, std::memory_order_release);
}

/** @copydoc SerialDebugScheduler::shutdown */
void SerialDebugScheduler::shutdown() noexcept {
  running_.store(false, std::memory_order_release);
  completion_cv_.notify_all();
}

/** @copydoc SerialDebugScheduler::name */
std::string SerialDebugScheduler::name() const {
  return "serial_debug";
}

/** @copydoc SerialDebugScheduler::get_stats */
std::string SerialDebugScheduler::get_stats() const {
  std::ostringstream oss;
  oss << "SerialDebugScheduler Stats:\n"
      << "  Running: "
      << (running_.load(std::memory_order_acquire) ? "yes" : "no") << "\n"
      << "  Tasks executed: "
      << tasks_executed_.load(std::memory_order_relaxed);
  return oss.str();
}

/** @copydoc SerialDebugScheduler::is_running */
bool SerialDebugScheduler::is_running() const noexcept {
  return running_.load(std::memory_order_acquire);
}

#if defined(PHOTOSPIDER_INTERNAL_SCHEDULER_TESTING)
/** @copydoc SerialDebugScheduler::testing_state */
SerialDebugScheduler::TestingState SerialDebugScheduler::testing_state() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return TestingState{
      active_epoch_,
      inline_tasks_to_complete_,
      borrowed_in_flight_tasks_,
      uncancellable_in_flight_tasks_,
      inline_exception_ != nullptr,
      !running_.load(std::memory_order_acquire) ||
          ((inline_tasks_to_complete_ <= 0 || inline_exception_ != nullptr) &&
           borrowed_in_flight_tasks_ == 0U &&
           uncancellable_in_flight_tasks_ == 0U)};
}
#endif

/** @copydoc SerialDebugScheduler::submit_initial_tasks */
void SerialDebugScheduler::submit_initial_tasks(std::vector<Task>&& tasks,
                                                int total_task_count,
                                                TaskPriority priority) {
  (void)priority;
  if (!is_running()) {
    throw std::logic_error("serial scheduler is not running");
  }
  const std::size_t valid_task_count = static_cast<std::size_t>(
      std::count_if(tasks.begin(), tasks.end(),
                    [](const Task& task) { return static_cast<bool>(task); }));
  validate_initial_count(valid_task_count, total_task_count);
  std::lock_guard<std::mutex> submission_lock(initial_submission_mutex_);
  std::uint64_t batch_epoch = 0U;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    batch_epoch = active_epoch_ == std::numeric_limits<std::uint64_t>::max()
                      ? 1U
                      : active_epoch_ + 1U;
    active_epoch_ = batch_epoch;
    inline_exception_ = nullptr;
    inline_tasks_to_complete_ = valid_task_count == 0U ? 0 : total_task_count;
  }
  for (Task& task : tasks) {
    if (!task || !try_begin_work(batch_epoch)) {
      continue;
    }
    std::exception_ptr error;
    {
      TaskContextScope context_scope(this, host_context_, -1, batch_epoch);
      try {
        task();
      } catch (...) {
        error = std::current_exception();
      }
    }
    if (error == nullptr) {
      tasks_executed_.fetch_add(1, std::memory_order_relaxed);
    }
    finish_work(batch_epoch, std::move(error));
  }
  completion_cv_.notify_all();
}

/** @copydoc SerialDebugScheduler::submit_initial_task_handles */
void SerialDebugScheduler::submit_initial_task_handles(
    std::vector<TaskHandle>&& handles, int total_task_count,
    TaskPriority priority) {
  (void)priority;
  if (!is_running()) {
    throw std::logic_error("serial scheduler is not running");
  }
  const std::size_t valid_handle_count = static_cast<std::size_t>(std::count_if(
      handles.begin(), handles.end(),
      [](const TaskHandle& handle) { return static_cast<bool>(handle); }));
  validate_initial_count(valid_handle_count, total_task_count);
  std::lock_guard<std::mutex> submission_lock(initial_submission_mutex_);
  std::uint64_t batch_epoch = 0U;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    batch_epoch = active_epoch_ == std::numeric_limits<std::uint64_t>::max()
                      ? 1U
                      : active_epoch_ + 1U;
    active_epoch_ = batch_epoch;
    inline_exception_ = nullptr;
    inline_tasks_to_complete_ = valid_handle_count == 0U ? 0 : total_task_count;
  }
  for (const TaskHandle& handle : handles) {
    if (!handle || !try_begin_work(batch_epoch)) {
      continue;
    }
    std::exception_ptr error;
    {
      TaskContextScope context_scope(this, host_context_, -1, batch_epoch);
      try {
        handle.run();
      } catch (...) {
        error = std::current_exception();
      }
    }
    if (error == nullptr) {
      tasks_executed_.fetch_add(1, std::memory_order_relaxed);
    }
    finish_work(batch_epoch, std::move(error));
  }
  completion_cv_.notify_all();
}

/** @copydoc SerialDebugScheduler::submit_ready_task_from_worker */
void SerialDebugScheduler::submit_ready_task_from_worker(
    Task&& task, TaskPriority priority) {
  if (tls_serial_owner != this || tls_serial_epoch == 0U) {
    return;
  }
  submit_ready_task_any_thread(std::move(task), priority, tls_serial_epoch);
}

/** @copydoc SerialDebugScheduler::submit_ready_task_handles_from_worker */
void SerialDebugScheduler::submit_ready_task_handles_from_worker(
    std::vector<TaskHandle>&& handles, TaskPriority priority) {
  (void)priority;
  if (tls_serial_owner != this || tls_serial_epoch == 0U) {
    return;
  }
  const std::uint64_t epoch = tls_serial_epoch;
  for (const TaskHandle& handle : handles) {
    if (!handle || !try_begin_work(epoch)) {
      continue;
    }
    std::exception_ptr error;
    {
      TaskContextScope context_scope(this, host_context_, -1, epoch);
      try {
        handle.run();
      } catch (...) {
        error = std::current_exception();
      }
    }
    if (error == nullptr) {
      tasks_executed_.fetch_add(1, std::memory_order_relaxed);
    }
    finish_work(epoch, std::move(error));
  }
}

/** @copydoc SerialDebugScheduler::submit_ready_task_any_thread */
void SerialDebugScheduler::submit_ready_task_any_thread(
    Task&& task, TaskPriority priority, std::optional<std::uint64_t> epoch) {
  (void)priority;
  if (!task) {
    return;
  }
  std::uint64_t selected_epoch = 0U;
  if (epoch.has_value()) {
    selected_epoch = *epoch;
  } else {
    std::lock_guard<std::mutex> lock(state_mutex_);
    selected_epoch = active_epoch_;
  }
  if (!try_begin_work(selected_epoch)) {
    return;
  }
  std::exception_ptr error;
  {
    TaskContextScope context_scope(this, host_context_, -1, selected_epoch);
    try {
      task();
    } catch (...) {
      error = std::current_exception();
    }
  }
  if (error == nullptr) {
    tasks_executed_.fetch_add(1, std::memory_order_relaxed);
  }
  finish_work(selected_epoch, std::move(error));
}

/** @copydoc SerialDebugScheduler::wait_for_completion */
void SerialDebugScheduler::wait_for_completion() {
  std::unique_lock<std::mutex> lock(state_mutex_);
  completion_cv_.wait(lock, [this]() {
    return !running_.load(std::memory_order_acquire) ||
           (inline_tasks_to_complete_ <= 0 && borrowed_in_flight_tasks_ == 0U &&
            uncancellable_in_flight_tasks_ == 0U) ||
           (inline_exception_ != nullptr && borrowed_in_flight_tasks_ == 0U &&
            uncancellable_in_flight_tasks_ == 0U);
  });
  if (inline_exception_ != nullptr) {
    const std::exception_ptr error = inline_exception_;
    lock.unlock();
    std::rethrow_exception(error);
  }
}

/** @copydoc SerialDebugScheduler::set_exception */
void SerialDebugScheduler::set_exception(std::exception_ptr error) {
  if (error == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const std::uint64_t publisher_epoch = current_call_epoch();
    if ((publisher_epoch == 0U || publisher_epoch == active_epoch_) &&
        inline_exception_ == nullptr) {
      inline_exception_ = std::move(error);
    }
  }
  completion_cv_.notify_all();
}

/** @copydoc SerialDebugScheduler::inc_tasks_to_complete */
void SerialDebugScheduler::inc_tasks_to_complete(int delta) {
  if (delta > 0) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const std::uint64_t publisher_epoch = current_call_epoch();
    if (publisher_epoch == 0U || publisher_epoch == active_epoch_) {
      if (inline_tasks_to_complete_ > std::numeric_limits<int>::max() - delta) {
        throw std::overflow_error(
            "serial scheduler completion counter overflow");
      }
      inline_tasks_to_complete_ += delta;
    }
  }
}

/** @copydoc SerialDebugScheduler::dec_tasks_to_complete */
void SerialDebugScheduler::dec_tasks_to_complete() {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const std::uint64_t publisher_epoch = current_call_epoch();
    if ((publisher_epoch == 0U || publisher_epoch == active_epoch_) &&
        inline_tasks_to_complete_ > 0) {
      --inline_tasks_to_complete_;
    }
  }
  completion_cv_.notify_all();
}

/** @copydoc SerialDebugScheduler::log_event */
void SerialDebugScheduler::log_event(SchedulerTraceAction action,
                                     int node_id) noexcept {
  if (host_context_ != nullptr) {
    host_context_->log_event(
        action, node_id, tls_serial_owner == this ? tls_serial_worker_id : -1,
        tls_serial_owner == this ? tls_serial_epoch : 0U);
  }
}

/** @copydoc SerialDebugScheduler::try_begin_work */
bool SerialDebugScheduler::try_begin_work(std::uint64_t epoch) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (inline_exception_ != nullptr || (epoch != 0U && epoch != active_epoch_)) {
    return false;
  }
  if (epoch == 0U) {
    ++uncancellable_in_flight_tasks_;
  } else {
    ++borrowed_in_flight_tasks_;
  }
  return true;
}

/** @copydoc SerialDebugScheduler::finish_work */
void SerialDebugScheduler::finish_work(std::uint64_t epoch,
                                       std::exception_ptr error) {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (epoch == 0U) {
      if (uncancellable_in_flight_tasks_ > 0U) {
        --uncancellable_in_flight_tasks_;
      }
    } else if (borrowed_in_flight_tasks_ > 0U) {
      --borrowed_in_flight_tasks_;
    }
    if (error != nullptr && (epoch == 0U || epoch == active_epoch_) &&
        inline_exception_ == nullptr) {
      inline_exception_ = std::move(error);
    }
  }
  completion_cv_.notify_all();
}

/** @copydoc SerialDebugScheduler::current_call_epoch */
std::uint64_t SerialDebugScheduler::current_call_epoch() const noexcept {
  return tls_serial_owner == this ? tls_serial_epoch : 0U;
}

}  // namespace ps
