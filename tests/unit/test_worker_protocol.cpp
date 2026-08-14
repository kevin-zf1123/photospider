/**
 * @file test_worker_protocol.cpp
 * @brief Verifies Issue #105 metadata control and artifact data separation.
 */
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "server/worker_process_launch.hpp"  // NOLINT(build/include_subdir)
#include "server/worker_protocol.hpp"        // NOLINT(build/include_subdir)
#include "server/worker_protocol_test_access.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {
namespace {

TEST(WorkerProcessLaunch, RoundTripsManagerDeadlinesWithoutLegacyCaps) {
  const WorkerProcessLaunchOptions expected{3, 5, 6,
                                            std::chrono::milliseconds(12345),
                                            std::chrono::milliseconds(4321)};
  WorkerProcessLaunchArguments arguments =
      make_worker_process_launch_arguments(expected);
  std::string executable = "photospider-worker";
  std::array<char*, 6U> argv{executable.data(),
                             arguments.control_fd.data(),
                             arguments.checkpoint_data_fd.data(),
                             arguments.output_data_fd.data(),
                             arguments.startup_timeout.data(),
                             arguments.io_timeout.data()};

  const WorkerProcessLaunchOptions parsed = parse_worker_process_launch_options(
      static_cast<int>(argv.size()), argv.data());

  EXPECT_EQ(parsed.control_fd, expected.control_fd);
  EXPECT_EQ(parsed.checkpoint_data_fd, expected.checkpoint_data_fd);
  EXPECT_EQ(parsed.output_data_fd, expected.output_data_fd);
  EXPECT_EQ(parsed.startup_timeout, expected.startup_timeout);
  EXPECT_EQ(parsed.io_timeout, expected.io_timeout);
}

TEST(WorkerProcessLaunch,
     RejectsMissingReorderedMalformedOrNonPositiveArguments) {
  std::string executable = "photospider-worker";
  std::string control = "--control-fd=3";
  std::string checkpoint = "--checkpoint-data-fd=5";
  std::string output = "--output-data-fd=6";
  std::string startup = "--startup-timeout-ms=12000";
  std::string io = "--io-timeout-ms=3500";
  std::array<char*, 6U> complete{executable.data(), control.data(),
                                 checkpoint.data(), output.data(),
                                 startup.data(),    io.data()};

  EXPECT_THROW(parse_worker_process_launch_options(2, complete.data()),
               std::invalid_argument);
  std::swap(complete[4], complete[5]);
  EXPECT_THROW(parse_worker_process_launch_options(
                   static_cast<int>(complete.size()), complete.data()),
               std::invalid_argument);
  std::swap(complete[4], complete[5]);
  startup = "--startup-timeout-ms=0";
  EXPECT_THROW(parse_worker_process_launch_options(
                   static_cast<int>(complete.size()), complete.data()),
               std::invalid_argument);
  startup = "--startup-timeout-ms=12000";
  io = "--io-timeout-ms=3500ms";
  EXPECT_THROW(parse_worker_process_launch_options(
                   static_cast<int>(complete.size()), complete.data()),
               std::invalid_argument);
  io = "--io-timeout-ms=3500";
  output = "--output-data-fd=5";
  EXPECT_THROW(parse_worker_process_launch_options(
                   static_cast<int>(complete.size()), complete.data()),
               std::invalid_argument);
}

/**
 * @brief Owns one connected local socket pair for protocol unit tests.
 * @throws std::system_error when `socketpair` fails.
 */
class ScopedSocketPair final {
 public:
  /** @brief Creates one connected AF_UNIX stream pair. */
  ScopedSocketPair() {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors_.data()) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "create worker protocol test socketpair");
    }
  }

  /** @brief Closes both exact descriptors. */
  ~ScopedSocketPair() noexcept {
    for (const int descriptor : descriptors_) {
      if (descriptor >= 0) {
        static_cast<void>(::close(descriptor));
      }
    }
  }

  /** @brief Prevents duplicate descriptor ownership. */
  ScopedSocketPair(const ScopedSocketPair& other) = delete;
  /** @brief Prevents duplicate descriptor assignment. */
  ScopedSocketPair& operator=(const ScopedSocketPair& other) = delete;

  /**
   * @brief Returns one endpoint descriptor.
   * @param index Zero or one.
   * @return Borrowed descriptor.
   * @throws std::out_of_range for another index.
   */
  int at(std::size_t index) const { return descriptors_.at(index); }

 private:
  /** @brief Sole two descriptor owners. */
  std::array<int, 2U> descriptors_{{-1, -1}};
};

/**
 * @brief Builds one complete bounded Job resource request.
 * @return Canonical CPU/host/output/staging/retention envelope.
 * @throws Nothing.
 */
JobResourceRequest protocol_resources() {
  JobResourceRequest resources;
  resources.cpu_slots = 2U;
  resources.host_memory_bytes = 1ULL << 30U;
  resources.output_bytes = 1U << 20U;
  resources.staging_bytes = 1U << 20U;
  resources.retention_bytes = 1U << 20U;
  return resources;
}

/**
 * @brief Builds resources large enough to isolate the aggregate frame bound.
 * @return Valid CPU/host envelope whose image-byte limits exceed one frame.
 * @throws Nothing.
 */
JobResourceRequest frame_bound_resources() {
  JobResourceRequest resources = protocol_resources();
  resources.output_bytes = 128U << 20U;
  resources.staging_bytes = 128U << 20U;
  resources.retention_bytes = 128U << 20U;
  return resources;
}

/**
 * @brief Builds one exact maximum-length valid opaque field.
 * @param prefix Nonempty valid opaque prefix shorter than the field maximum.
 * @param padding Valid alphanumeric byte used to fill the remaining width.
 * @return Exact `kMaximumOpaqueIdentityBytes`-byte token.
 * @throws std::invalid_argument for an unusable prefix or padding byte.
 * @throws std::bad_alloc when allocating the field exhausts memory.
 */
std::string maximum_opaque_field(std::string prefix, char padding) {
  const bool padding_valid = (padding >= 'a' && padding <= 'z') ||
                             (padding >= 'A' && padding <= 'Z') ||
                             (padding >= '0' && padding <= '9');
  if (prefix.empty() || prefix.size() >= kMaximumOpaqueIdentityBytes ||
      !padding_valid) {
    throw std::invalid_argument("maximum opaque test field is invalid");
  }
  prefix.append(kMaximumOpaqueIdentityBytes - prefix.size(), padding);
  return prefix;
}

/**
 * @brief Builds the largest valid encoded resource/device metadata shape.
 * @return Positive resources with 128 sorted maximum-length device labels.
 * @throws Contract or allocation failures unchanged.
 */
JobResourceRequest maximum_assignment_resources() {
  JobResourceRequest resources = frame_bound_resources();
  resources.devices.reserve(kMaximumConfiguredDevicesPerJob);
  for (std::size_t index = 0U; index < kMaximumConfiguredDevicesPerJob;
       ++index) {
    const std::string decimal = std::to_string(index);
    const std::string prefix =
        "device-" + std::string(3U - decimal.size(), '0') + decimal;
    resources.devices.push_back(
        DeviceResourceRequest{maximum_opaque_field(prefix, 'x'),
                              static_cast<std::uint64_t>(index + 1U)});
  }
  return resources;
}

/**
 * @brief Builds one valid identity joined to a supplied immutable JobSpec.
 * @param spec Exact JobSpec whose digest enters the tuple.
 * @return Complete valid attempt identity.
 * @throws Identity allocation failures unchanged.
 */
AttemptIdentity protocol_identity(const JobSpec& spec);

/**
 * @brief Builds one maximum-encoded-width identity joined to a JobSpec.
 * @param spec Exact JobSpec whose digest enters the tuple.
 * @param domain Distinct valid byte used to keep identity fields readable.
 * @return Complete valid maximum-field attempt identity.
 * @throws Contract or allocation failures unchanged.
 */
AttemptIdentity maximum_assignment_identity(const JobSpec& spec, char domain) {
  AttemptIdentity identity;
  identity.tenant_id = TenantId(maximum_opaque_field("tenant-", domain));
  identity.job_id = JobId(maximum_opaque_field("job-", domain));
  identity.job_spec_digest = spec.digest();
  identity.attempt_id = JobAttemptId(maximum_opaque_field("attempt-", domain));
  identity.worker_instance_id =
      WorkerInstanceId(maximum_opaque_field("worker-", domain));
  identity.worker_lease_generation =
      WorkerLeaseGeneration{std::numeric_limits<std::uint64_t>::max()};
  return identity;
}

/**
 * @brief Builds a valid checkpoint with caller-selected tight payload bytes.
 * @param spec JobSpec that names the checkpoint ArtifactId.
 * @param payload_bytes Positive payload width no larger than `INT_MAX`.
 * @return Complete digest-consistent crash-durable artifact record.
 * @throws Contract, allocation, or hashing failures unchanged.
 */
ArtifactRecord maximum_assignment_checkpoint(const JobSpec& spec,
                                             std::size_t payload_bytes) {
  if (!spec.checkpoint_artifact_id().has_value() || payload_bytes == 0U ||
      payload_bytes >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("maximum checkpoint test payload is invalid");
  }
  ArtifactRecord artifact;
  artifact.payload.assign(payload_bytes, std::byte{0x5a});
  artifact.receipt.attempt = maximum_assignment_identity(spec, 'r');
  artifact.receipt.attempt.tenant_id = TenantId("tenant.protocol");
  artifact.receipt.output_slot_id =
      OutputSlotId(maximum_opaque_field("checkpoint-slot-", 's'));
  artifact.receipt.artifact_id = *spec.checkpoint_artifact_id();
  artifact.receipt.output_commit_id =
      OutputCommitId(maximum_opaque_field("checkpoint-commit-", 'c'));
  artifact.receipt.descriptor.width = static_cast<int>(payload_bytes);
  artifact.receipt.descriptor.height = 1;
  artifact.receipt.descriptor.channels = 1;
  artifact.receipt.descriptor.type = DataType::UINT8;
  artifact.receipt.descriptor.row_bytes = payload_bytes;
  artifact.receipt.descriptor.payload_bytes = payload_bytes;
  artifact.receipt.content_digest =
      hash_artifact_content(artifact.payload.data(), artifact.payload.size());
  artifact.receipt.achieved_durability = ArtifactDurability::CrashDurable;
  return artifact;
}

/**
 * @brief Builds the maximum declared Assignment metadata with one-byte bulk.
 * @return Complete Assignment whose every bounded text/resource/receipt field
 * reaches its declared maximum while artifact bytes remain external.
 * @throws Contract, stream-setup, hash, or allocation failures unchanged.
 */
