#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "compute/compute_run.hpp"
#include "photospider/scheduler/scheduler_task_runtime.hpp"

namespace ps {
class CpuWorkStealingScheduler;
class SchedulerHostContext;
}  // namespace ps

namespace ps::compute {

/**
 * @brief Immutable execution metadata copied into one ready submission.
 *
 * The snapshot carries request identity and policy inputs needed for routing
 * and diagnostics without exposing a `ComputeRunDescriptor`, Graph, plan,
 * dependency map, result store, or commit owner to the execution service.
 *
 * @throws std::bad_alloc when graph identity storage is copied.
 * @note Accessors return values or immutable references only. The snapshot
 * remains stable after its originating dispatcher frame returns.
 */
class ReadyTaskMetadata final {
 public:
  /**
   * @brief Returns the opaque Run namespace.
   * @return Run id copied from the matching lease descriptor.
   * @throws Nothing.
   */
  ComputeRunId run_id() const noexcept { return run_id_; }

  /**
   * @brief Returns the stable graph/session identity.
   * @return Borrowed immutable string owned by this snapshot.
   * @throws Nothing.
   */
  const std::string& graph_identity() const noexcept { return graph_identity_; }

  /**
   * @brief Returns the topology-only submission revision.
   * @return Revision copied from the matching Run descriptor.
   * @throws Nothing.
   */
  ComputeRunSubmissionRevision revision() const noexcept { return revision_; }

  /**
   * @brief Returns the request target node.
   * @return Graph-local target node id.
   * @throws Nothing.
   */
  int target_node_id() const noexcept { return target_node_id_; }

  /**
   * @brief Returns the single-domain compute intent.
   * @return Intent copied from the matching Run descriptor.
   * @throws Nothing.
   */
  ComputeIntent intent() const noexcept { return intent_; }

  /**
   * @brief Returns the immutable output-quality marker.
   * @return Quality copied from the matching Run descriptor.
   * @throws Nothing.
   */
  ComputeRunQuality quality() const noexcept { return quality_; }

  /**
   * @brief Returns the copied scheduling service inputs.
   * @return Borrowed immutable QoS value owned by this snapshot.
   * @throws Nothing.
   */
  const ComputeRunQos& qos() const noexcept { return qos_; }

  /**
   * @brief Returns the planned node used for execution trace attribution.
   * @return Graph-local planned node id.
   * @throws Nothing.
   * @note This scalar is diagnostic metadata, not a Graph or task-graph
   * capability.
   */
  int trace_node_id() const noexcept { return trace_node_id_; }

  /**
   * @brief Reports whether this submission belongs to the initial ready set.
   * @return True for dispatcher-discovered initial work.
   * @throws Nothing.
   * @note The flag affects trace labeling only; it does not grant readiness.
   */
  bool is_initial_ready() const noexcept { return is_initial_ready_; }

 private:
  friend class ReadyTaskSubmission;

  /**
   * @brief Copies immutable Run and trace inputs into one service value.
   * @param descriptor Matching Run descriptor retained by the submission
   * lease.
   * @param trace_node_id Planned node id used only for trace attribution.
   * @param is_initial_ready Whether the dispatcher selected initial readiness.
   * @throws std::bad_alloc when graph identity storage cannot allocate.
   * @note Construction receives no mutable Run, Graph, plan, or dependency
   * state.
   */
  ReadyTaskMetadata(const ComputeRunDescriptor& descriptor, int trace_node_id,
                    bool is_initial_ready);

  /** @brief Opaque Run namespace copied from the matching descriptor. */
  ComputeRunId run_id_;

  /** @brief Owned stable graph/session label. */
  std::string graph_identity_;

  /** @brief Topology-only revision copied before execution. */
  ComputeRunSubmissionRevision revision_;

  /** @brief Request target node retained for immutable diagnostics. */
  int target_node_id_ = -1;

  /** @brief Single-domain intent independent from QoS. */
  ComputeIntent intent_ = ComputeIntent::GlobalHighPrecision;

  /** @brief Requested output quality independent from intent. */
  ComputeRunQuality quality_ = ComputeRunQuality::Full;

  /** @brief Copied policy inputs that mint no resource authority. */
  ComputeRunQos qos_;

