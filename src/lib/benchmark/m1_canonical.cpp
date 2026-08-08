/**
 * @file m1_canonical.cpp
 * @brief Implements reversible canonical M1 inner evidence replay.
 */
#include "benchmark/m1_canonical.hpp"  // NOLINT(build/include_subdir)

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "benchmark/b1_environment.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {
namespace {

/** @brief Exact ordered field names for the closed M1 v2 inner manifest. */
constexpr std::string_view kFieldNames[]{"schema_version",
                                         "replicate_ordinal",
                                         "boundaries",
                                         "protocol_flags",
                                         "interactive_occurrences",
                                         "interactive_sources",
                                         "batch_offers",
                                         "carryover",
                                         "first_measured_admission",
                                         "progress_windows",
                                         "graph_service_windows",
                                         "class_starts",
                                         "headroom_outcomes",
                                         "batch_sources",
                                         "temporal_snapshots",
                                         "mixed_observations",
                                         "paired_isolated_i1_p99_ns",
                                         "paired_isolated_b1_source",
                                         "batch_waste",
                                         "verdicts"};  // NOLINT

/** @brief Exact ordered field types for the closed M1 v2 inner manifest. */
constexpr std::string_view kFieldTypes[]{
    "uint64",
    "uint64",
    "m1-boundary-record-v2",
    "m1-protocol-flags-v2",
    "m1-i1-occurrence-list-v2",
    "m1-i1-source-list-v2",
    "m1-b1-offer-list-v2",
    "m1-carryover-list-v2",
    "m1-first-admission-record-v2",
    "m1-progress-window-list-v2",
    "m1-graph-service-window-list-v2",
    "m1-class-start-list-v2",
    "m1-headroom-outcome-list-v2",
    "m1-b1-source-list-v2",
    "m1-execution-snapshot-list-v2",
    "m1-observation-snapshot-v2",
    "uint64",
    "m1-b1-rate-source-v2",
    "m1-batch-waste-record-v2",
    "m1-five-axis-verdict-record-v2"};  // NOLINT

static_assert(std::size(kFieldNames) == std::size(kFieldTypes));

/**
 * @brief Creates one exact known canonical field.
 * @param name Closed field name.
 * @param type Closed field type.
 * @param payload Nonempty canonical payload.
 * @return Complete known field.
 * @throws std::invalid_argument when payload is empty.
 * @throws std::bad_alloc when owned strings allocate.
 */
B1CanonicalField known_field(std::string name, std::string type,
                             std::string payload) {
  if (payload.empty()) {
    throw std::invalid_argument("M1 canonical known payload is empty.");
  }
  return B1CanonicalField{std::move(name), B1ObservationState::Known, "none",
                          std::move(type), std::move(payload)};
}

/**
 * @brief Encodes one ordered list of already-canonical records.
 * @param records Complete records in semantic order.
 * @return Count prefix plus one canonical frame per record.
 * @throws std::bad_alloc when output ownership allocates.
 */
std::string encode_record_list(const std::vector<std::string>& records) {
  std::string result = std::to_string(records.size()) + ":";
  for (const std::string& record : records) {
    result.append(b1_environment_frame(record));
  }
  return result;
}

/**
 * @brief Returns the canonical lowercase boolean token.
 * @param value Boolean value.
 * @return `true` or `false`.
 * @throws Nothing.
 */
const char* boolean_text(bool value) noexcept {
  return value ? "true" : "false";
}

/**
 * @brief Encodes arbitrary retained source bytes as canonical lowercase hex.
 * @param bytes Exact byte sequence, including any embedded line terminators.
 * @return Two lowercase hexadecimal digits per input byte.
 * @throws std::length_error when doubling the input size is unrepresentable.
 * @throws std::bad_alloc when output ownership allocates.
 * @note This byte codec does not normalize text and therefore preserves source
 * identity while keeping the outer line-oriented manifest closed.
 */
std::string encode_source_bytes(std::string_view bytes) {
  if (bytes.size() > std::numeric_limits<std::size_t>::max() / 2U) {
    throw std::length_error("M1 source byte payload is too large.");
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded(bytes.size() * 2U, '0');
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    const auto value = static_cast<unsigned char>(bytes[index]);
    encoded[index * 2U] = kHex[value >> 4U];
    encoded[index * 2U + 1U] = kHex[value & 0x0fU];
  }
  return encoded;
}

/**
 * @brief Decodes one canonical lowercase-hex source byte sequence.
 * @param encoded Even-length lowercase hexadecimal spelling.
 * @return Exact original bytes without UTF-8 or path normalization.
 * @throws std::invalid_argument for odd length, uppercase, or non-hex input.
 * @throws std::bad_alloc when output ownership allocates.
 */
std::string parse_source_bytes(std::string_view encoded) {
  if (encoded.size() % 2U != 0U) {
    throw std::invalid_argument("M1 source byte payload has odd hex length.");
  }
  const auto nibble = [](char value) -> unsigned char {
    if (value >= '0' && value <= '9') {
      return static_cast<unsigned char>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
      return static_cast<unsigned char>(value - 'a' + 10);
    }
    throw std::invalid_argument(
        "M1 source byte payload is not canonical lowercase hex.");
  };
  std::string bytes(encoded.size() / 2U, '\0');
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<char>(
        static_cast<unsigned char>(nibble(encoded[index * 2U]) << 4U) |
        nibble(encoded[index * 2U + 1U]));
  }
  return bytes;
}

/**
 * @brief Parses one canonical lowercase boolean token.
 * @param value Candidate token.
 * @return Parsed boolean.
 * @throws std::invalid_argument for any other spelling.
 */
bool parse_boolean(std::string_view value) {
  if (value == "true") {
    return true;
  }
  if (value == "false") {
    return false;
  }
  throw std::invalid_argument("M1 canonical boolean is invalid.");
}

/**
 * @brief Parses one canonical signed decimal integer.
 * @param value Candidate signed token.
 * @return Exact signed 64-bit value.
 * @throws std::invalid_argument for overflow or noncanonical spelling.
 */
std::int64_t parse_int64(std::string_view value) {
  if (value.empty() || value.front() == '+' ||
      (value.size() > 1U && value.front() == '0') || value == "-0" ||
      (value.size() > 2U && value[0U] == '-' && value[1U] == '0')) {
    throw std::invalid_argument("M1 canonical int64 spelling is invalid.");
  }
  std::int64_t result = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    throw std::invalid_argument("M1 canonical int64 is invalid.");
  }
  return result;
}

/**
 * @brief Narrows one canonical uint64 to `size_t`.
 * @param value Candidate token.
 * @return Exact platform `size_t` value.
 * @throws std::invalid_argument when the value does not fit.
 */
std::size_t parse_size(std::string_view value) {
  const std::uint64_t parsed = parse_b1_canonical_uint64(value);
  if (parsed > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument("M1 canonical size exceeds size_t.");
  }
  return static_cast<std::size_t>(parsed);
}

/**
 * @brief Narrows one canonical uint64 to `uint32_t`.
 * @param value Candidate token.
 * @return Exact 32-bit unsigned value.
 * @throws std::invalid_argument when the value does not fit.
 */
std::uint32_t parse_uint32(std::string_view value) {
  const std::uint64_t parsed = parse_b1_canonical_uint64(value);
  if (parsed > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("M1 canonical uint32 exceeds range.");
  }
  return static_cast<std::uint32_t>(parsed);
}

/**
 * @brief Narrows one canonical signed integer to `int32_t`.
 * @param value Candidate token.
 * @return Exact 32-bit signed value.
 * @throws std::invalid_argument when the value does not fit.
 */
std::int32_t parse_int32(std::string_view value) {
  const std::int64_t parsed = parse_int64(value);
  if (parsed < std::numeric_limits<std::int32_t>::min() ||
      parsed > std::numeric_limits<std::int32_t>::max()) {
    throw std::invalid_argument("M1 canonical int32 exceeds range.");
  }
  return static_cast<std::int32_t>(parsed);
}

/**
 * @brief Encodes one steady-clock time point as signed nanoseconds.
 * @param value Exact process-monotonic point.
 * @return Canonical signed decimal nanosecond value.
 * @throws std::overflow_error when duration conversion is not exact.
 */
std::string encode_time(std::chrono::steady_clock::time_point value) {
  const auto source = value.time_since_epoch();
  const auto converted =
      std::chrono::duration_cast<std::chrono::nanoseconds>(source);
  if (std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          converted) != source) {
    throw std::overflow_error("M1 monotonic time is not nanosecond-exact.");
  }
  return std::to_string(converted.count());
}

/**
 * @brief Decodes one signed nanosecond monotonic point.
 * @param value Canonical signed decimal token.
 * @return Exact steady-clock time point.
 * @throws std::invalid_argument when the value cannot round-trip exactly.
 */
std::chrono::steady_clock::time_point parse_time(std::string_view value) {
  const std::chrono::nanoseconds source(parse_int64(value));
  const auto converted =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(source);
  if (std::chrono::duration_cast<std::chrono::nanoseconds>(converted) !=
      source) {
    throw std::invalid_argument("M1 monotonic time does not round-trip.");
  }
  return std::chrono::steady_clock::time_point(converted);
}

/**
 * @brief Returns the exact lowercase verdict token.
 * @param verdict Closed verdict.
 * @return `pass`, `fail`, or `invalid`.
 * @throws std::invalid_argument for an unknown enum representation.
 */
const char* verdict_text(I1Verdict verdict) {
  switch (verdict) {
    case I1Verdict::Pass:
      return "pass";
    case I1Verdict::Fail:
      return "fail";
    case I1Verdict::Invalid:
      return "invalid";
  }
  throw std::invalid_argument("M1 verdict enum is unknown.");
}

/**
 * @brief Parses one exact lowercase verdict token.
 * @param value Candidate token.
 * @return Closed verdict.
 * @throws std::invalid_argument for an unknown token.
 */
I1Verdict parse_verdict(std::string_view value) {
  if (value == "pass") {
    return I1Verdict::Pass;
  }
  if (value == "fail") {
    return I1Verdict::Fail;
  }
  if (value == "invalid") {
    return I1Verdict::Invalid;
  }
  throw std::invalid_argument("M1 canonical verdict is invalid.");
}

/**
 * @brief Returns the exact lowercase phase token.
 * @param phase Closed job phase.
 * @return `cold`, `warmup`, or `measured`.
 * @throws std::invalid_argument for an unknown enum representation.
 */
const char* phase_text(B1JobPhase phase) {
  switch (phase) {
    case B1JobPhase::Cold:
      return "cold";
    case B1JobPhase::Warmup:
      return "warmup";
    case B1JobPhase::Measured:
      return "measured";
  }
  throw std::invalid_argument("M1 phase enum is unknown.");
}

/**
 * @brief Parses one exact lowercase job phase.
 * @param value Candidate token.
 * @return Closed phase.
 * @throws std::invalid_argument for an unknown token.
 */
B1JobPhase parse_phase(std::string_view value) {
  if (value == "cold") {
    return B1JobPhase::Cold;
  }
  if (value == "warmup") {
    return B1JobPhase::Warmup;
  }
  if (value == "measured") {
    return B1JobPhase::Measured;
  }
  throw std::invalid_argument("M1 canonical phase is invalid.");
}

/**
 * @brief Parses and validates one canonical B1 job identity.
 * @param record Six-component canonical occurrence record.
 * @return Complete validated job identity.
 * @throws std::invalid_argument for framing, enum, identity, or spelling drift.
 */
B1JobInstance parse_job(std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 6U);
  B1JobInstance result{fields[0U],
                       parse_b1_canonical_uint64(fields[1U]),
                       parse_phase(fields[2U]),
                       parse_b1_canonical_uint64(fields[3U]),
                       parse_b1_canonical_uint64(fields[4U]),
                       parse_b1_canonical_uint64(fields[5U])};
  validate_b1_job_instance(result);
  if (encode_b1_job_instance(result) != record) {
    throw std::invalid_argument("M1 B1 job identity is noncanonical.");
  }
  return result;
}

/**
 * @brief Parses a closed zero-based enum encoded as uint64.
 * @tparam Enum Enum type.
 * @param value Candidate numeric token.
 * @param maximum Inclusive maximum numeric representation.
 * @param message Stable failure diagnostic.
 * @return Checked enum value.
 * @throws std::invalid_argument when representation exceeds `maximum`.
 */
template <typename Enum>
Enum parse_zero_based_enum(std::string_view value, std::uint64_t maximum,
                           const char* message) {
  const std::uint64_t parsed = parse_b1_canonical_uint64(value);
  if (parsed > maximum) {
    throw std::invalid_argument(message);
  }
  return static_cast<Enum>(parsed);
}

/**
 * @brief Encodes one complete Host resource vector.
 * @param value Exact vector.
 * @return Five-component canonical record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_resource_vector(const ResourceVector& value) {
  return encode_b1_fixed_record({std::to_string(value.cpu_slots),
                                 std::to_string(value.retained_memory_bytes),
                                 std::to_string(value.scratch_bytes),
                                 std::to_string(value.ready_entries),
                                 std::to_string(value.ready_bytes)});
}

/**
 * @brief Parses one complete Host resource vector.
 * @param record Five-component canonical record.
 * @return Exact vector.
 * @throws std::invalid_argument for framing or numeric drift.
 */
ResourceVector parse_resource_vector(std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 5U);
  return ResourceVector{parse_b1_canonical_uint64(fields[0U]),
                        parse_b1_canonical_uint64(fields[1U]),
                        parse_b1_canonical_uint64(fields[2U]),
                        parse_b1_canonical_uint64(fields[3U]),
                        parse_b1_canonical_uint64(fields[4U])};
}

/**
 * @brief Encodes one complete Compute I/O executor snapshot.
 * @param value Exact snapshot.
 * @return Nine-component canonical record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_io_snapshot(
    const execution::ComputeIoExecutorSnapshot& value) {
  return encode_b1_fixed_record(
      {std::to_string(value.task_limit),
       std::to_string(value.planned_bytes_limit),
       std::to_string(value.active_tasks),
       std::to_string(value.active_planned_bytes),
       std::to_string(value.constructing_tasks),
       std::to_string(value.queued_tasks), std::to_string(value.running_tasks),
       boolean_text(value.accepting), boolean_text(value.shutdown_complete)});
}

/**
 * @brief Parses one complete Compute I/O executor snapshot.
 * @param record Nine-component canonical record.
 * @return Exact snapshot.
 * @throws std::invalid_argument for framing, numeric, or boolean drift.
 */
execution::ComputeIoExecutorSnapshot parse_io_snapshot(
    std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 9U);
  return execution::ComputeIoExecutorSnapshot{
      parse_b1_canonical_uint64(fields[0U]),
      parse_b1_canonical_uint64(fields[1U]),
      parse_b1_canonical_uint64(fields[2U]),
      parse_b1_canonical_uint64(fields[3U]),
      parse_b1_canonical_uint64(fields[4U]),
      parse_b1_canonical_uint64(fields[5U]),
      parse_b1_canonical_uint64(fields[6U]),
      parse_boolean(fields[7U]),
      parse_boolean(fields[8U])};
}

