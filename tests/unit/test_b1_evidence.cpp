/**
 * @file test_b1_evidence.cpp
 * @brief Verifies B1 occurrence joins and four independent verdict axes.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/b1_evidence.hpp"        // NOLINT(build/include_subdir)
#include "benchmark/evidence_envelope.hpp"  // NOLINT(build/include_subdir)
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
  const execution::ComputeIoExecutorSnapshot payload_charged =
      make_io_snapshot(1U, kB1PayloadBytes);
  const execution::ComputeIoExecutorSnapshot payload_released =
      make_io_snapshot();
  const execution::ComputeIoExecutorSnapshot manifest_charged =
      make_io_snapshot(1U, manifest_bytes);
  const execution::ComputeIoExecutorSnapshot manifest_released =
      make_io_snapshot();
  const execution::ComputeIoAdmissionEvent payload_admission{
      10U,
      execution::ComputeIoAdmissionStatus::Accepted,
      kB1PayloadBytes,
      1U,
      kB1PayloadBytes,
      payload_charged};
  const execution::ComputeIoSettlementEvent payload_settlement{
      30U,
      payload_admission.sequence,
      execution::ComputeIoCompletionStatus::Succeeded,
      1U,
      kB1PayloadBytes,
      payload_released};
  const execution::ComputeIoAdmissionEvent manifest_admission{
      44U,
      execution::ComputeIoAdmissionStatus::Accepted,
      manifest_bytes,
      1U,
      manifest_bytes,
      manifest_charged};
  const execution::ComputeIoSettlementEvent manifest_settlement{
      90U,
      manifest_admission.sequence,
      execution::ComputeIoCompletionStatus::Succeeded,
      1U,
      manifest_bytes,
      manifest_released};
  return {
      {B1IoObservationPoint::Initial, std::nullopt, 0U, std::nullopt,
       std::nullopt, std::nullopt, std::nullopt, make_io_snapshot()},
      {B1IoObservationPoint::AcceptedAdmission, payload, kB1PayloadBytes,
       execution::ComputeIoAdmissionStatus::Accepted, std::nullopt,
       payload_admission, std::nullopt, payload_charged},
      {B1IoObservationPoint::Settlement, payload, kB1PayloadBytes,
       execution::ComputeIoAdmissionStatus::Accepted,
       execution::ComputeIoCompletionStatus::Succeeded, payload_admission,
       payload_settlement, payload_released},
      {B1IoObservationPoint::AcceptedAdmission, manifest, manifest_bytes,
       execution::ComputeIoAdmissionStatus::Accepted, std::nullopt,
       manifest_admission, std::nullopt, manifest_charged},
      {B1IoObservationPoint::Settlement, manifest, manifest_bytes,
       execution::ComputeIoAdmissionStatus::Accepted,
       execution::ComputeIoCompletionStatus::Succeeded, manifest_admission,
       manifest_settlement, manifest_released},
      {B1IoObservationPoint::Final, std::nullopt, 0U, std::nullopt,
       std::nullopt, std::nullopt, std::nullopt, make_io_snapshot()},
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

  const std::uint64_t run_id = job.job_index + 1U;
  evidence.physical_trace.job = job;
  evidence.physical_trace.current_generations.push_back(
      B1ObservedCurrentGeneration{
          1U, compute::ComputeRunObservationCoordinate{offered_at, 1U}});
  const std::vector<B1SemanticTask> semantic_plan =
      b1_frozen_semantic_plan(job.job_index);
  for (std::uint64_t task = 0U; task < kB1TasksPerJob; ++task) {
    const B1SemanticTask& planned =
        semantic_plan.at(static_cast<std::size_t>(task));
    if (planned.dependencies.size() > kB1ObservedDependencyCapacity) {
      throw std::logic_error(
          "B1 test semantic plan exceeds observation dependency capacity");
    }
    B1ObservedTaskReady ready;
    ready.run_id = run_id;
    ready.local_task_id = task;
    ready.dependency_count = planned.dependencies.size();
    std::copy(planned.dependencies.begin(), planned.dependencies.end(),
              ready.dependencies.begin());
    ready.resources = planned.resources;
    ready.coordinate =
        compute::ComputeRunObservationCoordinate{offered_at, 2U + task * 3U};
    evidence.physical_trace.task_readies.push_back(ready);
    evidence.physical_trace.service_starts.push_back(B1ObservedServiceStart{
        run_id, task, 1U,
        compute::ComputeRunQos{compute::ComputeRunQosClass::Throughput,
                               std::nullopt, 1U,
                               static_cast<std::uint32_t>(job.run_cap)},
        compute::ComputeRunObservationCoordinate{offered_at, 3U + task * 3U}});
    evidence.physical_trace.task_terminals.push_back(B1ObservedTaskTerminal{
        run_id, task, compute::ComputeRunTaskTerminalKind::Succeeded,
        compute::ComputeRunObservationCoordinate{offered_at, 4U + task * 3U}});
  }
  evidence.physical_trace.visible = B1ObservedRunTransition{
      run_id, compute::ComputeRunObservationCoordinate{offered_at, 773U}};
  evidence.physical_trace.terminal_kind =
      compute::ComputeRunTerminalKind::Succeeded;
  evidence.physical_trace.terminal = B1ObservedRunTransition{
      run_id, compute::ComputeRunObservationCoordinate{offered_at, 774U}};
  evidence.physical_trace.quiescent = B1ObservedRunTransition{
      run_id, compute::ComputeRunObservationCoordinate{offered_at, 775U}};
  evidence.physical_trace.resource_settled = B1ObservedRunTransition{
      run_id, compute::ComputeRunObservationCoordinate{offered_at, 776U}};
  evidence.physical_trace.visible_content_digest =
      ContentDigestResult{ContentDigestState::Available,
                          evidence.golden.logical_digest,
                          {}};
  evidence.semantic_trace = encode_b1_semantic_trace(
      make_b1_observed_semantic_records(evidence.physical_trace));
  evidence.semantic_trace_digest = b1_sha256(evidence.semantic_trace);

  const std::string commit_id = test_commit_id(job);
  const std::string manifest =
      b1_artifact_manifest(job.job_index, evidence.golden.raw_payload_digest);
  evidence.output.status = B1OutputCommitStatus::Succeeded;
  evidence.output.receipt = testing::B1OutputCommitReceiptTestAccess::mint(
      commit_id, std::filesystem::path("/tmp/photospider-b1-test-output"),
      std::filesystem::path("occurrence-" + commit_id), job,
      "dense-tensor-hwc-fp32-rgba-2048x2048", evidence.golden.logical_digest,
      1U, "output.rgba32le", "manifest.txt", kB1PayloadBytes,
      b1_manifest_length(job.job_index), evidence.golden.raw_payload_digest,
      b1_sha256(manifest), B1OutputDurability::CrashDurable,
      B1OutputDurability::CrashDurable, "dev=1;ino=1");
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
 * @brief Proves the real B1 evaluator output produces a loadable native pair.
 * @throws Test fixture, evaluator, canonical pack, and framework failures.
 */
