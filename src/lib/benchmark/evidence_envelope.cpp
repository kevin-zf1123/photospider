/**
 * @file evidence_envelope.cpp
 * @brief Implements canonical execution-profile rows, bundles, and resolution.
 */
#include "benchmark/evidence_envelope.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "benchmark/m1_profile.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {
namespace {

// NOLINTBEGIN(whitespace/indent_namespace)

/** @brief Exact section-domain prefix including its final LF. */
constexpr std::string_view kSectionDigestDomain =
    "execution-profile-evidence-section-digest-v1\n";

/** @brief Exact row-domain prefix including its final LF. */
constexpr std::string_view kRowDigestDomain =
    "execution-profile-evidence-row-digest-v1\n";

/** @brief Exact bundle-domain prefix including its final LF. */
constexpr std::string_view kBundleDigestDomain =
    "execution-profile-evidence-bundle-digest-v1\n";

/** @brief Exact ordered names of the canonical 15 row fields. */
constexpr std::array<std::string_view, 15U> kRowFieldNames{
    "workload_id",
    "subject_role",
    "replicate_ordinal",
    "run_cap",
    "base_environment_digest",
    "storage_environment_applicability",
    "storage_environment_digest",
    "environment_class_digest",
    "workload_manifest_digest",
    "job_instance_index_digest",
    "measurement_evidence_digest",
    "output_evidence_digest",
    "verdict_evidence_digest",
    "paired_isolated_i1",
    "paired_isolated_b1_cap8",
};

/** @brief Exact ordered types of the canonical 15 row fields. */
constexpr std::array<std::string_view, 15U> kRowFieldTypes{
    "workload-id-v1",
    "enum",
    "uint64",
    "uint64",
    "sha256",
    "enum",
    "sha256",
    "sha256",
    "sha256",
    "sha256",
    "sha256",
    "sha256",
    "sha256",
    "evidence-pair-reference-v1",
    "evidence-pair-reference-v1",
};

/** @brief Exact ordered names of the canonical five bundle fields. */
constexpr std::array<std::string_view, 5U> kBundleFieldNames{
    "workload_id", "subject_role", "bundle_provenance_digest",
    "comparison_reference_bundle_digest", "row_references"};

/** @brief Exact ordered types of the canonical five bundle fields. */
constexpr std::array<std::string_view, 5U> kBundleFieldTypes{
    "workload-id-v1", "enum", "sha256", "sha256", "row-reference-list-v1"};

// NOLINTEND

/**
 * @brief Appends one stable invalidation reason once.
 * @param reasons Mutable diagnostic collection.
 * @param reason Stable complete diagnostic.
 * @return Nothing.
 * @throws std::bad_alloc when diagnostic ownership allocates.
 */
void invalidate(std::vector<std::string>* reasons, std::string reason) {
  if (std::find(reasons->begin(), reasons->end(), reason) == reasons->end()) {
    reasons->push_back(std::move(reason));
  }
}

/**
 * @brief Tests one exact closed execution-profile workload token.
 * @param workload Candidate raw token.
 * @return True only for I1, I2, B1, or M1 v1.
 * @throws Nothing.
 */
bool valid_workload(std::string_view workload) noexcept {
  return workload == kI1WorkloadId || workload == "I2-progressive-v1" ||
         workload == kB1WorkloadId || workload == kM1WorkloadId;
}

/**
 * @brief Tests one lowercase SHA-256 spelling through the shared parser.
 * @param digest Candidate address bytes.
 * @return True only for exactly 64 lowercase hexadecimal characters.
 * @throws Nothing.
 */
bool valid_digest(std::string_view digest) noexcept {
  try {
    static_cast<void>(parse_b1_digest(digest));
    return true;
  } catch (...) {
    return false;
  }
}

/**
 * @brief Returns one canonical known field.
 * @param name Exact field name.
 * @param type Exact field type.
 * @param payload Nonempty canonical payload.
 * @return Complete known/none field.
 * @throws std::invalid_argument for an empty payload.
 * @throws std::bad_alloc when field ownership allocates.
 */
B1CanonicalField known_field(std::string name, std::string type,
                             std::string payload) {
  if (payload.empty()) {
    throw std::invalid_argument("Canonical known field payload is empty.");
  }
  return B1CanonicalField{std::move(name), B1ObservationState::Known, "none",
                          std::move(type), std::move(payload)};
}

/**
 * @brief Returns one exact not-applicable field.
 * @param name Exact field name.
 * @param type Exact field type.
 * @param reason Closed field-specific reason.
 * @return Complete N/A field with an empty payload.
 * @throws std::bad_alloc when field ownership allocates.
 */
B1CanonicalField not_applicable_field(std::string name, std::string type,
                                      std::string reason) {
  return B1CanonicalField{std::move(name), B1ObservationState::NotApplicable,
                          std::move(reason), std::move(type), ""};
}

/**
 * @brief Parses one canonical subject-role token.
 * @param token Candidate token.
 * @return Candidate or Reference.
 * @throws std::invalid_argument for an unknown role.
 */
EvidenceSubjectRole parse_subject_role(std::string_view token) {
  if (token == "candidate") {
    return EvidenceSubjectRole::Candidate;
  }
  if (token == "reference") {
    return EvidenceSubjectRole::Reference;
  }
  throw std::invalid_argument("Evidence subject role is invalid.");
}

/**
 * @brief Returns one canonical address-kind token.
 * @param kind Closed node kind.
 * @return Process-lifetime token.
 * @throws std::invalid_argument for an unknown representation.
 */
const char* address_kind_name(EvidenceAddressKind kind) {
  switch (kind) {
    case EvidenceAddressKind::Section:
      return "section";
    case EvidenceAddressKind::Row:
      return "row";
    case EvidenceAddressKind::Bundle:
      return "bundle";
  }
  throw std::invalid_argument("Evidence address kind is invalid.");
}

/**
 * @brief Parses one canonical address-kind token.
 * @param token Candidate token.
 * @return Exact closed node kind.
 * @throws std::invalid_argument for an unknown token.
 */
EvidenceAddressKind parse_address_kind(std::string_view token) {
  if (token == "section") {
    return EvidenceAddressKind::Section;
  }
  if (token == "row") {
    return EvidenceAddressKind::Row;
  }
  if (token == "bundle") {
    return EvidenceAddressKind::Bundle;
  }
  throw std::invalid_argument("Evidence address kind token is invalid.");
}

/**
 * @brief Encodes sorted unique typed address dependencies.
 * @param dependencies Complete dependency set in any order.
 * @return Canonical generic list of two-component fixed records.
 * @throws std::invalid_argument for invalid or duplicate addresses.
 * @throws std::bad_alloc when record ownership allocates.
 */
std::string encode_address_dependencies(
    const std::vector<EvidenceAddressReference>& dependencies) {
  std::vector<std::string> records;
  records.reserve(dependencies.size());
  for (const EvidenceAddressReference& dependency : dependencies) {
    if (!valid_digest(dependency.digest)) {
      throw std::invalid_argument(
          "Evidence dependency contains an invalid digest.");
    }
    records.push_back(encode_b1_fixed_record(
        {address_kind_name(dependency.kind), dependency.digest}));
  }
  std::sort(records.begin(), records.end());
  if (std::adjacent_find(records.begin(), records.end()) != records.end()) {
    throw std::invalid_argument("Evidence dependency list has a duplicate.");
  }
  std::string payload = std::to_string(records.size()) + ":";
  for (const std::string& record : records) {
    payload.append(b1_environment_frame(record));
  }
  return payload;
}

/**
 * @brief Parses one canonical typed address-dependency list.
 * @param payload Complete generic-list payload.
 * @return Typed dependencies in canonical record order.
 * @throws std::invalid_argument for list/record/domain/order drift.
 * @throws std::bad_alloc when result ownership allocates.
 */
std::vector<EvidenceAddressReference> parse_address_dependencies(
    std::string_view payload) {
  const std::vector<std::string> records = parse_b1_framed_list(payload);
  if (!std::is_sorted(records.begin(), records.end()) ||
      std::adjacent_find(records.begin(), records.end()) != records.end()) {
    throw std::invalid_argument(
        "Evidence dependency list is not canonical and unique.");
  }
  std::vector<EvidenceAddressReference> dependencies;
  dependencies.reserve(records.size());
  for (const std::string& record : records) {
    const std::vector<std::string> components =
        parse_b1_fixed_record(record, 2U);
    if (!valid_digest(components[1U])) {
      throw std::invalid_argument(
          "Evidence dependency digest is not canonical SHA-256.");
    }
    dependencies.push_back(EvidenceAddressReference{
        parse_address_kind(components[0U]), components[1U]});
  }
  return dependencies;
}

/**
 * @brief Returns a field by exact name from one parsed manifest.
 * @param manifest Parsed canonical manifest.
 * @param name Exact field name.
 * @return Borrowed matching field.
 * @throws std::invalid_argument when absent or duplicated.
 */
const B1CanonicalField& require_field(const B1CanonicalManifest& manifest,
                                      std::string_view name) {
  const B1CanonicalField* result = nullptr;
  for (const B1CanonicalField& field : manifest.fields) {
    if (field.name != name) {
      continue;
    }
    if (result != nullptr) {
      throw std::invalid_argument("Canonical manifest field is duplicated.");
    }
    result = &field;
  }
  if (result == nullptr) {
    throw std::invalid_argument("Canonical manifest field is missing.");
  }
  return *result;
}

/**
 * @brief Validates one retained section and explicit dependency field.
 * @param section Candidate retained section.
 * @param expected_name Exact row/provenance binding.
 * @param expected_schema Exact schema header.
 * @return Parsed canonical section.
 * @throws std::invalid_argument for identity, framing, dependency, or seal
 * drift.
 * @throws std::bad_alloc when parsed ownership allocates.
 */
B1CanonicalManifest validate_section(const EvidenceRetainedSection& section,
                                     std::string_view expected_name,
                                     std::string_view expected_schema) {
  if (section.section_name != expected_name ||
      section.schema_id != expected_schema || section.seal_ordinal == 0U) {
    throw std::invalid_argument(
        "Evidence retained-section identity or seal ordinal drifted.");
  }
  const B1CanonicalManifest manifest =
      parse_b1_canonical_manifest(section.bytes);
  if (manifest.schema != section.schema_id ||
      encode_b1_canonical_manifest(manifest.schema, manifest.fields) !=
          section.bytes) {
    throw std::invalid_argument(
        "Evidence retained section is not canonical for its schema.");
  }
  const auto dependency_field =
      std::find_if(manifest.fields.begin(), manifest.fields.end(),
                   [](const B1CanonicalField& field) {
                     return field.name == "address_dependencies";
                   });
  if (dependency_field == manifest.fields.end()) {
    if (!section.address_dependencies.empty()) {
      throw std::invalid_argument(
          "Evidence section omits its address dependency field.");
    }
  } else {
    if (dependency_field->state != B1ObservationState::Known ||
        dependency_field->reason != "none" ||
        dependency_field->type != "evidence-address-list-v1" ||
        parse_address_dependencies(dependency_field->payload) !=
            section.address_dependencies ||
        dependency_field->payload !=
            encode_address_dependencies(section.address_dependencies)) {
      throw std::invalid_argument(
          "Evidence section address dependencies are not exact.");
    }
  }
  return manifest;
}

/**
 * @brief Validates exact known state/type for one row or bundle field.
 * @param field Candidate field.
 * @param name Required name.
 * @param type Required type.
 * @return Nothing for a canonical known field.
 * @throws std::invalid_argument for any envelope drift.
 */
void require_known_field(const B1CanonicalField& field, std::string_view name,
                         std::string_view type) {
  if (field.name != name || field.type != type ||
      field.state != B1ObservationState::Known || field.reason != "none" ||
      field.payload.empty()) {
    throw std::invalid_argument("Canonical known evidence field drifted.");
  }
}

/**
 * @brief Encodes one exact pair-reference fixed record.
 * @param pair Complete target addresses and ordinal.
 * @return Three framed components in normative order.
 * @throws std::invalid_argument for digest or ordinal drift.
 * @throws std::bad_alloc when payload ownership allocates.
 */
std::string encode_pair(const EvidencePairReference& pair) {
  if (!valid_digest(pair.row_digest) || !valid_digest(pair.bundle_digest) ||
      pair.replicate_ordinal == 0U || pair.replicate_ordinal > 3U) {
    throw std::invalid_argument("Evidence pair reference is invalid.");
  }
  return encode_b1_fixed_record({pair.row_digest, pair.bundle_digest,
                                 std::to_string(pair.replicate_ordinal)});
}

/**
 * @brief Parses one exact pair-reference fixed record.
 * @param payload Complete three-component payload.
 * @return Typed pair reference.
 * @throws std::invalid_argument for record/digest/ordinal drift.
 * @throws std::bad_alloc when component ownership allocates.
 */
EvidencePairReference parse_pair(std::string_view payload) {
  const std::vector<std::string> components =
      parse_b1_fixed_record(payload, 3U);
  EvidencePairReference pair{components[0U], components[1U],
                             parse_b1_canonical_uint64(components[2U])};
  static_cast<void>(encode_pair(pair));
  return pair;
}

/**
 * @brief Returns the exact phase rank used by job-instance-list-v1.
 * @param phase Closed B1/M1 occurrence phase.
 * @return Zero for cold, one for warmup, and two for measured.
 * @throws std::invalid_argument for an unknown phase representation.
 */
int job_phase_rank(B1JobPhase phase) {
  switch (phase) {
    case B1JobPhase::Cold:
      return 0;
    case B1JobPhase::Warmup:
      return 1;
    case B1JobPhase::Measured:
      return 2;
  }
  throw std::invalid_argument("Evidence job phase is invalid.");
}

/**
 * @brief Orders job instances by the normative list key.
 * @param lhs First valid job.
 * @param rhs Second valid job.
 * @return Strict phase/cycle/job/remaining-payload order.
 * @throws Validation/allocation failures from canonical job encoding.
 */
bool job_precedes(const B1JobInstance& lhs, const B1JobInstance& rhs) {
  const int lhs_phase = job_phase_rank(lhs.phase);
  const int rhs_phase = job_phase_rank(rhs.phase);
  if (lhs_phase != rhs_phase) {
    return lhs_phase < rhs_phase;
  }
  if (lhs.cycle_ordinal != rhs.cycle_ordinal) {
    return lhs.cycle_ordinal < rhs.cycle_ordinal;
  }
  if (lhs.job_index != rhs.job_index) {
    return lhs.job_index < rhs.job_index;
  }
  return encode_b1_job_instance(lhs) < encode_b1_job_instance(rhs);
}

/**
 * @brief Constructs the exact job-instance-index retained section.
 * @param input Row whose occurrences and identity are authoritative.
 * @return Canonical one-field section with its domain metadata.
 * @throws std::invalid_argument for occurrence/schema/order/identity drift.
 * @throws std::bad_alloc when canonical ownership allocates.
 */
EvidenceRetainedSection make_job_index(const EvidenceRowInput& input) {
  const bool b1_bearing =
      input.workload_id == kB1WorkloadId || input.workload_id == kM1WorkloadId;
  if (b1_bearing == input.job_instances.empty()) {
    throw std::invalid_argument(
        "Evidence job index emptiness contradicts its workload.");
  }
  std::vector<B1JobInstance> jobs = input.job_instances;
  for (const B1JobInstance& job : jobs) {
    validate_b1_job_instance(job);
    if (job.row_workload_id != input.workload_id ||
        job.replicate_ordinal != input.replicate_ordinal ||
        job.run_cap != input.run_cap) {
      throw std::invalid_argument(
          "Evidence job instance does not join its enclosing row.");
    }
  }
  std::sort(jobs.begin(), jobs.end(), job_precedes);
  for (std::size_t index = 1U; index < jobs.size(); ++index) {
    if (jobs[index - 1U] == jobs[index] ||
        (jobs[index - 1U].phase == jobs[index].phase &&
         jobs[index - 1U].cycle_ordinal == jobs[index].cycle_ordinal &&
         jobs[index - 1U].job_index == jobs[index].job_index)) {
      throw std::invalid_argument(
          "Evidence job index has a duplicate occurrence coordinate.");
    }
  }
  std::string list = std::to_string(jobs.size()) + ":";
  for (const B1JobInstance& job : jobs) {
    list.append(b1_environment_frame(encode_b1_job_instance(job)));
  }
  const std::string bytes = encode_b1_canonical_manifest(
      kEvidenceJobIndexSchema,
      {known_field("job_instances", "job-instance-list-v1", list)});
  return EvidenceRetainedSection{"job-instance-index",
                                 kEvidenceJobIndexSchema,
                                 bytes,
                                 {},
                                 input.job_index_seal_ordinal};
}

/**
 * @brief Validates the exact workload-specific Run cap.
 * @param workload Valid frozen workload.
 * @param run_cap Candidate cap.
 * @return Nothing for the frozen allowed cap.
 * @throws std::invalid_argument for drift.
 */
void validate_run_cap(std::string_view workload, std::uint64_t run_cap) {
  const bool valid =
      (workload == kB1WorkloadId && (run_cap == 1U || run_cap == 8U)) ||
      (workload != kB1WorkloadId && run_cap == 8U);
  if (!valid) {
    throw std::invalid_argument(
        "Evidence row Run cap does not match its frozen workload.");
  }
}

/**
 * @brief Encodes one row-reference fixed-record payload.
 * @param reference Complete row reference.
 * @return Four framed components in normative order.
 * @throws std::invalid_argument for workload/digest/key drift.
 */
std::string encode_row_reference(const EvidenceRowReference& reference) {
  if (!valid_workload(reference.workload_id) ||
      reference.replicate_ordinal == 0U || reference.replicate_ordinal > 3U ||
      !valid_digest(reference.row_digest)) {
    throw std::invalid_argument("Evidence row reference is invalid.");
  }
  validate_run_cap(reference.workload_id, reference.run_cap);
  return encode_b1_fixed_record(
      {reference.workload_id, std::to_string(reference.run_cap),
       std::to_string(reference.replicate_ordinal), reference.row_digest});
}

/**
 * @brief Parses one row-reference fixed-record payload.
 * @param payload Complete four-component record.
 * @return Typed row reference.
 * @throws std::invalid_argument for shape/domain drift.
 */
EvidenceRowReference parse_row_reference(std::string_view payload) {
  const std::vector<std::string> components =
      parse_b1_fixed_record(payload, 4U);
  EvidenceRowReference reference{
      components[0U], parse_b1_canonical_uint64(components[1U]),
      parse_b1_canonical_uint64(components[2U]), components[3U]};
  static_cast<void>(encode_row_reference(reference));
  return reference;
}

/**
 * @brief Orders bundle row references by the normative functional-list key.
 * @param lhs First valid reference.
 * @param rhs Second valid reference.
 * @return Strict run-cap/ordinal/full-payload order.
 * @throws Validation/allocation failures from canonical encoding.
 */
bool row_reference_precedes(const EvidenceRowReference& lhs,
                            const EvidenceRowReference& rhs) {
  if (lhs.run_cap != rhs.run_cap) {
    return lhs.run_cap < rhs.run_cap;
  }
  if (lhs.replicate_ordinal != rhs.replicate_ordinal) {
    return lhs.replicate_ordinal < rhs.replicate_ordinal;
  }
  return encode_row_reference(lhs) < encode_row_reference(rhs);
}

/**
 * @brief Returns one workload-section digest field payload.
 * @param row Materialized row retaining its workload section.
 * @param preferred Workload-specific exact field name.
 * @return Borrowed known SHA-256 payload.
 * @throws std::invalid_argument when missing or malformed.
 */
std::string workload_fixture_digest(const EvidenceCanonicalRow& row,
                                    std::string_view preferred) {
  const B1CanonicalManifest manifest =
      parse_b1_canonical_manifest(row.source.workload_manifest.bytes);
  const B1CanonicalField* field = nullptr;
  for (const B1CanonicalField& candidate : manifest.fields) {
    if (candidate.name == preferred ||
        (field == nullptr && candidate.name == "fixture_digest")) {
      field = &candidate;
      if (candidate.name == preferred) {
        break;
      }
    }
  }
  if (field == nullptr || field->state != B1ObservationState::Known ||
      field->reason != "none" || field->type != "sha256" ||
      !valid_digest(field->payload)) {
    throw std::invalid_argument(
        "Evidence workload section lacks its exact fixture digest.");
  }
  return field->payload;
}

/**
 * @brief Returns one required exact named workload-section digest.
 * @param row Materialized row retaining its workload section.
 * @param field_name Exact component binding such as `b1_corpus_digest`.
 * @return Borrowed known SHA-256 payload.
 * @throws std::invalid_argument when absent, duplicated, or malformed.
 */
std::string workload_named_digest(const EvidenceCanonicalRow& row,
                                  std::string_view field_name) {
  const B1CanonicalManifest manifest =
      parse_b1_canonical_manifest(row.source.workload_manifest.bytes);
  const B1CanonicalField* result = nullptr;
  for (const B1CanonicalField& field : manifest.fields) {
    if (field.name != field_name) {
      continue;
    }
    if (result != nullptr) {
      throw std::invalid_argument(
          "Evidence workload component digest is duplicated.");
    }
    result = &field;
  }
  if (result == nullptr || result->state != B1ObservationState::Known ||
      result->reason != "none" || result->type != "sha256" ||
      !valid_digest(result->payload)) {
    throw std::invalid_argument(
        "Evidence workload section lacks a required component digest.");
  }
  return result->payload;
}

/**
 * @brief Validates one candidate row against its same-key reference target.
 * @param candidate Candidate row under evaluation.
 * @param reference Immutable reference row selected by functional key.
 * @return Nothing for equal schema/environment/fixture lineage.
 * @throws std::invalid_argument for any comparison incompatibility.
 */
void validate_comparison_row(const EvidenceCanonicalRow& candidate,
                             const EvidenceCanonicalRow& reference) {
  if (!compatible_b1_environments(candidate.source.environment,
                                  reference.source.environment,
                                  B1EnvironmentRelation::CandidateReference)) {
    throw std::invalid_argument(
        "Evidence candidate/reference environments are incompatible.");
  }
  if (workload_fixture_digest(candidate, "fixture_digest") !=
      workload_fixture_digest(reference, "fixture_digest")) {
    throw std::invalid_argument(
        "Evidence candidate/reference fixture lineage differs.");
  }
  if (candidate.source.workload_id == kB1WorkloadId ||
      candidate.source.workload_id == kM1WorkloadId) {
    if (workload_named_digest(candidate, "b1_corpus_digest") !=
            workload_named_digest(reference, "b1_corpus_digest") ||
        workload_named_digest(candidate, "b1_golden_digest") !=
            workload_named_digest(reference, "b1_golden_digest")) {
      throw std::invalid_argument(
          "Evidence candidate/reference corpus or golden lineage differs.");
    }
  }
}

/**
 * @brief Resolves exactly one retained row by claimed digest.
 * @param corpus Complete retained multiset.
 * @param digest Exact target address.
 * @return Borrowed sole matching object.
 * @throws std::invalid_argument for zero or multiple matches.
 */
const EvidenceCanonicalRow& resolve_row(const EvidenceCorpus& corpus,
                                        std::string_view digest) {
  const EvidenceCanonicalRow* result = nullptr;
  for (const EvidenceCanonicalRow& row : corpus.rows) {
    if (row.digest != digest) {
      continue;
    }
    if (result != nullptr) {
      throw std::invalid_argument(
          "Evidence row digest resolves to multiple retained objects.");
    }
    result = &row;
  }
  if (result == nullptr) {
    throw std::invalid_argument(
        "Evidence row digest resolves to no retained object.");
  }
  return *result;
}

/**
 * @brief Resolves exactly one retained bundle by claimed digest.
 * @param corpus Complete retained multiset.
 * @param digest Exact target address.
 * @return Borrowed sole matching object.
 * @throws std::invalid_argument for zero or multiple matches.
 */
const EvidenceCanonicalBundle& resolve_bundle(const EvidenceCorpus& corpus,
                                              std::string_view digest) {
  const EvidenceCanonicalBundle* result = nullptr;
  for (const EvidenceCanonicalBundle& bundle : corpus.bundles) {
    if (bundle.digest != digest) {
      continue;
    }
    if (result != nullptr) {
      throw std::invalid_argument(
          "Evidence bundle digest resolves to multiple retained objects.");
    }
    result = &bundle;
  }
  if (result == nullptr) {
    throw std::invalid_argument(
        "Evidence bundle digest resolves to no retained object.");
  }
  return *result;
}

/**
 * @brief Resolves exactly one retained section by independently computed
 * address.
 * @param corpus Complete retained object multiset.
 * @param digest Exact target section address.
 * @return Borrowed sole matching section/provenance object.
 * @throws std::invalid_argument for zero or multiple matches.
 */
const EvidenceRetainedSection& resolve_section(const EvidenceCorpus& corpus,
                                               std::string_view digest) {
  const EvidenceRetainedSection* result = nullptr;
  for (const EvidenceRetainedSection& section : corpus.sections) {
    const std::string candidate = digest_evidence_section(
        section.section_name, section.schema_id, section.bytes);
    if (candidate != digest) {
      continue;
    }
    if (result != nullptr) {
      throw std::invalid_argument(
          "Evidence section digest resolves to multiple retained objects.");
    }
    result = &section;
  }
  if (result == nullptr) {
    throw std::invalid_argument(
        "Evidence section digest resolves to no retained object.");
  }
  return *result;
}

/**
 * @brief Compares complete retained-section identity and dependency metadata.
 * @param lhs First retained section.
 * @param rhs Second retained section.
 * @return True only for byte-identical source objects and seal metadata.
 * @throws Nothing.
 */
bool same_section(const EvidenceRetainedSection& lhs,
                  const EvidenceRetainedSection& rhs) noexcept {
  return lhs.section_name == rhs.section_name &&
         lhs.schema_id == rhs.schema_id && lhs.bytes == rhs.bytes &&
         lhs.address_dependencies == rhs.address_dependencies &&
         lhs.seal_ordinal == rhs.seal_ordinal;
}

/**
 * @brief Tests whether one bundle declares an exact row reference once.
 * @param bundle Parsed/materialized target bundle.
 * @param row Target row.
 * @return True only for one identical key/digest item.
 * @throws Nothing.
 */
bool bundle_names_row(const EvidenceCanonicalBundle& bundle,
                      const EvidenceCanonicalRow& row) noexcept {
  return std::count_if(
             bundle.row_references.begin(), bundle.row_references.end(),
             [&row](const EvidenceRowReference& reference) {
               return reference.workload_id == row.source.workload_id &&
                      reference.run_cap == row.source.run_cap &&
                      reference.replicate_ordinal ==
                          row.source.replicate_ordinal &&
                      reference.row_digest == row.digest;
             }) == 1;
}

/**
 * @brief Validates one M1 isolated pair after exact object resolution.
 * @param m1 Enclosing M1 row.
 * @param pair Exact pair record.
 * @param target_workload I1 or B1 workload token.
 * @param corpus Complete retained object multiset.
 * @return Nothing for a same-role/ordinal/environment/fixture target.
 * @throws std::invalid_argument for any resolution or relation drift.
 */
void validate_m1_pair(const EvidenceCanonicalRow& m1,
                      const EvidencePairReference& pair,
                      std::string_view target_workload,
                      const EvidenceCorpus& corpus) {
  const EvidenceCanonicalBundle& bundle =
      resolve_bundle(corpus, pair.bundle_digest);
  const EvidenceCanonicalRow& row = resolve_row(corpus, pair.row_digest);
  if (bundle.digest != pair.bundle_digest || row.digest != pair.row_digest ||
      !bundle_names_row(bundle, row) ||
      bundle.source.subject_role != m1.source.subject_role ||
      bundle.source.workload_id != target_workload ||
      row.source.subject_role != m1.source.subject_role ||
      row.source.workload_id != target_workload || row.source.run_cap != 8U ||
      pair.replicate_ordinal != m1.source.replicate_ordinal ||
      row.source.replicate_ordinal != m1.source.replicate_ordinal ||
      row.source.environment.base_manifest !=
          m1.source.environment.base_manifest) {
    throw std::invalid_argument(
        "M1 isolated pair role/key/base relation is invalid.");
  }
  if (target_workload == kI1WorkloadId) {
    if (row.source.environment.storage_manifest.has_value() ||
        !compatible_b1_environments(
            m1.source.environment, row.source.environment,
            B1EnvironmentRelation::M1PairedI1BaseOnly) ||
        workload_fixture_digest(m1, "i1_fixture_digest") !=
            workload_fixture_digest(row, "i1_fixture_digest")) {
      throw std::invalid_argument(
          "M1 isolated-I1 base-only fixture relation is invalid.");
    }
  } else {
    if (!m1.source.environment.storage_manifest.has_value() ||
        !row.source.environment.storage_manifest.has_value() ||
        !compatible_b1_environments(
            m1.source.environment, row.source.environment,
            B1EnvironmentRelation::M1PairedB1CapEight) ||
        workload_fixture_digest(m1, "b1_fixture_digest") !=
            workload_fixture_digest(row, "b1_fixture_digest") ||
        workload_named_digest(m1, "b1_corpus_digest") !=
            workload_named_digest(row, "b1_corpus_digest") ||
        workload_named_digest(m1, "b1_golden_digest") !=
            workload_named_digest(row, "b1_golden_digest")) {
      throw std::invalid_argument(
          "M1 isolated-B1 full environment/fixture relation is invalid.");
    }
  }
}

}  // namespace

