/**
 * @file i1_edit_storm_benchmark.cpp
 * @brief Runs and records the exact manual I1 edit-storm verification profile.
 */
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "benchmark/i1_evidence.hpp"
#include "photospider/host/host.hpp"

#ifndef PHOTOSPIDER_I1_PROJECT_SOURCE_DIR
#error "PHOTOSPIDER_I1_PROJECT_SOURCE_DIR must name the project checkout"
#endif

namespace ps::benchmark {
namespace {

using Json = nlohmann::json;

/**
 * @brief Parsed explicit controls for one manual exact I1 replicate.
 * @throws Nothing for default construction.
 */
struct I1RunnerOptions final {
  /** @brief Caller-selected fresh disposable directory outside the checkout. */
  std::filesystem::path output_directory;
  /** @brief Normative replicate ordinal in `[1,3]`. */
  std::uint64_t replicate_ordinal = 1U;
  /** @brief Whether usage was requested without running product work. */
  bool help = false;
};

/**
 * @brief Prints the exact manual-runner invocation contract.
 * @param output Destination stream.
 * @return Nothing.
 * @throws std::ios_base::failure only when enabled on the stream by caller.
 */
void print_usage(std::ostream& output) {
  output
      << "Usage: i1_edit_storm_benchmark --output-dir ABSOLUTE_PATH "
         "[--replicate-ordinal 1|2|3]\n"
      << "Runs one exact 221-slot I1-edit-storm-v1 replicate. The output "
         "directory must be absent or empty, disposable, and outside the "
         "Photospider checkout. This target is manual and machine-dependent.\n";
}

/**
 * @brief Parses one strict positive decimal replicate ordinal.
 * @param text Complete argument bytes.
 * @return Parsed value in `[1,3]`.
 * @throws std::invalid_argument for malformed or out-of-range input.
 */
std::uint64_t parse_replicate_ordinal(std::string_view text) {
  std::uint64_t result = 0U;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      result == 0U || result > 3U) {
    throw std::invalid_argument("--replicate-ordinal must be 1, 2, or 3");
  }
  return result;
}

/**
 * @brief Parses the closed I1 runner command-line vocabulary.
 * @param argc Argument count supplied to main.
 * @param argv Argument vector supplied to main.
 * @return Complete validated options or a help request.
 * @throws std::invalid_argument for unknown, duplicate, or missing values.
 * @throws std::bad_alloc when path/string ownership cannot allocate.
 */
I1RunnerOptions parse_options(int argc, char** argv) {
  I1RunnerOptions options;
  bool saw_output = false;
  bool saw_replicate = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      options.help = true;
      continue;
    }
    if (argument == "--output-dir") {
      if (saw_output || index + 1 >= argc) {
        throw std::invalid_argument(
            "--output-dir must appear exactly once with a value");
      }
      saw_output = true;
      options.output_directory = argv[++index];
      continue;
    }
    if (argument == "--replicate-ordinal") {
      if (saw_replicate || index + 1 >= argc) {
        throw std::invalid_argument(
            "--replicate-ordinal must appear at most once with a value");
      }
      saw_replicate = true;
      options.replicate_ordinal = parse_replicate_ordinal(argv[++index]);
      continue;
    }
    throw std::invalid_argument("unknown argument: " + std::string(argument));
  }
  if (!options.help && !saw_output) {
    throw std::invalid_argument("--output-dir is required");
  }
  return options;
}

/**
 * @brief Tests whether one normalized path is equal to or below another.
 * @param candidate Absolute normalized candidate.
 * @param root Absolute normalized containment root.
 * @return True when every root component prefixes candidate.
 * @throws Nothing.
 */
bool path_is_within(const std::filesystem::path& candidate,
                    const std::filesystem::path& root) noexcept {
  auto candidate_part = candidate.begin();
  for (auto root_part = root.begin(); root_part != root.end(); ++root_part) {
    if (candidate_part == candidate.end() || *candidate_part != *root_part) {
      return false;
    }
    ++candidate_part;
  }
  return true;
}

/**
 * @brief Validates and creates one fresh disposable result directory.
 * @param requested Explicit absolute caller path.
 * @return Weakly canonical output path outside the checkout.
 * @throws std::invalid_argument for unsafe/nonempty/non-absolute paths.
 * @throws std::filesystem::filesystem_error for filesystem query/create
 * failures.
 * @note Existing contents are never deleted or overwritten by validation.
 */
