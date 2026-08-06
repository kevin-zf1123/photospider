/**
 * @file b1_environment.hpp
 * @brief Declares the closed B1 environment schemas and compatibility checks.
 */
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "benchmark/b1_profile.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {

/**
 * @brief Closed observation state carried by every environment field.
 * @throws Nothing for value construction and comparison.
 */
enum class B1ObservationState : std::uint8_t {
  /** @brief Canonical payload is completely observed and proved. */
  Known,
  /** @brief Exact schema-permitted layer absence is proved. */
  NotApplicable,
  /** @brief Probe completed but returned an indeterminate value. */
  Unknown,
  /** @brief Required probe did not produce an observation. */
  Unobserved,
  /** @brief Probe or platform capability is unsupported. */
  Unsupported,
  /** @brief Evidence cannot prove one unique effective value. */
  Unprovable,
};

/**
 * @brief One typed exact state/reason/type/payload field value.
 * @throws Nothing for ordinary value operations except owned string copy.
 */
struct B1CanonicalField final {
  /** @brief Exact schema field name. */
  std::string name;
  /** @brief Closed observation state. */
  B1ObservationState state = B1ObservationState::Unobserved;
  /** @brief Exact closed reason token. */
  std::string reason;
  /** @brief Exact declared scalar/composite type. */
  std::string type;
  /** @brief Canonical payload, empty for every non-known state. */
  std::string payload;

  /**
   * @brief Compares the complete typed field value.
   * @param other Candidate field.
   * @return True only when all five components match.
   * @throws Nothing.
   */
  bool operator==(const B1CanonicalField& other) const noexcept;
};

/**
 * @brief Exact parsed canonical manifest and its closed schema identity.
 * @throws Nothing for ordinary movement except owned storage allocation.
 */
struct B1CanonicalManifest final {
  /** @brief Exact literal header without its terminating LF. */
  std::string schema;
  /** @brief Fixed ordered fields reconstructed independently from bytes. */
  std::vector<B1CanonicalField> fields;
  /** @brief Complete retained canonical bytes including final LF. */
  std::string bytes;
};

/**
 * @brief Backend class selecting one closed raw-to-canonical adapter.
 * @throws Nothing for value construction and comparison.
 */
enum class B1StorageBackendKind : std::uint8_t {
  /** @brief Host filesystem path. */
  Filesystem,
  /** @brief Network filesystem path. */
  NetworkFilesystem,
  /** @brief Object storage namespace. */
  ObjectStore,
  /** @brief Volatile/persistent memory storage. */
  MemoryStore,
  /** @brief Explicit composition of multiple backend layers. */
  Composite,
};

/**
 * @brief One raw field observation plus retained mapping-proof state.
 * @throws Nothing for ordinary value operations except owned strings.
 */
struct B1RawFieldObservation final {
  /** @brief Closed raw observation state. */
  B1ObservationState state = B1ObservationState::Unobserved;
  /** @brief Exact closed reason token. */
  std::string reason{"probe-not-run"};
  /** @brief Exact canonical type expected after mapping. */
  std::string type;
  /** @brief Candidate canonical payload for known values. */
  std::string payload;
  /** @brief Whether retained raw evidence proves this exact mapping. */
  bool mapping_proved = false;
  /** @brief Whether a permitted N/A claim has exact layer-absence proof. */
  bool absence_proved = false;
};

/**
 * @brief Closed backend raw observation consumed by one adapter.
 * @throws std::bad_alloc when copied map/path storage allocates.
 * @note `fields` must name exactly the 21 storage schema fields; provider
 * extension keys are rejected rather than copied into canonical bytes.
 */
struct B1RawStorageObservation final {
  /** @brief Backend adapter selected from the observed path. */
  B1StorageBackendKind backend = B1StorageBackendKind::Filesystem;
  /** @brief Exact 21-field observation map before canonical ordering. */
  std::map<std::string, B1RawFieldObservation> fields;
  /** @brief Selected root spelling retained outside digest input. */
  std::filesystem::path selected_root;
  /** @brief Resolved root retained for containment proof. */
  std::filesystem::path resolved_root;
};

/**
 * @brief Canonical storage fields plus proof completeness retained by adapter.
 * @throws std::bad_alloc when copied field storage allocates.
 * @note The flags are derived from the same per-field observations used to
 * build `fields`; callers copy them into `B1StorageRawProof` rather than
 * making an unrelated aggregate claim.
 */
