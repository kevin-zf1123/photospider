#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "compute/request/compute_supersession.hpp"
#include "graph/graph_revision.hpp"  // NOLINT(build/include_subdir)
#include "photospider/core/compute_intent.hpp"
#include "photospider/core/device.hpp"
#include "photospider/memory/ready_fence.hpp"
#include "runtime/resource_ledger.hpp"  // NOLINT(build/include_subdir)

namespace ps {
class GraphModel;
class GraphTraversalService;
class ExecutionTaskRuntime;
class Value;
}  // namespace ps

namespace ps::testing {
class ComputeRequestCancellationSourceTestAccess;
}  // namespace ps::testing

namespace ps::compute {
class ComputeRunControl;
class ComputeRunCancellationSlot;
class ComputeRequestCancellationControl;
class ComputeRunDescriptor;
class HighPrecisionDirtyWriteBuffer;
class TaskSubmissionPlan;
class ComputeRun;
class ExecutionService;
class ExecutionLifecycleTelemetry;
class ProgressiveFinalGate;
class RunLifecycleRegistry;
enum class ComputeRunTerminalKind;
enum class ComputeRunCancellationReason;

/**
 * @brief Opaque stable identity for one request-owned compute Run.
 *
 * ComputeRunId values are minted by the private ComputeRun implementation from
 * one process-lifetime monotonic sequence. Numeric access exists for logging,
 * tests, and execution-service transport; callers must not derive ordering,
 * task identity, or policy from it.
 *
 * @throws Nothing for copy, comparison, and value access.
 * @note Value zero is reserved as invalid and is never minted. The current
 * process-lifetime sequence is ownership-neutral: it owns no worker, queue,
 * Graph, Run lifecycle, or resource policy.
 */
class ComputeRunId {
 public:
  /**
   * @brief Returns the opaque numeric representation.
   *
   * @return Non-zero process-lifetime Run identifier.
   * @throws Nothing.
   * @note The value is diagnostic identity only and carries no priority or
   * temporal policy.
   */
  uint64_t value() const noexcept { return value_; }

  /**
   * @brief Compares two Run identities for exact equality.
   *
   * @param other Candidate Run identity.
   * @return true only when both opaque values identify the same Run.
   * @throws Nothing.
   */
  bool operator==(const ComputeRunId& other) const noexcept {
    return value_ == other.value_;
  }

  /**
   * @brief Compares two Run identities for inequality.
   *
   * @param other Candidate Run identity.
   * @return true only when the opaque values differ.
   * @throws Nothing.
   */
  bool operator!=(const ComputeRunId& other) const noexcept {
    return !(*this == other);
  }

 private:
  friend class ComputeRun;
  friend class ComputeRunControl;

  /**
   * @brief Constructs an id from the private monotonic generator.
   *
   * @param value Non-zero generated value.
   * @throws Nothing.
   * @note Only ComputeRun may construct identities.
   */
  explicit ComputeRunId(uint64_t value) noexcept : value_(value) {}

  /** @brief Opaque non-zero process-lifetime identity value. */
  uint64_t value_ = 0;
};

/**
 * @brief Dense task identity whose numeric value is local to one ComputeRun.
 *
 * A local id has meaning only together with the Run id carried by
 * ComputeRunTaskIdentity. Different Runs intentionally reuse the same dense
 * values without sharing execution or completion state.
 *
 * @throws Nothing for construction, comparison, and value access.
 * @note This private backend value is not an execution epoch, worker id, graph
 * node id, or process-global task identity.
 */
class ComputeRunLocalTaskId {
 public:
  /**
   * @brief Constructs one Run-local task id from a dense nonnegative value.
   *
   * @param value Dense task value supplied by a Run-owned submission plan.
   * @throws Nothing.
   * @note Registration against a concrete Run plan is validated when the
   * identity is routed; constructing a value alone grants no execution right.
   */
  explicit ComputeRunLocalTaskId(uint64_t value) noexcept : value_(value) {}

  /**
   * @brief Returns the dense Run-local numeric value.
   *
   * @return Value interpreted only inside the matching Run.
   * @throws Nothing.
   */
  uint64_t value() const noexcept { return value_; }

  /**
   * @brief Compares two local task values.
   *
   * @param other Candidate local value.
   * @return true when the dense values match.
   * @throws Nothing.
   * @note Equal local values from different Runs remain different composite
   * task identities.
   */
  bool operator==(const ComputeRunLocalTaskId& other) const noexcept {
    return value_ == other.value_;
  }

  /**
   * @brief Compares two local task values for inequality.
   *
   * @param other Candidate local value.
   * @return true when the dense values differ.
   * @throws Nothing.
   */
  bool operator!=(const ComputeRunLocalTaskId& other) const noexcept {
    return !(*this == other);
  }

 private:
  /** @brief Dense value interpreted only by the matching Run plan. */
  uint64_t value_ = 0;
};

/**
 * @brief Composite completion identity for one task in one ComputeRun.
 *
 * Task execution, dependency release, completion, and worker-failure routing
 * compare both components. The value carries no lifetime; an accepted callback
 * must separately retain the matching ComputeRunLease.
 *
 * @throws Nothing for construction and comparison.
 * @note Execution batch epochs cannot substitute for this identity.
 */
class ComputeRunTaskIdentity {
 public:
  /**
   * @brief Constructs one composite task identity.
   *
   * @param run_id Opaque Run namespace.
   * @param local_task_id Dense task value within that Run.
   * @throws Nothing.
   * @note Route validation still requires a matching lease and a registered
   * local id.
   */
  ComputeRunTaskIdentity(ComputeRunId run_id,
                         ComputeRunLocalTaskId local_task_id) noexcept
      : run_id_(run_id), local_task_id_(local_task_id) {}

  /**
   * @brief Returns the Run namespace component.
   *
   * @return Opaque identity of the owning Run.
   * @throws Nothing.
   */
  ComputeRunId run_id() const noexcept { return run_id_; }

  /**
   * @brief Returns the Run-local task component.
   *
   * @return Dense task id interpreted by the owning Run plan.
   * @throws Nothing.
   */
  ComputeRunLocalTaskId local_task_id() const noexcept {
    return local_task_id_;
  }

  /**
   * @brief Compares complete task identities.
   *
   * @param other Candidate composite identity.
   * @return true only when both Run and local components match.
   * @throws Nothing.
   */
  bool operator==(const ComputeRunTaskIdentity& other) const noexcept {
    return run_id_ == other.run_id_ && local_task_id_ == other.local_task_id_;
  }

  /**
   * @brief Compares complete task identities for inequality.
   *
   * @param other Candidate composite identity.
   * @return true when either component differs.
   * @throws Nothing.
   */
  bool operator!=(const ComputeRunTaskIdentity& other) const noexcept {
    return !(*this == other);
  }

 private:
  /** @brief Opaque namespace of the owning Run. */
  ComputeRunId run_id_;

  /** @brief Dense task value registered by that Run's plan. */
  ComputeRunLocalTaskId local_task_id_;
};

/**
 * @brief Quality carried independently from compute intent and QoS.
 *
 * @throws Nothing for value operations.
 * @note GlobalHighPrecision Runs, including the HP child of a realtime update,
 * use full quality. The sibling RealTimeUpdate child uses interactive quality
 * without merging intent and quality.
 */
enum class ComputeRunQuality {
  /** @brief Full high-precision product output. */
  Full,

  /** @brief Interactive realtime proxy output. */
  Interactive,
};

/**
 * @brief Built-in scheduling service class requested independently from intent.
 *
 * @throws Nothing for value operations.
 * @note ExecutionService selects its private built-in policy only from this
 * value. Current Kernel requests use Throughput explicitly; private callers
 * may request Interactive without changing ComputeIntent or output quality.
 */
enum class ComputeRunQosClass {
  /** @brief Deadline-aware latency-sensitive built-in policy input. */
  Interactive,

  /** @brief Weighted completion-oriented built-in policy input. */
  Throughput,
};

/**
 * @brief Immutable-by-copy QoS inputs captured in a Run descriptor.
 *
 * The value does not own workers, reservations, ready entries, or policy
 * state. ExecutionService copies it into one admitted Run, consumes the
 * explicit class, deadline, and weight through its private policy strategies,
 * and applies maximum_parallelism to both root admission and physical starts.
 *
 * @throws Nothing for default construction; copying an optional time point
 * does not allocate.
 * @note Weight and maximum_parallelism are validated by ComputeRun. The
 * positive optional maximum caps admitted callback concurrency without
 * resizing the fixed service pool; intent, QoS, resource admission, and
 * commit policy remain distinct concepts.
 */
struct ComputeRunQos {
  /** @brief Requested scheduling service class. */
  ComputeRunQosClass service_class = ComputeRunQosClass::Throughput;

  /** @brief Optional absolute deadline on std::chrono::steady_clock. */
  std::optional<std::chrono::steady_clock::time_point> deadline;

  /** @brief Positive relative policy weight. */
  uint32_t weight = 1;

  /** @brief Optional positive cap on simultaneously executing Run tasks. */
  std::optional<uint32_t> maximum_parallelism;
};

/**
 * @brief Observation-only coordinate reserved at one product linearization.
 *
 * The observation sink samples the process steady clock and reserves one
 * strictly increasing observer-local sequence before the owning product state
 * transition. Product code then carries this immutable coordinate to the
 * corresponding callback even when callback delivery occurs later.
 *
 * @throws Nothing for value construction and copying.
 * @note The value grants no scheduling, cancellation, lifecycle, resource, or
 * commit authority. Sequence zero is reserved for absent/overflow evidence.
 */
struct ComputeRunObservationCoordinate final {
  /** @brief Steady-clock sample taken immediately before sequence reservation.
   */
  std::chrono::steady_clock::time_point observed_at;

