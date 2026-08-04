/**
 * @file i1_profile.hpp
 * @brief Declares the frozen I1 edit-storm admission and evidence primitives.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/i1_host.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/extension.hpp"
#include "photospider/data/value.hpp"

namespace ps::benchmark {

/** @brief Exact number of edits in one frozen I1 episode. */
inline constexpr std::size_t kI1EditCount = 12U;

/** @brief Exact number of cold, warmup, and measured I1 grid slots. */
inline constexpr std::size_t kI1GridSlotCount = 221U;

/** @brief Exact number of warmup I1 grid slots. */
inline constexpr std::size_t kI1WarmupSlotCount = 20U;

/** @brief Exact number of measured I1 grid slots. */
inline constexpr std::size_t kI1MeasuredSlotCount = 200U;

/** @brief Frozen square source-image edge in pixels. */
inline constexpr std::size_t kI1FrozenImageEdge = 2048U;

/** @brief Frozen Macro tile edge selected by every curve operation. */
inline constexpr std::size_t kI1FrozenCurveTileEdge = 256U;

/** @brief Exact number of tiled curve nodes in the frozen graph. */
inline constexpr std::size_t kI1FrozenCurveNodeCount = 4U;

/** @brief Exact monolithic source tasks in one complete frozen Run. */
inline constexpr std::size_t kI1FrozenSourceTaskCount = 1U;

// NOLINTBEGIN(whitespace/indent_namespace)
/** @brief Macro tiles populated for each frozen curve node. */
inline constexpr std::size_t kI1FrozenTilesPerCurveNode =
    ((kI1FrozenImageEdge + kI1FrozenCurveTileEdge - 1U) /
     kI1FrozenCurveTileEdge) *
    ((kI1FrozenImageEdge + kI1FrozenCurveTileEdge - 1U) /
     kI1FrozenCurveTileEdge);

/**
 * @brief Maximum physical service starts for one planned I1 task.
 * @note Initial-ready identities are deduplicated and dependency publication
 * occurs only on the one counter transition to zero. Pending-Value completion
 * resumes inside the task plan rather than creating another service start.
 */
inline constexpr std::size_t kI1MaximumServiceStartsPerTask = 1U;

/** @brief Maximum physical service starts in one complete frozen I1 Run. */
inline constexpr std::size_t kI1MaximumServiceStartsPerRun =
    (kI1FrozenSourceTaskCount +
     kI1FrozenCurveNodeCount * kI1FrozenTilesPerCurveNode) *
    kI1MaximumServiceStartsPerTask;

/**
 * @brief Lossless preallocated service-start capacity for one I1 episode.
 * @note The bound covers all twelve edits materializing complete frozen Runs;
 * timed product callbacks never grow storage.
 */
inline constexpr std::size_t kI1EpisodeServiceStartCapacity =
    kI1EditCount * kI1MaximumServiceStartsPerRun;
// NOLINTEND

/** @brief Exact frozen nominal spacing between I1 edit admissions. */
inline constexpr std::chrono::nanoseconds kI1EditStride{16666667};

/** @brief Inclusive maximum lateness for one I1 Host call. */
inline constexpr std::chrono::nanoseconds kI1AdmissionLateness{2000000};

/** @brief Exact monotonic budget anchored to the actual admission sample. */
inline constexpr std::chrono::nanoseconds kI1DeadlineBudget{150000000};

/** @brief Exact stride between consecutive isolated I1 episode origins. */
inline constexpr std::chrono::nanoseconds kI1EpisodeStride{750000000};

/** @brief Exact offset of the twelfth nominal marker and measurement start. */
inline constexpr std::chrono::nanoseconds kI1MeasurementStartOffset{183333337};

/** @brief Exact elapsed duration from inclusive `Q_start` to `Q_end`. */
inline constexpr std::chrono::nanoseconds kI1MeasurementDuration{500000000};

/** @brief Exact offset of the I1 causal history cut from episode origin. */
inline constexpr std::chrono::nanoseconds kI1MeasurementEndOffset{683333337};

/** @brief Exact guard from the I1 history cut to the next grid origin. */
inline constexpr std::chrono::nanoseconds kI1NextOriginGuard{66666663};

/** @brief Exact latest legal twelfth-edit deadline offset. */
inline constexpr std::chrono::nanoseconds kI1LatestFinalDeadlineOffset{
    335333337};  // NOLINT(whitespace/indent_namespace)

