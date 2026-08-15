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
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "benchmark/common/evidence_envelope.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/m1/m1_canonical.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/m1/m1_evidence.hpp"   // NOLINT(build/include_subdir)
#include "benchmark/m1/m1_profile.hpp"    // NOLINT(build/include_subdir)
#include "support/b1_test_environment.hpp"
#include "support/m1_test_evidence.hpp"

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
  /** @brief Whether nested M1 sources model equal-time supersession at B. */
  bool m1_equal_time_supersession = false;
  /** @brief Optional test-only rewrite applied before outer addressing. */
  std::function<std::string(std::string)> rewrite_m1_inner;
};

/**
 * @brief Builds exact isolated-I1 raw denominator fields for one replicate.
 * @param role Exact enclosing subject role.
 * @param ordinal Exact enclosing replicate ordinal.
 * @param options Raw sample cardinality/value and row-local claim.
 * @return Five-field raw isolated-I1 denominator source.
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
      testing::known_b1_field("pair_source_schema", "identifier",
                              kEvidenceI1PairDenominatorSchema),
      testing::known_b1_field("subject_role", "enum",
                              evidence_subject_role_name(role)),
      testing::known_b1_field("replicate_ordinal", "uint64",
                              std::to_string(ordinal)),
      testing::known_b1_field("source_inner_schema_version", "uint64",
                              std::to_string(kI1InnerRowSchemaVersion)),
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
      testing::known_b1_field("pair_source_schema", "identifier",
                              kEvidenceB1PairDenominatorSchema),
      testing::known_b1_field("subject_role", "enum",
                              evidence_subject_role_name(role)),
      testing::known_b1_field("replicate_ordinal", "uint64",
                              std::to_string(ordinal)),
      testing::known_b1_field("source_inner_schema_version", "uint64",
                              std::to_string(kB1InnerRowSchemaVersion)),
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
  M1InnerRowInput input;
  input.replicate_ordinal = 1U;
  input.protocol.replicate_ordinal = 1U;
  const auto measurement_start =
      std::chrono::steady_clock::time_point(std::chrono::seconds(100));
  const M1Timeline timeline = derive_m1_timeline(measurement_start);
  input.protocol.boundaries =
      M1BoundaryEvidence{{timeline.cold_start, 1U},
                         {timeline.warmup_start, 2U},
                         {timeline.measurement_start, 3U},
                         {timeline.measurement_end, 4U}};
  for (std::size_t index = 0U; index < kM1TotalI1OriginCount; ++index) {
    B1JobPhase phase = B1JobPhase::Measured;
    std::size_t ordinal = index - 1U - kM1WarmupI1OriginCount;
    if (index == 0U) {
      phase = B1JobPhase::Cold;
      ordinal = 0U;
    } else if (index <= kM1WarmupI1OriginCount) {
      phase = B1JobPhase::Warmup;
      ordinal = index - 1U;
    }
    std::chrono::steady_clock::time_point origin = timeline.cold_start;
    if (phase == B1JobPhase::Warmup) {
      origin = checked_i1_time_add(
          timeline.warmup_start,
          std::chrono::nanoseconds(static_cast<std::int64_t>(ordinal) *
                                   kI1EpisodeStride.count()));
    } else if (phase == B1JobPhase::Measured) {
      origin = checked_i1_time_add(
          timeline.measurement_start,
          std::chrono::nanoseconds(static_cast<std::int64_t>(ordinal) *
                                   kI1EpisodeStride.count()));
    }
    input.protocol.interactive_occurrences.push_back(
        M1InteractiveOccurrenceEvidence{phase,
                                        ordinal,
                                        {origin, index + 10U},
                                        origin + kI1MeasurementEndOffset,
                                        std::nullopt,
                                        std::nullopt,
                                        I1ServiceEvidence{},
                                        I1Verdict::Invalid,
                                        I1Verdict::Invalid,
                                        I1Verdict::Invalid,
                                        I1Verdict::Invalid,
                                        false,
                                        false,
                                        false});
  }
  testing::attach_m1_test_i1_sources(&input);
  if (options.m1_equal_time_supersession) {
    testing::configure_m1_test_equal_time_supersession(&input, true);
  }
  const B1JobInstance job{kM1WorkloadId,   1U, B1JobPhase::Cold, 0U,
                          kB1ColdJobIndex, 8U};
  input.protocol.batch_offers.push_back(
      M1BatchOfferEvidence{job,
                           0U,
                           0U,
                           {timeline.cold_start, 100U},
                           std::nullopt,
                           std::nullopt,
                           M1EventCoordinate{timeline.warmup_start, 101U},
                           false,
                           false,
                           false,
                           false,
                           false});
  input.protocol.carryover = {
      {"i1:warmup:6", B1JobPhase::Warmup, M1CarryoverState::Running, "", false,
       false, false},
      {"b1:warmup:a", B1JobPhase::Warmup, M1CarryoverState::Running, "", false,
       false, false},
      {"b1:warmup:b", B1JobPhase::Warmup, M1CarryoverState::Queued, "", false,
       false, false}};
  input.protocol.first_measured_admission.nominal_time =
      timeline.measurement_start;
  input.protocol.first_measured_admission.admission_sample =
      timeline.measurement_start;
  input.protocol.first_measured_admission.old_generation_settlement_endpoint =
      timeline.measurement_start + kI1MeasurementStartOffset;
  input.paired_isolated_i1_p99 =
      std::chrono::nanoseconds(options.m1_inner_i1_p99_ns);
  input.fairness.paired_isolated_b1 = M1PairedB1RateEvidence{
      options.m1_inner_b1_successful_operations, std::chrono::seconds(30)};
  testing::attach_m1_test_batch_sources(&input);
  testing::attach_m1_test_source_fairness_projection(&input);
  for (std::size_t index = 0U; index < 4U; ++index) {
    M1ExecutionSnapshot snapshot;
    snapshot.temporal_capture_ordinal = index;
    input.temporal_snapshots.push_back(std::move(snapshot));
  }
  M1FairnessObservationSnapshot observations;
  observations.stable_publication_cut = true;
  std::string canonical = materialize_m1_inner_row(
      evaluate_m1_inner_row(std::move(input)), observations);
  if (options.m1_progress_window_count != kM1MeasuredWindowCount) {
    if (options.m1_progress_window_count > kM1MeasuredWindowCount) {
      throw std::invalid_argument(
          "M1 envelope fixture progress count exceeds the source projection.");
    }
    B1CanonicalManifest manifest = parse_b1_canonical_manifest(canonical);
    std::vector<std::string> records =
        parse_b1_framed_list(manifest.fields[9U].payload);
    records.resize(options.m1_progress_window_count);
    manifest.fields[9U].payload = encode_test_record_list(records);
    canonical = encode_b1_canonical_manifest(manifest.schema, manifest.fields);
  }
  return options.rewrite_m1_inner
             ? options.rewrite_m1_inner(std::move(canonical))
             : canonical;
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
  if (workload == kB1WorkloadId) {
    input.job_instances.push_back(B1JobInstance{
        workload, ordinal, B1JobPhase::Cold, 0U, kB1ColdJobIndex, run_cap});
    for (const std::uint64_t job_index : kB1WarmupJobIndices) {
      input.job_instances.push_back(B1JobInstance{
          workload, ordinal, B1JobPhase::Warmup, 0U, job_index, run_cap});
    }
    for (std::uint64_t job_index = 0U; job_index < kB1MeasuredJobCount;
         ++job_index) {
      input.job_instances.push_back(B1JobInstance{
          workload, ordinal, B1JobPhase::Measured, 0U, job_index, run_cap});
    }
  } else if (workload == kM1WorkloadId) {
    input.job_instances.push_back(B1JobInstance{
        workload, ordinal, B1JobPhase::Cold, 0U, kB1ColdJobIndex, run_cap});
  }
  input.job_index_seal_ordinal =
      workload == kI1WorkloadId ? 2U : first_seal + 1U;
  input.measurement_evidence = make_section(
      "measurement-evidence", "execution-profile-measurement-evidence-v1",
      measurement_fields(workload, role, ordinal, denominators),
      first_seal + 2U);
  const bool denominator_only =
      workload == kI1WorkloadId || workload == kB1WorkloadId;
  const std::string pair_source_schema = workload == kI1WorkloadId
                                             ? kEvidenceI1PairDenominatorSchema
                                             : kEvidenceB1PairDenominatorSchema;
  input.output_evidence =
      denominator_only
          ? make_section(
                "output-evidence", "execution-profile-output-evidence-v1",
                {testing::known_b1_field("pair_source_schema", "identifier",
                                         pair_source_schema),
                 testing::known_b1_field("portable_output_claim_schema",
                                         "identifier",
                                         kEvidencePairNoOutputClaimSchema),
                 testing::known_b1_field("portable_output_authority", "enum",
                                         "not-claimed")},
                first_seal + 3U)
          : make_section("output-evidence",
                         "execution-profile-output-evidence-v1",
                         {testing::known_b1_field(
                             "raw_digest", "sha256",
                             test_digest(workload + std::string(":output:") +
                                         evidence_subject_role_name(role) +
                                         ":" + std::to_string(ordinal)))},
                         first_seal + 3U);
  input.verdict_evidence =
      denominator_only
          ? make_section(
                "verdict-evidence", "execution-profile-verdict-evidence-v1",
                {testing::known_b1_field("pair_source_schema", "identifier",
                                         pair_source_schema),
                 testing::known_b1_field("portable_claim_schema", "identifier",
                                         kEvidencePairNoVerdictClaimSchema),
                 testing::known_b1_field("portable_claim_scope", "enum",
                                         "denominator-only")},
                first_seal + 4U)
          : make_section("verdict-evidence",
                         "execution-profile-verdict-evidence-v1",
                         {testing::known_b1_field(
                             "raw_digest", "sha256",
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
            "9afe8a2124e1c583b23aabe46e801c9994f4c16635844c82ce49f03d0685b8b7");
  EXPECT_EQ(fixture.root_digest,
            "9a6e6885808f2d4688c1fbf13f2d6d4a34d71a57d21acef9146141701a77a7f7");
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
 * @brief Proves readers reject renewed verdict claims and incomplete B1 jobs.
 * @throws GoogleTest assertion control and fixture construction failures.
 */
