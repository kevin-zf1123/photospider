/**
 * @file i2_profile.cpp
 * @brief Implements frozen I2 admission, grid, and observation primitives.
 */
#include "benchmark/i2/i2_profile.hpp"

#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace ps::benchmark {
namespace {

/** @brief Lossless duplicate/cancellation diagnostic slots per edit. */
constexpr std::size_t kI2CancellationsPerEditCapacity = 4U;

/** @brief Lossless terminal diagnostic slots per edit. */
constexpr std::size_t kI2TerminalsPerEditCapacity = 4U;

/** @brief Current-generation diagnostic slots per edit. */
constexpr std::size_t kI2CurrentGenerationsPerEditCapacity = 2U;

/** @brief Final-trigger diagnostic slots per edit. */
constexpr std::size_t kI2FinalTriggersPerEditCapacity = 2U;

/** @brief Visible-output diagnostic slots per edit. */
constexpr std::size_t kI2VisibleOutputsPerEditCapacity = 4U;

/**
 * @brief Harness-only terminal state for one published visible payload.
 * @throws Nothing for value construction and comparison.
 * @note This state is private implementation metadata and is absent from the
 * frozen inner evidence schema and installed ABI.
 */
enum class I2VisibleOutputFreezeState : std::uint8_t {
  /** @brief Payload may still be traversed or explicitly acquired. */
  Pending,
  /** @brief Digest and acquisition succeeded and all facts are sticky. */
  Frozen,
  /** @brief Incomplete evidence was retained while payload ownership ended. */
  ReleasedUnfrozen,
};

/** @brief Quiescence diagnostic slots per edit. */
constexpr std::size_t kI2RunQuiescencesPerEditCapacity = 4U;

/** @brief Root-resource settlement diagnostic slots per edit. */
constexpr std::size_t kI2ResourceSettlementsPerEditCapacity = 4U;

/** @brief Exactly one Host settlement belongs to each edit request. */
constexpr std::size_t kI2HostSettlementsPerEditCapacity = 1U;

/** @brief Frozen compile-time grid/capacity arithmetic. */
static_assert(kI2GridSlotCount ==
              1U + kI2WarmupSlotCount + kI2MeasuredSlotCount);

/** @brief Frozen compile-time final-deadline plus guard arithmetic. */
static_assert(kI2LatestFinalDeadlineOffset + kI2TerminalGuard ==
              kI2EpisodeStride);

/**
 * @brief Fixed callback-written slot with release/acquire publication.
 * @tparam Record Complete observation record.
 * @throws Nothing when Record default construction is nonthrowing.
 * @note One callback writes before release-publishing; the harness reads only
 * after acquire-observing `published`.
 */
template <typename Record>
struct PublishedI2ObservationSlot final {
  /** @brief True only after complete record publication. */
  std::atomic<bool> published{false};
  /** @brief Callback-owned record storage. */
  Record value;
};

/**
 * @brief Appends all completely published fixed slots.
 * @tparam Record Observation record type.
 * @tparam Capacity Fixed category capacity.
 * @param slots Callback-written source slots.
 * @param output Harness-owned destination vector.
 * @return Nothing after copying the current atomic cut.
 * @throws std::bad_alloc when vector ownership allocates.
 */
template <typename Record, std::size_t Capacity>
void append_i2_observations(
    const std::array<PublishedI2ObservationSlot<Record>, Capacity>& slots,
    std::vector<Record>* output) {
  output->reserve(Capacity);
  for (const PublishedI2ObservationSlot<Record>& slot : slots) {
    if (slot.published.load(std::memory_order_acquire)) {
      output->push_back(slot.value);
    }
  }
}

/**
 * @brief Copies all scalar identity/policy fields from one child descriptor.
 * @param edit_index Request-scoped frozen edit identity.
 * @param descriptor Immutable product child descriptor.
 * @return Authority-free complete scalar child record.
 * @throws Nothing.
 */
I2ObservedChildDescriptor observe_i2_child(
    std::size_t edit_index,
    const compute::ComputeRunDescriptor& descriptor) noexcept {
  return I2ObservedChildDescriptor{
      edit_index,
      descriptor.id().value(),
      descriptor.graph_instance_id().value(),
      descriptor.revision().value(),
      descriptor.target_node_id(),
      descriptor.intent(),
      descriptor.quality(),
      descriptor.qos(),
      descriptor.supersession().generation.value(),
      descriptor.supersession().key.request_intent(),
      descriptor.supersession().accepted_coordinate};
}

/**
 * @brief Checked-multiplies one nonnegative nanosecond stride by an index.
 * @param stride Nonnegative exact stride.
 * @param index Nonnegative multiplier.
 * @return Exact nanosecond product.
 * @throws std::invalid_argument for a negative stride.
 * @throws std::overflow_error when the product exceeds nanoseconds::rep.
 */
std::chrono::nanoseconds checked_i2_time_multiply(
    std::chrono::nanoseconds stride, std::size_t index) {
  if (stride.count() < 0) {
    throw std::invalid_argument("I2 time stride must be nonnegative.");
  }
  const __int128 product =
      static_cast<__int128>(stride.count()) * static_cast<__int128>(index);
  using Rep = std::chrono::nanoseconds::rep;
  if (product > static_cast<__int128>(std::numeric_limits<Rep>::max())) {
    throw std::overflow_error("I2 time multiplication overflowed.");
  }
  return std::chrono::nanoseconds(static_cast<Rep>(product));
}

}  // namespace

