/**
 * @file evidence_envelope.hpp
 * @brief Declares the closed canonical execution-profile row/bundle envelope.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "benchmark/b1_evidence.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/i1_evidence.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/m1_host.hpp"      // NOLINT(build/include_subdir)

namespace ps::benchmark {

// NOLINTBEGIN(whitespace/indent_namespace)

/** @brief Exact canonical 15-field evidence-row schema header. */
inline constexpr char kEvidenceRowSchema[] =
    "execution-profile-evidence-row-v1";

/** @brief Exact canonical five-field evidence-bundle schema header. */
inline constexpr char kEvidenceBundleSchema[] =
    "execution-profile-evidence-bundle-v1";

/** @brief Exact job-instance-index retained-section schema header. */
inline constexpr char kEvidenceJobIndexSchema[] =
    "execution-profile-job-instance-index-v1";

/** @brief Exact bundle-provenance retained-section schema header. */
inline constexpr char kEvidenceBundleProvenanceSchema[] =
    "execution-profile-bundle-provenance-v1";

/** @brief Exact reusable retained pair-object pack schema header. */
inline constexpr char kEvidencePairObjectSchema[] =
    "execution-profile-pair-object-v1";

/** @brief Maximum accepted pair-object pack size in bytes. */
inline constexpr std::size_t kEvidencePairObjectMaxBytes = 16U * 1024U * 1024U;

/** @brief Portable isolated-I1 p99-denominator source contract identifier. */
inline constexpr char kEvidenceI1PairDenominatorSchema[] =
    "execution-profile-i1-pair-denominator-v1";

/** @brief Portable isolated-B1 rate-denominator source contract identifier. */
inline constexpr char kEvidenceB1PairDenominatorSchema[] =
    "execution-profile-b1-pair-denominator-v1";

/** @brief Contract identifier denying portable output-verdict authority. */
inline constexpr char kEvidencePairNoOutputClaimSchema[] =
    "execution-profile-pair-no-output-claim-v1";

/** @brief Contract identifier denying non-denominator verdict authority. */
inline constexpr char kEvidencePairNoVerdictClaimSchema[] =
    "execution-profile-pair-no-verdict-claim-v1";

// NOLINTEND

/**
 * @brief Closed candidate/reference role retained by every row and bundle.
 * @throws Nothing for value construction and comparison.
 */
enum class EvidenceSubjectRole : std::uint8_t {
  /** @brief Build under evaluation against an immutable reference. */
  Candidate,
  /** @brief Immutable baseline with no comparison-reference dependency. */
  Reference,
};

/**
 * @brief Closed content-address node kind used by section dependency records.
 * @throws Nothing for value construction and comparison.
 */
enum class EvidenceAddressKind : std::uint8_t {
  /** @brief Retained section or bundle-provenance address. */
  Section,
  /** @brief Canonical 15-field row address. */
  Row,
  /** @brief Canonical five-field bundle address. */
  Bundle,
};

/**
 * @brief One typed already-sealed content-address dependency.
 * @throws Nothing for ordinary movement except owned digest allocation.
 */
struct EvidenceAddressReference final {
  /** @brief Exact target node kind. */
  EvidenceAddressKind kind = EvidenceAddressKind::Section;
  /** @brief Canonical lowercase SHA-256 target address. */
  std::string digest;

  /**
   * @brief Compares kind and digest exactly.
   * @param other Candidate reference.
   * @return True only for the identical typed address.
   * @throws Nothing.
   */
  bool operator==(const EvidenceAddressReference& other) const noexcept;
};

/**
 * @brief Immutable retained section plus its declared address dependencies.
 *
 * `bytes` use the shared canonical field-manifest grammar and have the exact
 * `schema_id` header. A nonempty dependency set must be reproduced by the
 * section's closed `address_dependencies` field.
 *
 * @throws std::bad_alloc when owned bytes/dependencies are copied.
 */
struct EvidenceRetainedSection final {
  /** @brief Exact row-field binding or `bundle-provenance`. */
  std::string section_name;
  /** @brief Exact closed retained-section schema header. */
  std::string schema_id;
  /** @brief Complete canonical retained bytes including final LF. */
  std::string bytes;
  /** @brief Complete typed addresses used to derive canonical bytes. */
  std::vector<EvidenceAddressReference> address_dependencies;
  /** @brief Nonzero global topological sealing ordinal. */
  std::uint64_t seal_ordinal = 0U;
};

