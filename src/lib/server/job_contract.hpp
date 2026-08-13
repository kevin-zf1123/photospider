/**
 * @file job_contract.hpp
 * @brief Declares the source-private Issue #99 Job and artifact value model.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "photospider/core/image_buffer.hpp"

namespace ps::server {

/** @brief Maximum byte length of one accepted opaque textual identity. */
inline constexpr std::size_t kMaximumOpaqueIdentityBytes = 128U;

/**
 * @brief Maximum configured device-capacity rows in one Job envelope.
 * @note This semantic admission/recovery bound is independent of opaque
 * identity byte length. It applies equally to tenant quota configuration,
 * JobSpec construction/validation, canonical serialization, and recovery.
 */
inline constexpr std::size_t kMaximumConfiguredDevicesPerJob = 128U;

/**
 * @brief Strong source-private textual identity with one compile-time domain.
 *
 * @tparam Domain Empty tag that makes otherwise equal text a different type.
 * @throws std::invalid_argument when a nonempty value is too long, contains a
 * path separator, is a dot path component, or contains a byte outside the
 * accepted opaque-token set.
 * @throws std::bad_alloc when copying identity text exhausts memory.
 * @note Default construction creates an invalid empty sentinel for aggregate
 * result values. Every authority boundary calls `valid()` or accepts only a
 * value constructed from validated nonempty text.
 */
template <typename Domain>
class OpaqueTextId final {
 public:
  /**
   * @brief Creates an invalid empty sentinel for aggregate result values.
   * @throws Nothing.
   * @note Authority boundaries reject this sentinel through `valid()`.
   */
  OpaqueTextId() noexcept = default;

  /**
   * @brief Creates one validated nonempty opaque identity.
   * @param value Candidate ASCII token.
   * @throws std::invalid_argument for an invalid token.
   * @throws std::bad_alloc when storing the token exhausts memory.
   */
  explicit OpaqueTextId(std::string value) : value_(std::move(value)) {
    if (!valid_identity_text(value_)) {
      throw std::invalid_argument("opaque identity text is invalid");
    }
  }

  /**
   * @brief Reports whether this value is a usable nonempty authority identity.
   * @return True only for a validated nonempty token.
   * @throws Nothing.
   */
  bool valid() const noexcept { return !value_.empty(); }

  /**
   * @brief Returns the immutable opaque token.
   * @return Borrowed text valid for this value's lifetime.
   * @throws Nothing.
   */
  const std::string& value() const noexcept { return value_; }

  /**
   * @brief Compares exact text within this identity domain.
   * @param other Same-domain candidate.
   * @return True only when the complete tokens match.
   * @throws Nothing.
   */
  bool operator==(const OpaqueTextId& other) const noexcept {
    return value_ == other.value_;
  }

  /**
   * @brief Compares exact text for inequality within this identity domain.
   * @param other Same-domain candidate.
   * @return True when the complete tokens differ.
   * @throws Nothing.
   */
  bool operator!=(const OpaqueTextId& other) const noexcept {
    return !(*this == other);
  }

 private:
  /**
   * @brief Validates the closed identity-token alphabet without allocating.
   * @param text Candidate token, which must be nonempty.
   * @return True only for one bounded ASCII token that is not a path
   * component.
   * @throws Nothing.
   */
  static bool valid_identity_text(std::string_view text) noexcept {
    if (text.empty() || text == "." || text == ".." ||
        text.size() > kMaximumOpaqueIdentityBytes) {
      return false;
    }
    for (const unsigned char character : text) {
      const bool alpha_numeric = (character >= 'a' && character <= 'z') ||
                                 (character >= 'A' && character <= 'Z') ||
                                 (character >= '0' && character <= '9');
      if (!alpha_numeric && character != '-' && character != '_' &&
          character != '.' && character != ':') {
        return false;
      }
    }
    return true;
  }

  /** @brief Exact validated token, or empty only for the sentinel. */
  std::string value_;
};

/** @brief Tag for the one configured tenant identity domain. */
struct TenantIdDomain final {};
/** @brief Single-tenant authority identity, distinct from every other id. */
using TenantId = OpaqueTextId<TenantIdDomain>;

/** @brief Tag for accepted Job identity values. */
struct JobIdDomain final {};
/** @brief One immutable accepted Job identity. */
using JobId = OpaqueTextId<JobIdDomain>;

/** @brief Tag for one Job attempt identity domain. */
struct JobAttemptIdDomain final {};
/** @brief One non-reused attempt within a durable Job's retry history. */
using JobAttemptId = OpaqueTextId<JobAttemptIdDomain>;

