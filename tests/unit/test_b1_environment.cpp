/**
 * @file test_b1_environment.cpp
 * @brief Verifies B1 canonical schemas, mappings, eligibility, and pairing.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "benchmark/b1_environment.hpp"  // NOLINT(build/include_subdir)
#include "support/b1_test_environment.hpp"

namespace ps::benchmark {
namespace {

/**
 * @brief Owns one unique test-only filesystem root.
 * @throws std::filesystem::filesystem_error when creation fails.
 */
class ScopedB1EnvironmentRoot final {
 public:
  /** @brief Creates a unique empty root below the system temp directory. */
  ScopedB1EnvironmentRoot() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() /
            ("photospider-b1-environment-" + std::to_string(nonce));
    if (!std::filesystem::create_directory(root_)) {
      throw std::runtime_error("failed to create B1 environment test root");
    }
  }

  /** @brief Removes only the exact test-owned root. @throws Nothing. */
  ~ScopedB1EnvironmentRoot() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  /** @brief Prevents duplicate cleanup ownership. */
  ScopedB1EnvironmentRoot(const ScopedB1EnvironmentRoot&) = delete;
  /** @brief Prevents replacement of cleanup ownership. */
  ScopedB1EnvironmentRoot& operator=(const ScopedB1EnvironmentRoot&) = delete;

  /**
   * @brief Returns the exact owned root.
   * @return Borrowed path valid through this owner.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Exact recursively removed path. */
  std::filesystem::path root_;
};

/**
 * @brief Finds one mutable field by exact name in a test fixture.
 * @param fields In/out canonical field sequence.
 * @param name Exact field name.
 * @return Mutable field reference.
 * @throws std::runtime_error when the fixture omits the requested field.
 */
B1CanonicalField& find_test_field(std::vector<B1CanonicalField>* fields,
                                  std::string_view name) {
  if (fields == nullptr) {
    throw std::runtime_error("B1 test field sequence is null");
  }
  const auto found =
      std::find_if(fields->begin(), fields->end(),
                   [name](const auto& field) { return field.name == name; });
  if (found == fields->end()) {
    throw std::runtime_error("B1 test field is missing");
  }
  return *found;
}

/**
 * @brief Encodes one deliberately ordered counted record list.
 * @param records Complete record payloads in caller-selected order.
 * @return Count followed by one frame per record.
 * @throws std::bad_alloc when output allocation fails.
 */
std::string encode_test_record_list(const std::vector<std::string>& records) {
  std::string output = std::to_string(records.size()) + ":";
  for (const std::string& record : records) {
    output.append(b1_environment_frame(record));
  }
  return output;
}

/**
 * @brief Returns a complete canonical raw eligibility proof document.
 * @return Independently replayable backend-to-receipt evidence bytes.
 * @throws Validation and allocation failures unchanged.
 */
B1StorageRawProof complete_test_storage_proof() {
  return testing::b1_test_storage_raw_proof();
}

/**
 * @brief Rebinds the final performance cut to one candidate component array.
 * @param components Exact candidate pre-warmup observation.
 * @return Complete test proof with an equal post-replicate cut.
 * @throws std::bad_alloc when component strings are copied.
 */
B1PerformanceProofs stable_performance_proofs(
    const std::array<std::string, 37U>& components) {
  B1PerformanceProofs proofs = testing::b1_test_performance_proofs();
  proofs.final_components.assign(components.begin(), components.end());
  return proofs;
}

/**
 * @brief Proves all three fixed schemas round-trip through independent parse.
 * @throws Test fixture, validation, and framework failures unchanged.
 */
TEST(B1Environment, ExactSchemasRoundTripAndDigestCompleteBytes) {
  const B1EnvironmentEvidence environment =
      testing::make_b1_test_environment(8U, 2U);
  const B1CanonicalManifest base =
      parse_b1_environment_manifest(environment.base_manifest);
  const B1CanonicalManifest storage =
      parse_b1_environment_manifest(*environment.storage_manifest);
  const B1CanonicalManifest environment_class =
      parse_b1_environment_manifest(environment.environment_class_manifest);
  EXPECT_EQ(base.fields.size(), 24U);
  EXPECT_EQ(storage.fields.size(), 21U);
  EXPECT_EQ(environment_class.fields.size(), 4U);
  EXPECT_EQ(base.bytes, environment.base_manifest);
  EXPECT_EQ(storage.bytes, *environment.storage_manifest);
  EXPECT_EQ(digest_b1_environment_manifest(base.bytes),
            environment.claimed_base_digest);
  EXPECT_EQ(digest_b1_environment_manifest(storage.bytes),
            *environment.claimed_storage_digest);

  std::string malformed = storage.bytes;
  malformed.pop_back();
  EXPECT_THROW(parse_b1_environment_manifest(malformed), std::invalid_argument);
  malformed = storage.bytes;
  malformed.insert(malformed.find("field=") + 6U, "0:");
  EXPECT_THROW(parse_b1_environment_manifest(malformed), std::invalid_argument);
}

/**
 * @brief Proves typed composite and text helpers reject noncanonical input.
 * @throws Test-framework failures only.
 */
TEST(B1Environment, CompositeAndTextBindingsFailClosed) {
  EXPECT_EQ(b1_environment_frame("abc"), "3:abc");
  EXPECT_EQ(encode_b1_normalized_text("ABC"), "414243");
  EXPECT_EQ(encode_b1_normalized_text("\xc3\xa9"), "c3a9");
  EXPECT_EQ(encode_b1_normalized_text("\xea\xb0\x80"), "eab080");
  EXPECT_THROW(encode_b1_normalized_text("e\xcc\x81"), std::invalid_argument);
  EXPECT_THROW(encode_b1_normalized_text("\xe1\x84\x80\xe1\x85\xa1"),
               std::invalid_argument);
  EXPECT_THROW(encode_b1_normalized_text("\xf0\x28\x8c\x28"),
               std::invalid_argument);
  EXPECT_THROW(
      encode_b1_token_set({"payload-sync", "payload-sync"}, {"payload-sync"}),
      std::invalid_argument);
  EXPECT_THROW(encode_b1_map({{"a", "1"}, {"a", "2"}}), std::invalid_argument);
  EXPECT_EQ(encode_b1_ordered_text_list({encode_b1_normalized_text("first"),
                                         encode_b1_normalized_text("first")})
                .substr(0U, 2U),
            "2:");
}

