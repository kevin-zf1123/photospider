#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "compute/compute_run.hpp"
#include "execution/execution_task_runtime.hpp"
#include "runtime/resource_ledger.hpp"

namespace ps {
class ExecutionHostContext;
enum class PolicyClass;
struct HostPolicyConfig;
struct PolicyInfoSnapshot;

namespace policy {
class PolicyBinding;
class PolicyRegistry;
}  // namespace policy

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
 * This source-tree type carries every ledger dimension explicitly and converts
 * to the authority-neutral `ResourceVector` only when constructing the private
 * service.
 *
 * @throws Nothing for aggregate construction and conversion.
 * @note This is not an installed policy API or execution extension surface.
 */
struct ExecutionResourceLimits final {
  /** @brief Concurrent Run CPU execution rights. */
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
   * @brief Capacity excluded from Throughput Run admission.
   *
   * Throughput Runs must leave this component-wise subset available.
   * Explicitly Interactive Runs may consume it only after the ordinary ledger
   * transaction validates complete remaining capacity. Other private resource
   * owners continue to use the full ledger limits established by Issue #70.
   *
   * @note This value is an admission ceiling, not a reservation or token.
   * Zero preserves the full ledger limit for every service class.
   */
  ResourceVector interactive_headroom;

  /**
   * @brief Converts the complete private limit set to ledger dimensions.
   * @return Value-only capacity vector with the five ledger dimensions.
   * @throws Nothing.
   * @note Headroom is policy configuration over these limits and is not a
   * second ledger dimension.
   */
  ResourceVector resource_vector() const noexcept {
    return ResourceVector{cpu_slots, retained_memory_bytes, scratch_bytes,
                          ready_entries, ready_bytes};
  }
};

/**
 * @brief Immutable adapter-owned resource demand for one ready submission.
 *
 * The byte fields declare only ownership in addition to the mandatory service
 * envelope. Retained memory and scratch are held while the callback executes.
 * Ready bytes are held while the owned submission resides in the bounded
 * service store. Positive work units are ordering-only trusted estimates.
 * `ExecutionService` always adds its `RunState`, queue entry, metadata,
 * shared-control, ready-entry, and CPU execution charges.
 *
 * @throws Nothing for value construction and comparison.
 * @note Zero scratch means the current host contract declares no separately
 * metered scratch. It does not claim that backend, plugin, or runtime
 * allocation was measured as zero. Zero ready bytes remains valid because the
 * service structural ready envelope is always positive. Zero work is invalid
 * and is rejected before ready publication.
 */
struct ReadyTaskResourceDemand final {
  /** @brief Additional adapter bytes retained during callback execution. */
  std::uint64_t retained_memory_bytes = 0U;

  /** @brief Additional Host-declared temporary execution bytes. */
  std::uint64_t scratch_bytes = 0U;

  /** @brief Additional adapter bytes occupied while submission is ready. */
  std::uint64_t ready_bytes = 0U;

  /**
   * @brief Positive trusted estimate used only for fair policy service.
   * @note One is the conservative default. This abstract unit does not claim
   * measured callback duration and never changes ledger grant dimensions.
   */
  std::uint64_t work_units = 1U;
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
 * @return Equal retained/ready charges, zero scratch, and one work unit.
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
 * Work units are not ledger capacity; policy charges them when selecting each
 * ready submission. Mandatory service structural envelopes are added
 * independently.
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
   * @brief Returns the strong Graph instance identity.
   * @return Identity copied from the matching Run descriptor.
   * @throws Nothing.
   */
  GraphInstanceId graph_instance_id() const noexcept {
    return graph_instance_id_;
  }

  /**
   * @brief Returns the authoritative submission revision.
   * @return Revision copied from the matching Run descriptor.
   * @throws Nothing.
   */
  GraphRevision revision() const noexcept { return revision_; }

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

  /** @brief Strong Graph instance identity copied before execution. */
  GraphInstanceId graph_instance_id_;

  /** @brief Authoritative revision copied before execution. */
  GraphRevision revision_;

