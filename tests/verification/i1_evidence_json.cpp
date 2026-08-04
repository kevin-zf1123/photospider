/**
 * @file i1_evidence_json.cpp
 * @brief Implements verification-only JSON encoding for closed I1 evidence.
 */
#include "verification/i1_evidence_json.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace ps::benchmark {
namespace {

using Json = nlohmann::json;

/**
 * @brief Converts a steady-clock point to its retained signed nanosecond tick.
 * @param value Point in the runner process monotonic domain.
 * @return Nanoseconds since the implementation-defined steady-clock epoch.
 * @throws Nothing when the platform steady-clock duration is representable.
 */
std::int64_t monotonic_nanoseconds(
    std::chrono::steady_clock::time_point value) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             value.time_since_epoch())
      .count();
}

/**
 * @brief Encodes an optional monotonic point without an implicit sentinel.
 * @param value Optional process monotonic time.
 * @return JSON integer when present or JSON null when absent.
 * @throws nlohmann allocation errors unchanged.
 */
Json optional_monotonic_json(
    const std::optional<std::chrono::steady_clock::time_point>& value) {
  return value.has_value() ? Json(monotonic_nanoseconds(*value))
                           : Json(nullptr);
}

/**
 * @brief Returns lowercase hexadecimal for one canonical content digest.
 * @param digest Exact typed SHA-256 bytes.
 * @return Sixty-four lowercase hexadecimal characters.
 * @throws std::bad_alloc when result ownership cannot allocate.
 */
std::string digest_hex(const ContentDigest& digest) {
  constexpr std::array<char, 16U> kHexDigits{'0', '1', '2', '3', '4', '5',
                                             '6', '7', '8', '9', 'a', 'b',
                                             'c', 'd', 'e', 'f'};
  std::string result;
  result.reserve(kCanonicalDigestBytes * 2U);
  for (const std::byte byte_value : digest.bytes) {
    const auto value = static_cast<std::uint8_t>(byte_value);
    result.push_back(kHexDigits[value >> 4U]);
    result.push_back(kHexDigits[value & 0x0fU]);
  }
  return result;
}

/**
 * @brief Encodes one typed content-digest result with explicit state.
 * @param result Complete digest outcome.
 * @return Closed JSON object retaining state, algorithm, bytes, diagnostic.
 * @throws nlohmann/std allocation errors unchanged.
 */
Json content_digest_json(const ContentDigestResult& result) {
  std::string state;
  switch (result.state) {
    case ContentDigestState::Available:
      state = "available";
      break;
    case ContentDigestState::MissingProvider:
      state = "missing-provider";
      break;
    case ContentDigestState::UnsupportedSchemaVersion:
      state = "unsupported-schema-version";
      break;
    case ContentDigestState::InvalidDescriptor:
      state = "invalid-descriptor";
      break;
    case ContentDigestState::PayloadUnavailable:
      state = "payload-unavailable";
      break;
    case ContentDigestState::ProviderFailure:
      state = "provider-failure";
      break;
  }
  Json digest = nullptr;
  if (result.digest.has_value()) {
    digest = Json{{"algorithm", "sha256-canonical-v1"},
                  {"lowercase_hex", digest_hex(*result.digest)}};
  }
  return Json{{"state", state},
              {"digest", std::move(digest)},
              {"diagnostic", result.diagnostic}};
}

/**
 * @brief Encodes one public operation status without discarding diagnostics.
 * @param status Exact Host scheduling or settlement status.
 * @return Closed JSON status object.
 * @throws nlohmann allocation errors unchanged.
 */
Json operation_status_json(const OperationStatus& status) {
  return Json{{"ok", status.ok},
              {"domain", static_cast<std::uint32_t>(status.domain)},
              {"code", status.code},
              {"name", status.name},
              {"message", status.message}};
}

/**
 * @brief Encodes one Host resource vector with stable dimension names.
 * @param value Complete authoritative vector.
 * @return Closed JSON resource object.
 * @throws nlohmann allocation errors unchanged.
 */
Json resource_vector_json(const ResourceVector& value) {
  return Json{{"cpu_slots", value.cpu_slots},
              {"retained_memory_bytes", value.retained_memory_bytes},
              {"scratch_bytes", value.scratch_bytes},
              {"ready_entries", value.ready_entries},
              {"ready_bytes", value.ready_bytes}};
}