/**
 * @brief Shared fixed-capacity state behind I2 episode/edit observers.
 * @throws Nothing from product callbacks after construction.
 * @note Only harness snapshot/freeze paths allocate or enter explicit access.
 */
class I2EpisodeObservationCollector::Impl final {
 public:
  /**
   * @brief Edit-scoped no-authority Run observer backed by the shared store.
   * @throws Nothing from every callback and destructor.
   */
  class EditSink final : public compute::ComputeRunObservationSink {
   public:
    /**
     * @brief Binds one frozen edit to shared callback storage.
     * @param impl Shared store outliving every product callback.
     * @param edit_index Frozen zero-based edit identity.
     * @throws Nothing after shared-owner argument evaluation.
     */
    EditSink(std::shared_ptr<Impl> impl, std::size_t edit_index) noexcept
        : impl_(std::move(impl)), edit_index_(edit_index) {}

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
      impl_->publish(
          impl_->current_generations_, impl_->next_current_generation_,
          I1ObservedCurrentGeneration{
              edit_index_, identity.generation.value(), coordinate.observed_at,
              coordinate.causal_sequence, identity.accepted_coordinate});
    }

    /** @copydoc compute::ComputeRunObservationSink::on_service_start */
    void on_service_start(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunTaskIdentity task_identity,
        std::uint64_t service_charge,
        const compute::ComputeRunServiceStartObservation& observation,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      (void)observation;
      impl_->publish(impl_->service_starts_, impl_->next_service_start_,
                     I2ObservedServiceStart{
                         observe_i2_child(edit_index_, descriptor),
                         task_identity.local_task_id().value(), service_charge,
                         coordinate.observed_at, coordinate.causal_sequence});
    }

    /** @copydoc compute::ComputeRunObservationSink::on_cancellation */
    void on_cancellation(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunCancellationReason reason,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->publish(impl_->cancellations_, impl_->next_cancellation_,
                     I2ObservedCancellation{
                         observe_i2_child(edit_index_, descriptor), reason,
                         coordinate.observed_at, coordinate.causal_sequence});
    }

    /** @copydoc compute::ComputeRunObservationSink::on_terminal */
    void on_terminal(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunTerminalKind kind,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->publish(impl_->terminals_, impl_->next_terminal_,
                     I2ObservedTerminal{
                         observe_i2_child(edit_index_, descriptor), kind,
                         coordinate.observed_at, coordinate.causal_sequence});
    }

    /** @copydoc
     * compute::ComputeRunObservationSink::on_progressive_final_triggered */
    void on_progressive_final_triggered(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->publish(impl_->final_triggers_, impl_->next_final_trigger_,
                     I2ObservedFinalTrigger{
                         observe_i2_child(edit_index_, descriptor),
                         coordinate.observed_at, coordinate.causal_sequence});
    }

