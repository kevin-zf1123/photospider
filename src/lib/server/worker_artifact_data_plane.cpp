/**
 * @file worker_artifact_data_plane.cpp
 * @brief Implements the attempt-scoped Issue #105 worker artifact data plane.
 */
#include "server/worker_artifact_data_plane.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ps::server {
namespace {

/** @brief Maximum bytes moved by one manager-side nonblocking operation. */
constexpr std::size_t kDataPlaneChunkBytes = 64U << 10U;

/**
 * @brief Clears descriptor ownership before one non-retried close attempt.
 * @param descriptor Non-null exact descriptor owner.
 * @return Nothing after ownership is invalidated.
 * @throws Nothing; close failure, including `EINTR`, is ignored.
 */
void close_once(int* descriptor) noexcept {
  if (descriptor == nullptr) {
    return;
  }
  const int owned = std::exchange(*descriptor, -1);
  if (owned >= 0) {
    static_cast<void>(::close(owned));
  }
}

/**
 * @brief Small move-only descriptor owner for throwing setup paths.
 * @throws Nothing for construction, moves, release, and destruction.
 */
class ScopedDescriptor final {
 public:
  /**
   * @brief Creates one empty descriptor owner.
   * @throws Nothing.
   */
  ScopedDescriptor() noexcept = default;

  /**
   * @brief Takes ownership of one descriptor.
   * @param descriptor Exact descriptor or invalid sentinel.
   * @throws Nothing.
   */
  explicit ScopedDescriptor(int descriptor) noexcept
      : descriptor_(descriptor) {}

  /**
   * @brief Closes any retained descriptor exactly once.
   * @throws Nothing; close failures are ignored without numeric retry.
   */
  ~ScopedDescriptor() noexcept { close_once(&descriptor_); }

  /**
   * @brief Prevents duplicate descriptor ownership.
   * @param other Existing owner that remains unchanged.
   */
  ScopedDescriptor(const ScopedDescriptor& other) = delete;
  /**
   * @brief Prevents duplicate descriptor assignment.
   * @param other Existing owner that remains unchanged.
   * @return No assignment result because the operation is deleted.
   */
  ScopedDescriptor& operator=(const ScopedDescriptor& other) = delete;

  /**
   * @brief Transfers one descriptor owner.
   * @param other Source cleared by the move.
   * @throws Nothing.
   */
  ScopedDescriptor(ScopedDescriptor&& other) noexcept
      : descriptor_(other.release()) {}

  /**
   * @brief Replaces ownership from another descriptor owner.
   * @param other Source cleared by the move.
   * @return This owner.
   * @throws Nothing.
   */
  ScopedDescriptor& operator=(ScopedDescriptor&& other) noexcept {
    if (this != &other) {
      close_once(&descriptor_);
      descriptor_ = other.release();
    }
    return *this;
  }

  /**
   * @brief Returns the retained descriptor without transfer.
   * @return Exact descriptor or -1.
   * @throws Nothing.
   */
  int get() const noexcept { return descriptor_; }

  /**
   * @brief Transfers descriptor ownership to the caller.
   * @return Prior exact descriptor or -1.
   * @throws Nothing.
   */
  int release() noexcept { return std::exchange(descriptor_, -1); }

 private:
  /** @brief Sole exact descriptor owner. */
  int descriptor_ = -1;
};

/**
 * @brief Owns both endpoints while one directional lane is configured.
 * @throws Nothing for value operations.
 * @note Setup helpers release each endpoint only into one complete
 * `WorkerArtifactDataPlane`; otherwise member destruction closes both.
 */
struct StreamLane final {
  /** @brief Worker-side blocking endpoint delegated through fixed exec fd. */
  ScopedDescriptor worker;
  /** @brief Manager-side nonblocking endpoint retained by the supervisor. */
  ScopedDescriptor manager;
};

/**
 * @brief Sets close-on-exec and optional nonblocking status on one endpoint.
 * @param descriptor Valid stream descriptor.
 * @param nonblocking Whether manager-side operations must return on EAGAIN.
 * @return Nothing after both requested flags are installed.
 * @throws std::system_error when descriptor flags cannot be read or changed.
 */
void configure_stream_descriptor(int descriptor, bool nonblocking) {
  const int descriptor_flags = ::fcntl(descriptor, F_GETFD);
  if (descriptor_flags < 0 ||
      ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "configure worker data-plane close-on-exec");
  }
  if (!nonblocking) {
    return;
  }
  const int status_flags = ::fcntl(descriptor, F_GETFL);
  if (status_flags < 0 ||
      ::fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "configure worker data-plane nonblocking mode");
  }
}

