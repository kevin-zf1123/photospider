/**
 * @file i2_evidence.hpp
 * @brief Declares closed version-one I2 inner evidence and evaluation.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "benchmark/i1_evidence.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/i2_profile.hpp"   // NOLINT(build/include_subdir)

namespace ps::benchmark {

/** @brief Exact closed inner-row schema identifier owned by Issue #94. */
inline constexpr char kI2InnerRowSchema[] = "execution-profile-i2-inner-row-v1";

/** @brief Exact structural version of the closed I2 inner row. */
inline constexpr std::uint32_t kI2InnerRowSchemaVersion = 1U;

/** @brief Exact progressive workload identity frozen by ADR 0010. */
inline constexpr char kI2WorkloadId[] = "I2-progressive-v1";

/** @brief I2 measured preview p50 latency ceiling. */
inline constexpr std::chrono::nanoseconds kI2PreviewLatencyP50Limit{50000000};

/** @brief I2 measured preview p95 latency ceiling. */
inline constexpr std::chrono::nanoseconds kI2PreviewLatencyP95Limit{75000000};

/** @brief I2 measured preview p99 latency ceiling. */
inline constexpr std::chrono::nanoseconds kI2PreviewLatencyP99Limit{100000000};

/** @brief I2 measured final p95 latency ceiling. */
inline constexpr std::chrono::nanoseconds kI2FinalLatencyP95Limit{500000000};

/** @brief I2 measured final p99 latency ceiling. */
inline constexpr std::chrono::nanoseconds kI2FinalLatencyP99Limit{1000000000};

/** @brief Maximum measured discarded Interactive service ratio. */
inline constexpr double kI2DiscardedServiceRatioLimit = 0.25;

/**
 * @brief Copyable admission/settlement evidence for one I2 edit.
 * @throws std::bad_alloc when owned status strings are copied.
 * @note This closed value deliberately omits the move-only settlement future.
 */
struct I2EditEvidence final {
  /** @brief Zero-based edit identity. */
  std::size_t edit_index = 0U;
  /** @brief Exact frozen node-one coefficient. */
  double coefficient = 0.0;
  /** @brief Exact HP/source-space dirty Region. */
  PixelRect source_region;
  /** @brief Exact derived preview-space Region. */
  PixelRect preview_region;
  /** @brief Immutable nominal Host-call start. */
  std::chrono::steady_clock::time_point nominal_time;
  /** @brief Whether this edit reached admission sampling. */
  bool admission_attempted = false;
  /** @brief Sole pre-Host-call monotonic sample. */
  std::chrono::steady_clock::time_point admission_sample;
  /** @brief Whether the inclusive two-millisecond window was satisfied. */
  bool admission_window_valid = false;
  /** @brief Reserved nonzero row-local sequence. */
  std::optional<std::uint64_t> reserved_event_sequence;
  /** @brief Exact preview deadline. */
  std::optional<std::chrono::steady_clock::time_point> preview_deadline;
  /** @brief Exact final deadline. */
  std::optional<std::chrono::steady_clock::time_point> final_deadline;
  /** @brief Raw Host return facts. */
  std::optional<I1HostReturnEvidence> host_return;
  /** @brief Success-only accepted coordinate. */
  std::optional<compute::AcceptedBoundaryCoordinate> accepted_coordinate;
  /** @brief Product status consumed after the evidence cut. */
  std::optional<OperationStatus> settlement_status;
};

/**
 * @brief Product generation and optional materialized child identities.
 * @throws Nothing for construction and copying.
 * @note Accepted-coordinate equality, not numeric generation order, owns
 * currentness among bound identities.
 */
struct I2AcceptedProductIdentity final {
  /** @brief Nonzero product request generation. */
  std::uint64_t generation = 0U;
  /** @brief Exact product-bound accepted coordinate. */
  std::optional<compute::AcceptedBoundaryCoordinate> accepted_coordinate;
  /** @brief Optional materialized RT Interactive Run. */
  std::optional<std::uint64_t> preview_run_id;
  /** @brief Optional materialized HP Full Run. */
  std::optional<std::uint64_t> final_run_id;
};

/**
 * @brief Complete raw input required to evaluate one closed I2 episode row.
 * @throws std::bad_alloc when copied observations/snapshots allocate.
 * @note Every time point belongs to the same process steady-clock domain.
 */
