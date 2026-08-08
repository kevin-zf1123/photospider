/**
 * @file m1_evidence.hpp
 * @brief Declares the closed mixed-profile inner row and five-axis evaluator.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "benchmark/b1_evidence.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/m1_host.hpp"      // NOLINT(build/include_subdir)
#include "benchmark/m1_profile.hpp"   // NOLINT(build/include_subdir)

namespace ps::benchmark {

/** @brief Exact closed reversible inner-row schema owned by Issue #96. */
inline constexpr char kM1InnerRowSchema[] = "execution-profile-m1-inner-row-v2";

/** @brief Exact structural version of the closed M1 inner row. */
inline constexpr std::uint32_t kM1InnerRowSchemaVersion = 2U;

/** @brief Exact relative mixed/isolated I1 p99 multiplier ceiling. */
inline constexpr double kM1RelativeLatencyP99Limit = 2.0;

/**
 * @brief Exact B1 fault-free started-service evidence retained by M1.
 * @throws Nothing for value construction and copying.
 */
struct M1BatchWasteEvidence final {
  /** @brief Complete measured B1 started-service charge. */
  std::uint64_t all_started_service = 0U;
  /** @brief Started B1 service that did not reach a verified endpoint. */
  std::uint64_t discarded_started_service = 0U;
  /** @brief B1 service beginning after accepted cancellation. */
  std::uint64_t post_cancellation_started_service = 0U;
  /** @brief Duplicate Run-local B1 task starts. */
  std::size_t duplicate_service_starts = 0U;
  /** @brief Explicit B1 retry starts; fault-free M1 requires zero. */
  std::size_t retry_service_starts = 0U;
};

/**
 * @brief Complete raw Issue #93 source bound to one M1 I1 occurrence.
 *
 * The phase/ordinal/origin tuple is the M1 source identity. `episode` is the
 * complete closed evaluator input retained without a derived-verdict shortcut;
 * replay must reproduce the matching `M1InteractiveOccurrenceEvidence`.
 *
 * @throws std::bad_alloc when the complete episode evidence is copied.
 * @note The source owns observations only. It carries no Run, graph,
 * scheduling, cancellation, or storage authority.
 */
struct M1InteractiveSourceEvidence final {
  /** @brief Immutable cold/warmup/measured attribution. */
  B1JobPhase phase = B1JobPhase::Cold;
  /** @brief Zero-based ordinal within the immutable phase. */
  std::size_t phase_ordinal = 0U;
  /** @brief Exact M1 origin coordinate joined to the episode origin. */
  M1EventCoordinate origin;
  /** @brief Complete reversible Issue #93 evaluator input. */
  I1EpisodeEvidenceInput episode;
};

/**
 * @brief Authority-free observation of one store-minted B1 receipt.
 *
 * Every field used by the Issue #95 verified-endpoint predicate is copied from
 * the live receipt while its source-private capability exists. Rehydrating
 * this value never constructs a `B1OutputCommitReceipt`, opens a path, or
 * proves that storage remains live.
 *
 * @throws std::bad_alloc when strings, paths, or occurrence identity copy.
 */
struct M1BatchReceiptEvidence final {
  /** @brief Stable lowercase commit identity. */
  std::string commit_id;
  /** @brief Observed canonical output root; not a descriptor capability. */
  std::filesystem::path resolved_root;
  /** @brief Observed root-relative immutable occurrence slot. */
  std::filesystem::path rooted_slot;
  /** @brief Complete occurrence identity copied from the receipt. */
  B1JobInstance job;
  /** @brief Exact logical descriptor string. */
  std::string logical_descriptor;
  /** @brief Typed logical candidate content identity. */
  ContentDigest logical_content_digest;
  /** @brief Exact committed generation. */
  std::uint64_t committed_generation = 0U;
  /** @brief Exact payload leaf name. */
  std::string payload_name;
  /** @brief Exact manifest leaf name. */
  std::string manifest_name;
  /** @brief Exact committed payload length. */
  std::uint64_t payload_length = 0U;
  /** @brief Exact committed manifest length. */
  std::uint64_t manifest_length = 0U;
  /** @brief SHA-256 of exact payload bytes. */
  B1Sha256Digest payload_digest;
  /** @brief SHA-256 of exact canonical manifest bytes. */
  B1Sha256Digest manifest_digest;
  /** @brief Durability requested from the source store. */
  B1OutputDurability requested_durability = B1OutputDurability::CrashDurable;
  /** @brief Durability observed before the receipt was minted. */
  B1OutputDurability achieved_durability = B1OutputDurability::CrashDurable;
  /** @brief Nonempty source-observed published-manifest identity. */
  std::string published_manifest_identity;
};

