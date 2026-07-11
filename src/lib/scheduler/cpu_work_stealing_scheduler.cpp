// Photospider kernel: CpuWorkStealingScheduler implementation
// M3.3: 将现有的 run_loop 和队列逻辑迁移至可插拔调度器

#include "scheduler/cpu_work_stealing_scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <exception>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "runtime/graph_runtime.hpp"

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
#include "scheduler/scheduler_exception_test_hooks.hpp"
#endif

namespace ps {

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
namespace {

/** @brief Borrowed immutable CPU exception-publication hook for one test. */
using CpuExceptionHook = testing::SchedulerExceptionPublicationHook;
/** @brief Atomically published borrowed CPU exception hook. */
std::atomic<const CpuExceptionHook*> cpu_exception_publication_hook{nullptr};
/** @brief Borrowed CPU local-ready publication hook for one test. */
using CpuLocalReadyHook = testing::SchedulerCpuLocalReadyHook;
/** @brief Atomically published borrowed CPU local-ready hook. */
std::atomic<const CpuLocalReadyHook*> cpu_local_ready_hook{nullptr};
/** @brief Borrowed deterministic CPU failure injection hook for one test. */
using CpuFailureHook = testing::SchedulerFailureInjectionHook;
/** @brief Atomically published borrowed CPU failure hook. */
std::atomic<const CpuFailureHook*> cpu_failure_injection_hook{nullptr};
/** @brief Borrowed CPU start-publication hook for one deterministic test. */
using CpuStartHook = testing::SchedulerStartPublicationHook;
/** @brief Atomically published borrowed CPU start-publication hook. */
std::atomic<const CpuStartHook*> cpu_start_publication_hook{nullptr};

/**
 * @brief Invokes the installed CPU publication barrier without allocation.
 *
 * @return Nothing.
 * @throws Nothing; the callback contract is noexcept.
 * @note The hook is reached only by the first exception publisher after the
 * consumer-visible flag changes state.
 */
void invoke_cpu_exception_publication_hook() noexcept {
  const auto* hook =
      cpu_exception_publication_hook.load(std::memory_order_acquire);
  if (hook && hook->after_flag_visible) {
    hook->after_flag_visible(hook->context);
  }
}

/**
 * @brief Invokes the pre-visibility CPU exception barrier without allocation.
 * @return Nothing.
 * @throws Nothing; the callback contract is noexcept.
 * @note The caller holds `global_queues_mutex_`, so a new initial batch cannot
 * reset exception state until the current publisher completes all state stores.
 */
void invoke_cpu_exception_before_visibility_hook() noexcept {
  const auto* hook =
      cpu_exception_publication_hook.load(std::memory_order_acquire);
  if (hook && hook->before_flag_visible) {
    hook->before_flag_visible(hook->context);
  }
}

/**
 * @brief Invokes the CPU start-publication barrier without allocation.
 * @return Nothing.
 * @throws Nothing; the callback contract is noexcept.
 * @note The complete member worker vector is installed while public running
 * state is still false when this callback runs.
 */
void invoke_cpu_start_publication_hook() noexcept {
  const auto* hook = cpu_start_publication_hook.load(std::memory_order_acquire);
  if (hook && hook->before_running_visible) {
    hook->before_running_visible(hook->context);
  }
}

/**
 * @brief Invokes the installed local enqueue/ready barrier without allocation.
 * @return Nothing.
 * @throws Nothing; the callback contract is noexcept.
 * @note The caller holds global_queues_mutex_ and one local queue mutex.
 */
void invoke_cpu_local_ready_hook() noexcept {
  const auto* hook = cpu_local_ready_hook.load(std::memory_order_acquire);
  if (hook && hook->between_enqueue_and_ready) {
    hook->between_enqueue_and_ready(hook->context);
  }
}

/**
 * @brief Invokes one deterministic CPU failure point.
 * @param point Lifecycle or queue mutation about to execute.
 * @param attempt One-based attempt number within the current operation.
 * @return Nothing when the operation should continue.
 * @throws Any exception selected by the installed BUILD_TESTING hook.
 */
void invoke_cpu_failure_hook(testing::SchedulerFailurePoint point,
                             std::size_t attempt) {
  const auto* hook = cpu_failure_injection_hook.load(std::memory_order_acquire);
  if (hook && hook->before) {
    hook->before(hook->context, point, attempt);
  }
}

}  // namespace

namespace testing {

/** @copydoc set_cpu_scheduler_exception_publication_hook */
void set_cpu_scheduler_exception_publication_hook(
    const SchedulerExceptionPublicationHook* hook) noexcept {
  cpu_exception_publication_hook.store(hook, std::memory_order_release);
}

/** @copydoc set_cpu_scheduler_start_publication_hook */
void set_cpu_scheduler_start_publication_hook(
    const SchedulerStartPublicationHook* hook) noexcept {
  cpu_start_publication_hook.store(hook, std::memory_order_release);
}

/** @copydoc set_cpu_scheduler_local_ready_hook */
void set_cpu_scheduler_local_ready_hook(
    const SchedulerCpuLocalReadyHook* hook) noexcept {
  cpu_local_ready_hook.store(hook, std::memory_order_release);
}

/** @copydoc set_cpu_scheduler_failure_injection_hook */
void set_cpu_scheduler_failure_injection_hook(
    const SchedulerFailureInjectionHook* hook) noexcept {
  cpu_failure_injection_hook.store(hook, std::memory_order_release);
}

/** @copydoc cpu_scheduler_transactional_snapshot */
SchedulerTransactionalStateSnapshot cpu_scheduler_transactional_snapshot(
    void* scheduler) noexcept {
  auto* concrete = static_cast<CpuWorkStealingScheduler*>(scheduler);
  if (!concrete) {
    return {};
  }
  std::lock_guard<std::mutex> global_lock(concrete->global_queues_mutex_);
  std::size_t queued = concrete->high_priority_queue_.size() +
                       concrete->normal_priority_queue_.size();
  for (std::size_t index = 0; index < concrete->local_task_queues_.size();
       ++index) {
    if (index < concrete->local_queue_mutexes_.size() &&
        concrete->local_queue_mutexes_[index]) {
      std::lock_guard<std::mutex> local_lock(
          *concrete->local_queue_mutexes_[index]);
      queued += concrete->local_task_queues_[index].size();
    }
  }
  std::lock_guard<std::mutex> exception_lock(concrete->exception_mutex_);
  return {
      concrete->running_.load(std::memory_order_acquire),
      concrete->worker_loop_active_.load(std::memory_order_acquire),
      queued,
      concrete->ready_task_count_.load(std::memory_order_acquire),
      concrete->tasks_to_complete_.load(std::memory_order_acquire),
      static_cast<std::size_t>(
          concrete->in_flight_tasks_.load(std::memory_order_acquire)),
      concrete->active_epoch_.load(std::memory_order_acquire),
      concrete->epoch_counter_.load(std::memory_order_acquire),
      concrete->workers_.size(),
      concrete->local_task_queues_.size(),
      concrete->exception_claimed_.load(std::memory_order_acquire),
      concrete->has_exception_.load(std::memory_order_acquire),
      static_cast<bool>(concrete->first_exception_),
      concrete->exception_epoch_.load(std::memory_order_acquire),
      concrete->exception_cleanup_complete_.load(std::memory_order_acquire),
  };
}

/** @copydoc cpu_scheduler_exception_publication_snapshot */
SchedulerExceptionPublicationSnapshot
cpu_scheduler_exception_publication_snapshot(void* scheduler) noexcept {
  auto* concrete = static_cast<CpuWorkStealingScheduler*>(scheduler);
  if (!concrete) {
    return {};
  }
  std::lock_guard<std::mutex> lock(concrete->exception_mutex_);
  return {
      concrete->has_exception_.load(std::memory_order_acquire),
      static_cast<bool>(concrete->first_exception_),
      concrete->exception_cleanup_complete_.load(std::memory_order_acquire),
      static_cast<std::size_t>(
          concrete->in_flight_tasks_.load(std::memory_order_acquire)),
      static_cast<std::uint64_t>(
          concrete->exception_epoch_.load(std::memory_order_acquire)),
      static_cast<std::int64_t>(
          concrete->ready_task_count_.load(std::memory_order_acquire)),
  };
}

}  // namespace testing
#endif

// Thread-local storage for worker ID and epoch tracking
thread_local int CpuWorkStealingScheduler::tls_worker_id_ = -1;
thread_local uint64_t CpuWorkStealingScheduler::tls_active_epoch_ = 0;

/** @copydoc CpuWorkStealingScheduler::CpuWorkStealingScheduler */
CpuWorkStealingScheduler::CpuWorkStealingScheduler(unsigned int num_workers)
    : configured_workers_(num_workers) {
  if (configured_workers_ == 0) {
    configured_workers_ = std::max(1u, std::thread::hardware_concurrency());
  }
}

/** @copydoc CpuWorkStealingScheduler::~CpuWorkStealingScheduler */
CpuWorkStealingScheduler::~CpuWorkStealingScheduler() {
  shutdown();
}

/** @copydoc CpuWorkStealingScheduler::attach */
void CpuWorkStealingScheduler::attach(GraphRuntime* runtime) {
  runtime_ = runtime;
}

/** @copydoc CpuWorkStealingScheduler::detach */
void CpuWorkStealingScheduler::detach() {
  runtime_ = nullptr;
}

/** @copydoc CpuWorkStealingScheduler::start */
void CpuWorkStealingScheduler::start() {
  if (running_.load(std::memory_order_acquire)) {
    return;
  }

  const unsigned int staged_worker_count = configured_workers_;
  std::vector<std::deque<ScheduledTask>> staged_local_queues;
  std::vector<std::unique_ptr<std::mutex>> staged_local_mutexes;
  std::vector<std::thread> staged_workers;
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  std::size_t resource_attempt = 0;
  invoke_cpu_failure_hook(
      testing::SchedulerFailurePoint::StartResourceAllocation,
      ++resource_attempt);
#endif
  staged_local_queues.resize(staged_worker_count);
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  invoke_cpu_failure_hook(
      testing::SchedulerFailurePoint::StartResourceAllocation,
      ++resource_attempt);
#endif
  staged_local_mutexes.reserve(staged_worker_count);
  for (unsigned int index = 0; index < staged_worker_count; ++index) {
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
    invoke_cpu_failure_hook(
        testing::SchedulerFailurePoint::StartResourceAllocation,
        ++resource_attempt);
#endif
    staged_local_mutexes.push_back(std::make_unique<std::mutex>());
  }
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  invoke_cpu_failure_hook(
      testing::SchedulerFailurePoint::StartResourceAllocation,
      ++resource_attempt);
#endif
  staged_workers.reserve(staged_worker_count);

  reset_exception_state();
  ready_task_count_.store(0, std::memory_order_relaxed);
  sleeping_thread_count_.store(0, std::memory_order_relaxed);
  tasks_to_complete_.store(0, std::memory_order_relaxed);
  in_flight_tasks_.store(0, std::memory_order_relaxed);
  high_enqueued_.store(0, std::memory_order_relaxed);
  normal_enqueued_.store(0, std::memory_order_relaxed);
  high_executed_.store(0, std::memory_order_relaxed);
  normal_executed_.store(0, std::memory_order_relaxed);
  total_tasks_scheduled_.store(0, std::memory_order_relaxed);

  num_workers_ = staged_worker_count;
  local_task_queues_.swap(staged_local_queues);
  local_queue_mutexes_.swap(staged_local_mutexes);
  worker_loop_active_.store(true, std::memory_order_release);

  try {
    for (unsigned int index = 0; index < num_workers_; ++index) {
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
      invoke_cpu_failure_hook(testing::SchedulerFailurePoint::CpuThreadCreate,
                              index + 1);
#endif
      staged_workers.emplace_back(&CpuWorkStealingScheduler::run_loop, this,
                                  static_cast<int>(index));
    }
  } catch (...) {
    const std::exception_ptr start_error = std::current_exception();
    {
      std::scoped_lock<std::mutex, std::mutex> lock(global_queues_mutex_,
                                                    completion_mutex_);
      worker_loop_active_.store(false, std::memory_order_release);
      running_.store(false, std::memory_order_release);
    }
    cv_task_available_.notify_all();
    cv_completion_.notify_all();
    for (auto& worker : staged_workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    {
      std::lock_guard<std::mutex> lock(global_queues_mutex_);
      high_priority_queue_.clear();
      normal_priority_queue_.clear();
    }
    local_task_queues_.clear();
    local_queue_mutexes_.clear();
    num_workers_ = 0;
    ready_task_count_.store(0, std::memory_order_relaxed);
    sleeping_thread_count_.store(0, std::memory_order_relaxed);
    tasks_to_complete_.store(0, std::memory_order_relaxed);
    in_flight_tasks_.store(0, std::memory_order_relaxed);
    high_enqueued_.store(0, std::memory_order_relaxed);
    normal_enqueued_.store(0, std::memory_order_relaxed);
    high_executed_.store(0, std::memory_order_relaxed);
    normal_executed_.store(0, std::memory_order_relaxed);
    total_tasks_scheduled_.store(0, std::memory_order_relaxed);
    reset_exception_state();
    std::rethrow_exception(start_error);
  }
  workers_.swap(staged_workers);
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  invoke_cpu_start_publication_hook();
#endif
  running_.store(true, std::memory_order_release);
}

/** @copydoc CpuWorkStealingScheduler::shutdown */
void CpuWorkStealingScheduler::shutdown() {
  {
    std::scoped_lock<std::mutex, std::mutex> lock(global_queues_mutex_,
                                                  completion_mutex_);
    if (!running_.load(std::memory_order_acquire) &&
        !worker_loop_active_.load(std::memory_order_acquire)) {
      return;
    }
    running_.store(false, std::memory_order_release);
    worker_loop_active_.store(false, std::memory_order_release);
  }

  cv_task_available_.notify_all();
  cv_completion_.notify_all();

  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  local_task_queues_.clear();
  local_queue_mutexes_.clear();
  num_workers_ = 0;

  // Drain pending tasks
  {
    std::lock_guard<std::mutex> lock(global_queues_mutex_);
    while (!high_priority_queue_.empty()) {
      high_priority_queue_.pop_front();
    }
    while (!normal_priority_queue_.empty()) {
      normal_priority_queue_.pop_front();
    }
  }
  ready_task_count_.store(0, std::memory_order_relaxed);
  sleeping_thread_count_.store(0, std::memory_order_relaxed);
  tasks_to_complete_.store(0, std::memory_order_relaxed);
  in_flight_tasks_.store(0, std::memory_order_relaxed);
}

/** @copydoc CpuWorkStealingScheduler::name */
std::string CpuWorkStealingScheduler::name() const {
  return "CpuWorkStealingScheduler";
}

/** @copydoc CpuWorkStealingScheduler::get_stats */
std::string CpuWorkStealingScheduler::get_stats() const {
  std::ostringstream oss;
  oss << "Workers: " << num_workers_
      << ", Ready tasks: " << ready_task_count_.load(std::memory_order_relaxed)
      << ", Sleeping: "
      << sleeping_thread_count_.load(std::memory_order_relaxed)
      << ", High enqueued/executed: "
      << high_enqueued_.load(std::memory_order_relaxed) << "/"
      << high_executed_.load(std::memory_order_relaxed)
      << ", Normal enqueued/executed: "
      << normal_enqueued_.load(std::memory_order_relaxed) << "/"
      << normal_executed_.load(std::memory_order_relaxed)
      << ", Total scheduled: "
      << total_tasks_scheduled_.load(std::memory_order_relaxed);
  return oss.str();
}

/** @copydoc CpuWorkStealingScheduler::is_running */
bool CpuWorkStealingScheduler::is_running() const {
  return running_.load(std::memory_order_acquire);
}

/** @copydoc CpuWorkStealingScheduler::task_runtime_running */
bool CpuWorkStealingScheduler::task_runtime_running() const {
  return is_running();
}

/** @copydoc CpuWorkStealingScheduler::log_event */
void CpuWorkStealingScheduler::log_event(SchedulerTraceAction action,
                                         int node_id) {
  if (!runtime_) {
    return;
  }

  GraphRuntime::SchedulerEvent::Action runtime_action =
      GraphRuntime::SchedulerEvent::EXECUTE;
  switch (action) {
    case SchedulerTraceAction::AssignInitial:
      runtime_action = GraphRuntime::SchedulerEvent::ASSIGN_INITIAL;
      break;
    case SchedulerTraceAction::Execute:
      runtime_action = GraphRuntime::SchedulerEvent::EXECUTE;
      break;
    case SchedulerTraceAction::ExecuteTile:
      runtime_action = GraphRuntime::SchedulerEvent::EXECUTE_TILE;
      break;
    case SchedulerTraceAction::ExecuteDirtySource:
      runtime_action = GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_SOURCE;
      break;
    case SchedulerTraceAction::ExecuteDirtyDownstreamNode:
      runtime_action =
          GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_DOWNSTREAM_NODE;
      break;
    case SchedulerTraceAction::ExecuteDirtyDownstreamTile:
      runtime_action =
          GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_DOWNSTREAM_TILE;
      break;
    case SchedulerTraceAction::SkipStaleGeneration:
      runtime_action = GraphRuntime::SchedulerEvent::SKIP_STALE_GENERATION;
      break;
    case SchedulerTraceAction::RethrowException:
      runtime_action = GraphRuntime::SchedulerEvent::RETHROW_EXCEPTION;
      break;
  }
  runtime_->log_event(runtime_action, node_id, this_worker_id(),
                      this_task_epoch());
}

/** @copydoc CpuWorkStealingScheduler::this_worker_id */
int CpuWorkStealingScheduler::this_worker_id() {
  return tls_worker_id_;
}

/** @copydoc CpuWorkStealingScheduler::this_task_epoch */
uint64_t CpuWorkStealingScheduler::this_task_epoch() {
  return tls_active_epoch_;
}

/** @copydoc CpuWorkStealingScheduler::active_epoch */
uint64_t CpuWorkStealingScheduler::active_epoch() const {
  return active_epoch_.load(std::memory_order_acquire);
}

/** @copydoc CpuWorkStealingScheduler::begin_new_epoch */
uint64_t CpuWorkStealingScheduler::begin_new_epoch() {
  uint64_t next = epoch_counter_.fetch_add(1, std::memory_order_acq_rel) + 1;
  active_epoch_.store(next, std::memory_order_release);
  return next;
}

/** @copydoc CpuWorkStealingScheduler::should_cancel_epoch */
bool CpuWorkStealingScheduler::should_cancel_epoch(uint64_t epoch) const {
  if (epoch == 0) {
    return false;
  }
  return epoch < active_epoch();
}

/** @copydoc CpuWorkStealingScheduler::steal_task */
std::optional<CpuWorkStealingScheduler::ScheduledTask>
CpuWorkStealingScheduler::steal_task(int stealer_id) {
  int n = static_cast<int>(num_workers_);
  if (n <= 1) {
    return std::nullopt;
  }

  std::lock_guard<std::mutex> global_lock(global_queues_mutex_);

  static thread_local std::mt19937 rng(std::random_device{}() + stealer_id);
  int start = std::uniform_int_distribution<int>(0, n - 2)(rng);

  for (int i = 0; i < n - 1; ++i) {
    int victim_id = (start + i) % (n - 1);
    if (victim_id >= stealer_id) {
      victim_id++;
    }

    if (victim_id < 0 ||
        victim_id >= static_cast<int>(local_queue_mutexes_.size()) ||
        victim_id >= static_cast<int>(local_task_queues_.size())) {
      continue;
    }

    std::lock_guard<std::mutex> lock(*local_queue_mutexes_[victim_id]);
    if (!local_task_queues_[victim_id].empty()) {
      ScheduledTask stolen_task =
          std::move(local_task_queues_[victim_id].front());
      local_task_queues_[victim_id].pop_front();
      ready_task_count_.fetch_sub(1, std::memory_order_release);
      in_flight_tasks_.fetch_add(1, std::memory_order_acq_rel);
      return stolen_task;
    }
  }
  return std::nullopt;
}

/** @copydoc CpuWorkStealingScheduler::run_loop */
void CpuWorkStealingScheduler::run_loop(int thread_id) {
  tls_worker_id_ = thread_id;

  while (worker_loop_active_.load(std::memory_order_acquire)) {
    // If an exception was raised, park until cleared
    if (has_exception_.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> lock(global_queues_mutex_);
      cv_task_available_.wait(lock, [&] {
        return !has_exception_.load(std::memory_order_acquire) ||
               !worker_loop_active_.load(std::memory_order_acquire);
      });
      continue;
    }

    ScheduledTask scheduled;
    bool found_task = false;

    // 1) Try high priority global queue first (preemptive)
    {
      std::lock_guard<std::mutex> lock(global_queues_mutex_);
      if (!high_priority_queue_.empty()) {
        scheduled = std::move(high_priority_queue_.front());
        high_priority_queue_.pop_front();
        ready_task_count_.fetch_sub(1, std::memory_order_release);
        in_flight_tasks_.fetch_add(1, std::memory_order_acq_rel);
        found_task = true;
        high_executed_.fetch_add(1, std::memory_order_relaxed);
      }
    }

    // 2) Then try local normal queue (LIFO)
    if (!found_task) {
      if (thread_id >= 0 &&
          thread_id < static_cast<int>(local_queue_mutexes_.size())) {
        std::lock_guard<std::mutex> global_lock(global_queues_mutex_);
        std::lock_guard<std::mutex> lock(*local_queue_mutexes_[thread_id]);
        if (thread_id < static_cast<int>(local_task_queues_.size()) &&
            !local_task_queues_[thread_id].empty()) {
          scheduled = std::move(local_task_queues_[thread_id].back());
          local_task_queues_[thread_id].pop_back();
          ready_task_count_.fetch_sub(1, std::memory_order_release);
          in_flight_tasks_.fetch_add(1, std::memory_order_acq_rel);
          found_task = true;
          normal_executed_.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }

    // 3) Then try global normal queue (FIFO)
    if (!found_task) {
      std::lock_guard<std::mutex> lock(global_queues_mutex_);
      if (!normal_priority_queue_.empty()) {
        scheduled = std::move(normal_priority_queue_.front());
        normal_priority_queue_.pop_front();
        ready_task_count_.fetch_sub(1, std::memory_order_release);
        in_flight_tasks_.fetch_add(1, std::memory_order_acq_rel);
        found_task = true;
        normal_executed_.fetch_add(1, std::memory_order_relaxed);
      }
    }

    // 4) Finally, attempt to steal from other workers
    if (!found_task) {
      auto stolen_task = steal_task(thread_id);
      if (stolen_task) {
        scheduled = std::move(*stolen_task);
        found_task = true;
        normal_executed_.fetch_add(1, std::memory_order_relaxed);
      }
    }

    if (found_task) {
      if (should_cancel_epoch(scheduled.epoch)) {
        finish_in_flight_task();
        continue;
      }
      try {
        if (scheduled) {
          struct EpochScope {
            uint64_t* slot;
            uint64_t prev;
            EpochScope(uint64_t* s, uint64_t value) : slot(s), prev(*s) {
              *slot = value;
            }
            ~EpochScope() { *slot = prev; }
          } epoch_scope(&tls_active_epoch_, scheduled.epoch);
          struct RuntimeLogScope {
            RuntimeLogScope(int worker_id, uint64_t epoch) {
              GraphRuntime::set_scheduler_log_context(worker_id, epoch);
            }
            ~RuntimeLogScope() { GraphRuntime::clear_scheduler_log_context(); }
          } runtime_log_scope(thread_id, scheduled.epoch);
          scheduled.run();
        } else {
          set_exception(std::make_exception_ptr(std::runtime_error(
              "CpuWorkStealingScheduler: empty task invoked")));
        }
      } catch (...) {
        set_exception(std::current_exception());
      }
      finish_in_flight_task();
    } else {
      sleeping_thread_count_.fetch_add(1, std::memory_order_release);

      std::unique_lock<std::mutex> lock(global_queues_mutex_);

      if (ready_task_count_.load(std::memory_order_acquire) > 0) {
        sleeping_thread_count_.fetch_sub(1, std::memory_order_relaxed);
        continue;
      }

      cv_task_available_.wait(lock, [&] {
        return ready_task_count_.load(std::memory_order_acquire) > 0 ||
               !worker_loop_active_.load(std::memory_order_acquire) ||
               has_exception_.load(std::memory_order_acquire);
      });

      sleeping_thread_count_.fetch_sub(1, std::memory_order_relaxed);
    }
  }
}

/** @copydoc CpuWorkStealingScheduler::submit_initial_tasks */
void CpuWorkStealingScheduler::submit_initial_tasks(std::vector<Task>&& tasks,
                                                    int total_task_count,
                                                    TaskPriority priority) {
  if (total_task_count == 0 || tasks.empty() || num_workers_ == 0) {
    {
      std::lock_guard<std::mutex> gate(global_queues_mutex_);
      reset_exception_state();
      begin_new_epoch();
      tasks_to_complete_.store(0, std::memory_order_release);
    }
    cv_task_available_.notify_all();
    {
      std::lock_guard<std::mutex> lock(completion_mutex_);
      cv_completion_.notify_one();
    }
    return;
  }

  const int num_threads = static_cast<int>(num_workers_);
  uint64_t epoch = 0;
  int submitted = 0;
  if (priority == TaskPriority::High) {
    {
      std::lock_guard<std::mutex> lock(global_queues_mutex_);
      epoch = epoch_counter_.load(std::memory_order_acquire) + 1;
      const std::size_t original_size = high_priority_queue_.size();
      [[maybe_unused]] std::size_t push_attempt = 0;
      try {
        for (auto& task : tasks) {
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
          invoke_cpu_failure_hook(
              testing::SchedulerFailurePoint::BatchQueuePush, ++push_attempt);
#endif
          high_priority_queue_.push_back(ScheduledTask(epoch, std::move(task)));
          ++submitted;
        }
        reset_exception_state();
      } catch (...) {
        while (high_priority_queue_.size() > original_size) {
          high_priority_queue_.pop_back();
        }
        throw;
      }
      epoch_counter_.store(epoch, std::memory_order_release);
      active_epoch_.store(epoch, std::memory_order_release);
      tasks_to_complete_.store(submitted > 0 ? total_task_count : 0,
                               std::memory_order_release);
      ready_task_count_.fetch_add(submitted, std::memory_order_release);
      high_enqueued_.fetch_add(static_cast<std::uint64_t>(submitted),
                               std::memory_order_relaxed);
    }
    if (submitted > 0) {
      cv_task_available_.notify_all();
    } else {
      std::lock_guard<std::mutex> completion_lock(completion_mutex_);
      cv_completion_.notify_one();
    }
  } else {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::vector<std::size_t> original_sizes(
        static_cast<std::size_t>(num_threads));
    {
      std::lock_guard<std::mutex> global_lock(global_queues_mutex_);
      epoch = epoch_counter_.load(std::memory_order_acquire) + 1;
      for (int index = 0; index < num_threads; ++index) {
        std::lock_guard<std::mutex> local_lock(
            *local_queue_mutexes_[static_cast<std::size_t>(index)]);
        original_sizes[static_cast<std::size_t>(index)] =
            local_task_queues_[static_cast<std::size_t>(index)].size();
      }
      [[maybe_unused]] std::size_t push_attempt = 0;
      try {
        for (auto& task : tasks) {
          const int target_thread =
              std::uniform_int_distribution<int>(0, num_threads - 1)(rng);
          std::lock_guard<std::mutex> local_lock(
              *local_queue_mutexes_[static_cast<std::size_t>(target_thread)]);
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
          invoke_cpu_failure_hook(
              testing::SchedulerFailurePoint::BatchQueuePush, ++push_attempt);
#endif
          local_task_queues_[static_cast<std::size_t>(target_thread)].push_back(
              ScheduledTask(epoch, std::move(task)));
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
          invoke_cpu_local_ready_hook();
#endif
          ++submitted;
        }
        reset_exception_state();
      } catch (...) {
        for (int index = 0; index < num_threads; ++index) {
          const std::size_t queue_index = static_cast<std::size_t>(index);
          std::lock_guard<std::mutex> local_lock(
              *local_queue_mutexes_[queue_index]);
          while (local_task_queues_[queue_index].size() >
                 original_sizes[queue_index]) {
            local_task_queues_[queue_index].pop_back();
          }
        }
        throw;
      }
      epoch_counter_.store(epoch, std::memory_order_release);
      active_epoch_.store(epoch, std::memory_order_release);
      tasks_to_complete_.store(submitted > 0 ? total_task_count : 0,
                               std::memory_order_release);
      ready_task_count_.fetch_add(submitted, std::memory_order_release);
      normal_enqueued_.fetch_add(static_cast<std::uint64_t>(submitted),
                                 std::memory_order_relaxed);
    }
    if (submitted > 0) {
      cv_task_available_.notify_all();
    } else {
      std::lock_guard<std::mutex> completion_lock(completion_mutex_);
      cv_completion_.notify_one();
    }
  }
}

/** @copydoc CpuWorkStealingScheduler::submit_initial_task_handles */
void CpuWorkStealingScheduler::submit_initial_task_handles(
    std::vector<TaskHandle>&& handles, int total_task_count,
    TaskPriority priority) {
  if (total_task_count == 0 || handles.empty() || num_workers_ == 0) {
    {
      std::lock_guard<std::mutex> gate(global_queues_mutex_);
      reset_exception_state();
      begin_new_epoch();
      tasks_to_complete_.store(0, std::memory_order_release);
    }
    cv_task_available_.notify_all();
    {
      std::lock_guard<std::mutex> lock(completion_mutex_);
      cv_completion_.notify_one();
    }
    return;
  }
  const int num_threads = static_cast<int>(num_workers_);
  uint64_t epoch = 0;
  int submitted = 0;
  if (priority == TaskPriority::High) {
    {
      std::lock_guard<std::mutex> lock(global_queues_mutex_);
      epoch = epoch_counter_.load(std::memory_order_acquire) + 1;
      const std::size_t original_size = high_priority_queue_.size();
      [[maybe_unused]] std::size_t push_attempt = 0;
      try {
        for (TaskHandle handle : handles) {
          if (!handle) {
            continue;
          }
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
          invoke_cpu_failure_hook(
              testing::SchedulerFailurePoint::BatchQueuePush, ++push_attempt);
#endif
          high_priority_queue_.push_back(ScheduledTask(epoch, handle));
          ++submitted;
        }
        reset_exception_state();
      } catch (...) {
        while (high_priority_queue_.size() > original_size) {
          high_priority_queue_.pop_back();
        }
        throw;
      }
      epoch_counter_.store(epoch, std::memory_order_release);
      active_epoch_.store(epoch, std::memory_order_release);
      tasks_to_complete_.store(submitted > 0 ? total_task_count : 0,
                               std::memory_order_release);
      ready_task_count_.fetch_add(submitted, std::memory_order_release);
      high_enqueued_.fetch_add(static_cast<std::uint64_t>(submitted),
                               std::memory_order_relaxed);
    }
    if (submitted > 0) {
      cv_task_available_.notify_all();
    } else {
      std::lock_guard<std::mutex> completion_lock(completion_mutex_);
      cv_completion_.notify_one();
    }
    return;
  }

  static thread_local std::mt19937 rng(std::random_device{}());
  std::vector<std::size_t> original_sizes(
      static_cast<std::size_t>(num_threads));
  {
    std::lock_guard<std::mutex> global_lock(global_queues_mutex_);
    epoch = epoch_counter_.load(std::memory_order_acquire) + 1;
    for (int index = 0; index < num_threads; ++index) {
      std::lock_guard<std::mutex> local_lock(
          *local_queue_mutexes_[static_cast<std::size_t>(index)]);
      original_sizes[static_cast<std::size_t>(index)] =
          local_task_queues_[static_cast<std::size_t>(index)].size();
    }
    [[maybe_unused]] std::size_t push_attempt = 0;
    try {
      for (TaskHandle handle : handles) {
        if (!handle) {
          continue;
        }
        const int target_thread =
            std::uniform_int_distribution<int>(0, num_threads - 1)(rng);
        std::lock_guard<std::mutex> local_lock(
            *local_queue_mutexes_[static_cast<std::size_t>(target_thread)]);
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
        invoke_cpu_failure_hook(testing::SchedulerFailurePoint::BatchQueuePush,
                                ++push_attempt);
#endif
        local_task_queues_[static_cast<std::size_t>(target_thread)].push_back(
            ScheduledTask(epoch, handle));
        ++submitted;
      }
      reset_exception_state();
    } catch (...) {
      for (int index = 0; index < num_threads; ++index) {
        const std::size_t queue_index = static_cast<std::size_t>(index);
        std::lock_guard<std::mutex> local_lock(
            *local_queue_mutexes_[queue_index]);
        while (local_task_queues_[queue_index].size() >
               original_sizes[queue_index]) {
          local_task_queues_[queue_index].pop_back();
        }
      }
      throw;
    }
    epoch_counter_.store(epoch, std::memory_order_release);
    active_epoch_.store(epoch, std::memory_order_release);
    tasks_to_complete_.store(submitted > 0 ? total_task_count : 0,
                             std::memory_order_release);
    ready_task_count_.fetch_add(submitted, std::memory_order_release);
    normal_enqueued_.fetch_add(static_cast<std::uint64_t>(submitted),
                               std::memory_order_relaxed);
  }
  if (submitted > 0) {
    cv_task_available_.notify_all();
  } else {
    std::lock_guard<std::mutex> completion_lock(completion_mutex_);
    cv_completion_.notify_one();
  }
}

/** @copydoc CpuWorkStealingScheduler::submit_ready_task_any_thread */
void CpuWorkStealingScheduler::submit_ready_task_any_thread(
    Task&& task, TaskPriority priority, std::optional<uint64_t> epoch) {
  if (!task) {
    return;
  }
  uint64_t resolved_epoch = epoch.has_value() ? *epoch : active_epoch();
  if (should_cancel_epoch(resolved_epoch)) {
    return;
  }
  ScheduledTask scheduled(resolved_epoch, std::move(task));
  {
    std::lock_guard<std::mutex> lock(global_queues_mutex_);
    if (exception_claimed_.load(std::memory_order_acquire) ||
        should_cancel_epoch(resolved_epoch)) {
      return;
    }
    if (priority == TaskPriority::High) {
      high_priority_queue_.push_back(std::move(scheduled));
      high_enqueued_.fetch_add(1, std::memory_order_relaxed);
    } else {
      normal_priority_queue_.push_back(std::move(scheduled));
      normal_enqueued_.fetch_add(1, std::memory_order_relaxed);
    }
    ready_task_count_.fetch_add(1, std::memory_order_relaxed);
  }
  cv_task_available_.notify_one();
}

/** @copydoc CpuWorkStealingScheduler::submit_ready_task_handle_any_thread */
void CpuWorkStealingScheduler::submit_ready_task_handle_any_thread(
    TaskHandle handle, TaskPriority priority, std::optional<uint64_t> epoch) {
  if (!handle) {
    return;
  }
  uint64_t resolved_epoch = epoch.has_value() ? *epoch : active_epoch();
  if (should_cancel_epoch(resolved_epoch)) {
    return;
  }
  ScheduledTask scheduled(resolved_epoch, handle);
  {
    std::lock_guard<std::mutex> lock(global_queues_mutex_);
    if (exception_claimed_.load(std::memory_order_acquire) ||
        should_cancel_epoch(resolved_epoch)) {
      return;
    }
    if (priority == TaskPriority::High) {
      high_priority_queue_.push_back(std::move(scheduled));
      high_enqueued_.fetch_add(1, std::memory_order_relaxed);
    } else {
      normal_priority_queue_.push_back(std::move(scheduled));
      normal_enqueued_.fetch_add(1, std::memory_order_relaxed);
    }
    ready_task_count_.fetch_add(1, std::memory_order_relaxed);
  }
  cv_task_available_.notify_one();
}

/** @copydoc CpuWorkStealingScheduler::submit_ready_task_handles_any_thread */
void CpuWorkStealingScheduler::submit_ready_task_handles_any_thread(
    std::vector<TaskHandle>&& handles, TaskPriority priority,
    std::optional<uint64_t> epoch) {
  if (handles.empty()) {
    return;
  }
  uint64_t resolved_epoch = epoch.has_value() ? *epoch : active_epoch();
  if (should_cancel_epoch(resolved_epoch)) {
    return;
  }

  int submitted = 0;
  {
    std::lock_guard<std::mutex> lock(global_queues_mutex_);
    if (exception_claimed_.load(std::memory_order_acquire) ||
        should_cancel_epoch(resolved_epoch)) {
      return;
    }
    std::deque<ScheduledTask>& target_queue = priority == TaskPriority::High
                                                  ? high_priority_queue_
                                                  : normal_priority_queue_;
    const std::size_t original_size = target_queue.size();
    [[maybe_unused]] std::size_t push_attempt = 0;
    try {
      for (TaskHandle handle : handles) {
        if (!handle) {
          continue;
        }
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
        invoke_cpu_failure_hook(testing::SchedulerFailurePoint::BatchQueuePush,
                                ++push_attempt);
#endif
        target_queue.push_back(ScheduledTask(resolved_epoch, handle));
        ++submitted;
      }
    } catch (...) {
      while (target_queue.size() > original_size) {
        target_queue.pop_back();
      }
      throw;
    }
    if (submitted > 0) {
      ready_task_count_.fetch_add(submitted, std::memory_order_relaxed);
      if (priority == TaskPriority::High) {
        high_enqueued_.fetch_add(static_cast<std::uint64_t>(submitted),
                                 std::memory_order_relaxed);
      } else {
        normal_enqueued_.fetch_add(static_cast<std::uint64_t>(submitted),
                                   std::memory_order_relaxed);
      }
    }
  }
  if (submitted > 1) {
    cv_task_available_.notify_all();
  } else if (submitted == 1) {
    cv_task_available_.notify_one();
  }
}

/** @copydoc CpuWorkStealingScheduler::submit_ready_task_from_worker */
void CpuWorkStealingScheduler::submit_ready_task_from_worker(
    Task&& task, TaskPriority priority) {
  int worker_id = this_worker_id();
  if (worker_id < 0 ||
      worker_id >= static_cast<int>(local_task_queues_.size())) {
    submit_ready_task_any_thread(std::move(task), priority, std::nullopt);
    return;
  }
  if (!task) {
    return;
  }
  uint64_t epoch = tls_active_epoch_;
  if (epoch == 0) {
    epoch = active_epoch();
  }
  if (should_cancel_epoch(epoch)) {
    return;
  }
  ScheduledTask scheduled(epoch, std::move(task));
  if (priority == TaskPriority::High) {
    submit_ready_task_any_thread(std::move(scheduled.task), TaskPriority::High,
                                 epoch);
  } else {
    {
      std::lock_guard<std::mutex> global_lock(global_queues_mutex_);
      if (exception_claimed_.load(std::memory_order_acquire) ||
          should_cancel_epoch(epoch)) {
        return;
      }
      std::lock_guard<std::mutex> local_lock(*local_queue_mutexes_[worker_id]);
      local_task_queues_[worker_id].push_back(std::move(scheduled));
      normal_enqueued_.fetch_add(1, std::memory_order_relaxed);
      ready_task_count_.fetch_add(1, std::memory_order_release);
    }
    cv_task_available_.notify_one();
  }
}

/** @copydoc CpuWorkStealingScheduler::submit_ready_task_handle_from_worker */
void CpuWorkStealingScheduler::submit_ready_task_handle_from_worker(
    TaskHandle handle, TaskPriority priority) {
  int worker_id = this_worker_id();
  if (worker_id < 0 ||
      worker_id >= static_cast<int>(local_task_queues_.size())) {
    submit_ready_task_handle_any_thread(handle, priority, std::nullopt);
    return;
  }
  if (!handle) {
    return;
  }
  uint64_t epoch = tls_active_epoch_;
  if (epoch == 0) {
    epoch = active_epoch();
  }
  if (should_cancel_epoch(epoch)) {
    return;
  }
  ScheduledTask scheduled(epoch, handle);
  if (priority == TaskPriority::High) {
    submit_ready_task_handle_any_thread(handle, TaskPriority::High, epoch);
  } else {
    {
      std::lock_guard<std::mutex> global_lock(global_queues_mutex_);
      if (exception_claimed_.load(std::memory_order_acquire) ||
          should_cancel_epoch(epoch)) {
        return;
      }
      std::lock_guard<std::mutex> local_lock(*local_queue_mutexes_[worker_id]);
      local_task_queues_[worker_id].push_back(std::move(scheduled));
      normal_enqueued_.fetch_add(1, std::memory_order_relaxed);
      ready_task_count_.fetch_add(1, std::memory_order_release);
    }
    cv_task_available_.notify_one();
  }
}

/** @copydoc CpuWorkStealingScheduler::submit_ready_task_handles_from_worker */
void CpuWorkStealingScheduler::submit_ready_task_handles_from_worker(
    std::vector<TaskHandle>&& handles, TaskPriority priority) {
  if (handles.empty()) {
    return;
  }
  int worker_id = this_worker_id();
  if (worker_id < 0 ||
      worker_id >= static_cast<int>(local_task_queues_.size()) ||
      priority == TaskPriority::High) {
    submit_ready_task_handles_any_thread(std::move(handles), priority,
                                         tls_active_epoch_);
    return;
  }

  uint64_t epoch = tls_active_epoch_;
  if (epoch == 0) {
    epoch = active_epoch();
  }
  if (should_cancel_epoch(epoch)) {
    return;
  }

  int submitted = 0;
  {
    std::lock_guard<std::mutex> global_lock(global_queues_mutex_);
    if (exception_claimed_.load(std::memory_order_acquire) ||
        should_cancel_epoch(epoch)) {
      return;
    }
    std::lock_guard<std::mutex> local_lock(*local_queue_mutexes_[worker_id]);
    const std::size_t original_size =
        local_task_queues_[static_cast<std::size_t>(worker_id)].size();
    [[maybe_unused]] std::size_t push_attempt = 0;
    try {
      for (TaskHandle handle : handles) {
        if (!handle) {
          continue;
        }
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
        invoke_cpu_failure_hook(testing::SchedulerFailurePoint::BatchQueuePush,
                                ++push_attempt);
#endif
        local_task_queues_[static_cast<std::size_t>(worker_id)].push_back(
            ScheduledTask(epoch, handle));
        ++submitted;
      }
    } catch (...) {
      auto& target_queue =
          local_task_queues_[static_cast<std::size_t>(worker_id)];
      while (target_queue.size() > original_size) {
        target_queue.pop_back();
      }
      throw;
    }
    ready_task_count_.fetch_add(submitted, std::memory_order_release);
    normal_enqueued_.fetch_add(static_cast<std::uint64_t>(submitted),
                               std::memory_order_relaxed);
  }
  if (submitted > 1) {
    cv_task_available_.notify_all();
  } else if (submitted == 1) {
    cv_task_available_.notify_one();
  }
}

/** @copydoc CpuWorkStealingScheduler::dec_tasks_to_complete */
void CpuWorkStealingScheduler::dec_tasks_to_complete() {
  uint64_t epoch = tls_active_epoch_;
  if (epoch != 0 && epoch != active_epoch()) {
    return;
  }
  if (tasks_to_complete_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    std::lock_guard<std::mutex> lk(completion_mutex_);
    cv_completion_.notify_one();
  }
}

/** @copydoc CpuWorkStealingScheduler::inc_tasks_to_complete */
void CpuWorkStealingScheduler::inc_tasks_to_complete(int delta) {
  if (delta <= 0) {
    return;
  }
  uint64_t epoch = tls_active_epoch_;
  if (epoch != 0 && epoch != active_epoch()) {
    return;
  }
  tasks_to_complete_.fetch_add(delta, std::memory_order_relaxed);
}

/** @copydoc CpuWorkStealingScheduler::finish_in_flight_task */
void CpuWorkStealingScheduler::finish_in_flight_task() noexcept {
  std::lock_guard<std::mutex> lock(completion_mutex_);
  const int previous = in_flight_tasks_.fetch_sub(1, std::memory_order_acq_rel);
  if (previous <= 0) {
    std::terminate();
  }
  cv_completion_.notify_all();
}

/**
 * @brief Resets the synchronized first-exception publication state.
 *
 * @return Nothing.
 * @throws std::system_error only if locking a valid mutex fails.
 * @note New batches call this before publishing work. The caller owns the
 * scheduler lifecycle and must not overlap batch reset with previous work.
 */
void CpuWorkStealingScheduler::reset_exception_state() {
  std::lock_guard<std::mutex> lock(exception_mutex_);
  first_exception_ = nullptr;
  has_exception_.store(false, std::memory_order_release);
  exception_claimed_.store(false, std::memory_order_release);
  exception_epoch_.store(0, std::memory_order_release);
  exception_cleanup_complete_.store(false, std::memory_order_release);
}

/**
 * @brief Waits for task completion and rethrows an atomically published error.
 *
 * @return Nothing for a completed/stopped batch.
 * @throws The exact first worker exception when one was published.
 * @note The captured epoch cannot complete until its queue cleanup and every
 * dequeued callback have settled. Pointer and flag are then read/cleared
 * together under `exception_mutex_`; a null pointer is never rethrown.
 */
void CpuWorkStealingScheduler::wait_for_completion() {
  const uint64_t wait_epoch = active_epoch();
  {
    std::unique_lock<std::mutex> lock(completion_mutex_);
    cv_completion_.wait(lock, [&] {
      const bool settled =
          in_flight_tasks_.load(std::memory_order_acquire) == 0;
      const bool completed =
          tasks_to_complete_.load(std::memory_order_acquire) == 0 && settled;
      const bool failed =
          has_exception_.load(std::memory_order_acquire) &&
          exception_cleanup_complete_.load(std::memory_order_acquire) &&
          exception_epoch_.load(std::memory_order_acquire) == wait_epoch &&
          settled;
      return completed || failed || !running_.load(std::memory_order_acquire);
    });
  }

  std::exception_ptr e;
  {
    std::lock_guard<std::mutex> lock(exception_mutex_);
    if (has_exception_.load(std::memory_order_acquire) &&
        exception_epoch_.load(std::memory_order_acquire) == wait_epoch) {
      e = first_exception_;
      first_exception_ = nullptr;
      has_exception_.store(false, std::memory_order_release);
    }
  }
  if (e) {
    tasks_to_complete_.store(0, std::memory_order_release);
    cv_task_available_.notify_all();
    std::rethrow_exception(e);
  }
}

/**
 * @brief Selects and safely publishes the first exception of one batch.
 *
 * @param e Non-null exception captured at a worker boundary.
 * @return Nothing.
 * @throws Nothing under valid mutex state.
 * @note `global_queues_mutex_` is the transaction gate shared with every
 * global/local queue commit. The publisher acquires it before choosing the
 * batch epoch and claiming the exception, so a concurrent initial batch is
 * observed either wholly before or wholly after publication. The pointer and
 * epoch are then stored, new ready work is rejected, and the same gate remains
 * held while global/local queues are drained and the ready predicate is reset.
 * Cleanup is marked complete before `has_exception_` is release-published.
 */
void CpuWorkStealingScheduler::set_exception(std::exception_ptr e) {
  {
    std::lock_guard<std::mutex> global_lock(global_queues_mutex_);
    const uint64_t publisher_epoch =
        tls_active_epoch_ != 0 ? tls_active_epoch_ : active_epoch();
    if (publisher_epoch != active_epoch()) {
      return;
    }
    bool expected = false;
    if (!exception_claimed_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(exception_mutex_);
      first_exception_ = e;
      exception_epoch_.store(publisher_epoch, std::memory_order_release);
    }
    while (!high_priority_queue_.empty()) {
      high_priority_queue_.pop_front();
    }
    while (!normal_priority_queue_.empty()) {
      normal_priority_queue_.pop_front();
    }
    for (size_t i = 0; i < local_task_queues_.size(); ++i) {
      if (i < local_queue_mutexes_.size() && local_queue_mutexes_[i]) {
        std::lock_guard<std::mutex> local_lock(*local_queue_mutexes_[i]);
        local_task_queues_[i].clear();
      }
    }
    ready_task_count_.store(0, std::memory_order_relaxed);
    exception_cleanup_complete_.store(true, std::memory_order_release);
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
    invoke_cpu_exception_before_visibility_hook();
#endif
    has_exception_.store(true, std::memory_order_release);
  }
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  invoke_cpu_exception_publication_hook();
#endif
  cv_task_available_.notify_all();
  {
    std::lock_guard<std::mutex> lock(completion_mutex_);
    cv_completion_.notify_all();
  }
}

}  // namespace ps
