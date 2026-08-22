/**
 * @file b1_profile.hpp
 * @brief Declares the frozen B1 workload, identity, digest, and trace model.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "compute/compute_run.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/extension.hpp"
#include "photospider/data/value.hpp"
#include "photospider/host/host.hpp"

namespace ps::benchmark {

/** @brief Exact closed workload token for every B1 occurrence. */
inline constexpr char kB1WorkloadId[] = "B1-immutable-v1";

/** @brief Exact square edge of every B1 RGBA image. */
inline constexpr std::uint64_t kB1ImageEdge = 2048U;

/** @brief Exact channel count of every B1 output. */
inline constexpr std::uint64_t kB1ChannelCount = 4U;

/** @brief Exact active bytes in one tightly packed B1 output row. */
inline constexpr std::uint64_t kB1PayloadRowBytes = 32768U;

/** @brief Exact raw payload length for every valid B1 job. */
inline constexpr std::uint64_t kB1PayloadBytes = 67108864U;

/** @brief Exact process Compute I/O task-count limit used by B1. */
inline constexpr std::uint64_t kB1ComputeIoTaskLimit = 64U;

/** @brief Exact process Compute I/O planned-byte limit used by B1. */
inline constexpr std::uint64_t kB1ComputeIoPlannedByteLimit = 268435456U;

/** @brief Exact number of measured B1 jobs in one isolated row. */
inline constexpr std::size_t kB1MeasuredJobCount = 30U;

/** @brief Exact number of warmup-only B1 jobs in one isolated row. */
inline constexpr std::size_t kB1WarmupJobCount = 3U;

/** @brief Exact number of fresh-process replicates required by B1. */
inline constexpr std::uint64_t kB1ReplicateCount = 3U;

/** @brief Exact cold fixture seed executed before B1 warmup. */
inline constexpr std::uint64_t kB1ColdJobIndex = 252U;

/** @brief Exact ordered warmup fixture seeds. */
// NOLINTBEGIN(whitespace/indent_namespace)
inline constexpr std::array<std::uint64_t, kB1WarmupJobCount>
    kB1WarmupJobIndices{253U, 254U, 255U};
// NOLINTEND

/** @brief Exact required isolated B1 Run caps. */
inline constexpr std::array<std::uint64_t, 2U> kB1RunCaps{1U, 8U};

/** @brief Exact deterministic-plan task count for every frozen B1 job. */
inline constexpr std::size_t kB1TasksPerJob = 257U;

/** @brief Exact tile count in each of the four curve stages. */
inline constexpr std::size_t kB1TilesPerCurveStage = 64U;

/** @brief Exact logical ready payload bytes declared by one curve tile. */
inline constexpr std::uint64_t kB1CurveTileBytes = 1048576U;

/**
 * @brief Immutable phase encoded in one `job-instance-v1` occurrence.
 * @throws Nothing for value construction and comparison.
 */
enum class B1JobPhase : std::uint8_t {
  /** @brief Cold first-use occurrence excluded from steady-state aggregates. */
  Cold,
  /** @brief Warmup occurrence excluded from steady-state aggregates. */
  Warmup,
  /** @brief Measured occurrence included in the B1 row. */
  Measured,
};

/**
 * @brief Frozen Graph lane derived exclusively from B1 job parity.
 * @throws Nothing for value construction and comparison.
 */
enum class B1GraphRole : std::uint8_t {
  /** @brief Graph A owns even job indices. */
  A,
  /** @brief Graph B owns odd job indices. */
  B,
};

/**
 * @brief Closed logical Compute I/O stage vocabulary for one B1 artifact.
 * @throws Nothing for value construction and comparison.
 */
enum class B1IoStage : std::uint8_t {
  /** @brief Writes, hashes, synchronizes, and revalidates the payload. */
  PayloadStage,
  /** @brief Writes and durably publishes the manifest last. */
  ManifestCommit,
};

/**
 * @brief Complete immutable six-component `job-instance-v1` occurrence.
 *
 * @throws Nothing for default construction; validation is explicit.
 * @note `cycle_ordinal` identifies a producer-local M1 cycle and never retry.
 */