/** @brief Tag for one attempt worker identity domain. */
struct WorkerInstanceIdDomain final {};
/** @brief One fresh, never-reused Issue #100 worker process identity. */
using WorkerInstanceId = OpaqueTextId<WorkerInstanceIdDomain>;

/** @brief Tag for immutable graph artifact identity values. */
struct GraphArtifactIdDomain final {};
/** @brief Authorized graph material identity resolved outside JobSpec. */
using GraphArtifactId = OpaqueTextId<GraphArtifactIdDomain>;

/** @brief Tag for declared output slot identity values. */
struct OutputSlotIdDomain final {};
/** @brief One immutable output slot promised by JobSpec. */
using OutputSlotId = OpaqueTextId<OutputSlotIdDomain>;

/** @brief Tag for crash-durable committed artifact identities. */
struct ArtifactIdDomain final {};
/** @brief One immutable artifact version identity, never a content digest. */
using ArtifactId = OpaqueTextId<ArtifactIdDomain>;

/** @brief Tag for one stable idempotent output commit identity. */
struct OutputCommitIdDomain final {};
/** @brief One exact artifact commit event identity. */
using OutputCommitId = OpaqueTextId<OutputCommitIdDomain>;

/**
 * @brief Assignment generation bound to one exact worker instance.
 * @throws Nothing for value operations.
 * @note Zero is an invalid sentinel and never names an assignment.
 */
struct WorkerLeaseGeneration final {
  /** @brief Positive generation, or zero only for an invalid sentinel. */
  std::uint64_t value = 0U;

  /**
   * @brief Compares exact assignment generations.
   * @param other Candidate generation.
   * @return True when numeric generations match.
   * @throws Nothing.
   */
  bool operator==(const WorkerLeaseGeneration& other) const noexcept {
    return value == other.value;
  }

  /**
   * @brief Compares exact assignment generations for inequality.
   * @param other Candidate generation.
   * @return True when numeric generations differ.
   * @throws Nothing.
   */
  bool operator!=(const WorkerLeaseGeneration& other) const noexcept {
    return !(*this == other);
  }
};

/**
 * @brief Strong fixed SHA-256 value in one compile-time digest domain.
 * @tparam Domain Empty tag that prevents cross-domain substitution.
 * @throws Nothing for value operations except hexadecimal string allocation.
 */
template <typename Domain>
struct Sha256Digest final {
  /** @brief Exact 32 SHA-256 bytes in network/display order. */
  std::array<std::byte, 32U> bytes{};

  /**
   * @brief Compares every digest byte within this domain.
   * @param other Same-domain digest.
   * @return True only for exact byte equality.
   * @throws Nothing.
   */
  bool operator==(const Sha256Digest& other) const noexcept {
    return bytes == other.bytes;
  }

  /**
   * @brief Compares digest bytes for inequality within this domain.
   * @param other Same-domain digest.
   * @return True when at least one byte differs.
   * @throws Nothing.
   */
  bool operator!=(const Sha256Digest& other) const noexcept {
    return !(*this == other);
  }

  /**
   * @brief Encodes the digest as canonical lowercase hexadecimal.
   * @return Exactly 64 ASCII hexadecimal characters.
   * @throws std::bad_alloc when allocating the result exhausts memory.
   */
  std::string hex() const {
    constexpr char kHex[] = "0123456789abcdef";
    std::string result(bytes.size() * 2U, '0');
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
      const std::uint8_t value = std::to_integer<std::uint8_t>(bytes[index]);
      result[index * 2U] = kHex[value >> 4U];
      result[index * 2U + 1U] = kHex[value & 0x0fU];
    }
    return result;
  }
};

/** @brief Tag for canonical immutable JobSpec SHA-256 values. */
struct JobSpecDigestDomain final {};
/** @brief SHA-256 of exact canonical JobSpec bytes. */
using JobSpecDigest = Sha256Digest<JobSpecDigestDomain>;

/** @brief Tag for immutable tight artifact payload SHA-256 values. */
struct ArtifactContentDigestDomain final {};
/** @brief SHA-256 of exact immutable tight artifact payload bytes. */
using ArtifactContentDigest = Sha256Digest<ArtifactContentDigestDomain>;

/**
 * @brief Closed execution profile supported by the Issue #99 vertical.
 * @throws Nothing for value operations.
 */
