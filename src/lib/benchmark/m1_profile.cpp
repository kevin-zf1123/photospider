/**
 * @file m1_profile.cpp
 * @brief Implements deterministic M1 fairness aggregation and observation.
 */
#include "benchmark/m1_profile.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ps::benchmark {
namespace {

/**
 * @brief Appends one fail-closed structural diagnostic.
 * @param reasons Mutable complete diagnostic list.
 * @param reason Stable human-readable reason text.
 * @return Nothing.
 * @throws std::bad_alloc when diagnostic ownership allocates.
 */
void invalidate(std::vector<std::string>* reasons, std::string reason) {
  reasons->push_back(std::move(reason));
}

/**
 * @brief Selects one one-based nearest-rank floating sample.
 * @param samples Nonempty finite sample values copied for sorting.
 * @param numerator Positive percentile numerator.
 * @param denominator Positive denominator no smaller than numerator.
 * @return Sorted sample at `ceil(numerator*N/denominator)`.
 * @throws std::invalid_argument for empty samples or an invalid fraction.
 * @throws std::overflow_error when rank multiplication overflows.
 * @throws std::bad_alloc when sorting storage allocates.
 */
double m1_nearest_rank(std::vector<double> samples, std::uint32_t numerator,
                       std::uint32_t denominator) {
  if (samples.empty() || numerator == 0U || denominator == 0U ||
      numerator > denominator) {
    throw std::invalid_argument(
        "M1 nearest rank requires samples and a fraction in (0,1].");
  }
  const std::uint64_t count = static_cast<std::uint64_t>(samples.size());
  if (count > std::numeric_limits<std::uint64_t>::max() / numerator) {
    throw std::overflow_error("M1 nearest-rank multiplication overflowed.");
  }
  const std::uint64_t product = count * numerator;
  const std::uint64_t rank =
      product / denominator +
      static_cast<std::uint64_t>(product % denominator != 0U);
  std::sort(samples.begin(), samples.end());
  return samples[static_cast<std::size_t>(rank - 1U)];
}

/**
 * @brief Conjoins non-substitutable fairness verdicts with invalid priority.
 * @param verdicts Complete component verdict list.
 * @return Invalid if any input is invalid, otherwise Fail if any fails, else
 * Pass.
 * @throws Nothing.
 */
I1Verdict compose_fairness(
    const std::initializer_list<I1Verdict>& verdicts) noexcept {
  bool failed = false;
  for (const I1Verdict verdict : verdicts) {
    if (verdict == I1Verdict::Invalid) {
      return I1Verdict::Invalid;
    }
    if (verdict == I1Verdict::Fail) {
      failed = true;
    }
  }
  return failed ? I1Verdict::Fail : I1Verdict::Pass;
}

/**
 * @brief Tests whether a request tag matches an actual scheduling class.
 * @param tag Pre-admission observer tag.
 * @param service_class Immutable product Run class.
 * @return True for Interactive/Interactive or either Throughput tag/
 * Throughput.
 * @throws Nothing; an invalid tag fails closed.
 */
bool m1_tag_matches_class(M1ObservedRequestTag tag,
                          compute::ComputeRunQosClass service_class) noexcept {
  switch (tag) {
    case M1ObservedRequestTag::Interactive:
      return service_class == compute::ComputeRunQosClass::Interactive;
    case M1ObservedRequestTag::ThroughputGraphA:
    case M1ObservedRequestTag::ThroughputGraphB:
      return service_class == compute::ComputeRunQosClass::Throughput;
  }
  return false;
}

/**
 * @brief Validates the closed request-tag enum at construction time.
 * @param tag Candidate tag.
 * @return True for one of the three closed values.
 * @throws Nothing.
 */
bool valid_m1_request_tag(M1ObservedRequestTag tag) noexcept {
  switch (tag) {
    case M1ObservedRequestTag::Interactive:
    case M1ObservedRequestTag::ThroughputGraphA:
    case M1ObservedRequestTag::ThroughputGraphB:
      return true;
  }
  return false;
}

}  // namespace

/**
 * @brief Owns the shared fixed-capacity storage and tagged observer adapters.
 *
 * Every sink reserves coordinates from one atomic sequence and claims a
 * unique preallocated slot for each retained scalar product event. Snapshot
 * work remains outside product callbacks and sorts completed publications.
 *
 * @throws std::invalid_argument when constructed with zero capacity.
 * @throws std::bad_alloc when slot or shared sink ownership allocates.
 * @note Callback methods remain bounded, nonblocking, allocation-free, and
 * observation-only; exhaustion is sticky evidence rather than product flow.
 */