    /** @copydoc compute::ComputeRunObservationSink::on_current_visible */
    void on_current_visible(
        const compute::ComputeRunDescriptor& descriptor, Value output,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->publish(
          impl_->visible_outputs_, impl_->next_visible_output_,
          I2ObservedVisibleOutput{
              observe_i2_child(edit_index_, descriptor), coordinate.observed_at,
              coordinate.causal_sequence, std::move(output), false,
              std::nullopt, std::nullopt, ValueRevisionId{}, StorageBinding{},
              AllocationIdentity{}, 0U});
    }

    /** @copydoc compute::ComputeRunObservationSink::on_run_quiescent */
    void on_run_quiescent(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->publish(impl_->run_quiescences_, impl_->next_run_quiescence_,
                     I2ObservedRunLifecycleTransition{
                         observe_i2_child(edit_index_, descriptor),
                         coordinate.observed_at, coordinate.causal_sequence});
    }

    /** @copydoc compute::ComputeRunObservationSink::on_run_resource_settled */
    void on_run_resource_settled(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->publish(impl_->resource_settlements_,
                     impl_->next_resource_settlement_,
                     I2ObservedRunLifecycleTransition{
                         observe_i2_child(edit_index_, descriptor),
                         coordinate.observed_at, coordinate.causal_sequence});
    }

    /** @copydoc compute::ComputeRunObservationSink::on_host_settled */
    void on_host_settled(
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      if (impl_->publish(
              impl_->host_settlements_, impl_->next_host_settlement_,
              I1ObservedHostSettlement{edit_index_, coordinate.observed_at,
                                       coordinate.causal_sequence})) {
        impl_->published_host_settlement_count_.fetch_add(
            1U, std::memory_order_release);
      }
    }

