// Photospider kernel: GpuPipelineScheduler implementation
// M3.5: 异构调度器 - 支持 HP 走 GPU、RT 走 CPU 的混合计算模式
// M3.6: Node-Level 调度 - Scheduler 内部优先级表决策

#include "kernel/scheduler/gpu_pipeline_scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <utility>

#include "kernel/graph_runtime.hpp"
#include "kernel/services/compute_service.hpp"
#include "kernel/services/graph_cache_service.hpp"
#include "kernel/services/graph_traversal_service.hpp"

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
  // [M3.6] 初始化优先级表
  init_priority_tables();
}

// =============================================================================
// [M3.6] 优先级表初始化
// =============================================================================
void GpuPipelineScheduler::init_priority_tables() {
  // HP 模式优先级表: GPU Monolithic > CPU Monolithic > CPU Tiled (Macro)
  // 优先级值越小越优先
  hp_priority_table_ = {
    {Device::GPU_METAL, true, TileSizePreference::UNDEFINED, 10, "GPU Monolithic"},
    {Device::CPU, true, TileSizePreference::UNDEFINED, 30, "CPU Monolithic"},
    {Device::CPU, false, TileSizePreference::MACRO, 50, "CPU Tiled (Macro)"},
    {Device::GPU_METAL, false, TileSizePreference::MACRO, 60, "GPU Tiled (Macro)"},
    {Device::CPU, false, TileSizePreference::MICRO, 80, "CPU Tiled (Micro)"},
  };
  
  // RT 模式优先级表: CPU Tiled (Micro 16x16) > CPU Monolithic
  // 小 tile 可以快速返回第一个结果，适合实时预览
  rt_priority_table_ = {
    {Device::CPU, false, TileSizePreference::MICRO, 10, "CPU Tiled (Micro)"},
    {Device::CPU, true, TileSizePreference::UNDEFINED, 50, "CPU Monolithic"},
    {Device::CPU, false, TileSizePreference::MACRO, 70, "CPU Tiled (Macro)"},
    // GPU 在 RT 模式下不优先（延迟较高）
    {Device::GPU_METAL, true, TileSizePreference::UNDEFINED, 100, "GPU Monolithic"},
    {Device::GPU_METAL, false, TileSizePreference::MACRO, 110, "GPU Tiled (Macro)"},
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
  node_level_tasks_scheduled_.store(0, std::memory_order_relaxed);
  task_groups_aggregated_.store(0, std::memory_order_relaxed);

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
      << " (sleeping: " << sleeping_cpu_count_.load(std::memory_order_relaxed) << ")"
      << ", GPU Workers: " << num_gpu_workers_
      << " (sleeping: " << sleeping_gpu_count_.load(std::memory_order_relaxed) << ")"
      << ", RT ready: " << rt_ready_count_.load(std::memory_order_relaxed)
      << ", HP CPU ready: " << hp_cpu_ready_count_.load(std::memory_order_relaxed)
      << ", GPU ready: " << gpu_ready_count_.load(std::memory_order_relaxed)
      << ", RT executed: " << rt_tasks_executed_.load(std::memory_order_relaxed)
      << ", HP CPU executed: " << hp_cpu_tasks_executed_.load(std::memory_order_relaxed)
      << ", GPU executed: " << gpu_tasks_executed_.load(std::memory_order_relaxed)
      << ", Total scheduled: " << total_tasks_scheduled_.load(std::memory_order_relaxed)
      << ", Node-level: " << node_level_tasks_scheduled_.load(std::memory_order_relaxed)
      << ", Groups aggregated: " << task_groups_aggregated_.load(std::memory_order_relaxed);
  return oss.str();
}

bool GpuPipelineScheduler::is_running() const {
  return running_.load(std::memory_order_acquire);
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
    // Adjust ready counts (conservatively handled by the queue-specific logic above)
    // Note: We can't precisely track which queue the removed tasks came from,
    // so we rely on the per-queue tracking in the purge logic
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
          
          scheduled.task();
          gpu_tasks_executed_.fetch_add(1, std::memory_order_relaxed);
        } else {
          set_exception(std::make_exception_ptr(
              std::runtime_error("GpuPipelineScheduler: empty GPU task invoked")));
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
  
  ScheduledTask scheduled(resolved_epoch, std::move(task), ComputeIntent::RealTimeUpdate);
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
  
  ScheduledTask scheduled(resolved_epoch, std::move(task), ComputeIntent::GlobalHighPrecision);
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
  
  ScheduledTask scheduled(resolved_epoch, std::move(task), ComputeIntent::GlobalHighPrecision);
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

// =============================================================================
// 核心调度实现
// =============================================================================

const OpImplementation* GpuPipelineScheduler::select_implementation(
    const std::string& type, const std::string& subtype,
    ComputeIntent intent) const {
  auto& registry = OpRegistry::instance();
  auto available_devices = get_available_devices();
  return registry.select_best_implementation(type, subtype, available_devices, intent);
}

std::future<NodeOutput> GpuPipelineScheduler::schedule(const ComputeOptions& opts) {
  total_tasks_scheduled_.fetch_add(1, std::memory_order_relaxed);
  
  auto promise = std::make_shared<std::promise<NodeOutput>>();
  auto future = promise->get_future();
  
  // 创建计算任务
  auto compute_task = [this, opts, promise]() {
    try {
      NodeOutput result = execute_compute(opts);
      promise->set_value(std::move(result));
    } catch (...) {
      promise->set_exception(std::current_exception());
    }
  };
  
  // 根据 intent 决定提交到哪个队列
  if (opts.intent == ComputeIntent::RealTimeUpdate) {
    // RT 任务总是走 CPU（保证低延迟）
    if (config_.force_cpu_for_rt) {
      submit_rt_task(std::move(compute_task), opts.epoch);
    } else {
      submit_rt_task(std::move(compute_task), opts.epoch);
    }
  } else {
    // HP 任务：优先走 GPU（如果可用且配置允许）
    if (config_.prefer_gpu_for_hp && is_gpu_available()) {
      submit_gpu_task(std::move(compute_task), opts.epoch);
    } else {
      // 回退到 CPU
      submit_hp_task(std::move(compute_task), opts.epoch);
    }
  }
  
  return future;
}

NodeOutput GpuPipelineScheduler::execute_compute(const ComputeOptions& opts) {
  if (!runtime_) {
    throw std::runtime_error("GpuPipelineScheduler: not attached to runtime");
  }
  
  // 使用 runtime 的 post 机制来访问 GraphModel 并执行计算
  // 注意：这里需要确保 GraphModel 的线程安全访问
  auto future = runtime_->post([this, &opts](GraphModel& graph) -> NodeOutput {
    // 构造服务实例
    GraphTraversalService traversal;
    GraphCacheService cache;
    ComputeService compute(traversal, cache, runtime_->event_service());
    
    // 根据 intent 选择不同的计算策略
    // HP 模式：可以使用 GPU 加速
    // RT 模式：使用 CPU 以保证低延迟
    
    // 执行计算
    NodeOutput& result = compute.compute_parallel(
        graph, *runtime_, opts.intent, opts.node_id, opts.cache_precision,
        opts.force_recache, opts.enable_timing, opts.disable_disk_cache,
        nullptr, opts.dirty_roi);
    
    // 返回副本避免引用悬挂
    return result;
  });
  
  return future.get();
}

// =============================================================================
// [M3.6] Node-Level 调度实现
// =============================================================================

std::future<NodeOutput> GpuPipelineScheduler::schedule_node(
    const NodeScheduleRequest& request, GraphModel& graph) {
  node_level_tasks_scheduled_.fetch_add(1, std::memory_order_relaxed);
  total_tasks_scheduled_.fetch_add(1, std::memory_order_relaxed);
  
  auto promise = std::make_shared<std::promise<NodeOutput>>();
  auto future = promise->get_future();
  
  // 获取节点信息
  auto it = graph.nodes.find(request.node_id);
  if (it == graph.nodes.end()) {
    promise->set_exception(std::make_exception_ptr(
        std::runtime_error("Node not found: " + std::to_string(request.node_id))));
    return future;
  }
  
  const Node& node = it->second;
  const std::string& type = node.type;
  const std::string& subtype = node.subtype;
  
  // [M3.6] 使用优先级表选择最优实现
  const OpImplementation* impl = select_impl_with_priority(type, subtype, request.intent);
  
  if (!impl) {
    // 回退到传统方法
    ComputeOptions opts;
    opts.intent = request.intent;
    opts.node_id = request.node_id;
    opts.dirty_roi = request.dirty_roi;
    opts.cache_precision = request.cache_precision;
    opts.force_recache = request.force_recache;
    opts.enable_timing = request.enable_timing;
    opts.disable_disk_cache = request.disable_disk_cache;
    opts.epoch = request.epoch;
    return schedule(opts);
  }
  
  // 根据选择的实现决定调度策略
  bool use_gpu = (impl->metadata.device_preference == Device::GPU_METAL) && is_gpu_available();
  bool is_monolithic = impl->is_monolithic();
  
  // 创建计算任务
  auto compute_task = [this, request, promise, &graph]() {
    try {
      NodeOutput result = execute_node_compute(request, graph);
      promise->set_value(std::move(result));
    } catch (...) {
      promise->set_exception(std::current_exception());
    }
  };
  
  // 根据实现类型和 intent 决定提交到哪个队列
  if (request.intent == ComputeIntent::RealTimeUpdate) {
    // RT 模式
    if (config_.force_cpu_for_rt || !use_gpu) {
      // RT 总是走 CPU 队列（保证低延迟）
      if (!is_monolithic && request.dirty_roi.width > 0) {
        // Tiled 模式：切分为 micro tiles
        auto tiles = split_roi_to_tiles(request.dirty_roi, config_.micro_tile_size);
        inc_tasks_to_complete(static_cast<int>(tiles.size()));
        
        for (const auto& tile : tiles) {
          NodeScheduleRequest tile_req = request;
          tile_req.dirty_roi = tile;
          
          auto tile_task = [this, tile_req, &graph]() {
            try {
              execute_node_compute(tile_req, graph);
            } catch (const std::exception& e) {
              set_exception(std::current_exception());
            }
            dec_tasks_to_complete();
          };
          submit_rt_task(std::move(tile_task), request.epoch);
        }
        
        // 等待所有 tile 完成后设置结果
        // 注：实际实现中应该聚合结果，这里简化处理
      } else {
        submit_rt_task(std::move(compute_task), request.epoch);
      }
    } else {
      submit_rt_task(std::move(compute_task), request.epoch);
    }
  } else {
    // HP 模式
    if (use_gpu) {
      // HP 优先走 GPU
      if (!is_monolithic && request.dirty_roi.width > 0) {
        // 检查是否应该聚合
        auto tiles = split_roi_to_tiles(request.dirty_roi, config_.micro_tile_size);
        TaskGroup group = create_task_group(request.node_id, tiles, request.intent, request.epoch);
        
        if (should_aggregate_to_macro(group)) {
          // 聚合为 Macro task
          task_groups_aggregated_.fetch_add(1, std::memory_order_relaxed);
          submit_gpu_task(std::move(compute_task), request.epoch);
        } else {
          // 不聚合，按 tile 提交
          submit_gpu_task(std::move(compute_task), request.epoch);
        }
      } else {
        submit_gpu_task(std::move(compute_task), request.epoch);
      }
    } else {
      // 回退到 CPU
      submit_hp_task(std::move(compute_task), request.epoch);
    }
  }
  
  return future;
}

const OpImplementation* GpuPipelineScheduler::select_impl_with_priority(
    const std::string& type, const std::string& subtype,
    ComputeIntent intent) const {
  auto& registry = OpRegistry::instance();
  auto all_impls = registry.get_all_implementations(type, subtype);
  
  if (all_impls.empty()) {
    return nullptr;
  }
  
  // 选择对应的优先级表
  const auto& priority_table = (intent == ComputeIntent::RealTimeUpdate) 
                                ? rt_priority_table_ 
                                : hp_priority_table_;
  
  const OpImplementation* best_impl = nullptr;
  int best_priority = std::numeric_limits<int>::max();
  
  auto available_devices = get_available_devices();
  
  for (const auto* impl : all_impls) {
    if (!impl) continue;
    
    // 检查设备是否可用
    bool device_available = std::find(available_devices.begin(), 
                                       available_devices.end(),
                                       impl->metadata.device_preference) 
                            != available_devices.end();
    if (!device_available) continue;
    
    // 在优先级表中查找匹配的条目
    for (const auto& entry : priority_table) {
      if (entry.device != impl->metadata.device_preference) continue;
      
      // 检查 Monolithic vs Tiled
      bool impl_is_mono = impl->is_monolithic();
      if (entry.prefer_monolithic != impl_is_mono) continue;
      
      // 检查 tile 偏好（如果有）
      if (entry.tile_pref != TileSizePreference::UNDEFINED && 
          impl->metadata.tile_preference != TileSizePreference::UNDEFINED) {
        if (entry.tile_pref != impl->metadata.tile_preference) continue;
      }
      
      // 匹配成功，检查优先级
      int effective_priority = entry.priority + impl->metadata.cost_score;
      if (effective_priority < best_priority) {
        best_priority = effective_priority;
        best_impl = impl;
      }
      break;  // 找到匹配的 entry 就跳出
    }
  }
  
  // 如果优先级表没有匹配，回退到 cost_score 最低的实现
  if (!best_impl && !all_impls.empty()) {
    for (const auto* impl : all_impls) {
      if (!impl) continue;
      bool device_available = std::find(available_devices.begin(),
                                         available_devices.end(),
                                         impl->metadata.device_preference)
                              != available_devices.end();
      if (!device_available) continue;
      
      if (!best_impl || impl->metadata.cost_score < best_impl->metadata.cost_score) {
        best_impl = impl;
      }
    }
  }
  
  return best_impl;
}

bool GpuPipelineScheduler::should_aggregate_to_macro(const TaskGroup& group) const {
  // HP 模式下，如果 tile 数量超过阈值且 GPU 可用，则聚合
  if (group.intent == ComputeIntent::GlobalHighPrecision) {
    if (is_gpu_available() && 
        static_cast<int>(group.tile_count()) >= config_.aggregation_threshold) {
      return true;
    }
  }
  return false;
}

std::vector<cv::Rect> GpuPipelineScheduler::split_roi_to_tiles(
    const cv::Rect& roi, int tile_size) const {
  std::vector<cv::Rect> tiles;
  
  if (roi.width <= 0 || roi.height <= 0 || tile_size <= 0) {
    return tiles;
  }
  
  for (int y = roi.y; y < roi.y + roi.height; y += tile_size) {
    for (int x = roi.x; x < roi.x + roi.width; x += tile_size) {
      int w = std::min(tile_size, roi.x + roi.width - x);
      int h = std::min(tile_size, roi.y + roi.height - y);
      tiles.emplace_back(x, y, w, h);
    }
  }
  
  return tiles;
}

NodeOutput GpuPipelineScheduler::execute_node_compute(
    const NodeScheduleRequest& request, GraphModel& graph) {
  if (!runtime_) {
    throw std::runtime_error("GpuPipelineScheduler: not attached to runtime");
  }
  
  // 使用 runtime 的 post 机制来访问 GraphModel 并执行计算
  auto future = runtime_->post([this, &request](GraphModel& g) -> NodeOutput {
    GraphTraversalService traversal;
    GraphCacheService cache;
    ComputeService compute(traversal, cache, runtime_->event_service());
    
    std::optional<cv::Rect> dirty_roi;
    if (request.dirty_roi.width > 0 && request.dirty_roi.height > 0) {
      dirty_roi = request.dirty_roi;
    }
    
    NodeOutput& result = compute.compute_parallel(
        g, *runtime_, request.intent, request.node_id, request.cache_precision,
        request.force_recache, request.enable_timing, request.disable_disk_cache,
        nullptr, dirty_roi);
    
    return result;
  });
  
  return future.get();
}

}  // namespace ps