  /** @brief Planned node id used only for scheduler trace publication. */
  int trace_node_id_ = -1;

  /** @brief Whether this task was in the initial dependency-ready set. */
  bool is_initial_ready_ = false;
};

/**
 * @brief Move-owned ready-only work accepted by `ExecutionService`.
 *
 * A submission owns immutable metadata, a composite Run/local identity, one
 * matching non-forgeable Run lease, and an executable. It contains no borrowed
 * Graph, plan, task executor, dependency map, result slot, or commit callback.
 *
 * @throws std::invalid_argument when the executable is empty or identity and
 * lease name different Runs.
 * @throws std::bad_alloc when metadata or executable ownership allocates.
 * @note Moving transfers the lease and executable without changing readiness.
 * Copying is disabled so queue admission has one explicit ownership transfer.
 */
class ReadyTaskSubmission final {
 public:
  /**
   * @brief Owned task operation routed through the retained lease.
   *
   * @param lease Submission-owned matching Run lease used for completion and
   * failure routing.
   * @param identity Composite identity already validated at construction.
   * @param task_runtime Active ready-submission runtime used for completion,
   * dependent release, and trace publication.
   * @return Nothing.
   * @throws Any task, completion, dependency-release, or runtime exception
   * unchanged.
   */
  using Executable = std::function<void(ComputeRunLease& lease,
                                        const ComputeRunTaskIdentity& identity,
                                        SchedulerTaskRuntime& task_runtime)>;

  /**
   * @brief Creates one dependency-ready owned submission.
   * @param lease Strong Run lease transferred into submission ownership.
   * @param identity Composite Run/local task identity.
   * @param trace_node_id Planned node used for execution diagnostics.
   * @param is_initial_ready Whether the dispatcher selected initial readiness.
   * @param executable Owned operation invoked only after service admission.
   * @throws std::invalid_argument when executable is empty or the identity Run
   * differs from the lease Run.
   * @throws std::bad_alloc when metadata or executable storage allocates.
   * @note Readiness is a caller-established precondition. Construction does
   * not inspect a task graph or dependency map.
   */
  ReadyTaskSubmission(ComputeRunLease lease, ComputeRunTaskIdentity identity,
                      int trace_node_id, bool is_initial_ready,
                      Executable executable);

  /** @brief Transfers complete submission ownership. @throws Nothing. */
  ReadyTaskSubmission(ReadyTaskSubmission&& other) noexcept = default;

  /**
   * @brief Replaces this submission through complete ownership transfer.
   * @param other Submission whose ownership moves into this object.
   * @return Reference to this submission.
   * @throws Nothing.
   */
  ReadyTaskSubmission& operator=(ReadyTaskSubmission&& other) noexcept =
      default;

  /** @brief Prevents duplicating one service admission value. */
  ReadyTaskSubmission(const ReadyTaskSubmission& other) = delete;

  /** @brief Prevents duplicate lease/executable assignment. */
  ReadyTaskSubmission& operator=(const ReadyTaskSubmission& other) = delete;

  /**
   * @brief Returns the immutable copied execution metadata.
   * @return Borrowed snapshot valid for this submission lifetime.
   * @throws Nothing.
   */
  const ReadyTaskMetadata& metadata() const noexcept { return metadata_; }

  /**
   * @brief Returns the composite completion identity.
   * @return Run/local task identity copied by value.
   * @throws Nothing.
   */
  ComputeRunTaskIdentity identity() const noexcept { return identity_; }

  /**
   * @brief Executes this accepted ready task through its matching lease.
   * @param task_runtime Active service runtime receiving completion and newly
   * ready submissions.
   * @return Nothing.
   * @throws The exact executable exception after best-effort matching Run
   * failure publication.
   * @note The submission and runtime must remain alive until this call returns.
   */
  void execute(SchedulerTaskRuntime& task_runtime);

 private:
  /** @brief Immutable request and trace metadata copied before queueing. */
  ReadyTaskMetadata metadata_;

  /** @brief Composite identity validated against lease ownership. */
  ComputeRunTaskIdentity identity_;

  /** @brief Non-forgeable owner retaining matching Run-local state. */
  ComputeRunLease lease_;