   private:
    /** @brief Shared fixed store with no product authority. */
    std::shared_ptr<Impl> impl_;
    /** @brief Frozen edit identity attached before Host admission. */
    std::size_t edit_index_ = 0U;
  };

  /**
   * @brief Reserves one shared causal coordinate.
   * @return Current steady time and unique nonzero sequence when available.
   * @throws Nothing; exhaustion marks the collector overflowed.
   */
  compute::ComputeRunObservationCoordinate
  reserve_causal_coordinate() noexcept {
    const auto observed_at = std::chrono::steady_clock::now();
    const std::uint64_t sequence =
        next_causal_sequence_.fetch_add(1U, std::memory_order_relaxed);
    if (sequence == 0U ||
        sequence == std::numeric_limits<std::uint64_t>::max()) {
      overflowed_.store(true, std::memory_order_release);
    }
    return {observed_at, sequence};
  }

  /**
   * @brief Publishes one callback record into a unique fixed slot.
   * @tparam Record Complete observation record.
   * @tparam Capacity Fixed category capacity.
   * @param slots Category storage.
   * @param next Unique slot allocator.
   * @param record Complete callback-local record.
   * @return True after publication, false after capacity exhaustion.
   * @throws Nothing; exhaustion marks evidence invalid.
   */
  template <typename Record, std::size_t Capacity>
  bool publish(std::array<PublishedI2ObservationSlot<Record>, Capacity>& slots,
               std::atomic<std::size_t>& next, Record record) noexcept {
    const std::size_t index = next.fetch_add(1U, std::memory_order_relaxed);
    if (index >= Capacity) {
      overflowed_.store(true, std::memory_order_release);
      return false;
    }
    slots[index].value = std::move(record);
    slots[index].published.store(true, std::memory_order_release);
    return true;
  }

  /** @brief Nonzero shared causal sequence allocator. */
  std::atomic<std::uint64_t> next_causal_sequence_{1U};
  /** @brief Sticky losslessness failure. */
  std::atomic<bool> overflowed_{false};
  /** @brief Current-generation slot allocator. */
  std::atomic<std::size_t> next_current_generation_{0U};
  /** @brief Service-start slot allocator. */
  std::atomic<std::size_t> next_service_start_{0U};
  /** @brief Cancellation slot allocator. */
  std::atomic<std::size_t> next_cancellation_{0U};
  /** @brief Terminal slot allocator. */
  std::atomic<std::size_t> next_terminal_{0U};
  /** @brief Final-trigger slot allocator. */
  std::atomic<std::size_t> next_final_trigger_{0U};
  /** @brief Current-visible slot allocator. */
  std::atomic<std::size_t> next_visible_output_{0U};
  /** @brief Run-quiescence slot allocator. */
  std::atomic<std::size_t> next_run_quiescence_{0U};
  /** @brief Root-resource settlement slot allocator. */
  std::atomic<std::size_t> next_resource_settlement_{0U};
  /** @brief Host-settlement slot allocator. */
  std::atomic<std::size_t> next_host_settlement_{0U};
  /** @brief Completely release-published Host-settlement count. */
  std::atomic<std::size_t> published_host_settlement_count_{0U};

  /** @brief Fixed current-generation storage. */
  std::array<PublishedI2ObservationSlot<I1ObservedCurrentGeneration>,
             kI1EditCount * kI2CurrentGenerationsPerEditCapacity>
      current_generations_;
  /** @brief Fixed two-child physical-start storage. */
  std::array<PublishedI2ObservationSlot<I2ObservedServiceStart>,
             kI2EpisodeServiceStartCapacity>
      service_starts_;
  /** @brief Fixed cancellation storage. */
  std::array<PublishedI2ObservationSlot<I2ObservedCancellation>,
             kI1EditCount * kI2CancellationsPerEditCapacity>
      cancellations_;
  /** @brief Fixed terminal storage. */
  std::array<PublishedI2ObservationSlot<I2ObservedTerminal>,
             kI1EditCount * kI2TerminalsPerEditCapacity>
      terminals_;
  /** @brief Fixed final-trigger storage. */
  std::array<PublishedI2ObservationSlot<I2ObservedFinalTrigger>,
             kI1EditCount * kI2FinalTriggersPerEditCapacity>
      final_triggers_;
  /** @brief Fixed current-visible storage. */
  std::array<PublishedI2ObservationSlot<I2ObservedVisibleOutput>,
             kI1EditCount * kI2VisibleOutputsPerEditCapacity>
      visible_outputs_;
  /**
   * @brief Harness-only capture state parallel to `visible_outputs_`.
   * @note Collector API calls are externally serialized; product callbacks
   * publish a slot before the harness reads or mutates its corresponding
   * state, so this array needs no independent atomic protocol.
   */
  std::array<I2VisibleOutputFreezeState,
             kI1EditCount * kI2VisibleOutputsPerEditCapacity>
      visible_output_freeze_states_{};
  /** @brief Fixed physical-quiescence storage. */
  std::array<PublishedI2ObservationSlot<I2ObservedRunLifecycleTransition>,
             kI1EditCount * kI2RunQuiescencesPerEditCapacity>
      run_quiescences_;
  /** @brief Fixed root-resource settlement storage. */
  std::array<PublishedI2ObservationSlot<I2ObservedRunLifecycleTransition>,
             kI1EditCount * kI2ResourceSettlementsPerEditCapacity>
      resource_settlements_;
  /** @brief Fixed caller-visible Host-settlement storage. */
  std::array<PublishedI2ObservationSlot<I1ObservedHostSettlement>,
             kI1EditCount * kI2HostSettlementsPerEditCapacity>
      host_settlements_;
};

/** @copydoc I2EpisodeObservationCollector::I2EpisodeObservationCollector */
I2EpisodeObservationCollector::I2EpisodeObservationCollector()
    : I2EpisodeObservationCollector([] {
        return std::chrono::steady_clock::now();
      }) {}  // NOLINT(whitespace/indent_namespace)

/** @copydoc I2EpisodeObservationCollector::I2EpisodeObservationCollector */
I2EpisodeObservationCollector::I2EpisodeObservationCollector(
    I1MonotonicClock capture_clock)
    : impl_(std::make_shared<Impl>()),
      capture_clock_(std::move(capture_clock)) {
  if (!capture_clock_) {
    throw std::invalid_argument(
        "I2 observation collector capture clock must not be empty.");
  }
}

/** @copydoc I2EpisodeObservationCollector::~I2EpisodeObservationCollector */
I2EpisodeObservationCollector::~I2EpisodeObservationCollector() noexcept =
    default;  // NOLINT(whitespace/indent_namespace)

