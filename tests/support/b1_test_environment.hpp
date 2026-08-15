/**
 * @file b1_test_environment.hpp
 * @brief Builds exact deterministic B1 environment fixtures for focused tests.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "benchmark/b1/b1_environment.hpp"  // NOLINT(build/include_subdir)

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
  proofs.initial_observation_identity = "performance-before-warmup";
  proofs.final_observation_identity = "performance-after-replicate";
  const std::array<std::string, 37U> components =
      b1_test_performance_components();
  proofs.final_components.assign(components.begin(), components.end());
  proofs.mapped_option_proof_identities = {
      "option-access-mode",        "option-atime-policy",
      "option-cache-coherence",    "option-copy-on-write-mode",
      "option-data-write-mode",    "option-journal-mode",
      "option-metadata-write-mode"};
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
 * @brief Builds complete independently replayable raw storage evidence.
 * @return Backend, mount, performance, receipt, and containment observations.
 * @throws Validation or allocation failures unchanged.
 */
inline B1StorageRawEvidence b1_test_storage_raw_evidence() {
  const std::vector<B1CanonicalField> fields = b1_test_storage_fields();
  B1StorageRawEvidence evidence;
  evidence.backend.backend = B1StorageBackendKind::Filesystem;
  evidence.backend.selected_root = "/b1-test-root";
  evidence.backend.resolved_root = "/b1-test-root";
  for (const B1CanonicalField& field : fields) {
    const bool known = field.state == B1ObservationState::Known;
    evidence.backend.fields.emplace(
        field.name,
        B1RawFieldObservation{
            field.state, field.reason, field.type, field.payload,
            known ? field.payload : std::string{},
            known ? "raw-value-observed" : "complete-path-layer-absent",
            "field-" + field.name});
  }

  evidence.mount.options = {
      {"access_mode", "read-write"},         {"atime_policy", "none"},
      {"cache_coherence", "host-local"},     {"copy_on_write_mode", "disabled"},
      {"data_write_mode", "synchronous"},    {"journal_mode", "ordered"},
      {"metadata_write_mode", "synchronous"}};
  evidence.mount.defaults = {
      {"access_mode", "read-write"},         {"atime_policy", "none"},
      {"cache_coherence", "host-local"},     {"copy_on_write_mode", "disabled"},
      {"data_write_mode", "synchronous"},    {"journal_mode", "ordered"},
      {"metadata_write_mode", "synchronous"}};
  evidence.mount.observation_identity = "mount-observation";
  evidence.backend.fields.at("mount_effective_options").proof_identity =
      evidence.mount.observation_identity;

  evidence.performance_components = b1_test_performance_components();
  evidence.performance_proofs = b1_test_performance_proofs();
  evidence.backend.fields.at("b1_performance_configuration").proof_identity =
      evidence.performance_proofs.initial_observation_identity;

  const auto payload = [&fields](std::string_view name) -> const std::string& {
    const auto found = std::find_if(
        fields.begin(), fields.end(),
        [name](const B1CanonicalField& field) { return field.name == name; });
    if (found == fields.end()) {
      throw std::logic_error("B1 test storage field lookup failed.");
    }
    return found->payload;
  };
  B1StorageTransactionRawObservation& transaction = evidence.transaction;
  transaction.output_store_contract_id = payload("output_store_contract_id");
  transaction.output_store_contract_generation = 1U;
  transaction.backend_semantics_id = payload("backend_semantics_id");
  transaction.backend_semantics_generation = 1U;
  transaction.backend_instance_payload = payload("backend_instance_id");
  transaction.mount_identity_payload = payload("mount_identity");
  transaction.durability_endpoint_payload =
      payload("durability_endpoint_identity");
  transaction.durability_anchor_payload = payload("durability_anchor_identity");
  transaction.commit_semantics_payload = payload("commit_semantics");
  transaction.durability_capabilities_payload =
      payload("durability_capabilities");
  transaction.requested_durability = payload("requested_durability");
  transaction.achieved_durability = payload("achieved_durability");
  transaction.receipt_commit_id = b1_digest_hex(b1_sha256("b1-test-receipt"));
  transaction.receipt_root = evidence.backend.resolved_root;
  transaction.receipt_slot = "occurrence-" + transaction.receipt_commit_id;
  transaction.published_manifest_identity = "dev=1;ino=2";
  transaction.events = {
      {"manifest-published-no-replace", "event-manifest-published"},
      {"manifest-revalidated", "event-manifest-revalidated"},
      {"manifest-synchronized", "event-manifest-synchronized"},
      {"payload-revalidated", "event-payload-revalidated"},
      {"payload-synchronized", "event-payload-synchronized"},
      {"root-directory-synchronized", "event-root-synchronized"},
      {"slot-directory-synchronized", "event-slot-synchronized"}};

  evidence.containment.selected_root = evidence.backend.selected_root;
  evidence.containment.resolved_root = evidence.backend.resolved_root;
  evidence.containment.root_authority_identity = "root-authority-1";
  const std::filesystem::path receipt_destination =
      transaction.receipt_root / transaction.receipt_slot;
  evidence.containment.destinations.push_back(
      B1ContainmentDestinationObservation{
          receipt_destination, receipt_destination,
          evidence.containment.root_authority_identity, "transaction-receipt",
          transaction.receipt_commit_id});
  return evidence;
}