/** @brief Frozen node-one coefficient sequence indexed by edit_index. */
inline constexpr std::array<double, kI1EditCount> kI1EditCoefficients{
    0.82, 1.18, 0.86, 1.14, 0.90, 1.10,
    0.94, 1.06, 0.98, 1.02, 0.96, 1.04};  // NOLINT(whitespace/indent_namespace)

/** @brief Exact graph target node for every frozen I1 compute request. */
inline constexpr int kI1TargetNodeId = 4;

/**
 * @brief Monotonic clock callback injected into accepted-boundary collection.
 * @return Current point in the same steady-clock domain as Run deadlines.
 * @throws Implementations may throw; the collector propagates the exception
 * before creating a Host call or accepted coordinate.
 * @note Production uses `steady_clock::now`; injection exists for deterministic
 * arithmetic and failure tests only.
 */
using I1MonotonicClock = std::function<std::chrono::steady_clock::time_point()>;

/**
 * @brief Wait callback used to approach one immutable nominal admission time.
 * @param target Exact checked-derived nominal time.
 * @return Nothing after the caller chooses to resume.
 * @throws Implementations may propagate sleep/system/test failures.
 * @note Waking does not redefine the nominal time or permit a late backfill.
 */
using I1SleepUntil =
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    std::function<void(std::chrono::steady_clock::time_point target)>;

/**
 * @brief Phase identity derived solely from one isolated I1 grid slot.
 * @throws Nothing for value construction and comparison.
 */
enum class I1EpisodePhase : std::uint8_t {
  /** @brief Slot zero, excluded from aggregates. */
  Cold,
  /** @brief Slots one through twenty, excluded from measured aggregates. */
  Warmup,
  /** @brief Slots twenty-one through two hundred twenty. */
  Measured,
};

/**
 * @brief Frozen equal-time `Q_start` event vocabulary.
 * @throws Nothing for value construction and comparison.
 */
enum class I1MeasurementStartEventKind : std::uint8_t {
  /** @brief Nominal `Q_start=S_11` marker. */
  NominalMarker,
  /** @brief Actual accepted admission at the same timestamp. */
  AcceptedAdmission,
};

/**
 * @brief I1 name for the product-bound accepted-boundary coordinate type.
 * @throws std::invalid_argument when constructed with sequence zero.
 * @note This row-local coordinate is the pre-call `(A_i,event_sequence_i)`;
 * it is not the observation sink's independent causal coordinate.
 */
using I1AcceptedCoordinate = compute::AcceptedBoundaryCoordinate;

/**
 * @brief Raw status and timestamp observed when the final Host call returned.
 * @throws std::bad_alloc when status strings are copied.
 * @note These facts never define acceptance, currentness, or deadline order.
 */
struct I1HostReturnEvidence final {
  /** @brief Post-call monotonic sample retained only as raw evidence. */
  std::chrono::steady_clock::time_point return_time;

  /** @brief Exact scheduling status returned by the source-private Host call.
   */
  OperationStatus status;

  /** @brief Whether a successful status also carried a valid future. */
  bool future_valid = false;
};

/**
 * @brief Complete admission-boundary evidence for one frozen I1 edit.
 * @throws Nothing for movement after status/future ownership exists.
 * @note The future exists only after successful Host admission. A missing
 * accepted coordinate means no edit/current event may be synthesized.
 */
struct I1EditAdmissionResult final {
  /** @brief Zero-based frozen edit identity in `[0,11]`. */
  std::size_t edit_index = 0U;

  /** @brief Checked-derived immutable nominal Host-call start. */
  std::chrono::steady_clock::time_point nominal_time;

  /** @brief Sole monotonic sample taken at the attempted admission boundary. */
  std::chrono::steady_clock::time_point admission_sample;

  /** @brief Whether the sample lies in the inclusive two-millisecond window. */
  bool admission_window_valid = false;

  /** @brief Reserved row-local sequence, absent when no Host call is legal. */
  std::optional<std::uint64_t> reserved_event_sequence;

  /** @brief Checked absolute deadline, absent when no Host call is legal. */
  std::optional<std::chrono::steady_clock::time_point> deadline;

  /** @brief Raw Host return facts, absent when an invalid slot is not called.
   */
  std::optional<I1HostReturnEvidence> host_return;

  /** @brief Success-only normative coordinate, absent on every failure. */
  std::optional<I1AcceptedCoordinate> accepted_coordinate;

