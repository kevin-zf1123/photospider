// Photospider kernel: GpuPipelineScheduler implementation
// M3.5: 异构调度器 - 支持 HP 走 GPU、RT 走 CPU 的混合计算模式
// Scheduler dispatches already-planned HP/RT tasks.

#include "kernel/scheduler/gpu_pipeline_scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <utility>

#include "kernel/graph_runtime.hpp"

namespace ps {

// Thread-local storage definitions
thread_local int GpuPipelineScheduler::tls_worker_id_ = -1;
thread_local uint64_t GpuPipelineScheduler::tls_active_epoch_ = 0;
thread_local bool GpuPipelineScheduler::tls_is_gpu_worker_ = false;

GpuPipelineScheduler::GpuPipelineScheduler(const Config& config)
    : config_(config) {
  if (config_.cpu_workers == 0) {
    config_.cpu_workers = std::max(1u, std::thread::hardware_concurrency());
  }
  init_priority_tables();
}

// =============================================================================
// 资源偏好表初始化
// =============================================================================
void GpuPipelineScheduler::init_priority_tables() {
  // HP 模式优先级表；Macro/Micro 是 HP domain 内的粒度偏好。
  // 优先级值越小越优先
  hp_priority_table_ = {
      {Device::GPU_METAL, true, TileSizePreference::UNDEFINED, 10,
       "GPU Monolithic"},
      {Device::CPU, true, TileSizePreference::UNDEFINED, 30, "CPU Monolithic"},
      {Device::CPU, false, TileSizePreference::MACRO, 50, "CPU Tiled (Macro)"},
      {Device::GPU_METAL, false, TileSizePreference::MACRO, 60,
       "GPU Tiled (Macro)"},
      {Device::CPU, false, TileSizePreference::MICRO, 80, "CPU Tiled (Micro)"},
  };

  // RT 模式优先级表；Macro/Micro 是 RT domain 内的粒度偏好。
  // 小 tile 可以快速返回第一个结果，适合实时预览。
  rt_priority_table_ = {
      {Device::CPU, false, TileSizePreference::MICRO, 10, "CPU Tiled (Micro)"},
      {Device::CPU, true, TileSizePreference::UNDEFINED, 50, "CPU Monolithic"},
      {Device::CPU, false, TileSizePreference::MACRO, 70, "CPU Tiled (Macro)"},
      // GPU 在 RT 模式下不优先（延迟较高）
      {Device::GPU_METAL, true, TileSizePreference::UNDEFINED, 100,
       "GPU Monolithic"},
      {Device::GPU_METAL, false, TileSizePreference::MACRO, 110,
       "GPU Tiled (Macro)"},
  };
}

GpuPipelineScheduler::~GpuPipelineScheduler() {
  shutdown();
}

void GpuPipelineScheduler::attach(GraphRuntime* runtime) {
  runtime_ = runtime;
}

void GpuPipelineScheduler::detach() {
  runtime_ = nullptr;
}

void GpuPipelineScheduler::start() {
  if (running_.load(std::memory_order_acquire)) {
    return;
  }

  // Reset counters
  has_exception_.store(false, std::memory_order_relaxed);
  first_exception_ = nullptr;
  rt_ready_count_.store(0, std::memory_order_relaxed);
  hp_cpu_ready_count_.store(0, std::memory_order_relaxed);
  gpu_ready_count_.store(0, std::memory_order_relaxed);
  sleeping_cpu_count_.store(0, std::memory_order_relaxed);
  sleeping_gpu_count_.store(0, std::memory_order_relaxed);
  tasks_to_complete_.store(0, std::memory_order_relaxed);
  rt_tasks_executed_.store(0, std::memory_order_relaxed);
  hp_cpu_tasks_executed_.store(0, std::memory_order_relaxed);
  gpu_tasks_executed_.store(0, std::memory_order_relaxed);
  total_tasks_scheduled_.store(0, std::memory_order_relaxed);

  running_.store(true, std::memory_order_release);

  // Start CPU workers
  num_cpu_workers_ = config_.cpu_workers;
  cpu_workers_.reserve(num_cpu_workers_);
  for (unsigned int i = 0; i < num_cpu_workers_; ++i) {
    cpu_workers_.emplace_back(&GpuPipelineScheduler::cpu_run_loop, this,
                              static_cast<int>(i));
  }

  // Start GPU workers (if GPU is available)
  if (is_gpu_available()) {
    num_gpu_workers_ = config_.gpu_workers;
    gpu_workers_.reserve(num_gpu_workers_);
    for (unsigned int i = 0; i < num_gpu_workers_; ++i) {
      gpu_workers_.emplace_back(&GpuPipelineScheduler::gpu_run_loop, this,
                                static_cast<int>(num_cpu_workers_ + i));
    }
  }
}

void GpuPipelineScheduler::shutdown() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  running_.store(false, std::memory_order_release);

