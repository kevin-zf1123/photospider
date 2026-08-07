/**
 * @file test_evidence_envelope.cpp
 * @brief Verifies the closed execution-profile row/bundle evidence envelope.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/evidence_envelope.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/m1_profile.hpp"         // NOLINT(build/include_subdir)
#include "support/b1_test_environment.hpp"

namespace ps::benchmark {
namespace {

/**
 * @brief Returns a deterministic lowercase digest for fixture construction.
 * @param label Stable test-only domain label.
 * @return Exact lowercase SHA-256 spelling.
 * @throws std::bad_alloc when result ownership allocates.
 */
std::string test_digest(std::string_view label) {
  return b1_digest_hex(b1_sha256(label));
}

/**
 * @brief Recasts one eligible M1 environment as a base-only I1 peer.
 * @param m1 Complete same-subject M1 environment.
 * @return Self-validating storage-N/A I1 environment.
 * @throws Canonical encoding and allocation failures unchanged.
 */
B1EnvironmentEvidence make_i1_environment(B1EnvironmentEvidence m1) {
  m1.workload_id = kI1WorkloadId;
  m1.storage_manifest.reset();
  m1.claimed_storage_digest.reset();
  m1.storage_raw_proof.reset();
  m1.storage_eligibility.reset();
  m1.storage_actual_observation.reset();
  m1.environment_class_manifest = encode_b1_environment_class(
      {testing::known_b1_field("base_environment_digest", "sha256",
                               b1_digest_hex(m1.claimed_base_digest)),
       testing::known_b1_field("storage_environment_applicability", "enum",
                               "not-applicable"),
       testing::known_b1_field("storage_environment_not_applicable_reason",
                               "enum", "row-has-no-output-commit"),
       testing::not_applicable_b1_field("storage_environment_digest", "sha256",
                                        "row-has-no-output-commit")});
  m1.claimed_environment_class_digest =
      digest_b1_environment_manifest(m1.environment_class_manifest);
  return m1;
}

/**
 * @brief Encodes one canonical dependency list for an inner section.
 * @param dependencies Complete typed address set.
 * @return Sorted unique generic list payload.
 * @throws std::invalid_argument for duplicate dependencies.
 * @throws std::bad_alloc when output ownership allocates.
 */
std::string encode_test_dependencies(
    const std::vector<EvidenceAddressReference>& dependencies) {
  std::vector<std::string> records;
  for (const EvidenceAddressReference& dependency : dependencies) {
    std::string kind;
    switch (dependency.kind) {
      case EvidenceAddressKind::Section:
        kind = "section";
        break;
      case EvidenceAddressKind::Row:
        kind = "row";
        break;
      case EvidenceAddressKind::Bundle:
        kind = "bundle";
        break;
      default:
        throw std::invalid_argument("unknown test dependency kind");
    }
    records.push_back(encode_b1_fixed_record({kind, dependency.digest}));
  }
  std::sort(records.begin(), records.end());
  if (std::adjacent_find(records.begin(), records.end()) != records.end()) {
    throw std::invalid_argument("duplicate test dependency");
  }
  std::string payload = std::to_string(records.size()) + ":";
  for (const std::string& record : records) {
    payload.append(b1_environment_frame(record));
  }
  return payload;
}

/**
 * @brief Builds one closed retained section with optional explicit addresses.
 * @param name Exact row/provenance binding.
 * @param schema Exact inner schema header.
 * @param fields Inner known fields in schema order.
 * @param seal_ordinal Nonzero global sealing ordinal.
 * @param dependencies Complete already-sealed address inputs.
 * @return Canonical retained section.
 * @throws Canonical encoding and allocation failures unchanged.
 */
EvidenceRetainedSection make_section(
    std::string name, std::string schema, std::vector<B1CanonicalField> fields,
    std::uint64_t seal_ordinal,
    std::vector<EvidenceAddressReference> dependencies = {}) {
  if (!dependencies.empty()) {
    fields.push_back(testing::known_b1_field(
        "address_dependencies", "evidence-address-list-v1",
        encode_test_dependencies(dependencies)));
  }
  const std::string bytes = encode_b1_canonical_manifest(schema, fields);
  return EvidenceRetainedSection{std::move(name), std::move(schema), bytes,
                                 std::move(dependencies), seal_ordinal};
}