TEST(B1Evidence, ProducesCanonicalPairObjectFromCompleteCapEightRow) {
  B1InnerRowInput input = make_valid_row_input(8U, 1U);
  input.environment.fixture_digest = evidence_b1_component_digests().fixture;
  const B1InnerRow row = evaluate_b1_inner_row(std::move(input));
  ASSERT_TRUE(row.validity_reasons.empty());

  const EvidencePairObject produced = make_b1_evidence_pair_object(
      row, EvidencePairProducerOptions{EvidenceSubjectRole::Reference,
                                       std::nullopt});
  const std::string pack = materialize_evidence_pair_object(produced);
  const EvidencePairObject loaded = load_evidence_pair_object(
      pack, produced.row.digest, produced.bundle.digest);
  EXPECT_EQ(loaded.row.digest, produced.row.digest);
  EXPECT_EQ(loaded.bundle.digest, produced.bundle.digest);
  const B1CanonicalManifest measurement =
      parse_b1_canonical_manifest(loaded.row.source.measurement_evidence.bytes);
  EXPECT_EQ(measurement.schema, "execution-profile-measurement-evidence-v1");
  ASSERT_EQ(measurement.fields.size(), 8U);
  EXPECT_EQ(measurement.fields[0U].payload, kEvidenceB1PairDenominatorSchema);
  EXPECT_EQ(parse_b1_canonical_uint64(measurement.fields[3U].payload),
            kB1InnerRowSchemaVersion);
  EXPECT_EQ(parse_b1_framed_list(measurement.fields[6U].payload).size(),
            kB1MeasuredJobCount);
  EXPECT_EQ(parse_b1_canonical_uint64(measurement.fields[7U].payload),
            row.successful_site_operations);
  EXPECT_EQ(loaded.row.source.output_evidence.schema_id,
            "execution-profile-output-evidence-v1");
  EXPECT_EQ(loaded.row.source.verdict_evidence.schema_id,
            "execution-profile-verdict-evidence-v1");
  EXPECT_EQ(parse_b1_canonical_manifest(loaded.row.source.output_evidence.bytes)
                .fields[2U]
                .payload,
            "not-claimed");
  EXPECT_EQ(
      parse_b1_canonical_manifest(loaded.row.source.verdict_evidence.bytes)
          .fields[2U]
          .payload,
      "denominator-only");
  EXPECT_TRUE(valid_b1_environment_claims(loaded.row.source.environment));
  EXPECT_FALSE(valid_b1_environment_evidence(loaded.row.source.environment));
}

