/**
 * @file b1_evidence_json.cpp
 * @brief Implements complete verification-only JSON encoding for B1 evidence.
 */
#include "verification/b1_evidence_json.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace ps::benchmark {
namespace {

using Json = nlohmann::json;

/**
 * @brief Converts one steady-clock point to its retained nanosecond tick.
 * @param value Process-local monotonic time.
 * @return Signed nanoseconds since the implementation-defined clock epoch.
 * @throws Nothing when steady-clock duration conversion is representable.
 */
std::int64_t monotonic_nanoseconds(
    std::chrono::steady_clock::time_point value) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             value.time_since_epoch())
      .count();
}

/**
 * @brief Encodes one optional unsigned scalar without a sentinel.
 * @tparam Integer Unsigned integral payload type.
 * @param value Optional scalar.
 * @return JSON unsigned integer or null.
 * @throws nlohmann allocation failures unchanged.
 */
template <typename Integer>
Json optional_unsigned_json(const std::optional<Integer>& value) {
  static_assert(std::is_integral_v<Integer> && std::is_unsigned_v<Integer>,
                "optional_unsigned_json requires an unsigned integer");
  return value.has_value() ? Json(*value) : Json(nullptr);
}

/**
 * @brief Encodes one optional floating-point result without a sentinel.
 * @param value Optional scalar.
 * @return JSON number or null.
 * @throws nlohmann allocation failures unchanged.
 */
Json optional_double_json(const std::optional<double>& value) {
  return value.has_value() ? Json(*value) : Json(nullptr);
}

/**
 * @brief Returns lowercase hexadecimal for one typed logical digest.
 * @param digest Exact canonical digest bytes.
 * @return Sixty-four lowercase hexadecimal characters.
 * @throws std::bad_alloc when output ownership cannot allocate.
 */
