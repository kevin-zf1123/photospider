#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "compute/compute_run.hpp"
#include "photospider/scheduler/scheduler_task_runtime.hpp"
#include "runtime/resource_ledger.hpp"

namespace ps {
class SchedulerHostContext;

namespace testing {

/**
 * @brief Test-only bridge for private ExecutionService staging boundaries.
 *
 * @note The definition lives under tests/support and is never installed.
 */
class ExecutionServiceTestAccess;

}  // namespace testing
}  // namespace ps

namespace ps::compute {

/**
 * @brief Private composition-root limits for one CPU execution domain.
 *
 * This source-tree type replaces the removed installed
 * `kSchedulerWorkerProcessMax` policy constant. It carries every ledger
 * dimension explicitly and converts to the authority-neutral `ResourceVector`
 * only when constructing the private service.
 *
 * @throws Nothing for aggregate construction and conversion.
 * @note This is not an installed scheduler API or scheduler ABI surface.
 */
struct ExecutionResourceLimits final {
  /** @brief Concurrent Run and legacy-owner CPU execution rights. */
  std::uint64_t cpu_slots = 0U;

  /** @brief Host-owned retained-memory capacity. */
  std::uint64_t retained_memory_bytes = 0U;

  /** @brief Host-declared execution scratch capacity. */
  std::uint64_t scratch_bytes = 0U;

  /** @brief Logical entries available in the bounded ready store. */
  std::uint64_t ready_entries = 0U;

  /** @brief Accounted bytes available in the bounded ready store. */
  std::uint64_t ready_bytes = 0U;

  /**
   * @brief Converts the complete private limit set to ledger dimensions.
   * @return Value-only resource vector with identical fields.
   * @throws Nothing.
   */
  ResourceVector resource_vector() const noexcept {
    return ResourceVector{cpu_slots, retained_memory_bytes, scratch_bytes,
                          ready_entries, ready_bytes};
  }
};

/**
 * @brief Immutable adapter-owned resource demand for one ready submission.
 *
 * These fields declare only bytes in addition to the mandatory service
 * envelope. Retained memory and scratch are held while the callback executes.
 * Ready bytes are held while the owned submission resides in the bounded
 * service store. `ExecutionService` always adds its `RunState`, queue entry,
 * metadata, shared-control, ready-entry, and CPU execution charges.
 *
 * @throws Nothing for value construction and comparison.
 * @note Zero scratch means the current host contract declares no separately
 * metered scratch. It does not claim that backend, plugin, or runtime
 * allocation was measured as zero. Zero ready bytes remains valid because the
 * service structural ready envelope is always positive.
 */
struct ReadyTaskResourceDemand final {
  /** @brief Additional adapter bytes retained during callback execution. */
  std::uint64_t retained_memory_bytes = 0U;

  /** @brief Additional Host-declared temporary execution bytes. */
  std::uint64_t scratch_bytes = 0U;

  /** @brief Additional adapter bytes occupied while submission is ready. */
  std::uint64_t ready_bytes = 0U;
};

/**
 * @brief Compares every per-submission resource declaration.
 * @param lhs First trusted demand.
 * @param rhs Second trusted demand.
 * @return True only when every declared dimension is equal.
 * @throws Nothing.
 */
bool operator==(const ReadyTaskResourceDemand& lhs,
                const ReadyTaskResourceDemand& rhs) noexcept;

/**
 * @brief Reports whether any per-submission declaration differs.
 * @param lhs First trusted demand.
 * @param rhs Second trusted demand.
 * @return True when at least one dimension differs.
 * @throws Nothing.
 */
bool operator!=(const ReadyTaskResourceDemand& lhs,
                const ReadyTaskResourceDemand& rhs) noexcept;

/**
 * @brief Builds one adapter-owned callable demand from its capture size.
 * @param capture_bytes Trusted compile-time size of the owned capture object.
 * @return Equal retained/ready charges and zero separately owned scratch.
 * @throws GraphError when checked structural addition overflows.
 * @note The charge covers trusted capture/control storage only. It never
 * estimates opaque plugin or backend callback allocations.
 */
ReadyTaskResourceDemand owned_callback_resource_demand(
    std::uint64_t capture_bytes);

/**
 * @brief Complete adapter declaration for one synchronous CPU Run batch.
 *
 * Shared retained bytes are charged once for Run/control/plan/context
 * ownership. The per-task declaration is multiplied only by maximum callback
 * concurrency for retained/scratch and by logical task count for ready bytes.
 * Mandatory service structural envelopes are added independently.
 *
 * @throws Nothing for value construction.
 * @note Callers must not place the same shared context in the per-task field;
 * doing so would conservatively double-count it and can cause batch self-lock.
 */
struct CpuRunResourceDemand final {
  /** @brief Adapter-owned shared bytes retained for complete batch settlement.
   */
  std::uint64_t shared_retained_memory_bytes = 0U;

