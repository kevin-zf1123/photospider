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
  const bool cancellation = event.category == Category::ExplicitRequest ||
                            event.category == Category::Deadline ||
                            event.category == Category::Superseded ||
                            event.category == Category::GraphClose ||
                            event.category == Category::ProcessShutdown;
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
      return graph && !run && !group && generation && cancellation;
    case Kind::CandidateBegan:
      return graph && !run && !group && generation && none;
    case Kind::CandidateRolledBack:
      return graph && !run && !group && generation && (none || cancellation);
    case Kind::BundleAdmitted:
      return graph && run && generation && none;
    case Kind::CancellationRequested:
      return graph && !run && !group && generation && cancellation;
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
  return valid;
}

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

  if (!validate_m1_lifecycle_history(snapshots, !jobs.empty(),
                                     &row->validity_reasons)) {
    result.valid = false;
  }
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
