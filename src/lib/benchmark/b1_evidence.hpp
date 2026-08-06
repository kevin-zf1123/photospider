/**
 * @file b1_evidence.hpp
 * @brief Declares closed B1 physical observations and inner-row evaluation.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "benchmark/b1_environment.hpp"   // NOLINT(build/include_subdir)
#include "benchmark/b1_host.hpp"          // NOLINT(build/include_subdir)
#include "benchmark/b1_output_store.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/i1_evidence.hpp"      // NOLINT(build/include_subdir)

namespace ps::benchmark {

/** @brief Exact closed inner-row schema owned by Issue #95. */
inline constexpr char kB1InnerRowSchema[] = "execution-profile-b1-inner-row-v1";

/** @brief Exact structural version of the closed B1 inner row. */
inline constexpr std::uint32_t kB1InnerRowSchemaVersion = 1U;

/** @brief Exact logical work credited by one verified B1 job. */
inline constexpr std::uint64_t kB1SiteOperationsPerJob = 16777216U;

/** @brief Candidate/reference median throughput-ratio floor. */
inline constexpr double kB1MedianThroughputRatioLimit = 0.95;

/** @brief Candidate/reference every-replicate ratio floor. */
inline constexpr double kB1MinimumThroughputRatioLimit = 0.90;

/**
 * @brief One allocation-free physical service-start observation.
 * @throws Nothing for value construction and copying.
 */
struct B1ObservedServiceStart final {
  /** @brief Exact owning Run identity. */
  std::uint64_t run_id = 0U;
  /** @brief Exact Run-local deterministic callback identity. */
  std::uint64_t local_task_id = 0U;
  /** @brief Exact `work_units + ceil(ready_bytes/4096)` charge. */
  std::uint64_t service_charge = 0U;
  /** @brief Exact physical Run QoS copied at the start boundary. */
  compute::ComputeRunQos qos;
  /** @brief Observer-local causal coordinate. */
  compute::ComputeRunObservationCoordinate coordinate;
};

/**
 * @brief One raw Run lifecycle transition observed for a B1 job.
 * @throws Nothing for value construction and copying.
 */
struct B1ObservedRunTransition final {
  /** @brief Exact owning Run identity. */
  std::uint64_t run_id = 0U;
  /** @brief Observer-local causal coordinate. */
  compute::ComputeRunObservationCoordinate coordinate;
};

/**
 * @brief One accepted current-generation publication for a B1 request.
 * @throws Nothing for value construction and copying.
 * @note The request-scoped collector supplies occurrence identity; the value
 * carries only product generation and its authoritative causal coordinate.
 */
struct B1ObservedCurrentGeneration final {
  /** @brief Nonzero product-assigned generation becoming current. */
  std::uint64_t generation = 0U;
  /** @brief Observer-local causal coordinate of currentness publication. */
  compute::ComputeRunObservationCoordinate coordinate;
};

/**
 * @brief One accepted cancellation retained in the raw B1 physical trace.
 * @throws Nothing for value construction and copying.
 */
struct B1ObservedCancellation final {
  /** @brief Exact cancelled Run identity. */
  std::uint64_t run_id = 0U;
  /** @brief Product cancellation category that won terminal arbitration. */
  compute::ComputeRunCancellationReason reason =
      compute::ComputeRunCancellationReason::ExplicitRequest;
  /** @brief Observer-local causal coordinate of cancellation acceptance. */
  compute::ComputeRunObservationCoordinate coordinate;
};

/**
 * @brief Complete fixed-capacity physical observation snapshot for one job.
 * @throws std::bad_alloc when copied vectors allocate or visible digest reads.
 */
struct B1RunObservationSnapshot final {
  /** @brief Complete immutable occurrence bound before Host admission. */
  B1JobInstance job;
  /** @brief Whether any fixed category or causal sequence overflowed. */
  bool overflowed = false;
  /** @brief Complete accepted current-generation publications. */
  std::vector<B1ObservedCurrentGeneration> current_generations;
  /** @brief Exact physically committed callback starts. */
  std::vector<B1ObservedServiceStart> service_starts;
  /** @brief Complete accepted Run cancellations; fault-free B1 has none. */
  std::vector<B1ObservedCancellation> cancellations;
  /** @brief Exactly-once terminal kind when materialized. */
  std::optional<compute::ComputeRunTerminalKind> terminal_kind;
  /** @brief Terminal transition identity/coordinate. */
  std::optional<B1ObservedRunTransition> terminal;
  /** @brief Current-visible transition identity/coordinate. */
  std::optional<B1ObservedRunTransition> visible;
  /** @brief Physical quiescence transition identity/coordinate. */
  std::optional<B1ObservedRunTransition> quiescent;
  /** @brief Root-resource settlement transition identity/coordinate. */
  std::optional<B1ObservedRunTransition> resource_settled;
  /** @brief Typed digest of the exact product-visible Value when available. */
  ContentDigestResult visible_content_digest;
};

