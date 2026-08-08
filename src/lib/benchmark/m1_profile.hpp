/**
 * @file m1_profile.hpp
 * @brief Declares the deterministic M1 timeline, fairness, and observation
 * contract.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "benchmark/b1_environment.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/i1_evidence.hpp"     // NOLINT(build/include_subdir)
#include "benchmark/i1_profile.hpp"      // NOLINT(build/include_subdir)
#include "compute/compute_run.hpp"       // NOLINT(build/include_subdir)

namespace ps::benchmark {

/** @brief Exact closed mixed workload identifier frozen by ADR 0010. */
inline constexpr char kM1WorkloadId[] = "M1-shared-v1";

/** @brief Exact pre-measurement span from cold start to `B^M1`. */
inline constexpr std::chrono::nanoseconds kM1ColdToMeasurement{6000000000};

/** @brief Exact pre-measurement span from warmup start to `B^M1`. */
inline constexpr std::chrono::nanoseconds kM1WarmupToMeasurement{5000000000};

/** @brief Exact measured interval length. */
inline constexpr std::chrono::nanoseconds kM1MeasurementDuration{30000000000};

/** @brief Exact number of non-overlapping measured one-second windows. */
inline constexpr std::size_t kM1MeasuredWindowCount = 30U;

/** @brief Exact number of measured I1 episode origins. */
inline constexpr std::size_t kM1MeasuredI1OriginCount = 40U;

/** @brief Exact number of cold I1 origins before M1 warmup. */
inline constexpr std::size_t kM1ColdI1OriginCount = 1U;

/** @brief Exact number of M1 warmup I1 origins. */
inline constexpr std::size_t kM1WarmupI1OriginCount = 7U;

/** @brief Exact total I1 occurrences in one M1 replicate. */
// NOLINTBEGIN(whitespace/indent_namespace)
inline constexpr std::size_t kM1TotalI1OriginCount =
    kM1ColdI1OriginCount + kM1WarmupI1OriginCount + kM1MeasuredI1OriginCount;
// NOLINTEND

/** @brief Exact measured I1 admission-attempt count, `40 * 12`. */
// NOLINTBEGIN(whitespace/indent_namespace)
inline constexpr std::size_t kM1MeasuredI1AttemptCount =
    kM1MeasuredI1OriginCount * kI1EditCount;
// NOLINTEND

/** @brief Frozen nearest-rank p05 paired-throughput progress floor. */
inline constexpr double kM1ThroughputProgressP05Floor = 0.20;

/** @brief Frozen nearest-rank p05 Graph-peer Jain floor. */
inline constexpr double kM1GraphJainP05Floor = 0.95;

/** @brief Frozen maximum Interactive starts before Throughput. */
inline constexpr std::size_t kM1InteractiveBurstLimit = 3U;

/**
 * @brief Checked exact phase boundaries for one M1 replicate.
 * @throws Nothing for value construction and comparison.
 * @note All values remain in the same process steady-clock domain.
 */
struct M1Timeline final {
  /** @brief Exact cold boundary `C^M1=B^M1-6s`. */
  std::chrono::steady_clock::time_point cold_start;

  /** @brief Exact warmup boundary `W^M1=B^M1-5s`. */
  std::chrono::steady_clock::time_point warmup_start;

  /** @brief Exact measured boundary `B^M1`. */
  std::chrono::steady_clock::time_point measurement_start;

  /** @brief Exact terminal cutoff `U^M1=B^M1+30s`. */
  std::chrono::steady_clock::time_point measurement_end;
};

/**
 * @brief One row-local total-order coordinate for a boundary or lifecycle
 * event.
 * @throws Nothing for value construction and comparison.
 */
struct M1EventCoordinate final {
  /** @brief Monotonic timestamp in the single M1 process clock domain. */
  std::chrono::steady_clock::time_point timestamp;
  /** @brief Unique nonzero row-local tie-breaking sequence. */
  std::uint64_t event_sequence = 0U;

  /**
   * @brief Compares both coordinate components exactly.
   * @param other Candidate coordinate.
   * @return True only for equal timestamp and sequence.
   * @throws Nothing.
   */
  bool operator==(const M1EventCoordinate& other) const noexcept;

