#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <vector>

#include "photospider/core/device.hpp"

/**
 * @file scheduler_task_runtime.hpp
 * @brief Current task-dispatch contract in the provisional scheduler C++ ABI.
 *
 * Its virtual functions, `std::function`, containers, exception pointers, and
 * borrowed C++ objects require the matching SDK and compatible toolchain and
 * runtime. This header does not define a stable pure C scheduler ABI.
 */

namespace ps {

/**
 * @brief Priority class attached to scheduler-visible ready work.
 *
 * @throws Nothing.
 * @note The fixed representation is shared across the transitional C++ plugin
 *       ABI. It does not prescribe a concrete queue implementation.
 */
enum class SchedulerTaskPriority : std::uint32_t {
  /** @brief Normal planned work. */
  Normal = 0U,

  /** @brief Latency-sensitive planned work. */
  High = 1U,
};

/**
 * @brief Scheduler trace action understood by the public host context.
 *
 * @throws Nothing.
 * @note The host maps these stable labels to its private observation ring; a
 *       plugin never sees private runtime trace types or storage.
 */
enum class SchedulerTraceAction : std::uint32_t {
  /** @brief Publish the first ready set for a batch. */
  AssignInitial = 0U,
  /** @brief Execute monolithic planned work. */
  Execute = 1U,
  /** @brief Execute tiled planned work. */
  ExecuteTile = 2U,
  /** @brief Execute a dirty source node. */
  ExecuteDirtySource = 3U,
  /** @brief Execute a dirty downstream node. */
  ExecuteDirtyDownstreamNode = 4U,
  /** @brief Execute a dirty downstream tile. */
  ExecuteDirtyDownstreamTile = 5U,
  /** @brief Drop work whose epoch is stale. */
  SkipStaleGeneration = 6U,
  /** @brief Publish the first captured task exception. */
  RethrowException = 7U,
};

/**
 * @brief Executor interface borrowed by scheduler task handles.
 *
 * The compute dispatcher owns the executor, dependency graph, and completion
 * state. A scheduler may call `run_task` only while the matching submitted
 * batch is alive and MUST NOT retain the executor after
 * `SchedulerTaskRuntime::wait_for_completion` returns.
 *
 * @throws Submitted task exceptions unchanged from `run_task`.
 * @note Schedulers never own or delete an executor. Protected virtual
 *       destruction prevents a plugin from deleting the dispatcher-owned
 *       object through its borrowed base pointer.
 */
class TaskExecutor {
 protected:
  /**
   * @brief Supports concrete derived destruction by the dispatcher owner.
   * @throws Nothing under the concrete dispatcher lifetime contract.
   * @note Protected access makes deletion through borrowed `TaskExecutor*`
   * ill-formed while preserving ordinary stack or concrete-owner destruction.
   */
  virtual ~TaskExecutor() = default;

 public:
  /**
   * @brief Executes one dense task identifier.
   * @param task_id Dense id owned by the active dispatcher.
   * @return Nothing.
   * @throws Any operation, dependency, or dispatcher exception unchanged.
   * @note Completion accounting remains the executor's responsibility.
   */
  virtual void run_task(int task_id) = 0;
};

/**
 * @brief Non-owning queue entry for one dependency-ready planned task.
 *
 * @throws Nothing for construction and scalar inspection.
 * @note The executor and its graph state remain dispatcher-owned through the
 *       matching wait boundary. An empty handle MUST be ignored.
 */
struct TaskHandle {
  /** @brief Borrowed dispatcher executor, or null for an empty handle. */
  TaskExecutor* executor = nullptr;

  /** @brief Dense executor task id, or -1 for an empty handle. */
  int task_id = -1;

  /** @brief Backend node id used only for trace and diagnostics. */
  int node_id = -1;

  /**
   * @brief Reports whether this handle references runnable work.
   * @return True only when executor and task id are valid.
   * @throws Nothing.
   */
  explicit operator bool() const noexcept {
    return executor != nullptr && task_id >= 0;
  }

  /**
   * @brief Runs this handle through its borrowed executor.
   * @return Nothing.
   * @throws Any exception emitted by `TaskExecutor::run_task` unchanged.
   * @note Calling an empty handle is a no-op.
   */
  void run() const {
    if (executor != nullptr && task_id >= 0) {
      executor->run_task(task_id);
    }
  }
};

/**
 * @brief Minimal scheduler-facing runtime used by production compute dispatch.
 *
 * Implementations own queues, workers, epoch state, completion counters, and
 * first-exception transport. They borrow task handles until the matching wait
 * completes. Initial and worker-ready batches are explicit pure virtual calls
 * so a base implementation cannot silently decompose an atomic batch.
 *
 * @throws Concrete submission, synchronization, allocation, or task
 *         exceptions according to the individual operation contract.
 * @note Lifecycle and identity are supplied by `IScheduler`. This interface
 *       contains only operations used by the production dispatcher. A loaded
 *       plugin owner normalizes scheduler-implementation failures. Before host
 *       task handles, callbacks, and `set_exception` values enter a plugin, a
 *       preallocated owner-side slot registers their `exception_ptr` identity;
 *       those exceptions remain unchanged and plugin-visible, then are
 *       recognized and rethrown exactly when they surface at the host boundary.
 */
class SchedulerTaskRuntime {
 public:
  /**
   * @brief Owned callback accepted by the any-thread submission lane.
   * @throws std::bad_alloc when callable state construction cannot allocate.
   * @note Ownership moves into the concrete scheduler until execution or
   *       retirement.
   */
  using Task = std::function<void()>;