TEST(EvidenceEnvelope, RejectsPairVerdictClaimsAndIncompleteB1JobIndex) {
  const M1EnvelopeFixture fixture = make_m1_fixture();
  EvidencePairObject i1{fixture.corpus.rows[0U], fixture.corpus.bundles[0U]};
  const EvidencePairObject valid_b1{fixture.corpus.rows[1U],
                                    fixture.corpus.bundles[1U]};
  i1.row.source.verdict_evidence =
      make_section("verdict-evidence", "execution-profile-verdict-evidence-v1",
                   {testing::known_b1_field("latency", "verdict", "pass")},
                   i1.row.source.verdict_evidence.seal_ordinal);
  i1.row = materialize_evidence_row(i1.row.source);
  i1.bundle.source.rows = {i1.row};
  i1.bundle = materialize_evidence_bundle(i1.bundle.source);
  EXPECT_THROW(validate_evidence_m1_pair_objects(
                   i1, valid_b1, EvidenceSubjectRole::Reference, 1U,
                   fixture.corpus.rows[2U].source.environment,
                   parse_b1_digest(test_digest("i1-fixture")),
                   EvidenceB1ComponentDigests{
                       parse_b1_digest(test_digest("b1-fixture")),
                       parse_b1_digest(test_digest("b1-corpus")),
                       parse_b1_digest(test_digest("b1-golden"))}),
               std::invalid_argument);

  const EvidencePairObject valid_i1{fixture.corpus.rows[0U],
                                    fixture.corpus.bundles[0U]};
  EvidencePairObject incomplete_b1 = valid_b1;
  incomplete_b1.row.source.job_instances.pop_back();
  incomplete_b1.row = materialize_evidence_row(incomplete_b1.row.source);
  incomplete_b1.bundle.source.rows = {incomplete_b1.row};
  incomplete_b1.bundle =
      materialize_evidence_bundle(incomplete_b1.bundle.source);
  EXPECT_THROW(validate_evidence_m1_pair_objects(
                   valid_i1, incomplete_b1, EvidenceSubjectRole::Reference, 1U,
                   fixture.corpus.rows[2U].source.environment,
                   parse_b1_digest(test_digest("i1-fixture")),
                   EvidenceB1ComponentDigests{
                       parse_b1_digest(test_digest("b1-fixture")),
                       parse_b1_digest(test_digest("b1-corpus")),
                       parse_b1_digest(test_digest("b1-golden"))}),
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
#else
  const std::filesystem::path symlink = root / "pair-object-link.canonical";
  constexpr DWORD kAllowUnprivilegedSymlinkCreation = 0x2U;
  if (::CreateSymbolicLinkW(symlink.c_str(), regular.c_str(),
                            kAllowUnprivilegedSymlinkCreation) == 0) {
    const DWORD error = ::GetLastError();
    std::filesystem::remove_all(root);
    if (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_INVALID_PARAMETER ||
        error == ERROR_NOT_SUPPORTED) {
      GTEST_SKIP() << "Windows host cannot create the reparse-point fixture: "
                   << error;
    }
    FAIL() << "CreateSymbolicLinkW failed unexpectedly: " << error;
    return;
  }
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
            "M1 canonical row requires exactly 30 progress windows.");
}