  /** @brief Uniform additional demand carried by every logical task. */
  ReadyTaskResourceDemand task;
};

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
 * matching non-forgeable Run lease, trusted resource demand, and an
 * executable. It contains no borrowed Graph, plan, task executor, dependency
 * map, result slot, or commit callback.
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
   * @param priority Private high/normal ready-store ordering hint.
   * @param resource_demand Trusted immutable retained, scratch, and ready-byte
   * declaration.
   * @throws std::invalid_argument when executable is empty or the identity Run
   * differs from the lease Run.
   * @throws std::bad_alloc when metadata or executable storage allocates.
   * @note Readiness is a caller-established precondition. Construction does
   * not inspect a task graph or dependency map.
   */
  ReadyTaskSubmission(
      ComputeRunLease lease, ComputeRunTaskIdentity identity, int trace_node_id,
      bool is_initial_ready, Executable executable,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal,
      ReadyTaskResourceDemand resource_demand = default_resource_demand());

  /**
   * @brief Returns the empty adapter-owned demand.
   * @return Zero additional retained, scratch, and ready bytes.
   * @throws Nothing.
   * @note `ExecutionService` always charges its positive mandatory submission
   * envelope. Adapters with owned captures or scratch must pass an explicit
   * declaration instead of relying on this zero-addition default.
   */
  static ReadyTaskResourceDemand default_resource_demand() noexcept;

  /**
   * @brief Transfers complete submission ownership without duplicating it.
   * @param other Submission whose lease, executable, and demand are moved.
   * @throws Nothing.
   * @note The moved-from value remains destructible but carries no admission
   * authority that may be submitted again.
   */
  ReadyTaskSubmission(ReadyTaskSubmission&& other) noexcept = default;

  /**
   * @brief Replaces this submission through complete ownership transfer.
   * @param other Submission whose ownership moves into this object.
   * @return Reference to this submission.
   * @throws Nothing.
   */
  ReadyTaskSubmission& operator=(ReadyTaskSubmission&& other) noexcept =
      default;

  /**
   * @brief Prevents duplicating one service admission value.
   * @param other Submission whose lease and executable cannot be copied.
   * @throws Nothing because this operation is unavailable.
   */
  ReadyTaskSubmission(const ReadyTaskSubmission& other) = delete;

  /**
   * @brief Prevents assigning duplicate lease and executable ownership.
   * @param other Submission whose ownership cannot be copied.
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
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
   * @brief Returns the private service-ready queue hint for this submission.
   * @return High for latency-sensitive work, otherwise Normal.
   * @throws Nothing.
   * @note Priority does not establish dependency correctness or global
   * fairness. The dispatcher must already have established readiness.
   */
  SchedulerTaskPriority priority() const noexcept { return priority_; }

  /**
   * @brief Returns the immutable trusted-host resource declaration.
   * @return Per-submission retained, scratch, and ready bytes.
   * @throws Nothing.
   */
  ReadyTaskResourceDemand resource_demand() const noexcept {
    return resource_demand_;
  }

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

  /** @brief Private ready-queue hint that carries no dependency authority. */
  SchedulerTaskPriority priority_ = SchedulerTaskPriority::Normal;

