/**
 * @file test_worker_protocol.cpp
 * @brief Verifies Issue #105 metadata control and artifact data separation.
 */
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "server/worker_process_launch.hpp"  // NOLINT(build/include_subdir)
#include "server/worker_protocol.hpp"        // NOLINT(build/include_subdir)

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
 * @brief Result of one concurrently staged unit-test candidate.
 * @throws Nothing for default construction; retained values may allocate.
 */
struct CollectedWorkerOutput final {
  /** @brief Metadata emitted by the worker-side staging operation. */
  std::optional<WorkerOutputDataReference> reference;
  /** @brief Exact bytes drained from the manager-side lane. */
  std::vector<std::byte> payload;
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
  ArtifactContentHasher hasher;
  std::exception_ptr sender_failure;
  std::thread sender([&] {
    try {
      collected.reference = stage_worker_output(
          data_plane->worker_output_descriptor(), spec, output_stage, report);
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
      const std::size_t prior_size = collected.payload.size();
      const WorkerDataPlaneIoStatus status =
          data_plane->receive_output_chunk(&collected.payload);
      if (status == WorkerDataPlaneIoStatus::Progress) {
        hasher.update(collected.payload.data() + prior_size,
                      collected.payload.size() - prior_size);
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
          std::move(staged.payload), staged.digest);

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

  EXPECT_THROW(
      assignment_and_plane.second.materialize_report(
          std::move(report), output, std::move(staged.payload), staged.digest),
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
        std::move(report), staged.reference, std::move(staged.payload),
        staged.digest));
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
        std::move(report), staged.reference, std::move(staged.payload),
        staged.digest));
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