/**
 * @brief Proves every closed observation-state envelope and reason pairing.
 * @throws Test fixture, validation, and framework failures unchanged.
 */
TEST(B1Environment, ObservationStateReasonAndPayloadMatrixIsClosed) {
  struct StateReason final {
    B1ObservationState state;
    const char* reason;
  };
  const std::vector<StateReason> valid{
      {B1ObservationState::Unknown, "probe-returned-indeterminate"},
      {B1ObservationState::Unobserved, "probe-not-run"},
      {B1ObservationState::Unobserved, "probe-failed-before-observation"},
      {B1ObservationState::Unsupported, "probe-contract-unsupported"},
      {B1ObservationState::Unsupported, "platform-capability-unsupported"},
      {B1ObservationState::Unprovable, "evidence-chain-incomplete"},
      {B1ObservationState::Unprovable, "conflicting-effective-values"}};
  for (const StateReason& item : valid) {
    SCOPED_TRACE(item.reason);
    std::vector<B1CanonicalField> fields = testing::b1_test_storage_fields();
    B1CanonicalField& field = find_test_field(&fields, "filesystem_type");
    field.state = item.state;
    field.reason = item.reason;
    field.payload.clear();
    EXPECT_NO_THROW(encode_b1_storage_environment(fields));
  }

  std::vector<B1CanonicalField> fields = testing::b1_test_storage_fields();
  B1CanonicalField& cache =
      find_test_field(&fields, "hardware_write_cache_policy");
  cache.state = B1ObservationState::NotApplicable;
  cache.reason = "hardware-write-cache-layer-absent";
  cache.payload.clear();
  EXPECT_NO_THROW(encode_b1_storage_environment(fields));

  fields = testing::b1_test_storage_fields();
  B1CanonicalField& known = find_test_field(&fields, "filesystem_type");
  known.reason = "probe-not-run";
  EXPECT_THROW(encode_b1_storage_environment(fields), std::invalid_argument);
  known.reason = "none";
  known.payload.clear();
  EXPECT_THROW(encode_b1_storage_environment(fields), std::invalid_argument);

  fields = testing::b1_test_storage_fields();
  B1CanonicalField& non_known = find_test_field(&fields, "filesystem_type");
  non_known.state = B1ObservationState::Unobserved;
  non_known.reason = "probe-not-run";
  EXPECT_THROW(encode_b1_storage_environment(fields), std::invalid_argument);
  non_known.payload.clear();
  non_known.reason = "none";
  EXPECT_THROW(encode_b1_storage_environment(fields), std::invalid_argument);
  non_known.state = static_cast<B1ObservationState>(255U);
  EXPECT_THROW(encode_b1_storage_environment(fields), std::invalid_argument);

  fields = testing::b1_test_storage_fields();
  B1CanonicalField& forbidden_na = find_test_field(&fields, "filesystem_type");
  forbidden_na.state = B1ObservationState::NotApplicable;
  forbidden_na.reason = "mount-layer-absent";
  forbidden_na.payload.clear();
  EXPECT_THROW(encode_b1_storage_environment(fields), std::invalid_argument);
}

/**
 * @brief Locks scalar lexical boundaries and exact durability-set bytes.
 * @throws Test fixture, validation, and framework failures unchanged.
 */
TEST(B1Environment, ScalarAndDurabilitySetByteOraclesAreExact) {
  const std::vector<std::string> accepted_uint64{
      "1", "2", "8", "9", "10", "23", "18446744073709551615"};
  for (const std::string& value : accepted_uint64) {
    SCOPED_TRACE(value);
    std::vector<B1CanonicalField> fields = testing::b1_test_storage_fields();
    find_test_field(&fields, "output_store_contract_generation").payload =
        value;
    EXPECT_NO_THROW(encode_b1_storage_environment(fields));
  }
  EXPECT_EQ(testing::b1_test_performance_components()[2U], "0");
  EXPECT_EQ(testing::b1_test_performance_observation().state,
            B1ObservationState::Known);

  for (const std::string& value :
       std::vector<std::string>{"00", "01", "18446744073709551616"}) {
    SCOPED_TRACE(value);
    std::vector<B1CanonicalField> fields = testing::b1_test_storage_fields();
    find_test_field(&fields, "output_store_contract_generation").payload =
        value;
    EXPECT_THROW(encode_b1_storage_environment(fields), std::invalid_argument);
  }

  std::vector<B1CanonicalField> fields = testing::b1_test_storage_fields();
  find_test_field(&fields, "filesystem_type").payload = "Uppercase";
  EXPECT_THROW(encode_b1_storage_environment(fields), std::invalid_argument);

  const std::vector<std::string> capabilities{"atomic-no-replace",
                                              "atomic-visible",
                                              "crash-durable",
                                              "idempotent-reconcile",
                                              "manifest-last",
                                              "manifest-sync",
                                              "namespace-durability-barrier",
                                              "payload-sync"};
  const std::string encoded = encode_b1_token_set(capabilities, capabilities);
  EXPECT_EQ(encoded,
            "8:17:atomic-no-replace14:atomic-visible13:crash-durable"
            "20:idempotent-reconcile13:manifest-last13:manifest-sync"
            "28:namespace-durability-barrier12:payload-sync");
  EXPECT_EQ(encoded.size(), 156U);

  fields = testing::b1_test_storage_fields();
  const std::string manifest = encode_b1_storage_environment(fields);
  const std::size_t field_start =
      manifest.find("field=23:durability_capabilities");
  ASSERT_NE(field_start, std::string::npos);
  const std::size_t field_end = manifest.find('\n', field_start);
  ASSERT_NE(field_end, std::string::npos);
  EXPECT_EQ(field_end + 1U - field_start, 221U);
}

