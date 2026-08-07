/**
 * @file test_evidence_envelope.cpp
 * @brief Verifies the closed execution-profile row/bundle evidence envelope.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/evidence_envelope.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/m1_evidence.hpp"        // NOLINT(build/include_subdir)
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
 * @brief Encodes one canonical generic framed list for envelope fixtures.
 * @param records Complete records in authoritative order.
 * @return Count prefix plus one frame per record.
 * @throws std::bad_alloc when output ownership allocates.
 */
std::string encode_test_record_list(const std::vector<std::string>& records) {
  std::string payload = std::to_string(records.size()) + ":";
  for (const std::string& record : records) {
    payload.append(b1_environment_frame(record));
  }
  return payload;
}

/**
 * @brief Raw isolated sources and M1 claims varied by denominator tests.
 * @throws Nothing for value construction and copying.
 */
struct M1EnvelopeDenominatorOptions final {
  /** @brief Value repeated in every retained isolated-I1 sample. */
  std::uint64_t i1_sample_ns = 10U;
  /** @brief Number of retained isolated-I1 raw latency samples. */
  std::size_t i1_sample_count = 200U;
  /** @brief Isolated-I1 row-local nearest-rank p99 claim. */
  std::uint64_t i1_claimed_p99_ns = 10U;
  /** @brief Number of retained isolated-B1 raw job outcomes. */
  std::size_t b1_outcome_count = kB1MeasuredJobCount;
  /** @brief Number of retained M1 measured progress windows. */
  std::size_t m1_progress_window_count = kM1MeasuredWindowCount;
  /** @brief Isolated-B1 row-local successful-operation claim. */
  std::uint64_t b1_claimed_successful_operations =
      kB1MeasuredJobCount * kB1SiteOperationsPerJob;
  /** @brief M1 outer isolated-I1 denominator claim. */
  std::uint64_t m1_outer_i1_p99_ns = 10U;
  /** @brief M1 nested isolated-I1 denominator claim. */
  std::uint64_t m1_inner_i1_p99_ns = 10U;
  /** @brief M1 outer isolated-B1 numerator claim. */
  std::uint64_t m1_outer_b1_successful_operations =
      kB1MeasuredJobCount * kB1SiteOperationsPerJob;
  /** @brief M1 nested isolated-B1 numerator claim. */
  std::uint64_t m1_inner_b1_successful_operations =
      kB1MeasuredJobCount * kB1SiteOperationsPerJob;
};

/**
 * @brief Builds exact isolated-I1 raw denominator fields for one replicate.
 * @param role Exact enclosing subject role.
 * @param ordinal Exact enclosing replicate ordinal.
 * @param options Raw sample cardinality/value and row-local claim.
 * @return Four-field raw isolated-I1 measurement source.
 * @throws std::bad_alloc when records or canonical fields allocate.
 */
std::vector<B1CanonicalField> isolated_i1_measurement_fields(
    EvidenceSubjectRole role, std::uint64_t ordinal,
    const M1EnvelopeDenominatorOptions& options) {
  std::vector<std::string> samples;
  samples.assign(
      options.i1_sample_count,
      encode_b1_fixed_record({std::to_string(options.i1_sample_ns)}));
  return {
      testing::known_b1_field("subject_role", "enum",
                              evidence_subject_role_name(role)),
      testing::known_b1_field("replicate_ordinal", "uint64",
                              std::to_string(ordinal)),
      testing::known_b1_field("measured_final_latencies_ns", "uint64-list-v1",
                              encode_test_record_list(samples)),
      testing::known_b1_field("claimed_p99_ns", "uint64",
                              std::to_string(options.i1_claimed_p99_ns))};
}

/**
 * @brief Builds exact isolated-B1 raw denominator fields for one replicate.
 * @param role Exact enclosing subject role.
 * @param ordinal Exact enclosing replicate ordinal.
 * @param options Raw outcome cardinality and row-local numerator claim.
 * @return Raw isolated-B1 outcomes over exactly thirty seconds.
 * @throws std::bad_alloc when records or canonical fields allocate.
 */