  /** @brief Product settlement future, valid only with accepted_coordinate. */
  std::future<OperationStatus> settlement;
};

/**
 * @brief One product callback start retained by the I1 observation sink.
 * @throws Nothing for value construction and copying.
 */
struct I1ObservedServiceStart final {
  /** @brief Edit whose private sink received this event. */
  std::size_t edit_index = 0U;
  /** @brief Opaque product Run identity. */
  std::uint64_t run_id = 0U;
  /** @brief Product generation carried by the immutable Run descriptor. */
  std::uint64_t generation = 0U;
  /** @brief Dense Run-local task identity. */
  std::uint64_t local_task_id = 0U;
  /** @brief Full HP product quality retained independently from intent/QoS. */
  compute::ComputeRunQuality quality = compute::ComputeRunQuality::Full;
  /** @brief Exact immutable scheduling inputs observed at physical start. */
  compute::ComputeRunQos qos;
  /** @brief Exact work plus 4096-byte ready quanta. */
  std::uint64_t service_charge = 0U;
  /** @brief Steady-clock sample reserved at service-start linearization. */
  std::chrono::steady_clock::time_point observed_at;
  /** @brief Collector-local causal order across every product callback. */
  std::uint64_t causal_sequence = 0U;
};

/**
 * @brief One product cancellation accepted by a materialized I1 Run.
 * @throws Nothing for value construction and copying.
 */
struct I1ObservedCancellation final {
  /** @brief Edit whose Run accepted cancellation. */
  std::size_t edit_index = 0U;
  /** @brief Opaque cancelled Run identity. */
  std::uint64_t run_id = 0U;
  /** @brief Product generation whose cancellation won arbitration. */
  std::uint64_t generation = 0U;
  /** @brief Stable product cancellation reason. */
  compute::ComputeRunCancellationReason reason =
      compute::ComputeRunCancellationReason::ExplicitRequest;
  /** @brief Steady-clock sample reserved at cancellation acceptance. */
  std::chrono::steady_clock::time_point observed_at;
  /** @brief Collector-local causal order. */
  std::uint64_t causal_sequence = 0U;
};

/**
 * @brief One exactly-once product Run terminal observation.
 * @throws Nothing for value construction and copying.
 */
struct I1ObservedTerminal final {
  /** @brief Edit whose Run reached terminal state. */
  std::size_t edit_index = 0U;
  /** @brief Opaque terminal Run identity. */
  std::uint64_t run_id = 0U;
  /** @brief Product generation that reached this terminal state. */
  std::uint64_t generation = 0U;
  /** @brief Exact terminal category. */
  compute::ComputeRunTerminalKind kind =
      compute::ComputeRunTerminalKind::Failed;
  /** @brief Steady-clock sample reserved at terminal publication. */
  std::chrono::steady_clock::time_point observed_at;
  /** @brief Collector-local causal order. */
  std::uint64_t causal_sequence = 0U;
};

/**
 * @brief Product current-generation observation for one admitted edit.
 * @throws Nothing for value construction and copying.
 */
struct I1ObservedCurrentGeneration final {
  /** @brief Edit whose product generation became current. */
  std::size_t edit_index = 0U;
  /** @brief Product-assigned nonzero generation. */
  std::uint64_t generation = 0U;
  /** @brief Steady-clock sample reserved at currentness publication. */
  std::chrono::steady_clock::time_point observed_at;
  /** @brief Collector-local causal order. */
  std::uint64_t causal_sequence = 0U;
  /** @brief Exact product identity binding to the pre-call row coordinate. */
  std::optional<I1AcceptedCoordinate> accepted_coordinate;
};

/**
 * @brief Current-visible HP publication and retained immutable output Value.
 * @throws Nothing for Value handle copying and value construction.
 */
struct I1ObservedVisibleOutput final {
  /** @brief Edit whose current contender published visibly. */
  std::size_t edit_index = 0U;
  /** @brief Opaque successful product Run identity. */
  std::uint64_t run_id = 0U;
  /** @brief Exact product generation that remained current. */
  std::uint64_t generation = 0U;
  /** @brief Steady-clock sample reserved at visible publication. */
  std::chrono::steady_clock::time_point observed_at;
  /** @brief Collector-local causal order. */
  std::uint64_t causal_sequence = 0U;
  /** @brief Immutable image Value published by the current HP snapshot. */
  Value output;
};

/**
 * @brief One Run quiescence or resource-settlement lifecycle transition.
 * @throws Nothing for value construction and copying.
 * @note The containing observation vector fixes which transition kind this
 * record represents; the record itself preserves the common Run join keys.
 */