  // Notify all workers
  rt_cv_.notify_all();
  hp_cpu_cv_.notify_all();
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

  // Drain pending tasks
  {
    std::lock_guard<std::mutex> lock(rt_queue_mutex_);
    while (!rt_queue_.empty()) {
      rt_queue_.pop();
    }
  }
  {
    std::lock_guard<std::mutex> lock(hp_cpu_queue_mutex_);
    while (!hp_cpu_queue_.empty()) {
      hp_cpu_queue_.pop();
    }
  }
  {
    std::lock_guard<std::mutex> lock(gpu_queue_mutex_);
    while (!gpu_queue_.empty()) {
      gpu_queue_.pop();
    }
  }
}

std::string GpuPipelineScheduler::name() const {
  return "GpuPipelineScheduler";
}

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

bool GpuPipelineScheduler::is_running() const {
  return running_.load(std::memory_order_acquire);
}

bool GpuPipelineScheduler::task_runtime_running() const {
  return is_running();
}

void GpuPipelineScheduler::submit_initial_tasks(std::vector<Task>&& tasks,
                                                int total_task_count,
                                                TaskPriority priority) {
  has_exception_.store(false, std::memory_order_relaxed);
  first_exception_ = nullptr;

  uint64_t epoch = begin_new_epoch();
  tasks_to_complete_.store(total_task_count, std::memory_order_relaxed);

  if (total_task_count == 0) {
    std::lock_guard<std::mutex> lk(completion_mutex_);
    cv_completion_.notify_one();
    return;
  }

  if (tasks.empty()) {
    tasks_to_complete_.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(completion_mutex_);
    cv_completion_.notify_one();
    return;
  }

  for (auto& task : tasks) {
    submit_ready_task_any_thread(std::move(task), priority, epoch);
  }
}

void GpuPipelineScheduler::submit_ready_task_from_worker(
    Task&& task, TaskPriority priority) {
  submit_ready_task_any_thread(std::move(task), priority, tls_active_epoch_);
}

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
  if (config_.prefer_gpu_for_hp && is_gpu_available()) {
    submit_gpu_task(std::move(task), resolved_epoch);
  } else {
    submit_hp_task(std::move(task), resolved_epoch);
  }
}

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

int GpuPipelineScheduler::this_worker_id() {
  return tls_worker_id_;
}

uint64_t GpuPipelineScheduler::this_task_epoch() {
  return tls_active_epoch_;
}

uint64_t GpuPipelineScheduler::active_epoch() const {
  return active_epoch_.load(std::memory_order_acquire);
}

uint64_t GpuPipelineScheduler::begin_new_epoch() {
  uint64_t next = epoch_counter_.fetch_add(1, std::memory_order_acq_rel) + 1;
  active_epoch_.store(next, std::memory_order_release);
  cancel_stale_enqueued_tasks(next);
  return next;
}

bool GpuPipelineScheduler::should_cancel_epoch(uint64_t epoch) const {
  if (epoch == 0) {
    return false;
  }
  return epoch < active_epoch();
}