/**
 * @brief Proves collection cardinality, sort, duplicate, and shape rules.
 * @throws Test fixture, validation, and framework failures unchanged.
 */
TEST(B1Environment, CollectionAndFixedRecordBindingsRejectDrift) {
  const std::string cpu_a = encode_b1_fixed_record(
      {encode_b1_normalized_text("cpu-a"), encode_b1_normalized_text("vendor"),
       encode_b1_normalized_text("model"),
       encode_b1_normalized_text("features"), "1", "2"});
  const std::string cpu_b = encode_b1_fixed_record(
      {encode_b1_normalized_text("cpu-b"), encode_b1_normalized_text("vendor"),
       encode_b1_normalized_text("model"),
       encode_b1_normalized_text("features"), "1", "2"});
  std::vector<B1CanonicalField> base = testing::b1_test_base_fields();
  find_test_field(&base, "cpu_inventory").payload = "0:";
  EXPECT_THROW(encode_b1_base_environment(base), std::invalid_argument);
  find_test_field(&base, "cpu_inventory").payload =
      encode_test_record_list({cpu_a, cpu_a});
  EXPECT_THROW(encode_b1_base_environment(base), std::invalid_argument);
  find_test_field(&base, "cpu_inventory").payload =
      encode_test_record_list({cpu_b, cpu_a});
  EXPECT_THROW(encode_b1_base_environment(base), std::invalid_argument);
  find_test_field(&base, "cpu_inventory").payload =
      encode_test_record_list({cpu_a, cpu_b});
  EXPECT_NO_THROW(encode_b1_base_environment(base));

  base = testing::b1_test_base_fields();
  EXPECT_EQ(find_test_field(&base, "gpu_inventory").payload, "0:");
  EXPECT_EQ(find_test_field(&base, "other_device_inventory").payload, "0:");
  EXPECT_EQ(find_test_field(&base, "plugin_contracts").payload, "0:");
  find_test_field(&base, "provider_contracts").payload = "0:";
  EXPECT_THROW(encode_b1_base_environment(base), std::invalid_argument);

  const std::string invalid_device = encode_b1_fixed_record(
      {encode_b1_normalized_text("device"), "cpu",
       encode_b1_normalized_text("vendor"), encode_b1_normalized_text("model"),
       "driver", "1"});
  base = testing::b1_test_base_fields();
  find_test_field(&base, "gpu_inventory").payload =
      encode_test_record_list({invalid_device});
  EXPECT_THROW(encode_b1_base_environment(base), std::invalid_argument);

  const std::string zero_contract =
      encode_b1_fixed_record({"provider", "0", "operation-api", "1"});
  base = testing::b1_test_base_fields();
  find_test_field(&base, "provider_contracts").payload =
      encode_test_record_list({zero_contract});
  EXPECT_THROW(encode_b1_base_environment(base), std::invalid_argument);

  std::vector<B1CanonicalField> storage = testing::b1_test_storage_fields();
  B1CanonicalField& mount =
      find_test_field(&storage, "mount_effective_options");
  mount.payload.replace(0U, 2U, "6:");
  EXPECT_THROW(encode_b1_storage_environment(storage), std::invalid_argument);
  storage = testing::b1_test_storage_fields();
  B1CanonicalField& commit = find_test_field(&storage, "commit_semantics");
  commit.payload.replace(0U, 2U, "5:");
  EXPECT_THROW(encode_b1_storage_environment(storage), std::invalid_argument);

  base = testing::b1_test_base_fields();
  B1CanonicalField& resources = find_test_field(&base, "resource_limits");
  resources.payload.pop_back();
  EXPECT_THROW(encode_b1_base_environment(base), std::invalid_argument);

  base = testing::b1_test_base_fields();
  find_test_field(&base, "build_flags").payload = "0:";
  EXPECT_NO_THROW(encode_b1_base_environment(base));
  const B1CanonicalField& metal =
      find_test_field(&base, "metal_resource_limits");
  EXPECT_EQ(metal.state, B1ObservationState::NotApplicable);
  EXPECT_TRUE(metal.payload.empty());
  EXPECT_NE(find_test_field(&base, "build_flags").payload, metal.payload);
}

/**
 * @brief Proves mount defaults, case, duplicates, and unknowns require proof.
 * @throws Test-framework failures only.
 */