std::filesystem::path prepare_output_directory(
    const std::filesystem::path& requested) {
  if (requested.empty() || !requested.is_absolute()) {
    throw std::invalid_argument("--output-dir must be an absolute path");
  }
  const std::filesystem::path project_root =
      std::filesystem::weakly_canonical(PHOTOSPIDER_I1_PROJECT_SOURCE_DIR);
  const std::filesystem::path output =
      std::filesystem::weakly_canonical(requested);
  if (path_is_within(output, project_root)) {
    throw std::invalid_argument(
        "--output-dir must be outside the Photospider checkout");
  }
  if (std::filesystem::exists(output)) {
    if (!std::filesystem::is_directory(output) ||
        !std::filesystem::is_empty(output)) {
      throw std::invalid_argument(
          "--output-dir must be absent or an empty directory");
    }
  } else {
    const std::filesystem::path parent = output.parent_path();
    if (parent.empty() || !std::filesystem::is_directory(parent)) {
      throw std::invalid_argument(
          "--output-dir parent must already exist and be a directory");
    }
    if (!std::filesystem::create_directory(output)) {
      throw std::runtime_error("failed to create the output directory");
    }
  }
  return output;
}

/**
 * @brief Writes one complete text artifact without silent partial success.
 * @param path Fresh destination below the explicit output directory.
 * @param content Complete bytes to write.
 * @return Nothing after flush and close succeed.
 * @throws std::runtime_error when open/write/flush/close fails.
 */
void write_text_file(const std::filesystem::path& path,
                     std::string_view content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("failed to open output file: " + path.string());
  }
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write output file: " + path.string());
  }
}

/**
 * @brief Converts a steady-clock point to its retained signed nanosecond tick.
 * @param value Point in the runner's process monotonic domain.
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
         monotonic_nanoseconds(edit.accepted_coordinate->admission_time)},
        {"event_sequence", edit.accepted_coordinate->event_sequence},
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
      {"admission_sample_ns", monotonic_nanoseconds(edit.admission_sample)},
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
    generations.push_back(
        Json{{"edit_index", event.edit_index},
             {"generation", event.generation},
             {"observed_at_ns", monotonic_nanoseconds(event.observed_at)},
             {"causal_sequence", event.causal_sequence}});
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
      product =
          Json{{"edit_index", edit_index},
               {"generation", identity.generation},
               {"run_id", identity.run_id.has_value() ? Json(*identity.run_id)
                                                      : Json(nullptr)}};
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

/**
 * @brief Builds the exact baseline node-one YAML used between episodes.
 * @return Complete node replacement with `k=0.80` and source edge intact.
 * @throws std::bad_alloc when string ownership cannot allocate.
 */
std::string i1_baseline_node_one_yaml() {
  return R"YAML(id: 1
name: i1_curve_one
type: image_process
subtype: curve_transform
image_inputs:
  - from_node_id: 0
parameters:
  k: 0.80
)YAML";
}

/**
 * @brief Converts one failed Host status into a runner exception.
 * @param operation Stable operation label.
 * @param status Status to require as success.
 * @return Nothing on success.
 * @throws std::runtime_error containing the exact status diagnostic on failure.
 */
void require_success(std::string_view operation,
                     const OperationStatus& status) {
  if (!status.ok) {
    throw std::runtime_error(std::string(operation) +
                             " failed: " + status.message);
  }
}

/**
 * @brief Restores and fully materializes the exact baseline graph output.
 * @param host Embedded Host owning the loaded session.
 * @param session Exact frozen graph session.
 * @return Nothing after synchronous product settlement.
 * @throws Host failures as std::runtime_error and allocation errors unchanged.
 * @note This work occurs outside an episode, within the fixed terminal guard.
 */
void prepare_episode_baseline(Host& host, const GraphSessionId& session) {
  const VoidResult mutated =
      host.set_node_yaml(session, NodeId{1}, i1_baseline_node_one_yaml());
  require_success("baseline node mutation", mutated.status);
  HostComputeRequest request = make_i1_host_compute_request(session, 0U);
  request.dirty_roi = PixelRect{0, 0, 2048, 2048};
  const VoidResult computed = host.compute(request);
  require_success("baseline materialization", computed.status);
}

/**
 * @brief Closes one loaded graph on every normal or exceptional exit.
 * @throws Nothing from destruction; explicit close reports status separately.
 */
class ScopedGraphClose final {
 public:
  /**
   * @brief Binds cleanup to one live Host/session pair.
   * @param host Host that outlives this guard.
   * @param session Loaded session identity.
   * @throws std::bad_alloc when session identity copying allocates.
   */
  ScopedGraphClose(Host& host, GraphSessionId session)
      : host_(host), session_(std::move(session)) {}

