// Photospider kernel: SerialDebugScheduler
// M3.4: 串行调试调度器，用于调试和单线程执行场景
#pragma once

#include <atomic>
#include <future>
#include <memory>
#include <string>

#include "kernel/scheduler/i_scheduler.hpp"

namespace ps {

class GraphRuntime;
class GraphTraversalService;
class GraphCacheService;
class ComputeService;

// =============================================================================
// SerialDebugScheduler: 串行调试调度器
// 所有任务在当前线程顺序执行，便于调试和分析执行流程
// =============================================================================
class SerialDebugScheduler : public IScheduler {
 public:
  SerialDebugScheduler() = default;
  ~SerialDebugScheduler() override;

  // 禁用拷贝
  SerialDebugScheduler(const SerialDebugScheduler&) = delete;
  SerialDebugScheduler& operator=(const SerialDebugScheduler&) = delete;

  // ---------------------------------------------------------------------------
  // IScheduler 接口实现
  // ---------------------------------------------------------------------------
  void attach(GraphRuntime* runtime) override;
  void detach() override;
  void start() override;
  void shutdown() override;
  std::future<NodeOutput> schedule(const ComputeOptions& opts) override;
  std::string name() const override;
  std::string get_stats() const override;
  bool is_running() const override;

 private:
  GraphRuntime* runtime_{nullptr};
  std::atomic<bool> running_{false};
  
  // 统计信息
  std::atomic<uint64_t> tasks_executed_{0};
};

}  // namespace ps