/**
 * @brief Exact M1 or generic row pair target retained in one row field.
 * @throws Nothing for ordinary movement except owned digest allocation.
 */
struct EvidencePairReference final {
  /** @brief Exact target canonical row address. */
  std::string row_digest;
  /** @brief Exact target canonical bundle address. */
  std::string bundle_digest;
  /** @brief Same-subject target replicate ordinal in `[1,3]`. */
  std::uint64_t replicate_ordinal = 0U;

  /**
   * @brief Compares all three fixed-record components.
   * @param other Candidate pair.
   * @return True only for identical row, bundle, and ordinal.
   * @throws Nothing.
   */
  bool operator==(const EvidencePairReference& other) const noexcept;
};

/**
 * @brief Complete source values required to materialize one canonical row.
 * @throws std::bad_alloc when manifests, sections, jobs, or pairs are copied.
 * @note The complete environment object is retained so claimed row digests
 * can be independently recomputed and required-storage rows can re-observe
 * process-private machine authority. Canonical bytes alone cannot mint it.
 */
struct EvidenceRowInput final {
  /** @brief One exact frozen workload token. */
  std::string workload_id;
  /** @brief Candidate or reference role inherited by the enclosing bundle. */
  EvidenceSubjectRole subject_role = EvidenceSubjectRole::Candidate;
  /** @brief Exact replicate ordinal in `[1,3]`. */
  std::uint64_t replicate_ordinal = 0U;
  /** @brief Frozen workload Run cap. */
  std::uint64_t run_cap = 0U;
  /** @brief Complete self-validating environment and actual authority. */
  B1EnvironmentEvidence environment;
  /** @brief Closed workload-manifest retained section. */
  EvidenceRetainedSection workload_manifest;
  /** @brief Every B1 occurrence; empty exactly for I1/I2. */
  std::vector<B1JobInstance> job_instances;
  /** @brief Nonzero seal ordinal for the generated job-index section. */
  std::uint64_t job_index_seal_ordinal = 0U;
  /** @brief Closed measurement-evidence retained section. */
  EvidenceRetainedSection measurement_evidence;
  /** @brief Closed output-evidence retained section. */
  EvidenceRetainedSection output_evidence;
  /** @brief Closed verdict-evidence retained section. */
  EvidenceRetainedSection verdict_evidence;
  /** @brief M1 same-ordinal isolated-I1 pair; absent for non-M1. */
  std::optional<EvidencePairReference> paired_isolated_i1;
  /** @brief M1 same-ordinal isolated-B1-cap-eight pair; absent for non-M1. */
  std::optional<EvidencePairReference> paired_isolated_b1_cap8;
  /** @brief Nonzero global topological seal ordinal for the row. */
  std::uint64_t seal_ordinal = 0U;
};

/**
 * @brief Materialized canonical row plus all retained validation sources.
 * @throws std::bad_alloc when source/manifest ownership is copied.
 */
struct EvidenceCanonicalRow final {
  /** @brief Complete source values and retained section bytes. */
  EvidenceRowInput source;
  /** @brief Generated exact job-instance-index section. */
  EvidenceRetainedSection job_instance_index;
  /** @brief Exact 15-field canonical row manifest bytes. */
  std::string manifest_bytes;
  /** @brief Domain-separated lowercase canonical row address. */
  std::string digest;
};

/**
 * @brief Parsed canonical row values independent of producer source objects.
 * @throws std::bad_alloc when owned fields and pair records allocate.
 */
struct EvidenceParsedRow final {
  /** @brief Parsed exact workload token. */
  std::string workload_id;
  /** @brief Parsed candidate/reference role. */
  EvidenceSubjectRole subject_role = EvidenceSubjectRole::Candidate;
  /** @brief Parsed replicate ordinal. */
  std::uint64_t replicate_ordinal = 0U;
  /** @brief Parsed Run cap. */
  std::uint64_t run_cap = 0U;
  /** @brief Five independently retained section digests in row order. */
  std::vector<std::string> section_digests;
  /** @brief Parsed M1 I1 pair, when known. */
  std::optional<EvidencePairReference> paired_isolated_i1;
  /** @brief Parsed M1 B1 pair, when known. */
  std::optional<EvidencePairReference> paired_isolated_b1_cap8;
};