  /** @brief Strictly increasing observer-local causal sequence. */
  std::uint64_t causal_sequence = 0U;
};

/**
 * @brief Borrowed dependency-ready task facts for source-private observation.
 *
 * The dispatcher assembles this value from the exact `PlannedTask`, selected
 * device, and immutable resource declaration that are about to enter one
 * `ReadyTaskSubmission`. Dependency storage remains owned by the Run plan and
 * is valid only for the synchronous callback.
 *
 * @throws Nothing for value construction and copying.
 * @note This value grants no readiness, queue, resource, Graph, operation, or
 * completion authority. ROI fields describe the actual planned output shape;
 * consumers remain responsible for workload-specific logical byte mapping.
 */
struct ComputeRunTaskReadyObservation final {
  /** @brief Borrowed dense upstream task ids in plan-owned order. */
  const int* dependency_task_ids = nullptr;

  /** @brief Number of borrowed dependency ids. */
  std::size_t dependency_task_count = 0U;

  /** @brief Whether the actual planned task uses tiled execution. */
  bool tiled = false;

  /** @brief Planned output ROI left coordinate. */
  int output_x = 0;

  /** @brief Planned output ROI top coordinate. */
  int output_y = 0;

  /** @brief Planned output ROI width. */
  int output_width = 0;

  /** @brief Planned output ROI height. */
  int output_height = 0;

  /** @brief Device selected with the retained operation implementation. */
  Device device = Device::CPU;

  /** @brief Actual ready submission's declared logical work units. */
  std::uint64_t work_units = 0U;

  /** @brief Actual ready submission's declared retained Host bytes. */
  std::uint64_t retained_memory_bytes = 0U;

  /** @brief Actual ready submission's declared Host scratch bytes. */
  std::uint64_t scratch_bytes = 0U;

  /** @brief Actual ready submission's declared ready-store bytes. */
  std::uint64_t ready_bytes = 0U;
};

/**
 * @brief Closed logical terminal category for one actually entered task.
 * @throws Nothing for value construction and comparison.
 * @note The category is task-local evidence. It neither publishes nor
 * replaces the owning Run's terminal outcome.
 */
enum class ComputeRunTaskTerminalKind : std::uint8_t {
  /** @brief Task work and any deferred producer completion succeeded. */
  Succeeded,

  /** @brief Task execution or deferred producer completion failed. */
  Failed,

  /** @brief An already-terminal cancelled Run suppressed task work. */
  Cancelled,
};

/**
 * @brief Allocation-free irreversible service-start commit callback.
 * @param context Borrowed caller-owned context valid for the complete call.
 * @return True only after the physical route start commits irreversibly.
 * @throws Nothing; implementations must contain every failure.
 * @note `ExecutionService` supplies this callback while holding its pool and
 * Run-state locks. `ComputeRunControl` invokes it under the matching Run's
 * terminal-arbiter mutex so cancellation acceptance and route commitment share
 * one linearization authority.
 */
using ComputeRunServiceStartCommitCallback = bool (*)(void* context) noexcept;

/**
 * @brief Immutable evidence facts bound to one committed service start.
 *
 * The two class facts are sampled by `ExecutionService` from real ready-store
 * candidates, Run lifecycle, operation-gate, physical-route, and available
 * execution child-grant capacity immediately before the selected start stages
 * its own grant and enters irreversible Run arbitration. They describe the
 * evidence-startable applicability cut, not the scheduler-selectable class
 * competition that owns the three-to-one rule. Nominal request or offer
 * intervals cannot mint either value.
 *
 * @throws Nothing for value construction and comparison.
 * @note `execution_grant_committed` is true only after the selected child
 * grant, operation gate, physical route, ready removal, and start counters
 * commit. The value is observation-only and grants no queue, route, resource,
 * Run, or cancellation authority.
 */
struct ComputeRunServiceStartObservation final {
  /** @brief Whether a real Interactive candidate was evidence-startable. */
  bool interactive_candidate_startable = false;

  /** @brief Whether a real Throughput candidate was evidence-startable. */
  bool throughput_candidate_startable = false;

  /** @brief Whether the selected start committed its execution child grant. */
  bool execution_grant_committed = false;
};

/**
 * @brief Source-private, observation-only sink for one product compute request.
 *
 * The sink receives immutable identities, scalar service facts, terminal
 * categories, and a retained immutable output Value at the exact product
 * boundaries that own those facts. It cannot reach a Graph, scheduler, ready
 * queue, cancellation source, ledger token, resource grant, commit contender,
 * or mutable output, and its return values never participate in runtime
 * decisions.
 *
 * @throws Nothing. Implementations must contain every failure; an escaping
 * exception terminates because callbacks run on product correctness
 * boundaries.
 * @note Callbacks may run while the coordinator, Run, execution-service, or
 * lifecycle-registry fence is held. Callback bodies remain bounded and
 * nonblocking after coordinate reservation. A source-private coordinate
 * allocator may yield while retrying only an exception-free constant-work
 * atomic serialization owner; it must not sleep, allocate, acquire a product
 * mutex, or re-enter compute services. The interface is source-private and is
 * absent from installed Host, operation, policy, CLI, and IPC contracts.
 */
class ComputeRunObservationSink {
 public:
  /**
   * @brief Releases observation-only implementation state.
   * @throws Nothing.
   * @note Destruction owns no product work and requests no cancellation.
   */
  virtual ~ComputeRunObservationSink() noexcept = default;

  /**
   * @brief Reserves one coordinate at an authoritative product boundary.
   * @return Nonzero observer-local sequence paired with its steady-clock
   * sample.
   * @throws Nothing; implementations must contain sequence exhaustion.
   * @note Callers invoke this immediately before the transition represented by
   * the later callback. Reservation is allocation-free and does not itself
   * publish an event. An implementation may yield while retrying an
   * exception-free constant-work atomic serialization owner so that ordinary
   * contention cannot be misclassified as numeric sequence exhaustion.
   */
  virtual ComputeRunObservationCoordinate
  reserve_causal_coordinate() noexcept = 0;

  /**
   * @brief Retires one reservation whose represented callback will not run.
   * @param coordinate Exact coordinate returned by the preceding reservation.
   * @return Nothing.
   * @throws Nothing; implementations must contain every failure.
   * @note Callers invoke this exactly once only when the product transition did
   * not commit, or when a source-private causal marker deliberately has no
   * callback. The default no-op preserves observers that do not track
   * reservation lifecycle; collectors using reservation frontiers override it.
   * The operation remains bounded, nonblocking, allocation-free, and owns no
   * product authority.
   */
  virtual void abort_causal_coordinate(
      ComputeRunObservationCoordinate coordinate) noexcept {
    (void)coordinate;
  }

  /**
   * @brief Observes publication of one accepted current request generation.
   * @param identity Product-assigned key and generation becoming current.
   * @param coordinate Coordinate reserved immediately before currentness
   * publication.
   * @return Nothing.
   * @throws Nothing; implementations must contain every failure.
   * @note The coordinator invokes this immediately before publishing the same
   * generation to `is_current()`.
   */
  virtual void on_current_generation(
      const SupersessionIdentity& identity,
      ComputeRunObservationCoordinate coordinate) noexcept = 0;

  /**
   * @brief Reports whether this sink requests task-semantic observations.
   * @return True only for bounded collectors that consume ready/terminal task
   * facts.
   * @throws Nothing.
   * @note The default false preserves existing observation sequence domains:
   * dispatchers reserve no additional coordinates for sinks that do not opt
   * in. The result grants no product authority.
   */
  virtual bool observes_task_semantics() const noexcept { return false; }

  /**
   * @brief Observes one actual dependency-ready task materialization.
   * @param descriptor Immutable identity and policy inputs of the owning Run.
   * @param task_identity Exact Run-local ready task identity.
   * @param observation Borrowed actual dependency, shape, device, and resource
   * declaration valid only for this synchronous call.
   * @param coordinate Coordinate reserved immediately before callback
   * delivery after the owned ready submission is fully constructed.
   * @return Nothing.
   * @throws Nothing; implementations must contain every failure.
   * @note Only sinks returning true from `observes_task_semantics()` receive
   * this callback. It does not imply later service admission or start.
   */
  virtual void on_task_ready(
      const ComputeRunDescriptor& descriptor,
      ComputeRunTaskIdentity task_identity,
      const ComputeRunTaskReadyObservation& observation,
      ComputeRunObservationCoordinate coordinate) noexcept {
    (void)descriptor;
    (void)task_identity;
    (void)observation;
    (void)coordinate;
  }

  /**
   * @brief Observes one physically committed callback service start.
   * @param descriptor Immutable identity and policy inputs of the owning Run.
   * @param task_identity Exact Run-local callback identity.
   * @param service_charge Exact `work_units + ceil(ready_bytes/4096)` charge.
   * @param observation Real evidence applicability and committed-grant facts.
   * @param coordinate Coordinate reserved at reserved-start commitment.
   * @return Nothing.
   * @throws Nothing; implementations must contain every failure.
   * @note The service reserves `coordinate` and commits the route under the
   * Run terminal arbiter, then invokes this after ready removal,
   * grant/gate commitment, and start counters become authoritative. Callback
   * delivery occurs without the service pool, service Run-state, or terminal
   * arbiter mutex held. The shared sink sequence, rather than callback
   * scheduling order, total-orders start and cancellation.
   */
  virtual void on_service_start(
      const ComputeRunDescriptor& descriptor,
      ComputeRunTaskIdentity task_identity, std::uint64_t service_charge,
      const ComputeRunServiceStartObservation& observation,
      ComputeRunObservationCoordinate coordinate) noexcept = 0;