  /**
   * @brief Orders first by timestamp and then by row-local sequence.
   * @param other Candidate coordinate in the same row domain.
   * @return True when this coordinate precedes `other`.
   * @throws Nothing.
   */
  bool operator<(const M1EventCoordinate& other) const noexcept;
};

/**
 * @brief Exact four boundary coordinates retained by one M1 replicate.
 * @throws Nothing for value construction and copying.
 */
struct M1BoundaryEvidence final {
  /** @brief Cold-start boundary `(C^M1,c^M1)`. */
  M1EventCoordinate cold_start;
  /** @brief Warmup-start boundary `(W^M1,w^M1)`. */
  M1EventCoordinate warmup_start;
  /** @brief Atomic measurement boundary `(B^M1,b^M1)`. */
  M1EventCoordinate measurement_start;
  /** @brief Terminal offer cutoff `(U^M1,u^M1)`. */
  M1EventCoordinate measurement_end;
};

/**
 * @brief Closed queue state recorded for one incomplete carryover occurrence.
 * @throws Nothing for value construction and comparison.
 */
enum class M1CarryoverState : std::uint8_t {
  /** @brief Offered but not yet admitted to product ownership. */
  OfferedWaiting,
  /** @brief Host accepted the occurrence but no ready entry exists yet. */
  Accepted,
  /** @brief At least one occurrence-owned entry remains queued. */
  Queued,
  /** @brief Occurrence-owned service is physically running. */
  Running,
};

/**
 * @brief One cold, warmup, or measured I1 occurrence retained by M1.
 * @throws std::bad_alloc when inner diagnostic ownership is copied.
 * @note `service`, output, and independent verdicts are produced from the
 * Issue #93 collector/evaluator boundary; M1 does not synthesize task starts.
 */
struct M1InteractiveOccurrenceEvidence final {
  /** @brief Immutable cold/warmup/measured attribution. */
  B1JobPhase phase = B1JobPhase::Cold;
  /** @brief Zero-based ordinal within the immutable phase. */
  std::size_t phase_ordinal = 0U;
  /** @brief Exact nominal origin event coordinate. */
  M1EventCoordinate origin;
  /** @brief Fixed occurrence-local `origin+683,333,337 ns` cut. */
  std::chrono::steady_clock::time_point settlement_endpoint;
  /** @brief Actual old-occurrence settlement/quiescence event, when proved. */
  std::optional<M1EventCoordinate> settlement_observed;
  /** @brief Final accepted-to-visible latency, when structurally valid. */
  std::optional<std::chrono::nanoseconds> final_latency;
  /** @brief Exact Issue #93 started-service aggregate for this occurrence. */
  I1ServiceEvidence service;
  /** @brief Independent Issue #93 latency evidence verdict. */
  I1Verdict latency_verdict = I1Verdict::Invalid;
  /** @brief Independent Issue #93 waste evidence verdict. */
  I1Verdict waste_verdict = I1Verdict::Invalid;
  /** @brief Independent Issue #93 memory evidence verdict. */
  I1Verdict memory_verdict = I1Verdict::Invalid;
  /** @brief Independent Issue #93 output evidence verdict. */
  I1Verdict output_verdict = I1Verdict::Invalid;
  /** @brief True only when later events never rewrote phase identity. */
  bool phase_identity_immutable = false;
  /** @brief Final-warmup-only current state in the B boundary snapshot. */
  bool publication_current_at_measurement = false;
  /** @brief Whether settlement remained pending at the B boundary. */
  bool settlement_pending_at_measurement = false;
};

/**
 * @brief One immutable B1 offer and its predecessor/terminal evidence.
 * @throws std::bad_alloc when optional identity/string ownership is copied.
 */
