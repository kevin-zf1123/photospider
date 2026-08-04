#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/i1_profile.hpp"

namespace ps::benchmark {
namespace {

/**
 * @brief Captures private QoS facts from one fake Host admission.
 * @throws Nothing for value construction and copying.
 */
struct CapturedAdmission final {
  /** @brief Exact private QoS received by the final call. */
  compute::ComputeRunQos qos;

  /** @brief Ordinary request after collector normalization. */
  HostComputeRequest request;

  /** @brief Whether a non-null observation-only sink was supplied. */
  bool has_observation_sink = false;

  /** @brief Exact pre-call coordinate delivered at Host invocation. */
  std::optional<I1AcceptedCoordinate> accepted_coordinate;
};

/**
 * @brief Deterministic I1 Host fake used only for admission-boundary tests.
 *
 * The fake records final-call values and returns a preselected scheduling
 * status. It does not emulate Kernel, scheduling, cancellation, or graph state;
 * real-path behavior is covered separately by embedded integration tests.
 *
 * @throws std::bad_alloc when captured request/status storage allocates.
 */
class RecordingI1Host final : public I1Host {
 public:
  /** @copydoc I1Host::compute_i1_async */
  Result<std::future<OperationStatus>> compute_i1_async(
      I1HostComputeRequest request) override {
    admissions.push_back(CapturedAdmission{request.qos, request.request,
                                           request.observation_sink != nullptr,
                                           request.accepted_coordinate});
    Result<std::future<OperationStatus>> result;
    result.status = schedule_status;
    if (result.status.ok) {
      std::promise<OperationStatus> promise;
      result.value = promise.get_future();
      promise.set_value(settlement_status);
    }
    return result;
  }

  /** @copydoc I1Host::i1_execution_snapshot */
  I1ExecutionSnapshot i1_execution_snapshot(std::uint64_t after_cursor,
                                            std::size_t limit) const override {
    static_cast<void>(after_cursor);
    static_cast<void>(limit);
    return I1ExecutionSnapshot{};
  }

  /** @brief Scheduling status returned directly by the final Host call. */
  OperationStatus schedule_status;

  /** @brief Later status fulfilled through the accepted settlement future. */
  OperationStatus settlement_status;

  /** @brief Complete ordered list of final Host calls. */
  std::vector<CapturedAdmission> admissions;
};

/**
 * @brief Constructs a minimal ordinary request for fake-boundary verification.
 * @param edit_index Frozen edit whose Region is attached.
 * @return Host request ready for collector normalization.
 * @throws std::out_of_range for an invalid edit index.
 */
HostComputeRequest make_test_request(std::size_t edit_index) {
  HostComputeRequest request;
  request.session = GraphSessionId{"i1-test-session"};
  request.node = NodeId{4};
  request.dirty_roi = i1_edit_region(edit_index);
  return request;
}

/**
 * @brief Creates one edit-specific preallocated observation sink.
 * @param collector Episode observation store that outlives the sink.
 * @param edit_index Frozen edit identity.
 * @return Shared observation-only sink.
 * @throws The collector's index/allocation errors unchanged.
 */
std::shared_ptr<compute::ComputeRunObservationSink> make_test_sink(
    I1EpisodeObservationCollector& collector, std::size_t edit_index) {
  return collector.make_edit_sink(edit_index);
}

/**
 * @brief Preserves the exact pre-call coordinate and deadline on Host success.
 * @throws Nothing when deterministic fake inputs satisfy the frozen contract.
 */
TEST(I1AcceptedBoundaryCollector, SuccessfulCallUsesPreCallCoordinate) {
  RecordingI1Host host;
  I1EpisodeObservationCollector observations;
  const auto origin = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(1000000000));
  const auto nominal = checked_i1_time_add(origin, kI1MeasurementStartOffset);
  const auto admission = checked_i1_time_add(nominal, kI1AdmissionLateness);
  const auto returned =
      checked_i1_time_add(admission, std::chrono::nanoseconds(123));
  std::vector<std::chrono::steady_clock::time_point> samples{admission,
                                                             returned};
  std::size_t sample_index = 0U;
  std::vector<std::chrono::steady_clock::time_point> sleeps;
  I1AcceptedBoundaryCollector collector(
      host, [&samples, &sample_index] { return samples.at(sample_index++); },
      [&sleeps](auto target) { sleeps.push_back(target); }, 7U);