PreparedWorkerAssignment maximum_metadata_assignment() {
  const ArtifactId checkpoint_id(
      maximum_opaque_field("checkpoint-artifact-", 'k'));
  auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId(maximum_opaque_field("graph-", 'g')),
      std::numeric_limits<int>::max(),
      OutputSlotId(maximum_opaque_field("output-", 'o')),
      maximum_assignment_resources(), checkpoint_id);
  PreparedWorkerAssignment prepared;
  prepared.assignment.identity = maximum_assignment_identity(*spec, 'a');
  prepared.assignment.spec = spec;
  ArtifactRecord checkpoint = maximum_assignment_checkpoint(*spec, 1U);
  checkpoint.receipt.attempt.tenant_id = prepared.assignment.identity.tenant_id;
  prepared.assignment.checkpoint =
      std::make_shared<const ArtifactRecord>(std::move(checkpoint));
  WorkerArtifactDataPlane data_plane =
      WorkerArtifactDataPlane::create(prepared.assignment);
  prepared.data_plane = data_plane.assignment_metadata();
  prepared.graph.ok = true;
  prepared.graph.root_dir.assign(kMaximumWorkerTextFieldBytes, 'r');
  prepared.graph.yaml_path.assign(kMaximumWorkerTextFieldBytes, 'y');
  prepared.graph.config_path.assign(kMaximumWorkerTextFieldBytes, 'c');
  prepared.graph.cache_root_dir.assign(kMaximumWorkerTextFieldBytes, 'h');
  prepared.graph.message.assign(kMaximumWorkerTextFieldBytes, 'm');
  prepared.heartbeat_interval =
      std::chrono::milliseconds(std::numeric_limits<std::uint32_t>::max());
  return prepared;
}

/**
 * @brief Builds one valid manager Assignment and its local data-plane owner.
 * @param spec Immutable JobSpec retained by the assignment.
 * @param checkpoint Optional authorized checkpoint retained by the assignment.
 * @return Pair containing prepared control metadata and live descriptor owner.
 * @throws Data-plane, contract, socket, hash, or allocation failures unchanged.
 * @note The returned owner must remain alive while its metadata or directional
 * stream lane is used by a test.
 */
std::pair<PreparedWorkerAssignment, WorkerArtifactDataPlane>
prepared_assignment_with_data_plane(
    std::shared_ptr<const JobSpec> spec,
    std::shared_ptr<const ArtifactRecord> checkpoint = nullptr) {
  PreparedWorkerAssignment prepared;
  prepared.assignment.identity = protocol_identity(*spec);
  prepared.assignment.spec = std::move(spec);
  prepared.assignment.checkpoint = std::move(checkpoint);
  WorkerArtifactDataPlane data_plane =
      WorkerArtifactDataPlane::create(prepared.assignment);
  prepared.data_plane = data_plane.assignment_metadata();
  prepared.graph.ok = true;
  prepared.graph.yaml_path = "/trusted/graph.yaml";
  prepared.heartbeat_interval = std::chrono::milliseconds(125);
  return {std::move(prepared), std::move(data_plane)};
}

/**
 * @brief Builds one identity joined to a supplied immutable JobSpec.
 * @param spec Exact JobSpec whose digest enters the tuple.
 * @return Complete valid attempt identity.
 * @throws Identity allocation failures unchanged.
 */
AttemptIdentity protocol_identity(const JobSpec& spec) {
  AttemptIdentity identity;
  identity.tenant_id = TenantId("tenant.protocol");
  identity.job_id = JobId("job.protocol");
  identity.job_spec_digest = spec.digest();
  identity.attempt_id = JobAttemptId("attempt.protocol.1");
  identity.worker_instance_id = WorkerInstanceId("worker.protocol.1");
  identity.worker_lease_generation = WorkerLeaseGeneration{1U};
  return identity;
}

/**
 * @brief Builds one settled success report with a tight one-row CPU image.
 * @param identity Exact report identity.
 * @param diagnostic Variable bounded success diagnostic.
 * @param payload_bytes Positive one-row image payload size.
 * @return Complete successful candidate report.
 * @throws Image, string, or allocation failures unchanged.
 */
JobAttemptReport frame_bound_success_report(AttemptIdentity identity,
                                            std::string diagnostic,
                                            std::size_t payload_bytes) {
  if (payload_bytes == 0U ||
      payload_bytes >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("protocol boundary payload width is invalid");
  }
  JobAttemptReport report;
  report.identity = std::move(identity);
  report.outcome = JobAttemptOutcome::Succeeded;
  report.settled = true;
  report.failure = JobAttemptFailure::None;
  report.message = std::move(diagnostic);
  report.image = make_aligned_cpu_image_buffer(static_cast<int>(payload_bytes),
                                               1, 1, DataType::UINT8, 64U);
  return report;
}

/**
 * @brief Returns a near-term absolute protocol test deadline.
 * @return Current monotonic time plus two seconds.
 * @throws Nothing.
 */
std::chrono::steady_clock::time_point protocol_deadline() noexcept {
  return std::chrono::steady_clock::now() + std::chrono::seconds(2);
}

/**
 * @brief Deterministic monotonic time advanced at one protocol boundary.
 * @throws Nothing for aggregate initialization and value operations.
 */
struct ProtocolDeadlineHookState final {
  /** @brief Current synthetic monotonic tick count. */
  std::atomic<std::chrono::steady_clock::rep> now_ticks{0};
  /** @brief Exact synthetic deadline installed by the test. */
  std::chrono::steady_clock::time_point deadline;
  /** @brief Boundary that advances `now_ticks` exactly to `deadline`. */
  WorkerProtocolDeadlineTestPoint crossing_point =
      WorkerProtocolDeadlineTestPoint::FrameReadyBeforeAcceptance;
};

/**
 * @brief Returns one test's atomically controlled monotonic time.
 * @param context Non-null `ProtocolDeadlineHookState`.
 * @return Synthetic time point retained by the state.
 * @throws Nothing.
 */
std::chrono::steady_clock::time_point protocol_test_now(
    void* context) noexcept {
  const auto* state = static_cast<const ProtocolDeadlineHookState*>(context);
  return std::chrono::steady_clock::time_point{
      std::chrono::steady_clock::duration{
          state->now_ticks.load(std::memory_order_acquire)}};
}

/**
 * @brief Advances synthetic time exactly when the selected boundary is seen.
 * @param context Non-null `ProtocolDeadlineHookState`.
 * @param point Exact implementation boundary reached by the protocol.
 * @return Nothing.
 * @throws Nothing.
 */
void cross_protocol_test_deadline(
    void* context, WorkerProtocolDeadlineTestPoint point) noexcept {
  auto* state = static_cast<ProtocolDeadlineHookState*>(context);
  if (point == state->crossing_point) {
    state->now_ticks.store(state->deadline.time_since_epoch().count(),
                           std::memory_order_release);
  }
}

/**
 * @brief Attempts one stream send without permitting process-wide SIGPIPE.
 * @param fd Connected direction-reduced AF_UNIX stream endpoint.
 * @param byte Exact single byte to send.
 * @return Raw `send` result with `errno` available to the caller.
 * @throws Nothing.
 * @note Linux uses `MSG_NOSIGNAL`; Darwin relies on per-endpoint
 * `SO_NOSIGPIPE` installed by `WorkerArtifactDataPlane::create`. The helper
 * never changes the process-wide signal disposition.
 */
ssize_t send_direction_probe(int fd, const std::byte& byte) noexcept {
#ifdef MSG_NOSIGNAL
  constexpr int kSendFlags = MSG_NOSIGNAL;
#else
  constexpr int kSendFlags = 0;
#endif
  return ::send(fd, &byte, sizeof(byte), kSendFlags);
}

/**
 * @brief Reports whether one platform error denotes a disabled write half.
 * @param error Positive errno captured immediately after `send`.
 * @return True for the closed cross-Darwin/Linux stream error contract.
 * @throws Nothing.
 * @note Darwin normally reports `EPIPE`; Linux may additionally expose
 * `ECONNRESET` or `ENOTCONN` after peer-side stream state transitions.
 */
bool is_direction_rejection_error(int error) noexcept {
  return error == EPIPE || error == ECONNRESET || error == ENOTCONN;
}

/**
 * @brief Result of one concurrently staged unit-test candidate.
 * @throws Nothing for default construction; retained values may allocate.
 */
struct CollectedWorkerOutput final {
  /** @brief Metadata emitted by the worker-side staging operation. */
  std::optional<WorkerOutputDataReference> reference;
  /** @brief Exact final manager image populated directly by bounded receives.
   */
  std::optional<ImageBuffer> image;
  /** @brief Exact bytes written directly into `image`. */
  std::size_t received_bytes = 0U;
  /** @brief Independently accumulated digest of the drained bytes. */
  ArtifactContentDigest digest;
};

/**
 * @brief Stages worker output while concurrently draining its bounded lane.
 * @param data_plane Non-null exact two-ended test owner.
 * @param spec Immutable assignment JobSpec.
 * @param output_stage Exact manager-derived output stage metadata.
 * @param report Non-null report mutated by worker-side staging.
 * @return Candidate metadata plus exact manager-received bytes.
 * @throws Worker staging, stream, deadline, allocation, or thread failures
 * unchanged.
 * @note The sender thread represents only the killable worker side for this
 * unit boundary; production lifecycle coverage uses a real execed fixture.
 */