struct I2EpisodeEvidenceInput final {
  /** @brief Caller-owned replicate ordinal in `[1,3]`. */
  std::uint64_t replicate_ordinal = 0U;
  /** @brief Continuous-grid slot in `[0,110]`. */
  std::size_t slot = 0U;
  /** @brief Immutable replicate origin `G^I2`. */
  std::chrono::steady_clock::time_point grid_origin;
  /** @brief Checked-derived episode origin `E`. */
  std::chrono::steady_clock::time_point episode_origin;
  /** @brief Checked-derived stride-111 terminal boundary. */
  std::chrono::steady_clock::time_point terminal_boundary;
  /** @brief First observer coordinate excluded from the frozen history. */
  I1ObservationHistoryCut observation_cut;
  /** @brief All twelve admission/settlement records. */
  std::array<I2EditEvidence, kI1EditCount> edits;
  /** @brief Bounded child-aware product observations. */
  I2EpisodeObservationSnapshot observations;
  /** @brief Authoritative pre-episode process snapshot. */
  I1ExecutionSnapshot baseline;
  /** @brief Authoritative post-quiescent process snapshot. */
  I1ExecutionSnapshot final_snapshot;
  /** @brief Actual post-snapshot sample for terminal-boundary checking. */
  std::chrono::steady_clock::time_point final_snapshot_sample;
  /**
   * @brief Caller-supplied copy of the exact frozen edit-eleven preview
   * oracle.
   * @note Evaluation requires complete equality with
   * `i2_frozen_preview_content_digest()` before candidate comparison.
   */
  std::optional<ContentDigest> expected_preview_digest;
  /**
   * @brief Caller-supplied copy of the exact frozen I1 edit-eleven final
   * oracle.
   * @note Evaluation requires complete equality with
   * `i1_frozen_final_content_digest()` before candidate comparison.
   */
  std::optional<ContentDigest> expected_final_digest;
};

/**
 * @brief Two endpoint latencies derived from the shared twelfth admission.
 * @throws Nothing for value construction and copying.
 */
struct I2EpisodeLatencies final {
  /** @brief Accepted-to-current-preview duration. */
  std::optional<std::chrono::nanoseconds> preview;
  /** @brief Accepted-to-current-final duration. */
  std::optional<std::chrono::nanoseconds> final;
};

/**
 * @brief Closed typed Issue #94 inner row with raw and derived evidence.
 * @throws std::bad_alloc when input or reason ownership allocates.
 * @note This is not the canonical 15-field outer row and makes no outer digest
 * or reference-selection claim.
 */
struct I2EpisodeInnerRow final {
  /** @brief Exact schema identifier. */
  std::string schema = kI2InnerRowSchema;
  /** @brief Exact structural schema version. */
  std::uint32_t schema_version = kI2InnerRowSchemaVersion;
  /** @brief Exact frozen workload identity. */
  std::string workload_id = kI2WorkloadId;
  /** @brief Raw closed episode evidence. */
  I2EpisodeEvidenceInput evidence;
  /** @brief Product identity independently matched for every edit. */
  std::array<std::optional<I2AcceptedProductIdentity>, kI1EditCount>
      accepted_products;
  /** @brief Sole frozen twelfth-preview digest result. */
  ContentDigestResult preview_digest;
  /** @brief Sole frozen twelfth-final digest result. */
  ContentDigestResult final_digest;
  /** @brief Shared-anchor preview/final latency values. */
  I2EpisodeLatencies latencies;
  /** @brief Complete physical service sums and ratio. */
  I1ServiceEvidence service;
  /** @brief Whether authoritative process/resource state settled exactly. */
  bool memory_settled = false;
  /** @brief Fail-closed structural invalidation reasons. */
  std::vector<std::string> validity_reasons;
  /** @brief Independent endpoint-latency result. */
  I1Verdict latency_verdict = I1Verdict::Invalid;
  /** @brief Independent waste/no-hidden-work result. */
  I1Verdict waste_verdict = I1Verdict::Invalid;
  /** @brief Independent authoritative resource result. */
  I1Verdict memory_verdict = I1Verdict::Invalid;
  /** @brief Independent digest/access/no-I/O result. */
  I1Verdict output_verdict = I1Verdict::Invalid;
};

/**
 * @brief Nearest-rank preview/final aggregates over 100 measured episodes.
 * @throws Nothing for construction and copying.
 */