  /** @brief Best-effort closes an active session. @throws Nothing. */
  ~ScopedGraphClose() noexcept {
    if (active_) {
      try {
        (void)host_.close_graph(session_);
      } catch (...) {
      }
    }
  }

  /** @brief Prevents duplicate graph-close ownership. */
  ScopedGraphClose(const ScopedGraphClose&) = delete;
  /** @brief Prevents replacing graph-close ownership. */
  ScopedGraphClose& operator=(const ScopedGraphClose&) = delete;

  /**
   * @brief Closes now and reports the exact product status.
   * @return Close operation result.
   * @throws Host allocation failures unchanged.
   */
  VoidResult close_now() {
    VoidResult result = host_.close_graph(session_);
    if (result.status.ok) {
      active_ = false;
    }
    return result;
  }

 private:
  /** @brief Borrowed Host retaining session lifecycle authority. */
  Host& host_;
  /** @brief Exact loaded session to close once. */
  GraphSessionId session_;
  /** @brief True while this guard still owes close. */
  bool active_ = true;
};

/**
 * @brief Drops raw heavyweight Value/lifecycle ownership after NDJSON write.
 * @param row Evaluated row retained only for replicate aggregation.
 * @return Nothing after fields unused by `evaluate_i1_replicate` are cleared.
 * @throws Nothing under vector/Value destruction.
 * @note Derived digest, latency, service, verdicts, slot, and grid remain.
 */
void compact_row_for_summary(I1EpisodeInnerRow* row) noexcept {
  row->evidence.edits = {};
  row->evidence.observations = {};
  row->evidence.baseline = {};
  row->evidence.final_snapshot = {};
  row->accepted_products = {};
  row->validity_reasons.clear();
}

/**
 * @brief Reports whether one episode has any incomplete evidence dimension.
 * @param row Evaluated inner row.
 * @return True when at least one independent verdict is Invalid.
 * @throws Nothing.
 */
bool row_is_invalid(const I1EpisodeInnerRow& row) noexcept {
  return row.latency_verdict == I1Verdict::Invalid ||
         row.waste_verdict == I1Verdict::Invalid ||
         row.memory_verdict == I1Verdict::Invalid ||
         row.output_verdict == I1Verdict::Invalid;
}

/**
 * @brief Executes one exact continuous-grid I1 replicate and writes evidence.
 * @param options Validated runner options.
 * @param output_directory Fresh explicit result root.
 * @return Evaluated replicate summary after normal product close.
 * @throws std::runtime_error for setup, cadence, admission, settlement, I/O,
 * or invalid-evidence aborts; lower-level allocation/system errors propagate.
 * @note The function never shifts or backfills a nominal time. An invalid
 * admission synchronously closes the Graph to revoke publication and
 * cancel/drain earlier generations before throwing; every other exceptional
 * exit retains the same graph-close guard, and no later edit or slot is
 * submitted. At `Q_end` it first captures the shared causal-history cut, then
 * consumes futures and an eventual resource snapshot; only product lifecycle
 * and Host-settlement coordinates preceding that cut prove boundary membership.
 */
