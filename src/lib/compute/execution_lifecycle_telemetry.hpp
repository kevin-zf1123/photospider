/**
 * @file execution_lifecycle_telemetry.hpp
 * @brief Declares bounded source-private process execution lifecycle telemetry.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace ps::testing {
class ExecutionLifecycleTelemetryTestAccess;
}  // namespace ps::testing

namespace ps::compute {

/** @brief Exact source-private lifecycle telemetry schema version. */
inline constexpr std::uint32_t kExecutionLifecycleTelemetrySchemaVersion = 1U;

/** @brief Exact number of fixed records retained by one service. */
inline constexpr std::uint32_t kExecutionLifecycleTelemetryCapacity = 65536U;

/** @brief Minimum accepted lifecycle snapshot page size. */
inline constexpr std::uint32_t kExecutionLifecycleTelemetryMinPageSize = 1U;

/** @brief Maximum accepted lifecycle snapshot page size. */
inline constexpr std::uint32_t kExecutionLifecycleTelemetryMaxPageSize = 4096U;

/**
 * @brief Monotonic process execution-domain lifecycle state.
 * @throws Nothing for value construction or comparison.
 */
enum class ExecutionLifecycleServiceState : std::uint16_t {
  /** @brief Graph and Run candidates may still be admitted. */
  Accepting = 1U,
  /** @brief New admission is closed while existing ownership settles. */
  Stopping = 2U,
  /** @brief Registry, resources, routes, bindings, and workers are retired. */
  Stopped = 3U,
};

/**
 * @brief Exact version-1 execution lifecycle event vocabulary.
 * @throws Nothing for value construction or comparison.
 */
enum class ExecutionLifecycleEventKind : std::uint16_t {
  /** @brief A fully constructed service became observable. */
  ServiceStarted = 1U,
  /** @brief One complete Graph lifetime row became open. */
  GraphRegistered = 2U,
  /** @brief One Graph row changed monotonically to Closing. */
  GraphClosing = 3U,
  /** @brief One pre-publication admission candidate began. */
  CandidateBegan = 4U,
  /** @brief One candidate resolved without installing a bundle. */
  CandidateRolledBack = 5U,
  /** @brief One standalone or complete realtime bundle installed. */
  BundleAdmitted = 6U,
  /** @brief Trusted lifecycle ownership requested cancellation. */
  CancellationRequested = 7U,
  /** @brief One child Run published its terminal outcome. */
  RunTerminal = 8U,
  /** @brief One child Run retained only its registry lease. */
  RunQuiescent = 9U,
  /** @brief One Run root reservation and all child grants settled. */
  ResourceSettled = 10U,
  /** @brief One child Run left every registry index. */
  RunUnregistered = 11U,
  /** @brief One empty Closing Graph row was removed. */
  GraphRowRemoved = 12U,
  /** @brief One physical worker joined process shutdown. */
  WorkerJoined = 13U,
  /** @brief One current or displaced policy binding retired. */
  BindingRetired = 14U,
  /** @brief The final process execution-domain stop completed. */
  ServiceStopped = 15U,
};

/**
 * @brief Exact version-1 lifecycle event reason or terminal category.
 * @throws Nothing for value construction or comparison.
 */
enum class ExecutionLifecycleCategory : std::uint16_t {
  /** @brief Event has no cancellation or terminal category. */
  None = 0U,
  /** @brief Cancellation originated from an explicit request. */
  ExplicitRequest = 1U,
  /** @brief Cancellation originated from an immutable deadline. */
  Deadline = 2U,
  /** @brief Cancellation originated from supersession. */
  Superseded = 3U,
  /** @brief Cancellation originated from one Graph close. */
  GraphClose = 4U,
  /** @brief Cancellation originated from process shutdown. */
  ProcessShutdown = 5U,
  /** @brief Run terminal publication succeeded. */
  Succeeded = 6U,
  /** @brief Run terminal publication accepted cancellation. */
  Cancelled = 7U,
  /** @brief Run failed because trusted resource capacity was exhausted. */
  FailureResourceExhausted = 8U,
  /** @brief Run failed for any other exact exception. */
  FailureOther = 9U,
};

/**
 * @brief Exact post-transition version-1 lifecycle counter set.
 *
 * @throws Nothing for aggregate construction, copying, and comparison.
 * @note These values are diagnostics only and mint no lifecycle, resource,
 * scheduling, Graph, callback, or plugin authority.
 */