struct B1JobInstance final {
  /** @brief Closed row workload token; B1 uses `B1-immutable-v1`. */
  std::string row_workload_id{kB1WorkloadId};
  /** @brief Fresh-process replicate ordinal in `[1,3]`. */
  std::uint64_t replicate_ordinal = 0U;
  /** @brief Immutable cold, warmup, or measured phase. */
  B1JobPhase phase = B1JobPhase::Cold;
  /** @brief Phase-local or producer-local cycle ordinal. */
  std::uint64_t cycle_ordinal = 0U;
  /** @brief Fixture/golden selector in `[0,255]`. */
  std::uint64_t job_index = 0U;
  /** @brief Exact Run cap, currently one or eight. */
  std::uint64_t run_cap = 0U;

  /**
   * @brief Compares every identity component for exact equality.
   * @param other Candidate occurrence.
   * @return True only when all six components match.
   * @throws Nothing.
   */
  bool operator==(const B1JobInstance& other) const noexcept;

  /**
   * @brief Compares occurrence identities lexicographically.
   * @param other Candidate occurrence.
   * @return Strict order by workload, replicate, phase rank, cycle, job, cap.
   * @throws Nothing.
   * @note This order is for deterministic evidence storage, not scheduling.
   */
  bool operator<(const B1JobInstance& other) const noexcept;
};

/**
 * @brief Full task-attempt identity layered on one B1 occurrence.
 * @throws Nothing for default construction; validation is explicit.
 * @note Capacity rejection and idempotent duplicate retain attempt zero.
 */
struct B1IoTaskIdentity final {
  /** @brief Complete immutable job occurrence. */
  B1JobInstance job;
  /** @brief Logical payload or manifest stage. */
  B1IoStage stage = B1IoStage::PayloadStage;
  /** @brief Explicit post-terminal-failure retry ordinal. */
  std::uint64_t attempt = 0U;

  /**
   * @brief Compares every job/stage/attempt component.
   * @param other Candidate task identity.
   * @return True only for the same full attempt identity.
   * @throws Nothing.
   */
  bool operator==(const B1IoTaskIdentity& other) const noexcept;
};

/**
 * @brief Fixed 256-bit SHA-256 value used by raw B1 evidence domains.
 * @throws Nothing for value operations.
 * @note Typed logical `ContentDigest` remains a separate type and domain.
 */
struct B1Sha256Digest final {
  /** @brief Exact 32 digest bytes in network/display order. */
  std::array<std::byte, 32U> bytes{};

  /**
   * @brief Compares exact digest bytes.
   * @param other Candidate digest.
   * @return True when all bytes match.
   * @throws Nothing.
   */
  bool operator==(const B1Sha256Digest& other) const noexcept;

  /**
   * @brief Compares exact digest bytes for inequality.
   * @param other Candidate digest.
   * @return True when at least one byte differs.
   * @throws Nothing.
   */
  bool operator!=(const B1Sha256Digest& other) const noexcept {
    return !(*this == other);
  }
};

/**
 * @brief Incremental dependency-neutral SHA-256 calculator for raw B1 bytes.
 *
 * @throws Nothing for construction/update/finalization.
 * @note The instance is single-use after `finish()` and owns no file or Value.
 */
class B1Sha256 final {
 public:
  /** @brief Creates the standard SHA-256 initial state. @throws Nothing. */
  B1Sha256() noexcept;

  /**
   * @brief Appends one byte range to the digest input.
   * @param data Borrowed bytes, null only when `size` is zero.
   * @param size Number of bytes to append.
   * @return Nothing.
   * @throws std::invalid_argument for a null nonempty range or post-finish use.
   * @throws std::overflow_error when SHA-256 bit-length encoding overflows.
   */
  void update(const std::byte* data, std::size_t size);

  /**
   * @brief Appends one string byte range verbatim.
   * @param text Borrowed bytes.
   * @return Nothing.
   * @throws As `update`.
   */
  void update(std::string_view text);

  /**
   * @brief Finalizes and returns the exact SHA-256 value.
   * @return Immutable digest.
   * @throws std::logic_error when called more than once.
   */
  B1Sha256Digest finish();

 private:
  /** @brief Compresses one complete 64-byte block into the state. */
  void compress(const std::byte* block) noexcept;

  /** @brief Eight SHA-256 state words. */
  std::array<std::uint32_t, 8U> state_{};
  /** @brief Incomplete final input block. */
  std::array<std::byte, 64U> buffer_{};
  /** @brief Number of bytes currently stored in `buffer_`. */
  std::size_t buffered_ = 0U;
  /** @brief Complete input byte count before padding. */
  std::uint64_t total_bytes_ = 0U;
  /** @brief Whether `finish()` already consumed this instance. */
  bool finished_ = false;
};

