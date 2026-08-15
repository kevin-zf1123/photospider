/**
 * @file m1_evidence.cpp
 * @brief Implements the fail-closed five-axis M1 inner-row evaluator.
 */
#include "benchmark/m1_evidence.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ps::benchmark {
namespace {

// NOLINTBEGIN(whitespace/indent_namespace)

/** @brief Frozen process Host limits from the execution-profile contract. */
constexpr ResourceVector kM1HostLimits{32U, 1073741824U, 536870912U, 65536U,
                                       268435456U};

/** @brief Frozen Throughput capacity after Interactive headroom subtraction. */
constexpr ResourceVector kM1ThroughputCapacity{31U, 1006632960U, 503316480U,
                                               64512U, 251658240U};

/** @brief Frozen configured-Metal memory and scratch limits. */
constexpr DeviceResourceVector kM1MetalLimits{536870912U, 268435456U};

// NOLINTEND

/**
 * @brief Appends one stable diagnostic once.
 * @param reasons Mutable diagnostic collection.
 * @param reason Complete stable reason.
 * @return Nothing.
 * @throws std::bad_alloc when reason ownership allocates.
 */
void invalidate_m1(std::vector<std::string>* reasons, std::string reason) {
  if (std::find(reasons->begin(), reasons->end(), reason) == reasons->end()) {
    reasons->push_back(std::move(reason));
  }
}

/**
 * @brief Adds one uint64 contribution without wraparound.
 * @param value Mutable aggregate.
 * @param contribution Nonnegative contribution.
 * @return True when the exact sum was stored.
 * @throws Nothing.
 */
bool checked_accumulate(std::uint64_t* value,
                        std::uint64_t contribution) noexcept {
  if (*value > std::numeric_limits<std::uint64_t>::max() - contribution) {
    return false;
  }
  *value += contribution;
  return true;
}

/**
 * @brief Increments one size aggregate without wraparound.
 * @param value Mutable aggregate.
 * @return True when the exact increment was stored.
 * @throws Nothing.
 */
bool checked_size_increment(std::size_t* value) noexcept {
  if (*value == std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  ++*value;
  return true;
}

/**
 * @brief Returns the exact Issue #93 slot assigned to one M1 occurrence.
 * @param phase Immutable M1 phase.
 * @param ordinal Zero-based phase-local ordinal.
 * @return Cold slot zero, warmup slots one through seven, or measured slots
 * twenty-one through sixty.
 * @throws std::invalid_argument for an unsupported phase/ordinal pair.
 */
std::size_t m1_i1_source_slot(B1JobPhase phase, std::size_t ordinal) {
  switch (phase) {
    case B1JobPhase::Cold:
      if (ordinal == 0U) {
        return 0U;
      }
      break;
    case B1JobPhase::Warmup:
      if (ordinal < kM1WarmupI1OriginCount) {
        return 1U + ordinal;
      }
      break;
    case B1JobPhase::Measured:
      if (ordinal < kM1MeasuredI1OriginCount) {
        return kI1WarmupSlotCount + 1U + ordinal;
      }
      break;
  }
  throw std::invalid_argument("M1 I1 source phase/ordinal is invalid.");
}

/**
 * @brief Tests whether one I1 source replay exactly matches its M1 projection.
 * @param source Retained source identity and raw Issue #93 input.
 * @param occurrence Derived M1 projection to verify.
 * @param replicate_ordinal Enclosing M1 replicate identity.
 * @return True only when identity, latency, service, and four verdicts match.
 * @throws std::bad_alloc when Issue #93 replay allocates.
 * @throws Checked Issue #93 evaluation failures unchanged.
 */
bool m1_i1_source_matches(const M1InteractiveSourceEvidence& source,
                          const M1InteractiveOccurrenceEvidence& occurrence,
                          std::uint64_t replicate_ordinal) {
  if (source.phase != occurrence.phase ||
      source.phase_ordinal != occurrence.phase_ordinal ||
      !(source.origin == occurrence.origin) ||
      source.episode.replicate_ordinal != replicate_ordinal ||
      source.episode.slot !=
          m1_i1_source_slot(source.phase, source.phase_ordinal) ||
      source.episode.episode_origin != source.origin.timestamp) {
    return false;
  }
  const I1EpisodeInnerRow replay = evaluate_i1_episode(source.episode);
  return replay.final_latency == occurrence.final_latency &&
         replay.service.all_started_service ==
             occurrence.service.all_started_service &&
         replay.service.discarded_started_service ==
             occurrence.service.discarded_started_service &&
         replay.service.post_cancel_started_service ==
             occurrence.service.post_cancel_started_service &&
         replay.latency_verdict == occurrence.latency_verdict &&
         replay.waste_verdict == occurrence.waste_verdict &&
         replay.memory_verdict == occurrence.memory_verdict &&
         replay.output_verdict == occurrence.output_verdict;
}

/**
 * @brief Compares two optional accepted-boundary coordinates exactly.
 * @param lhs First optional coordinate.
 * @param rhs Second optional coordinate.
 * @return True only when absence or both scalar components match.
 * @throws Nothing.
 */
bool same_m1_accepted_coordinate(
    const std::optional<I1AcceptedCoordinate>& lhs,
    const std::optional<I1AcceptedCoordinate>& rhs) noexcept {
  return lhs.has_value() == rhs.has_value() &&
         (!lhs.has_value() || (lhs->admission_time() == rhs->admission_time() &&
                               lhs->event_sequence() == rhs->event_sequence()));
}

/**
 * @brief Orders two product observation coordinates lexicographically.
 * @param lhs_time First steady-clock sample.
 * @param lhs_sequence First nonzero shared observation sequence.
 * @param rhs_time Second steady-clock sample.
 * @param rhs_sequence Second nonzero shared observation sequence.
 * @return True only when the first coordinate precedes the second.
 * @throws Nothing.
 * @note M1's shared coordinate authority guarantees nondecreasing time in
 * sequence order; the sequence breaks equal-time ties without minting a new
 * acceptance or boundary rule.
 */
bool m1_observation_precedes(std::chrono::steady_clock::time_point lhs_time,
                             std::uint64_t lhs_sequence,
                             std::chrono::steady_clock::time_point rhs_time,
                             std::uint64_t rhs_sequence) noexcept {
  return lhs_time < rhs_time ||
         (lhs_time == rhs_time && lhs_sequence < rhs_sequence);
}

/**
 * @brief Tests whether one cancellation strictly follows measured current.
 * @param cancellation Final-warmup cancellation observation to classify.
 * @param measured_current Unique measured edit-zero current publication, or
 * null when the retained source cannot bind one.
 * @return True only when both events share the M1 observer domain and the
 * measured-current coordinate lexicographically precedes cancellation.
 * @throws Nothing.
 * @note The sequence belongs to the replicate-wide M1 observation fanout. It
 * must never be compared with the independent row-local accepted coordinate.
 * Missing current evidence and duplicate coordinates fail closed.
 */
bool m1_cancellation_follows_measured_current(
    const I1ObservedCancellation& cancellation,
    const I1ObservedCurrentGeneration* measured_current) noexcept {
  return measured_current != nullptr &&
         m1_observation_precedes(
             measured_current->observed_at, measured_current->causal_sequence,
             cancellation.observed_at, cancellation.causal_sequence);
}

/**
 * @brief Finds exactly one current-generation record for an edit.
 * @param source Complete retained Issue #93 source.
 * @param edit_index Exact zero-based edit identity.
 * @return Matching record, or null for missing/duplicate evidence.
 * @throws Nothing.
 */
const I1ObservedCurrentGeneration* unique_m1_current_generation(
    const M1InteractiveSourceEvidence& source,
    std::size_t edit_index) noexcept {
  const I1ObservedCurrentGeneration* result = nullptr;
  for (const I1ObservedCurrentGeneration& current :
       source.episode.observations.current_generations) {
    if (current.edit_index != edit_index) {
      continue;
    }
    if (result != nullptr) {
      return nullptr;
    }
    result = &current;
  }
  return result;
}

/**
 * @brief Finds exactly one visible publication for an edit and generation.
 * @param source Complete retained Issue #93 source.
 * @param edit_index Exact zero-based edit identity.
 * @param generation Exact nonzero product generation.
 * @return Matching record, or null for missing/duplicate evidence.
 * @throws Nothing.
 */
const I1ObservedVisibleOutput* unique_m1_visible_output(
    const M1InteractiveSourceEvidence& source, std::size_t edit_index,
    std::uint64_t generation) noexcept {
  const I1ObservedVisibleOutput* result = nullptr;
  for (const I1ObservedVisibleOutput& visible :
       source.episode.observations.visible_outputs) {
    if (visible.edit_index != edit_index || visible.generation != generation) {
      continue;
    }
    if (result != nullptr) {
      return nullptr;
    }
    result = &visible;
  }
  return result;
}

/**
 * @brief Source-only projection of the B-boundary current-hold exception.
 * @throws Nothing for scalar construction.
 */
struct M1AdmissionSourceProjection final {
  /** @brief Exact first measured admission/current replacement facts. */
  M1FirstMeasuredAdmissionEvidence admission;
  /** @brief Whether final warmup's immutable Q_end remained beyond B. */
  bool final_warmup_settlement_pending_at_measurement = false;
};

/**
 * @brief Derives first measured admission and old-publication replacement.
 * @param interactive_sources Exact ordered 48-source Issue #93 list.
 * @return Source-only admission/current-hold projection.
 * @throws std::invalid_argument for cardinality or boundary identities.
 * @note The rule reads no retained `first_measured_admission` field and is
 * therefore shared unchanged by the producer and canonical reader. Strictly
 * pre-B cancellation remains invalid. At B or later, only a cancellation that
 * strictly follows the measured-current publication in the replicate-wide M1
 * observer coordinate is ordinary accepted-coordinate supersession.
 */
M1AdmissionSourceProjection derive_m1_admission_source_projection(
    const std::vector<M1InteractiveSourceEvidence>& interactive_sources) {
  if (interactive_sources.size() != kM1TotalI1OriginCount) {
    throw std::invalid_argument(
        "M1 admission projection requires exactly 48 I1 sources.");
  }
  const std::size_t final_warmup_index =
      kM1ColdI1OriginCount + kM1WarmupI1OriginCount - 1U;
  const std::size_t measured_zero_index =
      kM1ColdI1OriginCount + kM1WarmupI1OriginCount;
  const M1InteractiveSourceEvidence& final_warmup =
      interactive_sources[final_warmup_index];
  const M1InteractiveSourceEvidence& measured_zero =
      interactive_sources[measured_zero_index];
  if (final_warmup.phase != B1JobPhase::Warmup ||
      final_warmup.phase_ordinal + 1U != kM1WarmupI1OriginCount ||
      measured_zero.phase != B1JobPhase::Measured ||
      measured_zero.phase_ordinal != 0U) {
    throw std::invalid_argument(
        "M1 admission projection source identities are out of order.");
  }

  const I1EditEvidence& warmup_final_edit =
      final_warmup.episode.edits[kI1EditCount - 1U];
  const I1EditEvidence& measured_first_edit = measured_zero.episode.edits[0U];
  const I1ObservedCurrentGeneration* warmup_current =
      unique_m1_current_generation(final_warmup, kI1EditCount - 1U);
  const I1ObservedCurrentGeneration* measured_current =
      unique_m1_current_generation(measured_zero, 0U);
  const I1ObservedVisibleOutput* warmup_visible =
      warmup_current == nullptr
          ? nullptr
          : unique_m1_visible_output(final_warmup, kI1EditCount - 1U,
                                     warmup_current->generation);

  const bool measured_coordinate_bound =
      measured_current != nullptr && measured_current->generation != 0U &&
      same_m1_accepted_coordinate(measured_current->accepted_coordinate,
                                  measured_first_edit.accepted_coordinate) &&
      measured_current->observed_at >= measured_first_edit.admission_sample;
  const I1ObservedCurrentGeneration* bound_measured_current =
      measured_coordinate_bound ? measured_current : nullptr;
  const bool warmup_coordinate_bound =
      warmup_current != nullptr && warmup_current->generation != 0U &&
      same_m1_accepted_coordinate(warmup_current->accepted_coordinate,
                                  warmup_final_edit.accepted_coordinate);
  const bool warmup_cancelled_before_measured_current =
      warmup_coordinate_bound &&
      std::any_of(
          final_warmup.episode.observations.cancellations.begin(),
          final_warmup.episode.observations.cancellations.end(),
          [&warmup_current, &bound_measured_current,
           &measured_first_edit](const I1ObservedCancellation& cancellation) {
            return cancellation.edit_index == kI1EditCount - 1U &&
                   cancellation.generation == warmup_current->generation &&
                   (cancellation.observed_at <
                        measured_first_edit.nominal_time ||
                    (cancellation.observed_at ==
                         measured_first_edit.nominal_time &&
                     !m1_cancellation_follows_measured_current(
                         cancellation, bound_measured_current)));
          });
  const bool warmup_publication_current =
      warmup_coordinate_bound && warmup_visible != nullptr &&
      warmup_current->observed_at <= warmup_visible->observed_at &&
      warmup_visible->observed_at < measured_first_edit.nominal_time &&
      !warmup_cancelled_before_measured_current;
  const bool replacement_ordered =
      warmup_publication_current && measured_coordinate_bound &&
      m1_observation_precedes(
          warmup_visible->observed_at, warmup_visible->causal_sequence,
          measured_current->observed_at, measured_current->causal_sequence);
  const bool boundary_only_cancellation = std::any_of(
      final_warmup.episode.observations.cancellations.begin(),
      final_warmup.episode.observations.cancellations.end(),
      [&bound_measured_current, &measured_first_edit,
       &warmup_current](const I1ObservedCancellation& cancellation) {
        return cancellation.edit_index == kI1EditCount - 1U &&
               warmup_current != nullptr &&
               cancellation.generation == warmup_current->generation &&
               cancellation.observed_at >= measured_first_edit.nominal_time &&
               !m1_cancellation_follows_measured_current(
                   cancellation, bound_measured_current);
      });

  M1AdmissionSourceProjection projection;
  projection.admission.edit_index = measured_first_edit.edit_index;
  projection.admission.nominal_time = measured_first_edit.nominal_time;
  projection.admission.attempted = measured_first_edit.admission_attempted;
  projection.admission.admission_sample = measured_first_edit.admission_sample;
  projection.admission.reserved_event_sequence =
      measured_first_edit.reserved_event_sequence;
  projection.admission.host_succeeded =
      measured_first_edit.host_return.has_value() &&
      measured_first_edit.host_return->status.ok &&
      measured_first_edit.host_return->future_valid;
  projection.admission.accepted_coordinate =
      measured_first_edit.accepted_coordinate;
  projection.admission.warmup_publication_current_before_acceptance =
      warmup_publication_current;
  projection.admission.superseded_exactly_at_acceptance = replacement_ordered;
  projection.admission.boundary_only_cancellation = boundary_only_cancellation;
  projection.admission.old_generation_settlement_endpoint =
      final_warmup.episode.measurement_end;
  projection.final_warmup_settlement_pending_at_measurement =
      final_warmup.episode.episode_origin < measured_first_edit.nominal_time &&
      measured_first_edit.nominal_time < final_warmup.episode.measurement_end;
  return projection;
}

/**
 * @brief Returns the frozen I1 identity assigned to one source-list index.
 * @param index Zero-based index in the exact 48-source list.
 * @return Cold/warmup/measured phase and its phase-local ordinal.
 * @throws std::invalid_argument when the index is outside the exact list.
 */
std::pair<B1JobPhase, std::size_t> expected_m1_i1_source_identity(
    std::size_t index) {
  if (index < kM1ColdI1OriginCount) {
    return {B1JobPhase::Cold, index};
  }
  if (index < kM1ColdI1OriginCount + kM1WarmupI1OriginCount) {
    return {B1JobPhase::Warmup, index - kM1ColdI1OriginCount};
  }
  if (index < kM1TotalI1OriginCount) {
    return {B1JobPhase::Measured,
            index - kM1ColdI1OriginCount - kM1WarmupI1OriginCount};
  }
  throw std::invalid_argument("M1 I1 source index exceeds 48 occurrences.");
}

/**
 * @brief Returns whether one B1 QoS value is the exact M1 Throughput shape.
 * @param qos Immutable source service-start QoS.
 * @param run_cap Exact occurrence cap.
 * @return True only for Throughput/no-deadline/weight-one/exact cap.
 * @throws Nothing.
 */
bool valid_m1_batch_qos(const compute::ComputeRunQos& qos,
                        std::uint64_t run_cap) noexcept {
  return qos.service_class == compute::ComputeRunQosClass::Throughput &&
         !qos.deadline.has_value() && qos.weight == 1U &&
         qos.maximum_parallelism.has_value() &&
         *qos.maximum_parallelism == run_cap;
}

/**
 * @brief Computes the stable B1 commit identity from one occurrence.
 * @param job Complete immutable B1 identity.
 * @return Lowercase SHA-256 commit id used by the output store.
 * @throws Digest, validation, and allocation failures unchanged.
 */
std::string expected_m1_batch_commit_id(const B1JobInstance& job) {
  B1Sha256 hash;
  hash.update("execution-profile-output-commit-id-v1\n");
  hash.update(encode_b1_job_instance(job));
  return b1_digest_hex(hash.finish());
}

/**
 * @brief Tests whether one observed rooted slot is a safe single component.
 * @param slot Authority-free copied path.
 * @return True only for one nonempty ordinary relative component.
 * @throws Nothing.
 */
bool valid_m1_batch_rooted_slot(const std::filesystem::path& slot) noexcept {
  return !slot.empty() && !slot.is_absolute() && slot.filename() == slot &&
         slot != "." && slot != "..";
}

/**
 * @brief Complete replay result for one authority-free B1 source.
 * @throws std::bad_alloc when diagnostics allocate.
 */
struct M1BatchSourceReplay final {
  /** @brief True when the retained raw source is lossless and coherent. */
  bool structurally_valid = true;
  /** @brief Exact Issue #95 successful endpoint eligibility. */
  bool verified_endpoint = false;
  /** @brief Sum of every retained physical service start. */
  std::uint64_t all_started_service = 0U;
  /** @brief Service not credited to a unique verified endpoint. */
  std::uint64_t discarded_started_service = 0U;
  /** @brief Service beginning after an accepted cancellation. */
  std::uint64_t post_cancellation_started_service = 0U;
  /** @brief Duplicate physical starts plus duplicate I/O admissions. */
  std::size_t duplicate_service_starts = 0U;
  /** @brief Explicit nonzero I/O attempt records. */
  std::size_t retry_service_starts = 0U;
};

/**
 * @brief Replays one complete authority-free B1 physical/output/I/O source.
 * @param source Retained canonical source evidence.
 * @return Verified endpoint and exact service accounting.
 * @throws std::bad_alloc when temporary semantic/index storage allocates.
 */
M1BatchSourceReplay replay_m1_batch_source(
    const M1BatchSourceEvidence& source) {
  M1BatchSourceReplay result;
  const B1RunObservationSnapshot& trace = source.physical_trace;
  if (!(trace.job == source.job) || trace.overflowed ||
      trace.current_generations.size() != 1U ||
      trace.current_generations.front().generation == 0U) {
    result.structurally_valid = false;
  }

  std::set<std::uint64_t> causal_sequences;
  const auto reserve_sequence = [&](std::uint64_t sequence) {
    if (sequence == 0U || !causal_sequences.insert(sequence).second) {
      result.structurally_valid = false;
    }
  };
  for (const B1ObservedCurrentGeneration& generation :
       trace.current_generations) {
    reserve_sequence(generation.coordinate.causal_sequence);
  }
  for (const B1ObservedTaskReady& ready : trace.task_readies) {
    if (ready.run_id == 0U) {
      result.structurally_valid = false;
    }
    reserve_sequence(ready.coordinate.causal_sequence);
  }
  for (const B1ObservedTaskTerminal& terminal : trace.task_terminals) {
    if (terminal.run_id == 0U) {
      result.structurally_valid = false;
    }
    reserve_sequence(terminal.coordinate.causal_sequence);
  }
  for (const B1ObservedCancellation& cancellation : trace.cancellations) {
    if (cancellation.run_id == 0U) {
      result.structurally_valid = false;
    }
    reserve_sequence(cancellation.coordinate.causal_sequence);
  }

  std::uint64_t run_id = 0U;
  std::set<std::uint64_t> local_tasks;
  std::uint64_t duplicate_service = 0U;
  for (const B1ObservedServiceStart& start : trace.service_starts) {
    reserve_sequence(start.coordinate.causal_sequence);
    if (start.run_id == 0U || start.service_charge == 0U ||
        !valid_m1_batch_qos(start.qos, source.job.run_cap)) {
      result.structurally_valid = false;
    }
    if (run_id == 0U) {
      run_id = start.run_id;
    } else if (run_id != start.run_id) {
      result.structurally_valid = false;
    }
    if (!checked_accumulate(&result.all_started_service,
                            start.service_charge)) {
      result.structurally_valid = false;
    }
    if (!local_tasks.insert(start.local_task_id).second) {
      ++result.duplicate_service_starts;
      if (!checked_accumulate(&duplicate_service, start.service_charge)) {
        result.structurally_valid = false;
      }
    }
    for (const B1ObservedCancellation& cancellation : trace.cancellations) {
      if (cancellation.run_id == start.run_id &&
          cancellation.coordinate.causal_sequence <
              start.coordinate.causal_sequence &&
          !checked_accumulate(&result.post_cancellation_started_service,
                              start.service_charge)) {
        result.structurally_valid = false;
      }
    }
  }
  if (trace.service_starts.size() != kB1TasksPerJob ||
      local_tasks.size() != kB1TasksPerJob) {
    result.structurally_valid = false;
  } else {
    std::uint64_t expected = 0U;
    for (const std::uint64_t task : local_tasks) {
      if (task != expected++) {
        result.structurally_valid = false;
        break;
      }
    }
  }

  bool lifecycle_valid =
      trace.terminal_kind.has_value() && trace.terminal.has_value() &&
      trace.visible.has_value() && trace.quiescent.has_value() &&
      trace.resource_settled.has_value();
  if (lifecycle_valid) {
    reserve_sequence(trace.terminal->coordinate.causal_sequence);
    reserve_sequence(trace.visible->coordinate.causal_sequence);
    reserve_sequence(trace.quiescent->coordinate.causal_sequence);
    reserve_sequence(trace.resource_settled->coordinate.causal_sequence);
    const std::uint64_t terminal_run = trace.terminal->run_id;
    lifecycle_valid = terminal_run != 0U && terminal_run == run_id &&
                      trace.visible->run_id == terminal_run &&
                      trace.quiescent->run_id == terminal_run &&
                      trace.resource_settled->run_id == terminal_run &&
                      trace.visible->coordinate.causal_sequence <
                          trace.terminal->coordinate.causal_sequence &&
                      trace.terminal->coordinate.causal_sequence <
                          trace.quiescent->coordinate.causal_sequence &&
                      trace.quiescent->coordinate.causal_sequence <
                          trace.resource_settled->coordinate.causal_sequence;
    for (const B1ObservedServiceStart& start : trace.service_starts) {
      lifecycle_valid =
          lifecycle_valid && start.coordinate.causal_sequence <
                                 trace.terminal->coordinate.causal_sequence;
    }
  }
  if (!lifecycle_valid) {
    result.structurally_valid = false;
  }

  bool trace_valid = false;
  try {
    const std::vector<B1SemanticRecord> parsed =
        parse_b1_semantic_trace(source.semantic_trace);
    const std::string observed =
        encode_b1_semantic_trace(make_b1_observed_semantic_records(trace));
    trace_valid =
        parsed.size() == kB1TasksPerJob * 3U &&
        encode_b1_semantic_trace(parsed) == source.semantic_trace &&
        observed == source.semantic_trace &&
        b1_sha256(source.semantic_trace) == source.semantic_trace_digest;
  } catch (const std::exception&) {
    result.structurally_valid = false;
  }

  const B1ComputeIoEvidenceInput io_input{source.job, source.output_status,
                                          source.output_receipt.has_value(),
                                          source.io_observations};
  const B1ComputeIoEvaluation io = evaluate_b1_compute_io_evidence(io_input);
  if (!io.structurally_valid) {
    result.structurally_valid = false;
  }
  result.duplicate_service_starts += io.duplicate_admissions;
  result.retry_service_starts = io.retry_records;

  bool artifact_valid = false;
  bool logical_match = false;
  bool raw_match = false;
  bool manifest_match = false;
  bool visible_digest_match = false;
  const bool receipt_relation =
      (source.output_status == B1OutputCommitStatus::Succeeded) ==
      source.output_receipt.has_value();
  if (!receipt_relation) {
    result.structurally_valid = false;
  }
  if (source.output_receipt.has_value()) {
    const M1BatchReceiptEvidence& receipt = *source.output_receipt;
    try {
      const B1JobGolden frozen = b1_frozen_job_golden(source.job.job_index);
      const std::string commit_id = expected_m1_batch_commit_id(source.job);
      const std::string manifest =
          b1_artifact_manifest(source.job.job_index, receipt.payload_digest);
      artifact_valid =
          receipt.commit_id == commit_id && receipt.job == source.job &&
          receipt.logical_descriptor ==
              "dense-tensor-hwc-fp32-rgba-2048x2048" &&
          receipt.committed_generation == 1U &&
          receipt.payload_name == "output.rgba32le" &&
          receipt.manifest_name == "manifest.txt" &&
          receipt.payload_length == kB1PayloadBytes &&
          receipt.manifest_length == b1_manifest_length(source.job.job_index) &&
          receipt.requested_durability == B1OutputDurability::CrashDurable &&
          receipt.achieved_durability == B1OutputDurability::CrashDurable &&
          !receipt.published_manifest_identity.empty() &&
          valid_m1_batch_rooted_slot(receipt.rooted_slot) &&
          receipt.rooted_slot ==
              std::filesystem::path("occurrence-" + commit_id) &&
          receipt.resolved_root.is_absolute();
      manifest_match = receipt.manifest_length == manifest.size() &&
                       receipt.manifest_digest == b1_sha256(manifest);
      logical_match = source.golden.job_index == source.job.job_index &&
                      source.golden.logical_digest == frozen.logical_digest &&
                      receipt.logical_content_digest == frozen.logical_digest;
      raw_match =
          source.golden.job_index == source.job.job_index &&
          source.golden.raw_payload_digest == frozen.raw_payload_digest &&
          receipt.payload_digest == frozen.raw_payload_digest;
      visible_digest_match =
          trace.visible_content_digest.state == ContentDigestState::Available &&
          trace.visible_content_digest.digest.has_value() &&
          *trace.visible_content_digest.digest ==
              receipt.logical_content_digest;
    } catch (const std::exception&) {
      artifact_valid = false;
    }
  }

  result.verified_endpoint =
      result.structurally_valid && lifecycle_valid && source.run_succeeded &&
      trace.terminal_kind == compute::ComputeRunTerminalKind::Succeeded &&
      trace_valid && artifact_valid && manifest_match && logical_match &&
      raw_match && visible_digest_match && io.fault_free_complete &&
      source.output_status == B1OutputCommitStatus::Succeeded &&
      source.output_receipt.has_value();
  result.discarded_started_service =
      result.verified_endpoint ? duplicate_service : result.all_started_service;
  return result;
}

/**
 * @brief Source replay facts bound to one exact protocol offer.
 * @throws Nothing for value construction and copying.
 */
struct M1ProjectedBatchSource final {
  /** @brief Exact protocol offer matched by list identity/order. */
  const M1BatchOfferEvidence* offer = nullptr;
  /** @brief Independently replayed endpoint and service facts. */
  M1BatchSourceReplay replay;
};

/**
 * @brief Tests whether one Graph has uninterrupted measured demand.
 * @param offers Complete exact-order protocol offer stream.
 * @param parity Zero for Graph A or one for Graph B.
 * @param start Inclusive one-second window start.
 * @param end Exclusive one-second window end.
 * @return True only when contiguous same-Graph offer intervals cover the full
 * window.
 * @throws Nothing.
 * @note Callers validate every offer endpoint before invoking this helper.
 */
bool m1_graph_demand_covers(
    const std::vector<M1BatchOfferEvidence>& offers, std::uint64_t parity,
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point end) noexcept {
  std::chrono::steady_clock::time_point covered = start;
  bool began = false;
  for (const M1BatchOfferEvidence& offer : offers) {
    if (offer.job.phase != B1JobPhase::Measured ||
        (offer.job.job_index & 1U) != parity || !offer.endpoint.has_value()) {
      continue;
    }
    const auto offered = offer.offered.timestamp;
    const auto endpoint = offer.endpoint->timestamp;
    if (!began) {
      if (offered <= start && start < endpoint) {
        began = true;
        covered = endpoint;
      }
      continue;
    }
    if (offered <= covered && covered < endpoint) {
      covered = endpoint;
    }
    if (end <= covered) {
      return true;
    }
  }
  return began && end <= covered;
}

/**
 * @brief Tests exact equality of two Host status values.
 * @param lhs First status.
 * @param rhs Second status.
 * @return True only when all five status fields match.
 * @throws Nothing.
 */
bool same_m1_operation_status(const OperationStatus& lhs,
                              const OperationStatus& rhs) noexcept {
  return lhs.ok == rhs.ok && lhs.domain == rhs.domain && lhs.code == rhs.code &&
         lhs.name == rhs.name && lhs.message == rhs.message;
}

/**
 * @brief Tests exact equality of two optional Host status values.
 * @param lhs First optional status.
 * @param rhs Second optional status.
 * @return True only when presence and every present field match.
 * @throws Nothing.
 */
bool same_m1_optional_status(
    const std::optional<OperationStatus>& lhs,
    const std::optional<OperationStatus>& rhs) noexcept {
  return lhs.has_value() == rhs.has_value() &&
         (!lhs.has_value() || same_m1_operation_status(*lhs, *rhs));
}

/**
 * @brief Tests exact equality of two first-admission/current-hold records.
 * @param lhs First admission projection.
 * @param rhs Second admission projection.
 * @return True only when all twelve retained components match.
 * @throws Nothing.
 */
bool same_m1_first_measured_admission(
    const M1FirstMeasuredAdmissionEvidence& lhs,
    const M1FirstMeasuredAdmissionEvidence& rhs) noexcept {
  return lhs.edit_index == rhs.edit_index &&
         lhs.nominal_time == rhs.nominal_time &&
         lhs.attempted == rhs.attempted &&
         lhs.admission_sample == rhs.admission_sample &&
         lhs.reserved_event_sequence == rhs.reserved_event_sequence &&
         lhs.host_succeeded == rhs.host_succeeded &&
         same_m1_accepted_coordinate(lhs.accepted_coordinate,
                                     rhs.accepted_coordinate) &&
         lhs.warmup_publication_current_before_acceptance ==
             rhs.warmup_publication_current_before_acceptance &&
         lhs.superseded_exactly_at_acceptance ==
             rhs.superseded_exactly_at_acceptance &&
         lhs.boundary_only_cancellation == rhs.boundary_only_cancellation &&
         lhs.old_generation_settlement_endpoint ==
             rhs.old_generation_settlement_endpoint;
}

/**
 * @brief Tests exact equality of two progress-window vectors.
 * @param lhs First ordered projection.
 * @param rhs Second ordered projection.
 * @return True only when cardinality, identity, numerator, and duration match.
 * @throws Nothing.
 */
bool same_m1_progress_windows(
    const std::vector<M1ThroughputProgressSample>& lhs,
    const std::vector<M1ThroughputProgressSample>& rhs) noexcept {
  return lhs.size() == rhs.size() &&
         std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                    [](const M1ThroughputProgressSample& left,
                       const M1ThroughputProgressSample& right) {
                      return left.window_ordinal == right.window_ordinal &&
                             left.successful_site_operations ==
                                 right.successful_site_operations &&
                             left.duration == right.duration;
                    });
}

/**
 * @brief Tests exact equality of two Graph-window vectors.
 * @param lhs First ordered projection.
 * @param rhs Second ordered projection.
 * @return True only when cardinality, identity, demand, and both services
 * match.
 * @throws Nothing.
 */
bool same_m1_graph_windows(
    const std::vector<M1GraphServiceWindow>& lhs,
    const std::vector<M1GraphServiceWindow>& rhs) noexcept {
  return lhs.size() == rhs.size() &&
         std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                    [](const M1GraphServiceWindow& left,
                       const M1GraphServiceWindow& right) {
                      return left.window_ordinal == right.window_ordinal &&
                             left.both_graphs_continuously_demanding ==
                                 right.both_graphs_continuously_demanding &&
                             left.graph_a_completed_service ==
                                 right.graph_a_completed_service &&
                             left.graph_b_completed_service ==
                                 right.graph_b_completed_service;
                    });
}