struct ExecutionLifecycleCounters final {
  /** @brief Registry rows, including Open and Closing rows. */
  std::uint64_t registered_graph_count = 0U;
  /** @brief Registry rows still accepting candidates. */
  std::uint64_t open_graph_count = 0U;
  /** @brief Registry rows monotonically closing. */
  std::uint64_t closing_graph_count = 0U;
  /** @brief Pre-publication candidates retaining Graph leases. */
  std::uint64_t pending_candidate_count = 0U;
  /** @brief Installed standalone child Runs. */
  std::uint64_t admitted_standalone_run_count = 0U;
  /** @brief Installed realtime group bundles. */
  std::uint64_t admitted_run_group_count = 0U;
  /** @brief Installed child Runs belonging to realtime groups. */
  std::uint64_t admitted_child_run_count = 0U;
  /** @brief Terminal child Runs with non-registry leases or physical work. */
  std::uint64_t terminal_not_quiescent_run_count = 0U;
  /** @brief Child Runs completing commit/discard and ownership retirement. */
  std::uint64_t finalizing_run_count = 0U;
  /** @brief Ready-store entries retaining child grants. */
  std::uint64_t ready_entry_count = 0U;
  /** @brief Operation callbacks that have entered and not returned. */
  std::uint64_t entered_callback_count = 0U;
  /** @brief Root ledger reservations not yet physically returned. */
  std::uint64_t live_root_reservation_count = 0U;
  /** @brief Ready or execution child grants not yet returned. */
  std::uint64_t live_child_grant_count = 0U;
  /** @brief Policy callbacks that have entered and not returned. */
  std::uint64_t live_policy_invocation_count = 0U;
  /** @brief Current or displaced bindings retaining their contexts/DSOs. */
  std::uint64_t live_policy_binding_count = 0U;
};

/**
 * @brief Source-private physical counter selector used by trusted owners.
 *
 * @throws Nothing for value construction or comparison.
 * @note Registry-derived counters are never selectable here. This vocabulary
 * cannot mint queue, callback, ledger, plugin, Graph, or Run authority.
 */
enum class ExecutionLifecyclePhysicalCounter : std::uint8_t {
  /** @brief Ready-store entries retaining one ready child grant. */
  ReadyEntry,
  /** @brief Operation callbacks that entered and have not returned. */
  EnteredCallback,
  /** @brief Root reservations not yet physically returned. */
  LiveRootReservation,
  /** @brief Ready or execution child grants not yet returned. */
  LiveChildGrant,
  /** @brief Policy callbacks that entered and have not returned. */
  LivePolicyInvocation,
  /** @brief Published current or displaced policy bindings. */
  LivePolicyBinding,
};

/**
 * @brief One fixed-size version-1 execution lifecycle record.
 *
 * @throws Nothing for construction and copying.
 * @note Zero optional identities mean absent. Records contain no label, path,
 * pointer, exception message, token, callback, lease, or mutable handle.
 */
struct ExecutionLifecycleEvent final {
  /** @brief Exact reader compatibility version. */
  std::uint32_t schema_version = kExecutionLifecycleTelemetrySchemaVersion;
  /** @brief Epoch-local nonzero event order. */
  std::uint64_t sequence = 0U;
  /** @brief Microseconds since this service telemetry origin. */
  std::uint64_t timestamp_us = 0U;
  /** @brief Whether duration conversion clamped to UINT64_MAX. */
  bool timestamp_saturated = false;
  /** @brief Nonzero process-nonreused service identity. */
  std::uint64_t service_instance_id = 0U;
  /** @brief Nonzero process-nonreused telemetry epoch. */
  std::uint64_t telemetry_epoch = 0U;
  /** @brief Optional exact GraphInstanceId scalar. */
  std::uint64_t graph_instance_id = 0U;
  /** @brief Optional exact ComputeRunId scalar. */
  std::uint64_t run_id = 0U;
  /** @brief Optional exact RunGroupId scalar. */
  std::uint64_t run_group_id = 0U;
  /** @brief Optional close/shutdown generation scalar. */
  std::uint64_t generation = 0U;
  /** @brief Exact event transition kind. */
  ExecutionLifecycleEventKind kind =
      ExecutionLifecycleEventKind::ServiceStarted;
  /** @brief Exact cancellation/terminal category or None. */
  ExecutionLifecycleCategory category = ExecutionLifecycleCategory::None;
  /** @brief Complete post-transition diagnostic counters. */
  ExecutionLifecycleCounters counters;
};