/**
 * @brief Builds complete canonical raw proof bytes for test storage fields.
 * @return Closed retained proof document.
 * @throws Validation or allocation failures unchanged.
 */
inline B1StorageRawProof b1_test_storage_raw_proof() {
  return B1StorageRawProof{
      encode_b1_storage_raw_proof(b1_test_storage_raw_evidence())};
}

/**
 * @brief Isolated test-only access to mint and mutate opaque output receipts.
 * @note This friend is defined only in test support. Product code has no
 * callable factory that accepts serialized receipt fields.
 */
struct B1OutputCommitReceiptTestAccess final {
  /**
   * @brief Mints one typed receipt for deterministic evaluator fixtures.
   * @param commit_id Stable lowercase commit identity.
   * @param resolved_root Canonical output root.
   * @param rooted_slot Root-relative occurrence slot.
   * @param job Complete occurrence identity.
   * @param logical_descriptor Fixed logical descriptor.
   * @param logical_content_digest Typed logical digest.
   * @param committed_generation Committed immutable generation.
   * @param payload_name Exact payload leaf.
   * @param manifest_name Exact manifest leaf.
   * @param payload_length Exact payload length.
   * @param manifest_length Exact manifest length.
   * @param payload_digest Exact raw payload digest.
   * @param manifest_digest Exact canonical manifest digest.
   * @param requested_durability Typed requested durability.
   * @param achieved_durability Typed achieved durability.
   * @param published_manifest_identity Published manifest identity.
   * @return Opaque typed receipt accepted by test-only live authority.
   * @throws Allocation failures from owned fixture fields unchanged.
   */
  static B1OutputCommitReceipt mint(
      std::string commit_id, std::filesystem::path resolved_root,
      std::filesystem::path rooted_slot, B1JobInstance job,
      std::string logical_descriptor, ContentDigest logical_content_digest,
      std::uint64_t committed_generation, std::string payload_name,
      std::string manifest_name, std::uint64_t payload_length,
      std::uint64_t manifest_length, B1Sha256Digest payload_digest,
      B1Sha256Digest manifest_digest, B1OutputDurability requested_durability,
      B1OutputDurability achieved_durability,
      std::string published_manifest_identity) {
    return B1OutputCommitReceipt(B1OutputCommitReceipt::Fields{
        std::move(commit_id), std::move(resolved_root), std::move(rooted_slot),
        std::move(job), std::move(logical_descriptor),
        std::move(logical_content_digest), committed_generation,
        std::move(payload_name), std::move(manifest_name), payload_length,
        manifest_length, std::move(payload_digest), std::move(manifest_digest),
        requested_durability, achieved_durability,
        std::move(published_manifest_identity)});
  }

  /**
   * @brief Mutates one receipt digest for a negative evaluator test.
   * @param receipt Existing test-minted receipt.
   * @param digest Replacement manifest digest.
   * @return Nothing.
   * @throws Nothing.
   * @note Product callers cannot mutate immutable receipt fields.
   */
  static void set_manifest_digest(B1OutputCommitReceipt* receipt,
                                  B1Sha256Digest digest) noexcept {
    receipt->fields_.manifest_digest = std::move(digest);
  }
};

