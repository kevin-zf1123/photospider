/**
 * @file evidence_envelope.hpp
 * @brief Declares the closed canonical execution-profile row/bundle envelope.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "benchmark/b1_environment.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/i1_evidence.hpp"     // NOLINT(build/include_subdir)

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

}  // namespace ps::benchmark