/**
 * @brief Fixed-capacity allocation-free observation owner for one B1 request.
 * @throws std::bad_alloc when shared fixed storage allocates at construction.
 * @note Product callbacks allocate nothing and never block or re-enter product
 * state; only `snapshot()` copies and traverses the retained visible Value.
 */
class B1RunObservationCollector final {
 public:
  /**
   * @brief Allocates fixed observation storage bound to one occurrence.
   * @param job Valid complete occurrence identity.
   * @throws std::invalid_argument for invalid job identity.
   * @throws std::bad_alloc when shared fixed storage allocation fails.
   */
  explicit B1RunObservationCollector(B1JobInstance job);

  /**
   * @brief Releases collector ownership after every product sink has unwound.
   * @throws Nothing.
   */
  ~B1RunObservationCollector() noexcept;

  /** @brief Prevents duplicating one request-local causal authority. */
  B1RunObservationCollector(const B1RunObservationCollector&) = delete;

  /** @brief Prevents assigning shared request-local observation state. */
  B1RunObservationCollector& operator=(const B1RunObservationCollector&) =
      delete;

  /**
   * @brief Returns the observation-only sink supplied to `B1Host`.
   * @return Shared sink retained safely through Run settlement.
   * @throws std::bad_alloc only when shared ownership is copied on an exotic
   * implementation.
   */
  std::shared_ptr<compute::ComputeRunObservationSink> sink() const;

  /**
   * @brief Copies all release-published observations after Host return.
   * @return Complete raw physical trace and visible logical digest.
   * @throws std::bad_alloc or content-digest errors while snapshotting.
   * @note The caller must wait for product settlement before treating absence
   * as evidence rather than a concurrent cut.
   */
  B1RunObservationSnapshot snapshot() const;

 private:
  /** @brief Opaque fixed callback store and sink implementation. */
  class Impl;
  /** @brief Shared store retained by the collector and product Run. */
  std::shared_ptr<Impl> impl_;
};

/**
 * @brief Raw and deterministic evidence for one cold/warmup/measured job.
 * @throws std::bad_alloc when owned trace/output/snapshot storage allocates.
 */
struct B1JobEvidence final {
  /** @brief Complete immutable occurrence identity. */
  B1JobInstance job;
  /** @brief Graph-local contiguous producer offer ordinal. */
  std::uint64_t producer_offer_ordinal = 0U;
  /** @brief Monotonic sample immediately before Host offer. */
  std::chrono::steady_clock::time_point offered_at;
  /** @brief Monotonic sample after Run/receipt/golden endpoint. */
  std::chrono::steady_clock::time_point endpoint_at;
  /** @brief Whether the real Host Run returned successful nonempty image. */
  bool run_succeeded = false;
  /** @brief Complete raw physical product-path observations. */
  B1RunObservationSnapshot physical_trace;
  /** @brief Snapshot immediately before Host offer. */
  B1ExecutionSnapshot execution_before;
  /** @brief Snapshot after complete job endpoint. */
  B1ExecutionSnapshot execution_after;
  /** @brief Exact crash-durable output result and I/O evidence. */
  B1OutputCommitResult output;
  /** @brief Independently initialized expected logical/raw identity. */
  B1JobGolden golden;
  /** @brief Complete canonical semantic trace bytes. */
  std::string semantic_trace;
  /** @brief SHA-256 over exact semantic trace bytes. */
  B1Sha256Digest semantic_trace_digest;
};

/**
 * @brief Complete raw input to one isolated cap-one or cap-eight B1 row.
 * @throws std::bad_alloc when job/environment/snapshot storage allocates.
 */
struct B1InnerRowInput final {
  /** @brief Fresh-process replicate ordinal in `[1,3]`. */
  std::uint64_t replicate_ordinal = 0U;
  /** @brief Exact isolated Run cap one or eight. */
  std::uint64_t run_cap = 0U;
  /** @brief Exact environment bytes/digests/eligibility for this row. */
  B1EnvironmentEvidence environment;
  /** @brief Cut immediately before both measured producer queues are offered.
   */
  std::chrono::steady_clock::time_point measurement_start;
  /** @brief Final measured job golden-verification completion cut. */
  std::chrono::steady_clock::time_point measurement_end;
  /** @brief Initial process resource/lifecycle/I/O state before cold job. */
  B1ExecutionSnapshot initial_snapshot;
  /** @brief Final quiescent state after all cold/warmup/measured jobs. */
  B1ExecutionSnapshot final_snapshot;
  /** @brief Exact cold, three warmup, and thirty measured job evidence. */
  std::vector<B1JobEvidence> jobs;
};

/**
 * @brief Closed typed B1 inner row with four independent verdict axes.
 * @throws std::bad_alloc when raw input/reasons are copied.
 * @note This is not the canonical 15-field outer row or comparison resolver.
 */
