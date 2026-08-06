/**
 * @file b1_environment.cpp
 * @brief Implements closed B1 environment encoding, parsing, and eligibility.
 */
#include "benchmark/b1_environment.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ps::benchmark {
namespace {

/** @brief Exact storage manifest header without its LF. */
constexpr char kStorageSchema[] = "execution-profile-storage-environment-v1";

/** @brief Exact base manifest header without its LF. */
constexpr char kBaseSchema[] = "execution-profile-base-environment-v1";

/** @brief Exact environment-class header without its LF. */
constexpr char kEnvironmentClassSchema[] =
    "execution-profile-environment-class-v1";  // NOLINT(whitespace/indent_namespace)

/** @brief Name/type pair in one closed manifest schema. */
struct FieldSchema final {
  /** @brief Exact field name. */
  std::string_view name;
  /** @brief Exact declared scalar/composite type. */
  std::string_view type;
};

/** @brief Exact 21-field storage schema in canonical order. */
constexpr std::array<FieldSchema, 21U> kStorageFields{{
    {"output_store_contract_id", "identifier"},
    {"output_store_contract_generation", "uint64"},
    {"backend_semantics_id", "identifier"},
    {"backend_semantics_generation", "uint64"},
    {"backend_instance_id", "text"},
    {"backend_class", "enum"},
    {"locality", "enum"},
    {"persistence", "enum"},
    {"filesystem_type", "identifier"},
    {"mount_identity", "text"},
    {"mount_effective_options", "mount-map-v1"},
    {"commit_semantics", "commit-semantics-v1"},
    {"durability_capabilities", "token-set-v1"},
    {"requested_durability", "enum"},
    {"achieved_durability", "enum"},
    {"durability_endpoint_identity", "text"},
    {"durability_anchor_identity", "text"},
    {"storage_class", "enum"},
    {"b1_performance_configuration", "b1-performance-configuration-v1"},
    {"hardware_write_cache_policy", "enum"},
    {"power_loss_protection_policy", "enum"},
}};

/** @brief Exact 24-field base schema in canonical order. */
constexpr std::array<FieldSchema, 24U> kBaseFields{{
    {"os_family", "enum"},
    {"os_release", "text"},
    {"kernel_name", "text"},
    {"kernel_release", "text"},
    {"architecture", "enum"},
    {"cpu_inventory", "cpu-record-list-v1"},
    {"gpu_inventory", "device-record-list-v1"},
    {"other_device_inventory", "device-record-list-v1"},
    {"compiler_id", "enum"},
    {"compiler_version", "text"},
    {"compiler_target", "text"},
    {"standard_library_id", "enum"},
    {"standard_library_version", "text"},
    {"build_mode", "enum"},
    {"build_flags", "ordered-text-list-v1"},
    {"process_worker_count", "uint64"},
    {"provider_contracts", "contract-record-list-v1"},
    {"plugin_contracts", "contract-record-list-v1"},
    {"resource_limits", "resource-limits-v1"},
    {"metal_resource_limits", "metal-resource-limits-v1"},
    {"cache_preconditions", "cache-preconditions-v1"},
    {"residency_preconditions", "residency-preconditions-v1"},
    {"power_policy", "power-policy-v1"},
    {"thermal_eligibility", "thermal-eligibility-v1"},
}};

/** @brief Exact four-field environment-class schema in canonical order. */
constexpr std::array<FieldSchema, 4U> kEnvironmentClassFields{{
    {"base_environment_digest", "sha256"},
    {"storage_environment_applicability", "enum"},
    {"storage_environment_not_applicable_reason", "enum"},
    {"storage_environment_digest", "sha256"},
}};

/** @brief Exact performance component type sequence. */
constexpr std::array<std::string_view, 37U> kPerformanceTypes{{
    "enum",       "identifier", "uint64",     "identifier", "enum",
    "identifier", "enum",       "identifier", "enum",       "uint64",
    "uint64",     "uint64",     "uint64",     "enum",       "enum",
    "enum",       "uint64",     "uint64",     "uint64",     "uint64",
    "identifier", "enum",       "identifier", "identifier", "enum",
    "uint64",     "enum",       "uint64",     "enum",       "identifier",
    "identifier", "uint64",     "identifier", "identifier", "identifier",
    "identifier", "identifier",
}};

/** @brief Exact required durability capability universe. */
const std::vector<std::string> kDurabilityCapabilities{
    "atomic-no-replace",
    "atomic-visible",
    "crash-durable",
    "idempotent-reconcile",
    "manifest-last",
    "manifest-sync",
    "namespace-durability-barrier",
    "payload-sync"};  // NOLINT(whitespace/indent_namespace)

/** @brief Exact storage eligibility reason order. */
constexpr std::array<std::string_view, 11U> kEligibilityReasonOrder{{
    "canonical-schema-invalid",
    "commit-semantics-inconsistent",
    "durability-class-not-crash-durable",
    "durability-path-inconsistent",
    "mount-normalization-unprovable",
    "not-applicable-proof-invalid",
    "performance-configuration-unprovable",
    "raw-observation-proof-incomplete",
    "required-capability-absent",
    "required-observation-ineligible",
    "root-containment-unproved",
}};

/**
 * @brief Tests membership in a closed string domain.
 * @param value Candidate token.
 * @param domain Closed domain.
 * @return True when an exact token exists.
 * @throws Nothing.
 */
bool in_domain(std::string_view value,
               const std::vector<std::string_view>& domain) noexcept {
  return std::find(domain.begin(), domain.end(), value) != domain.end();
}

/**
 * @brief Parses one canonical unpadded uint64 token.
 * @param text Candidate ASCII text.
 * @return Parsed value.
 * @throws std::invalid_argument for padding, malformed text, or overflow.
 */
std::uint64_t parse_uint64(std::string_view text) {
  if (text.empty() || (text.size() > 1U && text.front() == '0')) {
    throw std::invalid_argument("Environment uint64 is not canonical.");
  }
  std::uint64_t value = 0U;
  const char* const begin = text.data();
  const char* const end = begin + text.size();
  const std::from_chars_result result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    throw std::invalid_argument("Environment uint64 is invalid or overflowed.");
  }
  return value;
}

/**
 * @brief Checks the closed generic identifier lexical grammar.
 * @param value Candidate token.
 * @return True for `[a-z0-9][a-z0-9._+-]*`.
 * @throws Nothing.
 */