/**
 * @brief Builds the frozen workload-manifest digest bindings for one workload.
 * @param workload Exact I1, B1, or M1 token.
 * @return Closed deterministic known fields.
 * @throws std::bad_alloc when field ownership allocates.
 */
std::vector<B1CanonicalField> workload_fields(std::string_view workload,
                                              EvidenceSubjectRole role,
                                              std::uint64_t ordinal) {
  const std::string shared_fixture = test_digest("shared-fixture");
  const std::string identity = std::string(workload) + ":" +
                               evidence_subject_role_name(role) + ":" +
                               std::to_string(ordinal);
  std::vector<B1CanonicalField> fields{
      testing::known_b1_field("fixture_digest", "sha256", shared_fixture),
      testing::known_b1_field("row_identity_digest", "sha256",
                              test_digest(identity))};
  if (workload == kI1WorkloadId || workload == kM1WorkloadId) {
    fields.push_back(testing::known_b1_field("i1_fixture_digest", "sha256",
                                             test_digest("i1-fixture")));
  }
  if (workload == kB1WorkloadId || workload == kM1WorkloadId) {
    fields.push_back(testing::known_b1_field("b1_fixture_digest", "sha256",
                                             test_digest("b1-fixture")));
    fields.push_back(testing::known_b1_field("b1_corpus_digest", "sha256",
                                             test_digest("b1-corpus")));
    fields.push_back(testing::known_b1_field("b1_golden_digest", "sha256",
                                             test_digest("b1-golden")));
  }
  return fields;
}

/**
 * @brief Materializes one deterministic row with complete retained sections.
 * @param environment Self-validating environment with authoritative identity.
 * @param role Candidate or reference role.
 * @param first_seal First section seal; six consecutive values are consumed.
 * @param i1_pair Required M1 I1 target, otherwise absent.
 * @param b1_pair Required M1 B1 target, otherwise absent.
 * @return Canonical row suitable for bundle/corpus tests.
 * @throws Envelope, environment, canonical, and allocation failures unchanged.
 */
EvidenceCanonicalRow make_row(
    B1EnvironmentEvidence environment, EvidenceSubjectRole role,
    std::uint64_t first_seal,
    std::optional<EvidencePairReference> i1_pair = std::nullopt,
    std::optional<EvidencePairReference> b1_pair = std::nullopt) {
  const std::string workload = environment.workload_id;
  const std::uint64_t ordinal = environment.replicate_ordinal;
  const std::uint64_t run_cap = environment.run_cap;
  EvidenceRowInput input;
  input.workload_id = workload;
  input.subject_role = role;
  input.replicate_ordinal = ordinal;
  input.run_cap = run_cap;
  input.environment = std::move(environment);
  input.workload_manifest = make_section(
      "workload-manifest", "execution-profile-workload-manifest-v1",
      workload_fields(workload, role, ordinal), first_seal);
  if (workload == kB1WorkloadId || workload == kM1WorkloadId) {
    input.job_instances.push_back(B1JobInstance{
        workload, ordinal, B1JobPhase::Cold, 0U, kB1ColdJobIndex, run_cap});
  }
  input.job_index_seal_ordinal =
      workload == kI1WorkloadId ? 2U : first_seal + 1U;
  input.measurement_evidence = make_section(
      "measurement-evidence", "execution-profile-measurement-evidence-v1",
      {testing::known_b1_field(
          "raw_digest", "sha256",
          test_digest(workload + std::string(":measurement:") +
                      evidence_subject_role_name(role) + ":" +
                      std::to_string(ordinal)))},
      first_seal + 2U);
  input.output_evidence = make_section(
      "output-evidence", "execution-profile-output-evidence-v1",
      {testing::known_b1_field("raw_digest", "sha256",
                               test_digest(workload + std::string(":output:") +
                                           evidence_subject_role_name(role) +
                                           ":" + std::to_string(ordinal)))},
      first_seal + 3U);
  input.verdict_evidence = make_section(
      "verdict-evidence", "execution-profile-verdict-evidence-v1",
      {testing::known_b1_field("raw_digest", "sha256",
                               test_digest(workload + std::string(":verdict:") +
                                           evidence_subject_role_name(role) +
                                           ":" + std::to_string(ordinal)))},
      first_seal + 4U);
  input.paired_isolated_i1 = std::move(i1_pair);
  input.paired_isolated_b1_cap8 = std::move(b1_pair);
  input.seal_ordinal = first_seal + 5U;
  return materialize_evidence_row(std::move(input));
}