/**
 * @brief Encodes all fifteen lifecycle counters without aggregation.
 * @param value Exact counter vector.
 * @return Fifteen-component canonical record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_lifecycle_counters(
    const compute::ExecutionLifecycleCounters& value) {
  return encode_b1_fixed_record(
      {std::to_string(value.registered_graph_count),
       std::to_string(value.open_graph_count),
       std::to_string(value.closing_graph_count),
       std::to_string(value.pending_candidate_count),
       std::to_string(value.admitted_standalone_run_count),
       std::to_string(value.admitted_run_group_count),
       std::to_string(value.admitted_child_run_count),
       std::to_string(value.terminal_not_quiescent_run_count),
       std::to_string(value.finalizing_run_count),
       std::to_string(value.ready_entry_count),
       std::to_string(value.entered_callback_count),
       std::to_string(value.live_root_reservation_count),
       std::to_string(value.live_child_grant_count),
       std::to_string(value.live_policy_invocation_count),
       std::to_string(value.live_policy_binding_count)});
}

/**
 * @brief Parses all fifteen lifecycle counters without aggregation.
 * @param record Fifteen-component canonical record.
 * @return Exact counter vector.
 * @throws std::invalid_argument for framing or numeric drift.
 */
compute::ExecutionLifecycleCounters parse_lifecycle_counters(
    std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 15U);
  compute::ExecutionLifecycleCounters value;
  std::uint64_t* members[]{&value.registered_graph_count,
                           &value.open_graph_count,
                           &value.closing_graph_count,
                           &value.pending_candidate_count,
                           &value.admitted_standalone_run_count,
                           &value.admitted_run_group_count,
                           &value.admitted_child_run_count,
                           &value.terminal_not_quiescent_run_count,
                           &value.finalizing_run_count,
                           &value.ready_entry_count,
                           &value.entered_callback_count,
                           &value.live_root_reservation_count,
                           &value.live_child_grant_count,
                           &value.live_policy_invocation_count,
                           &value.live_policy_binding_count};
  for (std::size_t index = 0U; index < std::size(members); ++index) {
    *members[index] = parse_b1_canonical_uint64(fields[index]);
  }
  return value;
}

/**
 * @brief Encodes one complete lifecycle page and every retained event.
 * @param page Exact source-private page.
 * @return Sixteen-component page record with nested event list.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_lifecycle_page(const compute::ExecutionLifecyclePage& page) {
  std::vector<std::string> records;
  records.reserve(page.records.size());
  for (const compute::ExecutionLifecycleEvent& event : page.records) {
    records.push_back(encode_b1_fixed_record(
        {std::to_string(event.schema_version), std::to_string(event.sequence),
         std::to_string(event.timestamp_us),
         boolean_text(event.timestamp_saturated),
         std::to_string(event.service_instance_id),
         std::to_string(event.telemetry_epoch),
         std::to_string(event.graph_instance_id), std::to_string(event.run_id),
         std::to_string(event.run_group_id), std::to_string(event.generation),
         std::to_string(static_cast<std::uint32_t>(event.kind)),
         std::to_string(static_cast<std::uint32_t>(event.category)),
         encode_lifecycle_counters(event.counters)}));
  }
  return encode_b1_fixed_record(
      {std::to_string(page.schema_version), std::to_string(page.capacity),
       std::to_string(page.service_instance_id),
       std::to_string(page.telemetry_epoch),
       std::to_string(static_cast<std::uint32_t>(page.service_state)),
       std::to_string(page.shutdown_generation),
       std::to_string(page.snapshot_cut),
       std::to_string(page.first_retained_sequence),
       std::to_string(page.next_sequence),
       std::to_string(page.global_dropped_total),
       boolean_text(page.global_dropped_saturated),
       encode_lifecycle_counters(page.counters), encode_record_list(records),
       std::to_string(page.cursor_gap), std::to_string(page.next_cursor),
       boolean_text(page.has_more)});
}

/**
 * @brief Parses one complete lifecycle page and every retained event.
 * @param record Sixteen-component canonical page record.
 * @return Exact lifecycle page.
 * @throws std::invalid_argument for nested framing, enum, or numeric drift.
 */
compute::ExecutionLifecyclePage parse_lifecycle_page(std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 16U);
  compute::ExecutionLifecyclePage page;
  page.schema_version = parse_uint32(fields[0U]);
  page.capacity = parse_uint32(fields[1U]);
  page.service_instance_id = parse_b1_canonical_uint64(fields[2U]);
  page.telemetry_epoch = parse_b1_canonical_uint64(fields[3U]);
  const std::uint64_t state = parse_b1_canonical_uint64(fields[4U]);
  if (state < 1U || state > 3U) {
    throw std::invalid_argument("M1 lifecycle service state is unknown.");
  }
  page.service_state =
      static_cast<compute::ExecutionLifecycleServiceState>(state);
  page.shutdown_generation = parse_b1_canonical_uint64(fields[5U]);
  page.snapshot_cut = parse_b1_canonical_uint64(fields[6U]);
  page.first_retained_sequence = parse_b1_canonical_uint64(fields[7U]);
  page.next_sequence = parse_b1_canonical_uint64(fields[8U]);
  page.global_dropped_total = parse_b1_canonical_uint64(fields[9U]);
  page.global_dropped_saturated = parse_boolean(fields[10U]);
  page.counters = parse_lifecycle_counters(fields[11U]);
  for (const std::string& event_record : parse_b1_framed_list(fields[12U])) {
    const std::vector<std::string> event_fields =
        parse_b1_fixed_record(event_record, 13U);
    compute::ExecutionLifecycleEvent event;
    event.schema_version = parse_uint32(event_fields[0U]);
    event.sequence = parse_b1_canonical_uint64(event_fields[1U]);
    event.timestamp_us = parse_b1_canonical_uint64(event_fields[2U]);
    event.timestamp_saturated = parse_boolean(event_fields[3U]);
    event.service_instance_id = parse_b1_canonical_uint64(event_fields[4U]);
    event.telemetry_epoch = parse_b1_canonical_uint64(event_fields[5U]);
    event.graph_instance_id = parse_b1_canonical_uint64(event_fields[6U]);
    event.run_id = parse_b1_canonical_uint64(event_fields[7U]);
    event.run_group_id = parse_b1_canonical_uint64(event_fields[8U]);
    event.generation = parse_b1_canonical_uint64(event_fields[9U]);
    const std::uint64_t kind = parse_b1_canonical_uint64(event_fields[10U]);
    const std::uint64_t category = parse_b1_canonical_uint64(event_fields[11U]);
    if (kind < 1U || kind > 15U || category > 9U) {
      throw std::invalid_argument("M1 lifecycle event enum is unknown.");
    }
    event.kind = static_cast<compute::ExecutionLifecycleEventKind>(kind);
    event.category = static_cast<compute::ExecutionLifecycleCategory>(category);
    event.counters = parse_lifecycle_counters(event_fields[12U]);
    page.records.push_back(std::move(event));
  }
  page.cursor_gap = parse_b1_canonical_uint64(fields[13U]);
  page.next_cursor = parse_b1_canonical_uint64(fields[14U]);
  page.has_more = parse_boolean(fields[15U]);
  return page;
}

/**
 * @brief Encodes one complete M1 temporal execution snapshot.
 * @param snapshot Exact Host/device/I/O/ready/lifecycle cut.
 * @return Eleven-component canonical record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_execution_snapshot(const M1ExecutionSnapshot& snapshot) {
  std::vector<std::string> devices;
  devices.reserve(snapshot.device_resources.size());
  for (const ResourceLedger::DeviceSnapshot& device :
       snapshot.device_resources) {
    devices.push_back(encode_b1_fixed_record(
        {std::to_string(static_cast<std::uint32_t>(device.device.backend())),
         std::to_string(device.device.ordinal()),
         std::to_string(device.limits.device_memory_bytes),
         std::to_string(device.limits.device_scratch_bytes),
         std::to_string(device.reserved.device_memory_bytes),
         std::to_string(device.reserved.device_scratch_bytes),
         std::to_string(device.available.device_memory_bytes),
         std::to_string(device.available.device_scratch_bytes),
         std::to_string(device.high_water.device_memory_bytes),
         std::to_string(device.high_water.device_scratch_bytes)}));
  }
  return encode_b1_fixed_record(
      {encode_resource_vector(snapshot.host_resources.limits),
       encode_resource_vector(snapshot.host_resources.reserved),
       encode_resource_vector(snapshot.host_resources.high_water),
       encode_record_list(devices), encode_io_snapshot(snapshot.compute_io),
       encode_resource_vector(snapshot.throughput.capacity),
       encode_resource_vector(snapshot.throughput.reserved),
       encode_b1_fixed_record(
           {std::to_string(snapshot.ready_classes.interactive_entries),
            std::to_string(snapshot.ready_classes.throughput_entries),
            std::to_string(snapshot.ready_classes.total_entries),
            boolean_text(snapshot.ready_classes.valid)}),
       encode_lifecycle_page(snapshot.lifecycle),
       std::to_string(snapshot.lifecycle_after_cursor),
       std::to_string(snapshot.temporal_capture_ordinal)});
}

/**
 * @brief Parses one complete M1 temporal execution snapshot.
 * @param record Eleven-component canonical record.
 * @return Exact Host/device/I/O/ready/lifecycle cut.
 * @throws std::invalid_argument for nested framing, enum, or numeric drift.
 */
M1ExecutionSnapshot parse_execution_snapshot(std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 11U);
  M1ExecutionSnapshot snapshot;
  snapshot.host_resources.limits = parse_resource_vector(fields[0U]);
  snapshot.host_resources.reserved = parse_resource_vector(fields[1U]);
  snapshot.host_resources.high_water = parse_resource_vector(fields[2U]);
  for (const std::string& device_record : parse_b1_framed_list(fields[3U])) {
    const std::vector<std::string> device_fields =
        parse_b1_fixed_record(device_record, 10U);
    const DeviceBackend backend = parse_zero_based_enum<DeviceBackend>(
        device_fields[0U], 4U, "M1 device backend is unknown.");
    ResourceLedger::DeviceSnapshot device;
    device.device = DeviceId(backend, parse_uint32(device_fields[1U]));
    device.limits =
        DeviceResourceVector{parse_b1_canonical_uint64(device_fields[2U]),
                             parse_b1_canonical_uint64(device_fields[3U])};
    device.reserved =
        DeviceResourceVector{parse_b1_canonical_uint64(device_fields[4U]),
                             parse_b1_canonical_uint64(device_fields[5U])};
    device.available =
        DeviceResourceVector{parse_b1_canonical_uint64(device_fields[6U]),
                             parse_b1_canonical_uint64(device_fields[7U])};
    device.high_water =
        DeviceResourceVector{parse_b1_canonical_uint64(device_fields[8U]),
                             parse_b1_canonical_uint64(device_fields[9U])};
    snapshot.device_resources.push_back(std::move(device));
  }
  snapshot.compute_io = parse_io_snapshot(fields[4U]);
  snapshot.throughput.capacity = parse_resource_vector(fields[5U]);
  snapshot.throughput.reserved = parse_resource_vector(fields[6U]);
  const std::vector<std::string> ready = parse_b1_fixed_record(fields[7U], 4U);
  snapshot.ready_classes.interactive_entries =
      parse_b1_canonical_uint64(ready[0U]);
  snapshot.ready_classes.throughput_entries =
      parse_b1_canonical_uint64(ready[1U]);
  snapshot.ready_classes.total_entries = parse_b1_canonical_uint64(ready[2U]);
  snapshot.ready_classes.valid = parse_boolean(ready[3U]);
  snapshot.lifecycle = parse_lifecycle_page(fields[8U]);
  snapshot.lifecycle_after_cursor = parse_b1_canonical_uint64(fields[9U]);
  snapshot.temporal_capture_ordinal = parse_size(fields[10U]);
  return snapshot;
}

/**
 * @brief Encodes an optional Compute I/O admission event.
 * @param event Optional executor-authored event.
 * @return `not-applicable` or six-component canonical record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_io_admission_event(
    const std::optional<execution::ComputeIoAdmissionEvent>& event) {
  if (!event.has_value()) {
    return "not-applicable";
  }
  return encode_b1_fixed_record(
      {std::to_string(event->sequence),
       std::to_string(static_cast<std::uint32_t>(event->status)),
       std::to_string(event->offered_planned_bytes),
       std::to_string(event->charged_tasks),
       std::to_string(event->charged_planned_bytes),
       encode_io_snapshot(event->snapshot_after)});
}

/**
 * @brief Parses an optional Compute I/O admission event.
 * @param record `not-applicable` or six-component canonical record.
 * @return Exact optional event.
 * @throws std::invalid_argument for nested framing or enum drift.
 */
std::optional<execution::ComputeIoAdmissionEvent> parse_io_admission_event(
    std::string_view record) {
  if (record == "not-applicable") {
    return std::nullopt;
  }
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 6U);
  return execution::ComputeIoAdmissionEvent{
      parse_b1_canonical_uint64(fields[0U]),
      parse_zero_based_enum<execution::ComputeIoAdmissionStatus>(
          fields[1U], 4U, "M1 I/O admission status is unknown."),
      parse_b1_canonical_uint64(fields[2U]),
      parse_b1_canonical_uint64(fields[3U]),
      parse_b1_canonical_uint64(fields[4U]),
      parse_io_snapshot(fields[5U])};
}

/**
 * @brief Encodes an optional Compute I/O settlement event.
 * @param event Optional executor-authored event.
 * @return `not-applicable` or six-component canonical record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_io_settlement_event(
    const std::optional<execution::ComputeIoSettlementEvent>& event) {
  if (!event.has_value()) {
    return "not-applicable";
  }
  return encode_b1_fixed_record(
      {std::to_string(event->sequence),
       std::to_string(event->admission_sequence),
       std::to_string(static_cast<std::uint32_t>(event->status)),
       std::to_string(event->released_tasks),
       std::to_string(event->released_planned_bytes),
       encode_io_snapshot(event->snapshot_after)});
}

/**
 * @brief Parses an optional Compute I/O settlement event.
 * @param record `not-applicable` or six-component canonical record.
 * @return Exact optional event.
 * @throws std::invalid_argument for nested framing or enum drift.
 */
std::optional<execution::ComputeIoSettlementEvent> parse_io_settlement_event(
    std::string_view record) {
  if (record == "not-applicable") {
    return std::nullopt;
  }
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 6U);
  return execution::ComputeIoSettlementEvent{
      parse_b1_canonical_uint64(fields[0U]),
      parse_b1_canonical_uint64(fields[1U]),
      parse_zero_based_enum<execution::ComputeIoCompletionStatus>(
          fields[2U], 2U, "M1 I/O completion status is unknown."),
      parse_b1_canonical_uint64(fields[3U]),
      parse_b1_canonical_uint64(fields[4U]),
      parse_io_snapshot(fields[5U])};
}

/**
 * @brief Encodes one complete B1 Compute I/O observation.
 * @param observation Exact boundary or task transition.
 * @return Eight-component canonical record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_io_observation(const B1ComputeIoObservation& observation) {
  const std::string task =
      observation.task.has_value()
          ? encode_b1_fixed_record(
                {encode_b1_job_instance(observation.task->job),
                 std::to_string(
                     static_cast<std::uint32_t>(observation.task->stage)),
                 std::to_string(observation.task->attempt)})
          : "not-applicable";
  return encode_b1_fixed_record(
      {std::to_string(static_cast<std::uint32_t>(observation.point)), task,
       std::to_string(observation.planned_bytes),
       observation.admission.has_value()
           ? std::to_string(static_cast<std::uint32_t>(*observation.admission))
           : "not-applicable",
       observation.completion.has_value()
           ? std::to_string(static_cast<std::uint32_t>(*observation.completion))
           : "not-applicable",
       encode_io_admission_event(observation.admission_event),
       encode_io_settlement_event(observation.settlement_event),
       encode_io_snapshot(observation.snapshot)});
}

/**
 * @brief Parses one complete B1 Compute I/O observation.
 * @param record Eight-component canonical record.
 * @return Exact boundary or task transition.
 * @throws std::invalid_argument for nested framing, enum, or identity drift.
 */