/**
 * @brief Installs local SIGPIPE suppression on one exact stream endpoint.
 * @param descriptor Valid AF_UNIX endpoint, including a direction-reduced
 * receiver whose reverse-send rejection must remain process-safe.
 * @return Nothing after platform-specific local suppression is installed.
 * @throws std::system_error when Darwin rejects `SO_NOSIGPIPE`.
 * @note Linux uses `MSG_NOSIGNAL` on every send and needs no socket option.
 */
void configure_no_sigpipe(int descriptor) {
#ifdef SO_NOSIGPIPE
  int enabled = 1;
  if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                   sizeof(enabled)) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "configure worker data-plane SIGPIPE suppression");
  }
#else
  static_cast<void>(descriptor);
#endif
}

/**
 * @brief Creates one AF_UNIX stream pair and configures retained flags.
 * @return Two close-on-exec endpoints; manager endpoint is nonblocking.
 * @throws std::system_error for socket creation or flag failure.
 */
StreamLane create_stream_lane() {
  int descriptors[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "create worker artifact stream lane");
  }
  StreamLane lane{ScopedDescriptor(descriptors[0]),
                  ScopedDescriptor(descriptors[1])};
  configure_stream_descriptor(lane.worker.get(), false);
  configure_stream_descriptor(lane.manager.get(), true);
  configure_no_sigpipe(lane.worker.get());
  configure_no_sigpipe(lane.manager.get());
  return lane;
}

/**
 * @brief Creates the manager-send/worker-receive checkpoint lane.
 * @return Direction-reduced stream endpoints.
 * @throws std::system_error for socket, shutdown, flag, or option failure.
 */
StreamLane create_checkpoint_lane() {
  StreamLane lane = create_stream_lane();
  if (::shutdown(lane.worker.get(), SHUT_WR) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "reduce worker checkpoint stream direction");
  }
  return lane;
}

/**
 * @brief Creates the worker-send/manager-receive candidate lane.
 * @return Direction-reduced stream endpoints.
 * @throws std::system_error for socket, shutdown, flag, or option failure.
 */
StreamLane create_output_lane() {
  StreamLane lane = create_stream_lane();
  if (::shutdown(lane.manager.get(), SHUT_WR) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "reduce worker output stream direction");
  }
  return lane;
}

/**
 * @brief Requires one descriptor to be a connected stream socket.
 * @param descriptor Candidate inherited data-plane descriptor.
 * @return Nothing for `SOCK_STREAM`.
 * @throws std::system_error when socket type cannot be queried.
 * @throws WorkerArtifactDataPlaneError for another descriptor type.
 */
void require_stream_socket(int descriptor) {
  int type = 0;
  socklen_t size = sizeof(type);
  if (::getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &type, &size) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "inspect worker artifact stream descriptor");
  }
  if (size != sizeof(type) || type != SOCK_STREAM) {
    throw WorkerArtifactDataPlaneError(
        "worker artifact descriptor is not a stream lane");
  }
}

/**
 * @brief Sends one complete byte range from the killable worker.
 * @param descriptor Valid worker-side output stream.
 * @param bytes Borrowed input, null only for an empty range.
 * @param size Exact byte count.
 * @return Nothing after every byte is accepted by the stream.
 * @throws std::invalid_argument for null nonempty input.
 * @throws std::system_error for a non-interruption send failure.
 * @note Blocking is intentional here: WorkerManager owns, signals, and exactly
 * reaps this process under its absolute lifecycle deadlines.
 */
void send_complete_from_worker(int descriptor, const std::byte* bytes,
                               std::size_t size) {
  if (size != 0U && bytes == nullptr) {
    throw std::invalid_argument("worker artifact stream input is null");
  }
  std::size_t offset = 0U;
  while (offset != size) {
    const std::size_t chunk =
        std::min(size - offset,
                 static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
#ifdef MSG_NOSIGNAL
    constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    constexpr int kSendFlags = 0;
#endif
    const ssize_t sent = ::send(descriptor, bytes + offset, chunk, kSendFlags);
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    if (sent <= 0) {
      throw std::system_error(errno == 0 ? EPIPE : errno,
                              std::generic_category(),
                              "send worker artifact stream bytes");
    }
    offset += static_cast<std::size_t>(sent);
  }
}

/**
 * @brief Receives an exact range and then requires the manager's EOF marker.
 * @param descriptor Valid worker-side checkpoint stream.
 * @param size Exact receipt-declared payload size.
 * @return Independently owned exact bytes.
 * @throws std::system_error for a non-interruption receive failure.
 * @throws WorkerArtifactDataPlaneError for premature EOF or extra bytes.
 * @throws std::bad_alloc when bounded allocation fails.
 * @note Blocking is intentional inside the killable worker process.
 */
std::vector<std::byte> receive_exact_checkpoint(int descriptor,
                                                std::size_t size) {
  std::vector<std::byte> bytes(size);
  std::size_t offset = 0U;
  while (offset != size) {
    const std::size_t chunk =
        std::min(size - offset,
                 static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    const ssize_t received =
        ::recv(descriptor, bytes.data() + offset, chunk, 0);
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "receive worker checkpoint stream bytes");
    }
    if (received == 0) {
      throw WorkerArtifactDataPlaneError(
          "worker checkpoint stream ended before its receipt size");
    }
    offset += static_cast<std::size_t>(received);
  }
  std::byte extra{};
  for (;;) {
    const ssize_t received = ::recv(descriptor, &extra, sizeof(extra), 0);
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "receive worker checkpoint stream commit");
    }
    if (received != 0) {
      throw WorkerArtifactDataPlaneError(
          "worker checkpoint stream exceeds its receipt size");
    }
    break;
  }
  return bytes;
}