  /** @brief Owned operation containing no borrowed dispatcher stack state. */
  Executable executable_;
};

/**
 * @brief Private scheduler runtime that admits only ready submission values.
 *
 * TaskSubmissionPlan uses this extension after dependency-ready detection.
 * Generic scheduler ABI paths never see it, and no installed scheduler vtable
 * changes.
 *
 * @throws Concrete implementations define synchronization and allocation
 * failures.
 * @note Inherited raw callback and borrowed-handle entry points remain present
 * only to implement `SchedulerTaskRuntime`; the current ExecutionService
 * rejects them.
 */
class ReadyTaskSubmissionRuntime : public SchedulerTaskRuntime {
 public:
  /**
   * @brief Releases a private ready-submission runtime.
   * @throws Nothing under concrete destructor contracts.
   */
  ~ReadyTaskSubmissionRuntime() override = default;

  /**
   * @brief Transfers one dependency-ready task into service queue ownership.
   * @param submission Ready-only value whose ownership moves into the runtime.
   * @return Nothing.
   * @throws std::invalid_argument when it belongs to another active Run.
   * @throws std::bad_alloc when queue ownership cannot allocate.
   * @note The caller has already satisfied dependencies; the runtime must not
   * inspect or derive a task graph.
   */
  virtual void submit_ready_submission(ReadyTaskSubmission submission) = 0;
};

/**
 * @brief Owns the current process CPU execution slice for one active Run.
 *
 * The service owns a concrete CPU worker scheduler, serializes one complete
 * Run batch, and accepts only `ReadyTaskSubmission`. It owns no planning,
 * dependency, Graph/cache, dirty, commit, admission-ledger, or policy state.
 *
 * @throws std::bad_alloc or std::system_error from explicit execution setup.
 * @note This private source-tree service is explicitly composed and injected;
 * it is not a singleton, public Host API, or scheduler plugin ABI.
 */
class ExecutionService final : public ReadyTaskSubmissionRuntime {
 public:
  /**
   * @brief Creates a stopped execution domain with no worker threads.
   * @throws std::bad_alloc if host-proxy ownership cannot allocate.
   * @note CPU workers are created lazily for the first migrated Run.
   */
  ExecutionService();

  /**
   * @brief Joins and releases service-owned CPU workers.
   * @throws Nothing.
   * @note Kernel and every injected ComputeService must already be drained.
   */
  ~ExecutionService() noexcept override;

  /** @brief Prevents copying physical worker and Run-gate ownership. */
  ExecutionService(const ExecutionService& other) = delete;

  /** @brief Prevents assigning physical worker and Run-gate ownership. */
  ExecutionService& operator=(const ExecutionService& other) = delete;

  /**
   * @brief Executes one complete ready-only CPU Run batch synchronously.
   *
   * @param host Active Graph runtime observation context, borrowed only until
   * the settled wait finishes.
   * @param worker_count Exact built-in CPU grant in `[1,8]`.
   * @param initial_submissions Dispatcher-discovered initial ready values from
   * one Run.
   * @param total_task_count Complete logical planned-task count.
   * @return Nothing after every callback in the batch settles.
   * @throws std::invalid_argument for invalid grants, counts, empty active
   * batches, or mixed Run ids.
   * @throws std::logic_error for invalid service lifecycle or generic runtime
   * use.
   * @throws std::bad_alloc or std::system_error from pool/queue setup.
   * @throws The exact first worker task exception after settlement.
   * @note The single-Run mutex spans setup, submission, wait, and host unbind.
   * No Graph, plan, dependency, result, or commit object enters this method.
   */
  void execute_cpu_run(SchedulerHostContext& host, unsigned int worker_count,
                       std::vector<ReadyTaskSubmission> initial_submissions,
                       int total_task_count);

  /** @copydoc ReadyTaskSubmissionRuntime::submit_ready_submission */
  void submit_ready_submission(ReadyTaskSubmission submission) override;

  /**
   * @brief Reports the CPU-only capability exposed by the migrated slice.
   * @return One-element CPU device list.
   * @throws std::bad_alloc when result storage cannot allocate.
   */
  std::vector<Device> available_devices() const override;

