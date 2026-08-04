/**
 * @file i1_evidence.cpp
 * @brief Implements fail-closed I1 inner-row and replicate evaluation.
 */
#include "benchmark/i1_evidence.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ps::benchmark {
namespace {

/**
 * @brief Adds one structural invalidation reason without suppressing others.
 * @param reasons Destination diagnostic sequence.
 * @param reason Stable human-readable reason.
 * @return Nothing.
 * @throws std::bad_alloc when diagnostic ownership cannot allocate.
 */
void invalidate(std::vector<std::string>* reasons, std::string reason) {
  reasons->push_back(std::move(reason));
}

/**
 * @brief Checked-adds one service charge into an aggregate.
 * @param total In/out exact aggregate.
 * @param charge Nonnegative callback charge.
 * @return True after exact addition, false without mutation on overflow.
 * @throws Nothing.
 */
bool checked_add_service(std::uint64_t* total, std::uint64_t charge) noexcept {
  if (charge > std::numeric_limits<std::uint64_t>::max() - *total) {
    return false;
  }
  *total += charge;
  return true;
}

/**
 * @brief Tests component-wise monotonicity of Host resource snapshots.
 * @param lower Earlier component-wise values.
 * @param upper Later lifetime component-wise values.
 * @return True when no component decreases.
 * @throws Nothing.
 */
bool resources_not_less(const ResourceVector& lower,
                        const ResourceVector& upper) noexcept {
  return lower.cpu_slots <= upper.cpu_slots &&
         lower.retained_memory_bytes <= upper.retained_memory_bytes &&
         lower.scratch_bytes <= upper.scratch_bytes &&
         lower.ready_entries <= upper.ready_entries &&
         lower.ready_bytes <= upper.ready_bytes;
}

/**
 * @brief Tests whether every Host resource component is zero.
 * @param value Complete resource vector.
 * @return True only for the all-zero vector.
 * @throws Nothing.
 */
bool resources_zero(const ResourceVector& value) noexcept {
  return value == ResourceVector{};
}

/**
 * @brief Tests component-wise monotonicity of device resource snapshots.
 * @param lower Earlier component-wise values.
 * @param upper Later lifetime component-wise values.
 * @return True when no component decreases.
 * @throws Nothing.
 */
bool device_resources_not_less(const DeviceResourceVector& lower,
                               const DeviceResourceVector& upper) noexcept {
  return lower.device_memory_bytes <= upper.device_memory_bytes &&
         lower.device_scratch_bytes <= upper.device_scratch_bytes;
}

/**
 * @brief Tests whether both device resource components are zero.
 * @param value Complete device vector.
 * @return True only for the all-zero vector.
 * @throws Nothing.
 */
bool device_resources_zero(const DeviceResourceVector& value) noexcept {
  return value == DeviceResourceVector{};
}

/**
 * @brief Compares every execution-lifecycle counter exactly.
 * @param lhs First counter cut.
 * @param rhs Second counter cut.
 * @return True only when every counter is equal.
 * @throws Nothing.
 */
bool lifecycle_counters_equal(
    const compute::ExecutionLifecycleCounters& lhs,
    const compute::ExecutionLifecycleCounters& rhs) noexcept {
  return lhs.registered_graph_count == rhs.registered_graph_count &&
         lhs.open_graph_count == rhs.open_graph_count &&
         lhs.closing_graph_count == rhs.closing_graph_count &&
         lhs.pending_candidate_count == rhs.pending_candidate_count &&
         lhs.admitted_standalone_run_count ==
             rhs.admitted_standalone_run_count &&
         lhs.admitted_run_group_count == rhs.admitted_run_group_count &&
         lhs.admitted_child_run_count == rhs.admitted_child_run_count &&
         lhs.terminal_not_quiescent_run_count ==
             rhs.terminal_not_quiescent_run_count &&
         lhs.finalizing_run_count == rhs.finalizing_run_count &&
         lhs.ready_entry_count == rhs.ready_entry_count &&
         lhs.entered_callback_count == rhs.entered_callback_count &&
         lhs.live_root_reservation_count == rhs.live_root_reservation_count &&
         lhs.live_child_grant_count == rhs.live_child_grant_count &&
         lhs.live_policy_invocation_count == rhs.live_policy_invocation_count &&
         lhs.live_policy_binding_count == rhs.live_policy_binding_count;
}

/**
 * @brief Tests whether all row-active lifecycle counters are settled.
 * @param counters Complete process lifecycle cut.
 * @return True when graph-independent active work/ownership is zero.
 * @throws Nothing.
 * @note Registered/open Graph and live policy-binding counts may remain at
 * their pre-row baseline while the reusable session stays loaded.
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
 * @brief Reports whether an event's collector-local causal sequence is unique.
 * @param sequence Nonzero sequence to reserve in the seen set.
 * @param seen Complete event-category-independent set.
 * @return True only when insertion succeeds and sequence is nonzero.
 * @throws std::bad_alloc when the set node cannot allocate.
 */
bool reserve_observation_sequence(std::uint64_t sequence,
                                  std::set<std::uint64_t>* seen) {
  return sequence != 0U && seen->insert(sequence).second;
}

/**
 * @brief Returns the single observed generation for one edit.
 * @param observations Complete bounded episode observations.
 * @param edit_index Frozen edit identity.
 * @return Matching record only when exactly one exists.
 * @throws Nothing.
 */
std::optional<I1ObservedCurrentGeneration> single_generation_for_edit(
    const I1EpisodeObservationSnapshot& observations,
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
 * @brief Finds the accepted cancellation causal cut for one Run.
 * @param observations Complete episode callbacks.
 * @param run_id Opaque materialized Run identity.
 * @return Earliest matching causal sequence, or absent when not cancelled.
 * @throws Nothing.
 */
std::optional<std::uint64_t> cancellation_sequence_for_run(
    const I1EpisodeObservationSnapshot& observations,
    std::uint64_t run_id) noexcept {
  std::optional<std::uint64_t> result;
  for (const I1ObservedCancellation& cancellation :
       observations.cancellations) {
    if (cancellation.run_id != run_id) {
      continue;
    }
    if (!result.has_value() || cancellation.causal_sequence < *result) {
      result = cancellation.causal_sequence;
    }
  }
  return result;
}

/**
 * @brief Verifies one product coordinate belongs to authoritative `Q_end`.
 * @param observed_at Product-boundary steady-clock sample.
 * @param sequence Product-boundary causal sequence.
 * @param measurement_end Exact inclusive time boundary.
 * @param cut First collector sequence excluded from boundary history.
 * @return True when both physical time and causal order precede the cut.
 * @throws Nothing.
 */
bool event_within_cut(std::chrono::steady_clock::time_point observed_at,
                      std::uint64_t sequence,
                      std::chrono::steady_clock::time_point measurement_end,
                      const I1ObservationHistoryCut& cut) noexcept {
  return observed_at <= measurement_end && sequence < cut.causal_sequence;
}

/**
 * @brief Converts complete measured service sums to an optional ratio.
 * @param service In/out service evidence with complete integer sums.
 * @return Nothing after setting or clearing the ratio.
 * @throws Nothing.
 */
void set_service_ratio(I1ServiceEvidence* service) noexcept {
  if (service->all_started_service == 0U) {
    service->discarded_ratio.reset();
    return;
  }
  service->discarded_ratio =
      static_cast<double>(service->discarded_started_service) /
      static_cast<double>(service->all_started_service);
}

/**
 * @brief Converts one per-dimension row set to fail/invalid flags.
 * @param verdict Row verdict to inspect.
 * @param saw_fail In/out failure flag.
 * @param saw_invalid In/out invalidity flag.
 * @return Nothing.
 * @throws Nothing.
 */
void accumulate_verdict(I1Verdict verdict, bool* saw_fail,
                        bool* saw_invalid) noexcept {
  if (verdict == I1Verdict::Fail) {
    *saw_fail = true;
  } else if (verdict == I1Verdict::Invalid) {
    *saw_invalid = true;
  }
}

}  // namespace

/** @copydoc capture_i1_edit_evidence */
I1EditEvidence capture_i1_edit_evidence(
    const I1EditAdmissionResult& admission,
    std::optional<OperationStatus> settlement) {
  if (admission.edit_index >= kI1EditCount) {
    throw std::out_of_range("I1 evidence edit index is outside [0,11].");
  }
  return I1EditEvidence{
      admission.edit_index,
      kI1EditCoefficients[admission.edit_index],
      i1_edit_region(admission.edit_index),
      admission.nominal_time,
      admission.admission_sample,
      admission.admission_window_valid,
      admission.reserved_event_sequence,
      admission.deadline,
      admission.host_return,
      admission.accepted_coordinate,
      std::move(settlement),
  };
}

/** @copydoc i1_nearest_rank */
std::chrono::nanoseconds i1_nearest_rank(
    std::vector<std::chrono::nanoseconds> samples,
    std::uint32_t percentile_numerator, std::uint32_t percentile_denominator) {
  if (samples.empty() || percentile_numerator == 0U ||
      percentile_denominator == 0U ||
      percentile_numerator > percentile_denominator) {
    throw std::invalid_argument(
        "I1 nearest rank requires samples and a fraction in (0,1].");
  }
  const std::uint64_t count = static_cast<std::uint64_t>(samples.size());
  if (count >
      std::numeric_limits<std::uint64_t>::max() / percentile_numerator) {
    throw std::overflow_error("I1 nearest-rank multiplication overflowed.");
  }
  const std::uint64_t product = count * percentile_numerator;
  const std::uint64_t rank =
      product / percentile_denominator +
      static_cast<std::uint64_t>(product % percentile_denominator != 0U);
  std::sort(samples.begin(), samples.end());
  return samples[static_cast<std::size_t>(rank - 1U)];
}

/** @copydoc evaluate_i1_episode */
I1EpisodeInnerRow evaluate_i1_episode(I1EpisodeEvidenceInput input) {
  I1EpisodeInnerRow row;
  row.evidence = std::move(input);
  bool global_valid = true;
  bool latency_evidence_valid = true;
  bool waste_evidence_valid = true;
  bool memory_evidence_valid = true;
  bool output_evidence_valid = true;
  bool latency_gate_failure = false;

  const auto global_invalidate = [&](std::string reason) {
    global_valid = false;
    invalidate(&row.validity_reasons, std::move(reason));
  };
  const auto latency_invalidate = [&](std::string reason) {
    latency_evidence_valid = false;
    invalidate(&row.validity_reasons, std::move(reason));
  };
  const auto waste_invalidate = [&](std::string reason) {
    waste_evidence_valid = false;
    invalidate(&row.validity_reasons, std::move(reason));
  };
  const auto memory_invalidate = [&](std::string reason) {
    memory_evidence_valid = false;
    invalidate(&row.validity_reasons, std::move(reason));
  };
  const auto output_invalidate = [&](std::string reason) {
    output_evidence_valid = false;
    invalidate(&row.validity_reasons, std::move(reason));
  };

  if (row.evidence.replicate_ordinal == 0U ||
      row.evidence.replicate_ordinal > 3U) {
    global_invalidate("replicate ordinal is outside [1,3]");
  }
  if (row.evidence.slot >= kI1GridSlotCount) {
    global_invalidate("grid slot is outside [0,220]");
  } else {
    if (row.evidence.episode_origin !=
        i1_episode_origin(row.evidence.grid_origin, row.evidence.slot)) {
      global_invalidate("episode origin does not derive from the fixed grid");
    }
    if (row.evidence.terminal_boundary !=
        i1_terminal_boundary(row.evidence.grid_origin)) {
      global_invalidate("terminal boundary does not derive from the grid");
    }
    if (row.evidence.measurement_start !=
        checked_i1_time_add(row.evidence.episode_origin,
                            kI1MeasurementStartOffset)) {
      global_invalidate("measurement start is not the frozen S_11 boundary");
    }
    if (row.evidence.measurement_end !=
        checked_i1_time_add(row.evidence.episode_origin,
                            kI1MeasurementEndOffset)) {
      global_invalidate("measurement end is not the frozen Q_end boundary");
    }
  }
  if (row.evidence.observation_cut.causal_sequence == 0U) {
    global_invalidate("Q_end history cut has a zero causal sequence");
  }
  if (row.evidence.observation_cut.captured_at < row.evidence.measurement_end ||
      (row.evidence.slot + 1U < kI1GridSlotCount &&
       row.evidence.observation_cut.captured_at >=
           i1_episode_origin(row.evidence.grid_origin,
                             row.evidence.slot + 1U)) ||
      (row.evidence.slot + 1U == kI1GridSlotCount &&
       row.evidence.observation_cut.captured_at >=
           row.evidence.terminal_boundary)) {
    global_invalidate("Q_end history cut lies outside the terminal guard");
  }
  if (row.evidence.final_snapshot_sample <
          row.evidence.observation_cut.captured_at ||
      (row.evidence.slot + 1U < kI1GridSlotCount &&
       row.evidence.final_snapshot_sample >=
           i1_episode_origin(row.evidence.grid_origin,
                             row.evidence.slot + 1U)) ||
      (row.evidence.slot + 1U == kI1GridSlotCount &&
       row.evidence.final_snapshot_sample >= row.evidence.terminal_boundary)) {
    global_invalidate("final snapshot lies outside the terminal guard");
  }
  if (row.evidence.observations.overflowed) {
    global_invalidate("bounded Run observation storage overflowed");
  }

  std::uint64_t previous_event_sequence = 0U;
  std::uint64_t previous_generation = 0U;
  std::optional<I1AcceptedCoordinate> previous_product_coordinate;
  std::set<std::uint64_t> seen_materialized_run_ids;
  for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
    const I1EditEvidence& edit = row.evidence.edits[edit_index];
    const auto expected_nominal = checked_i1_time_add(
        row.evidence.episode_origin,
        std::chrono::nanoseconds(kI1EditStride.count() *
                                 static_cast<std::int64_t>(edit_index)));
    if (edit.edit_index != edit_index ||
        edit.coefficient != kI1EditCoefficients[edit_index] ||
        edit.region != i1_edit_region(edit_index) ||
        edit.nominal_time != expected_nominal) {
      global_invalidate("edit workload/index/nominal facts drifted");
      continue;
    }
    const auto latest_admission =
        checked_i1_time_add(expected_nominal, kI1AdmissionLateness);
    if (!edit.admission_window_valid ||
        edit.admission_sample < expected_nominal ||
        edit.admission_sample > latest_admission) {
      global_invalidate("edit admission is outside the inclusive window");
    }
    if (!edit.reserved_event_sequence.has_value() ||
        *edit.reserved_event_sequence == 0U ||
        *edit.reserved_event_sequence <= previous_event_sequence) {
      global_invalidate("edit event sequence is absent or not increasing");
    } else {
      previous_event_sequence = *edit.reserved_event_sequence;
    }
    if (!edit.deadline.has_value() ||
        *edit.deadline !=
            checked_i1_time_add(edit.admission_sample, kI1DeadlineBudget)) {
      global_invalidate("edit deadline is absent or not anchored to A_i");
    }
    if (!edit.host_return.has_value() || !edit.host_return->status.ok ||
        !edit.host_return->future_valid ||
        edit.host_return->return_time < edit.admission_sample) {
      global_invalidate("Host admission did not return canonical success");
    }
    if (!edit.accepted_coordinate.has_value() ||
        edit.accepted_coordinate->admission_time() != edit.admission_sample ||
        !edit.reserved_event_sequence.has_value() ||
        edit.accepted_coordinate->event_sequence() !=
            *edit.reserved_event_sequence) {
      global_invalidate("accepted coordinate does not equal reserved A_i/seq");
    }
    if (!edit.settlement_status.has_value()) {
      global_invalidate("edit product status is absent after bounded drain");
    }
    const I1ObservedHostSettlement* host_settlement = nullptr;
    std::size_t host_settlement_count = 0U;
    for (const I1ObservedHostSettlement& event :
         row.evidence.observations.host_settlements) {
      if (event.edit_index == edit_index) {
        ++host_settlement_count;
        host_settlement = &event;
      }
    }
    if (host_settlement_count != 1U) {
      global_invalidate("edit lacks exactly one Host settlement event");
    } else if (edit.host_return.has_value() &&
               host_settlement->observed_at < edit.host_return->return_time) {
      global_invalidate("Host settlement precedes the scheduling return");
    }

    const std::optional<I1ObservedCurrentGeneration> generation =
        single_generation_for_edit(row.evidence.observations, edit_index);
    if (!generation.has_value() || generation->generation == 0U ||
        generation->generation <= previous_generation) {
      global_invalidate("edit has missing, duplicate, or unordered generation");
      continue;
    }
    previous_generation = generation->generation;
    if (!generation->accepted_coordinate.has_value() ||
        !edit.accepted_coordinate.has_value() ||
        !(*generation->accepted_coordinate == *edit.accepted_coordinate)) {
      global_invalidate(
          "current generation is not bound to the accepted coordinate");
    } else if (previous_product_coordinate.has_value() &&
               !(*previous_product_coordinate <
                 *generation->accepted_coordinate)) {
      global_invalidate(
          "product accepted-coordinate ordering is not increasing");
    } else {
      previous_product_coordinate = generation->accepted_coordinate;
    }
    if (generation->observed_at < edit.admission_sample) {
      global_invalidate("current generation precedes accepted admission");
    }
    if (host_settlement != nullptr &&
        (host_settlement->causal_sequence <= generation->causal_sequence ||
         host_settlement->observed_at < generation->observed_at)) {
      global_invalidate("Host settlement does not follow current generation");
    }

    std::set<std::uint64_t> materialized_runs;
    for (const I1ObservedServiceStart& start :
         row.evidence.observations.service_starts) {
      if (start.edit_index == edit_index) {
        materialized_runs.insert(start.run_id);
        if (start.generation != generation->generation) {
          global_invalidate("service start generation does not match edit");
        }
      }
    }
    for (const I1ObservedCancellation& cancellation :
         row.evidence.observations.cancellations) {
      if (cancellation.edit_index == edit_index) {
        materialized_runs.insert(cancellation.run_id);
        if (cancellation.generation != generation->generation) {
          global_invalidate("cancellation generation does not match edit");
        }
      }
    }
    for (const I1ObservedTerminal& terminal :
         row.evidence.observations.terminals) {
      if (terminal.edit_index == edit_index) {
        materialized_runs.insert(terminal.run_id);
        if (terminal.generation != generation->generation) {
          global_invalidate("terminal generation does not match edit");
        }
      }
    }
    for (const I1ObservedVisibleOutput& visible :
         row.evidence.observations.visible_outputs) {
      if (visible.edit_index == edit_index) {
        materialized_runs.insert(visible.run_id);
        if (visible.generation != generation->generation) {
          global_invalidate("visible generation does not match edit");
        }
      }
    }
    for (const I1ObservedRunLifecycleTransition& quiescence :
         row.evidence.observations.run_quiescences) {
      if (quiescence.edit_index == edit_index) {
        materialized_runs.insert(quiescence.run_id);
        if (quiescence.generation != generation->generation) {
          global_invalidate("quiescence generation does not match edit");
        }
      }
    }
    for (const I1ObservedRunLifecycleTransition& settlement :
         row.evidence.observations.resource_settlements) {
      if (settlement.edit_index == edit_index) {
        materialized_runs.insert(settlement.run_id);
        if (settlement.generation != generation->generation) {
          global_invalidate(
              "resource settlement generation does not match edit");
        }
      }
    }
    if (materialized_runs.size() > 1U) {
      global_invalidate("one accepted edit materialized multiple Run ids");
      continue;
    }
    I1AcceptedProductIdentity product{generation->generation, std::nullopt,
                                      generation->accepted_coordinate};
    if (!materialized_runs.empty()) {
      product.run_id = *materialized_runs.begin();
      if (*product.run_id == 0U) {
        global_invalidate("materialized Run identity is zero");
      } else if (!seen_materialized_run_ids.insert(*product.run_id).second) {
        global_invalidate("materialized Run identity is reused across edits");
      }
      std::size_t terminal_count = 0U;
      std::size_t cancellation_count = 0U;
      std::size_t visible_count = 0U;
      std::size_t quiescence_count = 0U;
      std::size_t resource_settlement_count = 0U;
      const I1ObservedTerminal* terminal_event = nullptr;
      const I1ObservedCancellation* cancellation_event = nullptr;
      const I1ObservedVisibleOutput* visible_event = nullptr;
      const I1ObservedRunLifecycleTransition* quiescence_event = nullptr;
      const I1ObservedRunLifecycleTransition* resource_settlement_event =
          nullptr;
      for (const I1ObservedTerminal& terminal :
           row.evidence.observations.terminals) {
        if (terminal.run_id == *product.run_id) {
          ++terminal_count;
          terminal_event = &terminal;
        }
      }
      for (const I1ObservedCancellation& cancellation :
           row.evidence.observations.cancellations) {
        if (cancellation.run_id == *product.run_id) {
          ++cancellation_count;
          cancellation_event = &cancellation;
          if (cancellation.reason !=
                  compute::ComputeRunCancellationReason::Superseded &&
              cancellation.reason !=
                  compute::ComputeRunCancellationReason::DeadlineExceeded) {
            global_invalidate("Run cancellation has external/non-I1 reason");
          }
        }
      }
      for (const I1ObservedVisibleOutput& visible :
           row.evidence.observations.visible_outputs) {
        if (visible.run_id == *product.run_id) {
          ++visible_count;
          visible_event = &visible;
          if (edit.deadline.has_value() &&
              visible.observed_at > *edit.deadline) {
            if (edit_index == kI1EditCount - 1U) {
              latency_gate_failure = true;
            } else {
              global_invalidate(
                  "an intermediate edit published after its deadline");
            }
          }
        }
      }
      for (const I1ObservedRunLifecycleTransition& quiescence :
           row.evidence.observations.run_quiescences) {
        if (quiescence.run_id == *product.run_id) {
          ++quiescence_count;
          quiescence_event = &quiescence;
        }
      }
      for (const I1ObservedRunLifecycleTransition& settlement :
           row.evidence.observations.resource_settlements) {
        if (settlement.run_id == *product.run_id) {
          ++resource_settlement_count;
          resource_settlement_event = &settlement;
        }
      }
      if (terminal_count != 1U) {
        global_invalidate("materialized Run lacks exactly one terminal event");
      }
      if (cancellation_count > 1U || visible_count > 1U) {
        global_invalidate("Run has duplicate cancellation or visibility");
      }
      if (quiescence_count != 1U || resource_settlement_count != 1U) {
        global_invalidate(
            "Run lacks exactly one quiescence and resource settlement");
      }
      if (terminal_event != nullptr) {
        if (terminal_event->kind ==
                compute::ComputeRunTerminalKind::Cancelled &&
            cancellation_count != 1U) {
          global_invalidate("cancelled Run lacks one accepted cancellation");
        }
        if (terminal_event->kind !=
                compute::ComputeRunTerminalKind::Cancelled &&
            cancellation_count != 0U) {
          global_invalidate("non-cancelled Run carries accepted cancellation");
        }
        if (terminal_event->kind ==
                compute::ComputeRunTerminalKind::Succeeded &&
            visible_count != 1U) {
          global_invalidate("successful current Run lacks one visible output");
        }
        if (terminal_event->kind !=
                compute::ComputeRunTerminalKind::Succeeded &&
            visible_count != 0U) {
          global_invalidate("non-successful Run published visible output");
        }
        if (edit.settlement_status.has_value() &&
            (edit.settlement_status->ok !=
             (terminal_event->kind ==
              compute::ComputeRunTerminalKind::Succeeded))) {
          global_invalidate("Host settlement contradicts Run terminal kind");
        }
        if (generation->causal_sequence >= terminal_event->causal_sequence ||
            generation->observed_at > terminal_event->observed_at) {
          global_invalidate("Run terminal does not follow current generation");
        }
        for (const I1ObservedServiceStart& start :
             row.evidence.observations.service_starts) {
          if (start.run_id != *product.run_id) {
            continue;
          }
          if (start.causal_sequence <= generation->causal_sequence ||
              start.observed_at < generation->observed_at) {
            global_invalidate(
                "service start does not follow current generation");
          }
          if (start.causal_sequence >= terminal_event->causal_sequence ||
              start.observed_at > terminal_event->observed_at) {
            global_invalidate("service start does not precede Run terminal");
          }
        }
      }
      if (cancellation_event != nullptr && terminal_event != nullptr &&
          (cancellation_event->causal_sequence >=
               terminal_event->causal_sequence ||
           cancellation_event->observed_at > terminal_event->observed_at)) {
        global_invalidate("accepted cancellation does not precede terminal");
      }
      if (visible_event != nullptr && terminal_event != nullptr &&
          (visible_event->causal_sequence >= terminal_event->causal_sequence ||
           visible_event->observed_at > terminal_event->observed_at)) {
        global_invalidate("visible publication does not precede terminal");
      }
      if (terminal_event != nullptr && quiescence_event != nullptr &&
          (terminal_event->causal_sequence >=
               quiescence_event->causal_sequence ||
           terminal_event->observed_at > quiescence_event->observed_at)) {
        global_invalidate("Run quiescence does not follow terminal");
      }
      if (quiescence_event != nullptr && resource_settlement_event != nullptr &&
          (quiescence_event->causal_sequence >=
               resource_settlement_event->causal_sequence ||
           quiescence_event->observed_at >
               resource_settlement_event->observed_at)) {
        global_invalidate("resource settlement does not follow Run quiescence");
      }
      if (resource_settlement_event != nullptr && host_settlement != nullptr &&
          (resource_settlement_event->causal_sequence >=
               host_settlement->causal_sequence ||
           resource_settlement_event->observed_at >
               host_settlement->observed_at)) {
        global_invalidate(
            "Host settlement does not follow Run resource settlement");
      }
    } else if (edit.settlement_status.has_value() &&
               edit.settlement_status->ok) {
      global_invalidate("successful Host settlement has no materialized Run");
    }
    row.accepted_products[edit_index] = product;
  }

