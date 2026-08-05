/**
 * @file i2_evidence.cpp
 * @brief Implements fail-closed I2 inner-row and replicate evaluation.
 */
#include "benchmark/i2_evidence.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ps::benchmark {
namespace {

/** @brief Stable no-Metal reason owned by the closed I2 inner schema. */
constexpr char kNoMetalReason[] =
    "not-applicable: process Metal executor unavailable";  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Adds one structural invalidation reason without suppressing others.
 * @param reasons Destination diagnostic sequence.
 * @param reason Stable human-readable reason.
 * @return Nothing.
 * @throws std::bad_alloc when diagnostic ownership allocates.
 */
void invalidate_i2(std::vector<std::string>* reasons, std::string reason) {
  reasons->push_back(std::move(reason));
}

/**
 * @brief Checked-adds one service charge.
 * @param total In/out exact aggregate.
 * @param charge Nonnegative callback charge.
 * @return True after exact addition; false without mutation on overflow.
 * @throws Nothing.
 */
bool checked_add_i2_service(std::uint64_t* total,
                            std::uint64_t charge) noexcept {
  if (charge > std::numeric_limits<std::uint64_t>::max() - *total) {
    return false;
  }
  *total += charge;
  return true;
}

/**
 * @brief Computes one optional discarded-service ratio.
 * @param service In/out complete integer service sums.
 * @return Nothing after setting or clearing the ratio.
 * @throws Nothing.
 */
void set_i2_service_ratio(I1ServiceEvidence* service) noexcept {
  if (service->all_started_service == 0U) {
    service->discarded_ratio.reset();
    return;
  }
  service->discarded_ratio =
      static_cast<double>(service->discarded_started_service) /
      static_cast<double>(service->all_started_service);
}

/**
 * @brief Tests complete equality of compute-I/O observational snapshots.
 * @param left First snapshot.
 * @param right Second snapshot.
 * @return True only when every field is equal.
 * @throws Nothing.
 */
bool compute_io_equal(
    const execution::ComputeIoExecutorSnapshot& left,
    const execution::ComputeIoExecutorSnapshot& right) noexcept {
  return left.task_limit == right.task_limit &&
         left.planned_bytes_limit == right.planned_bytes_limit &&
         left.active_tasks == right.active_tasks &&
         left.active_planned_bytes == right.active_planned_bytes &&
         left.constructing_tasks == right.constructing_tasks &&
         left.queued_tasks == right.queued_tasks &&
         left.running_tasks == right.running_tasks &&
         left.accepting == right.accepting &&
         left.shutdown_complete == right.shutdown_complete;
}

/**
 * @brief Tests complete equality of Metal executor diagnostics.
 * @param left First diagnostic cut.
 * @param right Second diagnostic cut.
 * @return True only when every field is equal.
 * @throws Nothing.
 */
bool device_diagnostics_equal(
    const execution::DeviceExecutorDiagnostics& left,
    const execution::DeviceExecutorDiagnostics& right) noexcept {
  return left.device == right.device && left.queue_ready == right.queue_ready &&
         left.submission_count == right.submission_count &&
         left.invocation_count == right.invocation_count &&
         left.total_allocations == right.total_allocations &&
         left.live_allocations == right.live_allocations &&
         left.pipeline_cache_entries == right.pipeline_cache_entries;
}

/**
 * @brief Tests complete equality of device resource snapshots.
 * @param left First device snapshot.
 * @param right Second device snapshot.
 * @return True only when identity, limits, ownership, peaks, and availability
 * match.
 * @throws Nothing.
 */
bool device_snapshot_equal(
    const ResourceLedger::DeviceSnapshot& left,
    const ResourceLedger::DeviceSnapshot& right) noexcept {
  return left.device == right.device && left.limits == right.limits &&
         left.reserved == right.reserved &&
         left.high_water == right.high_water &&
         left.available == right.available;
}

/**
 * @brief Reports whether Host resource components fit their configured limits.
 * @param value Complete resource vector.
 * @param limits Complete corresponding limits.
 * @return True when every component is no greater than its limit.
 * @throws Nothing.
 */
bool host_resources_within(const ResourceVector& value,
                           const ResourceVector& limits) noexcept {
  return value.cpu_slots <= limits.cpu_slots &&
         value.retained_memory_bytes <= limits.retained_memory_bytes &&
         value.scratch_bytes <= limits.scratch_bytes &&
         value.ready_entries <= limits.ready_entries &&
         value.ready_bytes <= limits.ready_bytes;
}

/**
 * @brief Reports whether device byte components fit configured limits.
 * @param value Device byte vector.
 * @param limits Corresponding device limits.
 * @return True when memory and scratch are no greater than limits.
 * @throws Nothing.
 */
bool device_resources_within(const DeviceResourceVector& value,
                             const DeviceResourceVector& limits) noexcept {
  return value.device_memory_bytes <= limits.device_memory_bytes &&
         value.device_scratch_bytes <= limits.device_scratch_bytes;
}

/**
 * @brief Tests component-wise nondecrease of Host lifetime high-water values.
 * @param lower Earlier values.
 * @param upper Later values.
 * @return True when no component decreases.
 * @throws Nothing.
 */
bool host_resources_not_less(const ResourceVector& lower,
                             const ResourceVector& upper) noexcept {
  return lower.cpu_slots <= upper.cpu_slots &&
         lower.retained_memory_bytes <= upper.retained_memory_bytes &&
         lower.scratch_bytes <= upper.scratch_bytes &&
         lower.ready_entries <= upper.ready_entries &&
         lower.ready_bytes <= upper.ready_bytes;
}

/**
 * @brief Tests component-wise nondecrease of device high-water values.
 * @param lower Earlier values.
 * @param upper Later values.
 * @return True when neither byte peak decreases.
 * @throws Nothing.
 */
bool device_resources_not_less(const DeviceResourceVector& lower,
                               const DeviceResourceVector& upper) noexcept {
  return lower.device_memory_bytes <= upper.device_memory_bytes &&
         lower.device_scratch_bytes <= upper.device_scratch_bytes;
}

/**
 * @brief Compares every lifecycle counter exactly.
 * @param left First counter cut.
 * @param right Second counter cut.
 * @return True only when every maintained counter matches.
 * @throws Nothing.
 */
bool lifecycle_equal(
    const compute::ExecutionLifecycleCounters& left,
    const compute::ExecutionLifecycleCounters& right) noexcept {
  return left.registered_graph_count == right.registered_graph_count &&
         left.open_graph_count == right.open_graph_count &&
         left.closing_graph_count == right.closing_graph_count &&
         left.pending_candidate_count == right.pending_candidate_count &&
         left.admitted_standalone_run_count ==
             right.admitted_standalone_run_count &&
         left.admitted_run_group_count == right.admitted_run_group_count &&
         left.admitted_child_run_count == right.admitted_child_run_count &&
         left.terminal_not_quiescent_run_count ==
             right.terminal_not_quiescent_run_count &&
         left.finalizing_run_count == right.finalizing_run_count &&
         left.ready_entry_count == right.ready_entry_count &&
         left.entered_callback_count == right.entered_callback_count &&
         left.live_root_reservation_count ==
             right.live_root_reservation_count &&
         left.live_child_grant_count == right.live_child_grant_count &&
         left.live_policy_invocation_count ==
             right.live_policy_invocation_count &&
         left.live_policy_binding_count == right.live_policy_binding_count;
}

/**
 * @brief Tests whether all episode-active lifecycle counters settled.
 * @param counters Complete process lifecycle cut.
 * @return True when reusable graph/policy bindings are the only live state.
 * @throws Nothing.
 */
bool lifecycle_work_settled(
    const compute::ExecutionLifecycleCounters& counters) noexcept {
  return counters.closing_graph_count == 0U &&
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
         counters.live_policy_invocation_count == 0U;
}

/**
 * @brief Reserves one nonzero unique observer sequence.
 * @param sequence Candidate shared causal sequence.
 * @param seen Episode-wide set.
 * @return True only for a new nonzero sequence.
 * @throws std::bad_alloc when set storage allocates.
 */
bool reserve_i2_sequence(std::uint64_t sequence,
                         std::set<std::uint64_t>* seen) {
  return sequence != 0U && seen->insert(sequence).second;
}

/**
 * @brief Tests whether one event belongs to the frozen observation cut.
 * @param observed_at Product-boundary sample.
 * @param sequence Shared causal sequence.
 * @param cut First excluded sequence plus actual boundary sample.
 * @return True only when time and causal sequence precede the cut.
 * @throws Nothing.
 */
bool i2_event_within_cut(std::chrono::steady_clock::time_point observed_at,
                         std::uint64_t sequence,
                         const I1ObservationHistoryCut& cut) noexcept {
  return observed_at <= cut.captured_at && sequence < cut.causal_sequence;
}

/**
 * @brief Compares every authority-free child descriptor fact exactly.
 * @param left First copied child descriptor.
 * @param right Second copied child descriptor.
 * @return True only when identity, lineage, intent, quality, and QoS match.
 * @throws Nothing.
 */
bool i2_child_equal(const I2ObservedChildDescriptor& left,
                    const I2ObservedChildDescriptor& right) noexcept {
  return left.edit_index == right.edit_index && left.run_id == right.run_id &&
         left.graph_instance_id == right.graph_instance_id &&
         left.graph_revision == right.graph_revision &&
         left.target_node_id == right.target_node_id &&
         left.child_intent == right.child_intent &&
         left.quality == right.quality &&
         left.qos.service_class == right.qos.service_class &&
         left.qos.deadline == right.qos.deadline &&
         left.qos.weight == right.qos.weight &&
         left.qos.maximum_parallelism == right.qos.maximum_parallelism &&
         left.generation == right.generation &&
         left.request_intent == right.request_intent &&
         left.accepted_coordinate == right.accepted_coordinate;
}

/**
 * @brief Returns the sole current-generation record for one edit.
 * @param observations Complete episode observations.
 * @param edit_index Frozen edit identity.
 * @return Matching record only when exactly one exists.
 * @throws Nothing.
 */
std::optional<I1ObservedCurrentGeneration> single_i2_generation(
    const I2EpisodeObservationSnapshot& observations,
    std::size_t edit_index) noexcept {
  std::optional<I1ObservedCurrentGeneration> result;
  for (const I1ObservedCurrentGeneration& generation :
       observations.current_generations) {
    if (generation.edit_index != edit_index) {
      continue;
    }
    if (result.has_value()) {
      return std::nullopt;
    }
    result = generation;
  }
  return result;
}

/**
 * @brief Finds the earliest accepted cancellation sequence for one Run.
 * @param observations Complete child-aware episode observations.
 * @param run_id Opaque materialized Run identity.
 * @return Earliest matching sequence, or absent when not cancelled.
 * @throws Nothing.
 */
std::optional<std::uint64_t> i2_cancellation_sequence(
    const I2EpisodeObservationSnapshot& observations,
    std::uint64_t run_id) noexcept {
  std::optional<std::uint64_t> result;
  for (const I2ObservedCancellation& cancellation :
       observations.cancellations) {
    if (cancellation.child.run_id != run_id) {
      continue;
    }
    if (!result.has_value() || cancellation.causal_sequence < *result) {
      result = cancellation.causal_sequence;
    }
  }
  return result;
}

/**
 * @brief Tests closure of repeated allocation and byte facts in one access.
 * @param access Serialized access evidence to inspect without payload access.
 * @return True only when the repeated fields exactly name the binding.
 * @throws Nothing.
 * @note This helper checks closed evidence identity only; plan semantics and
 * cross-acquisition reuse remain independently validated by the caller.
 */
bool i2_access_binding_closed(const I2ValueAccessEvidence& access) noexcept {
  return access.binding.allocation == access.allocation &&
         access.binding.byte_size == access.storage_bytes;
}

/**
 * @brief Validates one explicit Host/conditional-Metal access record.
 * @param access Closed acquisition evidence.
 * @param revision Exact visible Value revision.
 * @param reasons Destination fail-closed reasons.
 * @return True when direct reuse, optional transfer/reuse, and zero I/O hold.
 * @throws std::bad_alloc when reasons allocate.
 */
bool validate_i2_acquisition(const I2ValueAcquisitionEvidence& access,
                             ValueRevisionId revision,
                             std::vector<std::string>* reasons) {
  bool valid = true;
  const auto fail = [&](const char* reason) {
    invalidate_i2(reasons, reason);
    valid = false;
  };
  if (!access.host_first.plan.has_value() ||
      !access.host_second.plan.has_value() ||
      access.host_first.plan->kind() != AccessPlanKind::Direct ||
      access.host_second.plan->kind() != AccessPlanKind::Direct ||
      access.host_first.plan->transfer_bytes() != 0U ||
      access.host_second.plan->transfer_bytes() != 0U ||
      access.host_first.executor_submitted ||
      access.host_second.executor_submitted ||
      access.host_first.revision != revision ||
      access.host_second.revision != revision ||
      access.host_first.binding != access.host_second.binding ||
      access.host_first.allocation != access.host_second.allocation ||
      access.host_first.storage_bytes != access.host_second.storage_bytes ||
      !i2_access_binding_closed(access.host_first) ||
      !i2_access_binding_closed(access.host_second) ||
      !access.host_first.binding.host_visible ||
      access.host_first.binding.device != DeviceId(DeviceBackend::CPU) ||
      access.host_first.binding.memory_domain != MemoryDomain::Host) {
    fail("I2 Host acquisitions are not exact Direct binding reuse");
  }
  if ((access.host_first.plan.has_value() &&
       (access.host_first.plan->source_revision() != revision.value() ||
        access.host_first.plan->source_binding() !=
            access.host_first.binding)) ||
      (access.host_second.plan.has_value() &&
       (access.host_second.plan->source_revision() != revision.value() ||
        access.host_second.plan->source_binding() !=
            access.host_second.binding))) {
    fail("I2 Host access plans do not name the captured Value binding");
  }
  if (!compute_io_equal(access.io_before, access.io_after) ||
      access.io_before.active_tasks != 0U ||
      access.io_before.active_planned_bytes != 0U ||
      access.io_before.constructing_tasks != 0U ||
      access.io_before.queued_tasks != 0U ||
      access.io_before.running_tasks != 0U) {
    fail("I2 acquisition changed or entered ComputeIoExecutor");
  }
  if (!access.metal.available) {
    if (access.metal.unavailable_reason != kNoMetalReason ||
        access.metal.before.has_value() || access.metal.first.has_value() ||
        access.metal.after_first.has_value() ||
        access.metal.second.has_value() ||
        access.metal.after_second.has_value() ||
        access.metal.resources_before.has_value() ||
        access.metal.resources_after_first.has_value() ||
        access.metal.resources_after_second.has_value()) {
      fail("I2 unavailable Metal evidence is not the frozen N/A shape");
    }
    return valid;
  }
  if (!access.metal.before.has_value() || !access.metal.first.has_value() ||
      !access.metal.after_first.has_value() ||
      !access.metal.second.has_value() ||
      !access.metal.after_second.has_value() ||
      !access.metal.resources_before.has_value() ||
      !access.metal.resources_after_first.has_value() ||
      !access.metal.resources_after_second.has_value()) {
    fail("I2 available Metal evidence is incomplete");
    return valid;
  }
  const I2ValueAccessEvidence& first = *access.metal.first;
  const I2ValueAccessEvidence& second = *access.metal.second;
  if (!first.allocation.valid() || first.binding != second.binding ||
      first.allocation != second.allocation ||
      first.storage_bytes != second.storage_bytes ||
      !i2_access_binding_closed(first) || !i2_access_binding_closed(second)) {
    fail("I2 Metal binding/allocation/storage-byte facts are not exact reuse");
  }
  if (!first.plan.has_value() || !second.plan.has_value() ||
      first.plan->kind() != AccessPlanKind::Transfer ||
      first.plan->transfer_bytes() != access.host_first.storage_bytes ||
      second.plan->kind() != AccessPlanKind::Direct ||
      second.plan->transfer_bytes() != 0U || !first.executor_submitted ||
      second.executor_submitted || first.revision != revision ||
      second.revision != revision ||
      first.binding == access.host_first.binding ||
      first.allocation == access.host_first.allocation ||
      first.storage_bytes != access.host_first.storage_bytes ||
      first.binding.device != DeviceId(DeviceBackend::Metal) ||
      first.binding.memory_domain != MemoryDomain::DeviceLocal ||
      first.binding.host_visible) {
    fail("I2 Metal first-transfer/second-reuse facts are inconsistent");
  }
  if ((first.plan.has_value() &&
       (first.plan->source_revision() != revision.value() ||
        first.plan->source_binding() != access.host_first.binding)) ||
      (second.plan.has_value() &&
       (second.plan->source_revision() != revision.value() ||
        second.plan->source_binding() != second.binding))) {
    fail("I2 Metal access plans do not name their exact source bindings");
  }
  const execution::DeviceExecutorDiagnostics& before = *access.metal.before;
  const execution::DeviceExecutorDiagnostics& after_first =
      *access.metal.after_first;
  const execution::DeviceExecutorDiagnostics& after_second =
      *access.metal.after_second;
  if (before.device != Device::GPU_METAL || !before.queue_ready ||
      after_first.submission_count != before.submission_count + 1U ||
      after_first.invocation_count != before.invocation_count + 1U ||
      after_first.total_allocations < before.total_allocations + 2U ||
      after_first.live_allocations != 0U ||
      !device_diagnostics_equal(after_first, after_second)) {
    fail("I2 Metal executor counters do not prove one transfer then reuse");
  }
  if (access.metal.resources_after_first->reserved.device_scratch_bytes != 0U ||
      !device_snapshot_equal(*access.metal.resources_after_first,
                             *access.metal.resources_after_second)) {
    fail("I2 Metal resource evidence did not settle scratch before reuse");
  }
  return valid;
}

/**
 * @brief Accumulates row verdicts into fail/invalid flags.
 * @param verdict Row verdict.
 * @param saw_fail In/out failure flag.
 * @param saw_invalid In/out invalidity flag.
 * @return Nothing.
 * @throws Nothing.
 */
void accumulate_i2_verdict(I1Verdict verdict, bool* saw_fail,
                           bool* saw_invalid) noexcept {
  if (verdict == I1Verdict::Fail) {
    *saw_fail = true;
  } else if (verdict == I1Verdict::Invalid) {
    *saw_invalid = true;
  }
}

}  // namespace

