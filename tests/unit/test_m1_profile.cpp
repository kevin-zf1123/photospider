/**
 * @file test_m1_profile.cpp
 * @brief Verifies deterministic M1 timeline, fairness, and observer contracts.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "benchmark/m1_evidence.hpp"         // NOLINT(build/include_subdir)
#include "benchmark/m1_profile.hpp"          // NOLINT(build/include_subdir)
#include "benchmark/observation_fanout.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/value.hpp"
#include "support/b1_test_environment.hpp"

namespace ps::benchmark {
namespace {

/**
 * @brief Pauses one deterministic observer callback after slot claim.
 * @throws Nothing for construction, destruction, or callback dispatch.
 * @note This deliberately blocking hook is test-only and must never be wired
 * into a production benchmark collector.
 */
class PausingPublicationHook final : public M1ObservationPublicationHook {
 public:
  /** @copydoc M1ObservationPublicationHook::after_slot_claim */
  void after_slot_claim() noexcept override {
    claimed_.store(true, std::memory_order_release);
    while (!released_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

  /**
   * @brief Tests whether the callback reached the post-claim hook.
   * @return True after the callback has claimed its unique slot.
   * @throws Nothing.
   */
  bool claimed() const noexcept {
    return claimed_.load(std::memory_order_acquire);
  }

  /**
   * @brief Releases the paused callback to publish and complete.
   * @return Nothing.
   * @throws Nothing.
   */
  void release() noexcept { released_.store(true, std::memory_order_release); }

 private:
  /** @brief True after the callback claims its slot. */
  std::atomic<bool> claimed_{false};

  /** @brief True when the callback may finish publication. */
  std::atomic<bool> released_{false};
};

/**
 * @brief Builds one complete passing inner-fairness input.
 * @return Exact 30-window evidence with every independent guard passing.
 * @throws std::bad_alloc when vector storage allocates.
 */
M1FairnessEvidenceInput make_passing_fairness_input() {
  M1FairnessEvidenceInput input;
  input.paired_isolated_b1 =
      M1PairedB1RateEvidence{1000000U, std::chrono::seconds(1)};
  for (std::size_t window = 0U; window < kM1MeasuredWindowCount; ++window) {
    input.progress_windows.push_back(
        M1ThroughputProgressSample{window, 200000U, std::chrono::seconds(1)});
    input.graph_service_windows.push_back(
        M1GraphServiceWindow{window, true, 100U, 100U});
  }
  for (std::size_t group = 0U; group < 3U; ++group) {
    input.class_starts.push_back(
        M1ClassStartSample{group + 1U, compute::ComputeRunQosClass::Interactive,
                           true, true, true});
  }
  input.class_starts.push_back(M1ClassStartSample{
      4U, compute::ComputeRunQosClass::Throughput, true, true, true});
  input.headroom_admissions = M1HeadroomAdmissionEvidence{
      kM1MeasuredI1AttemptCount, kM1MeasuredI1AttemptCount, 0U};
  for (std::size_t origin = 0U; origin < kM1MeasuredI1OriginCount; ++origin) {
    for (std::size_t edit = 0U; edit < kI1EditCount; ++edit) {
      input.headroom_outcomes.push_back(M1HeadroomAdmissionOutcome{
          origin, edit, true, OperationStatus{}, false});
    }
  }
  input.interactive_latency_verdict = I1Verdict::Pass;
  return input;
}

/**
 * @brief Builds one valid Run submission for direct observer-boundary tests.
 * @param graph_identity Unique Graph label copied into the Run descriptor.
 * @param graph_instance_id Nonzero Graph instance/revision test identity.
 * @param service_class Actual immutable scheduling class to observe.
 * @return Complete full-quality high-precision submission without a sink.
 * @throws std::bad_alloc when Graph identity ownership allocates.
 */
compute::ComputeRunSubmission make_observer_submission(
    std::string graph_identity, std::uint64_t graph_instance_id,
    compute::ComputeRunQosClass service_class) {
  return compute::ComputeRunSubmission{
      std::move(graph_identity),
      GraphInstanceId{graph_instance_id},
      GraphRevision{graph_instance_id},
      4,
      ComputeIntent::GlobalHighPrecision,
      compute::ComputeRunQuality::Full,
      compute::ComputeRunQos{service_class, std::nullopt, 1U, 1U},
      compute::SupersessionIdentity{
          compute::SupersessionKey(4, ComputeIntent::GlobalHighPrecision),
          compute::SupersessionGeneration(1U)},
      nullptr};
}

/**
 * @brief Fixed-storage spy for same-coordinate fanout boundary tests.
 *
 * Every callback records only scalar counts and the last coordinate, keeping
 * the `noexcept` observation boundary allocation-free and nonblocking.
 *
 * @throws Nothing after construction.
 */
class RecordingObservationSink final
    : public compute::ComputeRunObservationSink {
 public:
  /**
   * @brief Binds the coordinate returned by this spy's allocator.
   * @param coordinate Immutable reservation result.
   * @param task_semantics Whether this child requests task callbacks.
   * @throws Nothing.
   */
  RecordingObservationSink(compute::ComputeRunObservationCoordinate coordinate,
                           bool task_semantics) noexcept
      : coordinate_(coordinate), task_semantics_(task_semantics) {}

  /** @copydoc compute::ComputeRunObservationSink::reserve_causal_coordinate */
  compute::ComputeRunObservationCoordinate reserve_causal_coordinate() noexcept
      override {
    ++reservation_count;
    return coordinate_;
  }

  /** @copydoc compute::ComputeRunObservationSink::on_current_generation */
  void on_current_generation(
      const compute::SupersessionIdentity& identity,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(identity);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::observes_task_semantics */
  bool observes_task_semantics() const noexcept override {
    return task_semantics_;
  }

  /** @copydoc compute::ComputeRunObservationSink::on_task_ready */
  void on_task_ready(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTaskIdentity task_identity,
      const compute::ComputeRunTaskReadyObservation& observation,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(descriptor);
    static_cast<void>(task_identity);
    static_cast<void>(observation);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_service_start */
  void on_service_start(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTaskIdentity task_identity,
      std::uint64_t service_charge,
      const compute::ComputeRunServiceStartObservation& observation,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(descriptor);
    static_cast<void>(task_identity);
    static_cast<void>(service_charge);
    static_cast<void>(observation);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_task_terminal */
  void on_task_terminal(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTaskIdentity task_identity,
      compute::ComputeRunTaskTerminalKind kind,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(descriptor);
    static_cast<void>(task_identity);
    static_cast<void>(kind);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_cancellation */
  void on_cancellation(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunCancellationReason reason,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(descriptor);
    static_cast<void>(reason);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_terminal */
  void on_terminal(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTerminalKind kind,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(descriptor);
    static_cast<void>(kind);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_current_visible */
  void on_current_visible(
      const compute::ComputeRunDescriptor& descriptor, Value output,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(descriptor);
    static_cast<void>(output);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_run_quiescent */
  void on_run_quiescent(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(descriptor);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_run_resource_settled */
  void on_run_resource_settled(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(descriptor);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_host_settled */
  void on_host_settled(
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    record(coordinate);
  }

  /** @brief Number of times this child was asked to allocate a coordinate. */
  std::uint64_t reservation_count = 0U;

  /** @brief Number of callbacks forwarded to this child. */
  std::uint64_t callback_count = 0U;

  /** @brief Last exact coordinate received through a callback. */
  compute::ComputeRunObservationCoordinate last_coordinate;

 private:
  /**
   * @brief Records one scalar callback without allocation or blocking.
   * @param coordinate Exact coordinate supplied by the fanout.
   * @return Nothing.
   * @throws Nothing.
   */
  void record(compute::ComputeRunObservationCoordinate coordinate) noexcept {
    ++callback_count;
    last_coordinate = coordinate;
  }

  /** @brief Fixed coordinate returned by the spy allocator. */
  compute::ComputeRunObservationCoordinate coordinate_;

  /** @brief Fixed task-semantics opt-in used by composition tests. */
  bool task_semantics_ = false;
};

/**
 * @brief Recasts one eligible required-storage M1 environment as its I1 peer.
 * @param m1 Same-subject, same-ordinal M1 environment.
 * @return Base-identical storage-N/A isolated I1 environment evidence.
 * @throws Canonical encoding and allocation failures unchanged.
 */
B1EnvironmentEvidence make_isolated_i1_peer(B1EnvironmentEvidence m1) {
  m1.workload_id = kI1WorkloadId;
  m1.storage_manifest.reset();
  m1.claimed_storage_digest.reset();
  m1.storage_raw_proof.reset();
  m1.storage_eligibility.reset();
  m1.storage_actual_observation.reset();
  const std::vector<B1CanonicalField> environment_class{
      testing::known_b1_field("base_environment_digest", "sha256",
                              b1_digest_hex(m1.claimed_base_digest)),
      testing::known_b1_field("storage_environment_applicability", "enum",
                              "not-applicable"),
      testing::known_b1_field("storage_environment_not_applicable_reason",
                              "enum", "row-has-no-output-commit"),
      testing::not_applicable_b1_field("storage_environment_digest", "sha256",
                                       "row-has-no-output-commit")};
  m1.environment_class_manifest =
      encode_b1_environment_class(environment_class);
  m1.claimed_environment_class_digest =
      digest_b1_environment_manifest(m1.environment_class_manifest);
  return m1;
}

/**
 * @brief Builds one complete same-ordinal M1/I1/B1 environment triple.
 * @param ordinal Replicate ordinal in `[1,3]`.
 * @return M1 required-storage side, base-only I1 side, and full B1 cap-eight.
 * @throws Canonical fixture and allocation failures unchanged.
 */
std::array<B1EnvironmentEvidence, 3U> make_environment_triple(
    std::uint64_t ordinal) {
  B1EnvironmentEvidence m1 = testing::make_b1_test_environment(8U, ordinal);
  B1EnvironmentEvidence b1 = m1;
  m1.workload_id = kM1WorkloadId;
  return {m1, make_isolated_i1_peer(m1), b1};
}

/**
 * @brief Builds one exact deterministic M1 phase/offer/carryover protocol.
 * @return Structurally passing evidence in which Graph A independently enters
 * local cycle one while Graph B remains in cycle zero.
 * @throws Checked time, job identity, and allocation failures unchanged.
 */
M1ProtocolEvidenceInput make_passing_protocol() {
  const auto b =
      std::chrono::steady_clock::time_point(std::chrono::seconds(100));
  const M1Timeline timeline = derive_m1_timeline(b);
  const auto coordinate = [](std::chrono::steady_clock::time_point timestamp,
                             std::uint64_t sequence) {
    return M1EventCoordinate{timestamp, sequence};
  };

  M1ProtocolEvidenceInput input;
  input.replicate_ordinal = 1U;
  input.boundaries =
      M1BoundaryEvidence{coordinate(timeline.cold_start, 1U),
                         coordinate(timeline.warmup_start, 1000U),
                         coordinate(timeline.measurement_start, 2000U),
                         coordinate(timeline.measurement_end, 10000U)};

  for (std::size_t index = 0U; index < kM1TotalI1OriginCount; ++index) {
    B1JobPhase phase = B1JobPhase::Measured;
    std::size_t ordinal = 0U;
    std::chrono::steady_clock::time_point origin;
    std::uint64_t origin_sequence = 0U;
    if (index == 0U) {
      phase = B1JobPhase::Cold;
      origin = timeline.cold_start;
      origin_sequence = 2U;
    } else if (index <= kM1WarmupI1OriginCount) {
      phase = B1JobPhase::Warmup;
      ordinal = index - 1U;
      origin = checked_i1_time_add(
          timeline.warmup_start,
          std::chrono::nanoseconds(static_cast<std::int64_t>(ordinal) *
                                   kI1EpisodeStride.count()));
      origin_sequence = ordinal == 0U ? 1001U : 1100U + ordinal;
    } else {
      ordinal = index - 1U - kM1WarmupI1OriginCount;
      origin = checked_i1_time_add(
          timeline.measurement_start,
          std::chrono::nanoseconds(static_cast<std::int64_t>(ordinal) *
                                   kI1EpisodeStride.count()));
      origin_sequence = ordinal == 0U ? 2001U : 4000U + ordinal;
    }
    const auto settlement =
        checked_i1_time_add(origin, kI1MeasurementEndOffset);
    const bool final_warmup =
        phase == B1JobPhase::Warmup && ordinal + 1U == kM1WarmupI1OriginCount;
    input.interactive_occurrences.push_back(M1InteractiveOccurrenceEvidence{
        phase, ordinal, coordinate(origin, origin_sequence), settlement,
        coordinate(settlement, 20000U + index),
        phase == B1JobPhase::Measured ? std::optional<std::chrono::nanoseconds>(
                                            std::chrono::milliseconds(10))
                                      : std::nullopt,
        I1ServiceEvidence{100U, 20U, 0U, 0.2}, I1Verdict::Pass, I1Verdict::Pass,
        I1Verdict::Pass, I1Verdict::Pass, true, final_warmup, final_warmup});
  }

  const auto make_offer =
      [&](B1JobPhase phase, std::uint64_t cycle, std::uint64_t job_index,
          std::uint64_t local_ordinal, M1EventCoordinate offered,
          std::optional<B1JobInstance> predecessor,
          std::optional<M1EventCoordinate> predecessor_end,
          std::optional<M1EventCoordinate> endpoint, bool output_removed) {
        return M1BatchOfferEvidence{
            B1JobInstance{kM1WorkloadId, 1U, phase, cycle, job_index, 8U},
            local_ordinal,
            0U,
            offered,
            std::move(predecessor),
            predecessor_end,
            endpoint,
            endpoint.has_value(),
            output_removed,
            true,
            true,
            true};
      };
  const B1JobInstance a252{kM1WorkloadId, 1U, B1JobPhase::Cold, 0U, 252U, 8U};
  const B1JobInstance b253{kM1WorkloadId, 1U, B1JobPhase::Warmup, 0U, 253U, 8U};
  const B1JobInstance a254{kM1WorkloadId, 1U, B1JobPhase::Warmup, 0U, 254U, 8U};
  const B1JobInstance b255{kM1WorkloadId, 1U, B1JobPhase::Warmup, 0U, 255U, 8U};
  const M1EventCoordinate a252_endpoint = coordinate(
      checked_i1_time_add(timeline.cold_start, std::chrono::milliseconds(700)),
      500U);
  const M1EventCoordinate b253_endpoint = coordinate(
      checked_i1_time_add(timeline.warmup_start, std::chrono::seconds(1)),
      1499U);
  input.batch_offers.push_back(make_offer(
      B1JobPhase::Cold, 0U, 252U, 0U, coordinate(timeline.cold_start, 3U),
      std::nullopt, std::nullopt, a252_endpoint, true));
  input.batch_offers.push_back(
      make_offer(B1JobPhase::Warmup, 0U, 253U, 0U,
                 coordinate(timeline.warmup_start, 1002U), std::nullopt,
                 std::nullopt, b253_endpoint, true));
  input.batch_offers.push_back(
      make_offer(B1JobPhase::Warmup, 0U, 254U, 1U,
                 coordinate(timeline.warmup_start, 1003U), a252, a252_endpoint,
                 coordinate(checked_i1_time_add(timeline.measurement_start,
                                                std::chrono::milliseconds(250)),
                            2500U),
                 true));
  input.batch_offers.push_back(make_offer(
      B1JobPhase::Warmup, 0U, 255U, 1U,
      coordinate(b253_endpoint.timestamp, 1500U), b253, b253_endpoint,
      coordinate(checked_i1_time_add(timeline.measurement_start,
                                     std::chrono::milliseconds(500)),
                 2600U),
      true));

  const B1JobInstance first_a{
      kM1WorkloadId, 1U, B1JobPhase::Measured, 0U, 0U, 8U};
  const B1JobInstance first_b{
      kM1WorkloadId, 1U, B1JobPhase::Measured, 0U, 1U, 8U};
  M1EventCoordinate first_a_offer =
      coordinate(timeline.measurement_start, 2002U);
  M1EventCoordinate first_b_offer =
      coordinate(timeline.measurement_start, 2003U);
  M1EventCoordinate first_a_endpoint = coordinate(
      checked_i1_time_add(timeline.measurement_start, std::chrono::seconds(1)),
      3000U);
  input.batch_offers.push_back(make_offer(B1JobPhase::Measured, 0U, 0U, 2U,
                                          first_a_offer, a254, std::nullopt,
                                          first_a_endpoint, false));
  input.batch_offers.push_back(make_offer(
      B1JobPhase::Measured, 0U, 1U, 2U, first_b_offer, b255, std::nullopt,
      coordinate(timeline.measurement_end, 10002U), false));

  B1JobInstance prior_a = first_a;
  M1EventCoordinate prior_endpoint = first_a_endpoint;
  for (std::size_t local_index = 1U; local_index <= 15U; ++local_index) {
    const std::uint64_t cycle = local_index / 15U;
    const std::uint64_t job = 2U * (local_index % 15U);
    const B1JobInstance current{kM1WorkloadId, 1U,  B1JobPhase::Measured,
                                cycle,         job, 8U};
    const M1EventCoordinate offered =
        coordinate(prior_endpoint.timestamp, 3000U + 2U * local_index);
    const M1EventCoordinate endpoint =
        local_index == 15U
            ? coordinate(timeline.measurement_end, 10001U)
            : coordinate(
                  checked_i1_time_add(timeline.measurement_start,
                                      std::chrono::seconds(local_index + 1U)),
                  3001U + 2U * local_index);
    input.batch_offers.push_back(make_offer(B1JobPhase::Measured, cycle, job,
                                            2U + local_index, offered, prior_a,
                                            prior_endpoint, endpoint, false));
    prior_a = current;
    prior_endpoint = endpoint;
  }

  input.carryover = {
      M1CarryoverEntry{"i1:warmup:6", B1JobPhase::Warmup,
                       M1CarryoverState::Running, "", true, true, false},
      M1CarryoverEntry{"b1:" + encode_b1_job_instance(a254), B1JobPhase::Warmup,
                       M1CarryoverState::Running, "", true, false, false},
      M1CarryoverEntry{"b1:" + encode_b1_job_instance(b255), B1JobPhase::Warmup,
                       M1CarryoverState::Queued, "", true, false, false}};
  input.first_measured_admission = M1FirstMeasuredAdmissionEvidence{
      0U,
      timeline.measurement_start,
      true,
      checked_i1_time_add(timeline.measurement_start,
                          std::chrono::milliseconds(1)),
      2004U,
      true,
      I1AcceptedCoordinate(checked_i1_time_add(timeline.measurement_start,
                                               std::chrono::milliseconds(1)),
                           2004U),
      true,
      true,
      false,
      checked_i1_time_add(timeline.measurement_start,
                          kI1MeasurementStartOffset)};
  input.shared_execution_domain = true;
  input.boundary_was_zero_duration = true;
  input.raw_history_preserved = true;
  input.warmup_sources_closed = true;
  input.measured_counters_reset = true;
  input.final_settlement_proved = true;
  return input;
}

/**
 * @brief Builds one complete source-private execution snapshot for row tests.
 * @param active Whether the cut contains bounded active mixed work.
 * @param high_water Complete lifetime Host high-water at this cut.
 * @return Self-consistent limits, ready, I/O, lifecycle, and reservation data.
 * @throws std::bad_alloc when lifecycle/vector storage allocates.
 */
M1ExecutionSnapshot make_m1_snapshot(bool active, ResourceVector high_water) {
  M1ExecutionSnapshot snapshot;
  snapshot.host_resources.limits =
      ResourceVector{32U, 1073741824U, 536870912U, 65536U, 268435456U};
  snapshot.host_resources.reserved =
      active ? ResourceVector{4U, 1048576U, 524288U, 3U, 4096U}
             : ResourceVector{};
  snapshot.host_resources.high_water = high_water;
  snapshot.compute_io.task_limit = kB1ComputeIoTaskLimit;
  snapshot.compute_io.planned_bytes_limit = kB1ComputeIoPlannedByteLimit;
  snapshot.compute_io.active_tasks = 0U;
  snapshot.compute_io.active_planned_bytes = 0U;
  snapshot.compute_io.constructing_tasks = 0U;
  snapshot.compute_io.queued_tasks = 0U;
  snapshot.compute_io.running_tasks = 0U;
  snapshot.compute_io.accepting = true;
  snapshot.throughput.capacity =
      ResourceVector{31U, 1006632960U, 503316480U, 64512U, 251658240U};
  snapshot.throughput.reserved =
      active ? ResourceVector{3U, 524288U, 262144U, 2U, 2048U}
             : ResourceVector{};
  snapshot.ready_classes =
      active ? compute::ExecutionReadyClassSnapshot{1U, 2U, 3U, true}
             : compute::ExecutionReadyClassSnapshot{};
  snapshot.lifecycle.schema_version =
      compute::kExecutionLifecycleTelemetrySchemaVersion;
  snapshot.lifecycle.capacity = compute::kExecutionLifecycleTelemetryCapacity;
  snapshot.lifecycle.service_instance_id = 1U;
  snapshot.lifecycle.telemetry_epoch = 1U;
  snapshot.lifecycle.service_state =
      compute::ExecutionLifecycleServiceState::Accepting;
  if (active) {
    snapshot.lifecycle.counters.ready_entry_count = 3U;
    snapshot.lifecycle.counters.entered_callback_count = 2U;
    snapshot.lifecycle.counters.live_root_reservation_count = 2U;
    snapshot.lifecycle.counters.live_child_grant_count = 3U;
  }
  return snapshot;
}

/**
 * @brief Builds one canonical same-service lifecycle event for row tests.
 * @param sequence Exact positive telemetry order.
 * @param kind Closed event transition kind.
 * @param counters Complete post-transition counters.
 * @return Versioned event with deterministic identities and timestamp.
 * @throws Nothing.
 */
compute::ExecutionLifecycleEvent make_m1_lifecycle_event(
    std::uint64_t sequence, compute::ExecutionLifecycleEventKind kind,
    compute::ExecutionLifecycleCounters counters) noexcept {
  compute::ExecutionLifecycleEvent event;
  event.sequence = sequence;
  event.timestamp_us = sequence;
  event.service_instance_id = 1U;
  event.telemetry_epoch = 1U;
  event.kind = kind;
  event.counters = counters;
  if (kind != compute::ExecutionLifecycleEventKind::ServiceStarted &&
      kind != compute::ExecutionLifecycleEventKind::ServiceStopped) {
    event.graph_instance_id = 1U;
  }
  switch (kind) {
    case compute::ExecutionLifecycleEventKind::CandidateBegan:
      event.generation = 1U;
      break;
    case compute::ExecutionLifecycleEventKind::BundleAdmitted:
    case compute::ExecutionLifecycleEventKind::RunTerminal:
    case compute::ExecutionLifecycleEventKind::RunQuiescent:
    case compute::ExecutionLifecycleEventKind::ResourceSettled:
    case compute::ExecutionLifecycleEventKind::RunUnregistered:
      event.run_id = 1U;
      event.generation = 1U;
      break;
    case compute::ExecutionLifecycleEventKind::GraphClosing:
      event.category = compute::ExecutionLifecycleCategory::GraphClose;
      event.generation = 1U;
      break;
    case compute::ExecutionLifecycleEventKind::GraphRowRemoved:
      event.generation = 1U;
      break;
    default:
      break;
  }
  if (kind == compute::ExecutionLifecycleEventKind::RunTerminal) {
    event.category = compute::ExecutionLifecycleCategory::Succeeded;
  }
  return event;
}

/**
 * @brief Assigns one exact lossless cursor page to a temporal test snapshot.
 * @param snapshot Mutable execution snapshot.
 * @param ordinal Chronological capture coordinate.
 * @param after_cursor Exact requested prior cut.
 * @param snapshot_cut Exact atomic page cut.
 * @param records Complete contiguous records after the cursor.
 * @return Nothing.
 * @throws std::bad_alloc when record ownership moves.
 */
void set_m1_lifecycle_page(
    M1ExecutionSnapshot* snapshot, std::size_t ordinal,
    std::uint64_t after_cursor, std::uint64_t snapshot_cut,
    std::vector<compute::ExecutionLifecycleEvent> records) {
  snapshot->temporal_capture_ordinal = ordinal;
  snapshot->lifecycle_after_cursor = after_cursor;
  snapshot->lifecycle.snapshot_cut = snapshot_cut;
  snapshot->lifecycle.first_retained_sequence = snapshot_cut == 0U ? 0U : 1U;
  snapshot->lifecycle.next_sequence = snapshot_cut + 1U;
  snapshot->lifecycle.records = std::move(records);
  snapshot->lifecycle.next_cursor = snapshot_cut;
}

/**
 * @brief Populates the four passing snapshots with one continuous history.
 * @param snapshots Exact four-snapshot test timeline to mutate.
 * @return Nothing.
 * @throws std::invalid_argument when the timeline cardinality drifts.
 * @throws std::bad_alloc when event vectors allocate.
 */
void configure_passing_m1_lifecycle_history(
    std::vector<M1ExecutionSnapshot>* snapshots) {
  if (snapshots == nullptr || snapshots->size() != 4U) {
    throw std::invalid_argument(
        "M1 lifecycle test history requires exactly four snapshots");
  }
  const auto zero = snapshots->front().lifecycle.counters;
  const auto active = (*snapshots)[1U].lifecycle.counters;
  set_m1_lifecycle_page(
      &(*snapshots)[0U], 0U, 0U, 2U,
      {make_m1_lifecycle_event(
           1U, compute::ExecutionLifecycleEventKind::ServiceStarted, zero),
       make_m1_lifecycle_event(
           2U, compute::ExecutionLifecycleEventKind::GraphRegistered, zero)});
  set_m1_lifecycle_page(
      &(*snapshots)[1U], 1U, 2U, 8U,
      {make_m1_lifecycle_event(
           3U, compute::ExecutionLifecycleEventKind::CandidateBegan, active),
       make_m1_lifecycle_event(
           4U, compute::ExecutionLifecycleEventKind::BundleAdmitted, active),
       make_m1_lifecycle_event(
           5U, compute::ExecutionLifecycleEventKind::RunTerminal, active),
       make_m1_lifecycle_event(
           6U, compute::ExecutionLifecycleEventKind::RunQuiescent, active),
       make_m1_lifecycle_event(
           7U, compute::ExecutionLifecycleEventKind::ResourceSettled, active),
       make_m1_lifecycle_event(
           8U, compute::ExecutionLifecycleEventKind::RunUnregistered, active)});
  set_m1_lifecycle_page(&(*snapshots)[2U], 2U, 8U, 8U, {});
  set_m1_lifecycle_page(
      &(*snapshots)[3U], 3U, 8U, 10U,
      {make_m1_lifecycle_event(
           9U, compute::ExecutionLifecycleEventKind::GraphClosing, zero),
       make_m1_lifecycle_event(
           10U, compute::ExecutionLifecycleEventKind::GraphRowRemoved, zero)});
}

/**
 * @brief Builds one valid event-aligned executor snapshot for a test task.
 * @param active_tasks Zero or one active task.
 * @param active_bytes Exact active charge paired with `active_tasks`.
 * @return Frozen-limit snapshot with the task represented as queued.
 * @throws Nothing.
 */
execution::ComputeIoExecutorSnapshot make_m1_io_event_snapshot(
    std::uint64_t active_tasks, std::uint64_t active_bytes) noexcept {
  execution::ComputeIoExecutorSnapshot snapshot;
  snapshot.task_limit = kB1ComputeIoTaskLimit;
  snapshot.planned_bytes_limit = kB1ComputeIoPlannedByteLimit;
  snapshot.active_tasks = active_tasks;
  snapshot.active_planned_bytes = active_bytes;
  snapshot.queued_tasks = active_tasks;
  snapshot.accepting = true;
  return snapshot;
}

/**
 * @brief Builds one fault-free two-stage B1 I/O stream for an M1 offer.
 * @param offer Exact protocol offer and endpoint identity.
 * @param first_sequence First of four globally unique accounting sequences.
 * @return Minimal complete B1 job record accepted by the reusable I/O FSM.
 * @throws std::invalid_argument when the offer lacks its terminal endpoint.
 * @throws std::bad_alloc when observation storage allocates.
 */
B1JobEvidence make_m1_batch_io_job(const M1BatchOfferEvidence& offer,
                                   std::uint64_t first_sequence) {
  if (!offer.endpoint.has_value()) {
    throw std::invalid_argument("M1 test offer lacks an endpoint");
  }
  B1JobEvidence evidence;
  evidence.job = offer.job;
  evidence.producer_offer_ordinal = offer.producer_offer_ordinal;
  evidence.offered_at = offer.offered.timestamp;
  evidence.endpoint_at = offer.endpoint->timestamp;
  evidence.output.status = B1OutputCommitStatus::RevalidationFailed;

  const B1IoTaskIdentity payload{offer.job, B1IoStage::PayloadStage, 0U};
  const B1IoTaskIdentity manifest{offer.job, B1IoStage::ManifestCommit, 0U};
  const std::uint64_t manifest_bytes = b1_manifest_length(offer.job.job_index);
  const auto zero = make_m1_io_event_snapshot(0U, 0U);
  const auto payload_active = make_m1_io_event_snapshot(1U, kB1PayloadBytes);
  const auto manifest_active = make_m1_io_event_snapshot(1U, manifest_bytes);
  const execution::ComputeIoAdmissionEvent payload_admission{
      first_sequence,  execution::ComputeIoAdmissionStatus::Accepted,
      kB1PayloadBytes, 1U,
      kB1PayloadBytes, payload_active};
  const execution::ComputeIoSettlementEvent payload_settlement{
      first_sequence + 1U,
      first_sequence,
      execution::ComputeIoCompletionStatus::Succeeded,
      1U,
      kB1PayloadBytes,
      zero};
  const execution::ComputeIoAdmissionEvent manifest_admission{
      first_sequence + 2U, execution::ComputeIoAdmissionStatus::Accepted,
      manifest_bytes,      1U,
      manifest_bytes,      manifest_active};
  const execution::ComputeIoSettlementEvent manifest_settlement{
      first_sequence + 3U,
      first_sequence + 2U,
      execution::ComputeIoCompletionStatus::Succeeded,
      1U,
      manifest_bytes,
      zero};
  evidence.output.io_observations = {
      {B1IoObservationPoint::Initial, std::nullopt, 0U, std::nullopt,
       std::nullopt, std::nullopt, std::nullopt, zero},
      {B1IoObservationPoint::AcceptedAdmission, payload, kB1PayloadBytes,
       execution::ComputeIoAdmissionStatus::Accepted, std::nullopt,
       payload_admission, std::nullopt, payload_active},
      {B1IoObservationPoint::Settlement, payload, kB1PayloadBytes,
       execution::ComputeIoAdmissionStatus::Accepted,
       execution::ComputeIoCompletionStatus::Succeeded, payload_admission,
       payload_settlement, zero},
      {B1IoObservationPoint::AcceptedAdmission, manifest, manifest_bytes,
       execution::ComputeIoAdmissionStatus::Accepted, std::nullopt,
       manifest_admission, std::nullopt, manifest_active},
      {B1IoObservationPoint::Settlement, manifest, manifest_bytes,
       execution::ComputeIoAdmissionStatus::Accepted,
       execution::ComputeIoCompletionStatus::Succeeded, manifest_admission,
       manifest_settlement, zero},
      {B1IoObservationPoint::Final, std::nullopt, 0U, std::nullopt,
       std::nullopt, std::nullopt, std::nullopt, zero}};
  return evidence;
}

/**
 * @brief Builds one complete passing five-axis M1 row input.
 * @return Exact protocol, SLO samples, fault-free waste, and zero settlement.
 * @throws Allocation and checked-time failures unchanged.
 */
M1InnerRowInput make_passing_inner_row_input() {
  M1InnerRowInput input;
  input.replicate_ordinal = 1U;
  input.protocol = make_passing_protocol();
  input.fairness = make_passing_fairness_input();
  input.paired_isolated_i1_p99 = std::chrono::milliseconds(10);
  input.batch_waste = M1BatchWasteEvidence{1000U, 0U, 0U, 0U, 0U};
  std::uint64_t io_sequence = 1U;
  for (const M1BatchOfferEvidence& offer : input.protocol.batch_offers) {
    input.batch_jobs.push_back(make_m1_batch_io_job(offer, io_sequence));
    io_sequence += 4U;
  }
  const ResourceVector zero_high_water{};
  const ResourceVector active_high_water{4U, 1048576U, 524288U, 3U, 4096U};
  input.temporal_snapshots = {make_m1_snapshot(false, zero_high_water),
                              make_m1_snapshot(true, active_high_water),
                              make_m1_snapshot(true, active_high_water),
                              make_m1_snapshot(false, active_high_water)};
  configure_passing_m1_lifecycle_history(&input.temporal_snapshots);
  input.occurrence_attribution_proved = true;
  input.temporal_effects_complete = true;
  return input;
}

/**
 * @brief Proves exact M1 boundary derivation and checked arithmetic failures.
 * @throws GoogleTest assertion control and checked clock exceptions.
 */
TEST(M1Profile, DerivesExactTimelineAndRejectsClockOverflow) {
  const auto measurement_start =
      std::chrono::steady_clock::time_point(std::chrono::seconds(100));
  const M1Timeline timeline = derive_m1_timeline(measurement_start);
  EXPECT_EQ(timeline.cold_start,
            std::chrono::steady_clock::time_point(std::chrono::seconds(94)));
  EXPECT_EQ(timeline.warmup_start,
            std::chrono::steady_clock::time_point(std::chrono::seconds(95)));
  EXPECT_EQ(timeline.measurement_start, measurement_start);
  EXPECT_EQ(timeline.measurement_end,
            std::chrono::steady_clock::time_point(std::chrono::seconds(130)));

  EXPECT_THROW(derive_m1_timeline(std::chrono::steady_clock::time_point::min()),
               std::overflow_error);
  EXPECT_THROW(derive_m1_timeline(std::chrono::steady_clock::time_point::max()),
               std::overflow_error);
}

/**
 * @brief Proves exact cold/warmup/carryover and producer-local cycle protocol.
 * @throws GoogleTest assertion control and protocol allocation failures.
 */
TEST(M1Profile, PassesExactCrossBoundaryProtocolAndIndependentCycles) {
  const M1ProtocolEvidenceInput input = make_passing_protocol();
  const M1ProtocolSummary summary = evaluate_m1_protocol(input);
  EXPECT_EQ(summary.verdict, I1Verdict::Pass)
      << (summary.validity_reasons.empty() ? "no diagnostic"
                                           : summary.validity_reasons.front());
  EXPECT_TRUE(summary.validity_reasons.empty());
  ASSERT_GT(input.batch_offers.size(), 6U);
  EXPECT_EQ(input.batch_offers.back().job.cycle_ordinal, 1U);
  EXPECT_EQ(input.batch_offers.back().job.job_index, 0U);
  EXPECT_EQ(input.batch_offers[5U].job.cycle_ordinal, 0U);
  EXPECT_EQ(input.batch_offers[5U].job.job_index, 1U);
}

/**
 * @brief Proves B255, carryover, and final-warmup exception drift fail closed.
 * @throws GoogleTest assertion control and protocol allocation failures.
 */
TEST(M1Profile, RejectsWarmupCarryoverAndCurrentHoldDrift) {
  M1ProtocolEvidenceInput b255 = make_passing_protocol();
  b255.batch_offers[3U].predecessor_terminal.reset();
  EXPECT_EQ(evaluate_m1_protocol(std::move(b255)).verdict, I1Verdict::Invalid);

  M1ProtocolEvidenceInput carryover = make_passing_protocol();
  carryover.carryover.pop_back();
  EXPECT_EQ(evaluate_m1_protocol(std::move(carryover)).verdict,
            I1Verdict::Invalid);

  M1ProtocolEvidenceInput current_hold = make_passing_protocol();
  current_hold.interactive_occurrences[kM1WarmupI1OriginCount]
      .publication_current_at_measurement = false;
  EXPECT_EQ(evaluate_m1_protocol(std::move(current_hold)).verdict,
            I1Verdict::Invalid);

  M1ProtocolEvidenceInput admission = make_passing_protocol();
  admission.first_measured_admission.accepted_coordinate.reset();
  admission.first_measured_admission.host_succeeded = false;
  EXPECT_EQ(evaluate_m1_protocol(std::move(admission)).verdict,
            I1Verdict::Invalid);
}

/**
 * @brief Proves shared barriers, phase rewrites, and post-cutoff offers fail.
 * @throws GoogleTest assertion control and protocol allocation failures.
 */
TEST(M1Profile, RejectsProducerBarrierPhaseAndCutoffDrift) {
  M1ProtocolEvidenceInput phase = make_passing_protocol();
  phase.batch_offers[2U].job.phase = B1JobPhase::Measured;
  EXPECT_EQ(evaluate_m1_protocol(std::move(phase)).verdict, I1Verdict::Invalid);

  M1ProtocolEvidenceInput gap = make_passing_protocol();
  gap.batch_offers.back().offered.timestamp += std::chrono::nanoseconds(1);
  EXPECT_EQ(evaluate_m1_protocol(std::move(gap)).verdict, I1Verdict::Invalid);

  M1ProtocolEvidenceInput cutoff = make_passing_protocol();
  cutoff.batch_offers.back().offered = cutoff.boundaries.measurement_end;
  cutoff.batch_offers.back().offered.event_sequence += 100U;
  EXPECT_EQ(evaluate_m1_protocol(std::move(cutoff)).verdict,
            I1Verdict::Invalid);
}

/**
 * @brief Proves all five closed M1 axes pass without cross-axis substitution.
 * @throws GoogleTest assertion control and row evaluation allocation failures.
 */
TEST(M1Profile, PassesClosedFiveAxisInnerRow) {
  const M1InnerRow row = evaluate_m1_inner_row(make_passing_inner_row_input());
  EXPECT_EQ(row.schema, kM1InnerRowSchema);
  EXPECT_EQ(row.schema_version, kM1InnerRowSchemaVersion);
  EXPECT_EQ(row.workload_id, kM1WorkloadId);
  EXPECT_EQ(row.protocol.verdict, I1Verdict::Pass);
  ASSERT_TRUE(row.latency.has_value());
  EXPECT_EQ(row.latency->p50, std::chrono::milliseconds(10));
  EXPECT_EQ(row.latency->p95, std::chrono::milliseconds(10));
  EXPECT_EQ(row.latency->p99, std::chrono::milliseconds(10));
  ASSERT_TRUE(row.relative_latency_p99.has_value());
  EXPECT_DOUBLE_EQ(*row.relative_latency_p99, 1.0);
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.throughput_progress_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.fairness_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.overall_verdict, I1Verdict::Pass)
      << (row.validity_reasons.empty() ? "no diagnostic"
                                       : row.validity_reasons.front());
  EXPECT_DOUBLE_EQ(*row.interactive_discarded_ratio, 0.2);
  EXPECT_EQ(row.compute_io_task_high_water, 1U);
  EXPECT_EQ(row.compute_io_planned_byte_high_water, kB1PayloadBytes);
}

/**
 * @brief Proves a short accepted/settled I/O task entirely between sparse M1
 * cuts still contributes to event-aligned high-water.
 * @throws GoogleTest assertion control and row-evaluation allocation failures.
 */
TEST(M1Profile, DerivesIoHighWaterBetweenSparseTemporalCuts) {
  M1InnerRowInput input = make_passing_inner_row_input();
  ASSERT_TRUE(std::all_of(
      input.temporal_snapshots.begin(), input.temporal_snapshots.end(),
      [](const M1ExecutionSnapshot& snapshot) {
        return snapshot.compute_io.active_tasks == 0U &&
               snapshot.compute_io.active_planned_bytes == 0U;
      }));

  const M1InnerRow row = evaluate_m1_inner_row(std::move(input));
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.compute_io_task_high_water, 1U);
  EXPECT_EQ(row.compute_io_planned_byte_high_water, kB1PayloadBytes);
}

/**
 * @brief Proves latency, B1 waste, and final settlement fail independently.
 * @throws GoogleTest assertion control and row evaluation allocation failures.
 */
TEST(M1Profile, KeepsFiveAxisFailuresIndependent) {
  M1InnerRowInput latency_input = make_passing_inner_row_input();
  latency_input.paired_isolated_i1_p99 = std::chrono::milliseconds(4);
  const M1InnerRow latency = evaluate_m1_inner_row(std::move(latency_input));
  EXPECT_EQ(latency.latency_verdict, I1Verdict::Fail);
  EXPECT_EQ(latency.throughput_progress_verdict, I1Verdict::Pass);
  EXPECT_EQ(latency.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(latency.memory_verdict, I1Verdict::Pass);

  M1InnerRowInput waste_input = make_passing_inner_row_input();
  waste_input.batch_waste.discarded_started_service = 1U;
  const M1InnerRow waste = evaluate_m1_inner_row(std::move(waste_input));
  EXPECT_EQ(waste.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(waste.waste_verdict, I1Verdict::Fail);
  EXPECT_EQ(waste.memory_verdict, I1Verdict::Pass);

  M1InnerRowInput memory_input = make_passing_inner_row_input();
  memory_input.temporal_snapshots.back().host_resources.reserved.cpu_slots = 1U;
  const M1InnerRow memory = evaluate_m1_inner_row(std::move(memory_input));
  EXPECT_EQ(memory.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(memory.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(memory.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(memory.overall_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves M1 rejects every incomplete or contradictory raw I/O stream.
 * @throws GoogleTest assertion control and row-evaluation allocation failures.
 */
TEST(M1Profile, RejectsMissingDuplicateMalformedAndOverlimitIoEvidence) {
  M1InnerRowInput missing = make_passing_inner_row_input();
  missing.batch_jobs.pop_back();
  EXPECT_EQ(evaluate_m1_inner_row(std::move(missing)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput duplicate = make_passing_inner_row_input();
  ASSERT_GE(duplicate.batch_jobs.size(), 2U);
  const std::uint64_t duplicate_sequence = duplicate.batch_jobs[0U]
                                               .output.io_observations[1U]
                                               .admission_event->sequence;
  auto& duplicate_stream = duplicate.batch_jobs[1U].output.io_observations;
  duplicate_stream[1U].admission_event->sequence = duplicate_sequence;
  duplicate_stream[2U].admission_event->sequence = duplicate_sequence;
  duplicate_stream[2U].settlement_event->admission_sequence =
      duplicate_sequence;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(duplicate)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput reordered = make_passing_inner_row_input();
  std::swap(reordered.batch_jobs[0U].output.io_observations[1U],
            reordered.batch_jobs[0U].output.io_observations[2U]);
  EXPECT_EQ(evaluate_m1_inner_row(std::move(reordered)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput arithmetic = make_passing_inner_row_input();
  auto& arithmetic_stream = arithmetic.batch_jobs[0U].output.io_observations;
  --arithmetic_stream[1U].admission_event->charged_planned_bytes;
  --arithmetic_stream[2U].admission_event->charged_planned_bytes;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(arithmetic)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput unknown = make_passing_inner_row_input();
  unknown.batch_jobs[0U].output.io_observations[1U].point =
      static_cast<B1IoObservationPoint>(255U);
  EXPECT_EQ(evaluate_m1_inner_row(std::move(unknown)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput overlimit = make_passing_inner_row_input();
  auto& overlimit_stream = overlimit.batch_jobs[0U].output.io_observations;
  auto invalid_snapshot = overlimit_stream[1U].snapshot;
  invalid_snapshot.active_tasks = kB1ComputeIoTaskLimit + 1U;
  invalid_snapshot.queued_tasks = kB1ComputeIoTaskLimit + 1U;
  overlimit_stream[1U].snapshot = invalid_snapshot;
  overlimit_stream[1U].admission_event->snapshot_after = invalid_snapshot;
  overlimit_stream[2U].admission_event->snapshot_after = invalid_snapshot;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(overlimit)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput nonzero_final = make_passing_inner_row_input();
  nonzero_final.temporal_snapshots.back().compute_io.active_tasks = 1U;
  nonzero_final.temporal_snapshots.back().compute_io.queued_tasks = 1U;
  nonzero_final.temporal_snapshots.back().compute_io.active_planned_bytes = 1U;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(nonzero_final)).memory_verdict,
            I1Verdict::Invalid);
}

/**
 * @brief Proves lifecycle pages fail closed for continuity and post-stop drift.
 * @throws GoogleTest assertion control and row-evaluation allocation failures.
 */
TEST(M1Profile, RejectsLifecyclePageContinuityAndPostStopEvidence) {
  M1InnerRowInput empty = make_passing_inner_row_input();
  for (std::size_t index = 0U; index < empty.temporal_snapshots.size();
       ++index) {
    set_m1_lifecycle_page(&empty.temporal_snapshots[index], index, 0U, 0U, {});
  }
  EXPECT_EQ(evaluate_m1_inner_row(std::move(empty)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput missing_page = make_passing_inner_row_input();
  missing_page.temporal_snapshots.erase(
      missing_page.temporal_snapshots.begin() + 1);
  EXPECT_EQ(evaluate_m1_inner_row(std::move(missing_page)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput missing_record = make_passing_inner_row_input();
  missing_record.temporal_snapshots[1U].lifecycle.records.erase(
      missing_record.temporal_snapshots[1U].lifecycle.records.begin() + 2);
  EXPECT_EQ(evaluate_m1_inner_row(std::move(missing_record)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput duplicate = make_passing_inner_row_input();
  duplicate.temporal_snapshots[1U].lifecycle.records[1U].sequence = 3U;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(duplicate)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput reordered = make_passing_inner_row_input();
  std::swap(reordered.temporal_snapshots[1U].lifecycle.records[0U],
            reordered.temporal_snapshots[1U].lifecycle.records[1U]);
  EXPECT_EQ(evaluate_m1_inner_row(std::move(reordered)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput broken_cursor = make_passing_inner_row_input();
  broken_cursor.temporal_snapshots[2U].lifecycle_after_cursor = 7U;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(broken_cursor)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput broken_cut = make_passing_inner_row_input();
  broken_cut.temporal_snapshots[2U].lifecycle.snapshot_cut = 9U;
  broken_cut.temporal_snapshots[2U].lifecycle.next_cursor = 9U;
  broken_cut.temporal_snapshots[2U].lifecycle.next_sequence = 10U;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(broken_cut)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput broken_identity = make_passing_inner_row_input();
  broken_identity.temporal_snapshots[0U]
      .lifecycle.records[1U]
      .graph_instance_id = 0U;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(broken_identity)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput post_stop = make_passing_inner_row_input();
  M1ExecutionSnapshot& stopped = post_stop.temporal_snapshots.back();
  compute::ExecutionLifecycleEvent stop = make_m1_lifecycle_event(
      11U, compute::ExecutionLifecycleEventKind::ServiceStopped,
      stopped.lifecycle.counters);
  stop.generation = 1U;
  stopped.lifecycle.records.push_back(stop);
  stopped.lifecycle.snapshot_cut = 11U;
  stopped.lifecycle.next_cursor = 11U;
  stopped.lifecycle.next_sequence = std::numeric_limits<std::uint64_t>::max();
  stopped.lifecycle.shutdown_generation = 1U;
  stopped.lifecycle.service_state =
      compute::ExecutionLifecycleServiceState::Stopped;

  M1ExecutionSnapshot after_stop = stopped;
  set_m1_lifecycle_page(
      &after_stop, 4U, 11U, 12U,
      {make_m1_lifecycle_event(
          12U, compute::ExecutionLifecycleEventKind::GraphRegistered,
          stopped.lifecycle.counters)});
  after_stop.lifecycle.next_sequence =
      std::numeric_limits<std::uint64_t>::max();
  after_stop.lifecycle.shutdown_generation = 1U;
  after_stop.lifecycle.service_state =
      compute::ExecutionLifecycleServiceState::Stopped;
  post_stop.temporal_snapshots.push_back(std::move(after_stop));
  const M1InnerRow post_stop_row = evaluate_m1_inner_row(std::move(post_stop));
  EXPECT_EQ(post_stop_row.memory_verdict, I1Verdict::Invalid);
  EXPECT_TRUE(std::any_of(
      post_stop_row.validity_reasons.begin(),
      post_stop_row.validity_reasons.end(), [](const std::string& reason) {
        return reason.find("ordinary event appears after ServiceStopped") !=
               std::string::npos;
      }));
}

/**
 * @brief Proves missing temporal or phase-attribution evidence is Invalid.
 * @throws GoogleTest assertion control and row evaluation allocation failures.
 */
TEST(M1Profile, InvalidatesMissingTemporalAndAttributionEvidence) {
  M1InnerRowInput temporal = make_passing_inner_row_input();
  temporal.temporal_snapshots.resize(3U);
  const M1InnerRow temporal_row = evaluate_m1_inner_row(std::move(temporal));
  EXPECT_EQ(temporal_row.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(temporal_row.overall_verdict, I1Verdict::Invalid);

  M1InnerRowInput attribution = make_passing_inner_row_input();
  attribution.occurrence_attribution_proved = false;
  const M1InnerRow attribution_row =
      evaluate_m1_inner_row(std::move(attribution));
  EXPECT_EQ(attribution_row.latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(attribution_row.throughput_progress_verdict, I1Verdict::Invalid);
  EXPECT_EQ(attribution_row.fairness_verdict, I1Verdict::Invalid);
  EXPECT_EQ(attribution_row.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(attribution_row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(attribution_row.overall_verdict, I1Verdict::Invalid);

  M1InnerRowInput temporal_effects = make_passing_inner_row_input();
  temporal_effects.temporal_effects_complete = false;
  const M1InnerRow effects_row =
      evaluate_m1_inner_row(std::move(temporal_effects));
  EXPECT_EQ(effects_row.fairness_verdict, I1Verdict::Invalid);
  EXPECT_EQ(effects_row.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(effects_row.overall_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves every exact fairness threshold and guard passes independently.
 * @throws GoogleTest assertion control and evaluator allocation failures.
 */
TEST(M1Profile, PassesCompleteIndependentFairnessGuards) {
  const M1FairnessSummary summary =
      evaluate_m1_fairness(make_passing_fairness_input());
  ASSERT_TRUE(summary.throughput_progress_p05.has_value());
  EXPECT_DOUBLE_EQ(*summary.throughput_progress_p05,
                   kM1ThroughputProgressP05Floor);
  ASSERT_TRUE(summary.graph_jain_p05.has_value());
  EXPECT_DOUBLE_EQ(*summary.graph_jain_p05, 1.0);
  EXPECT_EQ(summary.maximum_interactive_burst, kM1InteractiveBurstLimit);
  EXPECT_TRUE(summary.validity_reasons.empty());
  EXPECT_EQ(summary.throughput_progress_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.graph_jain_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.class_start_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.interactive_headroom_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.interactive_latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.composite_fairness_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves nearest-rank p05 exposes starving windows despite high average.
 * @throws GoogleTest assertion control and evaluator allocation failures.
 */
TEST(M1Profile, ProgressP05FailsDespitePassingOverallAverage) {
  M1FairnessEvidenceInput input = make_passing_fairness_input();
  input.progress_windows[0U].successful_site_operations = 0U;
  input.progress_windows[1U].successful_site_operations = 190000U;
  for (std::size_t index = 2U; index < input.progress_windows.size(); ++index) {
    input.progress_windows[index].successful_site_operations = 1000000U;
  }
  const M1FairnessSummary summary = evaluate_m1_fairness(std::move(input));
  ASSERT_TRUE(summary.throughput_progress_p05.has_value());
  EXPECT_DOUBLE_EQ(*summary.throughput_progress_p05, 0.19);
  EXPECT_EQ(summary.throughput_progress_verdict, I1Verdict::Fail);
  EXPECT_EQ(summary.class_start_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.composite_fairness_verdict, I1Verdict::Fail);
}

/**
 * @brief Proves peer service and class-start failures remain independent.
 * @throws GoogleTest assertion control and evaluator allocation failures.
 */
TEST(M1Profile, FailsPeerJainAndFourInteractiveBurstIndependently) {
  M1FairnessEvidenceInput input = make_passing_fairness_input();
  for (M1GraphServiceWindow& window : input.graph_service_windows) {
    window.graph_a_completed_service = 100U;
    window.graph_b_completed_service = 1U;
  }
  input.class_starts.clear();
  for (std::size_t index = 0U; index < 4U; ++index) {
    input.class_starts.push_back(
        M1ClassStartSample{index + 1U, compute::ComputeRunQosClass::Interactive,
                           true, true, true});
  }
  input.class_starts.push_back(M1ClassStartSample{
      5U, compute::ComputeRunQosClass::Throughput, true, true, true});

  const M1FairnessSummary summary = evaluate_m1_fairness(std::move(input));
  ASSERT_TRUE(summary.graph_jain_p05.has_value());
  EXPECT_LT(*summary.graph_jain_p05, kM1GraphJainP05Floor);
  EXPECT_EQ(summary.graph_jain_verdict, I1Verdict::Fail);
  EXPECT_EQ(summary.maximum_interactive_burst, 4U);
  EXPECT_EQ(summary.class_start_verdict, I1Verdict::Fail);
  EXPECT_EQ(summary.throughput_progress_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.composite_fairness_verdict, I1Verdict::Fail);
}

/**
 * @brief Proves a fourth Interactive start counts only at a real dual-start
 * cut.
 * @throws GoogleTest assertion control and evaluator allocation failures.
 * @note Nominal offer overlap is deliberately absent from the evidence model;
 * the negative case differs only in product-authored Throughput startability.
 */
TEST(M1Profile, ExcludesFourthStartWhenThroughputWasNotReallyStartable) {
  M1FairnessEvidenceInput input = make_passing_fairness_input();
  input.class_starts = {
      {1U, compute::ComputeRunQosClass::Interactive, true, true, true},
      {2U, compute::ComputeRunQosClass::Interactive, true, true, true},
      {3U, compute::ComputeRunQosClass::Interactive, true, true, true},
      {4U, compute::ComputeRunQosClass::Interactive, true, false, true},
      {5U, compute::ComputeRunQosClass::Throughput, true, true, true}};

  const M1FairnessSummary summary = evaluate_m1_fairness(std::move(input));
  EXPECT_EQ(summary.maximum_interactive_burst, 3U);
  EXPECT_EQ(summary.class_start_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.composite_fairness_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves headroom and latency failures cannot substitute for each other.
 * @throws GoogleTest assertion control and evaluator allocation failures.
 */
TEST(M1Profile, KeepsHeadroomAndLatencyVerdictsNonSubstitutable) {
  M1FairnessEvidenceInput input = make_passing_fairness_input();
  input.headroom_admissions.throughput_headroom_failures = 1U;
  input.headroom_outcomes.front().host_status = OperationStatus{
      false, OperationErrorDomain::Graph, 1, "headroom", "headroom"};
  input.headroom_outcomes.front().throughput_headroom_failure = true;
  input.interactive_latency_verdict = I1Verdict::Fail;
  const M1FairnessSummary summary = evaluate_m1_fairness(std::move(input));
  EXPECT_EQ(summary.throughput_progress_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.graph_jain_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.class_start_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.interactive_headroom_verdict, I1Verdict::Fail);
  EXPECT_EQ(summary.interactive_latency_verdict, I1Verdict::Fail);
  EXPECT_EQ(summary.composite_fairness_verdict, I1Verdict::Fail);
}

/**
 * @brief Proves missing, zero, and overflowed evidence fails closed as invalid.
 * @throws GoogleTest assertion control and evaluator allocation failures.
 */
TEST(M1Profile, InvalidatesIncompleteAndOverflowedEvidence) {
  M1FairnessEvidenceInput input = make_passing_fairness_input();
  input.progress_windows.pop_back();
  input.graph_service_windows.front() = M1GraphServiceWindow{0U, true, 0U, 0U};
  input.class_starts.pop_back();
  input.headroom_admissions.classified_outcomes -= 1U;
  input.observation_overflowed = true;
  input.observation_sequence_exhausted = true;
  const M1FairnessSummary summary = evaluate_m1_fairness(std::move(input));
  EXPECT_EQ(summary.throughput_progress_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary.graph_jain_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary.class_start_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary.interactive_headroom_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary.composite_fairness_verdict, I1Verdict::Invalid);
  EXPECT_FALSE(summary.validity_reasons.empty());
}

/**
 * @brief Proves unknown closed verdict and QoS representations fail closed.
 * @throws GoogleTest assertion control and evaluator allocation failures.
 */
TEST(M1Profile, UnknownClosedEnumsInvalidateCompositeEvidence) {
  M1FairnessEvidenceInput unknown_verdict = make_passing_fairness_input();
  unknown_verdict.interactive_latency_verdict = static_cast<I1Verdict>(255U);
  const M1FairnessSummary verdict_summary =
      evaluate_m1_fairness(std::move(unknown_verdict));
  EXPECT_EQ(verdict_summary.composite_fairness_verdict, I1Verdict::Invalid);
  EXPECT_NE(std::find(verdict_summary.validity_reasons.begin(),
                      verdict_summary.validity_reasons.end(),
                      "M1 fairness verdict contains an unknown value"),
            verdict_summary.validity_reasons.end());

  M1FairnessEvidenceInput unknown_class = make_passing_fairness_input();
  unknown_class.class_starts.insert(
      unknown_class.class_starts.begin(),
      M1ClassStartSample{1U, static_cast<compute::ComputeRunQosClass>(255U),
                         true, true, true});
  const M1FairnessSummary class_summary =
      evaluate_m1_fairness(std::move(unknown_class));
  EXPECT_EQ(class_summary.class_start_verdict, I1Verdict::Invalid);
  EXPECT_EQ(class_summary.composite_fairness_verdict, I1Verdict::Invalid);
  EXPECT_NE(std::find(class_summary.validity_reasons.begin(),
                      class_summary.validity_reasons.end(),
                      "M1 class-start evidence contains an unknown QoS class"),
            class_summary.validity_reasons.end());
}

/**
 * @brief Proves fanout allocates once and forwards one identical coordinate.
 * @throws GoogleTest assertion control and shared-owner allocation failures.
 */
TEST(M1Profile, ObservationFanoutUsesOnlySharedSequenceAuthority) {
  const compute::ComputeRunObservationCoordinate authority_coordinate{
      std::chrono::steady_clock::time_point(std::chrono::nanoseconds(17)), 41U};
  const compute::ComputeRunObservationCoordinate mirror_coordinate{
      std::chrono::steady_clock::time_point(std::chrono::nanoseconds(99)),
      700U};
  const auto authority =
      std::make_shared<RecordingObservationSink>(authority_coordinate, false);
  const auto mirror =
      std::make_shared<RecordingObservationSink>(mirror_coordinate, true);

  EXPECT_THROW(make_compute_run_observation_fanout(nullptr, mirror),
               std::invalid_argument);
  EXPECT_THROW(make_compute_run_observation_fanout(authority, authority),
               std::invalid_argument);
  const auto fanout = make_compute_run_observation_fanout(authority, mirror);
  ASSERT_NE(fanout, nullptr);
  EXPECT_TRUE(fanout->observes_task_semantics());

  const compute::ComputeRunObservationCoordinate reserved =
      fanout->reserve_causal_coordinate();
  EXPECT_EQ(reserved.observed_at, authority_coordinate.observed_at);
  EXPECT_EQ(reserved.causal_sequence, authority_coordinate.causal_sequence);
  EXPECT_EQ(authority->reservation_count, 1U);
  EXPECT_EQ(mirror->reservation_count, 0U);

  compute::ComputeRun run(make_observer_submission(
      "m1-fanout", 60U, compute::ComputeRunQosClass::Interactive));
  fanout->on_service_start(
      run.descriptor(),
      compute::ComputeRunTaskIdentity(run.descriptor().id(),
                                      compute::ComputeRunLocalTaskId(1U)),
      8U, compute::ComputeRunServiceStartObservation{true, true, true},
      reserved);
  EXPECT_EQ(authority->callback_count, 1U);
  EXPECT_EQ(mirror->callback_count, 1U);
  EXPECT_EQ(authority->last_coordinate.observed_at,
            authority_coordinate.observed_at);
  EXPECT_EQ(mirror->last_coordinate.observed_at,
            authority_coordinate.observed_at);
  EXPECT_EQ(authority->last_coordinate.causal_sequence,
            authority_coordinate.causal_sequence);
  EXPECT_EQ(mirror->last_coordinate.causal_sequence,
            authority_coordinate.causal_sequence);
}

/**
 * @brief Proves observer construction, ordering, and exhaustion are exact.
 * @throws GoogleTest assertion control and observer allocation failures.
 */
TEST(M1Profile, ObservationSinksShareOneNonzeroCausalSequence) {
  EXPECT_THROW(M1FairnessObservationCollector(0U), std::invalid_argument);
  M1FairnessObservationCollector collector(2U);
  const auto interactive =
      collector.make_sink(M1ObservedRequestTag::Interactive);
  const auto throughput =
      collector.make_sink(M1ObservedRequestTag::ThroughputGraphA);
  ASSERT_NE(interactive, nullptr);
  ASSERT_NE(throughput, nullptr);
  EXPECT_TRUE(interactive->observes_task_semantics());
  EXPECT_TRUE(throughput->observes_task_semantics());

  compute::ComputeRun interactive_run(make_observer_submission(
      "m1-observer-interactive", 1U, compute::ComputeRunQosClass::Interactive));
  compute::ComputeRun throughput_run(make_observer_submission(
      "m1-observer-throughput", 2U, compute::ComputeRunQosClass::Throughput));
  const compute::ComputeRunObservationCoordinate first =
      interactive->reserve_causal_coordinate();
  const compute::ComputeRunObservationCoordinate second =
      throughput->reserve_causal_coordinate();
  EXPECT_NE(first.causal_sequence, 0U);
  EXPECT_EQ(second.causal_sequence, first.causal_sequence + 1U);

  throughput->on_service_start(
      throughput_run.descriptor(),
      compute::ComputeRunTaskIdentity(throughput_run.descriptor().id(),
                                      compute::ComputeRunLocalTaskId(2U)),
      22U, compute::ComputeRunServiceStartObservation{true, true, true},
      second);
  interactive->on_service_start(
      interactive_run.descriptor(),
      compute::ComputeRunTaskIdentity(interactive_run.descriptor().id(),
                                      compute::ComputeRunLocalTaskId(1U)),
      11U, compute::ComputeRunServiceStartObservation{true, true, true}, first);

  const compute::ComputeRunObservationCoordinate third =
      throughput->reserve_causal_coordinate();
  throughput->on_service_start(
      interactive_run.descriptor(),
      compute::ComputeRunTaskIdentity(interactive_run.descriptor().id(),
                                      compute::ComputeRunLocalTaskId(3U)),
      33U, compute::ComputeRunServiceStartObservation{true, true, true}, third);
  EXPECT_THROW(collector.make_sink(static_cast<M1ObservedRequestTag>(255U)),
               std::invalid_argument);
  const M1FairnessObservationSnapshot snapshot = collector.snapshot();
  ASSERT_EQ(snapshot.events.size(), 2U);
  EXPECT_EQ(snapshot.events[0U].causal_sequence, first.causal_sequence);
  EXPECT_EQ(snapshot.events[0U].request_tag, M1ObservedRequestTag::Interactive);
  EXPECT_EQ(snapshot.events[0U].service_charge, 11U);
  EXPECT_EQ(snapshot.events[1U].causal_sequence, second.causal_sequence);
  EXPECT_EQ(snapshot.events[1U].request_tag,
            M1ObservedRequestTag::ThroughputGraphA);
  EXPECT_EQ(snapshot.events[1U].service_charge, 22U);
  EXPECT_TRUE(snapshot.overflowed);
  EXPECT_FALSE(snapshot.sequence_exhausted);
  EXPECT_TRUE(snapshot.qos_mismatch);
}

/**
 * @brief Proves terminal sequence and bounded capacity become sticky invalid.
 * @throws GoogleTest assertion control and observer allocation failures.
 */
TEST(M1Profile, ObservationExhaustionCannotWrapBackToPass) {
  M1FairnessObservationCollector collector(
      1U, std::numeric_limits<std::uint64_t>::max());
  const auto sink = collector.make_sink(M1ObservedRequestTag::Interactive);
  const compute::ComputeRunObservationCoordinate last =
      sink->reserve_causal_coordinate();
  const compute::ComputeRunObservationCoordinate exhausted =
      sink->reserve_causal_coordinate();
  const compute::ComputeRunObservationCoordinate still_exhausted =
      sink->reserve_causal_coordinate();
  EXPECT_EQ(last.causal_sequence, std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(exhausted.causal_sequence, 0U);
  EXPECT_EQ(still_exhausted.causal_sequence, 0U);
  const M1FairnessObservationSnapshot snapshot = collector.snapshot();
  EXPECT_TRUE(snapshot.sequence_exhausted);
  EXPECT_TRUE(snapshot.events.empty());
}

/**
 * @brief Proves concurrent callback publication terminates with unique slots.
 * @throws GoogleTest assertion control, thread, and allocation failures.
 */
TEST(M1Profile, ObservationCallbacksRemainFiniteUnderConcurrency) {
  constexpr std::size_t kThreadCount = 8U;
  constexpr std::size_t kEventsPerThread = 32U;
  M1FairnessObservationCollector collector(kThreadCount * kEventsPerThread);
  const auto sink = collector.make_sink(M1ObservedRequestTag::Interactive);
  compute::ComputeRun run(make_observer_submission(
      "m1-observer-concurrent", 50U, compute::ComputeRunQosClass::Interactive));
  std::atomic<bool> release{false};
  std::vector<std::thread> workers;
  workers.reserve(kThreadCount);
  for (std::size_t thread = 0U; thread < kThreadCount; ++thread) {
    workers.emplace_back([&, thread] {
      while (!release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t event = 0U; event < kEventsPerThread; ++event) {
        const compute::ComputeRunObservationCoordinate coordinate =
            sink->reserve_causal_coordinate();
        sink->on_service_start(
            run.descriptor(),
            compute::ComputeRunTaskIdentity(
                run.descriptor().id(),
                compute::ComputeRunLocalTaskId(thread * kEventsPerThread +
                                               event + 1U)),
            1U, compute::ComputeRunServiceStartObservation{true, true, true},
            coordinate);
      }
    });
  }
  release.store(true, std::memory_order_release);
  for (std::thread& worker : workers) {
    worker.join();
  }
  const M1FairnessObservationSnapshot snapshot = collector.snapshot();
  EXPECT_FALSE(snapshot.overflowed);
  EXPECT_FALSE(snapshot.sequence_exhausted);
  EXPECT_FALSE(snapshot.qos_mismatch);
  EXPECT_TRUE(snapshot.stable_publication_cut);
  EXPECT_EQ(snapshot.callback_entry_frontier, kThreadCount * kEventsPerThread);
  EXPECT_EQ(snapshot.callback_completion_frontier,
            kThreadCount * kEventsPerThread);
  EXPECT_EQ(snapshot.claimed_slot_frontier, kThreadCount * kEventsPerThread);
  EXPECT_EQ(snapshot.published_slot_frontier, kThreadCount * kEventsPerThread);
  ASSERT_EQ(snapshot.events.size(), kThreadCount * kEventsPerThread);
  for (std::size_t index = 1U; index < snapshot.events.size(); ++index) {
    EXPECT_LT(snapshot.events[index - 1U].causal_sequence,
              snapshot.events[index].causal_sequence);
  }
}

/**
 * @brief Proves a claimed-but-unpublished callback invalidates the M1 cut.
 * @throws GoogleTest assertion control, thread, or observer allocation
 * failures.
 */
TEST(M1Profile, ObservationBoundaryRejectsClaimedUnpublishedCallback) {
  const auto hook = std::make_shared<PausingPublicationHook>();
  EXPECT_THROW(M1FairnessObservationCollector(1U, 1U, nullptr),
               std::invalid_argument);
  M1FairnessObservationCollector collector(1U, 1U, hook);
  const auto sink = collector.make_sink(M1ObservedRequestTag::Interactive);
  compute::ComputeRun run(make_observer_submission(
      "m1-observer-paused", 51U, compute::ComputeRunQosClass::Interactive));
  const M1FairnessObservationSnapshot before = collector.snapshot();
  ASSERT_TRUE(before.stable_publication_cut);

  std::thread worker([&] {
    const compute::ComputeRunObservationCoordinate coordinate =
        sink->reserve_causal_coordinate();
    sink->on_service_start(
        run.descriptor(),
        compute::ComputeRunTaskIdentity(run.descriptor().id(),
                                        compute::ComputeRunLocalTaskId(1U)),
        1U, compute::ComputeRunServiceStartObservation{true, true, true},
        coordinate);
  });
  for (std::size_t attempt = 0U; attempt < 1000000U && !hook->claimed();
       ++attempt) {
    std::this_thread::yield();
  }
  if (!hook->claimed()) {
    hook->release();
    worker.join();
    FAIL() << "observer callback did not reach the deterministic claim hook";
    return;
  }

  const M1FairnessObservationSnapshot in_flight = collector.snapshot();
  EXPECT_FALSE(in_flight.stable_publication_cut);
  EXPECT_EQ(in_flight.callback_entry_frontier, 1U);
  EXPECT_EQ(in_flight.callback_completion_frontier, 0U);
  EXPECT_EQ(in_flight.claimed_slot_frontier, 1U);
  EXPECT_EQ(in_flight.published_slot_frontier, 0U);
  EXPECT_TRUE(in_flight.events.empty());
  EXPECT_FALSE(m1_observation_cut_unchanged(before, in_flight));

  M1ProtocolEvidenceInput protocol = make_passing_protocol();
  protocol.boundary_was_zero_duration =
      m1_observation_cut_unchanged(before, in_flight);
  protocol.raw_history_preserved = protocol.boundary_was_zero_duration;
  EXPECT_EQ(evaluate_m1_protocol(std::move(protocol)).verdict,
            I1Verdict::Invalid);

  hook->release();
  worker.join();
  const M1FairnessObservationSnapshot after = collector.snapshot();
  EXPECT_TRUE(after.stable_publication_cut);
  EXPECT_EQ(after.callback_entry_frontier, 1U);
  EXPECT_EQ(after.callback_completion_frontier, 1U);
  EXPECT_EQ(after.claimed_slot_frontier, 1U);
  EXPECT_EQ(after.published_slot_frontier, 1U);
  ASSERT_EQ(after.events.size(), 1U);
  EXPECT_FALSE(m1_observation_cut_unchanged(before, after));
}

/**
 * @brief Proves malformed evidence fails both exact delegated pair relations.
 * @throws GoogleTest assertion control only.
 */
TEST(M1Profile, EnvironmentPairsDelegateAndFailClosed) {
  const B1EnvironmentEvidence malformed;
  const M1EnvironmentPairCompatibility compatibility =
      evaluate_m1_environment_pairs(malformed, malformed, malformed);
  EXPECT_FALSE(compatibility.isolated_i1_base_only);
  EXPECT_FALSE(compatibility.isolated_b1_cap_eight);
  EXPECT_FALSE(compatibility.compatible());
}

/**
 * @brief Proves two ordinals use distinct base-only and full pair relations.
 * @throws Canonical fixture, observer, and GoogleTest failures unchanged.
 */
TEST(M1Profile, EnvironmentPairsPassForTwoCompleteSameOrdinalTriples) {
  for (const std::uint64_t ordinal : {1U, 3U}) {
    const auto evidence = make_environment_triple(ordinal);
    const M1EnvironmentPairCompatibility compatibility =
        evaluate_m1_environment_pairs(evidence[0U], evidence[1U], evidence[2U]);
    EXPECT_TRUE(compatibility.isolated_i1_base_only);
    EXPECT_TRUE(compatibility.isolated_b1_cap_eight);
    EXPECT_TRUE(compatibility.compatible());
  }
}

/**
 * @brief Proves storage drift affects only the full B1 relation and that
 * argument-role reversal cannot accidentally pass either delegated check.
 * @throws Canonical fixture, observer, and GoogleTest failures unchanged.
 */
TEST(M1Profile, EnvironmentPairsDistinguishStorageAndArgumentRoles) {
  auto evidence = make_environment_triple(2U);
  evidence[2U] = testing::synchronously_recast_b1_test_storage(
      std::move(evidence[2U]), "forgedfs");
  const M1EnvironmentPairCompatibility storage_drift =
      evaluate_m1_environment_pairs(evidence[0U], evidence[1U], evidence[2U]);
  EXPECT_TRUE(storage_drift.isolated_i1_base_only);
  EXPECT_FALSE(storage_drift.isolated_b1_cap_eight);
  EXPECT_FALSE(storage_drift.compatible());

  const M1EnvironmentPairCompatibility reversed =
      evaluate_m1_environment_pairs(evidence[1U], evidence[0U], evidence[2U]);
  EXPECT_FALSE(reversed.isolated_i1_base_only);
  EXPECT_FALSE(reversed.isolated_b1_cap_eight);
  EXPECT_FALSE(reversed.compatible());
}

}  // namespace
}  // namespace ps::benchmark