  /**
   * @brief Observes the logical terminal category of one entered task.
   * @param descriptor Immutable identity and policy inputs of the owning Run.
   * @param task_identity Exact Run-local task reaching terminal state.
   * @param kind Succeeded, Failed, or Cancelled task-local category.
   * @param coordinate Coordinate reserved at task terminal publication.
   * @return Nothing.
   * @throws Nothing; implementations must contain every failure.
   * @note Only opted-in task-semantic sinks receive this callback. Deferred
   * Values publish terminal only after their producer fence settles; the
   * callback changes no Run terminal or dependency authority.
   */
  virtual void on_task_terminal(
      const ComputeRunDescriptor& descriptor,
      ComputeRunTaskIdentity task_identity, ComputeRunTaskTerminalKind kind,
      ComputeRunObservationCoordinate coordinate) noexcept {
    (void)descriptor;
    (void)task_identity;
    (void)kind;
    (void)coordinate;
  }

  /**
   * @brief Observes acceptance of one Run cancellation reason.
   * @param descriptor Immutable identity and policy inputs of the cancelled
   * Run.
   * @param reason Stable cancellation reason that won terminal arbitration.
   * @param coordinate Coordinate reserved at cancellation acceptance.
   * @return Nothing.
   * @throws Nothing; implementations must contain every failure.
   * @note Rejected or repeated cancellation requests produce no callback.
   */
  virtual void on_cancellation(
      const ComputeRunDescriptor& descriptor,
      ComputeRunCancellationReason reason,
      ComputeRunObservationCoordinate coordinate) noexcept = 0;

  /**
   * @brief Observes the exactly-once terminal category of one Run.
   * @param descriptor Immutable identity and policy inputs of the terminal Run.
   * @param kind Succeeded, Failed, or Cancelled terminal category.
   * @param coordinate Coordinate reserved at terminal publication.
   * @return Nothing.
   * @throws Nothing; implementations must contain every failure.
   * @note Failure exception payload is deliberately excluded from the
   * authority-free evidence seam.
   */
  virtual void on_terminal(
      const ComputeRunDescriptor& descriptor, ComputeRunTerminalKind kind,
      ComputeRunObservationCoordinate coordinate) noexcept = 0;

  /**
   * @brief Observes one current domain publication and retains its Value.
   * @param descriptor Immutable identity of the Run whose contender committed.
   * @param output Copy of the exact immutable image Value published visibly.
   * @param coordinate Coordinate reserved at visible publication.
   * @return Nothing.
   * @throws Nothing; implementations must contain every failure.
   * @note Value copying retains immutable shared state and grants no mutation
   * or graph ownership. HP emits the full result and progressive RT emits the
   * preview result. The commit contender emits this callback and its succeeding
   * terminal callback in one Run-arbiter resolution, so a stale, failed,
   * empty, or already-resolved contender produces no visibility.
   */
  virtual void on_current_visible(
      const ComputeRunDescriptor& descriptor, Value output,
      ComputeRunObservationCoordinate coordinate) noexcept = 0;

  /**
   * @brief Observes consumption of progressive final permission.
   * @param descriptor Immutable HP Full child descriptor about to be entered.
   * @param coordinate Coordinate reserved immediately before HP submission.
   * @return Nothing.
   * @throws Nothing; implementations must contain every failure.
   * @note The default no-op preserves existing source-private observers. The
   * event grants no authority. A Run-owned operation consumes the bound gate,
   * reserves this coordinate, and delivers the callback while holding the HP
   * Run terminal arbiter; successful return is immediately before HP provider
   * or ExecutionService entry. Implementations must not block or re-enter Run
   * state (tests may use a deliberate barrier to verify the lock boundary).
   */
  virtual void on_progressive_final_triggered(
      const ComputeRunDescriptor& descriptor,
      ComputeRunObservationCoordinate coordinate) noexcept {
    (void)descriptor;
    (void)coordinate;
  }

  /**
   * @brief Observes physical Run quiescence selected by lifecycle finalization.
   * @param descriptor Immutable identity of the quiescent Run.
   * @param coordinate Coordinate reserved at the quiescence transition.
   * @return Nothing.
   * @throws Nothing; implementations must contain every failure.
   * @note Terminal publication precedes this event. The registry remains the
   * sole lifecycle authority; this callback cannot retain a Run lease.
   */
  virtual void on_run_quiescent(
      const ComputeRunDescriptor& descriptor,
      ComputeRunObservationCoordinate coordinate) noexcept = 0;

  /**
   * @brief Observes exact return of one Run's physical root resources.
   * @param descriptor Immutable identity of the settled Run.
   * @param coordinate Coordinate reserved at resource settlement.
   * @return Nothing.
   * @throws Nothing; implementations must contain every failure.
   * @note Quiescence precedes this event. The callback carries no ledger token,
   * reservation, grant, or lifecycle-finalization authority.
   */
  virtual void on_run_resource_settled(
      const ComputeRunDescriptor& descriptor,
      ComputeRunObservationCoordinate coordinate) noexcept = 0;

  /**
   * @brief Observes publication and Host tracking settlement of one request.
   * @param coordinate Coordinate reserved only after the caller-visible status
   * future is ready and Host tracking records that publication.
   * @return Nothing.
   * @throws Nothing; implementations must contain every failure.
   * @note The request-scoped sink already identifies the edit. This event owns
   * no Run descriptor because supersession may settle before Run
   * materialization.
   */
  virtual void on_host_settled(
      ComputeRunObservationCoordinate coordinate) noexcept = 0;
};

/**
 * @brief Caller-supplied immutable inputs used to construct one domain Run.
 *
 * @throws std::bad_alloc when graph_identity is copied into Run ownership.
 * @note Kernel supplies a stable session label. Direct private ComputeService
 * callers may intentionally leave graph_identity empty.
 */
struct ComputeRunSubmission {
  /** @brief Stable graph/session label supplied by the request boundary. */
  std::string graph_identity;

  /** @brief Strong live Graph instance identity captured before planning. */
  GraphInstanceId graph_instance_id;

  /** @brief Authoritative Graph revision captured before domain planning. */
  GraphRevision revision;

  /** @brief Target graph node id requested by the caller. */
  int target_node_id = -1;

  /** @brief Single-domain HP or RT intent. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;

  /** @brief Quality independent from intent and QoS. */
  ComputeRunQuality quality = ComputeRunQuality::Full;

  /** @brief Explicit request QoS captured before service policy selection. */
  ComputeRunQos qos;

  /**
   * @brief Canonical request key/generation shared by realtime child Runs.
   * @note Child `intent` remains independent: the HP child of a realtime
   * request retains the realtime request key carried here.
   */
  SupersessionIdentity supersession;

  /**
   * @brief Optional source-private read-only observer retained by the Run.
   * @note The observer owns no product authority and is absent from installed
   * request or ABI values.
   */
  std::shared_ptr<ComputeRunObservationSink> observation_sink;
};

/**
 * @brief Immutable descriptor retained for the complete ComputeRun lifetime.
 *
 * The class exposes accessors only. A descriptor is created by ComputeRun
 * together with a fresh opaque id, preventing callers from supplying reused
 * identity.
 *
 * @throws Nothing for scalar access; graph_identity() returns a borrowed
 * reference.
 * @note Descriptor fields are request identity and policy inputs, not mutable
 * dispatcher or resource state.
 */
class ComputeRunDescriptor {
 public:
  /**
   * @brief Returns this Run's opaque identity.
   *
   * @return Stable id retained until Run destruction.
   * @throws Nothing.
   */
  ComputeRunId id() const noexcept { return id_; }

  /**
   * @brief Returns the captured graph/session identity.
   *
   * @return Borrowed immutable graph label.
   * @throws Nothing.
   * @note The reference remains valid only while this descriptor lives.
   */
  const std::string& graph_identity() const noexcept { return graph_identity_; }

  /**
   * @brief Returns the captured strong Graph instance identity.
   * @return Identity of the exact live Graph snapshot used for planning.
   * @throws Nothing.
   */
  GraphInstanceId graph_instance_id() const noexcept {
    return graph_instance_id_;
  }

  /**
   * @brief Returns the authoritative Graph submission revision.
   *
   * @return Revision value captured before planning.
   * @throws Nothing.
   */
  GraphRevision revision() const noexcept { return revision_; }

  /**
   * @brief Returns the requested target node id.
   *
   * @return Graph-local target node id.
   * @throws Nothing.
   */
  int target_node_id() const noexcept { return target_node_id_; }

  /**
   * @brief Returns the single-domain compute intent.
   *
   * @return GlobalHighPrecision or RealTimeUpdate for the single-domain Run.
   * @throws Nothing.
   */
  ComputeIntent intent() const noexcept { return intent_; }

  /**
   * @brief Returns the captured quality marker.
   *
   * @return Full for a GlobalHighPrecision Run or Interactive for a
   * RealTimeUpdate child.
   * @throws Nothing.
   */
  ComputeRunQuality quality() const noexcept { return quality_; }

  /**
   * @brief Returns the captured QoS value.
   *
   * @return Immutable reference to the Run-owned QoS copy.
   * @throws Nothing.
   * @note The reference remains valid only while this descriptor lives.
   */
  const ComputeRunQos& qos() const noexcept { return qos_; }

  /**
   * @brief Returns the immutable request lineage shared by sibling Runs.
   * @return Canonical request key and graph-wide generation.
   * @throws Nothing.
   * @note This value is independent from this descriptor's RunId, child
   * intent, GraphRevision, and execution epoch.
   */
  const SupersessionIdentity& supersession() const noexcept {
    return supersession_;
  }

  /**
   * @brief Returns the optional observation-only request sink.
   * @return Shared sink retained for the complete Run lifetime, or null.
   * @throws Nothing.
   * @note Copying the owner grants no scheduling, cancellation, resource, or
   * commit authority.
   */
  const std::shared_ptr<ComputeRunObservationSink>& observation_sink()
      const noexcept {
    return observation_sink_;
  }

