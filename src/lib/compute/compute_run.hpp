#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "photospider/core/compute_intent.hpp"
#include "photospider/core/device.hpp"

namespace ps {
class GraphModel;
class GraphTraversalService;
}  // namespace ps

namespace ps::compute {
class HighPrecisionDirtyWriteBuffer;
class TaskSubmissionPlan;

/**
 * @brief Opaque stable identity for one request-owned compute Run.
 *
 * ComputeRunId values are minted by the private ComputeRun implementation from
 * one process-lifetime monotonic sequence. Numeric access exists for logging,
 * tests, and later execution-service transport; callers must not derive
 * ordering, task identity, or policy from it.
 *
 * @throws Nothing for copy, comparison, and value access.
 * @note Value zero is reserved as invalid and is never minted. Issue #68 may
 * move the sequence owner into ExecutionService without changing this value
 * contract.
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
 * @brief Topology-only revision captured when issue #66 creates a Run.
 *
 * @throws Nothing for value operations.
 * @note This is not the authoritative graph-wide GraphRevision or a commit
 * predicate. Issue #72 owns that future boundary.
 */
struct ComputeRunSubmissionRevision {
  /**
   * @brief GraphModel topology generation observed before HP planning.
   *
   * @note Parameter, cache, and runtime-state changes do not advance this
   * value under the current GraphModel contract.
   */
  uint64_t topology_generation = 0;
};

/**
 * @brief Quality carried independently from compute intent and QoS.
 *
 * @throws Nothing for value operations.
 * @note Issue #66 implements only the full-quality HP Run slice.
 */
enum class ComputeRunQuality {
  /** @brief Full high-precision product output. */
  Full,
};

/**
 * @brief Scheduling service class requested independently from HP intent.
 *
 * @throws Nothing for value operations.
 * @note Current Host requests use Throughput explicitly. Future policy work may
 * supply Interactive without changing ComputeIntent semantics.
 */
enum class ComputeRunQosClass {
  /** @brief Latency-sensitive policy input reserved for future callers. */
  Interactive,

  /** @brief Completion-oriented policy input used by current HP requests. */
  Throughput,
};

/**
 * @brief Immutable-by-copy QoS inputs captured in a Run descriptor.
 *
 * The value does not own workers, reservations, or scheduler policy. It merely
 * records caller intent for later execution-domain slices.
 *
 * @throws Nothing for default construction; copying an optional time point
 * does not allocate.
 * @note Weight and maximum_parallelism are validated by ComputeRun. Intent,
 * QoS, resource admission, and commit policy remain distinct concepts.
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
 * @brief Caller-supplied immutable inputs used to construct one HP Run.
 *
 * @throws std::bad_alloc when graph_identity is copied into Run ownership.
 * @note Kernel supplies a stable session label. Direct private ComputeService
 * callers may intentionally leave graph_identity empty.
 */
struct ComputeRunSubmission {
  /** @brief Stable graph/session label supplied by the request boundary. */
  std::string graph_identity;

  /** @brief Topology-only revision captured before HP planning. */
  ComputeRunSubmissionRevision revision;

  /** @brief Target graph node id requested by the caller. */
  int target_node_id = -1;

  /** @brief Single-domain intent; issue #66 accepts only HP. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;

  /** @brief Quality independent from intent and QoS. */
  ComputeRunQuality quality = ComputeRunQuality::Full;

  /** @brief Explicit request QoS captured without applying policy. */
  ComputeRunQos qos;
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
   * @brief Returns the topology-only submission revision.
   *
   * @return Revision value captured before planning.
   * @throws Nothing.
   */
  ComputeRunSubmissionRevision revision() const noexcept { return revision_; }

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
   * @return GlobalHighPrecision for issue #66 Runs.
   * @throws Nothing.
   */
  ComputeIntent intent() const noexcept { return intent_; }

  /**
   * @brief Returns the captured quality marker.
   *
   * @return Full for issue #66 Runs.
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

 private:
  friend class ComputeRun;

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

  /** @brief Topology-only submission revision captured before planning. */
  ComputeRunSubmissionRevision revision_;

  /** @brief Graph-local requested target node id. */
  int target_node_id_ = -1;

  /** @brief Single-domain compute intent. */
  ComputeIntent intent_ = ComputeIntent::GlobalHighPrecision;

  /** @brief Output quality independent from intent and QoS. */
  ComputeRunQuality quality_ = ComputeRunQuality::Full;

  /** @brief Explicit QoS inputs retained without owning policy/resources. */
  ComputeRunQos qos_;
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

  /** @brief ComputeService accepted the request for local HP execution. */
  Admitted,

  /** @brief Scheduler-backed ready work is about to be submitted. */
  Queued,