std::string content_digest_hex(const ContentDigest& digest) {
  constexpr std::array<char, 16U> kHex{'0', '1', '2', '3', '4', '5', '6', '7',
                                       '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string result;
  result.reserve(kCanonicalDigestBytes * 2U);
  for (const std::byte byte_value : digest.bytes) {
    const auto value = static_cast<std::uint8_t>(byte_value);
    result.push_back(kHex[value >> 4U]);
    result.push_back(kHex[value & 0x0fU]);
  }
  return result;
}

/**
 * @brief Encodes one typed logical digest.
 * @param digest Exact digest value.
 * @return Algorithm plus lowercase bytes.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json content_digest_json(const ContentDigest& digest) {
  return Json{{"algorithm", static_cast<std::uint32_t>(digest.algorithm)},
              {"lowercase_hex", content_digest_hex(digest)}};
}

/**
 * @brief Encodes one complete typed logical digest result.
 * @param result Available or typed unavailable outcome.
 * @return State, optional digest, and diagnostic.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json content_digest_result_json(const ContentDigestResult& result) {
  return Json{
      {"state", static_cast<std::uint32_t>(result.state)},
      {"digest", result.digest.has_value() ? content_digest_json(*result.digest)
                                           : Json(nullptr)},
      {"diagnostic", result.diagnostic}};
}

/**
 * @brief Encodes one raw SHA-256 domain identity.
 * @param digest Exact 256-bit digest.
 * @return Algorithm plus lowercase bytes.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json sha256_json(const B1Sha256Digest& digest) {
  return Json{{"algorithm", "sha256"},
              {"lowercase_hex", b1_digest_hex(digest)}};
}

/**
 * @brief Encodes one complete immutable occurrence identity.
 * @param job Valid B1 job occurrence.
 * @return Six-component identity object.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json job_json(const B1JobInstance& job) {
  return Json{{"row_workload_id", job.row_workload_id},
              {"replicate_ordinal", job.replicate_ordinal},
              {"phase", b1_job_phase_name(job.phase)},
              {"cycle_ordinal", job.cycle_ordinal},
              {"job_index", job.job_index},
              {"graph", b1_graph_role_name(b1_graph_for_job(job.job_index))},
              {"run_cap", job.run_cap},
              {"canonical_identity", encode_b1_job_instance(job)}};
}

/**
 * @brief Encodes one exact Compute Run QoS value.
 * @param qos Product-observed QoS.
 * @return Service class, optional deadline/cap, and weight.
 * @throws nlohmann allocation failures unchanged.
 */
Json qos_json(const compute::ComputeRunQos& qos) {
  return Json{
      {"service_class", static_cast<std::uint32_t>(qos.service_class)},
      {"deadline_ns", qos.deadline.has_value()
                          ? Json(monotonic_nanoseconds(*qos.deadline))
                          : Json(nullptr)},
      {"weight", qos.weight},
      {"maximum_parallelism", optional_unsigned_json(qos.maximum_parallelism)}};
}

/**
 * @brief Encodes one observer-local causal coordinate.
 * @param coordinate Exact observed time and sequence.
 * @return Two-field monotonic coordinate.
 * @throws nlohmann allocation failures unchanged.
 */
Json coordinate_json(
    const compute::ComputeRunObservationCoordinate& coordinate) {
  return Json{{"observed_at_ns", monotonic_nanoseconds(coordinate.observed_at)},
              {"causal_sequence", coordinate.causal_sequence}};
}

/**
 * @brief Encodes one Host resource vector.
 * @param value Complete resource dimensions.
 * @return Stable resource object.
 * @throws nlohmann allocation failures unchanged.
 */
Json resource_vector_json(const ResourceVector& value) {
  return Json{{"cpu_slots", value.cpu_slots},
              {"retained_memory_bytes", value.retained_memory_bytes},
              {"scratch_bytes", value.scratch_bytes},
              {"ready_entries", value.ready_entries},
              {"ready_bytes", value.ready_bytes}};
}

/**
 * @brief Encodes one device resource vector.
 * @param value Complete persistent/scratch byte facts.
 * @return Stable device-resource object.
 * @throws nlohmann allocation failures unchanged.
 */
Json device_resource_vector_json(const DeviceResourceVector& value) {
  return Json{{"device_memory_bytes", value.device_memory_bytes},
              {"device_scratch_bytes", value.device_scratch_bytes}};
}

/**
 * @brief Encodes one concrete process device identity.
 * @param value Complete backend plus ordinal.
 * @return Stable numeric device object.
 * @throws nlohmann allocation failures unchanged.
 */
Json device_id_json(DeviceId value) {
  return Json{{"backend", static_cast<std::uint32_t>(value.backend())},
              {"ordinal", value.ordinal()}};
}

/**
 * @brief Encodes every lifecycle counter.
 * @param value Complete atomic-cut counters.
 * @return Closed counter object.
 * @throws nlohmann allocation failures unchanged.
 */
Json lifecycle_counters_json(const compute::ExecutionLifecycleCounters& value) {
  return Json{
      {"registered_graph_count", value.registered_graph_count},
      {"open_graph_count", value.open_graph_count},
      {"closing_graph_count", value.closing_graph_count},
      {"pending_candidate_count", value.pending_candidate_count},
      {"admitted_standalone_run_count", value.admitted_standalone_run_count},
      {"admitted_run_group_count", value.admitted_run_group_count},
      {"admitted_child_run_count", value.admitted_child_run_count},
      {"terminal_not_quiescent_run_count",
       value.terminal_not_quiescent_run_count},
      {"finalizing_run_count", value.finalizing_run_count},
      {"ready_entry_count", value.ready_entry_count},
      {"entered_callback_count", value.entered_callback_count},
      {"live_root_reservation_count", value.live_root_reservation_count},
      {"live_child_grant_count", value.live_child_grant_count},
      {"live_policy_invocation_count", value.live_policy_invocation_count},
      {"live_policy_binding_count", value.live_policy_binding_count}};
}

/**
 * @brief Encodes one lifecycle transition record.
 * @param value Fixed-size version-one transition.
 * @return Complete scalar event object.
 * @throws nlohmann allocation failures unchanged.
 */
Json lifecycle_event_json(const compute::ExecutionLifecycleEvent& value) {
  return Json{{"schema_version", value.schema_version},
              {"sequence", value.sequence},
              {"timestamp_us", value.timestamp_us},
              {"timestamp_saturated", value.timestamp_saturated},
              {"service_instance_id", value.service_instance_id},
              {"telemetry_epoch", value.telemetry_epoch},
              {"graph_instance_id", value.graph_instance_id},
              {"run_id", value.run_id},
              {"run_group_id", value.run_group_id},
              {"generation", value.generation},
              {"kind", static_cast<std::uint32_t>(value.kind)},
              {"category", static_cast<std::uint32_t>(value.category)},
              {"counters", lifecycle_counters_json(value.counters)}};
}

/**
 * @brief Encodes one complete lifecycle page.
 * @param value Bounded atomic-cut page.
 * @return Page metadata, counters, and every retained event.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json lifecycle_page_json(const compute::ExecutionLifecyclePage& value) {
  Json records = Json::array();
  for (const compute::ExecutionLifecycleEvent& event : value.records) {
    records.push_back(lifecycle_event_json(event));
  }
  return Json{
      {"schema_version", value.schema_version},
      {"capacity", value.capacity},
      {"service_instance_id", value.service_instance_id},
      {"telemetry_epoch", value.telemetry_epoch},
      {"service_state", static_cast<std::uint32_t>(value.service_state)},
      {"shutdown_generation", value.shutdown_generation},
      {"snapshot_cut", value.snapshot_cut},
      {"first_retained_sequence", value.first_retained_sequence},
      {"next_sequence", value.next_sequence},
      {"global_dropped_total", value.global_dropped_total},
      {"global_dropped_saturated", value.global_dropped_saturated},
      {"counters", lifecycle_counters_json(value.counters)},
      {"records", std::move(records)},
      {"cursor_gap", value.cursor_gap},
      {"next_cursor", value.next_cursor},
      {"has_more", value.has_more}};
}

/**
 * @brief Encodes one process Compute I/O snapshot.
 * @param value Complete budget and phase state.
 * @return Closed counter/state object.
 * @throws nlohmann allocation failures unchanged.
 */
Json compute_io_snapshot_json(
    const execution::ComputeIoExecutorSnapshot& value) {
  return Json{{"task_limit", value.task_limit},
              {"planned_bytes_limit", value.planned_bytes_limit},
              {"active_tasks", value.active_tasks},
              {"active_planned_bytes", value.active_planned_bytes},
              {"constructing_tasks", value.constructing_tasks},
              {"queued_tasks", value.queued_tasks},
              {"running_tasks", value.running_tasks},
              {"accepting", value.accepting},
              {"shutdown_complete", value.shutdown_complete}};
}

/**
 * @brief Encodes one authoritative B1 execution/resource snapshot.
 * @param value Complete Host/device/lifecycle/I/O cut.
 * @return Closed snapshot object.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json execution_snapshot_json(const B1ExecutionSnapshot& value) {
  Json devices = Json::array();
  for (const ResourceLedger::DeviceSnapshot& device : value.device_resources) {
    devices.push_back(
        Json{{"device", device_id_json(device.device)},
             {"limits", device_resource_vector_json(device.limits)},
             {"reserved", device_resource_vector_json(device.reserved)},
             {"high_water", device_resource_vector_json(device.high_water)},
             {"available", device_resource_vector_json(device.available)}});
  }
  return Json{
      {"host_resources",
       Json{{"limits", resource_vector_json(value.host_resources.limits)},
            {"reserved", resource_vector_json(value.host_resources.reserved)},
            {"high_water",
             resource_vector_json(value.host_resources.high_water)}}},
      {"device_resources", std::move(devices)},
      {"lifecycle", lifecycle_page_json(value.lifecycle)},
      {"compute_io", compute_io_snapshot_json(value.compute_io)}};
}

/**
 * @brief Encodes one optional physical transition.
 * @param value Optional Run transition.
 * @return Run/coordinate object or null.
 * @throws nlohmann allocation failures unchanged.
 */
Json optional_transition_json(
    const std::optional<B1ObservedRunTransition>& value) {
  if (!value.has_value()) {
    return nullptr;
  }
  return Json{{"run_id", value->run_id},
              {"coordinate", coordinate_json(value->coordinate)}};
}

/**
 * @brief Encodes one complete request-scoped physical trace.
 * @param value Frozen callback observation snapshot.
 * @return Closed occurrence/lifecycle/service trace.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json physical_trace_json(const B1RunObservationSnapshot& value) {
  Json generations = Json::array();
  for (const B1ObservedCurrentGeneration& generation :
       value.current_generations) {
    generations.push_back(
        Json{{"generation", generation.generation},
             {"coordinate", coordinate_json(generation.coordinate)}});
  }
  Json starts = Json::array();
  for (const B1ObservedServiceStart& start : value.service_starts) {
    starts.push_back(Json{{"run_id", start.run_id},
                          {"local_task_id", start.local_task_id},
                          {"service_charge", start.service_charge},
                          {"qos", qos_json(start.qos)},
                          {"coordinate", coordinate_json(start.coordinate)}});
  }
  Json cancellations = Json::array();
  for (const B1ObservedCancellation& cancellation : value.cancellations) {
    cancellations.push_back(
        Json{{"run_id", cancellation.run_id},
             {"reason", static_cast<std::uint32_t>(cancellation.reason)},
             {"coordinate", coordinate_json(cancellation.coordinate)}});
  }
  return Json{
      {"job", job_json(value.job)},
      {"overflowed", value.overflowed},
      {"current_generations", std::move(generations)},
      {"service_starts", std::move(starts)},
      {"cancellations", std::move(cancellations)},
      {"terminal_kind",
       value.terminal_kind.has_value()
           ? Json(static_cast<std::uint32_t>(*value.terminal_kind))
           : Json(nullptr)},
      {"terminal", optional_transition_json(value.terminal)},
      {"visible", optional_transition_json(value.visible)},
      {"quiescent", optional_transition_json(value.quiescent)},
      {"resource_settled", optional_transition_json(value.resource_settled)},
      {"visible_content_digest",
       content_digest_result_json(value.visible_content_digest)}};
}

/**
 * @brief Encodes one optional B1 Compute I/O task identity.
 * @param value Optional task identity.
 * @return Job/stage/attempt object or null.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json optional_io_task_json(const std::optional<B1IoTaskIdentity>& value) {
  if (!value.has_value()) {
    return nullptr;
  }
  return Json{{"job", job_json(value->job)},
              {"stage", b1_io_stage_name(value->stage)},
              {"attempt", value->attempt}};
}

/**
 * @brief Encodes one complete B1 Compute I/O observation.
 * @param value Event-aligned admission/accounting record.
 * @return Closed event object.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json io_observation_json(const B1ComputeIoObservation& value) {
  return Json{
      {"point", static_cast<std::uint32_t>(value.point)},
      {"task", optional_io_task_json(value.task)},
      {"planned_bytes", value.planned_bytes},
      {"admission", value.admission.has_value()
                        ? Json(static_cast<std::uint32_t>(*value.admission))
                        : Json(nullptr)},
      {"completion", value.completion.has_value()
                         ? Json(static_cast<std::uint32_t>(*value.completion))
                         : Json(nullptr)},
      {"snapshot", compute_io_snapshot_json(value.snapshot)}};
}

/**
 * @brief Encodes one optional successful durable receipt.
 * @param value Optional immutable receipt.
 * @return Complete receipt object or null.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json optional_receipt_json(const std::optional<B1OutputCommitReceipt>& value) {
  if (!value.has_value()) {
    return nullptr;
  }
  return Json{
      {"commit_id", value->commit_id},
      {"resolved_root", value->resolved_root.string()},
      {"rooted_slot", value->rooted_slot.generic_string()},
      {"job", job_json(value->job)},
      {"logical_descriptor", value->logical_descriptor},
      {"logical_content_digest",
       content_digest_json(value->logical_content_digest)},
      {"committed_generation", value->committed_generation},
      {"payload_name", value->payload_name},
      {"manifest_name", value->manifest_name},
      {"payload_length", value->payload_length},
      {"manifest_length", value->manifest_length},
      {"payload_digest", sha256_json(value->payload_digest)},
      {"manifest_digest", sha256_json(value->manifest_digest)},
      {"requested_durability",
       static_cast<std::uint32_t>(value->requested_durability)},
      {"achieved_durability",
       static_cast<std::uint32_t>(value->achieved_durability)},
      {"published_manifest_identity", value->published_manifest_identity}};
}

/**
 * @brief Encodes one complete B1 output result and event stream.
 * @param value Typed terminal output outcome.
 * @return Status, diagnostic, optional receipt, and all observations.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json output_result_json(const B1OutputCommitResult& value) {
  Json observations = Json::array();
  for (const B1ComputeIoObservation& observation : value.io_observations) {
    observations.push_back(io_observation_json(observation));
  }
  return Json{{"status", static_cast<std::uint32_t>(value.status)},
              {"diagnostic", value.diagnostic},
              {"receipt", optional_receipt_json(value.receipt)},
              {"io_observations", std::move(observations)}};
}

/**
 * @brief Encodes one independent immutable job golden.
 * @param value Exact logical/raw expected identities.
 * @return Job-indexed golden object.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json golden_json(const B1JobGolden& value) {
  return Json{{"job_index", value.job_index},
              {"logical_digest", content_digest_json(value.logical_digest)},
              {"raw_payload_digest", sha256_json(value.raw_payload_digest)}};
}

/**
 * @brief Encodes one complete cold/warmup/measured job evidence record.
 * @param value Raw occurrence evidence.
 * @return Closed job object.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json job_evidence_json(const B1JobEvidence& value) {
  return Json{
      {"job", job_json(value.job)},
      {"producer_offer_ordinal", value.producer_offer_ordinal},
      {"offered_at_ns", monotonic_nanoseconds(value.offered_at)},
      {"endpoint_at_ns", monotonic_nanoseconds(value.endpoint_at)},
      {"run_succeeded", value.run_succeeded},
      {"physical_trace", physical_trace_json(value.physical_trace)},
      {"execution_before", execution_snapshot_json(value.execution_before)},
      {"execution_after", execution_snapshot_json(value.execution_after)},
      {"output", output_result_json(value.output)},
      {"golden", golden_json(value.golden)},
      {"semantic_trace", value.semantic_trace},
      {"semantic_trace_digest", sha256_json(value.semantic_trace_digest)}};
}

/**
 * @brief Encodes one optional storage eligibility result.
 * @param value Optional exact truth-set evaluation.
 * @return Eligible/reasons object or null.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json optional_eligibility_json(
    const std::optional<B1StorageEligibility>& value) {
  return value.has_value()
             ? Json{{"eligible", value->eligible}, {"reasons", value->reasons}}
             : Json(nullptr);
}

/**
 * @brief Encodes one complete B1 environment evidence value.
 * @param value Exact canonical bytes, claims, eligibility, and identities.
 * @return Closed environment object.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json environment_json(const B1EnvironmentEvidence& value) {
  return Json{{"base_manifest", value.base_manifest},
              {"claimed_base_digest", sha256_json(value.claimed_base_digest)},
              {"storage_manifest", value.storage_manifest.has_value()
                                       ? Json(*value.storage_manifest)
                                       : Json(nullptr)},
              {"claimed_storage_digest",
               value.claimed_storage_digest.has_value()
                   ? sha256_json(*value.claimed_storage_digest)
                   : Json(nullptr)},
              {"environment_class_manifest", value.environment_class_manifest},
              {"claimed_environment_class_digest",
               sha256_json(value.claimed_environment_class_digest)},
              {"storage_eligibility",
               optional_eligibility_json(value.storage_eligibility)},
              {"workload_id", value.workload_id},
              {"fixture_digest", sha256_json(value.fixture_digest)},
              {"resource_identity", sha256_json(value.resource_identity)},
              {"run_cap", value.run_cap},
              {"replicate_ordinal", value.replicate_ordinal}};
}

/**
 * @brief Returns a stable token for one independent verdict.
 * @param value Typed verdict.
 * @return `pass`, `fail`, or `invalid`.
 * @throws Nothing.
 */
const char* verdict_text(I1Verdict value) noexcept {
  switch (value) {
    case I1Verdict::Pass:
      return "pass";
    case I1Verdict::Fail:
      return "fail";
    case I1Verdict::Invalid:
      return "invalid";
  }
  return "invalid";
}

}  // namespace