CollectedWorkerOutput stage_and_collect_output(
    WorkerArtifactDataPlane* data_plane, const JobSpec& spec,
    const WorkerOutputStageReference& output_stage, JobAttemptReport* report) {
  if (data_plane == nullptr || report == nullptr) {
    throw std::invalid_argument("worker output test state is incomplete");
  }
  CollectedWorkerOutput collected;
  PreparedWorkerOutputTransfer transfer =
      prepare_worker_output_transfer(spec, output_stage, report);
  collected.reference = transfer.reference;
  collected.image =
      data_plane->prepare_output_image(*report, collected.reference);
  const std::size_t expected_bytes =
      collected.reference.has_value()
          ? collected.reference->descriptor.payload_bytes
          : 0U;
  ArtifactContentHasher hasher;
  std::exception_ptr sender_failure;
  std::thread sender([&] {
    try {
      send_worker_output_transfer(data_plane->worker_output_descriptor(),
                                  transfer);
      data_plane->close_worker_descriptors();
    } catch (...) {
      sender_failure = std::current_exception();
      data_plane->close_worker_descriptors();
    }
  });
  try {
    bool eof = false;
    const auto deadline = protocol_deadline();
    while (!eof) {
      const std::size_t prior_size = collected.received_bytes;
      std::byte* destination =
          collected.image.has_value()
              ? static_cast<std::byte*>(collected.image->data.get())
              : nullptr;
      const WorkerDataPlaneIoStatus status = data_plane->receive_output_chunk(
          destination, expected_bytes, &collected.received_bytes);
      if (status == WorkerDataPlaneIoStatus::Progress) {
        hasher.update(destination + prior_size,
                      collected.received_bytes - prior_size);
      }
      if (status == WorkerDataPlaneIoStatus::EndOfStream) {
        eof = true;
        collected.digest = hasher.finish();
        data_plane->close_manager_output_descriptor();
      } else if (status == WorkerDataPlaneIoStatus::WouldBlock) {
        if (std::chrono::steady_clock::now() >= deadline) {
          throw std::runtime_error("worker output test drain timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  } catch (...) {
    data_plane->close_manager_output_descriptor();
    sender.join();
    throw;
  }
  sender.join();
  if (sender_failure != nullptr) {
    std::rethrow_exception(sender_failure);
  }
  return collected;
}

/**
 * @brief Transfers and materializes one checkpoint across the stream pair.
 * @param data_plane Non-null exact two-ended test owner.
 * @param assignment Metadata-only worker assignment to hydrate.
 * @param metadata Exact decoded checkpoint/output references.
 * @param payload Trusted manager-side checkpoint bytes.
 * @return Null for no checkpoint, otherwise the validated independent record.
 * @throws Stream, validation, allocation, deadline, or thread failures
 * unchanged.
 */
std::shared_ptr<const ArtifactRecord> transfer_checkpoint_for_test(
    WorkerArtifactDataPlane* data_plane, const JobAssignment& assignment,
    const WorkerDataPlaneAssignment& metadata,
    const std::vector<std::byte>& payload) {
  if (data_plane == nullptr) {
    throw std::invalid_argument("worker checkpoint test plane is null");
  }
  std::exception_ptr sender_failure;
  std::thread sender([&] {
    try {
      std::size_t offset = 0U;
      const auto deadline = protocol_deadline();
      while (offset != payload.size()) {
        const WorkerDataPlaneIoStatus status =
            data_plane->send_checkpoint_chunk(payload, &offset);
        if (status == WorkerDataPlaneIoStatus::EndOfStream) {
          throw std::runtime_error("worker checkpoint test peer closed");
        }
        if (status == WorkerDataPlaneIoStatus::WouldBlock) {
          if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("worker checkpoint test send timed out");
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      data_plane->close_manager_checkpoint_descriptor();
    } catch (...) {
      sender_failure = std::current_exception();
      data_plane->close_manager_checkpoint_descriptor();
    }
  });
  std::shared_ptr<const ArtifactRecord> checkpoint;
  std::exception_ptr receiver_failure;
  try {
    checkpoint = materialize_worker_checkpoint(
        data_plane->worker_checkpoint_descriptor(), assignment, metadata);
  } catch (...) {
    receiver_failure = std::current_exception();
  }
  data_plane->close_worker_descriptors();
  sender.join();
  if (sender_failure != nullptr) {
    std::rethrow_exception(sender_failure);
  }
  if (receiver_failure != nullptr) {
    std::rethrow_exception(receiver_failure);
  }
  return checkpoint;
}

/**
 * @brief Writes one exact raw byte range to a small local test socket.
 * @param fd Connected descriptor.
 * @param bytes Non-null exact bytes when `size` is nonzero.
 * @param size Exact byte count.
 * @return Nothing after all bytes are written.
 * @throws std::invalid_argument for null nonempty input.
 * @throws std::runtime_error for an unexpected write failure.
 */
void write_raw_range(int fd, const std::byte* bytes, std::size_t size) {
  if (size != 0U && bytes == nullptr) {
    throw std::invalid_argument("worker protocol test raw input is null");
  }
  std::size_t offset = 0U;
  while (offset != size) {
    const ssize_t written = ::write(fd, bytes + offset, size - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      throw std::runtime_error("worker protocol test raw write failed");
    }
    offset += static_cast<std::size_t>(written);
  }
}

/**
 * @brief Writes one complete fixed raw test header.
 * @param fd Connected descriptor.
 * @param bytes Exact twelve-byte header.
 * @return Nothing after all bytes are written.
 * @throws std::runtime_error for an unexpected write failure.
 */
void write_raw(int fd, const std::array<std::byte, 12U>& bytes) {
  write_raw_range(fd, bytes.data(), bytes.size());
}

/**
 * @brief Overwrites one existing payload field with a big-endian integer.
 * @param bytes Non-null mutable payload.
 * @param offset First byte of an existing four-byte field.
 * @param value Exact replacement value.
 * @return Nothing after four in-range bytes are replaced.
 * @throws std::out_of_range when the requested field is outside the payload.
 */
void overwrite_u32(std::vector<std::byte>* bytes, std::size_t offset,
                   std::uint32_t value) {
  if (bytes == nullptr || offset > bytes->size() ||
      bytes->size() - offset < 4U) {
    throw std::out_of_range("worker protocol test field is out of range");
  }
  for (std::size_t index = 0U; index < 4U; ++index) {
    const std::size_t shift = (3U - index) * 8U;
    (*bytes)[offset + index] =
        static_cast<std::byte>(static_cast<std::uint8_t>(value >> shift));
  }
}

/**
 * @brief One already-encoded canonical worker payload field.
 * @throws Nothing for aggregate initialization and value operations.
 * @note Offsets are derived by `AssignmentWireLayout` parsing rather than
 * maintained as protocol constants.
 */
struct EncodedWorkerField final {
  /** @brief First byte in the containing payload. */
  std::size_t offset = 0U;
  /** @brief Exact fixed or decoded field width. */
  std::size_t width = 0U;
};

/**
 * @brief Length prefix and bytes of one canonical worker string field.
 * @throws Nothing for aggregate initialization and value operations.
 */
struct EncodedWorkerString final {
  /** @brief Four-byte big-endian length prefix. */
  EncodedWorkerField length;
  /** @brief Exact bytes selected by that prefix. */
  EncodedWorkerField value;
};

/**
 * @brief Walks one product-encoded worker payload without interpreting it.
 *
 * The cursor consumes fixed-width integers and length-prefixed strings in
 * canonical field order. It exposes exact slices only to deterministic tests;
 * production decoding and validation remain owned by `worker_protocol.cpp`.
 *
 * @throws Nothing for construction; member operations throw on malformed test
 * fixture bytes.
 * @note The cursor borrows one immutable payload and performs no socket,
 * descriptor, artifact, process, or authority operation.
 */
class CanonicalWorkerPayloadCursor final {
 public:
  /**
   * @brief Borrows one canonical product payload.
   * @param payload Exact immutable bytes retained by the calling test.
   * @throws Nothing.
   */
  explicit CanonicalWorkerPayloadCursor(
      const std::vector<std::byte>& payload) noexcept
      : payload_(payload) {}

  /**
   * @brief Consumes one exact encoded field.
   * @param width Required byte width, including zero for an empty string.
   * @return Slice describing the consumed field.
   * @throws std::out_of_range when the canonical fixture is truncated.
   */
  EncodedWorkerField take(std::size_t width) {
    if (width > payload_.size() - offset_) {
      throw std::out_of_range("canonical worker fixture is truncated");
    }
    const EncodedWorkerField field{offset_, width};
    offset_ += width;
    return field;
  }

  /**
   * @brief Reads one consumed field as an unsigned big-endian integer.
   * @param field Existing one- through eight-byte slice from this payload.
   * @return Exact integer value.
   * @throws std::invalid_argument when the width is zero, exceeds
   * `sizeof(std::uint64_t)`, or the slice is not inside this payload.
   */
  std::uint64_t unsigned_value(const EncodedWorkerField& field) const {
    if (field.width == 0U || field.width > sizeof(std::uint64_t) ||
        field.offset > payload_.size() ||
        field.width > payload_.size() - field.offset) {
      throw std::invalid_argument("canonical worker integer field is invalid");
    }
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < field.width; ++index) {
      value = (value << 8U) |
              std::to_integer<std::uint8_t>(payload_[field.offset + index]);
    }
    return value;
  }

  /**
   * @brief Consumes one canonical length-prefixed string.
   * @return Four-byte length field and the exact following value bytes.
   * @throws std::out_of_range when the length prefix or value bytes are
   * truncated.
   */
  EncodedWorkerString take_string() {
    const EncodedWorkerField length = take(sizeof(std::uint32_t));
    const std::size_t value_width =
        static_cast<std::size_t>(unsigned_value(length));
    return EncodedWorkerString{length, take(value_width)};
  }

  /**
   * @brief Consumes and returns one closed boolean byte.
   * @return Canonical zero or one value.
   * @throws std::logic_error when the product encoder emitted another value.
   * @throws std::out_of_range for truncated fixture bytes.
   */
  bool take_bool() {
    const std::uint64_t value = unsigned_value(take(1U));
    if (value > 1U) {
      throw std::logic_error("canonical worker boolean is invalid");
    }
    return value != 0U;
  }

  /**
   * @brief Requires complete semantic consumption of the canonical payload.
   * @return Nothing when no encoded field remains.
   * @throws std::logic_error when the test layout omitted a product field.
   */
  void finish() const {
    if (offset_ != payload_.size()) {
      throw std::logic_error("canonical worker layout has trailing bytes");
    }
  }

 private:
  /** @brief Borrowed immutable canonical payload. */
  const std::vector<std::byte>& payload_;
  /** @brief First byte not yet consumed by semantic traversal. */
  std::size_t offset_ = 0U;
};

/**
 * @brief Wire slices retained while one AttemptIdentity is consumed.
 * @throws Nothing for aggregate initialization and value operations.
 */
struct EncodedAttemptIdentityFields final {
  /** @brief Fixed-width JobSpec digest inside the identity tuple. */
  EncodedWorkerField job_spec_digest;
  /** @brief Fixed-width worker lease generation. */
  EncodedWorkerField worker_lease_generation;
};

/**
 * @brief Consumes one canonical AttemptIdentity in production field order.
 * @param cursor Non-null cursor positioned at an identity boundary.
 * @return Slices needed by identity and digest mutation regressions.
 * @throws std::invalid_argument when `cursor` is null.
 * @throws Fixture traversal failures unchanged.
 */
EncodedAttemptIdentityFields take_attempt_identity_fields(
    CanonicalWorkerPayloadCursor* cursor) {
  if (cursor == nullptr) {
    throw std::invalid_argument("canonical identity cursor is null");
  }
  static_cast<void>(cursor->take_string());
  static_cast<void>(cursor->take_string());
  const EncodedWorkerField digest = cursor->take(JobSpecDigest{}.bytes.size());
  static_cast<void>(cursor->take_string());
  static_cast<void>(cursor->take_string());
  const EncodedWorkerField lease = cursor->take(sizeof(std::uint64_t));
  return EncodedAttemptIdentityFields{digest, lease};
}

/**
 * @brief Semantic slices in one canonical Assignment payload.
 * @throws Nothing for aggregate initialization and value operations.
 * @note Optional receipt fields exist only when both JobSpec and data-plane
 * metadata declare a checkpoint.
 */
struct AssignmentWireLayout final {
  /** @brief Assignment AttemptIdentity JobSpec digest bytes. */
  EncodedWorkerField attempt_job_spec_digest;
  /** @brief Assignment AttemptIdentity worker lease generation. */
  EncodedWorkerField attempt_worker_lease_generation;
  /** @brief Closed JobSpec execution-profile enum. */
  EncodedWorkerField execution_profile;
  /** @brief Configured-device count preceding the device sequence. */
  EncodedWorkerField device_count;
  /** @brief Output-stage reference bytes used by the data-plane join. */
  EncodedWorkerString output_reference;
  /** @brief Checkpoint receipt ArtifactId bytes when present. */
  std::optional<EncodedWorkerString> checkpoint_receipt_artifact_id;
  /** @brief Checkpoint receipt descriptor width when present. */
  std::optional<EncodedWorkerField> checkpoint_descriptor_width;
  /** @brief Checkpoint receipt durability enum when present. */
  std::optional<EncodedWorkerField> checkpoint_receipt_durability;
  /** @brief Length prefix for the first transported graph text field. */
  EncodedWorkerField graph_root_length;
  /** @brief Final nonzero heartbeat cadence. */
  EncodedWorkerField heartbeat;

  /**
   * @brief Derives every retained slice by walking a canonical Assignment.
   *
   * @param payload Product encoder output for one Assignment frame.
   * @return Complete semantic layout after exact payload consumption.
   * @throws std::logic_error for inconsistent canonical checkpoint flags or
   * omitted/trailing fields.
   * @throws Fixture traversal and allocation failures unchanged.
   * @note No offset is maintained independently of the product field order.
   */
  static AssignmentWireLayout parse(const std::vector<std::byte>& payload) {
    CanonicalWorkerPayloadCursor cursor(payload);
    AssignmentWireLayout layout;
    const EncodedAttemptIdentityFields identity =
        take_attempt_identity_fields(&cursor);
    layout.attempt_job_spec_digest = identity.job_spec_digest;
    layout.attempt_worker_lease_generation = identity.worker_lease_generation;

    static_cast<void>(cursor.take_string());
    static_cast<void>(cursor.take(sizeof(std::uint32_t)));
    static_cast<void>(cursor.take_string());
    layout.execution_profile = cursor.take(1U);
    static_cast<void>(cursor.take(1U));
    static_cast<void>(cursor.take(sizeof(std::uint32_t)));
    for (std::size_t index = 0U; index < 4U; ++index) {
      static_cast<void>(cursor.take(sizeof(std::uint64_t)));
    }
    layout.device_count = cursor.take(sizeof(std::uint32_t));
    const std::size_t device_count =
        static_cast<std::size_t>(cursor.unsigned_value(layout.device_count));
    for (std::size_t index = 0U; index < device_count; ++index) {
      static_cast<void>(cursor.take_string());
      static_cast<void>(cursor.take(sizeof(std::uint64_t)));
    }
    const bool spec_has_checkpoint = cursor.take_bool();
    if (spec_has_checkpoint) {
      static_cast<void>(cursor.take_string());
    }

    const bool data_plane_has_checkpoint = cursor.take_bool();
    if (spec_has_checkpoint != data_plane_has_checkpoint) {
      throw std::logic_error(
          "canonical Assignment checkpoint flags are inconsistent");
    }
    if (data_plane_has_checkpoint) {
      static_cast<void>(cursor.take_string());
      static_cast<void>(take_attempt_identity_fields(&cursor));
      static_cast<void>(cursor.take_string());
      layout.checkpoint_receipt_artifact_id = cursor.take_string();
      static_cast<void>(cursor.take_string());
      layout.checkpoint_descriptor_width = cursor.take(sizeof(std::uint32_t));
      static_cast<void>(cursor.take(sizeof(std::uint32_t)));
      static_cast<void>(cursor.take(sizeof(std::uint32_t)));
      static_cast<void>(cursor.take(1U));
      static_cast<void>(cursor.take(sizeof(std::uint64_t)));
      static_cast<void>(cursor.take(sizeof(std::uint64_t)));
      static_cast<void>(cursor.take(ArtifactContentDigest{}.bytes.size()));
      layout.checkpoint_receipt_durability = cursor.take(1U);
    }
    layout.output_reference = cursor.take_string();
    static_cast<void>(cursor.take_string());
    static_cast<void>(cursor.take(sizeof(std::uint64_t)));

    static_cast<void>(cursor.take_bool());
    layout.graph_root_length = cursor.take_string().length;
    for (std::size_t index = 0U; index < 4U; ++index) {
      static_cast<void>(cursor.take_string());
    }
    layout.heartbeat = cursor.take(sizeof(std::uint32_t));
    cursor.finish();
    return layout;
  }
};

/**
 * @brief Overwrites one semantic integer field in big-endian order.
 * @param payload Non-null canonical payload copy.
 * @param field Exact one- through eight-byte slice derived from its layout.
 * @param value Replacement value representable by `field.width` bytes.
 * @return Nothing after only the selected bytes are replaced.
 * @throws std::invalid_argument for null, empty, oversized, or out-of-range
 * field metadata, or a replacement that does not fit.
 */
void overwrite_worker_integer(std::vector<std::byte>* payload,
                              const EncodedWorkerField& field,
                              std::uint64_t value) {
  if (payload == nullptr || field.width == 0U ||
      field.width > sizeof(std::uint64_t) || field.offset > payload->size() ||
      field.width > payload->size() - field.offset ||
      (field.width < sizeof(std::uint64_t) &&
       value >= (std::uint64_t{1U} << (field.width * 8U)))) {
    throw std::invalid_argument("worker semantic integer mutation is invalid");
  }
  for (std::size_t index = 0U; index < field.width; ++index) {
    const std::size_t shift = (field.width - index - 1U) * 8U;
    (*payload)[field.offset + index] =
        static_cast<std::byte>(static_cast<std::uint8_t>(value >> shift));
  }
}

/**
 * @brief Replaces the first byte of one nonempty semantic field.
 * @param payload Non-null canonical payload copy.
 * @param field Nonempty in-range slice derived from its semantic layout.
 * @param value Exact replacement byte.
 * @return Nothing after one byte is replaced without changing framing.
 * @throws std::invalid_argument for null, empty, or out-of-range metadata.
 * @note Tests use valid opaque replacement characters or one digest bit so
 * earlier length and syntax validation remains satisfied.
 */
void replace_first_worker_field_byte(std::vector<std::byte>* payload,
                                     const EncodedWorkerField& field,
                                     std::byte value) {
  if (payload == nullptr || field.width == 0U ||
      field.offset > payload->size() ||
      field.width > payload->size() - field.offset) {
    throw std::invalid_argument("worker semantic byte mutation is invalid");
  }
  (*payload)[field.offset] = value;
}

/**
 * @brief Requires one direct Assignment semantic rejection and diagnostic.
 * @param frame Structurally mutated canonical Assignment frame.
 * @param expected_diagnostic Required semantic rejection substring.
 * @param mutation_name Human-readable matrix row used by GoogleTest tracing.
 * @return Nothing after observing `WorkerProtocolError` with the expected
 * semantic diagnostic.
 * @throws Test-framework or allocation failures unchanged.
 */
void expect_assignment_rejection(const WorkerProtocolFrame& frame,
                                 std::string_view expected_diagnostic,
                                 std::string_view mutation_name) {
  SCOPED_TRACE(std::string(mutation_name));
  try {
    static_cast<void>(decode_worker_assignment(frame));
    FAIL() << "semantic Assignment mutation was accepted";
  } catch (const WorkerProtocolError& error) {
    EXPECT_NE(std::string_view(error.what()).find(expected_diagnostic),
              std::string_view::npos)
        << error.what();
  } catch (const std::exception& error) {
    FAIL() << "semantic Assignment mutation raised wrong exception: "
           << error.what();
  }
}

TEST(WorkerProtocol, RoundTripsCompleteAssignmentAndExactLease) {
  ScopedSocketPair sockets;
  auto spec = std::make_shared<const JobSpec>(GraphArtifactId("graph.protocol"),
                                              7, OutputSlotId("image.final"),
                                              protocol_resources());
  auto prepared = prepared_assignment_with_data_plane(spec);
  PreparedWorkerAssignment& sent = prepared.first;
  sent.graph.ok = true;
  sent.graph.root_dir = "/trusted/root";
  sent.graph.yaml_path = "/trusted/root/graph.yaml";
  sent.graph.config_path = "/trusted/root/config.yaml";
  sent.graph.cache_root_dir = "/trusted/cache";
  sent.heartbeat_interval = std::chrono::milliseconds(125);

  send_worker_assignment(sockets.at(0U), sent, protocol_deadline());
  const PreparedWorkerAssignment received =
      receive_worker_assignment(sockets.at(1U), protocol_deadline());

  EXPECT_EQ(received.assignment.identity, sent.assignment.identity);
  ASSERT_NE(received.assignment.spec, nullptr);
  EXPECT_EQ(received.assignment.spec->canonical_bytes(),
            spec->canonical_bytes());
  EXPECT_EQ(received.assignment.spec->digest(), spec->digest());
  EXPECT_EQ(received.assignment.checkpoint, nullptr);
  EXPECT_EQ(received.data_plane.output.reference_id,
            sent.data_plane.output.reference_id);
  EXPECT_EQ(received.data_plane.output.output_slot_id,
            sent.data_plane.output.output_slot_id);
  EXPECT_EQ(received.data_plane.output.maximum_payload_bytes,
            sent.data_plane.output.maximum_payload_bytes);
  EXPECT_EQ(received.graph.yaml_path, sent.graph.yaml_path);
  EXPECT_EQ(received.heartbeat_interval, sent.heartbeat_interval);
}

/**
 * @brief Locks the pure Assignment semantic decoder and canonical wire oracle.
 * @return Nothing; GoogleTest reports decode, equality, or rejection failures.
 * @throws Allocation or fixture-construction failures unchanged.
 * @note Every strict payload prefix and one trailing-byte mutation exercises
 * the production semantic decoder without opening a socket or data-plane lane.
 */
TEST(WorkerProtocol,
     PureAssignmentDecoderCanonicalizesAndRejectsBoundedFramingMutations) {
  auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.protocol.pure-assignment"), 7,
      OutputSlotId("image.final"), protocol_resources());
  auto prepared = prepared_assignment_with_data_plane(spec);
  PreparedWorkerAssignment& sent = prepared.first;
  sent.graph.root_dir = "/trusted/root";
  sent.graph.yaml_path = "/trusted/root/graph.yaml";
  sent.graph.config_path = "/trusted/root/config.yaml";
  sent.graph.cache_root_dir = "/trusted/cache";
  sent.graph.message = "prepared";

  const WorkerProtocolFrame canonical = encode_worker_assignment(sent);
  const PreparedWorkerAssignment decoded = decode_worker_assignment(canonical);
  ASSERT_NE(decoded.assignment.spec, nullptr);
  EXPECT_EQ(decoded.assignment.identity, sent.assignment.identity);
  EXPECT_EQ(decoded.assignment.spec->canonical_bytes(),
            sent.assignment.spec->canonical_bytes());
  EXPECT_EQ(decoded.data_plane.output.reference_id,
            sent.data_plane.output.reference_id);
  EXPECT_EQ(decoded.graph.root_dir, sent.graph.root_dir);
  EXPECT_EQ(decoded.graph.yaml_path, sent.graph.yaml_path);
  EXPECT_EQ(decoded.graph.config_path, sent.graph.config_path);
  EXPECT_EQ(decoded.graph.cache_root_dir, sent.graph.cache_root_dir);
  EXPECT_EQ(decoded.graph.message, sent.graph.message);
  EXPECT_EQ(decoded.heartbeat_interval, sent.heartbeat_interval);
  const WorkerProtocolFrame reencoded =
      encode_worker_assignment_metadata(decoded);
  EXPECT_EQ(reencoded.kind, canonical.kind);
  EXPECT_EQ(reencoded.payload, canonical.payload);

  for (std::size_t retained = 0U; retained < canonical.payload.size();
       ++retained) {
    WorkerProtocolFrame truncated = canonical;
    truncated.payload.resize(retained);
    EXPECT_THROW(decode_worker_assignment(truncated), WorkerProtocolError)
        << "retained bytes=" << retained;
  }
  WorkerProtocolFrame trailing = canonical;
  trailing.payload.push_back(std::byte{0});
  EXPECT_THROW(decode_worker_assignment(trailing), WorkerProtocolError);

  WorkerProtocolFrame wrong_kind = canonical;
  wrong_kind.kind = WorkerMessageKind::Heartbeat;
  EXPECT_THROW(decode_worker_assignment(wrong_kind), WorkerProtocolError);
}

/**
 * @brief Locks the structured Assignment semantic rejection matrix.
 * @return Nothing; GoogleTest reports any accepted mutation, wrong exception,
 * or diagnostic that proves a different earlier field rejected the payload.
 * @throws Fixture construction, hashing, or allocation failures unchanged.
 * @note Every row starts from product-canonical bytes, changes one field at a
 * semantic cursor-derived offset without changing aggregate framing, and then
 * calls the production pure decoder directly. The checkpoint rows ensure the
 * receipt and tight-descriptor branches are actually present.
 */
TEST(WorkerProtocol, PureAssignmentDecoderRejectsStructuredFieldMutations) {
  auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.protocol.semantic-assignment"), 7,
      OutputSlotId("image.final"), protocol_resources());
  auto assignment = prepared_assignment_with_data_plane(spec);
  assignment.first.graph.root_dir = "/trusted/semantic-root";
  assignment.first.graph.yaml_path = "/trusted/semantic-root/graph.yaml";
  assignment.first.graph.message = "semantic mutation matrix";
  const WorkerProtocolFrame canonical =
      encode_worker_assignment(assignment.first);
  const AssignmentWireLayout layout =
      AssignmentWireLayout::parse(canonical.payload);

  WorkerProtocolFrame invalid_profile = canonical;
  overwrite_worker_integer(&invalid_profile.payload, layout.execution_profile,
                           0xffU);
  expect_assignment_rejection(invalid_profile,
                              "worker JobSpec profile is invalid",
                              "closed JobSpec execution-profile enum");

  WorkerProtocolFrame excessive_device_count = canonical;
  overwrite_worker_integer(
      &excessive_device_count.payload, layout.device_count,
      static_cast<std::uint64_t>(kMaximumConfiguredDevicesPerJob + 1U));
  expect_assignment_rejection(
      excessive_device_count, "worker JobSpec device count exceeds its bound",
      "configured-device count bound before sequence allocation");

  WorkerProtocolFrame oversized_graph_length = canonical;
  overwrite_worker_integer(
      &oversized_graph_length.payload, layout.graph_root_length,
      static_cast<std::uint64_t>(kMaximumWorkerTextFieldBytes + 1U));
  expect_assignment_rejection(oversized_graph_length,
                              "worker protocol string exceeds its bound",
                              "transported graph string length prefix");

  WorkerProtocolFrame incomplete_attempt = canonical;
  overwrite_worker_integer(&incomplete_attempt.payload,
                           layout.attempt_worker_lease_generation, 0U);
  expect_assignment_rejection(incomplete_attempt,
                              "attempt identity tuple is incomplete",
                              "AttemptIdentity lease generation");

  WorkerProtocolFrame mismatched_job_spec_digest = canonical;
  const std::byte changed_digest =
      mismatched_job_spec_digest
          .payload[layout.attempt_job_spec_digest.offset] ^
      std::byte{0x01};
  replace_first_worker_field_byte(&mismatched_job_spec_digest.payload,
                                  layout.attempt_job_spec_digest,
                                  changed_digest);
  expect_assignment_rejection(
      mismatched_job_spec_digest,
      "worker assignment JobSpec digest does not join identity",
      "AttemptIdentity to canonical JobSpec digest join");

  WorkerProtocolFrame mismatched_output_reference = canonical;
  replace_first_worker_field_byte(&mismatched_output_reference.payload,
                                  layout.output_reference.value,
                                  std::byte{'x'});
  expect_assignment_rejection(
      mismatched_output_reference,
      "worker output-stage metadata is inconsistent",
      "data-plane output reference join after complete JobSpec decode");

  WorkerProtocolFrame zero_heartbeat = canonical;
  overwrite_worker_integer(&zero_heartbeat.payload, layout.heartbeat, 0U);
  expect_assignment_rejection(zero_heartbeat,
                              "worker heartbeat cadence is zero",
                              "final heartbeat cadence");

  const ArtifactId checkpoint_id("artifact.protocol.semantic-checkpoint");
  auto checkpoint_spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.protocol.semantic-checkpoint"), 7,
      OutputSlotId("image.final"), protocol_resources(), checkpoint_id);
  auto checkpoint_assignment = prepared_assignment_with_data_plane(
      checkpoint_spec,
      std::make_shared<const ArtifactRecord>(
          maximum_assignment_checkpoint(*checkpoint_spec, 32U)));
  checkpoint_assignment.first.graph.root_dir = "/trusted/checkpoint-root";
  checkpoint_assignment.first.graph.yaml_path =
      "/trusted/checkpoint-root/graph.yaml";
  const WorkerProtocolFrame checkpoint_canonical =
      encode_worker_assignment(checkpoint_assignment.first);
  const PreparedWorkerAssignment checkpoint_decoded =
      decode_worker_assignment(checkpoint_canonical);
  ASSERT_TRUE(checkpoint_decoded.data_plane.checkpoint.has_value());
  EXPECT_EQ(encode_worker_assignment_metadata(checkpoint_decoded).payload,
            checkpoint_canonical.payload);
  const AssignmentWireLayout checkpoint_layout =
      AssignmentWireLayout::parse(checkpoint_canonical.payload);
  ASSERT_TRUE(checkpoint_layout.checkpoint_receipt_artifact_id.has_value());
  ASSERT_TRUE(checkpoint_layout.checkpoint_descriptor_width.has_value());
  ASSERT_TRUE(checkpoint_layout.checkpoint_receipt_durability.has_value());

  WorkerProtocolFrame mismatched_receipt = checkpoint_canonical;
  replace_first_worker_field_byte(
      &mismatched_receipt.payload,
      checkpoint_layout.checkpoint_receipt_artifact_id->value, std::byte{'z'});
  expect_assignment_rejection(
      mismatched_receipt,
      "worker checkpoint data-plane metadata is inconsistent",
      "checkpoint receipt ArtifactId join");

  WorkerProtocolFrame inconsistent_checkpoint_descriptor = checkpoint_canonical;
  overwrite_worker_integer(&inconsistent_checkpoint_descriptor.payload,
                           *checkpoint_layout.checkpoint_descriptor_width, 31U);
  expect_assignment_rejection(inconsistent_checkpoint_descriptor,
                              "worker artifact descriptor is inconsistent",
                              "checkpoint receipt tight descriptor width");

  WorkerProtocolFrame invalid_receipt_durability = checkpoint_canonical;
  overwrite_worker_integer(&invalid_receipt_durability.payload,
                           *checkpoint_layout.checkpoint_receipt_durability,
                           0xffU);
  expect_assignment_rejection(invalid_receipt_durability,
                              "worker artifact durability is invalid",
                              "checkpoint receipt durability enum");
}

TEST(WorkerProtocol, ExpiredBufferedAssignmentIsNotSemanticallyAccepted) {
  ScopedSocketPair sockets;
  auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.protocol.expired-assignment"), 7,
      OutputSlotId("image.final"), protocol_resources());
  auto prepared = prepared_assignment_with_data_plane(spec);
  prepared.first.graph.ok = true;
  prepared.first.graph.yaml_path = "/trusted/expired-assignment.yaml";

  send_worker_assignment(sockets.at(0U), prepared.first, protocol_deadline());

  EXPECT_THROW(receive_worker_assignment(sockets.at(1U),
                                         std::chrono::steady_clock::now()),
               WorkerProtocolTimeout);
}

TEST(WorkerProtocol,
     WorkerCheckpointLaneRejectsReverseSendAndPreservesManagerDirection) {
  auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.protocol.checkpoint-direction"), 7,
      OutputSlotId("image.final"), protocol_resources());
  auto assignment_and_plane = prepared_assignment_with_data_plane(spec);
  WorkerArtifactDataPlane& data_plane = assignment_and_plane.second;
  const std::byte expected{0x5a};

  errno = 0;
  EXPECT_EQ(
      send_direction_probe(data_plane.worker_checkpoint_descriptor(), expected),
      -1);
  EXPECT_TRUE(is_direction_rejection_error(errno)) << std::strerror(errno);

  const std::vector<std::byte> payload{expected};
  std::size_t offset = 0U;
  EXPECT_EQ(data_plane.send_checkpoint_chunk(payload, &offset),
            WorkerDataPlaneIoStatus::Progress);
  EXPECT_EQ(offset, payload.size());
  data_plane.close_manager_checkpoint_descriptor();

  std::byte received{};
  EXPECT_EQ(::recv(data_plane.worker_checkpoint_descriptor(), &received,
                   sizeof(received), 0),
            1);
  EXPECT_EQ(received, expected);
  EXPECT_EQ(::recv(data_plane.worker_checkpoint_descriptor(), &received,
                   sizeof(received), 0),
            0);
}