struct M1BatchOfferEvidence final {
  /** @brief Exact six-component cold/warmup/measured occurrence. */
  B1JobInstance job;
  /** @brief Zero-based contiguous local offer ordinal for its Graph. */
  std::uint64_t producer_offer_ordinal = 0U;
  /** @brief Retry attempt; frozen fault-free protocol requires zero. */
  std::uint64_t attempt = 0U;
  /** @brief Exact row-local offer coordinate. */
  M1EventCoordinate offered;
  /** @brief Prior same-Graph occurrence, absent only for first producer offer.
   */
  std::optional<B1JobInstance> predecessor;
  /** @brief Matching predecessor terminal coordinate when timing requires it.
   */
  std::optional<M1EventCoordinate> predecessor_terminal;
  /** @brief Unique Run/receipt/golden endpoint when complete. */
  std::optional<M1EventCoordinate> endpoint;
  /** @brief True after occurrence-owner resource settlement is proved. */
  bool owner_settled = false;
  /** @brief Cold/warmup output removal proof; measured output may remain. */
  bool output_removed = false;
  /** @brief True only when later events never rewrote phase identity. */
  bool phase_identity_immutable = false;
  /** @brief True only when queue position survived the B transition. */
  bool fifo_position_preserved = false;
  /** @brief True only when existing reservation/grant authority survived. */
  bool resource_authority_preserved = false;
};

/**
 * @brief One exact incomplete occurrence copied by the atomic B snapshot.
 * @throws std::bad_alloc when keys are copied.
 */
struct M1CarryoverEntry final {
  /** @brief Canonical occurrence key derived from immutable identity. */
  std::string occurrence_key;
  /** @brief Immutable warmup phase retained after the boundary. */
  B1JobPhase phase = B1JobPhase::Warmup;
  /** @brief Physical offered/accepted/queued/running state at the cut. */
  M1CarryoverState state = M1CarryoverState::OfferedWaiting;
  /** @brief Canonical same-Graph predecessor key, or empty for I1/no
   * predecessor. */
  std::string queue_predecessor_key;
  /** @brief True only when reservation/grant authority is unchanged. */
  bool resource_authority_preserved = false;
  /** @brief Final-warmup-I1-only publication-current marker. */
  bool publication_current = false;
  /** @brief False for every occurrence included by the incomplete snapshot. */
  bool owner_settled = false;
};

/**
 * @brief Frozen success-only first measured edit supersession evidence.
 * @throws Nothing for ordinary movement of optional scalar coordinates.
 */
struct M1FirstMeasuredAdmissionEvidence final {
  /** @brief Exact first edit identity; must be zero. */
  std::size_t edit_index = 0U;
  /** @brief Exact nominal origin `B^M1`. */
  std::chrono::steady_clock::time_point nominal_time;
  /** @brief Whether the pre-call admission boundary was reached. */
  bool attempted = false;
  /** @brief Sole pre-call monotonic sample `A_0`. */
  std::chrono::steady_clock::time_point admission_sample;
  /** @brief Nonzero sequence reserved before the Host invocation. */
  std::optional<std::uint64_t> reserved_event_sequence;
  /** @brief Whether Host admission succeeded with a valid future. */
  bool host_succeeded = false;
  /** @brief Success-only product-bound accepted coordinate. */
  std::optional<I1AcceptedCoordinate> accepted_coordinate;
  /** @brief True when final warmup publication was current before acceptance.
   */
  bool warmup_publication_current_before_acceptance = false;
  /** @brief True only when ordinary latest-wins superseded at acceptance. */
  bool superseded_exactly_at_acceptance = false;
  /** @brief Must remain false; phase transition cannot cancel the old Run. */
  bool boundary_only_cancellation = false;
  /** @brief Unchanged old-generation `B^M1+183,333,337 ns` cut. */
  std::chrono::steady_clock::time_point old_generation_settlement_endpoint;
};

/**
 * @brief Complete raw deterministic M1 phase/offer/carryover protocol.
 * @throws std::bad_alloc when occurrence, offer, or snapshot storage copies.
 */