/**
 * @brief Tests exact equality of two headroom aggregate values.
 * @param lhs First aggregate.
 * @param rhs Second aggregate.
 * @return True only when attempted, classified, and failure counts match.
 * @throws Nothing.
 */
bool same_m1_headroom_admissions(
    const M1HeadroomAdmissionEvidence& lhs,
    const M1HeadroomAdmissionEvidence& rhs) noexcept {
  return lhs.attempted_edits == rhs.attempted_edits &&
         lhs.classified_outcomes == rhs.classified_outcomes &&
         lhs.throughput_headroom_failures == rhs.throughput_headroom_failures;
}

/**
 * @brief Tests exact equality of two ordered headroom-outcome vectors.
 * @param lhs First retained projection.
 * @param rhs Second retained projection.
 * @return True only when cardinality, identities, status fields, and flags
 * match.
 * @throws Nothing.
 */
bool same_m1_headroom_outcomes(
    const std::vector<M1HeadroomAdmissionOutcome>& lhs,
    const std::vector<M1HeadroomAdmissionOutcome>& rhs) noexcept {
  return lhs.size() == rhs.size() &&
         std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                    [](const M1HeadroomAdmissionOutcome& left,
                       const M1HeadroomAdmissionOutcome& right) {
                      return left.origin_ordinal == right.origin_ordinal &&
                             left.edit_index == right.edit_index &&
                             left.admission_attempted ==
                                 right.admission_attempted &&
                             same_m1_optional_status(left.host_status,
                                                     right.host_status) &&
                             left.throughput_headroom_failure ==
                                 right.throughput_headroom_failure;
                    });
}