/**
 * @brief Complete authority-free B1 source used by M1 replay.
 *
 * Offer identity/cuts bind this source to exactly one protocol offer. The raw
 * physical trace, output receipt observation, independent golden, semantic
 * bytes, and executor-authored I/O stream are sufficient to recompute output
 * eligibility and service waste. No output-store or receipt capability is
 * retained or reconstructible.
 *
 * @throws std::bad_alloc when owned trace, receipt, or byte storage copies.
 */
struct M1BatchSourceEvidence final {
  /** @brief Complete immutable occurrence identity. */
  B1JobInstance job;
  /** @brief Graph-local contiguous producer offer ordinal. */
  std::uint64_t producer_offer_ordinal = 0U;
  /** @brief Monotonic sample immediately before the Host offer. */
  std::chrono::steady_clock::time_point offered_at;
  /** @brief Monotonic sample after the complete occurrence endpoint. */
  std::chrono::steady_clock::time_point endpoint_at;
  /** @brief Whether the real Host returned a successful frozen image. */
  bool run_succeeded = false;
  /** @brief Verified-endpoint projection exact-checked against raw evidence. */
  bool verified_endpoint = false;
  /** @brief Complete raw physical product-path observations. */
  B1RunObservationSnapshot physical_trace;
  /** @brief Exact output terminal status. */
  B1OutputCommitStatus output_status = B1OutputCommitStatus::InvalidRequest;
  /** @brief Plain copied receipt facts, never a rehydrated capability. */
  std::optional<M1BatchReceiptEvidence> output_receipt;
  /** @brief Complete executor-authored event-aligned I/O stream. */
  std::vector<B1ComputeIoObservation> io_observations;
  /** @brief Independently frozen expected logical/raw identity. */
  B1JobGolden golden;
  /** @brief Complete canonical semantic trace bytes. */
  std::string semantic_trace;
  /** @brief SHA-256 over exact semantic trace bytes. */
  B1Sha256Digest semantic_trace_digest;
};

/**
 * @brief Complete fairness projection derived only from retained I1/B1
 * sources.
 *
 * Progress and Graph service windows come from exact protocol boundaries,
 * offers, and replayed B1 endpoints/service charges. Headroom outcomes,
 * first-measured admission, and the final-warmup current-hold facts come from
 * the forty measured and final warmup Issue #93 episode sources. Class-start
 * observations, pair denominators, and observer health flags stay outside
 * this projection because they have independent retained authorities.
 *
 * @throws std::bad_alloc when projected vector or status ownership allocates.
 */
struct M1SourceFairnessProjection final {
  /** @brief Source-derived first measured admission/current replacement. */
  M1FirstMeasuredAdmissionEvidence first_measured_admission;
  /** @brief Whether final warmup's immutable Q_end remained beyond B. */
  bool final_warmup_settlement_pending_at_measurement = false;
  /** @brief Exact thirty source-derived one-second progress windows. */
  std::vector<M1ThroughputProgressSample> progress_windows;
  /** @brief Exact thirty source-derived Graph demand/service windows. */
  std::vector<M1GraphServiceWindow> graph_service_windows;
  /** @brief Checked aggregate over all source-derived headroom outcomes. */
  M1HeadroomAdmissionEvidence headroom_admissions;
  /** @brief Exact ordered forty-by-twelve source-derived outcomes. */
  std::vector<M1HeadroomAdmissionOutcome> headroom_outcomes;
};

/**
 * @brief Binds one evaluated Issue #93 row to its exact M1 occurrence.
 * @param phase Immutable M1 phase.
 * @param phase_ordinal Zero-based phase-local ordinal.
 * @param origin Exact M1 origin coordinate.
 * @param row Complete evaluated Issue #93 row retaining its raw input.
 * @return Complete source evidence suitable for canonical replay.
 * @throws std::invalid_argument when source identity or derived row facts do
 * not recompute exactly.
 * @throws std::bad_alloc when the complete episode input is copied.
 */
