/**
 * @file evidence_envelope.cpp
 * @brief Implements canonical execution-profile rows, bundles, and resolution.
 */
#include "benchmark/evidence_envelope.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "benchmark/b1_evidence.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/m1_evidence.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/m1_profile.hpp"   // NOLINT(build/include_subdir)

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

#if defined(_WIN32)
/**
 * @brief Owns one Windows evidence-file handle until verified close.
 * @throws Nothing for construction and destruction.
 * @note Destruction retries cleanup only when explicit close did not succeed;
 * the class never duplicates or reopens the path.
 */
class WindowsEvidenceReadHandle final {
 public:
  /**
   * @brief Takes ownership of one valid native file handle.
   * @param handle Handle returned by `CreateFileW`.
   * @throws Nothing.
   */
  explicit WindowsEvidenceReadHandle(HANDLE handle) noexcept
      : handle_(handle) {}

  /**
   * @brief Closes an unconsumed handle during exception unwinding.
   * @throws Nothing; cleanup failure cannot replace the primary exception.
   * @note Explicit `close` remains the only path that reports close failure.
   */
  ~WindowsEvidenceReadHandle() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) {
      static_cast<void>(::CloseHandle(handle_));
    }
  }

  /** @brief Prevents duplicating one opened-object identity. */
  WindowsEvidenceReadHandle(const WindowsEvidenceReadHandle&) = delete;

  /** @brief Prevents assigning or leaking one opened-object identity. */
  WindowsEvidenceReadHandle& operator=(const WindowsEvidenceReadHandle&) =
      delete;

  /**
   * @brief Returns the still-owned native handle without transferring it.
   * @return Valid handle until `close` succeeds.
   * @throws Nothing.
   */
  HANDLE get() const noexcept { return handle_; }

  /**
   * @brief Closes the handle exactly once and preserves a failure code.
   * @param error Mutable destination for `GetLastError` on failure.
   * @return True only when `CloseHandle` succeeded.
   * @throws Nothing.
   */
  bool close(DWORD* error) noexcept {
    if (::CloseHandle(handle_) == 0) {
      if (error != nullptr) {
        *error = ::GetLastError();
      }
      return false;
    }
    handle_ = INVALID_HANDLE_VALUE;
    return true;
  }

 private:
  /** @brief Sole native handle ownership. */
  HANDLE handle_;
};
#endif

/** @brief Exact ordered names of the canonical five bundle fields. */
constexpr std::array<std::string_view, 5U> kBundleFieldNames{
    "workload_id", "subject_role", "bundle_provenance_digest",
    "comparison_reference_bundle_digest", "row_references"};

/** @brief Exact ordered types of the canonical five bundle fields. */
constexpr std::array<std::string_view, 5U> kBundleFieldTypes{
    "workload-id-v1", "enum", "sha256", "sha256", "row-reference-list-v1"};

/** @brief Exact ordered fields of the reusable pair-object pack. */
constexpr std::array<std::string_view, 15U> kPairObjectFieldNames{
    "row_manifest",
    "bundle_manifest",
    "base_manifest",
    "claimed_base_digest",
    "storage_manifest",
    "claimed_storage_digest",
    "environment_class_manifest",
    "claimed_environment_class_digest",
    "storage_raw_proof",
    "storage_eligibility",
    "fixture_digest",
    "resource_identity",
    "retained_sections",
    "row_seal_ordinal",
    "bundle_seal_ordinal",
};

/** @brief Exact ordered types of the reusable pair-object pack. */
constexpr std::array<std::string_view, 15U> kPairObjectFieldTypes{
    "canonical-text-hex-v1",
    "canonical-text-hex-v1",
    "canonical-text-hex-v1",
    "sha256",
    "canonical-text-hex-v1",
    "sha256",
    "canonical-text-hex-v1",
    "sha256",
    "canonical-text-hex-v1",
    "b1-storage-eligibility-v1",
    "sha256",
    "sha256",
    "evidence-retained-section-list-v1",
    "uint64",
    "uint64",
};

/** @brief Exact retained sections embedded by every one-row pair pack. */
constexpr std::array<std::pair<std::string_view, std::string_view>, 6U>
    kPairSectionContracts{{
        {"workload-manifest", "execution-profile-workload-manifest-v1"},
        {"job-instance-index", kEvidenceJobIndexSchema},
        {"measurement-evidence", "execution-profile-measurement-evidence-v1"},
        {"output-evidence", "execution-profile-output-evidence-v1"},
        {"verdict-evidence", "execution-profile-verdict-evidence-v1"},
        {"bundle-provenance", kEvidenceBundleProvenanceSchema},
    }};

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
 * @brief Exact recomputed isolated-B1 denominator source.
 * @throws Nothing for value construction and copying.
 */
struct B1RateSource final {
  /** @brief Successful pixel-site operations across thirty raw outcomes. */
  std::uint64_t successful_site_operations = 0U;
  /** @brief Exact positive end-minus-start interval in nanoseconds. */
  std::uint64_t duration_ns = 0U;
};

/**
 * @brief Exact M1 claims that must equal resolved isolated raw sources.
 * @throws Nothing for value construction and copying.
 */
struct M1DenominatorClaims final {
  /** @brief Claimed same-ordinal isolated-I1 nearest-rank p99. */
  std::uint64_t isolated_i1_p99_ns = 0U;
  /** @brief Claimed isolated-B1 numerator and interval. */
  B1RateSource isolated_b1;
};

/**
 * @brief Requires one exact ordered all-known field contract.
 * @param manifest Parsed candidate manifest.
 * @param names Exact ordered field names.
 * @param types Exact ordered field types.
 * @return Nothing when the complete contract matches.
 * @throws std::invalid_argument for cardinality/name/type/state drift.
 */
void require_exact_known_fields(const B1CanonicalManifest& manifest,
                                const std::vector<std::string_view>& names,
                                const std::vector<std::string_view>& types) {
  if (names.size() != types.size() || manifest.fields.size() != names.size()) {
    throw std::invalid_argument(
        "Evidence measurement field cardinality is invalid.");
  }
  for (std::size_t index = 0U; index < names.size(); ++index) {
    require_known_field(manifest.fields[index], names[index], types[index]);
  }
}

/**
 * @brief Parses one exact lowercase canonical Boolean token.
 * @param payload Candidate scalar bytes.
 * @return True for `true` and false for `false`.
 * @throws std::invalid_argument for every other spelling.
 */
bool parse_canonical_boolean(std::string_view payload) {
  if (payload == "true") {
    return true;
  }
  if (payload == "false") {
    return false;
  }
  throw std::invalid_argument("Canonical Boolean token is invalid.");
}

/**
 * @brief Decodes canonical lowercase hexadecimal text without normalization.
 * @param payload Even-length lowercase hexadecimal bytes.
 * @return Exact original byte string.
 * @throws std::invalid_argument for empty, odd, or noncanonical input.
 * @throws std::bad_alloc when decoded ownership allocates.
 */
