/**
 * @file progressive_compute.hpp
 * @brief Declares source-private request state for ordered preview/final work.
 */
#pragma once

#include <atomic>
#include <cstdint>

#include "compute/compute_run.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {

/**
 * @brief Private options that opt one realtime request into progressive work.
 *
 * Presence on an internal Kernel or ComputeService request means the request's
 * ordinary QoS describes the RT Interactive preview child while `final_qos`
 * describes the HP Full final child. The value owns no Run, scheduler,
 * cancellation source, observer, Graph, or resource authority.
 *
 * @throws Nothing for value construction and copying.
 * @note This type remains under `src/lib` and is absent from installed Host,
 * CLI, IPC, operation-plugin, policy-plugin, and data-provider contracts.
 */
struct ProgressiveComputeOptions final {
  /** @brief Exact QoS captured by the HP Full final child. */
  ComputeRunQos final_qos;
};

/**
 * @brief Atomic request-local gate ordering preview success against final work.
 *
 * The gate starts Pending. A successful current RT publication may arm it;
 * accepted cancellation denies Pending or Armed from the matching Run
 * terminal arbitration interval before `Cancelled` is published; and the HP
 * Run-owned final-trigger operation may consume only Armed while holding the
 * HP Run terminal arbiter through observation publication. Triggered and
 * Denied are terminal gate states. Cleanup notifications are deliberately
 * outside this ordering. The gate never starts work itself and owns no
 * cancellation, scheduling, currentness, lifecycle, resource, Graph, or
 * commit authority.
 *
 * @throws Nothing from every operation.
 * @note Callers retain this object through shared request ownership. The one
 * atomic state is the shared cancellation/final-trigger linearization
 * authority only for whether HP submission is permitted. Matching RT and HP
 * Run arbiters call `deny()` before cancellation becomes terminal; product
 * currentness and Run terminal arbitration remain authoritative for visible
 * publication.
 */
class ProgressiveFinalGate final {
 public:
  /**
   * @brief Observable state of one progressive final gate.
   * @throws Nothing for value operations.
   */
  enum class State : std::uint8_t {
    /** @brief Preview has neither published successfully nor been cancelled. */
    Pending,
    /** @brief Current preview succeeded and final may be consumed once. */
    Armed,
    /** @brief Final permission was consumed before cancellation won. */
    Triggered,
    /** @brief Cancellation denied final submission before consumption. */
    Denied,
  };

  /**
   * @brief Creates one Pending request-local gate.
   * @throws Nothing.
   */
  ProgressiveFinalGate() noexcept = default;

  /**
   * @brief Arms final submission after successful current preview publication.
   * @return True only when this call changes Pending to Armed.
   * @throws Nothing.
   * @note A cancellation that already changed the gate to Denied wins; repeated
   * calls and terminal states remain unchanged.
   */
  bool arm() noexcept;

  /**
   * @brief Denies final submission when cancellation wins before consumption.
   * @return True only when this call changes Pending or Armed to Denied.
   * @throws Nothing.
   * @note Triggered is never rolled back. Later Run cancellation/currentness
   * still owns entered HP work and stale-commit rejection.
   */
  bool deny() noexcept;

  /**
   * @brief Consumes an armed final immediately before HP child submission.
   * @return True only when this call changes Armed to Triggered.
   * @throws Nothing.
   * @note Pending, Denied, and repeated Triggered observations return false and
   * grant no permission to enter provider or ExecutionService work. Product HP
   * submission calls this only through the Run-owned arbitration operation so
   * trigger observation and later terminal publication remain ordered.
   */
  bool try_trigger() noexcept;

  /**
   * @brief Returns the current atomic gate state.
   * @return Acquire-loaded state.
   * @throws Nothing.
   */
  State state() const noexcept;

 private:
  /** @brief Sole preview/cancellation/final linearization state. */
  std::atomic<State> state_{State::Pending};
};

}  // namespace ps::compute
