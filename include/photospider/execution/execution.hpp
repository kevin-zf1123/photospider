#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "photospider/compiler/compiler.hpp"
#include "photospider/core/status.hpp"
#include "photospider/data/input_snapshot.hpp"
#include "photospider/data/value.hpp"
#include "photospider/execution/cancellation.hpp"
#include "photospider/execution/dependencies.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "photospider/execution/resources.hpp"

namespace ps {
namespace execution_internal {
struct DemandCoordinator;
}

/**
 * @brief Fixed local resource configuration for one ExecutionContext.
 *
 * @note Configuration has process-local meaning and contains no daemon quota or
 * remote capacity.
 */
struct PHOTOSPIDER_API ExecutionContextConfig final {
  /** @brief Fixed CPU worker count; zero resolves to bounded hardware count. */
  std::uint32_t cpu_workers = 0;
  /** @brief Whether to create the configured native Metal or Vulkan device. */
  bool gpu_enabled = false;
  /**
   * @brief Single aggregate waiting-callback limit across CPU/GPU lanes.
   * @note A callback releases its slot when a worker starts it; running
   * callbacks do not consume this ExecutionContext-wide bound. A staged CPU
   * job holds one slot until its tile callbacks retire and it leaves the queue.
   */
  std::uint32_t maximum_queued_tasks = 1024;
  /** @brief Maximum reserved/allocated controlled computation buffer bytes. */
  std::uint64_t maximum_live_bytes = 256U * 1024U * 1024U;
  /** @brief Optional completed-result retention sublimit; zero disables it.
   * Same-snapshot exact demand Flights still share active computations.
   */
  std::uint64_t result_cache_bytes = 0;
  /** @brief Maximum live context-managed demand handles, 1..65536. */
  std::uint32_t maximum_demands = 1024;
  /** @brief Maximum live Result checkpoint scopes, 1..1048576.
   * @note Structured Result shared-producer entries and waiters use separate
   * Root Entries, Host and Metadata admission and do not consume this limit.
   */
  std::uint64_t maximum_result_checkpoint_scopes = 65536;
  /** @brief Retained dependency-cache proof units, 1..1048576.
   * @note Counts actual record owners, row/tag/coordinate storage and source
   * witnesses per manifest; shared owners in one manifest count once. Optional
   * exhaustion skips retention. Independent from the pixel allocation limit.
   */
  std::uint64_t maximum_dependency_cache_metadata = 65536;
  /** @brief Optional explicit limits for the context's managed resource root.
   * The context creates a root with default ResourceLimits when this is unset.
   * Existing maximum_live_bytes is also enforced as a Payload sublimit. Limits
   * cover instrumented resources, not uninstrumented legacy metadata, external
   * allocator/OS overhead, or RSS.
   */
  std::optional<ResourceLimits> managed_resources = {};
  /** @brief Enables monotonic timing for accepted CPU/GPU FIFO callbacks.
   * Default-off measurement reads three clocks per accepted/started callback.
   * The fixed envelope timestamp is present in both modes. Range-block claims
   * and work before pool submission are outside these observations.
   */
  bool collect_scheduler_timing = false;
};

/** @brief Cumulative accepted-callback observations for one backend FIFO.
 * @note Counters saturate at UINT64_MAX and set saturated. Counts and sums can
 * be differenced between unsaturated snapshots; maxima span context lifetime,
 * including warmup. These observations measure callbacks rather than range
 * blocks, requests, device commands or an execution-time decomposition.
 */
struct PHOTOSPIDER_API CallbackQueueStatistics final {
  std::uint64_t accepted_callbacks = 0, started_callbacks = 0;
  /** @brief Sum from submit entry to successful publication, including mutex
   * acquisition and queue storage. Failed submissions are excluded.
   */
  std::uint64_t submission_ns = 0;
  /** @brief Sum from successful publication to worker removal from the FIFO.
   * Admission-token retirement and callback execution follow that boundary.
   */
  std::uint64_t queue_wait_ns = 0;
  std::uint64_t maximum_queue_wait_ns = 0, maximum_queued_callbacks = 0;
  bool saturated = false;
};
/** @brief Context-local scheduler observations, separately synchronized per
 * lane. Disabled collection returns enabled=false and zero-valued lanes.
 */
struct PHOTOSPIDER_API SchedulerStatistics final {
  bool enabled = false;
  CallbackQueueStatistics cpu, gpu;
};

/** @brief One exact-name immutable Result input supplied to a Run.
 * The Result schema must match its workflow declaration.
 */
struct PHOTOSPIDER_API ExecutionBinding final {
  /** @brief Case-sensitive required declaration name. */
  std::string name;
  /** @brief Structured Result binding matching the declared schema. */
  ResultRef result = {};
};
/**
 * @brief Per-call input snapshot; duplicate entries remain visible to
 * validation.
 * @note Names and Result references are copied by execute; Result storage keeps
 * shared immutable ownership. Caller containers must not be modified during
 * copying. A Run retains its binding snapshot until admitted callbacks retire.
 * Input payloads never enter compiler/cache identity.
 */
struct PHOTOSPIDER_API ExecutionBindings final {
  /** @brief 0..4096 entries; every declaration must occur exactly once. */
  std::vector<ExecutionBinding> inputs;
};

/**
 * @brief Per-execution scheduling controls.
 *
 * @note A zero parallelism resolves to the ExecutionContext CPU worker count.
 */
struct PHOTOSPIDER_API ExecutionOptions final {
  /** @brief Maximum in-flight plan steps for this Run. */
  std::uint32_t maximum_parallelism = 0;
  /** @brief Per-session discovery/metadata limits for dependency plans. */
  DependencyLimits dependencies = {};
  /** @brief Run-wide bound on demand records and stage transitions. */
  std::uint64_t maximum_dependency_work = 1048576;
  /** @brief Separate optional cache-proof traversal/sample budget per Run.
   * @note One shared budget precharges structural traversal, metadata copies,
   * source sample/facet hashing, internal block keys and finite normalization.
   * Optional exhaustion skips verification/retention without failing
   * computation. Zero disables dependency cache verification and retention for
   * that Run.
   */
  std::uint64_t maximum_dependency_cache_work = 1048576;
  /** @brief Group already-ready Atomic outputs with optional CPU joint code. */
  bool enable_joint = true;
  /** @brief Maximum explicit Result/temporary read or write payload, positive.
   * Internal backing windows are operation-defined and admitted against root
   * capacity; this bound does not split those requests.
   */
  std::uint64_t maximum_result_window_bytes = 4096;
  /** @brief Collected atom observations for execute_atoms, at most 65536.
   * Zero permits only an empty query. This bound is not semantic identity.
   */
  std::uint64_t maximum_atom_observations = 65536;
  /** @brief Per-caller notification after a structured range is certified.
   * Each execute call has an independent subscription. Notifications are
   * serialized for that caller. Revision watermarks are keyed by logical step
   * and Result object identity: non-increasing revisions for the same object
   * are suppressed, while a different object at the same step starts its own
   * revision stream. A caller whose cancellation token is observed receives no
   * newly admitted notifications; an already-admitted callback may finish
   * before the call returns. The owning reference can be retained and read
   * explicitly after callback or context retirement. A prefix is not complete
   * execution success. `std::bad_alloc` maps to ResourceExhausted and other
   * callback exceptions map to OperationFailed; observer failure stops this Run
   * while prior certified ranges remain valid.
   */
  std::function<Status(ValueRef, const ResultRef&)> result_publication = {};
  /** @brief Maximum logical samples scanned for the optional Result digest.
   * Counts field elements and certified tensor samples. Zero disables the
   * digest; failed admission preflight omits it without failing execution.
   */
  std::uint64_t maximum_result_digest_samples = 65536;
};

/**
 * @brief Raw timing and outcome for one physical operation attempt.
 *
 * @note A GPU rejection followed by CPU fallback produces two attempt records.
 */
struct PHOTOSPIDER_API OperationTiming final {
  /** @brief Stable source node id. */
  ValueRef output;
  /** @brief Attempted local physical backend. */
  Backend backend = Backend::Cpu;
  /** @brief Monotonic callback duration in microseconds. */
  std::uint64_t duration_us = 0;
  /** @brief Attempt outcome category. */
  ErrorCode outcome = ErrorCode::Ok;
  /** @brief Number of attempts aggregated for this result/backend in regional
   * execution. */
  std::uint64_t invocation_count = 1;
  /** @brief Total logical output elements computed by these attempts.
   * Result publication counts newly certified field rows and tensor samples;
   * cache-hit publication contributes zero. Saturates at UINT64_MAX when the
   * cardinality or accumulated count exceeds uint64, with
   * computed_elements_saturated set. Diagnostic overflow does not invalidate
   * an otherwise legal Result publication.
   */
  std::uint64_t computed_elements = 0;
  /** @brief Actual native work for this attempt, zero for CPU/cache hits. */
  std::uint64_t native_dispatch_count = 0;
  std::uint64_t native_compute_us = 0;
  /** @brief Completed CPU computation stages and their tile callbacks. */
  std::uint64_t cpu_stage_count = 0, cpu_tile_callback_count = 0;
  /** @brief Actual numeric work of this attempt; empty for nonnumeric/cache
   * work.
   */
  NumericDiagnostics numeric = {};
  /** @brief True when computed_elements no longer represents an exact count.
   * False remains valid for an exact count equal to UINT64_MAX.
   */
  bool computed_elements_saturated = false;
};

/** @brief Context-local aggregate of cache observations, synchronized on read.
 * @note The context combines retained-result cache counters with structured
 * Result sharing counters. Their source-specific scopes are preserved rather
 * than normalized to one per-backend convention. Active Result producer
 * observations remain visible when completed-result retention is disabled.
 */
struct ResultCacheStatistics final {
  /** @brief Existing retained-result cache counters; each source keeps its
   * own hit, miss, and eviction scope. */
  std::uint64_t hits = 0, misses = 0, evictions = 0;
  /** @brief Cumulative shared-computation observations from the retained-result
   * cache and structured Result sharing. Structured Result sharing adds one
   * for each successful non-producer acquire, including a join to an active
   * producer or reuse of a completed weak Result. */
  std::uint64_t shared_computations = 0;
  /** @brief Retained cache bytes and entries; weak structured Result entries
   * do not add to either value. */
  std::uint64_t retained_bytes = 0, entries = 0;
  /** @brief Aggregate in-flight observations. Structured Result sharing counts
   * unfinished producer epochs while their producer lease exists, including
   * an older epoch replaced under the same key. Completion, failure, or lease
   * destruction retires that epoch once. */
  std::uint64_t in_flight = 0;
  /** @brief Unique retained native allocation capacity, included in
   * retained_bytes. */
  std::uint64_t native_retained_bytes = 0;
};

/**
 * @brief Raw local execution diagnostics.
 *
 * @note Diagnostics are observations, not verdicts, attestations, or receipts.
 */
struct PHOTOSPIDER_API ExecutionDiagnostics final {
  /** @brief Actual shared starts, polls and singleton group fallbacks. */
  std::uint64_t joint_groups = 0, joint_polls = 0, joint_fallbacks = 0;
  /** @brief Context-root model snapshot for structured execution; not RSS. */
  std::optional<ResourceStatistics> managed_resources = {};
  /** @brief Total execute call duration in microseconds. */
  std::uint64_t execute_us = 0;
  /** @brief Selected successful implementation backend per source result.
   * @note A staged GPU path may reuse completed state or resolve a constant
   * without new native work. Dispatch/submission fields report actual work.
   */
  std::map<ValueRef, Backend> selected_backends;
  /** @brief Number of explicit cross-backend input transfers. */
  std::uint64_t transfer_count = 0;
  /** @brief Sum of copied input bytes for explicit transfers. */
  std::uint64_t transfer_bytes = 0;
  /** @brief Actual native dispatches/submissions, including unpublished
   * attempts. */
  std::uint64_t native_dispatch_count = 0;
  std::uint64_t native_submission_count = 0;
  /** @brief Completed computation stages, independent of delivered output
   * tiles. */
  std::uint64_t cpu_stage_count = 0, cpu_tile_callback_count = 0;
  /** @brief Device command time; zero when unavailable, never host callback
   * time. */
  std::uint64_t native_compute_us = 0;
  /** @brief Bytes encoded as shader constants, separate from image copies. */
  std::uint64_t native_constant_bytes = 0;
  /** @brief Completed input-copy cache hits that avoid another upload. */
  std::uint64_t native_upload_hits = 0;
  /** @brief Shared GPU storage consumed on the host, without fabricated copies.
   */
  std::uint64_t host_access_count = 0;
  /** @brief Actual bytes copied when assembling collected output tiles. */
  std::uint64_t result_copy_bytes = 0;
  /** @brief Peak committed controlled Payload bytes attributed to this Run.
   * Includes payload allocated by parallel callbacks and this Run's own
   * shared producer epoch. Excludes caller-preexisting input backing and
   * earlier completed-cache storage. This reports controlled backing capacity,
   * not process RSS.
   */
  std::uint64_t peak_live_bytes = 0;
  /** @brief Maximum committed Payload peak among active shared producer epochs
   * used by this call. A joint producer reports its aggregate epoch peak.
   * @note Separate from caller-owned collection; shared work is never charged
   * twice in the context budget. Reusing a completed cached Result does not
   * carry forward the earlier producer epoch's peak.
   */
  std::uint64_t shared_peak_live_bytes = 0;
  /** @brief Peak reserved Payload capacity attributed to this Run, including
   * complete CPU reservations and incremental native reservations. This is an
   * admission capacity measure and may differ from committed live bytes.
   */
  std::uint64_t planned_peak_bytes = 0;
  /** @brief Caller-preexisting immutable input capacity outside the budget. */
  std::uint64_t retained_input_bytes = 0;
  /** @brief Maximum admitted plan callbacks, bounded by maximum_parallelism. */
  std::uint32_t peak_active_tasks = 0;
  /** @brief Successfully delivered output tile count, across named outputs. */
  std::uint64_t tile_count = 0;
  /** @brief Successful completed-result cache observations reused by this Run.
   */
  std::uint64_t cache_hits = 0;
  /** @brief Actual direct records visited by optional dependency cache proofs.
   */
  std::uint64_t dependency_cache_records_visited = 0;
  /** @brief Precharged proof, normalization, sample and internal block-key
   * work. */
  std::uint64_t dependency_cache_work = 0;
  /** @brief Completed internal state transitions reused/computed after keyed
   * lookup. */
  std::uint64_t block_cache_hits = 0, block_cache_misses = 0;
  /** @brief Active computations joined without duplicating producer timings. */
  std::uint64_t shared_computations = 0;
  /** @brief Successful source reads used to assemble Result inputs. */
  std::uint64_t source_read_count = 0;
  std::uint64_t source_read_bytes = 0;
  /** @brief Human-readable CPU fallback reasons in occurrence order. */
  std::vector<std::string> fallback_reasons;
  /** @brief Raw physical callback attempts. */
  std::vector<OperationTiming, ResourceAllocator<OperationTiming>>
      operation_timings;
  /** @brief Non-security digest of the executed physical plan. */
  ResourceString plan_digest;
  /** @brief Optional non-security content digest of named structured Results.
   * Structured execution hashes canonical schemas, field contents, and
   * certified logical tensor coverage and sample bytes in ExecutionResult's
   * results map. It excludes object identity, associations, and physical
   * layout; it is not authentication. Fragment execution leaves it empty.
   * Preflight omits the digest when sample, row-window, remaining-work, or
   * coverage-count representability checks cannot admit the full scan.
   * Failures during an admitted scan remain execution failures.
   */
  ResourceString result_digest;
};

/**
 * @brief Complete in-memory named execution result.
 *
 * @note Results have no durable identity, retention, receipt, or recovery
 * semantics.
 */
/** @brief Outcome of one named Result atom observation.
 * `output` identifies the requested compiled output and `key` identifies its
 * logical observation. The key omits tuple-channel and atomic trailing axes;
 * the associated Result covers those grouped samples at the selected
 * coordinates. `outcome` owns the published Result or retains a local failure
 * with its original upstream detail. `quality` carries any matching report.
 * Protocol, Run, Waiter, and call-level cancellation failures end the enclosing
 * call instead of becoming an observation outcome.
 */
struct AtomObservation final {
  ResourceString name;
  ValueRef output;
  AtomKey key;
  Result<ResultRef> outcome;
  std::optional<QualityReport> quality = {};
};
struct PHOTOSPIDER_API ExecutionResult final {
  /** @brief Raw compiler-independent execution diagnostics. */
  ExecutionDiagnostics diagnostics;
  /** @brief Direct structural evidence for a completed Result Run.
   * @note Owns no Result tensor backing storage.
   */
  ExecutionDependencies dependencies;
  /** @brief Paged named results; each retains descriptor, witness and backing.
   */
  ResourceMap<ResultRef> results = {};
  /** @brief Populated by execute_atoms; empty for ordinary execute calls. */
  ResourceVector<AtomObservation> atoms = {};
};

/**
 * @brief Shared owner for a pinned plan, immutable input bindings, and
 * registry.
 * @note Copies share one immutable state containing the plan, binding snapshot,
 * operation registry, and execution identity. The state survives replacement
 * or destruction of the source graph. Default objects fail Stale; capture does
 * not start work. A derived region owns a separate tile plan and state.
 */
class PHOTOSPIDER_API FrozenExecution final {
 public:
  FrozenExecution() = default;
  bool valid() const noexcept { return state_ != nullptr; }
  /** @brief Borrowed immutable pinned plan, valid for this object's lifetime.
   */
  const ExecutionPlan& plan() const noexcept;
  /** @brief Derives a frozen named-output region without recompilation.
   * @details Creates a new state with an independent tile plan and the captured
   * bindings, registry, and execution identity. Existing copies keep their
   * original plan and state.
   * For a sole-tensor Result output, the region uses full sample_shape,
   * including batch axes, and closes over the tensor's sample tuple contract.
   * @return Pinned region or Stale/InvalidArgument for invalid/outside demand.
   * @throws std::bad_alloc For copied metadata.
   */
  Result<FrozenExecution> for_region(const std::string& output,
                                     const Region& region) const;