  /** @brief Immutable host-authored resources checked at Run admission. */
  ReadyTaskResourceDemand resource_demand_;
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
 * @brief Owns one fixed Host CPU execution domain for concurrent Runs.
 *
 * The service owns a fixed worker pool and accepts only
 * `ReadyTaskSubmission`. Each active Run has isolated completion, exception,
 * trace-target, and settlement state while independent Runs may overlap. The
 * service also exclusively owns the host-authoritative resource ledger and
 * entry/byte-bounded ready store. It owns no planning, dependency,
 * Graph/cache, dirty propagation, visible commit, final lifecycle registry, or
 * fairness policy state.
 *
 * @throws std::bad_alloc or std::system_error from explicit execution setup.
 * @note This private source-tree service is explicitly composed and injected;
 * it is not a singleton, public Host API, or scheduler plugin ABI.
 */
class ExecutionService final : public ReadyTaskSubmissionRuntime {
 public:
  /**
   * @brief Returns bounded product defaults supplied by the composition root.
   * @return CPU, retained-memory, scratch, ready-entry, and ready-byte limits.
   * @throws Nothing.
   * @note Tests and alternate products may inject smaller isolated limits.
   */
  static ExecutionResourceLimits default_resource_limits() noexcept;

  /**
   * @brief Creates an unconfigured execution domain with no worker threads.
   * @throws std::bad_alloc if private pool-state ownership cannot allocate.
   * @note `configure_worker_count()` must freeze the worker count before the
   * first Run is submitted.
   */
  ExecutionService();

  /**
   * @brief Creates an unconfigured domain with explicit immutable limits.
   * @param resource_limits Complete private Host-composed limits.
   * @throws std::bad_alloc if private pool/ledger ownership cannot allocate.
   * @note The composition root must freeze workers before first Run admission.
   */
  explicit ExecutionService(ExecutionResourceLimits resource_limits);

  /**
   * @brief Creates and configures one fixed execution domain.
   * @param worker_count Zero for bounded hardware resolution or exact `[1,8]`.
   * @throws std::invalid_argument if the request exceeds eight.
   * @throws std::bad_alloc or std::system_error if pool construction fails.
   * @note The fixed pool is infrastructure. Individual Runs reserve CPU
   * execution rights and legacy scheduler owners reserve their own slots.
   */
  explicit ExecutionService(unsigned int worker_count);

  /**
   * @brief Creates a configured domain with explicit immutable limits.
   * @param worker_count Zero for bounded hardware resolution or exact `[1,8]`.
   * @param resource_limits Complete private Host-composed limits.
   * @throws std::invalid_argument if the worker request exceeds eight or the
   * configured CPU limit cannot permit the resolved fixed pool.
   * @throws std::bad_alloc or std::system_error if setup fails.
   */
  ExecutionService(unsigned int worker_count,
                   ExecutionResourceLimits resource_limits);

  /**
   * @brief Joins and releases service-owned CPU workers.
   * @throws Nothing.
   * @note Kernel and every injected ComputeService must already be drained.
   */
  ~ExecutionService() noexcept override;

  /**
   * @brief Prevents copying physical worker, ledger, and Run ownership.
   * @param other Service whose execution authority cannot be copied.
   * @throws Nothing because this operation is unavailable.
   */
  ExecutionService(const ExecutionService& other) = delete;

  /**
   * @brief Prevents assigning duplicate worker, ledger, and Run ownership.
   * @param other Service whose execution authority cannot be copied.
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  ExecutionService& operator=(const ExecutionService& other) = delete;

  /**
   * @brief Resolves and freezes the service worker count exactly once.
   *
   * @param worker_count Zero for bounded hardware resolution or exact `[1,8]`.
   * @return Nothing.
   * @throws std::invalid_argument for an out-of-range request or a positive
   * request that conflicts with the already fixed count.
   * @throws std::invalid_argument if the resolved pool exceeds configured CPU
   * capacity.
   * @throws std::bad_alloc or std::system_error if worker construction fails.
   * @note A later zero request preserves an existing fixed value and an equal
   * positive request is idempotent. Graph load, replacement, Run execution,
   * and dirty phases never resize the pool.
   */
  void configure_worker_count(unsigned int worker_count);

  /**
   * @brief Returns the fixed service worker count.
   * @return Zero before configuration, otherwise a value in `[1,8]`.
   * @throws std::system_error if private state locking fails.
   */
  unsigned int worker_count() const;

  /**
   * @brief Reports whether fixed workers have been configured.
   * @return True only after the complete worker pool is running.
   * @throws std::system_error if private state locking fails.
   */
  bool is_configured() const;