  if (row.evidence.edits.back().deadline.has_value() &&
      *row.evidence.edits.back().deadline >
          checked_i1_time_add(row.evidence.episode_origin,
                              kI1LatestFinalDeadlineOffset)) {
    global_invalidate("twelfth-edit deadline exceeds the frozen latest bound");
  }

  std::set<std::uint64_t> observation_sequences;
  const auto check_sequence_and_time =
      [&](std::uint64_t sequence,
          std::chrono::steady_clock::time_point observed_at) {
        if (!reserve_observation_sequence(sequence, &observation_sequences)) {
          global_invalidate("observation causal sequence is zero or duplicate");
        }
        if (!event_within_cut(observed_at, sequence,
                              row.evidence.measurement_end,
                              row.evidence.observation_cut)) {
          global_invalidate("observation lies after the Q_end history cut");
        }
      };
  for (const I1ObservedCurrentGeneration& event :
       row.evidence.observations.current_generations) {
    if (event.edit_index >= kI1EditCount) {
      global_invalidate("current-generation event has invalid edit index");
    }
    check_sequence_and_time(event.causal_sequence, event.observed_at);
  }
  for (const I1ObservedServiceStart& event :
       row.evidence.observations.service_starts) {
    if (event.edit_index >= kI1EditCount) {
      global_invalidate("service-start event has invalid edit index");
    }
    check_sequence_and_time(event.causal_sequence, event.observed_at);
  }
  for (const I1ObservedCancellation& event :
       row.evidence.observations.cancellations) {
    if (event.edit_index >= kI1EditCount) {
      global_invalidate("cancellation event has invalid edit index");
    }
    check_sequence_and_time(event.causal_sequence, event.observed_at);
  }
  for (const I1ObservedTerminal& event : row.evidence.observations.terminals) {
    if (event.edit_index >= kI1EditCount) {
      global_invalidate("terminal event has invalid edit index");
    }
    check_sequence_and_time(event.causal_sequence, event.observed_at);
  }
  for (const I1ObservedVisibleOutput& event :
       row.evidence.observations.visible_outputs) {
    if (event.edit_index >= kI1EditCount) {
      global_invalidate("visible-output event has invalid edit index");
    }
    check_sequence_and_time(event.causal_sequence, event.observed_at);
  }
  for (const I1ObservedRunLifecycleTransition& event :
       row.evidence.observations.run_quiescences) {
    if (event.edit_index >= kI1EditCount) {
      global_invalidate("Run-quiescence event has invalid edit index");
    }
    check_sequence_and_time(event.causal_sequence, event.observed_at);
  }
  for (const I1ObservedRunLifecycleTransition& event :
       row.evidence.observations.resource_settlements) {
    if (event.edit_index >= kI1EditCount) {
      global_invalidate("resource-settlement event has invalid edit index");
    }
    check_sequence_and_time(event.causal_sequence, event.observed_at);
  }
  for (const I1ObservedHostSettlement& event :
       row.evidence.observations.host_settlements) {
    if (event.edit_index >= kI1EditCount) {
      global_invalidate("Host-settlement event has invalid edit index");
    }
    check_sequence_and_time(event.causal_sequence, event.observed_at);
  }