/** @copydoc I2EpisodeObservationCollector::make_edit_sink */
std::shared_ptr<compute::ComputeRunObservationSink>
I2EpisodeObservationCollector::make_edit_sink(std::size_t edit_index) {
  if (edit_index >= kI1EditCount) {
    throw std::out_of_range("I2 edit observer index is outside [0,11].");
  }
  return std::make_shared<Impl::EditSink>(impl_, edit_index);
}

/** @copydoc I2EpisodeObservationCollector::snapshot */
I2EpisodeObservationSnapshot I2EpisodeObservationCollector::snapshot() const {
  I2EpisodeObservationSnapshot result;
  append_i2_observations(impl_->current_generations_,
                         &result.current_generations);
  append_i2_observations(impl_->service_starts_, &result.service_starts);
  append_i2_observations(impl_->cancellations_, &result.cancellations);
  append_i2_observations(impl_->terminals_, &result.terminals);
  append_i2_observations(impl_->final_triggers_, &result.final_triggers);
  append_i2_observations(impl_->visible_outputs_, &result.visible_outputs);
  append_i2_observations(impl_->run_quiescences_, &result.run_quiescences);
  append_i2_observations(impl_->resource_settlements_,
                         &result.resource_settlements);
  append_i2_observations(impl_->host_settlements_, &result.host_settlements);
  result.overflowed = impl_->overflowed_.load(std::memory_order_acquire);
  return result;
}

/** @copydoc I2EpisodeObservationCollector::freeze_visible_outputs */
std::size_t I2EpisodeObservationCollector::freeze_visible_outputs(
    I2Host& host, std::chrono::steady_clock::time_point capture_deadline) {
  std::size_t published_count = 0U;
  for (std::size_t index = 0U; index < impl_->visible_outputs_.size();
       ++index) {
    PublishedI2ObservationSlot<I2ObservedVisibleOutput>& slot =
        impl_->visible_outputs_[index];
    if (!slot.published.load(std::memory_order_acquire)) {
      continue;
    }
    ++published_count;
    I2VisibleOutputFreezeState& freeze_state =
        impl_->visible_output_freeze_states_[index];
    if (freeze_state != I2VisibleOutputFreezeState::Pending) {
      continue;
    }
    if (capture_clock_() >= capture_deadline) {
      throw std::runtime_error(
          "I2 visible-output capture deadline expired before payload work.");
    }
    I2ObservedVisibleOutput& visible = slot.value;
    visible.value_valid_at_capture = visible.output.valid();
    if (visible.output.valid() && !visible.value_revision.valid()) {
      visible.value_revision = visible.output.revision_id();
      visible.value_binding = visible.output.storage_binding();
      visible.value_allocation = visible.output.allocation_identity();
      visible.value_storage_bytes = visible.output.storage_size();
    }
    if (!visible.content_digest.has_value()) {
      visible.content_digest = compute_content_digest(visible.output);
    }
    if (!visible.acquisition.has_value() && visible.output.valid()) {
      if (capture_clock_() >= capture_deadline) {
        throw std::runtime_error(
            "I2 visible-output capture deadline expired before Host "
            "acquisition.");
      }
      I2ValueAcquisitionEvidence acquisition = host.acquire_i2_value(
          visible.output,
          I2ValueLineage{visible.child.graph_instance_id,
                         visible.child.target_node_id,
                         visible.child.request_intent, visible.child.generation,
                         visible.child.run_id},
          capture_deadline);
      if (capture_clock_() >= capture_deadline) {
        throw std::runtime_error(
            "I2 visible-output capture deadline expired after Host "
            "acquisition.");
      }
      visible.acquisition.emplace(std::move(acquisition));
    }
    if (visible.content_digest.has_value() && visible.acquisition.has_value()) {
      visible.output = Value{};
      freeze_state = I2VisibleOutputFreezeState::Frozen;
    }
  }
  return published_count;
}