enum class JobExecutionProfile : std::uint8_t {
  /** @brief Trusted in-process CPU providers through one fresh Embedded Host.
   */
  EmbeddedCpuV1,
};

/**
 * @brief Closed requested and achieved artifact durability vocabulary.
 * @throws Nothing for value operations.
 */
enum class ArtifactDurability : std::uint8_t {
  /** @brief Payload, manifest, and namespace survive a process restart. */
  CrashDurable,
};

/**
 * @brief Closed control-plane Job state vocabulary persisted by Issue #99.
 * @throws Nothing for value operations.
 */
enum class JobState : std::uint8_t {
  /** @brief Accepted current assignment has not entered worker execution. */
  Queued,
  /**
   * @brief Current assignment supervision has begun.
   * @note External process spawn, `AssignmentAccepted`, and the first
   * heartbeat may still be pending; this state is not worker readiness.
   */
  Running,
  /** @brief Monotonic cancellation intent awaits current settlement. */
  Cancelling,
  /** @brief Durable required output receipt completed the Job. */
  Succeeded,
  /** @brief Current attempt failed and may be explicitly retried by policy. */
  Failed,
  /** @brief Current attempt settled after accepted cancellation. */
  Cancelled,
};

/**
 * @brief Closed worker-local attempt outcome facts.
 * @throws Nothing for value operations.
 */
enum class JobAttemptOutcome : std::uint8_t {
  /** @brief No terminal attempt fact has been accepted yet. */
  None,
  /** @brief Worker produced one valid candidate after Host settlement. */
  Succeeded,
  /** @brief Worker reported typed failure facts. */
  Failed,
  /** @brief Worker observed control-plane cancellation and settled. */
  Cancelled,
};

/**
 * @brief Closed typed failure domains for worker and control-plane facts.
 * @throws Nothing for value operations.
 */
enum class JobAttemptFailure : std::uint8_t {
  /** @brief No failure belongs to a successful/no-outcome state. */
  None,
  /** @brief Worker rejected malformed immutable assignment data. */
  InvalidAssignment,
  /** @brief Trusted graph artifact resolution failed. */
  GraphResolution,
  /** @brief Fresh Embedded Host construction/seeding failed. */
  HostSetup,
  /** @brief Attempt-local graph load failed. */
  GraphLoad,
  /** @brief Compute or candidate-image validation failed. */
  Compute,
  /** @brief Host/graph ownership could not be settled. */
  Settlement,
  /** @brief Cooperative cancellation was observed and settled. */
  CancellationObserved,
  /** @brief Worker creation/execution crossed an unexpected exception boundary.
   */
  Unexpected,
  /** @brief Report tuple/enum/shape failed closed at the control plane. */
  ReportRejected,
  /** @brief Durable artifact or terminal control publication failed. */
  ArtifactCommit,
  /** @brief Restart found an interrupted process-local attempt. */
  RecoveryInterrupted,
  /** @brief External assignment preparation, process setup, or exec failed. */
  WorkerStartup,
  /** @brief Worker exited nonzero without a trustworthy terminal report. */
  WorkerExit,
  /** @brief Worker died by signal, including OOM-compatible SIGKILL. */
  WorkerSignal,
  /** @brief Private worker channel closed or failed unexpectedly. */
  WorkerChannel,
  /** @brief Worker violated the closed versioned transport contract. */
  WorkerProtocol,
  /** @brief Worker stopped producing heartbeats before its attempt settled. */
  WorkerHeartbeatTimeout,
  /** @brief Worker exceeded the configured total attempt runtime. */
  WorkerRuntimeTimeout,
  /** @brief Exact cancelled worker required terminate/kill escalation. */
  WorkerCancellationForced,
};

/**
 * @brief One configured device-capacity request in a canonical Job envelope.
 * @throws Nothing for default/value operations; copied text may allocate.
 * @note `device_id` is an opaque configured server label, never a native
 * device handle, runtime ordinal authority, or worker-local ledger token.
 */
struct DeviceResourceRequest final {
  /** @brief Nonempty opaque configured device label. */
  std::string device_id;
  /** @brief Positive reserved bytes on that configured device. */
  std::uint64_t bytes = 0U;

  /**
   * @brief Compares the exact configured label and byte request.
   * @param other Candidate device request.
   * @return True only when both fields match.
   * @throws Nothing.
   */
  bool operator==(const DeviceResourceRequest& other) const noexcept {
    return device_id == other.device_id && bytes == other.bytes;
  }
};