TEST(WorkerProtocol,
     ManagerOutputLaneRejectsReverseSendAndPreservesWorkerDirectionAndEof) {
  auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.protocol.output-direction"), 7,
      OutputSlotId("image.final"), protocol_resources());
  auto assignment_and_plane = prepared_assignment_with_data_plane(spec);
  WorkerArtifactDataPlane& data_plane = assignment_and_plane.second;
  const std::byte expected{0xa5};

  errno = 0;
  EXPECT_EQ(
      send_direction_probe(data_plane.manager_output_descriptor(), expected),
      -1);
  EXPECT_TRUE(is_direction_rejection_error(errno)) << std::strerror(errno);

  ASSERT_EQ(
      send_direction_probe(data_plane.worker_output_descriptor(), expected), 1);
  data_plane.close_worker_descriptors();
  std::byte received{};
  std::size_t offset = 0U;
  EXPECT_EQ(
      data_plane.receive_output_chunk(&received, sizeof(received), &offset),
      WorkerDataPlaneIoStatus::Progress);
  EXPECT_EQ(offset, sizeof(received));
  EXPECT_EQ(received, expected);
  EXPECT_EQ(
      data_plane.receive_output_chunk(&received, sizeof(received), &offset),
      WorkerDataPlaneIoStatus::EndOfStream);
}