/**
 * @brief One canonical bundle row-reference fixed record.
 * @throws Nothing for ordinary movement except owned-string allocation.
 */
struct EvidenceRowReference final {
  /** @brief Exact row workload token. */
  std::string workload_id;
  /** @brief Exact functional-key Run cap. */
  std::uint64_t run_cap = 0U;
  /** @brief Exact functional-key replicate ordinal. */
  std::uint64_t replicate_ordinal = 0U;
  /** @brief Exact canonical row address. */
  std::string row_digest;

  /**
   * @brief Compares all fixed-record components.
   * @param other Candidate reference.
   * @return True only for the identical complete payload.
   * @throws Nothing.
   */
  bool operator==(const EvidenceRowReference& other) const noexcept;
};

/**
 * @brief Complete source values required to materialize one canonical bundle.
 * @throws std::bad_alloc when provenance, rows, or addresses are copied.
 */
struct EvidenceBundleInput final {
  /** @brief Exact bundle workload shared by every referenced row. */
  std::string workload_id;
  /** @brief Candidate or reference role shared by every referenced row. */
  EvidenceSubjectRole subject_role = EvidenceSubjectRole::Candidate;
  /** @brief Closed retained bundle-provenance section. */
  EvidenceRetainedSection provenance;
  /** @brief Required candidate baseline; absent exactly for a reference. */
  std::optional<std::string> comparison_reference_bundle_digest;
  /** @brief Complete already-materialized rows enclosed by this bundle. */
  std::vector<EvidenceCanonicalRow> rows;
  /** @brief Nonzero global topological seal ordinal for the bundle. */
  std::uint64_t seal_ordinal = 0U;
};

/**
 * @brief Materialized canonical bundle plus retained validation sources.
 * @throws std::bad_alloc when source/list/manifest ownership is copied.
 */
struct EvidenceCanonicalBundle final {
  /** @brief Complete source values including enclosed immutable rows. */
  EvidenceBundleInput source;
  /** @brief Canonically ordered functionally unique row references. */
  std::vector<EvidenceRowReference> row_references;
  /** @brief Exact five-field canonical bundle manifest bytes. */
  std::string manifest_bytes;
  /** @brief Domain-separated lowercase canonical bundle address. */
  std::string digest;
};

/**
 * @brief Parsed canonical bundle values independent of producer objects.
 * @throws std::bad_alloc when owned strings/list values allocate.
 */
struct EvidenceParsedBundle final {
  /** @brief Parsed exact workload token. */
  std::string workload_id;
  /** @brief Parsed candidate/reference role. */
  EvidenceSubjectRole subject_role = EvidenceSubjectRole::Candidate;
  /** @brief Parsed retained provenance section digest. */
  std::string provenance_digest;
  /** @brief Parsed comparison target, when known. */
  std::optional<std::string> comparison_reference_bundle_digest;
  /** @brief Parsed canonical nonempty row-reference list. */
  std::vector<EvidenceRowReference> row_references;
};

/**
 * @brief Retained object multiset used by exact-one canonical resolution.
 * @throws std::bad_alloc when rows or bundles are copied.
 * @note A vector intentionally permits duplicate claims so validation can
 * reject ambiguity instead of silently deduplicating by path or insertion.
 */
struct EvidenceCorpus final {
  /** @brief Every retained section and bundle-provenance object. */
  std::vector<EvidenceRetainedSection> sections;
  /** @brief Every retained canonical row object. */
  std::vector<EvidenceCanonicalRow> rows;
  /** @brief Every retained canonical bundle object. */
  std::vector<EvidenceCanonicalBundle> bundles;
};

/**
 * @brief Fail-closed exact-one/DAG/canonical validation result.
 * @throws std::bad_alloc when diagnostics are copied or appended.
 */
struct EvidenceCorpusValidation final {
  /** @brief Complete stable structural invalidation reasons. */
  std::vector<std::string> reasons;
  /** @brief Pass for one fully resolved acyclic root, otherwise Invalid. */
  I1Verdict verdict = I1Verdict::Invalid;
};

/**
 * @brief One native isolated row/bundle object selected for M1 pairing.
 * @throws std::bad_alloc when canonical source ownership is copied.
 * @note The portable object is authoritative only for the isolated I1 p99 or
 * isolated B1 rate denominator retained in its workload-specific measurement
 * section. It deliberately omits process-private storage authority and makes
 * no output, waste, memory, determinism, or aggregate verdict claim.
 */