/**
 * @brief Resource vector encoded in one canonical semantic-trace task.
 * @throws Nothing for value construction and comparison.
 */
struct B1SemanticResourceVector final {
  /** @brief Declared logical work units. */
  std::uint64_t work_units = 0U;
  /** @brief Declared ready-entry count. */
  std::uint64_t ready_entries = 0U;
  /** @brief Declared ready bytes. */
  std::uint64_t ready_bytes = 0U;
  /** @brief Declared CPU slots. */
  std::uint64_t cpu_slots = 0U;
  /** @brief Declared Host retained bytes. */
  std::uint64_t host_retained_bytes = 0U;
  /** @brief Declared Host scratch bytes. */
  std::uint64_t host_scratch_bytes = 0U;
  /** @brief Declared device memory bytes. */
  std::uint64_t device_memory_bytes = 0U;
  /** @brief Declared device scratch bytes. */
  std::uint64_t device_scratch_bytes = 0U;

  /**
   * @brief Compares every resource dimension.
   * @param other Candidate vector.
   * @return True only when every dimension matches.
   * @throws Nothing.
   */
  bool operator==(const B1SemanticResourceVector& other) const noexcept;
};

/**
 * @brief One deterministic-plan task before physical execution.
 * @throws std::bad_alloc when dependency storage allocates.
 */
struct B1SemanticTask final {
  /** @brief Fixture job index carried in canonical trace bytes. */
  std::uint64_t job_index = 0U;
  /** @brief Frozen Graph role derived from job parity. */
  B1GraphRole graph = B1GraphRole::A;
  /** @brief Zero-based contiguous plan-relative task ordinal. */
  std::uint64_t task_ordinal = 0U;
  /** @brief Numerically sorted dependency task ordinals. */
  std::vector<std::uint64_t> dependencies;
  /** @brief Complete declared resource vector. */
  B1SemanticResourceVector resources;
};

/**
 * @brief Closed canonical semantic-trace action vocabulary.
 * @throws Nothing for value construction and comparison.
 */
enum class B1SemanticAction : std::uint8_t {
  /** @brief Task became dependency-ready. */
  Ready,
  /** @brief Physical execution irreversibly started. */
  Start,
  /** @brief Task reached its logical terminal outcome. */
  Terminal,
};

/**
 * @brief Closed canonical semantic-trace terminal outcome.
 * @throws Nothing for value construction and comparison.
 */
enum class B1SemanticOutcome : std::uint8_t {
  /** @brief Required dash sentinel on nonterminal records. */
  NotApplicable,
  /** @brief Fault-free terminal success. */
  Succeeded,
};

/**
 * @brief One normalized semantic-trace record.
 * @throws std::bad_alloc when copied task dependencies allocate.
 */
struct B1SemanticRecord final {
  /** @brief Complete deterministic task description. */
  B1SemanticTask task;
  /** @brief Ready, start, or terminal action. */
  B1SemanticAction action = B1SemanticAction::Ready;
  /** @brief Dash for nonterminal or succeeded for terminal. */
  B1SemanticOutcome outcome = B1SemanticOutcome::NotApplicable;
};

/**
 * @brief Immutable logical/raw golden for one B1 fixture job.
 * @throws Nothing for value operations.
 * @note Expected values are derived independently before candidate execution.
 */
struct B1JobGolden final {
  /** @brief Fixture selector in `[0,255]`. */
  std::uint64_t job_index = 0U;
  /** @brief Typed logical DenseTensor content identity. */
  ContentDigest logical_digest;
  /** @brief SHA-256 of exact tight little-endian payload bytes. */
  B1Sha256Digest raw_payload_digest;
};

/**
 * @brief Returns the stable lowercase name of one B1 phase.
 * @param phase Phase to name.
 * @return Process-lifetime closed token.
 * @throws std::invalid_argument for an invalid enum representation.
 */
const char* b1_job_phase_name(B1JobPhase phase);

/**
 * @brief Returns the stable Graph token of one B1 role.
 * @param role Graph role to name.
 * @return `A` or `B`.
 * @throws std::invalid_argument for an invalid enum representation.
 */
const char* b1_graph_role_name(B1GraphRole role);