bool EvidenceAddressReference::operator==(
    const EvidenceAddressReference& other) const noexcept {
  return kind == other.kind && digest == other.digest;
}

bool EvidencePairReference::operator==(
    const EvidencePairReference& other) const noexcept {
  return row_digest == other.row_digest &&
         bundle_digest == other.bundle_digest &&
         replicate_ordinal == other.replicate_ordinal;
}

bool EvidenceRowReference::operator==(
    const EvidenceRowReference& other) const noexcept {
  return workload_id == other.workload_id && run_cap == other.run_cap &&
         replicate_ordinal == other.replicate_ordinal &&
         row_digest == other.row_digest;
}

/** @copydoc evidence_subject_role_name */
const char* evidence_subject_role_name(EvidenceSubjectRole role) {
  switch (role) {
    case EvidenceSubjectRole::Candidate:
      return "candidate";
    case EvidenceSubjectRole::Reference:
      return "reference";
  }
  throw std::invalid_argument("Evidence subject role is invalid.");
}

/** @copydoc digest_evidence_section */
std::string digest_evidence_section(std::string_view section_name,
                                    std::string_view section_schema_id,
                                    std::string_view section_bytes) {
  std::string input(kSectionDigestDomain);
  input.append(b1_environment_frame(section_name));
  input.append(b1_environment_frame(section_schema_id));
  input.append(b1_environment_frame(section_bytes));
  return b1_digest_hex(b1_sha256(input));
}