/**
 * @brief Tests exact equality of two projected B1 waste records.
 * @param lhs First aggregate.
 * @param rhs Second aggregate.
 * @return True only when all five dimensions match.
 * @throws Nothing.
 */
bool same_m1_batch_waste(const M1BatchWasteEvidence& lhs,
                         const M1BatchWasteEvidence& rhs) noexcept {
  return lhs.all_started_service == rhs.all_started_service &&
         lhs.discarded_started_service == rhs.discarded_started_service &&
         lhs.post_cancellation_started_service ==
             rhs.post_cancellation_started_service &&
         lhs.duplicate_service_starts == rhs.duplicate_service_starts &&
         lhs.retry_service_starts == rhs.retry_service_starts;
}

/**
 * @brief Tests whether a Host resource vector is exactly zero.
 * @param value Complete resource vector.
 * @return True only when every dimension is zero.
 * @throws Nothing.
 */
bool zero_resources(const ResourceVector& value) noexcept {
  return value == ResourceVector{};
}

/**
 * @brief Tests component-wise high-water monotonicity.
 * @param prior Earlier lifetime high-water.
 * @param current Later lifetime high-water.
 * @return True when no dimension decreased.
 * @throws Nothing.
 */
bool resource_high_water_nondecreasing(const ResourceVector& prior,
                                       const ResourceVector& current) noexcept {
  return prior.cpu_slots <= current.cpu_slots &&
         prior.retained_memory_bytes <= current.retained_memory_bytes &&
         prior.scratch_bytes <= current.scratch_bytes &&
         prior.ready_entries <= current.ready_entries &&
         prior.ready_bytes <= current.ready_bytes;
}

/**
 * @brief Tests whether all lifecycle ownership counters are zero.
 * @param counters Complete final counter set.
 * @return True only for exact process settlement.
 * @throws Nothing.
 */
bool lifecycle_settled(
    const compute::ExecutionLifecycleCounters& counters) noexcept {
  return counters.registered_graph_count == 0U &&
         counters.open_graph_count == 0U &&
         counters.closing_graph_count == 0U &&
         counters.pending_candidate_count == 0U &&
         counters.admitted_standalone_run_count == 0U &&
         counters.admitted_run_group_count == 0U &&
         counters.admitted_child_run_count == 0U &&
         counters.terminal_not_quiescent_run_count == 0U &&
         counters.finalizing_run_count == 0U &&
         counters.ready_entry_count == 0U &&
         counters.entered_callback_count == 0U &&
         counters.live_root_reservation_count == 0U &&
         counters.live_child_grant_count == 0U &&
         counters.live_policy_invocation_count == 0U &&
         counters.live_policy_binding_count == 0U;
}

/**
 * @brief Tests one closed lifecycle service-state representation.
 * @param state Candidate enum.
 * @return True for Accepting, Stopping, or Stopped.
 * @throws Nothing.
 */
bool known_service_state(
    compute::ExecutionLifecycleServiceState state) noexcept {
  switch (state) {
    case compute::ExecutionLifecycleServiceState::Accepting:
    case compute::ExecutionLifecycleServiceState::Stopping:
    case compute::ExecutionLifecycleServiceState::Stopped:
      return true;
  }
  return false;
}

/**
 * @brief Tests one closed lifecycle event-kind representation.
 * @param kind Candidate enum.
 * @return True only for one declared version-1 event kind.
 * @throws Nothing.
 */
bool known_lifecycle_event_kind(
    compute::ExecutionLifecycleEventKind kind) noexcept {
  switch (kind) {
    case compute::ExecutionLifecycleEventKind::ServiceStarted:
    case compute::ExecutionLifecycleEventKind::GraphRegistered:
    case compute::ExecutionLifecycleEventKind::GraphClosing:
    case compute::ExecutionLifecycleEventKind::CandidateBegan:
    case compute::ExecutionLifecycleEventKind::CandidateRolledBack:
    case compute::ExecutionLifecycleEventKind::BundleAdmitted:
    case compute::ExecutionLifecycleEventKind::CancellationRequested:
    case compute::ExecutionLifecycleEventKind::RunTerminal:
    case compute::ExecutionLifecycleEventKind::RunQuiescent:
    case compute::ExecutionLifecycleEventKind::ResourceSettled:
    case compute::ExecutionLifecycleEventKind::RunUnregistered:
    case compute::ExecutionLifecycleEventKind::GraphRowRemoved:
    case compute::ExecutionLifecycleEventKind::WorkerJoined:
    case compute::ExecutionLifecycleEventKind::BindingRetired:
    case compute::ExecutionLifecycleEventKind::ServiceStopped:
      return true;
  }
  return false;
}

/**
 * @brief Tests one closed lifecycle event-category representation.
 * @param category Candidate enum.
 * @return True only for one declared version-1 category.
 * @throws Nothing.
 */
bool known_lifecycle_category(
    compute::ExecutionLifecycleCategory category) noexcept {
  switch (category) {
    case compute::ExecutionLifecycleCategory::None:
    case compute::ExecutionLifecycleCategory::ExplicitRequest:
    case compute::ExecutionLifecycleCategory::Deadline:
    case compute::ExecutionLifecycleCategory::Superseded:
    case compute::ExecutionLifecycleCategory::GraphClose:
    case compute::ExecutionLifecycleCategory::ProcessShutdown:
    case compute::ExecutionLifecycleCategory::Succeeded:
    case compute::ExecutionLifecycleCategory::Cancelled:
    case compute::ExecutionLifecycleCategory::FailureResourceExhausted:
    case compute::ExecutionLifecycleCategory::FailureOther:
      return true;
  }
  return false;
}

/**
 * @brief Validates the identity/category shape authored for one event kind.
 * @param event Candidate closed lifecycle record.
 * @return True only when required and absent identities match the producer.
 * @throws Nothing.
 * @note This validates scalar shape, not cross-record lifecycle order; sequence
 * and cursor replay provide that independent boundary.
 */
bool valid_lifecycle_event_identity(
    const compute::ExecutionLifecycleEvent& event) noexcept {
  using Category = compute::ExecutionLifecycleCategory;
  using Kind = compute::ExecutionLifecycleEventKind;
  const bool graph = event.graph_instance_id != 0U;
  const bool run = event.run_id != 0U;
  const bool group = event.run_group_id != 0U;
  const bool generation = event.generation != 0U;
  const bool none = event.category == Category::None;
  const bool terminal = event.category == Category::Succeeded ||
                        event.category == Category::Cancelled ||
                        event.category == Category::FailureResourceExhausted ||
                        event.category == Category::FailureOther;
  switch (event.kind) {
    case Kind::ServiceStarted:
      return !graph && !run && !group && !generation && none;
    case Kind::GraphRegistered:
      return graph && !run && !group && !generation && none;
    case Kind::GraphClosing:
      return graph && !run && !group && generation &&
             (event.category == Category::GraphClose ||
              event.category == Category::ProcessShutdown);
    case Kind::CandidateBegan:
      return graph && !run && !group && generation && none;
    case Kind::CandidateRolledBack:
      return graph && !run && !group && generation &&
             (none || event.category == Category::GraphClose ||
              event.category == Category::ProcessShutdown);
    case Kind::BundleAdmitted:
      return graph && run && generation && none;
    case Kind::CancellationRequested:
      return graph && !run && !group && generation &&
             (event.category == Category::GraphClose ||
              event.category == Category::ProcessShutdown);
    case Kind::RunTerminal:
      return graph && run && generation && terminal;
    case Kind::RunQuiescent:
    case Kind::ResourceSettled:
    case Kind::RunUnregistered:
      return graph && run && generation && none;
    case Kind::GraphRowRemoved:
      return graph && !run && !group && none;
    case Kind::WorkerJoined:
      return !graph && !run && !group && generation && none;
    case Kind::BindingRetired:
      return !graph && !run && !group && generation &&
             (none || event.category == Category::FailureOther);
    case Kind::ServiceStopped:
      return !graph && !run && !group && generation && none;
  }
  return false;
}

/**
 * @brief Invalid-priority conjunction of independent SLO verdicts.
 * @param verdicts Complete axis verdicts.
 * @param reasons Diagnostics receiving unknown enum evidence.
 * @return Invalid, Fail, or Pass without substitution.
 * @throws std::bad_alloc when an unknown-enum reason allocates.
 */
I1Verdict compose_m1_row(const std::vector<I1Verdict>& verdicts,
                         std::vector<std::string>* reasons) {
  bool failed = false;
  for (const I1Verdict verdict : verdicts) {
    switch (verdict) {
      case I1Verdict::Pass:
        break;
      case I1Verdict::Fail:
        failed = true;
        break;
      case I1Verdict::Invalid:
        return I1Verdict::Invalid;
      default:
        invalidate_m1(reasons, "M1 inner row contains an unknown verdict");
        return I1Verdict::Invalid;
    }
  }
  return failed ? I1Verdict::Fail : I1Verdict::Pass;
}

/**
 * @brief Memory validation classification separating shape from SLO failures.
 * @throws Nothing for value construction.
 */
struct M1MemoryValidation final {
  /** @brief False for missing, malformed, lossy, or contradictory evidence. */
  bool valid = true;
  /** @brief False when an authoritative high-water exceeds its fixed limit. */
  bool within_limits = true;
  /** @brief False when final ownership or Compute I/O remains nonzero. */
  bool settled = true;
};

/** @brief Exact number of policy bindings published before ServiceStarted. */
constexpr std::uint64_t kM1InitialPolicyBindingCount = 2U;

/**
 * @brief Monotonic state of one replayed Graph registry row.
 * @throws Nothing for value construction and comparison.
 */
enum class M1ReplayGraphState : std::uint8_t {
  /** @brief Candidate and bundle admission remains legal. */
  Open,
  /** @brief Admission is closed while candidates and bundles settle. */
  Closing,
};

/**
 * @brief Monotonic state of one replayed child Run.
 * @throws Nothing for value construction and comparison.
 */
enum class M1ReplayRunState : std::uint8_t {
  /** @brief The child is installed and has not published terminal state. */
  Admitted,
  /** @brief Terminal state is published but non-registry leases may remain. */
  Terminal,
  /** @brief Only the registry lease remains. */
  Quiescent,
  /** @brief Root reservation and every child grant have returned. */
  ResourceSettled,
  /** @brief The child has left every registry index. */
  Unregistered,
};