 private:
  friend class ExecutionContext;
  friend class DemandHandle;
  struct State;
  std::shared_ptr<const State> state_;
};

/** @brief Named sample subsets of the compiled output regions.
 * ColorArray output requests expand to complete colors. Returned fragments and
 * retained demand identities use the closure; Image requires complete channels.
 */
using DemandQuery = std::map<std::string, Footprint>;
/** @brief Complete sparse result; holes remain unauthorized and unallocated. */
struct PHOTOSPIDER_API DemandResult final {
  ResourceMap<ResultRef> results;
  ExecutionDiagnostics diagnostics;
  ExecutionDependencies dependencies;
  /** @brief Captured demand generation; zero for direct frozen execution. */
  std::uint64_t generation = 0;
};
/** @brief Bounds the total retained structural publications for one handle. */
struct DemandConfig final {
  /** @brief Retained metadata units; supported range 1..1048576. */
  std::uint64_t maximum_metadata_entries = 65536;
};
/** @brief Atomic binding replacement and potential change in recorded outputs.
 * @note Coverage excludes never-requested outputs. Dirty sets accumulate until
 * a new successful request replaces that exact query's publication. Hints do
 * not authorize clean results; byte comparisons use immutable input owners.
 */
struct PHOTOSPIDER_API DemandUpdate final {
  std::uint64_t generation = 0;
  DemandQuery coverage;
  DemandQuery potential_dirty;
};
/** @brief Context-managed immutable binding bundle with explicit replacement.
 * @note Copies share one handle. Requests are independently cancellable and
 * keep original Q. Context destruction cancels and drains active calls; later
 * handle calls fail Cancelled. No worker or pixel cache is owned by the handle.
 */
class PHOTOSPIDER_API DemandHandle final {
 public:
  DemandHandle() = default;
  bool valid() const noexcept { return impl_ != nullptr; }
  /** @brief Executes exact original Q against the captured latest generation.
   * @return Complete fragments/evidence, or typed failure without partial
   * publication. Atomic observations are isolated; terminal Q is never split.
   * Concurrent replacement makes old latest requests Stale; cancellation wins.
   * @note Equal Atomic observations or identical terminal Q in the same
   * immutable bundle can share active work only with deterministic, side-effect
   * free ancestors. Waiter cancellation sources are independent of the
   * producer.
   * @throws std::bad_alloc For request/structural metadata.
   */
  Result<DemandResult> request(const DemandQuery& query,
                               const CancellationToken& cancellation = {},
                               const ExecutionOptions& options = {}) const;
  /** @brief Validates immutable replacements and commits bundle/dirty together.
   * @note Replacement Results must retain the same static schemas. Required
   * old support samples are compared under the sample limit. Concurrent request
   * publication/replacement may return Stale for retry; no partial edit occurs.
   * @return New generation and accumulated dirty coverage, or typed failure.
   * @throws std::bad_alloc For immutable snapshots/metadata.
   */
  Result<DemandUpdate> replace_bindings(
      ExecutionBindings bindings,
      const SnapshotAccessOptions& options = {}) const;
  /** @brief Pins the current bundle for ordinary independent frozen execution.
   * @return Owning frozen work or Cancelled/Stale for an unusable handle.
   */
  Result<FrozenExecution> freeze() const;
  /** @brief Removes one exact query's retained structural subscription.
   * @note Does not cancel active requests. An already-running request for Q
   * can publish a new subscription after this removal.
   * @return Success, NotFound for an unregistered query, or stopped status.
   */
  Status release(const DemandQuery& query) const;
  /** @brief Cancels all handle requests and releases retained publications.
   * @return True on the first cancellation; running callbacks drain normally.
   * @throws Nothing.
   */
  bool cancel() const noexcept;
  Result<std::uint64_t> generation() const;

