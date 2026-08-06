/**
 * @file b1_test_environment.hpp
 * @brief Builds exact deterministic B1 environment fixtures for focused tests.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/b1_environment.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark::testing {

/**
 * @brief Creates one canonical known field.
 * @param name Exact schema name.
 * @param type Exact schema type.
 * @param payload Valid nonempty canonical payload.
 * @return Complete known/none field.
 * @throws std::bad_alloc when strings cannot be copied.
 */
inline B1CanonicalField known_b1_field(std::string name, std::string type,
                                       std::string payload) {
  return B1CanonicalField{std::move(name), B1ObservationState::Known, "none",
                          std::move(type), std::move(payload)};
}

/**
 * @brief Creates one exact permitted not-applicable field.
 * @param name Exact schema name.
 * @param type Exact schema type.
 * @param reason Exact field-specific absence reason.
 * @return Complete empty-payload N/A field.
 * @throws std::bad_alloc when strings cannot be copied.
 */
inline B1CanonicalField not_applicable_b1_field(std::string name,
                                                std::string type,
                                                std::string reason) {
  return B1CanonicalField{std::move(name),
                          B1ObservationState::NotApplicable,
                          std::move(reason),
                          std::move(type),
                          {}};
}

/**
 * @brief Encodes a sorted fixed-record list for one test manifest.
 * @param records Complete already canonical fixed-record payloads.
 * @return Count followed by one frame per sorted record.
 * @throws std::bad_alloc when staging/output allocation fails.
 */
inline std::string b1_test_record_list(std::vector<std::string> records) {
  std::sort(records.begin(), records.end());
  std::string result = std::to_string(records.size()) + ":";
  for (const std::string& record : records) {
    result.append(b1_environment_frame(record));
  }
  return result;
}

/**
 * @brief Returns the exact valid 37-component test configuration.
 * @return Components in normative order.
 * @throws std::bad_alloc when strings cannot be allocated.
 */
inline std::array<std::string, 37U> b1_test_performance_components() {
  return {{
      "disabled",
      "none",
      "0",
      "none",
      "none",
      "none",
      "disabled",
      "none",
      "disabled",
      "0",
      "0",
      "0",
      "0",
      "preallocated",
      "thick",
      "single",
      "1",
      "0",
      "1",
      "0",
      "none",
      "absent",
      "not-applicable",
      "none",
      "serial",
      "1",
      "serial",
      "1",
      "not-applicable",
      "not-applicable",
      "not-applicable",
      "0",
      "not-applicable",
      "not-applicable",
      "local-filesystem",
      "standard",
      "generic-local-device",
  }};
}

/**
 * @brief Returns complete proof state for the valid test configuration.
 * @return Frozen, stable mapping and every required absence proof.
 * @throws std::bad_alloc when proof tokens cannot be allocated.
 */
inline B1PerformanceProofs b1_test_performance_proofs() {
  B1PerformanceProofs proofs;
  proofs.proof_kinds = {
      "allocation-unit-bytes-absent", "logical-block-bytes-absent",
      "network-path-absent",          "physical-block-bytes-absent",
      "record-bytes-absent",          "upper-write-cache-absent"};
  proofs.one_frozen_observation = true;
  proofs.complete_option_mapping = true;
  proofs.stable_through_replicate = true;
  return proofs;
}

/**
 * @brief Builds a fully proved exact 37-component performance observation.
 * @return Known canonical performance fixed record.
 * @throws Validation or allocation errors unchanged.
 */
inline B1RawFieldObservation b1_test_performance_observation() {
  return map_b1_performance_configuration(b1_test_performance_components(),
                                          b1_test_performance_proofs());
}

/**
 * @brief Builds exact all-known eligible storage fields.
 * @return Twenty-one fields in normative order.
 * @throws Validation or allocation errors unchanged.
 */
