// Photospider kernel: SerialDebugScheduler implementation
// M3.4: 串行调试调度器，用于调试和单线程执行场景

#include "kernel/scheduler/serial_debug_scheduler.hpp"

#include <sstream>

#include "kernel/graph_runtime.hpp"
#include "kernel/services/compute_service.hpp"
#include "kernel/services/graph_cache_service.hpp"
#include "kernel/services/graph_traversal_service.hpp"

namespace ps {

SerialDebugScheduler::~SerialDebugScheduler() {
  shutdown();
}

void SerialDebugScheduler::attach(GraphRuntime* runtime) {
  runtime_ = runtime;
}

void SerialDebugScheduler::detach() {
  runtime_ = nullptr;
}

void SerialDebugScheduler::start() {
  running_.store(true, std::memory_order_release);
}

void SerialDebugScheduler::shutdown() {
  running_.store(false, std::memory_order_release);
}

std::string SerialDebugScheduler::name() const {
  return "serial_debug";
}

std::string SerialDebugScheduler::get_stats() const {
  std::ostringstream oss;
  oss << "SerialDebugScheduler Stats:\n"
      << "  Running: " << (running_.load() ? "yes" : "no") << "\n"
      << "  Tasks executed: " << tasks_executed_.load();
  return oss.str();
}

bool SerialDebugScheduler::is_running() const {
  return running_.load(std::memory_order_acquire);
}

std::future<NodeOutput> SerialDebugScheduler::schedule(const ComputeOptions& opts) {
  auto promise = std::make_shared<std::promise<NodeOutput>>();
  auto future = promise->get_future();
  
  // 在当前线程同步执行计算
  try {
    if (!runtime_) {
      throw std::runtime_error("SerialDebugScheduler: not attached to runtime");
    }
    
    if (!running_.load(std::memory_order_acquire)) {
      throw std::runtime_error("SerialDebugScheduler: scheduler is not running");
    }
    
    // 使用 runtime 的 post 机制来访问 GraphModel 并执行计算
    auto compute_future = runtime_->post([this, &opts](GraphModel& graph) -> NodeOutput {
      // 构造服务实例
      GraphTraversalService traversal;
      GraphCacheService cache;
      ComputeService compute(traversal, cache, runtime_->event_service());
      
      // 执行串行计算（不使用并行版本）
      if (opts.dirty_roi.has_value()) {
        // 有脏区时使用带 intent 的版本
        NodeOutput& result = compute.compute(
            graph, opts.intent, opts.node_id, opts.cache_precision,
            opts.force_recache, opts.enable_timing, opts.disable_disk_cache,
            nullptr, opts.dirty_roi);
        return result;
      } else {
        // 无脏区时使用基础版本
        NodeOutput& result = compute.compute(
            graph, opts.node_id, opts.cache_precision,
            opts.force_recache, opts.enable_timing, opts.disable_disk_cache,
            nullptr);
        return result;
      }
    });
    
    // 等待计算完成并获取结果
    NodeOutput result = compute_future.get();
    tasks_executed_.fetch_add(1, std::memory_order_relaxed);
    promise->set_value(std::move(result));
    
  } catch (...) {
    promise->set_exception(std::current_exception());
  }
  
  return future;
}

}  // namespace ps