struct M1ProtocolEvidenceInput final {
  /** @brief Fresh-process ordinal in `[1,3]`. */
  std::uint64_t replicate_ordinal = 0U;
  /** @brief Exact four boundary coordinates. */
  M1BoundaryEvidence boundaries;
  /** @brief Exact one cold, seven warmup, and forty measured I1 occurrences. */
  std::vector<M1InteractiveOccurrenceEvidence> interactive_occurrences;
  /** @brief Every cold/warmup/measured B1 offer in row-local event order. */
  std::vector<M1BatchOfferEvidence> batch_offers;
  /** @brief Complete atomic snapshot of all incomplete warmup occurrences. */
  std::vector<M1CarryoverEntry> carryover;
  /** @brief Sole measured edit-zero acceptance/current-hold transition. */
  M1FirstMeasuredAdmissionEvidence first_measured_admission;
  /** @brief True only when all work used one ExecutionService/process domain.
   */
  bool shared_execution_domain = false;
  /** @brief True only when boundary transition performed no
   * drain/pause/restart. */
  bool boundary_was_zero_duration = false;
  /** @brief True only when raw history survived accumulator reset unchanged. */
  bool raw_history_preserved = false;
  /** @brief True only when warmup producers were closed before the snapshot. */
  bool warmup_sources_closed = false;
  /** @brief True only when measured logical counters reset atomically at B. */
  bool measured_counters_reset = false;
  /** @brief True only when final shutdown settled every phase to zero. */
  bool final_settlement_proved = false;
};

/**
 * @brief Fail-closed deterministic M1 protocol result.
 * @throws std::bad_alloc when diagnostics allocate.
 */
struct M1ProtocolSummary final {
  /** @brief Complete stable structural invalidation reasons. */
  std::vector<std::string> validity_reasons;
  /** @brief Pass only for the exact frozen protocol, otherwise Invalid. */
  I1Verdict verdict = I1Verdict::Invalid;
};

/**
 * @brief One measured M1 Throughput progress window.
 * @throws Nothing for value construction and copying.
 */
struct M1ThroughputProgressSample final {
  /** @brief Exact zero-based measured-window ordinal in `[0,29]`. */
  std::size_t window_ordinal = 0U;

  /** @brief Verified successful B1 pixel-site operations in this window. */
  std::uint64_t successful_site_operations = 0U;

  /** @brief Exact positive measured window duration. */
  std::chrono::nanoseconds duration{0};
};

/**
 * @brief Exact isolated-B1 denominator claim retained by the M1 inner row.
 * @throws Nothing for value construction and copying.
 * @note Canonical corpus validation independently resolves the named paired
 * row and recomputes this tuple from its thirty raw job outcomes. This value
 * alone is not pairing authority.
 */
struct M1PairedB1RateEvidence final {
  /** @brief Successful isolated pixel-site operations over the full interval.
   */
  std::uint64_t successful_site_operations = 0U;

  /** @brief Exact positive isolated measurement interval duration. */
  std::chrono::nanoseconds duration{0};
};

/**
 * @brief Completed charged service retained for one Graph-peer window.
 * @throws Nothing for value construction and copying.
 */
struct M1GraphServiceWindow final {
  /** @brief Exact zero-based measured-window ordinal in `[0,29]`. */
  std::size_t window_ordinal = 0U;

  /** @brief True only when both producers retain demand for the full window. */
  bool both_graphs_continuously_demanding = false;

  /** @brief Successful completed charged service for Graph A. */
  std::uint64_t graph_a_completed_service = 0U;

  /** @brief Successful completed charged service for Graph B. */
  std::uint64_t graph_b_completed_service = 0U;
};

/**
 * @brief One actual class start and its authoritative applicability evidence.
 * @throws Nothing for value construction and copying.
 */
struct M1ClassStartSample final {
  /** @brief Shared nonzero causal sequence of the committed product start. */
  std::uint64_t causal_sequence = 0U;

  /** @brief Actual class copied from the immutable Run descriptor. */
  compute::ComputeRunQosClass service_class =
      compute::ComputeRunQosClass::Throughput;

  /** @brief Interactive evidence-startability at the product commit cut. */
  bool interactive_candidate_startable = false;

  /** @brief Throughput evidence-startability at the product commit cut. */
  bool throughput_candidate_startable = false;

  /** @brief True only after the selected execution child grant committed. */
  bool execution_grant_committed = false;
};

/**
 * @brief Complete measured Interactive headroom admission classification.
 * @throws Nothing for value construction and copying.
 */