/**
 * @brief Returns the stable stage token of one B1 Compute I/O task.
 * @param stage Stage to name.
 * @return `payload-stage` or `manifest-commit`.
 * @throws std::invalid_argument for an invalid enum representation.
 */
const char* b1_io_stage_name(B1IoStage stage);

/**
 * @brief Validates every closed component of one job occurrence.
 * @param job Candidate occurrence.
 * @return Nothing for a valid B1 job identity.
 * @throws std::invalid_argument for workload, replicate, job, or cap drift.
 */
void validate_b1_job_instance(const B1JobInstance& job);

/**
 * @brief Validates one full task-attempt identity.
 * @param task Candidate task identity.
 * @return Nothing for a valid task.
 * @throws std::invalid_argument for invalid job/stage state.
 */
void validate_b1_io_task_identity(const B1IoTaskIdentity& task);

/**
 * @brief Encodes one job instance as the exact fixed-record payload.
 * @param job Validated occurrence.
 * @return Six component frames in normative order.
 * @throws std::invalid_argument for invalid identity.
 * @throws std::bad_alloc when result storage cannot allocate.
 */
std::string encode_b1_job_instance(const B1JobInstance& job);

/**
 * @brief Derives Graph ownership from immutable job parity.
 * @param job_index Fixture selector in `[0,255]`.
 * @return Graph A for even jobs and Graph B for odd jobs.
 * @throws std::out_of_range outside `[0,255]`.
 */
B1GraphRole b1_graph_for_job(std::uint64_t job_index);

/**
 * @brief Computes exact canonical manifest length for one B1 job.
 * @param job_index Fixture selector in `[0,255]`.
 * @return `242 + decimal_digit_count(job_index)`.
 * @throws std::out_of_range outside `[0,255]`.
 */
std::uint64_t b1_manifest_length(std::uint64_t job_index);

/**
 * @brief Converts one SHA-256 value to lowercase hexadecimal.
 * @param digest Digest to encode.
 * @return Exactly 64 lowercase hexadecimal bytes.
 * @throws std::bad_alloc when string allocation fails.
 */
std::string b1_digest_hex(const B1Sha256Digest& digest);

/**
 * @brief Parses exactly one lowercase SHA-256 spelling.
 * @param text Candidate 64-byte lowercase hexadecimal value.
 * @return Parsed digest.
 * @throws std::invalid_argument for wrong length or noncanonical byte.
 */
B1Sha256Digest parse_b1_digest(std::string_view text);

/**
 * @brief Hashes one complete in-memory byte range with SHA-256.
 * @param data Borrowed bytes, null only when `size` is zero.
 * @param size Input byte count.
 * @return Raw SHA-256 digest.
 * @throws As `B1Sha256::update`.
 */
B1Sha256Digest b1_sha256(const std::byte* data, std::size_t size);

/**
 * @brief Hashes one complete string byte range with SHA-256.
 * @param text Borrowed bytes.
 * @return Raw SHA-256 digest.
 * @throws As `B1Sha256::update`.
 */
B1Sha256Digest b1_sha256(std::string_view text);

/**
 * @brief Builds the exact B1 graph document for one source seed.
 * @param job_index Source seed/fixture selector in `[0,255]`.
 * @return Complete five-node YAML document.
 * @throws std::out_of_range outside `[0,255]`.
 * @throws std::bad_alloc when output allocation fails.
 */
std::string b1_frozen_graph_yaml(std::uint64_t job_index);

/**
 * @brief Builds the exact replacement YAML for B1 source node zero.
 * @param job_index Source seed/fixture selector in `[0,255]`.
 * @return Complete source-node YAML value.
 * @throws std::out_of_range outside `[0,255]`.
 * @throws std::bad_alloc when output allocation fails.
 */
std::string b1_source_node_yaml(std::uint64_t job_index);

/**
 * @brief Builds the ordinary Host compute request for one B1 job.
 * @param session Graph A or B session.
 * @param run_cap Exact cap one or eight.
 * @return Full HP, force-recache, disk-disabled request for target node four.
 * @throws std::invalid_argument for unsupported cap.
 * @throws std::bad_alloc when request strings allocate.
 * @note Throughput QoS/weight are supplied by the source-private B1 Host seam.
 */
HostComputeRequest make_b1_host_compute_request(const GraphSessionId& session,
                                                std::uint64_t run_cap);