std::string decode_canonical_text_hex(std::string_view payload) {
  if (payload.empty() || payload.size() % 2U != 0U) {
    throw std::invalid_argument("Canonical text-hex payload is invalid.");
  }
  const auto nibble = [](char value) -> std::uint8_t {
    if (value >= '0' && value <= '9') {
      return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
      return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    throw std::invalid_argument("Canonical text-hex digit is invalid.");
  };
  std::string decoded(payload.size() / 2U, '\0');
  for (std::size_t index = 0U; index < decoded.size(); ++index) {
    decoded[index] = static_cast<char>((nibble(payload[index * 2U]) << 4U) |
                                       nibble(payload[index * 2U + 1U]));
  }
  return decoded;
}

/**
 * @brief Encodes one generic list from already canonical record payloads.
 * @param records Complete records in authoritative order.
 * @return Count prefix followed by one frame per record.
 * @throws std::bad_alloc when result ownership allocates.
 */
std::string encode_record_list(const std::vector<std::string>& records) {
  std::string result = std::to_string(records.size()) + ":";
  for (const std::string& record : records) {
    result.append(b1_environment_frame(record));
  }
  return result;
}

/**
 * @brief Encodes one retained section as an exact source record.
 * @param section Complete retained bytes, dependencies, and seal ordinal.
 * @return Five-component fixed record.
 * @throws Section validation and allocation failures unchanged.
 */
std::string encode_pair_section_record(const EvidenceRetainedSection& section) {
  static_cast<void>(
      validate_section(section, section.section_name, section.schema_id));
  return encode_b1_fixed_record(
      {section.section_name, section.schema_id,
       encode_b1_normalized_text(section.bytes),
       encode_address_dependencies(section.address_dependencies),
       std::to_string(section.seal_ordinal)});
}

/**
 * @brief Parses one exact retained-section source record.
 * @param record Complete five-component fixed record.
 * @return Reconstructed retained section.
 * @throws std::invalid_argument for framing, bytes, dependency, or seal drift.
 * @throws std::bad_alloc when source ownership allocates.
 */
EvidenceRetainedSection parse_pair_section_record(std::string_view record) {
  const std::vector<std::string> components = parse_b1_fixed_record(record, 5U);
  EvidenceRetainedSection section{
      components[0U],
      components[1U],
      decode_canonical_text_hex(components[2U]),
      parse_address_dependencies(components[3U]),
      parse_b1_canonical_uint64(components[4U]),
  };
  static_cast<void>(
      validate_section(section, section.section_name, section.schema_id));
  if (encode_pair_section_record(section) != record) {
    throw std::invalid_argument(
        "Evidence pair retained-section record is not canonical.");
  }
  return section;
}

/**
 * @brief Encodes retained storage eligibility without changing its truth set.
 * @param eligibility Complete derived verdict and ordered reasons.
 * @return Two-component fixed record containing a framed reason list.
 * @throws std::bad_alloc when record ownership allocates.
 */
std::string encode_storage_eligibility(
    const B1StorageEligibility& eligibility) {
  std::vector<std::string> reasons;
  reasons.reserve(eligibility.reasons.size());
  for (const std::string& reason : eligibility.reasons) {
    reasons.push_back(encode_b1_fixed_record({reason}));
  }
  return encode_b1_fixed_record(
      {eligibility.eligible ? "true" : "false", encode_record_list(reasons)});
}

/**
 * @brief Parses one retained storage-eligibility fixed record.
 * @param payload Exact Boolean and framed-reason components.
 * @return Reconstructed eligibility truth set.
 * @throws std::invalid_argument for Boolean/list/record drift.
 * @throws std::bad_alloc when reasons allocate.
 */
B1StorageEligibility parse_storage_eligibility(std::string_view payload) {
  const std::vector<std::string> components =
      parse_b1_fixed_record(payload, 2U);
  B1StorageEligibility result;
  result.eligible = parse_canonical_boolean(components[0U]);
  for (const std::string& record : parse_b1_framed_list(components[1U])) {
    result.reasons.push_back(parse_b1_fixed_record(record, 1U)[0U]);
  }
  if (encode_storage_eligibility(result) != payload) {
    throw std::invalid_argument(
        "Evidence pair storage eligibility is not canonical.");
  }
  return result;
}

/**
 * @brief Parses one closed B1 phase token from a job-instance record.
 * @param token Candidate lowercase phase token.
 * @return Cold, warmup, or measured.
 * @throws std::invalid_argument for an unknown token.
 */
B1JobPhase parse_job_phase(std::string_view token) {
  if (token == "cold") {
    return B1JobPhase::Cold;
  }
  if (token == "warmup") {
    return B1JobPhase::Warmup;
  }
  if (token == "measured") {
    return B1JobPhase::Measured;
  }
  throw std::invalid_argument("Evidence pair job phase is invalid.");
}

/**
 * @brief Parses one exact Issue #95 job-instance fixed record.
 * @param record Six-component canonical occurrence identity.
 * @return Validated job occurrence.
 * @throws std::invalid_argument for framing or identity drift.
 */
B1JobInstance parse_job_instance(std::string_view record) {
  const std::vector<std::string> components = parse_b1_fixed_record(record, 6U);
  B1JobInstance job{components[0U],
                    parse_b1_canonical_uint64(components[1U]),
                    parse_job_phase(components[2U]),
                    parse_b1_canonical_uint64(components[3U]),
                    parse_b1_canonical_uint64(components[4U]),
                    parse_b1_canonical_uint64(components[5U])};
  validate_b1_job_instance(job);
  if (encode_b1_job_instance(job) != record) {
    throw std::invalid_argument(
        "Evidence pair job-instance record is not canonical.");
  }
  return job;
}

/**
 * @brief Recovers every job source from an exact job-index section.
 * @param section Validated job-instance-index retained section.
 * @return Canonically ordered occurrence list, possibly empty for I1.
 * @throws std::invalid_argument for schema/field/list/job drift.
 * @throws std::bad_alloc when jobs allocate.
 */
std::vector<B1JobInstance> parse_job_index(
    const EvidenceRetainedSection& section) {
  const B1CanonicalManifest manifest =
      validate_section(section, "job-instance-index", kEvidenceJobIndexSchema);
  if (manifest.fields.size() != 1U) {
    throw std::invalid_argument(
        "Evidence pair job index field cardinality is invalid.");
  }
  require_known_field(manifest.fields[0U], "job_instances",
                      "job-instance-list-v1");
  std::vector<B1JobInstance> result;
  for (const std::string& record :
       parse_b1_framed_list(manifest.fields[0U].payload)) {
    result.push_back(parse_job_instance(record));
  }
  return result;
}

/**
 * @brief Builds one producer-owned retained section without dependencies.
 * @param name Exact row/provenance binding.
 * @param schema Exact retained-section schema.
 * @param fields Complete canonical fields.
 * @param seal_ordinal Nonzero topological seal ordinal.
 * @return Complete retained section.
 * @throws Canonical encoding and allocation failures unchanged.
 */
EvidenceRetainedSection make_producer_section(
    std::string name, std::string schema, std::vector<B1CanonicalField> fields,
    std::uint64_t seal_ordinal) {
  const std::string bytes = encode_b1_canonical_manifest(schema, fields);
  return EvidenceRetainedSection{std::move(name),
                                 std::move(schema),
                                 bytes,
                                 {},
                                 seal_ordinal};
}

/**
 * @brief Computes the shared resource identity from a compatible snapshot.
 * @tparam Snapshot B1ExecutionSnapshot or M1ExecutionSnapshot.
 * @param snapshot Settled Host/device/Compute-I/O state.
 * @return Domain-separated immutable-limit digest.
 * @throws std::bad_alloc when canonical bytes allocate.
 */
template <typename Snapshot>
B1Sha256Digest compute_resource_identity(const Snapshot& snapshot) {
  std::ostringstream canonical;
  const ResourceVector& host = snapshot.host_resources.limits;
  canonical << "execution-profile-b1-resource-identity-v1\n"
            << "worker-count=8\n"
            << "host=" << host.cpu_slots << ',' << host.retained_memory_bytes
            << ',' << host.scratch_bytes << ',' << host.ready_entries << ','
            << host.ready_bytes << '\n';
  for (const ResourceLedger::DeviceSnapshot& device :
       snapshot.device_resources) {
    canonical << "device="
              << static_cast<std::uint32_t>(device.device.backend()) << ','
              << device.device.ordinal() << ','
              << device.limits.device_memory_bytes << ','
              << device.limits.device_scratch_bytes << '\n';
  }
  canonical << "compute-io=" << snapshot.compute_io.task_limit << ','
            << snapshot.compute_io.planned_bytes_limit << '\n';
  return b1_sha256(canonical.str());
}

/**
 * @brief Encodes one typed logical digest for the frozen B1 fixture identity.
 * @param digest Exact independently initialized logical golden digest.
 * @return Algorithm-number-prefixed lowercase hexadecimal bytes.
 * @throws std::bad_alloc when the canonical identity cannot allocate.
 * @note This preserves the Issue #95 fixture identity byte-for-byte; pair-pack
 * production must not silently rename an already frozen B1 fixture.
 */
std::string encode_logical_digest_identity(const ContentDigest& digest) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string result =
      std::to_string(static_cast<std::uint32_t>(digest.algorithm));
  result.push_back(':');
  result.reserve(result.size() + digest.bytes.size() * 2U);
  for (const std::byte byte_value : digest.bytes) {
    const auto value = std::to_integer<std::uint8_t>(byte_value);
    result.push_back(kHex[value >> 4U]);
    result.push_back(kHex[value & 0x0fU]);
  }
  return result;
}

/**
 * @brief Validates explicit absence of portable output and verdict authority.
 * @param row Isolated denominator row whose non-measurement claims are closed.
 * @return Nothing for exact denominator-only scope declarations.
 * @throws std::invalid_argument for schema, field, or authority drift.
 * @throws std::bad_alloc when canonical manifests allocate.
 */
void validate_pair_denominator_claim_scope(const EvidenceCanonicalRow& row) {
  const std::string_view pair_source_schema =
      row.source.workload_id == kI1WorkloadId
          ? std::string_view(kEvidenceI1PairDenominatorSchema)
          : std::string_view(kEvidenceB1PairDenominatorSchema);
  const B1CanonicalManifest output =
      parse_b1_canonical_manifest(row.source.output_evidence.bytes);
  if (output.schema != "execution-profile-output-evidence-v1") {
    throw std::invalid_argument(
        "Paired denominator output claim schema is invalid.");
  }
  require_exact_known_fields(
      output,
      {"pair_source_schema", "portable_output_claim_schema",
       "portable_output_authority"},
      {"identifier", "identifier", "enum"});
  if (output.fields[0U].payload != pair_source_schema ||
      output.fields[1U].payload != kEvidencePairNoOutputClaimSchema ||
      output.fields[2U].payload != "not-claimed") {
    throw std::invalid_argument(
        "Paired denominator unexpectedly claims portable output authority.");
  }

  const B1CanonicalManifest verdict =
      parse_b1_canonical_manifest(row.source.verdict_evidence.bytes);
  if (verdict.schema != "execution-profile-verdict-evidence-v1") {
    throw std::invalid_argument(
        "Paired denominator verdict claim schema is invalid.");
  }
  require_exact_known_fields(
      verdict,
      {"pair_source_schema", "portable_claim_schema", "portable_claim_scope"},
      {"identifier", "identifier", "enum"});
  if (verdict.fields[0U].payload != pair_source_schema ||
      verdict.fields[1U].payload != kEvidencePairNoVerdictClaimSchema ||
      verdict.fields[2U].payload != "denominator-only") {
    throw std::invalid_argument(
        "Paired denominator unexpectedly claims non-denominator verdicts.");
  }
}

/**
 * @brief Recomputes isolated-I1 p99 from exactly 200 canonical raw samples.
 * @param row Exact resolved isolated-I1 row.
 * @return Positive nearest-rank p99 in nanoseconds.
 * @throws std::invalid_argument for schema/cardinality/scalar/claim drift.
 * @throws std::bad_alloc when samples allocate or sort.
 */