inline std::vector<B1CanonicalField> b1_test_storage_fields() {
  const std::vector<std::pair<std::string, std::string>> mount{
      {"access_mode", "read-write"},         {"atime_policy", "none"},
      {"cache_coherence", "host-local"},     {"copy_on_write_mode", "disabled"},
      {"data_write_mode", "synchronous"},    {"journal_mode", "ordered"},
      {"metadata_write_mode", "synchronous"}};
  const std::vector<std::pair<std::string, std::string>> commit{
      {"atomic_no_replace", "link-no-replace"},
      {"barrier", "file-then-leaf-to-root"},
      {"copy_on_write", "none"},
      {"directory_sync", "directory-fsync"},
      {"file_sync", "file-fsync"},
      {"rename", "same-namespace-atomic"}};
  const std::vector<std::string> capabilities{"atomic-no-replace",
                                              "atomic-visible",
                                              "crash-durable",
                                              "idempotent-reconcile",
                                              "manifest-last",
                                              "manifest-sync",
                                              "namespace-durability-barrier",
                                              "payload-sync"};
  const B1RawFieldObservation performance = b1_test_performance_observation();
  return {
      known_b1_field("output_store_contract_id", "identifier", "output-store"),
      known_b1_field("output_store_contract_generation", "uint64", "1"),
      known_b1_field("backend_semantics_id", "identifier", "posix-fs"),
      known_b1_field("backend_semantics_generation", "uint64", "1"),
      known_b1_field("backend_instance_id", "text",
                     encode_b1_normalized_text("test-root")),
      known_b1_field("backend_class", "enum", "filesystem"),
      known_b1_field("locality", "enum", "host-local"),
      known_b1_field("persistence", "enum", "host-restart-persistent"),
      known_b1_field("filesystem_type", "identifier", "testfs"),
      known_b1_field("mount_identity", "text",
                     encode_b1_normalized_text("test-mount")),
      known_b1_field("mount_effective_options", "mount-map-v1",
                     encode_b1_map(mount)),
      known_b1_field("commit_semantics", "commit-semantics-v1",
                     encode_b1_map(commit)),
      known_b1_field("durability_capabilities", "token-set-v1",
                     encode_b1_token_set(capabilities, capabilities)),
      known_b1_field("requested_durability", "enum", "crash-durable"),
      known_b1_field("achieved_durability", "enum", "crash-durable"),
      known_b1_field("durability_endpoint_identity", "text",
                     encode_b1_normalized_text("test-endpoint")),
      known_b1_field("durability_anchor_identity", "text",
                     encode_b1_normalized_text("test-anchor")),
      known_b1_field("storage_class", "enum", "local-block"),
      known_b1_field("b1_performance_configuration",
                     "b1-performance-configuration-v1", performance.payload),
      known_b1_field("hardware_write_cache_policy", "enum",
                     "write-back-protected"),
      known_b1_field("power_loss_protection_policy", "enum", "present"),
  };
}

/**
 * @brief Builds exact fixed base-environment fields for focused tests.
 * @return Twenty-four fields in normative order.
 * @throws Validation or allocation errors unchanged.
 */