I1ReplicateSummary run_exact_replicate(
    const I1RunnerOptions& options,
    const std::filesystem::path& output_directory) {
  const std::filesystem::path graph_path = output_directory / "i1-graph.yaml";
  write_text_file(graph_path, i1_frozen_graph_yaml());

  std::unique_ptr<Host> host = create_embedded_host();
  if (!host) {
    throw std::runtime_error("failed to create embedded Host");
  }
  require_success("seed_builtin_ops", host->seed_builtin_ops().status);
  HostExecutionConfig execution_config;
  execution_config.worker_count = 8U;
  require_success("configure_execution_defaults",
                  host->configure_execution_defaults(execution_config).status);

  GraphLoadRequest load;
  load.session = GraphSessionId{"i1-edit-storm-v1-r" +
                                std::to_string(options.replicate_ordinal)};
  load.root_dir = (output_directory / "sessions").string();
  load.yaml_path = graph_path.string();
  load.cache_root_dir = (output_directory / "cache").string();
  const Result<GraphSessionId> loaded = host->load_graph(load);
  require_success("load_graph", loaded.status);
  ScopedGraphClose graph_close(*host, loaded.value);

  I1Host* const i1_host = as_i1_host(*host);
  if (i1_host == nullptr) {
    throw std::runtime_error("embedded Host does not expose private I1 seam");
  }

  prepare_episode_baseline(*host, loaded.value);
  const auto grid_origin = checked_i1_time_add(std::chrono::steady_clock::now(),
                                               std::chrono::seconds(1));
  const auto terminal_boundary = i1_terminal_boundary(grid_origin);
  const Json invocation{
      {"schema", "execution-profile-i1-manual-invocation-v1"},
      {"workload_id", kI1WorkloadId},
      {"replicate_ordinal", options.replicate_ordinal},
      {"grid_origin_ns", monotonic_nanoseconds(grid_origin)},
      {"terminal_boundary_ns", monotonic_nanoseconds(terminal_boundary)},
      {"output_directory", output_directory.string()},
      {"worker_count", 8},
      {"workload_contract", workload_contract_json()},
      {"outer_canonical_envelope_claim", false},
  };
  write_text_file(output_directory / "invocation.json",
                  invocation.dump(2) + "\n");

  std::ofstream episode_output(output_directory / "episodes.ndjson",
                               std::ios::binary | std::ios::trunc);
  if (!episode_output) {
    throw std::runtime_error("failed to open episodes.ndjson");
  }

  std::optional<ContentDigest> expected_digest;
  std::vector<I1EpisodeInnerRow> compact_rows;
  compact_rows.reserve(kI1GridSlotCount);
  for (std::size_t slot = 0U; slot < kI1GridSlotCount; ++slot) {
    const auto episode_origin = i1_episode_origin(grid_origin, slot);
    if (slot != 0U) {
      prepare_episode_baseline(*host, loaded.value);
    }
    if (std::chrono::steady_clock::now() > episode_origin) {
      throw std::runtime_error(
          "baseline preparation missed fixed episode origin at slot " +
          std::to_string(slot));
    }
    const I1ExecutionSnapshot baseline =
        i1_host->i1_execution_snapshot(0U, 4096U);
    if (std::chrono::steady_clock::now() > episode_origin) {
      throw std::runtime_error(
          "baseline evidence missed fixed episode origin at slot " +
          std::to_string(slot));
    }

    I1EpisodeObservationCollector observations;
    I1AcceptedBoundaryCollector admissions(
        *i1_host, [] { return std::chrono::steady_clock::now(); },
        [](std::chrono::steady_clock::time_point target) {
          std::this_thread::sleep_until(target);
        });
    std::array<I1EditAdmissionResult, kI1EditCount> admission_results;
    for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
      const auto nominal = checked_i1_time_add(
          episode_origin,
          std::chrono::nanoseconds(kI1EditStride.count() *
                                   static_cast<std::int64_t>(edit_index)));
      std::this_thread::sleep_until(nominal);
      const auto latest_admission =
          checked_i1_time_add(nominal, kI1AdmissionLateness);
      if (std::chrono::steady_clock::now() <= latest_admission) {
        const VoidResult mutated = host->set_node_yaml(
            loaded.value, NodeId{1}, i1_edit_node_one_yaml(edit_index));
        require_success("I1 edit mutation", mutated.status);
      }
      admission_results[edit_index] = admissions.admit_edit(
          episode_origin, edit_index,
          make_i1_host_compute_request(loaded.value, edit_index),
          observations.make_edit_sink(edit_index));
      const I1EditAdmissionResult& admission = admission_results[edit_index];
      if (!admission.accepted_coordinate.has_value()) {
        const VoidResult revoked = graph_close.close_now();
        require_success("I1 invalid-admission publication revocation",
                        revoked.status);
        throw std::runtime_error(
            "I1 admission invalid/failed; graph-close cancellation was "
            "accepted without backfill at slot " +
            std::to_string(slot) + ", edit " + std::to_string(edit_index));
      }
    }

    const auto measurement_start =
        checked_i1_time_add(episode_origin, kI1MeasurementStartOffset);
    const auto measurement_end =
        checked_i1_time_add(episode_origin, kI1MeasurementEndOffset);
    std::this_thread::sleep_until(measurement_end);
    const I1ObservationHistoryCut observation_cut =
        observations.capture_history_cut();
    std::array<std::optional<OperationStatus>, kI1EditCount> settlements;
    for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
      std::future<OperationStatus>& future =
          admission_results[edit_index].settlement;
      if (!future.valid() || future.wait_for(std::chrono::nanoseconds(0)) !=
                                 std::future_status::ready) {
        throw std::runtime_error(
            "I1 settlement remained active at Q_end for slot " +
            std::to_string(slot) + ", edit " + std::to_string(edit_index));
      }
      settlements[edit_index] = future.get();
    }
    const auto settlement_publication_guard =
        slot + 1U < kI1GridSlotCount ? i1_episode_origin(grid_origin, slot + 1U)
                                     : terminal_boundary;
    while (observations.published_host_settlement_count() < kI1EditCount &&
           std::chrono::steady_clock::now() < settlement_publication_guard) {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    if (observations.published_host_settlement_count() != kI1EditCount) {
      throw std::runtime_error(
          "I1 Host settlement evidence missed the terminal guard at slot " +
          std::to_string(slot));
    }
    const I1ExecutionSnapshot final_snapshot =
        i1_host->i1_execution_snapshot(baseline.lifecycle.snapshot_cut, 4096U);
    const auto final_snapshot_sample = std::chrono::steady_clock::now();
    const I1EpisodeObservationSnapshot observation_snapshot =
        observations.snapshot();

    I1EpisodeEvidenceInput input;
    input.replicate_ordinal = options.replicate_ordinal;
    input.slot = slot;
    input.grid_origin = grid_origin;
    input.episode_origin = episode_origin;
    input.terminal_boundary = terminal_boundary;
    input.measurement_start = measurement_start;
    input.measurement_end = measurement_end;
    input.observation_cut = observation_cut;
    input.observations = observation_snapshot;
    input.baseline = baseline;
    input.final_snapshot = final_snapshot;
    input.final_snapshot_sample = final_snapshot_sample;
    input.expected_final_digest = expected_digest;
    for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
      input.edits[edit_index] = capture_i1_edit_evidence(
          admission_results[edit_index], std::move(settlements[edit_index]));
    }

    I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
    episode_output << inner_row_json(row).dump() << '\n';
    episode_output.flush();
    if (!episode_output) {
      throw std::runtime_error("failed to append episodes.ndjson");
    }
    std::cerr << "I1 slot " << slot + 1U << '/' << kI1GridSlotCount << " ("
              << phase_text(classify_i1_slot(slot).first) << ") recorded\n";

    if (!expected_digest.has_value() && row.final_digest.digest.has_value()) {
      expected_digest = row.final_digest.digest;
    }
    if (row_is_invalid(row)) {
      throw std::runtime_error(
          "I1 row became invalid; later fixed slots were not backfilled");
    }
    compact_row_for_summary(&row);
    compact_rows.push_back(std::move(row));
  }
  episode_output.close();
  if (!episode_output) {
    throw std::runtime_error("failed to close episodes.ndjson");
  }

  const I1ReplicateSummary summary = evaluate_i1_replicate(compact_rows);
  write_text_file(output_directory / "summary.json",
                  replicate_summary_json(summary).dump(2) + "\n");
  require_success("close_graph", graph_close.close_now().status);
  return summary;
}

}  // namespace
}  // namespace ps::benchmark