/**
 * @brief Encodes one device resource vector with stable dimension names.
 * @param value Complete authoritative device vector.
 * @return Closed JSON device-resource object.
 * @throws nlohmann allocation errors unchanged.
 */
Json device_resource_vector_json(const DeviceResourceVector& value) {
  return Json{{"device_memory_bytes", value.device_memory_bytes},
              {"device_scratch_bytes", value.device_scratch_bytes}};
}

/**
 * @brief Encodes one independent I1 verdict token.
 * @param verdict Typed verdict.
 * @return `pass`, `fail`, or `invalid`.
 * @throws Nothing.
 */
const char* verdict_text(I1Verdict verdict) noexcept {
  switch (verdict) {
    case I1Verdict::Pass:
      return "pass";
    case I1Verdict::Fail:
      return "fail";
    case I1Verdict::Invalid:
      return "invalid";
  }
  return "invalid";
}

/**
 * @brief Encodes every execution-lifecycle counter at one atomic cut.
 * @param value Complete version-one counter set.
 * @return Closed JSON counter object.
 * @throws nlohmann allocation errors unchanged.
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
      {"live_policy_binding_count", value.live_policy_binding_count},
  };
}

/**
 * @brief Encodes one complete lifecycle telemetry page including raw records.
 * @param page Atomic-cut page returned by the source-private Host seam.
 * @return Closed JSON page retaining gaps, drops, counters, and records.
 * @throws nlohmann/std allocation errors unchanged.
 */
Json lifecycle_page_json(const compute::ExecutionLifecyclePage& page) {
  Json records = Json::array();
  for (const compute::ExecutionLifecycleEvent& event : page.records) {
    records.push_back(Json{
        {"schema_version", event.schema_version},
        {"sequence", event.sequence},
        {"timestamp_us", event.timestamp_us},
        {"timestamp_saturated", event.timestamp_saturated},
        {"service_instance_id", event.service_instance_id},
        {"telemetry_epoch", event.telemetry_epoch},
        {"graph_instance_id", event.graph_instance_id},
        {"run_id", event.run_id},
        {"run_group_id", event.run_group_id},
        {"generation", event.generation},
        {"kind", static_cast<std::uint32_t>(event.kind)},
        {"category", static_cast<std::uint32_t>(event.category)},
        {"counters", lifecycle_counters_json(event.counters)},
    });
  }
  return Json{
      {"schema_version", page.schema_version},
      {"capacity", page.capacity},
      {"service_instance_id", page.service_instance_id},
      {"telemetry_epoch", page.telemetry_epoch},
      {"service_state", static_cast<std::uint32_t>(page.service_state)},
      {"shutdown_generation", page.shutdown_generation},
      {"snapshot_cut", page.snapshot_cut},
      {"first_retained_sequence", page.first_retained_sequence},
      {"next_sequence", page.next_sequence},
      {"global_dropped_total", page.global_dropped_total},
      {"global_dropped_saturated", page.global_dropped_saturated},
      {"counters", lifecycle_counters_json(page.counters)},
      {"records", std::move(records)},
      {"cursor_gap", page.cursor_gap},
      {"next_cursor", page.next_cursor},
      {"has_more", page.has_more},
  };
}

/**
 * @brief Encodes authoritative Host/device/lifecycle state at one boundary.
 * @param snapshot Source-private immutable execution snapshot.
 * @return Closed JSON snapshot with current and lifetime high-water facts.
 * @throws nlohmann/std allocation errors unchanged.
 */
Json execution_snapshot_json(const I1ExecutionSnapshot& snapshot) {
  Json devices = Json::array();
  for (const ResourceLedger::DeviceSnapshot& device :
       snapshot.device_resources) {
    devices.push_back(Json{
        {"backend", static_cast<std::uint32_t>(device.device.backend())},
        {"ordinal", device.device.ordinal()},
        {"limits", device_resource_vector_json(device.limits)},
        {"reserved", device_resource_vector_json(device.reserved)},
        {"high_water", device_resource_vector_json(device.high_water)},
        {"available", device_resource_vector_json(device.available)},
    });
  }
  return Json{
      {"host",
       Json{
           {"limits", resource_vector_json(snapshot.host_resources.limits)},
           {"reserved", resource_vector_json(snapshot.host_resources.reserved)},
           {"high_water",
            resource_vector_json(snapshot.host_resources.high_water)}}},
      {"devices", std::move(devices)},
      {"lifecycle", lifecycle_page_json(snapshot.lifecycle)},
  };
}