 private:
  friend class ComputeRun;
  friend class ComputeRunControl;

  /**
   * @brief Constructs one immutable descriptor from validated submission data.
   *
   * @param id Fresh opaque Run identity.
   * @param submission Caller-supplied descriptor inputs transferred into Run
   * ownership.
   * @throws std::bad_alloc if graph identity ownership cannot be transferred.
   * @note ComputeRun validates semantic inputs before invoking this
   * constructor.
   */
  ComputeRunDescriptor(ComputeRunId id, ComputeRunSubmission submission);

  /** @brief Fresh opaque identity minted for this Run. */
  ComputeRunId id_;

  /** @brief Owned graph/session label captured before planning. */
  std::string graph_identity_;

  /** @brief Strong Graph instance identity captured before planning. */
  GraphInstanceId graph_instance_id_;

  /** @brief Authoritative Graph revision captured before planning. */
  GraphRevision revision_;

  /** @brief Graph-local requested target node id. */
  int target_node_id_ = -1;

  /** @brief Single-domain compute intent. */
  ComputeIntent intent_ = ComputeIntent::GlobalHighPrecision;

  /** @brief Output quality independent from intent and QoS. */
  ComputeRunQuality quality_ = ComputeRunQuality::Full;

  /** @brief Explicit QoS inputs retained without owning policy/resources. */
  ComputeRunQos qos_;

  /** @brief Canonical request lineage version captured before planning. */
  SupersessionIdentity supersession_;

  /** @brief Optional observation-only owner retained through settlement. */
  std::shared_ptr<ComputeRunObservationSink> observation_sink_;
};

/**
 * @brief Monotonic execution phase for one ComputeRun.
 *
 * @throws Nothing for value operations.
 * @note Safe paths may skip nonterminal phases. Terminal is entered only by a
 * terminal publication method, never by advance_to().
 */
enum class ComputeRunPhase {
  /** @brief Descriptor exists but admission has not been recorded. */
  Created,

  /** @brief ComputeService accepted this single-domain HP or RT child. */
  Admitted,

  /** @brief Route-backed ready work is about to be submitted. */
  Queued,

  /** @brief Inline or route-backed operation work is active. */
  Running,

  /** @brief Staged output is ready for current visible commit. */
  CommitPending,

  /** @brief One exact terminal outcome has been published. */
  Terminal,
};

/**
 * @brief Exactly-one terminal outcome category.
 *
 * @throws Nothing for value operations.
 */
enum class ComputeRunTerminalKind {
  /** @brief Current path validated committed or reusable target output. */
  Succeeded,

  /** @brief Planning, execution, scheduling, cache, or validation failed. */
  Failed,

  /** @brief Cancellation claimed the terminal arbiter before success/failure.
   */
  Cancelled,
};

/**
 * @brief Stable cancellation reason retained by a cancelled Run.
 *
 * @throws Nothing for value operations.
 * @note Current Run terminal arbitration exposes no Host cancellation control
 * surface.
 */
enum class ComputeRunCancellationReason {
  /** @brief Caller explicitly requested cancellation. */
  ExplicitRequest,

  /** @brief Owning graph began close. */
  GraphClose,

  /** @brief Owning process began shutdown. */
  ProcessShutdown,

  /** @brief A newer request superseded this Run. */
  Superseded,

  /** @brief The Run's monotonic deadline expired. */
  DeadlineExceeded,
};

/**
 * @brief Immutable value snapshot of one published terminal outcome.
 *
 * Exactly one payload field is meaningful: failure is non-null for Failed,
 * while cancellation_reason is populated for Cancelled.
 *
 * @throws Nothing for copy and move under std::exception_ptr and optional
 * value semantics.
 * @note Succeeded carries neither failure nor cancellation payload.
 */
struct ComputeRunTerminalOutcome {
  /** @brief Published terminal category. */
  ComputeRunTerminalKind kind = ComputeRunTerminalKind::Succeeded;

  /** @brief Original exception identity for Failed, otherwise null. */
  std::exception_ptr failure;

  /** @brief Stable cancellation reason for Cancelled, otherwise nullopt. */
  std::optional<ComputeRunCancellationReason> cancellation_reason;
};

/**
 * @brief Injected monotonic time source used by cooperative deadline checks.
 *
 * @return Current point in the same `steady_clock` domain as Run deadlines.
 * @throws Implementations must not throw. An exception from a
 * contract-violating injected clock propagates from the observation boundary.
 * @note Production Runs use `std::chrono::steady_clock::now`. Injection exists
 * only on this private backend surface so deadline tests need no wall-clock
 * sleeps or timer thread.
 */
using ComputeRunMonotonicClock =
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    std::function<std::chrono::steady_clock::time_point()>;

/**
 * @brief Private cancellation authority for exactly one ComputeRun.
 *
 * Sources are minted only by their matching Run. Copying preserves the same
 * authority without granting it to ordinary Run leases. A source holds weak
 * control ownership, so retaining a request handle cannot prolong Run staging,
 * callback, or resource lifetime.
 *
 * @throws std::system_error if Run-state synchronization fails.
 * @note The first accepted reason publishes `Cancelled`; repeated requests,
 * requests after failure/success, and requests after a commit contender is
 * accepted are idempotent no-ops. This private type is not installed or exposed
 * through Host, CLI, IPC, operation, or Policy ABI contracts.
 */
class ComputeRunCancellationSource {
 public:
  /**
   * @brief Requests monotonic cancellation with one stable reason.
   * @param reason Internal reason proposed to the matching Run arbiter.
   * @return True only when this request claims the open terminal arbiter.
   * @throws std::system_error if Run-state or cleanup synchronization fails.
   * @throws Any exception from a registered cleanup callback after
   * cancellation becomes terminal.
   * @note A bound progressive gate is denied before cancellation becomes
   * terminal. Cleanup notifications run after the terminal mutex is released.
   * Every selected notification is attempted; the first callback exception is
   * rethrown only after the remaining slots have been invoked.
   */
  bool request_cancellation(ComputeRunCancellationReason reason) const;

  /**
   * @brief Reports whether the matching Run still exists.
   * @return True while some observer or lease retains the Run control block.
   * @throws Nothing.
   */
  bool has_live_run() const noexcept { return !control_.expired(); }

 private:
  friend class ComputeRun;
  friend class ComputeRequestCancellationSource;

  /**
   * @brief Binds private authority to one Run without retaining its lifetime.
   * @param control Weakly retained matching Run control.
   * @throws Nothing.
   */
  explicit ComputeRunCancellationSource(
      std::weak_ptr<ComputeRunControl> control) noexcept
      : control_(std::move(control)) {}

  /** @brief Weak matching control; expiry makes requests terminal no-ops. */
  std::weak_ptr<ComputeRunControl> control_;
};

/**
 * @brief Private request coordinator that fans cancellation into child Runs.
 *
 * ComputeService attaches one HP Run for an ordinary request and distinct HP/RT
 * children for a realtime request. The coordinator remembers the first request
 * reason and immediately applies it to children attached after that request.
 * It separately records whether that reason actually won at least one child
 * terminal arbiter. Child Runs keep independent ids, terminal arbiters,
 * deadlines, staging, and cleanup. This fan-out component aggregates no
 * result; a realtime RunGroup composes it with independent child ownership and
 * terminal aggregation.
 *
 * @throws std::bad_alloc when coordinator or child-source storage grows during
 * attachment.
 * @throws std::system_error when coordinator or child Run locking fails.
 * @note Child sources are weak with respect to Run lifetime. The class is a
 * private launch seam and never enters an installed request or IPC value.
 */
class ComputeRequestCancellationSource final {
 public:
  /**
   * @brief Creates one empty current-request cancellation coordinator.
   * @throws std::bad_alloc when shared control allocation fails.
   */
  ComputeRequestCancellationSource();

  /**
   * @brief Attaches one independent child Run source to this request.
   * @param run Current HP or RT child Run; duplicate attachment is idempotent.
   * @return Nothing.
   * @throws std::bad_alloc when source storage grows.
   * @throws std::system_error when coordinator or Run locking fails.
   * @throws Any exception from a child cleanup callback when attaching after
   * request cancellation.
   * @note If cancellation was already requested, the new child is cancelled
   * before this call returns using the stable first request reason.
   */
  void attach(ComputeRun& run);

  /**
   * @brief Broadcasts one private current-request cancellation reason.
   * @param reason Explicit, supersession, deadline, close, or shutdown reason
   * proposed by the trusted request owner.
   * @return True only for the first request-level acceptance.
   * @throws std::system_error when coordinator or child Run locking fails.
   * @throws Any exception from a child cleanup callback after the request
   * reason becomes stable.
   * @note The no-argument form remains explicit cancellation. The first call
   * transfers already attached weak child sources into an allocation-free local
   * snapshot before fan-out. Every child is attempted and the first child
   * exception is rethrown afterward. A true result does not imply every child
   * was still open; each child arbitrates the stable first request reason
   * independently.
   */
  bool request_cancellation(ComputeRunCancellationReason reason =
                                ComputeRunCancellationReason::ExplicitRequest);

  /**
   * @brief Fans out an already-committed lifecycle cancellation without unwind.
   * @param reason GraphClose or ProcessShutdown selected by the registry.
   * @return True only for the first request-level acceptance.
   * @throws Nothing; synchronization/structural failure terminates, while
   * cleanup callback failures are contained after every child is attempted.
   * @note Call only after a lifecycle Closing/Stopping transition has
   * linearized. At that point rollback is impossible and losing the
   * preallocated dispatch record would violate settlement progress.
   */
  bool request_cancellation_after_linearization(
      ComputeRunCancellationReason reason) noexcept;