  /** @brief Request target node retained for immutable diagnostics. */
  int target_node_id_ = -1;

  /** @brief Single-domain intent independent from QoS. */
  ComputeIntent intent_ = ComputeIntent::GlobalHighPrecision;

  /** @brief Requested output quality independent from intent. */
  ComputeRunQuality quality_ = ComputeRunQuality::Full;

  /** @brief Copied policy inputs that mint no resource authority. */
  ComputeRunQos qos_;

  /** @brief Planned node id used only for execution-trace publication. */
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
                                        ExecutionTaskRuntime& task_runtime)>;

  /**
   * @brief Creates one dependency-ready owned submission.
   * @param lease Strong Run lease transferred into submission ownership.
   * @param identity Composite Run/local task identity.
   * @param trace_node_id Planned node used for execution diagnostics.
   * @param is_initial_ready Whether the dispatcher selected initial readiness.
   * @param executable Owned operation invoked only after service admission.
   * @param priority Private high/normal ready-store ordering hint.
   * @param resource_demand Trusted immutable retained, scratch, ready-byte,
   * and positive work declaration.
   * @throws std::invalid_argument when executable is empty or the identity Run
   * differs from the lease Run.
   * @throws std::bad_alloc when metadata or executable storage allocates.
   * @note Readiness is a caller-established precondition. Construction does
   * not inspect a task graph or dependency map.
   */
  ReadyTaskSubmission(
      ComputeRunLease lease, ComputeRunTaskIdentity identity, int trace_node_id,
      bool is_initial_ready, Executable executable,
      ExecutionTaskPriority priority = ExecutionTaskPriority::Normal,
      ReadyTaskResourceDemand resource_demand = default_resource_demand());

  /**
   * @brief Returns the empty adapter-owned demand.
   * @return Zero additional bytes and one abstract work unit.
   * @throws Nothing.
   * @note `ExecutionService` always charges its positive mandatory submission
   * envelope. Adapters with owned captures or scratch must pass an explicit
   * declaration instead of relying on this byte-only zero-addition default.
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
   * @return High for same-Run preference, otherwise Normal.
   * @throws Nothing.
   * @note Priority does not establish dependency correctness or global
   * fairness or service class. The dispatcher must already have established
   * readiness, and explicit Run QoS selects the built-in policy.
   */
  ExecutionTaskPriority priority() const noexcept { return priority_; }

  /**
   * @brief Returns the immutable trusted-host resource declaration.
   * @return Per-submission retained, scratch, ready bytes, and work units.
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
   * @throws Any Run cancellation-observation synchronization exception after
   * the same best-effort failure publication.
   * @note Cancellation observed before entry skips the executable entirely.
   * Cancellation observed after a normal executable return is retained by the
   * Run and completed by the outer service worker; neither observation is
   * converted into a fabricated task exception. The submission and runtime
   * must remain alive until this call returns.
   */
  void execute(ExecutionTaskRuntime& task_runtime);

 private:
  friend class ExecutionService;

  /** @brief Immutable request and trace metadata copied before queueing. */
  ReadyTaskMetadata metadata_;

  /** @brief Composite identity validated against lease ownership. */
  ComputeRunTaskIdentity identity_;

  /** @brief Non-forgeable owner retaining matching Run-local state. */
  ComputeRunLease lease_;

  /** @brief Owned operation containing no borrowed dispatcher stack state. */
  Executable executable_;

  /** @brief Private ready-queue hint that carries no dependency authority. */
  ExecutionTaskPriority priority_ = ExecutionTaskPriority::Normal;

  /** @brief Immutable host-authored resources checked at Run admission. */
  ReadyTaskResourceDemand resource_demand_;
};

/**
 * @brief Private execution runtime that admits only ready submission values.
 *
 * TaskSubmissionPlan uses this extension after dependency-ready detection.
 * No installed extension ABI can construct or retain this source-tree value.
 *
 * @throws Concrete implementations define synchronization and allocation
 * failures.
 * @note Inherited raw callback and borrowed-handle entry points remain present
 * only to implement `ExecutionTaskRuntime`; the current ExecutionService
 * rejects them.
 */