/**
 * @brief Proves every closed nested schema drift fails after outer rehashing.
 * @throws GoogleTest assertion control and fixture construction failures.
 */
TEST(EvidenceEnvelope, RejectsClosedNestedM1SchemaDriftAfterRehash) {
  using ManifestRewrite = std::function<void(B1CanonicalManifest*)>;
  const auto rewrite_manifest = [](ManifestRewrite rewrite) {
    return [rewrite = std::move(rewrite)](std::string source) {
      B1CanonicalManifest manifest = parse_b1_canonical_manifest(source);
      rewrite(&manifest);
      return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
    };
  };
  const std::vector<std::function<std::string(std::string)>> rewrites{
      rewrite_manifest([](B1CanonicalManifest* manifest) {
        manifest->fields.push_back(
            testing::known_b1_field("extension", "uint64", "1"));
      }),
      rewrite_manifest(
          [](B1CanonicalManifest* manifest) { manifest->fields.pop_back(); }),
      rewrite_manifest([](B1CanonicalManifest* manifest) {
        std::swap(manifest->fields[0U], manifest->fields[1U]);
      }),
      rewrite_manifest([](B1CanonicalManifest* manifest) {
        manifest->fields[1U] = manifest->fields[0U];
      }),
      rewrite_manifest([](B1CanonicalManifest* manifest) {
        manifest->fields[0U].payload = "02";
      }),
      [](std::string source) { return source + "\n"; }};

  for (std::size_t index = 0U; index < rewrites.size(); ++index) {
    SCOPED_TRACE(index);
    M1EnvelopeDenominatorOptions options;
    options.rewrite_m1_inner = rewrites[index];
    const M1EnvelopeFixture fixture = make_m1_fixture(options);
    EXPECT_EQ(
        validate_evidence_corpus(fixture.corpus, fixture.root_digest).verdict,
        I1Verdict::Invalid);
  }
}