struct I1ObservedRunLifecycleTransition final {
  /** @brief Edit whose materialized Run reached the transition. */
  std::size_t edit_index = 0U;
  /** @brief Opaque materialized Run identity. */
  std::uint64_t run_id = 0U;
  /** @brief Product generation of the settled Run. */
  std::uint64_t generation = 0U;
  /** @brief Steady-clock sample reserved at transition linearization. */
  std::chrono::steady_clock::time_point observed_at;
  /** @brief Collector-local causal order reserved at linearization. */
  std::uint64_t causal_sequence = 0U;
};

/**
 * @brief Caller-visible future publication plus Host tracking settlement.
 * @throws Nothing for value construction and copying.
 * @note The edit-scoped sink supplies identity even when supersession prevents
 * materialization of a concrete Run.
 */
struct I1ObservedHostSettlement final {
  /** @brief Edit whose Host request completed tracking publication. */
  std::size_t edit_index = 0U;
  /** @brief Steady-clock sample reserved after future/tracking publication. */
  std::chrono::steady_clock::time_point observed_at;
  /** @brief Collector-local causal order reserved after publication. */
  std::uint64_t causal_sequence = 0U;
};

/**
 * @brief Authoritative causal cut reserved for the immutable `Q_end` boundary.
 * @throws Nothing for value construction and copying.
 * @note An event belongs to the boundary history only when its product
 * coordinate is no later than `Q_end` and its sequence precedes this cut.
 */
struct I1ObservationHistoryCut final {
  /** @brief Actual steady-clock sample paired with cut reservation. */
  std::chrono::steady_clock::time_point captured_at;
  /** @brief First collector sequence excluded from boundary history. */
  std::uint64_t causal_sequence = 0U;
};

/**
 * @brief Stable copy of all bounded observation events for one I1 episode.
 * @throws std::bad_alloc when event vectors allocate.
 * @note `overflowed=true` invalidates evidence; no event is silently dropped.
 */
struct I1EpisodeObservationSnapshot final {
  /** @brief Accepted current-generation publications. */
  std::vector<I1ObservedCurrentGeneration> current_generations;
  /** @brief Physically committed callback service starts. */
  std::vector<I1ObservedServiceStart> service_starts;
  /** @brief Accepted Run cancellations. */
  std::vector<I1ObservedCancellation> cancellations;
  /** @brief Exactly-once Run terminal categories. */
  std::vector<I1ObservedTerminal> terminals;
  /** @brief Current-visible HP outputs. */
  std::vector<I1ObservedVisibleOutput> visible_outputs;
  /** @brief Physical Run quiescence transitions. */
  std::vector<I1ObservedRunLifecycleTransition> run_quiescences;
  /** @brief Exact Run root-resource return transitions. */
  std::vector<I1ObservedRunLifecycleTransition> resource_settlements;
  /** @brief Caller-visible future plus Host tracking settlements. */
  std::vector<I1ObservedHostSettlement> host_settlements;
  /** @brief True when any fixed observation capacity was exceeded. */
  bool overflowed = false;
};

/**
 * @brief Preallocated request-scoped implementation of the read-only Run sink.
 *
 * Each edit receives a distinct lightweight sink backed by one shared bounded
 * episode store. Product callbacks publish into fixed slots with atomics only;
 * snapshot allocation happens later on the harness thread.
 *
 * @throws std::bad_alloc when shared store or edit sink ownership is created.
 * @note The collector must outlive every edit settlement future. Snapshot is
 * race-safe, but a snapshot taken while work remains active may omit slots not
 * yet published and is therefore only a boundary observation, not settlement.
 */
class I1EpisodeObservationCollector final {
 public:
  /** @brief Creates one empty fixed-capacity episode observation store. */
  I1EpisodeObservationCollector();

  /** @brief Releases shared observation storage after all sinks are gone. */
  ~I1EpisodeObservationCollector() noexcept;

  /** @brief Prevents copying one episode-local causal sequence authority. */
  I1EpisodeObservationCollector(const I1EpisodeObservationCollector&) = delete;

  /** @brief Prevents duplicate assignment of episode-local observation state.
   */
  I1EpisodeObservationCollector& operator=(
      const I1EpisodeObservationCollector&) = delete;