TEST(B1Environment, MountNormalizationUsesProvedEffectiveRules) {
  B1MountRawObservation input;
  input.options = {{"ACCESS_MODE", "READ-WRITE"},
                   {"ATIME_POLICY", "STRICT"},
                   {"ATIME_POLICY", "NONE"}};
  input.defaults = {
      {"access_mode", "read-only"},          {"atime_policy", "relaxed"},
      {"cache_coherence", "host-local"},     {"copy_on_write_mode", "disabled"},
      {"data_write_mode", "synchronous"},    {"journal_mode", "ordered"},
      {"metadata_write_mode", "synchronous"}};
  input.case_mode = B1MountCaseMode::AsciiCaseInsensitive;
  input.duplicate_policy = B1MountDuplicatePolicy::LastWins;
  input.observation_identity = "mount-observation";
  const B1RawFieldObservation normalized = normalize_b1_mount_options(input);
  EXPECT_EQ(normalized.state, B1ObservationState::Known);
  EXPECT_EQ(normalized.raw_payload, normalized.payload);
  EXPECT_EQ(normalized.proof_identity, input.observation_identity);

  B1MountRawObservation explicit_defaults = input;
  explicit_defaults.options = {{"metadata_write_mode", "synchronous"},
                               {"journal_mode", "ordered"},
                               {"data_write_mode", "synchronous"},
                               {"copy_on_write_mode", "disabled"},
                               {"cache_coherence", "host-local"},
                               {"atime_policy", "none"},
                               {"access_mode", "read-write"}};
  EXPECT_EQ(normalize_b1_mount_options(explicit_defaults).payload,
            normalized.payload);

  B1MountRawObservation repeated_same = input;
  repeated_same.options = {{"ACCESS_MODE", "READ-WRITE"},
                           {"ACCESS_MODE", "READ-WRITE"},
                           {"ATIME_POLICY", "NONE"}};
  repeated_same.duplicate_policy = B1MountDuplicatePolicy::RejectConflicts;
  EXPECT_EQ(normalize_b1_mount_options(repeated_same).payload,
            normalized.payload);

  input.duplicate_policy = B1MountDuplicatePolicy::RejectConflicts;
  EXPECT_EQ(normalize_b1_mount_options(input).state,
            B1ObservationState::Unprovable);
  input.duplicate_policy = B1MountDuplicatePolicy::LastWins;
  input.options.push_back({"unknown", "value"});
  EXPECT_EQ(normalize_b1_mount_options(input).state,
            B1ObservationState::Unprovable);
  input.excluded_options.push_back(
      B1ExcludedMountOptionProof{{"unknown", "value"},
                                 "unknown-option-no-effect"});
  EXPECT_EQ(normalize_b1_mount_options(input).payload, normalized.payload);

  input.options = {{"ACCESS_MODE", "READ-WRITE"}, {"bad\xc3\xa9", "value"}};
  EXPECT_THROW(normalize_b1_mount_options(input), std::invalid_argument);
}

/**
 * @brief Proves the 37-component mapper requires stable absence proofs.
 * @throws Test fixture, validation, and framework failures unchanged.
 */
TEST(B1Environment, PerformanceMappingRequiresEveryZeroAndAbsenceProof) {
  const B1RawFieldObservation valid =
      testing::b1_test_performance_observation();
  EXPECT_EQ(valid.state, B1ObservationState::Known);
  EXPECT_EQ(valid.raw_payload, valid.payload);

  const std::array<std::string, 37U> components =
      testing::b1_test_performance_components();
  B1PerformanceProofs proofs = testing::b1_test_performance_proofs();
  const std::vector<std::string> required_proofs = proofs.proof_kinds;
  for (const std::string& missing : required_proofs) {
    SCOPED_TRACE(missing);
    B1PerformanceProofs incomplete = proofs;
    incomplete.proof_kinds.erase(std::find(
        incomplete.proof_kinds.begin(), incomplete.proof_kinds.end(), missing));
    EXPECT_EQ(map_b1_performance_configuration(components, incomplete).state,
              B1ObservationState::Unprovable);
  }

  B1PerformanceProofs incomplete = proofs;
  incomplete.initial_observation_identity.clear();
  EXPECT_EQ(map_b1_performance_configuration(components, incomplete).state,
            B1ObservationState::Unprovable);
  incomplete = proofs;
  incomplete.mapped_option_proof_identities.clear();
  EXPECT_EQ(map_b1_performance_configuration(components, incomplete).state,
            B1ObservationState::Unprovable);
  incomplete = proofs;
  incomplete.final_components.back() = "drifted";
  EXPECT_EQ(map_b1_performance_configuration(components, incomplete).state,
            B1ObservationState::Unprovable);
  incomplete = proofs;
  incomplete.conflicting_components = {"journal-mode"};
  const B1RawFieldObservation conflicting =
      map_b1_performance_configuration(components, incomplete);
  EXPECT_EQ(conflicting.state, B1ObservationState::Unprovable);
  EXPECT_EQ(conflicting.reason, "conflicting-effective-values");

  std::array<std::string, 37U> enabled_compression = components;
  enabled_compression[0U] = "enabled";
  enabled_compression[1U] = "zstd";
  enabled_compression[2U] = "3";
  enabled_compression[3U] = "btrfs-zstd-3";
  const B1RawFieldObservation enabled = map_b1_performance_configuration(
      enabled_compression, stable_performance_proofs(enabled_compression));
  EXPECT_EQ(enabled.state, B1ObservationState::Known);
  EXPECT_NE(enabled.payload, valid.payload);
  enabled_compression[3U] = "none";
  EXPECT_THROW(
      map_b1_performance_configuration(
          enabled_compression, stable_performance_proofs(enabled_compression)),
      std::invalid_argument);

  std::array<std::string, 37U> provider = components;
  provider[15U] = "provider-managed";
  provider[16U] = "2";
  provider[17U] = "1";
  provider[18U] = "1";
  provider[19U] = "4096";
  provider[20U] = "provider-layout-a";
  EXPECT_EQ(map_b1_performance_configuration(
                provider, stable_performance_proofs(provider))
                .state,
            B1ObservationState::Known);
  const std::array<std::pair<std::size_t, const char*>, 4U> geometry{{
      {16U, "provider-layout-data-units-absent"},
      {17U, "provider-layout-parity-units-absent"},
      {18U, "provider-layout-replica-count-absent"},
      {19U, "provider-layout-stripe-unit-absent"},
  }};
  for (const auto& [index, proof_kind] : geometry) {
    SCOPED_TRACE(proof_kind);
    std::array<std::string, 37U> absent = provider;
    absent[index] = "0";
    B1PerformanceProofs absent_without_proof =
        stable_performance_proofs(absent);
    EXPECT_EQ(
        map_b1_performance_configuration(absent, absent_without_proof).state,
        B1ObservationState::Unprovable);
    B1PerformanceProofs proved_absent = absent_without_proof;
    proved_absent.proof_kinds.emplace_back(proof_kind);
    EXPECT_EQ(map_b1_performance_configuration(absent, proved_absent).state,
              B1ObservationState::Known);
    B1PerformanceProofs contradictory = stable_performance_proofs(provider);
    contradictory.proof_kinds.emplace_back(proof_kind);
    EXPECT_EQ(map_b1_performance_configuration(provider, contradictory).reason,
              "conflicting-effective-values");
  }

  for (const auto& [component_index, proof_kind] :
       std::vector<std::pair<std::size_t, std::string>>{
           {35U, "backend-performance-tier-absent"},
           {36U, "device-performance-profile-absent"}}) {
    SCOPED_TRACE(proof_kind);
    std::array<std::string, 37U> absent = components;
    absent[component_index] = "not-applicable";
    B1PerformanceProofs absent_without_proof =
        stable_performance_proofs(absent);
    EXPECT_EQ(
        map_b1_performance_configuration(absent, absent_without_proof).state,
        B1ObservationState::Unprovable);
    B1PerformanceProofs proved_absent = absent_without_proof;
    proved_absent.proof_kinds.push_back(proof_kind);
    EXPECT_EQ(map_b1_performance_configuration(absent, proved_absent).state,
              B1ObservationState::Known);
  }

  std::array<std::string, 37U> invalid = components;
  const std::array<std::size_t, 11U> enum_indices{0U,  4U,  6U,  8U,  13U, 14U,
                                                  15U, 21U, 24U, 26U, 28U};
  for (const std::size_t index : enum_indices) {
    SCOPED_TRACE(index);
    invalid = components;
    invalid[index] = "invalid";
    EXPECT_THROW(map_b1_performance_configuration(invalid, proofs),
                 std::invalid_argument);
  }
}