B1ComputeIoObservation parse_io_observation(std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 8U);
  B1ComputeIoObservation observation;
  observation.point = parse_zero_based_enum<B1IoObservationPoint>(
      fields[0U], 4U, "M1 I/O observation point is unknown.");
  if (fields[1U] != "not-applicable") {
    const std::vector<std::string> task = parse_b1_fixed_record(fields[1U], 3U);
    observation.task =
        B1IoTaskIdentity{parse_job(task[0U]),
                         parse_zero_based_enum<B1IoStage>(
                             task[1U], 1U, "M1 B1 I/O stage is unknown."),
                         parse_b1_canonical_uint64(task[2U])};
  }
  observation.planned_bytes = parse_b1_canonical_uint64(fields[2U]);
  if (fields[3U] != "not-applicable") {
    observation.admission =
        parse_zero_based_enum<execution::ComputeIoAdmissionStatus>(
            fields[3U], 4U, "M1 I/O admission status is unknown.");
  }
  if (fields[4U] != "not-applicable") {
    observation.completion =
        parse_zero_based_enum<execution::ComputeIoCompletionStatus>(
            fields[4U], 2U, "M1 I/O completion status is unknown.");
  }
  observation.admission_event = parse_io_admission_event(fields[5U]);
  observation.settlement_event = parse_io_settlement_event(fields[6U]);
  observation.snapshot = parse_io_snapshot(fields[7U]);
  return observation;
}

/**
 * @brief Encodes one typed logical digest without crossing digest domains.
 * @param digest Exact content digest.
 * @return Algorithm plus lowercase digest bytes.
 * @throws std::invalid_argument for an unknown algorithm representation.
 * @throws std::bad_alloc when framing allocates.
 */
std::string encode_content_digest(const ContentDigest& digest) {
  if (digest.algorithm != CanonicalDigestAlgorithm::Sha256CanonicalV1) {
    throw std::invalid_argument("M1 content digest algorithm is unknown.");
  }
  B1Sha256Digest bytes;
  bytes.bytes = digest.bytes;
  return encode_b1_fixed_record(
      {std::to_string(static_cast<std::uint32_t>(digest.algorithm)),
       b1_digest_hex(bytes)});
}

/**
 * @brief Parses one typed logical digest without treating it as raw B1 SHA.
 * @param record Algorithm plus lowercase digest bytes.
 * @return Exact typed content digest.
 * @throws std::invalid_argument for framing, algorithm, or hex drift.
 */
ContentDigest parse_content_digest(std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 2U);
  if (parse_b1_canonical_uint64(fields[0U]) !=
      static_cast<std::uint32_t>(CanonicalDigestAlgorithm::Sha256CanonicalV1)) {
    throw std::invalid_argument("M1 content digest algorithm is unknown.");
  }
  const B1Sha256Digest bytes = parse_b1_digest(fields[1U]);
  ContentDigest digest;
  digest.algorithm = CanonicalDigestAlgorithm::Sha256CanonicalV1;
  digest.bytes = bytes.bytes;
  return digest;
}

/**
 * @brief Encodes one complete typed content-digest outcome.
 * @param result Availability state, optional digest, and diagnostic.
 * @return Three-component canonical record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_content_digest_result(const ContentDigestResult& result) {
  const std::uint32_t state = static_cast<std::uint32_t>(result.state);
  if (state > 5U) {
    throw std::invalid_argument("M1 content digest state is unknown.");
  }
  return encode_b1_fixed_record({std::to_string(state),
                                 result.digest.has_value()
                                     ? encode_content_digest(*result.digest)
                                     : "not-applicable",
                                 encode_source_bytes(result.diagnostic)});
}

/**
 * @brief Parses one complete typed content-digest outcome.
 * @param record Three-component canonical record.
 * @return Exact availability state, optional digest, and diagnostic.
 * @throws std::invalid_argument for framing, enum, or digest drift.
 */
ContentDigestResult parse_content_digest_result(std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 3U);
  ContentDigestResult result;
  result.state = parse_zero_based_enum<ContentDigestState>(
      fields[0U], 5U, "M1 content digest state is unknown.");
  if (fields[1U] != "not-applicable") {
    result.digest = parse_content_digest(fields[1U]);
  }
  result.diagnostic = parse_source_bytes(fields[2U]);
  return result;
}

/**
 * @brief Encodes one complete public operation status.
 * @param status Exact status value.
 * @return Five-component canonical record.
 * @throws std::invalid_argument for an unknown error domain.
 * @throws std::bad_alloc when framing allocates.
 */
std::string encode_operation_status(const OperationStatus& status) {
  const std::uint32_t domain = static_cast<std::uint32_t>(status.domain);
  if (domain > 4U) {
    throw std::invalid_argument("M1 operation status domain is unknown.");
  }
  return encode_b1_fixed_record(
      {boolean_text(status.ok), std::to_string(domain),
       std::to_string(status.code), encode_source_bytes(status.name),
       encode_source_bytes(status.message)});
}

/**
 * @brief Parses one complete public operation status.
 * @param record Five-component canonical record.
 * @return Exact status value.
 * @throws std::invalid_argument for framing, enum, or integer drift.
 */
OperationStatus parse_operation_status(std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 5U);
  return OperationStatus{
      parse_boolean(fields[0U]),
      parse_zero_based_enum<OperationErrorDomain>(
          fields[1U], 4U, "M1 operation status domain is unknown."),
      parse_int32(fields[2U]), parse_source_bytes(fields[3U]),
      parse_source_bytes(fields[4U])};
}

/**
 * @brief Encodes one optional accepted-boundary coordinate.
 * @param coordinate Optional exact pre-call coordinate.
 * @return `not-applicable` or a two-component record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_accepted_coordinate(
    const std::optional<I1AcceptedCoordinate>& coordinate) {
  return coordinate.has_value()
             ? encode_b1_fixed_record(
                   {encode_time(coordinate->admission_time()),
                    std::to_string(coordinate->event_sequence())})
             : "not-applicable";
}

/**
 * @brief Parses one optional accepted-boundary coordinate.
 * @param record `not-applicable` or a two-component record.
 * @return Exact optional coordinate.
 * @throws std::invalid_argument for framing, time, or zero sequence.
 */
std::optional<I1AcceptedCoordinate> parse_accepted_coordinate(
    std::string_view record) {
  if (record == "not-applicable") {
    return std::nullopt;
  }
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 2U);
  return I1AcceptedCoordinate(parse_time(fields[0U]),
                              parse_b1_canonical_uint64(fields[1U]));
}

/**
 * @brief Encodes one immutable Compute QoS record.
 * @param qos Exact scheduling inputs.
 * @return Four-component canonical record.
 * @throws std::invalid_argument for an unknown class.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_compute_qos(const compute::ComputeRunQos& qos) {
  const std::uint32_t service_class =
      static_cast<std::uint32_t>(qos.service_class);
  if (service_class > 1U) {
    throw std::invalid_argument("M1 source QoS class is unknown.");
  }
  return encode_b1_fixed_record(
      {std::to_string(service_class),
       qos.deadline.has_value() ? encode_time(*qos.deadline) : "not-applicable",
       std::to_string(qos.weight),
       qos.maximum_parallelism.has_value()
           ? std::to_string(*qos.maximum_parallelism)
           : "not-applicable"});
}

/**
 * @brief Parses one immutable Compute QoS record.
 * @param record Four-component canonical record.
 * @return Exact scheduling inputs.
 * @throws std::invalid_argument for framing, enum, or numeric drift.
 */
compute::ComputeRunQos parse_compute_qos(std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 4U);
  compute::ComputeRunQos qos;
  qos.service_class = parse_zero_based_enum<compute::ComputeRunQosClass>(
      fields[0U], 1U, "M1 source QoS class is unknown.");
  if (fields[1U] != "not-applicable") {
    qos.deadline = parse_time(fields[1U]);
  }
  qos.weight = parse_uint32(fields[2U]);
  if (fields[3U] != "not-applicable") {
    qos.maximum_parallelism = parse_uint32(fields[3U]);
  }
  return qos;
}

/**
 * @brief Encodes one complete Issue #93 resource/lifecycle snapshot.
 * @param snapshot Authority-free Host/device/lifecycle cut.
 * @return Three-component canonical record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_i1_execution_snapshot(const I1ExecutionSnapshot& snapshot) {
  std::vector<std::string> devices;
  devices.reserve(snapshot.device_resources.size());
  for (const ResourceLedger::DeviceSnapshot& device :
       snapshot.device_resources) {
    devices.push_back(encode_b1_fixed_record(
        {std::to_string(static_cast<std::uint32_t>(device.device.backend())),
         std::to_string(device.device.ordinal()),
         std::to_string(device.limits.device_memory_bytes),
         std::to_string(device.limits.device_scratch_bytes),
         std::to_string(device.reserved.device_memory_bytes),
         std::to_string(device.reserved.device_scratch_bytes),
         std::to_string(device.available.device_memory_bytes),
         std::to_string(device.available.device_scratch_bytes),
         std::to_string(device.high_water.device_memory_bytes),
         std::to_string(device.high_water.device_scratch_bytes)}));
  }
  return encode_b1_fixed_record(
      {encode_b1_fixed_record(
           {encode_resource_vector(snapshot.host_resources.limits),
            encode_resource_vector(snapshot.host_resources.reserved),
            encode_resource_vector(snapshot.host_resources.high_water)}),
       encode_record_list(devices), encode_lifecycle_page(snapshot.lifecycle)});
}

/**
 * @brief Parses one complete Issue #93 resource/lifecycle snapshot.
 * @param record Three-component canonical record.
 * @return Exact authority-free Host/device/lifecycle cut.
 * @throws std::invalid_argument for framing, enum, or numeric drift.
 */
I1ExecutionSnapshot parse_i1_execution_snapshot(std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 3U);
  const std::vector<std::string> host = parse_b1_fixed_record(fields[0U], 3U);
  I1ExecutionSnapshot snapshot;
  snapshot.host_resources.limits = parse_resource_vector(host[0U]);
  snapshot.host_resources.reserved = parse_resource_vector(host[1U]);
  snapshot.host_resources.high_water = parse_resource_vector(host[2U]);
  for (const std::string& device_record : parse_b1_framed_list(fields[1U])) {
    const std::vector<std::string> device =
        parse_b1_fixed_record(device_record, 10U);
    ResourceLedger::DeviceSnapshot value;
    value.device =
        DeviceId(parse_zero_based_enum<DeviceBackend>(
                     device[0U], 4U, "M1 I1 source device backend is unknown."),
                 parse_uint32(device[1U]));
    value.limits = DeviceResourceVector{parse_b1_canonical_uint64(device[2U]),
                                        parse_b1_canonical_uint64(device[3U])};
    value.reserved =
        DeviceResourceVector{parse_b1_canonical_uint64(device[4U]),
                             parse_b1_canonical_uint64(device[5U])};
    value.available =
        DeviceResourceVector{parse_b1_canonical_uint64(device[6U]),
                             parse_b1_canonical_uint64(device[7U])};
    value.high_water =
        DeviceResourceVector{parse_b1_canonical_uint64(device[8U]),
                             parse_b1_canonical_uint64(device[9U])};
    snapshot.device_resources.push_back(std::move(value));
  }
  snapshot.lifecycle = parse_lifecycle_page(fields[2U]);
  return snapshot;
}

/**
 * @brief Encodes one complete Issue #93 edit source record.
 * @param edit Exact raw edit evidence.
 * @return Fifteen-component canonical record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_i1_edit(const I1EditEvidence& edit) {
  std::uint64_t coefficient_bits = 0U;
  static_assert(sizeof(coefficient_bits) == sizeof(edit.coefficient));
  std::memcpy(&coefficient_bits, &edit.coefficient, sizeof(coefficient_bits));
  const std::string host_return =
      edit.host_return.has_value()
          ? encode_b1_fixed_record(
                {encode_time(edit.host_return->return_time),
                 encode_operation_status(edit.host_return->status),
                 boolean_text(edit.host_return->future_valid)})
          : "not-applicable";
  return encode_b1_fixed_record(
      {std::to_string(edit.edit_index), std::to_string(coefficient_bits),
       std::to_string(edit.region.x), std::to_string(edit.region.y),
       std::to_string(edit.region.width), std::to_string(edit.region.height),
       encode_time(edit.nominal_time), boolean_text(edit.admission_attempted),
       encode_time(edit.admission_sample),
       boolean_text(edit.admission_window_valid),
       edit.reserved_event_sequence.has_value()
           ? std::to_string(*edit.reserved_event_sequence)
           : "not-applicable",
       edit.deadline.has_value() ? encode_time(*edit.deadline)
                                 : "not-applicable",
       host_return, encode_accepted_coordinate(edit.accepted_coordinate),
       edit.settlement_status.has_value()
           ? encode_operation_status(*edit.settlement_status)
           : "not-applicable"});
}

/**
 * @brief Parses one complete Issue #93 edit source record.
 * @param record Fifteen-component canonical record.
 * @return Exact raw edit evidence.
 * @throws std::invalid_argument for framing or typed-value drift.
 */
I1EditEvidence parse_i1_edit(std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 15U);
  I1EditEvidence edit;
  edit.edit_index = parse_size(fields[0U]);
  const std::uint64_t coefficient_bits = parse_b1_canonical_uint64(fields[1U]);
  static_assert(sizeof(coefficient_bits) == sizeof(edit.coefficient));
  std::memcpy(&edit.coefficient, &coefficient_bits, sizeof(coefficient_bits));
  edit.region = PixelRect{parse_int32(fields[2U]), parse_int32(fields[3U]),
                          parse_int32(fields[4U]), parse_int32(fields[5U])};
  edit.nominal_time = parse_time(fields[6U]);
  edit.admission_attempted = parse_boolean(fields[7U]);
  edit.admission_sample = parse_time(fields[8U]);
  edit.admission_window_valid = parse_boolean(fields[9U]);
  if (fields[10U] != "not-applicable") {
    edit.reserved_event_sequence = parse_b1_canonical_uint64(fields[10U]);
  }
  if (fields[11U] != "not-applicable") {
    edit.deadline = parse_time(fields[11U]);
  }
  if (fields[12U] != "not-applicable") {
    const std::vector<std::string> host =
        parse_b1_fixed_record(fields[12U], 3U);
    edit.host_return = I1HostReturnEvidence{parse_time(host[0U]),
                                            parse_operation_status(host[1U]),
                                            parse_boolean(host[2U])};
  }
  edit.accepted_coordinate = parse_accepted_coordinate(fields[13U]);
  if (fields[14U] != "not-applicable") {
    edit.settlement_status = parse_operation_status(fields[14U]);
  }
  return edit;
}