TEST(WorkerProtocol, AssignmentControlPayloadIsIndependentOfCheckpointBytes) {
  constexpr std::size_t kMetadataControlMaximum = 128U << 10U;
  const ArtifactId checkpoint_id("artifact.protocol.metadata-checkpoint");
  auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.protocol.metadata-assignment"), 7,
      OutputSlotId("image.final"), frame_bound_resources(), checkpoint_id);
  auto assignment_and_plane = prepared_assignment_with_data_plane(
      spec, std::make_shared<const ArtifactRecord>(
                maximum_assignment_checkpoint(*spec, 1U << 20U)));
  PreparedWorkerAssignment& prepared = assignment_and_plane.first;

  const WorkerProtocolFrame frame = encode_worker_assignment(prepared);

  EXPECT_LT(frame.payload.size(), kMetadataControlMaximum);
}

TEST(WorkerProtocol, MaximumDeclaredAssignmentMetadataFitsControlBound) {
  const WorkerProtocolFrame frame =
      encode_worker_assignment(maximum_metadata_assignment());

  EXPECT_EQ(frame.kind, WorkerMessageKind::Assignment);
  EXPECT_LE(frame.payload.size(), kMaximumWorkerControlPayloadBytes);
}

TEST(WorkerProtocol, ReportControlPayloadIsIndependentOfCandidateBytes) {
  constexpr std::size_t kMetadataControlMaximum = 128U << 10U;
  auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.protocol.metadata-report"), 7,
      OutputSlotId("image.final"), frame_bound_resources());
  auto assignment_and_plane = prepared_assignment_with_data_plane(spec);
  JobAttemptReport report =
      frame_bound_success_report(assignment_and_plane.first.assignment.identity,
                                 "candidate staged outside control", 1U << 20U);
  const CollectedWorkerOutput staged = stage_and_collect_output(
      &assignment_and_plane.second, *spec,
      assignment_and_plane.first.data_plane.output, &report);
  const std::optional<WorkerOutputDataReference>& output = staged.reference;
  const PreparedWorkerReport prepared{std::move(report), output};

  const WorkerProtocolFrame frame = encode_worker_report(
      prepared, *spec, assignment_and_plane.first.data_plane.output);

  EXPECT_LT(frame.payload.size(), kMetadataControlMaximum);
}