/**
 * @brief One atomic-cut, non-destructive copy of retained lifecycle records.
 *
 * @throws std::bad_alloc when caller-side record storage cannot be reserved.
 * @note The page carries no reference into the telemetry owner and may outlive
 * an individual snapshot call, but no telemetry handle may outlive its
 * ExecutionService.
 */
struct ExecutionLifecyclePage final {
  /** @brief Exact reader compatibility version. */
  std::uint32_t schema_version = kExecutionLifecycleTelemetrySchemaVersion;
  /** @brief Fixed production ring capacity. */
  std::uint32_t capacity = kExecutionLifecycleTelemetryCapacity;
  /** @brief Nonzero service identity. */
  std::uint64_t service_instance_id = 0U;
  /** @brief Nonzero telemetry epoch. */
  std::uint64_t telemetry_epoch = 0U;
  /** @brief Service state at the snapshot cut. */
  ExecutionLifecycleServiceState service_state =
      ExecutionLifecycleServiceState::Accepting;
  /** @brief Zero before shutdown or its one nonzero generation. */
  std::uint64_t shutdown_generation = 0U;
  /** @brief Last sequence included by this atomic cut. */
  std::uint64_t snapshot_cut = 0U;
  /** @brief Oldest retained sequence, or zero for an empty ring. */
  std::uint64_t first_retained_sequence = 0U;
  /** @brief Next assignable sequence or exhausted sentinel. */
  std::uint64_t next_sequence = 1U;
  /** @brief Saturating cumulative lost publication/eviction count. */
  std::uint64_t global_dropped_total = 0U;
  /** @brief Sticky proof that losses exceeded UINT64_MAX. */
  bool global_dropped_saturated = false;
  /** @brief Complete counters at the atomic snapshot cut. */
  ExecutionLifecycleCounters counters;
  /** @brief Copied records satisfying the cursor and cut. */
  std::vector<ExecutionLifecycleEvent> records;
  /** @brief Exact missing sequenced events before the retained window. */
  std::uint64_t cursor_gap = 0U;
  /** @brief Last returned sequence, cut, zero, or exhausted sentinel. */
  std::uint64_t next_cursor = 0U;
  /** @brief Whether more retained records remain under this same cut. */
  bool has_more = false;
};

/**
 * @brief Fixed-capacity allocation-free lifecycle publication owner.
 *
 * The owner preallocates all 65,536 record slots. Writers serialize only
 * fixed scalar/record copies. Readers reserve result storage before taking the
 * telemetry mutex, then copy one atomic cut without taking the lifecycle
 * registry fence.
 *
 * @throws std::bad_alloc when constructing the fixed ring.
 * @throws std::overflow_error when service/epoch identity space is exhausted.
 * @throws std::system_error when synchronization primitives fail.
 * @note The sole permitted cross-component lock order is lifecycle fence
 * before this telemetry mutex. Publication/drop never changes lifecycle
 * behavior.
 */
class ExecutionLifecycleTelemetry final {
 public:
  /**
   * @brief Preallocates an empty Accepting telemetry epoch.
   * @throws The class construction errors documented above.
   * @note `ServiceStarted` is published separately only after complete service
   * construction succeeds.
   */
  ExecutionLifecycleTelemetry();

  /**
   * @brief Releases the preallocated ring after service destruction begins.
   * @throws Nothing.
   */
  ~ExecutionLifecycleTelemetry() noexcept;

  /** @brief Prevents duplicating one sequence/drop authority. */
  ExecutionLifecycleTelemetry(const ExecutionLifecycleTelemetry&) = delete;
  /** @brief Prevents assigning one sequence/drop authority. */
  ExecutionLifecycleTelemetry& operator=(const ExecutionLifecycleTelemetry&) =
      delete;
  /** @brief Prevents moving the stable service diagnostics owner. */
  ExecutionLifecycleTelemetry(ExecutionLifecycleTelemetry&&) = delete;
  /** @brief Prevents move-assigning the stable diagnostics owner. */
  ExecutionLifecycleTelemetry& operator=(ExecutionLifecycleTelemetry&&) =
      delete;

  /**
   * @brief Returns this service's nonzero process-lifetime identity.
   * @return Stable service identity scalar.
   * @throws Nothing.
   */
  std::uint64_t service_instance_id() const noexcept {
    return service_instance_id_;
  }