/**
 * @brief Encodes every raw Issue #93 product observation.
 * @param snapshot Complete bounded observation snapshot.
 * @return Nine-component canonical record of typed ordered lists and overflow.
 * @throws std::invalid_argument while a visible source still retains Value.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_i1_observations(
    const I1EpisodeObservationSnapshot& snapshot) {
  std::vector<std::string> current;
  for (const I1ObservedCurrentGeneration& event :
       snapshot.current_generations) {
    current.push_back(encode_b1_fixed_record(
        {std::to_string(event.edit_index), std::to_string(event.generation),
         encode_time(event.observed_at), std::to_string(event.causal_sequence),
         encode_accepted_coordinate(event.accepted_coordinate)}));
  }
  std::vector<std::string> starts;
  for (const I1ObservedServiceStart& event : snapshot.service_starts) {
    const std::uint32_t quality = static_cast<std::uint32_t>(event.quality);
    if (quality > 1U) {
      throw std::invalid_argument("M1 I1 source quality is unknown.");
    }
    starts.push_back(encode_b1_fixed_record(
        {std::to_string(event.edit_index), std::to_string(event.run_id),
         std::to_string(event.generation), std::to_string(event.local_task_id),
         std::to_string(quality), encode_compute_qos(event.qos),
         std::to_string(event.service_charge), encode_time(event.observed_at),
         std::to_string(event.causal_sequence)}));
  }
  std::vector<std::string> cancellations;
  for (const I1ObservedCancellation& event : snapshot.cancellations) {
    const std::uint32_t reason = static_cast<std::uint32_t>(event.reason);
    if (reason > 4U) {
      throw std::invalid_argument(
          "M1 I1 source cancellation reason is unknown.");
    }
    cancellations.push_back(encode_b1_fixed_record(
        {std::to_string(event.edit_index), std::to_string(event.run_id),
         std::to_string(event.generation), std::to_string(reason),
         encode_time(event.observed_at),
         std::to_string(event.causal_sequence)}));
  }
  std::vector<std::string> terminals;
  for (const I1ObservedTerminal& event : snapshot.terminals) {
    const std::uint32_t kind = static_cast<std::uint32_t>(event.kind);
    if (kind > 2U) {
      throw std::invalid_argument("M1 I1 source terminal kind is unknown.");
    }
    terminals.push_back(encode_b1_fixed_record(
        {std::to_string(event.edit_index), std::to_string(event.run_id),
         std::to_string(event.generation), std::to_string(kind),
         encode_time(event.observed_at),
         std::to_string(event.causal_sequence)}));
  }
  std::vector<std::string> visible;
  for (const I1ObservedVisibleOutput& event : snapshot.visible_outputs) {
    if (event.output.valid()) {
      throw std::invalid_argument(
          "M1 canonical I1 source still retains a live Value handle.");
    }
    visible.push_back(encode_b1_fixed_record(
        {std::to_string(event.edit_index), std::to_string(event.run_id),
         std::to_string(event.generation), encode_time(event.observed_at),
         std::to_string(event.causal_sequence), "false",
         boolean_text(event.value_valid_at_capture),
         event.content_digest.has_value()
             ? encode_content_digest_result(*event.content_digest)
             : "not-applicable"}));
  }
  const auto encode_lifecycle = [](const auto& events) {
    std::vector<std::string> records;
    records.reserve(events.size());
    for (const I1ObservedRunLifecycleTransition& event : events) {
      records.push_back(encode_b1_fixed_record(
          {std::to_string(event.edit_index), std::to_string(event.run_id),
           std::to_string(event.generation), encode_time(event.observed_at),
           std::to_string(event.causal_sequence)}));
    }
    return encode_record_list(records);
  };
  std::vector<std::string> settlements;
  for (const I1ObservedHostSettlement& event : snapshot.host_settlements) {
    settlements.push_back(encode_b1_fixed_record(
        {std::to_string(event.edit_index), encode_time(event.observed_at),
         std::to_string(event.causal_sequence)}));
  }
  return encode_b1_fixed_record(
      {encode_record_list(current), encode_record_list(starts),
       encode_record_list(cancellations), encode_record_list(terminals),
       encode_record_list(visible), encode_lifecycle(snapshot.run_quiescences),
       encode_lifecycle(snapshot.resource_settlements),
       encode_record_list(settlements), boolean_text(snapshot.overflowed)});
}

/**
 * @brief Parses every raw Issue #93 product observation.
 * @param record Nine-component canonical snapshot record.
 * @return Complete bounded observation snapshot with no live Value handles.
 * @throws std::invalid_argument for framing, enum, or forbidden Value state.
 */
I1EpisodeObservationSnapshot parse_i1_observations(std::string_view record) {
  const std::vector<std::string> lists = parse_b1_fixed_record(record, 9U);
  I1EpisodeObservationSnapshot snapshot;
  for (const std::string& item : parse_b1_framed_list(lists[0U])) {
    const std::vector<std::string> fields = parse_b1_fixed_record(item, 5U);
    snapshot.current_generations.push_back(I1ObservedCurrentGeneration{
        parse_size(fields[0U]), parse_b1_canonical_uint64(fields[1U]),
        parse_time(fields[2U]), parse_b1_canonical_uint64(fields[3U]),
        parse_accepted_coordinate(fields[4U])});
  }
  for (const std::string& item : parse_b1_framed_list(lists[1U])) {
    const std::vector<std::string> fields = parse_b1_fixed_record(item, 9U);
    snapshot.service_starts.push_back(I1ObservedServiceStart{
        parse_size(fields[0U]), parse_b1_canonical_uint64(fields[1U]),
        parse_b1_canonical_uint64(fields[2U]),
        parse_b1_canonical_uint64(fields[3U]),
        parse_zero_based_enum<compute::ComputeRunQuality>(
            fields[4U], 1U, "M1 I1 source quality is unknown."),
        parse_compute_qos(fields[5U]), parse_b1_canonical_uint64(fields[6U]),
        parse_time(fields[7U]), parse_b1_canonical_uint64(fields[8U])});
  }
  for (const std::string& item : parse_b1_framed_list(lists[2U])) {
    const std::vector<std::string> fields = parse_b1_fixed_record(item, 6U);
    snapshot.cancellations.push_back(I1ObservedCancellation{
        parse_size(fields[0U]), parse_b1_canonical_uint64(fields[1U]),
        parse_b1_canonical_uint64(fields[2U]),
        parse_zero_based_enum<compute::ComputeRunCancellationReason>(
            fields[3U], 4U, "M1 I1 source cancellation reason is unknown."),
        parse_time(fields[4U]), parse_b1_canonical_uint64(fields[5U])});
  }
  for (const std::string& item : parse_b1_framed_list(lists[3U])) {
    const std::vector<std::string> fields = parse_b1_fixed_record(item, 6U);
    snapshot.terminals.push_back(I1ObservedTerminal{
        parse_size(fields[0U]), parse_b1_canonical_uint64(fields[1U]),
        parse_b1_canonical_uint64(fields[2U]),
        parse_zero_based_enum<compute::ComputeRunTerminalKind>(
            fields[3U], 2U, "M1 I1 source terminal kind is unknown."),
        parse_time(fields[4U]), parse_b1_canonical_uint64(fields[5U])});
  }
  for (const std::string& item : parse_b1_framed_list(lists[4U])) {
    const std::vector<std::string> fields = parse_b1_fixed_record(item, 8U);
    if (parse_boolean(fields[5U])) {
      throw std::invalid_argument(
          "M1 canonical I1 source cannot rehydrate a Value handle.");
    }
    I1ObservedVisibleOutput event;
    event.edit_index = parse_size(fields[0U]);
    event.run_id = parse_b1_canonical_uint64(fields[1U]);
    event.generation = parse_b1_canonical_uint64(fields[2U]);
    event.observed_at = parse_time(fields[3U]);
    event.causal_sequence = parse_b1_canonical_uint64(fields[4U]);
    event.value_valid_at_capture = parse_boolean(fields[6U]);
    if (fields[7U] != "not-applicable") {
      event.content_digest = parse_content_digest_result(fields[7U]);
    }
    snapshot.visible_outputs.push_back(std::move(event));
  }
  const auto parse_lifecycle = [](std::string_view payload, auto* output) {
    for (const std::string& item : parse_b1_framed_list(payload)) {
      const std::vector<std::string> fields = parse_b1_fixed_record(item, 5U);
      output->push_back(I1ObservedRunLifecycleTransition{
          parse_size(fields[0U]), parse_b1_canonical_uint64(fields[1U]),
          parse_b1_canonical_uint64(fields[2U]), parse_time(fields[3U]),
          parse_b1_canonical_uint64(fields[4U])});
    }
  };
  parse_lifecycle(lists[5U], &snapshot.run_quiescences);
  parse_lifecycle(lists[6U], &snapshot.resource_settlements);
  for (const std::string& item : parse_b1_framed_list(lists[7U])) {
    const std::vector<std::string> fields = parse_b1_fixed_record(item, 3U);
    snapshot.host_settlements.push_back(
        I1ObservedHostSettlement{parse_size(fields[0U]), parse_time(fields[1U]),
                                 parse_b1_canonical_uint64(fields[2U])});
  }
  snapshot.overflowed = parse_boolean(lists[8U]);
  return snapshot;
}

/**
 * @brief Encodes one complete reversible Issue #93 episode input.
 * @param source M1 source identity plus raw episode evidence.
 * @return Five-component source binding containing a fourteen-field episode.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_i1_source(const M1InteractiveSourceEvidence& source) {
  std::vector<std::string> edits;
  edits.reserve(source.episode.edits.size());
  for (const I1EditEvidence& edit : source.episode.edits) {
    edits.push_back(encode_i1_edit(edit));
  }
  const I1EpisodeEvidenceInput& episode = source.episode;
  const std::string episode_record = encode_b1_fixed_record(
      {std::to_string(episode.replicate_ordinal), std::to_string(episode.slot),
       encode_time(episode.grid_origin), encode_time(episode.episode_origin),
       encode_time(episode.terminal_boundary),
       encode_time(episode.measurement_start),
       encode_time(episode.measurement_end),
       encode_b1_fixed_record(
           {encode_time(episode.observation_cut.captured_at),
            std::to_string(episode.observation_cut.causal_sequence)}),
       encode_record_list(edits), encode_i1_observations(episode.observations),
       encode_i1_execution_snapshot(episode.baseline),
       encode_i1_execution_snapshot(episode.final_snapshot),
       encode_time(episode.final_snapshot_sample),
       episode.expected_final_digest.has_value()
           ? encode_content_digest(*episode.expected_final_digest)
           : "not-applicable"});
  return encode_b1_fixed_record(
      {phase_text(source.phase), std::to_string(source.phase_ordinal),
       encode_time(source.origin.timestamp),
       std::to_string(source.origin.event_sequence), episode_record});
}

/**
 * @brief Parses one complete reversible Issue #93 source binding.
 * @param record Five-component source record.
 * @return Exact source identity and raw episode evidence.
 * @throws std::invalid_argument for framing, enum, cardinality, or typed drift.
 */
M1InteractiveSourceEvidence parse_i1_source(std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 5U);
  M1InteractiveSourceEvidence source;
  source.phase = parse_phase(fields[0U]);
  source.phase_ordinal = parse_size(fields[1U]);
  source.origin = M1EventCoordinate{parse_time(fields[2U]),
                                    parse_b1_canonical_uint64(fields[3U])};
  const std::vector<std::string> episode =
      parse_b1_fixed_record(fields[4U], 14U);
  source.episode.replicate_ordinal = parse_b1_canonical_uint64(episode[0U]);
  source.episode.slot = parse_size(episode[1U]);
  source.episode.grid_origin = parse_time(episode[2U]);
  source.episode.episode_origin = parse_time(episode[3U]);
  source.episode.terminal_boundary = parse_time(episode[4U]);
  source.episode.measurement_start = parse_time(episode[5U]);
  source.episode.measurement_end = parse_time(episode[6U]);
  const std::vector<std::string> cut = parse_b1_fixed_record(episode[7U], 2U);
  source.episode.observation_cut = I1ObservationHistoryCut{
      parse_time(cut[0U]), parse_b1_canonical_uint64(cut[1U])};
  const std::vector<std::string> edits = parse_b1_framed_list(episode[8U]);
  if (edits.size() != kI1EditCount) {
    throw std::invalid_argument(
        "M1 I1 source does not retain exactly twelve edits.");
  }
  for (std::size_t index = 0U; index < edits.size(); ++index) {
    source.episode.edits[index] = parse_i1_edit(edits[index]);
  }
  source.episode.observations = parse_i1_observations(episode[9U]);
  source.episode.baseline = parse_i1_execution_snapshot(episode[10U]);
  source.episode.final_snapshot = parse_i1_execution_snapshot(episode[11U]);
  source.episode.final_snapshot_sample = parse_time(episode[12U]);
  if (episode[13U] != "not-applicable") {
    source.episode.expected_final_digest = parse_content_digest(episode[13U]);
  }
  return source;
}

/**
 * @brief Encodes one product observation coordinate.
 * @param coordinate Exact steady-clock/causal pair.
 * @return Two-component canonical record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_observation_coordinate(
    const compute::ComputeRunObservationCoordinate& coordinate) {
  return encode_b1_fixed_record({encode_time(coordinate.observed_at),
                                 std::to_string(coordinate.causal_sequence)});
}

/**
 * @brief Parses one product observation coordinate.
 * @param record Two-component canonical record.
 * @return Exact steady-clock/causal pair.
 * @throws std::invalid_argument for framing or numeric drift.
 */
compute::ComputeRunObservationCoordinate parse_observation_coordinate(
    std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 2U);
  return compute::ComputeRunObservationCoordinate{
      parse_time(fields[0U]), parse_b1_canonical_uint64(fields[1U])};
}

/**
 * @brief Encodes one complete B1 semantic resource vector.
 * @param resources Exact task-ready declaration.
 * @return Eight-component canonical record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_semantic_resources(
    const B1SemanticResourceVector& resources) {
  return encode_b1_fixed_record(
      {std::to_string(resources.work_units),
       std::to_string(resources.ready_entries),
       std::to_string(resources.ready_bytes),
       std::to_string(resources.cpu_slots),
       std::to_string(resources.host_retained_bytes),
       std::to_string(resources.host_scratch_bytes),
       std::to_string(resources.device_memory_bytes),
       std::to_string(resources.device_scratch_bytes)});
}

/**
 * @brief Parses one complete B1 semantic resource vector.
 * @param record Eight-component canonical record.
 * @return Exact task-ready declaration.
 * @throws std::invalid_argument for framing or numeric drift.
 */
B1SemanticResourceVector parse_semantic_resources(std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 8U);
  return B1SemanticResourceVector{parse_b1_canonical_uint64(fields[0U]),
                                  parse_b1_canonical_uint64(fields[1U]),
                                  parse_b1_canonical_uint64(fields[2U]),
                                  parse_b1_canonical_uint64(fields[3U]),
                                  parse_b1_canonical_uint64(fields[4U]),
                                  parse_b1_canonical_uint64(fields[5U]),
                                  parse_b1_canonical_uint64(fields[6U]),
                                  parse_b1_canonical_uint64(fields[7U])};
}

/**
 * @brief Encodes one optional raw B1 Run transition.
 * @param transition Optional run identity and coordinate.
 * @return `not-applicable` or a two-component record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_b1_transition(
    const std::optional<B1ObservedRunTransition>& transition) {
  return transition.has_value()
             ? encode_b1_fixed_record(
                   {std::to_string(transition->run_id),
                    encode_observation_coordinate(transition->coordinate)})
             : "not-applicable";
}

/**
 * @brief Parses one optional raw B1 Run transition.
 * @param record `not-applicable` or a two-component record.
 * @return Exact optional run identity and coordinate.
 * @throws std::invalid_argument for framing or numeric drift.
 */
std::optional<B1ObservedRunTransition> parse_b1_transition(
    std::string_view record) {
  if (record == "not-applicable") {
    return std::nullopt;
  }
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 2U);
  return B1ObservedRunTransition{parse_b1_canonical_uint64(fields[0U]),
                                 parse_observation_coordinate(fields[1U])};
}