std::vector<B1CanonicalField> isolated_b1_measurement_fields(
    EvidenceSubjectRole role, std::uint64_t ordinal,
    const M1EnvelopeDenominatorOptions& options) {
  std::vector<std::string> outcomes;
  for (std::size_t index = 0U; index < options.b1_outcome_count; ++index) {
    outcomes.push_back(
        encode_b1_fixed_record({std::to_string(index), "true",
                                std::to_string(kB1SiteOperationsPerJob)}));
  }
  return {
      testing::known_b1_field("subject_role", "enum",
                              evidence_subject_role_name(role)),
      testing::known_b1_field("replicate_ordinal", "uint64",
                              std::to_string(ordinal)),
      testing::known_b1_field("measurement_start_ns", "uint64", "100"),
      testing::known_b1_field("measurement_end_ns", "uint64", "30000000100"),
      testing::known_b1_field("measured_job_outcomes",
                              "b1-measured-job-outcome-list-v1",
                              encode_test_record_list(outcomes)),
      testing::known_b1_field(
          "successful_site_operations", "uint64",
          std::to_string(options.b1_claimed_successful_operations))};
}

/**
 * @brief Builds a shape-complete M1 inner row with exact denominator claims.
 * @param options Exact nested denominator claims.
 * @return Canonical nested row retaining 48/30/480/raw-stream cardinalities.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string make_test_m1_inner(const M1EnvelopeDenominatorOptions& options) {
  std::vector<std::string> interactive(kM1TotalI1OriginCount,
                                       encode_b1_fixed_record({"occurrence"}));
  std::vector<std::string> offers{encode_b1_fixed_record({"offer"})};
  std::vector<std::string> carryover(3U, encode_b1_fixed_record({"carryover"}));
  std::vector<std::string> progress;
  std::vector<std::string> graph;
  for (std::size_t index = 0U; index < options.m1_progress_window_count;
       ++index) {
    progress.push_back(encode_b1_fixed_record(
        {std::to_string(index), "200000", "1000000000"}));
    graph.push_back(
        encode_b1_fixed_record({std::to_string(index), "true", "1", "1"}));
  }
  std::vector<std::string> headroom;
  for (std::size_t origin = 0U; origin < kM1MeasuredI1OriginCount; ++origin) {
    for (std::size_t edit = 0U; edit < kI1EditCount; ++edit) {
      headroom.push_back(encode_b1_fixed_record(
          {std::to_string(origin), std::to_string(edit), "true", "true", "true",
           "0", "0", "", "", "false"}));
    }
  }
  const std::vector<std::string> io{encode_b1_fixed_record({"io"})};
  const std::vector<std::string> snapshots(
      4U, encode_b1_fixed_record({"snapshot"}));
  return encode_b1_canonical_manifest(
      kM1InnerRowSchema,
      {testing::known_b1_field("schema_version", "uint64", "1"),
       testing::known_b1_field("replicate_ordinal", "uint64", "1"),
       testing::known_b1_field("boundaries", "m1-boundary-record-v1",
                               encode_b1_fixed_record({"boundaries"})),
       testing::known_b1_field(
           "protocol_flags", "m1-protocol-flags-v1",
           encode_b1_fixed_record({"true", "true", "true", "true", "true",
                                   "true", "true", "true", "false", "false",
                                   "false"})),
       testing::known_b1_field("interactive_occurrences",
                               "m1-i1-occurrence-list-v1",
                               encode_test_record_list(interactive)),
       testing::known_b1_field("batch_offers", "m1-b1-offer-list-v1",
                               encode_test_record_list(offers)),
       testing::known_b1_field("carryover", "m1-carryover-list-v1",
                               encode_test_record_list(carryover)),
       testing::known_b1_field("first_measured_admission",
                               "m1-first-admission-record-v1",
                               encode_b1_fixed_record({"first"})),
       testing::known_b1_field("progress_windows", "m1-progress-window-list-v1",
                               encode_test_record_list(progress)),
       testing::known_b1_field("graph_service_windows",
                               "m1-graph-service-window-list-v1",
                               encode_test_record_list(graph)),
       testing::known_b1_field("class_starts", "m1-class-start-list-v1",
                               encode_test_record_list({encode_b1_fixed_record(
                                   {"1", "0", "true", "true", "true"})})),
       testing::known_b1_field("headroom_outcomes",
                               "m1-headroom-outcome-list-v1",
                               encode_test_record_list(headroom)),
       testing::known_b1_field("batch_io_streams", "m1-b1-io-stream-list-v1",
                               encode_test_record_list(io)),
       testing::known_b1_field("temporal_snapshots",
                               "m1-execution-snapshot-list-v1",
                               encode_test_record_list(snapshots)),
       testing::known_b1_field(
           "mixed_observations", "m1-observation-list-v1",
           encode_test_record_list({encode_b1_fixed_record({"event"})})),
       testing::known_b1_field("paired_isolated_i1_p99_ns", "uint64",
                               std::to_string(options.m1_inner_i1_p99_ns)),
       testing::known_b1_field(
           "paired_isolated_b1_source", "m1-b1-rate-source-v1",
           encode_b1_fixed_record(
               {std::to_string(options.m1_inner_b1_successful_operations),
                "30000000000"})),
       testing::known_b1_field("batch_waste", "m1-batch-waste-record-v1",
                               encode_b1_fixed_record({"waste"})),
       testing::known_b1_field("verdicts", "m1-five-axis-verdict-record-v1",
                               encode_b1_fixed_record({"pass"}))});
}

/**
 * @brief Builds exact workload-specific measurement source fields.
 * @param workload Frozen row workload token.
 * @param role Candidate/reference role for generic I2 fixture identity.
 * @param ordinal Replicate ordinal for generic I2 fixture identity.
 * @param options Raw denominator sources and M1 claims.
 * @return Closed raw/claim fields for I1, B1, M1, or the unaffected I2 stub.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::vector<B1CanonicalField> measurement_fields(
    std::string_view workload, EvidenceSubjectRole role, std::uint64_t ordinal,
    const M1EnvelopeDenominatorOptions& options) {
  if (workload == kI1WorkloadId) {
    return isolated_i1_measurement_fields(role, ordinal, options);
  }
  if (workload == kB1WorkloadId) {
    return isolated_b1_measurement_fields(role, ordinal, options);
  }
  if (workload == kM1WorkloadId) {
    return {testing::known_b1_field(
                "m1_inner_row", "canonical-text-hex-v1",
                encode_b1_normalized_text(make_test_m1_inner(options))),
            testing::known_b1_field("paired_isolated_i1_p99_ns", "uint64",
                                    std::to_string(options.m1_outer_i1_p99_ns)),
            testing::known_b1_field(
                "paired_isolated_b1_successful_site_operations", "uint64",
                std::to_string(options.m1_outer_b1_successful_operations)),
            testing::known_b1_field("paired_isolated_b1_duration_ns", "uint64",
                                    "30000000000")};
  }
  return {testing::known_b1_field(
      "raw_digest", "sha256",
      test_digest(std::string(workload) +
                  ":measurement:" + evidence_subject_role_name(role) + ":" +
                  std::to_string(ordinal)))};
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
 * @param denominators Raw denominator sources and M1 claims.
 * @return Canonical row suitable for bundle/corpus tests.
 * @throws Envelope, environment, canonical, and allocation failures unchanged.
 */