  std::set<std::uint64_t> committed_runs;
  for (const I1ObservedVisibleOutput& visible :
       row.evidence.observations.visible_outputs) {
    committed_runs.insert(visible.run_id);
  }
  for (const I1ObservedServiceStart& start :
       row.evidence.observations.service_starts) {
    if (start.edit_index >= kI1EditCount) {
      global_invalidate("service start has invalid edit index");
      waste_evidence_valid = false;
      continue;
    }
    const I1EditEvidence& edit = row.evidence.edits[start.edit_index];
    if (start.quality != compute::ComputeRunQuality::Full ||
        start.qos.service_class != compute::ComputeRunQosClass::Interactive ||
        start.qos.weight != 1U ||
        start.qos.maximum_parallelism != std::optional<std::uint32_t>{8U} ||
        start.qos.deadline != edit.deadline) {
      global_invalidate("physical service start QoS/quality drifted");
    }
    if (!checked_add_service(&row.service.all_started_service,
                             start.service_charge)) {
      waste_invalidate("all-started service sum overflowed");
      continue;
    }
    bool discarded = committed_runs.count(start.run_id) == 0U;
    for (const I1ObservedServiceStart& candidate :
         row.evidence.observations.service_starts) {
      if (candidate.run_id == start.run_id &&
          candidate.local_task_id == start.local_task_id &&
          candidate.causal_sequence < start.causal_sequence) {
        discarded = true;
        break;
      }
    }
    if (discarded &&
        !checked_add_service(&row.service.discarded_started_service,
                             start.service_charge)) {
      waste_invalidate("discarded service sum overflowed");
    }
    const std::optional<std::uint64_t> cancellation_sequence =
        cancellation_sequence_for_run(row.evidence.observations, start.run_id);
    if (cancellation_sequence.has_value() &&
        start.causal_sequence > *cancellation_sequence &&
        !checked_add_service(&row.service.post_cancel_started_service,
                             start.service_charge)) {
      waste_invalidate("post-cancel service sum overflowed");
    }
  }
  set_service_ratio(&row.service);
  if (!row.service.discarded_ratio.has_value()) {
    waste_invalidate("no physically started service was observed");
  }