  I1EditAdmissionResult result = collector.admit_edit(
      origin, 11U, make_test_request(11U), make_test_sink(observations, 11U));

  ASSERT_EQ(sleeps.size(), 1U);
  EXPECT_EQ(sleeps.front(), nominal);
  EXPECT_TRUE(result.admission_attempted);
  EXPECT_TRUE(result.admission_window_valid);
  ASSERT_TRUE(result.reserved_event_sequence.has_value());
  EXPECT_EQ(*result.reserved_event_sequence, 7U);
  ASSERT_TRUE(result.deadline.has_value());
  EXPECT_EQ(*result.deadline,
            checked_i1_time_add(admission, kI1DeadlineBudget));
  ASSERT_TRUE(result.host_return.has_value());
  EXPECT_EQ(result.host_return->return_time, returned);
  EXPECT_TRUE(result.host_return->status.ok);
  EXPECT_TRUE(result.host_return->future_valid);
  ASSERT_TRUE(result.accepted_coordinate.has_value());
  EXPECT_EQ(result.accepted_coordinate->admission_time(), admission);
  EXPECT_EQ(result.accepted_coordinate->event_sequence(), 7U);
  ASSERT_TRUE(result.settlement.valid());
  EXPECT_TRUE(result.settlement.get().ok);

  ASSERT_EQ(host.admissions.size(), 1U);
  const CapturedAdmission& captured = host.admissions.front();
  EXPECT_EQ(captured.qos.service_class,
            compute::ComputeRunQosClass::Interactive);
  EXPECT_EQ(captured.qos.deadline, result.deadline);
  EXPECT_EQ(captured.qos.weight, 1U);
  EXPECT_EQ(captured.qos.maximum_parallelism, std::optional<std::uint32_t>(8U));
  EXPECT_TRUE(captured.request.execution.parallel);
  EXPECT_EQ(captured.request.execution.maximum_parallelism,
            std::optional<std::uint32_t>(8U));
  EXPECT_EQ(captured.request.intent,
            std::optional<ComputeIntent>(ComputeIntent::GlobalHighPrecision));
  EXPECT_TRUE(captured.has_observation_sink);
  ASSERT_TRUE(captured.accepted_coordinate.has_value());
  EXPECT_EQ(captured.accepted_coordinate->admission_time(), admission);
  EXPECT_EQ(captured.accepted_coordinate->event_sequence(), 7U);
}

/**
 * @brief Retains reservation/return facts without accepting a failed call.
 * @throws Nothing when the fake returns one canonical Graph failure status.
 */
TEST(I1AcceptedBoundaryCollector, FailedHostCallCreatesNoAcceptedEvent) {
  RecordingI1Host host;
  host.schedule_status.ok = false;
  host.schedule_status.domain = OperationErrorDomain::Graph;
  host.schedule_status.code =
      static_cast<std::int32_t>(GraphErrc::ComputeError);
  host.schedule_status.name = "compute-error";
  I1EpisodeObservationCollector observations;
  const auto origin = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(2000000000));
  std::vector<std::chrono::steady_clock::time_point> samples{
      origin, checked_i1_time_add(origin, std::chrono::nanoseconds(50))};
  std::size_t sample_index = 0U;
  I1AcceptedBoundaryCollector collector(
      host, [&samples, &sample_index] { return samples.at(sample_index++); },
      [](auto) {}, 19U);

  I1EditAdmissionResult result = collector.admit_edit(
      origin, 0U, make_test_request(0U), make_test_sink(observations, 0U));

  EXPECT_TRUE(result.admission_attempted);
  EXPECT_TRUE(result.admission_window_valid);
  EXPECT_EQ(result.reserved_event_sequence, std::optional<std::uint64_t>(19U));
  EXPECT_TRUE(result.deadline.has_value());
  ASSERT_TRUE(result.host_return.has_value());
  EXPECT_FALSE(result.host_return->status.ok);
  EXPECT_FALSE(result.host_return->future_valid);
  EXPECT_FALSE(result.accepted_coordinate.has_value());
  EXPECT_FALSE(result.settlement.valid());
  ASSERT_EQ(host.admissions.size(), 1U);
  EXPECT_TRUE(observations.snapshot().current_generations.empty());
  ASSERT_TRUE(host.admissions.front().accepted_coordinate.has_value());
  EXPECT_EQ(host.admissions.front().accepted_coordinate->admission_time(),
            origin);
  EXPECT_EQ(host.admissions.front().accepted_coordinate->event_sequence(), 19U);
}