struct M1HeadroomAdmissionEvidence final {
  /** @brief Exact number of measured edit attempts observed. */
  std::size_t attempted_edits = 0U;

  /** @brief Number of attempts assigned one complete admission outcome. */
  std::size_t classified_outcomes = 0U;

  /** @brief Failures attributed to Throughput consuming declared headroom. */
  std::size_t throughput_headroom_failures = 0U;
};

/**
 * @brief One raw measured I1 admission outcome used for headroom evidence.
 * @throws std::bad_alloc when an optional Host status is copied.
 */
struct M1HeadroomAdmissionOutcome final {
  /** @brief Exact measured I1 origin ordinal in `[0,39]`. */
  std::size_t origin_ordinal = 0U;

  /** @brief Exact zero-based edit index in `[0,11]`. */
  std::size_t edit_index = 0U;

  /** @brief Whether the edit reached its Host admission boundary. */
  bool admission_attempted = false;

  /** @brief Complete Host return status, absent if no return was observed. */
  std::optional<OperationStatus> host_status;

  /** @brief Whether the failure was classified as a headroom violation. */
  bool throughput_headroom_failure = false;
};

/**
 * @brief Complete raw inputs for deterministic M1 fairness aggregation.
 * @throws std::bad_alloc when vector storage is copied.
 * @note The latency verdict is supplied by the accepted I1 evidence authority.
 */
struct M1FairnessEvidenceInput final {
  /** @brief Exact measured one-second progress samples in window order. */
  std::vector<M1ThroughputProgressSample> progress_windows;

  /** @brief Exact paired isolated-B1 numerator and measurement interval. */
  std::optional<M1PairedB1RateEvidence> paired_isolated_b1;

  /** @brief All retained Graph-peer service windows in time order. */
  std::vector<M1GraphServiceWindow> graph_service_windows;

  /** @brief Actual class starts with continuous-startability classification. */
  std::vector<M1ClassStartSample> class_starts;

  /** @brief Complete measured Interactive admission classification. */
  M1HeadroomAdmissionEvidence headroom_admissions;

  /** @brief All 40-by-12 measured Interactive raw admission outcomes. */
  std::vector<M1HeadroomAdmissionOutcome> headroom_outcomes;

  /** @brief Frozen I1 mixed-latency verdict computed by its own authority. */
  I1Verdict interactive_latency_verdict = I1Verdict::Invalid;

  /** @brief True when the bounded mixed observer dropped any required event. */
  bool observation_overflowed = false;

  /** @brief True when the shared nonzero causal sequence was exhausted. */
  bool observation_sequence_exhausted = false;

  /** @brief True when a request tag disagreed with actual descriptor QoS. */
  bool observation_qos_mismatch = false;

  /** @brief True when the final observation cut was not callback-quiescent. */
  bool observation_publication_unstable = false;
};

/**
 * @brief M1 aggregates and independent fail-closed fairness verdicts.
 * @throws std::bad_alloc when diagnostic ownership or sorting allocates.
 */
struct M1FairnessSummary final {
  /** @brief Nearest-rank p05 paired throughput ratio, absent if invalid. */
  std::optional<double> throughput_progress_p05;

  /** @brief Nearest-rank p05 eligible Graph-peer Jain index, absent if invalid.
   */
  std::optional<double> graph_jain_p05;

  /** @brief Maximum closed applicable Interactive burst observed. */
  std::size_t maximum_interactive_burst = 0U;

  /** @brief Human-readable structural invalidation reasons. */
  std::vector<std::string> validity_reasons;

  /** @brief Independent paired B1 progress verdict. */
  I1Verdict throughput_progress_verdict = I1Verdict::Invalid;

  /** @brief Independent completed-service Graph-peer verdict. */
  I1Verdict graph_jain_verdict = I1Verdict::Invalid;

  /** @brief Independent Interactive/Throughput start-order verdict. */
  I1Verdict class_start_verdict = I1Verdict::Invalid;

  /** @brief Independent protected Interactive admission verdict. */
  I1Verdict interactive_headroom_verdict = I1Verdict::Invalid;