std::uint64_t recompute_isolated_i1_p99(const EvidenceCanonicalRow& row) {
  validate_pair_denominator_claim_scope(row);
  const B1CanonicalManifest manifest =
      parse_b1_canonical_manifest(row.source.measurement_evidence.bytes);
  if (manifest.schema != "execution-profile-measurement-evidence-v1") {
    throw std::invalid_argument(
        "Paired isolated-I1 measurement schema is invalid.");
  }
  require_exact_known_fields(
      manifest,
      {"pair_source_schema", "subject_role", "replicate_ordinal",
       "source_inner_schema_version", "measured_final_latencies_ns",
       "claimed_p99_ns"},
      {"identifier", "enum", "uint64", "uint64", "uint64-list-v1", "uint64"});
  if (manifest.fields[0U].payload != kEvidenceI1PairDenominatorSchema ||
      manifest.fields[1U].payload !=
          evidence_subject_role_name(row.source.subject_role) ||
      parse_b1_canonical_uint64(manifest.fields[2U].payload) !=
          row.source.replicate_ordinal ||
      parse_b1_canonical_uint64(manifest.fields[3U].payload) !=
          kI1InnerRowSchemaVersion) {
    throw std::invalid_argument(
        "Paired isolated-I1 measurement identity is invalid.");
  }
  const std::vector<std::string> records =
      parse_b1_framed_list(manifest.fields[4U].payload);
  if (records.size() != 200U) {
    throw std::invalid_argument(
        "Paired isolated-I1 requires exactly 200 latency samples.");
  }
  std::vector<std::uint64_t> samples;
  samples.reserve(records.size());
  for (const std::string& record : records) {
    const std::vector<std::string> fields = parse_b1_fixed_record(record, 1U);
    const std::uint64_t sample = parse_b1_canonical_uint64(fields[0U]);
    if (sample == 0U) {
      throw std::invalid_argument(
          "Paired isolated-I1 contains a zero latency sample.");
    }
    samples.push_back(sample);
  }
  std::sort(samples.begin(), samples.end());
  const std::uint64_t recomputed = samples[197U];
  const std::uint64_t claimed =
      parse_b1_canonical_uint64(manifest.fields[5U].payload);
  if (claimed == 0U || claimed != recomputed) {
    throw std::invalid_argument(
        "Paired isolated-I1 p99 claim does not recompute.");
  }
  return recomputed;
}

/**
 * @brief Recomputes isolated-B1 numerator and interval from thirty outcomes.
 * @param row Exact resolved isolated-B1 cap-eight row.
 * @return Positive exact throughput source tuple.
 * @throws std::invalid_argument for schema/cardinality/outcome/claim drift.
 * @throws std::bad_alloc when framed records allocate.
 */
B1RateSource recompute_isolated_b1_rate_source(
    const EvidenceCanonicalRow& row) {
  validate_pair_denominator_claim_scope(row);
  const B1CanonicalManifest manifest =
      parse_b1_canonical_manifest(row.source.measurement_evidence.bytes);
  if (manifest.schema != "execution-profile-measurement-evidence-v1") {
    throw std::invalid_argument(
        "Paired isolated-B1 measurement schema is invalid.");
  }
  require_exact_known_fields(
      manifest,
      {"pair_source_schema", "subject_role", "replicate_ordinal",
       "source_inner_schema_version", "measurement_start_ns",
       "measurement_end_ns", "measured_job_outcomes",
       "successful_site_operations"},
      {"identifier", "enum", "uint64", "uint64", "uint64", "uint64",
       "b1-measured-job-outcome-list-v1", "uint64"});
  if (manifest.fields[0U].payload != kEvidenceB1PairDenominatorSchema ||
      manifest.fields[1U].payload !=
          evidence_subject_role_name(row.source.subject_role) ||
      parse_b1_canonical_uint64(manifest.fields[2U].payload) !=
          row.source.replicate_ordinal ||
      parse_b1_canonical_uint64(manifest.fields[3U].payload) !=
          kB1InnerRowSchemaVersion) {
    throw std::invalid_argument(
        "Paired isolated-B1 measurement identity is invalid.");
  }
  const std::vector<B1JobInstance> jobs =
      parse_job_index(row.job_instance_index);
  std::set<B1JobInstance> unique_jobs;
  std::size_t cold_count = 0U;
  std::size_t warmup_count = 0U;
  std::size_t measured_count = 0U;
  for (const B1JobInstance& job : jobs) {
    validate_b1_job_instance(job);
    if (job.row_workload_id != kB1WorkloadId ||
        job.replicate_ordinal != row.source.replicate_ordinal ||
        job.run_cap != row.source.run_cap || !unique_jobs.insert(job).second) {
      throw std::invalid_argument(
          "Paired isolated-B1 job index is duplicated or mismatched.");
    }
    switch (job.phase) {
      case B1JobPhase::Cold:
        ++cold_count;
        break;
      case B1JobPhase::Warmup:
        ++warmup_count;
        break;
      case B1JobPhase::Measured:
        ++measured_count;
        break;
    }
  }
  if (jobs.size() != 1U + kB1WarmupJobCount + kB1MeasuredJobCount ||
      unique_jobs.size() != jobs.size() || cold_count != 1U ||
      warmup_count != kB1WarmupJobCount ||
      measured_count != kB1MeasuredJobCount) {
    throw std::invalid_argument(
        "Paired isolated-B1 job index is not exact 1+3+30 evidence.");
  }
  const std::uint64_t start =
      parse_b1_canonical_uint64(manifest.fields[4U].payload);
  const std::uint64_t end =
      parse_b1_canonical_uint64(manifest.fields[5U].payload);
  if (end <= start) {
    throw std::invalid_argument(
        "Paired isolated-B1 measurement interval is not positive.");
  }
  const std::vector<std::string> records =
      parse_b1_framed_list(manifest.fields[6U].payload);
  if (records.size() != kB1MeasuredJobCount) {
    throw std::invalid_argument(
        "Paired isolated-B1 requires exactly 30 raw job outcomes.");
  }
  std::uint64_t successful = 0U;
  for (std::size_t index = 0U; index < records.size(); ++index) {
    const std::vector<std::string> fields =
        parse_b1_fixed_record(records[index], 3U);
    if (parse_b1_canonical_uint64(fields[0U]) != index ||
        (fields[1U] != "true" && fields[1U] != "false")) {
      throw std::invalid_argument(
          "Paired isolated-B1 job outcome identity is invalid.");
    }
    const bool verified = fields[1U] == "true";
    const std::uint64_t site_operations = parse_b1_canonical_uint64(fields[2U]);
    const std::uint64_t expected = verified ? kB1SiteOperationsPerJob : 0U;
    if (site_operations != expected) {
      throw std::invalid_argument(
          "Paired isolated-B1 job outcome charge is invalid.");
    }
    if (successful >
        std::numeric_limits<std::uint64_t>::max() - site_operations) {
      throw std::invalid_argument(
          "Paired isolated-B1 successful operations overflow.");
    }
    successful += site_operations;
  }
  const std::uint64_t claimed =
      parse_b1_canonical_uint64(manifest.fields[7U].payload);
  if (successful == 0U || claimed != successful) {
    throw std::invalid_argument(
        "Paired isolated-B1 numerator claim does not recompute.");
  }
  return B1RateSource{successful, end - start};
}

/**
 * @brief Parses exact M1 denominator claims and checks retained raw shape.
 * @param row Exact resolved M1 row.
 * @return Positive claims duplicated exactly by its nested inner row.
 * @throws std::invalid_argument for section/inner/cardinality/claim drift.
 * @throws std::bad_alloc when canonical lists allocate.
 */