EvidenceCanonicalRow make_row(
    B1EnvironmentEvidence environment, EvidenceSubjectRole role,
    std::uint64_t first_seal,
    std::optional<EvidencePairReference> i1_pair = std::nullopt,
    std::optional<EvidencePairReference> b1_pair = std::nullopt,
    const M1EnvelopeDenominatorOptions& denominators =
        M1EnvelopeDenominatorOptions{}) {
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
      measurement_fields(workload, role, ordinal, denominators),
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
 * @param denominators Raw isolated sources and exact M1 claims.
 * @return Complete acyclic same-role/same-ordinal corpus.
 * @throws Environment, envelope, digest, and allocation failures unchanged.
 */
M1EnvelopeFixture make_m1_fixture(
    const M1EnvelopeDenominatorOptions& denominators =
        M1EnvelopeDenominatorOptions{}) {
  B1EnvironmentEvidence m1 = testing::make_b1_test_environment(8U, 1U);
  B1EnvironmentEvidence b1 = m1;
  m1.workload_id = kM1WorkloadId;
  B1EnvironmentEvidence i1 = make_i1_environment(m1);

  const EvidenceCanonicalRow i1_row =
      make_row(std::move(i1), EvidenceSubjectRole::Reference, 1U, std::nullopt,
               std::nullopt, denominators);
  const EvidenceCanonicalBundle i1_bundle = make_reference_bundle(i1_row, 8U);
  const EvidenceCanonicalRow b1_row =
      make_row(std::move(b1), EvidenceSubjectRole::Reference, 10U, std::nullopt,
               std::nullopt, denominators);
  const EvidenceCanonicalBundle b1_bundle = make_reference_bundle(b1_row, 17U);

  const EvidencePairReference i1_pair{i1_row.digest, i1_bundle.digest, 1U};
  const EvidencePairReference b1_pair{b1_row.digest, b1_bundle.digest, 1U};
  const EvidenceCanonicalRow m1_row =
      make_row(std::move(m1), EvidenceSubjectRole::Reference, 20U, i1_pair,
               b1_pair, denominators);
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
            "6d7dff5804fd8d5774c6b6a5e32d02e3a019830877db7f538ac5e04d6da9b5f6");
  EXPECT_EQ(fixture.root_digest,
            "ccee55716d612497e216326d4e98f30411aa1a64705e269ec400b8ead4d975a2");
  EXPECT_EQ(
      digest_evidence_section(
          fixture.corpus.rows.back().source.workload_manifest.section_name,
          fixture.corpus.rows.back().source.workload_manifest.schema_id,
          fixture.corpus.rows.back().source.workload_manifest.bytes),
      "562395616c5d14f39805ad65d29f9cac0b299044af7f245aad651a41cb5b6d8d");
}