  /** @brief Unmodified frozen mixed-I1 latency verdict. */
  I1Verdict interactive_latency_verdict = I1Verdict::Invalid;

  /** @brief Conjunction of all five non-substitutable fairness guards. */
  I1Verdict composite_fairness_verdict = I1Verdict::Invalid;
};

/**
 * @brief Result of exact same-ordinal M1 environment delegation.
 * @throws Nothing for value construction and comparison.
 */
struct M1EnvironmentPairCompatibility final {
  /** @brief Base-only M1/isolated-I1 compatibility result. */
  bool isolated_i1_base_only = false;

  /** @brief Full eligible M1/isolated-B1-cap-eight compatibility result. */
  bool isolated_b1_cap_eight = false;

  /**
   * @brief Tests whether both independently delegated relations succeeded.
   * @return True only when both pair relations are compatible.
   * @throws Nothing.
   */
  bool compatible() const noexcept {
    return isolated_i1_base_only && isolated_b1_cap_eight;
  }
};

/**
 * @brief Stable request role attached only to M1 observation sinks.
 * @throws Nothing for value construction and comparison.
 */
enum class M1ObservedRequestTag : std::uint8_t {
  /** @brief One Interactive I1 request. */
  Interactive,

  /** @brief One Throughput request from B1 Graph A. */
  ThroughputGraphA,

  /** @brief One Throughput request from B1 Graph B. */
  ThroughputGraphB,
};

/**
 * @brief Closed scalar event vocabulary retained by the mixed collector.
 * @throws Nothing for value construction and comparison.
 */
enum class M1ObservationKind : std::uint8_t {
  /** @brief One physically committed task service start. */
  ServiceStart,

  /** @brief One entered task reached its logical terminal category. */
  TaskTerminal,

  /** @brief One Run published its exactly-once terminal category. */
  RunTerminal,

  /** @brief One Run returned its exact root resources. */
  RunResourceSettled,
};

/**
 * @brief One immutable scalar mixed-run observation.
 * @throws Nothing for value construction and copying.
 */
struct M1FairnessObservation final {
  /** @brief Product boundary represented by this record. */
  M1ObservationKind kind = M1ObservationKind::ServiceStart;

  /** @brief Request-local observer role fixed before Host admission. */
  M1ObservedRequestTag request_tag = M1ObservedRequestTag::Interactive;

  /** @brief Actual immutable QoS class copied from the Run descriptor. */
  compute::ComputeRunQosClass service_class =
      compute::ComputeRunQosClass::Throughput;

  /** @brief True only when tag and actual QoS class agree. */
  bool qos_matches_tag = false;

  /** @brief Shared observer-local nonzero causal sequence. */
  std::uint64_t causal_sequence = 0U;

  /** @brief Steady-clock sample paired with the causal sequence. */
  std::chrono::steady_clock::time_point observed_at;

  /** @brief Exact nonzero product Run identity. */
  std::uint64_t run_id = 0U;

  /** @brief Exact Run-local task identity for task records, otherwise zero. */
  std::uint64_t local_task_id = 0U;

  /** @brief Exact policy service charge for starts, otherwise zero. */
  std::uint64_t service_charge = 0U;

  /** @brief Task terminal category for task-terminal records. */
  compute::ComputeRunTaskTerminalKind task_terminal_kind =
      compute::ComputeRunTaskTerminalKind::Succeeded;

  /** @brief Run terminal category for Run-terminal records. */
  compute::ComputeRunTerminalKind run_terminal_kind =
      compute::ComputeRunTerminalKind::Succeeded;

  /** @brief Interactive evidence-startability for ServiceStart only. */
  bool interactive_candidate_startable = false;

  /** @brief Throughput evidence-startability for ServiceStart only. */
  bool throughput_candidate_startable = false;

  /** @brief Selected execution-grant commitment for ServiceStart only. */
  bool execution_grant_committed = false;
};

/**
 * @brief Copy of every completely published bounded M1 observation.
 * @throws std::bad_alloc when event storage allocates.
 */
