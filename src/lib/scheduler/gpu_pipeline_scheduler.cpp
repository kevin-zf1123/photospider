// Photospider kernel: GpuPipelineScheduler implementation
// M3.5: 异构调度器 - 支持 HP GPU 队列和 RT CPU 高优先级队列
// Scheduler dispatches already-planned HP/RT tasks.

#include "scheduler/gpu_pipeline_scheduler.hpp"

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

/** @brief Borrowed immutable GPU-pipeline publication hook for one test. */
using GpuExceptionHook = testing::SchedulerExceptionPublicationHook;
/** @brief Atomically published borrowed GPU-pipeline exception hook. */
std::atomic<const GpuExceptionHook*> gpu_exception_publication_hook{nullptr};
/** @brief Borrowed GPU CPU-wait handshake hook for one deterministic test. */
using GpuCpuWaitHook = testing::SchedulerCpuWaitHook;
/** @brief Atomically published borrowed GPU CPU-wait hook. */
std::atomic<const GpuCpuWaitHook*> gpu_cpu_wait_hook{nullptr};
/** @brief Borrowed deterministic pipeline failure injection hook. */
using GpuFailureHook = testing::SchedulerFailureInjectionHook;
/** @brief Atomically published borrowed GPU-pipeline failure hook. */
std::atomic<const GpuFailureHook*> gpu_failure_injection_hook{nullptr};
/** @brief BUILD_TESTING-only route override for GPU queue transaction tests. */
std::atomic<bool> gpu_force_route{false};
/** @brief Borrowed pipeline start-publication hook for one deterministic test.
 */
using GpuStartHook = testing::SchedulerStartPublicationHook;
/** @brief Atomically published borrowed pipeline start-publication hook. */
std::atomic<const GpuStartHook*> gpu_start_publication_hook{nullptr};

/**
 * @brief Invokes the installed GPU-pipeline barrier without allocation.
 *
 * @return Nothing.
 * @throws Nothing; the callback contract is noexcept.
 * @note CPU and GPU worker exception entrances both converge on this hook.
 */
void invoke_gpu_exception_publication_hook() noexcept {
  const auto* hook =
      gpu_exception_publication_hook.load(std::memory_order_acquire);
  if (hook && hook->after_flag_visible) {
    hook->after_flag_visible(hook->context);
  }
}

/**
 * @brief Invokes the pre-visibility pipeline exception barrier without
 * allocation.
 * @return Nothing.
 * @throws Nothing; the callback contract is noexcept.
 * @note The caller holds both queue mutexes, so initial batch reset cannot
 * overtake cleanup or consumer-visible exception publication.
 */
void invoke_gpu_exception_before_visibility_hook() noexcept {
  const auto* hook =
      gpu_exception_publication_hook.load(std::memory_order_acquire);
  if (hook && hook->before_flag_visible) {
    hook->before_flag_visible(hook->context);
  }
}

/**
 * @brief Invokes the pipeline start-publication barrier without allocation.
 * @return Nothing.
 * @throws Nothing; the callback contract is noexcept.
 * @note Complete CPU/GPU member worker vectors exist while public running state
 * is still false when this callback runs.
 */
void invoke_gpu_start_publication_hook() noexcept {
  const auto* hook = gpu_start_publication_hook.load(std::memory_order_acquire);
  if (hook && hook->before_running_visible) {
    hook->before_running_visible(hook->context);
  }
}

/**
 * @brief Invokes the installed CPU-wait hook without allocation.
 * @return Nothing.
 * @throws Nothing; the callback contract is noexcept.
 * @note The caller holds the shared RT/HP-CPU queue mutex.
 */
void invoke_gpu_cpu_wait_hook() noexcept {
  const auto* hook = gpu_cpu_wait_hook.load(std::memory_order_acquire);
  if (hook && hook->before_wait) {
    hook->before_wait(hook->context);
  }
}

/**
 * @brief Invokes one deterministic pipeline failure point.
 * @param point Lifecycle or queue mutation about to execute.
 * @param attempt One-based attempt number within the current operation.
 * @return Nothing when the operation should continue.
 * @throws Any exception selected by the installed test hook.
 */
void invoke_gpu_failure_hook(testing::SchedulerFailurePoint point,
                             std::size_t attempt) {
  const auto* hook = gpu_failure_injection_hook.load(std::memory_order_acquire);
  if (hook && hook->before) {
    hook->before(hook->context, point, attempt);
  }
}

}  // namespace