M1InteractiveSourceEvidence make_m1_interactive_source_evidence(
    B1JobPhase phase, std::size_t phase_ordinal, M1EventCoordinate origin,
    const I1EpisodeInnerRow& row);

/**
 * @brief Projects one complete B1 job into canonical M1 source evidence.
 * @param evidence Complete source-private B1 job evidence.
 * @return Exact offer binding, checked endpoint projection, raw trace, receipt
 * observation, golden, and I/O source fields.
 * @throws std::bad_alloc when owned source evidence is copied.
 * @note The projection cannot recover or serialize receipt/storage authority.
 */
M1BatchSourceEvidence make_m1_batch_source_evidence(
    const B1JobEvidence& evidence);

/**
 * @brief Rebuilds the complete retained-source fairness projection.
 * @param protocol Exact M1 boundaries, occurrence identities, and B1 offers.
 * @param interactive_sources Exact ordered Issue #93 source for every one of
 * the 48 protocol occurrences.
 * @param batch_sources Exact ordered authority-free B1 source for every
 * protocol offer.
 * @return Exact first admission/current hold, thirty progress windows, thirty
 * Graph windows, 480 headroom outcomes, and checked headroom aggregates.
 * @throws std::invalid_argument when cardinality, identity/order, endpoint,
 * source replay, edit identity, or retained endpoint projection is malformed.
 * @throws std::overflow_error when an exact operation, service, or aggregate
 * count is unrepresentable.
 * @throws std::bad_alloc when replay indexes, canonical traces, or projection
 * storage allocate.
 * @note This is the sole producer/reader rule for source-derived admission and
 * fairness. It does not derive class-start observations or paired denominators.
 */
M1SourceFairnessProjection derive_m1_source_fairness_projection(
    const M1ProtocolEvidenceInput& protocol,
    const std::vector<M1InteractiveSourceEvidence>& interactive_sources,
    const std::vector<M1BatchSourceEvidence>& batch_sources);

/**
 * @brief Recomputes measured B1 waste from complete canonical source rows.
 * @param sources Exact-one source for every M1 B1 protocol offer.
 * @return Measured-phase all/discarded/post-cancel/duplicate/retry aggregate.
 * @throws std::invalid_argument when a source is malformed, lossy, or
 * contradictory.
 * @throws std::overflow_error when an exact service sum is unrepresentable.
 * @throws std::bad_alloc when replay diagnostics or temporary indexes grow.
 * @note Output eligibility is recomputed from source fields and exact-checked
 * against the retained projection. No serialized receipt observation is
 * promoted into live storage authority.
 */
M1BatchWasteEvidence derive_m1_batch_waste_evidence(
    const std::vector<M1BatchSourceEvidence>& sources);

/**
 * @brief Complete raw inputs for one closed M1 replicate row.
 * @throws std::bad_alloc when protocol, fairness, or snapshots are copied.
 */
struct M1InnerRowInput final {
  /** @brief Fresh-process ordinal in `[1,3]`. */
  std::uint64_t replicate_ordinal = 0U;
  /** @brief Exact deterministic phase/offer/carryover protocol evidence. */
  M1ProtocolEvidenceInput protocol;
  /** @brief Raw progress, Graph, class-start, and headroom evidence. */
  M1FairnessEvidenceInput fairness;
  /** @brief Same-ordinal compatible isolated-I1 p99 denominator. */
  std::optional<std::chrono::nanoseconds> paired_isolated_i1_p99;
  /** @brief Exact-one reversible Issue #93 source for every I1 occurrence. */
  std::vector<M1InteractiveSourceEvidence> interactive_sources;
  /** @brief Complete fault-free measured B1 service/waste aggregate. */
  M1BatchWasteEvidence batch_waste;
  /** @brief Exact-one authority-free source for every protocol B1 offer. */
  std::vector<M1BatchSourceEvidence> batch_sources;
  /** @brief Chronological same-domain resource/lifecycle/ready/I/O samples. */
  std::vector<M1ExecutionSnapshot> temporal_snapshots;
  /** @brief True when occurrence-owned aggregates exclude cold/warmup work. */
  bool occurrence_attribution_proved = false;
  /** @brief True when measured-window physical effects include carryover work.
   */
  bool temporal_effects_complete = false;
};