/**
 * @brief Replay state for one live Graph row and its anonymous candidates.
 *
 * BundleAdmitted does not carry the consumed candidate id. The replay
 * therefore proves the only producer-observable relation: every explicit
 * rollback names a unique begun candidate and the number of commits plus
 * rollbacks never exceeds the number begun on this Graph.
 *
 * @throws std::bad_alloc when candidate identities are retained.
 */
struct M1ReplayGraph final {
  /** @brief Current monotonic row state. */
  M1ReplayGraphState state = M1ReplayGraphState::Open;
  /** @brief Nonzero close generation after Open-to-Closing. */
  std::uint64_t close_generation = 0U;
  /** @brief Cancellation category currently visible to pending candidates. */
  compute::ExecutionLifecycleCategory candidate_cancellation =
      compute::ExecutionLifecycleCategory::None;
  /** @brief Every unique candidate identity begun on this row. */
  std::set<std::uint64_t> candidate_ids;
  /** @brief Candidate identities that explicitly rolled back. */
  std::set<std::uint64_t> rolled_back_candidate_ids;
  /** @brief Number of anonymous candidates consumed by BundleAdmitted. */
  std::uint64_t committed_candidate_count = 0U;
};

/**
 * @brief Replay state for one child Run identity.
 * @throws Nothing for value construction and movement.
 */
struct M1ReplayRun final {
  /** @brief Exact globally non-reused child identity. */
  std::uint64_t run_id = 0U;
  /** @brief Current monotonic settlement phase. */
  M1ReplayRunState state = M1ReplayRunState::Admitted;
};

/**
 * @brief Replay state for one standalone or two-child realtime bundle.
 * @throws std::bad_alloc when the second realtime child identity is learned.
 */
struct M1ReplayBundle final {
  /** @brief Exact registry-private bundle identity. */
  std::uint64_t bundle_id = 0U;
  /** @brief Exact owning Graph identity. */
  std::uint64_t graph_instance_id = 0U;
  /** @brief Zero for standalone or the exact realtime group identity. */
  std::uint64_t run_group_id = 0U;
  /** @brief One for standalone or two for realtime. */
  std::size_t expected_run_count = 0U;
  /** @brief Children in producer order; realtime child two is learned later. */
  std::vector<M1ReplayRun> runs;
  /** @brief True after the admission was erased before unregister events. */
  bool detached = false;
  /** @brief True after close/shutdown captured its cancellation record. */
  bool cancellation_captured = false;
};

/**
 * @brief One pending second child event emitted under the registry fence.
 * @throws Nothing for value construction.
 */
struct M1ReplayGroupStep final {
  /** @brief Exact bundle whose second child must be emitted next. */
  std::uint64_t bundle_id = 0U;
  /** @brief Exact child transition kind that must complete the pair. */
  compute::ExecutionLifecycleEventKind kind =
      compute::ExecutionLifecycleEventKind::RunTerminal;
};

/** @brief Comparable key for one captured cancellation publication count. */
using M1CancelKey = std::tuple<std::uint64_t, std::uint16_t, std::uint64_t>;

/**
 * @brief Tests exact equality of the nine registry-derived counter fields.
 * @param observed Event/page counter view supplied by telemetry.
 * @param expected Independently replayed registry counter view.
 * @return True only when every registry-derived field is identical.
 * @throws Nothing.
 */
bool equal_registry_counters(
    const compute::ExecutionLifecycleCounters& observed,
    const compute::ExecutionLifecycleCounters& expected) noexcept {
  return observed.registered_graph_count == expected.registered_graph_count &&
         observed.open_graph_count == expected.open_graph_count &&
         observed.closing_graph_count == expected.closing_graph_count &&
         observed.pending_candidate_count == expected.pending_candidate_count &&
         observed.admitted_standalone_run_count ==
             expected.admitted_standalone_run_count &&
         observed.admitted_run_group_count ==
             expected.admitted_run_group_count &&
         observed.admitted_child_run_count ==
             expected.admitted_child_run_count &&
         observed.terminal_not_quiescent_run_count ==
             expected.terminal_not_quiescent_run_count &&
         observed.finalizing_run_count == expected.finalizing_run_count;
}

/**
 * @brief Replays one complete lossless lifecycle stream as producer state.
 *
 * The replay owns no product authority. It reconstructs Graph rows,
 * anonymous candidate consumption, standalone/group admission, ordered child
 * settlement, cancellation fan-out, Graph removal, and the no-event service
 * Stopping transition. Every event and page counter view is checked against
 * the resulting registry state. Physical counters remain independently
 * sampled facts and are checked only for producer-guaranteed capacity,
 * ownership reachability, origin, and final-zero constraints.
 *
 * @throws std::bad_alloc when replay maps, sets, vectors, or diagnostics grow.
 */
class M1LifecycleReplay final {
 public:
  /**
   * @brief Binds stable invalidation output for the complete replay.
   * @param reasons Mutable row diagnostics that outlive this replay.
   * @throws Nothing.
   */
  explicit M1LifecycleReplay(std::vector<std::string>* reasons) noexcept
      : reasons_(reasons) {}

  /**
   * @brief Reports whether the no-event service Stopping transition occurred.
   * @return True after begin_shutdown() accepts one generation.
   * @throws Nothing.
   */
  bool shutdown_started() const noexcept { return shutdown_started_; }

  /**
   * @brief Applies producer-atomic Accepting-to-Stopping state.
   *
   * Every live row becomes Closing before the first ProcessShutdown
   * GraphClosing event, candidate cancellation changes to ProcessShutdown,
   * and every not-yet-captured admission cancellation record is captured.
   *
   * @param generation Exact nonzero page shutdown generation.
   * @return True when the transition is new or idempotently identical.
   * @throws std::bad_alloc when shutdown enumeration/cancellation state grows.
   */
  bool begin_shutdown(std::uint64_t generation) {
    if (generation == 0U || service_stopped_) {
      return reject("shutdown transition has an invalid generation or state");
    }
    if (shutdown_started_) {
      if (shutdown_generation_ != generation) {
        return reject("shutdown generation changed during replay");
      }
      return true;
    }
    shutdown_started_ = true;
    shutdown_generation_ = generation;
    for (const std::uint64_t graph_id : graph_registration_order_) {
      const auto found = graphs_.find(graph_id);
      if (found == graphs_.end()) {
        continue;
      }
      M1ReplayGraph& graph = found->second;
      if (graph.state == M1ReplayGraphState::Open) {
        graph.state = M1ReplayGraphState::Closing;
        graph.close_generation = 1U;
      }
      graph.candidate_cancellation =
          compute::ExecutionLifecycleCategory::ProcessShutdown;
      shutdown_graph_closing_order_.push_back(graph_id);
      capture_cancellations(
          graph_id, compute::ExecutionLifecycleCategory::ProcessShutdown,
          generation);
    }
    return true;
  }

  /**
   * @brief Applies and validates one exact lifecycle event.
   * @param event Next globally sequenced producer record.
   * @return True when its transition and complete counter view are valid.
   * @throws std::bad_alloc when state or diagnostics grow.
   */
  bool apply(const compute::ExecutionLifecycleEvent& event) {
    if (service_stopped_) {
      return reject("ordinary event appears after ServiceStopped");
    }
    if (!expected_atomic_event(event)) {
      return false;
    }

    bool transition_valid = true;
    using Kind = compute::ExecutionLifecycleEventKind;
    switch (event.kind) {
      case Kind::ServiceStarted:
        if (service_started_ || event.sequence != 1U || !graphs_.empty() ||
            !bundles_.empty()) {
          transition_valid = reject(
              "ServiceStarted is duplicated or has prior producer state");
        } else {
          service_started_ = true;
        }
        break;
      case Kind::GraphRegistered:
        transition_valid = register_graph(event);
        break;
      case Kind::GraphClosing:
        transition_valid = close_graph(event);
        break;
      case Kind::CandidateBegan:
        transition_valid = begin_candidate(event);
        break;
      case Kind::CandidateRolledBack:
        transition_valid = rollback_candidate(event);
        break;
      case Kind::BundleAdmitted:
        transition_valid = admit_bundle(event);
        break;
      case Kind::CancellationRequested:
        transition_valid = consume_cancellation(event);
        break;
      case Kind::RunTerminal:
      case Kind::RunQuiescent:
      case Kind::ResourceSettled:
      case Kind::RunUnregistered:
        transition_valid = advance_run(event);
        break;
      case Kind::GraphRowRemoved:
        transition_valid = remove_graph(event);
        break;
      case Kind::WorkerJoined:
        if (!shutdown_started_ || event.generation != shutdown_generation_) {
          transition_valid =
              reject("WorkerJoined is outside its shutdown generation");
        }
        break;
      case Kind::BindingRetired:
        break;
      case Kind::ServiceStopped:
        transition_valid = stop_service(event);
        break;
    }

    const bool counters_valid = validate_counter_view(
        event.counters, event.kind == Kind::ServiceStarted,
        event.kind == Kind::ServiceStopped, "event");
    return transition_valid && counters_valid;
  }

  /**
   * @brief Validates the complete counter view at one atomic page cut.
   * @param page Exact copied telemetry page after all returned records.
   * @return True when registry and physical counter constraints hold.
   * @throws std::bad_alloc when diagnostics grow.
   */
  bool validate_page(const compute::ExecutionLifecyclePage& page) {
    bool valid = validate_counter_view(
        page.counters, false,
        page.service_state == compute::ExecutionLifecycleServiceState::Stopped,
        "page");
    if (shutdown_started_ && page.shutdown_generation != shutdown_generation_) {
      valid = reject("page shutdown generation differs from replay") && valid;
    }
    if (!shutdown_started_ && page.shutdown_generation != 0U) {
      valid = reject("Accepting replay has a shutdown generation") && valid;
    }
    if (page.service_state ==
            compute::ExecutionLifecycleServiceState::Accepting &&
        shutdown_started_) {
      valid = reject("page service state moved behind replay") && valid;
    }
    if (page.service_state ==
            compute::ExecutionLifecycleServiceState::Stopping &&
        (!shutdown_started_ || service_stopped_)) {
      valid =
          reject("Stopping page contradicts replayed service state") && valid;
    }
    if (page.service_state ==
            compute::ExecutionLifecycleServiceState::Stopped &&
        !service_stopped_) {
      valid = reject("Stopped page lacks replayed ServiceStopped") && valid;
    }
    return valid;
  }

  /**
   * @brief Verifies no producer-atomic transition or owned row remains open.
   * @return True only for a complete M1 final capture.
   * @throws std::bad_alloc when diagnostics grow.
   */
  bool complete() {
    bool valid = true;
    if (!service_started_) {
      valid = reject("history lacks the unique ServiceStarted origin") && valid;
    }
    if (!shutdown_started_ || !service_stopped_) {
      valid =
          reject("history lacks the final ServiceStopped settlement") && valid;
    }
    if (pending_group_step_.has_value() ||
        shutdown_graph_closing_index_ != shutdown_graph_closing_order_.size()) {
      valid = reject("history ends inside one producer-atomic event batch") &&
              valid;
    }
    if (!graphs_.empty() || !bundles_.empty() ||
        !pending_cancellations_.empty()) {
      valid = reject("history ends with live Graph, bundle, or cancellation") &&
              valid;
    }
    return valid;
  }

 private:
  /**
   * @brief Appends one stable lifecycle replay invalidation.
   * @param reason Detail without the common M1 prefix.
   * @return Always false for direct guard composition.
   * @throws std::bad_alloc when diagnostics allocate.
   */
  bool reject(std::string reason) {
    invalidate_m1(reasons_, "M1 memory evidence: lifecycle " + reason);
    return false;
  }

  /**
   * @brief Returns one Graph's current producer-visible candidate count.
   * @param graph Exact retained Graph state.
   * @return Begun minus rolled-back minus anonymously committed candidates.
   * @throws Nothing; impossible arithmetic terminates replay through caller
   * validation before this helper is used.
   */
  static std::uint64_t pending_candidates(const M1ReplayGraph& graph) noexcept {
    const std::uint64_t begun =
        static_cast<std::uint64_t>(graph.candidate_ids.size());
    const std::uint64_t rolled_back =
        static_cast<std::uint64_t>(graph.rolled_back_candidate_ids.size());
    return begun - rolled_back - graph.committed_candidate_count;
  }

  /**
   * @brief Recomputes the exact nine registry-derived counters.
   * @return Fresh authority-free replay view with physical fields left zero.
   * @throws Nothing.
   */
  compute::ExecutionLifecycleCounters counters() const noexcept {
    compute::ExecutionLifecycleCounters result;
    result.registered_graph_count = static_cast<std::uint64_t>(graphs_.size());
    for (const auto& entry : graphs_) {
      const M1ReplayGraph& graph = entry.second;
      if (graph.state == M1ReplayGraphState::Open) {
        ++result.open_graph_count;
      } else {
        ++result.closing_graph_count;
      }
      result.pending_candidate_count += pending_candidates(graph);
    }
    for (const auto& entry : bundles_) {
      const M1ReplayBundle& bundle = entry.second;
      if (bundle.detached) {
        continue;
      }
      if (bundle.run_group_id == 0U) {
        ++result.admitted_standalone_run_count;
      } else {
        ++result.admitted_run_group_count;
        result.admitted_child_run_count +=
            static_cast<std::uint64_t>(bundle.expected_run_count);
      }
      for (const M1ReplayRun& run : bundle.runs) {
        if (run.state == M1ReplayRunState::Terminal) {
          ++result.terminal_not_quiescent_run_count;
        }
        if (run.state == M1ReplayRunState::Terminal ||
            run.state == M1ReplayRunState::Quiescent ||
            run.state == M1ReplayRunState::ResourceSettled) {
          ++result.finalizing_run_count;
        }
      }
    }
    return result;
  }