 private:
  friend class ExecutionContext;
  friend struct execution_internal::DemandCoordinator;
  struct Impl;
  explicit DemandHandle(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};

/**
 * @brief Explicit owner of bounded local CPU/GPU execution resources.
 *
 * @note Independent contexts may run concurrently. Destruction requests stop,
 * rejects queued callbacks, releases their shared waiting admissions, and
 * joins every owned worker thread.
 */
class PHOTOSPIDER_API ExecutionContext final {
 public:
  /**
   * @brief Creates fixed workers, per-lane FIFOs, and shared admission owners.
   * @param operations Frozen operation registry retained for all Runs.
   * @param config Fixed local resource configuration.
   * @throws std::invalid_argument If registry/config is invalid.
   * @throws std::bad_alloc If worker/queue state allocation fails.
   * @note CPU execution is always created; GPU is optional. Both lanes consume
   * the single `maximum_queued_tasks` waiting bound.
   */
  explicit ExecutionContext(std::shared_ptr<OperationRegistry> operations,
                            ExecutionContextConfig config = {});

  /**
   * @brief Stops admission, joins workers, and verifies resource settlement.
   * @throws Nothing.
   * @note Direct execute/open/freeze calls must not race destruction. Existing
   * demand-handle calls may race shutdown; they are cancelled and drained.
   */
  ~ExecutionContext() noexcept;