/**
 * @brief Encodes the complete frozen workload/grid contract in every row.
 * @return Closed JSON contract object independent of runner defaults.
 * @throws nlohmann/std allocation errors unchanged.
 */
Json workload_contract_json() {
  Json coefficients = Json::array();
  Json regions = Json::array();
  for (std::size_t index = 0U; index < kI1EditCount; ++index) {
    coefficients.push_back(kI1EditCoefficients[index]);
    const PixelRect region = i1_edit_region(index);
    regions.push_back(Json{{"x", region.x},
                           {"y", region.y},
                           {"width", region.width},
                           {"height", region.height}});
  }
  return Json{
      {"source", Json{{"operation", "image_generator:coordinate_pattern"},
                      {"width", 2048},
                      {"height", 2048},
                      {"channels", 4},
                      {"encoding", "binary32"},
                      {"seed", 0},
                      {"sample_formula",
                       "((17*x+31*y+47*c+seed) mod 256)/255, binary32 RNE"}}},
      {"baseline_coefficients", Json::array({0.80, 1.00, 1.20, 1.40})},
      {"edit_coefficients", std::move(coefficients)},
      {"edit_regions", std::move(regions)},
      {"target_node", kI1TargetNodeId},
      {"intent", "global-high-precision"},
      {"quality", "full"},
      {"qos_class", "interactive"},
      {"weight", 1},
      {"run_cap", 8},
      {"deadline_budget_ns", kI1DeadlineBudget.count()},
      {"edit_stride_ns", kI1EditStride.count()},
      {"admission_lateness_ns", kI1AdmissionLateness.count()},
      {"grid_slot_count", kI1GridSlotCount},
      {"warmup_slot_count", kI1WarmupSlotCount},
      {"measured_slot_count", kI1MeasuredSlotCount},
      {"episode_stride_ns", kI1EpisodeStride.count()},
      {"measurement_start_offset_ns", kI1MeasurementStartOffset.count()},
      {"measurement_end_offset_ns", kI1MeasurementEndOffset.count()},
      {"next_origin_guard_ns", kI1NextOriginGuard.count()},
  };
}

/**
 * @brief Encodes one complete edit admission and settlement record.
 * @param edit Copyable closed edit evidence.
 * @return Closed JSON record retaining absent-state facts explicitly.
 * @throws nlohmann/std allocation errors unchanged.
 */
Json edit_evidence_json(const I1EditEvidence& edit) {
  const Json admission_sample =
      edit.admission_attempted
          ? Json(monotonic_nanoseconds(edit.admission_sample))
          : Json(nullptr);
  Json host_return = nullptr;
  if (edit.host_return.has_value()) {
    host_return = Json{
        {"return_time_ns",
         monotonic_nanoseconds(edit.host_return->return_time)},
        {"status", operation_status_json(edit.host_return->status)},
        {"future_valid", edit.host_return->future_valid},
    };
  }
  Json accepted = nullptr;
  if (edit.accepted_coordinate.has_value()) {
    accepted = Json{
        {"admission_time_ns",
         monotonic_nanoseconds(edit.accepted_coordinate->admission_time())},
        {"event_sequence", edit.accepted_coordinate->event_sequence()},
    };
  }
  Json settlement = nullptr;
  if (edit.settlement_status.has_value()) {
    settlement = operation_status_json(*edit.settlement_status);
  }
  return Json{
      {"edit_index", edit.edit_index},
      {"coefficient", edit.coefficient},
      {"region", Json{{"x", edit.region.x},
                      {"y", edit.region.y},
                      {"width", edit.region.width},
                      {"height", edit.region.height}}},
      {"nominal_time_ns", monotonic_nanoseconds(edit.nominal_time)},
      {"admission_attempted", edit.admission_attempted},
      {"admission_sample_ns", admission_sample},
      {"admission_window_valid", edit.admission_window_valid},
      {"reserved_event_sequence", edit.reserved_event_sequence},
      {"deadline_ns", optional_monotonic_json(edit.deadline)},
      {"host_return", std::move(host_return)},
      {"accepted_coordinate", std::move(accepted)},
      {"settlement_status", std::move(settlement)},
  };
}