  /**
   * @brief Validates exact registry counters and conservative physical facts.
   * @param observed Complete event/page counter view.
   * @param service_origin Whether this is the unique ServiceStarted record.
   * @param final_stop Whether this cut is the final ServiceStopped state.
   * @param context Stable `event` or `page` diagnostic label.
   * @return True only when every producer-guaranteed relation holds.
   * @throws std::bad_alloc when diagnostics allocate.
   * @note Physical current counters may rise or fall between lifecycle events;
   * this method deliberately derives no event-kind delta from them.
   */
  bool validate_counter_view(
      const compute::ExecutionLifecycleCounters& observed, bool service_origin,
      bool final_stop, const char* context) {
    bool valid = true;
    const compute::ExecutionLifecycleCounters expected = counters();
    if (!equal_registry_counters(observed, expected)) {
      valid = reject(std::string(context) +
                     " registry-derived counters differ from replay") &&
              valid;
    }

    if (observed.ready_entry_count > kM1HostLimits.ready_entries ||
        observed.ready_entry_count > std::numeric_limits<std::uint64_t>::max() -
                                         observed.entered_callback_count ||
        observed.ready_entry_count + observed.entered_callback_count >
            observed.live_child_grant_count) {
      valid = reject(std::string(context) +
                     " physical ready/callback/grant bounds are impossible") &&
              valid;
    }
    if (observed.live_child_grant_count != 0U &&
        observed.live_root_reservation_count == 0U) {
      valid = reject(std::string(context) +
                     " child grant has no reachable root reservation") &&
              valid;
    }
    if (observed.live_policy_invocation_count != 0U &&
        observed.live_policy_binding_count == 0U) {
      valid = reject(std::string(context) +
                     " policy invocation has no live binding") &&
              valid;
    }

    const std::uint64_t admitted_run_count =
        expected.admitted_standalone_run_count +
        expected.admitted_child_run_count;
    const bool execution_owner = observed.ready_entry_count != 0U ||
                                 observed.entered_callback_count != 0U ||
                                 observed.live_policy_invocation_count != 0U;
    if (execution_owner && admitted_run_count == 0U) {
      valid = reject(std::string(context) +
                     " executing Run owner has no admitted child") &&
              valid;
    }
    const bool resource_owner = observed.live_root_reservation_count != 0U ||
                                observed.live_child_grant_count != 0U;
    if (resource_owner && admitted_run_count == 0U &&
        expected.pending_candidate_count == 0U) {
      valid = reject(std::string(context) +
                     " resource owner has no admitted child or staged "
                     "candidate") &&
              valid;
    }

    if (service_origin &&
        (execution_owner || resource_owner ||
         observed.live_policy_binding_count != kM1InitialPolicyBindingCount)) {
      valid =
          reject("ServiceStarted physical origin is not producer-reachable") &&
          valid;
    }
    if (final_stop && !lifecycle_settled(observed)) {
      valid = reject("ServiceStopped counters are not exactly zero") && valid;
    }
    return valid;
  }

  /**
   * @brief Enforces multi-record batches published under one registry fence.
   * @param event Next event candidate.
   * @return True when no batch is open or this is its required next record.
   * @throws std::bad_alloc when diagnostics allocate.
   */
  bool expected_atomic_event(const compute::ExecutionLifecycleEvent& event) {
    if (pending_group_step_.has_value() &&
        (event.generation != pending_group_step_->bundle_id ||
         event.kind != pending_group_step_->kind)) {
      return reject("another event split one realtime child transition pair");
    }
    if (shutdown_graph_closing_index_ < shutdown_graph_closing_order_.size()) {
      const std::uint64_t expected_graph =
          shutdown_graph_closing_order_[shutdown_graph_closing_index_];
      if (event.kind != compute::ExecutionLifecycleEventKind::GraphClosing ||
          event.category !=
              compute::ExecutionLifecycleCategory::ProcessShutdown ||
          event.graph_instance_id != expected_graph) {
        return reject(
            "another event split the process-shutdown GraphClosing batch");
      }
    }
    return true;
  }

  /**
   * @brief Applies one GraphRegistered event.
   * @param event Exact candidate event.
   * @return True only for a fresh Graph during Accepting.
   * @throws std::bad_alloc when Graph/order storage grows.
   */
  bool register_graph(const compute::ExecutionLifecycleEvent& event) {
    if (!service_started_ || shutdown_started_ ||
        all_graph_ids_.count(event.graph_instance_id) != 0U) {
      return reject("GraphRegistered is not a fresh Accepting row");
    }
    graphs_.emplace(event.graph_instance_id, M1ReplayGraph{});
    all_graph_ids_.insert(event.graph_instance_id);
    graph_registration_order_.push_back(event.graph_instance_id);
    return true;
  }

  /**
   * @brief Captures every still-indexed cancellation record for one Graph.
   * @param graph_id Exact Graph identity.
   * @param category GraphClose or ProcessShutdown publication category.
   * @param generation Close or shutdown generation carried by cancellation.
   * @return Nothing.
   * @throws std::bad_alloc when a new expectation key is inserted.
   */
  void capture_cancellations(std::uint64_t graph_id,
                             compute::ExecutionLifecycleCategory category,
                             std::uint64_t generation) {
    const M1CancelKey key{graph_id, static_cast<std::uint16_t>(category),
                          generation};
    for (auto& entry : bundles_) {
      M1ReplayBundle& bundle = entry.second;
      if (bundle.graph_instance_id != graph_id || bundle.detached ||
          bundle.cancellation_captured) {
        continue;
      }
      bundle.cancellation_captured = true;
      ++pending_cancellations_[key];
    }
  }

  /**
   * @brief Applies explicit or process-shutdown GraphClosing publication.
   * @param event Exact next GraphClosing event.
   * @return True only when row state, generation, and shutdown order match.
   * @throws std::bad_alloc when cancellation expectations grow.
   */
  bool close_graph(const compute::ExecutionLifecycleEvent& event) {
    const auto found = graphs_.find(event.graph_instance_id);
    if (found == graphs_.end()) {
      return reject("GraphClosing names no live row");
    }
    M1ReplayGraph& graph = found->second;
    if (shutdown_started_) {
      if (event.category !=
              compute::ExecutionLifecycleCategory::ProcessShutdown ||
          shutdown_graph_closing_index_ >=
              shutdown_graph_closing_order_.size() ||
          shutdown_graph_closing_order_[shutdown_graph_closing_index_] !=
              event.graph_instance_id ||
          event.generation != graph.close_generation) {
        return reject("process-shutdown GraphClosing identity drifted");
      }
      ++shutdown_graph_closing_index_;
      return true;
    }
    if (graph.state != M1ReplayGraphState::Open || event.generation != 1U) {
      return reject("explicit GraphClosing did not perform Open-to-Closing");
    }
    graph.state = M1ReplayGraphState::Closing;
    graph.close_generation = event.generation;
    graph.candidate_cancellation = event.category;
    capture_cancellations(event.graph_instance_id, event.category,
                          event.generation);
    return true;
  }

  /**
   * @brief Applies one CandidateBegan event.
   * @param event Exact candidate identity publication.
   * @return True only for a fresh candidate on an Open Accepting row.
   * @throws std::bad_alloc when candidate identity sets grow.
   */
  bool begin_candidate(const compute::ExecutionLifecycleEvent& event) {
    const auto found = graphs_.find(event.graph_instance_id);
    if (shutdown_started_ || found == graphs_.end() ||
        found->second.state != M1ReplayGraphState::Open ||
        !all_candidate_ids_.insert(event.generation).second) {
      return reject("CandidateBegan is stale, reused, or not admissible");
    }
    found->second.candidate_ids.insert(event.generation);
    return true;
  }

  /**
   * @brief Applies one identity-bearing candidate rollback.
   * @param event Exact rollback publication.
   * @return True only for one unresolved candidate with the current reason.
   * @throws std::bad_alloc when rollback identity storage grows.
   */
  bool rollback_candidate(const compute::ExecutionLifecycleEvent& event) {
    const auto found = graphs_.find(event.graph_instance_id);
    if (found == graphs_.end()) {
      return reject("CandidateRolledBack names no live Graph");
    }
    M1ReplayGraph& graph = found->second;
    if (graph.candidate_ids.count(event.generation) == 0U ||
        graph.rolled_back_candidate_ids.count(event.generation) != 0U ||
        event.category != graph.candidate_cancellation) {
      return reject("CandidateRolledBack identity or cancellation drifted");
    }
    const std::uint64_t begun =
        static_cast<std::uint64_t>(graph.candidate_ids.size());
    const std::uint64_t next_rolled_back =
        static_cast<std::uint64_t>(graph.rolled_back_candidate_ids.size() + 1U);
    if (graph.committed_candidate_count > begun - next_rolled_back) {
      return reject("candidate rollback conflicts with anonymous commit");
    }
    graph.rolled_back_candidate_ids.insert(event.generation);
    return true;
  }

  /**
   * @brief Applies one standalone or realtime bundle admission.
   * @param event Bundle identity plus first child and optional group identity.
   * @return True only when one pending candidate can be consumed.
   * @throws std::bad_alloc when bundle/run/group identity storage grows.
   */
  bool admit_bundle(const compute::ExecutionLifecycleEvent& event) {
    const auto graph_found = graphs_.find(event.graph_instance_id);
    if (shutdown_started_ || graph_found == graphs_.end() ||
        graph_found->second.state != M1ReplayGraphState::Open ||
        pending_candidates(graph_found->second) == 0U ||
        !all_bundle_ids_.insert(event.generation).second ||
        !all_run_ids_.insert(event.run_id).second ||
        (event.run_group_id != 0U &&
         !all_run_group_ids_.insert(event.run_group_id).second)) {
      return reject("BundleAdmitted identity or candidate consumption drifted");
    }
    ++graph_found->second.committed_candidate_count;
    M1ReplayBundle bundle;
    bundle.bundle_id = event.generation;
    bundle.graph_instance_id = event.graph_instance_id;
    bundle.run_group_id = event.run_group_id;
    bundle.expected_run_count = event.run_group_id == 0U ? 1U : 2U;
    bundle.runs.push_back(
        M1ReplayRun{event.run_id, M1ReplayRunState::Admitted});
    bundles_.emplace(bundle.bundle_id, std::move(bundle));
    return true;
  }

  /**
   * @brief Consumes one previously captured cancellation publication.
   * @param event Graph/category/generation correlation record.
   * @return True only when one captured bundle record remains.
   * @throws std::bad_alloc when diagnostics allocate.
   */
  bool consume_cancellation(const compute::ExecutionLifecycleEvent& event) {
    const M1CancelKey key{event.graph_instance_id,
                          static_cast<std::uint16_t>(event.category),
                          event.generation};
    const auto found = pending_cancellations_.find(key);
    if (found == pending_cancellations_.end() || found->second == 0U) {
      return reject("CancellationRequested has no captured bundle");
    }
    if (--found->second == 0U) {
      pending_cancellations_.erase(found);
    }
    return true;
  }

  /**
   * @brief Maps one Run event kind to its required prior and next state.
   * @param kind RunTerminal, RunQuiescent, ResourceSettled, or RunUnregistered.
   * @return Exact prior/next pair.
   * @throws std::logic_error for a non-Run kind.
   */
  static std::pair<M1ReplayRunState, M1ReplayRunState> run_transition(
      compute::ExecutionLifecycleEventKind kind) {
    using Kind = compute::ExecutionLifecycleEventKind;
    switch (kind) {
      case Kind::RunTerminal:
        return {M1ReplayRunState::Admitted, M1ReplayRunState::Terminal};
      case Kind::RunQuiescent:
        return {M1ReplayRunState::Terminal, M1ReplayRunState::Quiescent};
      case Kind::ResourceSettled:
        return {M1ReplayRunState::Quiescent, M1ReplayRunState::ResourceSettled};
      case Kind::RunUnregistered:
        return {M1ReplayRunState::ResourceSettled,
                M1ReplayRunState::Unregistered};
      default:
        throw std::logic_error("M1 lifecycle replay received a non-Run kind");
    }
  }

  /**
   * @brief Applies one child transition with bundle-wide phase barriers.
   * @param event Exact child/bundle/Graph/group identity publication.
   * @return True only for terminal-to-quiescent-to-settled-to-unregistered.
   * @throws std::bad_alloc when the second realtime child is retained.
   */
  bool advance_run(const compute::ExecutionLifecycleEvent& event) {
    const auto bundle_found = bundles_.find(event.generation);
    if (bundle_found == bundles_.end()) {
      return reject("Run transition names no live or unregistering bundle");
    }
    M1ReplayBundle& bundle = bundle_found->second;
    if (event.graph_instance_id != bundle.graph_instance_id ||
        event.run_group_id != bundle.run_group_id) {
      return reject("Run transition crosses Graph or group identity");
    }

    auto run_found = std::find_if(bundle.runs.begin(), bundle.runs.end(),
                                  [&event](const M1ReplayRun& run) {
                                    return run.run_id == event.run_id;
                                  });
    if (run_found == bundle.runs.end()) {
      const bool learns_second_child =
          bundle.expected_run_count == 2U && bundle.runs.size() == 1U &&
          event.kind == compute::ExecutionLifecycleEventKind::RunTerminal &&
          pending_group_step_.has_value() &&
          pending_group_step_->bundle_id == bundle.bundle_id &&
          all_run_ids_.insert(event.run_id).second;
      if (!learns_second_child) {
        return reject(
            "Run transition uses an unknown or reused child identity");
      }
      bundle.runs.push_back(
          M1ReplayRun{event.run_id, M1ReplayRunState::Admitted});
      run_found = std::prev(bundle.runs.end());
    }

    const std::size_t run_index =
        static_cast<std::size_t>(std::distance(bundle.runs.begin(), run_found));
    const auto transition = run_transition(event.kind);
    if (run_found->state != transition.first) {
      return reject(
          "Run settlement phase is duplicated, skipped, or reordered");
    }
    if (bundle.expected_run_count == 2U) {
      if (run_index == 0U) {
        if (bundle.runs.size() != 2U &&
            event.kind != compute::ExecutionLifecycleEventKind::RunTerminal) {
          return reject("realtime second child is absent before later phases");
        }
        if (event.kind != compute::ExecutionLifecycleEventKind::RunTerminal &&
            bundle.runs[1U].state != transition.first) {
          return reject("realtime phase began before both children arrived");
        }
      } else if (run_index != 1U ||
                 bundle.runs[0U].state != transition.second) {
        return reject("realtime children are not in producer order");
      }
    }

    run_found->state = transition.second;
    if (event.kind == compute::ExecutionLifecycleEventKind::RunUnregistered &&
        run_index == 0U) {
      bundle.detached = true;
    }
    if (bundle.expected_run_count == 2U) {
      if (run_index == 0U) {
        pending_group_step_ = M1ReplayGroupStep{bundle.bundle_id, event.kind};
      } else {
        pending_group_step_.reset();
      }
    }

    const bool complete = std::all_of(
        bundle.runs.begin(), bundle.runs.end(), [](const M1ReplayRun& run) {
          return run.state == M1ReplayRunState::Unregistered;
        });
    if (complete) {
      bundles_.erase(bundle_found);
    }
    return true;
  }

  /**
   * @brief Applies registration rollback or empty Closing-row removal.
   * @param event Exact GraphRowRemoved publication.
   * @return True only after candidate/bundle settlement and valid generation.
   * @throws std::bad_alloc when diagnostics allocate.
   */
  bool remove_graph(const compute::ExecutionLifecycleEvent& event) {
    const auto found = graphs_.find(event.graph_instance_id);
    if (found == graphs_.end() || pending_candidates(found->second) != 0U) {
      return reject("GraphRowRemoved names no empty live row");
    }
    const bool has_bundle = std::any_of(
        bundles_.begin(), bundles_.end(), [&event](const auto& entry) {
          return entry.second.graph_instance_id == event.graph_instance_id;
        });
    if (has_bundle) {
      return reject("GraphRowRemoved precedes complete bundle unregistration");
    }
    if (found->second.state == M1ReplayGraphState::Open) {
      if (event.generation != 0U || !found->second.candidate_ids.empty()) {
        return reject("Graph registration rollback is not pristine");
      }
    } else if (event.generation != found->second.close_generation) {
      return reject("GraphRowRemoved close generation drifted");
    }
    graphs_.erase(found);
    return true;
  }

