#pragma once

#include <cstdint>
#include <string>

#include "photospider/core/device.hpp"
#include "photospider/scheduler/scheduler_task_runtime.hpp"

/**
 * @file scheduler.hpp
 * @brief Current lifecycle and host context for the provisional scheduler ABI.
 *
 * Scheduler plugins exchange C++ virtual interfaces, standard-library values,
 * callbacks, and exceptions across a DSO boundary. The matching numeric
 * handshake gates interface generation but does not provide a stable pure C or
 * cross-toolchain binary ABI.
 */

namespace ps {

/**
 * @brief Maximum explicit CPU/plugin worker request for one scheduler.
 *
 * @throws Nothing.
 * @note Zero remains the automatic-selection sentinel. Positive requests above
 *       this value are invalid and must not be clamped. Before construction,
 *       zero resolves once to
 *       `min(max(1, detected_hardware_concurrency), 8)`; values one through
 *       eight remain exact.
 */
inline constexpr unsigned int kSchedulerWorkerRequestMax = 8U;

/**
 * @brief Absolute worker ceiling for one built-in GPU scheduler instance.
 *
 * @throws Nothing.
 * @note The ceiling combines `kSchedulerWorkerRequestMax` CPU workers with the
 *       one configured potential GPU worker owned by the current pipeline.
 *       Admission charges that potential worker conservatively even when no
 *       device worker is started.
 */
inline constexpr unsigned int kGpuSchedulerWorkerInstanceMax = 9U;

/**
 * @brief Fixed aggregate scheduler-worker admission ceiling for one process.
 *
 * @throws Nothing.
 * @note This is the migration safety ceiling for per-Graph schedulers, not a
 *       remotely configurable execution-service capacity. It is shared by all
 *       embedded Hosts and Kernels in one process and counts planned
 *       scheduler-owned workers only, not all process threads.
 */
inline constexpr unsigned int kSchedulerWorkerProcessMax = 32U;

/**
 * @brief Non-owning host services available to an attached scheduler.
 *
 * The host owns this object and guarantees that it outlives every entered
 * attach attempt plus any subsequent start, dispatch, shutdown, and detach
 * cleanup, including fallback after a partially failed attach. Worker-facing
 * calls are thread-safe and non-throwing. The protected destructor prevents a
 * plugin from deleting the host object across a dynamic-library boundary.
 *
 * @throws Nothing through worker-facing capability and observation calls.
 * @note Implementations expose capability labels and observation services only;
 *       no graph, cache, native device, or runtime owner is reachable here.
 */
class SchedulerHostContext {
 public:
  /**
   * @brief Tests whether the host owns one physical device capability.
   * @param device Stable capability label to test.
   * @return True when the physical host capability is available.
   * @throws Nothing.
   * @note Scheduler configuration may still exclude an available device.
   */
  virtual bool is_device_available(Device device) const noexcept = 0;

  /**
   * @brief Sets host-observed worker and epoch identity on the calling thread.
   * @param worker_id Scheduler worker id, or -1 when unavailable.
   * @param epoch Active task epoch; zero denotes uncancellable compatibility
   *        work.
   * @return Nothing.
   * @throws Nothing.
   * @note The published context applies only to the calling thread.
   * @note Every top-level callback balances this call with
   *       `clear_task_context`, including exception paths.
   */
  virtual void set_task_context(int worker_id,
                                std::uint64_t epoch) noexcept = 0;

  /**
   * @brief Clears host-observed worker and epoch identity on this thread.
   * @return Nothing.
   * @throws Nothing.
   * @note Clearing one thread does not alter another worker's context.
   */
  virtual void clear_task_context() noexcept = 0;

  /**
   * @brief Publishes one scheduler trace with explicit task identity.
   * @param action Stable trace action.
   * @param node_id Backend node id, or -1 when unavailable.
   * @param worker_id Scheduler worker id, or -1 when unavailable.
   * @param epoch Active task epoch.
   * @return Nothing.
   * @throws Nothing; observation failure cannot replace task exceptions.
   * @note The context retains no reference to caller-provided scalar values.
   */
  virtual void log_event(SchedulerTraceAction action, int node_id,
                         int worker_id, std::uint64_t epoch) noexcept = 0;