/**
 * @brief Encodes the raw request-scoped product observation categories.
 * @param observations Complete bounded observation snapshot.
 * @return Closed JSON object retaining causal order and per-output digests.
 * @throws Digest and nlohmann/std allocation failures unchanged.
 */
Json observations_json(const I1EpisodeObservationSnapshot& observations) {
  Json generations = Json::array();
  for (const I1ObservedCurrentGeneration& event :
       observations.current_generations) {
    Json accepted_coordinate = nullptr;
    if (event.accepted_coordinate.has_value()) {
      accepted_coordinate = Json{
          {"admission_time_ns",
           monotonic_nanoseconds(event.accepted_coordinate->admission_time())},
          {"event_sequence", event.accepted_coordinate->event_sequence()},
      };
    }
    generations.push_back(
        Json{{"edit_index", event.edit_index},
             {"generation", event.generation},
             {"observed_at_ns", monotonic_nanoseconds(event.observed_at)},
             {"causal_sequence", event.causal_sequence},
             {"accepted_coordinate", std::move(accepted_coordinate)}});
  }
  Json starts = Json::array();
  for (const I1ObservedServiceStart& event : observations.service_starts) {
    starts.push_back(Json{
        {"edit_index", event.edit_index},
        {"run_id", event.run_id},
        {"generation", event.generation},
        {"local_task_id", event.local_task_id},
        {"quality", event.quality == compute::ComputeRunQuality::Full
                        ? "full"
                        : "interactive"},
        {"qos_class",
         event.qos.service_class == compute::ComputeRunQosClass::Interactive
             ? "interactive"
             : "throughput"},
        {"deadline_ns", optional_monotonic_json(event.qos.deadline)},
        {"weight", event.qos.weight},
        {"maximum_parallelism", event.qos.maximum_parallelism},
        {"service_charge", event.service_charge},
        {"observed_at_ns", monotonic_nanoseconds(event.observed_at)},
        {"causal_sequence", event.causal_sequence},
    });
  }
  Json cancellations = Json::array();
  for (const I1ObservedCancellation& event : observations.cancellations) {
    cancellations.push_back(Json{
        {"edit_index", event.edit_index},
        {"run_id", event.run_id},
        {"generation", event.generation},
        {"reason", static_cast<std::uint32_t>(event.reason)},
        {"observed_at_ns", monotonic_nanoseconds(event.observed_at)},
        {"causal_sequence", event.causal_sequence},
    });
  }
  Json terminals = Json::array();
  for (const I1ObservedTerminal& event : observations.terminals) {
    terminals.push_back(Json{
        {"edit_index", event.edit_index},
        {"run_id", event.run_id},
        {"generation", event.generation},
        {"kind", static_cast<std::uint32_t>(event.kind)},
        {"observed_at_ns", monotonic_nanoseconds(event.observed_at)},
        {"causal_sequence", event.causal_sequence},
    });
  }
  Json visible_outputs = Json::array();
  for (const I1ObservedVisibleOutput& event : observations.visible_outputs) {
    visible_outputs.push_back(Json{
        {"edit_index", event.edit_index},
        {"run_id", event.run_id},
        {"generation", event.generation},
        {"observed_at_ns", monotonic_nanoseconds(event.observed_at)},
        {"causal_sequence", event.causal_sequence},
        {"value_valid", event.output.valid()},
        {"content_digest",
         content_digest_json(compute_content_digest(event.output))},
    });
  }
  const auto lifecycle_transitions_json = [](const auto& events) {
    Json result = Json::array();
    for (const I1ObservedRunLifecycleTransition& event : events) {
      result.push_back(Json{
          {"edit_index", event.edit_index},
          {"run_id", event.run_id},
          {"generation", event.generation},
          {"observed_at_ns", monotonic_nanoseconds(event.observed_at)},
          {"causal_sequence", event.causal_sequence},
      });
    }
    return result;
  };
  Json host_settlements = Json::array();
  for (const I1ObservedHostSettlement& event : observations.host_settlements) {
    host_settlements.push_back(Json{
        {"edit_index", event.edit_index},
        {"observed_at_ns", monotonic_nanoseconds(event.observed_at)},
        {"causal_sequence", event.causal_sequence},
    });
  }
  return Json{{"current_generations", std::move(generations)},
              {"service_starts", std::move(starts)},
              {"cancellations", std::move(cancellations)},
              {"terminals", std::move(terminals)},
              {"visible_outputs", std::move(visible_outputs)},
              {"run_quiescences",
               lifecycle_transitions_json(observations.run_quiescences)},
              {"resource_settlements",
               lifecycle_transitions_json(observations.resource_settlements)},
              {"host_settlements", std::move(host_settlements)},
              {"overflowed", observations.overflowed}};
}

