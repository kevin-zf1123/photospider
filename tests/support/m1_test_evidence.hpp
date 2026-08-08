/**
 * @file m1_test_evidence.hpp
 * @brief Builds complete replayable I1/B1 source fixtures for M1 tests.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/m1_evidence.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/value.hpp"
#include "support/b1_test_environment.hpp"

namespace ps::benchmark::testing {

/**
 * @brief Creates one tiny deterministic Value for source digest freezing.
 * @return Ready one-byte CPU DenseTensor.
 * @throws Value publication and allocation failures unchanged.
 */
inline Value make_m1_test_output() {
  DenseTensorDescriptor descriptor;
  descriptor.shape = {1U, 1U};
  descriptor.element_semantics = ElementSemantics::UnsignedInteger;
  descriptor.storage_encoding = StorageEncoding{8U};
  return Value::from_cpu_dense_tensor(std::move(descriptor), std::nullopt,
                                      StridedLayout{{1, 1}}, {std::byte{0x2a}});
}

/**
 * @brief Maps one M1 I1 identity to the frozen Issue #93 source slot.
 * @param phase Immutable M1 phase.
 * @param ordinal Zero-based phase-local ordinal.
 * @return Cold zero, warmup one through seven, or measured 21 through 60.
 * @throws std::invalid_argument for an unsupported identity.
 */
inline std::size_t m1_test_i1_slot(B1JobPhase phase, std::size_t ordinal) {
  if (phase == B1JobPhase::Cold && ordinal == 0U) {
    return 0U;
  }
  if (phase == B1JobPhase::Warmup && ordinal < kM1WarmupI1OriginCount) {
    return 1U + ordinal;
  }
  if (phase == B1JobPhase::Measured && ordinal < kM1MeasuredI1OriginCount) {
    return kI1WarmupSlotCount + 1U + ordinal;
  }
  throw std::invalid_argument("M1 test I1 source identity is invalid.");
}

/**
 * @brief Builds one complete passing Issue #93 source at an exact M1 origin.
 * @param phase Immutable M1 phase.
 * @param ordinal Zero-based phase-local ordinal.
 * @param replicate_ordinal Enclosing M1 replicate identity.
 * @param origin Exact M1 episode origin timestamp.
 * @param final_latency Final accepted-to-visible duration.
 * @return Complete raw evaluator input with no discarded or post-cancel work.
 * @throws Checked-time, Value/digest, and allocation failures unchanged.
 */
