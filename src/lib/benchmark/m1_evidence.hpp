/**
 * @file m1_evidence.hpp
 * @brief Declares the closed mixed-profile inner row and five-axis evaluator.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "benchmark/b1_evidence.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/m1_host.hpp"      // NOLINT(build/include_subdir)
#include "benchmark/m1_profile.hpp"   // NOLINT(build/include_subdir)

namespace ps::benchmark {

/** @brief Exact closed inner-row schema owned by Issue #96. */
inline constexpr char kM1InnerRowSchema[] = "execution-profile-m1-inner-row-v1";

/** @brief Exact structural version of the closed M1 inner row. */
inline constexpr std::uint32_t kM1InnerRowSchemaVersion = 1U;

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
  /** @brief Complete fault-free measured B1 service/waste aggregate. */
  M1BatchWasteEvidence batch_waste;
  /** @brief Exact-one complete B1 job records for every protocol offer. */
  std::vector<B1JobEvidence> batch_jobs;
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
 * @note The evaluator derives Interactive latency/waste only from immutable
 * measured-phase occurrences, while fairness/memory consume all physical
 * effects inside the measured window, including warmup carryover.
 */
M1InnerRow evaluate_m1_inner_row(M1InnerRowInput input);

}  // namespace ps::benchmark