struct EvidencePairObject final {
  /** @brief Exact isolated row addressed by the M1 pair reference. */
  EvidenceCanonicalRow row;
  /** @brief Exact one-row isolated bundle that names `row` once. */
  EvidenceCanonicalBundle bundle;
};

/**
 * @brief Role and comparison direction used by isolated pack producers.
 * @throws std::bad_alloc when an optional comparison digest is copied.
 */
struct EvidencePairProducerOptions final {
  /** @brief Candidate or reference role shared by row and bundle. */
  EvidenceSubjectRole subject_role = EvidenceSubjectRole::Reference;
  /** @brief Required candidate baseline and forbidden reference dependency. */
  std::optional<std::string> comparison_reference_bundle_digest;
};

/**
 * @brief Frozen B1 component addresses shared by isolated B1 and embedded M1.
 * @throws Nothing for value construction and copying.
 */
struct EvidenceB1ComponentDigests final {
  /** @brief Complete graph/source/plan/golden fixture identity. */
  B1Sha256Digest fixture;
  /** @brief Exact cold/warmup/measured input corpus identity. */
  B1Sha256Digest corpus;
  /** @brief Exact frozen logical/raw golden table identity. */
  B1Sha256Digest golden;
};

/**
 * @brief Raw isolated denominator sources independently recomputed for M1.
 * @throws Nothing for value construction and copying.
 */
struct EvidenceM1PairDenominators final {
  /** @brief Nearest-rank p99 over exactly 200 isolated-I1 samples. */
  std::uint64_t isolated_i1_p99_ns = 0U;
  /** @brief Verified isolated-B1 pixel-site operation numerator. */
  std::uint64_t isolated_b1_successful_site_operations = 0U;
  /** @brief Positive isolated-B1 measured interval duration. */
  std::uint64_t isolated_b1_duration_ns = 0U;
};

/**
 * @brief Returns the canonical token for one closed subject role.
 * @param role Candidate or Reference.
 * @return Process-lifetime canonical token.
 * @throws std::invalid_argument for an unknown enum representation.
 */
const char* evidence_subject_role_name(EvidenceSubjectRole role);

/**
 * @brief Computes one exact domain-separated retained-section address.
 * @param section_name Exact normative row/provenance binding.
 * @param section_schema_id Exact closed schema header.
 * @param section_bytes Complete retained canonical octets.
 * @return Lowercase SHA-256 content address.
 * @throws std::bad_alloc when digest input/output ownership allocates.
 */
std::string digest_evidence_section(std::string_view section_name,
                                    std::string_view section_schema_id,
                                    std::string_view section_bytes);

/**
 * @brief Computes one exact domain-separated canonical row address.
 * @param manifest_bytes Complete 15-field row bytes.
 * @return Lowercase SHA-256 content address.
 * @throws std::bad_alloc when digest input/output ownership allocates.
 */
std::string digest_evidence_row(std::string_view manifest_bytes);

/**
 * @brief Computes one exact domain-separated canonical bundle address.
 * @param manifest_bytes Complete five-field bundle bytes.
 * @return Lowercase SHA-256 content address.
 * @throws std::bad_alloc when digest input/output ownership allocates.
 */
std::string digest_evidence_bundle(std::string_view manifest_bytes);

/**
 * @brief Materializes and validates one exact 15-field canonical row.
 * @param input Complete environments, sections, jobs, pairs, and seal order.
 * @return Immutable canonical row and domain-separated address.
 * @throws std::invalid_argument for any schema, environment, identity,
 * ordering, applicability, dependency, or sealing drift.
 * @throws std::bad_alloc when canonical ownership allocates.
 */
EvidenceCanonicalRow materialize_evidence_row(EvidenceRowInput input);

/**
 * @brief Parses and validates one exact 15-field canonical row manifest.
 * @param bytes Complete candidate canonical bytes.
 * @return Parsed closed values.
 * @throws std::invalid_argument for any header/field/type/domain/order drift.
 * @throws std::bad_alloc when parsed ownership allocates.
 */
EvidenceParsedRow parse_evidence_row(std::string_view bytes);