/**
 * @brief Complete immutable server quota demand for one Job attempt.
 * @throws Nothing for default/value operations; vector copies may allocate.
 * @note CPU slots are also the current Embedded Host maximum-parallelism cap.
 * Issue #100 additionally applies host memory as the POSIX worker address-
 * space ceiling. Configured device values remain admission declarations and
 * never mint worker-local `ResourceLedger` authority or a device sandbox.
 */
struct JobResourceRequest final {
  /** @brief Positive server CPU-slot reservation and Host callback cap. */
  std::uint32_t cpu_slots = 0U;
  /** @brief Positive declared host-memory bound in bytes. */
  std::uint64_t host_memory_bytes = 0U;
  /** @brief Positive maximum committed payload bytes. */
  std::uint64_t output_bytes = 0U;
  /** @brief Positive maximum private staging bytes. */
  std::uint64_t staging_bytes = 0U;
  /** @brief Positive maximum durable retained payload bytes. */
  std::uint64_t retention_bytes = 0U;
  /**
   * @brief Strictly device-id-sorted unique positive device requests.
   * @note The vector contains at most `kMaximumConfiguredDevicesPerJob` rows.
   */
  std::vector<DeviceResourceRequest> devices;

  /**
   * @brief Compares every scalar and ordered configured-device request.
   * @param other Candidate complete request.
   * @return True only when the complete envelopes match.
   * @throws Nothing.
   */
  bool operator==(const JobResourceRequest& other) const noexcept {
    return cpu_slots == other.cpu_slots &&
           host_memory_bytes == other.host_memory_bytes &&
           output_bytes == other.output_bytes &&
           staging_bytes == other.staging_bytes &&
           retention_bytes == other.retention_bytes && devices == other.devices;
  }
};

/**
 * @brief Exact assignment identity tuple retained by control plane and report.
 * @throws Nothing for default construction; string copies may allocate.
 */
struct AttemptIdentity final {
  /** @brief Configured single tenant that owns the Job. */
  TenantId tenant_id;
  /** @brief Immutable accepted Job identity. */
  JobId job_id;
  /** @brief Digest of the exact accepted canonical JobSpec. */
  JobSpecDigest job_spec_digest;
  /** @brief Current attempt identity. */
  JobAttemptId attempt_id;
  /** @brief Fresh worker instance identity. */
  WorkerInstanceId worker_instance_id;
  /** @brief Exact assignment generation for this worker. */
  WorkerLeaseGeneration worker_lease_generation;

  /**
   * @brief Compares the complete authority tuple.
   * @param other Candidate tuple.
   * @return True only when every independent identity matches.
   * @throws Nothing.
   */
  bool operator==(const AttemptIdentity& other) const noexcept;

  /**
   * @brief Compares the complete authority tuple for inequality.
   * @param other Candidate tuple.
   * @return True when any independent identity differs.
   * @throws Nothing.
   */
  bool operator!=(const AttemptIdentity& other) const noexcept {
    return !(*this == other);
  }
};

/**
 * @brief Immutable supported Issue #99 Job specification.
 *
 * Construction validates every field, derives exact canonical bytes, and
 * records their SHA-256 digest. Canonical `jobspec-v2` bytes frame graph,
 * target, output, profile, durability, every resource scalar, every ordered
 * device pair, and optional checkpoint presence/value. Integer fields use
 * canonical decimal spellings before framing. The type intentionally has no
 * path, session, descriptor, pointer, native handle, credential, plugin, or
 * IPC-id field.
 *
 * @throws std::invalid_argument for an empty identity, negative target,
 * invalid enum representation, invalid resource request, or malformed
 * checkpoint sentinel.
 * @throws std::bad_alloc when storing or canonicalizing values exhausts memory.
 * @throws std::overflow_error when canonical length framing cannot represent a
 * field.
 * @note Copies preserve the exact canonical bytes and digest. Accepted Jobs
 * retain `shared_ptr<const JobSpec>` so workers cannot replace the value.
 */
class JobSpec final {
 public:
  /**
   * @brief Validates and freezes one supported single-output JobSpec.
   * @param graph_artifact_id Immutable graph material identity.
   * @param target_node Nonnegative graph node selector.
   * @param output_slot_id Declared required image output slot.
   * @param resource_request Complete validated server quota demand.
   * @param checkpoint_artifact_id Optional durable checkpoint identity.
   * @param execution_profile Closed current execution profile.
   * @param requested_durability Closed current durability request.
   * @throws As documented on the class.
   */
  JobSpec(GraphArtifactId graph_artifact_id, int target_node,
          OutputSlotId output_slot_id, JobResourceRequest resource_request,
          std::optional<ArtifactId> checkpoint_artifact_id = std::nullopt,
          JobExecutionProfile execution_profile =
              JobExecutionProfile::EmbeddedCpuV1,
          ArtifactDurability requested_durability =
              ArtifactDurability::CrashDurable);