/**
 * @brief Isolated mutable live source behind one test authority capability.
 * @throws Allocation failures from copied fixture storage unchanged.
 * @note Tests may mutate this source after minting to prove validation performs
 * a new observation rather than trusting construction-time diagnostics.
 * Mutations are serialized between matcher calls and are never concurrent.
 */
struct B1TestStorageAuthoritySource final {
  /** @brief Selected root returned by the trusted test adapter. */
  std::filesystem::path selected_root;
  /** @brief Fresh root facts returned by each test observation. */
  B1OutputStoreRootObservation root;
  /** @brief Opaque typed receipts returned by each test observation. */
  std::vector<B1OutputCommitReceipt> receipts;
  /** @brief Fresh complete probe result returned by each observation. */
  std::optional<B1StorageRawEvidence> complete_probe;
  /** @brief Fresh external fields lacking authority. */
  std::vector<std::string> unverified_external_fields;
};

/**
 * @brief Isolated test-only mint seam for actual storage authority.
 * @note The product header only grants friendship to this support type; no
 * production function accepts raw evidence as an authority source.
 */
struct B1StorageActualObservationTestAccess final {
  /**
   * @brief Mints a capability over one mutable test-owned live source.
   * @param source Non-null source re-read on every validation call.
   * @return Opaque actual storage observation.
   * @throws std::invalid_argument when `source` is null.
   * @throws Initial observation, canonical encoding, or allocation failures
   * unchanged.
   */
  static B1StorageActualObservation mint(
      std::shared_ptr<B1TestStorageAuthoritySource> source) {
    if (!source) {
      throw std::invalid_argument("B1 test authority source is missing");
    }
    return B1StorageActualObservation(
        [source]() -> B1StorageActualObservation::AuthoritySnapshot {
          return B1StorageActualObservation::AuthoritySnapshot{
              source->selected_root, source->root, source->receipts,
              source->complete_probe, source->unverified_external_fields};
        });
  }
};

/**
 * @brief Test capability plus its independently mutable live source.
 * @throws Allocation failures from capability/source movement unchanged.
 */
struct B1TestStorageAuthorityFixture final {
  /** @brief Opaque capability copied into environment evidence. */
  B1StorageActualObservation observation;
  /** @brief Test-only source retained for live-drift injection. */
  std::shared_ptr<B1TestStorageAuthoritySource> source;
};

/**
 * @brief Builds a complete independently mutable test authority fixture.
 * @return Opaque observation plus the source it re-observes.
 * @throws Validation or allocation failures unchanged.
 * @note Raw evidence initializes only the isolated test source, never a
 * production authority factory.
 */
inline B1TestStorageAuthorityFixture b1_test_storage_authority_fixture() {
  auto source = std::make_shared<B1TestStorageAuthoritySource>();
  source->complete_probe = b1_test_storage_raw_evidence();
  const B1StorageRawEvidence& probe = *source->complete_probe;
  const B1StorageTransactionRawObservation& transaction = probe.transaction;
  const B1JobInstance job{kB1WorkloadId, 1U, B1JobPhase::Measured, 0U, 0U, 1U};
  const B1JobGolden golden = b1_frozen_job_golden(job.job_index);
  const std::string manifest =
      b1_artifact_manifest(job.job_index, golden.raw_payload_digest);
  source->selected_root = probe.backend.selected_root;
  source->root = B1OutputStoreRootObservation{
      probe.backend.resolved_root, probe.containment.root_authority_identity,
      probe.backend.fields.at("filesystem_type").payload};
  source->receipts.push_back(B1OutputCommitReceiptTestAccess::mint(
      transaction.receipt_commit_id, transaction.receipt_root,
      transaction.receipt_slot, job, "dense-tensor-hwc-fp32-rgba-2048x2048",
      golden.logical_digest, 1U, "output.rgba32le", "manifest.txt",
      kB1PayloadBytes, b1_manifest_length(job.job_index),
      golden.raw_payload_digest, b1_sha256(manifest),
      B1OutputDurability::CrashDurable, B1OutputDurability::CrashDurable,
      transaction.published_manifest_identity));
  B1StorageActualObservation observation =
      B1StorageActualObservationTestAccess::mint(source);
  return B1TestStorageAuthorityFixture{std::move(observation),
                                       std::move(source)};
}