/**
 * @brief Keeps `(B,n)` current then `(B,n+1)` cancellation source-closed.
 * @throws GoogleTest assertion control and fixture/canonical failures.
 * @note The nested row remains independently Invalid under Issue #93 because
 * the synthetic final-warmup Run has both visible success and cancellation;
 * the outer corpus must still accept its exact, source-closed evidence.
 */
TEST(EvidenceEnvelope,
     KeepsEqualTimeFollowingSupersessionClosedAfterOuterRehash) {
  const auto nested = std::make_shared<std::string>();
  M1EnvelopeDenominatorOptions options;
  options.m1_equal_time_supersession = true;
  options.rewrite_m1_inner = [nested](std::string source) {
    *nested = source;
    return source;
  };
  const M1EnvelopeFixture fixture = make_m1_fixture(options);
  const EvidenceCorpusValidation validation =
      validate_evidence_corpus(fixture.corpus, fixture.root_digest);
  EXPECT_EQ(validation.verdict, I1Verdict::Pass)
      << (validation.reasons.empty() ? "no diagnostic"
                                     : validation.reasons.front());
  EXPECT_TRUE(validation.reasons.empty());

  const M1CanonicalReplay replay =
      parse_and_recompute_m1_inner_row(*nested, 1U);
  EXPECT_TRUE(replay.row.source_evidence_closed);
  EXPECT_EQ(replay.row.overall_verdict, I1Verdict::Invalid);
  EXPECT_TRUE(replay.row.evidence.protocol.first_measured_admission
                  .warmup_publication_current_before_acceptance);
  EXPECT_FALSE(replay.row.evidence.protocol.first_measured_admission
                   .boundary_only_cancellation);
  EXPECT_FALSE(std::any_of(
      replay.row.validity_reasons.begin(), replay.row.validity_reasons.end(),
      [](const std::string& reason) {
        return reason.find("current hold differs") != std::string::npos ||
               reason.find("source-derived admission/fairness replay failed") !=
                   std::string::npos;
      }));
  EXPECT_EQ(materialize_m1_inner_row(replay.row, replay.observations), *nested);
}