  /**
   * @brief Copies service CPU execution diagnostics.
   * @return Text containing fixed workers, active Runs, and queued work.
   * @throws std::bad_alloc if formatting storage cannot allocate.
   * @throws std::system_error if private state locking fails.
   * @note The snapshot grants no worker, queue, Run, or lifecycle authority.
   */
  std::string get_stats() const;

  /**
   * @brief Atomically reserves one legacy scheduler owner CPU vector.
   * @param worker_slots Conservative scheduler plan charge.
   * @return Move-only ledger reservation, or `std::nullopt` when capacity is
   * unavailable.
   * @throws std::bad_alloc or std::system_error from ledger admission.
   * @note Serial's zero-slot reservation remains active and move-only.
   */
  std::optional<ResourceLedger::Reservation>
  try_reserve_legacy_scheduler_workers(unsigned int worker_slots);

  /**
   * @brief Atomically reserves independent HP and RT scheduler owner vectors.
   * @param hp_worker_slots Conservative HP owner charge.
   * @param rt_worker_slots Conservative RT owner charge.
   * @return Two owners committed all-or-none, or `std::nullopt`.
   * @throws std::bad_alloc or std::system_error from ledger admission.
   */
  std::optional<ResourceLedger::ReservationPair>
  try_reserve_legacy_scheduler_worker_pair(unsigned int hp_worker_slots,
                                           unsigned int rt_worker_slots);

  /**
   * @brief Copies non-authoritative resource diagnostics.
   * @return Immutable limits and current root commitments.
   * @throws std::system_error from ledger snapshot locking.
   */
  ResourceLedger::Snapshot resource_snapshot() const;

  /**
   * @brief Calculates the exact structural vector used for CPU Run admission.
   * @param representative Any submission from the Run, used for immutable
   * graph-identity metadata capacity.
   * @param total_task_count Positive complete logical task count.
   * @param run_resource_demand Shared and per-task adapter declarations.
   * @return Checked CPU, retained, scratch, ready-entry, and ready-byte vector.
   * @throws std::invalid_argument for a nonpositive task count.
   * @throws std::logic_error before fixed worker configuration.
   * @throws GraphError when any structural aggregation overflows.
   * @note This diagnostic mints no authority. `execute_cpu_run()` uses the same
   * calculation before ledger admission, and initial/dependent queue entries
   * use the same resulting per-task envelope.
   */
  ResourceVector estimate_cpu_run_resources(
      const ReadyTaskSubmission& representative, int total_task_count,
      CpuRunResourceDemand run_resource_demand) const;

  /**
   * @brief Executes one complete ready-only CPU Run synchronously.
   *
   * @param host Active Graph runtime observation context, borrowed only until
   * the settled wait finishes.
   * @param initial_submissions Dispatcher-discovered initial ready values from
   * one Run; their values move into QueueEntry ownership and the caller-side
   * vector backing is retired before publication.
   * @param total_task_count Complete logical planned-task count.
   * @param run_resource_demand Shared once-per-Run bytes plus the uniform
   * additional declaration for every logical task.
   * @return Nothing after every callback in the batch settles.
   * @throws std::invalid_argument for invalid counts, empty active batches,
   * mixed Run ids, or duplicate active Run ids.
   * @throws std::logic_error before fixed pool configuration or for invalid
   * generic runtime use.
   * @throws GraphError with `GraphErrc::ComputeError` when checked aggregation
   * overflows or the ledger cannot admit the complete Run vector.
   * @throws std::bad_alloc or std::system_error from pool/store setup.
   * @throws The exact first worker task exception after settlement.
   * @note Independent calls may overlap. Run-local state is removed only after
   * queued work is retired and every in-flight callback has exited. Initial
   * QueueEntry construction completes transactionally before the moved-from
   * input vector is released, so neither that vector nor its backing spans the
   * settlement wait. No Graph, plan, dependency, result, or commit object
   * enters this method.
   */
  void execute_cpu_run(SchedulerHostContext& host,
                       std::vector<ReadyTaskSubmission> initial_submissions,
                       int total_task_count,
                       CpuRunResourceDemand run_resource_demand = {});

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
   * @param handles Borrowed-handle batch rejected without inspection.
   * @param total_task_count Declared batch size rejected without inspection.
   * @param priority Ordering hint rejected without inspection.
   * @return Nothing.
   * @throws std::logic_error unconditionally.
   * @note Migrated work must enter as `ReadyTaskSubmission`.
   */
  void submit_initial_task_handles(std::vector<TaskHandle>&& handles,
                                   int total_task_count,
                                   SchedulerTaskPriority priority) override;