/**
 * @brief Returns the stable phase token for one grid slot classification.
 * @param phase Typed cold/warmup/measured phase.
 * @return Stable lowercase token.
 * @throws Nothing.
 */
const char* phase_text(I1EpisodePhase phase) noexcept {
  switch (phase) {
    case I1EpisodePhase::Cold:
      return "cold";
    case I1EpisodePhase::Warmup:
      return "warmup";
    case I1EpisodePhase::Measured:
      return "measured";
  }
  return "invalid";
}

/**
 * @brief Encodes one fully evaluated closed Issue #93 inner row.
 * @param row Raw and derived episode evidence.
 * @return Closed version-one JSON object; never an outer canonical row.
 * @throws Digest and nlohmann/std allocation failures unchanged.
 */
Json inner_row_json(const I1EpisodeInnerRow& row) {
  Json edits = Json::array();
  for (const I1EditEvidence& edit : row.evidence.edits) {
    edits.push_back(edit_evidence_json(edit));
  }
  Json products = Json::array();
  for (std::size_t edit_index = 0U; edit_index < row.accepted_products.size();
       ++edit_index) {
    Json product = nullptr;
    if (row.accepted_products[edit_index].has_value()) {
      const I1AcceptedProductIdentity& identity =
          *row.accepted_products[edit_index];
      Json accepted_coordinate = nullptr;
      if (identity.accepted_coordinate.has_value()) {
        accepted_coordinate = Json{
            {"admission_time_ns",
             monotonic_nanoseconds(
                 identity.accepted_coordinate->admission_time())},
            {"event_sequence", identity.accepted_coordinate->event_sequence()},
        };
      }
      product =
          Json{{"edit_index", edit_index},
               {"generation", identity.generation},
               {"run_id", identity.run_id.has_value() ? Json(*identity.run_id)
                                                      : Json(nullptr)},
               {"accepted_coordinate", std::move(accepted_coordinate)}};
    }
    products.push_back(std::move(product));
  }
  Json expected_digest = nullptr;
  if (row.evidence.expected_final_digest.has_value()) {
    expected_digest = Json{
        {"algorithm", "sha256-canonical-v1"},
        {"lowercase_hex", digest_hex(*row.evidence.expected_final_digest)},
    };
  }
  const auto [phase, phase_index] = classify_i1_slot(row.evidence.slot);
  return Json{
      {"schema", row.schema},
      {"schema_version", row.schema_version},
      {"outer_canonical_envelope_claim", false},
      {"workload_id", row.workload_id},
      {"workload_contract", workload_contract_json()},
      {"replicate_ordinal", row.evidence.replicate_ordinal},
      {"grid",
       Json{{"slot", row.evidence.slot},
            {"phase", phase_text(phase)},
            {"phase_index", phase_index},
            {"grid_origin_ns", monotonic_nanoseconds(row.evidence.grid_origin)},
            {"episode_origin_ns",
             monotonic_nanoseconds(row.evidence.episode_origin)},
            {"terminal_boundary_ns",
             monotonic_nanoseconds(row.evidence.terminal_boundary)},
            {"measurement_start_ns",
             monotonic_nanoseconds(row.evidence.measurement_start)},
            {"measurement_end_ns",
             monotonic_nanoseconds(row.evidence.measurement_end)},
            {"observation_cut_captured_at_ns",
             monotonic_nanoseconds(row.evidence.observation_cut.captured_at)},
            {"observation_cut_causal_sequence",
             row.evidence.observation_cut.causal_sequence},
            {"final_snapshot_sample_ns",
             monotonic_nanoseconds(row.evidence.final_snapshot_sample)}}},
      {"edits", std::move(edits)},
      {"accepted_products", std::move(products)},
      {"observations", observations_json(row.evidence.observations)},
      {"resource_baseline", execution_snapshot_json(row.evidence.baseline)},
      {"resource_high_water_and_final",
       execution_snapshot_json(row.evidence.final_snapshot)},
      {"expected_final_digest", std::move(expected_digest)},
      {"final_digest", content_digest_json(row.final_digest)},
      {"final_latency_ns", row.final_latency.has_value()
                               ? Json(row.final_latency->count())
                               : Json(nullptr)},
      {"service",
       Json{
           {"all_started_service", row.service.all_started_service},
           {"discarded_started_service", row.service.discarded_started_service},
           {"post_cancel_started_service",
            row.service.post_cancel_started_service},
           {"discarded_ratio", row.service.discarded_ratio.has_value()
                                   ? Json(*row.service.discarded_ratio)
                                   : Json(nullptr)}}},
      {"memory_settled", row.memory_settled},
      {"validity_reasons", row.validity_reasons},
      {"verdicts", Json{{"latency", verdict_text(row.latency_verdict)},
                        {"waste", verdict_text(row.waste_verdict)},
                        {"memory", verdict_text(row.memory_verdict)},
                        {"output", verdict_text(row.output_verdict)}}},
  };
}