struct M1FairnessObservationSnapshot final {
  /** @brief Records sorted by their shared nonzero causal sequence. */
  std::vector<M1FairnessObservation> events;

  /** @brief True when an event could not claim a fixed-capacity slot. */
  bool overflowed = false;

  /** @brief True after the shared nonzero causal sequence was exhausted. */
  bool sequence_exhausted = false;

  /** @brief True when at least one tag disagreed with actual descriptor QoS. */
  bool qos_mismatch = false;

  /** @brief Completed callback-entry count sampled at the snapshot cut. */
  std::uint64_t callback_entry_frontier = 0U;

  /** @brief Completed callback-exit count sampled at the snapshot cut. */
  std::uint64_t callback_completion_frontier = 0U;

  /** @brief Number of fixed slots claimed before the snapshot cut. */
  std::size_t claimed_slot_frontier = 0U;

  /** @brief Contiguous prefix of claimed slots published before the cut. */
  std::size_t published_slot_frontier = 0U;

  /** @brief True after either callback frontier can no longer advance. */
  bool callback_frontier_exhausted = false;

  /**
   * @brief True only for one quiescent, unchanged, contiguous publication cut.
   * @note A callback entering, completing, claiming, or publishing while the
   * snapshot is copied makes this false; callers must fail closed rather than
   * compare only the retained vector length.
   */
  bool stable_publication_cut = false;
};

/**
 * @brief Source-private deterministic hook after a callback claims its slot.
 *
 * Production collectors never install this hook. Tests may pause publication
 * after claim to prove that a snapshot cannot mistake an in-flight callback
 * for an unchanged boundary.
 *
 * @throws Nothing for destruction or callback dispatch.
 * @note Implementations must not throw. A blocking implementation is allowed
 * only in a deterministic test and would violate the production callback
 * latency contract if installed by a benchmark runner.
 */
class M1ObservationPublicationHook {
 public:
  /**
   * @brief Destroys the test hook through its abstract base.
   * @throws Nothing.
   */
  virtual ~M1ObservationPublicationHook() noexcept = default;

  /**
   * @brief Runs after a unique slot is claimed and before it is published.
   * @return Nothing.
   * @throws Nothing.
   */
  virtual void after_slot_claim() noexcept = 0;
};

/**
 * @brief Fixed-capacity source-private observer shared by one M1 replicate.
 *
 * Construction allocates every event slot. Per-request sinks share one
 * sequence and publish only scalar records without locks, blocking,
 * allocation, compute re-entry, or product authority.
 *
 * @throws std::invalid_argument when capacity is zero.
 * @throws std::bad_alloc when shared state or fixed slots cannot allocate.
 * @note Sinks may outlive this facade through shared state but never outlive
 * the product callbacks that retain them.
 */
class M1FairnessObservationCollector final {
 public:
  /**
   * @brief Allocates one complete bounded observation store.
   * @param capacity Maximum number of retained scalar events.
   * @throws std::invalid_argument when capacity is zero.
   * @throws std::bad_alloc when shared state or fixed slots cannot allocate.
   */
  explicit M1FairnessObservationCollector(std::size_t capacity = 4096U);

  /**
   * @brief Allocates a bounded store with an injected first sequence.
   * @param capacity Maximum number of retained scalar events.
   * @param first_sequence Nonzero first causal sequence to reserve.
   * @throws std::invalid_argument when capacity or first_sequence is zero.
   * @throws std::bad_alloc when shared state or fixed slots cannot allocate.
   * @note This source-private overload exists so deterministic tests can prove
   * terminal sequence handling without attempting `UINT64_MAX` callbacks.
   * Production callers use the one-argument constructor and start at one.
   */
  M1FairnessObservationCollector(std::size_t capacity,
                                 std::uint64_t first_sequence);

  /**
   * @brief Allocates a bounded store with a deterministic publication hook.
   * @param capacity Maximum number of retained scalar events.
   * @param first_sequence Nonzero first causal sequence to reserve.
   * @param publication_hook Non-null source-private post-claim test hook.
   * @throws std::invalid_argument when an argument is zero or the hook is null.
   * @throws std::bad_alloc when shared state or fixed slots cannot allocate.
   * @note This overload is solely for controlled concurrency tests. Production
   * callers must use a hook-free constructor so callbacks remain nonblocking.
   */
  M1FairnessObservationCollector(
      std::size_t capacity, std::uint64_t first_sequence,
      std::shared_ptr<M1ObservationPublicationHook> publication_hook);