  /**
   * @brief Returns the stable accepted request reason when requested.
   * @return Stable first request reason, otherwise nullopt.
   * @throws std::system_error when coordinator locking fails.
   * @note Presence records request-level acceptance only; use
   * accepted_child_cancellation_reason() to determine whether fan-out won an
   * attached child terminal arbiter.
   */
  std::optional<ComputeRunCancellationReason> accepted_reason() const;

  /**
   * @brief Returns the group reason only after it wins a child cancellation.
   * @return Stable first request reason after it claims at least one attached
   * child terminal arbiter, otherwise nullopt.
   * @throws std::system_error when coordinator locking fails.
   * @note An accepted request after every child has already succeeded or failed
   * remains observable through accepted_reason() but is absent here. The child
   * winner publishes before its Cancelled terminal outcome becomes observable,
   * so RunGroup aggregation cannot miss a cancellation it already observed.
   */
  std::optional<ComputeRunCancellationReason>
  accepted_child_cancellation_reason() const;

 private:
  friend class ::ps::testing::ComputeRequestCancellationSourceTestAccess;

  /**
   * @brief Repository-test observer after request-reason linearization.
   * @param context Opaque test context.
   * @return Nothing.
   * @throws Test-injected synchronization exception.
   * @note Production leaves this pointer null. The post-lifecycle method
   * catches any escape and terminates before a dispatch record can be lost.
   */
  using AfterLinearizationObserver = void (*)(void* context);

  /** @brief Optional repository-test post-linearization observer. */
  AfterLinearizationObserver after_linearization_observer_ = nullptr;
  /** @brief Opaque context paired with after_linearization_observer_. */
  void* after_linearization_observer_context_ = nullptr;

  /** @brief Shared coordinator state retained by request/test launch owners. */
  std::shared_ptr<ComputeRequestCancellationControl> control_;
};

/**
 * @brief Move-only lifetime for one private Run cancellation notification.
 *
 * Destruction deactivates the callback and synchronizes with any invocation
 * already selected by cancellation publication. It neither requests
 * cancellation nor retains Run control ownership.
 *
 * @throws Nothing from movement and destruction.
 * @note The registration is used by ExecutionService and realtime sibling
 * coordination only; it never crosses the installed Policy ABI.
 */
class ComputeRunCancellationRegistration final {
 public:
  /**
   * @brief Creates an inactive registration value.
   * @throws Nothing.
   */
  ComputeRunCancellationRegistration() noexcept = default;

  /**
   * @brief Transfers notification lifetime from another registration.
   * @param other Registration left inactive.
   * @throws Nothing.
   */
  ComputeRunCancellationRegistration(
      ComputeRunCancellationRegistration&& other) noexcept;

  /**
   * @brief Replaces this registration through synchronized transfer.
   * @param other Registration left inactive.
   * @return Reference to this value.
   * @throws Nothing.
   */
  ComputeRunCancellationRegistration& operator=(
      ComputeRunCancellationRegistration&& other) noexcept;

  /**
   * @brief Deactivates this notification and waits for an active callback.
   * @throws Nothing; synchronization failure terminates as an invariant breach.
   */
  ~ComputeRunCancellationRegistration() noexcept;

  /**
   * @brief Prevents copying one notification-deactivation owner.
   * @param other Registration that cannot be copied.
   * @throws Nothing because the operation is deleted.
   */
  ComputeRunCancellationRegistration(
      const ComputeRunCancellationRegistration&) = delete;

  /**
   * @brief Prevents copy assignment of notification lifetime.
   * @param other Registration that cannot be assigned.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ComputeRunCancellationRegistration& operator=(
      const ComputeRunCancellationRegistration&) = delete;

  /**
   * @brief Reports whether this value owns an installed notification slot.
   * @return True before movement or reset.
   * @throws Nothing.
   */
  bool active() const noexcept { return slot_ != nullptr; }

 private:
  friend class ComputeRunLease;

  /**
   * @brief Owns one installed callback slot.
   * @param slot Shared slot retained by Run control during notification.
   * @throws Nothing.
   */
  explicit ComputeRunCancellationRegistration(
      std::shared_ptr<ComputeRunCancellationSlot> slot) noexcept
      : slot_(std::move(slot)) {}

  /** @brief Deactivates and releases the current slot. */
  void reset() noexcept;

  /** @brief Shared slot whose own mutex serializes invoke/deactivate. */
  std::shared_ptr<ComputeRunCancellationSlot> slot_;
};

/**
 * @brief Move-only one-shot ownership of an accepted Run commit attempt.
 *
 * A contender can be minted only by a matching lease while the Run is
 * `CommitPending`, after a fresh deadline observation, and while the terminal
 * arbiter is open. It blocks later cancellation/failure contenders until its
 * owner resolves success or exact failure.
 *
 * @throws std::system_error when terminal synchronization fails.
 * @note Product code acquires this value inside the graph-state work item and
 * resolves it before that item returns. Destroying an unresolved value
 * publishes an internal failure; it never silently reopens the arbiter.
 */
class ComputeRunCommitContender final {
 public:
  /**
   * @brief Transfers the one-shot commit claim.
   * @param other Claim left empty.
   * @throws Nothing.
   */
  ComputeRunCommitContender(ComputeRunCommitContender&& other) noexcept;

  /**
   * @brief Transfers a claim after failing any unresolved previous claim.
   * @param other Claim left empty.
   * @return Reference to this contender.
   * @throws Nothing; failure to resolve an abandoned claim terminates.
   */
  ComputeRunCommitContender& operator=(
      ComputeRunCommitContender&& other) noexcept;

  /**
   * @brief Resolves an accidentally abandoned claim as internal failure.
   * @throws Nothing; allocation/synchronization failure terminates.
   */
  ~ComputeRunCommitContender() noexcept;

  /**
   * @brief Prevents copying one exclusive commit claim.
   * @param other Contender that cannot be copied.
   * @throws Nothing because the operation is deleted.
   */
  ComputeRunCommitContender(const ComputeRunCommitContender&) = delete;

  /**
   * @brief Prevents copy assignment of exclusive commit ownership.
   * @param other Contender that cannot be assigned.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ComputeRunCommitContender& operator=(const ComputeRunCommitContender&) =
      delete;

  /**
   * @brief Resolves this accepted commit after no-throw visible publication.
   * @return True only for this still-active matching contender.
   * @throws std::system_error if Run-state locking fails.
   * @note Success becomes observable before the serialized transaction returns.
   */
  bool publish_succeeded();

  /**
   * @brief Resolves visible HP success and its terminal event atomically.
   * @param output Exact immutable Value already published to the live Graph.
   * @return True only for this still-active matching contender; false emits
   * neither the current-visible nor terminal callback.
   * @throws std::system_error if Run-state locking fails.
   * @note The current-visible callback is emitted before the Succeeded terminal
   * callback while the same `CommitClaimed` arbitration remains owned. Product
   * code calls this only after no-throw live Graph publication.
   */
  bool publish_visible_succeeded(Value output);

  /**
   * @brief Resolves this accepted commit with its exact predicate/persist
   * error.
   * @param failure Non-null exception captured inside the commit transaction.
   * @return True only for this still-active matching contender.
   * @throws std::invalid_argument when failure is null.
   * @throws std::system_error if Run-state locking fails.
   */
  bool publish_failed(std::exception_ptr failure);

  /**
   * @brief Reports whether this value still owns an unresolved claim.
   * @return True before either terminal resolution or movement.
   * @throws Nothing.
   */
  bool active() const noexcept { return control_ != nullptr; }

 private:
  friend class ComputeRunLease;

  /**
   * @brief Binds the claim accepted under matching Run synchronization.
   * @param control Strong Run control retained through transaction resolution.
   * @throws Nothing.
   */
  explicit ComputeRunCommitContender(
      std::shared_ptr<ComputeRunControl> control) noexcept
      : control_(std::move(control)) {}

  /** @brief Publishes internal failure for an unresolved owned claim. */
  void abandon() noexcept;

  /** @brief Matching control while this contender remains unresolved. */
  std::shared_ptr<ComputeRunControl> control_;
};

/**
 * @brief Non-authoritative strong observer for registry settlement waits.
 *
 * The observer retains one Run control block without incrementing its active
 * lease count. It exists so RunLifecycleRegistry can release its lifecycle
 * fence before terminal, quiescence, and resource-settlement waits without
 * retaining a pointer into mutable registry storage or manufacturing another
 * execution/commit lease.
 *
 * @throws std::system_error when Run synchronization or waiting fails.
 * @note Only a matching ComputeRunLease can mint this source-private value.
 * Copying retains control lifetime but grants no task, commit, cancellation,
 * resource, or finalization authority.
 */
class ComputeRunSettlementObserver final {
 public:
  /** @brief Creates an inactive observer for fixed-size registry staging. */
  ComputeRunSettlementObserver() noexcept = default;

  /** @brief Retains the same non-authoritative Run observation. */
  ComputeRunSettlementObserver(const ComputeRunSettlementObserver&) noexcept =
      default;

  /** @brief Replaces this value with the same retained observation. */
  ComputeRunSettlementObserver& operator=(
      const ComputeRunSettlementObserver&) noexcept = default;

  /** @brief Transfers one retained observation. */
  ComputeRunSettlementObserver(ComputeRunSettlementObserver&&) noexcept =
      default;

  /** @brief Replaces this value by transfer. */
  ComputeRunSettlementObserver& operator=(
      ComputeRunSettlementObserver&&) noexcept = default;

  /** @brief Releases passive Run-control observation. */
  ~ComputeRunSettlementObserver() noexcept = default;

  /**
   * @brief Returns the current immutable terminal outcome.
   * @return Outcome snapshot, or nullopt before terminal publication.
   * @throws std::logic_error for an inactive observer.
   * @throws std::system_error when Run synchronization fails.
   */
  std::optional<ComputeRunTerminalOutcome> terminal_outcome() const;