/** @copydoc I2EpisodeObservationCollector::release_unfrozen_visible_outputs */
void I2EpisodeObservationCollector::
    release_unfrozen_visible_outputs() noexcept {  // NOLINT(whitespace/indent_namespace)
  for (std::size_t index = 0U; index < impl_->visible_outputs_.size();
       ++index) {
    PublishedI2ObservationSlot<I2ObservedVisibleOutput>& slot =
        impl_->visible_outputs_[index];
    if (!slot.published.load(std::memory_order_acquire)) {
      continue;
    }
    I2VisibleOutputFreezeState& freeze_state =
        impl_->visible_output_freeze_states_[index];
    if (freeze_state != I2VisibleOutputFreezeState::Pending) {
      continue;
    }
    I2ObservedVisibleOutput& visible = slot.value;
    visible.value_valid_at_capture =
        visible.value_valid_at_capture || visible.output.valid();
    visible.output = Value{};
    freeze_state = I2VisibleOutputFreezeState::ReleasedUnfrozen;
  }
}

/** @copydoc I2EpisodeObservationCollector::capture_history_cut */
I1ObservationHistoryCut
I2EpisodeObservationCollector::capture_history_cut() noexcept {
  const compute::ComputeRunObservationCoordinate coordinate =
      impl_->reserve_causal_coordinate();
  return {coordinate.observed_at, coordinate.causal_sequence};
}

/** @copydoc I2EpisodeObservationCollector::published_host_settlement_count */
std::size_t I2EpisodeObservationCollector::published_host_settlement_count()
    const noexcept {  // NOLINT(whitespace/indent_namespace)
  return impl_->published_host_settlement_count_.load(
      std::memory_order_acquire);
}

/** @copydoc I2AcceptedBoundaryCollector::I2AcceptedBoundaryCollector */
I2AcceptedBoundaryCollector::I2AcceptedBoundaryCollector(
    I2Host& host, I1MonotonicClock clock, I1SleepUntil sleep_until,
    std::uint64_t first_event_sequence)
    : host_(host),
      clock_(std::move(clock)),
      sleep_until_(std::move(sleep_until)),
      next_event_sequence_(first_event_sequence) {
  if (!clock_ || !sleep_until_ || first_event_sequence == 0U) {
    throw std::invalid_argument(
        "I2 collector requires clock, sleeper, and nonzero sequence.");
  }
}

/** @copydoc I2AcceptedBoundaryCollector::admit_edit */
I2EditAdmissionResult I2AcceptedBoundaryCollector::admit_edit(
    std::chrono::steady_clock::time_point episode_origin,
    std::size_t edit_index, HostComputeRequest request,
    std::shared_ptr<compute::ComputeRunObservationSink> observation_sink) {
  if (edit_index >= kI1EditCount) {
    throw std::out_of_range("I2 edit index is outside [0,11].");
  }
  if (observation_sink == nullptr) {
    throw std::invalid_argument("I2 edit observation sink must be present.");
  }

  I2EditAdmissionResult result;
  result.edit_index = edit_index;
  result.nominal_time = checked_i1_time_add(
      episode_origin, checked_i2_time_multiply(kI1EditStride, edit_index));
  sleep_until_(result.nominal_time);
  result.admission_attempted = true;
  result.admission_sample = clock_();
  const auto latest_start =
      checked_i1_time_add(result.nominal_time, kI1AdmissionLateness);
  result.admission_window_valid =
      result.admission_sample >= result.nominal_time &&
      result.admission_sample <= latest_start;
  if (!result.admission_window_valid) {
    return result;
  }
  if (event_sequence_exhausted_) {
    throw std::overflow_error("I2 row-local event sequence exhausted.");
  }
  const std::uint64_t event_sequence = next_event_sequence_;
  if (next_event_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    event_sequence_exhausted_ = true;
  } else {
    ++next_event_sequence_;
  }
  result.reserved_event_sequence = event_sequence;
  result.preview_deadline =
      checked_i1_time_add(result.admission_sample, kI2PreviewDeadlineBudget);
  result.final_deadline =
      checked_i1_time_add(result.admission_sample, kI2FinalDeadlineBudget);
  const compute::AcceptedBoundaryCoordinate accepted_coordinate(
      result.admission_sample, event_sequence);

  request.intent = ComputeIntent::RealTimeUpdate;
  request.execution.parallel = true;
  request.execution.quiet = true;
  request.execution.maximum_parallelism = 8U;
  I2HostComputeRequest private_request{
      std::move(request),
      compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                             result.preview_deadline, 1U, 8U},
      compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                             result.final_deadline, 1U, 8U},
      std::move(observation_sink), accepted_coordinate};
  Result<std::future<OperationStatus>> host_result =
      host_.compute_i2_async(std::move(private_request));
  const auto return_time = clock_();
  const bool future_valid = host_result.value.valid();
  const bool accepted = host_result.status.ok && future_valid;
  result.host_return.emplace(I1HostReturnEvidence{
      return_time, std::move(host_result.status), future_valid});
  if (accepted) {
    result.accepted_coordinate = accepted_coordinate;
    result.settlement = std::move(host_result.value);
  }
  return result;
}