  /**
   * @brief Creates one request-local tagged sink over the shared store.
   * @param tag Immutable Interactive or Throughput Graph role.
   * @return Shared observation-only sink suitable for I1Host or B1Host.
   * @throws std::invalid_argument for an invalid enum representation.
   * @throws std::bad_alloc when sink ownership allocates.
   */
  std::shared_ptr<compute::ComputeRunObservationSink> make_sink(
      M1ObservedRequestTag tag) const;

  /**
   * @brief Copies every completely published record in causal order.
   * @return Bounded events and explicit overflow/mismatch state.
   * @throws std::bad_alloc when result storage allocates.
   * @note Snapshotting never waits for an incompletely published slot. Take
   * the conformance snapshot only after every retained request has settled.
   */
  M1FairnessObservationSnapshot snapshot() const;

 private:
  /** @brief Opaque shared fixed-capacity implementation. */
  class Impl;

  /** @brief Store shared by every request-local sink. */
  std::shared_ptr<Impl> impl_;
};

/**
 * @brief Tests whether two snapshots prove one exact unchanged callback cut.
 * @param before Stable pre-boundary snapshot.
 * @param after Stable post-boundary snapshot.
 * @return True only when both cuts are stable and every entry, completion,
 * claim, publication, and retained-event frontier is exactly equal.
 * @throws Nothing.
 */
bool m1_observation_cut_unchanged(
    const M1FairnessObservationSnapshot& before,
    const M1FairnessObservationSnapshot& after) noexcept;

/**
 * @brief Checked-derives exact M1 phase boundaries from `B^M1`.
 * @param measurement_start Immutable measured boundary `B^M1`.
 * @return Exact `C^M1`, `W^M1`, `B^M1`, and `U^M1` values.
 * @throws std::overflow_error when any subtraction/addition is not exact.
 */
M1Timeline derive_m1_timeline(
    std::chrono::steady_clock::time_point measurement_start);

/**
 * @brief Validates exact M1 cold/warmup/measured protocol and attribution.
 * @param input Complete boundary, I1, B1, carryover, supersession, and final
 * settlement evidence from one fresh process replicate.
 * @return Pass only for the frozen v1 protocol; any missing, extra, malformed,
 * reordered, rewritten, later-stage, or unproved fact returns Invalid.
 * @throws std::bad_alloc when indexing/diagnostic ownership allocates.
 * @note Machine timing SLOs are evaluated separately; a structurally exact
 * protocol can still fail latency/progress/fairness/waste/memory.
 */
M1ProtocolSummary evaluate_m1_protocol(M1ProtocolEvidenceInput input);

/**
 * @brief Evaluates five independent M1 fairness guards fail-closed.
 * @param input Complete raw deterministic fairness evidence.
 * @return Aggregates, reasons, independent verdicts, and composite verdict.
 * @throws std::bad_alloc when copied/sorted values or reasons allocate.
 * @note This inner summary is not a canonical outer row or machine claim.
 */
M1FairnessSummary evaluate_m1_fairness(M1FairnessEvidenceInput input);

/**
 * @brief Delegates both frozen same-ordinal M1 environment relations.
 * @param m1 Complete M1 required-storage environment evidence.
 * @param isolated_i1 Same-subject isolated I1 storage-N/A evidence.
 * @param isolated_b1_cap_eight Same-subject isolated B1 cap-eight evidence.
 * @return Independent base-only I1 and full-environment B1 results.
 * @throws Nothing; malformed or unverified evidence fails closed as false.
 */
M1EnvironmentPairCompatibility evaluate_m1_environment_pairs(
    const B1EnvironmentEvidence& m1, const B1EnvironmentEvidence& isolated_i1,
    const B1EnvironmentEvidence& isolated_b1_cap_eight) noexcept;

}  // namespace ps::benchmark