  /**
   * @brief Waits until only the registry's authoritative lease remains.
   * @return Nothing after callback, queue, continuation, and caller leases
   * release.
   * @throws std::logic_error for an inactive observer or nonterminal Run.
   * @throws std::system_error when Run synchronization or waiting fails.
   * @note This observer is deliberately absent from active lease accounting.
   */
  void wait_until_registry_lease() const;

  /**
   * @brief Waits for all physical root reservations to settle.
   * @return Nothing after pending and live root counts both reach zero.
   * @throws std::logic_error for an inactive observer or nonterminal Run.
   * @throws std::system_error when Run synchronization or waiting fails.
   */
  void wait_for_resource_settlement() const;

  /**
   * @brief Reserves one coordinate for a registry-owned lifecycle transition.
   * @return Coordinate from the Run's sink, or nullopt when observation is
   * disabled.
   * @throws Nothing; sink exhaustion remains encoded in returned evidence.
   * @note RunLifecycleRegistry calls this while its lifecycle fence excludes a
   * competing transition, then commits the matching state before delivery.
   */
  std::optional<ComputeRunObservationCoordinate>
  reserve_lifecycle_observation_coordinate() const noexcept;

  /**
   * @brief Delivers one already-reserved physical-quiescence observation.
   * @param coordinate Optional coordinate reserved before the transition.
   * @return Nothing.
   * @throws Nothing; sink callbacks contain every failure.
   * @note A missing coordinate means the Run has no observation sink. This
   * method changes no Run or registry lifecycle state.
   */
  void observe_run_quiescent(
      std::optional<ComputeRunObservationCoordinate> coordinate) const noexcept;

  /**
   * @brief Delivers one already-reserved resource-settlement observation.
   * @param coordinate Optional coordinate reserved before the transition.
   * @return Nothing.
   * @throws Nothing; sink callbacks contain every failure.
   * @note The registry invokes this only after exact root settlement and never
   * uses the callback result for lifecycle decisions.
   */
  void observe_run_resource_settled(
      std::optional<ComputeRunObservationCoordinate> coordinate) const noexcept;

  /**
   * @brief Reports whether this value retains one Run control block.
   * @return True for an active observer.
   * @throws Nothing.
   */
  bool active() const noexcept { return control_ != nullptr; }

 private:
  friend class ComputeRunLease;

  /**
   * @brief Retains one lease-validated Run control without lease authority.
   * @param control Matching Run control.
   * @throws Nothing.
   */
  explicit ComputeRunSettlementObserver(
      std::shared_ptr<ComputeRunControl> control) noexcept
      : control_(std::move(control)) {}

  /** @brief Strong passive Run-control lifetime retained across waits. */
  std::shared_ptr<ComputeRunControl> control_;
};

/**
 * @brief Non-forgeable strong ownership of one ComputeRun control block.
 *
 * A lease is minted only by ComputeRun::acquire_lease() and remains bound to
 * that Run for its complete lifetime. Copying retains another active lease;
 * moving transfers one active lease without changing the count. Ready-task
 * callbacks use the lease to route composite task identity into the matching
 * Run-owned plan.
 *
 * @throws std::system_error from state observation or routed execution when a
 * valid mutex cannot be locked.
 * @note Destruction is non-throwing, publishes no terminal outcome, and never
 * requests cancellation. This type is private backend API, not an installed
 * Host or Policy ABI contract.
 */
class ComputeRunLease {
 public:
  /**
   * @brief Retains another active lease to the same control block.
   *
   * @param other Existing non-forgeable lease.
   * @throws Nothing.
   * @note Copying is required by execution-service std::function callbacks.
   */
  ComputeRunLease(const ComputeRunLease& other) noexcept;

  /**
   * @brief Replaces this lease with another retained control block.
   *
   * @param other Existing lease whose control block is retained.
   * @return Reference to this lease.
   * @throws Nothing.
   * @note The previous lease is released before the new lease is retained.
   */
  ComputeRunLease& operator=(const ComputeRunLease& other) noexcept;

  /**
   * @brief Transfers one active lease without incrementing its count.
   *
   * @param other Lease left empty after transfer.
   * @throws Nothing.
   */
  ComputeRunLease(ComputeRunLease&& other) noexcept;

  /**
   * @brief Replaces this lease by transferring another active lease.
   *
   * @param other Lease left empty after transfer.
   * @return Reference to this lease.
   * @throws Nothing.
   */
  ComputeRunLease& operator=(ComputeRunLease&& other) noexcept;

  /**
   * @brief Releases this active lease passively.
   *
   * @throws Nothing.
   * @note Releasing the last lease may make an observer-visible Run quiescent,
   * but never changes terminal state.
   */
  ~ComputeRunLease() noexcept;

  /**
   * @brief Returns the immutable descriptor retained by this lease.
   *
   * @return Borrowed descriptor valid while this lease remains alive.
   * @throws Nothing.
   */
  const ComputeRunDescriptor& descriptor() const noexcept;

  /**
   * @brief Estimates shared Host-owned control and installed Run storage.
   * @return Checked control block, descriptor string, plan, and HP staging
   * bytes retained by this lease.
   * @throws GraphError when checked structural arithmetic overflows.
   * @throws std::system_error when control/staging mutex locking fails.
   * @note The returned shared estimate must be charged once per service batch,
   * not once per copied lease. Graph state, image pixels, and opaque
   * plugin/backend owners are excluded.
   */
  std::uint64_t retained_memory_bytes() const;

  /**
   * @brief Estimates one cancellation-notification allocation envelope.
   *
   * @param callback_capture_bytes Structural bytes captured by the registered
   * callback and conservatively charged in case std::function owns them out of
   * line.
   * @return Checked slot object, shared-control, and callback-capture bytes.
   * @throws GraphError when checked structural arithmetic overflows.
   * @note The Run's weak-slot vector capacity is part of
   * retained_memory_bytes() and is intentionally excluded here.
   * Allocator-private metadata remains outside the structural accounting
   * boundary.
   */
  static std::uint64_t cancellation_notification_retained_memory_bytes(
      std::uint64_t callback_capture_bytes = 0U);

  /**
   * @brief Builds a composite identity in this lease's Run namespace.
   *
   * @param local_task_id Dense local value to pair with the retained Run id.
   * @return Composite Run/local identity.
   * @throws Nothing.
   * @note Registration is checked separately before task or failure routing.
   */
  ComputeRunTaskIdentity task_identity(uint64_t local_task_id) const noexcept;

  /**
   * @brief Tests whether an identity names a task registered by this Run.
   *
   * @param identity Candidate composite identity.
   * @return true only when both Run id and local plan registration match.
   * @throws std::system_error if the control mutex cannot be locked.
   * @note The check grants no execution by itself.
   */
  bool accepts_task_identity(const ComputeRunTaskIdentity& identity) const;

  /**
   * @brief Publishes a matching task failure through this Run's arbiter.
   *
   * @param identity Composite identity of the failing registered task.
   * @param failure Exact non-null exception captured at the worker boundary.
   * @return true only when identity matched and this failure won the terminal
   * arbiter; false for mismatch or an already terminal Run.
   * @throws std::invalid_argument when failure is null.
   * @throws std::system_error if the control mutex cannot be locked.
   * @note A mismatched identity mutates neither this Run nor the identity's
   * named Run.
   */
  bool publish_task_failure(const ComputeRunTaskIdentity& identity,
                            std::exception_ptr failure);

  /**
   * @brief Returns the current terminal outcome retained by the lease.
   *
   * @return Outcome snapshot, or nullopt before terminal publication.
   * @throws std::system_error if the control mutex cannot be locked.
   * @note This remains observable after the original ComputeRun observer is
   * destroyed.
   */
  std::optional<ComputeRunTerminalOutcome> terminal_outcome() const;

  /**
   * @brief Waits until this value is the only remaining active Run lease.
   *
   * @return Nothing after all callback, queue, continuation, and caller-owned
   * lease copies have released.
   * @throws std::logic_error unless a terminal outcome is already published.
   * @throws std::system_error when Run synchronization or waiting fails.
   * @note Only `RunLifecycleRegistry` invokes this on its final retained lease.
   * The wait has no finite bound when an entered callback never returns and
   * never holds the lifecycle registry fence.
   */
  void wait_until_only_lease() const;

  /**
   * @brief Waits for every attached physical root reservation to settle.
   *
   * @return Nothing after root owners closed, all child grants released, exact
   * ledger capacity returned, and companion quota observers completed.
   * @throws std::logic_error unless a terminal outcome is already published.
   * @throws std::system_error when Run synchronization or waiting fails.
   * @note RunLifecycleRegistry invokes this only after
   * wait_until_only_lease(), which closes future physical observation
   * registration. The ResourceLedger callback is non-owning and allocation-free
   * and never runs under the lifecycle registry fence.
   */
  void wait_for_resource_settlement() const;

  /**
   * @brief Observes explicit cancellation and the immutable Run deadline.
   * @return Stable cancellation reason when cancellation owns the terminal
   * outcome, otherwise nullopt for an open, failed, or succeeded Run.
   * @throws std::system_error when Run-state synchronization fails.
   * @throws Any exception from a contract-violating injected clock.
   * @throws Any exception from a registered cleanup callback when deadline
   * cancellation wins.
   * @note An expired injected monotonic deadline proposes `DeadlineExceeded`
   * through the same arbiter. Leases cannot choose any other reason.
   */
  std::optional<ComputeRunCancellationReason> observe_cancellation() const;

  /**
   * @brief Installs a private notification for accepted Run cancellation.
   * @param callback Non-empty cleanup callback receiving the stable reason.
   * @return Move-only registration that synchronizes callback deactivation.
   * @throws std::invalid_argument when callback is empty.
   * @throws std::bad_alloc when callback/slot storage cannot allocate.
   * @throws std::system_error when Run-state synchronization fails.
   * @throws Any callback exception when registering after cancellation.
   * @note Registration on an already cancelled Run invokes the callback before
   * return. Callbacks run without the Run control mutex and must be idempotent.
   */
  ComputeRunCancellationRegistration register_cancellation_notification(
      std::function<void(ComputeRunCancellationReason)> callback) const;