struct B1InnerRow final {
  /** @brief Exact inner schema identity. */
  std::string schema = kB1InnerRowSchema;
  /** @brief Exact structural version. */
  std::uint32_t schema_version = kB1InnerRowSchemaVersion;
  /** @brief Exact frozen workload identity. */
  std::string workload_id = kB1WorkloadId;
  /** @brief Complete retained raw row input. */
  B1InnerRowInput evidence;
  /** @brief Verified measured endpoints, at most thirty. */
  std::size_t verified_measured_jobs = 0U;
  /** @brief Successful logical site operations credited to throughput. */
  std::uint64_t successful_site_operations = 0U;
  /** @brief Measured MPix-op/s over the full required interval. */
  std::optional<double> throughput_mpix_ops_per_second;
  /** @brief Count of logical golden mismatches. */
  std::size_t logical_golden_mismatches = 0U;
  /** @brief Count of raw payload golden mismatches. */
  std::size_t raw_golden_mismatches = 0U;
  /** @brief Count of malformed/mismatched semantic traces. */
  std::size_t semantic_trace_mismatches = 0U;
  /** @brief Count of failed receipt/manifest identity checks. */
  std::size_t artifact_mismatches = 0U;
  /** @brief Exact all-started service charge. */
  std::uint64_t all_started_service = 0U;
  /** @brief Service belonging to non-verified, duplicate, or retry work. */
  std::uint64_t discarded_started_service = 0U;
  /** @brief Started service causally ordered after accepted cancellation. */
  std::uint64_t post_cancellation_started_service = 0U;
  /** @brief Exact discarded/all ratio, absent for a zero denominator. */
  std::optional<double> discarded_started_service_ratio;
  /** @brief Duplicate Run-local physical task-start count. */
  std::size_t duplicate_service_starts = 0U;
  /** @brief Retry start count; fault-free B1 requires zero. */
  std::size_t retry_service_starts = 0U;
  /** @brief Event-aligned Compute I/O task-count high-water. */
  std::uint64_t compute_io_task_high_water = 0U;
  /** @brief Event-aligned Compute I/O planned-byte high-water. */
  std::uint64_t compute_io_planned_byte_high_water = 0U;
  /** @brief Complete structural invalidation reasons. */
  std::vector<std::string> validity_reasons;
  /** @brief Independent endpoint/throughput evidence verdict. */
  I1Verdict throughput_verdict = I1Verdict::Invalid;
  /** @brief Independent exact digest/trace/golden verdict. */
  I1Verdict determinism_verdict = I1Verdict::Invalid;
  /** @brief Independent fault-free discard/duplicate/retry verdict. */
  I1Verdict waste_verdict = I1Verdict::Invalid;
  /** @brief Independent limits/high-water/final-settlement verdict. */
  I1Verdict memory_verdict = I1Verdict::Invalid;
};

/**
 * @brief Cross-row exact determinism comparison over caps and replicates.
 * @throws std::bad_alloc when mismatch reasons/grouping allocate.
 */
struct B1DeterminismSummary final {
  /** @brief Complete compared row count. */
  std::size_t row_count = 0U;
  /** @brief Aggregate exact identity mismatch count. */
  std::size_t mismatch_count = 0U;
  /** @brief Pass, Fail, or Invalid for missing/malformed corpus. */
  I1Verdict verdict = I1Verdict::Invalid;
};

/**
 * @brief Three-ordinal candidate/reference throughput-ratio result.
 * @throws std::bad_alloc when ratio storage allocates.
 */
struct B1ReferenceThroughputSummary final {
  /** @brief Ratios ordered by replicate ordinal one through three. */
  std::vector<double> replicate_ratios;
  /** @brief Median of exactly three valid ratios. */
  std::optional<double> median_ratio;
  /** @brief Independent reference-relative throughput verdict. */
  I1Verdict verdict = I1Verdict::Invalid;
};

/**
 * @brief Evaluates and closes one exact isolated B1 inner row fail-closed.
 * @param input Complete cold/warmup/measured/environment/resource evidence.
 * @return Closed four-axis row.
 * @throws std::bad_alloc when retained evidence/reasons allocate.
 */
B1InnerRow evaluate_b1_inner_row(B1InnerRowInput input);

/**
 * @brief Compares exact job identities across three replicates and both caps.
 * @param rows Six valid isolated rows in any order.
 * @return Exact cross-row mismatch count and verdict.
 * @throws std::bad_alloc when grouping retained identities allocates.
 */
B1DeterminismSummary evaluate_b1_cross_row_determinism(
    const std::vector<B1InnerRow>& rows);

/**
 * @brief Evaluates three same-environment candidate/reference ratios.
 * @param candidate Three candidate rows.
 * @param reference Three immutable reference rows.
 * @return Ordered ratios, median, and 0.95/0.90 verdict.
 * @throws std::bad_alloc when indexing/ratio storage allocates.
 */
B1ReferenceThroughputSummary evaluate_b1_reference_throughput(
    const std::vector<B1InnerRow>& candidate,
    const std::vector<B1InnerRow>& reference);

}  // namespace ps::benchmark