  /**
   * @brief Returns this store's nonzero process-lifetime epoch.
   * @return Stable telemetry epoch scalar.
   * @throws Nothing.
   */
  std::uint64_t telemetry_epoch() const noexcept { return telemetry_epoch_; }

  /**
   * @brief Publishes one ordinary post-transition event.
   * @param kind Any event except ServiceStopped.
   * @param category Exact reason/terminal category.
   * @param graph_instance_id Optional exact Graph identity scalar.
   * @param run_id Optional exact Run identity scalar.
   * @param run_group_id Optional exact group identity scalar.
   * @param generation Optional close/shutdown generation.
   * @param counters Complete post-transition counter view.
   * @return Assigned nonzero sequence, or zero when the event was dropped.
   * @throws std::invalid_argument when kind is ServiceStopped.
   * @throws std::system_error when telemetry locking fails.
   * @note Publication performs no allocation and is harmless after Stopped or
   * ordinary sequence exhaustion.
   */
  std::uint64_t publish(ExecutionLifecycleEventKind kind,
                        ExecutionLifecycleCategory category,
                        std::uint64_t graph_instance_id, std::uint64_t run_id,
                        std::uint64_t run_group_id, std::uint64_t generation,
                        const ExecutionLifecycleCounters& counters);

  /**
   * @brief Changes the service view monotonically to Stopping.
   * @param shutdown_generation Fresh nonzero process-shutdown generation.
   * @param counters Complete post-transition counter view.
   * @return Nothing.
   * @throws std::invalid_argument for zero or conflicting generation.
   * @throws std::logic_error after Stopped.
   * @throws std::system_error when telemetry locking fails.
   * @note Repeating the same generation while already Stopping is idempotent.
   */
  void mark_stopping(std::uint64_t shutdown_generation,
                     const ExecutionLifecycleCounters& counters);

  /**
   * @brief Publishes the one final ServiceStopped event and freezes counters.
   * @param shutdown_generation Exact generation selected by mark_stopping().
   * @param counters Complete final-zero lifecycle/resource counter view.
   * @return Assigned final sequence; repeated calls return the same sequence.
   * @throws std::invalid_argument for a zero/mismatched generation.
   * @throws std::logic_error when called directly from Accepting.
   * @throws std::system_error when telemetry locking fails.
   * @note Validation, final append, Stopped publication, and idempotent replay
   * share one telemetry critical section. Concurrent callers therefore return
   * the same final sequence without recording a spurious post-stop drop. The
   * current next sequence, including reserved UINT64_MAX-1, is used;
   * next_sequence becomes UINT64_MAX afterward.
   */
  std::uint64_t publish_service_stopped(
      std::uint64_t shutdown_generation,
      const ExecutionLifecycleCounters& counters);

  /**
   * @brief Copies one non-destructive page from a single atomic snapshot cut.
   * @param after_cursor Last caller-observed sequence, or zero for oldest.
   * @param limit Number of records requested in the exact range 1..4096.
   * @return Self-contained scalar and record page.
   * @throws std::invalid_argument for invalid limits or cursor semantics.
   * @throws std::bad_alloc before locking when result storage cannot reserve.
   * @throws std::system_error when telemetry locking fails.
   * @note Concurrent publications after the captured cut are excluded.
   */
  ExecutionLifecyclePage snapshot(std::uint64_t after_cursor,
                                  std::uint32_t limit) const;

  /**
   * @brief Adds one exact live physical owner to the current counter view.
   * @param counter Physical owner kind whose post-transition count advances.
   * @return Nothing.
   * @throws Nothing; locking failure or counter exhaustion terminates because
   * either condition prevents trustworthy lifecycle proof.
   * @note The operation allocates nothing and takes only the telemetry mutex.
   * Callers update the counter at the physical ownership transition and retain
   * no telemetry handle beyond the ExecutionService lifetime.
   */
  void increment_physical_counter(
      ExecutionLifecyclePhysicalCounter counter) noexcept;

  /**
   * @brief Removes one exact live physical owner from the current counter view.
   * @param counter Physical owner kind whose post-transition count decreases.
   * @return Nothing.
   * @throws Nothing; locking failure or underflow terminates as a trusted
   * lifecycle accounting invariant breach.
   * @note The operation allocates nothing and takes only the telemetry mutex.
   */
  void decrement_physical_counter(
      ExecutionLifecyclePhysicalCounter counter) noexcept;

