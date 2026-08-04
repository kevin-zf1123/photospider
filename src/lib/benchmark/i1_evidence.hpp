/**
 * @file i1_evidence.hpp
 * @brief Declares closed version-one I1 inner evidence and verdict evaluation.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "benchmark/i1_profile.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {

/** @brief Exact closed inner-row schema identifier owned by Issue #93. */
inline constexpr char kI1InnerRowSchema[] = "execution-profile-i1-inner-row-v1";

/** @brief Exact structural version of the closed I1 inner row. */
inline constexpr std::uint32_t kI1InnerRowSchemaVersion = 1U;

/** @brief Exact workload identifier frozen by ADR 0010. */
inline constexpr char kI1WorkloadId[] = "I1-edit-storm-v1";

/** @brief I1 measured nearest-rank p50 latency ceiling. */
inline constexpr std::chrono::nanoseconds kI1LatencyP50Limit{50000000};

/** @brief I1 measured nearest-rank p95 latency ceiling. */
inline constexpr std::chrono::nanoseconds kI1LatencyP95Limit{100000000};

/** @brief I1 measured nearest-rank p99 latency ceiling. */
inline constexpr std::chrono::nanoseconds kI1LatencyP99Limit{150000000};

/** @brief Maximum discarded/all started-service ratio for one replicate. */
inline constexpr double kI1DiscardedServiceRatioLimit = 0.25;

/**
 * @brief Independent outcome state for one I1 SLO dimension.
 * @throws Nothing for value construction or comparison.
 */
enum class I1Verdict : std::uint8_t {
  /** @brief Complete evidence satisfies the dimension's frozen gate. */
  Pass,
  /** @brief Complete evidence violates the dimension's frozen gate. */
  Fail,
  /** @brief Missing, malformed, contradictory, or lossy evidence. */
  Invalid,
};

/**
 * @brief Copyable admission and settlement evidence for one edit.
 * @throws std::bad_alloc when owned status strings are copied.
 * @note This value deliberately omits the move-only settlement future.
 */
struct I1EditEvidence final {
  /** @brief Zero-based edit identity in `[0,11]`. */
  std::size_t edit_index = 0U;
  /** @brief Exact frozen node-one coefficient for this edit. */
  double coefficient = 0.0;
  /** @brief Exact frozen source-space dirty Region. */
  PixelRect region;
  /** @brief Immutable checked-derived nominal Host-call start. */
  std::chrono::steady_clock::time_point nominal_time;
  /** @brief Whether this edit reached its admission boundary. */
  bool admission_attempted = false;
  /** @brief Sole pre-call monotonic sample when admission was attempted. */
  std::chrono::steady_clock::time_point admission_sample;
  /** @brief Whether the inclusive two-millisecond window was satisfied. */
  bool admission_window_valid = false;
  /** @brief Reserved nonzero row-local sequence, if reservation occurred. */
  std::optional<std::uint64_t> reserved_event_sequence;
  /** @brief Checked deadline, if a legal Host call was attempted. */
  std::optional<std::chrono::steady_clock::time_point> deadline;
  /** @brief Raw Host return facts, absent when no call was legal. */
  std::optional<I1HostReturnEvidence> host_return;
  /** @brief Success-only accepted pre-call coordinate. */
  std::optional<I1AcceptedCoordinate> accepted_coordinate;
  /** @brief Product status consumed after the authoritative history cut. */
  std::optional<OperationStatus> settlement_status;
};

/**
 * @brief Product identity and exact coordinate matched to one accepted edit.
 * @throws Nothing for value construction or comparison.
 * @note `run_id` is absent when supersession settles an accepted edit before a
 * concrete Run descriptor is materialized. A populated row always requires
 * `accepted_coordinate` to equal the edit's pre-call coordinate exactly.
 */
struct I1AcceptedProductIdentity final {
  /**
   * @brief Unique product generation assigned during request preparation.
   * @note Accepted-coordinate order, not numeric generation order, determines
   * currentness among bound identities.
   */
  std::uint64_t generation = 0U;
  /** @brief Optional opaque materialized Run identity. */
  std::optional<std::uint64_t> run_id;
  /** @brief Exact product-bound pre-call accepted coordinate. */
  std::optional<I1AcceptedCoordinate> accepted_coordinate;
};

/**
 * @brief Complete raw input required to evaluate one closed I1 inner row.
 * @throws std::bad_alloc when copied strings, observations, or snapshots
 * allocate.
 * @note Every time point uses the same process steady-clock domain.
 */
