/**
 * @file b1_environment.hpp
 * @brief Declares the closed B1 environment schemas and compatibility checks.
 */
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "benchmark/b1_output_store.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/b1_profile.hpp"       // NOLINT(build/include_subdir)

namespace ps::benchmark {

struct B1EnvironmentEvidence;

namespace testing {
struct B1StorageActualObservationTestAccess;
}  // namespace testing

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
  /** @brief Exact provider/probe bytes from which `payload` was mapped. */
  std::string raw_payload;
  /** @brief Closed mapping proof kind for this observation. */
  std::string proof_kind{"probe-state-observed"};
  /** @brief Stable probe/receipt/path observation identity. */
  std::string proof_identity;
};

/**
 * @brief Closed backend raw observation consumed by one adapter.
 * @throws std::bad_alloc when copied map/path storage allocates.
 * @note `fields` must name exactly the 21 storage schema fields; provider
 * extension keys are rejected rather than copied into canonical bytes.
 */
struct B1BackendRawObservation final {
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
 * @brief Canonical storage fields reconstructed by one backend adapter.
 * @throws std::bad_alloc when copied field storage allocates.
 * @note The adapter validates every retained per-field observation and never
 * emits or accepts an aggregate proof-completeness boolean.
 */
struct B1AdaptedStorageObservation final {
  /** @brief Exact 21 canonical fields in normative order. */
  std::vector<B1CanonicalField> fields;
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
 * @brief Closed case behavior retained with native mount observations.
 * @throws Nothing for value construction and comparison.
 */
enum class B1MountCaseMode : std::uint8_t {
  /** @brief Native option keys and values are case-sensitive. */
  CaseSensitive,
  /** @brief Native contract defines ASCII case-insensitive keys/values. */
  AsciiCaseInsensitive,
};

/**
 * @brief Closed duplicate-option winner policy retained from the platform.
 * @throws Nothing for value construction and comparison.
 */
enum class B1MountDuplicatePolicy : std::uint8_t {
  /** @brief Equal duplicates are harmless; conflicting duplicates fail. */
  RejectConflicts,
  /** @brief The platform contract proves the last occurrence effective. */
  LastWins,
};

/**
 * @brief Concrete no-effect proof for one excluded native mount option.
 * @throws Nothing for ordinary movement except owned-string allocation.
 */
struct B1ExcludedMountOptionProof final {
  /** @brief Exact native option copied from the same mount observation. */
  B1NativeMountOption option;
  /** @brief Stable backend-semantics proof identity for complete-path no
   * effect.
   */
  std::string proof_identity;
};

/**
 * @brief Complete native mount observation used for deterministic replay.
 * @throws std::bad_alloc when copied collections allocate.
 */
struct B1MountRawObservation final {
  /** @brief Options in provider order, possibly with duplicates. */
  std::vector<B1NativeMountOption> options;
  /** @brief Exact defaults for omitted keys. */
  std::map<std::string, std::string> defaults;
  /** @brief Platform-declared case behavior bound to backend semantics. */
  B1MountCaseMode case_mode = B1MountCaseMode::CaseSensitive;
  /** @brief Platform-declared duplicate winner behavior. */
  B1MountDuplicatePolicy duplicate_policy =
      B1MountDuplicatePolicy::RejectConflicts;
  /** @brief Concrete proof record for every excluded native option. */
  std::vector<B1ExcludedMountOptionProof> excluded_options;
  /** @brief Stable identity of the complete owning-mount observation cut. */
  std::string observation_identity;
};

/**
 * @brief Raw proof kinds required for zero/absence performance components.
 * @throws std::bad_alloc when copied token storage allocates.
 */
struct B1PerformanceProofs final {
  /** @brief Sorted unique retained raw proof-kind tokens. */
  std::vector<std::string> proof_kinds;
  /** @brief Identity of the pre-warmup complete-path observation. */
  std::string initial_observation_identity;
  /** @brief Identity of the post-replicate stability observation. */
  std::string final_observation_identity;
  /** @brief Exact 37 post-replicate values independently compared to input. */
  std::vector<std::string> final_components;
  /** @brief Sorted unique identities for every mapped effective native option.
   */
  std::vector<std::string> mapped_option_proof_identities;
  /** @brief Sorted unique component names with observed conflicting values. */
  std::vector<std::string> conflicting_components;
};

/**
 * @brief One retained transaction event proving a concrete commit stage.
 * @throws Nothing for ordinary movement except owned-string allocation.
 */
struct B1StorageTransactionEvent final {
  /** @brief Closed event kind in the required commit state machine. */
  std::string kind;
  /** @brief Stable descriptor/receipt observation identity. */
  std::string observation_identity;
};

/**
 * @brief Complete receipt-backed storage transaction observation.
 * @throws std::bad_alloc when copied identity/event storage allocates.
 */
struct B1StorageTransactionRawObservation final {
  /** @brief Contract id observed by the transaction authority. */
  std::string output_store_contract_id;
  /** @brief Contract generation observed by the transaction authority. */
  std::uint64_t output_store_contract_generation = 0U;
  /** @brief Backend normalization contract id used by the transaction. */
  std::string backend_semantics_id;
  /** @brief Backend normalization contract generation. */
  std::uint64_t backend_semantics_generation = 0U;
  /** @brief Exact canonical text payload for the backend instance. */
  std::string backend_instance_payload;
  /** @brief Exact canonical text payload for the mount or empty when absent. */
  std::string mount_identity_payload;
  /** @brief Exact canonical text payload for the durability endpoint. */
  std::string durability_endpoint_payload;
  /** @brief Exact canonical text payload for the durability anchor. */
  std::string durability_anchor_payload;
  /** @brief Exact six-entry canonical commit-semantics payload. */
  std::string commit_semantics_payload;
  /** @brief Exact canonical durability-capability token-set payload. */
  std::string durability_capabilities_payload;
  /** @brief Exact requested durability token. */
  std::string requested_durability;
  /** @brief Exact achieved durability token copied from the receipt. */
  std::string achieved_durability;
  /** @brief Stable lowercase SHA-256 commit identity from the receipt. */
  std::string receipt_commit_id;
  /** @brief Resolved receipt root spelling. */
  std::filesystem::path receipt_root;
  /** @brief Root-relative receipt slot. */
  std::filesystem::path receipt_slot;
  /** @brief Stable published-manifest filesystem identity. */
  std::string published_manifest_identity;
  /** @brief Complete required commit observations with concrete identities. */
  std::vector<B1StorageTransactionEvent> events;
};

/**
 * @brief One retained root-containment observation for a concrete destination.
 * @throws Nothing for ordinary movement except owned path/string allocation.
 */
struct B1ContainmentDestinationObservation final {
  /** @brief Caller/path spelling observed by the output owner. */
  std::filesystem::path spelling;
  /** @brief Resolved destination observed under the root authority. */
  std::filesystem::path resolved;
  /** @brief Exact root descriptor/volume identity used for resolution. */
  std::string root_authority_identity;
  /** @brief Closed owner kind: transaction receipt or runner artifact. */
  std::string owner_kind;
  /** @brief Concrete receipt commit id or runner-artifact identity. */
  std::string owner_identity;
};

/**
 * @brief Complete selected-root and destination containment observations.
 * @throws std::bad_alloc when copied path/identity storage allocates.
 */
struct B1RootContainmentRawObservation final {
  /** @brief Selected root spelling retained outside environment digest input.
   */
  std::filesystem::path selected_root;
  /** @brief Canonically resolved root at the observation cut. */
  std::filesystem::path resolved_root;
  /** @brief Stable root descriptor/volume identity at that cut. */
  std::string root_authority_identity;
  /** @brief Every measured or retained artifact destination. */
  std::vector<B1ContainmentDestinationObservation> destinations;
};

/**
 * @brief Complete typed raw storage evidence before canonical serialization.
 * @throws std::bad_alloc when copied observation storage allocates.
 */
struct B1StorageRawEvidence final {
  /** @brief Backend fields, root spellings, and root identity. */
  B1BackendRawObservation backend;
  /** @brief Native mount/default/duplicate/exclusion observations. */
  B1MountRawObservation mount;
  /** @brief Exact pre-warmup 37-component performance observation. */
  std::array<std::string, 37U> performance_components;
  /** @brief Post-replicate values and concrete proof identities. */
  B1PerformanceProofs performance_proofs;
  /** @brief Concrete commit/receipt/event observations. */
  B1StorageTransactionRawObservation transaction;
  /** @brief Concrete root and every destination observation. */
  B1RootContainmentRawObservation containment;
};

/**
 * @brief Retained canonical raw proof bytes used for independent replay.
 * @throws Nothing for ordinary movement except owned-byte allocation.
 * @note No derived eligibility or proof-completeness boolean is retained.
 */
struct B1StorageRawProof final {
  /** @brief Complete closed proof document including header and final LF. */
  std::string canonical_bytes;
};

/**
 * @brief Serializable receipt metadata exposed only for diagnostics.
 * @throws Nothing for ordinary movement except owned path/string allocation.
 * @note This aggregate is explicitly not receipt authority. Validation uses
 * the opaque typed `B1OutputCommitReceipt` retained by a live source instead.
 */
struct B1StorageReceiptDiagnostic final {
  /** @brief Stable commit id copied from the typed receipt. */
  std::string commit_id;
  /** @brief Canonical root copied from the typed receipt. */
  std::filesystem::path resolved_root;
  /** @brief Root-relative occurrence slot copied from the typed receipt. */
  std::filesystem::path rooted_slot;
  /** @brief Published-manifest identity copied from the typed receipt. */
  std::string published_manifest_identity;
  /** @brief Requested durability token copied from the typed receipt. */
  std::string requested_durability;
  /** @brief Achieved durability token copied from the typed receipt. */
  std::string achieved_durability;
};

/**
 * @brief Opaque source-private actual storage authority.
 *
 * The object is a copyable capability over one live authority observer. Every
 * validation re-invokes that observer to recheck the held root descriptor,
 * immutable typed receipts, trusted-probe source, and unverified-field state.
 * Public accessors expose only the construction-time diagnostic snapshot.
 * Retained proof bytes, parsed `B1StorageRawEvidence`, JSON, or copied strings
 * cannot call the private constructor or mint the observer.
 *
 * @throws std::bad_alloc when copied observer/diagnostic storage allocates.
 * @note Copies inside `B1InnerRowInput` and `B1InnerRow` share the same live
 * capability and therefore extend its descriptor/probe-source lifetime. A
 * future trusted adapter must own a live re-observation source rather than
 * caching `B1StorageRawEvidence` as authority.
 * @note Validation is synchronous. A source that supports test/adapter drift
 * must serialize its mutations between validation calls; concurrent mutation
 * during one observation is outside this capability contract.
 */
class B1StorageActualObservation final {
 public:
  /** @brief Copies one existing live authority capability. */
  B1StorageActualObservation(const B1StorageActualObservation&) = default;