  /**
   * @brief Forbids copying owned worker pools and resource accounting.
   * @param other Source context that cannot be copied.
   * @throws Nothing; the operation is deleted.
   * @note Construct a separate context for independent resource ownership.
   */
  ExecutionContext(const ExecutionContext& other) = delete;
  /**
   * @brief Forbids copy assignment of active local execution resources.
   * @param other Source context that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note In-flight Runs and queue ownership are never rebound.
   */
  ExecutionContext& operator=(const ExecutionContext& other) = delete;
  /**
   * @brief Forbids moving worker/resource ownership after construction.
   * @param other Source context that cannot be moved.
   * @throws Nothing; the operation is deleted.
   * @note Stable context lifetime bounds every private ExecutionRun.
   */
  ExecutionContext(ExecutionContext&& other) = delete;
  /**
   * @brief Forbids move assignment of worker pools and registry identity.
   * @param other Source context that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Destruction remains the only worker-ownership teardown path.
   */
  ExecutionContext& operator=(ExecutionContext&& other) = delete;

  /**
   * @brief Executes one validated plan through bounded local resources.
   * @param plan Immutable physical plan for one graph revision.
   * @param bindings Owned snapshot validated completely before callbacks.
   * Missing/extra/duplicate/malformed names fail InvalidArgument; a Result
   * schema mismatch fails TypeMismatch. Result descriptor and payload
   * validation failures use the corresponding typed status.
   * @param cancellation Cooperative cancellation observation.
   * @param options Per-Run parallelism controls.
   * @return Named result or typed cancellation/stale/backend/resource failure.
   * @throws std::bad_alloc If staging/result allocation fails before a
   * recoverable status can be built.
   * @note Default, stale, or foreign-registry plans fail Stale before bindings
   * or cancellation are read. After entry, observed cancellation precedes
   * stale graph state and ordinary failure. Concurrent calls may reuse a plan
   * with independent bindings; plan/options references must remain immutable
   * and valid. Caller-preexisting input retention is outside
   * maximum_live_bytes; controlled output/scratch/intermediate/transfer buffers
   * are charged until their last owner retires. Returned Results may outlive
   * this context. After every completion and after complete final
   * result/digest/timing assembly, publication rechecks cancellation before
   * plan currentness under the Run mutex. Passing that last check is the sole
   * success-publication linearization point; rejected local output is
   * discarded. Every non-side-effect-free operation is an execution root,
   * including when it has no named output. For a tensor output, the internal
   * actor identity uses slot zero with full coverage; other slots retain their
   * original Need and publication checks. Failure or cancellation of a
   * mandatory root fails this Run.
   */
  [[nodiscard]] Result<ExecutionResult> execute(
      const ExecutionPlan& plan, ExecutionBindings bindings = {},
      const CancellationToken& cancellation = CancellationToken(),
      const ExecutionOptions& options = {});