/** @copydoc i2_episode_origin */
std::chrono::steady_clock::time_point i2_episode_origin(
    std::chrono::steady_clock::time_point grid_origin, std::size_t slot) {
  if (slot >= kI2GridSlotCount) {
    throw std::out_of_range("I2 grid slot is outside [0,110].");
  }
  return checked_i1_time_add(grid_origin,
                             checked_i2_time_multiply(kI2EpisodeStride, slot));
}

/** @copydoc i2_terminal_boundary */
std::chrono::steady_clock::time_point i2_terminal_boundary(
    std::chrono::steady_clock::time_point grid_origin) {
  return checked_i1_time_add(
      grid_origin,
      checked_i2_time_multiply(kI2EpisodeStride, kI2GridSlotCount));
}

/** @copydoc classify_i2_slot */
std::pair<I2EpisodePhase, std::size_t> classify_i2_slot(std::size_t slot) {
  if (slot >= kI2GridSlotCount) {
    throw std::out_of_range("I2 grid slot is outside [0,110].");
  }
  if (slot == 0U) {
    return {I2EpisodePhase::Cold, 0U};
  }
  if (slot <= kI2WarmupSlotCount) {
    return {I2EpisodePhase::Warmup, slot - 1U};
  }
  return {I2EpisodePhase::Measured, slot - 1U - kI2WarmupSlotCount};
}

/** @copydoc i2_preview_region */
PixelRect i2_preview_region(std::size_t edit_index) {
  if (edit_index >= kI1EditCount) {
    throw std::out_of_range("I2 preview Region index is outside [0,11].");
  }
  return PixelRect{static_cast<int>(64U * (edit_index % 4U)),
                   static_cast<int>(64U * (edit_index / 4U)), 64, 64};
}

/** @copydoc i2_frozen_preview_content_digest */
ContentDigest i2_frozen_preview_content_digest() noexcept {
  return ContentDigest{
      CanonicalDigestAlgorithm::Sha256CanonicalV1,
      {std::byte{0x2a}, std::byte{0xf5}, std::byte{0xa5}, std::byte{0xb2},
       std::byte{0xe8}, std::byte{0x86}, std::byte{0x46}, std::byte{0xc5},
       std::byte{0x41}, std::byte{0xa6}, std::byte{0x0a}, std::byte{0x7b},
       std::byte{0x43}, std::byte{0x71}, std::byte{0x94}, std::byte{0xf1},
       std::byte{0x6d}, std::byte{0x1b}, std::byte{0xc2}, std::byte{0xc3},
       std::byte{0x4f}, std::byte{0xf2}, std::byte{0x0b}, std::byte{0xc5},
       std::byte{0x71}, std::byte{0xd3}, std::byte{0x7b}, std::byte{0xfd},
       std::byte{0x3c}, std::byte{0xac}, std::byte{0x3a}, std::byte{0xe2}}};
}

/** @copydoc make_i2_host_compute_request */
HostComputeRequest make_i2_host_compute_request(const GraphSessionId& session,
                                                std::size_t edit_index) {
  HostComputeRequest request =
      make_i1_host_compute_request(session, edit_index);
  request.intent = ComputeIntent::RealTimeUpdate;
  request.execution.quiet = true;
  return request;
}

}  // namespace ps::benchmark