/**
 * @brief Materializes one reference bundle enclosing exactly one row.
 * @param row Already sealed row.
 * @param seal_ordinal Bundle seal later than provenance and row.
 * @return Canonical reference bundle.
 * @throws Envelope and allocation failures unchanged.
 */
EvidenceCanonicalBundle make_reference_bundle(EvidenceCanonicalRow row,
                                              std::uint64_t seal_ordinal) {
  EvidenceBundleInput input;
  input.workload_id = row.source.workload_id;
  input.subject_role = EvidenceSubjectRole::Reference;
  input.provenance = make_section(
      "bundle-provenance", kEvidenceBundleProvenanceSchema,
      {testing::known_b1_field(
          "producer_digest", "sha256",
          test_digest(row.source.workload_id + std::string(":reference")))},
      seal_ordinal - 1U);
  input.rows.push_back(std::move(row));
  input.seal_ordinal = seal_ordinal;
  return materialize_evidence_bundle(std::move(input));
}

/**
 * @brief Adds every embedded section from one row exactly once to a corpus.
 * @param row Materialized row whose sources are authoritative.
 * @param corpus Mutable retained corpus.
 * @return Nothing.
 * @throws std::bad_alloc when section copies allocate.
 */
void retain_row(const EvidenceCanonicalRow& row, EvidenceCorpus* corpus) {
  const auto retain_once = [corpus](const EvidenceRetainedSection& section) {
    const std::string digest = digest_evidence_section(
        section.section_name, section.schema_id, section.bytes);
    const bool exists =
        std::any_of(corpus->sections.begin(), corpus->sections.end(),
                    [&digest](const EvidenceRetainedSection& retained) {
                      return digest_evidence_section(retained.section_name,
                                                     retained.schema_id,
                                                     retained.bytes) == digest;
                    });
    if (!exists) {
      corpus->sections.push_back(section);
    }
  };
  retain_once(row.source.workload_manifest);
  retain_once(row.job_instance_index);
  retain_once(row.source.measurement_evidence);
  retain_once(row.source.output_evidence);
  retain_once(row.source.verdict_evidence);
  corpus->rows.push_back(row);
}

/**
 * @brief Adds one bundle and its provenance after its rows are retained.
 * @param bundle Materialized bundle.
 * @param corpus Mutable retained corpus.
 * @return Nothing.
 * @throws std::bad_alloc when object copies allocate.
 */
void retain_bundle(const EvidenceCanonicalBundle& bundle,
                   EvidenceCorpus* corpus) {
  const std::string provenance_digest = digest_evidence_section(
      bundle.source.provenance.section_name, bundle.source.provenance.schema_id,
      bundle.source.provenance.bytes);
  const bool exists = std::any_of(
      corpus->sections.begin(), corpus->sections.end(),
      [&provenance_digest](const EvidenceRetainedSection& retained) {
        return digest_evidence_section(retained.section_name,
                                       retained.schema_id,
                                       retained.bytes) == provenance_digest;
      });
  if (!exists) {
    corpus->sections.push_back(bundle.source.provenance);
  }
  corpus->bundles.push_back(bundle);
}

/**
 * @brief Complete valid reference-role M1 corpus fixture.
 * @throws std::bad_alloc and canonical/environment failures unchanged.
 */
struct M1EnvelopeFixture final {
  /** @brief All exact-one retained nodes. */
  EvidenceCorpus corpus;
  /** @brief Root M1 bundle digest. */
  std::string root_digest;
  /** @brief Root M1 canonical row digest. */
  std::string m1_row_digest;
};

/**
 * @brief Builds isolated I1/B1 prerequisites followed by one M1 root.
 * @return Complete acyclic same-role/same-ordinal corpus.
 * @throws Environment, envelope, digest, and allocation failures unchanged.
 */