nlohmann::json b1_workload_contract_json() {
  return Json{
      {"workload_id", kB1WorkloadId},
      {"image", Json{{"width", kB1ImageEdge},
                     {"height", kB1ImageEdge},
                     {"channels", kB1ChannelCount},
                     {"type", "fp32"},
                     {"device", "cpu"}}},
      {"graph_count", 2U},
      {"curve_coefficients", Json::array({0.80, 1.00, 1.20, 1.40})},
      {"tasks_per_job", kB1TasksPerJob},
      {"curve_tiles_per_stage", kB1TilesPerCurveStage},
      {"curve_tile_bytes", kB1CurveTileBytes},
      {"payload_bytes", kB1PayloadBytes},
      {"compute_io_task_limit", kB1ComputeIoTaskLimit},
      {"compute_io_planned_byte_limit", kB1ComputeIoPlannedByteLimit},
      {"cold_job_index", kB1ColdJobIndex},
      {"warmup_job_indices", kB1WarmupJobIndices},
      {"measured_job_indices",
       Json::array({0U,  1U,  2U,  3U,  4U,  5U,  6U,  7U,  8U,  9U,
                    10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U, 19U,
                    20U, 21U, 22U, 23U, 24U, 25U, 26U, 27U, 28U, 29U})},
      {"run_caps", kB1RunCaps},
      {"replicate_count", kB1ReplicateCount},
      {"site_operations_per_job", kB1SiteOperationsPerJob},
      {"candidate_reference_median_ratio_floor", kB1MedianThroughputRatioLimit},
      {"candidate_reference_every_replicate_ratio_floor",
       kB1MinimumThroughputRatioLimit},
      {"outer_canonical_envelope_claim", false}};
}