  /**
   * @brief Applies the final service event after complete logical settlement.
   * @param event Exact ServiceStopped record.
   * @return True only for matching generation and empty replay ownership.
   * @throws std::bad_alloc when diagnostics allocate.
   */
  bool stop_service(const compute::ExecutionLifecycleEvent& event) {
    if (!shutdown_started_ || event.generation != shutdown_generation_ ||
        !graphs_.empty() || !bundles_.empty() ||
        !pending_cancellations_.empty() || pending_group_step_.has_value() ||
        shutdown_graph_closing_index_ != shutdown_graph_closing_order_.size()) {
      return reject("ServiceStopped precedes complete lifecycle settlement");
    }
    service_stopped_ = true;
    return true;
  }

  /** @brief Stable mutable diagnostic target supplied by the evaluator. */
  std::vector<std::string>* reasons_ = nullptr;
  /** @brief True after the unique sequence-one service origin. */
  bool service_started_ = false;
  /** @brief True after the no-event Accepting-to-Stopping transition. */
  bool shutdown_started_ = false;
  /** @brief True after the unique final ServiceStopped record. */
  bool service_stopped_ = false;
  /** @brief Stable process-shutdown generation after Stopping. */
  std::uint64_t shutdown_generation_ = 0U;
  /** @brief Live Graph rows keyed by exact GraphInstanceId scalar. */
  std::map<std::uint64_t, M1ReplayGraph> graphs_;
  /** @brief Process-nonreused Graph identities observed in this service. */
  std::set<std::uint64_t> all_graph_ids_;
  /** @brief Live-row insertion order used by shutdown enumeration. */
  std::vector<std::uint64_t> graph_registration_order_;
  /** @brief Process-nonreused candidate identities observed in this service. */
  std::set<std::uint64_t> all_candidate_ids_;
  /** @brief Live or mid-unregistration bundles keyed by bundle identity. */
  std::map<std::uint64_t, M1ReplayBundle> bundles_;
  /** @brief Process-nonreused bundle identities observed in this service. */
  std::set<std::uint64_t> all_bundle_ids_;
  /** @brief Process-nonreused child Run identities observed in this service. */
  std::set<std::uint64_t> all_run_ids_;
  /** @brief Process-nonreused realtime group identities in this service. */
  std::set<std::uint64_t> all_run_group_ids_;
  /** @brief Captured cancellation publications remaining by correlation. */
  std::map<M1CancelKey, std::uint64_t> pending_cancellations_;
  /** @brief Required second child transition in one fence-held group phase. */
  std::optional<M1ReplayGroupStep> pending_group_step_;
  /** @brief Exact row order emitted by begin_service_shutdown(). */
  std::vector<std::uint64_t> shutdown_graph_closing_order_;
  /** @brief Next required row in the shutdown GraphClosing batch. */
  std::size_t shutdown_graph_closing_index_ = 0U;
};

/**
 * @brief Replays every retained lifecycle page as one lossless cursor chain.
 *
 * The replay requires exact chronological capture ordinals, preserves each
 * request cursor, proves every sequence from cursor+1 through the atomic cut,
 * checks closed enums/identities/timestamps, and requires the lifecycle effects
 * necessarily produced by a nonempty mixed workload.
 *
 * @param snapshots Chronological M1 execution snapshots starting at cursor 0.
 * @param require_workload_effects True when retained protocol work exists.
 * @param reasons Mutable row diagnostics receiving stable invalidations.
 * @return True only for one complete continuous same-service history.
 * @throws std::bad_alloc when event-kind tracking or diagnostics allocate.
 */
bool validate_m1_lifecycle_history(
    const std::vector<M1ExecutionSnapshot>& snapshots,
    bool require_workload_effects, std::vector<std::string>* reasons) {
  bool valid = true;
  const auto invalid = [&valid, reasons](std::string reason) {
    valid = false;
    invalidate_m1(reasons, "M1 memory evidence: lifecycle " + reason);
  };
  if (snapshots.empty()) {
    invalid("history is empty");
    return false;
  }

  const std::uint64_t service_id =
      snapshots.front().lifecycle.service_instance_id;
  const std::uint64_t telemetry_epoch =
      snapshots.front().lifecycle.telemetry_epoch;
  std::uint64_t expected_cursor = 0U;
  std::uint64_t previous_timestamp_us = 0U;
  std::uint64_t shutdown_generation = 0U;
  compute::ExecutionLifecycleServiceState previous_state =
      compute::ExecutionLifecycleServiceState::Accepting;
  std::set<compute::ExecutionLifecycleEventKind> observed_kinds;
  bool observed_any_event = false;
  bool observed_service_stopped = false;
  M1LifecycleReplay replay(reasons);

  for (std::size_t index = 0U; index < snapshots.size(); ++index) {
    const M1ExecutionSnapshot& snapshot = snapshots[index];
    const compute::ExecutionLifecyclePage& page = snapshot.lifecycle;
    if (snapshot.temporal_capture_ordinal != index) {
      invalid("capture ordinal is missing, duplicated, or reordered");
    }
    if (snapshot.lifecycle_after_cursor != expected_cursor) {
      invalid("request cursor does not continue the prior page cut");
    }
    if (page.schema_version !=
            compute::kExecutionLifecycleTelemetrySchemaVersion ||
        page.capacity != compute::kExecutionLifecycleTelemetryCapacity ||
        service_id == 0U || page.service_instance_id != service_id ||
        telemetry_epoch == 0U || page.telemetry_epoch != telemetry_epoch ||
        !known_service_state(page.service_state) ||
        page.global_dropped_total != 0U || page.global_dropped_saturated ||
        page.cursor_gap != 0U || page.has_more) {
      invalid("schema, identity, enum, or losslessness drifted");
    }

    if (known_service_state(page.service_state) &&
        static_cast<std::uint16_t>(page.service_state) <
            static_cast<std::uint16_t>(previous_state)) {
      invalid("service state moved backwards");
    }
    previous_state = page.service_state;
    if (page.service_state ==
        compute::ExecutionLifecycleServiceState::Accepting) {
      if (page.shutdown_generation != 0U) {
        invalid("Accepting page carries a shutdown generation");
      }
    } else {
      if (page.shutdown_generation == 0U) {
        invalid("Stopping or Stopped page lacks a shutdown generation");
      } else if (shutdown_generation == 0U) {
        shutdown_generation = page.shutdown_generation;
      } else if (shutdown_generation != page.shutdown_generation) {
        invalid("shutdown generation changed between pages");
      }
    }

    if (page.snapshot_cut == 0U) {
      if (snapshot.lifecycle_after_cursor != 0U ||
          page.first_retained_sequence != 0U || page.next_sequence != 1U ||
          !page.records.empty() || page.next_cursor != 0U ||
          page.service_state !=
              compute::ExecutionLifecycleServiceState::Accepting) {
        invalid("empty-ring page contradicts the producer contract");
      }
      expected_cursor = 0U;
      continue;
    }

    if (page.first_retained_sequence != 1U) {
      invalid("lossless nonempty ring does not retain sequence one");
    }
    if (snapshot.lifecycle_after_cursor > page.snapshot_cut ||
        page.next_cursor != page.snapshot_cut) {
      invalid("request cursor, next cursor, and atomic cut are inconsistent");
    }
    std::uint64_t expected_record_count = 0U;
    if (snapshot.lifecycle_after_cursor <= page.snapshot_cut) {
      expected_record_count =
          page.snapshot_cut - snapshot.lifecycle_after_cursor;
    }
    if (expected_record_count >
            compute::kExecutionLifecycleTelemetryMaxPageSize ||
        page.records.size() != expected_record_count) {
      invalid("page omits, duplicates, or truncates sequenced records");
    }

    for (std::size_t record_index = 0U; record_index < page.records.size();
         ++record_index) {
      const compute::ExecutionLifecycleEvent& event =
          page.records[record_index];
      const std::uint64_t expected_sequence =
          snapshot.lifecycle_after_cursor + record_index + 1U;
      if (event.schema_version !=
              compute::kExecutionLifecycleTelemetrySchemaVersion ||
          event.sequence != expected_sequence ||
          event.service_instance_id != service_id ||
          event.telemetry_epoch != telemetry_epoch ||
          event.timestamp_saturated ||
          !known_lifecycle_event_kind(event.kind) ||
          !known_lifecycle_category(event.category) ||
          !valid_lifecycle_event_identity(event)) {
        invalid(
            "record schema, sequence, identity shape, timestamp, or enum "
            "drifted");
      }
      if (observed_any_event && event.timestamp_us < previous_timestamp_us) {
        invalid("record timestamps moved backwards");
      }
      previous_timestamp_us = event.timestamp_us;
      observed_any_event = true;
      observed_kinds.insert(event.kind);

      const bool shutdown_event =
          (event.kind == compute::ExecutionLifecycleEventKind::GraphClosing &&
           event.category ==
               compute::ExecutionLifecycleCategory::ProcessShutdown) ||
          event.kind == compute::ExecutionLifecycleEventKind::WorkerJoined ||
          event.kind == compute::ExecutionLifecycleEventKind::ServiceStopped;
      if (!replay.shutdown_started() &&
          page.service_state !=
              compute::ExecutionLifecycleServiceState::Accepting &&
          shutdown_event && !replay.begin_shutdown(page.shutdown_generation)) {
        valid = false;
      }
      if (!replay.apply(event)) {
        valid = false;
      }

      if (event.sequence == 1U &&
          (event.kind != compute::ExecutionLifecycleEventKind::ServiceStarted ||
           event.category != compute::ExecutionLifecycleCategory::None ||
           event.graph_instance_id != 0U || event.run_id != 0U ||
           event.run_group_id != 0U || event.generation != 0U)) {
        invalid("sequence one is not the canonical ServiceStarted event");
      }
      if (event.kind == compute::ExecutionLifecycleEventKind::ServiceStarted) {
        if (event.sequence != 1U ||
            event.category != compute::ExecutionLifecycleCategory::None) {
          invalid("ServiceStarted is duplicated or malformed");
        }
      }
      if (event.kind == compute::ExecutionLifecycleEventKind::ServiceStopped) {
        observed_service_stopped = true;
        if (page.service_state !=
                compute::ExecutionLifecycleServiceState::Stopped ||
            event.category != compute::ExecutionLifecycleCategory::None ||
            event.graph_instance_id != 0U || event.run_id != 0U ||
            event.run_group_id != 0U || event.generation == 0U ||
            event.generation != page.shutdown_generation) {
          invalid("ServiceStopped event is malformed or precedes Stopped");
        }
      } else if (observed_service_stopped) {
        invalid("ordinary event appears after ServiceStopped");
      }
    }

    if (!replay.shutdown_started() &&
        page.service_state !=
            compute::ExecutionLifecycleServiceState::Accepting &&
        !replay.begin_shutdown(page.shutdown_generation)) {
      valid = false;
    }
    if (!replay.validate_page(page)) {
      valid = false;
    }

    if (page.service_state ==
        compute::ExecutionLifecycleServiceState::Stopped) {
      if (page.next_sequence != std::numeric_limits<std::uint64_t>::max() ||
          !observed_service_stopped) {
        invalid("Stopped page lacks the final event or exhausted sentinel");
      }
    } else if (page.snapshot_cut == std::numeric_limits<std::uint64_t>::max() ||
               page.next_sequence != page.snapshot_cut + 1U) {
      invalid("next sequence does not immediately follow the atomic cut");
    }
    expected_cursor = page.snapshot_cut;
  }

  if (observed_kinds.count(
          compute::ExecutionLifecycleEventKind::ServiceStarted) != 1U) {
    invalid("history lacks the unique ServiceStarted origin");
  }
  if (require_workload_effects) {
    for (const compute::ExecutionLifecycleEventKind required :
         {compute::ExecutionLifecycleEventKind::GraphRegistered,
          compute::ExecutionLifecycleEventKind::CandidateBegan,
          compute::ExecutionLifecycleEventKind::BundleAdmitted,
          compute::ExecutionLifecycleEventKind::RunTerminal,
          compute::ExecutionLifecycleEventKind::RunQuiescent,
          compute::ExecutionLifecycleEventKind::ResourceSettled,
          compute::ExecutionLifecycleEventKind::RunUnregistered,
          compute::ExecutionLifecycleEventKind::GraphClosing,
          compute::ExecutionLifecycleEventKind::GraphRowRemoved}) {
      if (observed_kinds.count(required) == 0U) {
        invalid("history lacks one or more required mixed-workload effects");
        break;
      }
    }
  }
  if (!replay.complete()) {
    valid = false;
  }
  return valid;
}

/**
 * @brief Validates temporal snapshots and event-derived Compute I/O evidence.
 * @param snapshots Chronological same-service samples including final cut.
 * @param offers Complete immutable B1 protocol offer set.
 * @param jobs Exact-one authority-free B1 I/O evidence for every offer.
 * @param row Mutable result receiving high-water values and diagnostics.
 * @return Structural validity plus independently complete limit/settlement
 * outcomes.
 * @throws std::bad_alloc when diagnostics allocate.
 */