  /**
   * @brief Releases a runtime through its scheduler vtable.
   * @throws Nothing under the implementation destructor contract.
   * @note Concrete objects are normally destroyed through `IScheduler`.
   */
  virtual ~SchedulerTaskRuntime() = default;

  /**
   * @brief Reports device labels routable by this scheduler instance.
   * @return Devices ordered by scheduler preference.
   * @throws std::bad_alloc if result storage cannot be allocated.
   * @note The default CPU-only value preserves simple serial implementations.
   * A loaded plugin owner rejects numeric values outside the fixed public
   * `Device` enumerators before returning the list to host planning.
   */
  virtual std::vector<Device> available_devices() const {
    return {Device::CPU};
  }

  /**
   * @brief Atomically submits the initial ready handle batch.
   * @param handles Ready handles borrowed for the active batch.
   * @param total_task_count Total tasks that must reach completion.
   * @param priority Priority shared by the ready set.
   * @return Nothing.
   * @throws Concrete submission failures directly for host schedulers; a
   * plugin owner normalizes plugin-origin failures and restores exact host task
   * exceptions.
   * @note Failure MUST publish none of the handles or batch state.
   */
  virtual void submit_initial_task_handles(
      std::vector<TaskHandle>&& handles, int total_task_count,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) = 0;

  /**
   * @brief Atomically submits handles released together by a worker.
   * @param handles Newly ready handles from one dependency release.
   * @param priority Priority shared by the handles.
   * @return Nothing.
   * @throws Concrete submission failures directly for host schedulers; a
   * plugin owner normalizes plugin-origin failures and restores exact host task
   * exceptions.
   * @note Failure MUST enqueue none of the supplied handles.
   */
  virtual void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) = 0;

  /**
   * @brief Submits one callback from any caller thread.
   * @param task Callback whose state moves into the scheduler.
   * @param priority Requested priority.
   * @param epoch Optional batch epoch; zero remains an uncancellable task.
   * @return Nothing.
   * @throws Concrete submission failures directly for host schedulers; a
   * plugin owner normalizes plugin-origin failures and restores exact host task
   * exceptions.
   * @note The scheduler owns the moved callback until execution or retirement.
   */
  virtual void submit_ready_task_any_thread(
      Task&& task,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal,
      std::optional<std::uint64_t> epoch = std::nullopt) = 0;

  /**
   * @brief Waits for the active batch and rethrows its first task exception.
   * @return Nothing.
   * @throws The exact first captured task exception.
   * @note Return or throw ends the borrowing interval for submitted handles.
   */
  virtual void wait_for_completion() = 0;

  /**
   * @brief Publishes one task exception into first-exception transport.
   * @param error Exception identity to retain; null is ignored and clears no
   *        existing error.
   * @return Nothing.
   * @throws Concrete synchronization or test-injection exceptions; a loaded
   * plugin owner normalizes plugin-origin failures.
   * @note The first accepted non-null exception remains authoritative for the
   *       active batch; null input mutates no queue, epoch, or exception state.
   */
  virtual void set_exception(std::exception_ptr error) = 0;

  /**
   * @brief Adds dynamically created micro-tasks to completion accounting.
   * @param delta Positive task-count increment.
   * @return Nothing.
   * @throws Concrete validation or synchronization exceptions; a loaded plugin
   * owner normalizes plugin-origin failures.
   * @note Nonpositive-delta handling is defined by the concrete scheduler and
   *       must preserve counter integrity.
   */
  virtual void inc_tasks_to_complete(int delta) = 0;

  /**
   * @brief Marks one active task complete.
   * @return Nothing.
   * @throws Concrete validation or synchronization exceptions; a loaded plugin
   * owner normalizes plugin-origin failures.
   * @note Completion accounting must never wrap below its settled floor.
   */
  virtual void dec_tasks_to_complete() = 0;

  /**
   * @brief Publishes a scheduler trace for the current worker context.
   * @param action Stable scheduler action.
   * @param node_id Backend node id, or -1 when unavailable.
   * @return Nothing.
   * @throws Nothing under the public host-context contract.
   * @note Worker and epoch attribution come from the current scheduler task
   *       context.
   */
  virtual void log_event(SchedulerTraceAction action, int node_id) = 0;
};

}  // namespace ps