/**
 * @brief Proves native isolated objects survive pack round-trip and bind M1
 * denominators from their retained raw measurement sections.
 * @throws GoogleTest assertion control and fixture construction failures.
 */
TEST(EvidenceEnvelope, RoundTripsAndBindsNativePairObjects) {
  const M1EnvelopeFixture fixture = make_m1_fixture();
  const EvidencePairObject i1{fixture.corpus.rows[0U],
                              fixture.corpus.bundles[0U]};
  const EvidencePairObject b1{fixture.corpus.rows[1U],
                              fixture.corpus.bundles[1U]};
  const std::string i1_pack = materialize_evidence_pair_object(i1);
  const std::string b1_pack = materialize_evidence_pair_object(b1);
  const EvidencePairObject loaded_i1 =
      load_evidence_pair_object(i1_pack, i1.row.digest, i1.bundle.digest);
  const EvidencePairObject loaded_b1 =
      load_evidence_pair_object(b1_pack, b1.row.digest, b1.bundle.digest);
  EXPECT_EQ(loaded_i1.row.digest, i1.row.digest);
  EXPECT_EQ(loaded_i1.bundle.digest, i1.bundle.digest);
  EXPECT_EQ(loaded_b1.row.digest, b1.row.digest);
  EXPECT_EQ(loaded_b1.bundle.digest, b1.bundle.digest);
  EXPECT_TRUE(valid_b1_environment_claims(loaded_b1.row.source.environment));
  EXPECT_FALSE(valid_b1_environment_evidence(loaded_b1.row.source.environment));

  const EvidenceM1PairDenominators denominators =
      validate_evidence_m1_pair_objects(
          loaded_i1, loaded_b1, EvidenceSubjectRole::Reference, 1U,
          fixture.corpus.rows[2U].source.environment,
          parse_b1_digest(test_digest("i1-fixture")),
          EvidenceB1ComponentDigests{
              parse_b1_digest(test_digest("b1-fixture")),
              parse_b1_digest(test_digest("b1-corpus")),
              parse_b1_digest(test_digest("b1-golden"))});
  EXPECT_EQ(denominators.isolated_i1_p99_ns, 10U);
  EXPECT_EQ(denominators.isolated_b1_successful_site_operations,
            kB1MeasuredJobCount * kB1SiteOperationsPerJob);
  EXPECT_EQ(denominators.isolated_b1_duration_ns, 30000000000U);

  EvidenceCorpus retained;
  EXPECT_NO_THROW(append_evidence_pair_object(loaded_i1, &retained));
  EXPECT_NO_THROW(append_evidence_pair_object(loaded_b1, &retained));
  EXPECT_EQ(retained.rows.size(), 2U);
  EXPECT_EQ(retained.bundles.size(), 2U);
  EXPECT_THROW(append_evidence_pair_object(loaded_i1, &retained),
               std::invalid_argument);
}

