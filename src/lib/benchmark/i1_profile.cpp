/**
 * @file i1_profile.cpp
 * @brief Implements frozen I1 checked time, admission, and observation logic.
 */
#include "benchmark/i1_profile.hpp"

#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ps::benchmark {
namespace {

/** @brief Freezes the derived lossless collector bounds for workload v1. */
static_assert(kI1FrozenTilesPerCurveNode == 64U);
static_assert(kI1MaximumServiceStartsPerRun == 257U);
static_assert(kI1EpisodeServiceStartCapacity == 3084U);

/** @brief Maximum retained cancellations per edit before evidence invalidates.
 */
constexpr std::size_t kCancellationsPerEditCapacity = 4U;

/** @brief Maximum retained terminals per edit before evidence invalidates. */
constexpr std::size_t kTerminalsPerEditCapacity = 4U;

/** @brief Maximum retained current publications per edit. */
constexpr std::size_t kCurrentGenerationsPerEditCapacity = 2U;

/** @brief Maximum retained visible outputs per edit. */
constexpr std::size_t kVisibleOutputsPerEditCapacity = 2U;

/** @brief Maximum retained quiescence transitions per edit. */
constexpr std::size_t kRunQuiescencesPerEditCapacity = 2U;

/** @brief Maximum retained resource-settlement transitions per edit. */
constexpr std::size_t kResourceSettlementsPerEditCapacity = 2U;

/** @brief Exactly one Host-settlement slot is reserved for each edit. */
constexpr std::size_t kHostSettlementsPerEditCapacity = 1U;

/** @brief Canonical two-decimal spellings of frozen edit coefficients. */
constexpr std::array<std::string_view, kI1EditCount>
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    kI1EditCoefficientText{"0.82", "1.18", "0.86", "1.14", "0.90",
                           "1.10", "0.94", "1.06", "0.98", "1.02",
                           "0.96", "1.04"};  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Fixed callback-written slot with release/acquire publication.
 * @tparam Record Complete observation record type.
 * @throws Nothing for default construction when Record is no-throw default
 * constructible.
 * @note Exactly one callback owns `value`; readers copy it only after
 * `published` becomes true with acquire ordering.
 */
template <typename Record>
struct PublishedObservationSlot final {
  /** @brief True only after the complete record has been written. */
  std::atomic<bool> published{false};

  /** @brief Callback-owned record storage read after release publication. */
  Record value;
};

/**
 * @brief Checked-multiplies one nonnegative nanosecond stride by an index.
 * @param stride Nonnegative exact nanosecond stride.
 * @param index Nonnegative scalar multiplier.
 * @return Exact nanosecond product.
 * @throws std::invalid_argument for a negative stride.
 * @throws std::overflow_error when the product exceeds nanoseconds::rep.
 */
std::chrono::nanoseconds checked_i1_time_multiply(
    std::chrono::nanoseconds stride, std::size_t index) {
  if (stride.count() < 0) {
    throw std::invalid_argument("I1 time stride must be nonnegative.");
  }
  const __int128 product =
      static_cast<__int128>(stride.count()) * static_cast<__int128>(index);
  using NanosecondsRep = std::chrono::nanoseconds::rep;
  if (product >
      static_cast<__int128>(std::numeric_limits<NanosecondsRep>::max())) {
    throw std::overflow_error("I1 time multiplication overflowed.");
  }
  return std::chrono::nanoseconds(static_cast<NanosecondsRep>(product));
}

/**
 * @brief Appends all release-published fixed slots to one result vector.
 * @tparam Record Observation record type.
 * @tparam Capacity Compile-time fixed slot count.
 * @param slots Callback-written source slots.
 * @param output Destination vector owned by the snapshot caller.
 * @return Nothing after all currently published slots are copied.
 * @throws std::bad_alloc when vector growth cannot allocate.
 * @note A concurrently reserved but incomplete slot remains absent from this
 * atomic-cut snapshot and may appear in a later call.
 */
template <typename Record, std::size_t Capacity>
void append_published_observations(
    const std::array<PublishedObservationSlot<Record>, Capacity>& slots,
    std::vector<Record>* output) {
  output->reserve(Capacity);
  for (const PublishedObservationSlot<Record>& slot : slots) {
    if (slot.published.load(std::memory_order_acquire)) {
      output->push_back(slot.value);
    }
  }
}

}  // namespace