/**
 * @brief Encodes one complete raw B1 physical observation snapshot.
 * @param trace Exact current/ready/start/terminal/cancel/lifecycle evidence.
 * @return Thirteen-component canonical record.
 * @throws std::invalid_argument for unknown enum or dependency overflow.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_b1_physical_trace(const B1RunObservationSnapshot& trace) {
  std::vector<std::string> generations;
  for (const B1ObservedCurrentGeneration& event : trace.current_generations) {
    generations.push_back(encode_b1_fixed_record(
        {std::to_string(event.generation),
         encode_observation_coordinate(event.coordinate)}));
  }
  std::vector<std::string> readies;
  for (const B1ObservedTaskReady& event : trace.task_readies) {
    if (event.dependency_count > event.dependencies.size()) {
      throw std::invalid_argument(
          "M1 B1 source dependency count exceeds fixed capacity.");
    }
    std::vector<std::string> dependencies;
    for (std::size_t index = 0U; index < event.dependency_count; ++index) {
      dependencies.push_back(std::to_string(event.dependencies[index]));
    }
    readies.push_back(encode_b1_fixed_record(
        {std::to_string(event.run_id), std::to_string(event.local_task_id),
         encode_record_list(dependencies),
         std::to_string(event.declared_ready_bytes),
         encode_semantic_resources(event.resources),
         encode_observation_coordinate(event.coordinate)}));
  }
  std::vector<std::string> starts;
  for (const B1ObservedServiceStart& event : trace.service_starts) {
    starts.push_back(encode_b1_fixed_record(
        {std::to_string(event.run_id), std::to_string(event.local_task_id),
         std::to_string(event.service_charge), encode_compute_qos(event.qos),
         encode_observation_coordinate(event.coordinate)}));
  }
  std::vector<std::string> terminals;
  for (const B1ObservedTaskTerminal& event : trace.task_terminals) {
    const std::uint32_t kind = static_cast<std::uint32_t>(event.kind);
    if (kind > 2U) {
      throw std::invalid_argument("M1 B1 task terminal kind is unknown.");
    }
    terminals.push_back(encode_b1_fixed_record(
        {std::to_string(event.run_id), std::to_string(event.local_task_id),
         std::to_string(kind),
         encode_observation_coordinate(event.coordinate)}));
  }
  std::vector<std::string> cancellations;
  for (const B1ObservedCancellation& event : trace.cancellations) {
    const std::uint32_t reason = static_cast<std::uint32_t>(event.reason);
    if (reason > 4U) {
      throw std::invalid_argument("M1 B1 cancellation reason is unknown.");
    }
    cancellations.push_back(encode_b1_fixed_record(
        {std::to_string(event.run_id), std::to_string(reason),
         encode_observation_coordinate(event.coordinate)}));
  }
  std::string terminal_kind = "not-applicable";
  if (trace.terminal_kind.has_value()) {
    const std::uint32_t kind = static_cast<std::uint32_t>(*trace.terminal_kind);
    if (kind > 2U) {
      throw std::invalid_argument("M1 B1 Run terminal kind is unknown.");
    }
    terminal_kind = std::to_string(kind);
  }
  return encode_b1_fixed_record(
      {encode_b1_job_instance(trace.job), boolean_text(trace.overflowed),
       encode_record_list(generations), encode_record_list(readies),
       encode_record_list(starts), encode_record_list(terminals),
       encode_record_list(cancellations), terminal_kind,
       encode_b1_transition(trace.terminal),
       encode_b1_transition(trace.visible),
       encode_b1_transition(trace.quiescent),
       encode_b1_transition(trace.resource_settled),
       encode_content_digest_result(trace.visible_content_digest)});
}

/**
 * @brief Parses one complete raw B1 physical observation snapshot.
 * @param record Thirteen-component canonical record.
 * @return Exact current/ready/start/terminal/cancel/lifecycle evidence.
 * @throws std::invalid_argument for framing, enum, or capacity drift.
 */
B1RunObservationSnapshot parse_b1_physical_trace(std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 13U);
  B1RunObservationSnapshot trace;
  trace.job = parse_job(fields[0U]);
  trace.overflowed = parse_boolean(fields[1U]);
  for (const std::string& item : parse_b1_framed_list(fields[2U])) {
    const std::vector<std::string> value = parse_b1_fixed_record(item, 2U);
    trace.current_generations.push_back(
        B1ObservedCurrentGeneration{parse_b1_canonical_uint64(value[0U]),
                                    parse_observation_coordinate(value[1U])});
  }
  for (const std::string& item : parse_b1_framed_list(fields[3U])) {
    const std::vector<std::string> value = parse_b1_fixed_record(item, 6U);
    B1ObservedTaskReady ready;
    ready.run_id = parse_b1_canonical_uint64(value[0U]);
    ready.local_task_id = parse_b1_canonical_uint64(value[1U]);
    const std::vector<std::string> dependencies =
        parse_b1_framed_list(value[2U]);
    if (dependencies.size() > ready.dependencies.size()) {
      throw std::invalid_argument(
          "M1 B1 source dependency count exceeds fixed capacity.");
    }
    ready.dependency_count = dependencies.size();
    for (std::size_t index = 0U; index < dependencies.size(); ++index) {
      ready.dependencies[index] =
          parse_b1_canonical_uint64(dependencies[index]);
    }
    ready.declared_ready_bytes = parse_b1_canonical_uint64(value[3U]);
    ready.resources = parse_semantic_resources(value[4U]);
    ready.coordinate = parse_observation_coordinate(value[5U]);
    trace.task_readies.push_back(std::move(ready));
  }
  for (const std::string& item : parse_b1_framed_list(fields[4U])) {
    const std::vector<std::string> value = parse_b1_fixed_record(item, 5U);
    trace.service_starts.push_back(B1ObservedServiceStart{
        parse_b1_canonical_uint64(value[0U]),
        parse_b1_canonical_uint64(value[1U]),
        parse_b1_canonical_uint64(value[2U]), parse_compute_qos(value[3U]),
        parse_observation_coordinate(value[4U])});
  }
  for (const std::string& item : parse_b1_framed_list(fields[5U])) {
    const std::vector<std::string> value = parse_b1_fixed_record(item, 4U);
    trace.task_terminals.push_back(B1ObservedTaskTerminal{
        parse_b1_canonical_uint64(value[0U]),
        parse_b1_canonical_uint64(value[1U]),
        parse_zero_based_enum<compute::ComputeRunTaskTerminalKind>(
            value[2U], 2U, "M1 B1 task terminal kind is unknown."),
        parse_observation_coordinate(value[3U])});
  }
  for (const std::string& item : parse_b1_framed_list(fields[6U])) {
    const std::vector<std::string> value = parse_b1_fixed_record(item, 3U);
    trace.cancellations.push_back(B1ObservedCancellation{
        parse_b1_canonical_uint64(value[0U]),
        parse_zero_based_enum<compute::ComputeRunCancellationReason>(
            value[1U], 4U, "M1 B1 cancellation reason is unknown."),
        parse_observation_coordinate(value[2U])});
  }
  if (fields[7U] != "not-applicable") {
    trace.terminal_kind =
        parse_zero_based_enum<compute::ComputeRunTerminalKind>(
            fields[7U], 2U, "M1 B1 Run terminal kind is unknown.");
  }
  trace.terminal = parse_b1_transition(fields[8U]);
  trace.visible = parse_b1_transition(fields[9U]);
  trace.quiescent = parse_b1_transition(fields[10U]);
  trace.resource_settled = parse_b1_transition(fields[11U]);
  trace.visible_content_digest = parse_content_digest_result(fields[12U]);
  return trace;
}

/**
 * @brief Encodes one authority-free observed B1 receipt.
 * @param receipt Complete copied receipt fields.
 * @return Sixteen-component canonical record.
 * @throws std::invalid_argument for unknown durability.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_b1_receipt(const M1BatchReceiptEvidence& receipt) {
  const std::uint32_t requested =
      static_cast<std::uint32_t>(receipt.requested_durability);
  const std::uint32_t achieved =
      static_cast<std::uint32_t>(receipt.achieved_durability);
  if (requested > 1U || achieved > 1U) {
    throw std::invalid_argument("M1 B1 durability is unknown.");
  }
  return encode_b1_fixed_record(
      {encode_source_bytes(receipt.commit_id),
       encode_source_bytes(receipt.resolved_root.generic_string()),
       encode_source_bytes(receipt.rooted_slot.generic_string()),
       encode_b1_job_instance(receipt.job),
       encode_source_bytes(receipt.logical_descriptor),
       encode_content_digest(receipt.logical_content_digest),
       std::to_string(receipt.committed_generation),
       encode_source_bytes(receipt.payload_name),
       encode_source_bytes(receipt.manifest_name),
       std::to_string(receipt.payload_length),
       std::to_string(receipt.manifest_length),
       b1_digest_hex(receipt.payload_digest),
       b1_digest_hex(receipt.manifest_digest), std::to_string(requested),
       std::to_string(achieved),
       encode_source_bytes(receipt.published_manifest_identity)});
}

/**
 * @brief Parses one authority-free observed B1 receipt.
 * @param record Sixteen-component canonical record.
 * @return Complete copied receipt fields without minting a capability.
 * @throws std::invalid_argument for framing, enum, digest, or identity drift.
 */
M1BatchReceiptEvidence parse_b1_receipt(std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 16U);
  return M1BatchReceiptEvidence{
      parse_source_bytes(fields[0U]),
      std::filesystem::path(parse_source_bytes(fields[1U])),
      std::filesystem::path(parse_source_bytes(fields[2U])),
      parse_job(fields[3U]),
      parse_source_bytes(fields[4U]),
      parse_content_digest(fields[5U]),
      parse_b1_canonical_uint64(fields[6U]),
      parse_source_bytes(fields[7U]),
      parse_source_bytes(fields[8U]),
      parse_b1_canonical_uint64(fields[9U]),
      parse_b1_canonical_uint64(fields[10U]),
      parse_b1_digest(fields[11U]),
      parse_b1_digest(fields[12U]),
      parse_zero_based_enum<B1OutputDurability>(
          fields[13U], 1U, "M1 B1 requested durability is unknown."),
      parse_zero_based_enum<B1OutputDurability>(
          fields[14U], 1U, "M1 B1 achieved durability is unknown."),
      parse_source_bytes(fields[15U])};
}

/**
 * @brief Encodes one complete authority-free B1 M1 source.
 * @param source Exact offer/raw-trace/output/golden/I/O evidence.
 * @return Thirteen-component canonical source record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_b1_source(const M1BatchSourceEvidence& source) {
  const std::uint32_t status = static_cast<std::uint32_t>(source.output_status);
  if (status > 8U) {
    throw std::invalid_argument("M1 B1 output status is unknown.");
  }
  std::vector<std::string> io;
  io.reserve(source.io_observations.size());
  for (const B1ComputeIoObservation& observation : source.io_observations) {
    io.push_back(encode_io_observation(observation));
  }
  return encode_b1_fixed_record(
      {encode_b1_job_instance(source.job),
       std::to_string(source.producer_offer_ordinal),
       encode_time(source.offered_at), encode_time(source.endpoint_at),
       boolean_text(source.run_succeeded),
       boolean_text(source.verified_endpoint),
       encode_b1_physical_trace(source.physical_trace), std::to_string(status),
       source.output_receipt.has_value()
           ? encode_b1_receipt(*source.output_receipt)
           : "not-applicable",
       encode_record_list(io),
       encode_b1_fixed_record(
           {std::to_string(source.golden.job_index),
            encode_content_digest(source.golden.logical_digest),
            b1_digest_hex(source.golden.raw_payload_digest)}),
       encode_source_bytes(source.semantic_trace),
       b1_digest_hex(source.semantic_trace_digest)});
}

/**
 * @brief Parses one complete authority-free B1 M1 source.
 * @param record Thirteen-component canonical source record.
 * @return Exact offer/raw-trace/output/golden/I/O evidence.
 * @throws std::invalid_argument for framing, enum, digest, or identity drift.
 */
M1BatchSourceEvidence parse_b1_source(std::string_view record) {
  const std::vector<std::string> fields = parse_b1_fixed_record(record, 13U);
  M1BatchSourceEvidence source;
  source.job = parse_job(fields[0U]);
  source.producer_offer_ordinal = parse_b1_canonical_uint64(fields[1U]);
  source.offered_at = parse_time(fields[2U]);
  source.endpoint_at = parse_time(fields[3U]);
  source.run_succeeded = parse_boolean(fields[4U]);
  source.verified_endpoint = parse_boolean(fields[5U]);
  source.physical_trace = parse_b1_physical_trace(fields[6U]);
  source.output_status = parse_zero_based_enum<B1OutputCommitStatus>(
      fields[7U], 8U, "M1 B1 output status is unknown.");
  if (fields[8U] != "not-applicable") {
    source.output_receipt = parse_b1_receipt(fields[8U]);
  }
  for (const std::string& observation : parse_b1_framed_list(fields[9U])) {
    source.io_observations.push_back(parse_io_observation(observation));
  }
  const std::vector<std::string> golden =
      parse_b1_fixed_record(fields[10U], 3U);
  source.golden = B1JobGolden{parse_b1_canonical_uint64(golden[0U]),
                              parse_content_digest(golden[1U]),
                              parse_b1_digest(golden[2U])};
  source.semantic_trace = parse_source_bytes(fields[11U]);
  source.semantic_trace_digest = parse_b1_digest(fields[12U]);
  return source;
}

/**
 * @brief Compares complete class-start values.
 * @param lhs First start.
 * @param rhs Second start.
 * @return True only when every evaluator input field matches.
 * @throws Nothing.
 */
bool same_class_start(const M1ClassStartSample& lhs,
                      const M1ClassStartSample& rhs) noexcept {
  return lhs.causal_sequence == rhs.causal_sequence &&
         lhs.service_class == rhs.service_class &&
         lhs.interactive_candidate_startable ==
             rhs.interactive_candidate_startable &&
         lhs.throughput_candidate_startable ==
             rhs.throughput_candidate_startable &&
         lhs.execution_grant_committed == rhs.execution_grant_committed;
}

/**
 * @brief Derives measured-window class starts from closed mixed observations.
 * @param observations Complete ordered producer observation snapshot.
 * @param boundaries Exact M1 measured interval.
 * @return Every ServiceStart inside `[B,U)` in causal order.
 * @throws std::invalid_argument for event ordering, enum, or role drift.
 * @throws std::bad_alloc when result storage allocates.
 */