/**
 * @brief Proves B1 denominator production requires schema v1 and exact jobs.
 * @throws Test fixture, evaluator, canonical pack, and framework failures.
 */
TEST(B1Evidence, PairDenominatorRejectsSchemaAndOccurrenceDrift) {
  B1InnerRowInput input = make_valid_row_input(8U, 1U);
  input.environment.fixture_digest = evidence_b1_component_digests().fixture;
  const B1InnerRow valid = evaluate_b1_inner_row(std::move(input));
  ASSERT_TRUE(valid.validity_reasons.empty());
  const EvidencePairProducerOptions options{EvidenceSubjectRole::Reference,
                                            std::nullopt};

  B1InnerRow schema_drift = valid;
  schema_drift.schema_version = kB1InnerRowSchemaVersion + 1U;
  EXPECT_THROW(make_b1_evidence_pair_object(schema_drift, options),
               std::invalid_argument);

  B1InnerRow missing = valid;
  missing.evidence.jobs.pop_back();
  EXPECT_THROW(make_b1_evidence_pair_object(missing, options),
               std::invalid_argument);

  B1InnerRow duplicate = valid;
  duplicate.evidence.jobs.back() = duplicate.evidence.jobs.front();
  EXPECT_THROW(make_b1_evidence_pair_object(duplicate, options),
               std::invalid_argument);
}

/**
 * @brief Preserves the existing cap-one B1 runner while M1 binds only cap
 * eight.
 * @throws Test fixture, evaluator, canonical pack, and framework failures.
 */
TEST(B1Evidence, PairProducerPreservesCompleteCapOneRow) {
  B1InnerRowInput input = make_valid_row_input(1U, 1U);
  input.environment.fixture_digest = evidence_b1_component_digests().fixture;
  const B1InnerRow row = evaluate_b1_inner_row(std::move(input));
  ASSERT_TRUE(row.validity_reasons.empty());

  const EvidencePairObject produced = make_b1_evidence_pair_object(
      row, EvidencePairProducerOptions{EvidenceSubjectRole::Reference,
                                       std::nullopt});
  const EvidencePairObject loaded =
      load_evidence_pair_object(materialize_evidence_pair_object(produced),
                                produced.row.digest, produced.bundle.digest);
  EXPECT_EQ(loaded.row.source.run_cap, 1U);
  EXPECT_EQ(loaded.bundle.row_references.front().run_cap, 1U);
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
  const nlohmann::json& raw_proof =
      encoded.at("evidence").at("environment").at("storage_raw_proof");
  EXPECT_EQ(raw_proof.at("schema"),
            "execution-profile-b1-storage-raw-proof-v1");
  EXPECT_EQ(raw_proof.at("canonical_bytes"),
            row.evidence.environment.storage_raw_proof->canonical_bytes);
  EXPECT_FALSE(raw_proof.at("raw_evidence")
                   .at("containment")
                   .at("destinations")
                   .empty());
  EXPECT_EQ(raw_proof.at("raw_evidence").at("transaction").at("events").size(),
            7U);
  const nlohmann::json& actual_observation =
      encoded.at("evidence").at("environment").at("storage_actual_observation");
  EXPECT_EQ(actual_observation.at("authority_rehydration"),
            "live-process-required");
  EXPECT_EQ(actual_observation.at("filesystem_type"), "testfs");
  EXPECT_EQ(actual_observation.at("receipts").size(), 1U);
  EXPECT_FALSE(actual_observation.at("complete_probe_digest").is_null());
  EXPECT_TRUE(actual_observation.at("unverified_external_fields").empty());
  EXPECT_EQ(encoded.at("evidence")
                .at("jobs")
                .at(0U)
                .at("output")
                .at("io_observations")
                .at(1U)
                .at("admission_event")
                .at("charged_planned_bytes"),
            kB1PayloadBytes);
  EXPECT_EQ(encoded.at("evidence")
                .at("jobs")
                .at(0U)
                .at("output")
                .at("io_observations")
                .at(2U)
                .at("settlement_event")
                .at("released_planned_bytes"),
            kB1PayloadBytes);
  EXPECT_EQ(encoded.at("verdicts").at("throughput"), "pass");
  EXPECT_FALSE(encoded.at("outer_canonical_envelope_claim").get<bool>());
  EXPECT_FALSE(b1_workload_contract_json()
                   .at("outer_canonical_envelope_claim")
                   .get<bool>());
}