/**
 * @brief Materializes and validates one exact five-field canonical bundle.
 * @param input Provenance, comparison role, enclosed rows, and seal order.
 * @return Immutable canonical bundle and domain-separated address.
 * @throws std::invalid_argument for any schema, role, row-list, functional
 * key, dependency, or sealing drift.
 * @throws std::bad_alloc when canonical ownership allocates.
 */
EvidenceCanonicalBundle materialize_evidence_bundle(EvidenceBundleInput input);

/**
 * @brief Parses and validates one exact five-field canonical bundle manifest.
 * @param bytes Complete candidate canonical bytes.
 * @return Parsed closed values and row references.
 * @throws std::invalid_argument for header/field/type/domain/list/order drift.
 * @throws std::bad_alloc when parsed ownership allocates.
 */
EvidenceParsedBundle parse_evidence_bundle(std::string_view bytes);

/**
 * @brief Resolves and validates one root bundle through the retained corpus.
 *
 * Validation independently rehashes every reached object, requires exact-one
 * digest resolution, checks row/bundle/item consistency, resolves candidate
 * comparison and M1 pair targets, enforces same-role/same-ordinal functional
 * keys and environment relations, and rejects undeclared, later-stage, or
 * cyclic dependencies.
 *
 * @param corpus Retained object multiset, including every external target.
 * @param root_bundle_digest Exact root bundle address to resolve once.
 * @return Pass with no reasons, or Invalid with stable diagnostics.
 * @throws std::bad_alloc when graph/index/diagnostic ownership allocates.
 */
EvidenceCorpusValidation validate_evidence_corpus(
    const EvidenceCorpus& corpus, std::string_view root_bundle_digest);

/**
 * @brief Computes the exact I1 component fixture used by isolated I1 and M1.
 * @return SHA-256 over frozen graph bytes and independent final golden bytes.
 * @throws Profile validation or allocation failures unchanged.
 */
B1Sha256Digest evidence_i1_component_fixture_digest();

/**
 * @brief Computes the three B1 component identities used by B1 and M1.
 * @return Independently domain-separated fixture, corpus, and golden digests.
 * @throws Profile validation or allocation failures unchanged.
 */
EvidenceB1ComponentDigests evidence_b1_component_digests();

/**
 * @brief Computes one shared execution-resource identity from a B1 snapshot.
 * @param snapshot Authoritative settled process resource and Compute-I/O cut.
 * @return SHA-256 over worker count and immutable resource limits.
 * @throws std::bad_alloc when canonical identity bytes allocate.
 */
B1Sha256Digest evidence_resource_identity(const B1ExecutionSnapshot& snapshot);

/**
 * @brief Computes one shared execution-resource identity from an M1 snapshot.
 * @param snapshot Authoritative settled process resource and Compute-I/O cut.
 * @return Same domain and byte layout as the B1 overload.
 * @throws std::bad_alloc when canonical identity bytes allocate.
 */
B1Sha256Digest evidence_resource_identity(const M1ExecutionSnapshot& snapshot);

/**
 * @brief Produces one native isolated-I1 pair object from actual Issue #93
 * rows.
 *
 * The producer re-evaluates the complete 221-slot replicate, retains exactly
 * 200 measured final latencies, and materializes a denominator-only 15-field
 * row and five-field one-row bundle. Portable output/verdict sections
 * explicitly declare that they carry no non-denominator authority.
 *
 * @param rows Complete uncompacted Issue #93 episode rows.
 * @param environment Exact storage-N/A environment claims and resource
 * identity.
 * @param options Subject role and optional candidate comparison dependency.
 * @return Native canonical row/bundle suitable for pack materialization.
 * @throws std::invalid_argument for incomplete raw evidence or identity drift.
 * @throws std::bad_alloc when canonical source ownership allocates.
 */
EvidencePairObject make_i1_evidence_pair_object(
    const std::vector<I1EpisodeInnerRow>& rows,
    B1EnvironmentEvidence environment, EvidencePairProducerOptions options);

/**
 * @brief Produces one native isolated-B1 pair object from an actual Issue #95
 * row.
 *
 * The producer first requires the exact 34-occurrence schema/version domain,
 * then retains all thirty measured per-job outcomes and the exact measurement
 * interval using the same verified-endpoint predicate as the B1 evaluator.
 * Portable output/verdict sections explicitly carry no other axis claim.
 *
 * @param row Complete uncompacted Issue #95 isolated inner row.
 * @param options Subject role and optional candidate comparison dependency.
 * @return Native canonical row/bundle suitable for pack materialization.
 * @throws std::invalid_argument for incomplete raw evidence or identity drift.
 * @throws std::bad_alloc when canonical source ownership allocates.
 */