TEST(WorkerProtocol, PreparedGraphTextFieldsShareExactCatalogAndEncoderBounds) {
  /**
   * @brief Selects one transported graph string and readable test padding.
   * @throws Nothing for aggregate initialization and value operations.
   */
  struct GraphTextFieldCase final {
    /** @brief Human-readable field name used in assertion diagnostics. */
    const char* name;
    /** @brief Exact `ResolvedGraphArtifact` string member under test. */
    std::string ResolvedGraphArtifact::* member;
    /** @brief Distinct byte repeated to the selected transport boundary. */
    char padding;
  };
  const std::array<GraphTextFieldCase, 5U> cases{{
      {"root_dir", &ResolvedGraphArtifact::root_dir, 'r'},
      {"yaml_path", &ResolvedGraphArtifact::yaml_path, 'y'},
      {"config_path", &ResolvedGraphArtifact::config_path, 'c'},
      {"cache_root_dir", &ResolvedGraphArtifact::cache_root_dir, 'h'},
      {"message", &ResolvedGraphArtifact::message, 'm'},
  }};
  auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.protocol.graph-text-bound"), 7,
      OutputSlotId("image.final"), protocol_resources());

  for (const GraphTextFieldCase& test_case : cases) {
    ResolvedGraphArtifact exact_graph;
    exact_graph.ok = true;
    exact_graph.*(test_case.member) =
        std::string(kMaximumWorkerTextFieldBytes, test_case.padding);
    EXPECT_NO_THROW(validate_worker_assignment_graph_transport(exact_graph))
        << test_case.name;
    EXPECT_NO_THROW(
        PreparedExternalGraphCatalog(std::vector<PreparedExternalGraphEntry>{
            {spec->graph_artifact_id(), exact_graph}}))
        << test_case.name;
    PreparedWorkerAssignment exact_assignment;
    exact_assignment.assignment.identity = protocol_identity(*spec);
    exact_assignment.assignment.spec = spec;
    WorkerArtifactDataPlane data_plane =
        WorkerArtifactDataPlane::create(exact_assignment.assignment);
    exact_assignment.data_plane = data_plane.assignment_metadata();
    exact_assignment.graph = exact_graph;
    exact_assignment.heartbeat_interval = std::chrono::milliseconds(125);
    EXPECT_NO_THROW(encode_worker_assignment(exact_assignment))
        << test_case.name;

    ResolvedGraphArtifact oversized_graph = std::move(exact_graph);
    oversized_graph.*(test_case.member) =
        std::string(kMaximumWorkerTextFieldBytes + 1U, test_case.padding);
    EXPECT_THROW(validate_worker_assignment_graph_transport(oversized_graph),
                 std::length_error)
        << test_case.name;
    try {
      static_cast<void>(
          PreparedExternalGraphCatalog(std::vector<PreparedExternalGraphEntry>{
              {spec->graph_artifact_id(), oversized_graph}}));
      ADD_FAILURE() << test_case.name
                    << " catalog construction accepted one byte over";
    } catch (const std::length_error& error) {
      const std::string diagnostic(error.what());
      EXPECT_NE(diagnostic.find(test_case.name), std::string::npos)
          << diagnostic;
      EXPECT_NE(
          diagnostic.find(std::to_string(kMaximumWorkerTextFieldBytes + 1U)),
          std::string::npos)
          << diagnostic;
      EXPECT_NE(diagnostic.find(std::to_string(kMaximumWorkerTextFieldBytes)),
                std::string::npos)
          << diagnostic;
    } catch (const std::exception& error) {
      ADD_FAILURE() << test_case.name
                    << " raised wrong exception: " << error.what();
    }
    PreparedWorkerAssignment oversized_assignment;
    oversized_assignment.assignment.identity = protocol_identity(*spec);
    oversized_assignment.assignment.spec = spec;
    oversized_assignment.data_plane = data_plane.assignment_metadata();
    oversized_assignment.graph = std::move(oversized_graph);
    oversized_assignment.heartbeat_interval = std::chrono::milliseconds(125);
    EXPECT_THROW(encode_worker_assignment(oversized_assignment),
                 std::length_error)
        << test_case.name;
  }
}

TEST(WorkerProtocol, StatefulDecoderPreservesHeaderAndPayloadAcrossTimeouts) {
  ScopedSocketPair sockets;
  const std::array<std::byte, 18U> frame_bytes{
      std::byte{0x50}, std::byte{0x53}, std::byte{0x57}, std::byte{0x31},
      std::byte{0x00}, std::byte{0x02}, std::byte{0x00}, std::byte{0x03},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x06},
      std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40},
      std::byte{0x50}, std::byte{0x60}};
  WorkerFrameDecoder decoder;

  write_raw_range(sockets.at(0U), frame_bytes.data(), 5U);
  EXPECT_THROW(
      decoder.read_frame(sockets.at(1U), std::chrono::steady_clock::now() +
                                             std::chrono::milliseconds(10)),
      WorkerProtocolTimeout);

  write_raw_range(sockets.at(0U), frame_bytes.data() + 5U, 9U);
  EXPECT_THROW(
      decoder.read_frame(sockets.at(1U), std::chrono::steady_clock::now() +
                                             std::chrono::milliseconds(10)),
      WorkerProtocolTimeout);

  write_raw_range(sockets.at(0U), frame_bytes.data() + 14U, 4U);
  const WorkerProtocolFrame frame =
      decoder.read_frame(sockets.at(1U), protocol_deadline());
  EXPECT_EQ(frame.kind, WorkerMessageKind::Heartbeat);
  const std::vector<std::byte> expected(frame_bytes.begin() + 12,
                                        frame_bytes.end());
  EXPECT_EQ(frame.payload, expected);
}

TEST(WorkerProtocol, ExpiredBufferedFrameTimesOutAndRemainsAvailableForRetry) {
  ScopedSocketPair sockets;
  const std::vector<std::byte> expected{std::byte{0x10}, std::byte{0x20},
                                        std::byte{0x30}};
  write_worker_frame(sockets.at(0U), WorkerMessageKind::Heartbeat, expected,
                     protocol_deadline());
  WorkerFrameDecoder decoder;
  bool timed_out = false;

  try {
    static_cast<void>(
        decoder.read_frame(sockets.at(1U), std::chrono::steady_clock::now()));
  } catch (const WorkerProtocolTimeout&) {
    timed_out = true;
  }

  ASSERT_TRUE(timed_out);
  const WorkerProtocolFrame frame =
      decoder.read_frame(sockets.at(1U), protocol_deadline());
  EXPECT_EQ(frame.kind, WorkerMessageKind::Heartbeat);
  EXPECT_EQ(frame.payload, expected);
}