struct B1AdaptedStorageObservation final {
  /** @brief Exact 21 canonical fields in normative order. */
  std::vector<B1CanonicalField> fields;
  /** @brief Whether every known or N/A field has mapping proof. */
  bool raw_mapping_complete = false;
  /** @brief Whether every syntactically permitted N/A has absence proof. */
  bool not_applicable_proofs_valid = false;
};

/**
 * @brief Native mount option used by deterministic normalization.
 * @throws Nothing for ordinary value operations except string copy.
 */
struct B1NativeMountOption final {
  /** @brief Provider-native option key. */
  std::string key;
  /** @brief Provider-native effective/explicit value. */
  std::string value;
};

/**
 * @brief Complete native mount normalization input and retained proof flags.
 * @throws std::bad_alloc when copied collections allocate.
 */
struct B1MountNormalizationInput final {
  /** @brief Options in provider order, possibly with duplicates. */
  std::vector<B1NativeMountOption> options;
  /** @brief Exact defaults for omitted keys. */
  std::map<std::string, std::string> defaults;
  /** @brief Whether the native contract declares ASCII case-insensitivity. */
  bool ascii_case_insensitive = false;
  /** @brief Whether last occurrence is the proved effective duplicate winner.
   */
  bool duplicate_last_wins_proved = false;
  /** @brief Whether every unknown option has complete no-effect proof. */
  bool unknown_options_no_effect_proved = false;
};

/**
 * @brief Raw proof kinds required for zero/absence performance components.
 * @throws std::bad_alloc when copied token storage allocates.
 */
struct B1PerformanceProofs final {
  /** @brief Sorted unique retained raw proof-kind tokens. */
  std::vector<std::string> proof_kinds;
  /** @brief Whether all 37 values came from one frozen complete path cut. */
  bool one_frozen_observation = false;
  /** @brief Whether all performance-affecting native options were mapped. */
  bool complete_option_mapping = false;
  /** @brief Whether the frozen configuration remained effective throughout. */
  bool stable_through_replicate = false;
  /** @brief Whether retained observations contain conflicting values. */
  bool conflicting_values = false;
};

/**
 * @brief Independent semantic/proof facts evaluated after canonical parsing.
 * @throws Nothing for scalar state; vectors/paths allocate when copied.
 */
struct B1StorageRawProof final {
  /** @brief Complete raw-to-canonical proof for every required field. */
  bool raw_mapping_complete = false;
  /** @brief Six commit values agree with transaction/receipt behavior. */
  bool commit_semantics_consistent = false;
  /** @brief Contract/backend/path/receipt form one durability chain. */
  bool durability_path_consistent = false;
  /** @brief Present mount uniquely normalizes, or absence is proved. */
  bool mount_normalization_proved = false;
  /** @brief Every permitted N/A field has exact layer-absence proof. */
  bool not_applicable_proofs_valid = false;
  /** @brief Complete performance mapping is one stable frozen observation. */
  bool performance_configuration_proved = false;
  /** @brief Every measured/retained destination is below selected root. */
  bool root_containment_proved = false;
};

/**
 * @brief Exact eligibility result derived from canonical bytes and raw proof.
 * @throws std::bad_alloc when reason storage allocates.
 */
struct B1StorageEligibility final {
  /** @brief True exactly when the ordered reason list is empty. */
  bool eligible = false;
  /** @brief Complete truth set in the normative fixed order. */
  std::vector<std::string> reasons;

  /**
   * @brief Compares the complete derived eligibility truth set.
   * @param other Candidate retained result.
   * @return True only when the verdict and ordered reasons match exactly.
   * @throws Nothing.
   */
  bool operator==(const B1StorageEligibility& other) const noexcept;
};

/**
 * @brief Environment evidence used by exact compatibility relations.
 * @throws std::bad_alloc when retained bytes/identity storage allocates.
 */