  std::optional<I1ObservedVisibleOutput> final_visible;
  for (const I1ObservedVisibleOutput& visible :
       row.evidence.observations.visible_outputs) {
    if (visible.edit_index == kI1EditCount - 1U) {
      if (final_visible.has_value()) {
        global_invalidate(
            "twelfth edit published more than one visible output");
      } else {
        final_visible = visible;
      }
    }
  }
  if (final_visible.has_value()) {
    for (const I1ObservedVisibleOutput& visible :
         row.evidence.observations.visible_outputs) {
      if (visible.edit_index != kI1EditCount - 1U &&
          visible.causal_sequence > final_visible->causal_sequence) {
        global_invalidate("an older edit published after the final edit");
      }
    }
  }
  if (!final_visible.has_value()) {
    latency_gate_failure = true;
    invalidate(&row.validity_reasons,
               "latency gate has no twelfth-edit visible publication");
    output_invalidate("twelfth edit has no Value to digest");
  } else {
    const I1EditEvidence& final_edit = row.evidence.edits.back();
    if (!final_edit.accepted_coordinate.has_value()) {
      latency_invalidate("twelfth edit lacks an accepted latency start");
    } else if (final_visible->observed_at <
               final_edit.accepted_coordinate->admission_time()) {
      latency_invalidate("final visibility precedes accepted admission");
    } else {
      row.final_latency = std::chrono::duration_cast<std::chrono::nanoseconds>(
          final_visible->observed_at -
          final_edit.accepted_coordinate->admission_time());
    }
    row.final_digest = compute_content_digest(final_visible->output);
    if (row.final_digest.state != ContentDigestState::Available ||
        !row.final_digest.digest.has_value() ||
        row.final_digest.digest->algorithm !=
            CanonicalDigestAlgorithm::Sha256CanonicalV1) {
      output_invalidate(
          "final logical ContentDigest is unavailable or invalid");
    }
  }

