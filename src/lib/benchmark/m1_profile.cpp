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
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ps::benchmark {
namespace {

static_assert(std::atomic<bool>::is_always_lock_free,
              "M1 callbacks require lock-free Boolean atomics.");
static_assert(std::atomic<std::size_t>::is_always_lock_free,
              "M1 callbacks require lock-free slot atomics.");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "M1 callbacks require lock-free sequence atomics.");

/** @brief Fixed maximum CAS attempts at one observer callback boundary. */
constexpr std::size_t kM1AtomicAttemptLimit = 64U;

/** @brief Fixed maximum attempts to enter the coordinate sampling gate. */
constexpr std::size_t kM1CoordinateGateAttemptLimit = 4096U;

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
 * @param reasons Mutable diagnostics receiving unknown closed-enum evidence.
 * @return Invalid if any input is invalid, otherwise Fail if any fails, else
 * Pass.
 * @throws std::bad_alloc when an unknown verdict diagnostic allocates.
 */
I1Verdict compose_fairness(const std::initializer_list<I1Verdict>& verdicts,
                           std::vector<std::string>* reasons) {
  bool failed = false;
  for (const I1Verdict verdict : verdicts) {
    switch (verdict) {
      case I1Verdict::Pass:
        break;
      case I1Verdict::Fail:
        failed = true;
        break;
      case I1Verdict::Invalid:
        return I1Verdict::Invalid;
      default:
        invalidate(reasons, "M1 fairness verdict contains an unknown value");
        return I1Verdict::Invalid;
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

/**
 * @brief Tests the closed public Host status-domain vocabulary.
 * @param domain Candidate raw status domain.
 * @return True only for a declared `OperationErrorDomain` value.
 * @throws Nothing.
 */
bool valid_operation_error_domain(OperationErrorDomain domain) noexcept {
  switch (domain) {
    case OperationErrorDomain::None:
    case OperationErrorDomain::Transport:
    case OperationErrorDomain::Protocol:
    case OperationErrorDomain::Graph:
    case OperationErrorDomain::Daemon:
      return true;
  }
  return false;
}

}  // namespace

/** @copydoc M1EventCoordinate::operator== */
bool M1EventCoordinate::operator==(
    const M1EventCoordinate& other) const noexcept {
  return timestamp == other.timestamp && event_sequence == other.event_sequence;
}

/** @copydoc M1EventCoordinate::operator< */
bool M1EventCoordinate::operator<(
    const M1EventCoordinate& other) const noexcept {
  return timestamp < other.timestamp || (timestamp == other.timestamp &&
                                         event_sequence < other.event_sequence);
}

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
  Impl(std::size_t capacity, std::uint64_t first_sequence,
       std::shared_ptr<M1ObservationPublicationHook> publication_hook = {})
      : capacity_(capacity),
        next_sequence_(first_sequence),
        publication_hook_(std::move(publication_hook)) {
    if (capacity == 0U || first_sequence == 0U) {
      throw std::invalid_argument(
          "M1 observation capacity and first sequence must be positive.");
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

    /** @copydoc compute::ComputeRunObservationSink::abort_causal_coordinate */
    void abort_causal_coordinate(
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->complete_causal_coordinate(coordinate);
    }

    /** @copydoc compute::ComputeRunObservationSink::on_current_generation */
    void on_current_generation(
        const compute::SupersessionIdentity& identity,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      (void)identity;
      impl_->complete_causal_coordinate(coordinate);
    }

    /** @copydoc compute::ComputeRunObservationSink::observes_task_semantics */
    bool observes_task_semantics() const noexcept override { return true; }

    /** @copydoc compute::ComputeRunObservationSink::on_task_ready */
    void on_task_ready(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunTaskIdentity task_identity,
        const compute::ComputeRunTaskReadyObservation& observation,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      (void)descriptor;
      (void)task_identity;
      (void)observation;
      impl_->complete_causal_coordinate(coordinate);
    }

    /** @copydoc compute::ComputeRunObservationSink::on_service_start */
    void on_service_start(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunTaskIdentity task_identity,
        std::uint64_t service_charge,
        const compute::ComputeRunServiceStartObservation& observation,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->record(M1FairnessObservation{
          M1ObservationKind::ServiceStart, tag_, descriptor.qos().service_class,
          m1_tag_matches_class(tag_, descriptor.qos().service_class),
          coordinate.causal_sequence, coordinate.observed_at,
          descriptor.id().value(), task_identity.local_task_id().value(),
          service_charge, compute::ComputeRunTaskTerminalKind::Succeeded,
          compute::ComputeRunTerminalKind::Succeeded,
          observation.interactive_candidate_startable,
          observation.throughput_candidate_startable,
          observation.execution_grant_committed});
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
      impl_->complete_causal_coordinate(coordinate);
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
      impl_->complete_causal_coordinate(coordinate);
    }

    /** @copydoc
     * compute::ComputeRunObservationSink::on_progressive_final_triggered */
    void on_progressive_final_triggered(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      (void)descriptor;
      impl_->complete_causal_coordinate(coordinate);
    }

    /** @copydoc compute::ComputeRunObservationSink::on_run_quiescent */
    void on_run_quiescent(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      (void)descriptor;
      impl_->complete_causal_coordinate(coordinate);
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
      impl_->complete_causal_coordinate(coordinate);
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
   * @brief Completes one successfully reserved coordinate on scope exit.
   * @throws Nothing for construction and destruction.
   * @note The guard never owns product state and only advances the bounded
   * diagnostic completion frontier.
   */
  class ReservationCompletionGuard final {
   public:
    /**
     * @brief Binds one reserved coordinate to its collector implementation.
     * @param impl Non-null collector implementation.
     * @throws Nothing.
     */
    explicit ReservationCompletionGuard(Impl* impl) noexcept : impl_(impl) {}

    /**
     * @brief Advances the matching completion frontier.
     * @throws Nothing; bounded frontier failure becomes sticky evidence.
     */
    ~ReservationCompletionGuard() noexcept {
      impl_->advance_reservation_frontier(
          &impl_->reservation_completion_frontier_);
    }

    /** @brief Prevents double completion through copied scope ownership. */
    ReservationCompletionGuard(const ReservationCompletionGuard&) = delete;

    /** @brief Prevents replacing one completion responsibility. */
    ReservationCompletionGuard& operator=(const ReservationCompletionGuard&) =
        delete;

   private:
    /** @brief Non-owning implementation alive for the callback duration. */
    Impl* impl_;
  };

  /**
   * @brief Reserves one shared nonzero observer coordinate without blocking.
   * @return Steady sample and unique sequence, or zero after exhaustion.
   * @throws Nothing.
   */
  compute::ComputeRunObservationCoordinate
  reserve_causal_coordinate() noexcept {
    (void)advance_reservation_frontier(&reservation_entry_frontier_);
    if (sequence_exhausted_.load(std::memory_order_acquire)) {
      return compute::ComputeRunObservationCoordinate{
          std::chrono::steady_clock::now(), 0U};
    }
    for (std::size_t attempt = 0U; attempt < kM1CoordinateGateAttemptLimit;
         ++attempt) {
      if (coordinate_reservation_gate_.test_and_set(
              std::memory_order_acquire)) {
        if (publication_hook_ != nullptr) {
          publication_hook_->after_coordinate_contention();
        }
        continue;
      }
      const auto observed_at = std::chrono::steady_clock::now();
      if (publication_hook_ != nullptr) {
        publication_hook_->after_coordinate_sample();
      }
      const std::uint64_t current =
          next_sequence_.load(std::memory_order_relaxed);
      if (current == 0U) {
        sequence_exhausted_.store(true, std::memory_order_release);
        coordinate_reservation_gate_.clear(std::memory_order_release);
        return compute::ComputeRunObservationCoordinate{observed_at, 0U};
      }
      const std::uint64_t next =
          current == std::numeric_limits<std::uint64_t>::max() ? 0U
                                                               : current + 1U;
      next_sequence_.store(next, std::memory_order_relaxed);
      if (next == 0U) {
        sequence_exhausted_.store(true, std::memory_order_release);
      }
      coordinate_reservation_gate_.clear(std::memory_order_release);
      return compute::ComputeRunObservationCoordinate{observed_at, current};
    }
    sequence_exhausted_.store(true, std::memory_order_release);
    return compute::ComputeRunObservationCoordinate{
        std::chrono::steady_clock::now(), 0U};
  }

  /**
   * @brief Closes one reservation after callback delivery or explicit abort.
   * @param coordinate Exact reserved coordinate; scalar contents are retained
   * only by the caller-specific event path.
   * @return Nothing.
   * @throws Nothing; frontier failure becomes sticky invalidity evidence.
   */
  void complete_causal_coordinate(
      compute::ComputeRunObservationCoordinate coordinate) noexcept {
    (void)coordinate;
    (void)advance_reservation_frontier(&reservation_completion_frontier_);
  }

  /**
   * @brief Publishes one complete scalar event into a unique fixed slot.
   * @param event Fully assembled immutable callback record.
   * @return Nothing.
   * @throws Nothing; capacity/sequence exhaustion is recorded explicitly.
   */
  void record(M1FairnessObservation event) noexcept {
    ReservationCompletionGuard completion(this);
    if (event.causal_sequence == 0U) {
      sequence_exhausted_.store(true, std::memory_order_release);
      return;
    }
    if (!event.qos_matches_tag) {
      qos_mismatch_.store(true, std::memory_order_release);
    }
    if (overflowed_.load(std::memory_order_acquire)) {
      return;
    }
    std::size_t index = next_slot_.load(std::memory_order_relaxed);
    for (std::size_t attempt = 0U; attempt < kM1AtomicAttemptLimit; ++attempt) {
      if (index >= capacity_) {
        overflowed_.store(true, std::memory_order_release);
        return;
      }
      if (next_slot_.compare_exchange_strong(index, index + 1U,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
        if (publication_hook_ != nullptr) {
          publication_hook_->after_slot_claim();
        }
        slots_[index].value = event;
        slots_[index].published.store(true, std::memory_order_release);
        return;
      }
    }
    overflowed_.store(true, std::memory_order_release);
  }

  /**
   * @brief Copies fully published slots in shared causal order.
   * @return Complete bounded snapshot and invalidity flags.
   * @throws std::bad_alloc when event storage allocates.
   */
  M1FairnessObservationSnapshot snapshot() const {
    M1FairnessObservationSnapshot result;
    const std::uint64_t entry_before =
        reservation_entry_frontier_.load(std::memory_order_acquire);
    const std::uint64_t completion_before =
        reservation_completion_frontier_.load(std::memory_order_acquire);
    const std::size_t claimed_before =
        next_slot_.load(std::memory_order_acquire);
    const std::size_t bounded_claimed = std::min(claimed_before, capacity_);
    result.events.reserve(bounded_claimed);
    std::size_t published_prefix = 0U;
    while (published_prefix < bounded_claimed &&
           slots_[published_prefix].published.load(std::memory_order_acquire)) {
      result.events.push_back(slots_[published_prefix].value);
      ++published_prefix;
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
    const std::size_t claimed_after =
        next_slot_.load(std::memory_order_acquire);
    const std::uint64_t completion_after =
        reservation_completion_frontier_.load(std::memory_order_acquire);
    const std::uint64_t entry_after =
        reservation_entry_frontier_.load(std::memory_order_acquire);
    result.reservation_entry_frontier = entry_after;
    result.reservation_completion_frontier = completion_after;
    result.claimed_slot_frontier = claimed_after;
    result.published_slot_frontier = published_prefix;
    result.reservation_frontier_exhausted =
        reservation_frontier_exhausted_.load(std::memory_order_acquire);
    result.stable_publication_cut =
        !result.reservation_frontier_exhausted &&
        entry_before == completion_before && entry_before == entry_after &&
        completion_before == completion_after &&
        entry_after == completion_after && claimed_before == claimed_after &&
        claimed_after <= capacity_ && published_prefix == claimed_after;
    return result;
  }

 private:
  /**
   * @brief Advances one saturating reservation frontier with bounded retries.
   * @param frontier Reservation-entry or completion counter to advance once.
   * @return True only when the counter advanced without wrapping.
   * @throws Nothing; exhaustion or excessive contention becomes sticky
   * invalidity evidence.
   */
  bool advance_reservation_frontier(
      std::atomic<std::uint64_t>* frontier) noexcept {
    std::uint64_t current = frontier->load(std::memory_order_relaxed);
    for (std::size_t attempt = 0U; attempt < kM1AtomicAttemptLimit; ++attempt) {
      if (current == std::numeric_limits<std::uint64_t>::max()) {
        reservation_frontier_exhausted_.store(true, std::memory_order_release);
        return false;
      }
      if (frontier->compare_exchange_strong(current, current + 1U,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
        return true;
      }
    }
    reservation_frontier_exhausted_.store(true, std::memory_order_release);
    return false;
  }

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

  /** @brief Monotonic count of entered coordinate reservations. */
  std::atomic<std::uint64_t> reservation_entry_frontier_{0U};

  /** @brief Monotonic count closed by callbacks or explicit aborts. */
  std::atomic<std::uint64_t> reservation_completion_frontier_{0U};

  /** @brief Sticky reservation-frontier exhaustion/contention evidence. */
  std::atomic<bool> reservation_frontier_exhausted_{false};

  /** @brief Bounded lock-free gate pairing sequence order with sample order. */
  std::atomic_flag coordinate_reservation_gate_ = ATOMIC_FLAG_INIT;

  /** @brief Next shared nonzero observer-local causal sequence. */
  std::atomic<std::uint64_t> next_sequence_;

  /** @brief Sticky capacity-exhaustion evidence. */
  std::atomic<bool> overflowed_{false};

  /** @brief Sticky causal-sequence exhaustion evidence. */
  std::atomic<bool> sequence_exhausted_{false};

  /** @brief Sticky tag/actual-QoS contradiction evidence. */
  std::atomic<bool> qos_mismatch_{false};

  /** @brief Optional post-claim hook installed only by deterministic tests. */
  const std::shared_ptr<M1ObservationPublicationHook> publication_hook_;
};

/** @copydoc M1FairnessObservationCollector::M1FairnessObservationCollector */
M1FairnessObservationCollector::M1FairnessObservationCollector(
    std::size_t capacity)
    : M1FairnessObservationCollector(capacity, 1U) {}

/** @copydoc M1FairnessObservationCollector::M1FairnessObservationCollector */
M1FairnessObservationCollector::M1FairnessObservationCollector(
    std::size_t capacity, std::uint64_t first_sequence)
    : impl_(std::make_shared<Impl>(capacity, first_sequence)) {}

/** @copydoc M1FairnessObservationCollector::M1FairnessObservationCollector */
M1FairnessObservationCollector::M1FairnessObservationCollector(
    std::size_t capacity, std::uint64_t first_sequence,
    std::shared_ptr<M1ObservationPublicationHook> publication_hook) {
  if (publication_hook == nullptr) {
    throw std::invalid_argument(
        "M1 observation publication test hook must be non-null.");
  }
  impl_ = std::make_shared<Impl>(capacity, first_sequence,
                                 std::move(publication_hook));
}

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

/** @copydoc m1_observation_cut_unchanged */
bool m1_observation_cut_unchanged(
    const M1FairnessObservationSnapshot& before,
    const M1FairnessObservationSnapshot& after) noexcept {
  return before.stable_publication_cut && after.stable_publication_cut &&
         before.reservation_entry_frontier ==
             after.reservation_entry_frontier &&
         before.reservation_completion_frontier ==
             after.reservation_completion_frontier &&
         before.claimed_slot_frontier == after.claimed_slot_frontier &&
         before.published_slot_frontier == after.published_slot_frontier &&
         before.events.size() == after.events.size();
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

/** @copydoc evaluate_m1_protocol */
M1ProtocolSummary evaluate_m1_protocol(M1ProtocolEvidenceInput input) {
  M1ProtocolSummary summary;
  const auto fail = [&summary](std::string reason) {
    if (std::find(summary.validity_reasons.begin(),
                  summary.validity_reasons.end(),
                  reason) == summary.validity_reasons.end()) {
      summary.validity_reasons.push_back(std::move(reason));
    }
  };

  try {
    const M1Timeline timeline =
        derive_m1_timeline(input.boundaries.measurement_start.timestamp);
    const M1EventCoordinate& cold = input.boundaries.cold_start;
    const M1EventCoordinate& warmup = input.boundaries.warmup_start;
    const M1EventCoordinate& measured = input.boundaries.measurement_start;
    const M1EventCoordinate& terminal = input.boundaries.measurement_end;
    if (cold.timestamp != timeline.cold_start ||
        warmup.timestamp != timeline.warmup_start ||
        measured.timestamp != timeline.measurement_start ||
        terminal.timestamp != timeline.measurement_end || !(cold < warmup) ||
        !(warmup < measured) || !(measured < terminal)) {
      fail("M1 boundary timestamps or total order drifted");
    }

    std::set<std::uint64_t> event_sequences;
    const auto register_event = [&event_sequences, &fail](
                                    const M1EventCoordinate& coordinate,
                                    std::string_view identity) {
      if (coordinate.event_sequence == 0U) {
        fail("M1 event has a zero row-local sequence: " +
             std::string(identity));
      } else if (!event_sequences.insert(coordinate.event_sequence).second) {
        fail("M1 event sequence is duplicated");
      }
    };
    register_event(cold, "cold-boundary");
    register_event(warmup, "warmup-boundary");
    register_event(measured, "measurement-boundary");
    register_event(terminal, "terminal-boundary");

    if (input.replicate_ordinal == 0U || input.replicate_ordinal > 3U) {
      fail("M1 replicate ordinal is outside [1,3]");
    }
    if (!input.shared_execution_domain || !input.boundary_was_zero_duration ||
        !input.raw_history_preserved || !input.warmup_sources_closed ||
        !input.measured_counters_reset || !input.final_settlement_proved) {
      fail("M1 shared-domain boundary or final settlement proof is incomplete");
    }

    const auto phase_known = [](B1JobPhase phase) noexcept {
      switch (phase) {
        case B1JobPhase::Cold:
        case B1JobPhase::Warmup:
        case B1JobPhase::Measured:
          return true;
      }
      return false;
    };
    const auto verdict_known = [](I1Verdict verdict) noexcept {
      switch (verdict) {
        case I1Verdict::Pass:
        case I1Verdict::Fail:
          return true;
        case I1Verdict::Invalid:
          return false;
      }
      return false;
    };
    const auto expected_i1_origin = [&timeline](B1JobPhase phase,
                                                std::size_t ordinal) {
      switch (phase) {
        case B1JobPhase::Cold:
          return timeline.cold_start;
        case B1JobPhase::Warmup:
          return checked_i1_time_add(
              timeline.warmup_start,
              std::chrono::nanoseconds(static_cast<std::int64_t>(ordinal) *
                                       kI1EpisodeStride.count()));
        case B1JobPhase::Measured:
          return checked_i1_time_add(
              timeline.measurement_start,
              std::chrono::nanoseconds(static_cast<std::int64_t>(ordinal) *
                                       kI1EpisodeStride.count()));
      }
      throw std::invalid_argument("M1 I1 occurrence phase is unknown");
    };

    if (input.interactive_occurrences.size() != kM1TotalI1OriginCount) {
      fail("M1 requires exactly 48 I1 origins");
    }
    const M1InteractiveOccurrenceEvidence* final_warmup = nullptr;
    for (std::size_t index = 0U; index < input.interactive_occurrences.size();
         ++index) {
      const M1InteractiveOccurrenceEvidence& occurrence =
          input.interactive_occurrences[index];
      B1JobPhase expected_phase = B1JobPhase::Measured;
      std::size_t expected_ordinal = 0U;
      if (index < kM1ColdI1OriginCount) {
        expected_phase = B1JobPhase::Cold;
        expected_ordinal = index;
      } else if (index < kM1ColdI1OriginCount + kM1WarmupI1OriginCount) {
        expected_phase = B1JobPhase::Warmup;
        expected_ordinal = index - kM1ColdI1OriginCount;
      } else {
        expected_ordinal =
            index - kM1ColdI1OriginCount - kM1WarmupI1OriginCount;
      }
      if (!phase_known(occurrence.phase) ||
          occurrence.phase != expected_phase ||
          occurrence.phase_ordinal != expected_ordinal ||
          occurrence.origin.timestamp !=
              expected_i1_origin(expected_phase, expected_ordinal)) {
        fail("M1 I1 phase, ordinal, order, or nominal origin drifted");
      }
      register_event(occurrence.origin, "I1-origin");
      const auto expected_settlement = checked_i1_time_add(
          occurrence.origin.timestamp, kI1MeasurementEndOffset);
      if (occurrence.settlement_endpoint != expected_settlement ||
          !occurrence.settlement_observed.has_value() ||
          expected_settlement < occurrence.settlement_observed->timestamp) {
        fail("M1 I1 occurrence lacks its fixed settlement cut proof");
      } else {
        register_event(*occurrence.settlement_observed, "I1-settlement");
      }
      if (!occurrence.phase_identity_immutable ||
          !verdict_known(occurrence.latency_verdict) ||
          !verdict_known(occurrence.waste_verdict) ||
          !verdict_known(occurrence.memory_verdict) ||
          !verdict_known(occurrence.output_verdict)) {
        fail("M1 I1 inner evidence is invalid or phase-rewritten");
      }
      const bool is_final_warmup =
          expected_phase == B1JobPhase::Warmup &&
          expected_ordinal + 1U == kM1WarmupI1OriginCount;
      if (is_final_warmup) {
        final_warmup = &occurrence;
        if (!occurrence.publication_current_at_measurement ||
            !occurrence.settlement_pending_at_measurement ||
            !occurrence.settlement_observed.has_value() ||
            !(measured < *occurrence.settlement_observed)) {
          fail("M1 final warmup I1 carryover/current state is invalid");
        }
      } else if (occurrence.publication_current_at_measurement ||
                 occurrence.settlement_pending_at_measurement) {
        fail("M1 non-final-warmup I1 contains carryover-only state");
      }
      if ((expected_phase == B1JobPhase::Cold &&
           (!occurrence.settlement_observed.has_value() ||
            !(*occurrence.settlement_observed < warmup))) ||
          (expected_phase == B1JobPhase::Warmup && !is_final_warmup &&
           (!occurrence.settlement_observed.has_value() ||
            !(*occurrence.settlement_observed < measured)))) {
        fail("M1 cold or early-warmup I1 settlement crossed its cutoff");
      }
      if (expected_phase == B1JobPhase::Measured &&
          (!occurrence.final_latency.has_value() ||
           occurrence.final_latency->count() < 0)) {
        fail("M1 measured I1 latency sample is missing or negative");
      }
    }

    const auto job_key = [](const B1JobInstance& job) {
      return std::string("b1:") + encode_b1_job_instance(job);
    };
    const auto i1_key = [](std::size_t ordinal) {
      return std::string("i1:warmup:") + std::to_string(ordinal);
    };
    const auto require_job = [&fail, &input](const M1BatchOfferEvidence& offer,
                                             B1JobPhase phase,
                                             std::uint64_t cycle,
                                             std::uint64_t job_index,
                                             std::uint64_t local_ordinal) {
      try {
        validate_b1_job_instance(offer.job);
      } catch (...) {
        fail("M1 B1 offer contains an invalid job instance");
        return;
      }
      if (offer.job.row_workload_id != kM1WorkloadId ||
          offer.job.replicate_ordinal != input.replicate_ordinal ||
          offer.job.phase != phase || offer.job.cycle_ordinal != cycle ||
          offer.job.job_index != job_index || offer.job.run_cap != 8U ||
          offer.producer_offer_ordinal != local_ordinal ||
          offer.attempt != 0U || !offer.phase_identity_immutable) {
        fail("M1 B1 offer identity, cycle, attempt, or local ordinal drifted");
      }
    };

    if (input.batch_offers.size() < 6U) {
      fail("M1 B1 offer stream lacks the frozen prefix and measured starts");
    }
    for (std::size_t index = 0U; index < input.batch_offers.size(); ++index) {
      const M1BatchOfferEvidence& offer = input.batch_offers[index];
      register_event(offer.offered, "B1-offer");
      if (index > 0U &&
          !(input.batch_offers[index - 1U].offered < offer.offered)) {
        fail("M1 B1 offer stream is not in strict row-local order");
      }
      if (!(offer.offered < terminal)) {
        fail("M1 B1 offer occurred at or after the terminal cutoff");
      }
      if (offer.endpoint.has_value()) {
        register_event(*offer.endpoint, "B1-endpoint");
        if (!(offer.offered < *offer.endpoint) || !offer.owner_settled) {
          fail("M1 B1 endpoint does not follow offer and owner settlement");
        }
      } else if (offer.owner_settled) {
        fail("M1 B1 owner settlement lacks a unique endpoint");
      }
    }

    if (input.batch_offers.size() >= 6U) {
      const M1BatchOfferEvidence& a252 = input.batch_offers[0U];
      const M1BatchOfferEvidence& b253 = input.batch_offers[1U];
      const M1BatchOfferEvidence& a254 = input.batch_offers[2U];
      const M1BatchOfferEvidence& b255 = input.batch_offers[3U];
      const M1BatchOfferEvidence& a0 = input.batch_offers[4U];
      const M1BatchOfferEvidence& b1 = input.batch_offers[5U];
      require_job(a252, B1JobPhase::Cold, 0U, 252U, 0U);
      require_job(b253, B1JobPhase::Warmup, 0U, 253U, 0U);
      require_job(a254, B1JobPhase::Warmup, 0U, 254U, 1U);
      require_job(b255, B1JobPhase::Warmup, 0U, 255U, 1U);
      require_job(a0, B1JobPhase::Measured, 0U, 0U, 2U);
      require_job(b1, B1JobPhase::Measured, 0U, 1U, 2U);
      if (a252.offered.timestamp != timeline.cold_start ||
          !(cold < a252.offered) || !a252.endpoint.has_value() ||
          !(*a252.endpoint < warmup) || !a252.output_removed ||
          a252.predecessor.has_value()) {
        fail("M1 cold A252 offer/settlement protocol drifted");
      }
      if (b253.offered.timestamp != timeline.warmup_start ||
          a254.offered.timestamp != timeline.warmup_start ||
          !(warmup < b253.offered) || !(b253.offered < a254.offered) ||
          b253.predecessor.has_value() || !a252.endpoint.has_value() ||
          !a254.predecessor.has_value() || !(*a254.predecessor == a252.job) ||
          !a254.predecessor_terminal.has_value() ||
          !(*a254.predecessor_terminal == *a252.endpoint)) {
        fail("M1 W boundary B253/A254 offer order or predecessors drifted");
      }
      if (!b253.endpoint.has_value() || !b255.predecessor.has_value() ||
          !(*b255.predecessor == b253.job) ||
          !b255.predecessor_terminal.has_value() ||
          !(*b255.predecessor_terminal == *b253.endpoint) ||
          !(*b253.endpoint < b255.offered) ||
          b253.endpoint->timestamp != b255.offered.timestamp ||
          !(b255.offered < measured)) {
        fail("M1 B255 was not synchronously derived from B253 before B");
      }
      if (a0.offered.timestamp != timeline.measurement_start ||
          b1.offered.timestamp != timeline.measurement_start ||
          !(measured < a0.offered) || !(a0.offered < b1.offered) ||
          !a0.predecessor.has_value() || !(*a0.predecessor == a254.job) ||
          !b1.predecessor.has_value() || !(*b1.predecessor == b255.job)) {
        fail("M1 measured A0/B1 boundary offers or FIFO predecessors drifted");
      }
    }

    std::vector<const M1BatchOfferEvidence*> graph_a_measured;
    std::vector<const M1BatchOfferEvidence*> graph_b_measured;
    for (const M1BatchOfferEvidence& offer : input.batch_offers) {
      if (offer.job.phase != B1JobPhase::Measured) {
        continue;
      }
      if ((offer.job.job_index & 1U) == 0U) {
        graph_a_measured.push_back(&offer);
      } else {
        graph_b_measured.push_back(&offer);
      }
    }
    const auto validate_producer = [&](const auto& offers,
                                       std::uint64_t parity) {
      if (offers.empty()) {
        fail("M1 measured Graph producer has no offered backlog");
        return;
      }
      for (std::size_t index = 0U; index < offers.size(); ++index) {
        const std::uint64_t cycle = index / 15U;
        const std::uint64_t job = 2U * (index % 15U) + parity;
        require_job(*offers[index], B1JobPhase::Measured, cycle, job,
                    static_cast<std::uint64_t>(index) + 2U);
        if (index == 0U) {
          continue;
        }
        const M1BatchOfferEvidence& prior = *offers[index - 1U];
        const M1BatchOfferEvidence& current = *offers[index];
        if (!prior.endpoint.has_value() || !current.predecessor.has_value() ||
            !(*current.predecessor == prior.job) ||
            !current.predecessor_terminal.has_value() ||
            !(*current.predecessor_terminal == *prior.endpoint) ||
            prior.endpoint->timestamp != current.offered.timestamp ||
            !(*prior.endpoint < current.offered)) {
          fail("M1 measured producer inserted a gap or crossed predecessors");
        }
      }
      const M1BatchOfferEvidence& last = *offers.back();
      if (last.endpoint.has_value() && *last.endpoint < terminal) {
        fail("M1 producer stopped despite a pre-cutoff terminal endpoint");
      }
    };
    validate_producer(graph_a_measured, 0U);
    validate_producer(graph_b_measured, 1U);

    std::map<std::string, std::pair<bool, std::string>> expected_carryover;
    if (final_warmup != nullptr) {
      expected_carryover.emplace(i1_key(final_warmup->phase_ordinal),
                                 std::make_pair(true, std::string{}));
    }
    for (const M1BatchOfferEvidence& offer : input.batch_offers) {
      if (offer.job.phase != B1JobPhase::Warmup ||
          (offer.endpoint.has_value() && *offer.endpoint < measured)) {
        continue;
      }
      if (!offer.fifo_position_preserved ||
          !offer.resource_authority_preserved) {
        fail("M1 warmup B1 carryover lost FIFO or resource authority");
      }
      std::string predecessor_key;
      if (offer.predecessor.has_value()) {
        const auto predecessor =
            std::find_if(input.batch_offers.begin(), input.batch_offers.end(),
                         [&offer](const M1BatchOfferEvidence& candidate) {
                           return candidate.job == *offer.predecessor;
                         });
        if (predecessor != input.batch_offers.end() &&
            (!predecessor->endpoint.has_value() ||
             !(*predecessor->endpoint < measured))) {
          predecessor_key = job_key(predecessor->job);
        }
      }
      expected_carryover.emplace(job_key(offer.job),
                                 std::make_pair(false, predecessor_key));
    }

    std::set<std::string> actual_carryover;
    for (const M1CarryoverEntry& entry : input.carryover) {
      const auto expected = expected_carryover.find(entry.occurrence_key);
      bool state_known = false;
      switch (entry.state) {
        case M1CarryoverState::OfferedWaiting:
        case M1CarryoverState::Accepted:
        case M1CarryoverState::Queued:
        case M1CarryoverState::Running:
          state_known = true;
          break;
        default:
          break;
      }
      if (!actual_carryover.insert(entry.occurrence_key).second ||
          expected == expected_carryover.end() || !state_known ||
          entry.phase != B1JobPhase::Warmup ||
          entry.queue_predecessor_key != expected->second.second ||
          !entry.resource_authority_preserved ||
          entry.publication_current != expected->second.first ||
          entry.owner_settled) {
        fail("M1 carryover snapshot is extra, duplicated, or contradictory");
      }
    }
    if (actual_carryover.size() != expected_carryover.size()) {
      fail("M1 carryover snapshot omits an incomplete warmup occurrence");
    }

    const M1FirstMeasuredAdmissionEvidence& admission =
        input.first_measured_admission;
    const auto latest_admission =
        checked_i1_time_add(timeline.measurement_start, kI1AdmissionLateness);
    if (admission.edit_index != 0U ||
        admission.nominal_time != timeline.measurement_start ||
        !admission.attempted ||
        admission.admission_sample < timeline.measurement_start ||
        latest_admission < admission.admission_sample ||
        !admission.reserved_event_sequence.has_value() ||
        *admission.reserved_event_sequence == 0U || !admission.host_succeeded ||
        !admission.accepted_coordinate.has_value() ||
        admission.accepted_coordinate->admission_time() !=
            admission.admission_sample ||
        admission.accepted_coordinate->event_sequence() !=
            *admission.reserved_event_sequence ||
        !admission.warmup_publication_current_before_acceptance ||
        !admission.superseded_exactly_at_acceptance ||
        admission.boundary_only_cancellation || final_warmup == nullptr ||
        admission.old_generation_settlement_endpoint !=
            checked_i1_time_add(timeline.measurement_start,
                                kI1MeasurementStartOffset)) {
      fail("M1 first measured edit/current-hold exception evidence is invalid");
    } else {
      const M1EventCoordinate accepted{
          admission.accepted_coordinate->admission_time(),
          admission.accepted_coordinate->event_sequence()};
      register_event(accepted, "first-measured-accepted");
      if (input.batch_offers.size() >= 6U &&
          admission.admission_sample == timeline.measurement_start &&
          !(input.batch_offers[5U].offered < accepted)) {
        fail("M1 equal-time first acceptance did not follow both B1 offers");
      }
      if (final_warmup != nullptr &&
          admission.old_generation_settlement_endpoint !=
              final_warmup->settlement_endpoint) {
        fail("M1 old generation Q_end was shifted at supersession");
      }
    }
  } catch (const std::exception& error) {
    fail(std::string("M1 protocol validation raised: ") + error.what());
  } catch (...) {
    fail("M1 protocol validation raised a non-standard exception");
  }

  if (summary.validity_reasons.empty()) {
    summary.verdict = I1Verdict::Pass;
  }
  return summary;
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
  if (input.observation_publication_unstable) {
    invalidate(&summary.validity_reasons,
               "mixed observation publication cut was not stable");
  }
  const bool observation_valid = !input.observation_overflowed &&
                                 !input.observation_sequence_exhausted &&
                                 !input.observation_qos_mismatch &&
                                 !input.observation_publication_unstable;

  bool progress_valid = observation_valid;
  std::vector<double> progress_ratios;
  if (input.progress_windows.size() != kM1MeasuredWindowCount ||
      !input.paired_isolated_b1.has_value() ||
      input.paired_isolated_b1->successful_site_operations == 0U ||
      input.paired_isolated_b1->duration.count() <= 0) {
    progress_valid = false;
    invalidate(&summary.validity_reasons,
               "M1 progress requires 30 windows and an exact paired source");
  } else {
    progress_ratios.reserve(input.progress_windows.size());
    for (std::size_t index = 0U; index < input.progress_windows.size();
         ++index) {
      const M1ThroughputProgressSample& window = input.progress_windows[index];
      if (window.window_ordinal != index ||
          window.duration != std::chrono::seconds(1)) {
        progress_valid = false;
        invalidate(&summary.validity_reasons,
                   "M1 progress contains an unordered or non-one-second raw "
                   "window");
        break;
      }
      const long double numerator =
          static_cast<long double>(window.successful_site_operations) *
          static_cast<long double>(input.paired_isolated_b1->duration.count());
      const long double denominator =
          static_cast<long double>(window.duration.count()) *
          static_cast<long double>(
              input.paired_isolated_b1->successful_site_operations);
      const double ratio = static_cast<double>(numerator / denominator);
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
  if (input.graph_service_windows.size() != kM1MeasuredWindowCount) {
    jain_valid = false;
    invalidate(&summary.validity_reasons,
               "M1 Graph service requires exactly 30 raw windows");
  }
  for (std::size_t index = 0U; index < input.graph_service_windows.size();
       ++index) {
    const M1GraphServiceWindow& window = input.graph_service_windows[index];
    if (window.window_ordinal != index) {
      jain_valid = false;
      invalidate(&summary.validity_reasons,
                 "M1 Graph service windows are not in exact ordinal order");
      continue;
    }
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
  std::uint64_t prior_start_sequence = 0U;
  for (const M1ClassStartSample& start : input.class_starts) {
    if (start.causal_sequence == 0U ||
        start.causal_sequence <= prior_start_sequence ||
        !start.execution_grant_committed) {
      class_start_valid = false;
      invalidate(
          &summary.validity_reasons,
          "M1 class-start evidence lacks a unique committed product cut");
      continue;
    }
    prior_start_sequence = start.causal_sequence;
    if (start.service_class != compute::ComputeRunQosClass::Interactive &&
        start.service_class != compute::ComputeRunQosClass::Throughput) {
      class_start_valid = false;
      invalidate(&summary.validity_reasons,
                 "M1 class-start evidence contains an unknown QoS class");
      continue;
    }
    if (!start.interactive_candidate_startable ||
        !start.throughput_candidate_startable) {
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
      default:
        class_start_valid = false;
        invalidate(&summary.validity_reasons,
                   "M1 class-start evidence contains an unknown QoS class");
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

  M1HeadroomAdmissionEvidence recomputed_admissions;
  bool raw_headroom_valid =
      input.headroom_outcomes.size() == kM1MeasuredI1AttemptCount;
  for (std::size_t index = 0U; index < input.headroom_outcomes.size();
       ++index) {
    const M1HeadroomAdmissionOutcome& outcome = input.headroom_outcomes[index];
    const std::size_t expected_origin = index / kI1EditCount;
    const std::size_t expected_edit = index % kI1EditCount;
    if (outcome.origin_ordinal != expected_origin ||
        outcome.edit_index != expected_edit) {
      raw_headroom_valid = false;
    }
    if (outcome.admission_attempted) {
      ++recomputed_admissions.attempted_edits;
    }
    if (outcome.host_status.has_value()) {
      ++recomputed_admissions.classified_outcomes;
    }
    if (outcome.admission_attempted != outcome.host_status.has_value() ||
        (outcome.host_status.has_value() &&
         (!valid_operation_error_domain(outcome.host_status->domain) ||
          outcome.throughput_headroom_failure == outcome.host_status->ok))) {
      raw_headroom_valid = false;
    }
    if (outcome.throughput_headroom_failure) {
      ++recomputed_admissions.throughput_headroom_failures;
    }
  }
  const M1HeadroomAdmissionEvidence& admissions = input.headroom_admissions;
  const bool headroom_valid =
      observation_valid && raw_headroom_valid &&
      admissions.attempted_edits == recomputed_admissions.attempted_edits &&
      admissions.classified_outcomes ==
          recomputed_admissions.classified_outcomes &&
      admissions.throughput_headroom_failures ==
          recomputed_admissions.throughput_headroom_failures &&
      admissions.attempted_edits == kM1MeasuredI1AttemptCount &&
      admissions.classified_outcomes == admissions.attempted_edits;
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
       summary.interactive_latency_verdict},
      &summary.validity_reasons);
  return summary;
}

/** @copydoc evaluate_m1_environment_pairs */
M1EnvironmentPairCompatibility evaluate_m1_environment_pairs(
    const B1EnvironmentEvidence& m1, const B1EnvironmentEvidence& isolated_i1,
    const B1EnvironmentEvidence& isolated_b1_cap_eight) noexcept {
  if (m1.workload_id != kM1WorkloadId || m1.run_cap != 8U ||
      isolated_i1.workload_id != kI1WorkloadId || isolated_i1.run_cap != 8U ||
      isolated_b1_cap_eight.workload_id != kB1WorkloadId ||
      isolated_b1_cap_eight.run_cap != 8U || m1.replicate_ordinal == 0U ||
      m1.replicate_ordinal != isolated_i1.replicate_ordinal ||
      m1.replicate_ordinal != isolated_b1_cap_eight.replicate_ordinal) {
    return {};
  }
  return M1EnvironmentPairCompatibility{
      compatible_b1_environments(m1, isolated_i1,
                                 B1EnvironmentRelation::M1PairedI1BaseOnly),
      compatible_b1_environments(m1, isolated_b1_cap_eight,
                                 B1EnvironmentRelation::M1PairedB1CapEight)};
}

}  // namespace ps::benchmark