  /**
   * @brief Returns the immutable graph artifact identity.
   * @return Borrowed identity valid for this JobSpec's lifetime.
   * @throws Nothing.
   */
  const GraphArtifactId& graph_artifact_id() const noexcept {
    return graph_artifact_id_;
  }

  /**
   * @brief Returns the frozen nonnegative graph node selector.
   * @return Accepted target node.
   * @throws Nothing.
   */
  int target_node() const noexcept { return target_node_; }

  /**
   * @brief Returns the one declared required output slot.
   * @return Borrowed identity valid for this JobSpec's lifetime.
   * @throws Nothing.
   */
  const OutputSlotId& output_slot_id() const noexcept {
    return output_slot_id_;
  }

  /**
   * @brief Returns the complete immutable quota demand.
   * @return Borrowed canonical resource request.
   * @throws Nothing.
   */
  const JobResourceRequest& resource_request() const noexcept {
    return resource_request_;
  }

  /**
   * @brief Returns the optional authorized checkpoint identity.
   * @return Empty for a fresh Job, otherwise one durable ArtifactId.
   * @throws Nothing.
   */
  const std::optional<ArtifactId>& checkpoint_artifact_id() const noexcept {
    return checkpoint_artifact_id_;
  }

  /**
   * @brief Returns the closed execution profile.
   * @return Frozen supported execution profile.
   * @throws Nothing.
   */
  JobExecutionProfile execution_profile() const noexcept {
    return execution_profile_;
  }

  /**
   * @brief Returns the requested artifact durability.
   * @return Frozen crash-durable request.
   * @throws Nothing.
   */
  ArtifactDurability requested_durability() const noexcept {
    return requested_durability_;
  }

  /**
   * @brief Returns the frozen canonical `jobspec-v2` bytes.
   * @return Borrowed bytes valid for this JobSpec's lifetime.
   * @throws Nothing.
   */
  const std::string& canonical_bytes() const noexcept {
    return canonical_bytes_;
  }

  /**
   * @brief Returns SHA-256 of the exact frozen canonical bytes.
   * @return Immutable digest value.
   * @throws Nothing.
   */
  const JobSpecDigest& digest() const noexcept { return digest_; }

 private:
  /** @brief Immutable graph artifact identity. */
  GraphArtifactId graph_artifact_id_;
  /** @brief Nonnegative graph node selector. */
  int target_node_ = -1;
  /** @brief One required image output slot. */
  OutputSlotId output_slot_id_;
  /** @brief Complete immutable server quota demand. */
  JobResourceRequest resource_request_;
  /** @brief Optional durable checkpoint input identity. */
  std::optional<ArtifactId> checkpoint_artifact_id_;
  /** @brief Closed current execution profile. */
  JobExecutionProfile execution_profile_ = JobExecutionProfile::EmbeddedCpuV1;
  /** @brief Closed current requested durability. */
  ArtifactDurability requested_durability_ = ArtifactDurability::CrashDurable;
  /** @brief Exact versioned canonical field framing. */
  std::string canonical_bytes_;
  /** @brief SHA-256 of `canonical_bytes_`. */
  JobSpecDigest digest_;
};

/**
 * @brief Immutable image descriptor bound into an artifact receipt.
 * @throws Nothing for value operations.
 * @note `row_bytes` is always tight and `payload_bytes` excludes source row
 * padding.
 */
struct ArtifactImageDescriptor final {
  /** @brief Positive image width in pixels. */
  int width = 0;
  /** @brief Positive image height in pixels. */
  int height = 0;
  /** @brief Positive channel count. */
  int channels = 0;
  /** @brief Exact channel storage type. */
  DataType type = DataType::FLOAT32;
  /** @brief Active bytes in one tightly stored row. */
  std::size_t row_bytes = 0U;
  /** @brief Exact immutable tight payload length. */
  std::size_t payload_bytes = 0U;

  /**
   * @brief Compares every descriptor field.
   * @param other Candidate descriptor.
   * @return True only for an exact descriptor match.
   * @throws Nothing.
   */
  bool operator==(const ArtifactImageDescriptor& other) const noexcept;
};

