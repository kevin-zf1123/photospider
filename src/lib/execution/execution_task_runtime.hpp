#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <vector>

#include "photospider/core/device.hpp"

/**
 * @file execution_task_runtime.hpp
 * @brief Private task-dispatch and observation contract for physical routes.
 *
 * This source-tree-only header may use C++ values because it never crosses the
 * installed Policy ABI. Repository code owns every implementation, worker,
 * queue, callback, and resource transition.
 */

namespace ps {

/**
 * @brief Priority class attached to execution-visible ready work.
 *
 * @throws Nothing.
 * @note This private hint establishes neither readiness nor resource authority.
 */
enum class ExecutionTaskPriority : std::uint32_t {
  /** @brief Normal planned work. */
  Normal = 0U,

  /** @brief Latency-sensitive planned work. */
  High = 1U,
};

/**
 * @brief Private trace action mapped to copied Host execution observations.
 *
 * @throws Nothing.
 * @note Policy callbacks never receive this value or its storage.
 */
enum class ExecutionTraceAction : std::uint32_t {
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
 * @brief Borrowed observation context used by the private execution service.
 *
 * The Graph runtime owns this object through every accepted Run callback. The
 * service may publish worker/epoch attribution and trace values, but receives
 * no Graph, cache, native device, lifecycle owner, or mutable observation
 * storage through this boundary.
 *
 * @throws Nothing from every virtual operation.
 * @note The protected destructor prevents deletion through a borrowed pointer.
 */
class ExecutionHostContext {
 public:
  /**
   * @brief Tests one fixed physical device capability.
   * @param device Device label to test.
   * @return True when the process owns that capability.
   * @throws Nothing.
   */
  virtual bool is_device_available(Device device) const noexcept = 0;

  /**
   * @brief Publishes worker and epoch identity on the calling thread.
   * @param worker_id Private worker id, or -1 when unavailable.
   * @param epoch Active nonzero Run/route epoch.
   * @return Nothing.
   * @throws Nothing.
   * @note Every entered callback balances this call with
   * `clear_task_context()` on every exit path.
   */
  virtual void set_task_context(int worker_id,
                                std::uint64_t epoch) noexcept = 0;

  /**
   * @brief Clears calling-thread execution attribution.
   * @return Nothing.
   * @throws Nothing.
   */
  virtual void clear_task_context() noexcept = 0;

  /**
   * @brief Publishes one allocation-free execution trace observation.
   * @param action Stable private action.
   * @param node_id Backend node id, or -1 when unavailable.
   * @param worker_id Private worker id, or -1 when unavailable.
   * @param epoch Active Run/route epoch.
   * @return Nothing.
   * @throws Nothing; observation failure cannot replace task settlement.
   */
  virtual void log_event(ExecutionTraceAction action, int node_id,
                         int worker_id, std::uint64_t epoch) noexcept = 0;

 protected:
  /**
   * @brief Prevents deletion through the borrowed execution boundary.
   * @throws Nothing.
   */
  virtual ~ExecutionHostContext() = default;
};

/**
 * @brief Executor interface borrowed by execution task handles.
 *
 * The compute dispatcher owns the executor, dependency graph, and completion
 * state. A private route may call `run_task` only while the matching submitted
 * batch is alive and MUST NOT retain the executor after
 * `ExecutionTaskRuntime::wait_for_completion` returns.
 *
 * @throws Submitted task exceptions unchanged from `run_task`.
 * @note Private routes never own or delete the executor. Protected virtual
 * destruction prevents deletion through its borrowed base pointer.
 */
class ExecutionTaskExecutor {
 protected:
  /**
   * @brief Supports concrete derived destruction by the dispatcher owner.
   * @throws Nothing under the concrete dispatcher lifetime contract.
   * @note Deletion through a borrowed `ExecutionTaskExecutor*` is ill-formed;
   * ordinary concrete-owner destruction remains valid.
   */
  virtual ~ExecutionTaskExecutor() = default;

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
struct ExecutionTaskHandle {
  /** @brief Borrowed dispatcher executor, or null for an empty handle. */
  ExecutionTaskExecutor* executor = nullptr;

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
   * @throws Any exception emitted by `ExecutionTaskExecutor::run_task`
   * unchanged.
   * @note Calling an empty handle is a no-op.
   */
  void run() const {
    if (executor != nullptr && task_id >= 0) {
      executor->run_task(task_id);
    }
  }
};

/**
 * @brief Minimal execution-facing runtime used by production compute dispatch.
 *
 * Implementations own queues, workers, epoch state, completion counters, and
 * first-exception transport. They borrow task handles until the matching wait
 * completes. Initial and worker-ready batches are explicit pure virtual calls
 * so a base implementation cannot silently decompose an atomic batch.
 *
 * @throws Concrete submission, synchronization, allocation, or task
 *         exceptions according to the individual operation contract.
 * @note This repository-internal migration seam is not installed, loadable,
 * or reachable by a Policy DSO.
 */
class ExecutionTaskRuntime {
 public:
  /**
   * @brief Owned callback accepted by the any-thread submission lane.
   * @throws std::bad_alloc when callable state construction cannot allocate.
   * @note Ownership moves into the private execution service until execution or
   *       retirement.
   */
  using Task = std::function<void()>;