namespace testing {

/** @copydoc set_gpu_scheduler_exception_publication_hook */
void set_gpu_scheduler_exception_publication_hook(
    const SchedulerExceptionPublicationHook* hook) noexcept {
  gpu_exception_publication_hook.store(hook, std::memory_order_release);
}

/** @copydoc set_gpu_scheduler_start_publication_hook */
void set_gpu_scheduler_start_publication_hook(
    const SchedulerStartPublicationHook* hook) noexcept {
  gpu_start_publication_hook.store(hook, std::memory_order_release);
}

/** @copydoc set_gpu_scheduler_cpu_wait_hook */
void set_gpu_scheduler_cpu_wait_hook(
    const SchedulerCpuWaitHook* hook) noexcept {
  gpu_cpu_wait_hook.store(hook, std::memory_order_release);
}

/** @copydoc set_gpu_scheduler_failure_injection_hook */
void set_gpu_scheduler_failure_injection_hook(
    const SchedulerFailureInjectionHook* hook) noexcept {
  gpu_failure_injection_hook.store(hook, std::memory_order_release);
}

/** @copydoc set_gpu_scheduler_force_gpu_route */
void set_gpu_scheduler_force_gpu_route(bool enabled) noexcept {
  gpu_force_route.store(enabled, std::memory_order_release);
}

/** @copydoc gpu_scheduler_transactional_snapshot */
SchedulerTransactionalStateSnapshot gpu_scheduler_transactional_snapshot(
    void* scheduler) noexcept {
  auto* concrete = static_cast<GpuPipelineScheduler*>(scheduler);
  if (!concrete) {
    return {};
  }
  std::scoped_lock lock(concrete->rt_queue_mutex_, concrete->gpu_queue_mutex_);
  std::lock_guard<std::mutex> exception_lock(concrete->exception_mutex_);
  return {
      concrete->running_.load(std::memory_order_acquire),
      concrete->worker_loop_active_.load(std::memory_order_acquire),
      concrete->rt_queue_.size() + concrete->hp_cpu_queue_.size() +
          concrete->gpu_queue_.size(),
      concrete->rt_ready_count_.load(std::memory_order_acquire) +
          concrete->hp_cpu_ready_count_.load(std::memory_order_acquire) +
          concrete->gpu_ready_count_.load(std::memory_order_acquire),
      concrete->tasks_to_complete_.load(std::memory_order_acquire),
      static_cast<std::size_t>(
          concrete->in_flight_tasks_.load(std::memory_order_acquire)),
      concrete->active_epoch_.load(std::memory_order_acquire),
      concrete->epoch_counter_.load(std::memory_order_acquire),
      concrete->cpu_workers_.size() + concrete->gpu_workers_.size(),
      0,
      concrete->exception_claimed_.load(std::memory_order_acquire),
      concrete->has_exception_.load(std::memory_order_acquire),
      static_cast<bool>(concrete->first_exception_),
      concrete->exception_epoch_.load(std::memory_order_acquire),
      concrete->exception_cleanup_complete_.load(std::memory_order_acquire),
  };
}

/** @copydoc gpu_scheduler_exception_publication_snapshot */
SchedulerExceptionPublicationSnapshot
gpu_scheduler_exception_publication_snapshot(void* scheduler) noexcept {
  auto* concrete = static_cast<GpuPipelineScheduler*>(scheduler);
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
          concrete->rt_ready_count_.load(std::memory_order_acquire) +
          concrete->hp_cpu_ready_count_.load(std::memory_order_acquire) +
          concrete->gpu_ready_count_.load(std::memory_order_acquire)),
  };
}

}  // namespace testing
#endif

// Thread-local storage definitions
thread_local int GpuPipelineScheduler::tls_worker_id_ = -1;
thread_local uint64_t GpuPipelineScheduler::tls_active_epoch_ = 0;
thread_local bool GpuPipelineScheduler::tls_is_gpu_worker_ = false;

/** @copydoc GpuPipelineScheduler::GpuPipelineScheduler */
GpuPipelineScheduler::GpuPipelineScheduler(const Config& config)
    : config_(config) {
  if (config_.cpu_workers == 0) {
    config_.cpu_workers = std::max(1u, std::thread::hardware_concurrency());
  }
}

/** @copydoc GpuPipelineScheduler::~GpuPipelineScheduler */
GpuPipelineScheduler::~GpuPipelineScheduler() {
  shutdown();
}

/** @copydoc GpuPipelineScheduler::attach */
void GpuPipelineScheduler::attach(GraphRuntime* runtime) {
  runtime_ = runtime;
  if (running_.load(std::memory_order_acquire)) {
    start_gpu_workers_if_available();
  }
}

/** @copydoc GpuPipelineScheduler::detach */
void GpuPipelineScheduler::detach() {
  runtime_ = nullptr;
}