struct I1EpisodeEvidenceInput final {
  /** @brief Caller-owned replicate ordinal in the normative range `[1,3]`. */
  std::uint64_t replicate_ordinal = 0U;
  /** @brief Frozen continuous-grid slot in `[0,220]`. */
  std::size_t slot = 0U;
  /** @brief Single immutable replicate grid origin `G^I1`. */
  std::chrono::steady_clock::time_point grid_origin;
  /** @brief Checked-derived episode origin `E`. */
  std::chrono::steady_clock::time_point episode_origin;
  /** @brief Checked-derived terminal non-start boundary `T^I1`. */
  std::chrono::steady_clock::time_point terminal_boundary;
  /** @brief Exact nominal measurement start `Q_start=S_11`. */
  std::chrono::steady_clock::time_point measurement_start;
  /** @brief Exact causal-history boundary `Q_end`. */
  std::chrono::steady_clock::time_point measurement_end;
  /** @brief First collector coordinate excluded from the `Q_end` history. */
  I1ObservationHistoryCut observation_cut;
  /** @brief All twelve complete admission/settlement records. */
  std::array<I1EditEvidence, kI1EditCount> edits;
  /** @brief Bounded product-boundary observations for the episode. */
  I1EpisodeObservationSnapshot observations;
  /** @brief Authoritative snapshot after baseline materialization/settlement.
   */
  I1ExecutionSnapshot baseline;
  /** @brief Authoritative eventual snapshot after captured lifecycle history.
   */
  I1ExecutionSnapshot final_snapshot;
  /** @brief Actual post-snapshot sample retained for terminal-guard checking.
   */
  std::chrono::steady_clock::time_point final_snapshot_sample;
  /**
   * @brief Required independently frozen expected logical-content digest.
   * @note Optional representation preserves source-faithful invalid rows;
   * evaluation marks an absent, non-canonical, or non-frozen digest Invalid
   * and never learns this value from the observed candidate output.
   */
  std::optional<ContentDigest> expected_final_digest;
};

/**
 * @brief Derived started-service accounting for one episode.
 * @throws Nothing for value construction and copying.
 */
struct I1ServiceEvidence final {
  /** @brief Sum of every physically committed callback start charge. */
  std::uint64_t all_started_service = 0U;
  /** @brief Started service belonging to a Run that never committed visibly. */
  std::uint64_t discarded_started_service = 0U;
  /** @brief Service whose start ordered after accepted Run cancellation. */
  std::uint64_t post_cancel_started_service = 0U;
  /** @brief Exact double ratio, absent when the denominator is zero. */
  std::optional<double> discarded_ratio;
};

/**
 * @brief Closed typed Issue #93 inner row with raw and derived evidence.
 * @throws std::bad_alloc when input or diagnostic ownership allocates.
 * @note This is not the canonical 15-field outer row and carries no outer
 * section, row, or bundle digest claim.
 */
struct I1EpisodeInnerRow final {
  /** @brief Exact schema identifier; any other value is invalid. */
  std::string schema = kI1InnerRowSchema;
  /** @brief Exact structural version; any other value is invalid. */
  std::uint32_t schema_version = kI1InnerRowSchemaVersion;
  /** @brief Exact frozen workload identity. */
  std::string workload_id = kI1WorkloadId;
  /** @brief Raw closed episode evidence retained without normalization loss. */
  I1EpisodeEvidenceInput evidence;
  /** @brief Product identities independently matched by edit index. */
  std::array<std::optional<I1AcceptedProductIdentity>, kI1EditCount>
      accepted_products;
  /** @brief Typed digest result computed from the final visible Value. */
  ContentDigestResult final_digest;
  /** @brief Final accepted-to-visible duration when causally well formed. */
  std::optional<std::chrono::nanoseconds> final_latency;
  /** @brief Complete callback service sums and ratio. */
  I1ServiceEvidence service;
  /** @brief True when Host/device/lifecycle state returned to the baseline. */
  bool memory_settled = false;
  /** @brief Human-readable fail-closed structural invalidation reasons. */
  std::vector<std::string> validity_reasons;
  /** @brief Independent final-generation latency result for this episode. */
  I1Verdict latency_verdict = I1Verdict::Invalid;
  /** @brief Independent discarded/post-cancel service result. */
  I1Verdict waste_verdict = I1Verdict::Invalid;
  /** @brief Independent authoritative resource/settlement result. */
  I1Verdict memory_verdict = I1Verdict::Invalid;
  /** @brief Independent final logical-output result. */
  I1Verdict output_verdict = I1Verdict::Invalid;
};