/**
 * @brief Builds the exact deterministic plan for one frozen B1 job.
 * @param job_index Fixture selector in `[0,255]`.
 * @return Source task followed by four ordered 64-tile curve stages.
 * @throws std::out_of_range outside `[0,255]`.
 * @throws std::bad_alloc when task/dependency storage allocates.
 */
std::vector<B1SemanticTask> b1_frozen_semantic_plan(std::uint64_t job_index);

/**
 * @brief Builds three successful semantic records per deterministic task.
 * @param tasks Complete contiguous plan.
 * @return Ready/start/terminal records before canonical sorting.
 * @throws std::invalid_argument for an invalid plan.
 * @throws std::bad_alloc when record storage allocates.
 */
std::vector<B1SemanticRecord> make_b1_success_semantic_records(
    const std::vector<B1SemanticTask>& tasks);

/**
 * @brief Validates and encodes exact semantic-trace-v1 bytes.
 * @param records Complete record set for one or more jobs.
 * @return Canonical header and sorted LF-terminated records.
 * @throws std::invalid_argument for schema, ordering, dependency, or outcome
 * drift.
 * @throws std::bad_alloc when canonical storage allocates.
 */
std::string encode_b1_semantic_trace(
    const std::vector<B1SemanticRecord>& records);

/**
 * @brief Parses and validates exact semantic-trace-v1 bytes independently.
 * @param bytes Candidate complete canonical bytes.
 * @return Parsed records in canonical order.
 * @throws std::invalid_argument for any byte/schema/plan violation.
 * @throws std::bad_alloc when parsed storage allocates.
 */
std::vector<B1SemanticRecord> parse_b1_semantic_trace(std::string_view bytes);

/**
 * @brief Generates the independent frozen B1 oracle image for one job.
 * @param job_index Source seed/fixture selector in `[0,255]`.
 * @return Exact Ready CPU FP32 normalized `[0,1]` RGBA Value after four
 * binary32-RNE stages.
 * @throws std::out_of_range outside `[0,255]`.
 * @throws std::bad_alloc when the image allocation fails.
 * @throws std::runtime_error when the floating-point environment cannot be
 * captured, changed to RNE, or restored.
 * @note This path invokes no Host, Kernel, Graph, cache, scheduler, YAML, or
 * candidate provider code.
 */
Value generate_b1_oracle_image(std::uint64_t job_index);

/**
 * @brief Computes independent logical and raw goldens from the frozen oracle.
 * @param job_index Source seed/fixture selector in `[0,255]`.
 * @return Complete expected logical/raw identity.
 * @throws Oracle, Value validation, digest, or allocation errors unchanged.
 * @note Runners call this before candidate execution; candidate bytes never
 * initialize or replace the expected result. Logical identity binds
 * DenseTensor schema/Image facet structural version 2 and normalized `[0,1]`
 * Sample Domain facet structural version 1, while raw payload identity remains
 * independent of descriptor framing.
 */
B1JobGolden compute_b1_job_golden(std::uint64_t job_index);

/**
 * @brief Returns one compiled immutable B1 fixture golden.
 * @param job_index Required measured or cold/warmup seed.
 * @return Exact precomputed logical and raw identities.
 * @throws std::out_of_range outside `0..29,252..255`.
 * @throws std::logic_error if a compiled digest spelling is corrupt.
 * @note Candidate execution never initializes or updates this table. The
 * independent oracle exists only to regenerate and verify fixture constants;
 * its current logical entries bind DenseTensor schema/Image facet structural
 * version 2 plus normalized `[0,1]` Sample Domain facet structural version 1,
 * and retain the prior raw-payload entries.
 */
B1JobGolden b1_frozen_job_golden(std::uint64_t job_index);

/**
 * @brief Builds the exact canonical artifact manifest for one B1 payload.
 * @param job_index Fixture selector in `[0,255]`.
 * @param payload_digest SHA-256 of exact raw payload bytes.
 * @return Exact 243, 244, or 245-byte LF-terminated manifest.
 * @throws std::out_of_range outside `[0,255]`.
 * @throws std::logic_error if the fixed-length invariant is violated.
 * @throws std::bad_alloc when output storage allocates.
 */
std::string b1_artifact_manifest(std::uint64_t job_index,
                                 const B1Sha256Digest& payload_digest);

}  // namespace ps::benchmark