class ReadyTaskSubmissionRuntime : public ExecutionTaskRuntime {
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
 * policy-aware entry/byte-bounded ready store. Two private policy strategies
 * rank explicit interactive/throughput QoS with checked work/byte service,
 * Graph/Run fairness, dispatch aging, admission headroom, and bounded batch
 * progress. Policies own no physical or resource authority. The service owns
 * no planning, dependency, Graph/cache, dirty propagation, visible commit, or
 * final lifecycle-registry authority.
 *
 * @throws std::bad_alloc or std::system_error from explicit execution setup.
 * @note This private source-tree service is explicitly composed and injected;
 * it is not a singleton, public Host API, or execution plugin ABI.
 */
class ExecutionService final : public ReadyTaskSubmissionRuntime {
 public:
  /**
   * @brief Returns bounded product defaults supplied by the composition root.
   * @return Ledger limits plus protected interactive admission headroom.
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
   * @throws std::invalid_argument if interactive headroom exceeds a limit.
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
   * execution rights and other private owners reserve their own slots.
   */
  explicit ExecutionService(unsigned int worker_count);

  /**
   * @brief Creates a configured domain with explicit immutable limits.
   * @param worker_count Zero for bounded hardware resolution or exact `[1,8]`.
   * @param resource_limits Complete private Host-composed limits.
   * @throws std::invalid_argument if the worker request exceeds eight, the
   * configured CPU limit cannot permit the resolved fixed pool, or interactive
   * headroom exceeds a ledger limit.
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
   * @brief Copies canonical types from the process policy registry.
   * @return Lexically sorted built-in and DSO policy names.
   * @throws std::bad_alloc or `std::system_error` from copied observation.
   */
  std::vector<std::string> policy_available_types() const;

  /**
   * @brief Copies one process policy type description.
   * @param type_name Canonical registered type name.
   * @return Bounded Host-owned description.
   * @throws GraphError with `NotFound` when unavailable.
   * @throws std::bad_alloc when copied storage exhausts memory.
   */
  std::string policy_description(const std::string& type_name) const;

  /**
   * @brief Scans caller-ordered directories for complete policy DSOs.
   * @param directories Directory paths processed in caller order.
   * @return Number of complete DSOs atomically published.
   * @throws The exact policy registry scan errors.
   */
  std::size_t policy_scan(const std::vector<std::string>& directories);

  /**
   * @brief Loads one policy DSO transaction.
   * @param path Nonempty candidate library path.
   * @return Nothing after all-or-none publication.
   * @throws The exact policy registry load errors.
   */
  void policy_load(const std::string& path);

  /**
   * @brief Copies visible policy DSO labels.
   * @return Globally nondecreasing Host-owned paths.
   * @throws std::bad_alloc or `std::system_error` from copied observation.
   */
  std::vector<std::string> policy_loaded_plugins() const;

  /**
   * @brief Atomically replaces both class bindings.
   * @param config Canonical Interactive and Throughput type names.
   * @return Nothing after one nonallocating two-binding publication.
   * @throws GraphError for invalid/unsupported/create/generation failure.
   * @throws std::bad_alloc for Host or synchronous plugin setup OOM.
   * @note Both contexts are prepared without the service/store/ledger/Run lock.
   * Failure preserves both published bindings and generations.
   */
  void configure_policy_defaults(const HostPolicyConfig& config);

  /**
   * @brief Copies one class binding and immutable first fault.
   * @param policy_class Interactive or Throughput.
   * @return Complete process execution-domain policy snapshot.
   * @throws GraphError for an invalid enum and `std::bad_alloc` while copying.
   */
  PolicyInfoSnapshot policy_info(PolicyClass policy_class) const;

  /**
   * @brief Replaces exactly one class binding.
   * @param policy_class Interactive or Throughput.
   * @param type Canonical registered type supporting the class.
   * @return Nothing after a nonallocating binding publication.
   * @throws GraphError or `std::bad_alloc` from validation/preparation.
   * @note Success advances to a unique nonzero generation even for equal type.
   */
  void replace_policy(PolicyClass policy_class, const std::string& type);

