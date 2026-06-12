#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <vector>

namespace ps {

enum class SchedulerTaskPriority { Normal, High };

enum class SchedulerTraceAction {
  AssignInitial,
  Execute,
  ExecuteTile,
  ExecuteDirtySource,
  ExecuteDirtyDownstreamNode,
  ExecuteDirtyDownstreamTile,
  SkipStaleGeneration,
  RethrowException,
};

class SchedulerTaskRuntime {
 public:
  using Task = std::function<void()>;

  virtual ~SchedulerTaskRuntime() = default;

  virtual bool task_runtime_running() const = 0;

  virtual void submit_initial_tasks(
      std::vector<Task>&& tasks, int total_task_count,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) = 0;

  virtual void submit_ready_task_from_worker(
      Task&& task,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) = 0;

  virtual void submit_ready_task_any_thread(
      Task&& task,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) = 0;

  virtual void wait_for_completion() = 0;
  virtual void set_exception(std::exception_ptr e) = 0;
  virtual void inc_tasks_to_complete(int delta) = 0;
  virtual void dec_tasks_to_complete() = 0;
  virtual void log_event(SchedulerTraceAction action, int node_id) = 0;
};

}  // namespace ps