  /** @brief Moves one existing live authority capability. */
  B1StorageActualObservation(B1StorageActualObservation&&) noexcept = default;

  /** @brief Copies one existing live authority capability. */
  B1StorageActualObservation& operator=(const B1StorageActualObservation&) =
      default;

  /** @brief Moves one existing live authority capability. */
  B1StorageActualObservation& operator=(B1StorageActualObservation&&) noexcept =
      default;

  /** @brief Releases one observer owner and diagnostic snapshot. */
  ~B1StorageActualObservation() = default;

  /** @brief Returns the diagnostic selected-root spelling. */
  const std::filesystem::path& selected_root() const noexcept {
    return selected_root_;
  }

  /** @brief Returns the diagnostic canonical root. */
  const std::filesystem::path& resolved_root() const noexcept {
    return resolved_root_;
  }

  /** @brief Returns the diagnostic descriptor identity. */
  const std::string& root_authority_identity() const noexcept {
    return root_authority_identity_;
  }

  /** @brief Returns the diagnostic descriptor-derived filesystem type. */
  const std::string& filesystem_type() const noexcept {
    return filesystem_type_;
  }

  /** @brief Returns diagnostic copies of the typed receipt facts. */
  const std::vector<B1StorageReceiptDiagnostic>& receipt_diagnostics()
      const noexcept {
    return receipt_diagnostics_;
  }