struct B1EnvironmentEvidence final {
  /** @brief Complete canonical base manifest bytes. */
  std::string base_manifest;
  /** @brief Claimed base digest retained beside bytes. */
  B1Sha256Digest claimed_base_digest;
  /** @brief Complete canonical storage bytes when applicable. */
  std::optional<std::string> storage_manifest;
  /** @brief Claimed storage digest when applicable. */
  std::optional<B1Sha256Digest> claimed_storage_digest;
  /** @brief Complete canonical four-field environment-class bytes. */
  std::string environment_class_manifest;
  /** @brief Claimed environment-class digest retained beside bytes. */
  B1Sha256Digest claimed_environment_class_digest;
  /** @brief Independent raw proof retained when storage is applicable. */
  std::optional<B1StorageRawProof> storage_raw_proof;
  /** @brief Applicable eligibility result derived from retained bytes/proof. */
  std::optional<B1StorageEligibility> storage_eligibility;
  /** @brief Frozen workload identity. */
  std::string workload_id;
  /** @brief Exact fixture/corpus content address. */
  B1Sha256Digest fixture_digest;
  /** @brief Exact resource configuration identity. */
  B1Sha256Digest resource_identity;
  /** @brief Run cap represented by this evidence. */
  std::uint64_t run_cap = 0U;
  /** @brief Fresh-process replicate ordinal. */
  std::uint64_t replicate_ordinal = 0U;
};

/**
 * @brief Required exact relation between two environment evidence objects.
 * @throws Nothing for value construction and comparison.
 */
enum class B1EnvironmentRelation : std::uint8_t {
  /** @brief Candidate/reference peers with identical cap and replicate. */
  CandidateReference,
  /** @brief Isolated B1 cap-one/cap-eight peers in the same subject. */
  CapOneCapEight,
  /** @brief M1 row and paired isolated B1 cap-eight row. */
  M1PairedB1CapEight,
  /** @brief M1 row and paired isolated I1 base-only row. */
  M1PairedI1BaseOnly,
};

/**
 * @brief Produces one exact unpadded byte-length frame.
 * @param payload Arbitrary borrowed bytes.
 * @return Decimal byte length, colon, and exact payload.
 * @throws std::bad_alloc when output allocation fails.
 */
std::string b1_environment_frame(std::string_view payload);

/**
 * @brief Encodes one sorted unique closed token set.
 * @param tokens Candidate raw ASCII tokens.
 * @param domain Closed allowed token universe.
 * @return Count plus one frame per sorted token.
 * @throws std::invalid_argument for duplicates or out-of-domain tokens.
 * @throws std::bad_alloc when staging/output allocation fails.
 */
std::string encode_b1_token_set(std::vector<std::string> tokens,
                                const std::vector<std::string>& domain);

/**
 * @brief Encodes invocation-ordered canonical lowercase-hex text items.
 * @param encoded_text_items Canonical text payloads; repetition is allowed.
 * @return Count plus one frame per item in input order.
 * @throws std::invalid_argument for an invalid text payload.
 * @throws std::bad_alloc when output allocation fails.
 */
std::string encode_b1_ordered_text_list(
    const std::vector<std::string>& encoded_text_items);

/**
 * @brief Encodes one fixed record without component names or a count.
 * @param components Exact component payloads in schema order.
 * @return One length frame per component.
 * @throws std::bad_alloc when output allocation fails.
 */
std::string encode_b1_fixed_record(const std::vector<std::string>& components);

/**
 * @brief Encodes one sorted unique canonical map.
 * @param entries Candidate key/value pairs.
 * @return Pair count plus alternating key/value frames.
 * @throws std::invalid_argument for empty/duplicate/unsorted-domain drift.
 * @throws std::bad_alloc when staging/output allocation fails.
 */
std::string encode_b1_map(
    const std::vector<std::pair<std::string, std::string>>& entries);

/**
 * @brief Normalizes one UTF-8 text observation and returns lowercase hex.
 * @param utf8 Raw observation text.
 * @return Canonical lowercase-hex text payload.
 * @throws std::invalid_argument for empty, invalid UTF-8, or non-NFC input.
 * @throws std::bad_alloc when output allocation fails.
 * @note The adapter validates NFC rather than silently changing identity.
 */
std::string encode_b1_normalized_text(std::string_view utf8);

/**
 * @brief Normalizes seven effective mount keys or fails closed.
 * @param input Native observations/defaults and proof flags.
 * @return Known canonical map, or exact unprovable empty observation.
 * @throws std::invalid_argument when case normalization sees non-ASCII input.
 * @throws std::bad_alloc when normalization staging allocates.
 */
B1RawFieldObservation normalize_b1_mount_options(
    const B1MountNormalizationInput& input);