/** @copydoc digest_evidence_row */
std::string digest_evidence_row(std::string_view manifest_bytes) {
  std::string input(kRowDigestDomain);
  input.append(b1_environment_frame(manifest_bytes));
  return b1_digest_hex(b1_sha256(input));
}

/** @copydoc digest_evidence_bundle */
std::string digest_evidence_bundle(std::string_view manifest_bytes) {
  std::string input(kBundleDigestDomain);
  input.append(b1_environment_frame(manifest_bytes));
  return b1_digest_hex(b1_sha256(input));
}

/** @copydoc materialize_evidence_row */
EvidenceCanonicalRow materialize_evidence_row(EvidenceRowInput input) {
  if (!valid_workload(input.workload_id) || input.replicate_ordinal == 0U ||
      input.replicate_ordinal > 3U || input.seal_ordinal == 0U ||
      input.job_index_seal_ordinal == 0U) {
    throw std::invalid_argument(
        "Evidence row workload, ordinal, or seal is invalid.");
  }
  validate_run_cap(input.workload_id, input.run_cap);
  static_cast<void>(evidence_subject_role_name(input.subject_role));

  if (input.environment.workload_id != input.workload_id ||
      input.environment.run_cap != input.run_cap ||
      input.environment.replicate_ordinal != input.replicate_ordinal ||
      !valid_b1_environment_claims(input.environment)) {
    throw std::invalid_argument(
        "Evidence row environment identity or retained claims are invalid.");
  }
  const std::string base_digest =
      b1_digest_hex(input.environment.claimed_base_digest);
  const bool storage_required =
      input.workload_id == kB1WorkloadId || input.workload_id == kM1WorkloadId;
  std::optional<std::string> storage_digest;
  if (storage_required != input.environment.storage_manifest.has_value()) {
    throw std::invalid_argument(
        "Evidence row storage applicability contradicts its workload.");
  }
  if (input.environment.storage_manifest.has_value()) {
    storage_digest = b1_digest_hex(*input.environment.claimed_storage_digest);
  }
  const std::string class_digest =
      b1_digest_hex(input.environment.claimed_environment_class_digest);
  const B1CanonicalManifest environment_class = parse_b1_environment_manifest(
      input.environment.environment_class_manifest);
  if (require_field(environment_class, "base_environment_digest").payload !=
          base_digest ||
      (storage_required &&
       (require_field(environment_class, "storage_environment_applicability")
                .payload != "required" ||
        require_field(environment_class, "storage_environment_digest")
                .payload != *storage_digest)) ||
      (!storage_required &&
       require_field(environment_class, "storage_environment_applicability")
               .payload != "not-applicable")) {
    throw std::invalid_argument(
        "Evidence row environment class does not bind its manifests.");
  }

  const std::array<std::pair<std::string_view, std::string_view>, 4U>
      section_contracts{{
          {"workload-manifest", "execution-profile-workload-manifest-v1"},
          {"measurement-evidence", "execution-profile-measurement-evidence-v1"},
          {"output-evidence", "execution-profile-output-evidence-v1"},
          {"verdict-evidence", "execution-profile-verdict-evidence-v1"},
      }};
  const std::array<const EvidenceRetainedSection*, 4U> supplied{
      &input.workload_manifest, &input.measurement_evidence,
      &input.output_evidence, &input.verdict_evidence};
  for (std::size_t index = 0U; index < supplied.size(); ++index) {
    static_cast<void>(validate_section(*supplied[index],
                                       section_contracts[index].first,
                                       section_contracts[index].second));
    if (supplied[index]->seal_ordinal >= input.seal_ordinal) {
      throw std::invalid_argument(
          "Evidence row depends on an unsealed or later section.");
    }
  }
  EvidenceRetainedSection job_index = make_job_index(input);
  static_cast<void>(validate_section(job_index, "job-instance-index",
                                     kEvidenceJobIndexSchema));
  if (job_index.seal_ordinal >= input.seal_ordinal) {
    throw std::invalid_argument(
        "Evidence row depends on a later job-index section.");
  }

  const bool is_m1 = input.workload_id == kM1WorkloadId;
  if (is_m1 != (input.paired_isolated_i1.has_value() &&
                input.paired_isolated_b1_cap8.has_value())) {
    throw std::invalid_argument(
        "Evidence row isolated-pair applicability is invalid.");
  }
  if (!is_m1 && (input.paired_isolated_i1.has_value() ||
                 input.paired_isolated_b1_cap8.has_value())) {
    throw std::invalid_argument(
        "Non-M1 evidence row contains an isolated pair.");
  }

  const auto section_digest = [](const EvidenceRetainedSection& section) {
    return digest_evidence_section(section.section_name, section.schema_id,
                                   section.bytes);
  };
  std::vector<B1CanonicalField> fields;
  fields.reserve(kRowFieldNames.size());
  fields.push_back(
      known_field("workload_id", "workload-id-v1", input.workload_id));
  fields.push_back(known_field("subject_role", "enum",
                               evidence_subject_role_name(input.subject_role)));
  fields.push_back(known_field("replicate_ordinal", "uint64",
                               std::to_string(input.replicate_ordinal)));
  fields.push_back(
      known_field("run_cap", "uint64", std::to_string(input.run_cap)));
  fields.push_back(
      known_field("base_environment_digest", "sha256", base_digest));
  fields.push_back(
      known_field("storage_environment_applicability", "enum",
                  storage_required ? "required" : "not-applicable"));
  if (storage_required) {
    fields.push_back(
        known_field("storage_environment_digest", "sha256", *storage_digest));
  } else {
    fields.push_back(not_applicable_field(
        "storage_environment_digest", "sha256", "row-has-no-output-commit"));
  }
  fields.push_back(
      known_field("environment_class_digest", "sha256", class_digest));
  fields.push_back(known_field("workload_manifest_digest", "sha256",
                               section_digest(input.workload_manifest)));
  fields.push_back(known_field("job_instance_index_digest", "sha256",
                               section_digest(job_index)));
  fields.push_back(known_field("measurement_evidence_digest", "sha256",
                               section_digest(input.measurement_evidence)));
  fields.push_back(known_field("output_evidence_digest", "sha256",
                               section_digest(input.output_evidence)));
  fields.push_back(known_field("verdict_evidence_digest", "sha256",
                               section_digest(input.verdict_evidence)));
  if (is_m1) {
    fields.push_back(known_field("paired_isolated_i1",
                                 "evidence-pair-reference-v1",
                                 encode_pair(*input.paired_isolated_i1)));
    fields.push_back(known_field("paired_isolated_b1_cap8",
                                 "evidence-pair-reference-v1",
                                 encode_pair(*input.paired_isolated_b1_cap8)));
  } else {
    fields.push_back(not_applicable_field("paired_isolated_i1",
                                          "evidence-pair-reference-v1",
                                          "row-has-no-isolated-pair"));
    fields.push_back(not_applicable_field("paired_isolated_b1_cap8",
                                          "evidence-pair-reference-v1",
                                          "row-has-no-isolated-pair"));
  }
  const std::string bytes =
      encode_b1_canonical_manifest(kEvidenceRowSchema, fields);
  static_cast<void>(parse_evidence_row(bytes));
  return EvidenceCanonicalRow{std::move(input), std::move(job_index), bytes,
                              digest_evidence_row(bytes)};
}

