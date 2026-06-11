// Photospider kernel: SerialDebugScheduler
// M3.4: 串行调试调度器，用于调试和单线程执行场景
#pragma once

#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kernel/scheduler/i_scheduler.hpp"
#include "kernel/scheduler/scheduler_task_runtime.hpp"

namespace ps {

class GraphRuntime;

// =============================================================================
// SerialDebugScheduler: 串行调试调度器
// 所有任务在当前线程顺序执行，便于调试和分析执行流程
// =============================================================================
class SerialDebugScheduler : public IScheduler, public SchedulerTaskRuntime {
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
  std::string name() const override;
  std::string get_stats() const override;
  bool is_running() const override;
  bool task_runtime_running() const override;

  using Task = SchedulerTaskRuntime::Task;
  using TaskPriority = SchedulerTaskPriority;

  void submit_initial_tasks(
      std::vector<Task>&& tasks, int total_task_count,
      TaskPriority priority = TaskPriority::Normal) override;
  void submit_ready_task_from_worker(
      Task&& task, TaskPriority priority = TaskPriority::Normal) override;
  void submit_ready_task_any_thread(
      Task&& task, TaskPriority priority = TaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override;
  void wait_for_completion() override;
  void set_exception(std::exception_ptr e) override;
  void inc_tasks_to_complete(int delta) override;
  void dec_tasks_to_complete() override;
  void log_event(SchedulerTraceAction action, int node_id) override;

 private:
  GraphRuntime* runtime_{nullptr};
  std::atomic<bool> running_{false};
  int inline_tasks_to_complete_{0};
  std::exception_ptr inline_exception_;

  // 统计信息
  std::atomic<uint64_t> tasks_executed_{0};
};

}  // namespace ps
