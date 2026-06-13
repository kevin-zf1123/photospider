// Photospider kernel: GraphRuntime implementation (Objective-C++)
#include "kernel/graph_runtime.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <utility>

#ifdef __APPLE__
#import <Metal/Metal.h>
#endif

namespace ps {

thread_local int GraphRuntime::tls_worker_id_ = -1;
thread_local int GraphRuntime::tls_scheduler_log_worker_id_ = -1;
thread_local uint64_t GraphRuntime::tls_scheduler_log_epoch_ = 0;

struct GraphRuntime::GpuContext {
#ifdef __APPLE__
  id<MTLDevice> device;
  id<MTLCommandQueue> commandQueue;
#endif
};

GraphRuntime::GraphRuntime(const Info& info)
    : info_(info),
      model_(info.cache_root.empty() ? info.root / "cache" : info.cache_root),
      graph_state_(model_) {
  std::filesystem::create_directories(info_.root);
  if (!model_.cache_root.empty()) {
    std::filesystem::create_directories(model_.cache_root);
  }

#ifdef __APPLE__
  gpu_context_ = std::make_unique<GpuContext>();
  gpu_context_->device = MTLCreateSystemDefaultDevice();
  if (gpu_context_->device) {
    gpu_context_->commandQueue = [gpu_context_->device newCommandQueue];
  } else {
    fprintf(stderr, "Warning: Could not create default Metal device.\n");
  }
#else
  // On non-Apple platforms, gpu_context_ remains null
#endif
}

GraphRuntime::~GraphRuntime() {
  stop();
}

int GraphRuntime::this_worker_id() {
  if (tls_worker_id_ >= 0) {
    return tls_worker_id_;
  }
  return tls_scheduler_log_worker_id_;
}

id GraphRuntime::get_metal_device() {
#ifdef __APPLE__
  return gpu_context_ ? gpu_context_->device : nil;
#else
  return nullptr;
#endif
}

id GraphRuntime::get_metal_command_queue() {
#ifdef __APPLE__
  return gpu_context_ ? gpu_context_->commandQueue : nil;
#else
  return nullptr;
#endif
}

void GraphRuntime::start() {
  running_.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lock(schedulers_mutex_);
  for (auto& [intent, scheduler] : schedulers_) {
    (void)intent;
    if (scheduler && !scheduler->is_running()) {
      scheduler->start();
    }
  }
}

void GraphRuntime::stop() {
  running_.store(false, std::memory_order_release);
  std::lock_guard<std::mutex> lock(schedulers_mutex_);
  for (auto& [intent, scheduler] : schedulers_) {
    (void)intent;
    if (scheduler && scheduler->is_running()) {
      scheduler->shutdown();
    }
  }
}

uint64_t GraphRuntime::this_task_epoch() {
  return tls_scheduler_log_epoch_;
}

void GraphRuntime::set_scheduler_log_context(int worker_id, uint64_t epoch) {
  tls_scheduler_log_worker_id_ = worker_id;
  tls_scheduler_log_epoch_ = epoch;
}

void GraphRuntime::clear_scheduler_log_context() {
  tls_scheduler_log_worker_id_ = -1;
  tls_scheduler_log_epoch_ = 0;
}

void GraphRuntime::log_event(SchedulerEvent::Action action, int node_id) {
  uint64_t epoch = this_task_epoch();
  int worker_id = this_worker_id();
  if (worker_id < 0) {
    worker_id = tls_scheduler_log_worker_id_;
  }
  log_event(action, node_id, worker_id, epoch);
}

void GraphRuntime::log_event(SchedulerEvent::Action action, int node_id,
                             int worker_id, uint64_t epoch) {
  std::lock_guard<std::mutex> lock(log_mutex_);
  scheduler_log_.push_back({epoch, node_id, worker_id, action,
                            std::chrono::high_resolution_clock::now()});
}

std::vector<GraphRuntime::SchedulerEvent> GraphRuntime::get_scheduler_log()
    const {
  std::lock_guard<std::mutex> lock(log_mutex_);
  return scheduler_log_;
}

void GraphRuntime::clear_scheduler_log() {
  std::lock_guard<std::mutex> lock(log_mutex_);
  scheduler_log_.clear();
}

// =============================================================================
// [M3.2 新增] 调度器管理实现
// =============================================================================

void GraphRuntime::set_scheduler(ComputeIntent intent,
                                 std::unique_ptr<IScheduler> scheduler) {
  std::lock_guard<std::mutex> lock(schedulers_mutex_);

  // 如果已有调度器，先 detach
  auto it = schedulers_.find(intent);
  if (it != schedulers_.end() && it->second) {
    it->second->detach();
  }

  // 设置新调度器
  schedulers_[intent] = std::move(scheduler);

  // attach 到当前 runtime
  if (schedulers_[intent]) {
    schedulers_[intent]->attach(this);
    if (running_.load(std::memory_order_acquire)) {
      schedulers_[intent]->start();
    }
  }
}

IScheduler* GraphRuntime::get_scheduler(ComputeIntent intent) {
  std::lock_guard<std::mutex> lock(schedulers_mutex_);
  auto it = schedulers_.find(intent);
  return (it != schedulers_.end()) ? it->second.get() : nullptr;
}

const IScheduler* GraphRuntime::get_scheduler(ComputeIntent intent) const {
  std::lock_guard<std::mutex> lock(schedulers_mutex_);
  auto it = schedulers_.find(intent);
  return (it != schedulers_.end()) ? it->second.get() : nullptr;
}

void GraphRuntime::replace_scheduler(ComputeIntent intent,
                                     std::unique_ptr<IScheduler> scheduler) {
  std::lock_guard<std::mutex> lock(schedulers_mutex_);

  auto it = schedulers_.find(intent);
  if (it != schedulers_.end() && it->second) {
    // 停止旧调度器
    it->second->shutdown();
    it->second->detach();
  }

  // 设置新调度器
  schedulers_[intent] = std::move(scheduler);

  if (schedulers_[intent]) {
    // attach 并启动新调度器
    schedulers_[intent]->attach(this);
    if (running_) {
      schedulers_[intent]->start();
    }
  }
}

bool GraphRuntime::has_scheduler(ComputeIntent intent) const {
  std::lock_guard<std::mutex> lock(schedulers_mutex_);
  auto it = schedulers_.find(intent);
  return it != schedulers_.end() && it->second != nullptr;
}

}  // namespace ps