/**
 * @brief Validates and encodes one exact 37-component performance record.
 * @param components Exact values in normative order.
 * @param proofs Complete absence/mapping/freeze proof state.
 * @return Known fixed-record observation or exact unprovable/conflict state.
 * @throws std::invalid_argument for malformed component lexical/schema state.
 * @throws std::bad_alloc when output/proof staging allocates.
 */
B1RawFieldObservation map_b1_performance_configuration(
    const std::array<std::string, 37U>& components,
    const B1PerformanceProofs& proofs);

/**
 * @brief Maps one backend observation into the same fixed 21-field schema.
 * @param raw Closed backend observation and proofs.
 * @return Ordered fields and proof completeness derived from the same input.
 * @throws std::invalid_argument for missing/extra/type/backend-class drift.
 * @throws std::bad_alloc when ordered output allocates.
 */
B1AdaptedStorageObservation adapt_b1_storage_observation(
    const B1RawStorageObservation& raw);

/**
 * @brief Encodes exact 21-field storage environment bytes.
 * @param fields Fixed values in schema order.
 * @return Complete canonical bytes including header/final LF.
 * @throws std::invalid_argument for any lexical/schema/composite drift.
 * @throws std::bad_alloc when output allocation fails.
 */
std::string encode_b1_storage_environment(
    const std::vector<B1CanonicalField>& fields);

/**
 * @brief Encodes exact 24-field base environment bytes.
 * @param fields Fixed values in schema order.
 * @return Complete canonical bytes including header/final LF.
 * @throws std::invalid_argument for any lexical/schema/composite drift.
 * @throws std::bad_alloc when output allocation fails.
 */
std::string encode_b1_base_environment(
    const std::vector<B1CanonicalField>& fields);

/**
 * @brief Encodes exact four-field environment-class bytes.
 * @param fields Fixed values in schema order.
 * @return Complete canonical bytes including header/final LF.
 * @throws std::invalid_argument for any lexical/schema/relation drift.
 * @throws std::bad_alloc when output allocation fails.
 */
std::string encode_b1_environment_class(
    const std::vector<B1CanonicalField>& fields);

/**
 * @brief Independently parses exact canonical environment bytes.
 * @param bytes Complete candidate bytes.
 * @return Parsed schema/fields and retained bytes.
 * @throws std::invalid_argument for header, frame, field, or schema drift.
 * @throws std::bad_alloc when parsed storage allocates.
 * @note Parsing does not call an encoder or trust a claimed digest.
 */
B1CanonicalManifest parse_b1_environment_manifest(std::string_view bytes);

/**
 * @brief Computes plain SHA-256 over complete canonical manifest bytes.
 * @param bytes Complete exact bytes.
 * @return Raw SHA-256 digest.
 * @throws As `b1_sha256`.
 */
B1Sha256Digest digest_b1_environment_manifest(std::string_view bytes);

/**
 * @brief Evaluates the exact eleven-reason storage truth set.
 * @param storage_bytes Candidate complete storage manifest bytes.
 * @param raw Independent retained proof facts.
 * @return Singleton canonical-invalid or all true reasons in fixed order.
 * @throws std::bad_alloc when parsed/reason storage allocates.
 */
B1StorageEligibility evaluate_b1_storage_eligibility(
    std::string_view storage_bytes, const B1StorageRawProof& raw);

/**
 * @brief Proves every destination resolves below one selected canonical root.
 * @param selected_root Existing selected `OutputStore` root.
 * @param destinations Existing or weakly canonicalizable destinations.
 * @return True only when every resolved path has the complete root prefix.
 * @throws std::filesystem::filesystem_error when resolution fails.
 * @throws std::bad_alloc when path component staging allocates.
 */
bool prove_b1_root_containment(
    const std::filesystem::path& selected_root,
    const std::vector<std::filesystem::path>& destinations);

/**
 * @brief Checks exact candidate/reference or cap/pair compatibility.
 * @param lhs First retained evidence object.
 * @param rhs Second retained evidence object.
 * @param relation Required pairing relation.
 * @return True only after independent parse/digest/eligibility/identity checks.
 * @throws Nothing; malformed evidence fails closed as false.
 */
bool compatible_b1_environments(const B1EnvironmentEvidence& lhs,
                                const B1EnvironmentEvidence& rhs,
                                B1EnvironmentRelation relation) noexcept;

}  // namespace ps::benchmark
