#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "compute/compute_run.hpp"
#include "compute/dirty_sibling_commit_gate.hpp"

namespace ps::compute {

/**
 * @brief Opaque identity for one request-owned realtime RunGroup.
 * @throws Nothing for copy, comparison, and value access.
 * @note This value identifies coordination only; it is not a RunId,
 * supersession generation, GraphRevision, or lifecycle-registry entry.
 */
class RunGroupId {
 public:
  /**
   * @brief Returns the nonzero process-lifetime diagnostic value.
   * @return Opaque group identity scalar.
   * @throws Nothing.
   * @note Callers must not infer request ordering from this value.
   */
  std::uint64_t value() const noexcept { return value_; }

  /**
   * @brief Compares exact group identities.
   * @param other Candidate request-group identity.
   * @return True only when both opaque values match.
   * @throws Nothing.
   */
  bool operator==(const RunGroupId& other) const noexcept {
    return value_ == other.value_;
  }

 private:
  friend class RunGroup;

  /**
   * @brief Constructs one id from the private checked allocator.
   * @param value Fresh nonzero diagnostic scalar.
   * @throws Nothing.
   * @note Only RunGroup may mint this value.
   */
  explicit RunGroupId(std::uint64_t value) noexcept : value_(value) {}

  /** @brief Nonzero process-lifetime diagnostic identity. */
  std::uint64_t value_ = 0;
};

/**
 * @brief Request-owned coordination for one realtime HP/RT child pair.
 *
 * The group captures one supersession identity, owns distinct HP and RT Runs,
 * the request-wide cancellation fan-out source, and the monotonic RT-first
 * sibling gate. Children retain separate RunId, intent, staging, terminal,
 * plan, and resource ownership. The group owns no worker, dispatcher,
 * graph-lifetime lease, registry entry, or process-shutdown authority.
 *
 * @throws std::invalid_argument when child submissions do not describe one
 * realtime lineage with HP Full and RT Interactive children.
 * @throws std::overflow_error when RunGroup identity is exhausted.
 * @throws std::bad_alloc when group, child Run, source, or gate allocation
 * fails.
 * @throws std::logic_error or std::system_error if a freshly constructed child
 * cannot provide its group-owned observation lease.
 */
class RunGroup final {
 public:
  /**
   * @brief Creates both independent child Runs before shared preflight.
   * @param hp_submission HP Full child descriptor inputs.
   * @param rt_submission RT Interactive child descriptor inputs.
   * @param cancellation Existing request source, or null to allocate one.
   * @throws The validation/allocation errors documented by the class.
   * @note Both child descriptors must carry the exact same realtime request
   * key and generation.
   */
  RunGroup(
      ComputeRunSubmission hp_submission, ComputeRunSubmission rt_submission,
      std::shared_ptr<ComputeRequestCancellationSource> cancellation = nullptr);

  /**
   * @brief Passively releases group coordination at request scope exit.
   * @throws Nothing.
   * @note The request wrapper settles both children before destruction;
   * destruction itself requests no cancellation and publishes no outcome.
   */
  ~RunGroup() noexcept;

  /** @brief Prevents duplicating group/child terminal ownership. */
  RunGroup(const RunGroup&) = delete;
  /** @brief Prevents replacing group/child terminal ownership. */
  RunGroup& operator=(const RunGroup&) = delete;
  /** @brief Prevents moving stable child observer addresses. */
  RunGroup(RunGroup&&) = delete;
  /** @brief Prevents moving stable child observer addresses. */
  RunGroup& operator=(RunGroup&&) = delete;

  /**
   * @brief Returns the opaque request-group identity.
   * @return Stable process-lifetime diagnostic identity.
   * @throws Nothing.
   */
  RunGroupId id() const noexcept { return id_; }

  /**
   * @brief Returns the immutable shared request lineage.
   * @return Borrowed key/generation valid for the group lifetime.
   * @throws Nothing.
   */
  const SupersessionIdentity& supersession() const noexcept {
    return supersession_;
  }

  /**
   * @brief Returns the independently owned HP Full child.
   * @return Mutable child observer owned by this group.
   * @throws Nothing.
   */
  ComputeRun& hp_run() noexcept { return hp_run_; }