  /** @brief Returns the diagnostic trusted-probe digest, when one existed. */
  const std::optional<B1Sha256Digest>& complete_probe_digest() const noexcept {
    return complete_probe_digest_;
  }

  /** @brief Returns diagnostic unverified external-field names. */
  const std::vector<std::string>& unverified_external_fields() const noexcept {
    return unverified_external_fields_;
  }

 private:
  /**
   * @brief One fresh observation returned by the opaque live source.
   * @throws std::bad_alloc when receipt/probe storage is copied.
   * @note The complete probe is an observation result, never the authority
   * itself; the non-serializable observer that produced it is the capability.
   */
  struct AuthoritySnapshot final {
    /** @brief Selected-root spelling bound by the live source. */
    std::filesystem::path selected_root;
    /** @brief Fresh descriptor-derived root facts. */
    B1OutputStoreRootObservation root;
    /** @brief Store/test-owner-minted immutable typed receipts. */
    std::vector<B1OutputCommitReceipt> receipts;
    /** @brief Fresh complete trusted-probe observation, when supported. */
    std::optional<B1StorageRawEvidence> complete_probe;
    /** @brief Fresh sorted unverified-field set. */
    std::vector<std::string> unverified_external_fields;
  };

  /** @brief Non-serializable callable that must produce a fresh snapshot. */
  using AuthorityObserver = std::function<AuthoritySnapshot()>;