class M1FairnessObservationCollector::Impl final
    : public std::enable_shared_from_this<Impl> {
 public:
  /**
   * @brief Allocates every callback publication slot before product work.
   * @param capacity Positive maximum record count.
   * @throws std::invalid_argument when capacity is zero.
   * @throws std::bad_alloc when slot ownership cannot allocate.
   */
  explicit Impl(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0U) {
      throw std::invalid_argument("M1 observation capacity must be positive.");
    }
    slots_ = std::make_unique<Slot[]>(capacity);
  }

  /**
   * @brief Request-local adapter from product callbacks to shared scalar slots.
   * @throws Nothing for destruction; construction only copies shared ownership.
   */
  class Sink final : public compute::ComputeRunObservationSink {
   public:
    /**
     * @brief Binds one immutable tag to the shared observer state.
     * @param impl Shared store that outlives every callback.
     * @param tag Valid closed request role.
     * @throws Nothing for ownership transfer.
     */
    Sink(std::shared_ptr<Impl> impl, M1ObservedRequestTag tag) noexcept
        : impl_(std::move(impl)), tag_(tag) {}

    /** @copydoc compute::ComputeRunObservationSink::reserve_causal_coordinate
     */
    compute::ComputeRunObservationCoordinate
    reserve_causal_coordinate() noexcept override {
      return impl_->reserve_causal_coordinate();
    }

    /** @copydoc compute::ComputeRunObservationSink::on_current_generation */
    void on_current_generation(
        const compute::SupersessionIdentity& identity,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      (void)identity;
      (void)coordinate;
    }

    /** @copydoc compute::ComputeRunObservationSink::observes_task_semantics */
    bool observes_task_semantics() const noexcept override { return true; }

    /** @copydoc compute::ComputeRunObservationSink::on_service_start */
    void on_service_start(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunTaskIdentity task_identity,
        std::uint64_t service_charge,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->record(M1FairnessObservation{
          M1ObservationKind::ServiceStart, tag_, descriptor.qos().service_class,
          m1_tag_matches_class(tag_, descriptor.qos().service_class),
          coordinate.causal_sequence, coordinate.observed_at,
          descriptor.id().value(), task_identity.local_task_id().value(),
          service_charge, compute::ComputeRunTaskTerminalKind::Succeeded,
          compute::ComputeRunTerminalKind::Succeeded});
    }

    /** @copydoc compute::ComputeRunObservationSink::on_task_terminal */
    void on_task_terminal(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunTaskIdentity task_identity,
        compute::ComputeRunTaskTerminalKind kind,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->record(M1FairnessObservation{
          M1ObservationKind::TaskTerminal, tag_, descriptor.qos().service_class,
          m1_tag_matches_class(tag_, descriptor.qos().service_class),
          coordinate.causal_sequence, coordinate.observed_at,
          descriptor.id().value(), task_identity.local_task_id().value(), 0U,
          kind, compute::ComputeRunTerminalKind::Succeeded});
    }

    /** @copydoc compute::ComputeRunObservationSink::on_cancellation */
    void on_cancellation(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunCancellationReason reason,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      (void)descriptor;
      (void)reason;
      (void)coordinate;
    }

    /** @copydoc compute::ComputeRunObservationSink::on_terminal */
    void on_terminal(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunTerminalKind kind,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->record(M1FairnessObservation{
          M1ObservationKind::RunTerminal, tag_, descriptor.qos().service_class,
          m1_tag_matches_class(tag_, descriptor.qos().service_class),
          coordinate.causal_sequence, coordinate.observed_at,
          descriptor.id().value(), 0U, 0U,
          compute::ComputeRunTaskTerminalKind::Succeeded, kind});
    }

    /** @copydoc compute::ComputeRunObservationSink::on_current_visible */
    void on_current_visible(
        const compute::ComputeRunDescriptor& descriptor, Value output,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      (void)descriptor;
      (void)output;
      (void)coordinate;
    }

    /** @copydoc compute::ComputeRunObservationSink::on_run_quiescent */
    void on_run_quiescent(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      (void)descriptor;
      (void)coordinate;
    }

    /** @copydoc compute::ComputeRunObservationSink::on_run_resource_settled */
    void on_run_resource_settled(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->record(M1FairnessObservation{
          M1ObservationKind::RunResourceSettled, tag_,
          descriptor.qos().service_class,
          m1_tag_matches_class(tag_, descriptor.qos().service_class),
          coordinate.causal_sequence, coordinate.observed_at,
          descriptor.id().value(), 0U, 0U,
          compute::ComputeRunTaskTerminalKind::Succeeded,
          compute::ComputeRunTerminalKind::Succeeded});
    }

    /** @copydoc compute::ComputeRunObservationSink::on_host_settled */
    void on_host_settled(
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      (void)coordinate;
    }

   private:
    /** @brief Shared store retaining every scalar callback record. */
    std::shared_ptr<Impl> impl_;

    /** @brief Immutable request-local observer role. */
    M1ObservedRequestTag tag_;
  };

  /**
   * @brief Creates one tagged sink retaining this shared implementation.
   * @param tag Valid request-local role.
   * @return New observation-only sink.
   * @throws std::bad_alloc when sink ownership allocates.
   */
  std::shared_ptr<compute::ComputeRunObservationSink> make_sink(
      M1ObservedRequestTag tag) {
    return std::make_shared<Sink>(shared_from_this(), tag);
  }

  /**
   * @brief Reserves one shared nonzero observer coordinate without blocking.
   * @return Steady sample and unique sequence, or zero after exhaustion.
   * @throws Nothing.
   */
  compute::ComputeRunObservationCoordinate
  reserve_causal_coordinate() noexcept {
    const auto observed_at = std::chrono::steady_clock::now();
    std::uint64_t current = next_sequence_.load(std::memory_order_relaxed);
    while (current != 0U) {
      const std::uint64_t next =
          current == std::numeric_limits<std::uint64_t>::max() ? 0U
                                                               : current + 1U;
      if (next_sequence_.compare_exchange_weak(current, next,
                                               std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
        if (next == 0U) {
          sequence_exhausted_.store(true, std::memory_order_release);
        }
        return compute::ComputeRunObservationCoordinate{observed_at, current};
      }
    }
    sequence_exhausted_.store(true, std::memory_order_release);
    return compute::ComputeRunObservationCoordinate{observed_at, 0U};
  }

  /**
   * @brief Publishes one complete scalar event into a unique fixed slot.
   * @param event Fully assembled immutable callback record.
   * @return Nothing.
   * @throws Nothing; capacity/sequence exhaustion is recorded explicitly.
   */
  void record(M1FairnessObservation event) noexcept {
    if (event.causal_sequence == 0U) {
      sequence_exhausted_.store(true, std::memory_order_release);
      return;
    }
    if (!event.qos_matches_tag) {
      qos_mismatch_.store(true, std::memory_order_release);
    }
    std::size_t index = next_slot_.load(std::memory_order_relaxed);
    while (true) {
      if (index >= capacity_) {
        overflowed_.store(true, std::memory_order_release);
        return;
      }
      if (next_slot_.compare_exchange_weak(index, index + 1U,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {
        break;
      }
    }
    slots_[index].value = event;
    slots_[index].published.store(true, std::memory_order_release);
  }

  /**
   * @brief Copies fully published slots in shared causal order.
   * @return Complete bounded snapshot and invalidity flags.
   * @throws std::bad_alloc when event storage allocates.
   */
  M1FairnessObservationSnapshot snapshot() const {
    M1FairnessObservationSnapshot result;
    result.events.reserve(
        std::min(next_slot_.load(std::memory_order_acquire), capacity_));
    for (std::size_t index = 0U; index < capacity_; ++index) {
      if (slots_[index].published.load(std::memory_order_acquire)) {
        result.events.push_back(slots_[index].value);
      }
    }
    std::sort(
        result.events.begin(), result.events.end(),
        [](const M1FairnessObservation& lhs, const M1FairnessObservation& rhs) {
          return lhs.causal_sequence < rhs.causal_sequence;
        });
    result.overflowed = overflowed_.load(std::memory_order_acquire);
    result.sequence_exhausted =
        sequence_exhausted_.load(std::memory_order_acquire);
    result.qos_mismatch = qos_mismatch_.load(std::memory_order_acquire);
    return result;
  }

 private:
  /**
   * @brief One release-published fixed-capacity observation cell.
   * @throws Nothing for construction and destruction.
   */
  struct Slot final {
    /** @brief True only after `value` is immutable and visible. */
    std::atomic<bool> published{false};

    /** @brief Scalar event written exactly once by the claiming callback. */
    M1FairnessObservation value;
  };

  /** @brief Immutable number of preallocated event cells. */
  const std::size_t capacity_;

  /** @brief Complete fixed slot array allocated before product callbacks. */
  std::unique_ptr<Slot[]> slots_;

  /** @brief Next unique event-cell index claimed by a callback. */
  std::atomic<std::size_t> next_slot_{0U};

  /** @brief Next shared nonzero observer-local causal sequence. */
  std::atomic<std::uint64_t> next_sequence_{1U};

  /** @brief Sticky capacity-exhaustion evidence. */
  std::atomic<bool> overflowed_{false};

  /** @brief Sticky causal-sequence exhaustion evidence. */
  std::atomic<bool> sequence_exhausted_{false};

  /** @brief Sticky tag/actual-QoS contradiction evidence. */
  std::atomic<bool> qos_mismatch_{false};
};

/** @copydoc M1FairnessObservationCollector::M1FairnessObservationCollector */
M1FairnessObservationCollector::M1FairnessObservationCollector(
    std::size_t capacity)
    : impl_(std::make_shared<Impl>(capacity)) {}

/** @copydoc M1FairnessObservationCollector::make_sink */
std::shared_ptr<compute::ComputeRunObservationSink>
M1FairnessObservationCollector::make_sink(M1ObservedRequestTag tag) const {
  if (!valid_m1_request_tag(tag)) {
    throw std::invalid_argument("M1 observation request tag is invalid.");
  }
  return impl_->make_sink(tag);
}

/** @copydoc M1FairnessObservationCollector::snapshot */
M1FairnessObservationSnapshot M1FairnessObservationCollector::snapshot() const {
  return impl_->snapshot();
}

/** @copydoc derive_m1_timeline */
M1Timeline derive_m1_timeline(
    std::chrono::steady_clock::time_point measurement_start) {
  return M1Timeline{
      checked_i1_time_subtract(measurement_start, kM1ColdToMeasurement),
      checked_i1_time_subtract(measurement_start, kM1WarmupToMeasurement),
      measurement_start,
      checked_i1_time_add(measurement_start, kM1MeasurementDuration)};
}

/** @copydoc evaluate_m1_fairness */
M1FairnessSummary evaluate_m1_fairness(M1FairnessEvidenceInput input) {
  M1FairnessSummary summary;
  summary.interactive_latency_verdict = input.interactive_latency_verdict;

  if (input.observation_overflowed) {
    invalidate(&summary.validity_reasons,
               "mixed observation capacity was exhausted");
  }
  if (input.observation_sequence_exhausted) {
    invalidate(&summary.validity_reasons,
               "mixed observation causal sequence was exhausted");
  }
  if (input.observation_qos_mismatch) {
    invalidate(&summary.validity_reasons,
               "mixed observation request tag disagreed with actual QoS");
  }
  const bool observation_valid = !input.observation_overflowed &&
                                 !input.observation_sequence_exhausted &&
                                 !input.observation_qos_mismatch;

  bool progress_valid = observation_valid;
  std::vector<double> progress_ratios;
  if (input.progress_windows.size() != kM1MeasuredWindowCount) {
    progress_valid = false;
    invalidate(&summary.validity_reasons,
               "M1 progress requires exactly 30 measured windows");
  } else {
    progress_ratios.reserve(input.progress_windows.size());
    for (const M1ThroughputProgressSample& window : input.progress_windows) {
      if (!std::isfinite(window.measured_rate) ||
          !std::isfinite(window.paired_isolated_rate) ||
          window.measured_rate < 0.0 || window.paired_isolated_rate <= 0.0) {
        progress_valid = false;
        invalidate(&summary.validity_reasons,
                   "M1 progress contains a malformed rate or denominator");
        break;
      }
      const double ratio = window.measured_rate / window.paired_isolated_rate;
      if (!std::isfinite(ratio) || ratio < 0.0) {
        progress_valid = false;
        invalidate(&summary.validity_reasons,
                   "M1 progress ratio is not finite and nonnegative");
        break;
      }
      progress_ratios.push_back(ratio);
    }
  }
  if (progress_valid) {
    summary.throughput_progress_p05 =
        m1_nearest_rank(std::move(progress_ratios), 5U, 100U);
    summary.throughput_progress_verdict =
        *summary.throughput_progress_p05 >= kM1ThroughputProgressP05Floor
            ? I1Verdict::Pass
            : I1Verdict::Fail;
  }

  bool jain_valid = observation_valid;
  std::vector<double> jain_samples;
  for (const M1GraphServiceWindow& window : input.graph_service_windows) {
    if (!window.both_graphs_continuously_demanding) {
      continue;
    }
    const long double graph_a =
        static_cast<long double>(window.graph_a_completed_service);
    const long double graph_b =
        static_cast<long double>(window.graph_b_completed_service);
    if (graph_a + graph_b == 0.0L) {
      jain_valid = false;
      invalidate(&summary.validity_reasons,
                 "eligible M1 Graph window has zero completed service");
      break;
    }
    const long double numerator = (graph_a + graph_b) * (graph_a + graph_b);
    const long double denominator =
        2.0L * (graph_a * graph_a + graph_b * graph_b);
    const double jain = static_cast<double>(numerator / denominator);
    if (!std::isfinite(jain) || jain <= 0.0 || jain > 1.0) {
      jain_valid = false;
      invalidate(&summary.validity_reasons,
                 "eligible M1 Graph window has invalid Jain service");
      break;
    }
    jain_samples.push_back(jain);
  }
  if (jain_samples.empty()) {
    jain_valid = false;
    invalidate(&summary.validity_reasons,
               "M1 has no continuously demanding Graph-peer window");
  }
  if (jain_valid) {
    summary.graph_jain_p05 = m1_nearest_rank(std::move(jain_samples), 5U, 100U);
    summary.graph_jain_verdict = *summary.graph_jain_p05 >= kM1GraphJainP05Floor
                                     ? I1Verdict::Pass
                                     : I1Verdict::Fail;
  }

  bool class_start_valid = observation_valid;
  bool saw_applicable_start = false;
  bool saw_applicable_throughput = false;
  std::size_t interactive_burst = 0U;
  bool open_interactive_burst = false;
  for (const M1ClassStartSample& start : input.class_starts) {
    if (!start.both_classes_continuously_startable) {
      interactive_burst = 0U;
      open_interactive_burst = false;
      continue;
    }
    saw_applicable_start = true;
    switch (start.service_class) {
      case compute::ComputeRunQosClass::Interactive:
        ++interactive_burst;
        open_interactive_burst = true;
        summary.maximum_interactive_burst =
            std::max(summary.maximum_interactive_burst, interactive_burst);
        break;
      case compute::ComputeRunQosClass::Throughput:
        saw_applicable_throughput = true;
        interactive_burst = 0U;
        open_interactive_burst = false;
        break;
    }
  }
  if (!saw_applicable_start || !saw_applicable_throughput) {
    class_start_valid = false;
    invalidate(&summary.validity_reasons,
               "M1 lacks a complete both-class start sequence");
  } else if (open_interactive_burst) {
    class_start_valid = false;
    invalidate(&summary.validity_reasons,
               "M1 both-class start sequence ends before Throughput");
  }
  if (class_start_valid) {
    summary.class_start_verdict =
        summary.maximum_interactive_burst <= kM1InteractiveBurstLimit
            ? I1Verdict::Pass
            : I1Verdict::Fail;
  }

  const M1HeadroomAdmissionEvidence& admissions = input.headroom_admissions;
  const bool headroom_valid =
      observation_valid &&
      admissions.attempted_edits == kM1MeasuredI1AttemptCount &&
      admissions.classified_outcomes == admissions.attempted_edits &&
      admissions.throughput_headroom_failures <= admissions.classified_outcomes;
  if (!headroom_valid) {
    invalidate(&summary.validity_reasons,
               "M1 headroom admission classification is incomplete");
  } else {
    summary.interactive_headroom_verdict =
        admissions.throughput_headroom_failures == 0U ? I1Verdict::Pass
                                                      : I1Verdict::Fail;
  }

  summary.composite_fairness_verdict = compose_fairness(
      {summary.throughput_progress_verdict, summary.graph_jain_verdict,
       summary.class_start_verdict, summary.interactive_headroom_verdict,
       summary.interactive_latency_verdict});
  return summary;
}

/** @copydoc evaluate_m1_environment_pairs */
M1EnvironmentPairCompatibility evaluate_m1_environment_pairs(
    const B1EnvironmentEvidence& m1, const B1EnvironmentEvidence& isolated_i1,
    const B1EnvironmentEvidence& isolated_b1_cap_eight) noexcept {
  return M1EnvironmentPairCompatibility{
      compatible_b1_environments(m1, isolated_i1,
                                 B1EnvironmentRelation::M1PairedI1BaseOnly),
      compatible_b1_environments(m1, isolated_b1_cap_eight,
                                 B1EnvironmentRelation::M1PairedB1CapEight)};
}

}  // namespace ps::benchmark