  /**
   * @brief Binds one progressive final gate to cancellation arbitration.
   * @param gate Non-null request gate shared by the progressive RT/HP children.
   * @return Nothing.
   * @throws std::invalid_argument when `gate` is null.
   * @throws std::logic_error when this Run is already bound to another gate.
   * @throws std::system_error when Run-state synchronization fails.
   * @note Accepted cancellation denies the bound gate while holding the Run
   * terminal mutex and before publishing `Cancelled`; cleanup callbacks remain
   * outside that mutex and are not part of final-trigger linearization. Binding
   * an already-cancelled Run denies the gate before return. The gate retains no
   * Run, scheduler, currentness, resource, or commit authority.
   */
  void bind_progressive_final_gate(
      const std::shared_ptr<ProgressiveFinalGate>& gate) const;

  /**
   * @brief Atomically consumes and observes progressive final permission.
   * @return True only when this open Run consumed its bound Armed gate and
   * published the unique final-trigger observation.
   * @throws std::system_error when Run-state synchronization fails.
   * @throws Any exception from a contract-violating injected clock or from a
   * cancellation cleanup callback when deadline cancellation wins.
   * @note Deadline cancellation is observed first. The subsequent Open check,
   * gate consumption, coordinate reservation, and observation callback all
   * occur while the same Run terminal-arbiter mutex is held. Cancellation that
   * wins first publishes no trigger; a trigger winner is observed before any
   * later cancellation or terminal observation. A missing or non-Armed gate
   * returns false without creating HP submission authority.
   */
  bool try_publish_progressive_final_trigger() const;

  /**
   * @brief Attempts to reserve the terminal arbiter for visible commit.
   * @return Active one-shot contender when phase/deadline/arbiter permit
   * commit, otherwise nullopt without changing an existing terminal outcome.
   * @throws std::system_error when Run-state synchronization fails.
   * @throws Any exception from a contract-violating injected clock.
   * @throws Any exception from a registered cleanup callback when deadline
   * cancellation wins.
   * @note Deadline is observed immediately before the atomic claim. A deadline
   * winner denies any bound progressive gate before publishing cancellation.
   * Only the returned contender can resolve an accepted commit claim.
   */
  std::optional<ComputeRunCommitContender> try_claim_commit() const;

  /**
   * @brief Executes one registered task through the matching Run-owned plan.
   *
   * @param identity Composite task identity carried by an accepted callback.
   * @param task_runtime Execution runtime used for trace, ready submission, and
   * completion accounting.
   * @param callback_owns_completion Whether a runtime exact-once token, rather
   * than this lease route, owns retirement of the callback's pre-counted unit.
   * @return Nothing.
   * @throws std::invalid_argument when identity does not match this lease or a
   * registered local task.
   * @throws std::system_error if the control mutex cannot be locked.
   * @throws Exceptions propagated by Run-owned plan execution, execution
   * completion accounting, dependent-callback submission, or matching failure
   * publication.
   * @note A matching accepted callback whose Run is already terminal releases
   * its previously counted completion unit without entering plan execution. An
   * active valid task releases that unit only through successful plan
   * execution; an execution exception is passed to this Run's failure publisher
   * before unchanged rethrow to execution transport. If that publication
   * throws, its exception propagates instead.
   */
  void execute_task(const ComputeRunTaskIdentity& identity,
                    ExecutionTaskRuntime& task_runtime,
                    bool callback_owns_completion = false);

  /**
   * @brief Routes one terminal pending-Value continuation to its exact task.
   * @param identity Composite identity previously left AwaitingValue.
   * @param task_runtime Active runtime owning the added completion unit.
   * @param snapshot Terminal producer-completion observation.
   * @return Nothing.
   * @throws std::invalid_argument for a mismatched task or lease.
   * @throws ReadyFenceAccessError, GraphError, or runtime exceptions from
   * terminal materialization, dependency release, or completion retirement.
   * @note This route retires the dynamically added completion unit on every
   * normal, terminal-skip, and exceptional path. Active failures are published
   * to the exact ComputeRun before unchanged rethrow.
   */
  void complete_deferred_value(const ComputeRunTaskIdentity& identity,
                               ExecutionTaskRuntime& task_runtime,
                               ReadyFenceSnapshot snapshot);

  /**
   * @brief Runs the full-HP execution bootstrap through this lease.
   *
   * @param task_runtime Active execution batch receiving initial owned
   * callbacks and completion accounting.
   * @return Nothing.
   * @throws GraphError or standard exceptions from ready discovery,
   * completion accounting, trace publication, or callback submission.
   * @note A matching accepted bootstrap whose Run is already terminal releases
   * its previously counted completion unit without publishing planned work.
   * Otherwise the bootstrap unit is released only after every initial callback
   * is accepted; an exception is published to this Run and rethrown.
   */
  void execute_bootstrap(ExecutionTaskRuntime& task_runtime);

 private:
  friend class ComputeRun;
  friend class ComputeRunControl;
  friend class ExecutionService;
  friend class RunLifecycleRegistry;

  /**
   * @brief Mints the first active lease for one Run control block.
   *
   * @param control Shared Run state retained by this lease.
   * @throws Nothing.
   * @note Only ComputeRun may call this constructor.
   */
  explicit ComputeRunLease(std::shared_ptr<ComputeRunControl> control) noexcept;

  /**
   * @brief Increments the active lease count for a copied control block.
   *
   * @return Nothing.
   * @throws Nothing.
   */
  void retain() noexcept;

  /**
   * @brief Decrements the active lease count and clears this reference.
   *
   * @return Nothing.
   * @throws Nothing.
   */
  void release() noexcept;

  /**
   * @brief Mints a passive strong observer for registry settlement.
   * @return Observer retaining this Run without incrementing active leases.
   * @throws Nothing.
   * @note Only RunLifecycleRegistry uses this bridge; it grants no execution or
   * commit authority.
   */
  ComputeRunSettlementObserver settlement_observer() const noexcept {
    return ComputeRunSettlementObserver(control_);
  }

  /**
   * @brief Registers one pending physical root before ledger admission.
   * @param telemetry Stable service telemetry owner outliving this Run.
   * @return Non-owning callback for the matching ResourceLedger reservation.
   * @throws std::overflow_error when the per-Run pending count is exhausted.
   * @throws std::system_error when Run synchronization fails.
   * @note ExecutionService must call cancel_resource_settlement_observation()
   * when reservation admission does not commit.
   */
  ResourceLedger::ReservationSettlementObserver
  begin_resource_settlement_observation(
      ExecutionLifecycleTelemetry& telemetry) const;

  /**
   * @brief Commits one pending observation after ledger root admission.
   * @return Nothing.
   * @throws Nothing; pending underflow or telemetry failure terminates.
   * @note The matching root cannot settle before this call because its
   * Reservation owner is still retained by the admission caller.
   */
  void commit_resource_settlement_observation() const noexcept;

  /**
   * @brief Rolls back one pending observation when ledger admission fails.
   * @return Nothing.
   * @throws Nothing; underflow or synchronization failure terminates.
   */
  void cancel_resource_settlement_observation() const noexcept;

  /**
   * @brief Completes one pending observation after exact ledger root release.
   * @param context Borrowed stable ComputeRunControl address.
   * @return Nothing.
   * @throws Nothing; underflow or synchronization failure terminates.
   */
  static void complete_resource_settlement_observation(void* context) noexcept;

  /**
   * @brief Observes one successfully minted ready or execution child grant.
   * @param context Borrowed stable ComputeRunControl address.
   * @return Nothing.
   * @throws Nothing; missing telemetry or counter failure terminates.
   */
  static void observe_child_granted(void* context) noexcept;

  /**
   * @brief Observes exact release of one ready or execution child grant.
   * @param context Borrowed stable ComputeRunControl address.
   * @return Nothing.
   * @throws Nothing; missing telemetry or counter underflow terminates.
   */
  static void observe_child_released(void* context) noexcept;

  /**
   * @brief Arbitrates one irreversible physical start against Run terminal.
   * @param commit_callback Non-null allocation-free route commit operation.
   * @param context Borrowed callback context valid until return.
   * @param committed_coordinate Output coordinate present only when the sink
   * exists and the route commit succeeds.
   * @return True only when the Run remained open and the callback committed.
   * @throws std::invalid_argument when callback or output storage is null.
   * @throws std::system_error when Run-state synchronization fails.
   * @note ExecutionService already holds `pool -> RunState` locks after
   * releasing the resource-reservation mutex. This method then takes the Run
   * terminal-arbiter mutex, reserves the observation coordinate, and invokes
   * the irreversible callback. Cancellation accepted first prevents the
   * callback; a callback that returns false explicitly retires the reserved
   * coordinate through the observation sink. No service-start observation
   * callback runs under this lock.
   */
  bool try_commit_service_start(
      ComputeRunServiceStartCommitCallback commit_callback, void* context,
      std::optional<ComputeRunObservationCoordinate>* committed_coordinate)
      const;