  /**
   * @brief Mints diagnostics plus a retained live observer capability.
   * @param observer Nonempty source-private observer.
   * @throws std::invalid_argument for an empty observer.
   * @throws Observation, canonical encoding, and allocation failures from the
   * initial diagnostic snapshot unchanged.
   * @note Only the portable owner and isolated test access are friends; no
   * retained-proof or JSON parser can invoke this constructor.
   */
  explicit B1StorageActualObservation(AuthorityObserver observer);

  /**
   * @brief Re-observes the complete live authority source.
   * @return Fresh root, receipt, probe, and verification facts.
   * @throws Any source observation failure unchanged.
   */
  AuthoritySnapshot reobserve_authority() const;

  /** @brief Construction-time selected-root diagnostic. */
  std::filesystem::path selected_root_;
  /** @brief Construction-time canonical-root diagnostic. */
  std::filesystem::path resolved_root_;
  /** @brief Construction-time root-identity diagnostic. */
  std::string root_authority_identity_;
  /** @brief Construction-time filesystem-type diagnostic. */
  std::string filesystem_type_;
  /** @brief Construction-time typed-receipt diagnostics. */
  std::vector<B1StorageReceiptDiagnostic> receipt_diagnostics_;
  /** @brief Construction-time trusted-probe digest diagnostic. */
  std::optional<B1Sha256Digest> complete_probe_digest_;
  /** @brief Construction-time unverified-field diagnostics. */
  std::vector<std::string> unverified_external_fields_;
  /** @brief Opaque live root/receipt/probe re-observation capability. */
  AuthorityObserver authority_observer_;