  /** @brief Opens an immutable latest-demand bundle without starting callbacks.
   * @note Requires Result bindings that match the workflow declarations. The
   * handle owns its captured plan independently from later graph replacement.
   * @return Handle or Stale/typed validation/resource failure.
   */
  Result<DemandHandle> open_demand(const ExecutionPlan& plan,
                                   ExecutionBindings bindings = {},
                                   DemandConfig config = {});
  /** @brief Executes arbitrary exact subsets against independently frozen work.
   * @note Empty revalidates static metadata and skips Result callbacks and
   * reads. Complete terminal RequestRecord Q stays intact, including
   * noncontiguous requests. With a positive result cache,
   * deterministic/side-effect-free/ cacheable ancestry may reuse successful
   * exact observations after matching the complete old source witness against
   * current immutable bindings. Hits preserve per-output evidence under the
   * current bundle identity. Optional cache limits do not change the
   * observation or failure-isolation contract. CPU and native GPU callbacks use
   * the existing context workers. Result continuations receive authorized
   * tensor capabilities through bounded atlas/discovery services.
   * BackendUnavailable returned by a GPU start_result may retry on CPU only
   * when the operation supports CPU and permits fallback. This retry occurs
   * before a continuation or Need exists and starts the original Q again;
   * per-session limits apply to each attempt, within the shared Run work bound.
   * Sticky callback/protocol, observer, service, resource, cancellation, or
   * active-stop failures also block retry. Failures after continuation
   * startup, including poll and publication failures, do not trigger CPU
   * fallback. For Result producers, shared waiters observe the actual backend
   * selected after a permitted startup retry; this does not establish a
   * separate Result fallback cache-isolation rule. The existing fallback-
   * ancestry exclusion from completed-result, checkpoint and block reuse
   * applies to Result Flights and Whole records.
   * Diagnostics record actual backends and rejected physical attempts.
   * Caller must not race direct execution with context destruction.
   * A cancelled structured Result producer may use the frozen-plan peer
   * handoff described by `execute`; without a pending producer and live peer,
   * callbacks drain before return. A nonempty Q makes every
   * non-side-effect-free operation a mandatory root, even if its output is
   * outside Q. A completely Empty Q skips unrelated effect roots. For a tensor
   * output, the internal actor identity uses slot zero with full coverage;
   * other slots retain their original Need and publication checks. Failure or
   * cancellation of a mandatory root fails this Run.
   */
  Result<DemandResult> execute_fragments(
      const FrozenExecution& frozen, const DemandQuery& query,
      const CancellationToken& cancellation = {},
      const ExecutionOptions& options = {});