/**
 * @brief Converts one accepted uint64 resource bound to local `size_t`.
 * @param value Positive accepted Job resource bound.
 * @return Exactly represented local size.
 * @throws std::overflow_error when the platform cannot represent the value.
 */
std::size_t resource_size(std::uint64_t value) {
  if (value > std::numeric_limits<std::size_t>::max()) {
    throw std::overflow_error("worker artifact resource bound is unsupported");
  }
  return static_cast<std::size_t>(value);
}

/**
 * @brief Returns the exact output/staging/retention intersection.
 * @param resources Valid immutable Job resource request.
 * @return Positive local tight-byte maximum.
 * @throws std::overflow_error when the platform cannot represent the minimum.
 */
std::size_t output_payload_maximum(const JobResourceRequest& resources) {
  return resource_size(
      std::min({resources.output_bytes, resources.staging_bytes,
                resources.retention_bytes}));
}

/**
 * @brief Derives one fixed-width non-authorizing reference from exact fields.
 * @param identity Complete current assignment identity.
 * @param direction Stable checkpoint-read or output-stage token.
 * @param resource Exact ArtifactId or output-slot value.
 * @return Versioned SHA-256 reference text.
 * @throws Contract, allocation, or hash failures unchanged.
 */
std::string data_plane_reference(const AttemptIdentity& identity,
                                 const std::string& direction,
                                 const std::string& resource) {
  validate_attempt_identity(identity);
  std::string canonical = "worker-data-v1\n";
  canonical.append(direction);
  canonical.push_back('\n');
  canonical.append(identity.tenant_id.value());
  canonical.push_back('\n');
  canonical.append(identity.job_id.value());
  canonical.push_back('\n');
  canonical.append(identity.job_spec_digest.hex());
  canonical.push_back('\n');
  canonical.append(identity.attempt_id.value());
  canonical.push_back('\n');
  canonical.append(identity.worker_instance_id.value());
  canonical.push_back('\n');
  canonical.append(std::to_string(identity.worker_lease_generation.value));
  canonical.push_back('\n');
  canonical.append(resource);
  const JobSpecDigest digest = hash_job_spec_bytes(
      reinterpret_cast<const std::byte*>(canonical.data()), canonical.size());
  return "worker-data-v1." + digest.hex();
}

/**
 * @brief Validates one tight artifact descriptor without allocation.
 * @param descriptor Candidate descriptor.
 * @return Nothing when dimensions, type, row bytes, and payload agree.
 * @throws std::invalid_argument for invalid shape or checked overflow.
 */
void validate_tight_descriptor(const ArtifactImageDescriptor& descriptor) {
  if (descriptor.width <= 0 || descriptor.height <= 0 ||
      descriptor.channels <= 0) {
    throw std::invalid_argument("worker artifact descriptor is empty");
  }
  const std::size_t width = static_cast<std::size_t>(descriptor.width);
  const std::size_t height = static_cast<std::size_t>(descriptor.height);
  const std::size_t channels = static_cast<std::size_t>(descriptor.channels);
  const std::size_t channel_bytes =
      image_buffer_bytes_per_channel(descriptor.type);
  if (width > std::numeric_limits<std::size_t>::max() / channels ||
      width * channels >
          std::numeric_limits<std::size_t>::max() / channel_bytes) {
    throw std::invalid_argument("worker artifact row size overflowed");
  }
  const std::size_t row_bytes = width * channels * channel_bytes;
  if (row_bytes > std::numeric_limits<std::size_t>::max() / height ||
      descriptor.row_bytes != row_bytes ||
      descriptor.payload_bytes != row_bytes * height) {
    throw std::invalid_argument("worker artifact descriptor is inconsistent");
  }
}

/**
 * @brief Releases one exact anonymous candidate mapping at final owner death.
 * @throws Nothing; `munmap` failure cannot be recovered during destruction.
 * @note `mapped_bytes` is the exact logical candidate length supplied to
 * `mmap`; the kernel may internally round its virtual mapping to page size.
 */