/**
 * @brief Proves performance cross-component and contradiction rules.
 * @throws Test fixture, validation, and framework failures unchanged.
 */
TEST(B1Environment, PerformanceCrossComponentMatrixFailsClosed) {
  const std::array<std::string, 37U> valid =
      testing::b1_test_performance_components();
  const B1PerformanceProofs proofs = testing::b1_test_performance_proofs();

  std::array<std::string, 37U> candidate = valid;
  candidate[4U] = "filesystem";
  EXPECT_THROW(map_b1_performance_configuration(candidate, proofs),
               std::invalid_argument);
  candidate = valid;
  candidate[5U] = "aes-xts";
  EXPECT_THROW(map_b1_performance_configuration(candidate, proofs),
               std::invalid_argument);
  candidate = valid;
  candidate[6U] = "data-and-metadata";
  EXPECT_THROW(map_b1_performance_configuration(candidate, proofs),
               std::invalid_argument);
  candidate = valid;
  candidate[7U] = "sha256";
  EXPECT_THROW(map_b1_performance_configuration(candidate, proofs),
               std::invalid_argument);

  const std::vector<std::array<std::string, 5U>> layouts{
      {{"striped", "2", "0", "1", "4096"}},
      {{"mirrored", "1", "0", "2", "0"}},
      {{"replicated", "1", "0", "3", "0"}},
      {{"erasure-coded", "2", "1", "1", "4096"}}};
  for (const auto& layout : layouts) {
    SCOPED_TRACE(layout[0U]);
    candidate = valid;
    std::copy(layout.begin(), layout.end(), candidate.begin() + 15U);
    EXPECT_EQ(map_b1_performance_configuration(
                  candidate, stable_performance_proofs(candidate))
                  .state,
              B1ObservationState::Known);
  }
  candidate = valid;
  candidate[16U] = "2";
  EXPECT_THROW(map_b1_performance_configuration(candidate, proofs),
               std::invalid_argument);

  candidate = valid;
  candidate[24U] = "fixed";
  candidate[25U] = "0";
  EXPECT_THROW(map_b1_performance_configuration(candidate, proofs),
               std::invalid_argument);
  candidate = valid;
  candidate[26U] = "unbounded";
  candidate[27U] = "1";
  EXPECT_THROW(map_b1_performance_configuration(candidate, proofs),
               std::invalid_argument);

  candidate = valid;
  candidate[28U] = "lan";
  candidate[29U] = "tcp";
  candidate[30U] = "ethernet-10g";
  candidate[31U] = "1500";
  candidate[32U] = "default";
  candidate[33U] = "local";
  B1PerformanceProofs network_proofs = stable_performance_proofs(candidate);
  network_proofs.proof_kinds.erase(std::find(network_proofs.proof_kinds.begin(),
                                             network_proofs.proof_kinds.end(),
                                             "network-path-absent"));
  EXPECT_EQ(map_b1_performance_configuration(candidate, network_proofs).state,
            B1ObservationState::Known);
  EXPECT_EQ(map_b1_performance_configuration(
                candidate, stable_performance_proofs(candidate))
                .reason,
            "conflicting-effective-values");
  candidate[31U] = "0";
  network_proofs.final_components.assign(candidate.begin(), candidate.end());
  EXPECT_THROW(map_b1_performance_configuration(candidate, network_proofs),
               std::invalid_argument);

  candidate = valid;
  candidate[9U] = "4096";
  EXPECT_EQ(map_b1_performance_configuration(
                candidate, stable_performance_proofs(candidate))
                .reason,
            "conflicting-effective-values");

  candidate = valid;
  candidate[21U] = "disabled";
  candidate[22U] = "none";
  EXPECT_EQ(map_b1_performance_configuration(
                candidate, stable_performance_proofs(candidate))
                .reason,
            "conflicting-effective-values");
  B1PerformanceProofs no_upper_absence = stable_performance_proofs(candidate);
  no_upper_absence.proof_kinds.erase(std::find(
      no_upper_absence.proof_kinds.begin(), no_upper_absence.proof_kinds.end(),
      "upper-write-cache-absent"));
  EXPECT_EQ(map_b1_performance_configuration(candidate, no_upper_absence).state,
            B1ObservationState::Known);

  candidate = valid;
  candidate[35U] = "not-applicable";
  B1PerformanceProofs tier_absent = stable_performance_proofs(candidate);
  tier_absent.proof_kinds.push_back("backend-performance-tier-absent");
  EXPECT_EQ(map_b1_performance_configuration(candidate, tier_absent).state,
            B1ObservationState::Known);
  candidate[35U] = "standard";
  tier_absent.final_components.assign(candidate.begin(), candidate.end());
  EXPECT_EQ(map_b1_performance_configuration(candidate, tier_absent).reason,
            "conflicting-effective-values");

  candidate = valid;
  candidate[34U] = "none";
  EXPECT_THROW(map_b1_performance_configuration(candidate, proofs),
               std::invalid_argument);
}