/**
 * @brief Rejects early/late samples without calls, reservations, or backfill.
 * @throws Nothing when scripted samples cover both exclusive invalid sides.
 */
TEST(I1AcceptedBoundaryCollector, InvalidWindowsNeverCallOrReserve) {
  RecordingI1Host host;
  I1EpisodeObservationCollector observations;
  const auto origin = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(3000000000));
  const auto second_nominal = checked_i1_time_add(origin, kI1EditStride);
  std::vector<std::chrono::steady_clock::time_point> samples{
      origin - std::chrono::nanoseconds(1),
      checked_i1_time_add(second_nominal,
                          kI1AdmissionLateness + std::chrono::nanoseconds(1))};
  std::size_t sample_index = 0U;
  std::vector<std::chrono::steady_clock::time_point> sleeps;
  I1AcceptedBoundaryCollector collector(
      host, [&samples, &sample_index] { return samples.at(sample_index++); },
      [&sleeps](auto target) { sleeps.push_back(target); });

  I1EditAdmissionResult early = collector.admit_edit(
      origin, 0U, make_test_request(0U), make_test_sink(observations, 0U));
  I1EditAdmissionResult late = collector.admit_edit(
      origin, 1U, make_test_request(1U), make_test_sink(observations, 1U));

  EXPECT_TRUE(early.admission_attempted);
  EXPECT_TRUE(late.admission_attempted);
  EXPECT_FALSE(early.admission_window_valid);
  EXPECT_FALSE(late.admission_window_valid);
  EXPECT_FALSE(early.reserved_event_sequence.has_value());
  EXPECT_FALSE(late.reserved_event_sequence.has_value());
  EXPECT_FALSE(early.deadline.has_value());
  EXPECT_FALSE(late.deadline.has_value());
  EXPECT_FALSE(early.host_return.has_value());
  EXPECT_FALSE(late.host_return.has_value());
  EXPECT_TRUE(host.admissions.empty());
  EXPECT_TRUE(observations.snapshot().current_generations.empty());
  ASSERT_EQ(sleeps.size(), 2U);
  EXPECT_EQ(sleeps[0], origin);
  EXPECT_EQ(sleeps[1], second_nominal);
}

/**
 * @brief Accepts both inclusive window endpoints with increasing sequences.
 * @throws Nothing when fake calls and settlement futures succeed.
 */
TEST(I1AcceptedBoundaryCollector, AdmissionWindowEndpointsAreInclusive) {
  RecordingI1Host host;
  I1EpisodeObservationCollector observations;
  const auto origin = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(4000000000));
  const auto second_nominal = checked_i1_time_add(origin, kI1EditStride);
  std::vector<std::chrono::steady_clock::time_point> samples{
      origin, origin, checked_i1_time_add(second_nominal, kI1AdmissionLateness),
      checked_i1_time_add(second_nominal, kI1AdmissionLateness)};
  std::size_t sample_index = 0U;
  I1AcceptedBoundaryCollector collector(
      host, [&samples, &sample_index] { return samples.at(sample_index++); },
      [](auto) {}, 41U);

  I1EditAdmissionResult first = collector.admit_edit(
      origin, 0U, make_test_request(0U), make_test_sink(observations, 0U));
  I1EditAdmissionResult second = collector.admit_edit(
      origin, 1U, make_test_request(1U), make_test_sink(observations, 1U));

  EXPECT_TRUE(first.admission_window_valid);
  EXPECT_TRUE(second.admission_window_valid);
  EXPECT_EQ(first.accepted_coordinate->event_sequence(), 41U);
  EXPECT_EQ(second.accepted_coordinate->event_sequence(), 42U);
  EXPECT_EQ(host.admissions.size(), 2U);
}

/**
 * @brief Uses UINT64_MAX once and rejects sequence reuse before a second call.
 * @throws Nothing when exhaustion is reported as the required overflow error.
 */