void GpuPipelineScheduler::cancel_stale_enqueued_tasks(uint64_t min_epoch) {
  if (min_epoch == 0) {
    return;
  }

  size_t removed = 0;

  // Purge RT queue
  {
    std::lock_guard<std::mutex> lock(rt_queue_mutex_);
    std::queue<ScheduledTask> kept;
    while (!rt_queue_.empty()) {
      auto task = std::move(rt_queue_.front());
      rt_queue_.pop();
      if (task.epoch != 0 && task.epoch < min_epoch) {
        ++removed;
        continue;
      }
      kept.push(std::move(task));
    }
    rt_queue_.swap(kept);
  }

  // Purge HP CPU queue
  {
    std::lock_guard<std::mutex> lock(hp_cpu_queue_mutex_);
    std::queue<ScheduledTask> kept;
    while (!hp_cpu_queue_.empty()) {
      auto task = std::move(hp_cpu_queue_.front());
      hp_cpu_queue_.pop();
      if (task.epoch != 0 && task.epoch < min_epoch) {
        ++removed;
        continue;
      }
      kept.push(std::move(task));
    }
    hp_cpu_queue_.swap(kept);
  }

  // Purge GPU queue
  {
    std::lock_guard<std::mutex> lock(gpu_queue_mutex_);
    std::queue<ScheduledTask> kept;
    while (!gpu_queue_.empty()) {
      auto task = std::move(gpu_queue_.front());
      gpu_queue_.pop();
      if (task.epoch != 0 && task.epoch < min_epoch) {
        ++removed;
        continue;
      }
      kept.push(std::move(task));
    }
    gpu_queue_.swap(kept);
  }

  if (removed > 0) {
    // Adjust ready counts (conservatively handled by the queue-specific logic
    // above) Note: We can't precisely track which queue the removed tasks came
    // from, so we rely on the per-queue tracking in the purge logic
  }
}

bool GpuPipelineScheduler::is_gpu_available() const {
#ifdef __APPLE__
  return runtime_ && runtime_->get_metal_device() != nullptr;
#else
  return false;
#endif
}

std::vector<Device> GpuPipelineScheduler::get_available_devices() const {
  std::vector<Device> devices;
  devices.push_back(Device::CPU);

  if (is_gpu_available()) {
    devices.push_back(Device::GPU_METAL);
  }

  return devices;
}