bool valid_identifier(std::string_view value) noexcept {
  if (value.empty()) {
    return false;
  }
  for (std::size_t index = 0U; index < value.size(); ++index) {
    const char character = value[index];
    const bool alphanumeric = (character >= 'a' && character <= 'z') ||
                              (character >= '0' && character <= '9');
    if (!alphanumeric &&
        (index == 0U || (character != '.' && character != '_' &&
                         character != '+' && character != '-'))) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Validates a lowercase hexadecimal payload.
 * @param value Candidate text.
 * @param required_bytes Optional exact decoded byte count, zero for any.
 * @return True for nonempty even lowercase hexadecimal text and exact size.
 * @throws Nothing.
 */
bool valid_lower_hex(std::string_view value,
                     std::size_t required_bytes = 0U) noexcept {
  if (value.empty() || value.size() % 2U != 0U ||
      (required_bytes != 0U && value.size() != required_bytes * 2U)) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

/**
 * @brief Decodes one lowercase-hex text payload.
 * @param value Validated even hexadecimal text.
 * @return Decoded bytes.
 * @throws std::invalid_argument for non-hex input.
 * @throws std::bad_alloc when output allocation fails.
 */
std::vector<std::uint8_t> decode_hex(std::string_view value) {
  if (!valid_lower_hex(value)) {
    throw std::invalid_argument(
        "Environment text payload is not lowercase hex.");
  }
  const auto nibble = [](char character) -> std::uint8_t {
    if (character <= '9') {
      return static_cast<std::uint8_t>(character - '0');
    }
    return static_cast<std::uint8_t>(character - 'a' + 10);
  };
  std::vector<std::uint8_t> decoded(value.size() / 2U);
  for (std::size_t index = 0U; index < decoded.size(); ++index) {
    decoded[index] = static_cast<std::uint8_t>(
        (nibble(value[index * 2U]) << 4U) | nibble(value[index * 2U + 1U]));
  }
  return decoded;
}

/**
 * @brief Validates that bytes are the exact stable NFC form of UTF-8 text.
 * @param bytes Decoded text bytes.
 * @return True only for structurally valid nonempty NFC UTF-8 input.
 * @throws Nothing.
 * @note The explicit-length mapping preserves embedded U+0000 bytes. The
 * normalized allocation is always released before returning.
 */
bool valid_nfc_utf8(const std::vector<std::uint8_t>& bytes) noexcept {
  if (bytes.empty() ||
      bytes.size() > static_cast<std::size_t>(
                         std::numeric_limits<utf8proc_ssize_t>::max())) {
    return false;
  }
  utf8proc_uint8_t* normalized = nullptr;
  const utf8proc_option_t options =
      static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE);
  const utf8proc_ssize_t normalized_size =
      utf8proc_map(bytes.data(), static_cast<utf8proc_ssize_t>(bytes.size()),
                   &normalized, options);
  const bool valid =
      normalized_size == static_cast<utf8proc_ssize_t>(bytes.size()) &&
      normalized != nullptr &&
      std::equal(bytes.begin(), bytes.end(), normalized);
  std::free(normalized);
  return valid;
}

/**
 * @brief Parses one length frame from arbitrary bytes with checked bounds.
 * @param bytes Complete containing payload.
 * @param cursor In/out current byte offset.
 * @return Borrowed frame payload.
 * @throws std::invalid_argument for malformed length or truncation.
 */
std::string_view parse_frame(std::string_view bytes, std::size_t* cursor) {
  if (cursor == nullptr || *cursor >= bytes.size()) {
    throw std::invalid_argument("Environment frame is missing.");
  }
  const std::size_t colon = bytes.find(':', *cursor);
  if (colon == std::string_view::npos) {
    throw std::invalid_argument("Environment frame length has no colon.");
  }
  const std::uint64_t length =
      parse_uint64(bytes.substr(*cursor, colon - *cursor));
  const std::size_t payload_start = colon + 1U;
  if (length > static_cast<std::uint64_t>(bytes.size() - payload_start)) {
    throw std::invalid_argument("Environment frame exceeds remaining bytes.");
  }
  const std::size_t length_size = static_cast<std::size_t>(length);
  *cursor = payload_start + length_size;
  return bytes.substr(payload_start, length_size);
}

/**
 * @brief Parses count-prefixed framed items with checked consumption.
 * @param payload Complete composite payload.
 * @param frames_per_item Exact frames in one logical item.
 * @return Flattened frames in encoded order.
 * @throws std::invalid_argument for count, overflow, or consumption drift.
 */
std::vector<std::string_view> parse_counted_frames(
    std::string_view payload, std::size_t frames_per_item) {
  const std::size_t colon = payload.find(':');
  if (colon == std::string_view::npos || frames_per_item == 0U) {
    throw std::invalid_argument("Environment collection count is invalid.");
  }
  const std::uint64_t count = parse_uint64(payload.substr(0U, colon));
  if (count > std::numeric_limits<std::size_t>::max() / frames_per_item) {
    throw std::invalid_argument("Environment collection count overflowed.");
  }
  const std::size_t frame_count =
      static_cast<std::size_t>(count) * frames_per_item;
  std::vector<std::string_view> frames;
  frames.reserve(frame_count);
  std::size_t cursor = colon + 1U;
  for (std::size_t index = 0U; index < frame_count; ++index) {
    frames.push_back(parse_frame(payload, &cursor));
  }
  if (cursor != payload.size()) {
    throw std::invalid_argument("Environment collection consumption drifted.");
  }
  return frames;
}

/**
 * @brief Parses an exact number of fixed-record component frames.
 * @param payload Complete fixed-record payload.
 * @param component_count Required frame count.
 * @return Borrowed components in order.
 * @throws std::invalid_argument for missing/extra/truncated components.
 */
std::vector<std::string_view> parse_fixed_frames(std::string_view payload,
                                                 std::size_t component_count) {
  std::vector<std::string_view> components;
  components.reserve(component_count);
  std::size_t cursor = 0U;
  for (std::size_t index = 0U; index < component_count; ++index) {
    components.push_back(parse_frame(payload, &cursor));
  }
  if (cursor != payload.size()) {
    throw std::invalid_argument("Environment fixed-record shape drifted.");
  }
  return components;
}

/**
 * @brief Parses one closed observation-state token.
 * @param token Candidate token.
 * @return Exact state.
 * @throws std::invalid_argument for an unknown token.
 */
B1ObservationState parse_state(std::string_view token) {
  if (token == "known") {
    return B1ObservationState::Known;
  }
  if (token == "not-applicable") {
    return B1ObservationState::NotApplicable;
  }
  if (token == "unknown") {
    return B1ObservationState::Unknown;
  }
  if (token == "unobserved") {
    return B1ObservationState::Unobserved;
  }
  if (token == "unsupported") {
    return B1ObservationState::Unsupported;
  }
  if (token == "unprovable") {
    return B1ObservationState::Unprovable;
  }
  throw std::invalid_argument("Environment observation state is invalid.");
}

/**
 * @brief Returns the canonical token for one observation state.
 * @param state Valid state.
 * @return Process-lifetime token.
 * @throws std::invalid_argument for an invalid enum representation.
 */
const char* state_name(B1ObservationState state) {
  switch (state) {
    case B1ObservationState::Known:
      return "known";
    case B1ObservationState::NotApplicable:
      return "not-applicable";
    case B1ObservationState::Unknown:
      return "unknown";
    case B1ObservationState::Unobserved:
      return "unobserved";
    case B1ObservationState::Unsupported:
      return "unsupported";
    case B1ObservationState::Unprovable:
      return "unprovable";
  }
  throw std::invalid_argument("Environment observation state is invalid.");
}

/** @brief Forward declaration for explicit ASCII case normalization. */
std::string ascii_lower(std::string_view value);

/** @brief Forward declaration for closed performance-record validation. */
std::vector<std::string_view> validate_performance_payload(
    std::string_view payload);

/** @brief Forward declaration for scalar lexical/domain validation. */
void validate_scalar(std::string_view type, std::string_view payload,
                     std::string_view field);

/** @brief Forward declaration for state/reason/payload validation. */
void validate_field_envelope(const B1CanonicalField& field);

/** @brief Forward declaration for sorted fixed-record list validation. */
void validate_record_list(std::string_view payload, std::size_t minimum_count,
                          const std::vector<std::string_view>& component_types,
                          bool device_class = false);

/** @brief Forward declaration for closed map validation. */
void validate_closed_map(
    std::string_view payload,
    const std::vector<
        std::pair<std::string_view, std::vector<std::string_view>>>& expected);

/** @brief Forward declaration for exact composite validation. */
void validate_composite(std::string_view field, std::string_view type,
                        std::string_view payload);

/** @brief Forward declaration for exact field-sequence validation. */
template <std::size_t Size>
void validate_fields(const std::vector<B1CanonicalField>& fields,
                     const std::array<FieldSchema, Size>& schema);

/** @brief Forward declaration for storage cross-field validation. */
void validate_storage_cross_fields(const std::vector<B1CanonicalField>& fields);

/** @brief Forward declaration for base cross-field validation. */
void validate_base_cross_fields(const std::vector<B1CanonicalField>& fields);

/** @brief Forward declaration for environment-class relation validation. */
void validate_environment_class_cross_fields(
    const std::vector<B1CanonicalField>& fields);

/** @brief Forward declaration for final manifest assembly. */
std::string encode_manifest(std::string_view header,
                            const std::vector<B1CanonicalField>& fields);

/** @brief Forward declaration for closed field lookup. */
const B1CanonicalField& find_field(const B1CanonicalManifest& manifest,
                                   std::string_view name);

/** @brief Forward declaration for all-known field testing. */
bool fields_known(const B1CanonicalManifest& manifest,
                  const std::vector<std::string_view>& names);

}  // namespace

B1RawFieldObservation normalize_b1_mount_options(
    const B1MountNormalizationInput& input) {
  const std::vector<std::pair<std::string_view, std::vector<std::string_view>>>
      schema{
          {"access_mode", {"read-only", "read-write"}},
          {"atime_policy", {"strict", "relaxed", "none"}},
          {"cache_coherence",
           {"host-local", "close-to-open", "strong", "eventual"}},
          {"copy_on_write_mode", {"disabled", "enabled", "provider-managed"}},
          {"data_write_mode", {"buffered", "synchronous"}},
          {"journal_mode",
           {"none", "writeback", "ordered", "full", "provider-managed"}},
          {"metadata_write_mode", {"buffered", "synchronous"}}};
  std::map<std::string, std::string> effective;
  bool unknown_seen = false;
  for (const B1NativeMountOption& option : input.options) {
    const std::string key =
        input.ascii_case_insensitive ? ascii_lower(option.key) : option.key;
    const std::string value =
        input.ascii_case_insensitive ? ascii_lower(option.value) : option.value;
    const auto schema_iterator =
        std::find_if(schema.begin(), schema.end(),
                     [&key](const auto& entry) { return entry.first == key; });
    if (schema_iterator == schema.end()) {
      unknown_seen = true;
      continue;
    }
    if (!in_domain(value, schema_iterator->second)) {
      return B1RawFieldObservation{B1ObservationState::Unprovable,
                                   "conflicting-effective-values",
                                   "mount-map-v1",
                                   "",
                                   false,
                                   false};
    }
    const auto prior = effective.find(key);
    if (prior != effective.end() && prior->second != value &&
        !input.duplicate_last_wins_proved) {
      return B1RawFieldObservation{B1ObservationState::Unprovable,
                                   "conflicting-effective-values",
                                   "mount-map-v1",
                                   "",
                                   false,
                                   false};
    }
    effective[key] = value;
  }
  if (unknown_seen && !input.unknown_options_no_effect_proved) {
    return B1RawFieldObservation{B1ObservationState::Unprovable,
                                 "evidence-chain-incomplete",
                                 "mount-map-v1",
                                 "",
                                 false,
                                 false};
  }
  for (const auto& [key_view, domain] : schema) {
    const std::string key(key_view);
    if (effective.find(key) != effective.end()) {
      continue;
    }
    const auto default_iterator = input.defaults.find(key);
    if (default_iterator == input.defaults.end()) {
      return B1RawFieldObservation{B1ObservationState::Unprovable,
                                   "evidence-chain-incomplete",
                                   "mount-map-v1",
                                   "",
                                   false,
                                   false};
    }
    const std::string value = input.ascii_case_insensitive
                                  ? ascii_lower(default_iterator->second)
                                  : default_iterator->second;
    if (!in_domain(value, domain)) {
      return B1RawFieldObservation{B1ObservationState::Unprovable,
                                   "conflicting-effective-values",
                                   "mount-map-v1",
                                   "",
                                   false,
                                   false};
    }
    effective.emplace(key, value);
  }
  std::vector<std::pair<std::string, std::string>> entries(effective.begin(),
                                                           effective.end());
  return B1RawFieldObservation{
      B1ObservationState::Known, "none", "mount-map-v1",
      encode_b1_map(entries),    true,   false};
}

B1RawFieldObservation map_b1_performance_configuration(
    const std::array<std::string, 37U>& components,
    const B1PerformanceProofs& proofs) {
  std::vector<std::string> values(components.begin(), components.end());
  const std::string payload = encode_b1_fixed_record(values);
  static_cast<void>(validate_performance_payload(payload));
  if (proofs.conflicting_values) {
    return B1RawFieldObservation{B1ObservationState::Unprovable,
                                 "conflicting-effective-values",
                                 "b1-performance-configuration-v1",
                                 "",
                                 false,
                                 false};
  }
  if (!proofs.one_frozen_observation || !proofs.complete_option_mapping ||
      !proofs.stable_through_replicate) {
    return B1RawFieldObservation{B1ObservationState::Unprovable,
                                 "evidence-chain-incomplete",
                                 "b1-performance-configuration-v1",
                                 "",
                                 false,
                                 false};
  }
  std::vector<std::string> proof_kinds = proofs.proof_kinds;
  std::sort(proof_kinds.begin(), proof_kinds.end());
  if (std::adjacent_find(proof_kinds.begin(), proof_kinds.end()) !=
      proof_kinds.end()) {
    throw std::invalid_argument(
        "B1 performance proof kinds contain duplicate.");
  }
  const auto has_proof = [&proof_kinds](std::string_view proof) {
    return std::binary_search(proof_kinds.begin(), proof_kinds.end(), proof);
  };
  const std::array<std::pair<std::size_t, std::string_view>, 4U> byte_units{{
      {9U, "logical-block-bytes-absent"},
      {10U, "physical-block-bytes-absent"},
      {11U, "record-bytes-absent"},
      {12U, "allocation-unit-bytes-absent"},
  }};
  for (const auto& [index, proof] : byte_units) {
    const bool zero = parse_uint64(components[index]) == 0U;
    const bool proved_absent = has_proof(proof);
    if (zero && !proved_absent) {
      return B1RawFieldObservation{B1ObservationState::Unprovable,
                                   "evidence-chain-incomplete",
                                   "b1-performance-configuration-v1",
                                   "",
                                   false,
                                   false};
    }
    if (!zero && proved_absent) {
      return B1RawFieldObservation{B1ObservationState::Unprovable,
                                   "conflicting-effective-values",
                                   "b1-performance-configuration-v1",
                                   "",
                                   false,
                                   false};
    }
  }
  if (components[15U] == "provider-managed") {
    const std::array<std::pair<std::size_t, std::string_view>, 4U> geometry{{
        {16U, "provider-layout-data-units-absent"},
        {17U, "provider-layout-parity-units-absent"},
        {18U, "provider-layout-replica-count-absent"},
        {19U, "provider-layout-stripe-unit-absent"},
    }};
    for (const auto& [index, proof] : geometry) {
      const bool zero = parse_uint64(components[index]) == 0U;
      if (zero != has_proof(proof)) {
        return B1RawFieldObservation{
            B1ObservationState::Unprovable,
            zero ? "evidence-chain-incomplete" : "conflicting-effective-values",
            "b1-performance-configuration-v1",
            "",
            false,
            false};
      }
    }
  }
  const bool upper_cache_absence_proved = has_proof("upper-write-cache-absent");
  if (components[21U] != "absent" && upper_cache_absence_proved) {
    return B1RawFieldObservation{B1ObservationState::Unprovable,
                                 "conflicting-effective-values",
                                 "b1-performance-configuration-v1",
                                 "",
                                 false,
                                 false};
  }
  if ((components[21U] == "absent" &&
       (components[22U] != "not-applicable" || !upper_cache_absence_proved)) ||
      (components[21U] == "disabled" && components[22U] != "none") ||
      ((components[21U] == "write-through" || components[21U] == "write-back" ||
        components[21U] == "provider-managed") &&
       components[22U] == "none")) {
    return B1RawFieldObservation{B1ObservationState::Unprovable,
                                 "evidence-chain-incomplete",
                                 "b1-performance-configuration-v1",
                                 "",
                                 false,
                                 false};
  }
  const bool network_absence_proved = has_proof("network-path-absent");
  if (components[28U] != "not-applicable" && network_absence_proved) {
    return B1RawFieldObservation{B1ObservationState::Unprovable,
                                 "conflicting-effective-values",
                                 "b1-performance-configuration-v1",
                                 "",
                                 false,
                                 false};
  }
  if (components[28U] == "not-applicable" && !network_absence_proved) {
    return B1RawFieldObservation{B1ObservationState::Unprovable,
                                 "evidence-chain-incomplete",
                                 "b1-performance-configuration-v1",
                                 "",
                                 false,
                                 false};
  }
  const bool backend_tier_absence_proved =
      has_proof("backend-performance-tier-absent");
  const bool device_profile_absence_proved =
      has_proof("device-performance-profile-absent");
  if ((components[35U] != "not-applicable" && backend_tier_absence_proved) ||
      (components[36U] != "not-applicable" && device_profile_absence_proved)) {
    return B1RawFieldObservation{B1ObservationState::Unprovable,
                                 "conflicting-effective-values",
                                 "b1-performance-configuration-v1",
                                 "",
                                 false,
                                 false};
  }
  if ((components[35U] == "not-applicable" && !backend_tier_absence_proved) ||
      (components[36U] == "not-applicable" && !device_profile_absence_proved)) {
    return B1RawFieldObservation{B1ObservationState::Unprovable,
                                 "evidence-chain-incomplete",
                                 "b1-performance-configuration-v1",
                                 "",
                                 false,
                                 false};
  }
  return B1RawFieldObservation{B1ObservationState::Known,
                               "none",
                               "b1-performance-configuration-v1",
                               payload,
                               true,
                               false};
}

B1AdaptedStorageObservation adapt_b1_storage_observation(
    const B1RawStorageObservation& raw) {
  if (raw.fields.size() != kStorageFields.size()) {
    throw std::invalid_argument(
        "Raw storage observation does not contain exactly 21 fields.");
  }
  std::vector<B1CanonicalField> fields;
  fields.reserve(kStorageFields.size());
  bool raw_mapping_complete = true;
  bool not_applicable_proofs_valid = true;
  for (const FieldSchema& schema : kStorageFields) {
    const auto iterator = raw.fields.find(std::string(schema.name));
    if (iterator == raw.fields.end() || iterator->second.type != schema.type) {
      throw std::invalid_argument(
          "Raw storage observation field/type set drifted.");
    }
    fields.push_back(
        B1CanonicalField{std::string(schema.name), iterator->second.state,
                         iterator->second.reason, iterator->second.type,
                         iterator->second.payload});
    if ((iterator->second.state == B1ObservationState::Known ||
         iterator->second.state == B1ObservationState::NotApplicable) &&
        !iterator->second.mapping_proved) {
      raw_mapping_complete = false;
    }
    if (iterator->second.state == B1ObservationState::NotApplicable &&
        !iterator->second.absence_proved) {
      not_applicable_proofs_valid = false;
    }
  }
  const std::string_view expected_backend = [&raw]() -> std::string_view {
    switch (raw.backend) {
      case B1StorageBackendKind::Filesystem:
        return "filesystem";
      case B1StorageBackendKind::NetworkFilesystem:
        return "network-filesystem";
      case B1StorageBackendKind::ObjectStore:
        return "object-store";
      case B1StorageBackendKind::MemoryStore:
        return "memory-store";
      case B1StorageBackendKind::Composite:
        return "composite";
    }
    return "";
  }();
  if (fields[5U].state == B1ObservationState::Known &&
      fields[5U].payload != expected_backend) {
    throw std::invalid_argument(
        "Raw adapter backend kind conflicts with backend_class.");
  }
  validate_fields(fields, kStorageFields);
  validate_storage_cross_fields(fields);
  return B1AdaptedStorageObservation{std::move(fields), raw_mapping_complete,
                                     not_applicable_proofs_valid};
}

std::string encode_b1_storage_environment(
    const std::vector<B1CanonicalField>& fields) {
  validate_fields(fields, kStorageFields);
  validate_storage_cross_fields(fields);
  return encode_manifest(kStorageSchema, fields);
}

std::string encode_b1_base_environment(
    const std::vector<B1CanonicalField>& fields) {
  validate_fields(fields, kBaseFields);
  validate_base_cross_fields(fields);
  return encode_manifest(kBaseSchema, fields);
}

std::string encode_b1_environment_class(
    const std::vector<B1CanonicalField>& fields) {
  validate_fields(fields, kEnvironmentClassFields);
  validate_environment_class_cross_fields(fields);
  return encode_manifest(kEnvironmentClassSchema, fields);
}

B1CanonicalManifest parse_b1_environment_manifest(std::string_view bytes) {
  if (bytes.empty() || bytes.back() != '\n' ||
      bytes.find('\r') != std::string_view::npos ||
      bytes.find('\0') != std::string_view::npos ||
      (bytes.size() >= 3U && static_cast<unsigned char>(bytes[0U]) == 0xefU &&
       static_cast<unsigned char>(bytes[1U]) == 0xbbU &&
       static_cast<unsigned char>(bytes[2U]) == 0xbfU)) {
    throw std::invalid_argument("Environment manifest envelope is invalid.");
  }
  const std::size_t header_end = bytes.find('\n');
  if (header_end == std::string_view::npos || header_end == 0U) {
    throw std::invalid_argument("Environment manifest header is invalid.");
  }
  B1CanonicalManifest manifest;
  manifest.schema = std::string(bytes.substr(0U, header_end));
  std::size_t line_start = header_end + 1U;
  while (line_start < bytes.size()) {
    const std::size_t line_end = bytes.find('\n', line_start);
    if (line_end == std::string_view::npos || line_end == line_start) {
      throw std::invalid_argument("Environment field line framing is invalid.");
    }
    const std::string_view line =
        bytes.substr(line_start, line_end - line_start);
    if (line.substr(0U, 6U) != "field=") {
      throw std::invalid_argument("Environment field prefix is invalid.");
    }
    std::size_t cursor = 6U;
    const std::string_view name = parse_frame(line, &cursor);
    const std::string_view state = parse_frame(line, &cursor);
    const std::string_view reason = parse_frame(line, &cursor);
    const std::string_view type = parse_frame(line, &cursor);
    const std::string_view payload = parse_frame(line, &cursor);
    if (cursor != line.size()) {
      throw std::invalid_argument("Environment field has trailing bytes.");
    }
    manifest.fields.push_back(B1CanonicalField{
        std::string(name), parse_state(state), std::string(reason),
        std::string(type), std::string(payload)});
    line_start = line_end + 1U;
  }
  if (manifest.schema == kStorageSchema) {
    validate_fields(manifest.fields, kStorageFields);
    validate_storage_cross_fields(manifest.fields);
  } else if (manifest.schema == kBaseSchema) {
    validate_fields(manifest.fields, kBaseFields);
    validate_base_cross_fields(manifest.fields);
  } else if (manifest.schema == kEnvironmentClassSchema) {
    validate_fields(manifest.fields, kEnvironmentClassFields);
    validate_environment_class_cross_fields(manifest.fields);
  } else {
    throw std::invalid_argument("Environment manifest schema is unknown.");
  }
  manifest.bytes = std::string(bytes);
  return manifest;
}

B1Sha256Digest digest_b1_environment_manifest(std::string_view bytes) {
  return b1_sha256(bytes);
}

B1StorageEligibility evaluate_b1_storage_eligibility(
    std::string_view storage_bytes, const B1StorageRawProof& raw) {
  B1CanonicalManifest manifest;
  try {
    manifest = parse_b1_environment_manifest(storage_bytes);
    if (manifest.schema != kStorageSchema) {
      throw std::invalid_argument("Eligibility input is not storage schema.");
    }
  } catch (const std::exception&) {
    return B1StorageEligibility{false, {"canonical-schema-invalid"}};
  }

  std::array<bool, kEligibilityReasonOrder.size()> predicates{};
  const B1CanonicalField& commit = find_field(manifest, "commit_semantics");
  predicates[1U] = commit.state == B1ObservationState::Known &&
                   !raw.commit_semantics_consistent;

  const B1CanonicalField& requested =
      find_field(manifest, "requested_durability");
  const B1CanonicalField& achieved =
      find_field(manifest, "achieved_durability");
  predicates[2U] = (requested.state == B1ObservationState::Known &&
                    requested.payload != "crash-durable") ||
                   (achieved.state == B1ObservationState::Known &&
                    achieved.payload != "crash-durable");

  const bool complete_known_path = fields_known(
      manifest, {"output_store_contract_id", "output_store_contract_generation",
                 "backend_semantics_id", "backend_semantics_generation",
                 "backend_instance_id", "durability_endpoint_identity",
                 "durability_anchor_identity", "commit_semantics"});
  predicates[3U] = complete_known_path && !raw.durability_path_consistent;

  const B1CanonicalField& mount_identity =
      find_field(manifest, "mount_identity");
  const B1CanonicalField& mount_options =
      find_field(manifest, "mount_effective_options");
  predicates[4U] = mount_identity.state == B1ObservationState::Unprovable ||
                   mount_options.state == B1ObservationState::Unprovable ||
                   ((mount_identity.state == B1ObservationState::Known ||
                     mount_options.state == B1ObservationState::Known) &&
                    !raw.mount_normalization_proved);

  const bool has_not_applicable =
      std::any_of(manifest.fields.begin(), manifest.fields.end(),
                  [](const B1CanonicalField& field) {
                    return field.state == B1ObservationState::NotApplicable;
                  });
  predicates[5U] = has_not_applicable && !raw.not_applicable_proofs_valid;

  const B1CanonicalField& performance =
      find_field(manifest, "b1_performance_configuration");
  predicates[6U] = performance.state != B1ObservationState::Known ||
                   !raw.performance_configuration_proved;
  predicates[7U] = !raw.raw_mapping_complete;

  bool capability_absent = false;
  if (mount_options.state == B1ObservationState::Known) {
    const std::vector<std::string_view> map_frames =
        parse_counted_frames(mount_options.payload, 2U);
    capability_absent = map_frames.size() >= 2U &&
                        map_frames[0U] == "access_mode" &&
                        map_frames[1U] == "read-only";
  }
  const B1CanonicalField& capabilities =
      find_field(manifest, "durability_capabilities");
  if (capabilities.state == B1ObservationState::Known) {
    const std::vector<std::string_view> present =
        parse_counted_frames(capabilities.payload, 1U);
    for (const std::string& required : kDurabilityCapabilities) {
      if (std::find(present.begin(), present.end(), required) ==
          present.end()) {
        capability_absent = true;
      }
    }
  }
  predicates[8U] = capability_absent;
  predicates[9U] =
      std::any_of(manifest.fields.begin(), manifest.fields.end(),
                  [](const B1CanonicalField& field) {
                    return field.state == B1ObservationState::Unknown ||
                           field.state == B1ObservationState::Unobserved ||
                           field.state == B1ObservationState::Unsupported ||
                           field.state == B1ObservationState::Unprovable;
                  });
  predicates[10U] = !raw.root_containment_proved;

  B1StorageEligibility result;
  for (std::size_t index = 1U; index < predicates.size(); ++index) {
    if (predicates[index]) {
      result.reasons.emplace_back(kEligibilityReasonOrder[index]);
    }
  }
  result.eligible = result.reasons.empty();
  return result;
}

bool prove_b1_root_containment(
    const std::filesystem::path& selected_root,
    const std::vector<std::filesystem::path>& destinations) {
  const std::filesystem::path root = std::filesystem::canonical(selected_root);
  if (!std::filesystem::is_directory(root)) {
    return false;
  }
  for (const std::filesystem::path& destination : destinations) {
    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(destination);
    auto root_component = root.begin();
    auto destination_component = resolved.begin();
    for (; root_component != root.end() &&
           destination_component != resolved.end();
         ++root_component, ++destination_component) {
      if (*root_component != *destination_component) {
        return false;
      }
    }
    if (root_component != root.end()) {
      return false;
    }
  }
  return true;
}

bool compatible_b1_environments(const B1EnvironmentEvidence& lhs,
                                const B1EnvironmentEvidence& rhs,
                                B1EnvironmentRelation relation) noexcept {
  try {
    const B1CanonicalManifest lhs_base =
        parse_b1_environment_manifest(lhs.base_manifest);
    const B1CanonicalManifest rhs_base =
        parse_b1_environment_manifest(rhs.base_manifest);
    if (lhs_base.schema != kBaseSchema || rhs_base.schema != kBaseSchema ||
        lhs.base_manifest != rhs.base_manifest ||
        digest_b1_environment_manifest(lhs.base_manifest) !=
            lhs.claimed_base_digest ||
        digest_b1_environment_manifest(rhs.base_manifest) !=
            rhs.claimed_base_digest ||
        lhs.claimed_base_digest != rhs.claimed_base_digest ||
        lhs.replicate_ordinal == 0U ||
        lhs.replicate_ordinal != rhs.replicate_ordinal ||
        lhs.resource_identity != rhs.resource_identity) {
      return false;
    }

    const auto valid_class = [](const B1EnvironmentEvidence& evidence) {
      const B1CanonicalManifest parsed =
          parse_b1_environment_manifest(evidence.environment_class_manifest);
      return parsed.schema == kEnvironmentClassSchema &&
             digest_b1_environment_manifest(
                 evidence.environment_class_manifest) ==
                 evidence.claimed_environment_class_digest;
    };
    if (!valid_class(lhs) || !valid_class(rhs)) {
      return false;
    }

    if (relation == B1EnvironmentRelation::M1PairedI1BaseOnly) {
      const bool identities = (lhs.workload_id == "M1-shared-v1" &&
                               rhs.workload_id == "I1-edit-storm-v1") ||
                              (rhs.workload_id == "M1-shared-v1" &&
                               lhs.workload_id == "I1-edit-storm-v1");
      return identities && lhs.run_cap == 8U && rhs.run_cap == 8U;
    }

    if (!lhs.storage_manifest.has_value() ||
        !rhs.storage_manifest.has_value() ||
        !lhs.claimed_storage_digest.has_value() ||
        !rhs.claimed_storage_digest.has_value() ||
        !lhs.storage_eligibility.has_value() ||
        !rhs.storage_eligibility.has_value() ||
        !lhs.storage_eligibility->eligible ||
        !rhs.storage_eligibility->eligible ||
        !lhs.storage_eligibility->reasons.empty() ||
        !rhs.storage_eligibility->reasons.empty() ||
        *lhs.storage_manifest != *rhs.storage_manifest ||
        digest_b1_environment_manifest(*lhs.storage_manifest) !=
            *lhs.claimed_storage_digest ||
        digest_b1_environment_manifest(*rhs.storage_manifest) !=
            *rhs.claimed_storage_digest ||
        *lhs.claimed_storage_digest != *rhs.claimed_storage_digest ||
        lhs.environment_class_manifest != rhs.environment_class_manifest ||
        lhs.claimed_environment_class_digest !=
            rhs.claimed_environment_class_digest ||
        lhs.fixture_digest != rhs.fixture_digest) {
      return false;
    }
    const B1CanonicalManifest lhs_storage =
        parse_b1_environment_manifest(*lhs.storage_manifest);
    const B1CanonicalManifest rhs_storage =
        parse_b1_environment_manifest(*rhs.storage_manifest);
    if (lhs_storage.schema != kStorageSchema ||
        rhs_storage.schema != kStorageSchema) {
      return false;
    }

    switch (relation) {
      case B1EnvironmentRelation::CandidateReference:
        return lhs.workload_id == rhs.workload_id && lhs.run_cap == rhs.run_cap;
      case B1EnvironmentRelation::CapOneCapEight:
        return lhs.workload_id == kB1WorkloadId &&
               rhs.workload_id == kB1WorkloadId &&
               ((lhs.run_cap == 1U && rhs.run_cap == 8U) ||
                (lhs.run_cap == 8U && rhs.run_cap == 1U));
      case B1EnvironmentRelation::M1PairedB1CapEight:
        return lhs.run_cap == 8U && rhs.run_cap == 8U &&
               ((lhs.workload_id == "M1-shared-v1" &&
                 rhs.workload_id == kB1WorkloadId) ||
                (rhs.workload_id == "M1-shared-v1" &&
                 lhs.workload_id == kB1WorkloadId));
      case B1EnvironmentRelation::M1PairedI1BaseOnly:
        return false;
    }
  } catch (...) {
    return false;
  }
  return false;
}

namespace {

/**
 * @brief Validates one known composite payload by its closed concrete schema.
 * @param field Exact field name.
 * @param type Exact composite type.
 * @param payload Candidate complete composite bytes.
 * @return Nothing for a valid closed payload.
 * @throws std::invalid_argument for cardinality, lexical, or relation drift.
 */
void validate_composite(std::string_view field, std::string_view type,
                        std::string_view payload) {
  if (type == "mount-map-v1") {
    validate_closed_map(
        payload,
        {{"access_mode", {"read-only", "read-write"}},
         {"atime_policy", {"strict", "relaxed", "none"}},
         {"cache_coherence",
          {"host-local", "close-to-open", "strong", "eventual"}},
         {"copy_on_write_mode", {"disabled", "enabled", "provider-managed"}},
         {"data_write_mode", {"buffered", "synchronous"}},
         {"journal_mode",
          {"none", "writeback", "ordered", "full", "provider-managed"}},
         {"metadata_write_mode", {"buffered", "synchronous"}}});
    return;
  }
  if (type == "commit-semantics-v1") {
    validate_closed_map(
        payload,
        {{"atomic_no_replace",
          {"rename-no-replace", "link-no-replace", "conditional-create",
           "provider-transaction"}},
         {"barrier",
          {"file-then-leaf-to-root", "write-through", "provider-transaction"}},
         {"copy_on_write", {"none", "filesystem", "backend"}},
         {"directory_sync",
          {"directory-fsync", "full-fsync", "write-through",
           "provider-transaction"}},
         {"file_sync",
          {"file-fsync", "full-fsync", "write-through",
           "provider-transaction"}},
         {"rename",
          {"same-namespace-atomic", "conditional-rebind",
           "provider-transaction"}}});
    return;
  }
  if (type == "token-set-v1") {
    const std::vector<std::string_view> frames =
        parse_counted_frames(payload, 1U);
    if (!std::is_sorted(frames.begin(), frames.end()) ||
        std::adjacent_find(frames.begin(), frames.end()) != frames.end()) {
      throw std::invalid_argument(
          "Environment token set order/uniqueness is invalid.");
    }
    for (const std::string_view token : frames) {
      if (std::find(kDurabilityCapabilities.begin(),
                    kDurabilityCapabilities.end(),
                    token) == kDurabilityCapabilities.end()) {
        throw std::invalid_argument(
            "Environment durability capability is out of domain.");
      }
    }
    return;
  }
  if (type == "b1-performance-configuration-v1") {
    static_cast<void>(validate_performance_payload(payload));
    return;
  }
  if (type == "ordered-text-list-v1") {
    const std::vector<std::string_view> frames =
        parse_counted_frames(payload, 1U);
    for (const std::string_view item : frames) {
      if (!valid_nfc_utf8(decode_hex(item))) {
        throw std::invalid_argument(
            "Environment ordered text list item is invalid.");
      }
    }
    return;
  }
  if (type == "cpu-record-list-v1") {
    validate_record_list(payload, 1U,
                         {"text", "text", "text", "text", "uint64", "uint64"});
    return;
  }
  if (type == "device-record-list-v1") {
    validate_record_list(
        payload, 0U, {"text", "enum", "text", "text", "identifier", "uint64"},
        true);
    return;
  }
  if (type == "contract-record-list-v1") {
    validate_record_list(payload, field == "provider_contracts" ? 1U : 0U,
                         {"identifier", "uint64", "identifier", "uint64"});
    const std::vector<std::string_view> records =
        parse_counted_frames(payload, 1U);
    for (const std::string_view record : records) {
      const std::vector<std::string_view> components =
          parse_fixed_frames(record, 4U);
      if (parse_uint64(components[1U]) == 0U ||
          parse_uint64(components[3U]) == 0U) {
        throw std::invalid_argument(
            "Environment contract generation must be positive.");
      }
    }
    return;
  }
  if (type == "resource-limits-v1") {
    const std::vector<std::string_view> components =
        parse_fixed_frames(payload, 12U);
    constexpr std::array<std::string_view, 12U> kExpected{
        {"32", "1073741824", "536870912", "65536", "268435456", "1", "67108864",
         "33554432", "1024", "16777216", "64", "268435456"}};
    if (!std::equal(components.begin(), components.end(), kExpected.begin())) {
      throw std::invalid_argument("Environment resource limits are not v1.");
    }
    return;
  }
  if (type == "metal-resource-limits-v1") {
    const std::vector<std::string_view> components =
        parse_fixed_frames(payload, 3U);
    if (components[0U] != "metal" || components[1U] != "536870912" ||
        components[2U] != "268435456") {
      throw std::invalid_argument(
          "Environment Metal resource limits are not v1.");
    }
    return;
  }
  if (type == "cache-preconditions-v1") {
    const std::vector<std::string_view> components =
        parse_fixed_frames(payload, 4U);
    if (!std::all_of(
            components.begin(), components.end(),
            [](std::string_view value) { return value == "disabled"; })) {
      throw std::invalid_argument("Environment cache preconditions drifted.");
    }
    return;
  }
  if (type == "residency-preconditions-v1") {
    const std::vector<std::string_view> components =
        parse_fixed_frames(payload, 5U);
    constexpr std::array<std::string_view, 5U> kExpected{
        {"baseline-and-current", "baseline-preview-final",
         "conditional-first-upload-then-reuse", "disabled",
         "single-process-domain"}};
    if (!std::equal(components.begin(), components.end(), kExpected.begin())) {
      throw std::invalid_argument(
          "Environment residency preconditions drifted.");
    }
    return;
  }
  if (type == "power-policy-v1") {
    const std::vector<std::string_view> components =
        parse_fixed_frames(payload, 3U);
    if (!in_domain(components[0U],
                   std::vector<std::string_view>{"external-ac", "battery"}) ||
        !in_domain(
            components[1U],
            std::vector<std::string_view>{"automatic", "balanced",
                                          "high-performance", "low-power"}) ||
        !in_domain(components[2U],
                   std::vector<std::string_view>{"inhibited", "allowed"})) {
      throw std::invalid_argument("Environment power policy is invalid.");
    }
    return;
  }
  if (type == "thermal-eligibility-v1") {
    const std::vector<std::string_view> components =
        parse_fixed_frames(payload, 2U);
    const std::vector<std::string_view> domain{"nominal", "fair", "serious",
                                               "critical"};
    if (!in_domain(components[0U], domain) ||
        !in_domain(components[1U], domain)) {
      throw std::invalid_argument("Environment thermal record is invalid.");
    }
    return;
  }
  throw std::invalid_argument("Environment composite type is unsupported.");
}

/**
 * @brief Validates fields against one exact ordered schema.
 * @tparam Size Fixed field count.
 * @param fields Candidate fields.
 * @param schema Exact name/type table.
 * @return Nothing for a canonical field sequence.
 * @throws std::invalid_argument for any schema or payload drift.
 */
template <std::size_t Size>
void validate_fields(const std::vector<B1CanonicalField>& fields,
                     const std::array<FieldSchema, Size>& schema) {
  if (fields.size() != Size) {
    throw std::invalid_argument("Environment field count drifted.");
  }
  for (std::size_t index = 0U; index < Size; ++index) {
    const B1CanonicalField& field = fields[index];
    if (field.name != schema[index].name || field.type != schema[index].type) {
      throw std::invalid_argument("Environment field name/type order drifted.");
    }
    validate_field_envelope(field);
    if (field.state != B1ObservationState::Known) {
      continue;
    }
    if (field.type == "identifier" || field.type == "uint64" ||
        field.type == "boolean" || field.type == "sha256" ||
        field.type == "text" || field.type == "enum") {
      validate_scalar(field.type, field.payload, field.name);
    } else {
      validate_composite(field.name, field.type, field.payload);
    }
  }
}

/**
 * @brief Applies cross-field constraints for one storage manifest.
 * @param fields Already individually validated 21 fields.
 * @return Nothing for coherent schema-level relations.
 * @throws std::invalid_argument for generation or backend relation drift.
 */
void validate_storage_cross_fields(
    const std::vector<B1CanonicalField>& fields) {
  if (fields[1U].state == B1ObservationState::Known &&
      parse_uint64(fields[1U].payload) == 0U) {
    throw std::invalid_argument(
        "OutputStore contract generation must be positive.");
  }
  if (fields[3U].state == B1ObservationState::Known &&
      parse_uint64(fields[3U].payload) == 0U) {
    throw std::invalid_argument(
        "Backend semantics generation must be positive.");
  }
}

/**
 * @brief Applies cross-field constraints for one base manifest.
 * @param fields Already individually validated 24 fields.
 * @return Nothing for coherent fixed relations.
 * @throws std::invalid_argument for zero worker count.
 */
void validate_base_cross_fields(const std::vector<B1CanonicalField>& fields) {
  if (fields[15U].state == B1ObservationState::Known &&
      parse_uint64(fields[15U].payload) == 0U) {
    throw std::invalid_argument(
        "Environment process worker count must be positive.");
  }
}

/**
 * @brief Applies exact applicability/digest relations for environment class.
 * @param fields Already individually validated four fields.
 * @return Nothing for required or fixed N/A encoding.
 * @throws std::invalid_argument for mixed relation state.
 */
void validate_environment_class_cross_fields(
    const std::vector<B1CanonicalField>& fields) {
  if (fields[0U].state != B1ObservationState::Known ||
      fields[1U].state != B1ObservationState::Known ||
      fields[2U].state != B1ObservationState::Known) {
    throw std::invalid_argument(
        "Environment class base/applicability/reason must be known.");
  }
  if (fields[1U].payload == "required") {
    if (fields[2U].payload != "none" ||
        fields[3U].state != B1ObservationState::Known) {
      throw std::invalid_argument(
          "Required storage environment-class relation is invalid.");
    }
  } else if (fields[1U].payload == "not-applicable") {
    if (fields[2U].payload != "row-has-no-output-commit" ||
        fields[3U].state != B1ObservationState::NotApplicable ||
        fields[3U].reason != "row-has-no-output-commit") {
      throw std::invalid_argument(
          "Storage-N/A environment-class relation is invalid.");
    }
  } else {
    throw std::invalid_argument(
        "Environment class storage applicability is invalid.");
  }
}

/**
 * @brief Encodes one prevalidated exact field sequence.
 * @param header Literal schema without LF.
 * @param fields Canonical fixed fields.
 * @return Complete header/records/final LF bytes.
 * @throws std::bad_alloc when output allocation fails.
 */
std::string encode_manifest(std::string_view header,
                            const std::vector<B1CanonicalField>& fields) {
  std::string output(header);
  output.push_back('\n');
  for (const B1CanonicalField& field : fields) {
    output.append("field=");
    output.append(b1_environment_frame(field.name));
    output.append(b1_environment_frame(state_name(field.state)));
    output.append(b1_environment_frame(field.reason));
    output.append(b1_environment_frame(field.type));
    output.append(b1_environment_frame(field.payload));
    output.push_back('\n');
  }
  return output;
}

/**
 * @brief Looks up one field by exact name in an already canonical manifest.
 * @param manifest Parsed manifest.
 * @param name Exact field name.
 * @return Borrowed field.
 * @throws std::logic_error when the closed schema lacks the field.
 */
const B1CanonicalField& find_field(const B1CanonicalManifest& manifest,
                                   std::string_view name) {
  const auto iterator = std::find_if(
      manifest.fields.begin(), manifest.fields.end(),
      [name](const B1CanonicalField& field) { return field.name == name; });
  if (iterator == manifest.fields.end()) {
    throw std::logic_error("Canonical environment field lookup failed.");
  }
  return *iterator;
}

/**
 * @brief Returns whether every named field is known.
 * @param manifest Canonical manifest.
 * @param names Required field names.
 * @return True only when all names have known state.
 * @throws std::logic_error when a closed schema field is missing.
 */
bool fields_known(const B1CanonicalManifest& manifest,
                  const std::vector<std::string_view>& names) {
  return std::all_of(
      names.begin(), names.end(), [&manifest](std::string_view name) {
        return find_field(manifest, name).state == B1ObservationState::Known;
      });
}

}  // namespace

bool B1CanonicalField::operator==(
    const B1CanonicalField& other) const noexcept {
  return name == other.name && state == other.state && reason == other.reason &&
         type == other.type && payload == other.payload;
}

std::string b1_environment_frame(std::string_view payload) {
  return std::to_string(payload.size()) + ":" + std::string(payload);
}

std::string encode_b1_token_set(std::vector<std::string> tokens,
                                const std::vector<std::string>& domain) {
  for (const std::string& token : tokens) {
    if (std::find(domain.begin(), domain.end(), token) == domain.end()) {
      throw std::invalid_argument("Environment token is outside its domain.");
    }
  }
  std::sort(tokens.begin(), tokens.end());
  if (std::adjacent_find(tokens.begin(), tokens.end()) != tokens.end()) {
    throw std::invalid_argument("Environment token set contains a duplicate.");
  }
  std::string output = std::to_string(tokens.size()) + ":";
  for (const std::string& token : tokens) {
    output.append(b1_environment_frame(token));
  }
  return output;
}

std::string encode_b1_ordered_text_list(
    const std::vector<std::string>& encoded_text_items) {
  std::string output = std::to_string(encoded_text_items.size()) + ":";
  for (const std::string& item : encoded_text_items) {
    if (!valid_nfc_utf8(decode_hex(item))) {
      throw std::invalid_argument(
          "Environment ordered text item is invalid or non-NFC.");
    }
    output.append(b1_environment_frame(item));
  }
  return output;
}

std::string encode_b1_fixed_record(const std::vector<std::string>& components) {
  std::string output;
  for (const std::string& component : components) {
    output.append(b1_environment_frame(component));
  }
  return output;
}

std::string encode_b1_map(
    const std::vector<std::pair<std::string, std::string>>& entries) {
  std::vector<std::pair<std::string, std::string>> sorted = entries;
  std::sort(sorted.begin(), sorted.end());
  for (std::size_t index = 1U; index < sorted.size(); ++index) {
    if (sorted[index - 1U].first == sorted[index].first) {
      throw std::invalid_argument("Environment map contains a duplicate key.");
    }
  }
  std::string output = std::to_string(sorted.size()) + ":";
  for (const auto& [key, value] : sorted) {
    if (key.empty() || value.empty()) {
      throw std::invalid_argument("Environment map key/value is empty.");
    }
    output.append(b1_environment_frame(key));
    output.append(b1_environment_frame(value));
  }
  return output;
}

std::string encode_b1_normalized_text(std::string_view utf8) {
  if (utf8.empty()) {
    throw std::invalid_argument("Environment text is empty.");
  }
  std::vector<std::uint8_t> bytes;
  bytes.reserve(utf8.size());
  for (const char character : utf8) {
    bytes.push_back(static_cast<std::uint8_t>(character));
  }
  if (!valid_nfc_utf8(bytes)) {
    throw std::invalid_argument(
        "Environment text is invalid UTF-8 or non-NFC.");
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output(utf8.size() * 2U, '0');
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    output[index * 2U] = kHex[bytes[index] >> 4U];
    output[index * 2U + 1U] = kHex[bytes[index] & 0x0fU];
  }
  return output;
}

namespace {

/**
 * @brief Converts ASCII bytes to lowercase under an explicit native contract.
 * @param value Candidate native key/value.
 * @return Lowercase ASCII spelling.
 * @throws std::invalid_argument when input contains non-ASCII bytes.
 * @throws std::bad_alloc when output allocation fails.
 */
std::string ascii_lower(std::string_view value) {
  std::string output;
  output.reserve(value.size());
  for (const unsigned char character : value) {
    if (character > 0x7fU) {
      throw std::invalid_argument("Native mount option is not ASCII.");
    }
    output.push_back(static_cast<char>(std::tolower(character)));
  }
  return output;
}

/**
 * @brief Returns the closed enum domain for one schema field.
 * @param field Exact schema field name.
 * @return Closed domain, empty when the field is not an enum.
 * @throws Nothing.
 */
std::vector<std::string_view> enum_domain_for_field(std::string_view field) {
  if (field == "backend_class") {
    return {"filesystem", "network-filesystem", "object-store", "memory-store",
            "composite"};
  }
  if (field == "locality") {
    return {"process-local", "host-local", "network-remote"};
  }
  if (field == "persistence") {
    return {"volatile", "host-restart-persistent", "externally-persistent"};
  }
  if (field == "requested_durability" || field == "achieved_durability") {
    return {"atomic-visible", "crash-durable"};
  }
  if (field == "storage_class") {
    return {"memory", "local-block", "remote-block", "network-filesystem",
            "object", "composite"};
  }
  if (field == "hardware_write_cache_policy") {
    return {"disabled",
            "write-through",
            "write-back-protected",
            "write-back-unprotected",
            "provider-managed-protected",
            "provider-managed-unprotected"};
  }
  if (field == "power_loss_protection_policy") {
    return {"present", "absent", "provider-guaranteed",
            "provider-not-guaranteed"};
  }
  if (field == "os_family") {
    return {"darwin", "linux", "windows"};
  }
  if (field == "architecture") {
    return {"aarch64", "x86_64"};
  }
  if (field == "compiler_id") {
    return {"apple-clang", "clang", "gcc", "msvc"};
  }
  if (field == "standard_library_id") {
    return {"libcxx", "libstdcxx", "msvc"};
  }
  if (field == "build_mode") {
    return {"debug", "release", "relwithdebinfo", "minsizerel"};
  }
  if (field == "storage_environment_applicability") {
    return {"required", "not-applicable"};
  }
  if (field == "storage_environment_not_applicable_reason") {
    return {"none", "row-has-no-output-commit"};
  }
  return {};
}

/**
 * @brief Returns the closed enum domain for one performance component index.
 * @param index Zero-based component index.
 * @return Closed domain, empty for non-enum components.
 * @throws Nothing.
 */
std::vector<std::string_view> performance_enum_domain(std::size_t index) {
  switch (index) {
    case 0U:
      return {"disabled", "enabled", "provider-managed"};
    case 4U:
      return {"none",         "host-client",     "filesystem",
              "block-device", "network-service", "provider-managed",
              "composite"};
    case 6U:
      return {"disabled", "metadata-only", "data-only", "data-and-metadata",
              "provider-managed"};
    case 8U:
      return {"disabled", "inline", "post-process", "provider-managed"};
    case 13U:
      return {"preallocated",  "on-demand",       "sparse",
              "copy-on-write", "memory-resident", "provider-managed"};
    case 14U:
      return {"thick", "thin", "elastic", "memory-resident",
              "provider-managed"};
    case 15U:
      return {"single",     "striped",       "mirrored",
              "replicated", "erasure-coded", "provider-managed"};
    case 21U:
      return {"absent", "disabled", "write-through", "write-back",
              "provider-managed"};
    case 24U:
    case 26U:
      return {"serial", "fixed", "unbounded", "provider-managed"};
    case 28U:
      return {"not-applicable", "host-loopback", "lan", "wan",
              "provider-internal"};
    default:
      return {};
  }
}

/**
 * @brief Validates one known scalar payload against an exact declared type.
 * @param type Scalar type.
 * @param payload Candidate canonical payload.
 * @param field Field name supplying any enum domain.
 * @return Nothing for a valid payload.
 * @throws std::invalid_argument for lexical or domain drift.
 */
void validate_scalar(std::string_view type, std::string_view payload,
                     std::string_view field) {
  if (type == "identifier") {
    if (!valid_identifier(payload)) {
      throw std::invalid_argument("Environment identifier payload is invalid.");
    }
    return;
  }
  if (type == "uint64") {
    static_cast<void>(parse_uint64(payload));
    return;
  }
  if (type == "boolean") {
    if (payload != "true" && payload != "false") {
      throw std::invalid_argument("Environment boolean payload is invalid.");
    }
    return;
  }
  if (type == "sha256") {
    if (!valid_lower_hex(payload, 32U)) {
      throw std::invalid_argument("Environment SHA-256 payload is invalid.");
    }
    return;
  }
  if (type == "text") {
    if (!valid_nfc_utf8(decode_hex(payload))) {
      throw std::invalid_argument("Environment text payload is invalid.");
    }
    return;
  }
  if (type == "enum") {
    const std::vector<std::string_view> domain = enum_domain_for_field(field);
    if (domain.empty() || !in_domain(payload, domain)) {
      throw std::invalid_argument("Environment enum payload is invalid.");
    }
    return;
  }
  throw std::invalid_argument("Environment scalar type is unsupported.");
}

/**
 * @brief Returns whether one permitted N/A field/reason pair is exact.
 * @param field Exact field name.
 * @param reason Candidate N/A reason.
 * @return True only for the closed table.
 * @throws Nothing.
 */
bool valid_not_applicable_pair(std::string_view field,
                               std::string_view reason) noexcept {
  return (field == "filesystem_type" && reason == "filesystem-layer-absent") ||
         ((field == "mount_identity" || field == "mount_effective_options") &&
          reason == "mount-layer-absent") ||
         (field == "hardware_write_cache_policy" &&
          reason == "hardware-write-cache-layer-absent") ||
         (field == "power_loss_protection_policy" &&
          reason == "power-loss-protection-layer-absent") ||
         (field == "metal_resource_limits" &&
          reason == "configured-metal-executor-absent") ||
         (field == "storage_environment_digest" &&
          reason == "row-has-no-output-commit");
}

/**
 * @brief Validates one field's state/reason/payload envelope.
 * @param field Candidate field with already checked name/type.
 * @return Nothing for a valid envelope.
 * @throws std::invalid_argument for a forbidden pair or payload presence.
 */
void validate_field_envelope(const B1CanonicalField& field) {
  if (field.state == B1ObservationState::Known) {
    if (field.reason != "none" || field.payload.empty()) {
      throw std::invalid_argument(
          "Known environment field requires reason none and payload.");
    }
    return;
  }
  if (!field.payload.empty()) {
    throw std::invalid_argument(
        "Non-known environment field must have an empty payload.");
  }
  switch (field.state) {
    case B1ObservationState::NotApplicable:
      if (!valid_not_applicable_pair(field.name, field.reason)) {
        throw std::invalid_argument(
            "Environment not-applicable pair is not permitted.");
      }
      return;
    case B1ObservationState::Unknown:
      if (field.reason != "probe-returned-indeterminate") {
        throw std::invalid_argument("Environment unknown reason is invalid.");
      }
      return;
    case B1ObservationState::Unobserved:
      if (field.reason != "probe-not-run" &&
          field.reason != "probe-failed-before-observation") {
        throw std::invalid_argument(
            "Environment unobserved reason is invalid.");
      }
      return;
    case B1ObservationState::Unsupported:
      if (field.reason != "probe-contract-unsupported" &&
          field.reason != "platform-capability-unsupported") {
        throw std::invalid_argument(
            "Environment unsupported reason is invalid.");
      }
      return;
    case B1ObservationState::Unprovable:
      if (field.reason != "evidence-chain-incomplete" &&
          field.reason != "conflicting-effective-values") {
        throw std::invalid_argument(
            "Environment unprovable reason is invalid.");
      }
      return;
    case B1ObservationState::Known:
      break;
  }
  throw std::invalid_argument("Environment observation state is invalid.");
}

/**
 * @brief Validates sorted unique complete record-list payloads.
 * @param payload Count-prefixed framed record list.
 * @param minimum_count Required minimum item count.
 * @param component_types Exact type sequence for each framed record.
 * @param device_class Whether component one is the device-class enum.
 * @return Nothing for canonical list/record bytes.
 * @throws std::invalid_argument for ordering, uniqueness, or record drift.
 */
void validate_record_list(std::string_view payload, std::size_t minimum_count,
                          const std::vector<std::string_view>& component_types,
                          bool device_class) {
  const std::vector<std::string_view> records =
      parse_counted_frames(payload, 1U);
  if (records.size() < minimum_count ||
      !std::is_sorted(records.begin(), records.end()) ||
      std::adjacent_find(records.begin(), records.end()) != records.end()) {
    throw std::invalid_argument(
        "Environment record list cardinality/order is invalid.");
  }
  for (const std::string_view record : records) {
    const std::vector<std::string_view> components =
        parse_fixed_frames(record, component_types.size());
    for (std::size_t index = 0U; index < components.size(); ++index) {
      if (device_class && index == 1U) {
        if (!in_domain(components[index], std::vector<std::string_view>{
                                              "gpu", "accelerator", "io"})) {
          throw std::invalid_argument("Environment device class is invalid.");
        }
      } else {
        validate_scalar(component_types[index], components[index], "");
      }
    }
  }
}

/**
 * @brief Validates one exact mount or commit map payload.
 * @param payload Count-prefixed alternating key/value frames.
 * @param expected Exact sorted key and value-domain pairs.
 * @return Nothing for a canonical complete map.
 * @throws std::invalid_argument for cardinality, order, key, or value drift.
 */
void validate_closed_map(
    std::string_view payload,
    const std::vector<
        std::pair<std::string_view, std::vector<std::string_view>>>& expected) {
  const std::vector<std::string_view> frames =
      parse_counted_frames(payload, 2U);
  if (frames.size() != expected.size() * 2U) {
    throw std::invalid_argument("Environment closed map cardinality drifted.");
  }
  for (std::size_t index = 0U; index < expected.size(); ++index) {
    if (frames[index * 2U] != expected[index].first ||
        !in_domain(frames[index * 2U + 1U], expected[index].second)) {
      throw std::invalid_argument("Environment closed map key/value drifted.");
    }
  }
}

/**
 * @brief Validates one exact 37-component performance payload.
 * @param payload Complete fixed-record bytes.
 * @return Parsed components after all lexical/cross-field checks.
 * @throws std::invalid_argument for any incomplete or inconsistent record.
 */
std::vector<std::string_view> validate_performance_payload(
    std::string_view payload) {
  const std::vector<std::string_view> values =
      parse_fixed_frames(payload, kPerformanceTypes.size());
  for (std::size_t index = 0U; index < values.size(); ++index) {
    if (kPerformanceTypes[index] == "enum") {
      const std::vector<std::string_view> domain =
          performance_enum_domain(index);
      if (!in_domain(values[index], domain)) {
        throw std::invalid_argument(
            "B1 performance enum component is invalid.");
      }
    } else {
      validate_scalar(kPerformanceTypes[index], values[index], "");
    }
  }
  const std::uint64_t compression_level = parse_uint64(values[2U]);
  if ((values[0U] == "disabled" &&
       (values[1U] != "none" || compression_level != 0U ||
        values[3U] != "none")) ||
      (values[0U] == "enabled" &&
       (values[1U] == "none" || values[1U] == "provider-managed" ||
        values[3U] == "none")) ||
      (values[0U] == "provider-managed" &&
       (values[1U] != "provider-managed" || compression_level != 0U ||
        values[3U] == "none"))) {
    throw std::invalid_argument(
        "B1 performance compression relationship is invalid.");
  }
  if ((values[4U] == "none") != (values[5U] == "none")) {
    throw std::invalid_argument(
        "B1 performance encryption relationship is invalid.");
  }
  if ((values[6U] == "disabled") != (values[7U] == "none")) {
    throw std::invalid_argument(
        "B1 performance checksum relationship is invalid.");
  }
  const std::uint64_t data = parse_uint64(values[16U]);
  const std::uint64_t parity = parse_uint64(values[17U]);
  const std::uint64_t replicas = parse_uint64(values[18U]);
  const std::uint64_t stripe = parse_uint64(values[19U]);
  const std::string_view layout = values[15U];
  const bool valid_layout =
      (layout == "single" && data == 1U && parity == 0U && replicas == 1U &&
       stripe == 0U) ||
      (layout == "striped" && data >= 2U && parity == 0U && replicas == 1U &&
       stripe > 0U) ||
      ((layout == "mirrored" || layout == "replicated") && data == 1U &&
       parity == 0U && replicas >= 2U && stripe == 0U) ||
      (layout == "erasure-coded" && data > 0U && parity > 0U &&
       replicas == 1U && stripe > 0U) ||
      (layout == "provider-managed" && values[20U] != "none" &&
       values[20U] != "provider-managed");
  if (!valid_layout) {
    throw std::invalid_argument("B1 performance layout geometry is invalid.");
  }
  const std::uint64_t queue_depth = parse_uint64(values[25U]);
  const std::uint64_t concurrency_limit = parse_uint64(values[27U]);
  const auto valid_limit = [](std::string_view policy,
                              std::uint64_t limit) noexcept {
    return (policy == "serial" && limit == 1U) ||
           (policy == "unbounded" && limit == 0U) ||
           ((policy == "fixed" || policy == "provider-managed") && limit > 0U);
  };
  if (!valid_limit(values[24U], queue_depth) ||
      !valid_limit(values[26U], concurrency_limit)) {
    throw std::invalid_argument(
        "B1 performance queue/concurrency limit is invalid.");
  }
  const std::uint64_t mtu = parse_uint64(values[31U]);
  if (values[28U] == "not-applicable") {
    if (values[29U] != "not-applicable" || values[30U] != "not-applicable" ||
        mtu != 0U || values[32U] != "not-applicable" ||
        values[33U] != "not-applicable") {
      throw std::invalid_argument(
          "B1 no-network performance relationship is invalid.");
    }
  } else if (mtu == 0U || values[29U] == "not-applicable" ||
             values[30U] == "not-applicable" ||
             values[32U] == "not-applicable" ||
             values[33U] == "not-applicable") {
    throw std::invalid_argument(
        "B1 network performance relationship is invalid.");
  }
  if (values[34U] == "none" || values[35U] == "none" || values[36U] == "none") {
    throw std::invalid_argument(
        "B1 service/tier/device profile must name a value or proved absence.");
  }
  return values;
}

}  // namespace

}  // namespace ps::benchmark