  friend bool b1_storage_actual_observation_matches(
      const B1EnvironmentEvidence& evidence) noexcept;
  friend B1StorageActualObservation make_b1_portable_runner_storage_observation(
      B1OutputStoreRootAuthority root_authority,
      std::vector<B1OutputCommitReceipt> receipts);
  friend struct testing::B1StorageActualObservationTestAccess;
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
  /**
   * @brief Process-private actual storage authority for required-storage rows.
   * @note Retained manifests, raw-proof bytes, JSON, or arbitrary identity
   * strings cannot initialize this authority. Every self/cap/reference/mixed
   * comparison validates each side against its own observation.
   */
  std::optional<B1StorageActualObservation> storage_actual_observation;
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
 * @brief Parses one canonical unsigned decimal scalar from the shared grammar.
 * @param payload Nonempty decimal without sign or noncanonical leading zero.
 * @return Exact uint64 value.
 * @throws std::invalid_argument for lexical, range, or canonicality drift.
 */
std::uint64_t parse_b1_canonical_uint64(std::string_view payload);

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
 * @brief Parses one exact fixed record through the shared frame grammar.
 * @param payload Complete concatenated frame payload.
 * @param component_count Exact required component count.
 * @return Owned component bytes in encoded order.
 * @throws std::invalid_argument for malformed, truncated, or extra frames.
 * @throws std::bad_alloc when result ownership allocates.
 */
std::vector<std::string> parse_b1_fixed_record(std::string_view payload,
                                               std::size_t component_count);

/**
 * @brief Parses one counted generic list through the shared frame grammar.
 * @param payload Count followed by one frame per item.
 * @return Owned item payloads in encoded order.
 * @throws std::invalid_argument for count, frame, overflow, or consumption
 * drift.
 * @throws std::bad_alloc when result ownership allocates.
 */
std::vector<std::string> parse_b1_framed_list(std::string_view payload);

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
    const B1MountRawObservation& input);

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
    const B1BackendRawObservation& raw);

/**
 * @brief Encodes complete raw storage evidence with the shared field grammar.
 * @param evidence Backend/mount/performance/receipt/path observations.
 * @return Closed canonical proof bytes including final LF.
 * @throws std::invalid_argument for missing, duplicate, stale, or drifted raw
 * evidence.
 * @throws std::bad_alloc when canonical staging allocates.
 * @note This is a proof document, not an alternate storage manifest; the
 * canonical 21-field storage manifest remains the sole compatibility value.
 */
std::string encode_b1_storage_raw_proof(const B1StorageRawEvidence& evidence);

/**
 * @brief Parses and validates one complete canonical raw storage proof.
 * @param bytes Exact retained proof bytes.
 * @return Typed observations sufficient to replay every proof predicate.
 * @throws std::invalid_argument for framing, schema, observation, or binding
 * drift.
 * @throws std::bad_alloc when parsed storage allocates.
 */
B1StorageRawEvidence parse_b1_storage_raw_proof(std::string_view bytes);

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
 * @brief Encodes one prevalidated manifest through the shared field grammar.
 * @param schema Literal header without its terminating LF.
 * @param fields Exact ordered fields already validated by their closed schema.
 * @return Complete header, field records, and final LF.
 * @throws std::invalid_argument for an empty or line-breaking schema header or
 * an invalid closed observation-state representation.
 * @throws std::bad_alloc when output ownership allocates.
 * @note This primitive owns only the byte grammar. Callers remain responsible
 * for their exact field names, types, state/reason rules, and payload domains.
 */
std::string encode_b1_canonical_manifest(
    std::string_view schema, const std::vector<B1CanonicalField>& fields);

/**
 * @brief Parses only the shared canonical manifest envelope and field frames.
 * @param bytes Complete candidate bytes including one final LF.
 * @return Header, ordered fields, and retained exact bytes.
 * @throws std::invalid_argument for BOM/CR/NUL/header/line/frame/state drift.
 * @throws std::bad_alloc when parsed ownership allocates.
 * @note Closed schema validation remains with the caller; environment callers
 * use `parse_b1_environment_manifest` for the stronger exact schema checks.
 */
B1CanonicalManifest parse_b1_canonical_manifest(std::string_view bytes);

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
 * @brief Checks retained required-storage evidence against actual authority.
 * @param evidence Complete retained evidence plus process-private observation.
 * @return True only when a complete independent probe, live root facts, and at
 * least one actual receipt bind the retained storage bytes and raw proof.
 * @throws Nothing; missing, incomplete, malformed, or drifting authority fails
 * closed as false.
 * @note Durable evidence files alone can never make this return true. A caller
 * must re-observe the selected root and transaction receipts in the validating
 * process or supply a concretely verified attestation adapter.
 */
bool b1_storage_actual_observation_matches(
    const B1EnvironmentEvidence& evidence) noexcept;

/**
 * @brief Validates one environment object against its retained claims and
 * process-private authority.
 * @param evidence Complete base/storage/class evidence object.
 * @return True only when every manifest, claimed digest, applicability rule,
 * retained proof, derived eligibility result, and applicable live authority
 * is mutually consistent.
 * @throws Nothing; malformed, incomplete, unsupported, or drifting evidence
 * fails closed as false.
 * @note This is a self-validation boundary. Cross-row compatibility still
 * requires `compatible_b1_environments` with the appropriate relation.
 */
bool valid_b1_environment_evidence(
    const B1EnvironmentEvidence& evidence) noexcept;

/**
 * @brief Validates retained environment manifests, claims, raw proof, and
 * derived eligibility without treating serialized facts as live authority.
 * @param evidence Complete retained environment object.
 * @return True only when canonical bytes, independent digests,
 * applicability, proof, and derived eligibility are mutually consistent.
 * @throws Nothing; malformed or incomplete claims fail closed as false.
 * @note This boundary exists only so an ineligible manual run can retain a
 * canonical Invalid row. It is insufficient for conformance, compatibility,
 * or Pass; those require `valid_b1_environment_evidence` and actual authority.
 */
bool valid_b1_environment_claims(
    const B1EnvironmentEvidence& evidence) noexcept;

/**
 * @brief Builds the exact incomplete observation used by the portable runner.
 * @param root_authority Store-minted live held-root descriptor capability.
 * @param receipts Actual typed successful output receipts from the row.
 * @return Process-private root/receipt facts with no complete probe and the
 * exact sorted set of external declarations the portable path cannot verify.
 * @throws Root re-observation and allocation failures unchanged.
 * @note This is the manual runner's production construction path. It always
 * remains machine-ineligible until a separate trusted adapter supplies a
 * complete live probe source; retained proof bytes are not an input.
 */
B1StorageActualObservation make_b1_portable_runner_storage_observation(
    B1OutputStoreRootAuthority root_authority,
    std::vector<B1OutputCommitReceipt> receipts);

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