  /** @brief Inline or scheduler-backed operation work is active. */
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
 * @note Issue #66 models terminal arbitration but exposes no Host cancellation
 * control surface.
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
 * @brief Request-owned HP execution descriptor, state, storage, and arbiter.
 *
 * ComputeRun is the issue #66 ownership boundary. It keeps immutable request
 * identity, monotonic phase, exactly-one terminal state, full HP dispatcher
 * state, temporary results, and standalone dirty HP staging alive for one
 * synchronous service execution.
 *
 * @throws std::invalid_argument when constructed with non-HP intent,
 * nonpositive QoS weight, or zero maximum parallelism.
 * @throws std::overflow_error when process-lifetime Run identities are
 * exhausted.
 * @throws std::bad_alloc when descriptor or storage ownership cannot allocate.
 * @note Scheduler/task handles remain borrowed and must synchronously drain
 * before Run destruction. Stable RunLease lifetime begins in issue #67.
 */
class ComputeRun {
 public:
  /**
   * @brief Constructs one Run and mints a fresh identity.
   *
   * @param submission Immutable request values captured before HP planning.
   * @throws std::invalid_argument for unsupported intent or invalid QoS.
   * @throws std::overflow_error when no non-reused Run id remains.
   * @throws std::bad_alloc when descriptor ownership cannot allocate.
   * @note Construction leaves the Run in Created with no terminal outcome or
   * execution storage.
   */
  explicit ComputeRun(ComputeRunSubmission submission);

  /**
   * @brief Passively destroys Run-owned storage.
   *
   * @throws Nothing.
   * @note Destruction never publishes cancellation or another terminal
   * outcome. Callers must synchronously drain borrowed scheduler handles first.
   */
  ~ComputeRun() noexcept;

  /**
   * @brief Prevents two owners from copying one Run identity and arbiter.
   *
   * @param other Source Run that cannot be copied.
   * @throws Nothing because the operation is deleted.
   * @note ComputeRun ownership is unique to one service request.
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
   * @note Current borrowed task handles require address-stable Run storage.
   */
  ComputeRun(ComputeRun&& other) = delete;

  /**
   * @brief Prevents move assignment of Run identity, mutex, and storage.
   *
   * @param other Source Run that cannot be assigned.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   * @note Stable lease-based mobility is not introduced by issue #66.
   */
  ComputeRun& operator=(ComputeRun&& other) = delete;

  /**
   * @brief Returns the immutable request descriptor.
   *
   * @return Borrowed descriptor valid for the Run lifetime.
   * @throws Nothing.
   */
  const ComputeRunDescriptor& descriptor() const noexcept {
    return descriptor_;
  }

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
   * @brief Publishes cancelled terminal outcome with a stable reason.
   *
   * @param reason Cancellation cause accepted by the Run arbiter.
   * @return true only for the winning terminal publication.
   * @throws std::system_error if mutex locking fails.
   * @note Current issue #66 product paths do not call this method; it exists so
   * exact terminal arbitration is complete before a control surface arrives.
   */
  bool publish_cancelled(ComputeRunCancellationReason reason);

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
   * @brief Constructs and owns the scheduler submission plan for this Run.
   *
   * @param graph Graph used for HP planning and operation resolution.
   * @param traversal Traversal service used to build the cache-pruned plan.
   * @param node_id Target node id.
   * @param available_devices Devices exposed by the borrowed scheduler.
   * @return Mutable Run-owned plan used until synchronous scheduler drainage
   * and commit finish.
   * @throws std::logic_error when a plan already exists or the Run is terminal.
   * @throws GraphError or standard exceptions from plan construction.
   * @note Task closures may borrow this plan only until the current dispatcher
   * wait completes; issue #67 introduces stable leases.
   */
  TaskSubmissionPlan& emplace_submission_plan(
      GraphModel& graph, GraphTraversalService& traversal, int node_id,
      std::vector<Device> available_devices);

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
   * @note Paired realtime sibling staging remains outside issue #66.
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
   * @brief Attempts one exact terminal publication under the Run mutex.
   *
   * @param outcome Fully formed terminal value.
   * @return true when outcome won, false when another terminal already exists.
   * @throws std::system_error if mutex locking fails.
   * @note Losing contenders do not mutate phase or existing payload.
   */
  bool publish_terminal(ComputeRunTerminalOutcome outcome);

  /** @brief Immutable identity and request inputs captured before planning. */
  const ComputeRunDescriptor descriptor_;

  /** @brief Guards phase, terminal arbiter, and optional storage installation.
   */
  mutable std::mutex mutex_;

  /** @brief Latest monotonic nonterminal phase before settlement. */
  ComputeRunPhase phase_ = ComputeRunPhase::Created;

  /** @brief Exactly-one terminal outcome, absent before settlement. */
  std::optional<ComputeRunTerminalOutcome> terminal_outcome_;

  /** @brief Optional Run-owned full HP plan, dependency state, and temp output.
   */
  std::unique_ptr<TaskSubmissionPlan> submission_plan_;

  /** @brief Optional Run-owned standalone dirty HP staging output. */
  std::unique_ptr<HighPrecisionDirtyWriteBuffer> dirty_hp_write_buffer_;
};

}  // namespace ps::compute