  /**
   * @brief Releases a runtime through its private execution vtable.
   * @throws Nothing under the implementation destructor contract.
   * @note Concrete objects remain owned by repository composition roots.
   */
  virtual ~ExecutionTaskRuntime() = default;

  /**
   * @brief Reports device labels routable by the private execution domain.
   * @return Devices ordered by private route preference.
   * @throws std::bad_alloc if result storage cannot be allocated.
   * @note The default CPU-only value preserves simple private routes.
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
   * @throws Concrete private submission failures unchanged.
   * @note Failure MUST publish none of the handles or batch state.
   */
  virtual void submit_initial_task_handles(
      std::vector<ExecutionTaskHandle>&& handles, int total_task_count,
      ExecutionTaskPriority priority = ExecutionTaskPriority::Normal) = 0;

  /**
   * @brief Atomically submits handles released together by a worker.
   * @param handles Newly ready handles from one dependency release.
   * @param priority Priority shared by the handles.
   * @return Nothing.
   * @throws Concrete private submission failures unchanged.
   * @note Failure MUST enqueue none of the supplied handles.
   */
  virtual void submit_ready_task_handles_from_worker(
      std::vector<ExecutionTaskHandle>&& handles,
      ExecutionTaskPriority priority = ExecutionTaskPriority::Normal) = 0;

  /**
   * @brief Submits one callback from any caller thread.
   * @param task Callback whose state moves into the private route.
   * @param priority Requested priority.
   * @param epoch Optional batch epoch; zero remains an uncancellable task.
   * @return Nothing.
   * @throws Concrete private submission failures unchanged.
   * @note The private route owns the moved callback until execution or
   * retirement.
   */
  virtual void submit_ready_task_any_thread(
      Task&& task,
      ExecutionTaskPriority priority = ExecutionTaskPriority::Normal,
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
   * @throws Concrete synchronization or test-injection exceptions unchanged.
   * @note The first accepted non-null exception remains authoritative for the
   *       active batch; null input mutates no queue, epoch, or exception state.
   */
  virtual void set_exception(std::exception_ptr error) = 0;

  /**
   * @brief Adds dynamically created micro-tasks to completion accounting.
   * @param delta Positive task-count increment.
   * @return Nothing.
   * @throws Concrete validation or synchronization exceptions unchanged.
   * @note Nonpositive-delta handling must preserve counter integrity.
   */
  virtual void inc_tasks_to_complete(int delta) = 0;

  /**
   * @brief Marks one active task complete.
   * @return Nothing.
   * @throws Concrete validation or synchronization exceptions unchanged.
   * @note Completion accounting must never wrap below its settled floor.
   */
  virtual void dec_tasks_to_complete() = 0;

  /**
   * @brief Publishes an execution trace for the current worker context.
   * @param action Stable private execution action.
   * @param node_id Backend node id, or -1 when unavailable.
   * @return Nothing.
   * @throws Nothing under the public host-context contract.
   * @note Worker and epoch attribution come from the current execution task
   *       context.
   */
  virtual void log_event(ExecutionTraceAction action, int node_id) = 0;
};

}  // namespace ps