  const ResourceLedger::Snapshot& baseline_host =
      row.evidence.baseline.host_resources;
  const ResourceLedger::Snapshot& final_host =
      row.evidence.final_snapshot.host_resources;
  bool memory_limit_or_settlement_failure = false;
  if (baseline_host.limits != final_host.limits) {
    memory_invalidate("Host resource limits changed within the episode");
  }
  if (!resources_zero(baseline_host.reserved)) {
    memory_invalidate("pre-row Host reservation baseline is nonzero");
  }
  if (final_host.reserved != baseline_host.reserved) {
    memory_limit_or_settlement_failure = true;
  }
  if (!resources_not_less(baseline_host.high_water, final_host.high_water)) {
    memory_invalidate("Host lifetime high-water decreased");
  }
  if (!resources_fit(final_host.high_water, final_host.limits)) {
    memory_limit_or_settlement_failure = true;
  }

  const auto& baseline_devices = row.evidence.baseline.device_resources;
  const auto& final_devices = row.evidence.final_snapshot.device_resources;
  if (baseline_devices.size() != final_devices.size()) {
    memory_invalidate("configured device snapshot cardinality changed");
  } else {
    for (std::size_t index = 0U; index < baseline_devices.size(); ++index) {
      const ResourceLedger::DeviceSnapshot& before = baseline_devices[index];
      const ResourceLedger::DeviceSnapshot& after = final_devices[index];
      if (before.device != after.device || before.limits != after.limits) {
        memory_invalidate("configured device identity/limits changed");
        continue;
      }
      if (!device_resources_zero(before.reserved)) {
        memory_invalidate("pre-row device reservation baseline is nonzero");
      }
      if (after.reserved != before.reserved ||
          !device_resources_fit(after.high_water, after.limits)) {
        memory_limit_or_settlement_failure = true;
      }
      if (!device_resources_not_less(before.high_water, after.high_water)) {
        memory_invalidate("device lifetime high-water decreased");
      }
    }
  }