/** @copydoc parse_evidence_row */
EvidenceParsedRow parse_evidence_row(std::string_view bytes) {
  const B1CanonicalManifest manifest = parse_b1_canonical_manifest(bytes);
  if (manifest.schema != kEvidenceRowSchema ||
      manifest.fields.size() != kRowFieldNames.size()) {
    throw std::invalid_argument("Evidence row schema or field count drifted.");
  }
  for (std::size_t index = 0U; index < manifest.fields.size(); ++index) {
    if (manifest.fields[index].name != kRowFieldNames[index] ||
        manifest.fields[index].type != kRowFieldTypes[index]) {
      throw std::invalid_argument(
          "Evidence row field name/type order drifted.");
    }
  }
  for (std::size_t index = 0U; index < 6U; ++index) {
    require_known_field(manifest.fields[index], kRowFieldNames[index],
                        kRowFieldTypes[index]);
  }
  require_known_field(manifest.fields[7U], kRowFieldNames[7U],
                      kRowFieldTypes[7U]);
  for (std::size_t index = 8U; index <= 12U; ++index) {
    require_known_field(manifest.fields[index], kRowFieldNames[index],
                        kRowFieldTypes[index]);
    if (!valid_digest(manifest.fields[index].payload)) {
      throw std::invalid_argument("Evidence row section digest is invalid.");
    }
  }
  EvidenceParsedRow row;
  row.workload_id = manifest.fields[0U].payload;
  if (!valid_workload(row.workload_id)) {
    throw std::invalid_argument("Evidence row workload-id-v1 is invalid.");
  }
  row.subject_role = parse_subject_role(manifest.fields[1U].payload);
  row.replicate_ordinal =
      parse_b1_canonical_uint64(manifest.fields[2U].payload);
  row.run_cap = parse_b1_canonical_uint64(manifest.fields[3U].payload);
  if (row.replicate_ordinal == 0U || row.replicate_ordinal > 3U) {
    throw std::invalid_argument("Evidence row replicate ordinal is invalid.");
  }
  validate_run_cap(row.workload_id, row.run_cap);
  if (!valid_digest(manifest.fields[4U].payload) ||
      !valid_digest(manifest.fields[7U].payload)) {
    throw std::invalid_argument("Evidence row environment digest is invalid.");
  }
  const bool storage_required =
      row.workload_id == kB1WorkloadId || row.workload_id == kM1WorkloadId;
  if (manifest.fields[5U].payload !=
      (storage_required ? "required" : "not-applicable")) {
    throw std::invalid_argument(
        "Evidence row storage applicability is invalid.");
  }
  if (storage_required) {
    require_known_field(manifest.fields[6U], kRowFieldNames[6U],
                        kRowFieldTypes[6U]);
    if (!valid_digest(manifest.fields[6U].payload)) {
      throw std::invalid_argument("Evidence row storage digest is invalid.");
    }
  } else if (manifest.fields[6U].state != B1ObservationState::NotApplicable ||
             manifest.fields[6U].reason != "row-has-no-output-commit" ||
             !manifest.fields[6U].payload.empty()) {
    throw std::invalid_argument(
        "Evidence row storage N/A encoding is invalid.");
  }
  for (std::size_t index = 8U; index <= 12U; ++index) {
    row.section_digests.push_back(manifest.fields[index].payload);
  }
  const bool is_m1 = row.workload_id == kM1WorkloadId;
  for (std::size_t index = 13U; index <= 14U; ++index) {
    if (is_m1) {
      require_known_field(manifest.fields[index], kRowFieldNames[index],
                          kRowFieldTypes[index]);
    } else if (manifest.fields[index].state !=
                   B1ObservationState::NotApplicable ||
               manifest.fields[index].reason != "row-has-no-isolated-pair" ||
               !manifest.fields[index].payload.empty()) {
      throw std::invalid_argument(
          "Evidence row isolated-pair N/A encoding is invalid.");
    }
  }
  if (is_m1) {
    row.paired_isolated_i1 = parse_pair(manifest.fields[13U].payload);
    row.paired_isolated_b1_cap8 = parse_pair(manifest.fields[14U].payload);
    if (row.paired_isolated_i1->replicate_ordinal != row.replicate_ordinal ||
        row.paired_isolated_b1_cap8->replicate_ordinal !=
            row.replicate_ordinal) {
      throw std::invalid_argument(
          "Evidence M1 pair ordinal does not equal its row ordinal.");
    }
  }
  if (encode_b1_canonical_manifest(manifest.schema, manifest.fields) != bytes) {
    throw std::invalid_argument("Evidence row bytes are not canonical.");
  }
  return row;
}