/**
 * @brief Proves backend adapters bind the same exact 21-field schema.
 * @throws Test fixture, validation, and framework failures unchanged.
 */
TEST(B1Environment, RawBackendAdapterRejectsTypeAndBackendDrift) {
  const std::vector<B1CanonicalField> fields =
      testing::b1_test_storage_fields();
  B1BackendRawObservation raw = testing::b1_test_storage_raw_evidence().backend;
  const B1AdaptedStorageObservation adapted = adapt_b1_storage_observation(raw);
  EXPECT_EQ(adapted.fields, fields);

  raw.fields.at("filesystem_type").raw_payload = "drifted";
  EXPECT_THROW(adapt_b1_storage_observation(raw), std::invalid_argument);
  raw.fields.at("filesystem_type").raw_payload =
      raw.fields.at("filesystem_type").payload;

  B1RawFieldObservation& cache = raw.fields.at("hardware_write_cache_policy");
  cache.state = B1ObservationState::NotApplicable;
  cache.reason = "hardware-write-cache-layer-absent";
  cache.payload.clear();
  cache.raw_payload.clear();
  cache.proof_kind = "probe-state-observed";
  EXPECT_THROW(adapt_b1_storage_observation(raw), std::invalid_argument);
  cache.proof_kind = "complete-path-layer-absent";
  EXPECT_NO_THROW(adapt_b1_storage_observation(raw));

  raw.backend = B1StorageBackendKind::ObjectStore;
  EXPECT_THROW(adapt_b1_storage_observation(raw), std::invalid_argument);
  raw.backend = B1StorageBackendKind::Filesystem;
  raw.fields.erase("mount_identity");
  EXPECT_THROW(adapt_b1_storage_observation(raw), std::invalid_argument);
}

/**
 * @brief Proves all eligibility failures are emitted in normative order.
 * @throws Test fixture, validation, and framework failures unchanged.
 */
TEST(B1Environment, EligibilityReturnsCompleteOrderedTruthSet) {
  const std::string storage =
      encode_b1_storage_environment(testing::b1_test_storage_fields());
  const B1StorageRawProof valid = complete_test_storage_proof();
  EXPECT_TRUE(evaluate_b1_storage_eligibility(storage, valid).eligible);

  B1StorageRawProof failed;
  const B1StorageEligibility result =
      evaluate_b1_storage_eligibility(storage, failed);
  EXPECT_FALSE(result.eligible);
  EXPECT_EQ(
      result.reasons,
      (std::vector<std::string>{
          "commit-semantics-inconsistent", "durability-path-inconsistent",
          "mount-normalization-unprovable",
          "performance-configuration-unprovable",
          "raw-observation-proof-incomplete", "root-containment-unproved"}));
  EXPECT_EQ(evaluate_b1_storage_eligibility("invalid", failed).reasons,
            std::vector<std::string>{"canonical-schema-invalid"});

  std::vector<B1CanonicalField> all_failed = testing::b1_test_storage_fields();
  find_test_field(&all_failed, "requested_durability").payload =
      "atomic-visible";
  B1CanonicalField& filesystem =
      find_test_field(&all_failed, "filesystem_type");
  filesystem.state = B1ObservationState::Unsupported;
  filesystem.reason = "platform-capability-unsupported";
  filesystem.payload.clear();
  B1CanonicalField& cache =
      find_test_field(&all_failed, "hardware_write_cache_policy");
  cache.state = B1ObservationState::NotApplicable;
  cache.reason = "hardware-write-cache-layer-absent";
  cache.payload.clear();
  const std::vector<std::pair<std::string, std::string>> read_only_mount{
      {"access_mode", "read-only"},          {"atime_policy", "none"},
      {"cache_coherence", "host-local"},     {"copy_on_write_mode", "disabled"},
      {"data_write_mode", "synchronous"},    {"journal_mode", "ordered"},
      {"metadata_write_mode", "synchronous"}};
  find_test_field(&all_failed, "mount_effective_options").payload =
      encode_b1_map(read_only_mount);
  const std::vector<std::string> incomplete_capabilities{
      "atomic-no-replace",           "atomic-visible", "crash-durable",
      "idempotent-reconcile",        "manifest-last",  "manifest-sync",
      "namespace-durability-barrier"};
  const std::vector<std::string> complete_capabilities{
      "atomic-no-replace",
      "atomic-visible",
      "crash-durable",
      "idempotent-reconcile",
      "manifest-last",
      "manifest-sync",
      "namespace-durability-barrier",
      "payload-sync"};
  find_test_field(&all_failed, "durability_capabilities").payload =
      encode_b1_token_set(incomplete_capabilities, complete_capabilities);
  const B1StorageEligibility complete_failure = evaluate_b1_storage_eligibility(
      encode_b1_storage_environment(all_failed), B1StorageRawProof{});
  EXPECT_FALSE(complete_failure.eligible);
  EXPECT_EQ(
      complete_failure.reasons,
      (std::vector<std::string>{
          "commit-semantics-inconsistent", "durability-class-not-crash-durable",
          "durability-path-inconsistent", "mount-normalization-unprovable",
          "not-applicable-proof-invalid",
          "performance-configuration-unprovable",
          "raw-observation-proof-incomplete", "required-capability-absent",
          "required-observation-ineligible", "root-containment-unproved"}));
}

/**
 * @brief Proves canonical raw bytes replay independently and every missing,
 * stale, unknown, duplicate, or cross-observation drift fails closed.
 * @throws Test fixture, validation, allocation, and framework failures.
 */