struct AnonymousOutputMappingDeleter final {
  /** @brief Exact byte length originally passed to `mmap`. */
  std::size_t mapped_bytes = 0U;

  /**
   * @brief Releases the retained mapping once.
   * @param mapping Exact mapping base, or null after an empty move.
   * @return Nothing.
   * @throws Nothing.
   */
  void operator()(void* mapping) const noexcept {
    if (mapping != nullptr && mapped_bytes != 0U) {
      static_cast<void>(::munmap(mapping, mapped_bytes));
    }
  }
};

/**
 * @brief Validates one metadata-first candidate before mapping or hydration.
 * @param assignment Exact manager-derived output stage.
 * @param report Metadata-only worker outcome facts.
 * @param output Optional metadata-only candidate reference.
 * @return Nothing when report shape, stage, descriptor, and size join.
 * @throws WorkerArtifactDataPlaneError for any mismatch.
 * @note This check touches no candidate byte and performs no descriptor or
 * filesystem I/O.
 */
void validate_output_report_metadata(
    const WorkerOutputStageReference& assignment,
    const JobAttemptReport& report,
    const std::optional<WorkerOutputDataReference>& output) {
  const bool successful_shape =
      report.outcome == JobAttemptOutcome::Succeeded && report.settled &&
      report.failure == JobAttemptFailure::None;
  if (report.image.has_value() || successful_shape != output.has_value()) {
    throw WorkerArtifactDataPlaneError(
        "worker output metadata does not match its report shape");
  }
  if (!output.has_value()) {
    return;
  }
  if (output->reference_id != assignment.reference_id ||
      output->output_slot_id != assignment.output_slot_id) {
    throw WorkerArtifactDataPlaneError(
        "worker output metadata does not join its assigned stage");
  }
  try {
    validate_tight_descriptor(output->descriptor);
  } catch (const std::exception& error) {
    throw WorkerArtifactDataPlaneError(
        std::string("worker output descriptor is invalid: ") + error.what());
  }
  if (output->descriptor.payload_bytes > assignment.maximum_payload_bytes) {
    throw WorkerArtifactDataPlaneError(
        "worker output-stage size exceeds its assigned stage");
  }
}

/**
 * @brief Creates one lazy pathless exact-length tight CPU mapping.
 * @param descriptor Valid positive tight output descriptor.
 * @return `ImageBuffer` whose logical capacity is exactly payload bytes.
 * @throws WorkerArtifactDataPlaneError when anonymous mapping fails.
 * @throws std::bad_alloc when the shared owner control block cannot allocate.
 * @note `mmap` reserves the contiguous virtual range without a bytewise
 * initialization pass; manager receives later populate it in fixed slices.
 */
ImageBuffer make_anonymous_output_image(
    const ArtifactImageDescriptor& descriptor) {
  void* mapping =
      ::mmap(nullptr, descriptor.payload_bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) {
    throw WorkerArtifactDataPlaneError(
        std::string("worker output anonymous mapping failed: ") +
        std::strerror(errno));
  }
  std::shared_ptr<void> owner(
      mapping, AnonymousOutputMappingDeleter{descriptor.payload_bytes});
  ImageBuffer image;
  image.width = descriptor.width;
  image.height = descriptor.height;
  image.channels = descriptor.channels;
  image.type = descriptor.type;
  image.device = Device::CPU;
  image.step = descriptor.row_bytes;
  image.data = std::move(owner);
  return image;
}

/**
 * @brief Validates typed metadata in one durable artifact receipt.
 * @param receipt Candidate metadata-only checkpoint receipt.
 * @return Nothing when identity, descriptor, digest, and durability are valid.
 * @throws std::invalid_argument for incomplete receipt metadata.
 */
void validate_artifact_receipt_metadata(const OutputCommitReceipt& receipt) {
  validate_attempt_identity(receipt.attempt);
  if (!receipt.output_slot_id.valid() || !receipt.artifact_id.valid() ||
      !receipt.output_commit_id.valid() ||
      receipt.achieved_durability != ArtifactDurability::CrashDurable) {
    throw std::invalid_argument(
        "worker artifact receipt identity or durability is invalid");
  }
  validate_tight_descriptor(receipt.descriptor);
}

/**
 * @brief Builds one exact bounded Assignment metadata contract.
 * @param assignment Valid manager-owned assignment and optional checkpoint.
 * @return Complete deterministic references and output bound.
 * @throws Validation, allocation, and bounded metadata hashing failures.
 * @note Durable checkpoint payload bytes are not copied or hashed here. The
 * killable worker verifies them while receiving the checkpoint stream.
 */