TEST(WorkerProtocol,
     CompleteDecodeCrossingDeadlineTimesOutAndRemainsAvailableForRetry) {
  ScopedSocketPair sockets;
  const std::vector<std::byte> expected{std::byte{0x41}, std::byte{0x42},
                                        std::byte{0x43}};
  write_worker_frame(sockets.at(0U), WorkerMessageKind::Report, expected,
                     protocol_deadline());
  WorkerFrameDecoder decoder;
  ProtocolDeadlineHookState state;
  const auto synthetic_now = std::chrono::steady_clock::now();
  state.now_ticks.store(synthetic_now.time_since_epoch().count(),
                        std::memory_order_release);
  state.deadline = synthetic_now + std::chrono::seconds(1);
  state.crossing_point =
      WorkerProtocolDeadlineTestPoint::FrameReadyBeforeAcceptance;
  const WorkerProtocolDeadlineTestHooks hooks{&state, protocol_test_now,
                                              cross_protocol_test_deadline};
  bool timed_out = false;

  {
    const ScopedWorkerProtocolDeadlineTestHooks scoped_hooks(&hooks);
    try {
      static_cast<void>(decoder.read_frame(sockets.at(1U), state.deadline));
    } catch (const WorkerProtocolTimeout&) {
      timed_out = true;
    }
  }

  ASSERT_TRUE(timed_out);
  const WorkerProtocolFrame frame =
      decoder.read_frame(sockets.at(1U), protocol_deadline());
  EXPECT_EQ(frame.kind, WorkerMessageKind::Report);
  EXPECT_EQ(frame.payload, expected);
}

TEST(WorkerProtocol,
     ZeroBudgetProbeAcceptsReadyFrameBeforeIndependentLifecycleDeadline) {
  ScopedSocketPair sockets;
  const std::vector<std::byte> expected{std::byte{0x7a}};
  write_worker_frame(sockets.at(0U), WorkerMessageKind::Heartbeat, expected,
                     protocol_deadline());
  WorkerFrameDecoder decoder;
  const auto poll_deadline = std::chrono::steady_clock::now();

  const WorkerProtocolFrame frame =
      decoder.read_frame(sockets.at(1U), poll_deadline, protocol_deadline());

  EXPECT_EQ(frame.kind, WorkerMessageKind::Heartbeat);
  EXPECT_EQ(frame.payload, expected);
}

TEST(WorkerProtocol, ZeroBudgetProbeRetainsPartialFrameForNextBulkSlice) {
  ScopedSocketPair sockets;
  const std::array<std::byte, 18U> frame_bytes{
      std::byte{0x50}, std::byte{0x53}, std::byte{0x57}, std::byte{0x31},
      std::byte{0x00}, std::byte{0x02}, std::byte{0x00}, std::byte{0x03},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x06},
      std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40},
      std::byte{0x50}, std::byte{0x60}};
  WorkerFrameDecoder decoder;
  write_raw_range(sockets.at(0U), frame_bytes.data(), 5U);

  EXPECT_THROW(
      decoder.read_frame(sockets.at(1U), std::chrono::steady_clock::now(),
                         protocol_deadline()),
      WorkerProtocolTimeout);

  write_raw_range(sockets.at(0U), frame_bytes.data() + 5U,
                  frame_bytes.size() - 5U);
  const WorkerProtocolFrame frame =
      decoder.read_frame(sockets.at(1U), protocol_deadline());
  EXPECT_EQ(frame.kind, WorkerMessageKind::Heartbeat);
  const std::vector<std::byte> expected(frame_bytes.begin() + 12,
                                        frame_bytes.end());
  EXPECT_EQ(frame.payload, expected);
}

TEST(WorkerProtocol, ExpiredWritableFrameDoesNotSendAnyBytes) {
  ScopedSocketPair sockets;
  const std::vector<std::byte> payload{std::byte{0x31}};

  EXPECT_THROW(write_worker_frame(sockets.at(0U), WorkerMessageKind::Heartbeat,
                                  payload, std::chrono::steady_clock::now()),
               WorkerProtocolTimeout);

  std::byte received{};
  errno = 0;
  EXPECT_EQ(::recv(sockets.at(1U), &received, sizeof(received), MSG_DONTWAIT),
            -1);
  EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
}

TEST(WorkerProtocol, FinalSendCrossingDeadlineFailsClosedWithoutRetry) {
  ScopedSocketPair sockets;
  ProtocolDeadlineHookState state;
  const auto synthetic_now = std::chrono::steady_clock::now();
  state.now_ticks.store(synthetic_now.time_since_epoch().count(),
                        std::memory_order_release);
  state.deadline = synthetic_now + std::chrono::seconds(1);
  state.crossing_point =
      WorkerProtocolDeadlineTestPoint::WriteProgressAfterSend;
  const WorkerProtocolDeadlineTestHooks hooks{&state, protocol_test_now,
                                              cross_protocol_test_deadline};
  bool timed_out = false;

  {
    const ScopedWorkerProtocolDeadlineTestHooks scoped_hooks(&hooks);
    try {
      write_worker_frame(sockets.at(0U), WorkerMessageKind::Heartbeat, {},
                         state.deadline);
    } catch (const WorkerProtocolTimeout&) {
      timed_out = true;
    }
  }

  EXPECT_TRUE(timed_out);
  const WorkerProtocolFrame delivered =
      read_worker_frame(sockets.at(1U), protocol_deadline());
  EXPECT_EQ(delivered.kind, WorkerMessageKind::Heartbeat);
  EXPECT_TRUE(delivered.payload.empty());
}

TEST(WorkerProtocol, RoundTripsCompletionReadyAcknowledgementIdentity) {
  ScopedSocketPair sockets;
  const JobSpec spec(GraphArtifactId("graph.protocol"), 7,
                     OutputSlotId("image.final"), protocol_resources());
  const AttemptIdentity identity = protocol_identity(spec);

  send_worker_identity(sockets.at(0U), WorkerMessageKind::CompletionReady,
                       identity, protocol_deadline());
  const WorkerProtocolFrame frame =
      read_worker_frame(sockets.at(1U), protocol_deadline());

  EXPECT_EQ(frame.kind, WorkerMessageKind::CompletionReady);
  EXPECT_EQ(decode_worker_identity(frame, WorkerMessageKind::CompletionReady),
            identity);
}

TEST(WorkerProtocol, RebuildsTightImageIntoIndependentCpuOwner) {
  ScopedSocketPair sockets;
  auto spec = std::make_shared<const JobSpec>(GraphArtifactId("graph.protocol"),
                                              7, OutputSlotId("image.final"),
                                              protocol_resources());
  auto assignment_and_plane = prepared_assignment_with_data_plane(spec);
  JobAttemptReport sent;
  sent.identity = assignment_and_plane.first.assignment.identity;
  sent.outcome = JobAttemptOutcome::Succeeded;
  sent.settled = true;
  sent.failure = JobAttemptFailure::None;
  sent.image = make_aligned_cpu_image_buffer(2, 2, 3, DataType::UINT8, 64U);
  const ImageBuffer source = *sent.image;
  auto* sent_bytes = static_cast<std::byte*>(sent.image->data.get());
  for (std::size_t row = 0U; row < 2U; ++row) {
    for (std::size_t column = 0U; column < 6U; ++column) {
      sent_bytes[row * sent.image->step + column] =
          static_cast<std::byte>(row * 10U + column);
    }
  }

  CollectedWorkerOutput staged = stage_and_collect_output(
      &assignment_and_plane.second, *spec,
      assignment_and_plane.first.data_plane.output, &sent);
  const std::optional<WorkerOutputDataReference>& output = staged.reference;
  const PreparedWorkerReport prepared{sent, output};
  send_worker_report(sockets.at(0U), prepared, *spec,
                     assignment_and_plane.first.data_plane.output,
                     protocol_deadline());
  const WorkerProtocolFrame frame =
      read_worker_frame(sockets.at(1U), protocol_deadline());
  PreparedWorkerReport received_metadata = decode_worker_report(
      frame, *spec, assignment_and_plane.first.data_plane.output);
  const JobAttemptReport received =
      assignment_and_plane.second.materialize_report(
          std::move(received_metadata.report), received_metadata.output,
          std::move(staged.image), staged.received_bytes, staged.digest);

  ASSERT_TRUE(received.image.has_value());
  EXPECT_NE(received.image->data.get(), source.data.get());
  EXPECT_EQ(image_buffer_row_bytes(*received.image), 6U);
  for (std::size_t row = 0U; row < 2U; ++row) {
    EXPECT_EQ(std::memcmp(
                  image_buffer_row_data(*received.image, static_cast<int>(row)),
                  image_buffer_row_data(source, static_cast<int>(row)), 6U),
              0);
  }
}

TEST(WorkerProtocol, MaterializesCheckpointLargerThanControlBound) {
  ScopedSocketPair sockets;
  constexpr std::size_t kCheckpointBytes =
      kMaximumWorkerControlPayloadBytes * 2U;
  const ArtifactId checkpoint_id("artifact.protocol.bulk-checkpoint");
  auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.protocol.bulk-checkpoint"), 7,
      OutputSlotId("image.final"), frame_bound_resources(), checkpoint_id);
  auto assignment_and_plane = prepared_assignment_with_data_plane(
      spec, std::make_shared<const ArtifactRecord>(
                maximum_assignment_checkpoint(*spec, kCheckpointBytes)));
  const WorkerProtocolFrame frame =
      encode_worker_assignment(assignment_and_plane.first);
  ASSERT_LT(frame.payload.size(), kMaximumWorkerControlPayloadBytes);

  send_worker_assignment(sockets.at(0U), assignment_and_plane.first,
                         protocol_deadline());
  PreparedWorkerAssignment decoded =
      receive_worker_assignment(sockets.at(1U), protocol_deadline());
  ASSERT_EQ(decoded.assignment.checkpoint, nullptr);
  decoded.assignment.checkpoint = transfer_checkpoint_for_test(
      &assignment_and_plane.second, decoded.assignment, decoded.data_plane,
      assignment_and_plane.first.assignment.checkpoint->payload);

  ASSERT_NE(decoded.assignment.checkpoint, nullptr);
  EXPECT_EQ(decoded.assignment.checkpoint->payload.size(), kCheckpointBytes);
  EXPECT_EQ(
      decoded.assignment.checkpoint->receipt.content_digest,
      assignment_and_plane.first.assignment.checkpoint->receipt.content_digest);
}

TEST(WorkerProtocol, RejectsImageBytesAtControlEncoder) {
  const JobSpec spec(GraphArtifactId("graph.protocol.image-rejected"), 7,
                     OutputSlotId("image.final"), protocol_resources());
  JobAttemptReport report = frame_bound_success_report(protocol_identity(spec),
                                                       "must stage first", 8U);
  PreparedWorkerReport prepared{std::move(report), std::nullopt};
  WorkerOutputStageReference output_stage;
  output_stage.reference_id = "worker-data-v1.test";
  output_stage.output_slot_id = spec.output_slot_id();
  output_stage.maximum_payload_bytes = protocol_resources().output_bytes;

  EXPECT_THROW(encode_worker_report(prepared, spec, output_stage),
               std::invalid_argument);
}