/** @copydoc GpuPipelineScheduler::start */
void GpuPipelineScheduler::start() {
  if (running_.load(std::memory_order_acquire)) {
    start_gpu_workers_if_available();
    return;
  }

  const unsigned int staged_cpu_count = config_.cpu_workers;
  unsigned int staged_gpu_count = 0;
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  const bool gpu_available =
      is_gpu_available() || gpu_force_route.load(std::memory_order_acquire);
#else
  const bool gpu_available = is_gpu_available();
#endif
  if (gpu_available) {
    staged_gpu_count = config_.gpu_workers;
  }
  std::vector<std::thread> staged_cpu_workers;
  std::vector<std::thread> staged_gpu_workers;
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  invoke_gpu_failure_hook(
      testing::SchedulerFailurePoint::StartResourceAllocation, 1);
#endif
  staged_cpu_workers.reserve(staged_cpu_count);
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  invoke_gpu_failure_hook(
      testing::SchedulerFailurePoint::StartResourceAllocation, 2);
#endif
  staged_gpu_workers.reserve(staged_gpu_count);

  reset_exception_state();
  rt_ready_count_.store(0, std::memory_order_relaxed);
  hp_cpu_ready_count_.store(0, std::memory_order_relaxed);
  gpu_ready_count_.store(0, std::memory_order_relaxed);
  sleeping_cpu_count_.store(0, std::memory_order_relaxed);
  sleeping_gpu_count_.store(0, std::memory_order_relaxed);
  tasks_to_complete_.store(0, std::memory_order_relaxed);
  in_flight_tasks_.store(0, std::memory_order_relaxed);
  rt_tasks_executed_.store(0, std::memory_order_relaxed);
  hp_cpu_tasks_executed_.store(0, std::memory_order_relaxed);
  gpu_tasks_executed_.store(0, std::memory_order_relaxed);
  total_tasks_scheduled_.store(0, std::memory_order_relaxed);

  num_cpu_workers_ = staged_cpu_count;
  num_gpu_workers_ = 0;
  worker_loop_active_.store(true, std::memory_order_release);

  try {
    for (unsigned int index = 0; index < staged_cpu_count; ++index) {
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
      invoke_gpu_failure_hook(testing::SchedulerFailurePoint::CpuThreadCreate,
                              index + 1);
#endif
      staged_cpu_workers.emplace_back(&GpuPipelineScheduler::cpu_run_loop, this,
                                      static_cast<int>(index));
    }
    for (unsigned int index = 0; index < staged_gpu_count; ++index) {
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
      invoke_gpu_failure_hook(testing::SchedulerFailurePoint::GpuThreadCreate,
                              index + 1);
#endif
      staged_gpu_workers.emplace_back(
          &GpuPipelineScheduler::gpu_run_loop, this,
          static_cast<int>(staged_cpu_count + index));
    }
  } catch (...) {
    const std::exception_ptr start_error = std::current_exception();
    {
      std::scoped_lock lock(rt_queue_mutex_, gpu_queue_mutex_,
                            completion_mutex_);
      worker_loop_active_.store(false, std::memory_order_release);
      running_.store(false, std::memory_order_release);
    }
    rt_cv_.notify_all();
    gpu_cv_.notify_all();
    cv_completion_.notify_all();
    for (auto& worker : staged_cpu_workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    for (auto& worker : staged_gpu_workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    {
      std::scoped_lock lock(rt_queue_mutex_, gpu_queue_mutex_);
      rt_queue_.clear();
      hp_cpu_queue_.clear();
      gpu_queue_.clear();
    }
    num_cpu_workers_ = 0;
    num_gpu_workers_ = 0;
    rt_ready_count_.store(0, std::memory_order_relaxed);
    hp_cpu_ready_count_.store(0, std::memory_order_relaxed);
    gpu_ready_count_.store(0, std::memory_order_relaxed);
    sleeping_cpu_count_.store(0, std::memory_order_relaxed);
    sleeping_gpu_count_.store(0, std::memory_order_relaxed);
    tasks_to_complete_.store(0, std::memory_order_relaxed);
    in_flight_tasks_.store(0, std::memory_order_relaxed);
    rt_tasks_executed_.store(0, std::memory_order_relaxed);
    hp_cpu_tasks_executed_.store(0, std::memory_order_relaxed);
    gpu_tasks_executed_.store(0, std::memory_order_relaxed);
    total_tasks_scheduled_.store(0, std::memory_order_relaxed);
    reset_exception_state();
    std::rethrow_exception(start_error);
  }

  cpu_workers_.swap(staged_cpu_workers);
  gpu_workers_.swap(staged_gpu_workers);
  num_gpu_workers_ = staged_gpu_count;
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  invoke_gpu_start_publication_hook();
#endif
  running_.store(true, std::memory_order_release);
}

/** @copydoc GpuPipelineScheduler::shutdown */
void GpuPipelineScheduler::shutdown() {
  {
    std::scoped_lock lock(rt_queue_mutex_, gpu_queue_mutex_, completion_mutex_);
    if (!running_.load(std::memory_order_acquire) &&
        !worker_loop_active_.load(std::memory_order_acquire)) {
      return;
    }
    running_.store(false, std::memory_order_release);
    worker_loop_active_.store(false, std::memory_order_release);
  }

  // Notify all workers
  rt_cv_.notify_all();
  gpu_cv_.notify_all();
  cv_completion_.notify_all();

  // Join CPU workers
  for (auto& worker : cpu_workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  cpu_workers_.clear();

  // Join GPU workers
  for (auto& worker : gpu_workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  gpu_workers_.clear();
  num_cpu_workers_ = 0;
  num_gpu_workers_ = 0;

  // Drain pending tasks
  {
    std::lock_guard<std::mutex> lock(rt_queue_mutex_);
    while (!rt_queue_.empty()) {
      rt_queue_.pop_front();
    }
    while (!hp_cpu_queue_.empty()) {
      hp_cpu_queue_.pop_front();
    }
  }
  rt_ready_count_.store(0, std::memory_order_relaxed);
  hp_cpu_ready_count_.store(0, std::memory_order_relaxed);
  gpu_ready_count_.store(0, std::memory_order_relaxed);
  sleeping_cpu_count_.store(0, std::memory_order_relaxed);
  sleeping_gpu_count_.store(0, std::memory_order_relaxed);
  tasks_to_complete_.store(0, std::memory_order_relaxed);
  in_flight_tasks_.store(0, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(gpu_queue_mutex_);
    while (!gpu_queue_.empty()) {
      gpu_queue_.pop_front();
    }
  }
}

/** @copydoc GpuPipelineScheduler::name */
std::string GpuPipelineScheduler::name() const {
  return "GpuPipelineScheduler";
}

/** @copydoc GpuPipelineScheduler::get_stats */
std::string GpuPipelineScheduler::get_stats() const {
  std::ostringstream oss;
  oss << "CPU Workers: " << num_cpu_workers_
      << " (sleeping: " << sleeping_cpu_count_.load(std::memory_order_relaxed)
      << ")"
      << ", GPU Workers: " << num_gpu_workers_
      << " (sleeping: " << sleeping_gpu_count_.load(std::memory_order_relaxed)
      << ")"
      << ", RT ready: " << rt_ready_count_.load(std::memory_order_relaxed)
      << ", HP CPU ready: "
      << hp_cpu_ready_count_.load(std::memory_order_relaxed)
      << ", GPU ready: " << gpu_ready_count_.load(std::memory_order_relaxed)
      << ", RT executed: " << rt_tasks_executed_.load(std::memory_order_relaxed)
      << ", HP CPU executed: "
      << hp_cpu_tasks_executed_.load(std::memory_order_relaxed)
      << ", GPU executed: "
      << gpu_tasks_executed_.load(std::memory_order_relaxed)
      << ", Total scheduled: "
      << total_tasks_scheduled_.load(std::memory_order_relaxed);
  return oss.str();
}

/** @copydoc GpuPipelineScheduler::is_running */
bool GpuPipelineScheduler::is_running() const {
  return running_.load(std::memory_order_acquire);
}

/** @copydoc GpuPipelineScheduler::task_runtime_running */
bool GpuPipelineScheduler::task_runtime_running() const {
  return is_running();
}

/** @copydoc GpuPipelineScheduler::submit_initial_tasks */
void GpuPipelineScheduler::submit_initial_tasks(std::vector<Task>&& tasks,
                                                int total_task_count,
                                                TaskPriority priority) {
  if (total_task_count == 0 || tasks.empty()) {
    {
      std::scoped_lock gate(rt_queue_mutex_, gpu_queue_mutex_);
      reset_exception_state();
      begin_new_epoch();
      tasks_to_complete_.store(0, std::memory_order_release);
    }
    rt_cv_.notify_all();
    gpu_cv_.notify_all();
    {
      std::lock_guard<std::mutex> lock(completion_mutex_);
      cv_completion_.notify_one();
    }
    return;
  }

  const bool dispatch_gpu =
      priority != TaskPriority::High && can_dispatch_hp_to_gpu();
  std::deque<ScheduledTask>* target_queue = nullptr;
  std::atomic<int>* ready_count = nullptr;
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
  if (priority == TaskPriority::High) {
    target_queue = &rt_queue_;
    ready_count = &rt_ready_count_;
    intent = ComputeIntent::RealTimeUpdate;
  } else if (dispatch_gpu) {
    target_queue = &gpu_queue_;
    ready_count = &gpu_ready_count_;
  } else {
    target_queue = &hp_cpu_queue_;
    ready_count = &hp_cpu_ready_count_;
  }

  int submitted = 0;
  {
    std::scoped_lock gate(rt_queue_mutex_, gpu_queue_mutex_);
    const uint64_t epoch = epoch_counter_.load(std::memory_order_acquire) + 1;
    const std::size_t original_size = target_queue->size();
    [[maybe_unused]] std::size_t push_attempt = 0;
    try {
      for (auto& task : tasks) {
        if (!task) {
          continue;
        }
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
        invoke_gpu_failure_hook(testing::SchedulerFailurePoint::BatchQueuePush,
                                ++push_attempt);
#endif
        target_queue->push_back(ScheduledTask(epoch, std::move(task), intent));
        ++submitted;
      }
      reset_exception_state();
    } catch (...) {
      while (target_queue->size() > original_size) {
        target_queue->pop_back();
      }
      throw;
    }
    epoch_counter_.store(epoch, std::memory_order_release);
    active_epoch_.store(epoch, std::memory_order_release);
    tasks_to_complete_.store(submitted > 0 ? total_task_count : 0,
                             std::memory_order_release);
    ready_count->fetch_add(submitted, std::memory_order_release);
  }
  rt_cv_.notify_all();
  gpu_cv_.notify_all();
  if (submitted == 0) {
    std::lock_guard<std::mutex> lock(completion_mutex_);
    cv_completion_.notify_one();
  }
}

/** @copydoc GpuPipelineScheduler::submit_initial_task_handles */
void GpuPipelineScheduler::submit_initial_task_handles(
    std::vector<TaskHandle>&& handles, int total_task_count,
    TaskPriority priority) {
  if (total_task_count == 0 || handles.empty()) {
    {
      std::scoped_lock gate(rt_queue_mutex_, gpu_queue_mutex_);
      reset_exception_state();
      begin_new_epoch();
      tasks_to_complete_.store(0, std::memory_order_release);
    }
    rt_cv_.notify_all();
    gpu_cv_.notify_all();
    {
      std::lock_guard<std::mutex> lock(completion_mutex_);
      cv_completion_.notify_one();
    }
    return;
  }

  const bool dispatch_gpu =
      priority != TaskPriority::High && can_dispatch_hp_to_gpu();
  std::deque<ScheduledTask>* target_queue = nullptr;
  std::atomic<int>* ready_count = nullptr;
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
  if (priority == TaskPriority::High) {
    target_queue = &rt_queue_;
    ready_count = &rt_ready_count_;
    intent = ComputeIntent::RealTimeUpdate;
  } else if (dispatch_gpu) {
    target_queue = &gpu_queue_;
    ready_count = &gpu_ready_count_;
  } else {
    target_queue = &hp_cpu_queue_;
    ready_count = &hp_cpu_ready_count_;
  }

  int submitted = 0;
  {
    std::scoped_lock gate(rt_queue_mutex_, gpu_queue_mutex_);
    const uint64_t epoch = epoch_counter_.load(std::memory_order_acquire) + 1;
    const std::size_t original_size = target_queue->size();
    [[maybe_unused]] std::size_t push_attempt = 0;
    try {
      for (TaskHandle handle : handles) {
        if (!handle) {
          continue;
        }
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
        invoke_gpu_failure_hook(testing::SchedulerFailurePoint::BatchQueuePush,
                                ++push_attempt);
#endif
        target_queue->push_back(ScheduledTask(epoch, handle, intent));
        ++submitted;
      }
      reset_exception_state();
    } catch (...) {
      while (target_queue->size() > original_size) {
        target_queue->pop_back();
      }
      throw;
    }
    epoch_counter_.store(epoch, std::memory_order_release);
    active_epoch_.store(epoch, std::memory_order_release);
    tasks_to_complete_.store(submitted > 0 ? total_task_count : 0,
                             std::memory_order_release);
    ready_count->fetch_add(submitted, std::memory_order_release);
  }
  rt_cv_.notify_all();
  gpu_cv_.notify_all();
  if (submitted == 0) {
    std::lock_guard<std::mutex> lock(completion_mutex_);
    cv_completion_.notify_one();
  }
}

/** @copydoc GpuPipelineScheduler::submit_ready_task_from_worker */
void GpuPipelineScheduler::submit_ready_task_from_worker(
    Task&& task, TaskPriority priority) {
  submit_ready_task_any_thread(std::move(task), priority, tls_active_epoch_);
}

/** @copydoc GpuPipelineScheduler::submit_ready_task_handle_from_worker */
void GpuPipelineScheduler::submit_ready_task_handle_from_worker(
    TaskHandle handle, TaskPriority priority) {
  submit_ready_task_handle_any_thread(handle, priority, tls_active_epoch_);
}

/** @copydoc GpuPipelineScheduler::submit_ready_task_handles_from_worker */
void GpuPipelineScheduler::submit_ready_task_handles_from_worker(
    std::vector<TaskHandle>&& handles, TaskPriority priority) {
  submit_ready_task_handles_any_thread(std::move(handles), priority,
                                       tls_active_epoch_);
}

/** @copydoc GpuPipelineScheduler::submit_ready_task_any_thread */
void GpuPipelineScheduler::submit_ready_task_any_thread(
    Task&& task, TaskPriority priority, std::optional<uint64_t> epoch) {
  if (!task) {
    return;
  }
  uint64_t resolved_epoch = epoch.value_or(active_epoch());
  if (should_cancel_epoch(resolved_epoch)) {
    return;
  }
  if (priority == TaskPriority::High) {
    submit_rt_task(std::move(task), resolved_epoch);
    return;
  }
  if (can_dispatch_hp_to_gpu()) {
    submit_gpu_task(std::move(task), resolved_epoch);
  } else {
    submit_hp_task(std::move(task), resolved_epoch);
  }
}

/** @copydoc GpuPipelineScheduler::submit_ready_task_handle_any_thread */
void GpuPipelineScheduler::submit_ready_task_handle_any_thread(
    TaskHandle handle, TaskPriority priority, std::optional<uint64_t> epoch) {
  if (!handle) {
    return;
  }
  uint64_t resolved_epoch = epoch.value_or(active_epoch());
  if (should_cancel_epoch(resolved_epoch)) {
    return;
  }
  if (priority == TaskPriority::High) {
    submit_rt_task_handle(handle, resolved_epoch);
    return;
  }
  if (can_dispatch_hp_to_gpu()) {
    submit_gpu_task_handle(handle, resolved_epoch);
  } else {
    submit_hp_task_handle(handle, resolved_epoch);
  }
}

/** @copydoc GpuPipelineScheduler::submit_ready_task_handles_any_thread */
void GpuPipelineScheduler::submit_ready_task_handles_any_thread(
    std::vector<TaskHandle>&& handles, TaskPriority priority,
    std::optional<uint64_t> epoch) {
  if (handles.empty()) {
    return;
  }
  uint64_t resolved_epoch = epoch.value_or(active_epoch());
  if (should_cancel_epoch(resolved_epoch)) {
    return;
  }

  if (priority == TaskPriority::High) {
    int submitted = 0;
    {
      std::lock_guard<std::mutex> lock(rt_queue_mutex_);
      if (exception_claimed_.load(std::memory_order_acquire) ||
          should_cancel_epoch(resolved_epoch)) {
        return;
      }
      const std::size_t original_size = rt_queue_.size();
      [[maybe_unused]] std::size_t push_attempt = 0;
      try {
        for (TaskHandle handle : handles) {
          if (!handle) {
            continue;
          }
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
          invoke_gpu_failure_hook(
              testing::SchedulerFailurePoint::BatchQueuePush, ++push_attempt);
#endif
          rt_queue_.push_back(ScheduledTask(resolved_epoch, handle,
                                            ComputeIntent::RealTimeUpdate));
          ++submitted;
        }
      } catch (...) {
        while (rt_queue_.size() > original_size) {
          rt_queue_.pop_back();
        }
        throw;
      }
      rt_ready_count_.fetch_add(submitted, std::memory_order_release);
    }
    if (submitted > 0) {
      rt_cv_.notify_all();
    }
    return;
  }

  const bool dispatch_gpu = can_dispatch_hp_to_gpu();
  int submitted = 0;
  if (dispatch_gpu) {
    {
      std::lock_guard<std::mutex> lock(gpu_queue_mutex_);
      if (exception_claimed_.load(std::memory_order_acquire) ||
          should_cancel_epoch(resolved_epoch)) {
        return;
      }
      const std::size_t original_size = gpu_queue_.size();
      [[maybe_unused]] std::size_t push_attempt = 0;
      try {
        for (TaskHandle handle : handles) {
          if (!handle) {
            continue;
          }
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
          invoke_gpu_failure_hook(
              testing::SchedulerFailurePoint::BatchQueuePush, ++push_attempt);
#endif
          gpu_queue_.push_back(ScheduledTask(
              resolved_epoch, handle, ComputeIntent::GlobalHighPrecision));
          ++submitted;
        }
      } catch (...) {
        while (gpu_queue_.size() > original_size) {
          gpu_queue_.pop_back();
        }
        throw;
      }
      gpu_ready_count_.fetch_add(submitted, std::memory_order_release);
    }
    if (submitted > 0) {
      gpu_cv_.notify_all();
    }
    return;
  }

  {
    std::lock_guard<std::mutex> lock(rt_queue_mutex_);
    if (exception_claimed_.load(std::memory_order_acquire) ||
        should_cancel_epoch(resolved_epoch)) {
      return;
    }
    const std::size_t original_size = hp_cpu_queue_.size();
    [[maybe_unused]] std::size_t push_attempt = 0;
    try {
      for (TaskHandle handle : handles) {
        if (!handle) {
          continue;
        }
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
        invoke_gpu_failure_hook(testing::SchedulerFailurePoint::BatchQueuePush,
                                ++push_attempt);
#endif
        hp_cpu_queue_.push_back(ScheduledTask(
            resolved_epoch, handle, ComputeIntent::GlobalHighPrecision));
        ++submitted;
      }
    } catch (...) {
      while (hp_cpu_queue_.size() > original_size) {
        hp_cpu_queue_.pop_back();
      }
      throw;
    }
    hp_cpu_ready_count_.fetch_add(submitted, std::memory_order_release);
  }
  if (submitted > 0) {
    rt_cv_.notify_all();
  }
}

/** @copydoc GpuPipelineScheduler::log_event */
void GpuPipelineScheduler::log_event(SchedulerTraceAction action, int node_id) {
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

/** @copydoc GpuPipelineScheduler::this_worker_id */
int GpuPipelineScheduler::this_worker_id() {
  return tls_worker_id_;
}

/** @copydoc GpuPipelineScheduler::this_task_epoch */
uint64_t GpuPipelineScheduler::this_task_epoch() {
  return tls_active_epoch_;
}

/** @copydoc GpuPipelineScheduler::active_epoch */
uint64_t GpuPipelineScheduler::active_epoch() const {
  return active_epoch_.load(std::memory_order_acquire);
}

/** @copydoc GpuPipelineScheduler::begin_new_epoch */
uint64_t GpuPipelineScheduler::begin_new_epoch() {
  uint64_t next = epoch_counter_.fetch_add(1, std::memory_order_acq_rel) + 1;
  active_epoch_.store(next, std::memory_order_release);
  return next;
}

/** @copydoc GpuPipelineScheduler::should_cancel_epoch */
bool GpuPipelineScheduler::should_cancel_epoch(uint64_t epoch) const {
  if (epoch == 0) {
    return false;
  }
  return epoch < active_epoch();
}

/** @copydoc GpuPipelineScheduler::is_gpu_available */
bool GpuPipelineScheduler::is_gpu_available() const {
#ifdef __APPLE__
  return runtime_ && runtime_->get_metal_device() != nullptr;
#else
  return false;
#endif
}

/** @copydoc GpuPipelineScheduler::start_gpu_workers_if_available */
void GpuPipelineScheduler::start_gpu_workers_if_available() {
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  const bool gpu_available =
      is_gpu_available() || gpu_force_route.load(std::memory_order_acquire);
#else
  const bool gpu_available = is_gpu_available();
#endif
  if (!gpu_available || config_.gpu_workers == 0 || num_gpu_workers_ > 0) {
    return;
  }

  std::vector<std::thread> staged_workers;
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  invoke_gpu_failure_hook(
      testing::SchedulerFailurePoint::StartResourceAllocation, 1);
#endif
  staged_workers.reserve(config_.gpu_workers);
  try {
    for (unsigned int index = 0; index < config_.gpu_workers; ++index) {
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
      invoke_gpu_failure_hook(testing::SchedulerFailurePoint::GpuThreadCreate,
                              index + 1);
#endif
      staged_workers.emplace_back(&GpuPipelineScheduler::gpu_run_loop, this,
                                  static_cast<int>(num_cpu_workers_ + index));
    }
  } catch (...) {
    const std::exception_ptr start_error = std::current_exception();
    {
      std::scoped_lock lock(rt_queue_mutex_, gpu_queue_mutex_,
                            completion_mutex_);
      worker_loop_active_.store(false, std::memory_order_release);
      running_.store(false, std::memory_order_release);
    }
    rt_cv_.notify_all();
    gpu_cv_.notify_all();
    cv_completion_.notify_all();
    for (auto& worker : staged_workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    for (auto& worker : cpu_workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    for (auto& worker : gpu_workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    cpu_workers_.clear();
    gpu_workers_.clear();
    {
      std::scoped_lock lock(rt_queue_mutex_, gpu_queue_mutex_);
      rt_queue_.clear();
      hp_cpu_queue_.clear();
      gpu_queue_.clear();
    }
    num_cpu_workers_ = 0;
    num_gpu_workers_ = 0;
    rt_ready_count_.store(0, std::memory_order_relaxed);
    hp_cpu_ready_count_.store(0, std::memory_order_relaxed);
    gpu_ready_count_.store(0, std::memory_order_relaxed);
    sleeping_cpu_count_.store(0, std::memory_order_relaxed);
    sleeping_gpu_count_.store(0, std::memory_order_relaxed);
    tasks_to_complete_.store(0, std::memory_order_relaxed);
    in_flight_tasks_.store(0, std::memory_order_relaxed);
    rt_tasks_executed_.store(0, std::memory_order_relaxed);
    hp_cpu_tasks_executed_.store(0, std::memory_order_relaxed);
    gpu_tasks_executed_.store(0, std::memory_order_relaxed);
    total_tasks_scheduled_.store(0, std::memory_order_relaxed);
    reset_exception_state();
    std::rethrow_exception(start_error);
  }
  gpu_workers_.swap(staged_workers);
  num_gpu_workers_ = config_.gpu_workers;
}

/** @copydoc GpuPipelineScheduler::can_dispatch_hp_to_gpu */
bool GpuPipelineScheduler::can_dispatch_hp_to_gpu() const {
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  if (gpu_force_route.load(std::memory_order_acquire)) {
    return true;
  }
#endif
  if (!config_.prefer_gpu_for_hp || !is_gpu_available() ||
      config_.gpu_workers == 0) {
    return false;
  }
  if (!running_.load(std::memory_order_acquire)) {
    return true;
  }
  return num_gpu_workers_ > 0;
}

/** @copydoc GpuPipelineScheduler::get_available_devices */
std::vector<Device> GpuPipelineScheduler::get_available_devices() const {
  std::vector<Device> devices;
  devices.push_back(Device::CPU);

  if (can_dispatch_hp_to_gpu()) {
    devices.push_back(Device::GPU_METAL);
  }

  return devices;
}

/** @copydoc GpuPipelineScheduler::available_devices */
std::vector<Device> GpuPipelineScheduler::available_devices() const {
  return get_available_devices();
}

// =============================================================================
// CPU 工作线程主循环
// 优先处理 RT 任务，然后处理 HP CPU 任务
// =============================================================================
/** @copydoc GpuPipelineScheduler::cpu_run_loop */
void GpuPipelineScheduler::cpu_run_loop(int thread_id) {
  tls_worker_id_ = thread_id;
  tls_is_gpu_worker_ = false;

  while (worker_loop_active_.load(std::memory_order_acquire)) {
    // 检查异常状态
    if (has_exception_.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> lock(rt_queue_mutex_);
      rt_cv_.wait(lock, [&] {
        return !has_exception_.load(std::memory_order_acquire) ||
               !worker_loop_active_.load(std::memory_order_acquire);
      });
      continue;
    }

    ScheduledTask scheduled;
    bool found_task = false;

    // 1) 优先处理 RT 队列（高优先级抢占）
    {
      std::lock_guard<std::mutex> lock(rt_queue_mutex_);
      if (!rt_queue_.empty()) {
        scheduled = std::move(rt_queue_.front());
        rt_queue_.pop_front();
        in_flight_tasks_.fetch_add(1, std::memory_order_acq_rel);
        found_task = true;
        rt_ready_count_.fetch_sub(1, std::memory_order_relaxed);
      }
    }

    // 2) 然后处理 HP CPU 队列
    if (!found_task) {
      std::lock_guard<std::mutex> lock(rt_queue_mutex_);
      if (!hp_cpu_queue_.empty()) {
        scheduled = std::move(hp_cpu_queue_.front());
        hp_cpu_queue_.pop_front();
        in_flight_tasks_.fetch_add(1, std::memory_order_acq_rel);
        found_task = true;
        hp_cpu_ready_count_.fetch_sub(1, std::memory_order_relaxed);
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

          // 更新统计
          if (scheduled.intent == ComputeIntent::RealTimeUpdate) {
            rt_tasks_executed_.fetch_add(1, std::memory_order_relaxed);
          } else {
            hp_cpu_tasks_executed_.fetch_add(1, std::memory_order_relaxed);
          }
        } else {
          set_exception(std::make_exception_ptr(
              std::runtime_error("GpuPipelineScheduler: empty task invoked")));
        }
      } catch (...) {
        set_exception(std::current_exception());
      }
      finish_in_flight_task();
    } else {
      // 没有任务，进入休眠
      sleeping_cpu_count_.fetch_add(1, std::memory_order_release);

      std::unique_lock<std::mutex> lock(rt_queue_mutex_);
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
      invoke_gpu_cpu_wait_hook();
#endif
      rt_cv_.wait(lock, [&] {
        return rt_ready_count_.load(std::memory_order_acquire) > 0 ||
               hp_cpu_ready_count_.load(std::memory_order_acquire) > 0 ||
               !worker_loop_active_.load(std::memory_order_acquire) ||
               has_exception_.load(std::memory_order_acquire);
      });

      sleeping_cpu_count_.fetch_sub(1, std::memory_order_relaxed);
    }
  }
}

// =============================================================================
// GPU 工作线程主循环
// 专门处理 GPU 任务
// =============================================================================
/** @copydoc GpuPipelineScheduler::gpu_run_loop */
void GpuPipelineScheduler::gpu_run_loop(int thread_id) {
  tls_worker_id_ = thread_id;
  tls_is_gpu_worker_ = true;

  while (worker_loop_active_.load(std::memory_order_acquire)) {
    // 检查异常状态
    if (has_exception_.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> lock(gpu_queue_mutex_);
      gpu_cv_.wait(lock, [&] {
        return !has_exception_.load(std::memory_order_acquire) ||
               !worker_loop_active_.load(std::memory_order_acquire);
      });
      continue;
    }

    ScheduledTask scheduled;
    bool found_task = false;

    // 从 GPU 队列获取任务
    {
      std::lock_guard<std::mutex> lock(gpu_queue_mutex_);
      if (!gpu_queue_.empty()) {
        scheduled = std::move(gpu_queue_.front());
        gpu_queue_.pop_front();
        in_flight_tasks_.fetch_add(1, std::memory_order_acq_rel);
        found_task = true;
        gpu_ready_count_.fetch_sub(1, std::memory_order_relaxed);
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
          gpu_tasks_executed_.fetch_add(1, std::memory_order_relaxed);
        } else {
          set_exception(std::make_exception_ptr(std::runtime_error(
              "GpuPipelineScheduler: empty GPU task invoked")));
        }
      } catch (...) {
        set_exception(std::current_exception());
      }
      finish_in_flight_task();
    } else {
      // 没有任务，进入休眠
      sleeping_gpu_count_.fetch_add(1, std::memory_order_release);

      std::unique_lock<std::mutex> lock(gpu_queue_mutex_);
      gpu_cv_.wait(lock, [&] {
        return gpu_ready_count_.load(std::memory_order_acquire) > 0 ||
               !worker_loop_active_.load(std::memory_order_acquire) ||
               has_exception_.load(std::memory_order_acquire);
      });

      sleeping_gpu_count_.fetch_sub(1, std::memory_order_relaxed);
    }
  }
}

// =============================================================================
// 任务提交 API
// =============================================================================

/** @copydoc GpuPipelineScheduler::submit_rt_task */
void GpuPipelineScheduler::submit_rt_task(Task&& task, uint64_t epoch) {
  if (!task) {
    return;
  }
  uint64_t resolved_epoch = epoch != 0 ? epoch : active_epoch();
  if (should_cancel_epoch(resolved_epoch)) {
    return;
  }

  ScheduledTask scheduled(resolved_epoch, std::move(task),
                          ComputeIntent::RealTimeUpdate);
  {
    std::lock_guard<std::mutex> lock(rt_queue_mutex_);
    if (exception_claimed_.load(std::memory_order_acquire) ||
        should_cancel_epoch(resolved_epoch)) {
      return;
    }
    rt_queue_.push_back(std::move(scheduled));
    rt_ready_count_.fetch_add(1, std::memory_order_relaxed);
  }
  rt_cv_.notify_one();
}

/** @copydoc GpuPipelineScheduler::submit_rt_task_handle */
void GpuPipelineScheduler::submit_rt_task_handle(TaskHandle handle,
                                                 uint64_t epoch) {
  if (!handle) {
    return;
  }
  uint64_t resolved_epoch = epoch != 0 ? epoch : active_epoch();
  if (should_cancel_epoch(resolved_epoch)) {
    return;
  }

  ScheduledTask scheduled(resolved_epoch, handle,
                          ComputeIntent::RealTimeUpdate);
  {
    std::lock_guard<std::mutex> lock(rt_queue_mutex_);
    if (exception_claimed_.load(std::memory_order_acquire) ||
        should_cancel_epoch(resolved_epoch)) {
      return;
    }
    rt_queue_.push_back(std::move(scheduled));
    rt_ready_count_.fetch_add(1, std::memory_order_relaxed);
  }
  rt_cv_.notify_one();
}

/** @copydoc GpuPipelineScheduler::submit_hp_task */
void GpuPipelineScheduler::submit_hp_task(Task&& task, uint64_t epoch) {
  if (!task) {
    return;
  }
  uint64_t resolved_epoch = epoch != 0 ? epoch : active_epoch();
  if (should_cancel_epoch(resolved_epoch)) {
    return;
  }

  ScheduledTask scheduled(resolved_epoch, std::move(task),
                          ComputeIntent::GlobalHighPrecision);
  {
    std::lock_guard<std::mutex> lock(rt_queue_mutex_);
    if (exception_claimed_.load(std::memory_order_acquire) ||
        should_cancel_epoch(resolved_epoch)) {
      return;
    }
    hp_cpu_queue_.push_back(std::move(scheduled));
    hp_cpu_ready_count_.fetch_add(1, std::memory_order_relaxed);
  }
  rt_cv_.notify_one();  // CPU workers also handle HP tasks
}

/** @copydoc GpuPipelineScheduler::submit_hp_task_handle */
void GpuPipelineScheduler::submit_hp_task_handle(TaskHandle handle,
                                                 uint64_t epoch) {
  if (!handle) {
    return;
  }
  uint64_t resolved_epoch = epoch != 0 ? epoch : active_epoch();
  if (should_cancel_epoch(resolved_epoch)) {
    return;
  }

  ScheduledTask scheduled(resolved_epoch, handle,
                          ComputeIntent::GlobalHighPrecision);
  {
    std::lock_guard<std::mutex> lock(rt_queue_mutex_);
    if (exception_claimed_.load(std::memory_order_acquire) ||
        should_cancel_epoch(resolved_epoch)) {
      return;
    }
    hp_cpu_queue_.push_back(std::move(scheduled));
    hp_cpu_ready_count_.fetch_add(1, std::memory_order_relaxed);
  }
  rt_cv_.notify_one();
}

/** @copydoc GpuPipelineScheduler::submit_gpu_task */
void GpuPipelineScheduler::submit_gpu_task(Task&& task, uint64_t epoch) {
  if (!task) {
    return;
  }
  uint64_t resolved_epoch = epoch != 0 ? epoch : active_epoch();
  if (should_cancel_epoch(resolved_epoch)) {
    return;
  }

  ScheduledTask scheduled(resolved_epoch, std::move(task),
                          ComputeIntent::GlobalHighPrecision);
  {
    std::lock_guard<std::mutex> lock(gpu_queue_mutex_);
    if (exception_claimed_.load(std::memory_order_acquire) ||
        should_cancel_epoch(resolved_epoch)) {
      return;
    }
    gpu_queue_.push_back(std::move(scheduled));
    gpu_ready_count_.fetch_add(1, std::memory_order_relaxed);
  }
  gpu_cv_.notify_one();
}

/** @copydoc GpuPipelineScheduler::submit_gpu_task_handle */
void GpuPipelineScheduler::submit_gpu_task_handle(TaskHandle handle,
                                                  uint64_t epoch) {
  if (!handle) {
    return;
  }
  uint64_t resolved_epoch = epoch != 0 ? epoch : active_epoch();
  if (should_cancel_epoch(resolved_epoch)) {
    return;
  }

  ScheduledTask scheduled(resolved_epoch, handle,
                          ComputeIntent::GlobalHighPrecision);
  {
    std::lock_guard<std::mutex> lock(gpu_queue_mutex_);
    if (exception_claimed_.load(std::memory_order_acquire) ||
        should_cancel_epoch(resolved_epoch)) {
      return;
    }
    gpu_queue_.push_back(std::move(scheduled));
    gpu_ready_count_.fetch_add(1, std::memory_order_relaxed);
  }
  gpu_cv_.notify_one();
}

/**
 * @brief Resets the shared CPU/GPU first-exception publication state.
 *
 * @return Nothing.
 * @throws std::system_error only if locking a valid mutex fails.
 * @note Pipeline batch setup owns this lifecycle transition and must not
 * overlap work from the previous batch.
 */
void GpuPipelineScheduler::reset_exception_state() {
  std::lock_guard<std::mutex> lock(exception_mutex_);
  first_exception_ = nullptr;
  has_exception_.store(false, std::memory_order_release);
  exception_claimed_.store(false, std::memory_order_release);
  exception_epoch_.store(0, std::memory_order_release);
  exception_cleanup_complete_.store(false, std::memory_order_release);
}

/** @copydoc GpuPipelineScheduler::finish_in_flight_task */
void GpuPipelineScheduler::finish_in_flight_task() noexcept {
  std::lock_guard<std::mutex> lock(completion_mutex_);
  const int previous = in_flight_tasks_.fetch_sub(1, std::memory_order_acq_rel);
  if (previous <= 0) {
    std::terminate();
  }
  cv_completion_.notify_all();
}

/**
 * @brief Waits for pipeline completion and rethrows the exact first exception.
 *
 * @return Nothing for a completed/stopped batch.
 * @throws The first CPU/GPU worker exception when published.
 * @note The captured epoch's queue cleanup and in-flight CPU/GPU callback count
 * must settle before flag and pointer are consumed together under
 * `exception_mutex_`; queue waiters are awakened only after that clear.
 */
void GpuPipelineScheduler::wait_for_completion() {
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
    rt_cv_.notify_all();
    gpu_cv_.notify_all();
    std::rethrow_exception(e);
  }
}

/** @copydoc GpuPipelineScheduler::dec_tasks_to_complete */
void GpuPipelineScheduler::dec_tasks_to_complete() {
  uint64_t epoch = tls_active_epoch_;
  if (epoch != 0 && epoch != active_epoch()) {
    return;
  }
  if (tasks_to_complete_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    std::lock_guard<std::mutex> lk(completion_mutex_);
    cv_completion_.notify_one();
  }
}

/** @copydoc GpuPipelineScheduler::inc_tasks_to_complete */
void GpuPipelineScheduler::inc_tasks_to_complete(int delta) {
  if (delta <= 0) {
    return;
  }
  uint64_t epoch = tls_active_epoch_;
  if (epoch != 0 && epoch != active_epoch()) {
    return;
  }
  tasks_to_complete_.fetch_add(delta, std::memory_order_relaxed);
}

/**
 * @brief Selects and safely publishes one pipeline-batch exception.
 *
 * @param e Non-null exception captured by either worker loop.
 * @return Nothing.
 * @throws Nothing under valid mutex state.
 * @note Both queue mutexes form the pipeline transaction gate. Publication
 * acquires that gate before choosing the epoch and claiming the exception, so
 * every concurrent queue batch is observed as wholly committed or absent. It
 * then stores `first_exception_` and its epoch, drains all queues, marks
 * cleanup complete, and release-stores `has_exception_`. The claim latch
 * rejects every later publisher until batch reset.
 */
void GpuPipelineScheduler::set_exception(std::exception_ptr e) {
  {
    std::scoped_lock queue_lock(rt_queue_mutex_, gpu_queue_mutex_);
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

    // Drain all queues before making this batch exception consumer-visible.
    while (!rt_queue_.empty()) {
      rt_queue_.pop_front();
    }
    rt_ready_count_.store(0, std::memory_order_relaxed);
    while (!hp_cpu_queue_.empty()) {
      hp_cpu_queue_.pop_front();
    }
    hp_cpu_ready_count_.store(0, std::memory_order_relaxed);
    while (!gpu_queue_.empty()) {
      gpu_queue_.pop_front();
    }
    gpu_ready_count_.store(0, std::memory_order_relaxed);
    exception_cleanup_complete_.store(true, std::memory_order_release);
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
    invoke_gpu_exception_before_visibility_hook();
#endif
    has_exception_.store(true, std::memory_order_release);
  }
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  invoke_gpu_exception_publication_hook();
#endif

  rt_cv_.notify_all();
  gpu_cv_.notify_all();
  {
    std::lock_guard<std::mutex> lock(completion_mutex_);
    cv_completion_.notify_all();
  }
}

}  // namespace ps
