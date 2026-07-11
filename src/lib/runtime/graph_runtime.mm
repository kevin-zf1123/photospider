// Photospider kernel: GraphRuntime implementation (Objective-C++)
#include "kernel/graph_runtime.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include "kernel/services/compute-service/realtime_proxy_graph.hpp"

#ifdef __APPLE__
#import <Metal/Metal.h>
#endif

namespace ps {
namespace {

/**
 * @brief Runs explicit scheduler shutdown and detach as a best-effort sweep.
 *
 * Shutdown is attempted before detach. Each stage has an independent exception
 * fence so a hostile shutdown cannot prevent detach.
 *
 * @param scheduler Scheduler whose lifecycle ownership is being rolled back or
 * released; null is accepted as an already-clean owner.
 * @return The exact first lifecycle exception, or an empty pointer on success.
 * @throws Nothing; lifecycle failures are returned to the caller.
 * @note The helper neither destroys nor publishes the scheduler owner.
 */
std::exception_ptr cleanup_scheduler_lifecycle(IScheduler* scheduler) noexcept {
  if (!scheduler) {
    return nullptr;
  }

  std::exception_ptr first_error;
  try {
    scheduler->shutdown();
  } catch (...) {
    first_error = std::current_exception();
  }
  try {
    scheduler->detach();
  } catch (...) {
    if (!first_error) {
      first_error = std::current_exception();
    }
  }
  return first_error;
}

}  // namespace

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
      graph_state_(model_),
      realtime_proxy_graph_(std::make_unique<compute::RealtimeProxyGraph>()) {
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

GraphRuntime::~GraphRuntime() noexcept {
  try {
    stop();
  } catch (...) {
    // Scheduler owners and built-in destructors retain no-throw fallback
    // cleanup; a hostile explicit plugin lifecycle call cannot escape here.
  }
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

compute::RealtimeProxyGraph& GraphRuntime::realtime_proxy_graph() {
  return *realtime_proxy_graph_;
}

/** @copydoc GraphRuntime::start */
void GraphRuntime::start() {
  std::lock_guard<std::mutex> lock(schedulers_mutex_);
  if (running_.load(std::memory_order_acquire)) {
    return;
  }

  std::vector<IScheduler*> started_schedulers;
  started_schedulers.reserve(schedulers_.size());
  try {
    for (auto& [intent, scheduler] : schedulers_) {
      (void)intent;
      if (scheduler && !scheduler->is_running()) {
        started_schedulers.push_back(scheduler.get());
        scheduler->start();
      }
    }
  } catch (...) {
    const std::exception_ptr start_error = std::current_exception();
    for (auto it = started_schedulers.rbegin(); it != started_schedulers.rend();
         ++it) {
      try {
        if (*it) {
          (*it)->shutdown();
        }
      } catch (...) {
      }
    }
    running_.store(false, std::memory_order_release);
    std::rethrow_exception(start_error);
  }
  running_.store(true, std::memory_order_release);
}

/** @copydoc GraphRuntime::stop */
void GraphRuntime::stop() {
  std::lock_guard<std::mutex> lock(schedulers_mutex_);
  running_.store(false, std::memory_order_release);
  std::exception_ptr first_stop_error;
  for (auto& [intent, scheduler] : schedulers_) {
    (void)intent;
    if (!scheduler) {
      continue;
    }

    bool shutdown_required = true;
    try {
      shutdown_required = scheduler->is_running();
    } catch (...) {
      if (!first_stop_error) {
        first_stop_error = std::current_exception();
      }
    }

    if (!shutdown_required) {
      continue;
    }

    try {
      scheduler->shutdown();
    } catch (...) {
      if (!first_stop_error) {
        first_stop_error = std::current_exception();
      }
    }
  }
  if (first_stop_error) {
    std::rethrow_exception(first_stop_error);
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
    const {  // NOLINT(whitespace/indent_namespace)
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

/** @copydoc GraphRuntime::set_scheduler */
void GraphRuntime::set_scheduler(ComputeIntent intent,
                                 std::unique_ptr<IScheduler> scheduler) {
  replace_scheduler(intent, std::move(scheduler));
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

/** @copydoc GraphRuntime::replace_scheduler */
void GraphRuntime::replace_scheduler(ComputeIntent intent,
                                     std::unique_ptr<IScheduler> scheduler) {
  std::lock_guard<std::mutex> lock(schedulers_mutex_);

  // Reserve any new map node before candidate lifecycle calls. Once this
  // succeeds, publication into the existing unique_ptr slot cannot allocate.
  auto [slot, inserted] = schedulers_.try_emplace(intent, nullptr);
  try {
    if (scheduler) {
      scheduler->attach(this);
      if (running_.load(std::memory_order_acquire)) {
        scheduler->start();
      }
    }
  } catch (...) {
    const std::exception_ptr candidate_error = std::current_exception();
    (void)cleanup_scheduler_lifecycle(scheduler.get());
    if (inserted) {
      schedulers_.erase(slot);
    }
    std::rethrow_exception(candidate_error);
  }

  // Candidate preparation is complete. Swap publishes it without allocation
  // or ownership destruction; scheduler now owns the previous map value.
  slot->second.swap(scheduler);

  const std::exception_ptr old_cleanup_error =
      cleanup_scheduler_lifecycle(scheduler.get());
  scheduler.reset();
  if (old_cleanup_error) {
    std::rethrow_exception(old_cleanup_error);
  }
}

bool GraphRuntime::has_scheduler(ComputeIntent intent) const {
  std::lock_guard<std::mutex> lock(schedulers_mutex_);
  auto it = schedulers_.find(intent);
  return it != schedulers_.end() && it->second != nullptr;
}

}  // namespace ps