/**
 * @brief Shared fixed-capacity implementation behind episode/edit observers.
 * @throws Nothing from product callbacks after construction.
 * @note Only the harness-thread snapshot path allocates.
 */
class I1EpisodeObservationCollector::Impl final {
 public:
  /**
   * @brief Per-edit no-authority Run observer backed by the shared store.
   * @throws Nothing from every callback and destructor.
   */
  class EditSink final : public compute::ComputeRunObservationSink {
   public:
    /**
     * @brief Binds one edit identity to shared fixed observation storage.
     * @param impl Shared store outliving all product callbacks.
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
          I1ObservedCurrentGeneration{edit_index_, identity.generation.value(),
                                      coordinate.observed_at,
                                      coordinate.causal_sequence});
    }

    /** @copydoc compute::ComputeRunObservationSink::on_service_start */
    void on_service_start(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunTaskIdentity task_identity,
        std::uint64_t service_charge,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->publish(impl_->service_starts_, impl_->next_service_start_,
                     I1ObservedServiceStart{
                         edit_index_, descriptor.id().value(),
                         descriptor.supersession().generation.value(),
                         task_identity.local_task_id().value(),
                         descriptor.quality(), descriptor.qos(), service_charge,
                         coordinate.observed_at, coordinate.causal_sequence});
    }

    /** @copydoc compute::ComputeRunObservationSink::on_cancellation */
    void on_cancellation(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunCancellationReason reason,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->publish(impl_->cancellations_, impl_->next_cancellation_,
                     I1ObservedCancellation{
                         edit_index_, descriptor.id().value(),
                         descriptor.supersession().generation.value(), reason,
                         coordinate.observed_at, coordinate.causal_sequence});
    }

    /** @copydoc compute::ComputeRunObservationSink::on_terminal */
    void on_terminal(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunTerminalKind kind,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->publish(impl_->terminals_, impl_->next_terminal_,
                     I1ObservedTerminal{
                         edit_index_, descriptor.id().value(),
                         descriptor.supersession().generation.value(), kind,
                         coordinate.observed_at, coordinate.causal_sequence});
    }

    /** @copydoc compute::ComputeRunObservationSink::on_current_visible */
    void on_current_visible(
        const compute::ComputeRunDescriptor& descriptor, Value output,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->publish(impl_->visible_outputs_, impl_->next_visible_output_,
                     I1ObservedVisibleOutput{
                         edit_index_, descriptor.id().value(),
                         descriptor.supersession().generation.value(),
                         coordinate.observed_at, coordinate.causal_sequence,
                         std::move(output)});
    }

    /** @copydoc compute::ComputeRunObservationSink::on_run_quiescent */
    void on_run_quiescent(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->publish(impl_->run_quiescences_, impl_->next_run_quiescence_,
                     I1ObservedRunLifecycleTransition{
                         edit_index_, descriptor.id().value(),
                         descriptor.supersession().generation.value(),
                         coordinate.observed_at, coordinate.causal_sequence});
    }