TEST(I1AcceptedBoundaryCollector, SequenceExhaustionPreventsSecondHostCall) {
  RecordingI1Host host;
  I1EpisodeObservationCollector observations;
  const auto origin = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(5000000000));
  const auto second_nominal = checked_i1_time_add(origin, kI1EditStride);
  std::vector<std::chrono::steady_clock::time_point> samples{origin, origin,
                                                             second_nominal};
  std::size_t sample_index = 0U;
  I1AcceptedBoundaryCollector collector(
      host, [&samples, &sample_index] { return samples.at(sample_index++); },
      [](auto) {}, std::numeric_limits<std::uint64_t>::max());

  I1EditAdmissionResult first = collector.admit_edit(
      origin, 0U, make_test_request(0U), make_test_sink(observations, 0U));
  ASSERT_TRUE(first.accepted_coordinate.has_value());
  EXPECT_EQ(first.accepted_coordinate->event_sequence(),
            std::numeric_limits<std::uint64_t>::max());
  EXPECT_THROW(collector.admit_edit(origin, 1U, make_test_request(1U),
                                    make_test_sink(observations, 1U)),
               std::overflow_error);
  EXPECT_EQ(host.admissions.size(), 1U);
}

/**
 * @brief Derives the sole grid, Regions, guard, and frozen equal-time order.
 * @throws Nothing when all exact constants and checked boundaries agree.
 */
TEST(I1FrozenArithmetic, GridRegionsAndTieOrderRemainExact) {
  const auto grid_origin = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(6000000000));
  EXPECT_EQ(i1_episode_origin(grid_origin, 0U), grid_origin);
  EXPECT_EQ(i1_episode_origin(grid_origin, 220U),
            checked_i1_time_add(grid_origin,
                                std::chrono::nanoseconds(220LL * 750000000LL)));
  EXPECT_EQ(i1_terminal_boundary(grid_origin),
            checked_i1_time_add(grid_origin,
                                std::chrono::nanoseconds(221LL * 750000000LL)));
  EXPECT_EQ(classify_i1_slot(0U),
            (std::pair<I1EpisodePhase, std::size_t>{I1EpisodePhase::Cold, 0U}));
  EXPECT_EQ(classify_i1_slot(20U), (std::pair<I1EpisodePhase, std::size_t>{
                                       I1EpisodePhase::Warmup, 19U}));
  EXPECT_EQ(classify_i1_slot(21U), (std::pair<I1EpisodePhase, std::size_t>{
                                       I1EpisodePhase::Measured, 0U}));
  EXPECT_EQ(classify_i1_slot(220U), (std::pair<I1EpisodePhase, std::size_t>{
                                        I1EpisodePhase::Measured, 199U}));
  EXPECT_EQ(i1_edit_region(0U), (PixelRect{0, 0, 256, 256}));
  EXPECT_EQ(i1_edit_region(11U), (PixelRect{768, 512, 256, 256}));
  EXPECT_EQ(kI1MeasurementEndOffset + kI1NextOriginGuard, kI1EpisodeStride);
  const auto latest_final_admission = checked_i1_time_add(
      checked_i1_time_add(grid_origin, kI1MeasurementStartOffset),
      kI1AdmissionLateness);
  EXPECT_EQ(checked_i1_time_add(latest_final_admission, kI1DeadlineBudget),
            checked_i1_time_add(grid_origin, kI1LatestFinalDeadlineOffset));
  EXPECT_EQ(
      i1_measurement_start_tie_rank(I1MeasurementStartEventKind::NominalMarker),
      0);
  EXPECT_EQ(i1_measurement_start_tie_rank(
                I1MeasurementStartEventKind::AcceptedAdmission),
            1);
  EXPECT_THROW(i1_episode_origin(grid_origin, kI1GridSlotCount),
               std::out_of_range);
  EXPECT_THROW(checked_i1_time_add(std::chrono::steady_clock::time_point::max(),
                                   std::chrono::nanoseconds(1)),
               std::overflow_error);
}

/**
 * @brief Proves the derived service-start store is lossless at its exact bound.
 * @throws Allocation and ComputeRun construction failures unchanged.
 * @note Synthetic callbacks exercise only the fixed collector slots; this is
 * not the 221-slot benchmark workload and performs no product computation.
 */