WorkerDataPlaneAssignment make_assignment_metadata(
    const JobAssignment& assignment) {
  validate_attempt_identity(assignment.identity);
  if (assignment.spec == nullptr) {
    throw std::invalid_argument("worker data-plane assignment has no JobSpec");
  }
  validate_job_spec(*assignment.spec);
  if (assignment.spec->digest() != assignment.identity.job_spec_digest) {
    throw std::invalid_argument(
        "worker data-plane JobSpec digest does not join identity");
  }
  const bool checkpoint_declared =
      assignment.spec->checkpoint_artifact_id().has_value();
  if (checkpoint_declared != (assignment.checkpoint != nullptr)) {
    throw std::invalid_argument(
        "worker data-plane checkpoint binding is incomplete");
  }

  WorkerDataPlaneAssignment metadata;
  if (assignment.checkpoint != nullptr) {
    const ArtifactRecord& checkpoint = *assignment.checkpoint;
    validate_artifact_receipt_metadata(checkpoint.receipt);
    if (checkpoint.receipt.attempt.tenant_id != assignment.identity.tenant_id ||
        checkpoint.receipt.artifact_id !=
            *assignment.spec->checkpoint_artifact_id() ||
        checkpoint.receipt.descriptor.payload_bytes !=
            checkpoint.payload.size() ||
        checkpoint.payload.size() >
            resource_size(
                assignment.spec->resource_request().host_memory_bytes)) {
      throw std::invalid_argument(
          "worker data-plane checkpoint does not match durable authority");
    }
    WorkerCheckpointDataReference reference;
    reference.reference_id =
        data_plane_reference(assignment.identity, "checkpoint-read",
                             checkpoint.receipt.artifact_id.value());
    reference.receipt = checkpoint.receipt;
    metadata.checkpoint = std::move(reference);
  }
  metadata.output.reference_id =
      data_plane_reference(assignment.identity, "output-stage",
                           assignment.spec->output_slot_id().value());
  metadata.output.output_slot_id = assignment.spec->output_slot_id();
  metadata.output.maximum_payload_bytes =
      output_payload_maximum(assignment.spec->resource_request());
  validate_worker_data_plane_assignment(assignment.identity, *assignment.spec,
                                        metadata);
  return metadata;
}

}  // namespace

/** @copydoc ps::server::validate_worker_data_plane_assignment */
void validate_worker_data_plane_assignment(
    const AttemptIdentity& identity, const JobSpec& spec,
    const WorkerDataPlaneAssignment& data_plane) {
  validate_attempt_identity(identity);
  validate_job_spec(spec);
  if (spec.digest() != identity.job_spec_digest) {
    throw std::invalid_argument(
        "worker data-plane metadata JobSpec digest is inconsistent");
  }
  const std::string expected_output = data_plane_reference(
      identity, "output-stage", spec.output_slot_id().value());
  const std::size_t expected_maximum =
      output_payload_maximum(spec.resource_request());
  if (data_plane.output.reference_id != expected_output ||
      data_plane.output.reference_id.empty() ||
      data_plane.output.reference_id.size() >
          kMaximumWorkerDataPlaneReferenceBytes ||
      data_plane.output.output_slot_id != spec.output_slot_id() ||
      data_plane.output.maximum_payload_bytes != expected_maximum ||
      expected_maximum == 0U) {
    throw std::invalid_argument("worker output-stage metadata is inconsistent");
  }

  const bool checkpoint_declared = spec.checkpoint_artifact_id().has_value();
  if (checkpoint_declared != data_plane.checkpoint.has_value()) {
    throw std::invalid_argument(
        "worker checkpoint data-plane metadata is incomplete");
  }
  if (!checkpoint_declared) {
    return;
  }
  const WorkerCheckpointDataReference& checkpoint = *data_plane.checkpoint;
  const OutputCommitReceipt& receipt = checkpoint.receipt;
  validate_artifact_receipt_metadata(receipt);
  const std::string expected_reference = data_plane_reference(
      identity, "checkpoint-read", receipt.artifact_id.value());
  if (checkpoint.reference_id != expected_reference ||
      checkpoint.reference_id.empty() ||
      checkpoint.reference_id.size() > kMaximumWorkerDataPlaneReferenceBytes ||
      receipt.attempt.tenant_id != identity.tenant_id ||
      receipt.artifact_id != *spec.checkpoint_artifact_id() ||
      receipt.descriptor.payload_bytes >
          resource_size(spec.resource_request().host_memory_bytes)) {
    throw std::invalid_argument(
        "worker checkpoint data-plane metadata is inconsistent");
  }
}