  /**
   * @brief Pins a current matching plan and immutable Result bindings.
   * @return Frozen work or Stale/typed binding validation error.
   * @throws std::bad_alloc For snapshot metadata.
   * @note Capture rechecks graph currentness before returning. No callback
   * executes, and frozen work never observes later edits.
   */
  Result<FrozenExecution> freeze(const ExecutionPlan& plan,
                                 ExecutionBindings bindings = {}) const;
  /** @brief Executes pinned work, independently cancellable per call.
   * @return Named result or typed failure. Ordinary completion drains callbacks
   * before return.
   * @note If cancellation arrives while a structured Result producer callback
   * is pending and a peer still needs it, the context may retain the
   * coordinator for that peer and return after retiring this caller's
   * subscription. The peer later drives the coordinator on its own polling
   * thread. This handoff requires the pinned plan and does not apply when no
   * peer remains; those paths drain synchronously.
   * @throws std::bad_alloc For metadata allocation.
   */
  Result<ExecutionResult> execute(const FrozenExecution& frozen,
                                  const CancellationToken& cancellation = {},
                                  const ExecutionOptions& options = {});

  /** @brief Drops optional retained results; active readers remain valid.
   * @note Concurrent-safe; active producers cannot refill the cleared epoch.
   */
  void clear_result_cache();
  /** @brief Returns synchronized context-local cache observations.
   * @note The aggregate preserves the source-specific accounting of the
   * retained-result cache and adds structured Result sharing counters; it is
   * not a normalized per-backend counter set.
   */
  ResultCacheStatistics cache_statistics() const;
  /** @brief Returns cumulative FIFO observations when collection is enabled.
   * @note Thread-safe. Each lane is sampled under its own mutex, so the two
   * lane snapshots can represent different instants. Concurrent Runs share
   * these counters; per-request subtraction does not isolate one Run.
   */
  SchedulerStatistics scheduler_statistics() const;