M1DenominatorClaims parse_m1_denominator_claims(
    const EvidenceCanonicalRow& row) {
  const B1CanonicalManifest measurement =
      parse_b1_canonical_manifest(row.source.measurement_evidence.bytes);
  if (measurement.schema != "execution-profile-measurement-evidence-v1") {
    throw std::invalid_argument("M1 measurement schema is invalid.");
  }
  require_exact_known_fields(
      measurement,
      {"m1_inner_row", "paired_isolated_i1_p99_ns",
       "paired_isolated_b1_successful_site_operations",
       "paired_isolated_b1_duration_ns"},
      {"canonical-text-hex-v1", "uint64", "uint64", "uint64"});
  M1DenominatorClaims claims{
      parse_b1_canonical_uint64(measurement.fields[1U].payload),
      B1RateSource{parse_b1_canonical_uint64(measurement.fields[2U].payload),
                   parse_b1_canonical_uint64(measurement.fields[3U].payload)}};
  if (claims.isolated_i1_p99_ns == 0U ||
      claims.isolated_b1.successful_site_operations == 0U ||
      claims.isolated_b1.duration_ns == 0U) {
    throw std::invalid_argument("M1 denominator claim is zero.");
  }

  const B1CanonicalManifest inner = parse_b1_canonical_manifest(
      decode_canonical_text_hex(measurement.fields[0U].payload));
  const std::vector<std::string_view> names{"schema_version",
                                            "replicate_ordinal",
                                            "boundaries",
                                            "protocol_flags",
                                            "interactive_occurrences",
                                            "batch_offers",
                                            "carryover",
                                            "first_measured_admission",
                                            "progress_windows",
                                            "graph_service_windows",
                                            "class_starts",
                                            "headroom_outcomes",
                                            "batch_io_streams",
                                            "temporal_snapshots",
                                            "mixed_observations",
                                            "paired_isolated_i1_p99_ns",
                                            "paired_isolated_b1_source",
                                            "batch_waste",
                                            "verdicts"};
  const std::vector<std::string_view> types{"uint64",
                                            "uint64",
                                            "m1-boundary-record-v1",
                                            "m1-protocol-flags-v1",
                                            "m1-i1-occurrence-list-v1",
                                            "m1-b1-offer-list-v1",
                                            "m1-carryover-list-v1",
                                            "m1-first-admission-record-v1",
                                            "m1-progress-window-list-v1",
                                            "m1-graph-service-window-list-v1",
                                            "m1-class-start-list-v1",
                                            "m1-headroom-outcome-list-v1",
                                            "m1-b1-io-stream-list-v1",
                                            "m1-execution-snapshot-list-v1",
                                            "m1-observation-list-v1",
                                            "uint64",
                                            "m1-b1-rate-source-v1",
                                            "m1-batch-waste-record-v1",
                                            "m1-five-axis-verdict-record-v1"};
  if (inner.schema != kM1InnerRowSchema) {
    throw std::invalid_argument("M1 nested inner schema is invalid.");
  }
  require_exact_known_fields(inner, names, types);
  if (parse_b1_canonical_uint64(inner.fields[0U].payload) !=
          kM1InnerRowSchemaVersion ||
      parse_b1_canonical_uint64(inner.fields[1U].payload) !=
          row.source.replicate_ordinal) {
    throw std::invalid_argument("M1 nested schema version or ordinal drifted.");
  }
  const std::vector<std::string> protocol_flags =
      parse_b1_fixed_record(inner.fields[3U].payload, 12U);
  for (const std::string& flag : protocol_flags) {
    static_cast<void>(parse_canonical_boolean(flag));
  }
  const std::size_t interactive_count =
      parse_b1_framed_list(inner.fields[4U].payload).size();
  const std::vector<std::string> offers =
      parse_b1_framed_list(inner.fields[5U].payload);
  const std::vector<std::string> progress =
      parse_b1_framed_list(inner.fields[8U].payload);
  const std::vector<std::string> graph =
      parse_b1_framed_list(inner.fields[9U].payload);
  const std::vector<std::string> headroom =
      parse_b1_framed_list(inner.fields[11U].payload);
  const std::vector<std::string> io =
      parse_b1_framed_list(inner.fields[12U].payload);
  const std::vector<std::string> snapshots =
      parse_b1_framed_list(inner.fields[13U].payload);
  if (interactive_count != kM1TotalI1OriginCount || offers.empty() ||
      progress.size() != kM1MeasuredWindowCount ||
      graph.size() != kM1MeasuredWindowCount ||
      headroom.size() != kM1MeasuredI1AttemptCount ||
      io.size() != offers.size() || snapshots.size() < 4U) {
    throw std::invalid_argument("M1 nested raw evidence cardinality drifted.");
  }
  for (std::size_t index = 0U; index < progress.size(); ++index) {
    const std::vector<std::string> fields =
        parse_b1_fixed_record(progress[index], 3U);
    if (parse_b1_canonical_uint64(fields[0U]) != index ||
        parse_b1_canonical_uint64(fields[2U]) == 0U) {
      throw std::invalid_argument("M1 raw progress window is invalid.");
    }
    static_cast<void>(parse_b1_canonical_uint64(fields[1U]));
  }
  for (std::size_t index = 0U; index < graph.size(); ++index) {
    const std::vector<std::string> fields =
        parse_b1_fixed_record(graph[index], 4U);
    if (parse_b1_canonical_uint64(fields[0U]) != index) {
      throw std::invalid_argument("M1 raw Graph window is unordered.");
    }
    static_cast<void>(parse_canonical_boolean(fields[1U]));
    static_cast<void>(parse_b1_canonical_uint64(fields[2U]));
    static_cast<void>(parse_b1_canonical_uint64(fields[3U]));
  }
  for (std::size_t index = 0U; index < headroom.size(); ++index) {
    const std::vector<std::string> fields =
        parse_b1_fixed_record(headroom[index], 10U);
    if (parse_b1_canonical_uint64(fields[0U]) != index / kI1EditCount ||
        parse_b1_canonical_uint64(fields[1U]) != index % kI1EditCount) {
      throw std::invalid_argument("M1 raw headroom outcome is unordered.");
    }
    const bool attempted = parse_canonical_boolean(fields[2U]);
    const bool has_status = parse_canonical_boolean(fields[3U]);
    const bool status_ok = parse_canonical_boolean(fields[4U]);
    static_cast<void>(parse_b1_canonical_uint64(fields[5U]));
    const bool headroom_failure = parse_canonical_boolean(fields[9U]);
    if (!attempted || !has_status || headroom_failure == status_ok) {
      throw std::invalid_argument(
          "M1 raw headroom status or classification is invalid.");
    }
  }
  const std::vector<std::string> starts =
      parse_b1_framed_list(inner.fields[10U].payload);
  std::uint64_t prior_start_sequence = 0U;
  for (const std::string& start : starts) {
    const std::vector<std::string> fields = parse_b1_fixed_record(start, 5U);
    const std::uint64_t sequence = parse_b1_canonical_uint64(fields[0U]);
    const std::uint64_t service_class = parse_b1_canonical_uint64(fields[1U]);
    if (sequence == 0U || sequence <= prior_start_sequence ||
        service_class > static_cast<std::uint64_t>(
                            compute::ComputeRunQosClass::Throughput) ||
        !parse_canonical_boolean(fields[4U])) {
      throw std::invalid_argument("M1 raw class start is invalid.");
    }
    static_cast<void>(parse_canonical_boolean(fields[2U]));
    static_cast<void>(parse_canonical_boolean(fields[3U]));
    prior_start_sequence = sequence;
  }
  const std::uint64_t inner_i1 =
      parse_b1_canonical_uint64(inner.fields[15U].payload);
  const std::vector<std::string> inner_b1 =
      parse_b1_fixed_record(inner.fields[16U].payload, 2U);
  if (inner_i1 != claims.isolated_i1_p99_ns ||
      parse_b1_canonical_uint64(inner_b1[0U]) !=
          claims.isolated_b1.successful_site_operations ||
      parse_b1_canonical_uint64(inner_b1[1U]) !=
          claims.isolated_b1.duration_ns) {
    throw std::invalid_argument(
        "M1 inner and measurement denominator claims disagree.");
  }
  return claims;
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
    const M1DenominatorClaims claims = parse_m1_denominator_claims(m1);
    if (recompute_isolated_i1_p99(row) != claims.isolated_i1_p99_ns) {
      throw std::invalid_argument(
          "M1 isolated-I1 denominator differs from resolved raw evidence.");
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
    const M1DenominatorClaims claims = parse_m1_denominator_claims(m1);
    const B1RateSource source = recompute_isolated_b1_rate_source(row);
    if (source.successful_site_operations !=
            claims.isolated_b1.successful_site_operations ||
        source.duration_ns != claims.isolated_b1.duration_ns) {
      throw std::invalid_argument(
          "M1 isolated-B1 denominator differs from resolved raw evidence.");
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

/** @copydoc evidence_i1_component_fixture_digest */
B1Sha256Digest evidence_i1_component_fixture_digest() {
  B1Sha256 hash;
  hash.update("execution-profile-m1-i1-fixture-v1\n");
  hash.update(i1_frozen_graph_yaml());
  const ContentDigest golden = i1_frozen_final_content_digest();
  for (const std::byte value : golden.bytes) {
    const char byte = static_cast<char>(std::to_integer<std::uint8_t>(value));
    hash.update(std::string_view(&byte, 1U));
  }
  return hash.finish();
}

/** @copydoc evidence_b1_component_digests */
EvidenceB1ComponentDigests evidence_b1_component_digests() {
  B1Sha256 fixture;
  B1Sha256 corpus;
  B1Sha256 golden;
  fixture.update("execution-profile-b1-fixture-identity-v1\n");
  corpus.update("execution-profile-b1-corpus-identity-v1\n");
  golden.update("execution-profile-b1-golden-identity-v1\n");
  const auto add = [&](std::uint64_t seed) {
    const std::string seed_text = std::to_string(seed) + "\n";
    const std::string graph = b1_frozen_graph_yaml(seed);
    const std::string source = b1_source_node_yaml(seed);
    const std::string trace = encode_b1_semantic_trace(
        make_b1_success_semantic_records(b1_frozen_semantic_plan(seed)));
    const B1JobGolden job_golden = b1_frozen_job_golden(seed);
    fixture.update(seed_text);
    fixture.update(graph);
    fixture.update(source);
    fixture.update(trace);
    corpus.update(seed_text);
    corpus.update(graph);
    corpus.update(source);
    corpus.update(trace);
    const std::string logical_identity =
        encode_logical_digest_identity(job_golden.logical_digest);
    fixture.update(logical_identity);
    fixture.update("\n");
    const std::string algorithm = std::to_string(
        static_cast<std::uint32_t>(job_golden.logical_digest.algorithm));
    golden.update(seed_text);
    golden.update(algorithm);
    golden.update("\n");
    for (const std::byte value : job_golden.logical_digest.bytes) {
      const char byte = static_cast<char>(std::to_integer<std::uint8_t>(value));
      golden.update(std::string_view(&byte, 1U));
    }
    fixture.update(b1_digest_hex(job_golden.raw_payload_digest));
    fixture.update("\n");
    golden.update("\n");
    golden.update(b1_digest_hex(job_golden.raw_payload_digest));
    golden.update("\n");
  };
  add(kB1ColdJobIndex);
  for (const std::uint64_t seed : kB1WarmupJobIndices) {
    add(seed);
  }
  for (std::uint64_t seed = 0U; seed < kB1MeasuredJobCount; ++seed) {
    add(seed);
  }
  return EvidenceB1ComponentDigests{fixture.finish(), corpus.finish(),
                                    golden.finish()};
}

/** @copydoc evidence_resource_identity */
B1Sha256Digest evidence_resource_identity(const B1ExecutionSnapshot& snapshot) {
  return compute_resource_identity(snapshot);
}

/** @copydoc evidence_resource_identity */
B1Sha256Digest evidence_resource_identity(const M1ExecutionSnapshot& snapshot) {
  return compute_resource_identity(snapshot);
}

/** @copydoc make_i1_evidence_pair_object */
EvidencePairObject make_i1_evidence_pair_object(
    const std::vector<I1EpisodeInnerRow>& rows,
    B1EnvironmentEvidence environment, EvidencePairProducerOptions options) {
  const B1Sha256Digest fixture = evidence_i1_component_fixture_digest();
  if (environment.workload_id != kI1WorkloadId || environment.run_cap != 8U ||
      environment.replicate_ordinal == 0U ||
      environment.replicate_ordinal > 3U ||
      environment.fixture_digest != fixture ||
      environment.storage_manifest.has_value() ||
      !valid_b1_environment_claims(environment)) {
    throw std::invalid_argument(
        "I1 pair producer environment identity or claims are invalid.");
  }
  if ((options.subject_role == EvidenceSubjectRole::Candidate) !=
      options.comparison_reference_bundle_digest.has_value()) {
    throw std::invalid_argument(
        "I1 pair producer comparison direction contradicts its role.");
  }
  const I1ReplicateSummary summary = evaluate_i1_replicate(rows);
  if (summary.replicate_ordinal != environment.replicate_ordinal ||
      summary.measured_sample_count != 200U || !summary.latency.has_value() ||
      summary.latency->p99.count() <= 0 ||
      summary.latency_verdict == I1Verdict::Invalid) {
    throw std::invalid_argument(
        "I1 pair producer lacks one complete positive measured replicate.");
  }

  std::vector<const I1EpisodeInnerRow*> ordered;
  ordered.reserve(rows.size());
  for (const I1EpisodeInnerRow& row : rows) {
    ordered.push_back(&row);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const I1EpisodeInnerRow* lhs, const I1EpisodeInnerRow* rhs) {
              return lhs->evidence.slot < rhs->evidence.slot;
            });
  std::vector<std::string> samples;
  for (const I1EpisodeInnerRow* row : ordered) {
    if (row->schema != kI1InnerRowSchema ||
        row->schema_version != kI1InnerRowSchemaVersion ||
        row->workload_id != kI1WorkloadId ||
        row->evidence.replicate_ordinal != environment.replicate_ordinal) {
      throw std::invalid_argument(
          "I1 pair producer row identity differs from its environment.");
    }
    if (classify_i1_slot(row->evidence.slot).first !=
        I1EpisodePhase::Measured) {
      continue;
    }
    if (!row->final_latency.has_value() || row->final_latency->count() <= 0) {
      throw std::invalid_argument(
          "I1 pair producer measured row lacks positive final latency.");
    }
    samples.push_back(
        encode_b1_fixed_record({std::to_string(row->final_latency->count())}));
  }
  if (samples.size() != 200U) {
    throw std::invalid_argument(
        "I1 pair producer did not retain exactly 200 measured samples.");
  }

  EvidenceRowInput input;
  input.workload_id = kI1WorkloadId;
  input.subject_role = options.subject_role;
  input.replicate_ordinal = environment.replicate_ordinal;
  input.run_cap = 8U;
  input.environment = std::move(environment);
  input.workload_manifest = make_producer_section(
      "workload-manifest", "execution-profile-workload-manifest-v1",
      {known_field("fixture_digest", "sha256", b1_digest_hex(fixture)),
       known_field("i1_fixture_digest", "sha256", b1_digest_hex(fixture)),
       known_field("row_identity_digest", "sha256",
                   b1_digest_hex(b1_sha256(
                       std::string(kI1WorkloadId) + ":" +
                       evidence_subject_role_name(options.subject_role) + ":" +
                       std::to_string(input.replicate_ordinal))))},
      1U);
  input.job_index_seal_ordinal = 2U;
  input.measurement_evidence = make_producer_section(
      "measurement-evidence", "execution-profile-measurement-evidence-v1",
      {known_field("pair_source_schema", "identifier",
                   kEvidenceI1PairDenominatorSchema),
       known_field("subject_role", "enum",
                   evidence_subject_role_name(options.subject_role)),
       known_field("replicate_ordinal", "uint64",
                   std::to_string(input.replicate_ordinal)),
       known_field("source_inner_schema_version", "uint64",
                   std::to_string(kI1InnerRowSchemaVersion)),
       known_field("measured_final_latencies_ns", "uint64-list-v1",
                   encode_record_list(samples)),
       known_field("claimed_p99_ns", "uint64",
                   std::to_string(summary.latency->p99.count()))},
      3U);
  input.output_evidence = make_producer_section(
      "output-evidence", "execution-profile-output-evidence-v1",
      {known_field("pair_source_schema", "identifier",
                   kEvidenceI1PairDenominatorSchema),
       known_field("portable_output_claim_schema", "identifier",
                   kEvidencePairNoOutputClaimSchema),
       known_field("portable_output_authority", "enum", "not-claimed")},
      4U);
  input.verdict_evidence = make_producer_section(
      "verdict-evidence", "execution-profile-verdict-evidence-v1",
      {known_field("pair_source_schema", "identifier",
                   kEvidenceI1PairDenominatorSchema),
       known_field("portable_claim_schema", "identifier",
                   kEvidencePairNoVerdictClaimSchema),
       known_field("portable_claim_scope", "enum", "denominator-only")},
      5U);
  input.seal_ordinal = 6U;
  EvidenceCanonicalRow outer_row = materialize_evidence_row(std::move(input));

  EvidenceBundleInput bundle_input;
  bundle_input.workload_id = kI1WorkloadId;
  bundle_input.subject_role = options.subject_role;
  bundle_input.provenance = make_producer_section(
      "bundle-provenance", kEvidenceBundleProvenanceSchema,
      {known_field("producer_schema", "identifier",
                   "execution-profile-i1-manual-runner-v1"),
       known_field("environment_authority", "enum", "storage-not-applicable")},
      7U);
  bundle_input.comparison_reference_bundle_digest =
      std::move(options.comparison_reference_bundle_digest);
  bundle_input.rows.push_back(outer_row);
  bundle_input.seal_ordinal = 8U;
  EvidenceCanonicalBundle outer_bundle =
      materialize_evidence_bundle(std::move(bundle_input));
  return EvidencePairObject{std::move(outer_row), std::move(outer_bundle)};
}

/** @copydoc make_b1_evidence_pair_object */
EvidencePairObject make_b1_evidence_pair_object(
    const B1InnerRow& row, EvidencePairProducerOptions options) {
  if ((options.subject_role == EvidenceSubjectRole::Candidate) !=
      options.comparison_reference_bundle_digest.has_value()) {
    throw std::invalid_argument(
        "B1 pair producer comparison direction contradicts its role.");
  }
  if (row.schema != kB1InnerRowSchema ||
      row.schema_version != kB1InnerRowSchemaVersion ||
      row.workload_id != kB1WorkloadId ||
      row.evidence.replicate_ordinal == 0U ||
      row.evidence.replicate_ordinal > 3U ||
      (row.evidence.run_cap != 1U && row.evidence.run_cap != 8U) ||
      row.evidence.environment.workload_id != kB1WorkloadId ||
      row.evidence.environment.replicate_ordinal !=
          row.evidence.replicate_ordinal ||
      row.evidence.environment.run_cap != row.evidence.run_cap ||
      !valid_b1_environment_claims(row.evidence.environment)) {
    throw std::invalid_argument(
        "B1 pair producer row identity or environment claims are invalid.");
  }
  const EvidenceB1ComponentDigests components = evidence_b1_component_digests();
  if (row.evidence.environment.fixture_digest != components.fixture) {
    throw std::invalid_argument(
        "B1 pair producer environment fixture identity drifted.");
  }
  const auto to_ns = [](std::chrono::steady_clock::time_point point) {
    const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           point.time_since_epoch())
                           .count();
    if (value < 0) {
      throw std::invalid_argument(
          "B1 pair producer monotonic timestamp is negative.");
    }
    return static_cast<std::uint64_t>(value);
  };
  const std::uint64_t measurement_start = to_ns(row.evidence.measurement_start);
  const std::uint64_t measurement_end = to_ns(row.evidence.measurement_end);
  if (measurement_end <= measurement_start) {
    throw std::invalid_argument(
        "B1 pair producer measurement interval is not positive.");
  }

  std::vector<const B1JobEvidence*> measured;
  std::vector<B1JobInstance> job_instances;
  job_instances.reserve(row.evidence.jobs.size());
  std::set<B1JobInstance> unique_jobs;
  std::size_t cold_count = 0U;
  std::size_t warmup_count = 0U;
  for (const B1JobEvidence& job : row.evidence.jobs) {
    validate_b1_job_instance(job.job);
    if (job.job.row_workload_id != kB1WorkloadId ||
        job.job.replicate_ordinal != row.evidence.replicate_ordinal ||
        job.job.run_cap != row.evidence.run_cap ||
        !unique_jobs.insert(job.job).second) {
      throw std::invalid_argument(
          "B1 pair producer job identity is duplicated or mismatched.");
    }
    job_instances.push_back(job.job);
    switch (job.job.phase) {
      case B1JobPhase::Cold:
        ++cold_count;
        break;
      case B1JobPhase::Warmup:
        ++warmup_count;
        break;
      case B1JobPhase::Measured:
        measured.push_back(&job);
        break;
    }
  }
  if (row.evidence.jobs.size() !=
          1U + kB1WarmupJobCount + kB1MeasuredJobCount ||
      unique_jobs.size() != row.evidence.jobs.size() || cold_count != 1U ||
      warmup_count != kB1WarmupJobCount) {
    throw std::invalid_argument(
        "B1 pair producer requires exactly one cold, three warmup, and "
        "thirty measured jobs.");
  }
  std::sort(measured.begin(), measured.end(),
            [](const B1JobEvidence* lhs, const B1JobEvidence* rhs) {
              return lhs->job.job_index < rhs->job.job_index;
            });
  if (measured.size() != kB1MeasuredJobCount) {
    throw std::invalid_argument(
        "B1 pair producer requires exactly 30 measured jobs.");
  }
  std::vector<std::string> outcomes;
  std::uint64_t successful = 0U;
  for (std::size_t index = 0U; index < measured.size(); ++index) {
    if (measured[index]->job.job_index != index) {
      throw std::invalid_argument(
          "B1 pair producer measured job order or identity drifted.");
    }
    const bool verified = b1_job_has_verified_endpoint(*measured[index]);
    const std::uint64_t site_operations =
        verified ? kB1SiteOperationsPerJob : 0U;
    successful += site_operations;
    outcomes.push_back(encode_b1_fixed_record(
        {std::to_string(index), verified ? "true" : "false",
         std::to_string(site_operations)}));
  }
  if (successful != row.successful_site_operations) {
    throw std::invalid_argument(
        "B1 pair producer outcomes disagree with the evaluated numerator.");
  }

  EvidenceRowInput input;
  input.workload_id = kB1WorkloadId;
  input.subject_role = options.subject_role;
  input.replicate_ordinal = row.evidence.replicate_ordinal;
  input.run_cap = row.evidence.run_cap;
  input.environment = row.evidence.environment;
  input.workload_manifest = make_producer_section(
      "workload-manifest", "execution-profile-workload-manifest-v1",
      {known_field("fixture_digest", "sha256",
                   b1_digest_hex(components.fixture)),
       known_field("b1_fixture_digest", "sha256",
                   b1_digest_hex(components.fixture)),
       known_field("b1_corpus_digest", "sha256",
                   b1_digest_hex(components.corpus)),
       known_field("b1_golden_digest", "sha256",
                   b1_digest_hex(components.golden)),
       known_field("row_identity_digest", "sha256",
                   b1_digest_hex(b1_sha256(
                       std::string(kB1WorkloadId) + ":" +
                       evidence_subject_role_name(options.subject_role) + ":" +
                       std::to_string(input.replicate_ordinal) + ":" +
                       std::to_string(input.run_cap))))},
      1U);
  input.job_instances = std::move(job_instances);
  input.job_index_seal_ordinal = 2U;
  input.measurement_evidence = make_producer_section(
      "measurement-evidence", "execution-profile-measurement-evidence-v1",
      {known_field("pair_source_schema", "identifier",
                   kEvidenceB1PairDenominatorSchema),
       known_field("subject_role", "enum",
                   evidence_subject_role_name(options.subject_role)),
       known_field("replicate_ordinal", "uint64",
                   std::to_string(input.replicate_ordinal)),
       known_field("source_inner_schema_version", "uint64",
                   std::to_string(kB1InnerRowSchemaVersion)),
       known_field("measurement_start_ns", "uint64",
                   std::to_string(measurement_start)),
       known_field("measurement_end_ns", "uint64",
                   std::to_string(measurement_end)),
       known_field("measured_job_outcomes", "b1-measured-job-outcome-list-v1",
                   encode_record_list(outcomes)),
       known_field("successful_site_operations", "uint64",
                   std::to_string(successful))},
      3U);
  input.output_evidence = make_producer_section(
      "output-evidence", "execution-profile-output-evidence-v1",
      {known_field("pair_source_schema", "identifier",
                   kEvidenceB1PairDenominatorSchema),
       known_field("portable_output_claim_schema", "identifier",
                   kEvidencePairNoOutputClaimSchema),
       known_field("portable_output_authority", "enum", "not-claimed")},
      4U);
  input.verdict_evidence = make_producer_section(
      "verdict-evidence", "execution-profile-verdict-evidence-v1",
      {known_field("pair_source_schema", "identifier",
                   kEvidenceB1PairDenominatorSchema),
       known_field("portable_claim_schema", "identifier",
                   kEvidencePairNoVerdictClaimSchema),
       known_field("portable_claim_scope", "enum", "denominator-only")},
      5U);
  input.seal_ordinal = 6U;
  EvidenceCanonicalRow outer_row = materialize_evidence_row(std::move(input));

  EvidenceBundleInput bundle_input;
  bundle_input.workload_id = kB1WorkloadId;
  bundle_input.subject_role = options.subject_role;
  bundle_input.provenance = make_producer_section(
      "bundle-provenance", kEvidenceBundleProvenanceSchema,
      {known_field("producer_schema", "identifier",
                   "execution-profile-b1-manual-runner-v1"),
       known_field("environment_authority", "enum",
                   valid_b1_environment_evidence(outer_row.source.environment)
                       ? "complete-live-authority"
                       : "portable-incomplete-live-authority")},
      7U);
  bundle_input.comparison_reference_bundle_digest =
      std::move(options.comparison_reference_bundle_digest);
  bundle_input.rows.push_back(outer_row);
  bundle_input.seal_ordinal = 8U;
  EvidenceCanonicalBundle outer_bundle =
      materialize_evidence_bundle(std::move(bundle_input));
  return EvidencePairObject{std::move(outer_row), std::move(outer_bundle)};
}

/** @copydoc materialize_evidence_pair_object */
std::string materialize_evidence_pair_object(const EvidencePairObject& object) {
  const EvidenceCanonicalRow rebuilt_row =
      materialize_evidence_row(object.row.source);
  const EvidenceCanonicalBundle rebuilt_bundle =
      materialize_evidence_bundle(object.bundle.source);
  if (rebuilt_row.manifest_bytes != object.row.manifest_bytes ||
      rebuilt_row.digest != object.row.digest ||
      rebuilt_bundle.manifest_bytes != object.bundle.manifest_bytes ||
      rebuilt_bundle.digest != object.bundle.digest ||
      rebuilt_bundle.row_references != object.bundle.row_references ||
      object.bundle.source.rows.size() != 1U ||
      object.bundle.source.rows.front().digest != object.row.digest ||
      !bundle_names_row(object.bundle, object.row)) {
    throw std::invalid_argument(
        "Evidence pair object is not one reproducible row/bundle source.");
  }
  if (!valid_b1_environment_claims(object.row.source.environment)) {
    throw std::invalid_argument(
        "Evidence pair object environment claims are invalid.");
  }
  const std::array<const EvidenceRetainedSection*, 6U> sections{
      &object.row.source.workload_manifest,
      &object.row.job_instance_index,
      &object.row.source.measurement_evidence,
      &object.row.source.output_evidence,
      &object.row.source.verdict_evidence,
      &object.bundle.source.provenance,
  };
  std::vector<std::string> section_records;
  section_records.reserve(sections.size());
  std::set<std::string> section_digests;
  for (std::size_t index = 0U; index < sections.size(); ++index) {
    if (sections[index]->section_name != kPairSectionContracts[index].first ||
        sections[index]->schema_id != kPairSectionContracts[index].second) {
      throw std::invalid_argument(
          "Evidence pair object retained-section contract drifted.");
    }
    const std::string digest = digest_evidence_section(
        sections[index]->section_name, sections[index]->schema_id,
        sections[index]->bytes);
    if (!section_digests.insert(digest).second) {
      throw std::invalid_argument(
          "Evidence pair object contains a duplicate section address.");
    }
    section_records.push_back(encode_pair_section_record(*sections[index]));
  }

  const B1EnvironmentEvidence& environment = object.row.source.environment;
  std::vector<B1CanonicalField> fields{
      known_field("row_manifest", "canonical-text-hex-v1",
                  encode_b1_normalized_text(object.row.manifest_bytes)),
      known_field("bundle_manifest", "canonical-text-hex-v1",
                  encode_b1_normalized_text(object.bundle.manifest_bytes)),
      known_field("base_manifest", "canonical-text-hex-v1",
                  encode_b1_normalized_text(environment.base_manifest)),
      known_field("claimed_base_digest", "sha256",
                  b1_digest_hex(environment.claimed_base_digest)),
  };
  if (environment.storage_manifest.has_value()) {
    if (!environment.claimed_storage_digest.has_value() ||
        !environment.storage_raw_proof.has_value() ||
        !environment.storage_eligibility.has_value()) {
      throw std::invalid_argument(
          "Evidence pair required-storage source is incomplete.");
    }
    fields.push_back(
        known_field("storage_manifest", "canonical-text-hex-v1",
                    encode_b1_normalized_text(*environment.storage_manifest)));
    fields.push_back(
        known_field("claimed_storage_digest", "sha256",
                    b1_digest_hex(*environment.claimed_storage_digest)));
  } else {
    fields.push_back(not_applicable_field("storage_manifest",
                                          "canonical-text-hex-v1",
                                          "row-has-no-output-commit"));
    fields.push_back(not_applicable_field("claimed_storage_digest", "sha256",
                                          "row-has-no-output-commit"));
  }
  fields.push_back(known_field(
      "environment_class_manifest", "canonical-text-hex-v1",
      encode_b1_normalized_text(environment.environment_class_manifest)));
  fields.push_back(
      known_field("claimed_environment_class_digest", "sha256",
                  b1_digest_hex(environment.claimed_environment_class_digest)));
  if (environment.storage_manifest.has_value()) {
    fields.push_back(
        known_field("storage_raw_proof", "canonical-text-hex-v1",
                    encode_b1_normalized_text(
                        environment.storage_raw_proof->canonical_bytes)));
    fields.push_back(known_field(
        "storage_eligibility", "b1-storage-eligibility-v1",
        encode_storage_eligibility(*environment.storage_eligibility)));
  } else {
    fields.push_back(not_applicable_field("storage_raw_proof",
                                          "canonical-text-hex-v1",
                                          "row-has-no-output-commit"));
    fields.push_back(not_applicable_field("storage_eligibility",
                                          "b1-storage-eligibility-v1",
                                          "row-has-no-output-commit"));
  }
  fields.push_back(known_field("fixture_digest", "sha256",
                               b1_digest_hex(environment.fixture_digest)));
  fields.push_back(known_field("resource_identity", "sha256",
                               b1_digest_hex(environment.resource_identity)));
  fields.push_back(known_field("retained_sections",
                               "evidence-retained-section-list-v1",
                               encode_record_list(section_records)));
  fields.push_back(known_field("row_seal_ordinal", "uint64",
                               std::to_string(object.row.source.seal_ordinal)));
  fields.push_back(
      known_field("bundle_seal_ordinal", "uint64",
                  std::to_string(object.bundle.source.seal_ordinal)));
  const std::string bytes =
      encode_b1_canonical_manifest(kEvidencePairObjectSchema, fields);
  if (bytes.size() > kEvidencePairObjectMaxBytes) {
    throw std::invalid_argument(
        "Evidence pair object exceeds the bounded pack size.");
  }
  return bytes;
}

/** @copydoc load_evidence_pair_object */
EvidencePairObject load_evidence_pair_object(
    std::string_view bytes, std::string_view expected_row_digest,
    std::string_view expected_bundle_digest) {
  if (bytes.empty() || bytes.size() > kEvidencePairObjectMaxBytes ||
      !valid_digest(expected_row_digest) ||
      !valid_digest(expected_bundle_digest)) {
    throw std::invalid_argument(
        "Evidence pair pack size or expected address is invalid.");
  }
  const B1CanonicalManifest manifest = parse_b1_canonical_manifest(bytes);
  if (manifest.schema != kEvidencePairObjectSchema ||
      manifest.fields.size() != kPairObjectFieldNames.size()) {
    throw std::invalid_argument(
        "Evidence pair pack schema or field cardinality drifted.");
  }
  for (std::size_t index = 0U; index < manifest.fields.size(); ++index) {
    if (manifest.fields[index].name != kPairObjectFieldNames[index] ||
        manifest.fields[index].type != kPairObjectFieldTypes[index]) {
      throw std::invalid_argument(
          "Evidence pair pack field name/type order drifted.");
    }
  }
  for (const std::size_t index :
       {0U, 1U, 2U, 3U, 6U, 7U, 10U, 11U, 12U, 13U, 14U}) {
    require_known_field(manifest.fields[index], kPairObjectFieldNames[index],
                        kPairObjectFieldTypes[index]);
  }
  const bool has_storage =
      manifest.fields[4U].state == B1ObservationState::Known;
  for (const std::size_t index : {4U, 5U, 8U, 9U}) {
    const B1CanonicalField& field = manifest.fields[index];
    if (has_storage) {
      require_known_field(field, kPairObjectFieldNames[index],
                          kPairObjectFieldTypes[index]);
    } else if (field.state != B1ObservationState::NotApplicable ||
               field.reason != "row-has-no-output-commit" ||
               !field.payload.empty()) {
      throw std::invalid_argument(
          "Evidence pair storage applicability fields disagree.");
    }
  }
  const std::string row_bytes =
      decode_canonical_text_hex(manifest.fields[0U].payload);
  const std::string bundle_bytes =
      decode_canonical_text_hex(manifest.fields[1U].payload);
  const EvidenceParsedRow parsed_row = parse_evidence_row(row_bytes);
  const EvidenceParsedBundle parsed_bundle =
      parse_evidence_bundle(bundle_bytes);
  if (digest_evidence_row(row_bytes) != expected_row_digest ||
      digest_evidence_bundle(bundle_bytes) != expected_bundle_digest ||
      parsed_bundle.workload_id != parsed_row.workload_id ||
      parsed_bundle.subject_role != parsed_row.subject_role ||
      parsed_bundle.row_references.size() != 1U ||
      parsed_bundle.row_references.front().row_digest != expected_row_digest ||
      parsed_bundle.row_references.front().workload_id !=
          parsed_row.workload_id ||
      parsed_bundle.row_references.front().run_cap != parsed_row.run_cap ||
      parsed_bundle.row_references.front().replicate_ordinal !=
          parsed_row.replicate_ordinal) {
    throw std::invalid_argument(
        "Evidence pair row/bundle address or functional key mismatched.");
  }

  const std::vector<std::string> section_records =
      parse_b1_framed_list(manifest.fields[12U].payload);
  if (section_records.size() != kPairSectionContracts.size()) {
    throw std::invalid_argument(
        "Evidence pair pack does not contain exactly six sections.");
  }
  std::array<EvidenceRetainedSection, 6U> sections;
  std::set<std::string> section_digests;
  for (std::size_t index = 0U; index < section_records.size(); ++index) {
    sections[index] = parse_pair_section_record(section_records[index]);
    if (sections[index].section_name != kPairSectionContracts[index].first ||
        sections[index].schema_id != kPairSectionContracts[index].second) {
      throw std::invalid_argument(
          "Evidence pair retained section is missing, reordered, or unknown.");
    }
    const std::string digest = digest_evidence_section(
        sections[index].section_name, sections[index].schema_id,
        sections[index].bytes);
    if (!section_digests.insert(digest).second) {
      throw std::invalid_argument(
          "Evidence pair retained section address is ambiguous.");
    }
  }

  B1EnvironmentEvidence environment;
  environment.base_manifest =
      decode_canonical_text_hex(manifest.fields[2U].payload);
  environment.claimed_base_digest =
      parse_b1_digest(manifest.fields[3U].payload);
  if (has_storage) {
    environment.storage_manifest =
        decode_canonical_text_hex(manifest.fields[4U].payload);
    environment.claimed_storage_digest =
        parse_b1_digest(manifest.fields[5U].payload);
  }
  environment.environment_class_manifest =
      decode_canonical_text_hex(manifest.fields[6U].payload);
  environment.claimed_environment_class_digest =
      parse_b1_digest(manifest.fields[7U].payload);
  if (has_storage) {
    environment.storage_raw_proof = B1StorageRawProof{
        decode_canonical_text_hex(manifest.fields[8U].payload)};
    environment.storage_eligibility =
        parse_storage_eligibility(manifest.fields[9U].payload);
  }
  environment.workload_id = parsed_row.workload_id;
  environment.fixture_digest = parse_b1_digest(manifest.fields[10U].payload);
  environment.resource_identity = parse_b1_digest(manifest.fields[11U].payload);
  environment.run_cap = parsed_row.run_cap;
  environment.replicate_ordinal = parsed_row.replicate_ordinal;
  environment.storage_actual_observation.reset();
  if (!valid_b1_environment_claims(environment)) {
    throw std::invalid_argument(
        "Evidence pair retained environment claims do not self-validate.");
  }

  EvidenceRowInput row_input;
  row_input.workload_id = parsed_row.workload_id;
  row_input.subject_role = parsed_row.subject_role;
  row_input.replicate_ordinal = parsed_row.replicate_ordinal;
  row_input.run_cap = parsed_row.run_cap;
  row_input.environment = std::move(environment);
  row_input.workload_manifest = sections[0U];
  row_input.job_instances = parse_job_index(sections[1U]);
  row_input.job_index_seal_ordinal = sections[1U].seal_ordinal;
  row_input.measurement_evidence = sections[2U];
  row_input.output_evidence = sections[3U];
  row_input.verdict_evidence = sections[4U];
  row_input.paired_isolated_i1 = parsed_row.paired_isolated_i1;
  row_input.paired_isolated_b1_cap8 = parsed_row.paired_isolated_b1_cap8;
  row_input.seal_ordinal =
      parse_b1_canonical_uint64(manifest.fields[13U].payload);
  EvidenceCanonicalRow row = materialize_evidence_row(std::move(row_input));
  if (row.manifest_bytes != row_bytes || row.digest != expected_row_digest) {
    throw std::invalid_argument(
        "Evidence pair row source does not reproduce its addressed bytes.");
  }

  EvidenceBundleInput bundle_input;
  bundle_input.workload_id = parsed_bundle.workload_id;
  bundle_input.subject_role = parsed_bundle.subject_role;
  bundle_input.provenance = sections[5U];
  bundle_input.comparison_reference_bundle_digest =
      parsed_bundle.comparison_reference_bundle_digest;
  bundle_input.rows.push_back(row);
  bundle_input.seal_ordinal =
      parse_b1_canonical_uint64(manifest.fields[14U].payload);
  EvidenceCanonicalBundle bundle =
      materialize_evidence_bundle(std::move(bundle_input));
  if (bundle.manifest_bytes != bundle_bytes ||
      bundle.digest != expected_bundle_digest) {
    throw std::invalid_argument(
        "Evidence pair bundle source does not reproduce its addressed bytes.");
  }

  EvidenceCorpus closure;
  closure.sections.assign(sections.begin(), sections.end());
  closure.rows.push_back(row);
  closure.bundles.push_back(bundle);
  for (const EvidenceRetainedSection& section : closure.sections) {
    for (const EvidenceAddressReference& dependency :
         section.address_dependencies) {
      std::size_t matches = 0U;
      std::uint64_t target_seal = 0U;
      if (dependency.kind == EvidenceAddressKind::Section) {
        for (const EvidenceRetainedSection& target : closure.sections) {
          if (digest_evidence_section(target.section_name, target.schema_id,
                                      target.bytes) == dependency.digest) {
            ++matches;
            target_seal = target.seal_ordinal;
          }
        }
      } else if (dependency.kind == EvidenceAddressKind::Row) {
        if (row.digest == dependency.digest) {
          matches = 1U;
          target_seal = row.source.seal_ordinal;
        }
      } else if (dependency.kind == EvidenceAddressKind::Bundle) {
        if (bundle.digest == dependency.digest) {
          matches = 1U;
          target_seal = bundle.source.seal_ordinal;
        }
      }
      if (matches != 1U || target_seal >= section.seal_ordinal) {
        throw std::invalid_argument(
            "Evidence pair dependency is unresolved, ambiguous, or later.");
      }
    }
  }
  EvidencePairObject object{std::move(row), std::move(bundle)};
  if (materialize_evidence_pair_object(object) != bytes) {
    throw std::invalid_argument(
        "Evidence pair pack bytes do not round-trip canonically.");
  }
  return object;
}

/** @copydoc read_evidence_pair_object_file */
std::string read_evidence_pair_object_file(const std::filesystem::path& path) {
  if (path.empty() || !path.is_absolute()) {
    throw std::invalid_argument("Evidence pair object path must be absolute.");
  }
#if !defined(_WIN32)
  const int descriptor =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (errno == ELOOP) {
      throw std::invalid_argument(
          "Evidence pair object path must not be a symbolic link.");
    }
    throw std::runtime_error("Failed to open evidence pair object: " +
                             std::string(std::strerror(errno)));
  }
  const auto close_descriptor = [descriptor]() noexcept {
    int result = 0;
    do {
      result = ::close(descriptor);
    } while (result < 0 && errno == EINTR);
    return result;
  };
  struct stat status{};
  if (::fstat(descriptor, &status) != 0) {
    const std::string diagnostic = std::strerror(errno);
    static_cast<void>(close_descriptor());
    throw std::runtime_error("Failed to stat evidence pair object: " +
                             diagnostic);
  }
  if (!S_ISREG(status.st_mode) || status.st_size <= 0 ||
      static_cast<std::uintmax_t>(status.st_size) >
          kEvidencePairObjectMaxBytes) {
    static_cast<void>(close_descriptor());
    throw std::invalid_argument(
        "Evidence pair object must be a nonempty bounded regular file.");
  }
  std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
  std::size_t consumed = 0U;
  while (consumed < bytes.size()) {
    const ssize_t count =
        ::read(descriptor, bytes.data() + consumed, bytes.size() - consumed);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      const std::string diagnostic =
          count == 0 ? "unexpected end of file" : std::strerror(errno);
      static_cast<void>(close_descriptor());
      throw std::runtime_error("Failed to read evidence pair object: " +
                               diagnostic);
    }
    consumed += static_cast<std::size_t>(count);
  }
  char extra = '\0';
  ssize_t extra_count = 0;
  do {
    extra_count = ::read(descriptor, &extra, 1U);
  } while (extra_count < 0 && errno == EINTR);
  if (extra_count != 0) {
    const std::string diagnostic =
        extra_count < 0 ? std::strerror(errno) : "file grew while reading";
    static_cast<void>(close_descriptor());
    throw std::runtime_error("Evidence pair object changed during read: " +
                             diagnostic);
  }
  if (close_descriptor() != 0) {
    throw std::runtime_error("Failed to close evidence pair object: " +
                             std::string(std::strerror(errno)));
  }
  return bytes;