M1MemoryValidation validate_m1_memory(
    const std::vector<M1ExecutionSnapshot>& snapshots,
    const std::vector<M1BatchOfferEvidence>& offers,
    const std::vector<M1BatchSourceEvidence>& jobs, M1InnerRow* row) {
  M1MemoryValidation result;
  const auto invalid = [&result, row](std::string reason) {
    result.valid = false;
    invalidate_m1(&row->validity_reasons, "M1 memory evidence: " + reason);
  };
  if (snapshots.size() < 4U) {
    invalid("fewer than four boundary/final snapshots");
    return result;
  }

  std::map<B1JobInstance, const M1BatchSourceEvidence*> indexed_jobs;
  for (const M1BatchSourceEvidence& job : jobs) {
    if (!indexed_jobs.emplace(job.job, &job).second) {
      invalid("multiple B1 I/O streams claim the same occurrence");
    }
  }
  if (jobs.size() != offers.size() || indexed_jobs.size() != offers.size()) {
    invalid("B1 I/O stream cardinality does not match protocol offers");
  }
  std::set<std::uint64_t> accounting_sequences;
  for (const M1BatchOfferEvidence& offer : offers) {
    const auto found = indexed_jobs.find(offer.job);
    if (found == indexed_jobs.end()) {
      invalid("a protocol B1 offer lacks its complete I/O stream");
      continue;
    }
    const M1BatchSourceEvidence& job = *found->second;
    if (job.producer_offer_ordinal != offer.producer_offer_ordinal ||
        job.offered_at != offer.offered.timestamp ||
        !offer.endpoint.has_value() ||
        job.endpoint_at != offer.endpoint->timestamp) {
      invalid("B1 I/O stream identity or endpoint differs from its offer");
    }
    const B1ComputeIoEvidenceInput io_input{job.job, job.output_status,
                                            job.output_receipt.has_value(),
                                            job.io_observations};
    const B1ComputeIoEvaluation io = evaluate_b1_compute_io_evidence(io_input);
    if (!io.structurally_valid || !io.fault_free_complete) {
      invalid("B1 I/O stream is malformed or not fault-free complete");
      for (const std::string& reason : io.validity_reasons) {
        invalidate_m1(&row->validity_reasons, "M1 memory evidence: " + reason);
      }
    }
    row->compute_io_task_high_water =
        std::max(row->compute_io_task_high_water, io.task_high_water);
    row->compute_io_planned_byte_high_water = std::max(
        row->compute_io_planned_byte_high_water, io.planned_byte_high_water);
    for (const B1ComputeIoObservation& observation : job.io_observations) {
      std::optional<std::uint64_t> sequence;
      if (observation.point == B1IoObservationPoint::AcceptedAdmission ||
          observation.point == B1IoObservationPoint::OfferRejected) {
        if (observation.admission_event.has_value()) {
          sequence = observation.admission_event->sequence;
        }
      } else if (observation.point == B1IoObservationPoint::Settlement &&
                 observation.settlement_event.has_value()) {
        sequence = observation.settlement_event->sequence;
      }
      if (sequence.has_value() &&
          !accounting_sequences.insert(*sequence).second) {
        invalid("Compute I/O accounting sequence is duplicated across jobs");
      }
    }
  }

  if (!validate_m1_lifecycle_history(snapshots, !jobs.empty(),
                                     &row->validity_reasons)) {
    result.valid = false;
  }
  ResourceVector prior_high_water = snapshots.front().host_resources.high_water;
  const std::vector<ResourceLedger::DeviceSnapshot>& initial_devices =
      snapshots.front().device_resources;
  std::map<DeviceId, DeviceResourceVector> prior_device_high_water;
  for (const ResourceLedger::DeviceSnapshot& device : initial_devices) {
    if (!prior_device_high_water.emplace(device.device, device.high_water)
             .second) {
      invalid("configured device inventory contains a duplicate identity");
    }
  }

  for (const M1ExecutionSnapshot& snapshot : snapshots) {
    if (snapshot.host_resources.limits != kM1HostLimits ||
        snapshot.throughput.capacity != kM1ThroughputCapacity) {
      invalid("Host limits or Throughput headroom capacity drifted");
    }
    if (!resources_fit(snapshot.host_resources.reserved,
                       snapshot.host_resources.limits) ||
        !resources_fit(snapshot.host_resources.reserved,
                       snapshot.host_resources.high_water) ||
        !resource_high_water_nondecreasing(
            prior_high_water, snapshot.host_resources.high_water)) {
      invalid("Host reservation or lifetime high-water is contradictory");
    }
    if (!resources_fit(snapshot.host_resources.high_water,
                       snapshot.host_resources.limits)) {
      result.within_limits = false;
    }
    prior_high_water = snapshot.host_resources.high_water;
    if (!resources_fit(snapshot.throughput.reserved,
                       snapshot.throughput.capacity)) {
      invalid("Throughput reservation exceeds general capacity");
    }
    if (!snapshot.ready_classes.valid ||
        snapshot.ready_classes.interactive_entries >
            std::numeric_limits<std::uint64_t>::max() -
                snapshot.ready_classes.throughput_entries ||
        snapshot.ready_classes.interactive_entries +
                snapshot.ready_classes.throughput_entries !=
            snapshot.ready_classes.total_entries) {
      invalid("ready-store class partition is malformed");
    }
    const execution::ComputeIoExecutorSnapshot& io = snapshot.compute_io;
    if (io.task_limit != kB1ComputeIoTaskLimit ||
        io.planned_bytes_limit != kB1ComputeIoPlannedByteLimit ||
        io.constructing_tasks >
            std::numeric_limits<std::uint64_t>::max() - io.queued_tasks ||
        io.constructing_tasks + io.queued_tasks >
            std::numeric_limits<std::uint64_t>::max() - io.running_tasks ||
        io.constructing_tasks + io.queued_tasks + io.running_tasks !=
            io.active_tasks ||
        io.active_tasks > io.task_limit ||
        io.active_planned_bytes > io.planned_bytes_limit) {
      invalid("Compute I/O limits, phase partition, or active state drifted");
    }
    if (snapshot.device_resources.size() != initial_devices.size()) {
      invalid("configured device inventory cardinality changed");
    } else {
      for (std::size_t index = 0U; index < initial_devices.size(); ++index) {
        const ResourceLedger::DeviceSnapshot& initial = initial_devices[index];
        const ResourceLedger::DeviceSnapshot& current =
            snapshot.device_resources[index];
        if (current.device != initial.device ||
            current.limits != initial.limits ||
            current.available.device_memory_bytes >
                current.limits.device_memory_bytes ||
            current.available.device_scratch_bytes >
                current.limits.device_scratch_bytes ||
            current.available.device_memory_bytes +
                    current.reserved.device_memory_bytes !=
                current.limits.device_memory_bytes ||
            current.available.device_scratch_bytes +
                    current.reserved.device_scratch_bytes !=
                current.limits.device_scratch_bytes) {
          invalid("device identity, limits, reservation, or available drifted");
        }
        const auto prior = prior_device_high_water.find(current.device);
        if (prior == prior_device_high_water.end() ||
            !device_resources_fit(current.reserved, current.high_water) ||
            !device_resources_fit(prior->second, current.high_water)) {
          invalid("device reservation or lifetime high-water is contradictory");
        } else {
          prior->second = current.high_water;
        }
        if (!device_resources_fit(current.reserved, current.limits)) {
          invalid("device reservation exceeds its configured limits");
        }
        if (!device_resources_fit(current.high_water, current.limits)) {
          result.within_limits = false;
        }
        if (current.device.backend() == DeviceBackend::Metal &&
            current.limits != kM1MetalLimits) {
          invalid("configured Metal limits differ from the frozen profile");
        }
      }
    }
  }

  const M1ExecutionSnapshot& initial = snapshots.front();
  if (initial.compute_io.active_tasks != 0U ||
      initial.compute_io.active_planned_bytes != 0U ||
      initial.compute_io.constructing_tasks != 0U ||
      initial.compute_io.queued_tasks != 0U ||
      initial.compute_io.running_tasks != 0U) {
    invalid("initial Compute I/O boundary is not zero");
  }
  const M1ExecutionSnapshot& final = snapshots.back();
  if (!zero_resources(final.host_resources.reserved) ||
      !zero_resources(final.throughput.reserved) ||
      final.ready_classes.total_entries != 0U ||
      final.compute_io.active_tasks != 0U ||
      final.compute_io.active_planned_bytes != 0U ||
      final.compute_io.constructing_tasks != 0U ||
      final.compute_io.queued_tasks != 0U ||
      final.compute_io.running_tasks != 0U ||
      !lifecycle_settled(final.lifecycle.counters)) {
    result.settled = false;
    invalid("final Host, ready, I/O, or lifecycle ownership is not zero");
  }
  for (const ResourceLedger::DeviceSnapshot& device : final.device_resources) {
    if (device.reserved != DeviceResourceVector{}) {
      result.settled = false;
      invalid("final device ownership is not zero");
    }
  }
  return result;
}

}  // namespace

/** @copydoc make_m1_interactive_source_evidence */
M1InteractiveSourceEvidence make_m1_interactive_source_evidence(
    B1JobPhase phase, std::size_t phase_ordinal, M1EventCoordinate origin,
    const I1EpisodeInnerRow& row) {
  if (row.schema != kI1InnerRowSchema ||
      row.schema_version != kI1InnerRowSchemaVersion ||
      row.workload_id != kI1WorkloadId ||
      row.evidence.slot != m1_i1_source_slot(phase, phase_ordinal) ||
      row.evidence.episode_origin != origin.timestamp) {
    throw std::invalid_argument(
        "M1 I1 source identity differs from its Issue #93 row.");
  }
  const I1EpisodeInnerRow replay = evaluate_i1_episode(row.evidence);
  if (replay.final_latency != row.final_latency ||
      replay.service.all_started_service != row.service.all_started_service ||
      replay.service.discarded_started_service !=
          row.service.discarded_started_service ||
      replay.service.post_cancel_started_service !=
          row.service.post_cancel_started_service ||
      replay.latency_verdict != row.latency_verdict ||
      replay.waste_verdict != row.waste_verdict ||
      replay.memory_verdict != row.memory_verdict ||
      replay.output_verdict != row.output_verdict) {
    throw std::invalid_argument(
        "M1 I1 source row does not recompute its derived projection.");
  }
  return M1InteractiveSourceEvidence{phase, phase_ordinal, origin,
                                     row.evidence};
}

/** @copydoc make_m1_batch_source_evidence */
M1BatchSourceEvidence make_m1_batch_source_evidence(
    const B1JobEvidence& evidence) {
  M1BatchSourceEvidence source;
  source.job = evidence.job;
  source.producer_offer_ordinal = evidence.producer_offer_ordinal;
  source.offered_at = evidence.offered_at;
  source.endpoint_at = evidence.endpoint_at;
  source.run_succeeded = evidence.run_succeeded;
  source.verified_endpoint = b1_job_has_verified_endpoint(evidence);
  source.physical_trace = evidence.physical_trace;
  source.output_status = evidence.output.status;
  source.io_observations = evidence.output.io_observations;
  source.golden = evidence.golden;
  source.semantic_trace = evidence.semantic_trace;
  source.semantic_trace_digest = evidence.semantic_trace_digest;
  if (evidence.output.receipt.has_value()) {
    const B1OutputCommitReceipt& receipt = *evidence.output.receipt;
    source.output_receipt =
        M1BatchReceiptEvidence{receipt.commit_id(),
                               receipt.resolved_root(),
                               receipt.rooted_slot(),
                               receipt.job(),
                               receipt.logical_descriptor(),
                               receipt.logical_content_digest(),
                               receipt.committed_generation(),
                               receipt.payload_name(),
                               receipt.manifest_name(),
                               receipt.payload_length(),
                               receipt.manifest_length(),
                               receipt.payload_digest(),
                               receipt.manifest_digest(),
                               receipt.requested_durability(),
                               receipt.achieved_durability(),
                               receipt.published_manifest_identity()};
  }
  return source;
}

/** @copydoc derive_m1_source_fairness_projection */
M1SourceFairnessProjection derive_m1_source_fairness_projection(
    const M1ProtocolEvidenceInput& protocol,
    const std::vector<M1InteractiveSourceEvidence>& interactive_sources,
    const std::vector<M1BatchSourceEvidence>& batch_sources) {
  M1SourceFairnessProjection projection;
  projection.progress_windows.reserve(kM1MeasuredWindowCount);
  projection.graph_service_windows.reserve(kM1MeasuredWindowCount);
  projection.headroom_outcomes.reserve(kM1MeasuredI1AttemptCount);

  if (protocol.interactive_occurrences.size() != kM1TotalI1OriginCount ||
      interactive_sources.size() != kM1TotalI1OriginCount ||
      interactive_sources.size() != protocol.interactive_occurrences.size()) {
    throw std::invalid_argument(
        "M1 I1 source cardinality differs from 48 occurrences.");
  }
  std::size_t measured_source_count = 0U;
  for (std::size_t index = 0U; index < interactive_sources.size(); ++index) {
    const auto expected = expected_m1_i1_source_identity(index);
    const M1InteractiveSourceEvidence& source = interactive_sources[index];
    const M1InteractiveOccurrenceEvidence& occurrence =
        protocol.interactive_occurrences[index];
    if (source.phase != expected.first ||
        source.phase_ordinal != expected.second ||
        occurrence.phase != expected.first ||
        occurrence.phase_ordinal != expected.second ||
        !m1_i1_source_matches(source, occurrence, protocol.replicate_ordinal)) {
      throw std::invalid_argument(
          "M1 I1 source identity/order or derived projection drifted.");
    }
    if (source.phase != B1JobPhase::Measured) {
      continue;
    }
    if (!checked_size_increment(&measured_source_count)) {
      throw std::overflow_error("M1 measured I1 source count overflowed.");
    }
    for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
      const I1EditEvidence& edit = source.episode.edits[edit_index];
      if (edit.edit_index != edit_index) {
        throw std::invalid_argument(
            "M1 measured I1 edit identity/order drifted.");
      }
      const bool headroom_failure =
          edit.host_return.has_value() && !edit.host_return->status.ok;
      projection.headroom_outcomes.push_back(M1HeadroomAdmissionOutcome{
          source.phase_ordinal, edit.edit_index, edit.admission_attempted,
          edit.host_return.has_value()
              ? std::optional<OperationStatus>(edit.host_return->status)
              : std::nullopt,
          headroom_failure});
      if ((edit.admission_attempted &&
           !checked_size_increment(
               &projection.headroom_admissions.attempted_edits)) ||
          (edit.host_return.has_value() &&
           !checked_size_increment(
               &projection.headroom_admissions.classified_outcomes)) ||
          (headroom_failure &&
           !checked_size_increment(
               &projection.headroom_admissions.throughput_headroom_failures))) {
        throw std::overflow_error("M1 headroom aggregate overflowed.");
      }
    }
  }
  const M1AdmissionSourceProjection admission_projection =
      derive_m1_admission_source_projection(interactive_sources);
  projection.first_measured_admission = admission_projection.admission;
  projection.final_warmup_settlement_pending_at_measurement =
      admission_projection.final_warmup_settlement_pending_at_measurement;
  if (measured_source_count != kM1MeasuredI1OriginCount ||
      projection.headroom_outcomes.size() != kM1MeasuredI1AttemptCount) {
    throw std::invalid_argument(
        "M1 measured I1 source projection differs from 40 by 12 outcomes.");
  }

  const auto measurement_start =
      protocol.boundaries.measurement_start.timestamp;
  const auto measurement_end =
      checked_i1_time_add(measurement_start, kM1MeasurementDuration);
  if (protocol.boundaries.measurement_end.timestamp != measurement_end) {
    throw std::invalid_argument(
        "M1 fairness projection boundaries do not span exactly 30 seconds.");
  }
  if (batch_sources.size() != protocol.batch_offers.size()) {
    throw std::invalid_argument(
        "M1 B1 source cardinality differs from protocol offers.");
  }

  std::set<B1JobInstance> source_jobs;
  std::vector<M1ProjectedBatchSource> projected_sources;
  projected_sources.reserve(batch_sources.size());
  for (std::size_t index = 0U; index < batch_sources.size(); ++index) {
    const M1BatchSourceEvidence& source = batch_sources[index];
    const M1BatchOfferEvidence& offer = protocol.batch_offers[index];
    validate_b1_job_instance(source.job);
    if (!source_jobs.insert(source.job).second) {
      throw std::invalid_argument("M1 B1 source identity/order is duplicated.");
    }
    if (!offer.endpoint.has_value() || !(source.job == offer.job) ||
        source.producer_offer_ordinal != offer.producer_offer_ordinal ||
        source.offered_at != offer.offered.timestamp ||
        source.endpoint_at != offer.endpoint->timestamp ||
        !(source.physical_trace.job == source.job)) {
      throw std::invalid_argument(
          "M1 B1 source identity/order or offer endpoint drifted.");
    }
    M1BatchSourceReplay replay = replay_m1_batch_source(source);
    if (!replay.structurally_valid) {
      throw std::invalid_argument("M1 B1 source replay is malformed or lossy.");
    }
    if (replay.verified_endpoint != source.verified_endpoint) {
      throw std::invalid_argument(
          "M1 B1 source verified-endpoint projection drifted.");
    }
    projected_sources.push_back(
        M1ProjectedBatchSource{&offer, std::move(replay)});
  }

  for (std::size_t window = 0U; window < kM1MeasuredWindowCount; ++window) {
    const auto start = checked_i1_time_add(
        measurement_start,
        std::chrono::seconds(static_cast<std::int64_t>(window)));
    const auto end = checked_i1_time_add(start, std::chrono::seconds(1));
    std::uint64_t successful_site_operations = 0U;
    std::uint64_t graph_service[2U]{0U, 0U};
    for (const M1ProjectedBatchSource& source : projected_sources) {
      const M1BatchOfferEvidence& offer = *source.offer;
      if (offer.job.phase != B1JobPhase::Measured ||
          !source.replay.verified_endpoint || !offer.endpoint.has_value() ||
          offer.endpoint->timestamp < start ||
          !(offer.endpoint->timestamp < end)) {
        continue;
      }
      const std::size_t graph =
          static_cast<std::size_t>(offer.job.job_index & 1U);
      if (!checked_accumulate(&successful_site_operations,
                              kB1SiteOperationsPerJob) ||
          !checked_accumulate(&graph_service[graph],
                              source.replay.all_started_service)) {
        throw std::overflow_error(
            "M1 source-derived progress or Graph service overflowed.");
      }
    }
    projection.progress_windows.push_back(M1ThroughputProgressSample{
        window, successful_site_operations, std::chrono::seconds(1)});
    projection.graph_service_windows.push_back(M1GraphServiceWindow{
        window,
        m1_graph_demand_covers(protocol.batch_offers, 0U, start, end) &&
            m1_graph_demand_covers(protocol.batch_offers, 1U, start, end),
        graph_service[0U], graph_service[1U]});
  }
  if (projection.progress_windows.size() != kM1MeasuredWindowCount ||
      projection.graph_service_windows.size() != kM1MeasuredWindowCount) {
    throw std::invalid_argument(
        "M1 source-derived fairness projection cardinality drifted.");
  }
  return projection;
}