/** @copydoc capture_i2_edit_evidence */
I2EditEvidence capture_i2_edit_evidence(
    const I2EditAdmissionResult& admission,
    std::optional<OperationStatus> settlement) {
  if (admission.edit_index >= kI1EditCount) {
    throw std::out_of_range("I2 evidence edit index is outside [0,11].");
  }
  return I2EditEvidence{admission.edit_index,
                        kI1EditCoefficients[admission.edit_index],
                        i1_edit_region(admission.edit_index),
                        i2_preview_region(admission.edit_index),
                        admission.nominal_time,
                        admission.admission_attempted,
                        admission.admission_sample,
                        admission.admission_window_valid,
                        admission.reserved_event_sequence,
                        admission.preview_deadline,
                        admission.final_deadline,
                        admission.host_return,
                        admission.accepted_coordinate,
                        std::move(settlement)};
}

/** @copydoc evaluate_i2_episode */
I2EpisodeInnerRow evaluate_i2_episode(I2EpisodeEvidenceInput input) {
  I2EpisodeInnerRow row;
  row.evidence = std::move(input);
  std::vector<std::string>& reasons = row.validity_reasons;
  bool structure_valid = true;
  bool output_complete = true;
  bool output_matches = true;

  const auto structural_failure = [&](const char* reason) {
    invalidate_i2(&reasons, reason);
    structure_valid = false;
  };
  if (row.evidence.replicate_ordinal < 1U ||
      row.evidence.replicate_ordinal > 3U) {
    structural_failure("I2 replicate ordinal is outside [1,3]");
  }
  if (row.evidence.slot >= kI2GridSlotCount) {
    structural_failure("I2 slot is outside [0,110]");
  } else {
    try {
      if (row.evidence.episode_origin !=
              i2_episode_origin(row.evidence.grid_origin, row.evidence.slot) ||
          row.evidence.terminal_boundary !=
              i2_terminal_boundary(row.evidence.grid_origin)) {
        structural_failure("I2 grid origins do not match the frozen formulas");
      }
    } catch (const std::exception&) {
      structural_failure("I2 grid arithmetic could not be recomputed");
    }
  }
  std::optional<std::chrono::steady_clock::time_point> episode_end;
  try {
    episode_end =
        checked_i1_time_add(row.evidence.episode_origin, kI2EpisodeStride);
  } catch (const std::exception&) {
    structural_failure("I2 episode terminal-guard arithmetic overflowed");
  }
  if (row.evidence.observation_cut.causal_sequence == 0U ||
      !episode_end.has_value() ||
      row.evidence.observation_cut.captured_at >= *episode_end ||
      row.evidence.final_snapshot_sample <
          row.evidence.observation_cut.captured_at ||
      row.evidence.final_snapshot_sample >= *episode_end ||
      row.evidence.final_snapshot_sample > row.evidence.terminal_boundary) {
    structural_failure(
        "I2 observation/final snapshot lies outside the episode guard");
  }
  if (row.evidence.observations.overflowed) {
    structural_failure("I2 bounded observation collector overflowed");
  }

  std::set<std::uint64_t> accepted_sequences;
  std::set<std::uint64_t> generations;
  std::array<std::optional<I1ObservedCurrentGeneration>, kI1EditCount>
      current_generation_events;
  std::uint64_t previous_accepted_sequence = 0U;
  for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
    const I2EditEvidence& edit = row.evidence.edits[edit_index];
    bool edit_valid = true;
    try {
      edit_valid =
          edit.edit_index == edit_index &&
          edit.coefficient == kI1EditCoefficients[edit_index] &&
          edit.source_region == i1_edit_region(edit_index) &&
          edit.preview_region == i2_preview_region(edit_index) &&
          edit.nominal_time ==
              checked_i1_time_add(
                  row.evidence.episode_origin,
                  kI1EditStride * static_cast<std::int64_t>(edit_index));
    } catch (const std::exception&) {
      edit_valid = false;
    }
    try {
      if (!edit.admission_attempted || !edit.admission_window_valid ||
          edit.admission_sample < edit.nominal_time ||
          edit.admission_sample >
              checked_i1_time_add(edit.nominal_time, kI1AdmissionLateness) ||
          !edit.reserved_event_sequence.has_value() ||
          *edit.reserved_event_sequence == 0U ||
          *edit.reserved_event_sequence <= previous_accepted_sequence ||
          !accepted_sequences.insert(*edit.reserved_event_sequence).second ||
          !edit.preview_deadline.has_value() ||
          !edit.final_deadline.has_value() ||
          *edit.preview_deadline !=
              checked_i1_time_add(edit.admission_sample,
                                  kI2PreviewDeadlineBudget) ||
          *edit.final_deadline != checked_i1_time_add(edit.admission_sample,
                                                      kI2FinalDeadlineBudget) ||
          !edit.host_return.has_value() || !edit.host_return->status.ok ||
          !edit.host_return->future_valid ||
          edit.host_return->return_time < edit.admission_sample ||
          !edit.accepted_coordinate.has_value() ||
          edit.accepted_coordinate->admission_time() != edit.admission_sample ||
          edit.accepted_coordinate->event_sequence() !=
              *edit.reserved_event_sequence ||
          !edit.settlement_status.has_value()) {
        edit_valid = false;
      }
    } catch (const std::exception&) {
      edit_valid = false;
    }
    if (edit.reserved_event_sequence.has_value() &&
        *edit.reserved_event_sequence > previous_accepted_sequence) {
      previous_accepted_sequence = *edit.reserved_event_sequence;
    }
    const auto generation =
        single_i2_generation(row.evidence.observations, edit_index);
    if (!generation.has_value() || generation->generation == 0U ||
        !generations.insert(generation->generation).second ||
        generation->observed_at < edit.admission_sample ||
        !generation->accepted_coordinate.has_value() ||
        !edit.accepted_coordinate.has_value() ||
        !(generation->accepted_coordinate == edit.accepted_coordinate)) {
      edit_valid = false;
    } else {
      current_generation_events[edit_index] = generation;
      row.accepted_products[edit_index] = I2AcceptedProductIdentity{
          generation->generation, generation->accepted_coordinate, std::nullopt,
          std::nullopt};
    }
    if (!edit_valid) {
      structural_failure(
          "I2 edit admission/current-generation evidence is malformed");
    }
  }

  std::set<std::uint64_t> seen_sequences;
  const auto check_event = [&](std::chrono::steady_clock::time_point time,
                               std::uint64_t sequence) {
    if (!reserve_i2_sequence(sequence, &seen_sequences) ||
        !i2_event_within_cut(time, sequence, row.evidence.observation_cut)) {
      structural_failure("I2 observation sequence is duplicate or outside cut");
    }
  };
  /**
   * @brief Validates one child event against the cut and generation ordering.
   * @param child Descriptor identifying the accepted edit and child Run.
   * @param time Steady-clock observation time carried by the event.
   * @param sequence Request-scoped causal sequence carried by the event.
   * @return Nothing.
   * @throws std::bad_alloc when sequence or validity-reason storage allocates.
   * @note Malformed evidence is recorded as a structural failure; this helper
   * does not grant authority to alter the observed product lifecycle.
   */
  const auto check_child_event = [&](const I2ObservedChildDescriptor& child,
                                     std::chrono::steady_clock::time_point time,
                                     std::uint64_t sequence) {
    check_event(time, sequence);
    if (child.edit_index >= kI1EditCount ||
        !current_generation_events[child.edit_index].has_value() ||
        current_generation_events[child.edit_index]->causal_sequence >=
            sequence) {
      structural_failure(
          "I2 child event does not follow its current generation");
    }
  };
  for (const I1ObservedCurrentGeneration& event :
       row.evidence.observations.current_generations) {
    check_event(event.observed_at, event.causal_sequence);
  }
  for (const I2ObservedServiceStart& event :
       row.evidence.observations.service_starts) {
    check_child_event(event.child, event.observed_at, event.causal_sequence);
  }
  for (const I2ObservedCancellation& event :
       row.evidence.observations.cancellations) {
    check_child_event(event.child, event.observed_at, event.causal_sequence);
  }
  for (const I2ObservedTerminal& event : row.evidence.observations.terminals) {
    check_child_event(event.child, event.observed_at, event.causal_sequence);
  }
  for (const I2ObservedFinalTrigger& event :
       row.evidence.observations.final_triggers) {
    check_child_event(event.child, event.observed_at, event.causal_sequence);
  }
  for (const I2ObservedVisibleOutput& event :
       row.evidence.observations.visible_outputs) {
    check_child_event(event.child, event.observed_at, event.causal_sequence);
  }
  for (const I2ObservedRunLifecycleTransition& event :
       row.evidence.observations.run_quiescences) {
    check_child_event(event.child, event.observed_at, event.causal_sequence);
  }
  for (const I2ObservedRunLifecycleTransition& event :
       row.evidence.observations.resource_settlements) {
    check_child_event(event.child, event.observed_at, event.causal_sequence);
  }
  for (const I1ObservedHostSettlement& event :
       row.evidence.observations.host_settlements) {
    check_event(event.observed_at, event.causal_sequence);
  }

  using RunKey = std::pair<std::size_t, std::uint64_t>;
  std::map<RunKey, I2ObservedTerminal> terminals;
  std::set<std::uint64_t> global_run_ids;
  std::array<std::size_t, kI1EditCount> terminal_counts{};
  for (const I2ObservedTerminal& terminal :
       row.evidence.observations.terminals) {
    const I2ObservedChildDescriptor& child = terminal.child;
    bool valid_child =
        child.edit_index < kI1EditCount && child.run_id != 0U &&
        child.graph_instance_id != 0U && child.graph_revision != 0U &&
        child.target_node_id == kI1TargetNodeId &&
        child.request_intent == ComputeIntent::RealTimeUpdate &&
        child.generation != 0U && child.accepted_coordinate.has_value();
    if (valid_child) {
      const I2EditEvidence& edit = row.evidence.edits[child.edit_index];
      const auto& product = row.accepted_products[child.edit_index];
      valid_child =
          product.has_value() && child.generation == product->generation &&
          child.accepted_coordinate == edit.accepted_coordinate &&
          child.qos.service_class == compute::ComputeRunQosClass::Interactive &&
          child.qos.weight == 1U &&
          child.qos.maximum_parallelism == std::optional<std::uint32_t>{8U};
      if (child.quality == compute::ComputeRunQuality::Interactive) {
        valid_child = valid_child &&
                      child.child_intent == ComputeIntent::RealTimeUpdate &&
                      child.qos.deadline == edit.preview_deadline;
      } else {
        valid_child =
            valid_child &&
            child.child_intent == ComputeIntent::GlobalHighPrecision &&
            child.qos.deadline == edit.final_deadline;
      }
    }
    const RunKey key{child.edit_index, child.run_id};
    if (!valid_child || !global_run_ids.insert(child.run_id).second ||
        !terminals.emplace(key, terminal).second) {
      structural_failure("I2 child descriptor/terminal identity is malformed");
      continue;
    }
    ++terminal_counts[child.edit_index];
    I2AcceptedProductIdentity& product =
        *row.accepted_products[child.edit_index];
    std::optional<std::uint64_t>& slot =
        child.quality == compute::ComputeRunQuality::Interactive
            ? product.preview_run_id
            : product.final_run_id;
    if (slot.has_value()) {
      structural_failure("I2 edit has duplicate same-quality child Runs");
    } else {
      slot = child.run_id;
    }
  }
  for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
    if (terminal_counts[edit_index] != 0U &&
        terminal_counts[edit_index] != 2U) {
      structural_failure(
          "I2 materialized request did not settle both children");
    }
  }

  std::map<RunKey, I2ObservedRunLifecycleTransition> quiescences;
  std::map<RunKey, I2ObservedRunLifecycleTransition> settlements;
  for (const I2ObservedRunLifecycleTransition& event :
       row.evidence.observations.run_quiescences) {
    const RunKey key{event.child.edit_index, event.child.run_id};
    const auto terminal = terminals.find(key);
    if (terminal == terminals.end() ||
        !i2_child_equal(event.child, terminal->second.child) ||
        !quiescences.emplace(key, event).second) {
      structural_failure("I2 quiescence does not match one terminal child");
    }
  }
  for (const I2ObservedRunLifecycleTransition& event :
       row.evidence.observations.resource_settlements) {
    const RunKey key{event.child.edit_index, event.child.run_id};
    const auto terminal = terminals.find(key);
    if (terminal == terminals.end() ||
        !i2_child_equal(event.child, terminal->second.child) ||
        !settlements.emplace(key, event).second) {
      structural_failure("I2 resource settlement does not match one child");
    }
  }
  for (const auto& [key, terminal] : terminals) {
    if (quiescences.count(key) != 1U || settlements.count(key) != 1U ||
        terminal.causal_sequence >= quiescences[key].causal_sequence ||
        quiescences[key].causal_sequence >= settlements[key].causal_sequence) {
      structural_failure("I2 terminal/quiescence/resource order is incomplete");
    }
  }

  std::map<RunKey, I2ObservedVisibleOutput> visible;
  std::set<std::uint64_t> visible_run_ids;
  for (const I2ObservedVisibleOutput& output :
       row.evidence.observations.visible_outputs) {
    const RunKey key{output.child.edit_index, output.child.run_id};
    const auto terminal = terminals.find(key);
    if (terminal == terminals.end() ||
        terminal->second.kind != compute::ComputeRunTerminalKind::Succeeded ||
        !i2_child_equal(output.child, terminal->second.child) ||
        output.causal_sequence >= terminal->second.causal_sequence ||
        !visible.emplace(key, output).second) {
      structural_failure(
          "I2 visible output is stale, duplicate, or non-successful");
      continue;
    }
    visible_run_ids.insert(output.child.run_id);
    if (!output.value_valid_at_capture || !output.value_revision.valid() ||
        !output.value_allocation.valid() ||
        output.value_binding.allocation != output.value_allocation ||
        output.value_binding.byte_size != output.value_storage_bytes ||
        !output.content_digest.has_value() ||
        output.content_digest->state != ContentDigestState::Available ||
        !output.content_digest->digest.has_value() ||
        output.content_digest->digest->algorithm !=
            CanonicalDigestAlgorithm::Sha256CanonicalV1 ||
        !output.acquisition.has_value()) {
      invalidate_i2(&reasons,
                    "I2 visible output lacks frozen digest/access evidence");
      output_complete = false;
    } else if (!validate_i2_acquisition(*output.acquisition,
                                        output.value_revision, &reasons)) {
      output_complete = false;
    } else if (output.acquisition->host_first.binding != output.value_binding ||
               output.acquisition->host_first.allocation !=
                   output.value_allocation ||
               output.acquisition->host_first.storage_bytes !=
                   output.value_storage_bytes) {
      invalidate_i2(&reasons,
                    "I2 access evidence does not bind the captured Value");
      output_complete = false;
    }
  }

  std::map<RunKey, I2ObservedFinalTrigger> triggers;
  for (const I2ObservedFinalTrigger& trigger :
       row.evidence.observations.final_triggers) {
    const RunKey key{trigger.child.edit_index, trigger.child.run_id};
    const auto terminal = terminals.find(key);
    if (terminal == terminals.end() ||
        trigger.child.quality != compute::ComputeRunQuality::Full ||
        !i2_child_equal(trigger.child, terminal->second.child) ||
        trigger.causal_sequence >= terminal->second.causal_sequence ||
        !triggers.emplace(key, trigger).second) {
      structural_failure("I2 final trigger does not name one HP child");
    }
  }
  for (const auto& [key, trigger] : triggers) {
    const auto& product = row.accepted_products[key.first];
    if (!product.has_value() || !product->preview_run_id.has_value()) {
      structural_failure("I2 final trigger lacks a sibling preview Run");
      continue;
    }
    const RunKey preview_key{key.first, *product->preview_run_id};
    const auto preview_visible = visible.find(preview_key);
    if (preview_visible == visible.end() ||
        preview_visible->second.causal_sequence >= trigger.causal_sequence) {
      structural_failure("I2 final trigger did not follow current preview");
    }
    for (const I2ObservedServiceStart& start :
         row.evidence.observations.service_starts) {
      if (start.child.run_id == key.second &&
          start.causal_sequence <= trigger.causal_sequence) {
        structural_failure("I2 HP service started before final trigger");
      }
    }
  }

  std::map<RunKey, I2ObservedCancellation> cancellations;
  for (const I2ObservedCancellation& cancellation :
       row.evidence.observations.cancellations) {
    const RunKey key{cancellation.child.edit_index, cancellation.child.run_id};
    const auto terminal = terminals.find(key);
    if (terminal == terminals.end() ||
        !i2_child_equal(cancellation.child, terminal->second.child) ||
        terminal->second.kind != compute::ComputeRunTerminalKind::Cancelled ||
        cancellation.causal_sequence >= terminal->second.causal_sequence ||
        !cancellations.emplace(key, cancellation).second) {
      structural_failure(
          "I2 cancellation/terminal state machine is inconsistent");
    }
  }
  for (const auto& [key, terminal] : terminals) {
    const bool terminal_is_cancelled =
        terminal.kind == compute::ComputeRunTerminalKind::Cancelled;
    const bool has_cancellation = cancellations.count(key) == 1U;
    if (terminal_is_cancelled != has_cancellation) {
      structural_failure(
          "I2 terminal/cancellation cardinality is not exactly one-to-one");
    }
  }

  for (const I2ObservedServiceStart& start :
       row.evidence.observations.service_starts) {
    const RunKey key{start.child.edit_index, start.child.run_id};
    const auto terminal = terminals.find(key);
    if (terminal == terminals.end() ||
        !i2_child_equal(start.child, terminal->second.child) ||
        start.causal_sequence >= terminal->second.causal_sequence) {
      structural_failure("I2 service start does not match one terminal child");
      continue;
    }
    if (start.child.quality == compute::ComputeRunQuality::Full) {
      const auto trigger = triggers.find(key);
      if (trigger == triggers.end() ||
          start.causal_sequence <= trigger->second.causal_sequence) {
        structural_failure("I2 HP service start lacks a preceding trigger");
      }
    }
  }
  for (const auto& [key, terminal] : terminals) {
    if (terminal.child.quality != compute::ComputeRunQuality::Full) {
      continue;
    }
    const RunKey run_key = key;
    const bool has_trigger = triggers.count(run_key) == 1U;
    const bool has_service =
        std::any_of(row.evidence.observations.service_starts.begin(),
                    row.evidence.observations.service_starts.end(),
                    [run_key](const I2ObservedServiceStart& start) {
                      return start.child.edit_index == run_key.first &&
                             start.child.run_id == run_key.second;
                    });
    const bool has_visible = visible.count(run_key) == 1U;
    if ((!has_trigger &&
         (has_service || has_visible ||
          terminal.kind != compute::ComputeRunTerminalKind::Cancelled)) ||
        (has_trigger &&
         terminal.kind == compute::ComputeRunTerminalKind::Succeeded &&
         !has_visible)) {
      structural_failure("I2 HP trigger/materialization state is incomplete");
    }
  }

  for (std::size_t edit_index = 0U; edit_index + 1U < kI1EditCount;
       ++edit_index) {
    const auto& product = row.accepted_products[edit_index];
    if (!product.has_value() || !product->preview_run_id.has_value()) {
      structural_failure("I2 early edit did not materialize a preview child");
      continue;
    }
    const RunKey preview_key{edit_index, *product->preview_run_id};
    const auto preview = visible.find(preview_key);
    if (preview == visible.end() ||
        preview->second.observed_at >=
            row.evidence.edits[edit_index + 1U].admission_sample) {
      structural_failure(
          "I2 preview was not visible before the next admission");
    }
    if (product->final_run_id.has_value()) {
      const RunKey final_key{edit_index, *product->final_run_id};
      const auto stale_final = visible.find(final_key);
      if (stale_final != visible.end() &&
          stale_final->second.observed_at >=
              row.evidence.edits[edit_index + 1U].admission_sample) {
        structural_failure(
            "I2 stale final became visible after newer admission");
      }
    }
  }

  constexpr std::size_t kFinalEdit = kI1EditCount - 1U;
  const auto& final_product = row.accepted_products[kFinalEdit];
  if (!final_product.has_value() ||
      !final_product->preview_run_id.has_value() ||
      !final_product->final_run_id.has_value()) {
    structural_failure("I2 twelfth edit lacks both child identities");
  } else {
    const RunKey preview_key{kFinalEdit, *final_product->preview_run_id};
    const RunKey final_key{kFinalEdit, *final_product->final_run_id};
    const auto preview = visible.find(preview_key);
    const auto final = visible.find(final_key);
    const auto trigger = triggers.find(final_key);
    if (preview == visible.end() || final == visible.end() ||
        trigger == triggers.end() ||
        preview->second.causal_sequence >= trigger->second.causal_sequence ||
        trigger->second.causal_sequence >= final->second.causal_sequence) {
      structural_failure(
          "I2 twelfth preview/trigger/final order is incomplete");
    } else {
      const auto accepted =
          row.evidence.edits[kFinalEdit].accepted_coordinate->admission_time();
      row.latencies.preview =
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              preview->second.observed_at - accepted);
      row.latencies.final =
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              final->second.observed_at - accepted);
      if (!preview->second.content_digest.has_value() ||
          !final->second.content_digest.has_value()) {
        invalidate_i2(&reasons, "I2 endpoint digest observations are missing");
        output_complete = false;
      } else {
        row.preview_digest = *preview->second.content_digest;
        row.final_digest = *final->second.content_digest;
      }
      if (!row.evidence.expected_preview_digest.has_value() ||
          !row.evidence.expected_final_digest.has_value() ||
          row.evidence.expected_preview_digest->algorithm !=
              CanonicalDigestAlgorithm::Sha256CanonicalV1 ||
          row.evidence.expected_final_digest->algorithm !=
              CanonicalDigestAlgorithm::Sha256CanonicalV1) {
        invalidate_i2(&reasons, "I2 independent expected digests are missing");
        output_complete = false;
      } else if (preview->second.content_digest.has_value() &&
                 final->second.content_digest.has_value()) {
        output_matches =
            row.preview_digest.digest == row.evidence.expected_preview_digest &&
            row.final_digest.digest == row.evidence.expected_final_digest;
      }
    }
  }

  for (const I2ObservedServiceStart& start :
       row.evidence.observations.service_starts) {
    if (!checked_add_i2_service(&row.service.all_started_service,
                                start.service_charge)) {
      structural_failure("I2 all-started service sum overflowed");
      break;
    }
    if (visible_run_ids.count(start.child.run_id) == 0U &&
        !checked_add_i2_service(&row.service.discarded_started_service,
                                start.service_charge)) {
      structural_failure("I2 discarded service sum overflowed");
      break;
    }
    const auto cancellation =
        i2_cancellation_sequence(row.evidence.observations, start.child.run_id);
    if (cancellation.has_value() && start.causal_sequence > *cancellation &&
        !checked_add_i2_service(&row.service.post_cancel_started_service,
                                start.service_charge)) {
      structural_failure("I2 post-cancel service sum overflowed");
      break;
    }
  }
  set_i2_service_ratio(&row.service);

  std::array<std::size_t, kI1EditCount> host_settlement_counts{};
  std::array<const I1ObservedHostSettlement*, kI1EditCount> host_settlements{};
  for (const I1ObservedHostSettlement& settlement :
       row.evidence.observations.host_settlements) {
    if (settlement.edit_index >= kI1EditCount) {
      structural_failure("I2 Host settlement has invalid edit identity");
    } else {
      ++host_settlement_counts[settlement.edit_index];
      host_settlements[settlement.edit_index] = &settlement;
    }
  }
  for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
    if (host_settlement_counts[edit_index] != 1U) {
      structural_failure("I2 accepted edit lacks exactly one Host settlement");
    } else if (row.evidence.edits[edit_index].host_return.has_value() &&
               host_settlements[edit_index]->observed_at <
                   row.evidence.edits[edit_index].host_return->return_time) {
      structural_failure("I2 Host settlement precedes its scheduling return");
    }
  }

  const ResourceLedger::Snapshot& baseline_host =
      row.evidence.baseline.host_resources;
  const ResourceLedger::Snapshot& final_host =
      row.evidence.final_snapshot.host_resources;
  bool memory_valid =
      baseline_host.limits == final_host.limits &&
      baseline_host.reserved == final_host.reserved &&
      host_resources_not_less(baseline_host.high_water,
                              final_host.high_water) &&
      host_resources_within(final_host.reserved, final_host.limits) &&
      host_resources_within(final_host.high_water, final_host.limits) &&
      lifecycle_equal(row.evidence.baseline.lifecycle.counters,
                      row.evidence.final_snapshot.lifecycle.counters) &&
      row.evidence.baseline.lifecycle.service_instance_id ==
          row.evidence.final_snapshot.lifecycle.service_instance_id &&
      row.evidence.baseline.lifecycle.telemetry_epoch ==
          row.evidence.final_snapshot.lifecycle.telemetry_epoch &&
      lifecycle_work_settled(row.evidence.baseline.lifecycle.counters) &&
      lifecycle_work_settled(row.evidence.final_snapshot.lifecycle.counters) &&
      row.evidence.final_snapshot.lifecycle.cursor_gap == 0U &&
      row.evidence.final_snapshot.lifecycle.global_dropped_total ==
          row.evidence.baseline.lifecycle.global_dropped_total;
  if (row.evidence.baseline.device_resources.size() !=
      row.evidence.final_snapshot.device_resources.size()) {
    memory_valid = false;
  } else {
    for (std::size_t index = 0U;
         index < row.evidence.baseline.device_resources.size(); ++index) {
      const auto& before = row.evidence.baseline.device_resources[index];
      const auto& after = row.evidence.final_snapshot.device_resources[index];
      memory_valid =
          memory_valid && before.device == after.device &&
          before.limits == after.limits &&
          after.reserved.device_scratch_bytes ==
              before.reserved.device_scratch_bytes &&
          device_resources_not_less(before.high_water, after.high_water) &&
          device_resources_within(after.reserved, after.limits) &&
          device_resources_within(after.high_water, after.limits);
    }
  }
  row.memory_settled = memory_valid;

  if (!structure_valid) {
    row.latency_verdict = I1Verdict::Invalid;
    row.waste_verdict = I1Verdict::Invalid;
    row.memory_verdict = I1Verdict::Invalid;
    row.output_verdict = I1Verdict::Invalid;
    return row;
  }
  if (!row.latencies.preview.has_value() || !row.latencies.final.has_value() ||
      *row.latencies.preview < std::chrono::nanoseconds::zero() ||
      *row.latencies.final < std::chrono::nanoseconds::zero()) {
    row.latency_verdict = I1Verdict::Invalid;
  } else {
    row.latency_verdict = *row.latencies.preview <= kI2PreviewDeadlineBudget &&
                                  *row.latencies.final <= kI2FinalDeadlineBudget
                              ? I1Verdict::Pass
                              : I1Verdict::Fail;
  }
  row.waste_verdict = row.service.post_cancel_started_service == 0U
                          ? I1Verdict::Pass
                          : I1Verdict::Fail;
  row.memory_verdict = memory_valid ? I1Verdict::Pass : I1Verdict::Fail;
  row.output_verdict =
      !output_complete ? I1Verdict::Invalid
                       : (output_matches ? I1Verdict::Pass : I1Verdict::Fail);
  return row;
}