#else
  const DWORD open_flags = FILE_FLAG_OPEN_REPARSE_POINT |
                           FILE_FLAG_BACKUP_SEMANTICS |
                           FILE_FLAG_SEQUENTIAL_SCAN;
  const HANDLE opened =
      ::CreateFileW(path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, open_flags, nullptr);
  if (opened == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    throw std::runtime_error(
        "Failed to open evidence pair object with no-follow semantics: " +
        std::to_string(error));
  }
  WindowsEvidenceReadHandle handle(opened);

  ::SetLastError(ERROR_SUCCESS);
  const DWORD file_type = ::GetFileType(handle.get());
  const DWORD file_type_error = ::GetLastError();
  if (file_type == FILE_TYPE_UNKNOWN && file_type_error != ERROR_SUCCESS) {
    throw std::runtime_error(
        "Failed to classify evidence pair object handle: " +
        std::to_string(file_type_error));
  }
  if (file_type != FILE_TYPE_DISK) {
    throw std::invalid_argument(
        "Evidence pair object handle must name a disk file.");
  }

  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (::GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo,
                                     &attributes, sizeof(attributes)) == 0) {
    const DWORD error = ::GetLastError();
    throw std::runtime_error(
        "Failed to inspect evidence pair object reparse attributes: " +
        std::to_string(error));
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    throw std::invalid_argument(
        "Evidence pair object path must not name a reparse point.");
  }

  FILE_STANDARD_INFO standard{};
  if (::GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &standard,
                                     sizeof(standard)) == 0) {
    const DWORD error = ::GetLastError();
    throw std::runtime_error("Failed to inspect evidence pair object size: " +
                             std::to_string(error));
  }
  if (standard.Directory != 0 || standard.EndOfFile.QuadPart <= 0 ||
      static_cast<std::uint64_t>(standard.EndOfFile.QuadPart) >
          kEvidencePairObjectMaxBytes) {
    throw std::invalid_argument(
        "Evidence pair object must be a nonempty bounded regular file.");
  }

  const std::size_t size =
      static_cast<std::size_t>(standard.EndOfFile.QuadPart);
  std::string bytes(size, '\0');
  std::size_t consumed = 0U;
  while (consumed < bytes.size()) {
    const std::size_t remaining = bytes.size() - consumed;
    const DWORD requested = static_cast<DWORD>(
        std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
    DWORD read = 0U;
    if (::ReadFile(handle.get(), bytes.data() + consumed, requested, &read,
                   nullptr) == 0) {
      const DWORD error = ::GetLastError();
      throw std::runtime_error("Failed to read evidence pair object: " +
                               std::to_string(error));
    }
    if (read == 0U) {
      throw std::runtime_error(
          "Failed to read evidence pair object: unexpected end of file");
    }
    consumed += read;
  }

  char extra = '\0';
  DWORD extra_read = 0U;
  if (::ReadFile(handle.get(), &extra, 1U, &extra_read, nullptr) == 0) {
    const DWORD error = ::GetLastError();
    throw std::runtime_error(
        "Failed to verify evidence pair object end of file: " +
        std::to_string(error));
  }
  if (extra_read != 0U) {
    throw std::runtime_error(
        "Evidence pair object grew while reading from its opened handle.");
  }
  DWORD close_error = ERROR_SUCCESS;
  if (!handle.close(&close_error)) {
    throw std::runtime_error("Failed to close evidence pair object: " +
                             std::to_string(close_error));
  }
  return bytes;
#endif
}