TEST(B1Environment, CanonicalRawProofReplayRejectsEveryEvidenceBoundaryDrift) {
  const std::vector<B1CanonicalField> fields =
      testing::b1_test_storage_fields();
  const std::string storage = encode_b1_storage_environment(fields);
  const B1StorageRawEvidence complete = testing::b1_test_storage_raw_evidence();
  const std::string proof_bytes = encode_b1_storage_raw_proof(complete);
  EXPECT_EQ(
      encode_b1_storage_raw_proof(parse_b1_storage_raw_proof(proof_bytes)),
      proof_bytes);
  EXPECT_TRUE(
      evaluate_b1_storage_eligibility(storage, B1StorageRawProof{proof_bytes})
          .eligible);

  B1StorageRawEvidence drift = complete;
  drift.backend.fields.at("filesystem_type").raw_payload = "different-raw";
  const B1StorageEligibility raw_payload_drift =
      evaluate_b1_storage_eligibility(
          storage, B1StorageRawProof{encode_b1_storage_raw_proof(drift)});
  EXPECT_NE(std::find(raw_payload_drift.reasons.begin(),
                      raw_payload_drift.reasons.end(),
                      "raw-observation-proof-incomplete"),
            raw_payload_drift.reasons.end());

  drift = complete;
  drift.mount.options.front().value = "read-only";
  const B1StorageEligibility mount_drift = evaluate_b1_storage_eligibility(
      storage, B1StorageRawProof{encode_b1_storage_raw_proof(drift)});
  EXPECT_NE(std::find(mount_drift.reasons.begin(), mount_drift.reasons.end(),
                      "mount-normalization-unprovable"),
            mount_drift.reasons.end());

  drift = complete;
  drift.performance_proofs.final_components.back() = "stale-device";
  const B1StorageEligibility performance_drift =
      evaluate_b1_storage_eligibility(
          storage, B1StorageRawProof{encode_b1_storage_raw_proof(drift)});
  EXPECT_NE(std::find(performance_drift.reasons.begin(),
                      performance_drift.reasons.end(),
                      "performance-configuration-unprovable"),
            performance_drift.reasons.end());

  drift = complete;
  drift.transaction.events.pop_back();
  const B1StorageEligibility missing_event = evaluate_b1_storage_eligibility(
      storage, B1StorageRawProof{encode_b1_storage_raw_proof(drift)});
  EXPECT_NE(
      std::find(missing_event.reasons.begin(), missing_event.reasons.end(),
                "commit-semantics-inconsistent"),
      missing_event.reasons.end());

  drift = complete;
  drift.transaction.events.push_back(drift.transaction.events.front());
  EXPECT_THROW(encode_b1_storage_raw_proof(drift), std::invalid_argument);
  drift = complete;
  drift.transaction.events.front().kind = "unknown-event";
  EXPECT_THROW(encode_b1_storage_raw_proof(drift), std::invalid_argument);

  std::string unknown_field = proof_bytes;
  const std::string known_name = "backend_observation";
  const std::size_t known_position = unknown_field.find(known_name);
  ASSERT_NE(known_position, std::string::npos);
  unknown_field.replace(known_position, known_name.size(),
                        "backend_observatiox");
  EXPECT_THROW(parse_b1_storage_raw_proof(unknown_field),
               std::invalid_argument);

  const std::size_t first_field = proof_bytes.find('\n') + 1U;
  const std::size_t first_field_end = proof_bytes.find('\n', first_field);
  ASSERT_NE(first_field_end, std::string::npos);
  const std::string first_field_line =
      proof_bytes.substr(first_field, first_field_end - first_field + 1U);
  std::string duplicated_field = proof_bytes;
  duplicated_field.insert(first_field_end + 1U, first_field_line);
  EXPECT_THROW(parse_b1_storage_raw_proof(duplicated_field),
               std::invalid_argument);
  std::string missing_field = proof_bytes;
  missing_field.erase(first_field, first_field_line.size());
  EXPECT_THROW(parse_b1_storage_raw_proof(missing_field),
               std::invalid_argument);

  std::vector<B1CanonicalField> valid_drifted_fields = fields;
  find_test_field(&valid_drifted_fields, "storage_class").payload =
      "remote-block";
  const std::string valid_drifted_storage =
      encode_b1_storage_environment(valid_drifted_fields);
  EXPECT_NE(digest_b1_environment_manifest(valid_drifted_storage),
            digest_b1_environment_manifest(storage));
  EXPECT_EQ(evaluate_b1_storage_eligibility(valid_drifted_storage,
                                            B1StorageRawProof{proof_bytes})
                .reasons,
            std::vector<std::string>{"raw-observation-proof-incomplete"});
}

/**
 * @brief Proves containment and exact cap/reference compatibility relations.
 * @throws Test fixture, filesystem, validation, and framework failures.
 */