/**
 * @brief Runs one exact manual I1 replicate or prints the strict usage text.
 * @param argc Command-line argument count.
 * @param argv Command-line argument vector.
 * @return Zero only when all four independent verdicts pass, two for a
 * complete failing replicate, and one for parsing/setup/invalid evidence.
 * @throws Nothing; all standard exceptions are converted to stderr/failure
 * JSON when a safe explicit output directory was already prepared.
 * @note This executable is EXCLUDE_FROM_ALL and absent from CTest/default CI.
 */
int main(int argc, char** argv) {
  std::optional<std::filesystem::path> output_directory;
  try {
    const ps::benchmark::I1RunnerOptions options =
        ps::benchmark::parse_options(argc, argv);
    if (options.help) {
      ps::benchmark::print_usage(std::cout);
      return 0;
    }
    output_directory =
        ps::benchmark::prepare_output_directory(options.output_directory);
    const ps::benchmark::I1ReplicateSummary summary =
        ps::benchmark::run_exact_replicate(options, *output_directory);
    const bool passed =
        summary.latency_verdict == ps::benchmark::I1Verdict::Pass &&
        summary.waste_verdict == ps::benchmark::I1Verdict::Pass &&
        summary.memory_verdict == ps::benchmark::I1Verdict::Pass &&
        summary.output_verdict == ps::benchmark::I1Verdict::Pass;
    return passed ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << "i1_edit_storm_benchmark: " << error.what() << '\n';
    if (output_directory.has_value()) {
      try {
        const ps::benchmark::Json failure{
            {"schema", "execution-profile-i1-manual-failure-v1"},
            {"workload_id", ps::benchmark::kI1WorkloadId},
            {"diagnostic", error.what()},
            {"later_slots_backfilled", false},
            {"outer_canonical_envelope_claim", false},
        };
        ps::benchmark::write_text_file(*output_directory / "failure.json",
                                       failure.dump(2) + "\n");
      } catch (...) {
      }
    }
    return 1;
  }
}