/** @copydoc materialize_evidence_bundle */
EvidenceCanonicalBundle materialize_evidence_bundle(EvidenceBundleInput input) {
  if (!valid_workload(input.workload_id) || input.rows.empty() ||
      input.seal_ordinal == 0U) {
    throw std::invalid_argument(
        "Evidence bundle workload, row list, or seal is invalid.");
  }
  static_cast<void>(evidence_subject_role_name(input.subject_role));
  static_cast<void>(validate_section(input.provenance, "bundle-provenance",
                                     kEvidenceBundleProvenanceSchema));
  if (input.provenance.seal_ordinal >= input.seal_ordinal) {
    throw std::invalid_argument("Evidence bundle depends on later provenance.");
  }
  if ((input.subject_role == EvidenceSubjectRole::Candidate) !=
      input.comparison_reference_bundle_digest.has_value()) {
    throw std::invalid_argument(
        "Evidence bundle comparison applicability contradicts its role.");
  }
  if (input.comparison_reference_bundle_digest.has_value() &&
      !valid_digest(*input.comparison_reference_bundle_digest)) {
    throw std::invalid_argument(
        "Evidence bundle comparison digest is invalid.");
  }

  std::vector<EvidenceRowReference> references;
  references.reserve(input.rows.size());
  for (const EvidenceCanonicalRow& row : input.rows) {
    if (row.source.workload_id != input.workload_id ||
        row.source.subject_role != input.subject_role ||
        row.source.seal_ordinal >= input.seal_ordinal ||
        digest_evidence_row(row.manifest_bytes) != row.digest) {
      throw std::invalid_argument(
          "Evidence bundle row identity, role, digest, or seal drifted.");
    }
    const EvidenceParsedRow parsed = parse_evidence_row(row.manifest_bytes);
    if (parsed.workload_id != row.source.workload_id ||
        parsed.subject_role != row.source.subject_role ||
        parsed.run_cap != row.source.run_cap ||
        parsed.replicate_ordinal != row.source.replicate_ordinal) {
      throw std::invalid_argument(
          "Evidence bundle row source contradicts its canonical bytes.");
    }
    references.push_back(
        EvidenceRowReference{parsed.workload_id, parsed.run_cap,
                             parsed.replicate_ordinal, row.digest});
  }
  std::sort(references.begin(), references.end(), row_reference_precedes);
  for (std::size_t index = 1U; index < references.size(); ++index) {
    if (references[index - 1U] == references[index] ||
        (references[index - 1U].workload_id == references[index].workload_id &&
         references[index - 1U].run_cap == references[index].run_cap &&
         references[index - 1U].replicate_ordinal ==
             references[index].replicate_ordinal)) {
      throw std::invalid_argument(
          "Evidence bundle row list has a duplicate functional key.");
    }
  }
  std::string row_list = std::to_string(references.size()) + ":";
  for (const EvidenceRowReference& reference : references) {
    row_list.append(b1_environment_frame(encode_row_reference(reference)));
  }
  const std::string provenance_digest = digest_evidence_section(
      input.provenance.section_name, input.provenance.schema_id,
      input.provenance.bytes);
  std::vector<B1CanonicalField> fields{
      known_field("workload_id", "workload-id-v1", input.workload_id),
      known_field("subject_role", "enum",
                  evidence_subject_role_name(input.subject_role)),
      known_field("bundle_provenance_digest", "sha256", provenance_digest)};
  if (input.comparison_reference_bundle_digest.has_value()) {
    fields.push_back(known_field("comparison_reference_bundle_digest", "sha256",
                                 *input.comparison_reference_bundle_digest));
  } else {
    fields.push_back(
        not_applicable_field("comparison_reference_bundle_digest", "sha256",
                             "reference-has-no-comparison-baseline"));
  }
  fields.push_back(
      known_field("row_references", "row-reference-list-v1", row_list));
  const std::string bytes =
      encode_b1_canonical_manifest(kEvidenceBundleSchema, fields);
  static_cast<void>(parse_evidence_bundle(bytes));
  return EvidenceCanonicalBundle{std::move(input), std::move(references), bytes,
                                 digest_evidence_bundle(bytes)};
}