/** @copydoc append_evidence_pair_object */
void append_evidence_pair_object(const EvidencePairObject& object,
                                 EvidenceCorpus* corpus) {
  if (corpus == nullptr) {
    throw std::invalid_argument("Evidence pair corpus destination is null.");
  }
  static_cast<void>(materialize_evidence_pair_object(object));
  const std::array<const EvidenceRetainedSection*, 6U> sections{
      &object.row.source.workload_manifest,
      &object.row.job_instance_index,
      &object.row.source.measurement_evidence,
      &object.row.source.output_evidence,
      &object.row.source.verdict_evidence,
      &object.bundle.source.provenance,
  };
  for (const EvidenceRetainedSection* section : sections) {
    const std::string digest = digest_evidence_section(
        section->section_name, section->schema_id, section->bytes);
    const std::size_t matches =
        std::count_if(corpus->sections.begin(), corpus->sections.end(),
                      [&digest](const EvidenceRetainedSection& retained) {
                        return digest_evidence_section(
                                   retained.section_name, retained.schema_id,
                                   retained.bytes) == digest;
                      });
    if (matches != 0U) {
      throw std::invalid_argument(
          "Evidence pair section address already exists in corpus.");
    }
  }
  if (std::any_of(corpus->rows.begin(), corpus->rows.end(),
                  [&object](const EvidenceCanonicalRow& row) {
                    return row.digest == object.row.digest;
                  }) ||
      std::any_of(corpus->bundles.begin(), corpus->bundles.end(),
                  [&object](const EvidenceCanonicalBundle& bundle) {
                    return bundle.digest == object.bundle.digest;
                  })) {
    throw std::invalid_argument(
        "Evidence pair row or bundle address already exists in corpus.");
  }
  for (const EvidenceRetainedSection* section : sections) {
    corpus->sections.push_back(*section);
  }
  corpus->rows.push_back(object.row);
  corpus->bundles.push_back(object.bundle);
}