inline I1EpisodeEvidenceInput make_m1_test_i1_episode(
    B1JobPhase phase, std::size_t ordinal, std::uint64_t replicate_ordinal,
    std::chrono::steady_clock::time_point origin,
    std::chrono::nanoseconds final_latency = std::chrono::milliseconds(10)) {
  I1EpisodeEvidenceInput input;
  input.replicate_ordinal = replicate_ordinal;
  input.slot = m1_test_i1_slot(phase, ordinal);
  input.grid_origin = checked_i1_time_subtract(
      origin, std::chrono::nanoseconds(static_cast<std::int64_t>(input.slot) *
                                       kI1EpisodeStride.count()));
  input.episode_origin = origin;
  input.terminal_boundary = i1_terminal_boundary(input.grid_origin);
  input.measurement_start =
      checked_i1_time_add(origin, kI1MeasurementStartOffset);
  input.measurement_end = checked_i1_time_add(origin, kI1MeasurementEndOffset);
  input.final_snapshot_sample = input.measurement_end;
  input.expected_final_digest = i1_frozen_final_content_digest();

  input.baseline.host_resources.limits =
      ResourceVector{8U, 1U << 20U, 1U << 20U, 1024U, 1U << 20U};
  input.final_snapshot.host_resources.limits =
      input.baseline.host_resources.limits;
  input.final_snapshot.host_resources.high_water =
      ResourceVector{1U, 4096U, 0U, 1U, 4096U};
  input.baseline.lifecycle.service_instance_id = 41U;
  input.baseline.lifecycle.telemetry_epoch = 43U;
  input.baseline.lifecycle.snapshot_cut = 10U;
  input.baseline.lifecycle.counters.registered_graph_count = 1U;
  input.baseline.lifecycle.counters.open_graph_count = 1U;
  input.baseline.lifecycle.counters.live_policy_binding_count = 1U;
  input.final_snapshot.lifecycle = input.baseline.lifecycle;
  input.final_snapshot.lifecycle.snapshot_cut = 1000U;
  input.final_snapshot.lifecycle.next_cursor = 1000U;

  const Value output = make_m1_test_output();
  std::uint64_t causal_sequence = 1U;
  for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
    const auto nominal = checked_i1_time_add(
        origin,
        std::chrono::nanoseconds(kI1EditStride.count() *
                                 static_cast<std::int64_t>(edit_index)));
    const auto admission =
        checked_i1_time_add(nominal, std::chrono::milliseconds(1));
    const auto deadline = checked_i1_time_add(admission, kI1DeadlineBudget);
    input.edits[edit_index] = I1EditEvidence{
        edit_index,
        kI1EditCoefficients[edit_index],
        i1_edit_region(edit_index),
        nominal,
        true,
        admission,
        true,
        static_cast<std::uint64_t>(edit_index + 1U),
        deadline,
        I1HostReturnEvidence{
            checked_i1_time_add(admission, std::chrono::microseconds(100)),
            OperationStatus{}, true},
        I1AcceptedCoordinate(admission, edit_index + 1U),
        OperationStatus{}};

    const std::uint64_t generation = edit_index + 1U;
    const std::uint64_t run_id = 100U + edit_index;
    input.observations.current_generations.push_back(
        I1ObservedCurrentGeneration{
            edit_index, generation, admission, causal_sequence++,
            I1AcceptedCoordinate(admission, edit_index + 1U)});
    input.observations.service_starts.push_back(I1ObservedServiceStart{
        edit_index, run_id, generation, 0U, compute::ComputeRunQuality::Full,
        compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                               deadline, 1U, 8U},
        100U, checked_i1_time_add(admission, std::chrono::milliseconds(1)),
        causal_sequence++});
    const auto visible_at =
        checked_i1_time_add(admission, edit_index + 1U == kI1EditCount
                                           ? final_latency
                                           : std::chrono::milliseconds(2));
    input.observations.visible_outputs.push_back(I1ObservedVisibleOutput{
        edit_index, run_id, generation, visible_at, causal_sequence++, output,
        false, std::nullopt});
    input.observations.terminals.push_back(I1ObservedTerminal{
        edit_index, run_id, generation,
        compute::ComputeRunTerminalKind::Succeeded,
        checked_i1_time_add(visible_at, std::chrono::microseconds(1)),
        causal_sequence++});
    input.observations.run_quiescences.push_back(
        I1ObservedRunLifecycleTransition{
            edit_index, run_id, generation,
            checked_i1_time_add(visible_at, std::chrono::microseconds(2)),
            causal_sequence++});
    input.observations.resource_settlements.push_back(
        I1ObservedRunLifecycleTransition{
            edit_index, run_id, generation,
            checked_i1_time_add(visible_at, std::chrono::microseconds(3)),
            causal_sequence++});
    input.observations.host_settlements.push_back(I1ObservedHostSettlement{
        edit_index,
        checked_i1_time_add(visible_at, std::chrono::microseconds(4)),
        causal_sequence++});
  }
  input.observation_cut =
      I1ObservationHistoryCut{input.measurement_end, causal_sequence};
  for (I1ObservedVisibleOutput& visible : input.observations.visible_outputs) {
    freeze_i1_visible_output_digest(&visible);
  }
  input.observations.visible_outputs.back().content_digest =
      ContentDigestResult{ContentDigestState::Available,
                          i1_frozen_final_content_digest(),
                          {}};
  return input;
}

/**
 * @brief Attaches exact Issue #93 sources and overwrites all derived I1 fields.
 * @param input Mutable M1 input with a complete ordered occurrence projection.
 * @return Nothing after all 48 source rows replay and bind exactly.
 * @throws std::invalid_argument for null or malformed occurrence input.
 * @throws Replay, checked-time, digest, and allocation failures unchanged.
 */
inline void attach_m1_test_i1_sources(M1InnerRowInput* input) {
  if (input == nullptr ||
      input->protocol.interactive_occurrences.size() != kM1TotalI1OriginCount) {
    throw std::invalid_argument(
        "M1 test requires exactly 48 I1 occurrence projections.");
  }
  input->interactive_sources.clear();
  for (M1InteractiveOccurrenceEvidence& occurrence :
       input->protocol.interactive_occurrences) {
    const I1EpisodeInnerRow row = evaluate_i1_episode(make_m1_test_i1_episode(
        occurrence.phase, occurrence.phase_ordinal, input->replicate_ordinal,
        occurrence.origin.timestamp));
    occurrence.final_latency = row.final_latency;
    occurrence.service = row.service;
    occurrence.latency_verdict = row.latency_verdict;
    occurrence.waste_verdict = row.waste_verdict;
    occurrence.memory_verdict = row.memory_verdict;
    occurrence.output_verdict = row.output_verdict;
    input->interactive_sources.push_back(make_m1_interactive_source_evidence(
        occurrence.phase, occurrence.phase_ordinal, occurrence.origin, row));
  }
}

/**
 * @brief Creates one event-aligned B1 I/O snapshot for source fixtures.
 * @param active_tasks Zero or one charged task.
 * @param active_bytes Exact charge paired with the active task.
 * @return Frozen-limit executor snapshot.
 * @throws Nothing.
 */
