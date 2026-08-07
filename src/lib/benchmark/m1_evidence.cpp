/**
 * @file m1_evidence.cpp
 * @brief Implements the fail-closed five-axis M1 inner-row evaluator.
 */
#include "benchmark/m1_evidence.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
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

/**
 * @brief Validates temporal snapshots and event-derived Compute I/O evidence.
 * @param snapshots Chronological same-service samples including final cut.
 * @param offers Complete immutable B1 protocol offer set.
 * @param jobs Exact-one complete B1 evidence for every offer.
 * @param row Mutable result receiving high-water values and diagnostics.
 * @return Structural validity plus independently complete limit/settlement
 * outcomes.
 * @throws std::bad_alloc when diagnostics allocate.
 */
M1MemoryValidation validate_m1_memory(
    const std::vector<M1ExecutionSnapshot>& snapshots,
    const std::vector<M1BatchOfferEvidence>& offers,
    const std::vector<B1JobEvidence>& jobs, M1InnerRow* row) {
  M1MemoryValidation result;
  const auto invalid = [&result, row](std::string reason) {
    result.valid = false;
    invalidate_m1(&row->validity_reasons, "M1 memory evidence: " + reason);
  };
  if (snapshots.size() < 4U) {
    invalid("fewer than four boundary/final snapshots");
    return result;
  }

  std::map<B1JobInstance, const B1JobEvidence*> indexed_jobs;
  for (const B1JobEvidence& job : jobs) {
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
    const B1JobEvidence& job = *found->second;
    if (job.producer_offer_ordinal != offer.producer_offer_ordinal ||
        job.offered_at != offer.offered.timestamp ||
        !offer.endpoint.has_value() ||
        job.endpoint_at != offer.endpoint->timestamp) {
      invalid("B1 I/O stream identity or endpoint differs from its offer");
    }
    const B1ComputeIoEvaluation io = evaluate_b1_compute_io_evidence(job);
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
    for (const B1ComputeIoObservation& observation :
         job.output.io_observations) {
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

  const std::uint64_t service_id =
      snapshots.front().lifecycle.service_instance_id;
  const std::uint64_t telemetry_epoch =
      snapshots.front().lifecycle.telemetry_epoch;
  ResourceVector prior_high_water = snapshots.front().host_resources.high_water;
  const std::vector<ResourceLedger::DeviceSnapshot>& initial_devices =
      snapshots.front().device_resources;

  for (const M1ExecutionSnapshot& snapshot : snapshots) {
    if (snapshot.host_resources.limits != kM1HostLimits ||
        snapshot.throughput.capacity != kM1ThroughputCapacity) {
      invalid("Host limits or Throughput headroom capacity drifted");
    }
    if (!resources_fit(snapshot.host_resources.reserved,
                       snapshot.host_resources.limits) ||
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
        if (!device_resources_fit(current.reserved, current.limits) ||
            !device_resources_fit(current.high_water, current.limits)) {
          result.within_limits = false;
        }
        if (current.device.backend() == DeviceBackend::Metal &&
            current.limits != kM1MetalLimits) {
          invalid("configured Metal limits differ from the frozen profile");
        }
      }
    }

    const compute::ExecutionLifecyclePage& lifecycle = snapshot.lifecycle;
    if (lifecycle.schema_version !=
            compute::kExecutionLifecycleTelemetrySchemaVersion ||
        lifecycle.capacity != compute::kExecutionLifecycleTelemetryCapacity ||
        lifecycle.service_instance_id == 0U ||
        lifecycle.service_instance_id != service_id || telemetry_epoch == 0U ||
        lifecycle.telemetry_epoch != telemetry_epoch ||
        !known_service_state(lifecycle.service_state) ||
        lifecycle.global_dropped_total != 0U ||
        lifecycle.global_dropped_saturated || lifecycle.cursor_gap != 0U ||
        lifecycle.has_more) {
      invalid("lifecycle schema, identity, enum, or losslessness drifted");
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

/** @copydoc evaluate_m1_inner_row */
M1InnerRow evaluate_m1_inner_row(M1InnerRowInput input) {
  M1InnerRow row;
  row.evidence = std::move(input);
  row.protocol = evaluate_m1_protocol(row.evidence.protocol);
  row.validity_reasons = row.protocol.validity_reasons;

  if (row.evidence.replicate_ordinal == 0U ||
      row.evidence.replicate_ordinal > 3U ||
      row.evidence.protocol.replicate_ordinal !=
          row.evidence.replicate_ordinal) {
    invalidate_m1(&row.validity_reasons,
                  "M1 inner row replicate identity drifted");
  }
  const bool protocol_valid =
      row.protocol.verdict == I1Verdict::Pass &&
      row.evidence.replicate_ordinal != 0U &&
      row.evidence.replicate_ordinal <= 3U &&
      row.evidence.protocol.replicate_ordinal == row.evidence.replicate_ordinal;
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

  const M1MemoryValidation memory = validate_m1_memory(
      row.evidence.temporal_snapshots, row.evidence.protocol.batch_offers,
      row.evidence.batch_jobs, &row);
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