/** @copydoc validate_evidence_m1_pair_objects */
EvidenceM1PairDenominators validate_evidence_m1_pair_objects(
    const EvidencePairObject& isolated_i1,
    const EvidencePairObject& isolated_b1_cap8,
    EvidenceSubjectRole subject_role, std::uint64_t replicate_ordinal,
    const B1EnvironmentEvidence& m1_environment,
    const B1Sha256Digest& i1_fixture,
    const EvidenceB1ComponentDigests& b1_components) {
  static_cast<void>(materialize_evidence_pair_object(isolated_i1));
  static_cast<void>(materialize_evidence_pair_object(isolated_b1_cap8));
  if (replicate_ordinal == 0U || replicate_ordinal > 3U ||
      m1_environment.workload_id != kM1WorkloadId ||
      m1_environment.run_cap != 8U ||
      m1_environment.replicate_ordinal != replicate_ordinal ||
      !valid_b1_environment_claims(m1_environment)) {
    throw std::invalid_argument(
        "M1 pair binding context is malformed or incomplete.");
  }
  const auto validate_identity = [&](const EvidencePairObject& object,
                                     std::string_view workload) {
    if (object.row.source.workload_id != workload ||
        object.bundle.source.workload_id != workload ||
        object.row.source.subject_role != subject_role ||
        object.bundle.source.subject_role != subject_role ||
        object.row.source.replicate_ordinal != replicate_ordinal ||
        object.row.source.run_cap != 8U ||
        object.bundle.source.rows.size() != 1U ||
        !bundle_names_row(object.bundle, object.row)) {
      throw std::invalid_argument(
          "M1 loaded pair role, workload, cap, ordinal, or membership "
          "drifted.");
    }
  };
  validate_identity(isolated_i1, kI1WorkloadId);
  validate_identity(isolated_b1_cap8, kB1WorkloadId);
  if (!compatible_b1_environment_claims(
          m1_environment, isolated_i1.row.source.environment,
          B1EnvironmentRelation::M1PairedI1BaseOnly)) {
    throw std::invalid_argument(
        "M1 loaded isolated-I1 base/resource claims are incompatible.");
  }
  if (!compatible_b1_environment_claims(
          m1_environment, isolated_b1_cap8.row.source.environment,
          B1EnvironmentRelation::M1PairedB1CapEight)) {
    throw std::invalid_argument(
        "M1 loaded isolated-B1 full environment claims are incompatible.");
  }
  if (workload_fixture_digest(isolated_i1.row, "i1_fixture_digest") !=
          b1_digest_hex(i1_fixture) ||
      workload_fixture_digest(isolated_b1_cap8.row, "b1_fixture_digest") !=
          b1_digest_hex(b1_components.fixture) ||
      workload_named_digest(isolated_b1_cap8.row, "b1_corpus_digest") !=
          b1_digest_hex(b1_components.corpus) ||
      workload_named_digest(isolated_b1_cap8.row, "b1_golden_digest") !=
          b1_digest_hex(b1_components.golden)) {
    throw std::invalid_argument(
        "M1 loaded pair component fixture/corpus/golden identity drifted.");
  }
  const std::uint64_t i1_p99 = recompute_isolated_i1_p99(isolated_i1.row);
  const B1RateSource b1_source =
      recompute_isolated_b1_rate_source(isolated_b1_cap8.row);
  constexpr std::uint64_t kMaximumNanoseconds = static_cast<std::uint64_t>(
      std::numeric_limits<std::chrono::nanoseconds::rep>::max());
  if (b1_source.successful_site_operations == 0U ||
      i1_p99 > kMaximumNanoseconds ||
      b1_source.duration_ns > kMaximumNanoseconds) {
    throw std::invalid_argument(
        "M1 loaded pair denominator is zero or exceeds the evaluator "
        "representation.");
  }
  return EvidenceM1PairDenominators{
      i1_p99, b1_source.successful_site_operations, b1_source.duration_ns};
}

}  // namespace ps::benchmark