  /**
   * @brief Rejects borrowed task-handle initial batches.
   * @throws std::logic_error unconditionally.
   * @note Migrated work must enter as `ReadyTaskSubmission`.
   */
  void submit_initial_task_handles(std::vector<TaskHandle>&& handles,
                                   int total_task_count,
                                   SchedulerTaskPriority priority) override;

  /**
   * @brief Rejects borrowed task handles released by workers.
   * @throws std::logic_error unconditionally.
   * @note Dispatcher dependents re-enter as `ReadyTaskSubmission`.
   */
  void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      SchedulerTaskPriority priority) override;

  /**
   * @brief Rejects anonymous raw callback submission.
   * @throws std::logic_error unconditionally.
   * @note Every accepted migrated callback must carry explicit Run identity and
   * lease ownership in `ReadyTaskSubmission`.
   */
  void submit_ready_task_any_thread(
      Task&& task, SchedulerTaskPriority priority,
      std::optional<std::uint64_t> epoch) override;

  /**
   * @brief Waits for the active CPU batch to settle.
   * @return Nothing on success.
   * @throws std::logic_error outside an active Run.
   * @throws The exact first worker exception.
   */
  void wait_for_completion() override;

  /**
   * @brief Publishes an exact exception into the active CPU fence.
   * @param error Non-null worker exception identity.
   * @return Nothing.
   * @throws std::logic_error outside an active Run.
   */
  void set_exception(std::exception_ptr error) override;

  /**
   * @brief Adds logical work to the active CPU completion count.
   * @param delta Positive work count.
   * @return Nothing.
   * @throws std::logic_error outside an active Run.
   * @throws std::overflow_error when CPU completion accounting overflows.
   */
  void inc_tasks_to_complete(int delta) override;

  /**
   * @brief Retires one logical task from the active CPU batch.
   * @return Nothing.
   * @throws std::logic_error outside an active Run.
   */
  void dec_tasks_to_complete() override;

  /**
   * @brief Publishes one active-worker scheduler trace.
   * @param action Stable trace action.
   * @param node_id Planned Graph node id.
   * @return Nothing.
   * @throws std::logic_error outside an active Run.
   */
  void log_event(SchedulerTraceAction action, int node_id) override;

 private:
  /** @brief Service-owned host proxy attached for the CPU pool lifetime. */
  class HostContextProxy;

  /**
   * @brief Creates or replaces the CPU pool for an exact worker grant.
   * @param worker_count Validated exact grant in `[1,8]`.
   * @return Nothing.
   * @throws std::bad_alloc or std::system_error from scheduler setup.
   * @throws Any built-in lifecycle exception unchanged.
   * @note The Run gate is held and no callback is active.
   */
  void configure_cpu_scheduler(unsigned int worker_count);

  /**
   * @brief Returns the scheduler serving the active Run.
   * @return Borrowed stable CPU scheduler.
   * @throws std::logic_error when no Run batch is active.
   * @note The caller executes under the single-Run lifetime interval.
   */
  CpuWorkStealingScheduler& active_scheduler();

  /**
   * @brief Wraps one owned submission for CPU queue storage.
   * @param submission Ready value transferred into shared callback ownership.
   * @return Scheduler-owned callback retaining the submission.
   * @throws std::bad_alloc when shared or function state cannot allocate.
   * @note The callback captures this service, which outlives the settled batch.
   */
  Task make_cpu_task(ReadyTaskSubmission submission);

  /** @brief Serializes the complete setup-to-settlement single-Run interval. */
  std::mutex run_mutex_;

  /** @brief Stable observation proxy attached to the current CPU scheduler. */
  std::unique_ptr<HostContextProxy> host_context_;

  /** @brief Lazily configured service-owned physical CPU runtime. */
  std::unique_ptr<CpuWorkStealingScheduler> cpu_scheduler_;

  /** @brief Exact grant used by the current CPU scheduler, or zero if absent.
   */
  unsigned int configured_workers_ = 0U;

  /** @brief Active Run namespace while callbacks may enter, otherwise empty. */
  std::optional<ComputeRunId> active_run_id_;
};

}  // namespace ps::compute