inline std::vector<B1CanonicalField> b1_test_base_fields() {
  const std::string cpu_record = encode_b1_fixed_record(
      {encode_b1_normalized_text("cpu0"), encode_b1_normalized_text("vendor"),
       encode_b1_normalized_text("model"),
       encode_b1_normalized_text("features"), "1", "32"});
  const std::string provider_record =
      encode_b1_fixed_record({"opencv-provider", "1", "operation-api", "2"});
  return {
      known_b1_field("os_family", "enum", "darwin"),
      known_b1_field("os_release", "text", encode_b1_normalized_text("test")),
      known_b1_field("kernel_name", "text", encode_b1_normalized_text("xnu")),
      known_b1_field("kernel_release", "text",
                     encode_b1_normalized_text("test")),
      known_b1_field("architecture", "enum", "aarch64"),
      known_b1_field("cpu_inventory", "cpu-record-list-v1",
                     b1_test_record_list({cpu_record})),
      known_b1_field("gpu_inventory", "device-record-list-v1", "0:"),
      known_b1_field("other_device_inventory", "device-record-list-v1", "0:"),
      known_b1_field("compiler_id", "enum", "apple-clang"),
      known_b1_field("compiler_version", "text",
                     encode_b1_normalized_text("test")),
      known_b1_field("compiler_target", "text",
                     encode_b1_normalized_text("arm64-apple-darwin")),
      known_b1_field("standard_library_id", "enum", "libcxx"),
      known_b1_field("standard_library_version", "text",
                     encode_b1_normalized_text("test")),
      known_b1_field("build_mode", "enum", "relwithdebinfo"),
      known_b1_field(
          "build_flags", "ordered-text-list-v1",
          encode_b1_ordered_text_list({encode_b1_normalized_text("-O2")})),
      known_b1_field("process_worker_count", "uint64", "32"),
      known_b1_field("provider_contracts", "contract-record-list-v1",
                     b1_test_record_list({provider_record})),
      known_b1_field("plugin_contracts", "contract-record-list-v1", "0:"),
      known_b1_field(
          "resource_limits", "resource-limits-v1",
          encode_b1_fixed_record({"32", "1073741824", "536870912", "65536",
                                  "268435456", "1", "67108864", "33554432",
                                  "1024", "16777216", "64", "268435456"})),
      not_applicable_b1_field("metal_resource_limits",
                              "metal-resource-limits-v1",
                              "configured-metal-executor-absent"),
      known_b1_field("cache_preconditions", "cache-preconditions-v1",
                     encode_b1_fixed_record(
                         {"disabled", "disabled", "disabled", "disabled"})),
      known_b1_field("residency_preconditions", "residency-preconditions-v1",
                     encode_b1_fixed_record(
                         {"baseline-and-current", "baseline-preview-final",
                          "conditional-first-upload-then-reuse", "disabled",
                          "single-process-domain"})),
      known_b1_field("power_policy", "power-policy-v1",
                     encode_b1_fixed_record(
                         {"external-ac", "high-performance", "inhibited"})),
      known_b1_field("thermal_eligibility", "thermal-eligibility-v1",
                     encode_b1_fixed_record({"nominal", "nominal"})),
  };
}

/**
 * @brief Builds self-consistent eligible environment evidence for one row.
 * @param run_cap Exact isolated cap one or eight.
 * @param replicate_ordinal Exact fresh-process ordinal one through three.
 * @return Complete base/storage/class bytes, claims, and fixed identities.
 * @throws Validation or allocation errors unchanged.
 */
inline B1EnvironmentEvidence make_b1_test_environment(
    std::uint64_t run_cap, std::uint64_t replicate_ordinal) {
  const std::string base = encode_b1_base_environment(b1_test_base_fields());
  const std::string storage =
      encode_b1_storage_environment(b1_test_storage_fields());
  const B1Sha256Digest base_digest = digest_b1_environment_manifest(base);
  const B1Sha256Digest storage_digest = digest_b1_environment_manifest(storage);
  const std::vector<B1CanonicalField> environment_class{
      known_b1_field("base_environment_digest", "sha256",
                     b1_digest_hex(base_digest)),
      known_b1_field("storage_environment_applicability", "enum", "required"),
      known_b1_field("storage_environment_not_applicable_reason", "enum",
                     "none"),
      known_b1_field("storage_environment_digest", "sha256",
                     b1_digest_hex(storage_digest)),
  };
  const std::string class_bytes =
      encode_b1_environment_class(environment_class);
  B1StorageRawProof proof;
  proof.raw_mapping_complete = true;
  proof.commit_semantics_consistent = true;
  proof.durability_path_consistent = true;
  proof.mount_normalization_proved = true;
  proof.not_applicable_proofs_valid = true;
  proof.performance_configuration_proved = true;
  proof.root_containment_proved = true;
  return B1EnvironmentEvidence{
      base,
      base_digest,
      storage,
      storage_digest,
      class_bytes,
      digest_b1_environment_manifest(class_bytes),
      evaluate_b1_storage_eligibility(storage, proof),
      kB1WorkloadId,
      b1_sha256("b1-test-fixture"),
      b1_sha256("b1-test-resources"),
      run_cap,
      replicate_ordinal,
  };
}

}  // namespace ps::benchmark::testing