  /**
   * @brief Rejects borrowed task handles released by workers.
   * @param handles Borrowed-handle batch rejected without inspection.
   * @param priority Ordering hint rejected without inspection.
   * @return Nothing.
   * @throws std::logic_error unconditionally.
   * @note Dispatcher dependents re-enter as `ReadyTaskSubmission`.
   */
  void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      SchedulerTaskPriority priority) override;

  /**
   * @brief Rejects anonymous raw callback submission.
   * @param task Anonymous callback rejected without execution.
   * @param priority Ordering hint rejected without inspection.
   * @param epoch Optional epoch rejected without inspection.
   * @return Nothing.
   * @throws std::logic_error unconditionally.
   * @note Every accepted migrated callback must carry explicit Run identity and
   * lease ownership in `ReadyTaskSubmission`.
   */
  void submit_ready_task_any_thread(
      Task&& task, SchedulerTaskPriority priority,
      std::optional<std::uint64_t> epoch) override;

  /**
   * @brief Rejects unscoped generic completion waits.
   * @return Nothing.
   * @throws std::logic_error unconditionally.
   * @note `execute_cpu_run()` owns the caller-side wait for one explicit Run;
   * a generic wait has no unambiguous Run state in the multi-Run domain.
   */
  void wait_for_completion() override;

  /**
   * @brief Publishes an exact exception into the current worker's Run fence.
   * @param error Non-null worker exception identity.
   * @return Nothing.
   * @throws std::logic_error outside a service worker callback.
   * @note A null exception is ignored without resolving worker context.
   */
  void set_exception(std::exception_ptr error) override;

  /**
   * @brief Adds logical work to the current worker's Run completion count.
   * @param delta Positive work count.
   * @return Nothing.
   * @throws std::invalid_argument when `delta` is not positive.
   * @throws std::logic_error outside a service worker callback.
   * @throws std::overflow_error when CPU completion accounting overflows.
   */
  void inc_tasks_to_complete(int delta) override;

  /**
   * @brief Retires one logical task from the current worker's Run.
   * @return Nothing.
   * @throws std::logic_error outside a service worker callback or when the
   * completion count would underflow.
   */
  void dec_tasks_to_complete() override;

  /**
   * @brief Publishes one current-worker Run trace.
   * @param action Stable trace action.
   * @param node_id Planned Graph node id.
   * @return Nothing.
   * @throws std::logic_error outside a service worker callback.
   */
  void log_event(SchedulerTraceAction action, int node_id) override;

 private:
  friend class ::ps::testing::ExecutionServiceTestAccess;

  /** @brief Per-Run completion, failure, trace, and settlement state. */
  struct RunState;

  /** @brief One owned ready queue entry paired with matching Run state. */
  struct QueueEntry;

  /** @brief Entry/byte-bounded owner of high and normal ready lanes. */
  class BoundedReadyStore;

  /** @brief Private worker, store, registry, and ledger ownership. */
  class PoolState;

  /** @brief Checked root vector and uniform child-grant envelopes. */
  struct CpuRunAdmissionEstimate;

  /**
   * @brief Calculates mandatory bytes for one service-owned submission.
   * @param graph_identity Stable copied metadata string.
   * @return Queue, shared-control, store-handle, and string envelope bytes.
   * @throws GraphError when checked structural arithmetic overflows.
   * @note String storage uses the copied value's actual capacity plus its null
   * terminator, not logical character count.
   */
  static std::uint64_t service_submission_envelope_bytes(
      const std::string& graph_identity);

  /**
   * @brief Calculates mandatory once-per-Run service retained bytes.
   * @return Run, registry, shared-control, and reservation-state bytes.
   * @throws GraphError when checked structural arithmetic overflows.
   */
  static std::uint64_t service_run_envelope_bytes();