// =============================================================================
// CPU 工作线程主循环
// 优先处理 RT 任务，然后处理 HP CPU 任务
// =============================================================================
void GpuPipelineScheduler::cpu_run_loop(int thread_id) {
  tls_worker_id_ = thread_id;
  tls_is_gpu_worker_ = false;

  while (running_.load(std::memory_order_acquire)) {
    // 检查异常状态
    if (has_exception_.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> lock(rt_queue_mutex_);
      rt_cv_.wait(lock, [&] {
        return !has_exception_.load(std::memory_order_acquire) ||
               !running_.load(std::memory_order_acquire);
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
        rt_queue_.pop();
        found_task = true;
        rt_ready_count_.fetch_sub(1, std::memory_order_relaxed);
      }
    }

    // 2) 然后处理 HP CPU 队列
    if (!found_task) {
      std::lock_guard<std::mutex> lock(hp_cpu_queue_mutex_);
      if (!hp_cpu_queue_.empty()) {
        scheduled = std::move(hp_cpu_queue_.front());
        hp_cpu_queue_.pop();
        found_task = true;
        hp_cpu_ready_count_.fetch_sub(1, std::memory_order_relaxed);
      }
    }

    if (found_task) {
      if (should_cancel_epoch(scheduled.epoch)) {
        continue;
      }

      try {
        if (scheduled.task) {
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

          scheduled.task();

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
    } else {
      // 没有任务，进入休眠
      sleeping_cpu_count_.fetch_add(1, std::memory_order_release);

      std::unique_lock<std::mutex> lock(rt_queue_mutex_);
      rt_cv_.wait(lock, [&] {
        return rt_ready_count_.load(std::memory_order_acquire) > 0 ||
               hp_cpu_ready_count_.load(std::memory_order_acquire) > 0 ||
               !running_.load(std::memory_order_acquire) ||
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
void GpuPipelineScheduler::gpu_run_loop(int thread_id) {
  tls_worker_id_ = thread_id;
  tls_is_gpu_worker_ = true;

  while (running_.load(std::memory_order_acquire)) {
    // 检查异常状态
    if (has_exception_.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> lock(gpu_queue_mutex_);
      gpu_cv_.wait(lock, [&] {
        return !has_exception_.load(std::memory_order_acquire) ||
               !running_.load(std::memory_order_acquire);
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
        gpu_queue_.pop();
        found_task = true;
        gpu_ready_count_.fetch_sub(1, std::memory_order_relaxed);
      }
    }

    if (found_task) {
      if (should_cancel_epoch(scheduled.epoch)) {
        continue;
      }

      try {
        if (scheduled.task) {
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

          scheduled.task();
          gpu_tasks_executed_.fetch_add(1, std::memory_order_relaxed);
        } else {
          set_exception(std::make_exception_ptr(std::runtime_error(
              "GpuPipelineScheduler: empty GPU task invoked")));
        }
      } catch (...) {
        set_exception(std::current_exception());
      }
    } else {
      // 没有任务，进入休眠
      sleeping_gpu_count_.fetch_add(1, std::memory_order_release);

      std::unique_lock<std::mutex> lock(gpu_queue_mutex_);
      gpu_cv_.wait(lock, [&] {
        return gpu_ready_count_.load(std::memory_order_acquire) > 0 ||
               !running_.load(std::memory_order_acquire) ||
               has_exception_.load(std::memory_order_acquire);
      });

      sleeping_gpu_count_.fetch_sub(1, std::memory_order_relaxed);
    }
  }
}

// =============================================================================
// 任务提交 API
// =============================================================================

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
    rt_queue_.push(std::move(scheduled));
    rt_ready_count_.fetch_add(1, std::memory_order_relaxed);
  }
  rt_cv_.notify_one();
}

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
    std::lock_guard<std::mutex> lock(hp_cpu_queue_mutex_);
    hp_cpu_queue_.push(std::move(scheduled));
    hp_cpu_ready_count_.fetch_add(1, std::memory_order_relaxed);
  }
  hp_cpu_cv_.notify_one();
  rt_cv_.notify_one();  // CPU workers also handle HP tasks
}

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
    gpu_queue_.push(std::move(scheduled));
    gpu_ready_count_.fetch_add(1, std::memory_order_relaxed);
  }
  gpu_cv_.notify_one();
}

void GpuPipelineScheduler::wait_for_completion() {
  {
    std::unique_lock<std::mutex> lock(completion_mutex_);
    cv_completion_.wait(lock, [&] {
      return tasks_to_complete_.load(std::memory_order_acquire) == 0 ||
             has_exception_.load(std::memory_order_acquire) ||
             !running_.load(std::memory_order_acquire);
    });
  }

  if (has_exception_.load(std::memory_order_relaxed)) {
    std::exception_ptr e;
    {
      std::lock_guard<std::mutex> lock(exception_mutex_);
      e = first_exception_;
      first_exception_ = nullptr;
      has_exception_.store(false, std::memory_order_release);
    }
    rt_cv_.notify_all();
    hp_cpu_cv_.notify_all();
    gpu_cv_.notify_all();
    std::rethrow_exception(e);
  }
}

void GpuPipelineScheduler::dec_tasks_to_complete() {
  uint64_t epoch = tls_active_epoch_;
  if (epoch != 0 && should_cancel_epoch(epoch)) {
    return;
  }
  if (tasks_to_complete_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    std::lock_guard<std::mutex> lk(completion_mutex_);
    cv_completion_.notify_one();
  }
}

void GpuPipelineScheduler::inc_tasks_to_complete(int delta) {
  if (delta <= 0) {
    return;
  }
  uint64_t epoch = tls_active_epoch_;
  if (epoch != 0 && should_cancel_epoch(epoch)) {
    return;
  }
  tasks_to_complete_.fetch_add(delta, std::memory_order_relaxed);
}

void GpuPipelineScheduler::set_exception(std::exception_ptr e) {
  if (!has_exception_.exchange(true, std::memory_order_acq_rel)) {
    std::lock_guard<std::mutex> lock(exception_mutex_);
    first_exception_ = e;

    // Drain all queues
    {
      std::lock_guard<std::mutex> ql(rt_queue_mutex_);
      while (!rt_queue_.empty()) {
        rt_queue_.pop();
      }
      rt_ready_count_.store(0, std::memory_order_relaxed);
    }
    {
      std::lock_guard<std::mutex> ql(hp_cpu_queue_mutex_);
      while (!hp_cpu_queue_.empty()) {
        hp_cpu_queue_.pop();
      }
      hp_cpu_ready_count_.store(0, std::memory_order_relaxed);
    }
    {
      std::lock_guard<std::mutex> ql(gpu_queue_mutex_);
      while (!gpu_queue_.empty()) {
        gpu_queue_.pop();
      }
      gpu_ready_count_.store(0, std::memory_order_relaxed);
    }

    // Notify all workers
    rt_cv_.notify_all();
    hp_cpu_cv_.notify_all();
    gpu_cv_.notify_all();
    {
      std::lock_guard<std::mutex> lk_comp(completion_mutex_);
      cv_completion_.notify_all();
    }
  }
}

}  // namespace ps