EvidencePairObject make_b1_evidence_pair_object(
    const B1InnerRow& row, EvidencePairProducerOptions options);

/**
 * @brief Materializes one native pair object into a closed canonical pack.
 * @param object Exact one-row isolated bundle and all six retained sections.
 * @return Canonical source pack bytes including one final LF.
 * @throws std::invalid_argument for source/row/bundle/section/seal drift.
 * @throws std::bad_alloc when pack bytes allocate.
 * @note Process-private actual storage authority is intentionally excluded.
 */
std::string materialize_evidence_pair_object(const EvidencePairObject& object);

/**
 * @brief Loads and reconstructs one claims-only native canonical pair object.
 * @param bytes Complete canonical pack bytes.
 * @param expected_row_digest Exact caller-addressed row digest.
 * @param expected_bundle_digest Exact caller-addressed bundle digest.
 * @return Rebuilt row/bundle with every source claim and retained section.
 * @throws std::invalid_argument for source tamper, missing/duplicate sections,
 * digest mismatch, unresolved dependency, unsafe seal order, or schema drift.
 * @throws std::bad_alloc when reconstructed ownership allocates.
 * @note The returned required-storage environment has no actual observation.
 */
EvidencePairObject load_evidence_pair_object(
    std::string_view bytes, std::string_view expected_row_digest,
    std::string_view expected_bundle_digest);

/**
 * @brief Reads one bounded pair-object pack through a read-only no-follow path.
 * @param path Absolute existing regular-file path.
 * @return Exact bytes read from the same validated descriptor or handle.
 * @throws std::invalid_argument for relative, symlink, non-regular, empty, or
 * oversized inputs.
 * @throws std::runtime_error for open/stat/read/close failures.
 * @throws std::bad_alloc when byte storage allocates.
 * @note The helper never creates, replaces, truncates, deletes, or follows an
 * input path after opening it.
 */
std::string read_evidence_pair_object_file(const std::filesystem::path& path);

/**
 * @brief Adds one pair object to a retained corpus without deduplication.
 * @param object Exact row, bundle, and six retained sections.
 * @param corpus Mutable destination object multiset.
 * @return Nothing after all addresses are appended once.
 * @throws std::invalid_argument when any address already resolves in corpus.
 * @throws std::bad_alloc when destination vectors grow.
 * @note Rejecting rather than silently deduplicating preserves exact-one
 * ambiguity as evidence.
 */
void append_evidence_pair_object(const EvidencePairObject& object,
                                 EvidenceCorpus* corpus);

/**
 * @brief Binds two loaded pair objects to one pre-timed M1 source context.
 *
 * This claims-only boundary checks exact role/workload/cap/ordinal, one-row
 * bundle membership, base/full-environment relations, component fixture/
 * corpus/golden identities, and independently recomputes both denominators.
 * It does not promote retained storage claims to actual authority or Pass.
 *
 * @param isolated_i1 Loaded native isolated-I1 object.
 * @param isolated_b1_cap8 Loaded native isolated-B1 cap-eight object.
 * @param subject_role Required same-subject role.
 * @param replicate_ordinal Required same-ordinal key in `[1,3]`.
 * @param m1_environment Retained M1 claims and shared resource identity.
 * @param i1_fixture Exact embedded-I1 component fixture address.
 * @param b1_components Exact embedded-B1 component identities.
 * @return Positive raw denominator tuple for `M1InnerRowInput`.
 * @throws std::invalid_argument for every missing, mismatched, malformed, or
 * non-recomputing pair fact.
 * @throws std::bad_alloc when canonical raw records allocate.
 */
EvidenceM1PairDenominators validate_evidence_m1_pair_objects(
    const EvidencePairObject& isolated_i1,
    const EvidencePairObject& isolated_b1_cap8,
    EvidenceSubjectRole subject_role, std::uint64_t replicate_ordinal,
    const B1EnvironmentEvidence& m1_environment,
    const B1Sha256Digest& i1_fixture,
    const EvidenceB1ComponentDigests& b1_components);

}  // namespace ps::benchmark