inline execution::ComputeIoExecutorSnapshot make_m1_test_io_snapshot(
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
 * @brief Builds the exact successful two-stage B1 I/O stream.
 * @param job Complete immutable occurrence.
 * @param first_sequence First of four globally unique accounting sequences.
 * @return Initial, admissions, settlements, and final boundary.
 * @throws Validation and allocation failures unchanged.
 */
inline std::vector<B1ComputeIoObservation> make_m1_test_io_observations(
    const B1JobInstance& job, std::uint64_t first_sequence) {
  const B1IoTaskIdentity payload{job, B1IoStage::PayloadStage, 0U};
  const B1IoTaskIdentity manifest{job, B1IoStage::ManifestCommit, 0U};
  const std::uint64_t manifest_bytes = b1_manifest_length(job.job_index);
  const auto zero = make_m1_test_io_snapshot(0U, 0U);
  const auto payload_active = make_m1_test_io_snapshot(1U, kB1PayloadBytes);
  const auto manifest_active = make_m1_test_io_snapshot(1U, manifest_bytes);
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
  return {{B1IoObservationPoint::Initial, std::nullopt, 0U, std::nullopt,
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
}

/**
 * @brief Computes one exact source service charge without overflow.
 * @param resources Frozen semantic task resource vector.
 * @return Work units plus 4096-byte ready quanta.
 * @throws std::overflow_error when the exact sum is unrepresentable.
 */
inline std::uint64_t m1_test_service_charge(
    const B1SemanticResourceVector& resources) {
  const std::uint64_t ready_quanta =
      resources.ready_bytes / 4096U +
      static_cast<std::uint64_t>(resources.ready_bytes % 4096U != 0U);
  if (resources.work_units >
      std::numeric_limits<std::uint64_t>::max() - ready_quanta) {
    throw std::overflow_error("M1 test service charge overflowed.");
  }
  return resources.work_units + ready_quanta;
}

/**
 * @brief Builds one complete successful B1 source-private job.
 * @param offer Exact protocol offer and endpoint.
 * @param source_ordinal Unique row-local source ordinal for Run identity.
 * @param io_sequence First globally unique I/O accounting sequence.
 * @return Complete raw job evidence accepted by M1 source replay.
 * @throws Validation, digest, receipt, and allocation failures unchanged.
 */
inline B1JobEvidence make_m1_test_batch_job(const M1BatchOfferEvidence& offer,
                                            std::size_t source_ordinal,
                                            std::uint64_t io_sequence) {
  if (!offer.endpoint.has_value()) {
    throw std::invalid_argument("M1 test B1 offer lacks an endpoint.");
  }
  B1JobEvidence evidence;
  evidence.job = offer.job;
  evidence.producer_offer_ordinal = offer.producer_offer_ordinal;
  evidence.offered_at = offer.offered.timestamp;
  evidence.endpoint_at = offer.endpoint->timestamp;
  evidence.run_succeeded = true;
  evidence.golden = b1_frozen_job_golden(offer.job.job_index);

  const std::uint64_t run_id = 100000U + source_ordinal;
  evidence.physical_trace.job = offer.job;
  evidence.physical_trace.current_generations.push_back(
      B1ObservedCurrentGeneration{1U, compute::ComputeRunObservationCoordinate{
                                          evidence.offered_at, 1U}});
  const std::vector<B1SemanticTask> plan =
      b1_frozen_semantic_plan(offer.job.job_index);
  for (std::uint64_t task = 0U; task < kB1TasksPerJob; ++task) {
    const B1SemanticTask& planned = plan.at(static_cast<std::size_t>(task));
    if (planned.dependencies.size() > kB1ObservedDependencyCapacity) {
      throw std::logic_error("M1 test B1 dependency capacity drifted.");
    }
    B1ObservedTaskReady ready;
    ready.run_id = run_id;
    ready.local_task_id = task;
    ready.dependency_count = planned.dependencies.size();
    std::copy(planned.dependencies.begin(), planned.dependencies.end(),
              ready.dependencies.begin());
    ready.declared_ready_bytes = 0U;
    ready.resources = planned.resources;
    ready.coordinate = compute::ComputeRunObservationCoordinate{
        evidence.offered_at, 2U + task * 3U};
    evidence.physical_trace.task_readies.push_back(ready);
    evidence.physical_trace.service_starts.push_back(B1ObservedServiceStart{
        run_id, task, m1_test_service_charge(planned.resources),
        compute::ComputeRunQos{compute::ComputeRunQosClass::Throughput,
                               std::nullopt, 1U,
                               static_cast<std::uint32_t>(offer.job.run_cap)},
        compute::ComputeRunObservationCoordinate{evidence.offered_at,
                                                 3U + task * 3U}});
    evidence.physical_trace.task_terminals.push_back(B1ObservedTaskTerminal{
        run_id, task, compute::ComputeRunTaskTerminalKind::Succeeded,
        compute::ComputeRunObservationCoordinate{evidence.offered_at,
                                                 4U + task * 3U}});
  }
  evidence.physical_trace.visible = B1ObservedRunTransition{
      run_id,
      compute::ComputeRunObservationCoordinate{evidence.offered_at, 773U}};
  evidence.physical_trace.terminal_kind =
      compute::ComputeRunTerminalKind::Succeeded;
  evidence.physical_trace.terminal = B1ObservedRunTransition{
      run_id,
      compute::ComputeRunObservationCoordinate{evidence.offered_at, 774U}};
  evidence.physical_trace.quiescent = B1ObservedRunTransition{
      run_id,
      compute::ComputeRunObservationCoordinate{evidence.offered_at, 775U}};
  evidence.physical_trace.resource_settled = B1ObservedRunTransition{
      run_id,
      compute::ComputeRunObservationCoordinate{evidence.offered_at, 776U}};
  evidence.physical_trace.visible_content_digest =
      ContentDigestResult{ContentDigestState::Available,
                          evidence.golden.logical_digest,
                          {}};
  evidence.semantic_trace = encode_b1_semantic_trace(
      make_b1_observed_semantic_records(evidence.physical_trace));
  evidence.semantic_trace_digest = b1_sha256(evidence.semantic_trace);

  B1Sha256 commit_hash;
  commit_hash.update("execution-profile-output-commit-id-v1\n");
  commit_hash.update(encode_b1_job_instance(offer.job));
  const std::string commit_id = b1_digest_hex(commit_hash.finish());
  const std::string manifest = b1_artifact_manifest(
      offer.job.job_index, evidence.golden.raw_payload_digest);
  evidence.output.status = B1OutputCommitStatus::Succeeded;
  evidence.output.receipt = B1OutputCommitReceiptTestAccess::mint(
      commit_id, std::filesystem::path("/tmp/photospider-m1-test-output"),
      std::filesystem::path("occurrence-" + commit_id), offer.job,
      "dense-tensor-hwc-fp32-rgba-2048x2048", evidence.golden.logical_digest,
      1U, "output.rgba32le", "manifest.txt", kB1PayloadBytes,
      b1_manifest_length(offer.job.job_index),
      evidence.golden.raw_payload_digest, b1_sha256(manifest),
      B1OutputDurability::CrashDurable, B1OutputDurability::CrashDurable,
      "dev=1;ino=1");
  evidence.output.io_observations =
      make_m1_test_io_observations(offer.job, io_sequence);
  return evidence;
}

/**
 * @brief Attaches complete B1 sources and derives the canonical waste
 * projection.
 * @param input Mutable M1 input with complete ordered protocol offers.
 * @return Nothing after every offer owns one replayable source.
 * @throws std::invalid_argument for null input.
 * @throws Source construction, replay, digest, and allocation failures
 * unchanged.
 */
inline void attach_m1_test_batch_sources(M1InnerRowInput* input) {
  if (input == nullptr) {
    throw std::invalid_argument("M1 test input is null.");
  }
  input->batch_sources.clear();
  std::uint64_t io_sequence = 1U;
  for (std::size_t index = 0U; index < input->protocol.batch_offers.size();
       ++index) {
    input->batch_sources.push_back(
        make_m1_batch_source_evidence(make_m1_test_batch_job(
            input->protocol.batch_offers[index], index, io_sequence)));
    io_sequence += 4U;
  }
  input->batch_waste = derive_m1_batch_waste_evidence(input->batch_sources);
}

/**
 * @brief Rebuilds and attaches the shared source-derived fairness projection.
 * @param input Mutable M1 input with complete protocol and I1/B1 sources.
 * @return Nothing after progress, Graph, headroom outcomes, and aggregates are
 * replaced by the production derivation.
 * @throws std::invalid_argument when `input` is null or source joins drift.
 * @throws std::overflow_error when checked source aggregation overflows.
 * @throws std::bad_alloc when replay or projection storage allocates.
 * @note Pair denominators, class-start observations, and observer health flags
 * remain unchanged because they have independent authorities.
 */
inline void attach_m1_test_source_fairness_projection(M1InnerRowInput* input) {
  if (input == nullptr) {
    throw std::invalid_argument("M1 test fairness input is null.");
  }
  M1SourceFairnessProjection projection = derive_m1_source_fairness_projection(
      input->protocol, input->interactive_sources, input->batch_sources);
  input->fairness.progress_windows = std::move(projection.progress_windows);
  input->fairness.graph_service_windows =
      std::move(projection.graph_service_windows);
  input->fairness.headroom_admissions = projection.headroom_admissions;
  input->fairness.headroom_outcomes = std::move(projection.headroom_outcomes);
}

}  // namespace ps::benchmark::testing