  /**
   * @brief Returns the independently owned RT Interactive child.
   * @return Mutable child observer owned by this group.
   * @throws Nothing.
   */
  ComputeRun& rt_run() noexcept { return rt_run_; }

  /**
   * @brief Returns the group-owned HP child observation lease.
   * @return Borrowed lease retained until explicit lifecycle release.
   * @throws std::logic_error after release_lifecycle_leases().
   */
  const ComputeRunLease& hp_lease() const;

  /**
   * @brief Returns the group-owned RT child observation lease.
   * @return Borrowed lease retained until explicit lifecycle release.
   * @throws std::logic_error after release_lifecycle_leases().
   */
  const ComputeRunLease& rt_lease() const;

  /**
   * @brief Returns the request-wide child cancellation coordinator.
   * @return Mutable fan-out source shared with the request coordinator.
   * @throws Nothing.
   */
  ComputeRequestCancellationSource& cancellation_source() noexcept {
    return *cancellation_;
  }

  /**
   * @brief Returns shared request cancellation ownership for registry fan-out.
   * @return Strong coordinator owner retained by the group and registry.
   * @throws Nothing.
   * @note The coordinator carries cancellation only; it retains child Runs
   * weakly and owns no Graph, resource, worker, or terminal result.
   */
  std::shared_ptr<ComputeRequestCancellationSource> cancellation_source_owner()
      const noexcept {
    return cancellation_;
  }

  /**
   * @brief Returns shared ownership of the monotonic RT-first gate.
   * @return Gate retained by the group and dirty child callbacks.
   * @throws Nothing.
   */
  std::shared_ptr<DirtySiblingCommitGate> sibling_commit_gate() const noexcept {
    return sibling_commit_gate_;
  }

  /**
   * @brief Requests stable group-origin cancellation for both children.
   * @param reason Trusted private request reason.
   * @return True only when the request source accepts its first group reason.
   * @throws Cancellation callback/synchronization failures after the group
   * reason becomes stable.
   * @note The pending sibling gate is denied before fan-out. An already
   * committed RT gate remains committed and visible. Request-level acceptance
   * does not imply cancellation won either child terminal arbiter.
   */
  bool request_cancellation(ComputeRunCancellationReason reason);

  /**
   * @brief Selects one deterministic aggregate after both children settle.
   * @return Failure before cancellation before success; resource exhaustion
   * before other failure; RT before HP within a class; stable group-origin
   * cancellation that won at least one child before a child-only reason.
   * @throws std::logic_error when either child is not terminal.
   * @throws std::system_error for child/source synchronization failure.
   * @note A late group request after both children succeeded cannot replace
   * aggregate success. The returned snapshot changes neither child terminal
   * outcome nor an already committed RT proxy.
   */
  ComputeRunTerminalOutcome aggregate_terminal_outcome() const;

  /**
   * @brief Releases both group-owned observation leases before unregister.
   * @return Nothing.
   * @throws Nothing.
   * @note Both children must already be terminal and no later group method may
   * request either lease. Registry-owned minimum leases remain alive until
   * complete bundle finalization.
   */
  void release_lifecycle_leases() noexcept;

 private:
  /** @brief Fresh request-group identity. */
  RunGroupId id_;
  /** @brief Shared canonical realtime request lineage. */
  SupersessionIdentity supersession_;
  /** @brief Independent HP Full child Run. */
  ComputeRun hp_run_;
  /** @brief Independent RT Interactive child Run. */
  ComputeRun rt_run_;
  /** @brief Baseline HP observation retained until group destruction. */
  std::optional<ComputeRunLease> hp_lease_;
  /** @brief Baseline RT observation retained until group destruction. */
  std::optional<ComputeRunLease> rt_lease_;
  /** @brief Group-wide cancellation source attached to both children. */
  std::shared_ptr<ComputeRequestCancellationSource> cancellation_;
  /** @brief Pending/RT-committed/denied asymmetric sibling gate. */
  std::shared_ptr<DirtySiblingCommitGate> sibling_commit_gate_;
};

}  // namespace ps::compute