/** @copydoc evaluate_i2_replicate */
I2ReplicateSummary evaluate_i2_replicate(
    const std::vector<I2EpisodeInnerRow>& rows) {
  I2ReplicateSummary summary;
  if (!rows.empty()) {
    summary.replicate_ordinal = rows.front().evidence.replicate_ordinal;
  }
  std::array<const I2EpisodeInnerRow*, kI2GridSlotCount> by_slot{};
  bool common_valid = rows.size() == kI2GridSlotCount;
  for (const I2EpisodeInnerRow& row : rows) {
    if (row.schema != kI2InnerRowSchema ||
        row.schema_version != kI2InnerRowSchemaVersion ||
        row.workload_id != kI2WorkloadId ||
        row.evidence.replicate_ordinal != summary.replicate_ordinal ||
        row.evidence.slot >= kI2GridSlotCount ||
        by_slot[row.evidence.slot] != nullptr) {
      common_valid = false;
      continue;
    }
    by_slot[row.evidence.slot] = &row;
  }
  for (const I2EpisodeInnerRow* row : by_slot) {
    common_valid = common_valid && row != nullptr;
  }
  if (!common_valid) {
    invalidate_i2(&summary.validity_reasons,
                  "I2 replicate slots/schema/ordinal are incomplete");
  }
  bool grid_valid = true;
  if (!rows.empty()) {
    const auto common_grid_origin = rows.front().evidence.grid_origin;
    std::optional<std::chrono::steady_clock::time_point> expected_terminal;
    try {
      expected_terminal = i2_terminal_boundary(common_grid_origin);
    } catch (const std::exception&) {
      grid_valid = false;
    }
    for (const I2EpisodeInnerRow& row : rows) {
      if (row.evidence.grid_origin != common_grid_origin ||
          !expected_terminal.has_value() ||
          row.evidence.terminal_boundary != *expected_terminal) {
        grid_valid = false;
      }
      if (row.evidence.slot >= kI2GridSlotCount) {
        continue;
      }
      try {
        if (row.evidence.episode_origin !=
            i2_episode_origin(common_grid_origin, row.evidence.slot)) {
          grid_valid = false;
        }
      } catch (const std::exception&) {
        grid_valid = false;
      }
    }
  }
  if (!grid_valid) {
    invalidate_i2(
        &summary.validity_reasons,
        "I2 replicate grid origin/episode origins/terminal boundary do not "
        "form one checked 111-slot grid");
  }
  common_valid = common_valid && grid_valid;

  std::vector<std::chrono::nanoseconds> preview_samples;
  std::vector<std::chrono::nanoseconds> final_samples;
  preview_samples.reserve(kI2MeasuredSlotCount);
  final_samples.reserve(kI2MeasuredSlotCount);
  bool latency_fail = false;
  bool latency_invalid = !common_valid;
  bool waste_fail = false;
  bool waste_invalid = !common_valid;
  bool memory_fail = false;
  bool memory_invalid = !common_valid;
  bool output_fail = false;
  bool output_invalid = !common_valid;
  for (std::size_t slot = 0U; slot < kI2GridSlotCount; ++slot) {
    const I2EpisodeInnerRow* row = by_slot[slot];
    if (row == nullptr) {
      continue;
    }
    accumulate_i2_verdict(row->latency_verdict, &latency_fail,
                          &latency_invalid);
    accumulate_i2_verdict(row->waste_verdict, &waste_fail, &waste_invalid);
    accumulate_i2_verdict(row->memory_verdict, &memory_fail, &memory_invalid);
    accumulate_i2_verdict(row->output_verdict, &output_fail, &output_invalid);
    if (slot <= kI2WarmupSlotCount) {
      continue;
    }
    if (!row->latencies.preview.has_value() ||
        !row->latencies.final.has_value()) {
      latency_invalid = true;
    } else {
      preview_samples.push_back(*row->latencies.preview);
      final_samples.push_back(*row->latencies.final);
    }
    if (!checked_add_i2_service(&summary.measured_service.all_started_service,
                                row->service.all_started_service) ||
        !checked_add_i2_service(
            &summary.measured_service.discarded_started_service,
            row->service.discarded_started_service) ||
        !checked_add_i2_service(
            &summary.measured_service.post_cancel_started_service,
            row->service.post_cancel_started_service)) {
      waste_invalid = true;
    }
  }
  summary.measured_sample_count = preview_samples.size();
  set_i2_service_ratio(&summary.measured_service);
  if (preview_samples.size() != kI2MeasuredSlotCount ||
      final_samples.size() != kI2MeasuredSlotCount) {
    latency_invalid = true;
  } else {
    summary.latency =
        I2LatencyPercentiles{i1_nearest_rank(preview_samples, 50U, 100U),
                             i1_nearest_rank(preview_samples, 95U, 100U),
                             i1_nearest_rank(preview_samples, 99U, 100U),
                             i1_nearest_rank(final_samples, 50U, 100U),
                             i1_nearest_rank(final_samples, 95U, 100U),
                             i1_nearest_rank(final_samples, 99U, 100U)};
    latency_fail = latency_fail ||
                   summary.latency->preview_p50 > kI2PreviewLatencyP50Limit ||
                   summary.latency->preview_p95 > kI2PreviewLatencyP95Limit ||
                   summary.latency->preview_p99 > kI2PreviewLatencyP99Limit ||
                   summary.latency->final_p95 > kI2FinalLatencyP95Limit ||
                   summary.latency->final_p99 > kI2FinalLatencyP99Limit;
  }
  if (!summary.measured_service.discarded_ratio.has_value()) {
    waste_invalid = true;
  } else if (*summary.measured_service.discarded_ratio >
                 kI2DiscardedServiceRatioLimit ||
             summary.measured_service.post_cancel_started_service != 0U) {
    waste_fail = true;
  }
  summary.latency_verdict =
      latency_invalid ? I1Verdict::Invalid
                      : (latency_fail ? I1Verdict::Fail : I1Verdict::Pass);
  summary.waste_verdict =
      waste_invalid ? I1Verdict::Invalid
                    : (waste_fail ? I1Verdict::Fail : I1Verdict::Pass);
  summary.memory_verdict =
      memory_invalid ? I1Verdict::Invalid
                     : (memory_fail ? I1Verdict::Fail : I1Verdict::Pass);
  summary.output_verdict =
      output_invalid ? I1Verdict::Invalid
                     : (output_fail ? I1Verdict::Fail : I1Verdict::Pass);
  return summary;
}

}  // namespace ps::benchmark