TEST(I1EpisodeObservationCollector,
     DerivedServiceStartCapacityFailsClosedOnlyAfterBoundary) {
  EXPECT_EQ(kI1FrozenTilesPerCurveNode, 64U);
  EXPECT_EQ(kI1MaximumServiceStartsPerRun, 257U);
  EXPECT_EQ(kI1EpisodeServiceStartCapacity, 3084U);

  I1EpisodeObservationCollector collector;
  std::shared_ptr<compute::ComputeRunObservationSink> sink =
      collector.make_edit_sink(0U);
  compute::ComputeRunSubmission submission{
      "i1-service-start-capacity",
      GraphInstanceId{7001U},
      GraphRevision{7001U},
      kI1TargetNodeId,
      ComputeIntent::GlobalHighPrecision,
      compute::ComputeRunQuality::Full,
      compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                             std::nullopt, 1U, 8U},
      compute::SupersessionIdentity{
          compute::SupersessionKey(kI1TargetNodeId,
                                   ComputeIntent::GlobalHighPrecision),
          compute::SupersessionGeneration(1U)},
      nullptr};
  compute::ComputeRun run(std::move(submission));
  compute::ComputeRunLease lease = run.acquire_lease();

  for (std::size_t index = 0U; index < kI1EpisodeServiceStartCapacity;
       ++index) {
    const compute::ComputeRunObservationCoordinate coordinate =
        sink->reserve_causal_coordinate();
    sink->on_service_start(lease.descriptor(), lease.task_identity(index), 1U,
                           coordinate);
  }
  const I1EpisodeObservationSnapshot at_capacity = collector.snapshot();
  EXPECT_FALSE(at_capacity.overflowed);
  EXPECT_EQ(at_capacity.service_starts.size(), kI1EpisodeServiceStartCapacity);

  const compute::ComputeRunObservationCoordinate overflow_coordinate =
      sink->reserve_causal_coordinate();
  sink->on_service_start(lease.descriptor(),
                         lease.task_identity(kI1EpisodeServiceStartCapacity),
                         1U, overflow_coordinate);
  const I1EpisodeObservationSnapshot overflowed = collector.snapshot();
  EXPECT_TRUE(overflowed.overflowed);
  EXPECT_EQ(overflowed.service_starts.size(), kI1EpisodeServiceStartCapacity);
}

/**
 * @brief Freezes the exact source/serial-transform document and Host request.
 * @throws Nothing when owned YAML/request construction matches the workload.
 */
TEST(I1FrozenWorkload, GraphMutationAndRequestRemainExact) {
  const std::string graph = i1_frozen_graph_yaml();
  EXPECT_NE(graph.find("width: 2048\n    height: 2048\n    channels: 4\n"
                       "    seed: 0\n"),
            std::string::npos);
  EXPECT_NE(graph.find("id: 1\n  name: i1_curve_one"), std::string::npos);
  EXPECT_NE(graph.find("id: 4\n  name: i1_curve_four"), std::string::npos);
  EXPECT_NE(graph.find("k: 0.80"), std::string::npos);
  EXPECT_NE(graph.find("k: 1.00"), std::string::npos);
  EXPECT_NE(graph.find("k: 1.20"), std::string::npos);
  EXPECT_NE(graph.find("k: 1.40"), std::string::npos);

  const std::string final_edit = i1_edit_node_one_yaml(11U);
  EXPECT_NE(final_edit.find("from_node_id: 0"), std::string::npos);
  EXPECT_NE(final_edit.find("k: 1.04\n"), std::string::npos);
  EXPECT_THROW(i1_edit_node_one_yaml(kI1EditCount), std::out_of_range);

  const GraphSessionId session{"i1-frozen-workload"};
  const HostComputeRequest request = make_i1_host_compute_request(session, 11U);
  EXPECT_EQ(request.session.value, session.value);
  EXPECT_EQ(request.node.value, kI1TargetNodeId);
  EXPECT_EQ(request.cache.precision, "fp32");
  EXPECT_TRUE(request.cache.force_recache);
  EXPECT_TRUE(request.cache.disable_disk_cache);
  EXPECT_TRUE(request.cache.nosave);
  EXPECT_TRUE(request.execution.parallel);
  EXPECT_TRUE(request.execution.quiet);
  EXPECT_EQ(request.execution.maximum_parallelism,
            std::optional<std::uint32_t>{8U});
  EXPECT_EQ(request.intent,
            std::optional<ComputeIntent>{ComputeIntent::GlobalHighPrecision});
  EXPECT_EQ(request.dirty_roi, (PixelRect{768, 512, 256, 256}));
}

}  // namespace
}  // namespace ps::benchmark