  /**
   * @brief Returns the fixed private physical route vocabulary.
   * @return Exactly `cpu`, `gpu_pipeline`, `serial_debug` in lexical order.
   * @throws std::bad_alloc when result storage exhausts memory.
   */
  static std::vector<std::string> available_execution_types();

  /**
   * @brief Copies one private route description.
   * @param type_name Exact private route name.
   * @return Host-owned display text.
   * @throws GraphError with `NotFound` for an unknown or removed alias.
   */
  static std::string execution_description(const std::string& type_name);

  /**
   * @brief Tests the exact private route vocabulary without allocation.
   * @param type_name Candidate route string.
   * @return True only for `cpu`, `gpu_pipeline`, or `serial_debug`.
   * @throws Nothing.
   */
  static bool is_execution_type(const std::string& type_name) noexcept;

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
   * @note This diagnostic mints no authority. `execute_run()` uses the same
   * calculation before ledger admission, and initial/dependent queue entries
   * use the same resulting per-task envelope. Ordering-only work units and
   * interactive headroom do not change this diagnostic resource vector.
   */
  ResourceVector estimate_cpu_run_resources(
      const ReadyTaskSubmission& representative, int total_task_count,
      CpuRunResourceDemand run_resource_demand) const;

  /**
   * @brief Executes one complete ready-only Run on a private physical route.
   *
   * @param host Active Graph runtime observation context, borrowed only until
   * the settled wait finishes.
   * @param execution_type Exact private route selected by the Graph binding.
   * @param initial_submissions Dispatcher-discovered initial ready values from
   * one Run; their values move into QueueEntry ownership and the caller-side
   * vector backing is retired before publication.
   * @param total_task_count Complete logical planned-task count.
   * @param run_resource_demand Shared once-per-Run bytes plus the uniform
   * additional declaration for every logical task.
   * @return Nothing after every callback in the batch settles.
   * @throws std::invalid_argument for invalid counts, empty active batches,
   * mixed Run ids, or zero work.
   * @throws std::logic_error before fixed pool configuration, for duplicate
   * active Run ids, or for invalid generic runtime use.
   * @throws GraphError with `GraphErrc::ComputeError` when checked aggregation
   * or policy cost overflows, or the ledger/policy ceiling cannot admit the
   * complete Run vector.
   * @throws std::bad_alloc or std::system_error from pool/store setup.
   * @throws The exact first worker task exception after settlement.
   * @note Independent calls may overlap. Run-local state is removed only after
   * queued work is retired and every in-flight callback has exited. Accepted
   * cancellation purges only this exact Run's queued entries, rejects later
   * dependent publication, and settles only after its running callbacks drain;
   * cancellation already visible before service admission returns without
   * publishing an active Run. Initial QueueEntry construction completes
   * transactionally before the moved-from input vector is released, so neither
   * that vector nor its backing spans the settlement wait. No Graph, plan,
   * dependency, result, or commit object enters this method.
   */
  void execute_run(ExecutionHostContext& host,
                   const std::string& execution_type,
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
  void submit_initial_task_handles(std::vector<ExecutionTaskHandle>&& handles,
                                   int total_task_count,
                                   ExecutionTaskPriority priority) override;

  /**
   * @brief Rejects borrowed task handles released by workers.
   * @param handles Borrowed-handle batch rejected without inspection.
   * @param priority Ordering hint rejected without inspection.
   * @return Nothing.
   * @throws std::logic_error unconditionally.
   * @note Dispatcher dependents re-enter as `ReadyTaskSubmission`.
   */
  void submit_ready_task_handles_from_worker(
      std::vector<ExecutionTaskHandle>&& handles,
      ExecutionTaskPriority priority) override;

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
      Task&& task, ExecutionTaskPriority priority,
      std::optional<std::uint64_t> epoch) override;