M1EnvelopeFixture make_m1_fixture() {
  B1EnvironmentEvidence m1 = testing::make_b1_test_environment(8U, 1U);
  B1EnvironmentEvidence b1 = m1;
  m1.workload_id = kM1WorkloadId;
  B1EnvironmentEvidence i1 = make_i1_environment(m1);

  const EvidenceCanonicalRow i1_row =
      make_row(std::move(i1), EvidenceSubjectRole::Reference, 1U);
  const EvidenceCanonicalBundle i1_bundle = make_reference_bundle(i1_row, 8U);
  const EvidenceCanonicalRow b1_row =
      make_row(std::move(b1), EvidenceSubjectRole::Reference, 10U);
  const EvidenceCanonicalBundle b1_bundle = make_reference_bundle(b1_row, 17U);

  const EvidencePairReference i1_pair{i1_row.digest, i1_bundle.digest, 1U};
  const EvidencePairReference b1_pair{b1_row.digest, b1_bundle.digest, 1U};
  const EvidenceCanonicalRow m1_row = make_row(
      std::move(m1), EvidenceSubjectRole::Reference, 20U, i1_pair, b1_pair);
  const EvidenceCanonicalBundle m1_bundle = make_reference_bundle(m1_row, 27U);

  M1EnvelopeFixture fixture;
  retain_row(i1_row, &fixture.corpus);
  retain_bundle(i1_bundle, &fixture.corpus);
  retain_row(b1_row, &fixture.corpus);
  retain_bundle(b1_bundle, &fixture.corpus);
  retain_row(m1_row, &fixture.corpus);
  retain_bundle(m1_bundle, &fixture.corpus);
  fixture.root_digest = m1_bundle.digest;
  fixture.m1_row_digest = m1_row.digest;
  return fixture;
}

/**
 * @brief Proves deterministic exact bytes, domains, parsing, and M1 pairing.
 * @throws GoogleTest assertion control and fixture construction failures.
 */
TEST(EvidenceEnvelope, MaterializesCanonicalM1RowAndBundle) {
  const M1EnvelopeFixture fixture = make_m1_fixture();
  const EvidenceCorpusValidation result =
      validate_evidence_corpus(fixture.corpus, fixture.root_digest);
  EXPECT_EQ(result.verdict, I1Verdict::Pass)
      << (result.reasons.empty() ? "no diagnostic" : result.reasons.front());
  EXPECT_TRUE(result.reasons.empty());
  ASSERT_EQ(fixture.corpus.bundles.size(), 3U);
  const EvidenceCanonicalBundle& root = fixture.corpus.bundles.back();
  const EvidenceParsedBundle parsed_bundle =
      parse_evidence_bundle(root.manifest_bytes);
  ASSERT_EQ(parsed_bundle.row_references.size(), 1U);
  EXPECT_EQ(parsed_bundle.row_references.front().row_digest,
            fixture.m1_row_digest);
  const EvidenceParsedRow parsed_row =
      parse_evidence_row(fixture.corpus.rows.back().manifest_bytes);
  EXPECT_EQ(parsed_row.workload_id, kM1WorkloadId);
  EXPECT_EQ(parsed_row.section_digests.size(), 5U);
  ASSERT_TRUE(parsed_row.paired_isolated_i1.has_value());
  ASSERT_TRUE(parsed_row.paired_isolated_b1_cap8.has_value());
  EXPECT_EQ(digest_evidence_row(fixture.corpus.rows.back().manifest_bytes),
            fixture.m1_row_digest);
  EXPECT_EQ(digest_evidence_bundle(root.manifest_bytes), fixture.root_digest);
  EXPECT_EQ(fixture.m1_row_digest,
            "3a08a8db6cf2e721e25e340e41d39e58452f92488c84caef73339ac7bd75ccdd");
  EXPECT_EQ(fixture.root_digest,
            "a81466b1df82f35d3f2e361e6353bb724159545a6cdbf2239131b7dfa6dba86a");
  EXPECT_EQ(
      digest_evidence_section(
          fixture.corpus.rows.back().source.workload_manifest.section_name,
          fixture.corpus.rows.back().source.workload_manifest.schema_id,
          fixture.corpus.rows.back().source.workload_manifest.bytes),
      "562395616c5d14f39805ad65d29f9cac0b299044af7f245aad651a41cb5b6d8d");
}