  /**
   * @brief Creates the observation-only sink for one frozen edit.
   * @param edit_index Zero-based frozen edit identity in `[0,11]`.
   * @return Shared sink ready to attach to the private Host request.
   * @throws std::out_of_range for an invalid edit index.
   * @throws std::bad_alloc when sink ownership cannot allocate.
   */
  std::shared_ptr<compute::ComputeRunObservationSink> make_edit_sink(
      std::size_t edit_index);

  /**
   * @brief Copies every completely published bounded observation slot.
   * @return Stable event vectors and overflow status.
   * @throws std::bad_alloc when result vectors allocate.
   * @note Callers capture the `Q_end` history cut first and may copy published
   * events later; each coordinate still proves its own cut membership. This
   * method never waits or cancels work.
   */
  I1EpisodeObservationSnapshot snapshot() const;

  /**
   * @brief Reserves the first observation coordinate excluded at `Q_end`.
   * @return Authoritative cut sharing every edit sink's causal sequence.
   * @throws Nothing; sequence exhaustion is reflected by snapshot overflow.
   * @note The caller first waits until the immutable `Q_end` time. Events with
   * later timestamps remain outside the boundary even if scheduler lateness
   * lets their sequence precede this reservation.
   */
  I1ObservationHistoryCut capture_history_cut() noexcept;

  /**
   * @brief Returns completely published Host-settlement observation count.
   * @return Number of fixed slots release-published by status workers.
   * @throws Nothing.
   * @note The runner may poll this only after consuming all settlement futures;
   * product callbacks never wait for the poller.
   */
  std::size_t published_host_settlement_count() const noexcept;

 private:
  /** @brief Opaque shared fixed-capacity store defined in the implementation.
   */
  class Impl;

  /** @brief Shared store retained by per-edit product sinks. */
  std::shared_ptr<Impl> impl_;
};

/**
 * @brief Reusable success-only I1 Host admission collector.
 *
 * The collector owns the sole row-local logical event sequence, derives every
 * nominal/deadline with checked arithmetic, takes one pre-call sample, refuses
 * early/late backfill, and creates an accepted coordinate only from a
 * successful Host scheduling return carrying a valid future.
 *
 * @throws std::invalid_argument for empty clock/sleep callbacks or zero first
 * sequence.
 * @throws std::overflow_error for time/sequence exhaustion.
 * @throws Host and injected callback exceptions unchanged.
 * @note The collector owns no Run, cancellation source, graph, queue, ledger,
 * or commit authority. It can be reused by later mixed-profile orchestration.
 */
class I1AcceptedBoundaryCollector final {
 public:
  /**
   * @brief Binds collection to one source-private Host and injected time.
   * @param host Real embedded Host I1 capability.
   * @param clock Monotonic sample source.
   * @param sleep_until Waiter used only to approach immutable nominal times.
   * @param first_event_sequence Nonzero first row-local sequence to reserve.
   * @throws std::invalid_argument for empty callbacks or zero sequence.
   */
  I1AcceptedBoundaryCollector(I1Host& host, I1MonotonicClock clock,
                              I1SleepUntil sleep_until,
                              std::uint64_t first_event_sequence = 1U);

  /**
   * @brief Attempts one frozen edit at its checked-derived nominal boundary.
   * @param episode_origin Immutable episode origin `E`.
   * @param edit_index Zero-based frozen edit identity in `[0,11]`.
   * @param request Ordinary Host request with exact node mutation/Region.
   * @param observation_sink Distinct preallocated sink for this edit.
   * @return Move-only raw/accepted admission evidence and settlement future.
   * @throws std::out_of_range for an invalid edit index.
   * @throws std::invalid_argument for a null observer.
   * @throws std::overflow_error for checked time/sequence exhaustion.
   * @throws Injected clock/sleep or Host allocation/synchronization failures.
   * @note If the sole sample is early or more than two milliseconds late, no
   * Host call, sequence reservation, deadline, or accepted event is created;
   * later nominal times never shift or backfill.
   */
  I1EditAdmissionResult admit_edit(
      std::chrono::steady_clock::time_point episode_origin,
      std::size_t edit_index, HostComputeRequest request,
      std::shared_ptr<compute::ComputeRunObservationSink> observation_sink);

 private:
  /** @brief Real final Host admission capability borrowed from the runner. */
  I1Host& host_;

  /** @brief Injected monotonic source used exactly at admission/return samples.
   */
  I1MonotonicClock clock_;

  /** @brief Injected wait used without changing derived nominal values. */
  I1SleepUntil sleep_until_;