  /**
   * @brief Builds one checked service-plus-adapter admission calculation.
   * @param configured_workers Frozen positive service worker count.
   * @param graph_identity Stable metadata copied by every logical task.
   * @param total_task_count Positive complete task count.
   * @param demand Shared and uniform per-task adapter declaration.
   * @return Root vector and uniform ready/execution child envelopes.
   * @throws std::invalid_argument for a nonpositive task count.
   * @throws GraphError when any structural arithmetic overflows.
   */
  static CpuRunAdmissionEstimate calculate_cpu_run_admission(
      unsigned int configured_workers, const std::string& graph_identity,
      int total_task_count, CpuRunResourceDemand demand);

  /**
   * @brief Runs the shared worker loop until service shutdown.
   * @param worker_id Stable zero-based id in the fixed service pool.
   * @return Nothing.
   * @throws Nothing; task exceptions are routed into matching Run state.
   */
  void worker_loop(int worker_id) noexcept;

  /**
   * @brief Enqueues one owned ready submission for a registered Run.
   * @param run Matching active Run state.
   * @param submission Ready value transferred into service queue ownership.
   * @return Nothing.
   * @throws std::invalid_argument when Run identity does not match.
   * @throws GraphError when declared or reserved ready capacity is invalid.
   * @throws std::logic_error when the Run or service no longer accepts work.
   * @throws std::bad_alloc if store ownership cannot allocate.
   */
  void enqueue_submission(const std::shared_ptr<RunState>& run,
                          ReadyTaskSubmission submission);

  /**
   * @brief Grants and owns one uniformly estimated ready queue entry.
   * @param run Matching active Run whose envelope was admitted.
   * @param submission Initial or dependency-released submission to transfer.
   * @return Shared queue entry carrying the exact ready grant.
   * @throws std::invalid_argument for mismatched Run identity.
   * @throws GraphError for demand mismatch or unavailable reserved capacity.
   * @throws std::bad_alloc when queue-entry ownership cannot allocate.
   * @note Initial and dependent publication both use this helper so neither
   * path can bypass or recompute a different ready-byte envelope.
   */
  std::shared_ptr<QueueEntry> make_queue_entry(
      const std::shared_ptr<RunState>& run, ReadyTaskSubmission submission);

  /**
   * @brief Releases moved-from initial-batch storage before Run publication.
   * @param submissions Initial values already transferred into QueueEntry
   * ownership.
   * @return Nothing.
   * @throws Nothing.
   * @note The empty-vector swap deterministically transfers the complete
   * caller-side backing allocation into a temporary that is destroyed before
   * active-Run publication or settlement waiting. Callers invoke this only
   * after every initial QueueEntry and ready grant has been staged
   * successfully; earlier exceptions unwind the original vector normally.
   */
  static void release_initial_submission_storage(
      std::vector<ReadyTaskSubmission>& submissions) noexcept;

  /**
   * @brief Claims one Run's first failure and retires its queued work.
   * @param run Matching Run state retained through cleanup.
   * @param failure Exact non-null worker exception.
   * @return Nothing.
   * @throws Nothing; worker failure transport cannot be replaced by cleanup.
   */
  void fail_run(const std::shared_ptr<RunState>& run,
                std::exception_ptr failure) noexcept;

  /**
   * @brief Resolves a registered Run from one submission identity.
   * @param run_id Run namespace carried by an owned submission.
   * @return Strong active Run state.
   * @throws std::invalid_argument when no matching active Run exists.
   * @throws std::system_error if private state locking fails.
   */
  std::shared_ptr<RunState> find_active_run(ComputeRunId run_id);

  /**
   * @brief Returns the Run currently executing on this service worker.
   * @return Borrowed Run state valid for the callback interval.
   * @throws std::logic_error outside a service worker callback.
   */
  static RunState& current_worker_run();

  /** @brief Private fixed-pool, ready-store, registry, and ledger owner. */
  std::unique_ptr<PoolState> pool_;

  /** @brief Current service-worker Run context, null outside callbacks. */
  static thread_local RunState* tls_run_state_;

  /** @brief Current fixed worker id, or -1 outside service callbacks. */
  static thread_local int tls_worker_id_;
};

}  // namespace ps::compute