/**
 * @brief Complete immutable output commit receipt for one assignment and slot.
 * @throws Nothing for default construction; copied identity strings may
 * allocate.
 * @note The receipt is returned only after crash-durability barriers complete.
 */
struct OutputCommitReceipt final {
  /** @brief Full assignment tuple authorized for this commit. */
  AttemptIdentity attempt;
  /** @brief Required JobSpec output slot fulfilled by the artifact. */
  OutputSlotId output_slot_id;
  /** @brief Fresh immutable artifact-version identity. */
  ArtifactId artifact_id;
  /** @brief Fresh exact commit-event identity. */
  OutputCommitId output_commit_id;
  /** @brief Immutable tight image descriptor. */
  ArtifactImageDescriptor descriptor;
  /** @brief SHA-256 of exact tight payload bytes. */
  ArtifactContentDigest content_digest;
  /** @brief Exact achieved crash-durable capability. */
  ArtifactDurability achieved_durability = ArtifactDurability::CrashDurable;
};

/**
 * @brief Read-only crash-durable artifact record returned by lookup.
 * @throws std::bad_alloc when copying payload or receipt values exhausts
 * memory.
 * @note Records expose no filesystem path, runtime handle, or mutation API.
 */
struct ArtifactRecord final {
  /** @brief Identity-complete commit receipt. */
  OutputCommitReceipt receipt;
  /** @brief Exact tight immutable image payload. */
  std::vector<std::byte> payload;
};

/**
 * @brief Recomputes SHA-256 of arbitrary raw bytes in the Job contract domain.
 * @param bytes Borrowed bytes, null only when `size` is zero.
 * @param size Number of input bytes.
 * @return Exact digest in the JobSpec domain.
 * @throws std::invalid_argument for a null nonempty range.
 * @throws std::overflow_error when SHA-256 bit-length encoding overflows.
 */
JobSpecDigest hash_job_spec_bytes(const std::byte* bytes, std::size_t size);

/**
 * @brief Computes SHA-256 of an immutable artifact payload.
 * @param bytes Borrowed bytes, null only when `size` is zero.
 * @param size Number of input bytes.
 * @return Exact digest in the artifact-content domain.
 * @throws std::invalid_argument for a null nonempty range.
 * @throws std::overflow_error when SHA-256 bit-length encoding overflows.
 */
ArtifactContentDigest hash_artifact_content(const std::byte* bytes,
                                            std::size_t size);

/**
 * @brief Computes SHA-256 over the exact tight active rows of one CPU image.
 * @param image Valid nonempty CPU image; source row padding is excluded.
 * @return Exact digest matching a row-by-row tight artifact payload.
 * @throws std::invalid_argument for invalid, empty, or non-CPU image state.
 * @throws std::overflow_error when row or SHA-256 length arithmetic overflows.
 * @throws std::logic_error only if the internal single-use hash lifecycle is
 * violated.
 * @note The borrowed image and its storage remain caller-owned and unchanged.
 * Hashing performs no full-payload copy and is not a durability operation.
 */
ArtifactContentDigest hash_image_artifact_content(const ImageBuffer& image);

/**
 * @brief Validates a complete assignment identity tuple.
 * @param identity Candidate assignment.
 * @return Nothing after validation.
 * @throws std::invalid_argument when any identity is empty or lease generation
 * is zero.
 */
void validate_attempt_identity(const AttemptIdentity& identity);

/**
 * @brief Revalidates one frozen JobSpec and its canonical digest.
 * @param spec Candidate immutable spec.
 * @return Nothing after exact field, canonical-byte, and digest validation.
 * @throws std::invalid_argument when any current contract invariant differs.
 * @throws std::bad_alloc when recomputing canonical bytes exhausts memory.
 * @throws std::overflow_error when framing or hashing overflows.
 * @note Workers call this before resolving graph material.
 */
void validate_job_spec(const JobSpec& spec);

/**
 * @brief Validates a complete canonical Job resource request.
 * @param request Candidate immutable demand.
 * @return Nothing after scalar, token, order, and uniqueness validation.
 * @throws std::invalid_argument when a required scalar is zero, device count
 * exceeds `kMaximumConfiguredDevicesPerJob`, a device label is not a bounded
 * opaque token, device bytes are zero, or labels are not strictly ascending.
 * @note This validates declared shape only; `TenantQuotaAuthority` owns
 * capacity admission.
 */
void validate_job_resource_request(const JobResourceRequest& request);

}  // namespace ps::server