struct I2LatencyPercentiles final {
  /** @brief Preview nearest-rank p50. */
  std::chrono::nanoseconds preview_p50{0};
  /** @brief Preview nearest-rank p95. */
  std::chrono::nanoseconds preview_p95{0};
  /** @brief Preview nearest-rank p99. */
  std::chrono::nanoseconds preview_p99{0};
  /** @brief Final nearest-rank p50 retained as raw aggregate. */
  std::chrono::nanoseconds final_p50{0};
  /** @brief Final nearest-rank p95. */
  std::chrono::nanoseconds final_p95{0};
  /** @brief Final nearest-rank p99. */
  std::chrono::nanoseconds final_p99{0};
};

/**
 * @brief Replicate-level aggregates and four independent I2 verdicts.
 * @throws std::bad_alloc when rows/reasons are copied or samples sorted.
 */
struct I2ReplicateSummary final {
  /** @brief Exact inner-summary schema identifier. */
  std::string schema = "execution-profile-i2-inner-summary-v1";
  /** @brief Caller-owned replicate ordinal. */
  std::uint64_t replicate_ordinal = 0U;
  /** @brief Structurally accepted measured endpoint-pair count. */
  std::size_t measured_sample_count = 0U;
  /** @brief Measured nearest-rank values. */
  std::optional<I2LatencyPercentiles> latency;
  /** @brief Physical service aggregate from measured slots `11..110` only. */
  I1ServiceEvidence measured_service;
  /** @brief Complete replicate invalidation reasons. */
  std::vector<std::string> validity_reasons;
  /** @brief Independent preview/final latency verdict. */
  I1Verdict latency_verdict = I1Verdict::Invalid;
  /** @brief Independent measured waste/no-hidden-work verdict. */
  I1Verdict waste_verdict = I1Verdict::Invalid;
  /** @brief Independent all-phase memory/settlement verdict. */
  I1Verdict memory_verdict = I1Verdict::Invalid;
  /** @brief Independent all-phase digest/access verdict. */
  I1Verdict output_verdict = I1Verdict::Invalid;
};

/**
 * @brief Copies one move-only admission result into closed edit evidence.
 * @param admission Admission whose future remains caller-owned.
 * @param settlement Completed status consumed after the evidence cut.
 * @return Copyable record with exact coefficient and both Regions.
 * @throws std::out_of_range for an invalid edit identity.
 * @throws std::bad_alloc when status ownership allocates.
 */
I2EditEvidence capture_i2_edit_evidence(
    const I2EditAdmissionResult& admission,
    std::optional<OperationStatus> settlement);

/**
 * @brief Evaluates and closes one raw I2 episode fail-closed.
 *
 * The evaluator first validates the continuous-grid and child-lifecycle
 * structure, then derives endpoint latency, service, acquisition, and resource
 * evidence. It independently binds the expected preview and final digests to
 * their frozen workload oracles before comparing the already captured
 * candidate digests. Missing, unsupported, or substituted expected evidence
 * therefore produces output Invalid; a complete candidate-only mismatch
 * produces output Fail without changing the other three axes.
 *
 * @param input Complete raw evidence captured at frozen boundaries.
 * @return Closed row with matched children, sums, and independent verdicts.
 * @throws std::bad_alloc when copied evidence/reasons allocate.
 * @note The function traverses no Value and trusts no eventual-zero backfill.
 * Each accepted current-generation observation must precede every matching
 * child event. Every Cancelled terminal must have exactly one matching earlier
 * cancellation, and every other terminal must have none.
 */
I2EpisodeInnerRow evaluate_i2_episode(I2EpisodeEvidenceInput input);

/**
 * @brief Aggregates one exact continuous 111-slot I2 replicate.
 *
 * Memory and output verdicts consume all 111 rows. Latency and waste consume
 * complete verdicts, endpoint samples, and service only from measured slots
 * `11..110`; cold slot zero and warmup slots `1..10` propagate Invalid only,
 * while their Pass or Fail values do not enter steady-state aggregates.
 *
 * @param rows Evaluated rows in any order; slots must be unique and complete,
 * share one exact grid origin and stride-111 terminal boundary, and retain
 * checked-derived episode origins for their slot identities.
 * @return Measured percentiles/service sums and four verdicts.
 * @throws std::bad_alloc when indexing, copying, or sorting allocates.
 * @note Checked grid arithmetic failures and non-measured latency/waste
 * invalidity are captured as Invalid summary evidence and never escape as
 * arithmetic exceptions. Non-measured Fail never becomes a measured failure.
 */
I2ReplicateSummary evaluate_i2_replicate(
    const std::vector<I2EpisodeInnerRow>& rows);

}  // namespace ps::benchmark