  /** @brief Next nonzero unique row-local event sequence. */
  std::uint64_t next_event_sequence_ = 1U;

  /** @brief True after UINT64_MAX was reserved exactly once. */
  bool event_sequence_exhausted_ = false;
};

/**
 * @brief Checked-adds an exact nanosecond offset to a steady time point.
 * @param origin Base monotonic time point.
 * @param offset Nonnegative exact nanosecond offset.
 * @return Exactly representable derived time point.
 * @throws std::invalid_argument for a negative offset.
 * @throws std::overflow_error when conversion or addition is not exact.
 */
std::chrono::steady_clock::time_point checked_i1_time_add(
    std::chrono::steady_clock::time_point origin,
    std::chrono::nanoseconds offset);

/**
 * @brief Derives one isolated I1 episode origin from the single grid origin.
 * @param grid_origin Immutable replicate origin `G^I1`.
 * @param slot Frozen slot index in `[0,220]`.
 * @return `G^I1 + slot * 750,000,000 ns` with checked arithmetic.
 * @throws std::out_of_range for an invalid slot.
 * @throws std::overflow_error on multiplication/conversion/addition failure.
 */
std::chrono::steady_clock::time_point i1_episode_origin(
    std::chrono::steady_clock::time_point grid_origin, std::size_t slot);

/**
 * @brief Derives the non-start terminal boundary of one isolated replicate.
 * @param grid_origin Immutable replicate origin `G^I1`.
 * @return `G^I1 + 221 * 750,000,000 ns` with checked arithmetic.
 * @throws std::overflow_error on multiplication/conversion/addition failure.
 */
std::chrono::steady_clock::time_point i1_terminal_boundary(
    std::chrono::steady_clock::time_point grid_origin);

/**
 * @brief Classifies one frozen isolated grid slot and phase-local index.
 * @param slot Frozen slot in `[0,220]`.
 * @return Phase plus zero-based ordinal within that phase.
 * @throws std::out_of_range for an invalid slot.
 */
std::pair<I1EpisodePhase, std::size_t> classify_i1_slot(std::size_t slot);

/**
 * @brief Returns the exact frozen source-space Region for one edit.
 * @param edit_index Zero-based frozen edit identity in `[0,11]`.
 * @return `(256*(i mod 4),256*floor(i/4),256,256)`.
 * @throws std::out_of_range for an invalid edit index.
 */
PixelRect i1_edit_region(std::size_t edit_index);

/**
 * @brief Returns the frozen tie rank at `Q_start=S_11`.
 * @param kind NominalMarker or AcceptedAdmission.
 * @return Zero for nominal marker and one for actual admission.
 * @throws std::invalid_argument for a non-`Q_start` event kind.
 */
int i1_measurement_start_tie_rank(I1MeasurementStartEventKind kind);

/**
 * @brief Builds the exact normative I1 source and four-transform graph YAML.
 * @return Complete five-node YAML document using the 2048 RGBA FP32
 * coordinate-pattern source and baseline coefficients `[0.80,1.00,1.20,1.40]`.
 * @throws std::bad_alloc when string ownership cannot allocate.
 * @note Node zero is the generated source; nodes one through four are serial
 * `curve_transform` nodes and node four is the sole target.
 */
std::string i1_frozen_graph_yaml();

/**
 * @brief Builds the exact node-one replacement YAML for one frozen edit.
 * @param edit_index Zero-based coefficient identity in `[0,11]`.
 * @return Complete node-one YAML preserving the source edge and exact
 * two-decimal coefficient spelling.
 * @throws std::out_of_range for an invalid edit index.
 * @throws std::bad_alloc when string ownership cannot allocate.
 */
std::string i1_edit_node_one_yaml(std::size_t edit_index);

/**
 * @brief Builds the ordinary Host portion of one exact I1 edit request.
 * @param session Loaded frozen graph session.
 * @param edit_index Zero-based frozen edit identity in `[0,11]`.
 * @return Node-four HP request with exact Region, no disk/cache output, and
 * parallel cap eight. The accepted-boundary collector supplies private QoS.
 * @throws std::out_of_range for an invalid edit index.
 * @throws std::bad_alloc when request string ownership cannot allocate.
 * @note The request itself grants no Run acceptance or scheduling authority.
 */
HostComputeRequest make_i1_host_compute_request(const GraphSessionId& session,
                                                std::size_t edit_index);

}  // namespace ps::benchmark