nlohmann::json b1_inner_row_json(const B1InnerRow& row) {
  Json jobs = Json::array();
  for (const B1JobEvidence& job : row.evidence.jobs) {
    jobs.push_back(job_evidence_json(job));
  }
  return Json{
      {"schema", row.schema},
      {"schema_version", row.schema_version},
      {"workload_id", row.workload_id},
      {"evidence",
       Json{{"replicate_ordinal", row.evidence.replicate_ordinal},
            {"run_cap", row.evidence.run_cap},
            {"environment", environment_json(row.evidence.environment)},
            {"measurement_start_ns",
             monotonic_nanoseconds(row.evidence.measurement_start)},
            {"measurement_end_ns",
             monotonic_nanoseconds(row.evidence.measurement_end)},
            {"initial_snapshot",
             execution_snapshot_json(row.evidence.initial_snapshot)},
            {"final_snapshot",
             execution_snapshot_json(row.evidence.final_snapshot)},
            {"jobs", std::move(jobs)}}},
      {"verified_measured_jobs", row.verified_measured_jobs},
      {"successful_site_operations", row.successful_site_operations},
      {"throughput_mpix_ops_per_second",
       optional_double_json(row.throughput_mpix_ops_per_second)},
      {"logical_golden_mismatches", row.logical_golden_mismatches},
      {"raw_golden_mismatches", row.raw_golden_mismatches},
      {"semantic_trace_mismatches", row.semantic_trace_mismatches},
      {"artifact_mismatches", row.artifact_mismatches},
      {"all_started_service", row.all_started_service},
      {"discarded_started_service", row.discarded_started_service},
      {"post_cancellation_started_service",
       row.post_cancellation_started_service},
      {"discarded_started_service_ratio",
       optional_double_json(row.discarded_started_service_ratio)},
      {"duplicate_service_starts", row.duplicate_service_starts},
      {"retry_service_starts", row.retry_service_starts},
      {"compute_io_task_high_water", row.compute_io_task_high_water},
      {"compute_io_planned_byte_high_water",
       row.compute_io_planned_byte_high_water},
      {"validity_reasons", row.validity_reasons},
      {"verdicts", Json{{"throughput", verdict_text(row.throughput_verdict)},
                        {"determinism", verdict_text(row.determinism_verdict)},
                        {"waste", verdict_text(row.waste_verdict)},
                        {"memory", verdict_text(row.memory_verdict)}}},
      {"outer_canonical_envelope_claim", false}};
}

}  // namespace ps::benchmark