/** @copydoc ps::server::WorkerArtifactDataPlane::WorkerArtifactDataPlane */
WorkerArtifactDataPlane::WorkerArtifactDataPlane(
    int checkpoint_descriptor, int checkpoint_sender_descriptor,
    int output_descriptor, int output_reader_descriptor,
    WorkerDataPlaneAssignment metadata) noexcept
    : worker_checkpoint_descriptor_(checkpoint_descriptor),
      manager_checkpoint_descriptor_(checkpoint_sender_descriptor),
      worker_output_descriptor_(output_descriptor),
      manager_output_descriptor_(output_reader_descriptor),
      assignment_metadata_(std::move(metadata)) {
}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ps::server::WorkerArtifactDataPlane::create */
WorkerArtifactDataPlane WorkerArtifactDataPlane::create(
    const JobAssignment& assignment) {
  WorkerDataPlaneAssignment metadata = make_assignment_metadata(assignment);
  StreamLane checkpoint = create_checkpoint_lane();
  StreamLane output = create_output_lane();
  return WorkerArtifactDataPlane(
      checkpoint.worker.release(), checkpoint.manager.release(),
      output.worker.release(), output.manager.release(), std::move(metadata));
}

/** @copydoc ps::server::WorkerArtifactDataPlane::~WorkerArtifactDataPlane */
WorkerArtifactDataPlane::~WorkerArtifactDataPlane() noexcept {
  close_worker_descriptors();
  close_manager_checkpoint_descriptor();
  close_manager_output_descriptor();
}

/** @copydoc ps::server::WorkerArtifactDataPlane::WorkerArtifactDataPlane */
WorkerArtifactDataPlane::WorkerArtifactDataPlane(
    WorkerArtifactDataPlane&& other) noexcept
    : worker_checkpoint_descriptor_(
          std::exchange(other.worker_checkpoint_descriptor_, -1)),
      manager_checkpoint_descriptor_(
          std::exchange(other.manager_checkpoint_descriptor_, -1)),
      worker_output_descriptor_(
          std::exchange(other.worker_output_descriptor_, -1)),
      manager_output_descriptor_(
          std::exchange(other.manager_output_descriptor_, -1)),
      assignment_metadata_(std::move(other.assignment_metadata_)) {
}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ps::server::WorkerArtifactDataPlane::operator= */
WorkerArtifactDataPlane& WorkerArtifactDataPlane::operator=(
    WorkerArtifactDataPlane&& other) noexcept {
  if (this != &other) {
    close_worker_descriptors();
    close_manager_checkpoint_descriptor();
    close_manager_output_descriptor();
    worker_checkpoint_descriptor_ =
        std::exchange(other.worker_checkpoint_descriptor_, -1);
    manager_checkpoint_descriptor_ =
        std::exchange(other.manager_checkpoint_descriptor_, -1);
    worker_output_descriptor_ =
        std::exchange(other.worker_output_descriptor_, -1);
    manager_output_descriptor_ =
        std::exchange(other.manager_output_descriptor_, -1);
    assignment_metadata_ = std::move(other.assignment_metadata_);
  }
  return *this;
}

/** @copydoc ps::server::WorkerArtifactDataPlane::close_worker_descriptors */
void WorkerArtifactDataPlane::close_worker_descriptors() noexcept {
  close_once(&worker_checkpoint_descriptor_);
  close_once(&worker_output_descriptor_);
}

/** @copydoc ps::server::WorkerArtifactDataPlane::send_checkpoint_chunk */
WorkerDataPlaneIoStatus WorkerArtifactDataPlane::send_checkpoint_chunk(
    const std::vector<std::byte>& payload, std::size_t* offset) {
  if (offset == nullptr || *offset > payload.size()) {
    throw std::invalid_argument("worker checkpoint stream offset is invalid");
  }
  if (*offset == payload.size()) {
    return WorkerDataPlaneIoStatus::Progress;
  }
  const std::size_t size =
      std::min(kDataPlaneChunkBytes, payload.size() - *offset);
#ifdef MSG_NOSIGNAL
  constexpr int kSendFlags = MSG_NOSIGNAL;
#else
  constexpr int kSendFlags = 0;
#endif
  const ssize_t sent = ::send(manager_checkpoint_descriptor_,
                              payload.data() + *offset, size, kSendFlags);
  if (sent > 0) {
    *offset += static_cast<std::size_t>(sent);
    return WorkerDataPlaneIoStatus::Progress;
  }
  if (sent == 0 || errno == EPIPE || errno == ECONNRESET) {
    return WorkerDataPlaneIoStatus::EndOfStream;
  }
  if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
    return WorkerDataPlaneIoStatus::WouldBlock;
  }
  throw std::system_error(errno, std::generic_category(),
                          "send manager checkpoint stream chunk");
}

/** @copydoc
 * ps::server::WorkerArtifactDataPlane::close_manager_checkpoint_descriptor */
void WorkerArtifactDataPlane::close_manager_checkpoint_descriptor() noexcept {
  close_once(&manager_checkpoint_descriptor_);
}