/**
 * @brief Encodes one exact replicate aggregate and frozen gate thresholds.
 * @param summary Evaluated 221-slot aggregate.
 * @return Closed summary JSON object.
 * @throws nlohmann/std allocation errors unchanged.
 */
Json replicate_summary_json(const I1ReplicateSummary& summary) {
  Json percentiles = nullptr;
  if (summary.latency.has_value()) {
    percentiles = Json{{"p50_ns", summary.latency->p50.count()},
                       {"p95_ns", summary.latency->p95.count()},
                       {"p99_ns", summary.latency->p99.count()}};
  }
  return Json{
      {"schema", summary.schema},
      {"workload_id", kI1WorkloadId},
      {"outer_canonical_envelope_claim", false},
      {"replicate_ordinal", summary.replicate_ordinal},
      {"grid_slot_count", kI1GridSlotCount},
      {"measured_sample_count", summary.measured_sample_count},
      {"latency_percentiles", std::move(percentiles)},
      {"latency_limits_ns", Json{{"p50", kI1LatencyP50Limit.count()},
                                 {"p95", kI1LatencyP95Limit.count()},
                                 {"p99", kI1LatencyP99Limit.count()}}},
      {"measured_service",
       Json{{"all_started_service",
             summary.measured_service.all_started_service},
            {"discarded_started_service",
             summary.measured_service.discarded_started_service},
            {"post_cancel_started_service",
             summary.measured_service.post_cancel_started_service},
            {"discarded_ratio",
             summary.measured_service.discarded_ratio.has_value()
                 ? Json(*summary.measured_service.discarded_ratio)
                 : Json(nullptr)},
            {"discarded_ratio_limit", kI1DiscardedServiceRatioLimit}}},
      {"validity_reasons", summary.validity_reasons},
      {"verdicts", Json{{"latency", verdict_text(summary.latency_verdict)},
                        {"waste", verdict_text(summary.waste_verdict)},
                        {"memory", verdict_text(summary.memory_verdict)},
                        {"output", verdict_text(summary.output_verdict)}}},
  };
}

}  // namespace

/**
 * @brief Encodes the complete frozen I1 workload and grid contract.
 * @return Closed JSON contract object independent of runner defaults.
 * @throws nlohmann/std allocation errors unchanged.
 */
nlohmann::json i1_workload_contract_json() {
  return workload_contract_json();
}

/**
 * @brief Returns the stable token for one I1 grid-slot phase.
 * @param phase Typed cold, warmup, or measured phase.
 * @return Stable lowercase phase token.
 * @throws Nothing.
 */
const char* i1_phase_text(I1EpisodePhase phase) noexcept {
  return phase_text(phase);
}

/**
 * @brief Encodes one fully evaluated closed Issue #93 inner row.
 * @param row Raw and derived episode evidence.
 * @return Closed version-one JSON object; never an outer canonical row.
 * @throws Digest and nlohmann/std allocation failures unchanged.
 */
nlohmann::json i1_inner_row_json(const I1EpisodeInnerRow& row) {
  return inner_row_json(row);
}

/**
 * @brief Encodes one exact replicate aggregate and frozen gate thresholds.
 * @param summary Evaluated 221-slot aggregate.
 * @return Closed summary JSON object.
 * @throws nlohmann/std allocation errors unchanged.
 */
nlohmann::json i1_replicate_summary_json(const I1ReplicateSummary& summary) {
  return replicate_summary_json(summary);
}

}  // namespace ps::benchmark