 protected:
  /**
   * @brief Prevents deletion through a borrowed plugin-side pointer.
   * @throws Nothing.
   * @note Only the host implementation may destroy the concrete context.
   */
  virtual ~SchedulerHostContext() = default;
};

/**
 * @brief Complete scheduler lifecycle plus minimal production task runtime.
 *
 * Every object returned by the scheduler plugin create ABI implements this
 * single inherited interface. The host owns the scheduler through the matching
 * plugin destroy export, while the scheduler may borrow its host context from
 * entry into attach until the matching explicit or fallback detach completes.
 *
 * @throws Concrete lifecycle, allocation, synchronization, and submitted-task
 *         exceptions according to the individual operation contract.
 * @note Lifecycle calls are externally serialized. A scheduler MUST shutdown
 *       before detach and MUST clear its saved context during detach. When a
 *       DSO implementation is reached through the host plugin owner, its
 *       exceptions are converted to host-owned `GraphError` or
 *       `std::bad_alloc`; host task exceptions remain exact. The complete
 *       virtual interface is part of the current provisional C++ ABI and
 *       requires the matching SDK and a compatible toolchain/runtime.
 */
class IScheduler : public SchedulerTaskRuntime {
 public:
  /**
   * @brief Releases one scheduler through its complete vtable.
   * @throws Nothing under valid implementation destructor contracts.
   * @note Plugin-created instances are destroyed through their matching ABI
   *       export rather than direct host-side deletion.
   */
  ~IScheduler() override = default;

  /**
   * @brief Attaches a borrowed host context.
   * @param host Context that outlives shutdown and detach.
   * @return Nothing.
   * @throws Implementation preparation exceptions directly for host-owned
   * schedulers; a plugin owner applies the DSO exception boundary above.
   * @note Attachment never transfers host ownership. A plugin that retains the
   *       pointer before throwing must release it when fallback detach runs.
   */
  virtual void attach(SchedulerHostContext& host) = 0;

  /**
   * @brief Clears the borrowed host context after shutdown.
   * @return Nothing.
   * @throws Explicit implementation lifecycle exceptions directly for
   * host-owned schedulers; a plugin owner applies the DSO exception boundary.
   * @note Successful detach clears the exact pointer accepted by `attach()`.
   */
  virtual void detach() = 0;

  /**
   * @brief Starts worker and queue resources.
   * @return Nothing.
   * @throws Implementation allocation or thread exceptions directly for
   * host-owned schedulers; a plugin owner applies the DSO exception boundary.
   * @note Repeated-start behavior is implementation-defined but must preserve
   *       a coherent externally observable lifecycle.
   */
  virtual void start() = 0;

  /**
   * @brief Stops dispatch and joins scheduler-owned workers.
   * @return Nothing.
   * @throws Explicit implementation lifecycle exceptions directly for
   * host-owned schedulers; a plugin owner applies the DSO exception boundary.
   * @note Shutdown does not detach, so a runtime may restart this scheduler.
   */
  virtual void shutdown() = 0;

  /**
   * @brief Returns the stable scheduler type name.
   * @return Owned human-readable scheduler name.
   * @throws std::bad_alloc if result allocation fails.
   * @note The returned string does not alias plugin metadata storage.
   */
  virtual std::string name() const = 0;

  /**
   * @brief Returns a human-readable runtime statistics snapshot.
   * @return Owned statistics text.
   * @throws std::bad_alloc if result allocation fails.
   * @note The text is diagnostic and is not a control protocol.
   */
  virtual std::string get_stats() const = 0;

  /**
   * @brief Reports whether dispatch is running.
   * @return True after successful start and before completed shutdown.
   * @throws Implementation lifecycle-observation exceptions directly for
   * host-owned schedulers; a plugin owner applies the DSO exception boundary.
   * @note Callers use this only as a lifecycle snapshot, not synchronization.
   */
  virtual bool is_running() const = 0;
};

}  // namespace ps