  /**
   * @brief Returns the fixed resolved CPU worker count.
   * @return Positive worker count.
   * @throws Nothing.
   * @note The value never changes during context lifetime.
   */
  [[nodiscard]] std::uint32_t cpu_workers() const noexcept;

  /**
   * @brief Reports whether the optional local GPU lane exists.
   * @return Configured availability.
   * @throws Nothing.
   * @note Availability does not imply every operation supports GPU.
   */
  [[nodiscard]] bool gpu_enabled() const noexcept;
  /** @brief Shares the context's managed resource root with
   * temporary storage clients.
   * @return A handle to the context root.
   * Its leases can outlive this context. Does not start or retain computation.
   */
  Result<ResourceBudget> resource_budget() const;
  /** @brief Collects exact CPU Atomic Result observations for named outputs.
   * Uses the context's managed Root and requires a structured Result dependency
   * plan. Each requested name maps to a sample Footprint in the output's full
   * sample shape, including batch axes. The output must declare Atomic,
   * PerAtomOutcome, and joint contract 2. Tensor tuple-channel and atomic
   * trailing axes are grouped into one observation; the remaining coordinates
   * form its `AtomKey`.
   * Admission validates every named output and the total observation count
   * before preparing actors or invoking producer callbacks. The count is
   * bounded by `maximum_atom_observations` and a hard limit of 65,536. Each
   * member returns an owning Result or a local failure and optional quality;
   * protocol, Run, Waiter, and call-level cancellation failures end the call.
   * Joint contract 2 may batch up to 64 one-observation queries, including
   * different queries of one output, and still uses its required one-member
   * joint callback when grouping is disabled. It has no GPU path. Ordinary
   * `execute` keeps its fail-fast contract. A nonempty query makes every
   * non-side-effect-free operation a mandatory root; a completely Empty query
   * skips unrelated effect roots. For a tensor output, the internal actor
   * identity uses slot zero with full coverage; other slots retain their
   * original Need and publication checks. Failure or cancellation of a
   * mandatory root fails this Run.
   */
  Result<ExecutionResult> execute_atoms(
      const ExecutionPlan& plan, ExecutionBindings bindings,
      const DemandQuery& requested, const CancellationToken& cancellation = {},
      const ExecutionOptions& options = {});

 private:
  Result<ExecutionResult> execute_regions(
      const ExecutionPlan& plan, ExecutionBindings bindings,
      const CancellationToken& cancellation, const ExecutionOptions& options,
      const std::string& snapshot_identity = {}, bool atom_outcomes = false,
      const DemandQuery* requested = nullptr,
      std::shared_ptr<const ExecutionPlan> plan_owner = {});
  /** @brief Opaque pools, shared waiting admission, and resource ledger. */
  struct Impl;
  /** @brief Unique local execution ownership. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps
