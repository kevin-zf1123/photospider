/**
 * @file i2_profile.hpp
 * @brief Declares the frozen I2 progressive admission and observation profile.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "benchmark/i1_profile.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/i2_host.hpp"     // NOLINT(build/include_subdir)

namespace ps::benchmark {

/** @brief Exact number of cold, warmup, and measured I2 episode slots. */
inline constexpr std::size_t kI2GridSlotCount = 111U;

/** @brief Exact number of warmup I2 episode slots. */
inline constexpr std::size_t kI2WarmupSlotCount = 10U;

/** @brief Exact number of measured I2 episode slots. */
inline constexpr std::size_t kI2MeasuredSlotCount = 100U;

/** @brief Exact stride between consecutive I2 episode origins. */
inline constexpr std::chrono::nanoseconds kI2EpisodeStride{1500000000};

/** @brief Exact preview deadline budget from the sole admission sample. */
inline constexpr std::chrono::nanoseconds kI2PreviewDeadlineBudget{100000000};

/** @brief Exact final deadline budget from the sole admission sample. */
inline constexpr std::chrono::nanoseconds kI2FinalDeadlineBudget{1000000000};

/** @brief Latest legal twelfth-final offset from one episode origin. */
inline constexpr std::chrono::nanoseconds kI2LatestFinalDeadlineOffset{
    1185333337};  // NOLINT(whitespace/indent_namespace)

/** @brief Exact guard after the latest legal final and before the next slot. */
inline constexpr std::chrono::nanoseconds kI2TerminalGuard{314666663};

/** @brief Frozen preview image edge in pixels. */
inline constexpr std::size_t kI2PreviewImageEdge = 512U;

/** @brief Frozen aligned source-to-preview scale factor. */
inline constexpr std::size_t kI2PreviewDownsampleFactor = 4U;

/**
 * @brief Maximum retained physical starts for both children of one episode.
 * @note The conservative lossless bound allows two complete I1-sized child
 * plans per edit; the actual 512 preview plan is smaller.
 */
// NOLINTBEGIN(whitespace/indent_namespace)
inline constexpr std::size_t kI2EpisodeServiceStartCapacity =
    kI1EditCount * 2U * kI1MaximumServiceStartsPerRun;
// NOLINTEND

/**
 * @brief Phase identity derived solely from one continuous I2 grid slot.
 * @throws Nothing for value construction and comparison.
 */
enum class I2EpisodePhase : std::uint8_t {
  /** @brief Slot zero, excluded from aggregates. */
  Cold,
  /** @brief Slots one through ten, excluded from aggregates. */
  Warmup,
  /** @brief Slots eleven through one hundred ten. */
  Measured,
};

/**
 * @brief Complete scalar descriptor copied from one I2 child Run.
 * @throws Nothing for construction and copying.
 * @note The record owns no Run, Graph, observer, scheduler, cancellation,
 * resource, currentness, payload, or native authority.
 */
struct I2ObservedChildDescriptor final {
  /** @brief Zero-based frozen edit identity. */
  std::size_t edit_index = 0U;
  /** @brief Nonzero product Run identity. */
  std::uint64_t run_id = 0U;
  /** @brief Nonzero exact live Graph identity. */
  std::uint64_t graph_instance_id = 0U;
  /** @brief Nonzero authoritative Graph revision. */
  std::uint64_t graph_revision = 0U;
  /** @brief Canonical target node. */
  int target_node_id = -1;
  /** @brief Child-domain intent. */
  ComputeIntent child_intent = ComputeIntent::GlobalHighPrecision;
  /** @brief Child output quality. */
  compute::ComputeRunQuality quality = compute::ComputeRunQuality::Full;
  /** @brief Complete immutable child scheduling policy. */
  compute::ComputeRunQos qos;
  /** @brief Shared request supersession generation. */
  std::uint64_t generation = 0U;
  /** @brief Shared canonical request intent. */
  ComputeIntent request_intent = ComputeIntent::RealTimeUpdate;
  /** @brief Shared success-only accepted coordinate. */
  std::optional<compute::AcceptedBoundaryCoordinate> accepted_coordinate;
};

/**
 * @brief One physically committed I2 child service start.
 * @throws Nothing for construction and copying.
 */
struct I2ObservedServiceStart final {
  /** @brief Exact child descriptor. */
  I2ObservedChildDescriptor child;
  /** @brief Dense Run-local task identity. */
  std::uint64_t local_task_id = 0U;
  /** @brief Exact work plus ready-byte service charge. */
  std::uint64_t service_charge = 0U;
  /** @brief Product steady-clock sample. */
  std::chrono::steady_clock::time_point observed_at;
  /** @brief Collector-local causal sequence. */
  std::uint64_t causal_sequence = 0U;
};

/**
 * @brief One accepted I2 child cancellation.
 * @throws Nothing for construction and copying.
 */
struct I2ObservedCancellation final {
  /** @brief Exact cancelled child descriptor. */
  I2ObservedChildDescriptor child;
  /** @brief Stable accepted cancellation reason. */
  compute::ComputeRunCancellationReason reason =
      compute::ComputeRunCancellationReason::ExplicitRequest;
  /** @brief Product steady-clock sample. */
  std::chrono::steady_clock::time_point observed_at;
  /** @brief Collector-local causal sequence. */
  std::uint64_t causal_sequence = 0U;
};

/**
 * @brief One exactly-once I2 child terminal observation.
 * @throws Nothing for construction and copying.
 */
struct I2ObservedTerminal final {
  /** @brief Exact terminal child descriptor. */
  I2ObservedChildDescriptor child;
  /** @brief Product terminal category. */
  compute::ComputeRunTerminalKind kind =
      compute::ComputeRunTerminalKind::Failed;
  /** @brief Product steady-clock sample. */
  std::chrono::steady_clock::time_point observed_at;
  /** @brief Collector-local causal sequence. */
  std::uint64_t causal_sequence = 0U;
};

/**
 * @brief One progressive final-trigger boundary.
 * @throws Nothing for construction and copying.
 */
struct I2ObservedFinalTrigger final {
  /** @brief Exact HP Full child about to be submitted. */
  I2ObservedChildDescriptor child;
  /** @brief Product steady-clock sample immediately before submission. */
  std::chrono::steady_clock::time_point observed_at;
  /** @brief Collector-local causal sequence. */
  std::uint64_t causal_sequence = 0U;
};

/**
 * @brief One current-visible I2 child Value plus frozen harness evidence.
 * @throws std::bad_alloc when digest/acquisition diagnostics allocate.
 * @note Product callbacks populate only descriptor, coordinate, and `output`.
 * The harness freezes digest and access evidence once, then releases Value.
 */
struct I2ObservedVisibleOutput final {
  /** @brief Exact visibly published child descriptor. */
  I2ObservedChildDescriptor child;
  /** @brief Product steady-clock sample. */
  std::chrono::steady_clock::time_point observed_at;
  /** @brief Collector-local causal sequence. */
  std::uint64_t causal_sequence = 0U;
  /** @brief Exact immutable current Value retained until harness capture. */
  Value output;
  /** @brief Whether output was valid at the sole harness capture. */
  bool value_valid_at_capture = false;
  /** @brief Sole typed canonical digest result. */
  std::optional<ContentDigestResult> content_digest;
  /** @brief Sole closed Host/conditional-Metal acquisition evidence. */
  std::optional<I2ValueAcquisitionEvidence> acquisition;
  /** @brief Exact Value revision copied before releasing payload ownership. */
  ValueRevisionId value_revision;
  /** @brief Exact storage binding copied before releasing payload ownership. */
  StorageBinding value_binding;
  /** @brief Exact allocation identity copied before releasing payload
   * ownership. */
  AllocationIdentity value_allocation;
  /** @brief Exact retained storage bytes copied before payload release. */
  std::size_t value_storage_bytes = 0U;
};

/**
 * @brief One I2 child quiescence or root-resource settlement transition.
 * @throws Nothing for construction and copying.
 */
struct I2ObservedRunLifecycleTransition final {
  /** @brief Exact settled child descriptor. */
  I2ObservedChildDescriptor child;
  /** @brief Product steady-clock sample. */
  std::chrono::steady_clock::time_point observed_at;
  /** @brief Collector-local causal sequence. */
  std::uint64_t causal_sequence = 0U;
};

/**
 * @brief Stable copy of all bounded product observations for one I2 episode.
 * @throws std::bad_alloc when event-vector copies allocate.
 * @note Overflow is sticky and invalidates evidence; no callback is silently
 * reclassified or inferred from later zero-resource state.
 */
struct I2EpisodeObservationSnapshot final {
  /** @brief Accepted request-generation publications. */
  std::vector<I1ObservedCurrentGeneration> current_generations;
  /** @brief Physically committed child service starts. */
  std::vector<I2ObservedServiceStart> service_starts;
  /** @brief Accepted child cancellations. */
  std::vector<I2ObservedCancellation> cancellations;
  /** @brief Exactly-once child terminals. */
  std::vector<I2ObservedTerminal> terminals;
  /** @brief Final-trigger boundaries. */
  std::vector<I2ObservedFinalTrigger> final_triggers;
  /** @brief Current-visible preview/final outputs. */
  std::vector<I2ObservedVisibleOutput> visible_outputs;
  /** @brief Physical child quiescence transitions. */
  std::vector<I2ObservedRunLifecycleTransition> run_quiescences;
  /** @brief Exact root-resource returns. */
  std::vector<I2ObservedRunLifecycleTransition> resource_settlements;
  /** @brief Caller-future plus Host-tracking settlements. */
  std::vector<I1ObservedHostSettlement> host_settlements;
  /** @brief Sticky fixed-capacity or causal-sequence exhaustion. */
  bool overflowed = false;
};

/**
 * @brief Fixed-capacity child-aware observation collector for one I2 episode.
 *
 * @throws std::bad_alloc when shared storage or edit-sink ownership allocates.
 * @note Product callbacks perform bounded atomic publication only. Snapshot,
 * digest traversal, Host/Metal acquisition, and Value release run later on the
 * single harness thread and own no product control authority.
 */
class I2EpisodeObservationCollector final {
 public:
  /** @brief Allocates one empty fixed-capacity episode store. */
  I2EpisodeObservationCollector();

  /** @brief Releases shared storage after every edit sink is gone. */
  ~I2EpisodeObservationCollector() noexcept;

  /** @brief Prevents duplicate causal-sequence ownership. */
  I2EpisodeObservationCollector(const I2EpisodeObservationCollector&) = delete;

  /** @brief Prevents duplicate assignment of observation ownership. */
  I2EpisodeObservationCollector& operator=(
      const I2EpisodeObservationCollector&) = delete;

  /**
   * @brief Creates one edit-scoped no-authority product sink.
   * @param edit_index Frozen zero-based edit identity in `[0,11]`.
   * @return Shared sink ready for one private I2 Host request.
   * @throws std::out_of_range for an invalid edit index.
   * @throws std::bad_alloc when shared sink ownership allocates.
   */
  std::shared_ptr<compute::ComputeRunObservationSink> make_edit_sink(
      std::size_t edit_index);

  /**
   * @brief Copies every completely release-published event.
   * @return Stable vectors and sticky overflow state.
   * @throws std::bad_alloc when vectors allocate.
   * @note The method never waits, hashes, acquires residency, or cancels work.
   */
  I2EpisodeObservationSnapshot snapshot() const;

  /**
   * @brief Freezes digest and closed access evidence for visible Values.
   * @param host Real embedded I2 capability used only for explicit access.
   * @return Number of completely published visible slots encountered.
   * @throws Digest, Value, Host, Metal, allocation, and synchronization
   * failures unchanged.
   * @note Each slot is traversed/acquired at most once; its Value is released
   * before successful return and later calls perform no payload work.
   */
  std::size_t freeze_visible_outputs(I2Host& host);

  /**
   * @brief Releases every visible Value not successfully frozen.
   * @return Nothing after all release-published Value handles are empty.
   * @throws Nothing.
   * @note Missing digest/acquisition evidence remains explicit and Invalid.
   */
  void release_unfrozen_visible_outputs() noexcept;

  /**
   * @brief Reserves the first causal sequence outside a frozen boundary.
   * @return Authoritative time/sequence cut from the shared observer source.
   * @throws Nothing; sequence exhaustion marks the collector overflowed.
   */
  I1ObservationHistoryCut capture_history_cut() noexcept;

  /**
   * @brief Returns completely published Host-settlement count.
   * @return Number of settled edit-scoped Host records.
   * @throws Nothing.
   */
  std::size_t published_host_settlement_count() const noexcept;

 private:
  /** @brief Opaque shared fixed-capacity implementation. */
  class Impl;

  /** @brief Shared store retained by every request-scoped sink. */
  std::shared_ptr<Impl> impl_;
};

/**
 * @brief Complete admission-boundary evidence for one frozen I2 edit.
 * @throws Nothing for movement after status/future ownership exists.
 * @note Preview/final deadlines share one sample and accepted coordinate.
 */
struct I2EditAdmissionResult final {
  /** @brief Zero-based edit identity. */
  std::size_t edit_index = 0U;
  /** @brief Checked-derived immutable nominal start. */
  std::chrono::steady_clock::time_point nominal_time;
  /** @brief Whether this position reached admission sampling. */
  bool admission_attempted = false;
  /** @brief Sole pre-Host-call monotonic sample. */
  std::chrono::steady_clock::time_point admission_sample;
  /** @brief Whether the sample lies in the inclusive lateness window. */
  bool admission_window_valid = false;
  /** @brief Reserved nonzero row-local sequence. */
  std::optional<std::uint64_t> reserved_event_sequence;
  /** @brief Exact `A_i+100 ms` preview deadline. */
  std::optional<std::chrono::steady_clock::time_point> preview_deadline;
  /** @brief Exact `A_i+1,000 ms` final deadline. */
  std::optional<std::chrono::steady_clock::time_point> final_deadline;
  /** @brief Raw post-call Host return facts. */
  std::optional<I1HostReturnEvidence> host_return;
  /** @brief Success-only accepted coordinate. */
  std::optional<compute::AcceptedBoundaryCoordinate> accepted_coordinate;
  /** @brief Complete progressive request settlement future. */
  std::future<OperationStatus> settlement;
};