/** @copydoc ps::server::WorkerArtifactDataPlane::receive_output_chunk */
WorkerDataPlaneIoStatus WorkerArtifactDataPlane::receive_output_chunk(
    std::byte* payload, std::size_t payload_size, std::size_t* offset) {
  if (offset == nullptr || *offset > payload_size ||
      (payload_size != 0U && payload == nullptr)) {
    throw std::invalid_argument("worker output stream destination is invalid");
  }
  if (payload_size > assignment_metadata_.output.maximum_payload_bytes) {
    throw WorkerArtifactDataPlaneError(
        "worker output destination exceeds its assigned stage");
  }
  std::byte excess{};
  const bool checking_commit = *offset == payload_size;
  std::byte* destination = checking_commit ? &excess : payload + *offset;
  const std::size_t requested =
      checking_commit ? 1U
                      : std::min(kDataPlaneChunkBytes, payload_size - *offset);
  const ssize_t received =
      ::recv(manager_output_descriptor_, destination, requested, 0);
  if (received > 0) {
    if (checking_commit) {
      throw WorkerArtifactDataPlaneError(
          "worker output stream exceeds its assigned stage");
    }
    *offset += static_cast<std::size_t>(received);
    return WorkerDataPlaneIoStatus::Progress;
  }
  if (received == 0) {
    if (*offset != payload_size) {
      throw WorkerArtifactDataPlaneError(
          "worker output-stage size exceeds or differs from metadata");
    }
    return WorkerDataPlaneIoStatus::EndOfStream;
  }
  if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
    return WorkerDataPlaneIoStatus::WouldBlock;
  }
  throw std::system_error(errno, std::generic_category(),
                          "receive manager output stream chunk");
}

/** @copydoc
 * ps::server::WorkerArtifactDataPlane::close_manager_output_descriptor */
void WorkerArtifactDataPlane::close_manager_output_descriptor() noexcept {
  close_once(&manager_output_descriptor_);
}

/** @copydoc ps::server::WorkerArtifactDataPlane::prepare_output_image */
std::optional<ImageBuffer> WorkerArtifactDataPlane::prepare_output_image(
    const JobAttemptReport& report,
    const std::optional<WorkerOutputDataReference>& output) const {
  validate_output_report_metadata(assignment_metadata_.output, report, output);
  if (!output.has_value()) {
    return std::nullopt;
  }
  return make_anonymous_output_image(output->descriptor);
}

/** @copydoc ps::server::WorkerArtifactDataPlane::materialize_report */
JobAttemptReport WorkerArtifactDataPlane::materialize_report(
    JobAttemptReport report,
    const std::optional<WorkerOutputDataReference>& output,
    std::optional<ImageBuffer> image, std::size_t payload_size,
    const ArtifactContentDigest& payload_digest) const {
  validate_output_report_metadata(assignment_metadata_.output, report, output);
  if (!output.has_value()) {
    if (image.has_value() || payload_size != 0U) {
      throw WorkerArtifactDataPlaneError(
          "image-free worker report sent output-stage bytes");
    }
    return report;
  }
  if (!image.has_value()) {
    throw WorkerArtifactDataPlaneError(
        "worker output-stage final owner is missing");
  }
  try {
    validate_image_buffer(*image);
  } catch (const std::exception& error) {
    throw WorkerArtifactDataPlaneError(
        std::string("worker output final image is invalid: ") + error.what());
  }
  if (image->width != output->descriptor.width ||
      image->height != output->descriptor.height ||
      image->channels != output->descriptor.channels ||
      image->type != output->descriptor.type || image->device != Device::CPU ||
      image->step != output->descriptor.row_bytes ||
      output->descriptor.payload_bytes != payload_size) {
    throw WorkerArtifactDataPlaneError(
        "worker output-stage size exceeds or differs from metadata");
  }
  if (payload_digest != output->content_digest) {
    throw WorkerArtifactDataPlaneError(
        "worker output-stage content digest is inconsistent");
  }
  report.image = std::move(*image);
  return report;
}

/** @copydoc ps::server::materialize_worker_checkpoint */
std::shared_ptr<const ArtifactRecord> materialize_worker_checkpoint(
    int checkpoint_descriptor, const JobAssignment& assignment,
    const WorkerDataPlaneAssignment& data_plane) {
  if (assignment.checkpoint != nullptr || assignment.spec == nullptr) {
    throw WorkerArtifactDataPlaneError(
        "worker checkpoint materialization state is invalid");
  }
  try {
    validate_worker_data_plane_assignment(assignment.identity, *assignment.spec,
                                          data_plane);
  } catch (const std::exception& error) {
    throw WorkerArtifactDataPlaneError(
        std::string("worker checkpoint metadata is invalid: ") + error.what());
  }
  require_stream_socket(checkpoint_descriptor);
  const std::size_t expected_size =
      data_plane.checkpoint.has_value()
          ? data_plane.checkpoint->receipt.descriptor.payload_bytes
          : 0U;
  std::vector<std::byte> payload =
      receive_exact_checkpoint(checkpoint_descriptor, expected_size);
  if (!data_plane.checkpoint.has_value()) {
    return nullptr;
  }
  const OutputCommitReceipt& receipt = data_plane.checkpoint->receipt;
  if (hash_artifact_content(payload.data(), payload.size()) !=
      receipt.content_digest) {
    throw WorkerArtifactDataPlaneError(
        "worker checkpoint content digest is inconsistent");
  }
  ArtifactRecord record;
  record.receipt = receipt;
  record.payload = std::move(payload);
  return std::make_shared<const ArtifactRecord>(std::move(record));
}