  /** @brief Shared Run control retained independently from observers. */
  std::shared_ptr<ComputeRunControl> control_;
};

/**
 * @brief Request observer for one single-domain HP or RT execution Run.
 *
 * ComputeRun creates the shared control block established by issues #66 and
 * #67. The observer settles service-level lifecycle, while non-forgeable
 * leases keep dispatcher state, owned callbacks, temporary results, exception
 * state, and dirty HP staging alive independently from the original observer.
 *
 * @throws std::invalid_argument when constructed with unsupported intent,
 * intent/quality mismatch, nonpositive QoS weight, or zero maximum parallelism.
 * @throws std::overflow_error when process-lifetime Run identities are
 * exhausted.
 * @throws std::bad_alloc when descriptor or storage ownership cannot allocate.
 * @note Full/dirty HP and RT work uses stable leases and composite task
 * identity through ExecutionService. The private injected runtime retains the
 * synchronously drained borrowed-handle path for repository-internal callers.
 * Realtime transactions own
 * separate current HP and RT child Runs.
 */
class ComputeRun {
 public:
  /**
   * @brief Constructs one Run and mints a fresh identity.
   *
   * @param submission Immutable request values captured before this domain's
   * planning and preflight.
   * @param monotonic_clock Non-empty private steady-clock source used for
   * deterministic deadline observation.
   * @throws std::invalid_argument for unsupported intent, intent/quality
   * mismatch, or invalid QoS.
   * @throws std::overflow_error when no non-reused Run id remains.
   * @throws std::bad_alloc when descriptor ownership cannot allocate.
   * @note Construction leaves the Run in Created with no terminal outcome or
   * execution storage.
   */
  explicit ComputeRun(
      ComputeRunSubmission submission,
      ComputeRunMonotonicClock monotonic_clock = [] {
        return std::chrono::steady_clock::now();
      });

  /**
   * @brief Passively releases the request observer.
   *
   * @throws Nothing.
   * @note Destruction never publishes cancellation or another terminal
   * outcome. Active leases retain the shared control block after this observer
   * disappears.
   */
  ~ComputeRun() noexcept;

  /**
   * @brief Prevents two owners from copying one Run identity and arbiter.
   *
   * @param other Source Run that cannot be copied.
   * @throws Nothing because the operation is deleted.
   * @note One request has one settlement observer; callback ownership is
   * represented only by ComputeRunLease.
   */
  ComputeRun(const ComputeRun& other) = delete;

  /**
   * @brief Prevents copy assignment from replacing Run identity/state.
   *
   * @param other Source Run that cannot be assigned.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   * @note ComputeRun identity and mutex state are never replaceable.
   */
  ComputeRun& operator=(const ComputeRun& other) = delete;

  /**
   * @brief Prevents moving storage away from its request-owned address.
   *
   * @param other Source Run that cannot be moved.
   * @throws Nothing because the operation is deleted.
   * @note Moving an observer would obscure the single service settlement
   * boundary even though leased control storage is address-stable.
   */
  ComputeRun(ComputeRun&& other) = delete;

  /**
   * @brief Prevents move assignment of Run identity, mutex, and storage.
   *
   * @param other Source Run that cannot be assigned.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   * @note Stable callback ownership uses ComputeRunLease instead.
   */
  ComputeRun& operator=(ComputeRun&& other) = delete;

  /**
   * @brief Returns the immutable request descriptor.
   *
   * @return Borrowed descriptor valid for the Run lifetime.
   * @throws Nothing.
   */
  const ComputeRunDescriptor& descriptor() const noexcept {
    return descriptor_ref();
  }

  /**
   * @brief Acquires one non-forgeable active lease before accepting work.
   *
   * @return Strong lease retaining the shared Run control block.
   * @throws std::logic_error when the Run is already terminal.
   * @throws std::system_error if the control mutex cannot be locked.
   * @note Dispatcher and commit continuations keep their own lease while every
   * accepted callback receives a copied or moved lease.
   */
  ComputeRunLease acquire_lease() const;

  /**
   * @brief Mints private cancellation authority for this exact Run.
   * @return Weak-lifetime source bound to this Run's terminal arbiter.
   * @throws Nothing.
   * @note Ordinary callbacks receive leases instead and cannot request an
   * arbitrary cancellation reason.
   */
  ComputeRunCancellationSource cancellation_source() const noexcept;

  /**
   * @brief Observes explicit cancellation and the immutable Run deadline.
   * @return Stable cancellation reason when this Run is cancelled, otherwise
   * nullopt.
   * @throws std::system_error when Run-state synchronization fails.
   * @throws Any exception from a contract-violating injected clock.
   * @throws Any exception from a registered cleanup callback when deadline
   * cancellation wins.
   * @note This request-observer convenience has the same read-only authority as
   * lease observation.
   */
  std::optional<ComputeRunCancellationReason> observe_cancellation() const;

  /**
   * @brief Reports current Run-local physical quiescence.
   *
   * @return true only when no active ComputeRunLease exists.
   * @throws Nothing.
   * @note Terminal publication does not imply quiescence. This count does not
   * represent future graph/resource/registry settlement.
   */
  bool is_quiescent() const noexcept;

  /**
   * @brief Returns the current phase snapshot.
   *
   * @return Terminal after outcome publication, otherwise latest nonterminal
   * phase.
   * @throws std::system_error if mutex locking fails.
   * @note The snapshot may become stale immediately when another thread settles
   * the Run.
   */
  ComputeRunPhase phase() const;

  /**
   * @brief Advances to a later applicable nonterminal phase.
   *
   * @param next Requested phase; Terminal is invalid here.
   * @return true when the phase advanced, false for the same phase or after a
   * terminal outcome already exists.
   * @throws std::invalid_argument when next is Terminal.
   * @throws std::logic_error when next precedes the current phase.
   * @throws std::system_error if mutex locking fails.
   * @note Safe forward skips are supported for cache-hit and inline paths.
   */
  bool advance_to(ComputeRunPhase next);

  /**
   * @brief Publishes successful terminal outcome if the arbiter is unclaimed.
   *
   * @return true only for the winning terminal publication.
   * @throws std::system_error if mutex locking fails.
   * @note Callers must invoke this only after validated commit or validated
   * reusable-output success.
   */
  bool publish_succeeded();

  /**
   * @brief Publishes failed terminal outcome with original exception identity.
   *
   * @param failure Non-null exception pointer caught at the service boundary.
   * @return true only for the winning terminal publication.
   * @throws std::invalid_argument when failure is null.
   * @throws std::system_error if mutex locking fails.
   * @note The method stores exception identity without rethrowing it.
   */
  bool publish_failed(std::exception_ptr failure);

  /**
   * @brief Copies the current terminal outcome if one exists.
   *
   * @return Published outcome snapshot, otherwise nullopt.
   * @throws std::system_error if mutex locking fails.
   * @note The copied std::exception_ptr retains the original exception object.
   */
  std::optional<ComputeRunTerminalOutcome> terminal_outcome() const;

  /**
   * @brief Reports whether a terminal outcome has been published.
   *
   * @return true after any terminal claimant wins.
   * @throws std::system_error if mutex locking fails.
   */
  bool is_terminal() const;

  /**
   * @brief Constructs and owns the task submission plan for this Run.
   *
   * @param graph Graph used for HP planning and operation resolution.
   * @param traversal Traversal service used to build the cache-pruned plan.
   * @param node_id Target node id.
   * @param available_devices Devices exposed by the active execution route,
   * copied into the Run-owned plan.
   * @param publish_plan_inspection Whether plan construction immediately
   * updates GraphModel diagnostics.
   * @param allow_reusable_cache Whether exact complete formal HP cache may
   * satisfy nodes before task population.
   * @return Mutable Run-owned plan retained by the shared control block.
   * @throws std::logic_error when a plan already exists or the Run is terminal.
   * @throws GraphError or standard exceptions from plan construction.
   * @note Full-HP task callbacks reach this plan only through a matching lease.
   * Force-recache callers disable reusable cache while retaining current Graph
   * output until installed execution owns visible mutation.
   */
  TaskSubmissionPlan& emplace_submission_plan(
      GraphModel& graph, GraphTraversalService& traversal, int node_id,
      std::vector<Device> available_devices,
      bool publish_plan_inspection = true, bool allow_reusable_cache = true);

  /**
   * @brief Returns the Run-owned submission plan when installed.
   *
   * @return Borrowed plan pointer, or nullptr before construction.
   * @throws std::system_error if mutex locking fails.
   * @note The pointer remains valid until Run destruction.
   */
  TaskSubmissionPlan* submission_plan();

  /**
   * @brief Constructs and owns standalone dirty HP staged output storage.
   *
   * @param seed_existing_outputs Whether new entries copy current HP output.
   * @return Mutable Run-owned write buffer.
   * @throws std::logic_error when a buffer already exists or Run is terminal.
   * @throws std::bad_alloc when buffer allocation fails.
   * @note A realtime HP child may own this buffer; its RT sibling uses separate
   * proxy staging outside the HP Run.
   */
  HighPrecisionDirtyWriteBuffer& emplace_dirty_hp_write_buffer(
      bool seed_existing_outputs);

  /**
   * @brief Returns the Run-owned dirty HP write buffer when installed.
   *
   * @return Borrowed buffer pointer, or nullptr before construction.
   * @throws std::system_error if mutex locking fails.
   * @note The pointer remains valid until Run destruction.
   */
  HighPrecisionDirtyWriteBuffer* dirty_hp_write_buffer();

 private:
  /**
   * @brief Returns the descriptor from the shared control block.
   *
   * @return Borrowed immutable descriptor.
   * @throws Nothing.
   * @note The request observer itself retains the control block for the
   * returned reference.
   */
  const ComputeRunDescriptor& descriptor_ref() const noexcept;

  /**
   * @brief Attempts one exact terminal publication under the Run mutex.
   *
   * @param outcome Fully formed terminal value.
   * @return true when outcome won, false when another terminal already exists.
   * @throws std::system_error if mutex locking fails.
   * @note Losing contenders do not mutate phase or existing payload.
   */
  bool publish_terminal(ComputeRunTerminalOutcome outcome);

  /** @brief Shared state retained by this observer and every active lease. */
  std::shared_ptr<ComputeRunControl> control_;
};

}  // namespace ps::compute