/**
 * @brief Proves the portable-runner authority path stays Invalid and its JSON
 * remains diagnostic rather than rehydratable authority.
 * @throws Test fixture, evaluation, JSON, and framework failures unchanged.
 */
TEST(B1Evidence, PortableRunnerAuthorityFailsClosedAndJsonIsDiagnostic) {
  B1InnerRowInput input = make_valid_row_input(8U, 1U);
  testing::B1TestStorageAuthorityFixture authority =
      testing::b1_test_storage_authority_fixture();
  authority.source->complete_probe.reset();
  authority.source->unverified_external_fields = {
      "b1_performance_configuration", "hardware_write_cache_policy",
      "mount_effective_options",      "mount_identity",
      "power_loss_protection_policy", "transaction_observation.events",
  };
  input.environment.storage_actual_observation =
      testing::B1StorageActualObservationTestAccess::mint(authority.source);

  const B1InnerRow row = evaluate_b1_inner_row(std::move(input));
  EXPECT_EQ(row.throughput_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.determinism_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Invalid);
  EXPECT_NE(std::find(row.validity_reasons.begin(), row.validity_reasons.end(),
                      "B1 row storage is not bound to independent live "
                      "authority"),
            row.validity_reasons.end());

  const nlohmann::json encoded = b1_inner_row_json(row);
  const nlohmann::json& actual =
      encoded.at("evidence").at("environment").at("storage_actual_observation");
  EXPECT_TRUE(actual.at("complete_probe_digest").is_null());
  EXPECT_EQ(actual.at("authority_rehydration"), "live-process-required");
  const std::vector<std::string> expected_unverified{
      "b1_performance_configuration", "hardware_write_cache_policy",
      "mount_effective_options",      "mount_identity",
      "power_loss_protection_policy", "transaction_observation.events",
  };
  EXPECT_EQ(
      actual.at("unverified_external_fields").get<std::vector<std::string>>(),
      expected_unverified);
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
 * @brief Proves actual task observations fail closed on every semantic drift.
 * @throws Test fixture, evaluation, and framework failures unchanged.
 */
TEST(B1Evidence,
     ActualSemanticObservationsRejectMissingDuplicateGapAndFieldDrift) {
  const auto expect_determinism_invalid = [](B1InnerRowInput input) {
    const B1InnerRow row = evaluate_b1_inner_row(std::move(input));
    EXPECT_EQ(row.determinism_verdict, I1Verdict::Invalid);
    EXPECT_GT(row.semantic_trace_mismatches, 0U);
  };

  B1InnerRowInput missing = make_valid_row_input(8U, 1U);
  missing.jobs[4U].physical_trace.task_readies.pop_back();
  expect_determinism_invalid(std::move(missing));

  B1InnerRowInput duplicate = make_valid_row_input(8U, 1U);
  duplicate.jobs[4U].physical_trace.task_readies.back().local_task_id = 0U;
  expect_determinism_invalid(std::move(duplicate));

  B1InnerRowInput gap = make_valid_row_input(8U, 1U);
  gap.jobs[4U].physical_trace.task_terminals.back().local_task_id =
      kB1TasksPerJob;
  expect_determinism_invalid(std::move(gap));

  B1InnerRowInput dependency = make_valid_row_input(8U, 1U);
  ASSERT_EQ(
      dependency.jobs[4U].physical_trace.task_readies[1U].dependency_count, 1U);
  dependency.jobs[4U].physical_trace.task_readies[1U].dependencies[0U] = 2U;
  expect_determinism_invalid(std::move(dependency));

  B1InnerRowInput resource = make_valid_row_input(8U, 1U);
  ++resource.jobs[4U].physical_trace.task_readies[1U].resources.ready_bytes;
  expect_determinism_invalid(std::move(resource));

  B1InnerRowInput ready_declaration = make_valid_row_input(8U, 1U);
  ++ready_declaration.jobs[4U]
        .physical_trace.task_readies[1U]
        .declared_ready_bytes;
  expect_determinism_invalid(std::move(ready_declaration));

  B1InnerRowInput outcome = make_valid_row_input(8U, 1U);
  outcome.jobs[4U].physical_trace.task_terminals[1U].kind =
      compute::ComputeRunTaskTerminalKind::Failed;
  expect_determinism_invalid(std::move(outcome));
}

/**
 * @brief Proves the exact Compute I/O FSM rejects structural mutation matrix.
 * @throws Test fixture, evaluation, and framework failures unchanged.
 */
TEST(
    B1Evidence,
    ComputeIoFsmRejectsMissingDuplicateReorderIdentityStatusAttemptAndSnapshot) {
  const auto expect_row_invalid = [](B1InnerRowInput input) {
    const B1InnerRow row = evaluate_b1_inner_row(std::move(input));
    EXPECT_EQ(row.throughput_verdict, I1Verdict::Invalid);
    EXPECT_EQ(row.determinism_verdict, I1Verdict::Invalid);
    EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
    EXPECT_EQ(row.memory_verdict, I1Verdict::Invalid);
  };

  B1InnerRowInput missing = make_valid_row_input(8U, 1U);
  missing.jobs[4U].output.io_observations.erase(
      missing.jobs[4U].output.io_observations.begin() + 2);
  expect_row_invalid(std::move(missing));

  B1InnerRowInput duplicate = make_valid_row_input(8U, 1U);
  duplicate.jobs[4U].output.io_observations.insert(
      duplicate.jobs[4U].output.io_observations.begin() + 2,
      duplicate.jobs[4U].output.io_observations[1U]);
  expect_row_invalid(std::move(duplicate));

  B1InnerRowInput reordered = make_valid_row_input(8U, 1U);
  std::swap(reordered.jobs[4U].output.io_observations[2U],
            reordered.jobs[4U].output.io_observations[3U]);
  expect_row_invalid(std::move(reordered));

  B1InnerRowInput wrong_stage = make_valid_row_input(8U, 1U);
  wrong_stage.jobs[4U].output.io_observations[1U].task->stage =
      B1IoStage::ManifestCommit;
  expect_row_invalid(std::move(wrong_stage));

  B1InnerRowInput wrong_job = make_valid_row_input(8U, 1U);
  ++wrong_job.jobs[4U].output.io_observations[1U].task->job.job_index;
  expect_row_invalid(std::move(wrong_job));

  B1InnerRowInput wrong_status = make_valid_row_input(8U, 1U);
  wrong_status.jobs[4U].output.io_observations[1U].admission =
      execution::ComputeIoAdmissionStatus::TaskLimit;
  expect_row_invalid(std::move(wrong_status));

  B1InnerRowInput wrong_completion = make_valid_row_input(8U, 1U);
  wrong_completion.jobs[4U].output.io_observations[2U].completion =
      execution::ComputeIoCompletionStatus::Failed;
  expect_row_invalid(std::move(wrong_completion));

  B1InnerRowInput attempt_gap = make_valid_row_input(8U, 1U);
  attempt_gap.jobs[4U].output.io_observations[1U].task->attempt = 1U;
  expect_row_invalid(std::move(attempt_gap));

  B1InnerRowInput misplaced_final = make_valid_row_input(8U, 1U);
  std::swap(misplaced_final.jobs[4U].output.io_observations[4U],
            misplaced_final.jobs[4U].output.io_observations[5U]);
  expect_row_invalid(std::move(misplaced_final));

  B1InnerRowInput invalid_snapshot = make_valid_row_input(8U, 1U);
  invalid_snapshot.jobs[4U].output.io_observations[1U].snapshot.active_tasks =
      kB1ComputeIoTaskLimit + 1U;
  expect_row_invalid(std::move(invalid_snapshot));

  B1InnerRowInput admitted_undercharge = make_valid_row_input(8U, 1U);
  admitted_undercharge.jobs[4U]
      .output.io_observations[1U]
      .admission_event->charged_planned_bytes = 0U;
  expect_row_invalid(std::move(admitted_undercharge));

  B1InnerRowInput fake_zero_admission = make_valid_row_input(8U, 1U);
  B1ComputeIoObservation& fake =
      fake_zero_admission.jobs[4U].output.io_observations[1U];
  fake.snapshot = make_io_snapshot();
  fake.admission_event->snapshot_after = fake.snapshot;
  expect_row_invalid(std::move(fake_zero_admission));

  B1InnerRowInput settlement_undercharge = make_valid_row_input(8U, 1U);
  settlement_undercharge.jobs[4U]
      .output.io_observations[2U]
      .settlement_event->released_planned_bytes = 1U;
  expect_row_invalid(std::move(settlement_undercharge));

  B1InnerRowInput wrong_settlement_admission = make_valid_row_input(8U, 1U);
  ++wrong_settlement_admission.jobs[4U]
        .output.io_observations[2U]
        .settlement_event->admission_sequence;
  expect_row_invalid(std::move(wrong_settlement_admission));

  B1InnerRowInput post_final = make_valid_row_input(8U, 1U);
  post_final.jobs[4U].output.io_observations.push_back(
      post_final.jobs[4U].output.io_observations.front());
  expect_row_invalid(std::move(post_final));

  B1InnerRowInput reconciled_without_earlier_stream =
      make_valid_row_input(8U, 1U);
  reconciled_without_earlier_stream.jobs[4U].output.io_observations.clear();
  expect_row_invalid(std::move(reconciled_without_earlier_stream));
}

/**
 * @brief Proves global event-number gaps are legal but task FSM rows are not.
 * @throws Test fixture, evaluation, and framework failures unchanged.
 */
TEST(B1Evidence,
     ComputeIoFsmAllowsNumericSequenceGapsButNotMissingTransitions) {
  B1InnerRowInput valid = make_valid_row_input(8U, 1U);
  const std::vector<B1ComputeIoObservation>& observations =
      valid.jobs[4U].output.io_observations;
  ASSERT_TRUE(observations[1U].admission_event.has_value());
  ASSERT_TRUE(observations[2U].settlement_event.has_value());
  ASSERT_TRUE(observations[3U].admission_event.has_value());
  ASSERT_EQ(observations[1U].admission_event->sequence, 10U);
  ASSERT_EQ(observations[2U].settlement_event->sequence, 30U);
  ASSERT_EQ(observations[3U].admission_event->sequence, 44U);
  const B1InnerRow valid_row = evaluate_b1_inner_row(std::move(valid));
  EXPECT_TRUE(valid_row.validity_reasons.empty());

  B1InnerRowInput missing_transition = make_valid_row_input(8U, 1U);
  missing_transition.jobs[4U].output.io_observations.erase(
      missing_transition.jobs[4U].output.io_observations.begin() + 2);
  const B1InnerRow invalid_row =
      evaluate_b1_inner_row(std::move(missing_transition));
  EXPECT_EQ(invalid_row.throughput_verdict, I1Verdict::Invalid);
  EXPECT_EQ(invalid_row.determinism_verdict, I1Verdict::Invalid);
  EXPECT_EQ(invalid_row.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(invalid_row.memory_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves unrelated process I/O may remain active at a job-local Final.
 * @throws Test fixture, evaluation, and framework failures unchanged.
 * @note Exact own-charge release remains proved by the settlement event; only
 * the complete isolated row's final execution snapshot must be quiescent.
 */
TEST(B1Evidence, JobFinalAllowsUnrelatedActiveExecutorCharge) {
  B1InnerRowInput input = make_valid_row_input(8U, 1U);
  execution::ComputeIoExecutorSnapshot& final =
      input.jobs[4U].output.io_observations.back().snapshot;
  final.active_tasks = 1U;
  final.active_planned_bytes = 7U;
  final.queued_tasks = 1U;
  const B1InnerRow row = evaluate_b1_inner_row(std::move(input));
  EXPECT_TRUE(row.validity_reasons.empty());
  EXPECT_EQ(row.throughput_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.determinism_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves every active Compute I/O charge occupies exactly one phase.
 * @throws Test fixture, evaluation, and framework failures unchanged.
 */
TEST(B1Evidence, ComputeIoSnapshotRequiresExactSinglePhasePartition) {
  const auto expect_pass = [](execution::ComputeIoExecutorSnapshot snapshot) {
    B1InnerRowInput input = make_valid_row_input(8U, 1U);
    input.jobs[4U].output.io_observations.back().snapshot = snapshot;
    const B1InnerRow row = evaluate_b1_inner_row(std::move(input));
    EXPECT_TRUE(row.validity_reasons.empty());
  };
  const auto expect_invalid =
      [](execution::ComputeIoExecutorSnapshot snapshot) {
        B1InnerRowInput input = make_valid_row_input(8U, 1U);
        input.jobs[4U].output.io_observations.back().snapshot = snapshot;
        const B1InnerRow row = evaluate_b1_inner_row(std::move(input));
        EXPECT_EQ(row.throughput_verdict, I1Verdict::Invalid);
        EXPECT_EQ(row.determinism_verdict, I1Verdict::Invalid);
        EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
        EXPECT_EQ(row.memory_verdict, I1Verdict::Invalid);
      };

  execution::ComputeIoExecutorSnapshot constructing = make_io_snapshot();
  constructing.active_tasks = 1U;
  constructing.active_planned_bytes = 1U;
  constructing.constructing_tasks = 1U;
  expect_pass(constructing);

  execution::ComputeIoExecutorSnapshot queued = make_io_snapshot(1U, 1U);
  expect_pass(queued);

  execution::ComputeIoExecutorSnapshot running = make_io_snapshot();
  running.active_tasks = 1U;
  running.active_planned_bytes = 1U;
  running.running_tasks = 1U;
  expect_pass(running);

  execution::ComputeIoExecutorSnapshot boundary = make_io_snapshot();
  boundary.active_tasks = kB1ComputeIoTaskLimit;
  boundary.active_planned_bytes = kB1ComputeIoTaskLimit;
  boundary.constructing_tasks = kB1ComputeIoTaskLimit;
  expect_pass(boundary);

  execution::ComputeIoExecutorSnapshot missing_phase = make_io_snapshot();
  missing_phase.active_tasks = 1U;
  missing_phase.active_planned_bytes = 1U;
  expect_invalid(missing_phase);

  execution::ComputeIoExecutorSnapshot duplicate_phase = make_io_snapshot();
  duplicate_phase.active_tasks = 1U;
  duplicate_phase.active_planned_bytes = 1U;
  duplicate_phase.constructing_tasks = 1U;
  duplicate_phase.running_tasks = 1U;
  expect_invalid(duplicate_phase);
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

  testing::B1OutputCommitReceiptTestAccess::set_manifest_digest(
      &*rows.back().evidence.jobs.back().output.receipt, b1_sha256("mismatch"));
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

/**
 * @brief Proves candidate/reference evaluation rejects a synchronized retained
 * storage/proof/class recast on either comparison side.
 * @throws Test fixture, environment encoding, evaluation, and framework
 * failures unchanged.
 */
TEST(B1Evidence, ReferenceThroughputRequiresActualAuthorityOnEachSide) {
  std::vector<B1InnerRow> candidate;
  std::vector<B1InnerRow> reference;
  for (std::uint64_t replicate = 1U; replicate <= 3U; ++replicate) {
    B1InnerRowInput candidate_input = make_valid_row_input(8U, replicate);
    if (replicate == 1U) {
      candidate_input.environment =
          testing::synchronously_recast_b1_test_storage(
              std::move(candidate_input.environment), "forgedfs");
    }
    candidate.push_back(evaluate_b1_inner_row(std::move(candidate_input)));
    reference.push_back(
        evaluate_b1_inner_row(make_valid_row_input(8U, replicate)));
  }
  EXPECT_EQ(evaluate_b1_reference_throughput(candidate, reference).verdict,
            I1Verdict::Invalid);

  candidate.clear();
  reference.clear();
  for (std::uint64_t replicate = 1U; replicate <= 3U; ++replicate) {
    candidate.push_back(
        evaluate_b1_inner_row(make_valid_row_input(8U, replicate)));
    B1InnerRowInput reference_input = make_valid_row_input(8U, replicate);
    if (replicate == 2U) {
      reference_input.environment =
          testing::synchronously_recast_b1_test_storage(
              std::move(reference_input.environment), "forgedfs");
    }
    reference.push_back(evaluate_b1_inner_row(std::move(reference_input)));
  }
  EXPECT_EQ(evaluate_b1_reference_throughput(candidate, reference).verdict,
            I1Verdict::Invalid);
}

}  // namespace
}  // namespace ps::benchmark