  const compute::ExecutionLifecyclePage& baseline_lifecycle =
      row.evidence.baseline.lifecycle;
  const compute::ExecutionLifecyclePage& final_lifecycle =
      row.evidence.final_snapshot.lifecycle;
  if (baseline_lifecycle.schema_version !=
          compute::kExecutionLifecycleTelemetrySchemaVersion ||
      final_lifecycle.schema_version !=
          compute::kExecutionLifecycleTelemetrySchemaVersion ||
      baseline_lifecycle.service_instance_id !=
          final_lifecycle.service_instance_id ||
      baseline_lifecycle.telemetry_epoch != final_lifecycle.telemetry_epoch) {
    memory_invalidate("lifecycle schema/service/epoch changed");
  }
  if (final_lifecycle.cursor_gap != 0U || final_lifecycle.has_more ||
      final_lifecycle.global_dropped_total !=
          baseline_lifecycle.global_dropped_total ||
      final_lifecycle.global_dropped_saturated !=
          baseline_lifecycle.global_dropped_saturated) {
    global_invalidate("lifecycle evidence has a gap, drop, or incomplete page");
    memory_evidence_valid = false;
  }
  if (!lifecycle_work_settled(baseline_lifecycle.counters)) {
    memory_invalidate("pre-row lifecycle baseline has active work");
  }
  if (!lifecycle_work_settled(final_lifecycle.counters) ||
      !lifecycle_counters_equal(baseline_lifecycle.counters,
                                final_lifecycle.counters)) {
    memory_limit_or_settlement_failure = true;
  }
  row.memory_settled =
      memory_evidence_valid && !memory_limit_or_settlement_failure;