/**
 * @brief Proves digest-only, digest/object mismatch, source tamper, and missing
 * or duplicate sections all fail before an M1 timed row can begin.
 * @throws GoogleTest assertion control and canonical fixture failures.
 */
TEST(EvidenceEnvelope, RejectsIncompleteOrTamperedPairPacks) {
  const M1EnvelopeFixture fixture = make_m1_fixture();
  const EvidencePairObject i1{fixture.corpus.rows[0U],
                              fixture.corpus.bundles[0U]};
  const std::string pack = materialize_evidence_pair_object(i1);
  EXPECT_THROW(load_evidence_pair_object("", i1.row.digest, i1.bundle.digest),
               std::invalid_argument);
  EXPECT_THROW(load_evidence_pair_object(pack, test_digest("wrong-row"),
                                         i1.bundle.digest),
               std::invalid_argument);
  EXPECT_THROW(load_evidence_pair_object(pack, i1.row.digest,
                                         test_digest("wrong-bundle")),
               std::invalid_argument);

  B1CanonicalManifest tampered = parse_b1_canonical_manifest(pack);
  tampered.fields[2U].payload.push_back('0');
  EXPECT_THROW(load_evidence_pair_object(encode_b1_canonical_manifest(
                                             tampered.schema, tampered.fields),
                                         i1.row.digest, i1.bundle.digest),
               std::invalid_argument);

  B1CanonicalManifest missing = parse_b1_canonical_manifest(pack);
  std::vector<std::string> records =
      parse_b1_framed_list(missing.fields[12U].payload);
  records.pop_back();
  missing.fields[12U].payload = encode_test_record_list(records);
  EXPECT_THROW(load_evidence_pair_object(
                   encode_b1_canonical_manifest(missing.schema, missing.fields),
                   i1.row.digest, i1.bundle.digest),
               std::invalid_argument);

  B1CanonicalManifest duplicate = parse_b1_canonical_manifest(pack);
  records = parse_b1_framed_list(duplicate.fields[12U].payload);
  records[1U] = records[0U];
  duplicate.fields[12U].payload = encode_test_record_list(records);
  EXPECT_THROW(
      load_evidence_pair_object(
          encode_b1_canonical_manifest(duplicate.schema, duplicate.fields),
          i1.row.digest, i1.bundle.digest),
      std::invalid_argument);

  B1CanonicalManifest reordered = parse_b1_canonical_manifest(pack);
  records = parse_b1_framed_list(reordered.fields[12U].payload);
  std::swap(records[0U], records[1U]);
  reordered.fields[12U].payload = encode_test_record_list(records);
  EXPECT_THROW(
      load_evidence_pair_object(
          encode_b1_canonical_manifest(reordered.schema, reordered.fields),
          i1.row.digest, i1.bundle.digest),
      std::invalid_argument);
}