  /**
   * @brief Rejects unscoped generic completion waits.
   * @return Nothing.
   * @throws std::logic_error unconditionally.
   * @note `execute_run()` owns the caller-side wait for one explicit Run;
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
  void log_event(ExecutionTraceAction action, int node_id) override;

 private:
  friend class ::ps::testing::ExecutionServiceTestAccess;

  /** @brief Per-Run completion, failure, trace, and settlement state. */
  struct RunState;

  /** @brief One owned ready queue entry paired with matching Run state. */
  struct QueueEntry;

  /** @brief Authority-free strategy for one explicit QoS service class. */
  class BuiltinPolicy;

  /** @brief Deadline-aware strategy allowed to consume protected headroom. */
  class InteractiveBuiltinPolicy;

  /** @brief Weighted deterministic strategy confined to general capacity. */
  class ThroughputBuiltinPolicy;

  /** @brief Policy-aware entry/byte-bounded owner of all ready entries. */
  class BoundedReadyStore;

  /** @brief Private worker, store, registry, and ledger ownership. */
  class PoolState;

  /** @brief Checked root vector and uniform child-grant envelopes. */
  struct CpuRunAdmissionEstimate;

  /**
   * @brief Test-only decision at the reserved-start rollback boundary.
   * @param context Opaque repository-test state.
   * @param candidate_id Nonzero ready-entry identity retained by the pin.
   * @param entry_version Nonzero nonreused entry version retained by the pin.
   * @param route_generation Immutable Run route generation retained by the pin.
   * @param execution_resources Exact child grant staged for this start.
   * @return True to abort this attempt as obsolete before every route, ready,
   * fairness, or in-flight mutation; false to continue production commit.
   * @throws Nothing; observers must be allocation-free and noexcept.
   * @note The bridge definition lives under tests/support. Production leaves
   * this pointer null, and no public or installed API exposes the seam.
   */
  using ReservedStartRollbackObserver =
      bool (*)(void* context, std::uint64_t candidate_id,
               std::uint64_t entry_version, std::uint64_t route_generation,
               const ResourceVector& execution_resources) noexcept;

  /**
   * @brief Test-only observer invoked at the initial-storage retirement seam.
   *
   * @param context Opaque repository-test context installed by the private
   * test bridge.
   * @param admitted_resources Complete checked vector already reserved for
   * this Run.
   * @param staged_size Moved-from element count before storage retirement.
   * @param staged_capacity Backing capacity before storage retirement.
   * @param released_size Element count after the empty-vector swap.
   * @param released_capacity Backing capacity after the empty-vector swap.
   * @return Nothing.
   * @throws Nothing; observers must be allocation-free and noexcept.
   * @note The bridge definition lives under tests/support. Production leaves
   * this pointer null, and no public or installed API exposes the seam.
   */
  using InitialSubmissionStorageObserver = void (*)(
      void* context, const ResourceVector& admitted_resources,
      std::size_t staged_size, std::size_t staged_capacity,
      std::size_t released_size, std::size_t released_capacity) noexcept;

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
   * @param graph_identity Pre-copied policy Graph identity retained by Run.
   * @param graph_key Pre-copied conservative Graph-map key allocation.
   * @return Run, registry, shared-control, and reservation-state bytes.
   * @throws GraphError when checked structural arithmetic overflows.
   */
  static std::uint64_t service_run_envelope_bytes(
      const std::string& graph_identity, const std::string& graph_key);

  /**
   * @brief Calculates one checked ordering-only policy service cost.
   * @param demand Trusted positive work declaration.
   * @param complete_ready_bytes Exact ready grant bytes including service
   * envelope.
   * @return Work units plus started 4096-byte ready quanta.
   * @throws std::invalid_argument when work units are zero.
   * @throws GraphError when byte rounding or cost addition overflows.
   * @note The returned scalar never mints or changes resource authority.
   */
  static std::uint64_t calculate_policy_service_cost(
      ReadyTaskResourceDemand demand, std::uint64_t complete_ready_bytes);

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

  /** @brief Ready-byte conversion unit used by both built-in policies. */
  static constexpr std::uint64_t kPolicyReadyByteQuantum = 4096U;