    /** @copydoc compute::ComputeRunObservationSink::on_run_resource_settled */
    void on_run_resource_settled(
        const compute::ComputeRunDescriptor& descriptor,
        compute::ComputeRunObservationCoordinate coordinate) noexcept override {
      impl_->publish(impl_->resource_settlements_,
                     impl_->next_resource_settlement_,
                     I1ObservedRunLifecycleTransition{
                         edit_index_, descriptor.id().value(),
                         descriptor.supersession().generation.value(),
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
    /** @brief Shared fixed store; owns no product execution state. */
    std::shared_ptr<Impl> impl_;

    /** @brief Frozen edit identity attached before final Host admission. */
    std::size_t edit_index_ = 0U;
  };

  /**
   * @brief Reserves one collector-local causal coordinate at linearization.
   * @return Current time/sequence, including UINT64_MAX on its sole legal use.
   * @throws Nothing; exhaustion marks evidence overflowed and later callbacks
   * remain non-authoritative diagnostics.
   */
  compute::ComputeRunObservationCoordinate
  reserve_causal_coordinate() noexcept {
    const std::chrono::steady_clock::time_point observed_at =
        std::chrono::steady_clock::now();
    const std::uint64_t sequence =
        next_causal_sequence_.fetch_add(1U, std::memory_order_relaxed);
    if (sequence == 0U ||
        sequence == std::numeric_limits<std::uint64_t>::max()) {
      overflowed_.store(true, std::memory_order_release);
    }
    return compute::ComputeRunObservationCoordinate{observed_at, sequence};
  }

  /**
   * @brief Publishes one callback record into a unique fixed slot.
   * @tparam Record Observation record type.
   * @tparam Capacity Fixed store capacity for that event category.
   * @param slots Complete category storage.
   * @param next Unique category-local slot allocator.
   * @param record Complete callback-local record to publish.
   * @return True after publication, false after capacity exhaustion.
   * @throws Nothing; capacity exhaustion marks evidence invalid.
   */
  template <typename Record, std::size_t Capacity>
  bool publish(std::array<PublishedObservationSlot<Record>, Capacity>& slots,
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

  /** @brief Nonzero collector-local causal sequence allocator. */
  std::atomic<std::uint64_t> next_causal_sequence_{1U};

  /** @brief Sticky losslessness failure flag. */
  std::atomic<bool> overflowed_{false};

  /** @brief Unique current-generation slot allocator. */
  std::atomic<std::size_t> next_current_generation_{0U};

  /** @brief Unique service-start slot allocator. */
  std::atomic<std::size_t> next_service_start_{0U};

  /** @brief Unique cancellation slot allocator. */
  std::atomic<std::size_t> next_cancellation_{0U};

  /** @brief Unique terminal slot allocator. */
  std::atomic<std::size_t> next_terminal_{0U};

  /** @brief Unique current-visible slot allocator. */
  std::atomic<std::size_t> next_visible_output_{0U};

  /** @brief Unique physical-quiescence slot allocator. */
  std::atomic<std::size_t> next_run_quiescence_{0U};

  /** @brief Unique resource-settlement slot allocator. */
  std::atomic<std::size_t> next_resource_settlement_{0U};

  /** @brief Unique Host-settlement slot allocator. */
  std::atomic<std::size_t> next_host_settlement_{0U};

  /** @brief Completely release-published Host-settlement record count. */
  std::atomic<std::size_t> published_host_settlement_count_{0U};

  /** @brief Fixed current-generation storage. */
  std::array<PublishedObservationSlot<I1ObservedCurrentGeneration>,
             kI1EditCount * kCurrentGenerationsPerEditCapacity>
      current_generations_;

  /** @brief Fixed physical service-start storage. */
  std::array<PublishedObservationSlot<I1ObservedServiceStart>,
             kI1EpisodeServiceStartCapacity>
      service_starts_;

  /** @brief Fixed accepted-cancellation storage. */
  std::array<PublishedObservationSlot<I1ObservedCancellation>,
             kI1EditCount * kCancellationsPerEditCapacity>
      cancellations_;

  /** @brief Fixed exactly-once terminal storage. */
  std::array<PublishedObservationSlot<I1ObservedTerminal>,
             kI1EditCount * kTerminalsPerEditCapacity>
      terminals_;

  /** @brief Fixed current-visible output storage. */
  std::array<PublishedObservationSlot<I1ObservedVisibleOutput>,
             kI1EditCount * kVisibleOutputsPerEditCapacity>
      visible_outputs_;

  /** @brief Fixed physical Run-quiescence storage. */
  std::array<PublishedObservationSlot<I1ObservedRunLifecycleTransition>,
             kI1EditCount * kRunQuiescencesPerEditCapacity>
      run_quiescences_;

  /** @brief Fixed exact root-resource settlement storage. */
  std::array<PublishedObservationSlot<I1ObservedRunLifecycleTransition>,
             kI1EditCount * kResourceSettlementsPerEditCapacity>
      resource_settlements_;

  /** @brief Fixed caller-visible Host-settlement storage. */
  std::array<PublishedObservationSlot<I1ObservedHostSettlement>,
             kI1EditCount * kHostSettlementsPerEditCapacity>
      host_settlements_;
};

/** @copydoc I1EpisodeObservationCollector::I1EpisodeObservationCollector */
I1EpisodeObservationCollector::I1EpisodeObservationCollector()
    : impl_(std::make_shared<Impl>()) {}

/** @copydoc I1EpisodeObservationCollector::~I1EpisodeObservationCollector */
I1EpisodeObservationCollector::~I1EpisodeObservationCollector() noexcept =
    default;  // NOLINT(whitespace/indent_namespace)

/** @copydoc I1EpisodeObservationCollector::make_edit_sink */
std::shared_ptr<compute::ComputeRunObservationSink>
I1EpisodeObservationCollector::make_edit_sink(std::size_t edit_index) {
  if (edit_index >= kI1EditCount) {
    throw std::out_of_range("I1 edit observer index is outside [0,11].");
  }
  return std::make_shared<Impl::EditSink>(impl_, edit_index);
}

/** @copydoc I1EpisodeObservationCollector::snapshot */
I1EpisodeObservationSnapshot I1EpisodeObservationCollector::snapshot() const {
  I1EpisodeObservationSnapshot result;
  append_published_observations(impl_->current_generations_,
                                &result.current_generations);
  append_published_observations(impl_->service_starts_, &result.service_starts);
  append_published_observations(impl_->cancellations_, &result.cancellations);
  append_published_observations(impl_->terminals_, &result.terminals);
  append_published_observations(impl_->visible_outputs_,
                                &result.visible_outputs);
  append_published_observations(impl_->run_quiescences_,
                                &result.run_quiescences);
  append_published_observations(impl_->resource_settlements_,
                                &result.resource_settlements);
  append_published_observations(impl_->host_settlements_,
                                &result.host_settlements);
  result.overflowed = impl_->overflowed_.load(std::memory_order_acquire);
  return result;
}

/** @copydoc I1EpisodeObservationCollector::capture_history_cut */
I1ObservationHistoryCut
I1EpisodeObservationCollector::capture_history_cut() noexcept {
  const compute::ComputeRunObservationCoordinate coordinate =
      impl_->reserve_causal_coordinate();
  return I1ObservationHistoryCut{coordinate.observed_at,
                                 coordinate.causal_sequence};
}

/** @copydoc I1EpisodeObservationCollector::published_host_settlement_count */
std::size_t I1EpisodeObservationCollector::published_host_settlement_count()
    const noexcept {  // NOLINT(whitespace/indent_namespace)
  return impl_->published_host_settlement_count_.load(
      std::memory_order_acquire);
}

/** @copydoc I1AcceptedBoundaryCollector::I1AcceptedBoundaryCollector */
I1AcceptedBoundaryCollector::I1AcceptedBoundaryCollector(
    I1Host& host, I1MonotonicClock clock, I1SleepUntil sleep_until,
    std::uint64_t first_event_sequence)
    : host_(host),
      clock_(std::move(clock)),
      sleep_until_(std::move(sleep_until)),
      next_event_sequence_(first_event_sequence) {
  if (!clock_ || !sleep_until_ || first_event_sequence == 0U) {
    throw std::invalid_argument(
        "I1 collector requires clock, sleeper, and nonzero sequence.");
  }
}

/** @copydoc I1AcceptedBoundaryCollector::admit_edit */
I1EditAdmissionResult I1AcceptedBoundaryCollector::admit_edit(
    std::chrono::steady_clock::time_point episode_origin,
    std::size_t edit_index, HostComputeRequest request,
    std::shared_ptr<compute::ComputeRunObservationSink> observation_sink) {
  if (edit_index >= kI1EditCount) {
    throw std::out_of_range("I1 edit index is outside [0,11].");
  }
  if (observation_sink == nullptr) {
    throw std::invalid_argument("I1 edit observation sink must be present.");
  }

  I1EditAdmissionResult result;
  result.edit_index = edit_index;
  result.nominal_time = checked_i1_time_add(
      episode_origin, checked_i1_time_multiply(kI1EditStride, edit_index));
  sleep_until_(result.nominal_time);
  result.admission_sample = clock_();
  const std::chrono::steady_clock::time_point latest_start =
      checked_i1_time_add(result.nominal_time, kI1AdmissionLateness);
  result.admission_window_valid =
      result.admission_sample >= result.nominal_time &&
      result.admission_sample <= latest_start;
  if (!result.admission_window_valid) {
    return result;
  }

  if (event_sequence_exhausted_) {
    throw std::overflow_error("I1 row-local event sequence exhausted.");
  }
  const std::uint64_t event_sequence = next_event_sequence_;
  if (next_event_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    event_sequence_exhausted_ = true;
  } else {
    ++next_event_sequence_;
  }
  result.reserved_event_sequence = event_sequence;
  result.deadline =
      checked_i1_time_add(result.admission_sample, kI1DeadlineBudget);

  request.intent = ComputeIntent::GlobalHighPrecision;
  request.execution.parallel = true;
  request.execution.maximum_parallelism = 8U;
  benchmark::I1HostComputeRequest private_request{
      std::move(request),
      compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                             result.deadline, 1U, 8U},
      std::move(observation_sink)};
  Result<std::future<OperationStatus>> host_result =
      host_.compute_i1_async(std::move(private_request));
  const std::chrono::steady_clock::time_point return_time = clock_();
  const bool future_valid = host_result.value.valid();
  const bool accepted = host_result.status.ok && future_valid;
  result.host_return.emplace(I1HostReturnEvidence{
      return_time, std::move(host_result.status), future_valid});
  if (accepted) {
    result.accepted_coordinate =
        I1AcceptedCoordinate{result.admission_sample, event_sequence};
    result.settlement = std::move(host_result.value);
  }
  return result;
}

/** @copydoc checked_i1_time_add */
std::chrono::steady_clock::time_point checked_i1_time_add(
    std::chrono::steady_clock::time_point origin,
    std::chrono::nanoseconds offset) {
  if (offset.count() < 0) {
    throw std::invalid_argument("I1 time offset must be nonnegative.");
  }
  using ClockDuration = std::chrono::steady_clock::duration;
  using ClockRep = ClockDuration::rep;
  static_assert(std::is_integral_v<ClockRep>,
                "I1 checked time requires an integral steady-clock rep");
  const ClockDuration converted =
      std::chrono::duration_cast<ClockDuration>(offset);
  if (std::chrono::duration_cast<std::chrono::nanoseconds>(converted) !=
      offset) {
    throw std::overflow_error(
        "I1 nanosecond offset is not exactly representable by steady_clock.");
  }
  const __int128 sum =
      static_cast<__int128>(origin.time_since_epoch().count()) +
      static_cast<__int128>(converted.count());
  if (sum < static_cast<__int128>(std::numeric_limits<ClockRep>::lowest()) ||
      sum > static_cast<__int128>(std::numeric_limits<ClockRep>::max())) {
    throw std::overflow_error("I1 steady-clock addition overflowed.");
  }
  return std::chrono::steady_clock::time_point(
      ClockDuration(static_cast<ClockRep>(sum)));
}

/** @copydoc i1_episode_origin */
std::chrono::steady_clock::time_point i1_episode_origin(
    std::chrono::steady_clock::time_point grid_origin, std::size_t slot) {
  if (slot >= kI1GridSlotCount) {
    throw std::out_of_range("I1 grid slot is outside [0,220].");
  }
  return checked_i1_time_add(grid_origin,
                             checked_i1_time_multiply(kI1EpisodeStride, slot));
}

/** @copydoc i1_terminal_boundary */
std::chrono::steady_clock::time_point i1_terminal_boundary(
    std::chrono::steady_clock::time_point grid_origin) {
  return checked_i1_time_add(
      grid_origin,
      checked_i1_time_multiply(kI1EpisodeStride, kI1GridSlotCount));
}

/** @copydoc classify_i1_slot */
std::pair<I1EpisodePhase, std::size_t> classify_i1_slot(std::size_t slot) {
  if (slot >= kI1GridSlotCount) {
    throw std::out_of_range("I1 grid slot is outside [0,220].");
  }
  if (slot == 0U) {
    return {I1EpisodePhase::Cold, 0U};
  }
  if (slot <= kI1WarmupSlotCount) {
    return {I1EpisodePhase::Warmup, slot - 1U};
  }
  return {I1EpisodePhase::Measured, slot - 1U - kI1WarmupSlotCount};
}

/** @copydoc i1_edit_region */
PixelRect i1_edit_region(std::size_t edit_index) {
  if (edit_index >= kI1EditCount) {
    throw std::out_of_range("I1 edit Region index is outside [0,11].");
  }
  return PixelRect{static_cast<int>(256U * (edit_index % 4U)),
                   static_cast<int>(256U * (edit_index / 4U)), 256, 256};
}

/** @copydoc i1_measurement_start_tie_rank */
int i1_measurement_start_tie_rank(I1MeasurementStartEventKind kind) {
  switch (kind) {
    case I1MeasurementStartEventKind::NominalMarker:
      return 0;
    case I1MeasurementStartEventKind::AcceptedAdmission:
      return 1;
  }
  throw std::invalid_argument("Event kind is not valid at I1 Q_start.");
}

/** @copydoc i1_frozen_graph_yaml */
std::string i1_frozen_graph_yaml() {
  return R"YAML(- id: 0
  name: i1_coordinate_pattern
  type: image_generator
  subtype: coordinate_pattern
  parameters:
    width: 2048
    height: 2048
    channels: 4
    seed: 0
- id: 1
  name: i1_curve_one
  type: image_process
  subtype: curve_transform
  image_inputs:
    - from_node_id: 0
  parameters:
    k: 0.80
- id: 2
  name: i1_curve_two
  type: image_process
  subtype: curve_transform
  image_inputs:
    - from_node_id: 1
  parameters:
    k: 1.00
- id: 3
  name: i1_curve_three
  type: image_process
  subtype: curve_transform
  image_inputs:
    - from_node_id: 2
  parameters:
    k: 1.20
- id: 4
  name: i1_curve_four
  type: image_process
  subtype: curve_transform
  image_inputs:
    - from_node_id: 3
  parameters:
    k: 1.40
)YAML";
}

/** @copydoc i1_edit_node_one_yaml */
std::string i1_edit_node_one_yaml(std::size_t edit_index) {
  if (edit_index >= kI1EditCount) {
    throw std::out_of_range("I1 edit coefficient index is outside [0,11].");
  }
  std::ostringstream output;
  output << "id: 1\n"
         << "name: i1_curve_one\n"
         << "type: image_process\n"
         << "subtype: curve_transform\n"
         << "image_inputs:\n"
         << "  - from_node_id: 0\n"
         << "parameters:\n"
         << "  k: " << kI1EditCoefficientText[edit_index] << '\n';
  return output.str();
}

/** @copydoc make_i1_host_compute_request */
HostComputeRequest make_i1_host_compute_request(const GraphSessionId& session,
                                                std::size_t edit_index) {
  HostComputeRequest request;
  request.session = session;
  request.node = NodeId{kI1TargetNodeId};
  request.cache.precision = "fp32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  request.execution.parallel = true;
  request.execution.quiet = true;
  request.execution.maximum_parallelism = 8U;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.dirty_roi = i1_edit_region(edit_index);
  return request;
}

}  // namespace ps::benchmark