  /**
   * @brief Tests whether every source-private physical counter is exactly zero.
   * @return True only after ready entries, callbacks, resources, grants,
   * invocations, and bindings have all retired.
   * @throws std::system_error when telemetry locking fails.
   * @note This is a shutdown proof predicate and grants no ownership.
   */
  bool physical_counters_zero() const;

 private:
  friend class ::ps::testing::ExecutionLifecycleTelemetryTestAccess;

  /**
   * @brief Publishes one event using an explicit elapsed duration for tests.
   * @param kind Ordinary or final event kind selected by the caller.
   * @param category Exact category.
   * @param graph_instance_id Optional Graph identity.
   * @param run_id Optional Run identity.
   * @param run_group_id Optional group identity.
   * @param generation Optional close/shutdown generation.
   * @param counters Complete post-transition counters.
   * @param elapsed Explicit steady duration since origin.
   * @param final_stop Whether to consume the reserved final sequence.
   * @return Assigned sequence, or zero for a dropped ordinary event.
   * @throws std::system_error when telemetry locking fails.
   * @note Caller performs semantic validation before invoking this helper.
   */
  std::uint64_t publish_at(ExecutionLifecycleEventKind kind,
                           ExecutionLifecycleCategory category,
                           std::uint64_t graph_instance_id,
                           std::uint64_t run_id, std::uint64_t run_group_id,
                           std::uint64_t generation,
                           const ExecutionLifecycleCounters& counters,
                           std::chrono::steady_clock::duration elapsed,
                           bool final_stop);

  /**
   * @brief Increments the cumulative loss view with sticky overflow evidence.
   * @return Nothing.
   * @throws Nothing.
   * @note Caller holds mutex_.
   */
  void record_drop_locked() noexcept;

  /**
   * @brief Stores one already sequenced event, evicting the oldest if full.
   * @param event Complete fixed record.
   * @return Nothing.
   * @throws Nothing.
   * @note Caller holds mutex_; eviction counts as one global drop.
   */
  void append_locked(const ExecutionLifecycleEvent& event) noexcept;

  /**
   * @brief Selects one mutable physical field in the current counter set.
   * @param counter Exact source-private physical counter kind.
   * @return Mutable counter field.
   * @throws Nothing; invalid trusted enum values terminate.
   * @note Caller holds mutex_.
   */
  std::uint64_t& physical_counter_locked(
      ExecutionLifecyclePhysicalCounter counter) noexcept;

  /**
   * @brief Overlays current physical counters on registry-derived counters.
   * @param counters Registry-derived lifecycle view supplied by the publisher.
   * @return Complete current lifecycle and physical counter set.
   * @throws Nothing.
   * @note Caller holds mutex_; caller-supplied physical fields are ignored.
   */
  ExecutionLifecycleCounters complete_counters_locked(
      const ExecutionLifecycleCounters& counters) const noexcept;

  /** @brief Serializes fixed ring/state publication and snapshot copies. */
  mutable std::mutex mutex_;
  /** @brief Heap-owned fixed record slots allocated exactly once. */
  std::unique_ptr<ExecutionLifecycleEvent[]> ring_;
  /** @brief Stable steady-clock origin for production timestamps. */
  const std::chrono::steady_clock::time_point origin_;
  /** @brief Nonzero process-nonreused service identity. */
  const std::uint64_t service_instance_id_;
  /** @brief Nonzero process-nonreused telemetry epoch. */
  const std::uint64_t telemetry_epoch_;
  /** @brief Current monotonic service state. */
  ExecutionLifecycleServiceState service_state_ =
      ExecutionLifecycleServiceState::Accepting;
  /** @brief One shutdown generation, or zero before Stopping. */
  std::uint64_t shutdown_generation_ = 0U;
  /** @brief Oldest physical slot in the circular ring. */
  std::uint32_t head_ = 0U;
  /** @brief Number of live retained slots. */
  std::uint32_t retained_count_ = 0U;
  /** @brief Next ordinary/final assignable sequence. */
  std::uint64_t next_sequence_ = 1U;
  /** @brief Final ServiceStopped sequence, or zero before publication. */
  std::uint64_t final_stop_sequence_ = 0U;
  /** @brief Saturating cumulative eviction/drop count. */
  std::uint64_t global_dropped_total_ = 0U;
  /** @brief Sticky evidence that actual drops exceeded UINT64_MAX. */
  bool global_dropped_saturated_ = false;
  /** @brief Last published post-transition counters. */
  ExecutionLifecycleCounters counters_;
};

}  // namespace ps::compute