/**
 * @brief Nearest-rank latency aggregates over exactly 200 measured samples.
 * @throws Nothing for value construction and copying.
 */
struct I1LatencyPercentiles final {
  /** @brief One-based nearest-rank p50 sample. */
  std::chrono::nanoseconds p50{0};
  /** @brief One-based nearest-rank p95 sample. */
  std::chrono::nanoseconds p95{0};
  /** @brief One-based nearest-rank p99 sample. */
  std::chrono::nanoseconds p99{0};
};

/**
 * @brief Replicate-level aggregates and four independent I1 verdicts.
 * @throws std::bad_alloc when inputs/reasons are copied or sorted.
 */
struct I1ReplicateSummary final {
  /** @brief Exact inner-summary schema identifier. */
  std::string schema = "execution-profile-i1-inner-summary-v1";
  /** @brief Caller-owned replicate ordinal shared by all 221 rows. */
  std::uint64_t replicate_ordinal = 0U;
  /** @brief Number of structurally accepted measured latency samples. */
  std::size_t measured_sample_count = 0U;
  /** @brief Measured nearest-rank values, absent on invalid evidence. */
  std::optional<I1LatencyPercentiles> latency;
  /** @brief Measured-phase started-service aggregate. */
  I1ServiceEvidence measured_service;
  /** @brief Structural invalidation reasons for the complete replicate. */
  std::vector<std::string> validity_reasons;
  /** @brief Independent p50/p95/p99 and per-episode latency verdict. */
  I1Verdict latency_verdict = I1Verdict::Invalid;
  /** @brief Independent measured waste/post-cancel verdict. */
  I1Verdict waste_verdict = I1Verdict::Invalid;
  /** @brief Independent all-phase resource/settlement verdict. */
  I1Verdict memory_verdict = I1Verdict::Invalid;
  /** @brief Independent all-phase digest consistency verdict. */
  I1Verdict output_verdict = I1Verdict::Invalid;
};

/**
 * @brief Copies one move-only admission result into closed row evidence.
 * @param admission Admission result whose future remains caller-owned.
 * @param settlement Completed status consumed after the history cut, if
 * available; the matching Host-settlement event proves whether it belonged to
 * the `Q_end` history.
 * @return Copyable evidence with frozen coefficient and Region facts.
 * @throws std::out_of_range when the edit identity is invalid.
 * @throws std::bad_alloc when status ownership cannot allocate.
 */
I1EditEvidence capture_i1_edit_evidence(
    const I1EditAdmissionResult& admission,
    std::optional<OperationStatus> settlement);

/**
 * @brief Selects one exact nearest-rank duration sample.
 * @param samples Nonempty unsorted sample vector.
 * @param percentile_numerator Positive numerator no greater than denominator.
 * @param percentile_denominator Positive exact denominator.
 * @return Sorted one-based sample at `ceil(numerator*N/denominator)`.
 * @throws std::invalid_argument for an empty vector or invalid fraction.
 * @throws std::overflow_error when rank multiplication is not representable.
 * @throws std::bad_alloc when sorting storage cannot allocate.
 */
std::chrono::nanoseconds i1_nearest_rank(
    std::vector<std::chrono::nanoseconds> samples,
    std::uint32_t percentile_numerator, std::uint32_t percentile_denominator);

/**
 * @brief Evaluates and closes one raw I1 episode row fail-closed.
 * @param input Complete raw evidence captured at the frozen boundaries.
 * @return Closed row with matched identities, derived sums, and verdicts.
 * @throws std::bad_alloc when copied evidence/diagnostics allocate.
 * @note Threshold failure yields `Fail`; missing, contradictory, overflowed,
 * lossy, unfrozen-output, or missing-golden evidence yields `Invalid`
 * independently per affected dimension. Product generations must be nonzero
 * and unique, but need not increase with edit order because bound currentness
 * linearizes by accepted coordinate. This function never traverses a Value.
 */
I1EpisodeInnerRow evaluate_i1_episode(I1EpisodeEvidenceInput input);

/**
 * @brief Aggregates one exact continuous 221-slot I1 replicate.
 * @param rows Evaluated rows in any order; slots must be unique and complete.
 * @return Measured nearest-rank/service results and independent verdicts.
 * @throws std::bad_alloc when rows are indexed, samples sorted, or reasons
 * allocate.
 * @note Cold/warmup rows are excluded from steady-state latency/waste numbers
 * but remain mandatory validity, memory, and output evidence.
 */
I1ReplicateSummary evaluate_i1_replicate(
    const std::vector<I1EpisodeInnerRow>& rows);

}  // namespace ps::benchmark
