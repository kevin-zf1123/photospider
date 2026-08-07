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
 * @brief One measured M1 Throughput progress window.
 * @throws Nothing for value construction and copying.
 */
struct M1ThroughputProgressSample final {
  /** @brief Verified successful M1 B1 rate for this one-second window. */
  double measured_rate = 0.0;

  /** @brief Same-ordinal storage-compatible isolated B1 cap-eight rate. */
  double paired_isolated_rate = 0.0;
};

/**
 * @brief Completed charged service retained for one Graph-peer window.
 * @throws Nothing for value construction and copying.
 */
struct M1GraphServiceWindow final {
  /** @brief True only when both producers retain demand for the full window. */
  bool both_graphs_continuously_demanding = false;

  /** @brief Successful completed charged service for Graph A. */
  std::uint64_t graph_a_completed_service = 0U;

  /** @brief Successful completed charged service for Graph B. */
  std::uint64_t graph_b_completed_service = 0U;
};

/**
 * @brief One actual class start and its applicability evidence.
 * @throws Nothing for value construction and copying.
 */
struct M1ClassStartSample final {
  /** @brief Actual class copied from the immutable Run descriptor. */
  compute::ComputeRunQosClass service_class =
      compute::ComputeRunQosClass::Throughput;

  /** @brief True only while both classes are continuously startable. */
  bool both_classes_continuously_startable = false;
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
 * @brief Complete raw inputs for deterministic M1 fairness aggregation.
 * @throws std::bad_alloc when vector storage is copied.
 * @note The latency verdict is supplied by the accepted I1 evidence authority.
 */
struct M1FairnessEvidenceInput final {
  /** @brief Exact measured one-second progress samples in window order. */
  std::vector<M1ThroughputProgressSample> progress_windows;

  /** @brief All retained Graph-peer service windows in time order. */
  std::vector<M1GraphServiceWindow> graph_service_windows;

  /** @brief Actual class starts with continuous-startability classification. */
  std::vector<M1ClassStartSample> class_starts;

  /** @brief Complete measured Interactive admission classification. */
  M1HeadroomAdmissionEvidence headroom_admissions;

  /** @brief Frozen I1 mixed-latency verdict computed by its own authority. */
  I1Verdict interactive_latency_verdict = I1Verdict::Invalid;

  /** @brief True when the bounded mixed observer dropped any required event. */
  bool observation_overflowed = false;

  /** @brief True when the shared nonzero causal sequence was exhausted. */
  bool observation_sequence_exhausted = false;

  /** @brief True when a request tag disagreed with actual descriptor QoS. */
  bool observation_qos_mismatch = false;
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
 * @brief Checked-derives exact M1 phase boundaries from `B^M1`.
 * @param measurement_start Immutable measured boundary `B^M1`.
 * @return Exact `C^M1`, `W^M1`, `B^M1`, and `U^M1` values.
 * @throws std::overflow_error when any subtraction/addition is not exact.
 */
M1Timeline derive_m1_timeline(
    std::chrono::steady_clock::time_point measurement_start);

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