/**
 * @brief Proves unknown, reordered, and malformed N/A row fields fail closed.
 * @throws GoogleTest assertion control and canonical parsing failures.
 */
TEST(EvidenceEnvelope, RejectsClosedRowSchemaDrift) {
  const M1EnvelopeFixture fixture = make_m1_fixture();
  const std::string& bytes = fixture.corpus.rows.front().manifest_bytes;
  B1CanonicalManifest reordered = parse_b1_canonical_manifest(bytes);
  std::swap(reordered.fields[0U], reordered.fields[1U]);
  EXPECT_THROW(parse_evidence_row(encode_b1_canonical_manifest(
                   reordered.schema, reordered.fields)),
               std::invalid_argument);

  B1CanonicalManifest malformed_na = parse_b1_canonical_manifest(bytes);
  malformed_na.fields[6U].reason = "none";
  EXPECT_THROW(parse_evidence_row(encode_b1_canonical_manifest(
                   malformed_na.schema, malformed_na.fields)),
               std::invalid_argument);

  B1CanonicalManifest unknown = parse_b1_canonical_manifest(bytes);
  unknown.fields.push_back(
      testing::known_b1_field("extension", "sha256", test_digest("extension")));
  EXPECT_THROW(parse_evidence_row(encode_b1_canonical_manifest(unknown.schema,
                                                               unknown.fields)),
               std::invalid_argument);
}

/**
 * @brief Proves exact-one object and section resolution rejects ambiguity.
 * @throws GoogleTest assertion control and fixture construction failures.
 */
TEST(EvidenceEnvelope, RejectsMissingAndMultiplyRetainedObjects) {
  M1EnvelopeFixture fixture = make_m1_fixture();
  fixture.corpus.rows.push_back(fixture.corpus.rows.back());
  EXPECT_EQ(
      validate_evidence_corpus(fixture.corpus, fixture.root_digest).verdict,
      I1Verdict::Invalid);

  fixture = make_m1_fixture();
  fixture.corpus.sections.pop_back();
  EXPECT_EQ(
      validate_evidence_corpus(fixture.corpus, fixture.root_digest).verdict,
      I1Verdict::Invalid);

  fixture = make_m1_fixture();
  fixture.corpus.sections.push_back(fixture.corpus.sections.front());
  EXPECT_EQ(
      validate_evidence_corpus(fixture.corpus, fixture.root_digest).verdict,
      I1Verdict::Invalid);
}

/**
 * @brief Proves rehash and global pair sealing cannot be bypassed by sidecars.
 * @throws GoogleTest assertion control and fixture construction failures.
 */
TEST(EvidenceEnvelope, RejectsTamperingAndLaterPairTargets) {
  M1EnvelopeFixture fixture = make_m1_fixture();
  fixture.corpus.rows.back().manifest_bytes.push_back('\n');
  EXPECT_EQ(
      validate_evidence_corpus(fixture.corpus, fixture.root_digest).verdict,
      I1Verdict::Invalid);

  fixture = make_m1_fixture();
  fixture.corpus.rows.back().source.seal_ordinal = 7U;
  EXPECT_EQ(
      validate_evidence_corpus(fixture.corpus, fixture.root_digest).verdict,
      I1Verdict::Invalid);
}

/**
 * @brief Proves candidate comparison uses exact functional rows and
 * environment.
 * @throws GoogleTest assertion control and fixture construction failures.
 */