std::vector<M1ClassStartSample> derive_class_starts(
    const M1FairnessObservationSnapshot& observations,
    const M1BoundaryEvidence& boundaries) {
  std::vector<M1ClassStartSample> result;
  std::uint64_t prior_sequence = 0U;
  std::optional<std::chrono::steady_clock::time_point> prior_time;
  for (const M1FairnessObservation& event : observations.events) {
    const std::uint32_t kind = static_cast<std::uint32_t>(event.kind);
    const std::uint32_t tag = static_cast<std::uint32_t>(event.request_tag);
    const std::uint32_t service_class =
        static_cast<std::uint32_t>(event.service_class);
    const std::uint32_t task_terminal =
        static_cast<std::uint32_t>(event.task_terminal_kind);
    const std::uint32_t run_terminal =
        static_cast<std::uint32_t>(event.run_terminal_kind);
    if (kind > 3U || tag > 2U || service_class > 1U || task_terminal > 2U ||
        run_terminal > 2U || event.causal_sequence == 0U ||
        event.causal_sequence <= prior_sequence || event.run_id == 0U ||
        (prior_time.has_value() && event.observed_at < *prior_time)) {
      throw std::invalid_argument(
          "M1 mixed observation is invalid or unordered.");
    }
    const bool expected_qos =
        (event.request_tag == M1ObservedRequestTag::Interactive &&
         event.service_class == compute::ComputeRunQosClass::Interactive) ||
        (event.request_tag != M1ObservedRequestTag::Interactive &&
         event.service_class == compute::ComputeRunQosClass::Throughput);
    if (event.qos_matches_tag != expected_qos) {
      throw std::invalid_argument(
          "M1 mixed observation QoS relation is contradictory.");
    }
    const bool service_start = event.kind == M1ObservationKind::ServiceStart;
    const bool task_terminal_event =
        event.kind == M1ObservationKind::TaskTerminal;
    if ((service_start &&
         (event.local_task_id == 0U || event.service_charge == 0U)) ||
        (task_terminal_event &&
         (event.local_task_id == 0U || event.service_charge != 0U)) ||
        (!service_start && !task_terminal_event &&
         (event.local_task_id != 0U || event.service_charge != 0U)) ||
        (!service_start && (event.interactive_candidate_startable ||
                            event.throughput_candidate_startable ||
                            event.execution_grant_committed))) {
      throw std::invalid_argument(
          "M1 mixed observation kind-specific fields are contradictory.");
    }
    if (service_start &&
        boundaries.measurement_start.timestamp <= event.observed_at &&
        event.observed_at < boundaries.measurement_end.timestamp) {
      result.push_back(M1ClassStartSample{event.causal_sequence,
                                          event.service_class,
                                          event.interactive_candidate_startable,
                                          event.throughput_candidate_startable,
                                          event.execution_grant_committed});
    }
    prior_sequence = event.causal_sequence;
    prior_time = event.observed_at;
  }
  return result;
}

/**
 * @brief Validates that retained class starts are exactly event-derived.
 * @param input Complete M1 evaluator input.
 * @param observations Complete mixed observation snapshot.
 * @return Nothing after exact field-by-field agreement.
 * @throws std::invalid_argument for observer flag or class-start drift.
 */
void validate_observation_projection(
    const M1InnerRowInput& input,
    const M1FairnessObservationSnapshot& observations) {
  if (input.fairness.observation_overflowed != observations.overflowed ||
      input.fairness.observation_sequence_exhausted !=
          observations.sequence_exhausted ||
      input.fairness.observation_qos_mismatch != observations.qos_mismatch ||
      input.fairness.observation_publication_unstable ==
          observations.stable_publication_cut) {
    throw std::invalid_argument(
        "M1 observer flags disagree with the retained observation cut.");
  }
  if (observations.callback_completion_frontier >
          observations.callback_entry_frontier ||
      observations.published_slot_frontier >
          observations.claimed_slot_frontier ||
      observations.published_slot_frontier != observations.events.size() ||
      (observations.stable_publication_cut &&
       (observations.callback_frontier_exhausted ||
        observations.callback_entry_frontier !=
            observations.callback_completion_frontier ||
        observations.claimed_slot_frontier !=
            observations.published_slot_frontier))) {
    throw std::invalid_argument(
        "M1 observation publication frontiers are contradictory.");
  }
  const std::vector<M1ClassStartSample> derived =
      derive_class_starts(observations, input.protocol.boundaries);
  if (derived.size() != input.fairness.class_starts.size()) {
    throw std::invalid_argument(
        "M1 class starts do not match retained mixed observations.");
  }
  for (std::size_t index = 0U; index < derived.size(); ++index) {
    if (!same_class_start(derived[index], input.fairness.class_starts[index])) {
      throw std::invalid_argument(
          "M1 class start differs from its mixed observation.");
    }
  }
}

/**
 * @brief Compares the six closed M1 verdict fields exactly.
 * @param lhs First evaluated row.
 * @param rhs Second evaluated row.
 * @return True only when all five axes and overall match.
 * @throws Nothing.
 */
bool same_verdicts(const M1InnerRow& lhs, const M1InnerRow& rhs) noexcept {
  return lhs.latency_verdict == rhs.latency_verdict &&
         lhs.throughput_progress_verdict == rhs.throughput_progress_verdict &&
         lhs.fairness_verdict == rhs.fairness_verdict &&
         lhs.waste_verdict == rhs.waste_verdict &&
         lhs.memory_verdict == rhs.memory_verdict &&
         lhs.overall_verdict == rhs.overall_verdict;
}

}  // namespace

/** @copydoc materialize_m1_inner_row */
std::string materialize_m1_inner_row(
    const M1InnerRow& row, const M1FairnessObservationSnapshot& observations) {
  if (row.schema != kM1InnerRowSchema ||
      row.schema_version != kM1InnerRowSchemaVersion ||
      row.workload_id != kM1WorkloadId) {
    throw std::invalid_argument("M1 inner row schema identity is invalid.");
  }
  const M1InnerRow recomputed = evaluate_m1_inner_row(row.evidence);
  if (!recomputed.source_evidence_closed) {
    throw std::invalid_argument(
        "M1 canonical row source evidence is not exactly replayable.");
  }
  if (!same_verdicts(row, recomputed)) {
    throw std::invalid_argument(
        "M1 supplied verdicts do not recompute from retained evidence.");
  }
  validate_observation_projection(row.evidence, observations);
  if (!row.evidence.paired_isolated_i1_p99.has_value() ||
      row.evidence.paired_isolated_i1_p99->count() <= 0 ||
      !row.evidence.fairness.paired_isolated_b1.has_value() ||
      row.evidence.fairness.paired_isolated_b1->successful_site_operations ==
          0U ||
      row.evidence.fairness.paired_isolated_b1->duration.count() <= 0) {
    throw std::invalid_argument(
        "M1 canonical row requires both positive isolated denominators.");
  }

  const M1BoundaryEvidence& boundaries = row.evidence.protocol.boundaries;
  const std::string boundary_record = encode_b1_fixed_record(
      {encode_time(boundaries.cold_start.timestamp),
       std::to_string(boundaries.cold_start.event_sequence),
       encode_time(boundaries.warmup_start.timestamp),
       std::to_string(boundaries.warmup_start.event_sequence),
       encode_time(boundaries.measurement_start.timestamp),
       std::to_string(boundaries.measurement_start.event_sequence),
       encode_time(boundaries.measurement_end.timestamp),
       std::to_string(boundaries.measurement_end.event_sequence)});
  const std::string protocol_flags = encode_b1_fixed_record(
      {boolean_text(row.evidence.protocol.shared_execution_domain),
       boolean_text(row.evidence.protocol.boundary_was_zero_duration),
       boolean_text(row.evidence.protocol.raw_history_preserved),
       boolean_text(row.evidence.protocol.warmup_sources_closed),
       boolean_text(row.evidence.protocol.measured_counters_reset),
       boolean_text(row.evidence.protocol.final_settlement_proved),
       boolean_text(row.evidence.occurrence_attribution_proved),
       boolean_text(row.evidence.temporal_effects_complete),
       boolean_text(row.evidence.fairness.observation_overflowed),
       boolean_text(row.evidence.fairness.observation_sequence_exhausted),
       boolean_text(row.evidence.fairness.observation_qos_mismatch),
       boolean_text(row.evidence.fairness.observation_publication_unstable)});

  std::vector<std::string> interactive_records;
  interactive_records.reserve(
      row.evidence.protocol.interactive_occurrences.size());
  for (const M1InteractiveOccurrenceEvidence& occurrence :
       row.evidence.protocol.interactive_occurrences) {
    interactive_records.push_back(encode_b1_fixed_record(
        {phase_text(occurrence.phase), std::to_string(occurrence.phase_ordinal),
         encode_time(occurrence.origin.timestamp),
         std::to_string(occurrence.origin.event_sequence),
         encode_time(occurrence.settlement_endpoint),
         occurrence.settlement_observed.has_value()
             ? encode_time(occurrence.settlement_observed->timestamp)
             : "not-applicable",
         occurrence.settlement_observed.has_value()
             ? std::to_string(occurrence.settlement_observed->event_sequence)
             : "not-applicable",
         occurrence.final_latency.has_value()
             ? std::to_string(occurrence.final_latency->count())
             : "not-applicable",
         std::to_string(occurrence.service.all_started_service),
         std::to_string(occurrence.service.discarded_started_service),
         std::to_string(occurrence.service.post_cancel_started_service),
         verdict_text(occurrence.latency_verdict),
         verdict_text(occurrence.waste_verdict),
         verdict_text(occurrence.memory_verdict),
         verdict_text(occurrence.output_verdict),
         boolean_text(occurrence.phase_identity_immutable),
         boolean_text(occurrence.publication_current_at_measurement),
         boolean_text(occurrence.settlement_pending_at_measurement)}));
  }

  if (row.evidence.interactive_sources.size() != interactive_records.size()) {
    throw std::invalid_argument(
        "M1 canonical row requires one I1 source per occurrence.");
  }
  std::vector<std::string> interactive_source_records;
  interactive_source_records.reserve(row.evidence.interactive_sources.size());
  for (std::size_t index = 0U; index < row.evidence.interactive_sources.size();
       ++index) {
    const M1InteractiveSourceEvidence& source =
        row.evidence.interactive_sources[index];
    const M1InteractiveOccurrenceEvidence& occurrence =
        row.evidence.protocol.interactive_occurrences[index];
    if (source.phase != occurrence.phase ||
        source.phase_ordinal != occurrence.phase_ordinal ||
        !(source.origin == occurrence.origin)) {
      throw std::invalid_argument(
          "M1 I1 sources are missing, duplicated, or reordered.");
    }
    interactive_source_records.push_back(encode_i1_source(source));
  }

  std::vector<std::string> offer_records;
  offer_records.reserve(row.evidence.protocol.batch_offers.size());
  for (const M1BatchOfferEvidence& offer : row.evidence.protocol.batch_offers) {
    offer_records.push_back(encode_b1_fixed_record(
        {encode_b1_job_instance(offer.job),
         std::to_string(offer.producer_offer_ordinal),
         std::to_string(offer.attempt), encode_time(offer.offered.timestamp),
         std::to_string(offer.offered.event_sequence),
         offer.predecessor.has_value()
             ? encode_b1_job_instance(*offer.predecessor)
             : "not-applicable",
         offer.predecessor_terminal.has_value()
             ? encode_time(offer.predecessor_terminal->timestamp)
             : "not-applicable",
         offer.predecessor_terminal.has_value()
             ? std::to_string(offer.predecessor_terminal->event_sequence)
             : "not-applicable",
         offer.endpoint.has_value() ? encode_time(offer.endpoint->timestamp)
                                    : "not-applicable",
         offer.endpoint.has_value()
             ? std::to_string(offer.endpoint->event_sequence)
             : "not-applicable",
         boolean_text(offer.owner_settled), boolean_text(offer.output_removed),
         boolean_text(offer.phase_identity_immutable),
         boolean_text(offer.fifo_position_preserved),
         boolean_text(offer.resource_authority_preserved)}));
  }

  std::vector<std::string> carryover_records;
  carryover_records.reserve(row.evidence.protocol.carryover.size());
  for (const M1CarryoverEntry& entry : row.evidence.protocol.carryover) {
    const std::uint32_t state = static_cast<std::uint32_t>(entry.state);
    if (state > 3U) {
      throw std::invalid_argument("M1 carryover state is unknown.");
    }
    carryover_records.push_back(encode_b1_fixed_record(
        {entry.occurrence_key, phase_text(entry.phase), std::to_string(state),
         entry.queue_predecessor_key,
         boolean_text(entry.resource_authority_preserved),
         boolean_text(entry.publication_current),
         boolean_text(entry.owner_settled)}));
  }

  const M1FirstMeasuredAdmissionEvidence& first =
      row.evidence.protocol.first_measured_admission;
  const std::string first_record = encode_b1_fixed_record(
      {std::to_string(first.edit_index), encode_time(first.nominal_time),
       boolean_text(first.attempted), encode_time(first.admission_sample),
       first.reserved_event_sequence.has_value()
           ? std::to_string(*first.reserved_event_sequence)
           : "not-applicable",
       boolean_text(first.host_succeeded),
       first.accepted_coordinate.has_value()
           ? encode_time(first.accepted_coordinate->admission_time())
           : "not-applicable",
       first.accepted_coordinate.has_value()
           ? std::to_string(first.accepted_coordinate->event_sequence())
           : "not-applicable",
       boolean_text(first.warmup_publication_current_before_acceptance),
       boolean_text(first.superseded_exactly_at_acceptance),
       boolean_text(first.boundary_only_cancellation),
       encode_time(first.old_generation_settlement_endpoint)});

  std::vector<std::string> progress_records;
  progress_records.reserve(row.evidence.fairness.progress_windows.size());
  for (const M1ThroughputProgressSample& window :
       row.evidence.fairness.progress_windows) {
    if (window.duration != std::chrono::seconds(1)) {
      throw std::invalid_argument(
          "M1 canonical progress window is not exactly one second.");
    }
    progress_records.push_back(encode_b1_fixed_record(
        {std::to_string(window.window_ordinal),
         std::to_string(window.successful_site_operations),
         std::to_string(window.duration.count())}));
  }

  std::vector<std::string> graph_records;
  graph_records.reserve(row.evidence.fairness.graph_service_windows.size());
  for (const M1GraphServiceWindow& window :
       row.evidence.fairness.graph_service_windows) {
    graph_records.push_back(encode_b1_fixed_record(
        {std::to_string(window.window_ordinal),
         boolean_text(window.both_graphs_continuously_demanding),
         std::to_string(window.graph_a_completed_service),
         std::to_string(window.graph_b_completed_service)}));
  }

  std::vector<std::string> class_records;
  class_records.reserve(row.evidence.fairness.class_starts.size());
  for (const M1ClassStartSample& start : row.evidence.fairness.class_starts) {
    const std::uint32_t service_class =
        static_cast<std::uint32_t>(start.service_class);
    if (service_class > 1U) {
      throw std::invalid_argument("M1 class-start QoS class is unknown.");
    }
    class_records.push_back(encode_b1_fixed_record(
        {std::to_string(start.causal_sequence), std::to_string(service_class),
         boolean_text(start.interactive_candidate_startable),
         boolean_text(start.throughput_candidate_startable),
         boolean_text(start.execution_grant_committed)}));
  }

  std::size_t attempted = 0U;
  std::size_t classified = 0U;
  std::size_t failures = 0U;
  std::vector<std::string> headroom_records;
  headroom_records.reserve(row.evidence.fairness.headroom_outcomes.size());
  for (const M1HeadroomAdmissionOutcome& outcome :
       row.evidence.fairness.headroom_outcomes) {
    attempted += outcome.admission_attempted ? 1U : 0U;
    classified += outcome.host_status.has_value() ? 1U : 0U;
    failures += outcome.throughput_headroom_failure ? 1U : 0U;
    if (outcome.host_status.has_value()) {
      const std::uint32_t domain =
          static_cast<std::uint32_t>(outcome.host_status->domain);
      if (domain > 4U) {
        throw std::invalid_argument("M1 Host status domain is unknown.");
      }
    }
    headroom_records.push_back(encode_b1_fixed_record(
        {std::to_string(outcome.origin_ordinal),
         std::to_string(outcome.edit_index),
         boolean_text(outcome.admission_attempted),
         boolean_text(outcome.host_status.has_value()),
         outcome.host_status.has_value() ? boolean_text(outcome.host_status->ok)
                                         : "not-applicable",
         outcome.host_status.has_value()
             ? std::to_string(
                   static_cast<std::uint32_t>(outcome.host_status->domain))
             : "not-applicable",
         outcome.host_status.has_value()
             ? std::to_string(outcome.host_status->code)
             : "not-applicable",
         outcome.host_status.has_value()
             ? encode_source_bytes(outcome.host_status->name)
             : "not-applicable",
         outcome.host_status.has_value()
             ? encode_source_bytes(outcome.host_status->message)
             : "not-applicable",
         boolean_text(outcome.throughput_headroom_failure)}));
  }
  if (attempted != row.evidence.fairness.headroom_admissions.attempted_edits ||
      classified !=
          row.evidence.fairness.headroom_admissions.classified_outcomes ||
      failures != row.evidence.fairness.headroom_admissions
                      .throughput_headroom_failures) {
    throw std::invalid_argument(
        "M1 headroom aggregate does not derive from raw outcomes.");
  }

  std::vector<std::string> batch_source_records;
  batch_source_records.reserve(row.evidence.batch_sources.size());
  for (std::size_t index = 0U; index < row.evidence.batch_sources.size();
       ++index) {
    const M1BatchSourceEvidence& source = row.evidence.batch_sources[index];
    if (index >= row.evidence.protocol.batch_offers.size() ||
        !(source.job == row.evidence.protocol.batch_offers[index].job)) {
      throw std::invalid_argument(
          "M1 B1 sources are not in canonical protocol-offer order.");
    }
    batch_source_records.push_back(encode_b1_source(source));
  }

  std::vector<std::string> snapshot_records;
  snapshot_records.reserve(row.evidence.temporal_snapshots.size());
  for (const M1ExecutionSnapshot& snapshot : row.evidence.temporal_snapshots) {
    snapshot_records.push_back(encode_execution_snapshot(snapshot));
  }

  std::vector<std::string> mixed_records;
  mixed_records.reserve(observations.events.size());
  for (const M1FairnessObservation& event : observations.events) {
    mixed_records.push_back(encode_b1_fixed_record(
        {std::to_string(static_cast<std::uint32_t>(event.kind)),
         std::to_string(static_cast<std::uint32_t>(event.request_tag)),
         std::to_string(static_cast<std::uint32_t>(event.service_class)),
         std::to_string(event.causal_sequence), encode_time(event.observed_at),
         std::to_string(event.run_id), std::to_string(event.local_task_id),
         std::to_string(event.service_charge),
         std::to_string(static_cast<std::uint32_t>(event.task_terminal_kind)),
         std::to_string(static_cast<std::uint32_t>(event.run_terminal_kind)),
         boolean_text(event.qos_matches_tag),
         boolean_text(event.interactive_candidate_startable),
         boolean_text(event.throughput_candidate_startable),
         boolean_text(event.execution_grant_committed)}));
  }
  const std::string mixed_snapshot_record = encode_b1_fixed_record(
      {encode_record_list(mixed_records), boolean_text(observations.overflowed),
       boolean_text(observations.sequence_exhausted),
       boolean_text(observations.qos_mismatch),
       std::to_string(observations.callback_entry_frontier),
       std::to_string(observations.callback_completion_frontier),
       std::to_string(observations.claimed_slot_frontier),
       std::to_string(observations.published_slot_frontier),
       boolean_text(observations.callback_frontier_exhausted),
       boolean_text(observations.stable_publication_cut)});

  const M1BatchWasteEvidence& waste = row.evidence.batch_waste;
  const std::string waste_record = encode_b1_fixed_record(
      {std::to_string(waste.all_started_service),
       std::to_string(waste.discarded_started_service),
       std::to_string(waste.post_cancellation_started_service),
       std::to_string(waste.duplicate_service_starts),
       std::to_string(waste.retry_service_starts)});
  const std::string verdict_record = encode_b1_fixed_record(
      {verdict_text(row.latency_verdict),
       verdict_text(row.throughput_progress_verdict),
       verdict_text(row.fairness_verdict), verdict_text(row.waste_verdict),
       verdict_text(row.memory_verdict), verdict_text(row.overall_verdict)});
  const M1PairedB1RateEvidence& paired_b1 =
      *row.evidence.fairness.paired_isolated_b1;

  std::vector<B1CanonicalField> fields{
      known_field(std::string(kFieldNames[0U]), std::string(kFieldTypes[0U]),
                  std::to_string(kM1InnerRowSchemaVersion)),
      known_field(std::string(kFieldNames[1U]), std::string(kFieldTypes[1U]),
                  std::to_string(row.evidence.replicate_ordinal)),
      known_field(std::string(kFieldNames[2U]), std::string(kFieldTypes[2U]),
                  boundary_record),
      known_field(std::string(kFieldNames[3U]), std::string(kFieldTypes[3U]),
                  protocol_flags),
      known_field(std::string(kFieldNames[4U]), std::string(kFieldTypes[4U]),
                  encode_record_list(interactive_records)),
      known_field(std::string(kFieldNames[5U]), std::string(kFieldTypes[5U]),
                  encode_record_list(interactive_source_records)),
      known_field(std::string(kFieldNames[6U]), std::string(kFieldTypes[6U]),
                  encode_record_list(offer_records)),
      known_field(std::string(kFieldNames[7U]), std::string(kFieldTypes[7U]),
                  encode_record_list(carryover_records)),
      known_field(std::string(kFieldNames[8U]), std::string(kFieldTypes[8U]),
                  first_record),
      known_field(std::string(kFieldNames[9U]), std::string(kFieldTypes[9U]),
                  encode_record_list(progress_records)),
      known_field(std::string(kFieldNames[10U]), std::string(kFieldTypes[10U]),
                  encode_record_list(graph_records)),
      known_field(std::string(kFieldNames[11U]), std::string(kFieldTypes[11U]),
                  encode_record_list(class_records)),
      known_field(std::string(kFieldNames[12U]), std::string(kFieldTypes[12U]),
                  encode_record_list(headroom_records)),
      known_field(std::string(kFieldNames[13U]), std::string(kFieldTypes[13U]),
                  encode_record_list(batch_source_records)),
      known_field(std::string(kFieldNames[14U]), std::string(kFieldTypes[14U]),
                  encode_record_list(snapshot_records)),
      known_field(std::string(kFieldNames[15U]), std::string(kFieldTypes[15U]),
                  mixed_snapshot_record),
      known_field(std::string(kFieldNames[16U]), std::string(kFieldTypes[16U]),
                  std::to_string(row.evidence.paired_isolated_i1_p99->count())),
      known_field(std::string(kFieldNames[17U]), std::string(kFieldTypes[17U]),
                  encode_b1_fixed_record(
                      {std::to_string(paired_b1.successful_site_operations),
                       std::to_string(paired_b1.duration.count())})),
      known_field(std::string(kFieldNames[18U]), std::string(kFieldTypes[18U]),
                  waste_record),
      known_field(std::string(kFieldNames[19U]), std::string(kFieldTypes[19U]),
                  verdict_record)};
  return encode_b1_canonical_manifest(kM1InnerRowSchema, fields);
}