/** @copydoc parse_evidence_bundle */
EvidenceParsedBundle parse_evidence_bundle(std::string_view bytes) {
  const B1CanonicalManifest manifest = parse_b1_canonical_manifest(bytes);
  if (manifest.schema != kEvidenceBundleSchema ||
      manifest.fields.size() != kBundleFieldNames.size()) {
    throw std::invalid_argument(
        "Evidence bundle schema or field count drifted.");
  }
  for (std::size_t index = 0U; index < manifest.fields.size(); ++index) {
    if (manifest.fields[index].name != kBundleFieldNames[index] ||
        manifest.fields[index].type != kBundleFieldTypes[index]) {
      throw std::invalid_argument(
          "Evidence bundle field name/type order drifted.");
    }
  }
  require_known_field(manifest.fields[0U], kBundleFieldNames[0U],
                      kBundleFieldTypes[0U]);
  require_known_field(manifest.fields[1U], kBundleFieldNames[1U],
                      kBundleFieldTypes[1U]);
  require_known_field(manifest.fields[2U], kBundleFieldNames[2U],
                      kBundleFieldTypes[2U]);
  require_known_field(manifest.fields[4U], kBundleFieldNames[4U],
                      kBundleFieldTypes[4U]);
  EvidenceParsedBundle bundle;
  bundle.workload_id = manifest.fields[0U].payload;
  if (!valid_workload(bundle.workload_id)) {
    throw std::invalid_argument("Evidence bundle workload-id-v1 is invalid.");
  }
  bundle.subject_role = parse_subject_role(manifest.fields[1U].payload);
  bundle.provenance_digest = manifest.fields[2U].payload;
  if (!valid_digest(bundle.provenance_digest)) {
    throw std::invalid_argument(
        "Evidence bundle provenance digest is invalid.");
  }
  if (bundle.subject_role == EvidenceSubjectRole::Candidate) {
    require_known_field(manifest.fields[3U], kBundleFieldNames[3U],
                        kBundleFieldTypes[3U]);
    if (!valid_digest(manifest.fields[3U].payload)) {
      throw std::invalid_argument(
          "Evidence candidate comparison digest is invalid.");
    }
    bundle.comparison_reference_bundle_digest = manifest.fields[3U].payload;
  } else if (manifest.fields[3U].state != B1ObservationState::NotApplicable ||
             manifest.fields[3U].reason !=
                 "reference-has-no-comparison-baseline" ||
             !manifest.fields[3U].payload.empty()) {
    throw std::invalid_argument(
        "Evidence reference comparison N/A encoding is invalid.");
  }
  const std::vector<std::string> records =
      parse_b1_framed_list(manifest.fields[4U].payload);
  if (records.empty() || !std::is_sorted(records.begin(), records.end())) {
    throw std::invalid_argument(
        "Evidence bundle row-reference list is empty or unordered.");
  }
  for (const std::string& record : records) {
    EvidenceRowReference reference = parse_row_reference(record);
    if (reference.workload_id != bundle.workload_id) {
      throw std::invalid_argument(
          "Evidence bundle row-reference workload differs.");
    }
    bundle.row_references.push_back(std::move(reference));
  }
  if (!std::is_sorted(bundle.row_references.begin(),
                      bundle.row_references.end(), row_reference_precedes)) {
    throw std::invalid_argument(
        "Evidence bundle row-reference semantic order drifted.");
  }
  for (std::size_t index = 1U; index < bundle.row_references.size(); ++index) {
    const EvidenceRowReference& prior = bundle.row_references[index - 1U];
    const EvidenceRowReference& current = bundle.row_references[index];
    if (prior == current ||
        (prior.workload_id == current.workload_id &&
         prior.run_cap == current.run_cap &&
         prior.replicate_ordinal == current.replicate_ordinal)) {
      throw std::invalid_argument(
          "Evidence bundle row functional key is duplicated.");
    }
  }
  if (encode_b1_canonical_manifest(manifest.schema, manifest.fields) != bytes) {
    throw std::invalid_argument("Evidence bundle bytes are not canonical.");
  }
  return bundle;
}

