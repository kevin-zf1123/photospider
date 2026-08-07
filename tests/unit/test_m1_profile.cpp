/**
 * @file test_m1_profile.cpp
 * @brief Verifies deterministic M1 timeline, fairness, and observer contracts.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/m1_profile.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {
namespace {

/**
 * @brief Builds one complete passing inner-fairness input.
 * @return Exact 30-window evidence with every independent guard passing.
 * @throws std::bad_alloc when vector storage allocates.
 */
M1FairnessEvidenceInput make_passing_fairness_input() {
  M1FairnessEvidenceInput input;
  input.progress_windows.assign(kM1MeasuredWindowCount,
                                M1ThroughputProgressSample{0.20, 1.0});
  input.graph_service_windows.assign(kM1MeasuredWindowCount,
                                     M1GraphServiceWindow{true, 100U, 100U});
  for (std::size_t group = 0U; group < 3U; ++group) {
    input.class_starts.push_back(
        M1ClassStartSample{compute::ComputeRunQosClass::Interactive, true});
  }
  input.class_starts.push_back(
      M1ClassStartSample{compute::ComputeRunQosClass::Throughput, true});
  input.headroom_admissions = M1HeadroomAdmissionEvidence{
      kM1MeasuredI1AttemptCount, kM1MeasuredI1AttemptCount, 0U};
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
  input.progress_windows[0U].measured_rate = 0.0;
  input.progress_windows[1U].measured_rate = 0.19;
  for (std::size_t index = 2U; index < input.progress_windows.size(); ++index) {
    input.progress_windows[index].measured_rate = 1.0;
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
  input.graph_service_windows.assign(kM1MeasuredWindowCount,
                                     M1GraphServiceWindow{true, 100U, 1U});
  input.class_starts.clear();
  for (std::size_t index = 0U; index < 4U; ++index) {
    input.class_starts.push_back(
        M1ClassStartSample{compute::ComputeRunQosClass::Interactive, true});
  }
  input.class_starts.push_back(
      M1ClassStartSample{compute::ComputeRunQosClass::Throughput, true});

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
 * @brief Proves headroom and latency failures cannot substitute for each other.
 * @throws GoogleTest assertion control and evaluator allocation failures.
 */
TEST(M1Profile, KeepsHeadroomAndLatencyVerdictsNonSubstitutable) {
  M1FairnessEvidenceInput input = make_passing_fairness_input();
  input.headroom_admissions.throughput_headroom_failures = 1U;
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
  input.graph_service_windows.front() = M1GraphServiceWindow{true, 0U, 0U};
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
      22U, second);
  interactive->on_service_start(
      interactive_run.descriptor(),
      compute::ComputeRunTaskIdentity(interactive_run.descriptor().id(),
                                      compute::ComputeRunLocalTaskId(1U)),
      11U, first);

  const compute::ComputeRunObservationCoordinate third =
      throughput->reserve_causal_coordinate();
  throughput->on_service_start(
      interactive_run.descriptor(),
      compute::ComputeRunTaskIdentity(interactive_run.descriptor().id(),
                                      compute::ComputeRunLocalTaskId(3U)),
      33U, third);
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

}  // namespace
}  // namespace ps::benchmark
