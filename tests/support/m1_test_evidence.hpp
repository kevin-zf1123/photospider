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

#include "benchmark/m1_canonical.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/m1_evidence.hpp"   // NOLINT(build/include_subdir)
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
 * @param first_event_sequence First nonzero row-local admission sequence.
 * @param final_latency Final accepted-to-visible duration.
 * @return Complete raw evaluator input with no discarded or post-cancel work.
 * @throws std::invalid_argument when the first event sequence is zero.
 * @throws std::overflow_error when the twelve-sequence block would wrap.
 * @throws Checked-time, Value/digest, and allocation failures unchanged.
 */
inline I1EpisodeEvidenceInput make_m1_test_i1_episode(
    B1JobPhase phase, std::size_t ordinal, std::uint64_t replicate_ordinal,
    std::chrono::steady_clock::time_point origin,
    std::uint64_t first_event_sequence,
    std::chrono::nanoseconds final_latency = std::chrono::milliseconds(10)) {
  if (first_event_sequence == 0U) {
    throw std::invalid_argument("M1 test I1 sequence block starts at zero.");
  }
  if (first_event_sequence >
      std::numeric_limits<std::uint64_t>::max() - (kI1EditCount - 1U)) {
    throw std::overflow_error("M1 test I1 sequence block wraps.");
  }
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
        first_event_sequence + static_cast<std::uint64_t>(edit_index),
        deadline,
        I1HostReturnEvidence{
            checked_i1_time_add(admission, std::chrono::microseconds(100)),
            OperationStatus{}, true},
        I1AcceptedCoordinate(
            admission,
            first_event_sequence + static_cast<std::uint64_t>(edit_index)),
        OperationStatus{}};

    const std::uint64_t generation = edit_index + 1U;
    const std::uint64_t run_id = 100U + edit_index;
    input.observations.current_generations.push_back(
        I1ObservedCurrentGeneration{
            edit_index, generation, admission, causal_sequence++,
            I1AcceptedCoordinate(admission,
                                 first_event_sequence +
                                     static_cast<std::uint64_t>(edit_index))});
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
  for (std::size_t index = 0U;
       index < input->protocol.interactive_occurrences.size(); ++index) {
    M1InteractiveOccurrenceEvidence& occurrence =
        input->protocol.interactive_occurrences[index];
    const bool first_measured = occurrence.phase == B1JobPhase::Measured &&
                                occurrence.phase_ordinal == 0U;
    const std::uint64_t first_event_sequence =
        first_measured && input->protocol.first_measured_admission
                              .reserved_event_sequence.has_value()
            ? *input->protocol.first_measured_admission.reserved_event_sequence
            : 1U;
    const I1EpisodeInnerRow row = evaluate_i1_episode(make_m1_test_i1_episode(
        occurrence.phase, occurrence.phase_ordinal, input->replicate_ordinal,
        occurrence.origin.timestamp, first_event_sequence));
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
 * @brief Replays one retained I1 source into its matching M1 occurrence.
 * @param input Mutable M1 input owning exact ordered source/occurrence lists.
 * @param index Zero-based shared index in both exact 48-element lists.
 * @return Nothing after all Issue #93-derived occurrence fields are replaced.
 * @throws std::invalid_argument for null, out-of-range, or identity drift.
 * @throws Replay and allocation failures unchanged.
 * @note M1-only current-hold and settlement-at-B fields remain untouched; the
 * shared source-fairness projection owns those fields separately.
 */
inline void synchronize_m1_test_i1_occurrence(M1InnerRowInput* input,
                                              std::size_t index) {
  if (input == nullptr || index >= input->interactive_sources.size() ||
      index >= input->protocol.interactive_occurrences.size()) {
    throw std::invalid_argument(
        "M1 test I1 occurrence synchronization index is invalid.");
  }
  const M1InteractiveSourceEvidence& source = input->interactive_sources[index];
  M1InteractiveOccurrenceEvidence& occurrence =
      input->protocol.interactive_occurrences[index];
  if (source.phase != occurrence.phase ||
      source.phase_ordinal != occurrence.phase_ordinal ||
      !(source.origin == occurrence.origin)) {
    throw std::invalid_argument(
        "M1 test I1 source and occurrence identities differ.");
  }
  const I1EpisodeInnerRow replay = evaluate_i1_episode(source.episode);
  occurrence.final_latency = replay.final_latency;
  occurrence.service = replay.service;
  occurrence.latency_verdict = replay.latency_verdict;
  occurrence.waste_verdict = replay.waste_verdict;
  occurrence.memory_verdict = replay.memory_verdict;
  occurrence.output_verdict = replay.output_verdict;
}

/**
 * @brief Creates the Issue #96 equal-time measured-current supersession case.
 * @param input Mutable complete M1 test input with all 48 retained I1 sources.
 * @param cancellation_follows_current Whether cancellation is `(B,n+1)`;
 * false produces the fail-closed reverse order `(B,n-1)`.
 * @return Nothing after raw sources and their Issue #93 projections agree.
 * @throws std::invalid_argument for null, malformed, or unexpected fixtures.
 * @throws std::overflow_error when observer-sequence remapping would wrap.
 * @throws Replay, checked-time, and allocation failures unchanged.
 * @note The measured observer coordinate is deliberately independent from the
 * retained accepted-row coordinate. The final-warmup Run already published a
 * successful visible output, so adding an accepted cancellation makes that
 * source independently Invalid under Issue #93; callers must not interpret
 * that separate verdict as an M1 current-hold/source-closure failure.
 */
inline void configure_m1_test_equal_time_supersession(
    M1InnerRowInput* input, bool cancellation_follows_current) {
  constexpr std::size_t final_warmup_index =
      kM1ColdI1OriginCount + kM1WarmupI1OriginCount - 1U;
  constexpr std::size_t measured_zero_index =
      kM1ColdI1OriginCount + kM1WarmupI1OriginCount;
  constexpr std::uint64_t measured_current_sequence = 100000U;
  if (input == nullptr ||
      input->interactive_sources.size() != kM1TotalI1OriginCount ||
      input->protocol.interactive_occurrences.size() != kM1TotalI1OriginCount) {
    throw std::invalid_argument(
        "M1 equal-time test requires exactly 48 I1 sources and occurrences.");
  }

  I1EpisodeEvidenceInput& measured =
      input->interactive_sources[measured_zero_index].episode;
  I1EditEvidence& measured_edit = measured.edits[0U];
  const auto measured_current =
      std::find_if(measured.observations.current_generations.begin(),
                   measured.observations.current_generations.end(),
                   [](const I1ObservedCurrentGeneration& current) {
                     return current.edit_index == 0U;
                   });
  if (measured_edit.edit_index != 0U ||
      !measured_edit.reserved_event_sequence.has_value() ||
      !measured_edit.host_return.has_value() ||
      measured_current == measured.observations.current_generations.end() ||
      measured_current->causal_sequence != 1U) {
    throw std::invalid_argument(
        "M1 equal-time test measured edit-zero source is malformed.");
  }

  const auto boundary = input->protocol.boundaries.measurement_start.timestamp;
  measured_edit.admission_sample = boundary;
  measured_edit.deadline = checked_i1_time_add(boundary, kI1DeadlineBudget);
  measured_edit.host_return->return_time =
      checked_i1_time_add(boundary, std::chrono::microseconds(100));
  measured_edit.accepted_coordinate.emplace(
      boundary, *measured_edit.reserved_event_sequence);
  measured_current->observed_at = boundary;
  measured_current->accepted_coordinate = measured_edit.accepted_coordinate;
  for (I1ObservedServiceStart& start : measured.observations.service_starts) {
    if (start.edit_index == 0U) {
      start.qos.deadline = *measured_edit.deadline;
    }
  }

  // Preserve global observer order while reserving n+1 for supersession.
  const auto remap_sequence = [](std::uint64_t* sequence) {
    if (sequence == nullptr || *sequence == 0U ||
        (*sequence != 1U &&
         *sequence > std::numeric_limits<std::uint64_t>::max() -
                         measured_current_sequence)) {
      throw std::overflow_error(
          "M1 equal-time test observer sequence cannot be remapped.");
    }
    *sequence = *sequence == 1U ? measured_current_sequence
                                : measured_current_sequence + *sequence;
  };
  for (I1ObservedCurrentGeneration& event :
       measured.observations.current_generations) {
    remap_sequence(&event.causal_sequence);
  }
  for (I1ObservedServiceStart& event : measured.observations.service_starts) {
    remap_sequence(&event.causal_sequence);
  }
  for (I1ObservedCancellation& event : measured.observations.cancellations) {
    remap_sequence(&event.causal_sequence);
  }
  for (I1ObservedTerminal& event : measured.observations.terminals) {
    remap_sequence(&event.causal_sequence);
  }
  for (I1ObservedVisibleOutput& event : measured.observations.visible_outputs) {
    remap_sequence(&event.causal_sequence);
  }
  for (I1ObservedRunLifecycleTransition& event :
       measured.observations.run_quiescences) {
    remap_sequence(&event.causal_sequence);
  }
  for (I1ObservedRunLifecycleTransition& event :
       measured.observations.resource_settlements) {
    remap_sequence(&event.causal_sequence);
  }
  for (I1ObservedHostSettlement& event :
       measured.observations.host_settlements) {
    remap_sequence(&event.causal_sequence);
  }
  remap_sequence(&measured.observation_cut.causal_sequence);

  I1EpisodeEvidenceInput& final_warmup =
      input->interactive_sources[final_warmup_index].episode;
  const auto warmup_current =
      std::find_if(final_warmup.observations.current_generations.begin(),
                   final_warmup.observations.current_generations.end(),
                   [](const I1ObservedCurrentGeneration& current) {
                     return current.edit_index + 1U == kI1EditCount;
                   });
  const auto warmup_visible =
      std::find_if(final_warmup.observations.visible_outputs.begin(),
                   final_warmup.observations.visible_outputs.end(),
                   [](const I1ObservedVisibleOutput& visible) {
                     return visible.edit_index + 1U == kI1EditCount;
                   });
  if (warmup_current == final_warmup.observations.current_generations.end() ||
      warmup_visible == final_warmup.observations.visible_outputs.end() ||
      warmup_current->generation != warmup_visible->generation ||
      !final_warmup.observations.cancellations.empty()) {
    throw std::invalid_argument(
        "M1 equal-time test final-warmup source is malformed.");
  }
  const std::uint64_t cancellation_sequence =
      cancellation_follows_current ? measured_current_sequence + 1U
                                   : measured_current_sequence - 1U;
  final_warmup.observations.cancellations.push_back(I1ObservedCancellation{
      kI1EditCount - 1U, warmup_visible->run_id, warmup_current->generation,
      compute::ComputeRunCancellationReason::Superseded, boundary,
      cancellation_sequence});
  final_warmup.observation_cut.causal_sequence = std::max(
      final_warmup.observation_cut.causal_sequence, cancellation_sequence + 1U);

  synchronize_m1_test_i1_occurrence(input, final_warmup_index);
  synchronize_m1_test_i1_occurrence(input, measured_zero_index);
}

/**
 * @brief Reverses only the equal-time cancellation/current canonical order.
 * @param canonical Complete source-closed canonical M1 test row containing the
 * `(B,n)` measured current and exactly one `(B,n+1)` warmup cancellation.
 * @return Re-encoded row whose cancellation sequence is `n-1` while every
 * retained projection, verdict claim, and enclosing field remains unchanged.
 * @throws std::invalid_argument for malformed or unexpected canonical input.
 * @throws Canonical framing and allocation failures unchanged.
 * @note This mutation deliberately creates a source/current-hold closure
 * contradiction for strict reader and fully rehashed outer-envelope tests.
 */
inline std::string reverse_m1_test_equal_time_cancellation_order(
    std::string canonical) {
  const auto encode_records = [](const std::vector<std::string>& records) {
    std::string encoded = std::to_string(records.size()) + ":";
    for (const std::string& record : records) {
      encoded.append(b1_environment_frame(record));
    }
    return encoded;
  };

  B1CanonicalManifest manifest = parse_b1_canonical_manifest(canonical);
  if (manifest.fields.size() != 20U) {
    throw std::invalid_argument(
        "M1 equal-time canonical test row has unexpected field count.");
  }
  std::vector<std::string> sources =
      parse_b1_framed_list(manifest.fields[5U].payload);
  constexpr std::size_t final_warmup_index =
      kM1ColdI1OriginCount + kM1WarmupI1OriginCount - 1U;
  if (sources.size() != kM1TotalI1OriginCount) {
    throw std::invalid_argument(
        "M1 equal-time canonical test row has unexpected source count.");
  }
  std::vector<std::string> source =
      parse_b1_fixed_record(sources[final_warmup_index], 5U);
  std::vector<std::string> episode = parse_b1_fixed_record(source[4U], 14U);
  std::vector<std::string> observations =
      parse_b1_fixed_record(episode[9U], 9U);
  std::vector<std::string> cancellations =
      parse_b1_framed_list(observations[2U]);
  if (cancellations.size() != 1U) {
    throw std::invalid_argument(
        "M1 equal-time canonical test row lacks one cancellation.");
  }
  std::vector<std::string> cancellation =
      parse_b1_fixed_record(cancellations[0U], 6U);
  const std::uint64_t following_sequence =
      parse_b1_canonical_uint64(cancellation[5U]);
  if (following_sequence <= 2U) {
    throw std::invalid_argument(
        "M1 equal-time canonical cancellation cannot move before current.");
  }
  cancellation[5U] = std::to_string(following_sequence - 2U);
  cancellations[0U] = encode_b1_fixed_record(cancellation);
  observations[2U] = encode_records(cancellations);
  episode[9U] = encode_b1_fixed_record(observations);
  source[4U] = encode_b1_fixed_record(episode);
  sources[final_warmup_index] = encode_b1_fixed_record(source);
  manifest.fields[5U].payload = encode_records(sources);
  return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
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
 * @return Nothing after first admission/current hold, progress, Graph,
 * headroom outcomes, and aggregates are replaced by production derivation.
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
  input->protocol.first_measured_admission =
      projection.first_measured_admission;
  const std::size_t final_warmup_index =
      kM1ColdI1OriginCount + kM1WarmupI1OriginCount - 1U;
  input->protocol.interactive_occurrences[final_warmup_index]
      .publication_current_at_measurement =
      projection.first_measured_admission
          .warmup_publication_current_before_acceptance;
  input->protocol.interactive_occurrences[final_warmup_index]
      .settlement_pending_at_measurement =
      projection.final_warmup_settlement_pending_at_measurement;
  input->fairness.progress_windows = std::move(projection.progress_windows);
  input->fairness.graph_service_windows =
      std::move(projection.graph_service_windows);
  input->fairness.headroom_admissions = projection.headroom_admissions;
  input->fairness.headroom_outcomes = std::move(projection.headroom_outcomes);
}

}  // namespace ps::benchmark::testing
