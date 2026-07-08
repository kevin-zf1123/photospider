#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <vector>

#include "ps_types.hpp"  // NOLINT(build/include_subdir)

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

/**
 * @brief Executor vtable for scheduler-visible task handles.
 *
 * TaskExecutor is owned by the dispatcher that also owns dependency state and
 * task graph metadata for the active submission. Schedulers borrow the pointer
 * only while the submitted batch is alive and must not retain handles after the
 * corresponding wait_for_completion() returns.
 *
 * @note The scheduler invokes run_task() after epoch checks. Dependency release
 * and completion accounting remain the executor's responsibility.
 */
class TaskExecutor {
 public:
  /** @brief Allows derived dispatcher executors to clean up through vtable. */
  virtual ~TaskExecutor() = default;

  /**
   * @brief Executes one task id from the dispatcher's immutable task graph.
   *
   * @param task_id Dense task id selected by the dispatcher.
   * @throws Any operation, dependency, or dispatcher exception for scheduler
   * exception transport.
   * @note Implementations must call SchedulerTaskRuntime::dec_tasks_to_complete
   * exactly once for successfully started non-cancelled tasks.
   */
  virtual void run_task(int task_id) = 0;
};

/**
 * @brief Compact scheduler queue entry for ready planned work.
 *
 * TaskHandle replaces per-ready-task closures on the compute dispatcher path.
 * It carries the borrowed executor pointer, dense task id, and diagnostic node
 * id used by tests and fallback wrappers. The handle owns no graph, plan, or
 * buffer memory.
 *
 * @note A default-constructed handle is empty and must be ignored by
 * schedulers.
 */
struct TaskHandle {
  /** @brief Borrowed executor that owns task graph and dependency state. */
  TaskExecutor* executor = nullptr;

  /** @brief Dense task id to pass to TaskExecutor::run_task(). */
  int task_id = -1;

  /** @brief Graph node id for diagnostics; not used for dependency release. */
  int node_id = -1;

  /** @brief Returns true when the handle can be executed. */
  explicit operator bool() const { return executor != nullptr && task_id >= 0; }

  /**
   * @brief Runs the referenced task through its dispatcher executor.
   *
   * @throws GraphError or standard exceptions propagated by the executor.
   * @note Schedulers call this after epoch and cancellation policy checks.
   */
  void run() const {
    if (executor) {
      executor->run_task(task_id);
    }
  }
};

class SchedulerTaskRuntime {
 public:
  using Task = std::function<void()>;

  virtual ~SchedulerTaskRuntime() = default;

  virtual bool task_runtime_running() const = 0;

  /**
   * @brief Reports devices usable by tasks submitted to this runtime.
   *
   * @return Device list ordered by runtime preference. The base scheduler
   * runtime exposes CPU only, which preserves existing schedulers that do not
   * manage accelerator resources.
   * @throws std::bad_alloc if vector allocation fails.
   * @note Compute task submission uses this list when selecting registered
   * per-device operation implementations. Concrete heterogeneous schedulers
   * should override it when accelerator availability is runtime-dependent.
   */
  virtual std::vector<Device> available_devices() const {
    return {Device::CPU};
  }

  virtual void submit_initial_tasks(
      std::vector<Task>&& tasks, int total_task_count,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) = 0;

  /**
   * @brief Submits initial ready task handles for a new dependency batch.
   *
   * @param handles Ready task handles owned by the active dispatcher.
   * @param total_task_count Number of active tasks that must complete.
   * @param priority Scheduler priority for the initial ready set.
   * @throws Exceptions from concrete scheduler submission.
   * @note The default adapter preserves plugin compatibility by wrapping
   * handles in closures; built-in schedulers override this to queue handles
   * directly.
   */
  virtual void submit_initial_task_handles(
      std::vector<TaskHandle>&& handles, int total_task_count,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) {
    std::vector<Task> tasks;
    tasks.reserve(handles.size());
    for (TaskHandle handle : handles) {
      tasks.push_back([handle]() { handle.run(); });
    }
    submit_initial_tasks(std::move(tasks), total_task_count, priority);
  }

  virtual void submit_ready_task_from_worker(
      Task&& task,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) = 0;

  /**
   * @brief Submits one dependency-ready task handle from a worker thread.
   *
   * @param handle Ready task handle released by dispatcher dependency state.
   * @param priority Scheduler priority for the ready task.
   * @throws Exceptions from concrete scheduler submission.
   * @note The default adapter wraps the handle for plugin compatibility.
   */
  virtual void submit_ready_task_handle_from_worker(
      TaskHandle handle,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) {
    submit_ready_task_from_worker([handle]() { handle.run(); }, priority);
  }

  /**
   * @brief Submits a batch of ready task handles from a worker thread.
   *
   * @param handles Ready handles released together by dependency state.
   * @param priority Scheduler priority for the batch.
   * @throws Exceptions from concrete scheduler submission.
   * @note Built-in schedulers override this to reduce lock and notify churn.
   */
  virtual void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) {
    for (TaskHandle handle : handles) {
      submit_ready_task_handle_from_worker(handle, priority);
    }
  }

  virtual void submit_ready_task_any_thread(
      Task&& task,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) = 0;

  /**
   * @brief Submits one ready task handle from any caller thread.
   *
   * @param handle Ready task handle released outside a scheduler worker.
   * @param priority Scheduler priority for the ready task.
   * @param epoch Optional epoch to attach to the submitted handle.
   * @throws Exceptions from concrete scheduler submission.
   * @note The default adapter wraps the handle for plugin compatibility.
   */
  virtual void submit_ready_task_handle_any_thread(
      TaskHandle handle,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) {
    submit_ready_task_any_thread([handle]() { handle.run(); }, priority, epoch);
  }

  /**
   * @brief Submits a batch of ready task handles from any caller thread.
   *
   * @param handles Ready handles to enqueue.
   * @param priority Scheduler priority shared by the batch.
   * @param epoch Optional epoch to attach to all submitted handles.
   * @throws Exceptions from concrete scheduler submission.
   * @note Built-in schedulers override this to acquire queue locks once.
   */
  virtual void submit_ready_task_handles_any_thread(
      std::vector<TaskHandle>&& handles,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) {
    for (TaskHandle handle : handles) {
      submit_ready_task_handle_any_thread(handle, priority, epoch);
    }
  }

  virtual void wait_for_completion() = 0;
  virtual void set_exception(std::exception_ptr e) = 0;
  virtual void inc_tasks_to_complete(int delta) = 0;
  virtual void dec_tasks_to_complete() = 0;
  virtual void log_event(SchedulerTraceAction action, int node_id) = 0;
};

}  // namespace ps