/**
 * @brief Proves role/ordinal mismatch and unsafe file paths fail independently
 * of canonical object bytes.
 * @throws GoogleTest assertion control, fixture, and temporary-file failures.
 */
TEST(EvidenceEnvelope, RejectsWrongPairIdentityAndUnsafeInputPaths) {
  const M1EnvelopeFixture fixture = make_m1_fixture();
  const EvidencePairObject i1{fixture.corpus.rows[0U],
                              fixture.corpus.bundles[0U]};
  const EvidencePairObject b1{fixture.corpus.rows[1U],
                              fixture.corpus.bundles[1U]};
  EXPECT_THROW(validate_evidence_m1_pair_objects(
                   i1, b1, EvidenceSubjectRole::Candidate, 1U,
                   fixture.corpus.rows[2U].source.environment,
                   parse_b1_digest(test_digest("i1-fixture")),
                   EvidenceB1ComponentDigests{
                       parse_b1_digest(test_digest("b1-fixture")),
                       parse_b1_digest(test_digest("b1-corpus")),
                       parse_b1_digest(test_digest("b1-golden"))}),
               std::invalid_argument);
  EXPECT_THROW(validate_evidence_m1_pair_objects(
                   i1, b1, EvidenceSubjectRole::Reference, 2U,
                   fixture.corpus.rows[2U].source.environment,
                   parse_b1_digest(test_digest("i1-fixture")),
                   EvidenceB1ComponentDigests{
                       parse_b1_digest(test_digest("b1-fixture")),
                       parse_b1_digest(test_digest("b1-corpus")),
                       parse_b1_digest(test_digest("b1-golden"))}),
               std::invalid_argument);
  EXPECT_THROW(read_evidence_pair_object_file("relative-pair-object"),
               std::invalid_argument);

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("photospider-pair-object-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  ASSERT_TRUE(std::filesystem::create_directory(root));
  EXPECT_THROW(read_evidence_pair_object_file(root), std::invalid_argument);
  const std::filesystem::path empty = root / "empty.canonical";
  {
    std::ofstream output(empty, std::ios::binary);
  }
  EXPECT_THROW(read_evidence_pair_object_file(empty), std::invalid_argument);
  const std::filesystem::path oversized = root / "oversized.canonical";
  {
    std::ofstream output(oversized, std::ios::binary);
    output.put('x');
  }
  std::filesystem::resize_file(oversized, kEvidencePairObjectMaxBytes + 1U);
  EXPECT_THROW(read_evidence_pair_object_file(oversized),
               std::invalid_argument);
  const std::filesystem::path regular = root / "pair-object.canonical";
  {
    std::ofstream output(regular, std::ios::binary);
    output << materialize_evidence_pair_object(i1);
  }
  EXPECT_EQ(read_evidence_pair_object_file(regular),
            materialize_evidence_pair_object(i1));
#if !defined(_WIN32)
  const std::filesystem::path symlink = root / "pair-object-link.canonical";
  std::filesystem::create_symlink(regular, symlink);
  EXPECT_THROW(read_evidence_pair_object_file(symlink), std::invalid_argument);
#endif
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves a valid but different isolated-I1 source cannot substitute for
 * the M1 denominator named by the canonical pair.
 * @throws GoogleTest assertion control and fixture construction failures.
 */
TEST(EvidenceEnvelope, RejectsSubstitutedIsolatedI1Denominator) {
  M1EnvelopeDenominatorOptions denominators;
  denominators.i1_sample_ns = 11U;
  denominators.i1_claimed_p99_ns = 11U;
  const M1EnvelopeFixture fixture = make_m1_fixture(denominators);

  const EvidenceCorpusValidation validation =
      validate_evidence_corpus(fixture.corpus, fixture.root_digest);
  EXPECT_EQ(validation.verdict, I1Verdict::Invalid);
  ASSERT_EQ(validation.reasons.size(), 1U);
  EXPECT_EQ(validation.reasons.front(),
            "M1 isolated-I1 denominator differs from resolved raw evidence.");
}

/**
 * @brief Proves omission of one isolated-B1 raw outcome invalidates the M1
 * pair even when all row and bundle addresses are consistently rebuilt.
 * @throws GoogleTest assertion control and fixture construction failures.
 */
TEST(EvidenceEnvelope, RejectsOmittedIsolatedB1Outcome) {
  M1EnvelopeDenominatorOptions denominators;
  denominators.b1_outcome_count = kB1MeasuredJobCount - 1U;
  denominators.b1_claimed_successful_operations =
      denominators.b1_outcome_count * kB1SiteOperationsPerJob;
  const M1EnvelopeFixture fixture = make_m1_fixture(denominators);

  const EvidenceCorpusValidation validation =
      validate_evidence_corpus(fixture.corpus, fixture.root_digest);
  EXPECT_EQ(validation.verdict, I1Verdict::Invalid);
  ASSERT_EQ(validation.reasons.size(), 1U);
  EXPECT_EQ(validation.reasons.front(),
            "Paired isolated-B1 requires exactly 30 raw job outcomes.");
}

/**
 * @brief Proves omission of one M1 raw progress window fails even when the
 * nested row and all enclosing content addresses are consistently rebuilt.
 * @throws GoogleTest assertion control and fixture construction failures.
 */
TEST(EvidenceEnvelope, RejectsOmittedM1RawProgressWindow) {
  M1EnvelopeDenominatorOptions denominators;
  denominators.m1_progress_window_count = kM1MeasuredWindowCount - 1U;
  const M1EnvelopeFixture fixture = make_m1_fixture(denominators);

  const EvidenceCorpusValidation validation =
      validate_evidence_corpus(fixture.corpus, fixture.root_digest);
  EXPECT_EQ(validation.verdict, I1Verdict::Invalid);
  ASSERT_EQ(validation.reasons.size(), 1U);
  EXPECT_EQ(validation.reasons.front(),
            "M1 nested raw evidence cardinality drifted.");
}

/**
 * @brief Proves M1 claim tampering and a self-consistent false B1 denominator
 * both fail after all enclosing content addresses are rebuilt.
 * @throws GoogleTest assertion control and fixture construction failures.
 */
TEST(EvidenceEnvelope, RejectsM1DenominatorClaimTamperingAndMismatch) {
  M1EnvelopeDenominatorOptions denominators;
  denominators.m1_outer_i1_p99_ns = 11U;
  M1EnvelopeFixture fixture = make_m1_fixture(denominators);
  EvidenceCorpusValidation validation =
      validate_evidence_corpus(fixture.corpus, fixture.root_digest);
  EXPECT_EQ(validation.verdict, I1Verdict::Invalid);
  ASSERT_EQ(validation.reasons.size(), 1U);
  EXPECT_EQ(validation.reasons.front(),
            "M1 inner and measurement denominator claims disagree.");

  denominators = M1EnvelopeDenominatorOptions{};
  denominators.m1_outer_b1_successful_operations -= kB1SiteOperationsPerJob;
  denominators.m1_inner_b1_successful_operations =
      denominators.m1_outer_b1_successful_operations;
  fixture = make_m1_fixture(denominators);
  validation = validate_evidence_corpus(fixture.corpus, fixture.root_digest);
  EXPECT_EQ(validation.verdict, I1Verdict::Invalid);
  ASSERT_EQ(validation.reasons.size(), 1U);
  EXPECT_EQ(validation.reasons.front(),
            "M1 isolated-B1 denominator differs from resolved raw evidence.");
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