  /** @brief Successful dispatches after which one waiter becomes aged. */
  static constexpr std::uint64_t kPolicyAgingDispatches = 8U;

  /** @brief Maximum interactive selections while throughput remains ready. */
  static constexpr std::uint64_t kInteractiveBurstLimit = 3U;

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
   * @brief Reports deterministic initial-storage retirement to tests.
   * @param admitted_resources Complete checked Run vector already reserved.
   * @param staged_size Moved-from element count before retirement.
   * @param staged_capacity Backing capacity before retirement.
   * @param submissions Initial vector after production retirement.
   * @return Nothing.
   * @throws Nothing.
   * @note This method runs after all QueueEntry/grant staging and before the
   * active-Run index, bounded ready store, worker notification, or settlement
   * wait. A null observer is the production default.
   */
  void observe_initial_submission_storage(
      const ResourceVector& admitted_resources, std::size_t staged_size,
      std::size_t staged_capacity,
      const std::vector<ReadyTaskSubmission>& submissions) const noexcept;

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
   * @brief Closes one exact Run admission and purges only its queued entries.
   * @param run Matching Run state retained through cleanup.
   * @param reason Stable cancellation reason already accepted by ComputeRun.
   * @return Nothing.
   * @throws Nothing; cancellation cleanup cannot replace terminal ownership.
   * @note Acquires the pool mutex before the Run mutex, matching publication,
   * failure cleanup, and worker dequeue. In-flight callbacks remain counted
   * until their actual exit and are never synchronously interrupted.
   */
  void cancel_run(const std::shared_ptr<RunState>& run,
                  ComputeRunCancellationReason reason) noexcept;

  /**
   * @brief Resolves a registered Run from one submission identity.
   * @param run_id Run namespace carried by an owned submission.
   * @return Strong active Run state.
   * @throws std::invalid_argument when no matching active Run exists.
   * @throws std::system_error if private state locking fails.
   */
  std::shared_ptr<RunState> find_active_run(ComputeRunId run_id);

  /**
   * @brief Copies the built-in Throughput admission charge for repository
   * tests.
   * @return Exact active Throughput root vectors, excluding Interactive
   * reservations.
   * @throws std::system_error when private accounting locking fails.
   * @note This private diagnostic mints no authority and is exposed only
   * through `ExecutionServiceTestAccess`, never an installed API.
   */
  ResourceVector throughput_reservation_snapshot_for_testing() const;

  /**
   * @brief Returns the Run currently executing on this service worker.
   * @return Borrowed Run state valid for the callback interval.
   * @throws std::logic_error outside a service worker callback.
   */
  static RunState& current_worker_run();

  /** @brief Private fixed-pool, ready-store, registry, and ledger owner. */
  std::unique_ptr<PoolState> pool_;

  /**
   * @brief Optional repository-test callback for the retirement boundary.
   *
   * @note Installed only before isolated test execution and cleared after the
   * synchronous Run settles; production composition leaves it null.
   */
  InitialSubmissionStorageObserver initial_submission_storage_observer_ =
      nullptr;

  /**
   * @brief Opaque test context paired with the optional observer.
   *
   * @note The private test bridge guarantees this context outlives every Run
   * that can invoke the observer.
   */
  void* initial_submission_storage_observer_context_ = nullptr;

  /**
   * @brief Optional repository-test callback after child-grant staging.
   * @note A true result exercises normal RAII grant rollback before any
   * observable start mutation. Production composition leaves this null.
   */
  ReservedStartRollbackObserver reserved_start_rollback_observer_ = nullptr;

  /**
   * @brief Opaque context paired with the reserved-start observer.
   * @note The private bridge guarantees lifetime through the observed Run.
   */
  void* reserved_start_rollback_observer_context_ = nullptr;

  /** @brief Current service-worker Run context, null outside callbacks. */
  static thread_local RunState* tls_run_state_;

  /** @brief Current fixed worker id, or -1 outside service callbacks. */
  static thread_local int tls_worker_id_;
};

}  // namespace ps::compute