/** @copydoc validate_evidence_corpus */
EvidenceCorpusValidation validate_evidence_corpus(
    const EvidenceCorpus& corpus, std::string_view root_bundle_digest) {
  EvidenceCorpusValidation result;
  if (!valid_digest(root_bundle_digest)) {
    invalidate(&result.reasons, "root bundle digest is not canonical SHA-256");
    return result;
  }

  enum class VisitState : std::uint8_t { Visiting, Complete };
  std::map<std::string, VisitState> visits;
  std::function<void(const EvidenceRetainedSection&)> visit_section;
  std::function<void(const EvidenceCanonicalBundle&)> visit_bundle;
  std::function<void(const EvidenceCanonicalRow&)> visit_row;

  const auto enter = [&visits](const std::string& key) {
    const auto found = visits.find(key);
    if (found != visits.end()) {
      if (found->second == VisitState::Visiting) {
        throw std::invalid_argument(
            "Evidence address dependency graph contains a cycle.");
      }
      return false;
    }
    visits.emplace(key, VisitState::Visiting);
    return true;
  };
  const auto leave = [&visits](const std::string& key) {
    visits.at(key) = VisitState::Complete;
  };

  visit_section = [&](const EvidenceRetainedSection& section) {
    const std::string digest = digest_evidence_section(
        section.section_name, section.schema_id, section.bytes);
    const std::string key = "section:" + digest;
    if (!enter(key)) {
      return;
    }
    static_cast<void>(
        validate_section(section, section.section_name, section.schema_id));
    for (const EvidenceAddressReference& dependency :
         section.address_dependencies) {
      switch (dependency.kind) {
        case EvidenceAddressKind::Section: {
          const EvidenceRetainedSection& target =
              resolve_section(corpus, dependency.digest);
          if (target.seal_ordinal >= section.seal_ordinal) {
            throw std::invalid_argument(
                "Evidence section depends on a later section.");
          }
          visit_section(target);
          break;
        }
        case EvidenceAddressKind::Row: {
          const EvidenceCanonicalRow& target =
              resolve_row(corpus, dependency.digest);
          if (target.source.seal_ordinal >= section.seal_ordinal) {
            throw std::invalid_argument(
                "Evidence section depends on a later row.");
          }
          visit_row(target);
          break;
        }
        case EvidenceAddressKind::Bundle: {
          const EvidenceCanonicalBundle& target =
              resolve_bundle(corpus, dependency.digest);
          if (target.source.seal_ordinal >= section.seal_ordinal) {
            throw std::invalid_argument(
                "Evidence section depends on a later bundle.");
          }
          visit_bundle(target);
          break;
        }
        default:
          throw std::invalid_argument(
              "Evidence section contains an unknown dependency kind.");
      }
    }
    leave(key);
  };

  visit_row = [&](const EvidenceCanonicalRow& row) {
    const std::string key = "row:" + row.digest;
    if (!enter(key)) {
      return;
    }
    if (digest_evidence_row(row.manifest_bytes) != row.digest) {
      throw std::invalid_argument(
          "Retained evidence row digest does not recompute.");
    }
    if (!valid_b1_environment_evidence(row.source.environment)) {
      throw std::invalid_argument(
          "Retained evidence row lacks valid actual environment authority.");
    }
    const EvidenceParsedRow parsed = parse_evidence_row(row.manifest_bytes);
    const EvidenceCanonicalRow rebuilt = materialize_evidence_row(row.source);
    if (rebuilt.manifest_bytes != row.manifest_bytes ||
        rebuilt.digest != row.digest) {
      throw std::invalid_argument(
          "Retained evidence row source does not reproduce its bytes.");
    }
    const std::array<const EvidenceRetainedSection*, 5U> sections{
        &row.source.workload_manifest, &row.job_instance_index,
        &row.source.measurement_evidence, &row.source.output_evidence,
        &row.source.verdict_evidence};
    for (std::size_t index = 0U; index < sections.size(); ++index) {
      const EvidenceRetainedSection* section = sections[index];
      if (section->seal_ordinal >= row.source.seal_ordinal) {
        throw std::invalid_argument(
            "Evidence row section is not sealed before its row.");
      }
      const EvidenceRetainedSection& retained =
          resolve_section(corpus, parsed.section_digests[index]);
      if (!same_section(*section, retained)) {
        throw std::invalid_argument(
            "Evidence row section source differs from its exact-one object.");
      }
      visit_section(retained);
    }
    if (parsed.workload_id == kM1WorkloadId) {
      const EvidenceCanonicalBundle& i1_bundle =
          resolve_bundle(corpus, parsed.paired_isolated_i1->bundle_digest);
      const EvidenceCanonicalBundle& b1_bundle =
          resolve_bundle(corpus, parsed.paired_isolated_b1_cap8->bundle_digest);
      const EvidenceCanonicalRow& i1_row =
          resolve_row(corpus, parsed.paired_isolated_i1->row_digest);
      const EvidenceCanonicalRow& b1_row =
          resolve_row(corpus, parsed.paired_isolated_b1_cap8->row_digest);
      if (i1_bundle.source.seal_ordinal >= row.source.seal_ordinal ||
          b1_bundle.source.seal_ordinal >= row.source.seal_ordinal ||
          i1_row.source.seal_ordinal >= row.source.seal_ordinal ||
          b1_row.source.seal_ordinal >= row.source.seal_ordinal) {
        throw std::invalid_argument(
            "M1 row depends on an unsealed or later isolated pair.");
      }
      visit_bundle(i1_bundle);
      visit_bundle(b1_bundle);
      visit_row(i1_row);
      visit_row(b1_row);
      validate_m1_pair(row, *parsed.paired_isolated_i1, kI1WorkloadId, corpus);
      validate_m1_pair(row, *parsed.paired_isolated_b1_cap8, kB1WorkloadId,
                       corpus);
    }
    leave(key);
  };

  visit_bundle = [&](const EvidenceCanonicalBundle& bundle) {
    const std::string key = "bundle:" + bundle.digest;
    if (!enter(key)) {
      return;
    }
    if (digest_evidence_bundle(bundle.manifest_bytes) != bundle.digest) {
      throw std::invalid_argument(
          "Retained evidence bundle digest does not recompute.");
    }
    const EvidenceParsedBundle parsed =
        parse_evidence_bundle(bundle.manifest_bytes);
    const EvidenceCanonicalBundle rebuilt =
        materialize_evidence_bundle(bundle.source);
    if (rebuilt.manifest_bytes != bundle.manifest_bytes ||
        rebuilt.digest != bundle.digest ||
        rebuilt.row_references != bundle.row_references) {
      throw std::invalid_argument(
          "Retained evidence bundle source does not reproduce its bytes.");
    }
    if (bundle.source.provenance.seal_ordinal >= bundle.source.seal_ordinal) {
      throw std::invalid_argument(
          "Evidence bundle provenance is not sealed before its bundle.");
    }
    const EvidenceRetainedSection& retained_provenance =
        resolve_section(corpus, parsed.provenance_digest);
    if (!same_section(bundle.source.provenance, retained_provenance)) {
      throw std::invalid_argument(
          "Evidence bundle provenance differs from its exact-one object.");
    }
    visit_section(retained_provenance);
    for (const EvidenceRowReference& reference : parsed.row_references) {
      const EvidenceCanonicalRow& row =
          resolve_row(corpus, reference.row_digest);
      if (row.source.seal_ordinal >= bundle.source.seal_ordinal ||
          row.source.workload_id != reference.workload_id ||
          row.source.run_cap != reference.run_cap ||
          row.source.replicate_ordinal != reference.replicate_ordinal ||
          row.source.subject_role != parsed.subject_role) {
        throw std::invalid_argument(
            "Evidence bundle row resolution or sealing is invalid.");
      }
      visit_row(row);
    }
    if (parsed.comparison_reference_bundle_digest.has_value()) {
      const EvidenceCanonicalBundle& reference =
          resolve_bundle(corpus, *parsed.comparison_reference_bundle_digest);
      if (reference.source.seal_ordinal >= bundle.source.seal_ordinal ||
          reference.source.subject_role != EvidenceSubjectRole::Reference ||
          reference.source.workload_id != bundle.source.workload_id) {
        throw std::invalid_argument(
            "Evidence comparison bundle role/workload/sealing is invalid.");
      }
      visit_bundle(reference);
      for (const EvidenceRowReference& candidate_row : parsed.row_references) {
        const EvidenceRowReference* target = nullptr;
        for (const EvidenceRowReference& reference_row :
             reference.row_references) {
          if (reference_row.workload_id != candidate_row.workload_id ||
              reference_row.run_cap != candidate_row.run_cap ||
              reference_row.replicate_ordinal !=
                  candidate_row.replicate_ordinal) {
            continue;
          }
          if (target != nullptr) {
            throw std::invalid_argument(
                "Evidence comparison bundle duplicates a functional row.");
          }
          target = &reference_row;
        }
        if (target == nullptr) {
          throw std::invalid_argument(
              "Evidence comparison bundle lacks one exact functional row.");
        }
        const EvidenceCanonicalRow& candidate_object =
            resolve_row(corpus, candidate_row.row_digest);
        const EvidenceCanonicalRow& target_object =
            resolve_row(corpus, target->row_digest);
        validate_comparison_row(candidate_object, target_object);
      }
    }
    leave(key);
  };

  try {
    const EvidenceCanonicalBundle& root =
        resolve_bundle(corpus, root_bundle_digest);
    visit_bundle(root);
    result.verdict = I1Verdict::Pass;
  } catch (const std::exception& error) {
    invalidate(&result.reasons, error.what());
    result.verdict = I1Verdict::Invalid;
  } catch (...) {
    invalidate(&result.reasons,
               "evidence validation raised a non-standard exception");
    result.verdict = I1Verdict::Invalid;
  }
  return result;
}

}  // namespace ps::benchmark