TEST(WorkerProtocol, SuccessfulCandidateAboveResourcesBecomesTypedFailure) {
  JobResourceRequest resources = protocol_resources();
  resources.output_bytes = 1U;
  resources.staging_bytes = 1U;
  resources.retention_bytes = 1U;
  auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.protocol.resource-bound"), 7,
      OutputSlotId("image.final"), resources);
  auto assignment_and_plane = prepared_assignment_with_data_plane(spec);
  JobAttemptReport report = frame_bound_success_report(
      assignment_and_plane.first.assignment.identity, "candidate computed", 2U);

  const CollectedWorkerOutput staged = stage_and_collect_output(
      &assignment_and_plane.second, *spec,
      assignment_and_plane.first.data_plane.output, &report);
  const std::optional<WorkerOutputDataReference>& output = staged.reference;
  const PreparedWorkerReport prepared{report, output};
  const WorkerProtocolFrame frame = encode_worker_report(
      prepared, *spec, assignment_and_plane.first.data_plane.output);
  const PreparedWorkerReport decoded = decode_worker_report(
      frame, *spec, assignment_and_plane.first.data_plane.output);

  EXPECT_EQ(decoded.report.identity, report.identity);
  EXPECT_EQ(decoded.report.outcome, JobAttemptOutcome::Failed);
  EXPECT_TRUE(decoded.report.settled);
  EXPECT_EQ(decoded.report.failure, JobAttemptFailure::Compute);
  EXPECT_EQ(decoded.report.message,
            "worker candidate image exceeds accepted artifact data-plane "
            "bounds");
  EXPECT_FALSE(decoded.report.image.has_value());
  EXPECT_FALSE(decoded.output.has_value());
}

TEST(WorkerProtocol, RejectsOutputDigestThatDoesNotMatchStagedBytes) {
  auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.protocol.digest-mismatch"), 7,
      OutputSlotId("image.final"), protocol_resources());
  auto assignment_and_plane = prepared_assignment_with_data_plane(spec);
  JobAttemptReport report = frame_bound_success_report(
      assignment_and_plane.first.assignment.identity, "candidate computed", 8U);
  CollectedWorkerOutput staged = stage_and_collect_output(
      &assignment_and_plane.second, *spec,
      assignment_and_plane.first.data_plane.output, &report);
  std::optional<WorkerOutputDataReference> output = staged.reference;
  ASSERT_TRUE(output.has_value());
  output->content_digest.bytes.at(0U) ^= std::byte{0x01};

  EXPECT_THROW(assignment_and_plane.second.materialize_report(
                   std::move(report), output, std::move(staged.image),
                   staged.received_bytes, staged.digest),
               WorkerArtifactDataPlaneError);
}

TEST(WorkerProtocol, RejectsRealCheckpointReferenceMismatchBeforeLaneRead) {
  const ArtifactId checkpoint_id("artifact.protocol.reference-checkpoint");
  auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.protocol.checkpoint-reference-mismatch"), 7,
      OutputSlotId("image.final"), protocol_resources(), checkpoint_id);
  auto assignment_and_plane = prepared_assignment_with_data_plane(
      spec, std::make_shared<const ArtifactRecord>(
                maximum_assignment_checkpoint(*spec, 32U)));
  JobAssignment worker_assignment = assignment_and_plane.first.assignment;
  worker_assignment.checkpoint.reset();
  WorkerDataPlaneAssignment mismatched = assignment_and_plane.first.data_plane;
  ASSERT_TRUE(mismatched.checkpoint.has_value());
  mismatched.checkpoint->reference_id.append(".stale");

  try {
    static_cast<void>(materialize_worker_checkpoint(
        assignment_and_plane.second.worker_checkpoint_descriptor(),
        worker_assignment, mismatched));
    FAIL() << "checkpoint reference mismatch was accepted";
  } catch (const WorkerArtifactDataPlaneError& error) {
    EXPECT_NE(
        std::string(error.what()).find("worker checkpoint metadata is invalid"),
        std::string::npos);
  }
}

TEST(WorkerProtocol,
     RejectsRealCheckpointContentMismatchOnlyInsideKillableLaneReader) {
  const ArtifactId checkpoint_id("artifact.protocol.content-checkpoint");
  auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.protocol.checkpoint-content-mismatch"), 7,
      OutputSlotId("image.final"), protocol_resources(), checkpoint_id);
  auto checkpoint = std::make_shared<ArtifactRecord>(
      maximum_assignment_checkpoint(*spec, 32U));
  auto assignment_and_plane =
      prepared_assignment_with_data_plane(spec, checkpoint);
  checkpoint->payload.at(0U) ^= std::byte{0x01};

  EXPECT_NO_THROW(encode_worker_assignment(assignment_and_plane.first));
  JobAssignment worker_assignment = assignment_and_plane.first.assignment;
  worker_assignment.checkpoint.reset();
  try {
    static_cast<void>(transfer_checkpoint_for_test(
        &assignment_and_plane.second, worker_assignment,
        assignment_and_plane.first.data_plane, checkpoint->payload));
    FAIL() << "checkpoint content mismatch was accepted";
  } catch (const WorkerArtifactDataPlaneError& error) {
    EXPECT_EQ(std::string(error.what()),
              "worker checkpoint content digest is inconsistent");
  }
}

TEST(WorkerProtocol, RejectsRealOutputReferenceMismatchAtExactStageJoin) {
  auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.protocol.output-reference-mismatch"), 7,
      OutputSlotId("image.final"), protocol_resources());
  auto assignment_and_plane = prepared_assignment_with_data_plane(spec);
  JobAttemptReport report =
      frame_bound_success_report(assignment_and_plane.first.assignment.identity,
                                 "candidate computed", 32U);
  CollectedWorkerOutput staged = stage_and_collect_output(
      &assignment_and_plane.second, *spec,
      assignment_and_plane.first.data_plane.output, &report);
  ASSERT_TRUE(staged.reference.has_value());
  staged.reference->reference_id.append(".stale");

  try {
    static_cast<void>(assignment_and_plane.second.materialize_report(
        std::move(report), staged.reference, std::move(staged.image),
        staged.received_bytes, staged.digest));
    FAIL() << "output reference mismatch was accepted";
  } catch (const WorkerArtifactDataPlaneError& error) {
    EXPECT_EQ(std::string(error.what()),
              "worker output metadata does not join its assigned stage");
  }
}

TEST(WorkerProtocol, RejectsRealOutputDescriptorMismatchAfterReferenceJoin) {
  auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.protocol.output-descriptor-mismatch"), 7,
      OutputSlotId("image.final"), protocol_resources());
  auto assignment_and_plane = prepared_assignment_with_data_plane(spec);
  JobAttemptReport report =
      frame_bound_success_report(assignment_and_plane.first.assignment.identity,
                                 "candidate computed", 32U);
  CollectedWorkerOutput staged = stage_and_collect_output(
      &assignment_and_plane.second, *spec,
      assignment_and_plane.first.data_plane.output, &report);
  ASSERT_TRUE(staged.reference.has_value());
  ++staged.reference->descriptor.width;

  try {
    static_cast<void>(assignment_and_plane.second.materialize_report(
        std::move(report), staged.reference, std::move(staged.image),
        staged.received_bytes, staged.digest));
    FAIL() << "output descriptor mismatch was accepted";
  } catch (const WorkerArtifactDataPlaneError& error) {
    EXPECT_NE(
        std::string(error.what()).find("worker output descriptor is invalid"),
        std::string::npos);
  }
}

TEST(WorkerProtocol, RejectsUnsupportedVersionBeforePayloadAllocation) {
  ScopedSocketPair sockets;
  const std::array<std::byte, 12U> header{
      std::byte{0x50}, std::byte{0x53}, std::byte{0x57}, std::byte{0x31},
      std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x03},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
  write_raw(sockets.at(0U), header);
  EXPECT_THROW(read_worker_frame(sockets.at(1U), protocol_deadline()),
               WorkerProtocolError);
}

TEST(WorkerProtocol, RejectsOversizedDeclaredPayloadBeforeRead) {
  ScopedSocketPair sockets;
  const std::array<std::byte, 12U> header{
      std::byte{0x50}, std::byte{0x53}, std::byte{0x57}, std::byte{0x31},
      std::byte{0x00}, std::byte{0x02}, std::byte{0x00}, std::byte{0x03},
      std::byte{0x00}, std::byte{0x02}, std::byte{0x00}, std::byte{0x01}};
  write_raw(sockets.at(0U), header);
  EXPECT_THROW(read_worker_frame(sockets.at(1U), protocol_deadline()),
               WorkerProtocolError);
}

TEST(WorkerProtocol, RejectsWorkerControlledImageShapeBeyondJobBounds) {
  ScopedSocketPair sockets;
  auto spec = std::make_shared<const JobSpec>(GraphArtifactId("graph.protocol"),
                                              7, OutputSlotId("image.final"),
                                              protocol_resources());
  auto assignment_and_plane = prepared_assignment_with_data_plane(spec);
  JobAttemptReport sent;
  sent.identity = assignment_and_plane.first.assignment.identity;
  sent.outcome = JobAttemptOutcome::Succeeded;
  sent.settled = true;
  sent.failure = JobAttemptFailure::None;
  sent.image = make_aligned_cpu_image_buffer(1, 1, 1, DataType::UINT8, 64U);
  const CollectedWorkerOutput staged = stage_and_collect_output(
      &assignment_and_plane.second, *spec,
      assignment_and_plane.first.data_plane.output, &sent);
  const std::optional<WorkerOutputDataReference>& output = staged.reference;
  ASSERT_TRUE(output.has_value());
  const PreparedWorkerReport prepared{sent, output};
  send_worker_report(sockets.at(0U), prepared, *spec,
                     assignment_and_plane.first.data_plane.output,
                     protocol_deadline());
  WorkerProtocolFrame frame =
      read_worker_frame(sockets.at(1U), protocol_deadline());

  const AttemptIdentity& identity = sent.identity;
  const std::size_t dimension_offset =
      4U + identity.tenant_id.value().size() + 4U +
      identity.job_id.value().size() + identity.job_spec_digest.bytes.size() +
      4U + identity.attempt_id.value().size() + 4U +
      identity.worker_instance_id.value().size() + sizeof(std::uint64_t) + 3U +
      4U + sent.message.size() + 1U + 4U + output->reference_id.size() + 4U +
      output->output_slot_id.value().size();
  overwrite_u32(&frame.payload, dimension_offset, 2U << 20U);
  EXPECT_THROW(decode_worker_report(
                   frame, *spec, assignment_and_plane.first.data_plane.output),
               WorkerProtocolError);
}

}  // namespace
}  // namespace ps::server