/** @copydoc parse_and_recompute_m1_inner_row */
M1CanonicalReplay parse_and_recompute_m1_inner_row(
    std::string_view canonical_bytes,
    std::uint64_t expected_replicate_ordinal) {
  if (expected_replicate_ordinal == 0U || expected_replicate_ordinal > 3U) {
    throw std::invalid_argument("M1 enclosing replicate ordinal is invalid.");
  }
  const B1CanonicalManifest manifest =
      parse_b1_canonical_manifest(canonical_bytes);
  if (manifest.schema != kM1InnerRowSchema ||
      manifest.fields.size() != std::size(kFieldNames) ||
      manifest.bytes != canonical_bytes) {
    throw std::invalid_argument(
        "M1 canonical inner manifest shape is invalid.");
  }
  for (std::size_t index = 0U; index < manifest.fields.size(); ++index) {
    const B1CanonicalField& field = manifest.fields[index];
    if (field.name != kFieldNames[index] || field.type != kFieldTypes[index] ||
        field.state != B1ObservationState::Known || field.reason != "none" ||
        field.payload.empty()) {
      throw std::invalid_argument(
          "M1 canonical inner field is unknown, missing, or reordered.");
    }
  }
  if (encode_b1_canonical_manifest(manifest.schema, manifest.fields) !=
      canonical_bytes) {
    throw std::invalid_argument("M1 canonical inner bytes are noncanonical.");
  }
  if (parse_b1_canonical_uint64(manifest.fields[0U].payload) !=
      kM1InnerRowSchemaVersion) {
    throw std::invalid_argument("M1 canonical inner schema version drifted.");
  }

  M1InnerRowInput input;
  input.replicate_ordinal =
      parse_b1_canonical_uint64(manifest.fields[1U].payload);
  if (input.replicate_ordinal != expected_replicate_ordinal) {
    throw std::invalid_argument(
        "M1 canonical inner replicate ordinal drifted.");
  }
  input.protocol.replicate_ordinal = input.replicate_ordinal;

  const std::vector<std::string> boundaries =
      parse_b1_fixed_record(manifest.fields[2U].payload, 8U);
  input.protocol.boundaries = M1BoundaryEvidence{
      M1EventCoordinate{parse_time(boundaries[0U]),
                        parse_b1_canonical_uint64(boundaries[1U])},
      M1EventCoordinate{parse_time(boundaries[2U]),
                        parse_b1_canonical_uint64(boundaries[3U])},
      M1EventCoordinate{parse_time(boundaries[4U]),
                        parse_b1_canonical_uint64(boundaries[5U])},
      M1EventCoordinate{parse_time(boundaries[6U]),
                        parse_b1_canonical_uint64(boundaries[7U])}};

  const std::vector<std::string> flags =
      parse_b1_fixed_record(manifest.fields[3U].payload, 12U);
  input.protocol.shared_execution_domain = parse_boolean(flags[0U]);
  input.protocol.boundary_was_zero_duration = parse_boolean(flags[1U]);
  input.protocol.raw_history_preserved = parse_boolean(flags[2U]);
  input.protocol.warmup_sources_closed = parse_boolean(flags[3U]);
  input.protocol.measured_counters_reset = parse_boolean(flags[4U]);
  input.protocol.final_settlement_proved = parse_boolean(flags[5U]);
  input.occurrence_attribution_proved = parse_boolean(flags[6U]);
  input.temporal_effects_complete = parse_boolean(flags[7U]);
  input.fairness.observation_overflowed = parse_boolean(flags[8U]);
  input.fairness.observation_sequence_exhausted = parse_boolean(flags[9U]);
  input.fairness.observation_qos_mismatch = parse_boolean(flags[10U]);
  input.fairness.observation_publication_unstable = parse_boolean(flags[11U]);

  const std::vector<std::string> interactive_records =
      parse_b1_framed_list(manifest.fields[4U].payload);
  if (interactive_records.size() != kM1TotalI1OriginCount) {
    throw std::invalid_argument(
        "M1 canonical row requires exactly 48 I1 occurrences.");
  }
  input.protocol.interactive_occurrences.reserve(interactive_records.size());
  for (std::size_t index = 0U; index < interactive_records.size(); ++index) {
    const std::string& record = interactive_records[index];
    const std::vector<std::string> fields = parse_b1_fixed_record(record, 18U);
    const bool settlement_absent = fields[5U] == "not-applicable";
    if (settlement_absent != (fields[6U] == "not-applicable")) {
      throw std::invalid_argument(
          "M1 I1 settlement coordinate is partially absent.");
    }
    M1InteractiveOccurrenceEvidence occurrence;
    occurrence.phase = parse_phase(fields[0U]);
    occurrence.phase_ordinal = parse_size(fields[1U]);
    const B1JobPhase expected_phase =
        index == 0U ? B1JobPhase::Cold
                    : (index <= kM1WarmupI1OriginCount ? B1JobPhase::Warmup
                                                       : B1JobPhase::Measured);
    const std::size_t expected_ordinal =
        index == 0U ? 0U
                    : (index <= kM1WarmupI1OriginCount
                           ? index - 1U
                           : index - 1U - kM1WarmupI1OriginCount);
    if (occurrence.phase != expected_phase ||
        occurrence.phase_ordinal != expected_ordinal) {
      throw std::invalid_argument(
          "M1 I1 occurrences are duplicated, missing, or reordered.");
    }
    occurrence.origin = M1EventCoordinate{
        parse_time(fields[2U]), parse_b1_canonical_uint64(fields[3U])};
    occurrence.settlement_endpoint = parse_time(fields[4U]);
    if (!settlement_absent) {
      occurrence.settlement_observed = M1EventCoordinate{
          parse_time(fields[5U]), parse_b1_canonical_uint64(fields[6U])};
    }
    if (fields[7U] != "not-applicable") {
      const std::int64_t duration = parse_int64(fields[7U]);
      if (duration <= 0) {
        throw std::invalid_argument("M1 I1 final latency is not positive.");
      }
      occurrence.final_latency = std::chrono::nanoseconds(duration);
    }
    occurrence.service.all_started_service =
        parse_b1_canonical_uint64(fields[8U]);
    occurrence.service.discarded_started_service =
        parse_b1_canonical_uint64(fields[9U]);
    occurrence.service.post_cancel_started_service =
        parse_b1_canonical_uint64(fields[10U]);
    occurrence.latency_verdict = parse_verdict(fields[11U]);
    occurrence.waste_verdict = parse_verdict(fields[12U]);
    occurrence.memory_verdict = parse_verdict(fields[13U]);
    occurrence.output_verdict = parse_verdict(fields[14U]);
    occurrence.phase_identity_immutable = parse_boolean(fields[15U]);
    occurrence.publication_current_at_measurement = parse_boolean(fields[16U]);
    occurrence.settlement_pending_at_measurement = parse_boolean(fields[17U]);
    input.protocol.interactive_occurrences.push_back(std::move(occurrence));
  }

  const std::vector<std::string> interactive_source_records =
      parse_b1_framed_list(manifest.fields[5U].payload);
  if (interactive_source_records.size() != kM1TotalI1OriginCount) {
    throw std::invalid_argument(
        "M1 canonical row requires exactly 48 I1 source rows.");
  }
  for (std::size_t index = 0U; index < interactive_source_records.size();
       ++index) {
    M1InteractiveSourceEvidence source =
        parse_i1_source(interactive_source_records[index]);
    const M1InteractiveOccurrenceEvidence& occurrence =
        input.protocol.interactive_occurrences[index];
    if (source.phase != occurrence.phase ||
        source.phase_ordinal != occurrence.phase_ordinal ||
        !(source.origin == occurrence.origin)) {
      throw std::invalid_argument(
          "M1 I1 sources are missing, duplicated, or reordered.");
    }
    input.interactive_sources.push_back(std::move(source));
  }

  const std::vector<std::string> offer_records =
      parse_b1_framed_list(manifest.fields[6U].payload);
  if (offer_records.empty()) {
    throw std::invalid_argument("M1 canonical row has no B1 offer evidence.");
  }
  input.protocol.batch_offers.reserve(offer_records.size());
  std::set<B1JobInstance> offered_jobs;
  std::optional<M1EventCoordinate> prior_offer;
  for (const std::string& record : offer_records) {
    const std::vector<std::string> fields = parse_b1_fixed_record(record, 15U);
    const bool predecessor_terminal_absent = fields[6U] == "not-applicable";
    const bool endpoint_absent = fields[8U] == "not-applicable";
    if (predecessor_terminal_absent != (fields[7U] == "not-applicable") ||
        endpoint_absent != (fields[9U] == "not-applicable")) {
      throw std::invalid_argument(
          "M1 B1 offer contains a partially absent coordinate.");
    }
    M1BatchOfferEvidence offer;
    offer.job = parse_job(fields[0U]);
    offer.producer_offer_ordinal = parse_b1_canonical_uint64(fields[1U]);
    offer.attempt = parse_b1_canonical_uint64(fields[2U]);
    offer.offered = M1EventCoordinate{parse_time(fields[3U]),
                                      parse_b1_canonical_uint64(fields[4U])};
    if (!offered_jobs.insert(offer.job).second ||
        (prior_offer.has_value() && !(prior_offer.value() < offer.offered))) {
      throw std::invalid_argument(
          "M1 B1 offers are duplicated or not in exact event order.");
    }
    prior_offer = offer.offered;
    if (fields[5U] != "not-applicable") {
      offer.predecessor = parse_job(fields[5U]);
    }
    if (!predecessor_terminal_absent) {
      offer.predecessor_terminal = M1EventCoordinate{
          parse_time(fields[6U]), parse_b1_canonical_uint64(fields[7U])};
    }
    if (!endpoint_absent) {
      offer.endpoint = M1EventCoordinate{parse_time(fields[8U]),
                                         parse_b1_canonical_uint64(fields[9U])};
    }
    offer.owner_settled = parse_boolean(fields[10U]);
    offer.output_removed = parse_boolean(fields[11U]);
    offer.phase_identity_immutable = parse_boolean(fields[12U]);
    offer.fifo_position_preserved = parse_boolean(fields[13U]);
    offer.resource_authority_preserved = parse_boolean(fields[14U]);
    input.protocol.batch_offers.push_back(std::move(offer));
  }

  const std::vector<std::string> carryover_records =
      parse_b1_framed_list(manifest.fields[7U].payload);
  if (carryover_records.size() != 3U) {
    throw std::invalid_argument(
        "M1 canonical row requires exactly three carryover records.");
  }
  std::set<std::string> carryover_keys;
  for (const std::string& record : carryover_records) {
    const std::vector<std::string> fields = parse_b1_fixed_record(record, 7U);
    if (fields[0U].empty() || !carryover_keys.insert(fields[0U]).second) {
      throw std::invalid_argument(
          "M1 carryover identity is empty or duplicated.");
    }
    input.protocol.carryover.push_back(
        M1CarryoverEntry{fields[0U], parse_phase(fields[1U]),
                         parse_zero_based_enum<M1CarryoverState>(
                             fields[2U], 3U, "M1 carryover state is unknown."),
                         fields[3U], parse_boolean(fields[4U]),
                         parse_boolean(fields[5U]), parse_boolean(fields[6U])});
  }

  const std::vector<std::string> first =
      parse_b1_fixed_record(manifest.fields[8U].payload, 12U);
  const bool accepted_absent = first[6U] == "not-applicable";
  if (accepted_absent != (first[7U] == "not-applicable")) {
    throw std::invalid_argument(
        "M1 first admission contains a partially absent coordinate.");
  }
  input.protocol.first_measured_admission.edit_index = parse_size(first[0U]);
  input.protocol.first_measured_admission.nominal_time = parse_time(first[1U]);
  input.protocol.first_measured_admission.attempted = parse_boolean(first[2U]);
  input.protocol.first_measured_admission.admission_sample =
      parse_time(first[3U]);
  if (first[4U] != "not-applicable") {
    input.protocol.first_measured_admission.reserved_event_sequence =
        parse_b1_canonical_uint64(first[4U]);
  }
  input.protocol.first_measured_admission.host_succeeded =
      parse_boolean(first[5U]);
  if (!accepted_absent) {
    input.protocol.first_measured_admission.accepted_coordinate.emplace(
        parse_time(first[6U]), parse_b1_canonical_uint64(first[7U]));
  }
  input.protocol.first_measured_admission
      .warmup_publication_current_before_acceptance = parse_boolean(first[8U]);
  input.protocol.first_measured_admission.superseded_exactly_at_acceptance =
      parse_boolean(first[9U]);
  input.protocol.first_measured_admission.boundary_only_cancellation =
      parse_boolean(first[10U]);
  input.protocol.first_measured_admission.old_generation_settlement_endpoint =
      parse_time(first[11U]);

  const std::vector<std::string> progress_records =
      parse_b1_framed_list(manifest.fields[9U].payload);
  if (progress_records.size() != kM1MeasuredWindowCount) {
    throw std::invalid_argument(
        "M1 canonical row requires exactly 30 progress windows.");
  }
  for (std::size_t index = 0U; index < progress_records.size(); ++index) {
    const std::vector<std::string> fields =
        parse_b1_fixed_record(progress_records[index], 3U);
    const std::int64_t duration = parse_int64(fields[2U]);
    if (parse_size(fields[0U]) != index ||
        duration != std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::seconds(1))
                        .count()) {
      throw std::invalid_argument(
          "M1 progress windows are unordered or not exactly one second.");
    }
    input.fairness.progress_windows.push_back(
        M1ThroughputProgressSample{index, parse_b1_canonical_uint64(fields[1U]),
                                   std::chrono::nanoseconds(duration)});
  }

  const std::vector<std::string> graph_records =
      parse_b1_framed_list(manifest.fields[10U].payload);
  if (graph_records.size() != kM1MeasuredWindowCount) {
    throw std::invalid_argument(
        "M1 canonical row requires exactly 30 Graph windows.");
  }
  for (std::size_t index = 0U; index < graph_records.size(); ++index) {
    const std::vector<std::string> fields =
        parse_b1_fixed_record(graph_records[index], 4U);
    if (parse_size(fields[0U]) != index) {
      throw std::invalid_argument("M1 Graph windows are unordered.");
    }
    input.fairness.graph_service_windows.push_back(M1GraphServiceWindow{
        index, parse_boolean(fields[1U]), parse_b1_canonical_uint64(fields[2U]),
        parse_b1_canonical_uint64(fields[3U])});
  }

  for (const std::string& record :
       parse_b1_framed_list(manifest.fields[11U].payload)) {
    const std::vector<std::string> fields = parse_b1_fixed_record(record, 5U);
    input.fairness.class_starts.push_back(M1ClassStartSample{
        parse_b1_canonical_uint64(fields[0U]),
        parse_zero_based_enum<compute::ComputeRunQosClass>(
            fields[1U], 1U, "M1 class-start QoS class is unknown."),
        parse_boolean(fields[2U]), parse_boolean(fields[3U]),
        parse_boolean(fields[4U])});
  }

  const std::vector<std::string> headroom_records =
      parse_b1_framed_list(manifest.fields[12U].payload);
  if (headroom_records.size() != kM1MeasuredI1AttemptCount) {
    throw std::invalid_argument(
        "M1 canonical row requires exactly 480 headroom outcomes.");
  }
  for (std::size_t index = 0U; index < headroom_records.size(); ++index) {
    const std::vector<std::string> fields =
        parse_b1_fixed_record(headroom_records[index], 10U);
    const std::size_t origin = parse_size(fields[0U]);
    const std::size_t edit = parse_size(fields[1U]);
    if (origin != index / kI1EditCount || edit != index % kI1EditCount) {
      throw std::invalid_argument("M1 headroom outcomes are unordered.");
    }
    M1HeadroomAdmissionOutcome outcome;
    outcome.origin_ordinal = origin;
    outcome.edit_index = edit;
    outcome.admission_attempted = parse_boolean(fields[2U]);
    const bool status_present = parse_boolean(fields[3U]);
    if (status_present) {
      OperationStatus status;
      status.ok = parse_boolean(fields[4U]);
      status.domain = parse_zero_based_enum<OperationErrorDomain>(
          fields[5U], 4U, "M1 Host status domain is unknown.");
      status.code = parse_int32(fields[6U]);
      status.name = parse_source_bytes(fields[7U]);
      status.message = parse_source_bytes(fields[8U]);
      outcome.host_status = std::move(status);
    } else if (fields[4U] != "not-applicable" ||
               fields[5U] != "not-applicable" ||
               fields[6U] != "not-applicable" ||
               fields[7U] != "not-applicable" ||
               fields[8U] != "not-applicable") {
      throw std::invalid_argument(
          "M1 absent Host status retains contradictory payload.");
    }
    outcome.throughput_headroom_failure = parse_boolean(fields[9U]);
    input.fairness.headroom_admissions.attempted_edits +=
        outcome.admission_attempted ? 1U : 0U;
    input.fairness.headroom_admissions.classified_outcomes +=
        outcome.host_status.has_value() ? 1U : 0U;
    input.fairness.headroom_admissions.throughput_headroom_failures +=
        outcome.throughput_headroom_failure ? 1U : 0U;
    input.fairness.headroom_outcomes.push_back(std::move(outcome));
  }

  const std::vector<std::string> batch_source_records =
      parse_b1_framed_list(manifest.fields[13U].payload);
  if (batch_source_records.size() != input.protocol.batch_offers.size()) {
    throw std::invalid_argument(
        "M1 B1 source cardinality differs from protocol offers.");
  }
  for (std::size_t index = 0U; index < batch_source_records.size(); ++index) {
    M1BatchSourceEvidence source = parse_b1_source(batch_source_records[index]);
    if (!(source.job == input.protocol.batch_offers[index].job)) {
      throw std::invalid_argument(
          "M1 B1 sources are missing, duplicated, or reordered.");
    }
    input.batch_sources.push_back(std::move(source));
  }

  const std::vector<std::string> snapshot_records =
      parse_b1_framed_list(manifest.fields[14U].payload);
  if (snapshot_records.size() < 4U) {
    throw std::invalid_argument(
        "M1 canonical row requires at least four temporal snapshots.");
  }
  for (std::size_t index = 0U; index < snapshot_records.size(); ++index) {
    M1ExecutionSnapshot snapshot =
        parse_execution_snapshot(snapshot_records[index]);
    if (snapshot.temporal_capture_ordinal != index) {
      throw std::invalid_argument("M1 temporal snapshots are unordered.");
    }
    input.temporal_snapshots.push_back(std::move(snapshot));
  }

  M1FairnessObservationSnapshot observations;
  const std::vector<std::string> observation_snapshot =
      parse_b1_fixed_record(manifest.fields[15U].payload, 10U);
  for (const std::string& record :
       parse_b1_framed_list(observation_snapshot[0U])) {
    const std::vector<std::string> fields = parse_b1_fixed_record(record, 14U);
    M1FairnessObservation event;
    event.kind = parse_zero_based_enum<M1ObservationKind>(
        fields[0U], 3U, "M1 observation kind is unknown.");
    event.request_tag = parse_zero_based_enum<M1ObservedRequestTag>(
        fields[1U], 2U, "M1 observation request tag is unknown.");
    event.service_class = parse_zero_based_enum<compute::ComputeRunQosClass>(
        fields[2U], 1U, "M1 observation QoS class is unknown.");
    event.causal_sequence = parse_b1_canonical_uint64(fields[3U]);
    event.observed_at = parse_time(fields[4U]);
    event.run_id = parse_b1_canonical_uint64(fields[5U]);
    event.local_task_id = parse_b1_canonical_uint64(fields[6U]);
    event.service_charge = parse_b1_canonical_uint64(fields[7U]);
    event.task_terminal_kind =
        parse_zero_based_enum<compute::ComputeRunTaskTerminalKind>(
            fields[8U], 2U, "M1 task terminal kind is unknown.");
    event.run_terminal_kind =
        parse_zero_based_enum<compute::ComputeRunTerminalKind>(
            fields[9U], 2U, "M1 Run terminal kind is unknown.");
    event.qos_matches_tag = parse_boolean(fields[10U]);
    event.interactive_candidate_startable = parse_boolean(fields[11U]);
    event.throughput_candidate_startable = parse_boolean(fields[12U]);
    event.execution_grant_committed = parse_boolean(fields[13U]);
    observations.events.push_back(std::move(event));
  }
  observations.overflowed = parse_boolean(observation_snapshot[1U]);
  observations.sequence_exhausted = parse_boolean(observation_snapshot[2U]);
  observations.qos_mismatch = parse_boolean(observation_snapshot[3U]);
  observations.callback_entry_frontier =
      parse_b1_canonical_uint64(observation_snapshot[4U]);
  observations.callback_completion_frontier =
      parse_b1_canonical_uint64(observation_snapshot[5U]);
  observations.claimed_slot_frontier = parse_size(observation_snapshot[6U]);
  observations.published_slot_frontier = parse_size(observation_snapshot[7U]);
  observations.callback_frontier_exhausted =
      parse_boolean(observation_snapshot[8U]);
  observations.stable_publication_cut = parse_boolean(observation_snapshot[9U]);

  const std::uint64_t isolated_i1 =
      parse_b1_canonical_uint64(manifest.fields[16U].payload);
  if (isolated_i1 == 0U ||
      isolated_i1 > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) {
    throw std::invalid_argument("M1 isolated-I1 denominator is invalid.");
  }
  input.paired_isolated_i1_p99 =
      std::chrono::nanoseconds(static_cast<std::int64_t>(isolated_i1));
  const std::vector<std::string> paired_b1 =
      parse_b1_fixed_record(manifest.fields[17U].payload, 2U);
  const std::uint64_t successful = parse_b1_canonical_uint64(paired_b1[0U]);
  const std::int64_t paired_duration = parse_int64(paired_b1[1U]);
  if (successful == 0U || paired_duration <= 0) {
    throw std::invalid_argument("M1 isolated-B1 denominator is invalid.");
  }
  input.fairness.paired_isolated_b1 = M1PairedB1RateEvidence{
      successful, std::chrono::nanoseconds(paired_duration)};

  const std::vector<std::string> waste =
      parse_b1_fixed_record(manifest.fields[18U].payload, 5U);
  input.batch_waste =
      M1BatchWasteEvidence{parse_b1_canonical_uint64(waste[0U]),
                           parse_b1_canonical_uint64(waste[1U]),
                           parse_b1_canonical_uint64(waste[2U]),
                           parse_size(waste[3U]), parse_size(waste[4U])};

  const std::vector<std::string> verdicts =
      parse_b1_fixed_record(manifest.fields[19U].payload, 6U);
  const I1Verdict claimed_latency = parse_verdict(verdicts[0U]);
  const I1Verdict claimed_progress = parse_verdict(verdicts[1U]);
  const I1Verdict claimed_fairness = parse_verdict(verdicts[2U]);
  const I1Verdict claimed_waste = parse_verdict(verdicts[3U]);
  const I1Verdict claimed_memory = parse_verdict(verdicts[4U]);
  const I1Verdict claimed_overall = parse_verdict(verdicts[5U]);
  input.fairness.interactive_latency_verdict = claimed_latency;

  validate_observation_projection(input, observations);
  M1CanonicalReplay replay;
  replay.observations = std::move(observations);
  replay.row = evaluate_m1_inner_row(std::move(input));
  if (replay.row.latency_verdict != claimed_latency ||
      replay.row.throughput_progress_verdict != claimed_progress ||
      replay.row.fairness_verdict != claimed_fairness ||
      replay.row.waste_verdict != claimed_waste ||
      replay.row.memory_verdict != claimed_memory ||
      replay.row.overall_verdict != claimed_overall) {
    throw std::invalid_argument(
        "M1 canonical five-axis or overall verdict does not recompute.");
  }
  if (materialize_m1_inner_row(replay.row, replay.observations) !=
      canonical_bytes) {
    throw std::invalid_argument(
        "M1 canonical inner evidence does not round-trip exactly.");
  }
  return replay;
}

}  // namespace ps::benchmark
