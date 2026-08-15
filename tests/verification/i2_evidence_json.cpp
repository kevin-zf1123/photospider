/**
 * @file i2_evidence_json.cpp
 * @brief Implements complete verification-only JSON encoding for I2 evidence.
 */
#include "verification/i2_evidence_json.hpp"

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
 * @brief Encodes an optional monotonic point without a sentinel.
 * @param value Optional process-local monotonic time.
 * @return JSON integer or null.
 * @throws nlohmann allocation failures unchanged.
 */
Json optional_time_json(
    const std::optional<std::chrono::steady_clock::time_point>& value) {
  return value.has_value() ? Json(monotonic_nanoseconds(*value))
                           : Json(nullptr);
}

/**
 * @brief Encodes one optional unsigned scalar without implicit conversion.
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
 * @brief Returns lowercase hexadecimal for one typed content digest.
 * @param digest Exact canonical digest bytes.
 * @return Sixty-four lowercase hexadecimal characters.
 * @throws std::bad_alloc when output ownership cannot allocate.
 */
std::string digest_hex(const ContentDigest& digest) {
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
 * @brief Encodes one typed digest identity.
 * @param digest Exact digest value.
 * @return Algorithm plus lowercase bytes.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json digest_json(const ContentDigest& digest) {
  return Json{{"algorithm", static_cast<std::uint32_t>(digest.algorithm)},
              {"lowercase_hex", digest_hex(digest)}};
}

/**
 * @brief Encodes an optional typed digest.
 * @param digest Optional exact digest.
 * @return Digest object or null.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json optional_digest_json(const std::optional<ContentDigest>& digest) {
  return digest.has_value() ? digest_json(*digest) : Json(nullptr);
}

/**
 * @brief Encodes one complete typed digest outcome.
 * @param result Available or typed unavailable outcome.
 * @return State, optional digest, and diagnostic.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json digest_result_json(const ContentDigestResult& result) {
  return Json{{"state", static_cast<std::uint32_t>(result.state)},
              {"digest", optional_digest_json(result.digest)},
              {"diagnostic", result.diagnostic}};
}

/**
 * @brief Encodes one public operation status exactly.
 * @param status Status observed at Host return or settlement.
 * @return Complete status object.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json operation_status_json(const OperationStatus& status) {
  return Json{{"ok", status.ok},
              {"domain", static_cast<std::uint32_t>(status.domain)},
              {"code", status.code},
              {"name", status.name},
              {"message", status.message}};
}

/**
 * @brief Encodes one pixel rectangle.
 * @param value Complete source- or preview-space rectangle.
 * @return Four-field rectangle object.
 * @throws nlohmann allocation failures unchanged.
 */
Json pixel_rect_json(const PixelRect& value) {
  return Json{{"x", value.x},
              {"y", value.y},
              {"width", value.width},
              {"height", value.height}};
}

/**
 * @brief Encodes one optional accepted-boundary coordinate.
 * @param value Optional success-only coordinate.
 * @return Admission-time/sequence object or null.
 * @throws nlohmann allocation failures unchanged.
 */
Json accepted_coordinate_json(
    const std::optional<compute::AcceptedBoundaryCoordinate>& value) {
  if (!value.has_value()) {
    return nullptr;
  }
  return Json{
      {"admission_time_ns", monotonic_nanoseconds(value->admission_time())},
      {"event_sequence", value->event_sequence()}};
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
 * @brief Encodes one immutable storage binding.
 * @param value Complete allocation/device/domain/byte facts.
 * @return Closed storage-binding object.
 * @throws nlohmann allocation failures unchanged.
 */
Json storage_binding_json(const StorageBinding& value) {
  return Json{
      {"allocation_identity", value.allocation.value()},
      {"device", device_id_json(value.device)},
      {"memory_domain", static_cast<std::uint32_t>(value.memory_domain)},
      {"byte_size", value.byte_size},
      {"host_visible", value.host_visible}};
}

/**
 * @brief Encodes one classified explicit access plan.
 * @param value Complete plan observation.
 * @return Source, target, visibility, lease, and transfer facts.
 * @throws nlohmann allocation failures unchanged.
 */
Json access_plan_json(const AccessPlan& value) {
  const AccessTarget target = value.target();
  const VisibilityObligations visibility = value.visibility();
  return Json{
      {"kind", static_cast<std::uint32_t>(value.kind())},
      {"source_revision", value.source_revision()},
      {"source_binding", storage_binding_json(value.source_binding())},
      {"target",
       Json{{"device", device_id_json(target.device)},
            {"memory_domain", static_cast<std::uint32_t>(target.memory_domain)},
            {"host_read", target.host_read},
            {"require_distinct_binding", target.require_distinct_binding}}},
      {"visibility",
       Json{{"await_producer", visibility.await_producer},
            {"synchronize_memory", visibility.synchronize_memory},
            {"transfer_ownership", visibility.transfer_ownership}}},
      {"lease_kind", static_cast<std::uint32_t>(value.lease_kind())},
      {"transfer_bytes", value.transfer_bytes()}};
}

/**
 * @brief Encodes one optional access plan.
 * @param value Optional classified plan.
 * @return Plan object or null.
 * @throws nlohmann allocation failures unchanged.
 */
Json optional_access_plan_json(const std::optional<AccessPlan>& value) {
  return value.has_value() ? access_plan_json(*value) : Json(nullptr);
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
 * @brief Encodes one authoritative execution/resource snapshot.
 * @param value Complete Host/device/lifecycle cut.
 * @return Closed snapshot object.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json execution_snapshot_json(const I1ExecutionSnapshot& value) {
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
      {"lifecycle", lifecycle_page_json(value.lifecycle)}};
}

/**
 * @brief Encodes one immutable child descriptor.
 * @param value Complete copied descriptor facts.
 * @return Closed identity/intent/quality/QoS object.
 * @throws nlohmann allocation failures unchanged.
 */
Json child_json(const I2ObservedChildDescriptor& value) {
  return Json{
      {"edit_index", value.edit_index},
      {"run_id", value.run_id},
      {"graph_instance_id", value.graph_instance_id},
      {"graph_revision", value.graph_revision},
      {"target_node_id", value.target_node_id},
      {"child_intent", static_cast<std::uint32_t>(value.child_intent)},
      {"quality", static_cast<std::uint32_t>(value.quality)},
      {"qos", Json{{"service_class",
                    static_cast<std::uint32_t>(value.qos.service_class)},
                   {"deadline_ns", optional_time_json(value.qos.deadline)},
                   {"weight", value.qos.weight},
                   {"maximum_parallelism",
                    optional_unsigned_json(value.qos.maximum_parallelism)}}},
      {"generation", value.generation},
      {"request_intent", static_cast<std::uint32_t>(value.request_intent)},
      {"accepted_coordinate",
       accepted_coordinate_json(value.accepted_coordinate)}};
}

/**
 * @brief Encodes one explicit Host or Metal access observation.
 * @param value Complete authority-free access facts.
 * @return Closed plan/revision/binding/allocation/byte record.
 * @throws nlohmann allocation failures unchanged.
 */
Json value_access_json(const I2ValueAccessEvidence& value) {
  return Json{{"plan", optional_access_plan_json(value.plan)},
              {"revision", value.revision.value()},
              {"binding", storage_binding_json(value.binding)},
              {"allocation_identity", value.allocation.value()},
              {"storage_bytes", value.storage_bytes},
              {"executor_submitted", value.executor_submitted}};
}

/**
 * @brief Encodes one compute-I/O state snapshot.
 * @param value Complete bounded executor observation.
 * @return Closed counter/state object.
 * @throws nlohmann allocation failures unchanged.
 */
Json compute_io_json(const execution::ComputeIoExecutorSnapshot& value) {
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
 * @brief Encodes one Metal executor diagnostic observation.
 * @param value Complete diagnostic counters.
 * @return Closed executor state object.
 * @throws nlohmann allocation failures unchanged.
 */
Json device_diagnostics_json(
    const execution::DeviceExecutorDiagnostics& value) {
  return Json{{"device", static_cast<std::uint32_t>(value.device)},
              {"queue_ready", value.queue_ready},
              {"submission_count", value.submission_count},
              {"invocation_count", value.invocation_count},
              {"total_allocations", value.total_allocations},
              {"live_allocations", value.live_allocations},
              {"pipeline_cache_entries", value.pipeline_cache_entries}};
}

/**
 * @brief Encodes optional Metal executor diagnostics.
 * @param value Optional diagnostic observation.
 * @return Diagnostic object or null.
 * @throws nlohmann allocation failures unchanged.
 */
Json optional_device_diagnostics_json(
    const std::optional<execution::DeviceExecutorDiagnostics>& value) {
  return value.has_value() ? device_diagnostics_json(*value) : Json(nullptr);
}

/**
 * @brief Encodes one device ledger snapshot.
 * @param value Complete exact-device resource cut.
 * @return Closed device-resource object.
 * @throws nlohmann allocation failures unchanged.
 */
Json device_snapshot_json(const ResourceLedger::DeviceSnapshot& value) {
  return Json{{"device", device_id_json(value.device)},
              {"limits", device_resource_vector_json(value.limits)},
              {"reserved", device_resource_vector_json(value.reserved)},
              {"high_water", device_resource_vector_json(value.high_water)},
              {"available", device_resource_vector_json(value.available)}};
}

/**
 * @brief Encodes optional device resource evidence.
 * @param value Optional device ledger snapshot.
 * @return Snapshot object or null.
 * @throws nlohmann allocation failures unchanged.
 */
Json optional_device_snapshot_json(
    const std::optional<ResourceLedger::DeviceSnapshot>& value) {
  return value.has_value() ? device_snapshot_json(*value) : Json(nullptr);
}

/**
 * @brief Encodes one complete Host/conditional-Metal acquisition.
 * @param value Closed explicit acquisition evidence.
 * @return Complete access, executor, resource, and no-I/O object.
 * @throws nlohmann/std allocation failures unchanged.
 */
Json acquisition_json(const I2ValueAcquisitionEvidence& value) {
  const auto optional_access = [](const auto& access) -> Json {
    return access.has_value() ? value_access_json(*access) : Json(nullptr);
  };
  return Json{
      {"host_first", value_access_json(value.host_first)},
      {"host_second", value_access_json(value.host_second)},
      {"metal",
       Json{{"available", value.metal.available},
            {"unavailable_reason", value.metal.unavailable_reason},
            {"before", optional_device_diagnostics_json(value.metal.before)},
            {"first", optional_access(value.metal.first)},
            {"after_first",
             optional_device_diagnostics_json(value.metal.after_first)},
            {"second", optional_access(value.metal.second)},
            {"after_second",
             optional_device_diagnostics_json(value.metal.after_second)},
            {"resources_before",
             optional_device_snapshot_json(value.metal.resources_before)},
            {"resources_after_first",
             optional_device_snapshot_json(value.metal.resources_after_first)},
            {"resources_after_second",
             optional_device_snapshot_json(
                 value.metal.resources_after_second)}}},
      {"compute_io_before", compute_io_json(value.io_before)},
      {"compute_io_after", compute_io_json(value.io_after)}};
}

/**
 * @brief Returns the stable token for one independent verdict.
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

/**
 * @brief Encodes optional nanosecond duration without a sentinel.
 * @param value Optional derived duration.
 * @return Signed integer nanoseconds or null.
 * @throws nlohmann allocation failures unchanged.
 */
Json optional_duration_json(
    const std::optional<std::chrono::nanoseconds>& value) {
  return value.has_value() ? Json(value->count()) : Json(nullptr);
}

/**
 * @brief Encodes one service aggregate.
 * @param value Complete integer sums and optional ratio.
 * @return Closed service object.
 * @throws nlohmann allocation failures unchanged.
 */
Json service_json(const I1ServiceEvidence& value) {
  return Json{
      {"all_started_service", value.all_started_service},
      {"discarded_started_service", value.discarded_started_service},
      {"post_cancel_started_service", value.post_cancel_started_service},
      {"discarded_ratio", value.discarded_ratio.has_value()
                              ? Json(*value.discarded_ratio)
                              : Json(nullptr)}};
}

}  // namespace

/** @copydoc i2_phase_text */
const char* i2_phase_text(I2EpisodePhase phase) noexcept {
  switch (phase) {
    case I2EpisodePhase::Cold:
      return "cold";
    case I2EpisodePhase::Warmup:
      return "warmup";
    case I2EpisodePhase::Measured:
      return "measured";
  }
  return "unknown";
}

/** @copydoc i2_workload_contract_json */
Json i2_workload_contract_json() {
  Json edits = Json::array();
  for (std::size_t index = 0U; index < kI1EditCount; ++index) {
    edits.push_back(
        Json{{"edit_index", index},
             {"coefficient", kI1EditCoefficients[index]},
             {"source_region", pixel_rect_json(i1_edit_region(index))},
             {"preview_region", pixel_rect_json(i2_preview_region(index))}});
  }
  return Json{
      {"workload_id", kI2WorkloadId},
      {"source", Json{{"width", kI1FrozenImageEdge},
                      {"height", kI1FrozenImageEdge},
                      {"channels", 4U},
                      {"encoding", "rgba-fp32"},
                      {"seed", 0}}},
      {"preview",
       Json{{"width", kI2PreviewImageEdge},
            {"height", kI2PreviewImageEdge},
            {"channels", 4U},
            {"downsample_factor", kI2PreviewDownsampleFactor},
            {"normalization", "aligned-4x4-mean-one-binary32-round"}}},
      {"target_node_id", kI1TargetNodeId},
      {"edits", std::move(edits)},
      {"edit_stride_ns", kI1EditStride.count()},
      {"admission_lateness_ns", kI1AdmissionLateness.count()},
      {"episode_stride_ns", kI2EpisodeStride.count()},
      {"grid_slots", kI2GridSlotCount},
      {"cold_slots", 1},
      {"warmup_slots", kI2WarmupSlotCount},
      {"measured_slots", kI2MeasuredSlotCount},
      {"preview_deadline_budget_ns", kI2PreviewDeadlineBudget.count()},
      {"final_deadline_budget_ns", kI2FinalDeadlineBudget.count()},
      {"latest_final_deadline_offset_ns", kI2LatestFinalDeadlineOffset.count()},
      {"terminal_guard_ns", kI2TerminalGuard.count()},
      {"preview_golden", digest_json(i2_frozen_preview_content_digest())},
      {"final_golden", digest_json(i1_frozen_final_content_digest())}};
}

/** @copydoc i2_inner_row_json */
Json i2_inner_row_json(const I2EpisodeInnerRow& row) {
  Json edits = Json::array();
  for (const I2EditEvidence& edit : row.evidence.edits) {
    Json host_return = nullptr;
    if (edit.host_return.has_value()) {
      host_return =
          Json{{"return_time_ns",
                monotonic_nanoseconds(edit.host_return->return_time)},
               {"status", operation_status_json(edit.host_return->status)},
               {"future_valid", edit.host_return->future_valid}};
    }
    edits.push_back(Json{
        {"edit_index", edit.edit_index},
        {"coefficient", edit.coefficient},
        {"source_region", pixel_rect_json(edit.source_region)},
        {"preview_region", pixel_rect_json(edit.preview_region)},
        {"nominal_time_ns", monotonic_nanoseconds(edit.nominal_time)},
        {"admission_attempted", edit.admission_attempted},
        {"admission_sample_ns", monotonic_nanoseconds(edit.admission_sample)},
        {"admission_window_valid", edit.admission_window_valid},
        {"reserved_event_sequence",
         optional_unsigned_json(edit.reserved_event_sequence)},
        {"preview_deadline_ns", optional_time_json(edit.preview_deadline)},
        {"final_deadline_ns", optional_time_json(edit.final_deadline)},
        {"host_return", std::move(host_return)},
        {"accepted_coordinate",
         accepted_coordinate_json(edit.accepted_coordinate)},
        {"settlement_status",
         edit.settlement_status.has_value()
             ? operation_status_json(*edit.settlement_status)
             : Json(nullptr)}});
  }

  Json products = Json::array();
  for (const auto& product : row.accepted_products) {
    if (!product.has_value()) {
      products.push_back(nullptr);
      continue;
    }
    products.push_back(Json{
        {"generation", product->generation},
        {"accepted_coordinate",
         accepted_coordinate_json(product->accepted_coordinate)},
        {"preview_run_id", optional_unsigned_json(product->preview_run_id)},
        {"final_run_id", optional_unsigned_json(product->final_run_id)}});
  }

  Json current_generations = Json::array();
  for (const I1ObservedCurrentGeneration& event :
       row.evidence.observations.current_generations) {
    current_generations.push_back(
        Json{{"edit_index", event.edit_index},
             {"generation", event.generation},
             {"observed_at_ns", monotonic_nanoseconds(event.observed_at)},
             {"causal_sequence", event.causal_sequence},
             {"accepted_coordinate",
              accepted_coordinate_json(event.accepted_coordinate)}});
  }
  Json service_starts = Json::array();
  for (const I2ObservedServiceStart& event :
       row.evidence.observations.service_starts) {
    service_starts.push_back(
        Json{{"child", child_json(event.child)},
             {"local_task_id", event.local_task_id},
             {"service_charge", event.service_charge},
             {"observed_at_ns", monotonic_nanoseconds(event.observed_at)},
             {"causal_sequence", event.causal_sequence}});
  }
  Json cancellations = Json::array();
  for (const I2ObservedCancellation& event :
       row.evidence.observations.cancellations) {
    cancellations.push_back(
        Json{{"child", child_json(event.child)},
             {"reason", static_cast<std::uint32_t>(event.reason)},
             {"observed_at_ns", monotonic_nanoseconds(event.observed_at)},
             {"causal_sequence", event.causal_sequence}});
  }
  Json terminals = Json::array();
  for (const I2ObservedTerminal& event : row.evidence.observations.terminals) {
    terminals.push_back(
        Json{{"child", child_json(event.child)},
             {"kind", static_cast<std::uint32_t>(event.kind)},
             {"observed_at_ns", monotonic_nanoseconds(event.observed_at)},
             {"causal_sequence", event.causal_sequence}});
  }
  Json triggers = Json::array();
  for (const I2ObservedFinalTrigger& event :
       row.evidence.observations.final_triggers) {
    triggers.push_back(
        Json{{"child", child_json(event.child)},
             {"observed_at_ns", monotonic_nanoseconds(event.observed_at)},
             {"causal_sequence", event.causal_sequence}});
  }
  Json visible = Json::array();
  for (const I2ObservedVisibleOutput& event :
       row.evidence.observations.visible_outputs) {
    visible.push_back(
        Json{{"child", child_json(event.child)},
             {"observed_at_ns", monotonic_nanoseconds(event.observed_at)},
             {"causal_sequence", event.causal_sequence},
             {"payload_retained_at_serialization", event.output.valid()},
             {"value_valid_at_capture", event.value_valid_at_capture},
             {"content_digest", event.content_digest.has_value()
                                    ? digest_result_json(*event.content_digest)
                                    : Json(nullptr)},
             {"acquisition", event.acquisition.has_value()
                                 ? acquisition_json(*event.acquisition)
                                 : Json(nullptr)},
             {"value_revision", event.value_revision.value()},
             {"value_binding", storage_binding_json(event.value_binding)},
             {"value_allocation", event.value_allocation.value()},
             {"value_storage_bytes", event.value_storage_bytes}});
  }
  const auto lifecycle_transitions_json = [](const auto& events) {
    Json result = Json::array();
    for (const auto& event : events) {
      result.push_back(
          Json{{"child", child_json(event.child)},
               {"observed_at_ns", monotonic_nanoseconds(event.observed_at)},
               {"causal_sequence", event.causal_sequence}});
    }
    return result;
  };
  Json host_settlements = Json::array();
  for (const I1ObservedHostSettlement& event :
       row.evidence.observations.host_settlements) {
    host_settlements.push_back(
        Json{{"edit_index", event.edit_index},
             {"observed_at_ns", monotonic_nanoseconds(event.observed_at)},
             {"causal_sequence", event.causal_sequence}});
  }

  return Json{
      {"schema", row.schema},
      {"schema_version", row.schema_version},
      {"workload_id", row.workload_id},
      {"replicate_ordinal", row.evidence.replicate_ordinal},
      {"grid",
       Json{{"slot", row.evidence.slot},
            {"phase", i2_phase_text(classify_i2_slot(row.evidence.slot).first)},
            {"grid_origin_ns", monotonic_nanoseconds(row.evidence.grid_origin)},
            {"episode_origin_ns",
             monotonic_nanoseconds(row.evidence.episode_origin)},
            {"terminal_boundary_ns",
             monotonic_nanoseconds(row.evidence.terminal_boundary)}}},
      {"observation_cut",
       Json{{"captured_at_ns",
             monotonic_nanoseconds(row.evidence.observation_cut.captured_at)},
            {"causal_sequence", row.evidence.observation_cut.causal_sequence}}},
      {"edits", std::move(edits)},
      {"accepted_products", std::move(products)},
      {"observations",
       Json{{"current_generations", std::move(current_generations)},
            {"service_starts", std::move(service_starts)},
            {"cancellations", std::move(cancellations)},
            {"terminals", std::move(terminals)},
            {"final_triggers", std::move(triggers)},
            {"visible_outputs", std::move(visible)},
            {"run_quiescences", lifecycle_transitions_json(
                                    row.evidence.observations.run_quiescences)},
            {"resource_settlements",
             lifecycle_transitions_json(
                 row.evidence.observations.resource_settlements)},
            {"host_settlements", std::move(host_settlements)},
            {"overflowed", row.evidence.observations.overflowed}}},
      {"baseline", execution_snapshot_json(row.evidence.baseline)},
      {"final_snapshot", execution_snapshot_json(row.evidence.final_snapshot)},
      {"final_snapshot_sample_ns",
       monotonic_nanoseconds(row.evidence.final_snapshot_sample)},
      {"expected_preview_digest",
       optional_digest_json(row.evidence.expected_preview_digest)},
      {"expected_final_digest",
       optional_digest_json(row.evidence.expected_final_digest)},
      {"preview_digest", digest_result_json(row.preview_digest)},
      {"final_digest", digest_result_json(row.final_digest)},
      {"latencies_ns",
       Json{{"preview", optional_duration_json(row.latencies.preview)},
            {"final", optional_duration_json(row.latencies.final)}}},
      {"service", service_json(row.service)},
      {"memory_settled", row.memory_settled},
      {"validity_reasons", row.validity_reasons},
      {"verdicts", Json{{"latency", verdict_text(row.latency_verdict)},
                        {"waste", verdict_text(row.waste_verdict)},
                        {"memory", verdict_text(row.memory_verdict)},
                        {"output", verdict_text(row.output_verdict)}}},
      {"outer_canonical_envelope_claim", false}};
}

/** @copydoc i2_replicate_summary_json */
Json i2_replicate_summary_json(const I2ReplicateSummary& summary) {
  Json latency = nullptr;
  if (summary.latency.has_value()) {
    latency = Json{{"preview_p50_ns", summary.latency->preview_p50.count()},
                   {"preview_p95_ns", summary.latency->preview_p95.count()},
                   {"preview_p99_ns", summary.latency->preview_p99.count()},
                   {"final_p50_ns", summary.latency->final_p50.count()},
                   {"final_p95_ns", summary.latency->final_p95.count()},
                   {"final_p99_ns", summary.latency->final_p99.count()}};
  }
  return Json{
      {"schema", summary.schema},
      {"workload_id", kI2WorkloadId},
      {"replicate_ordinal", summary.replicate_ordinal},
      {"measured_sample_count", summary.measured_sample_count},
      {"latency", std::move(latency)},
      {"measured_service", service_json(summary.measured_service)},
      {"thresholds",
       Json{{"preview_p50_ns", kI2PreviewLatencyP50Limit.count()},
            {"preview_p95_ns", kI2PreviewLatencyP95Limit.count()},
            {"preview_p99_ns", kI2PreviewLatencyP99Limit.count()},
            {"final_p95_ns", kI2FinalLatencyP95Limit.count()},
            {"final_p99_ns", kI2FinalLatencyP99Limit.count()},
            {"discarded_service_ratio", kI2DiscardedServiceRatioLimit}}},
      {"validity_reasons", summary.validity_reasons},
      {"verdicts", Json{{"latency", verdict_text(summary.latency_verdict)},
                        {"waste", verdict_text(summary.waste_verdict)},
                        {"memory", verdict_text(summary.memory_verdict)},
                        {"output", verdict_text(summary.output_verdict)}}},
      {"outer_canonical_envelope_claim", false}};
}

}  // namespace ps::benchmark