  if (!global_valid) {
    row.latency_verdict = I1Verdict::Invalid;
    row.waste_verdict = I1Verdict::Invalid;
    row.memory_verdict = I1Verdict::Invalid;
    row.output_verdict = I1Verdict::Invalid;
    return row;
  }

  if (!latency_evidence_valid || !row.final_latency.has_value() ||
      !row.evidence.edits.back().deadline.has_value()) {
    row.latency_verdict = I1Verdict::Invalid;
  } else if (latency_gate_failure ||
             final_visible->observed_at > *row.evidence.edits.back().deadline ||
             *row.final_latency > kI1LatencyP99Limit) {
    row.latency_verdict = I1Verdict::Fail;
  } else {
    row.latency_verdict = I1Verdict::Pass;
  }

  if (!waste_evidence_valid || !row.service.discarded_ratio.has_value()) {
    row.waste_verdict = I1Verdict::Invalid;
  } else if (*row.service.discarded_ratio > kI1DiscardedServiceRatioLimit ||
             row.service.post_cancel_started_service != 0U) {
    row.waste_verdict = I1Verdict::Fail;
  } else {
    row.waste_verdict = I1Verdict::Pass;
  }

  if (!memory_evidence_valid) {
    row.memory_verdict = I1Verdict::Invalid;
  } else {
    row.memory_verdict =
        memory_limit_or_settlement_failure ? I1Verdict::Fail : I1Verdict::Pass;
  }

  if (!output_evidence_valid || !row.final_digest.digest.has_value()) {
    row.output_verdict = I1Verdict::Invalid;
  } else if (row.evidence.expected_final_digest.has_value() &&
             !(*row.final_digest.digest ==
               *row.evidence.expected_final_digest)) {
    row.output_verdict = I1Verdict::Fail;
  } else {
    row.output_verdict = I1Verdict::Pass;
  }
  return row;
}