/**
 * @brief Success-only accepted-boundary collector for frozen I2 edits.
 * @throws std::invalid_argument for empty callbacks or zero first sequence.
 * @throws std::overflow_error for checked time/sequence exhaustion.
 * @note The collector never waits for edits `0..10` to settle and creates no
 * accepted identity after a late sample or unsuccessful Host return.
 */
class I2AcceptedBoundaryCollector final {
 public:
  /**
   * @brief Binds collection to one real private Host and injected time.
   * @param host Borrowed embedded I2 Host capability.
   * @param clock Sole monotonic sampling source.
   * @param sleep_until Waiter used only to approach immutable nominal starts.
   * @param first_event_sequence Nonzero first row-local sequence.
   * @throws std::invalid_argument for empty callbacks or zero sequence.
   */
  I2AcceptedBoundaryCollector(I2Host& host, I1MonotonicClock clock,
                              I1SleepUntil sleep_until,
                              std::uint64_t first_event_sequence = 1U);

  /**
   * @brief Attempts one edit at its checked-derived frozen boundary.
   * @param episode_origin Immutable episode origin `E`.
   * @param edit_index Frozen zero-based edit identity.
   * @param request Ordinary Host request containing the source-space Region.
   * @param observation_sink Distinct preallocated sink for this edit.
   * @return Raw/success-only admission evidence and settlement future.
   * @throws std::out_of_range for an invalid edit index.
   * @throws std::invalid_argument for a null sink.
   * @throws Checked-time, sequence, Host, allocation, and synchronization
   * failures unchanged.
   */
  I2EditAdmissionResult admit_edit(
      std::chrono::steady_clock::time_point episode_origin,
      std::size_t edit_index, HostComputeRequest request,
      std::shared_ptr<compute::ComputeRunObservationSink> observation_sink);

 private:
  /** @brief Borrowed real I2 Host capability. */
  I2Host& host_;
  /** @brief Sole injected monotonic sampling source. */
  I1MonotonicClock clock_;
  /** @brief Injected nominal-boundary waiter. */
  I1SleepUntil sleep_until_;
  /** @brief Next nonzero unique row-local event sequence. */
  std::uint64_t next_event_sequence_ = 1U;
  /** @brief True after UINT64_MAX was reserved exactly once. */
  bool event_sequence_exhausted_ = false;
};

/**
 * @brief Derives one I2 episode origin from the sole replicate grid.
 * @param grid_origin Immutable `G^I2`.
 * @param slot Frozen slot in `[0,110]`.
 * @return `G^I2 + slot * 1,500,000,000 ns` with checked arithmetic.
 * @throws std::out_of_range for an invalid slot.
 * @throws std::overflow_error on multiplication or time addition overflow.
 */
std::chrono::steady_clock::time_point i2_episode_origin(
    std::chrono::steady_clock::time_point grid_origin, std::size_t slot);

/**
 * @brief Derives the non-start stride-111 terminal boundary.
 * @param grid_origin Immutable `G^I2`.
 * @return `G^I2 + 111 * 1,500,000,000 ns` with checked arithmetic.
 * @throws std::overflow_error on multiplication or time addition overflow.
 */
std::chrono::steady_clock::time_point i2_terminal_boundary(
    std::chrono::steady_clock::time_point grid_origin);

/**
 * @brief Classifies one frozen I2 slot and phase-local index.
 * @param slot Frozen slot in `[0,110]`.
 * @return Phase and zero-based index inside that phase.
 * @throws std::out_of_range for an invalid slot.
 */
std::pair<I2EpisodePhase, std::size_t> classify_i2_slot(std::size_t slot);

/**
 * @brief Returns the exact preview-space Region for one edit.
 * @param edit_index Frozen zero-based edit identity.
 * @return `(64*(i mod 4),64*floor(i/4),64,64)`.
 * @throws std::out_of_range for an invalid edit index.
 */
PixelRect i2_preview_region(std::size_t edit_index);

/**
 * @brief Returns the independently frozen edit-eleven preview digest.
 * @return Typed canonical-v1 SHA-256 digest for the exact preview contract.
 * @throws Nothing.
 */
ContentDigest i2_frozen_preview_content_digest() noexcept;

/**
 * @brief Builds the ordinary Host portion of one exact I2 edit request.
 * @param session Loaded frozen graph session.
 * @param edit_index Frozen zero-based edit identity.
 * @return Node-four RT request using I1 source Region, no I/O, and cap eight.
 * @throws I1 request construction failures unchanged.
 * @note The private collector adds child QoS, accepted coordinate, and gate.
 */
HostComputeRequest make_i2_host_compute_request(const GraphSessionId& session,
                                                std::size_t edit_index);

}  // namespace ps::benchmark