TEST(B1Environment, ContainmentAndCompatibilityUseExactBytesAndRelations) {
  ScopedB1EnvironmentRoot temp;
  const std::filesystem::path child = temp.root() / "child";
  ASSERT_TRUE(std::filesystem::create_directory(child));
  EXPECT_TRUE(prove_b1_root_containment(temp.root(), {child}));
  EXPECT_FALSE(prove_b1_root_containment(
      child, {std::filesystem::temp_directory_path()}));

  B1EnvironmentEvidence cap_one = testing::make_b1_test_environment(1U, 1U);
  B1EnvironmentEvidence cap_eight = testing::make_b1_test_environment(8U, 1U);
  EXPECT_TRUE(compatible_b1_environments(
      cap_one, cap_eight, B1EnvironmentRelation::CapOneCapEight));
  EXPECT_TRUE(compatible_b1_environments(
      cap_one, cap_one, B1EnvironmentRelation::CandidateReference));

  B1EnvironmentEvidence drift = cap_one;
  drift.replicate_ordinal = 2U;
  EXPECT_FALSE(compatible_b1_environments(
      cap_one, drift, B1EnvironmentRelation::CandidateReference));
  drift = cap_one;
  drift.resource_identity = b1_sha256("different-resource");
  EXPECT_FALSE(compatible_b1_environments(
      cap_one, drift, B1EnvironmentRelation::CandidateReference));
  drift = cap_one;
  drift.fixture_digest = b1_sha256("different-fixture");
  EXPECT_FALSE(compatible_b1_environments(
      cap_one, drift, B1EnvironmentRelation::CandidateReference));
  drift = cap_one;
  drift.storage_eligibility->eligible = false;
  drift.storage_eligibility->reasons = {"root-containment-unproved"};
  EXPECT_FALSE(compatible_b1_environments(
      cap_one, drift, B1EnvironmentRelation::CandidateReference));

  drift = cap_one;
  drift.storage_raw_proof.reset();
  EXPECT_FALSE(compatible_b1_environments(
      cap_one, drift, B1EnvironmentRelation::CandidateReference));

  drift = cap_one;
  B1StorageRawEvidence drifted_raw =
      parse_b1_storage_raw_proof(drift.storage_raw_proof->canonical_bytes);
  drifted_raw.transaction.durability_anchor_payload =
      encode_b1_normalized_text("different-anchor");
  drift.storage_raw_proof->canonical_bytes =
      encode_b1_storage_raw_proof(drifted_raw);
  EXPECT_FALSE(compatible_b1_environments(
      cap_one, drift, B1EnvironmentRelation::CandidateReference));

  drift = cap_one;
  std::vector<B1CanonicalField> weak_storage =
      parse_b1_environment_manifest(*drift.storage_manifest).fields;
  find_test_field(&weak_storage, "requested_durability").payload =
      "atomic-visible";
  find_test_field(&weak_storage, "achieved_durability").payload =
      "atomic-visible";
  drift.storage_manifest = encode_b1_storage_environment(weak_storage);
  drift.claimed_storage_digest =
      digest_b1_environment_manifest(*drift.storage_manifest);
  B1CanonicalManifest weak_class =
      parse_b1_environment_manifest(drift.environment_class_manifest);
  find_test_field(&weak_class.fields, "storage_environment_digest").payload =
      b1_digest_hex(*drift.claimed_storage_digest);
  drift.environment_class_manifest =
      encode_b1_environment_class(weak_class.fields);
  drift.claimed_environment_class_digest =
      digest_b1_environment_manifest(drift.environment_class_manifest);
  ASSERT_TRUE(drift.storage_eligibility->eligible);
  ASSERT_TRUE(drift.storage_eligibility->reasons.empty());
  EXPECT_FALSE(compatible_b1_environments(
      drift, drift, B1EnvironmentRelation::CandidateReference));

  drift = cap_one;
  B1CanonicalManifest drifted_class =
      parse_b1_environment_manifest(drift.environment_class_manifest);
  find_test_field(&drifted_class.fields, "base_environment_digest").payload =
      b1_digest_hex(b1_sha256("different-base-payload"));
  drift.environment_class_manifest =
      encode_b1_environment_class(drifted_class.fields);
  drift.claimed_environment_class_digest =
      digest_b1_environment_manifest(drift.environment_class_manifest);
  EXPECT_FALSE(compatible_b1_environments(
      cap_one, drift, B1EnvironmentRelation::CandidateReference));

  drift = cap_one;
  drifted_class =
      parse_b1_environment_manifest(drift.environment_class_manifest);
  find_test_field(&drifted_class.fields, "storage_environment_digest").payload =
      b1_digest_hex(b1_sha256("different-storage-payload"));
  drift.environment_class_manifest =
      encode_b1_environment_class(drifted_class.fields);
  drift.claimed_environment_class_digest =
      digest_b1_environment_manifest(drift.environment_class_manifest);
  EXPECT_FALSE(compatible_b1_environments(
      cap_one, drift, B1EnvironmentRelation::CandidateReference));

  B1EnvironmentEvidence m1 = testing::make_b1_test_environment(8U, 2U);
  B1EnvironmentEvidence paired_b1 = m1;
  m1.workload_id = "M1-shared-v1";
  EXPECT_TRUE(compatible_b1_environments(
      m1, paired_b1, B1EnvironmentRelation::M1PairedB1CapEight));

  B1EnvironmentEvidence paired_i1 = m1;
  paired_i1.workload_id = "I1-edit-storm-v1";
  paired_i1.storage_manifest.reset();
  paired_i1.claimed_storage_digest.reset();
  paired_i1.storage_raw_proof.reset();
  paired_i1.storage_eligibility.reset();
  const std::vector<B1CanonicalField> i1_class{
      testing::known_b1_field("base_environment_digest", "sha256",
                              b1_digest_hex(paired_i1.claimed_base_digest)),
      testing::known_b1_field("storage_environment_applicability", "enum",
                              "not-applicable"),
      testing::known_b1_field("storage_environment_not_applicable_reason",
                              "enum", "row-has-no-output-commit"),
      testing::not_applicable_b1_field("storage_environment_digest", "sha256",
                                       "row-has-no-output-commit")};
  paired_i1.environment_class_manifest = encode_b1_environment_class(i1_class);
  paired_i1.claimed_environment_class_digest =
      digest_b1_environment_manifest(paired_i1.environment_class_manifest);
  EXPECT_TRUE(compatible_b1_environments(
      m1, paired_i1, B1EnvironmentRelation::M1PairedI1BaseOnly));
  paired_i1.run_cap = 1U;
  EXPECT_FALSE(compatible_b1_environments(
      m1, paired_i1, B1EnvironmentRelation::M1PairedI1BaseOnly));

  cap_eight.claimed_storage_digest = b1_sha256("drift");
  EXPECT_FALSE(compatible_b1_environments(
      cap_one, cap_eight, B1EnvironmentRelation::CapOneCapEight));
}

}  // namespace
}  // namespace ps::benchmark