/** @copydoc evaluate_i1_replicate */
I1ReplicateSummary evaluate_i1_replicate(
    const std::vector<I1EpisodeInnerRow>& rows) {
  I1ReplicateSummary summary;
  std::array<const I1EpisodeInnerRow*, kI1GridSlotCount> by_slot{};
  bool structure_valid = true;
  const auto structure_invalidate = [&](std::string reason) {
    structure_valid = false;
    invalidate(&summary.validity_reasons, std::move(reason));
  };

  if (rows.size() != kI1GridSlotCount) {
    structure_invalidate("replicate does not contain exactly 221 inner rows");
  }
  for (const I1EpisodeInnerRow& row : rows) {
    if (row.schema != kI1InnerRowSchema ||
        row.schema_version != kI1InnerRowSchemaVersion ||
        row.workload_id != kI1WorkloadId) {
      structure_invalidate("inner-row schema/version/workload id drifted");
      continue;
    }
    if (row.evidence.slot >= kI1GridSlotCount) {
      structure_invalidate("inner row has an out-of-range slot");
      continue;
    }
    if (by_slot[row.evidence.slot] != nullptr) {
      structure_invalidate("replicate contains a duplicate grid slot");
      continue;
    }
    by_slot[row.evidence.slot] = &row;
    if (summary.replicate_ordinal == 0U) {
      summary.replicate_ordinal = row.evidence.replicate_ordinal;
    } else if (summary.replicate_ordinal != row.evidence.replicate_ordinal) {
      structure_invalidate("replicate ordinal differs across rows");
    }
  }
  for (const I1EpisodeInnerRow* row : by_slot) {
    if (row == nullptr) {
      structure_invalidate("replicate is missing one or more grid slots");
      break;
    }
  }
  if (by_slot.front() != nullptr) {
    const auto grid_origin = by_slot.front()->evidence.grid_origin;
    const auto terminal_boundary = by_slot.front()->evidence.terminal_boundary;
    for (const I1EpisodeInnerRow* row : by_slot) {
      if (row != nullptr &&
          (row->evidence.grid_origin != grid_origin ||
           row->evidence.terminal_boundary != terminal_boundary)) {
        structure_invalidate("rows do not share one continuous grid");
        break;
      }
    }
  }

  bool latency_fail = false;
  bool latency_invalid = !structure_valid;
  bool waste_fail = false;
  bool waste_invalid = !structure_valid;
  bool memory_fail = false;
  bool memory_invalid = !structure_valid;
  bool output_fail = false;
  bool output_invalid = !structure_valid;
  std::vector<std::chrono::nanoseconds> measured_latencies;
  measured_latencies.reserve(kI1MeasuredSlotCount);
  std::optional<ContentDigest> cold_digest;

  for (std::size_t slot = 0U; slot < kI1GridSlotCount; ++slot) {
    const I1EpisodeInnerRow* row = by_slot[slot];
    if (row == nullptr) {
      continue;
    }
    accumulate_verdict(row->memory_verdict, &memory_fail, &memory_invalid);
    accumulate_verdict(row->output_verdict, &output_fail, &output_invalid);
    if (row->final_digest.digest.has_value()) {
      if (!cold_digest.has_value()) {
        cold_digest = row->final_digest.digest;
      } else if (!(*cold_digest == *row->final_digest.digest)) {
        output_fail = true;
      }
    }

    const I1EpisodePhase phase = classify_i1_slot(slot).first;
    if (phase != I1EpisodePhase::Measured) {
      if (row->latency_verdict == I1Verdict::Invalid) {
        latency_invalid = true;
      }
      if (row->waste_verdict == I1Verdict::Invalid) {
        waste_invalid = true;
      }
      continue;
    }
    accumulate_verdict(row->latency_verdict, &latency_fail, &latency_invalid);
    accumulate_verdict(row->waste_verdict, &waste_fail, &waste_invalid);
    if (row->final_latency.has_value()) {
      measured_latencies.push_back(*row->final_latency);
    }
    if (!checked_add_service(&summary.measured_service.all_started_service,
                             row->service.all_started_service) ||
        !checked_add_service(
            &summary.measured_service.discarded_started_service,
            row->service.discarded_started_service) ||
        !checked_add_service(
            &summary.measured_service.post_cancel_started_service,
            row->service.post_cancel_started_service)) {
      waste_invalid = true;
      invalidate(&summary.validity_reasons,
                 "measured service aggregate overflowed");
    }
  }

  summary.measured_sample_count = measured_latencies.size();
  if (measured_latencies.size() != kI1MeasuredSlotCount) {
    latency_invalid = true;
    invalidate(&summary.validity_reasons,
               "replicate lacks exactly 200 measured latency samples");
  } else {
    summary.latency = I1LatencyPercentiles{
        i1_nearest_rank(measured_latencies, 50U, 100U),
        i1_nearest_rank(measured_latencies, 95U, 100U),
        i1_nearest_rank(std::move(measured_latencies), 99U, 100U)};
    if (summary.latency->p50 > kI1LatencyP50Limit ||
        summary.latency->p95 > kI1LatencyP95Limit ||
        summary.latency->p99 > kI1LatencyP99Limit) {
      latency_fail = true;
    }
  }
  set_service_ratio(&summary.measured_service);
  if (!summary.measured_service.discarded_ratio.has_value()) {
    waste_invalid = true;
    invalidate(&summary.validity_reasons,
               "measured started-service denominator is zero");
  } else if (*summary.measured_service.discarded_ratio >
                 kI1DiscardedServiceRatioLimit ||
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