/**
 * @brief Rejects `(B,n-1)` cancellation after every outer address is rehashed.
 * @throws GoogleTest assertion control and fixture/canonical failures.
 * @note Only the nested raw cancellation sequence changes; retained positive
 * current-hold projection and already-Invalid Issue #93 verdicts stay fixed.
 */
TEST(EvidenceEnvelope,
     RejectsRehashedEqualTimeCancellationBeforeMeasuredCurrent) {
  const auto nested = std::make_shared<std::string>();
  M1EnvelopeDenominatorOptions options;
  options.m1_equal_time_supersession = true;
  options.rewrite_m1_inner = [nested](std::string source) {
    *nested = testing::reverse_m1_test_equal_time_cancellation_order(
        std::move(source));
    return *nested;
  };
  const M1EnvelopeFixture fixture = make_m1_fixture(options);
  const EvidenceCorpusValidation validation =
      validate_evidence_corpus(fixture.corpus, fixture.root_digest);
  EXPECT_EQ(validation.verdict, I1Verdict::Invalid);
  try {
    static_cast<void>(parse_and_recompute_m1_inner_row(*nested, 1U));
    FAIL() << "reversed equal-time cancellation unexpectedly replayed";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what())
                  .find("source evidence is not exactly replayable"),
              std::string::npos);
  }
}

/**
 * @brief Proves a well-formed I1 projection contradiction remains invalid after
 * the nested manifest and every enclosing address are recomputed.
 * @throws GoogleTest assertion control and fixture construction failures.
 */