TEST(EvidenceEnvelope, ResolvesCandidateReferenceByFunctionalKey) {
  B1EnvironmentEvidence reference_environment =
      make_i1_environment(testing::make_b1_test_environment(8U, 2U));
  B1EnvironmentEvidence candidate_environment = reference_environment;
  const EvidenceCanonicalRow reference_row = make_row(
      std::move(reference_environment), EvidenceSubjectRole::Reference, 1U);
  const EvidenceCanonicalBundle reference_bundle =
      make_reference_bundle(reference_row, 8U);
  const EvidenceCanonicalRow candidate_row = make_row(
      std::move(candidate_environment), EvidenceSubjectRole::Candidate, 10U);

  EvidenceBundleInput candidate_input;
  candidate_input.workload_id = kI1WorkloadId;
  candidate_input.subject_role = EvidenceSubjectRole::Candidate;
  candidate_input.provenance =
      make_section("bundle-provenance", kEvidenceBundleProvenanceSchema,
                   {testing::known_b1_field("producer_digest", "sha256",
                                            test_digest("candidate-producer"))},
                   16U);
  candidate_input.comparison_reference_bundle_digest = reference_bundle.digest;
  candidate_input.rows.push_back(candidate_row);
  candidate_input.seal_ordinal = 17U;
  const EvidenceCanonicalBundle candidate_bundle =
      materialize_evidence_bundle(std::move(candidate_input));

  EvidenceCorpus corpus;
  retain_row(reference_row, &corpus);
  retain_bundle(reference_bundle, &corpus);
  retain_row(candidate_row, &corpus);
  retain_bundle(candidate_bundle, &corpus);
  const EvidenceCorpusValidation comparison =
      validate_evidence_corpus(corpus, candidate_bundle.digest);
  EXPECT_EQ(comparison.verdict, I1Verdict::Pass)
      << (comparison.reasons.empty() ? "no diagnostic"
                                     : comparison.reasons.front());

  corpus.rows.front().source.environment.fixture_digest =
      b1_sha256("drifted-fixture");
  EXPECT_EQ(validate_evidence_corpus(corpus, candidate_bundle.digest).verdict,
            I1Verdict::Invalid);
}

/**
 * @brief Proves one bundle cannot contain duplicate functional row keys.
 * @throws GoogleTest assertion control and fixture construction failures.
 */
TEST(EvidenceEnvelope, RejectsDuplicateBundleFunctionalKeys) {
  B1EnvironmentEvidence environment =
      make_i1_environment(testing::make_b1_test_environment(8U, 3U));
  EvidenceCanonicalRow first =
      make_row(environment, EvidenceSubjectRole::Reference, 1U);
  EvidenceCanonicalRow second =
      make_row(std::move(environment), EvidenceSubjectRole::Reference, 10U);
  second.source.measurement_evidence.bytes = encode_b1_canonical_manifest(
      "execution-profile-measurement-evidence-v1",
      {testing::known_b1_field("raw_digest", "sha256",
                               test_digest("different-measurement"))});
  second = materialize_evidence_row(std::move(second.source));

  EvidenceBundleInput input;
  input.workload_id = kI1WorkloadId;
  input.subject_role = EvidenceSubjectRole::Reference;
  input.provenance =
      make_section("bundle-provenance", kEvidenceBundleProvenanceSchema,
                   {testing::known_b1_field("producer_digest", "sha256",
                                            test_digest("producer"))},
                   20U);
  input.rows = {std::move(first), std::move(second)};
  input.seal_ordinal = 21U;
  EXPECT_THROW(materialize_evidence_bundle(std::move(input)),
               std::invalid_argument);
}

/**
 * @brief Proves required-storage claims remain serializable but cannot Pass
 * after actual authority is removed.
 * @throws GoogleTest assertion control and fixture construction failures.
 */
TEST(EvidenceEnvelope, MissingMachineAuthorityMaterializesOnlyInvalidCorpus) {
  M1EnvelopeFixture fixture = make_m1_fixture();
  EvidenceRowInput invalid_source = fixture.corpus.rows.back().source;
  invalid_source.environment.storage_actual_observation.reset();

  const EvidenceCanonicalRow invalid_row =
      materialize_evidence_row(std::move(invalid_source));
  EXPECT_EQ(invalid_row.digest, fixture.m1_row_digest);
  fixture.corpus.rows.back() = invalid_row;

  const EvidenceCorpusValidation validation =
      validate_evidence_corpus(fixture.corpus, fixture.root_digest);
  EXPECT_EQ(validation.verdict, I1Verdict::Invalid);
  ASSERT_EQ(validation.reasons.size(), 1U);
  EXPECT_EQ(validation.reasons.front(),
            "Retained evidence row lacks valid actual environment authority.");
}

}  // namespace
}  // namespace ps::benchmark
