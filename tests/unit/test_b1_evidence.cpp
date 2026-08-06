/**
 * @file test_b1_evidence.cpp
 * @brief Verifies B1 occurrence joins and four independent verdict axes.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/b1_evidence.hpp"  // NOLINT(build/include_subdir)
#include "support/b1_test_environment.hpp"
#include "verification/b1_evidence_json.hpp"

namespace ps::benchmark {
namespace {

/** @brief Frozen Host limits duplicated as an independent test oracle. */
constexpr ResourceVector kExpectedHostLimits{
    32U, 1073741824U, 536870912U, 65536U,
    268435456U};  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Builds one coherent quiescent Compute I/O snapshot.
 * @param active_tasks Current charged task count.
 * @param active_bytes Current charged planned bytes.
 * @return Exact frozen-limit snapshot.
 * @throws Nothing.
 */
execution::ComputeIoExecutorSnapshot make_io_snapshot(
    std::uint64_t active_tasks = 0U, std::uint64_t active_bytes = 0U) noexcept {
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
 * @brief Builds one lossless authority-free execution snapshot.
 * @return Frozen limits, zero reservations, stable lifecycle identity, and
 * quiescent Compute I/O.
 * @throws Nothing for aggregate construction.
 */
B1ExecutionSnapshot make_execution_snapshot() {
  B1ExecutionSnapshot snapshot;
  snapshot.host_resources = ResourceLedger::Snapshot{
      kExpectedHostLimits, ResourceVector{}, ResourceVector{}};
  snapshot.lifecycle.service_instance_id = 17U;
  snapshot.lifecycle.telemetry_epoch = 23U;
  snapshot.lifecycle.snapshot_cut = 1U;
  snapshot.lifecycle.next_sequence = 2U;
  snapshot.lifecycle.next_cursor = 1U;
  snapshot.compute_io = make_io_snapshot();
  return snapshot;
}

/**
 * @brief Computes the stable B1 commit id independently for a test receipt.
 * @param job Complete valid occurrence.
 * @return Exact lowercase SHA-256 commit id.
 * @throws Digest, validation, or allocation errors unchanged.
 */
std::string test_commit_id(const B1JobInstance& job) {
  B1Sha256 hash;
  hash.update("execution-profile-output-commit-id-v1\n");
  hash.update(encode_b1_job_instance(job));
  return b1_digest_hex(hash.finish());
}

/**
 * @brief Returns the graph-local contiguous offer ordinal for one fixture.
 * @param job Complete isolated occurrence.
 * @return Exact ordinal across cold/warmup/measured offers.
 * @throws std::invalid_argument for an unsupported occurrence.
 */
std::uint64_t test_offer_ordinal(const B1JobInstance& job) {
  if (job.phase == B1JobPhase::Cold) {
    return 0U;
  }
  if (job.phase == B1JobPhase::Warmup) {
    return job.job_index == 253U ? 0U : 1U;
  }
  if (job.phase == B1JobPhase::Measured) {
    return 2U + job.job_index / 2U;
  }
  throw std::invalid_argument("unknown B1 test occurrence");
}

/**
 * @brief Builds exact two-task successful Compute I/O observations.
 * @param job Complete immutable occurrence.
 * @return Initial, two admissions, two settlements, and final snapshots.
 * @throws Validation or allocation errors unchanged.
 */
std::vector<B1ComputeIoObservation> make_io_observations(
    const B1JobInstance& job) {
  const B1IoTaskIdentity payload{job, B1IoStage::PayloadStage, 0U};
  const B1IoTaskIdentity manifest{job, B1IoStage::ManifestCommit, 0U};
  const std::uint64_t manifest_bytes = b1_manifest_length(job.job_index);
  return {
      {B1IoObservationPoint::Initial, std::nullopt, 0U, std::nullopt,
       std::nullopt, make_io_snapshot()},
      {B1IoObservationPoint::AcceptedAdmission, payload, kB1PayloadBytes,
       execution::ComputeIoAdmissionStatus::Accepted, std::nullopt,
       make_io_snapshot(1U, kB1PayloadBytes)},
      {B1IoObservationPoint::Settlement, payload, kB1PayloadBytes,
       execution::ComputeIoAdmissionStatus::Accepted,
       execution::ComputeIoCompletionStatus::Succeeded, make_io_snapshot()},
      {B1IoObservationPoint::AcceptedAdmission, manifest, manifest_bytes,
       execution::ComputeIoAdmissionStatus::Accepted, std::nullopt,
       make_io_snapshot(1U, manifest_bytes)},
      {B1IoObservationPoint::Settlement, manifest, manifest_bytes,
       execution::ComputeIoAdmissionStatus::Accepted,
       execution::ComputeIoCompletionStatus::Succeeded, make_io_snapshot()},
      {B1IoObservationPoint::Final, std::nullopt, 0U, std::nullopt,
       std::nullopt, make_io_snapshot()},
  };
}

/**
 * @brief Builds a complete successful job with all five digest domains.
 * @param job Complete valid occurrence.
 * @param offered_at Deterministic monotonic offer cut.
 * @return Closed raw job evidence accepted by the evaluator.
 * @throws Validation, digest, and allocation errors unchanged.
 */
B1JobEvidence make_valid_job(const B1JobInstance& job,
                             std::chrono::steady_clock::time_point offered_at) {
  B1JobEvidence evidence;
  evidence.job = job;
  evidence.producer_offer_ordinal = test_offer_ordinal(job);
  evidence.offered_at = offered_at;
  evidence.endpoint_at = offered_at + std::chrono::milliseconds(1);
  evidence.run_succeeded = true;
  evidence.execution_before = make_execution_snapshot();
  evidence.execution_after = make_execution_snapshot();
  evidence.golden = b1_frozen_job_golden(job.job_index);
  evidence.semantic_trace = encode_b1_semantic_trace(
      make_b1_success_semantic_records(b1_frozen_semantic_plan(job.job_index)));
  evidence.semantic_trace_digest = b1_sha256(evidence.semantic_trace);

  const std::uint64_t run_id = job.job_index + 1U;
  evidence.physical_trace.job = job;
  evidence.physical_trace.current_generations.push_back(
      B1ObservedCurrentGeneration{
          1U, compute::ComputeRunObservationCoordinate{offered_at, 1U}});
  for (std::uint64_t task = 0U; task < kB1TasksPerJob; ++task) {
    evidence.physical_trace.service_starts.push_back(B1ObservedServiceStart{
        run_id, task, 1U,
        compute::ComputeRunQos{compute::ComputeRunQosClass::Throughput,
                               std::nullopt, 1U,
                               static_cast<std::uint32_t>(job.run_cap)},
        compute::ComputeRunObservationCoordinate{offered_at, task + 2U}});
  }
  evidence.physical_trace.visible = B1ObservedRunTransition{
      run_id, compute::ComputeRunObservationCoordinate{offered_at, 259U}};
  evidence.physical_trace.terminal_kind =
      compute::ComputeRunTerminalKind::Succeeded;
  evidence.physical_trace.terminal = B1ObservedRunTransition{
      run_id, compute::ComputeRunObservationCoordinate{offered_at, 260U}};
  evidence.physical_trace.quiescent = B1ObservedRunTransition{
      run_id, compute::ComputeRunObservationCoordinate{offered_at, 261U}};
  evidence.physical_trace.resource_settled = B1ObservedRunTransition{
      run_id, compute::ComputeRunObservationCoordinate{offered_at, 262U}};
  evidence.physical_trace.visible_content_digest =
      ContentDigestResult{ContentDigestState::Available,
                          evidence.golden.logical_digest,
                          {}};

  const std::string commit_id = test_commit_id(job);
  const std::string manifest =
      b1_artifact_manifest(job.job_index, evidence.golden.raw_payload_digest);
  evidence.output.status = B1OutputCommitStatus::Succeeded;
  evidence.output.receipt = B1OutputCommitReceipt{
      commit_id,
      std::filesystem::path("/tmp/photospider-b1-test-output"),
      std::filesystem::path("occurrence-" + commit_id),
      job,
      "dense-tensor-hwc-fp32-rgba-2048x2048",
      evidence.golden.logical_digest,
      1U,
      "output.rgba32le",
      "manifest.txt",
      kB1PayloadBytes,
      b1_manifest_length(job.job_index),
      evidence.golden.raw_payload_digest,
      b1_sha256(manifest),
      B1OutputDurability::CrashDurable,
      B1OutputDurability::CrashDurable,
      "dev=1;ino=1"};
  evidence.output.io_observations = make_io_observations(job);
  return evidence;
}

/**
 * @brief Builds one complete valid 34-occurrence isolated row input.
 * @param run_cap Exact cap one or eight.
 * @param replicate_ordinal Exact replicate one through three.
 * @return Evaluator-ready row input.
 * @throws Validation, digest, and allocation errors unchanged.
 */
B1InnerRowInput make_valid_row_input(std::uint64_t run_cap,
                                     std::uint64_t replicate_ordinal) {
  B1InnerRowInput input;
  input.replicate_ordinal = replicate_ordinal;
  input.run_cap = run_cap;
  input.environment =
      testing::make_b1_test_environment(run_cap, replicate_ordinal);
  input.measurement_start =
      std::chrono::steady_clock::time_point(std::chrono::seconds(100));
  input.measurement_end = input.measurement_start + std::chrono::seconds(1);
  input.initial_snapshot = make_execution_snapshot();
  input.final_snapshot = make_execution_snapshot();
  std::vector<B1JobInstance> jobs;
  jobs.push_back(B1JobInstance{kB1WorkloadId, replicate_ordinal,
                               B1JobPhase::Cold, 0U, 252U, run_cap});
  for (const std::uint64_t seed : {253U, 254U, 255U}) {
    jobs.push_back(B1JobInstance{kB1WorkloadId, replicate_ordinal,
                                 B1JobPhase::Warmup, 0U, seed, run_cap});
  }
  for (std::uint64_t seed = 0U; seed < 30U; ++seed) {
    jobs.push_back(B1JobInstance{kB1WorkloadId, replicate_ordinal,
                                 B1JobPhase::Measured, 0U, seed, run_cap});
  }
  input.jobs.reserve(jobs.size());
  for (std::size_t index = 0U; index < jobs.size(); ++index) {
    input.jobs.push_back(make_valid_job(
        jobs[index],
        input.measurement_start + std::chrono::microseconds(index)));
  }
  return input;
}

/**
 * @brief Proves a complete row passes all four axes with exact accounting.
 * @throws Test fixture, evaluation, and framework failures unchanged.
 */
TEST(B1Evidence, CompleteExactRowPassesEveryIndependentAxis) {
  const B1InnerRow row = evaluate_b1_inner_row(make_valid_row_input(8U, 1U));
  EXPECT_TRUE(row.validity_reasons.empty());
  EXPECT_EQ(row.verified_measured_jobs, 30U);
  EXPECT_EQ(row.successful_site_operations, 503316480U);
  ASSERT_TRUE(row.throughput_mpix_ops_per_second.has_value());
  EXPECT_DOUBLE_EQ(*row.throughput_mpix_ops_per_second, 503.31648);
  EXPECT_EQ(row.all_started_service, 34U * kB1TasksPerJob);
  EXPECT_EQ(row.discarded_started_service, 0U);
  ASSERT_TRUE(row.discarded_started_service_ratio.has_value());
  EXPECT_DOUBLE_EQ(*row.discarded_started_service_ratio, 0.0);
  EXPECT_EQ(row.compute_io_task_high_water, 1U);
  EXPECT_EQ(row.compute_io_planned_byte_high_water, kB1PayloadBytes);
  EXPECT_EQ(row.throughput_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.determinism_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves verification JSON retains the complete closed row and no outer
 * claim.
 * @throws Test fixture, evaluation, JSON, and framework failures unchanged.
 */
TEST(B1Evidence, VerificationJsonRetainsAllOccurrencesAndClosedIdentity) {
  const B1InnerRow row = evaluate_b1_inner_row(make_valid_row_input(8U, 1U));
  const nlohmann::json encoded = b1_inner_row_json(row);
  EXPECT_EQ(encoded.at("schema"), kB1InnerRowSchema);
  EXPECT_EQ(encoded.at("schema_version"), kB1InnerRowSchemaVersion);
  EXPECT_EQ(encoded.at("evidence").at("jobs").size(), 34U);
  EXPECT_EQ(encoded.at("evidence")
                .at("jobs")
                .at(0U)
                .at("physical_trace")
                .at("service_starts")
                .size(),
            kB1TasksPerJob);
  EXPECT_EQ(encoded.at("verdicts").at("throughput"), "pass");
  EXPECT_FALSE(encoded.at("outer_canonical_envelope_claim").get<bool>());
  EXPECT_FALSE(b1_workload_contract_json()
                   .at("outer_canonical_envelope_claim")
                   .get<bool>());
}

/**
 * @brief Proves a golden mismatch fails output/throughput without hiding work.
 * @throws Test fixture, evaluation, and framework failures unchanged.
 */
TEST(B1Evidence, GoldenMismatchFailsDeterminismAndChargesDiscardedService) {
  B1InnerRowInput input = make_valid_row_input(1U, 1U);
  B1JobEvidence& measured = input.jobs[4U];
  measured.golden.raw_payload_digest = b1_sha256("wrong-golden");
  const B1InnerRow row = evaluate_b1_inner_row(std::move(input));
  EXPECT_EQ(row.throughput_verdict, I1Verdict::Fail);
  EXPECT_EQ(row.determinism_verdict, I1Verdict::Fail);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Fail);
  EXPECT_EQ(row.raw_golden_mismatches, 1U);
  EXPECT_EQ(row.verified_measured_jobs, 29U);
  EXPECT_EQ(row.discarded_started_service, kB1TasksPerJob);
}

/**
 * @brief Proves malformed trace and snapshot gaps invalidate only affected
 * axes.
 * @throws Test fixture, evaluation, and framework failures unchanged.
 */
TEST(B1Evidence, MissingTraceAndResourceClosureInvalidateAffectedAxes) {
  B1InnerRowInput input = make_valid_row_input(8U, 1U);
  input.jobs[4U].semantic_trace.clear();
  input.final_snapshot.host_resources.reserved.ready_bytes = 1U;
  const B1InnerRow row = evaluate_b1_inner_row(std::move(input));
  EXPECT_EQ(row.throughput_verdict, I1Verdict::Fail);
  EXPECT_EQ(row.determinism_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Fail);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Invalid);
  EXPECT_FALSE(row.validity_reasons.empty());
}

/**
 * @brief Proves duplicate and post-cancellation service cannot pass zero waste.
 * @throws Test fixture, evaluation, and framework failures unchanged.
 */
TEST(B1Evidence, DuplicateAndPostCancellationStartsRemainCharged) {
  B1InnerRowInput input = make_valid_row_input(8U, 1U);
  B1JobEvidence& measured = input.jobs[4U];
  measured.physical_trace.cancellations.push_back(B1ObservedCancellation{
      1U, compute::ComputeRunCancellationReason::ExplicitRequest,
      compute::ComputeRunObservationCoordinate{measured.offered_at, 100U}});
  measured.physical_trace.service_starts.back().local_task_id = 0U;
  measured.physical_trace.service_starts.back().coordinate.causal_sequence =
      300U;
  const B1InnerRow row = evaluate_b1_inner_row(std::move(input));
  EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
  EXPECT_GT(row.duplicate_service_starts, 0U);
  EXPECT_GT(row.post_cancellation_started_service, 0U);
}

/**
 * @brief Proves six compatible cap/replicate rows compare exact identities.
 * @throws Test fixture, evaluation, and framework failures unchanged.
 */
TEST(B1Evidence, CrossRowDeterminismRequiresAllCapsAndReplicates) {
  std::vector<B1InnerRow> rows;
  for (const std::uint64_t cap : {1U, 8U}) {
    for (std::uint64_t replicate = 1U; replicate <= 3U; ++replicate) {
      rows.push_back(
          evaluate_b1_inner_row(make_valid_row_input(cap, replicate)));
    }
  }
  B1DeterminismSummary summary = evaluate_b1_cross_row_determinism(rows);
  EXPECT_EQ(summary.row_count, 6U);
  EXPECT_EQ(summary.mismatch_count, 0U);
  EXPECT_EQ(summary.verdict, I1Verdict::Pass);

  rows.back().evidence.jobs.back().output.receipt->manifest_digest =
      b1_sha256("mismatch");
  summary = evaluate_b1_cross_row_determinism(rows);
  EXPECT_EQ(summary.verdict, I1Verdict::Fail);
  EXPECT_EQ(summary.mismatch_count, 1U);
}

/**
 * @brief Proves ordinal pairing and independent median/every-ratio thresholds.
 * @throws Test fixture, evaluation, and framework failures unchanged.
 */
TEST(B1Evidence, ReferenceThroughputUsesThreeOrdinalRatios) {
  std::vector<B1InnerRow> candidate;
  std::vector<B1InnerRow> reference;
  for (std::uint64_t replicate = 1U; replicate <= 3U; ++replicate) {
    candidate.push_back(
        evaluate_b1_inner_row(make_valid_row_input(8U, replicate)));
    reference.push_back(
        evaluate_b1_inner_row(make_valid_row_input(8U, replicate)));
  }
  B1ReferenceThroughputSummary summary =
      evaluate_b1_reference_throughput(candidate, reference);
  EXPECT_EQ(summary.verdict, I1Verdict::Pass);
  ASSERT_EQ(summary.replicate_ratios.size(), 3U);
  EXPECT_DOUBLE_EQ(*summary.median_ratio, 1.0);

  *candidate[0U].throughput_mpix_ops_per_second *= 0.89;
  summary = evaluate_b1_reference_throughput(candidate, reference);
  EXPECT_EQ(summary.verdict, I1Verdict::Fail);
  EXPECT_DOUBLE_EQ(*summary.median_ratio, 1.0);
}

}  // namespace
}  // namespace ps::benchmark