TEST(EvidenceEnvelope, RejectsRehashedM1SourceProjectionContradiction) {
  M1EnvelopeDenominatorOptions options;
  options.rewrite_m1_inner = [](std::string source) {
    B1CanonicalManifest manifest = parse_b1_canonical_manifest(source);
    std::vector<std::string> records =
        parse_b1_framed_list(manifest.fields[4U].payload);
    std::vector<std::string> occurrence =
        parse_b1_fixed_record(records[8U], 18U);
    occurrence[8U] =
        std::to_string(parse_b1_canonical_uint64(occurrence[8U]) + 1U);
    records[8U] = encode_b1_fixed_record(occurrence);
    manifest.fields[4U].payload = encode_test_record_list(records);
    return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
  };
  const M1EnvelopeFixture fixture = make_m1_fixture(options);
  const EvidenceCorpusValidation validation =
      validate_evidence_corpus(fixture.corpus, fixture.root_digest);
  EXPECT_EQ(validation.verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves admission/current-hold forgeries survive no outer rehash.
 * @throws GoogleTest assertion control and fixture construction failures.
 * @note Each rewrite changes all six nested verdict claims to Invalid;
 * retained Issue #93 sources remain unchanged and therefore source closure
 * must still reject the completely re-addressed row and corpus.
 */
TEST(EvidenceEnvelope,
     RejectsRehashedM1AdmissionCurrentHoldWithSynchronizedVerdicts) {
  const std::vector<std::function<std::string(std::string)>> rewrites{
      [](std::string source) {
        B1CanonicalManifest manifest = parse_b1_canonical_manifest(source);
        std::vector<std::string> first =
            parse_b1_fixed_record(manifest.fields[8U].payload, 12U);
        first[3U] = std::to_string(parse_b1_canonical_uint64(first[3U]) + 1U);
        first[6U] = first[3U];
        manifest.fields[8U].payload = encode_b1_fixed_record(first);
        manifest.fields[19U].payload = encode_b1_fixed_record(
            {"invalid", "invalid", "invalid", "invalid", "invalid", "invalid"});
        return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
      },
      [](std::string source) {
        B1CanonicalManifest manifest = parse_b1_canonical_manifest(source);
        std::vector<std::string> occurrences =
            parse_b1_framed_list(manifest.fields[4U].payload);
        const std::size_t final_warmup_index =
            kM1ColdI1OriginCount + kM1WarmupI1OriginCount - 1U;
        std::vector<std::string> final_warmup =
            parse_b1_fixed_record(occurrences[final_warmup_index], 18U);
        final_warmup[16U] = "false";
        occurrences[final_warmup_index] = encode_b1_fixed_record(final_warmup);
        manifest.fields[4U].payload = encode_test_record_list(occurrences);
        manifest.fields[19U].payload = encode_b1_fixed_record(
            {"invalid", "invalid", "invalid", "invalid", "invalid", "invalid"});
        return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
      }};

  for (std::size_t index = 0U; index < rewrites.size(); ++index) {
    SCOPED_TRACE(index);
    M1EnvelopeDenominatorOptions options;
    options.rewrite_m1_inner = rewrites[index];
    const M1EnvelopeFixture fixture = make_m1_fixture(options);
    const EvidenceCorpusValidation validation =
        validate_evidence_corpus(fixture.corpus, fixture.root_digest);
    EXPECT_EQ(validation.verdict, I1Verdict::Invalid);
  }
}

/**
 * @brief Proves memory underreporting remains Invalid after complete rehash.
 *
 * Each rewrite changes the nested temporal snapshots before the enclosing
 * section, row, bundle, and corpus-root addresses are materialized.  Honest
 * retained Invalid verdicts keep the address corpus structurally valid, while
 * strict nested replay must still report the exact Host/device contradiction.
 *
 * @throws GoogleTest assertion control and fixture construction failures.
 */
TEST(EvidenceEnvelope,
     KeepsUnderreportedM1MemoryInvalidAfterCompleteOuterRehash) {
  const auto rewrite_snapshot =
      [](std::string source,
         const std::function<void(std::vector<std::string>*)>& rewrite) {
        B1CanonicalManifest manifest = parse_b1_canonical_manifest(source);
        std::vector<std::string> snapshots =
            parse_b1_framed_list(manifest.fields[14U].payload);
        rewrite(&snapshots);
        manifest.fields[14U].payload = encode_test_record_list(snapshots);
        return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
      };
  const auto encode_device = [](std::uint64_t reserved,
                                std::uint64_t high_water) {
    constexpr std::uint64_t kMemoryLimit = 536870912U;
    constexpr std::uint64_t kScratchLimit = 268435456U;
    return encode_b1_fixed_record(
        {"1", "0", std::to_string(kMemoryLimit), std::to_string(kScratchLimit),
         std::to_string(reserved), "0", std::to_string(kMemoryLimit - reserved),
         std::to_string(kScratchLimit), std::to_string(high_water), "0"});
  };
  const auto replace_devices =
      [&encode_device](std::vector<std::string>* snapshots,
                       const std::array<std::uint64_t, 4U>& reserved,
                       const std::array<std::uint64_t, 4U>& high_water) {
        for (std::size_t index = 0U; index < snapshots->size(); ++index) {
          std::vector<std::string> snapshot =
              parse_b1_fixed_record((*snapshots)[index], 11U);
          snapshot[3U] = encode_test_record_list(
              {encode_device(reserved[index], high_water[index])});
          (*snapshots)[index] = encode_b1_fixed_record(snapshot);
        }
      };

  const std::vector<
      std::pair<std::function<std::string(std::string)>, std::string>>
      rewrites{{[rewrite_snapshot](std::string source) {
                  return rewrite_snapshot(
                      std::move(source),
                      [](std::vector<std::string>* snapshots) {
                        std::vector<std::string> snapshot =
                            parse_b1_fixed_record((*snapshots)[1U], 11U);
                        std::vector<std::string> reserved =
                            parse_b1_fixed_record(snapshot[1U], 5U);
                        std::vector<std::string> high_water =
                            parse_b1_fixed_record(snapshot[2U], 5U);
                        reserved[0U] = "1";
                        high_water[0U] = "0";
                        snapshot[1U] = encode_b1_fixed_record(reserved);
                        snapshot[2U] = encode_b1_fixed_record(high_water);
                        (*snapshots)[1U] = encode_b1_fixed_record(snapshot);
                      });
                },
                "Host reservation or lifetime high-water is contradictory"},
               {[rewrite_snapshot, replace_devices](std::string source) {
                  return rewrite_snapshot(
                      std::move(source),
                      [&replace_devices](std::vector<std::string>* snapshots) {
                        replace_devices(snapshots, {0U, 2U, 0U, 0U},
                                        {0U, 1U, 2U, 2U});
                      });
                },
                "device reservation or lifetime high-water is contradictory"},
               {[rewrite_snapshot, replace_devices](std::string source) {
                  return rewrite_snapshot(
                      std::move(source),
                      [&replace_devices](std::vector<std::string>* snapshots) {
                        replace_devices(snapshots, {0U, 0U, 0U, 0U},
                                        {0U, 2U, 1U, 2U});
                      });
                },
                "device reservation or lifetime high-water is contradictory"}};

  for (std::size_t index = 0U; index < rewrites.size(); ++index) {
    SCOPED_TRACE(index);
    const auto rewritten = std::make_shared<std::string>();
    M1EnvelopeDenominatorOptions options;
    options.rewrite_m1_inner = [rewritten, &rewrites,
                                index](std::string source) {
      *rewritten = rewrites[index].first(std::move(source));
      return *rewritten;
    };
    const M1EnvelopeFixture fixture = make_m1_fixture(options);
    EXPECT_EQ(
        validate_evidence_corpus(fixture.corpus, fixture.root_digest).verdict,
        I1Verdict::Pass);
    const M1CanonicalReplay replay =
        parse_and_recompute_m1_inner_row(*rewritten, 1U);
    EXPECT_EQ(replay.row.memory_verdict, I1Verdict::Invalid);
    EXPECT_TRUE(std::any_of(
        replay.row.validity_reasons.begin(), replay.row.validity_reasons.end(),
        [&rewrites, index](const std::string& reason) {
          return reason.find(rewrites[index].second) != std::string::npos;
        }));
  }
}

/**
 * @brief Proves rehashed progress, Graph, and headroom contradictions remain
 * invalid after all retained verdicts are synchronized to the already-Invalid
 * protocol row.
 * @throws GoogleTest assertion control and fixture construction failures.
 */
TEST(EvidenceEnvelope,
     RejectsRehashedM1SourceDerivedFairnessProjectionContradictions) {
  const auto rewrite_list_record = [](std::size_t field_index,
                                      std::size_t component_index,
                                      std::string replacement) {
    return [field_index, component_index,
            replacement = std::move(replacement)](std::string source) {
      B1CanonicalManifest manifest = parse_b1_canonical_manifest(source);
      std::vector<std::string> records =
          parse_b1_framed_list(manifest.fields[field_index].payload);
      std::vector<std::string> fields = parse_b1_fixed_record(
          records[0U],
          field_index == 9U ? 3U : (field_index == 10U ? 4U : 10U));
      fields[component_index] = replacement;
      records[0U] = encode_b1_fixed_record(fields);
      manifest.fields[field_index].payload = encode_test_record_list(records);
      manifest.fields[19U].payload = encode_b1_fixed_record(
          {"invalid", "invalid", "invalid", "invalid", "invalid", "invalid"});
      return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
    };
  };
  const std::vector<std::function<std::string(std::string)>> rewrites{
      rewrite_list_record(9U, 1U, "1"), rewrite_list_record(10U, 1U, "true"),
      rewrite_list_record(12U, 7U, "78")};

  for (std::size_t index = 0U; index < rewrites.size(); ++index) {
    SCOPED_TRACE(index);
    M1EnvelopeDenominatorOptions options;
    options.rewrite_m1_inner = rewrites[index];
    const M1EnvelopeFixture fixture = make_m1_fixture(options);
    const EvidenceCorpusValidation validation =
        validate_evidence_corpus(fixture.corpus, fixture.root_digest);
    EXPECT_EQ(validation.verdict, I1Verdict::Invalid);
  }
}

/**
 * @brief Proves placeholder/tampered nested records cannot survive rehashing.
 * @throws GoogleTest assertion control and fixture construction failures.
 */
TEST(EvidenceEnvelope, RejectsNestedPlaceholderAndVerdictTamperingAfterRehash) {
  const auto replace_list_record = [](std::size_t field_index,
                                      std::size_t record_index,
                                      std::string replacement) {
    return [field_index, record_index,
            replacement = std::move(replacement)](std::string source) {
      B1CanonicalManifest manifest = parse_b1_canonical_manifest(source);
      std::vector<std::string> records =
          parse_b1_framed_list(manifest.fields[field_index].payload);
      if (record_index < records.size()) {
        records[record_index] = replacement;
      } else {
        records.push_back(replacement);
      }
      manifest.fields[field_index].payload = encode_test_record_list(records);
      return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
    };
  };
  const auto replace_scalar = [](std::size_t field_index,
                                 std::string replacement) {
    return [field_index,
            replacement = std::move(replacement)](std::string source) {
      B1CanonicalManifest manifest = parse_b1_canonical_manifest(source);
      manifest.fields[field_index].payload = replacement;
      return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
    };
  };
  const auto replace_observation_record = [](std::string replacement) {
    return [replacement = std::move(replacement)](std::string source) {
      B1CanonicalManifest manifest = parse_b1_canonical_manifest(source);
      std::vector<std::string> snapshot =
          parse_b1_fixed_record(manifest.fields[15U].payload, 10U);
      std::vector<std::string> records = parse_b1_framed_list(snapshot[0U]);
      records.push_back(replacement);
      snapshot[0U] = encode_test_record_list(records);
      snapshot[4U] = "1";
      snapshot[5U] = "1";
      snapshot[6U] = "1";
      snapshot[7U] = "1";
      manifest.fields[15U].payload = encode_b1_fixed_record(snapshot);
      return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
    };
  };
  const std::vector<std::function<std::string(std::string)>> rewrites{
      replace_list_record(4U, 0U, encode_b1_fixed_record({"occurrence"})),
      replace_list_record(6U, 0U, encode_b1_fixed_record({"offer"})),
      replace_observation_record(encode_b1_fixed_record({"event"})),
      replace_scalar(18U, encode_b1_fixed_record({"waste"})),
      replace_scalar(19U, encode_b1_fixed_record({"pass"})),
      replace_scalar(19U, encode_b1_fixed_record({"pass", "pass", "pass",
                                                  "pass", "pass", "pass"})),
      [](std::string source) {
        B1CanonicalManifest manifest = parse_b1_canonical_manifest(source);
        std::vector<std::string> records =
            parse_b1_framed_list(manifest.fields[4U].payload);
        records[1U] = records[0U];
        manifest.fields[4U].payload = encode_test_record_list(records);
        return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
      },
      [](std::string source) {
        B1CanonicalManifest manifest = parse_b1_canonical_manifest(source);
        std::vector<std::string> records =
            parse_b1_framed_list(manifest.fields[4U].payload);
        std::swap(records[0U], records[1U]);
        manifest.fields[4U].payload = encode_test_record_list(records);
        return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
      },
      [](std::string source) {
        B1CanonicalManifest manifest = parse_b1_canonical_manifest(source);
        std::vector<std::string> records =
            parse_b1_framed_list(manifest.fields[4U].payload);
        std::vector<std::string> fields =
            parse_b1_fixed_record(records[0U], 18U);
        fields[0U] = "future";
        records[0U] = encode_b1_fixed_record(fields);
        manifest.fields[4U].payload = encode_test_record_list(records);
        return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
      },
      [](std::string source) {
        B1CanonicalManifest manifest = parse_b1_canonical_manifest(source);
        std::vector<std::string> records =
            parse_b1_framed_list(manifest.fields[9U].payload);
        std::vector<std::string> fields =
            parse_b1_fixed_record(records[0U], 3U);
        fields[2U] = "500000000";
        records[0U] = encode_b1_fixed_record(fields);
        manifest.fields[9U].payload = encode_test_record_list(records);
        return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
      },
      [](std::string source) {
        B1CanonicalManifest manifest = parse_b1_canonical_manifest(source);
        std::vector<std::string> records =
            parse_b1_framed_list(manifest.fields[14U].payload);
        records.pop_back();
        manifest.fields[14U].payload = encode_test_record_list(records);
        return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
      },
      [](std::string source) {
        B1CanonicalManifest manifest = parse_b1_canonical_manifest(source);
        std::vector<std::string> snapshot =
            parse_b1_fixed_record(manifest.fields[15U].payload, 10U);
        snapshot[6U] = "1";
        manifest.fields[15U].payload = encode_b1_fixed_record(snapshot);
        return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
      }};

  for (std::size_t index = 0U; index < rewrites.size(); ++index) {
    SCOPED_TRACE(index);
    M1EnvelopeDenominatorOptions options;
    options.rewrite_m1_inner = rewrites[index];
    const M1EnvelopeFixture fixture = make_m1_fixture(options);
    EXPECT_EQ(
        validate_evidence_corpus(fixture.corpus, fixture.root_digest).verdict,
        I1Verdict::Invalid);
  }
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
 * environment while shared denominator-only claims retain one object identity.
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
  EvidenceCanonicalRow candidate_row = make_row(
      std::move(candidate_environment), EvidenceSubjectRole::Candidate, 10U);
  candidate_row.source.output_evidence = reference_row.source.output_evidence;
  candidate_row.source.verdict_evidence = reference_row.source.verdict_evidence;
  candidate_row = materialize_evidence_row(std::move(candidate_row.source));

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