/** @copydoc derive_m1_batch_waste_evidence */
M1BatchWasteEvidence derive_m1_batch_waste_evidence(
    const std::vector<M1BatchSourceEvidence>& sources) {
  M1BatchWasteEvidence result;
  std::set<B1JobInstance> jobs;
  for (const M1BatchSourceEvidence& source : sources) {
    validate_b1_job_instance(source.job);
    if (!jobs.insert(source.job).second) {
      throw std::invalid_argument("M1 B1 source identity is duplicated.");
    }
    const M1BatchSourceReplay replay = replay_m1_batch_source(source);
    if (!replay.structurally_valid) {
      throw std::invalid_argument("M1 B1 source replay is malformed or lossy.");
    }
    if (replay.verified_endpoint != source.verified_endpoint) {
      throw std::invalid_argument(
          "M1 B1 source verified-endpoint projection drifted.");
    }
    if (source.job.phase != B1JobPhase::Measured) {
      continue;
    }
    if (!checked_accumulate(&result.all_started_service,
                            replay.all_started_service) ||
        !checked_accumulate(&result.discarded_started_service,
                            replay.discarded_started_service) ||
        !checked_accumulate(&result.post_cancellation_started_service,
                            replay.post_cancellation_started_service)) {
      throw std::overflow_error("M1 B1 source service aggregate overflowed.");
    }
    if (result.duplicate_service_starts >
            std::numeric_limits<std::size_t>::max() -
                replay.duplicate_service_starts ||
        result.retry_service_starts > std::numeric_limits<std::size_t>::max() -
                                          replay.retry_service_starts) {
      throw std::overflow_error("M1 B1 source count aggregate overflowed.");
    }
    result.duplicate_service_starts += replay.duplicate_service_starts;
    result.retry_service_starts += replay.retry_service_starts;
  }
  return result;
}

/** @copydoc evaluate_m1_inner_row */
M1InnerRow evaluate_m1_inner_row(M1InnerRowInput input) {
  M1InnerRow row;
  row.evidence = std::move(input);

  bool fairness_sources_valid = true;
  try {
    const M1SourceFairnessProjection projection =
        derive_m1_source_fairness_projection(row.evidence.protocol,
                                             row.evidence.interactive_sources,
                                             row.evidence.batch_sources);
    if (!same_m1_first_measured_admission(
            projection.first_measured_admission,
            row.evidence.protocol.first_measured_admission)) {
      fairness_sources_valid = false;
      invalidate_m1(
          &row.validity_reasons,
          "M1 first admission/current hold differs from retained raw sources");
    }
    const std::size_t final_warmup_index =
        kM1ColdI1OriginCount + kM1WarmupI1OriginCount - 1U;
    const M1InteractiveOccurrenceEvidence& final_warmup =
        row.evidence.protocol.interactive_occurrences[final_warmup_index];
    if (final_warmup.publication_current_at_measurement !=
            projection.first_measured_admission
                .warmup_publication_current_before_acceptance ||
        final_warmup.settlement_pending_at_measurement !=
            projection.final_warmup_settlement_pending_at_measurement) {
      fairness_sources_valid = false;
      invalidate_m1(
          &row.validity_reasons,
          "M1 final-warmup current hold differs from retained raw sources");
    }
    if (!same_m1_progress_windows(projection.progress_windows,
                                  row.evidence.fairness.progress_windows)) {
      fairness_sources_valid = false;
      invalidate_m1(&row.validity_reasons,
                    "M1 progress projection differs from retained raw sources");
    }
    if (!same_m1_graph_windows(projection.graph_service_windows,
                               row.evidence.fairness.graph_service_windows)) {
      fairness_sources_valid = false;
      invalidate_m1(&row.validity_reasons,
                    "M1 Graph projection differs from retained raw sources");
    }
    if (!same_m1_headroom_outcomes(projection.headroom_outcomes,
                                   row.evidence.fairness.headroom_outcomes)) {
      fairness_sources_valid = false;
      invalidate_m1(
          &row.validity_reasons,
          "M1 headroom outcome projection differs from retained raw sources");
    }
    if (!same_m1_headroom_admissions(
            projection.headroom_admissions,
            row.evidence.fairness.headroom_admissions)) {
      fairness_sources_valid = false;
      invalidate_m1(&row.validity_reasons,
                    "M1 headroom aggregate differs from retained raw sources");
    }
  } catch (const std::exception& error) {
    fairness_sources_valid = false;
    invalidate_m1(&row.validity_reasons,
                  std::string("M1 source-derived admission/fairness replay "
                              "failed closed: ") +
                      error.what());
  }

  bool batch_waste_valid = true;
  try {
    const M1BatchWasteEvidence replayed =
        derive_m1_batch_waste_evidence(row.evidence.batch_sources);
    if (!same_m1_batch_waste(replayed, row.evidence.batch_waste)) {
      batch_waste_valid = false;
      invalidate_m1(&row.validity_reasons,
                    "M1 B1 waste projection differs from retained raw sources");
    }
  } catch (const std::exception&) {
    batch_waste_valid = false;
    invalidate_m1(&row.validity_reasons, "M1 B1 source replay failed closed");
  }
  row.source_evidence_closed = fairness_sources_valid && batch_waste_valid;

  row.protocol = evaluate_m1_protocol(row.evidence.protocol);
  for (const std::string& reason : row.protocol.validity_reasons) {
    invalidate_m1(&row.validity_reasons, reason);
  }

  if (row.evidence.replicate_ordinal == 0U ||
      row.evidence.replicate_ordinal > 3U ||
      row.evidence.protocol.replicate_ordinal !=
          row.evidence.replicate_ordinal) {
    invalidate_m1(&row.validity_reasons,
                  "M1 inner row replicate identity drifted");
  }
  const bool protocol_valid =
      row.protocol.verdict == I1Verdict::Pass && row.source_evidence_closed &&
      row.evidence.replicate_ordinal != 0U &&
      row.evidence.replicate_ordinal <= 3U &&
      row.evidence.protocol.replicate_ordinal == row.evidence.replicate_ordinal;
  const M1MemoryValidation memory = validate_m1_memory(
      row.evidence.temporal_snapshots, row.evidence.protocol.batch_offers,
      row.evidence.batch_sources, &row);
  if (!protocol_valid) {
    return row;
  }

  std::vector<std::chrono::nanoseconds> latency_samples;
  bool every_latency_complete = true;
  bool every_output_passed = true;
  bool every_interactive_waste_complete = true;
  bool interactive_sum_valid = true;
  for (const M1InteractiveOccurrenceEvidence& occurrence :
       row.evidence.protocol.interactive_occurrences) {
    if (occurrence.phase != B1JobPhase::Measured) {
      continue;
    }
    if (!occurrence.final_latency.has_value()) {
      every_latency_complete = false;
    } else {
      latency_samples.push_back(*occurrence.final_latency);
    }
    every_latency_complete = every_latency_complete &&
                             occurrence.latency_verdict != I1Verdict::Invalid;
    every_output_passed =
        every_output_passed && occurrence.output_verdict == I1Verdict::Pass;
    every_interactive_waste_complete =
        every_interactive_waste_complete &&
        occurrence.waste_verdict != I1Verdict::Invalid;
    interactive_sum_valid =
        interactive_sum_valid &&
        checked_accumulate(&row.interactive_all_started_service,
                           occurrence.service.all_started_service) &&
        checked_accumulate(&row.interactive_discarded_started_service,
                           occurrence.service.discarded_started_service) &&
        checked_accumulate(&row.interactive_post_cancellation_started_service,
                           occurrence.service.post_cancel_started_service);
  }

  bool latency_valid = every_latency_complete &&
                       latency_samples.size() == kM1MeasuredI1OriginCount &&
                       row.evidence.paired_isolated_i1_p99.has_value() &&
                       row.evidence.paired_isolated_i1_p99->count() > 0 &&
                       row.evidence.occurrence_attribution_proved;
  if (latency_valid) {
    row.latency =
        I1LatencyPercentiles{i1_nearest_rank(latency_samples, 50U, 100U),
                             i1_nearest_rank(latency_samples, 95U, 100U),
                             i1_nearest_rank(latency_samples, 99U, 100U)};
    row.relative_latency_p99 =
        static_cast<double>(row.latency->p99.count()) /
        static_cast<double>(row.evidence.paired_isolated_i1_p99->count());
    const bool every_episode_passed =
        std::all_of(row.evidence.protocol.interactive_occurrences.begin(),
                    row.evidence.protocol.interactive_occurrences.end(),
                    [](const M1InteractiveOccurrenceEvidence& occurrence) {
                      return occurrence.phase != B1JobPhase::Measured ||
                             occurrence.latency_verdict == I1Verdict::Pass;
                    });
    row.latency_verdict =
        every_episode_passed && every_output_passed &&
                row.latency->p50 <= kI1LatencyP50Limit &&
                row.latency->p95 <= kI1LatencyP95Limit &&
                row.latency->p99 <= kI1LatencyP99Limit &&
                std::isfinite(*row.relative_latency_p99) &&
                *row.relative_latency_p99 <= kM1RelativeLatencyP99Limit
            ? I1Verdict::Pass
            : I1Verdict::Fail;
  } else {
    invalidate_m1(&row.validity_reasons,
                  "M1 latency evidence or paired isolated p99 is incomplete");
  }

  M1FairnessEvidenceInput fairness_input = row.evidence.fairness;
  fairness_input.interactive_latency_verdict = row.latency_verdict;
  row.fairness = evaluate_m1_fairness(std::move(fairness_input));
  row.throughput_progress_verdict = row.fairness.throughput_progress_verdict;
  row.fairness_verdict = row.fairness.composite_fairness_verdict;
  for (const std::string& reason : row.fairness.validity_reasons) {
    invalidate_m1(&row.validity_reasons, "M1 fairness evidence: " + reason);
  }
  if (!row.evidence.occurrence_attribution_proved) {
    row.throughput_progress_verdict = I1Verdict::Invalid;
    row.fairness_verdict = I1Verdict::Invalid;
    invalidate_m1(&row.validity_reasons,
                  "M1 occurrence-owned phase attribution is unproved");
  }
  if (!row.evidence.temporal_effects_complete) {
    row.fairness_verdict = I1Verdict::Invalid;
    invalidate_m1(&row.validity_reasons,
                  "M1 measured-window carryover physical effects are missing");
  }

  const M1BatchWasteEvidence& batch = row.evidence.batch_waste;
  bool waste_valid =
      every_interactive_waste_complete && interactive_sum_valid &&
      row.evidence.occurrence_attribution_proved &&
      row.interactive_all_started_service > 0U &&
      row.interactive_discarded_started_service <=
          row.interactive_all_started_service &&
      batch.all_started_service > 0U &&
      batch.discarded_started_service <= batch.all_started_service;
  if (waste_valid) {
    row.interactive_discarded_ratio =
        static_cast<double>(row.interactive_discarded_started_service) /
        static_cast<double>(row.interactive_all_started_service);
    const bool each_interactive_passed =
        std::all_of(row.evidence.protocol.interactive_occurrences.begin(),
                    row.evidence.protocol.interactive_occurrences.end(),
                    [](const M1InteractiveOccurrenceEvidence& occurrence) {
                      return occurrence.phase != B1JobPhase::Measured ||
                             occurrence.waste_verdict == I1Verdict::Pass;
                    });
    row.waste_verdict =
        each_interactive_passed &&
                *row.interactive_discarded_ratio <=
                    kI1DiscardedServiceRatioLimit &&
                row.interactive_post_cancellation_started_service == 0U &&
                batch.discarded_started_service == 0U &&
                batch.post_cancellation_started_service == 0U &&
                batch.duplicate_service_starts == 0U &&
                batch.retry_service_starts == 0U
            ? I1Verdict::Pass
            : I1Verdict::Fail;
  } else {
    invalidate_m1(&row.validity_reasons,
                  "M1 measured Interactive/B1 waste evidence is incomplete");
  }

  if (memory.valid && row.evidence.temporal_effects_complete) {
    row.memory_verdict = memory.within_limits && memory.settled
                             ? I1Verdict::Pass
                             : I1Verdict::Fail;
  } else if (!row.evidence.temporal_effects_complete) {
    invalidate_m1(&row.validity_reasons,
                  "M1 temporal resource evidence is incomplete");
  }

  row.overall_verdict = compose_m1_row(
      {row.latency_verdict, row.throughput_progress_verdict,
       row.fairness_verdict, row.waste_verdict, row.memory_verdict},
      &row.validity_reasons);
  return row;
}

}  // namespace ps::benchmark