/** @copydoc ps::server::prepare_worker_output_transfer */
PreparedWorkerOutputTransfer prepare_worker_output_transfer(
    const JobSpec& spec, const WorkerOutputStageReference& output_stage,
    JobAttemptReport* report) {
  if (report == nullptr) {
    throw std::invalid_argument("worker output report is null");
  }
  validate_attempt_identity(report->identity);
  validate_job_spec(spec);
  const std::string expected_reference = data_plane_reference(
      report->identity, "output-stage", spec.output_slot_id().value());
  if (report->identity.job_spec_digest != spec.digest() ||
      output_stage.reference_id != expected_reference ||
      output_stage.output_slot_id != spec.output_slot_id() ||
      output_stage.maximum_payload_bytes !=
          output_payload_maximum(spec.resource_request())) {
    throw std::invalid_argument("worker output stage does not join assignment");
  }
  if (!report->image.has_value()) {
    return {};
  }
  if (report->outcome != JobAttemptOutcome::Succeeded || !report->settled ||
      report->failure != JobAttemptFailure::None) {
    throw std::invalid_argument(
        "only a settled successful report may stage an image");
  }
  const ImageBuffer& image = *report->image;
  validate_image_buffer(image);
  if (image.device != Device::CPU || image.width <= 0 || image.height <= 0 ||
      image.channels <= 0 || image.data == nullptr) {
    throw std::invalid_argument("worker output image is not nonempty CPU data");
  }
  const std::size_t row_bytes = image_buffer_row_bytes(image);
  if (row_bytes > std::numeric_limits<std::size_t>::max() /
                      static_cast<std::size_t>(image.height)) {
    throw std::overflow_error("worker output image size overflowed");
  }
  const std::size_t payload_bytes =
      row_bytes * static_cast<std::size_t>(image.height);
  if (payload_bytes > output_stage.maximum_payload_bytes) {
    report->outcome = JobAttemptOutcome::Failed;
    report->settled = true;
    report->failure = JobAttemptFailure::Compute;
    report->message =
        "worker candidate image exceeds accepted artifact data-plane bounds";
    report->image.reset();
    return {};
  }

  WorkerOutputDataReference output;
  output.reference_id = output_stage.reference_id;
  output.output_slot_id = output_stage.output_slot_id;
  output.descriptor.width = image.width;
  output.descriptor.height = image.height;
  output.descriptor.channels = image.channels;
  output.descriptor.type = image.type;
  output.descriptor.row_bytes = row_bytes;
  output.descriptor.payload_bytes = payload_bytes;
  output.content_digest = hash_image_artifact_content(image);
  PreparedWorkerOutputTransfer transfer;
  transfer.reference = std::move(output);
  transfer.source = image;
  report->image.reset();
  return transfer;
}

/** @copydoc ps::server::send_worker_output_transfer */
void send_worker_output_transfer(int output_descriptor,
                                 const PreparedWorkerOutputTransfer& transfer) {
  require_stream_socket(output_descriptor);
  if (transfer.reference.has_value() != transfer.source.has_value()) {
    throw std::invalid_argument("worker output transfer is incomplete");
  }
  if (!transfer.reference.has_value()) {
    return;
  }
  const WorkerOutputDataReference& output = *transfer.reference;
  const ImageBuffer& image = *transfer.source;
  validate_image_buffer(image);
  if (image.device != Device::CPU || image.width != output.descriptor.width ||
      image.height != output.descriptor.height ||
      image.channels != output.descriptor.channels ||
      image.type != output.descriptor.type ||
      image_buffer_row_bytes(image) != output.descriptor.row_bytes ||
      output.descriptor.row_bytes >
          std::numeric_limits<std::size_t>::max() /
              static_cast<std::size_t>(image.height) ||
      output.descriptor.row_bytes * static_cast<std::size_t>(image.height) !=
          output.descriptor.payload_bytes) {
    throw std::invalid_argument("worker output transfer source is invalid");
  }
  for (int row = 0; row < image.height; ++row) {
    send_complete_from_worker(output_descriptor,
                              image_buffer_row_data(image, row),
                              output.descriptor.row_bytes);
  }
}

}  // namespace ps::server