/**
 * @brief Closed typed Issue #96 M1 inner row with five independent axes.
 * @throws std::bad_alloc when retained input or diagnostics are copied.
 * @note This is not the canonical 15-field outer row. Its raw evidence is
 * packaged into retained outer sections only after this evaluator closes it.
 */
struct M1InnerRow final {
  /** @brief Exact inner schema identity. */
  std::string schema = kM1InnerRowSchema;
  /** @brief Exact structural version. */
  std::uint32_t schema_version = kM1InnerRowSchemaVersion;
  /** @brief Exact frozen workload identity. */
  std::string workload_id = kM1WorkloadId;
  /** @brief Complete retained raw row input. */
  M1InnerRowInput evidence;
  /** @brief Exact deterministic protocol result. */
  M1ProtocolSummary protocol;
  /**
   * @brief True only when every I1/B1 source replays and exact-matches its
   * ordered occurrence, endpoint, waste, progress, Graph, and headroom
   * projection.
   */
  bool source_evidence_closed = false;
  /** @brief Nearest-rank p50/p95/p99 over exactly forty measured episodes. */
  std::optional<I1LatencyPercentiles> latency;
  /** @brief Mixed p99 divided by paired isolated-I1 p99. */
  std::optional<double> relative_latency_p99;
  /** @brief Complete progress/Graph/class-start/headroom/latency aggregation.
   */
  M1FairnessSummary fairness;
  /** @brief Measured Interactive all-started service. */
  std::uint64_t interactive_all_started_service = 0U;
  /** @brief Measured Interactive discarded started service. */
  std::uint64_t interactive_discarded_started_service = 0U;
  /** @brief Measured Interactive post-cancellation started service. */
  std::uint64_t interactive_post_cancellation_started_service = 0U;
  /** @brief Exact measured Interactive discarded/all ratio. */
  std::optional<double> interactive_discarded_ratio;
  /** @brief Event-aligned process Compute I/O task high-water. */
  std::uint64_t compute_io_task_high_water = 0U;
  /** @brief Event-aligned process Compute I/O planned-byte high-water. */
  std::uint64_t compute_io_planned_byte_high_water = 0U;
  /** @brief Complete structural invalidation reasons across all five axes. */
  std::vector<std::string> validity_reasons;
  /** @brief Independent absolute/relative mixed-I1 latency verdict. */
  I1Verdict latency_verdict = I1Verdict::Invalid;
  /** @brief Independent paired B1 p05 progress verdict. */
  I1Verdict throughput_progress_verdict = I1Verdict::Invalid;
  /** @brief Independent five-guard mixed fairness verdict. */
  I1Verdict fairness_verdict = I1Verdict::Invalid;
  /** @brief Independent Interactive plus fault-free B1 waste verdict. */
  I1Verdict waste_verdict = I1Verdict::Invalid;
  /** @brief Independent authoritative limits/high-water/zero-settlement
   * verdict. */
  I1Verdict memory_verdict = I1Verdict::Invalid;
  /** @brief Invalid-priority conjunction of all five non-substitutable axes. */
  I1Verdict overall_verdict = I1Verdict::Invalid;
};

/**
 * @brief Evaluates and closes one exact M1 inner row fail-closed.
 * @param input Complete protocol, measured I1/B1, fairness, pairing, and
 * temporal resource evidence.
 * @return Closed five-axis M1 row retaining every raw input.
 * @throws std::bad_alloc when evidence, percentile, or diagnostics allocate.
 * @note The evaluator first replays every retained Issue #93 source and every
 * retained B1 physical/output/I/O source, exact-matches their occurrence,
 * endpoint, waste, progress, Graph, headroom-outcome, and headroom-aggregate
 * projections, and only then evaluates the five axes. Fairness/memory consume
 * all physical effects inside the measured window, including warmup carryover.
 */
M1InnerRow evaluate_m1_inner_row(M1InnerRowInput input);

}  // namespace ps::benchmark