/**
 * @brief Builds independent process-private authority for the test probe.
 * @return Complete live-root/receipt facts plus the separately initialized raw
 * observation.
 * @throws Validation or allocation failures unchanged.
 * @note Tests deliberately initialize this value separately from retained
 * environment evidence so synchronized evidence recasting cannot change it.
 */
inline B1StorageActualObservation b1_test_storage_actual_observation() {
  return b1_test_storage_authority_fixture().observation;
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
  const B1StorageRawProof proof = b1_test_storage_raw_proof();
  return B1EnvironmentEvidence{
      base,
      base_digest,
      storage,
      storage_digest,
      class_bytes,
      digest_b1_environment_manifest(class_bytes),
      proof,
      evaluate_b1_storage_eligibility(storage, proof),
      kB1WorkloadId,
      b1_sha256("b1-test-fixture"),
      b1_sha256("b1-test-resources"),
      run_cap,
      replicate_ordinal,
      b1_test_storage_actual_observation(),
  };
}

/**
 * @brief Synchronously recasts every retained storage claim around a new
 * filesystem type while preserving independent actual authority unchanged.
 * @param evidence Complete required-storage environment to recast.
 * @param filesystem_type New valid canonical identifier.
 * @return Self-consistent retained storage/proof/class/digest/eligibility
 * claims whose actual observation still describes the original filesystem.
 * @throws std::invalid_argument for missing/malformed required evidence.
 * @throws Allocation or canonical encoding failures unchanged.
 * @note This models an actor that can edit every durable evidence input but
 * cannot alter the validating process, root descriptor, or actual receipts.
 */
inline B1EnvironmentEvidence synchronously_recast_b1_test_storage(
    B1EnvironmentEvidence evidence, std::string filesystem_type) {
  if (!evidence.storage_manifest.has_value() ||
      !evidence.storage_raw_proof.has_value()) {
    throw std::invalid_argument(
        "B1 synchronized recast requires retained storage evidence");
  }
  B1CanonicalManifest storage =
      parse_b1_environment_manifest(*evidence.storage_manifest);
  const auto storage_field =
      std::find_if(storage.fields.begin(), storage.fields.end(),
                   [](const B1CanonicalField& field) {
                     return field.name == "filesystem_type";
                   });
  if (storage_field == storage.fields.end()) {
    throw std::invalid_argument("B1 synchronized recast lacks filesystem_type");
  }
  storage_field->payload = filesystem_type;
  evidence.storage_manifest = encode_b1_storage_environment(storage.fields);
  evidence.claimed_storage_digest =
      digest_b1_environment_manifest(*evidence.storage_manifest);

  B1StorageRawEvidence proof =
      parse_b1_storage_raw_proof(evidence.storage_raw_proof->canonical_bytes);
  B1RawFieldObservation& raw = proof.backend.fields.at("filesystem_type");
  raw.payload = filesystem_type;
  raw.raw_payload = filesystem_type;
  evidence.storage_raw_proof =
      B1StorageRawProof{encode_b1_storage_raw_proof(proof)};
  evidence.storage_eligibility = evaluate_b1_storage_eligibility(
      *evidence.storage_manifest, *evidence.storage_raw_proof);

  B1CanonicalManifest environment_class =
      parse_b1_environment_manifest(evidence.environment_class_manifest);
  const auto class_field = std::find_if(
      environment_class.fields.begin(), environment_class.fields.end(),
      [](const B1CanonicalField& field) {
        return field.name == "storage_environment_digest";
      });
  if (class_field == environment_class.fields.end()) {
    throw std::invalid_argument(
        "B1 synchronized recast lacks class storage digest");
  }
  class_field->payload = b1_digest_hex(*evidence.claimed_storage_digest);
  evidence.environment_class_manifest =
      encode_b1_environment_class(environment_class.fields);
  evidence.claimed_environment_class_digest =
      digest_b1_environment_manifest(evidence.environment_class_manifest);
  return evidence;
}

}  // namespace ps::benchmark::testing
