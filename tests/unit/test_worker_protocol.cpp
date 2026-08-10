/**
 * @file test_worker_protocol.cpp
 * @brief Verifies bounded Issue #100 worker protocol reconstruction.
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
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "server/worker_process_launch.hpp"  // NOLINT(build/include_subdir)
#include "server/worker_protocol.hpp"        // NOLINT(build/include_subdir)

namespace ps::server {
namespace {

TEST(WorkerProcessLaunch, RoundTripsManagerDeadlinesWithoutLegacyCaps) {
  const WorkerProcessLaunchOptions expected{3, std::chrono::milliseconds(12345),
                                            std::chrono::milliseconds(4321)};
  WorkerProcessLaunchArguments arguments =
      make_worker_process_launch_arguments(expected);
  std::string executable = "photospider-worker";
  std::array<char*, 4U> argv{executable.data(), arguments.control_fd.data(),
                             arguments.startup_timeout.data(),
                             arguments.io_timeout.data()};

  const WorkerProcessLaunchOptions parsed = parse_worker_process_launch_options(
      static_cast<int>(argv.size()), argv.data());

  EXPECT_EQ(parsed.control_fd, expected.control_fd);
  EXPECT_EQ(parsed.startup_timeout, expected.startup_timeout);
  EXPECT_EQ(parsed.io_timeout, expected.io_timeout);
}

TEST(WorkerProcessLaunch,
     RejectsMissingReorderedMalformedOrNonPositiveArguments) {
  std::string executable = "photospider-worker";
  std::string control = "--control-fd=3";
  std::string startup = "--startup-timeout-ms=12000";
  std::string io = "--io-timeout-ms=3500";
  std::array<char*, 4U> complete{executable.data(), control.data(),
                                 startup.data(), io.data()};

  EXPECT_THROW(parse_worker_process_launch_options(2, complete.data()),
               std::invalid_argument);
  std::swap(complete[2], complete[3]);
  EXPECT_THROW(parse_worker_process_launch_options(
                   static_cast<int>(complete.size()), complete.data()),
               std::invalid_argument);
  std::swap(complete[2], complete[3]);
  startup = "--startup-timeout-ms=0";
  EXPECT_THROW(parse_worker_process_launch_options(
                   static_cast<int>(complete.size()), complete.data()),
               std::invalid_argument);
  startup = "--startup-timeout-ms=12000";
  io = "--io-timeout-ms=3500ms";
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
 * @brief Builds one valid identity with caller-selected encoded field lengths.
 * @param spec Exact JobSpec whose digest enters the tuple.
 * @param padding Number of safe suffix bytes added to every opaque text id.
 * @return Complete valid attempt identity.
 * @throws Identity allocation or validation failures unchanged.
 */
AttemptIdentity padded_protocol_identity(const JobSpec& spec,
                                         std::size_t padding) {
  AttemptIdentity identity = protocol_identity(spec);
  identity.tenant_id = TenantId("tenant.frame." + std::string(padding, 't'));
  identity.job_id = JobId("job.frame." + std::string(padding, 'j'));
  identity.attempt_id =
      JobAttemptId("attempt.frame." + std::string(padding, 'a'));
  identity.worker_instance_id =
      WorkerInstanceId("worker.frame." + std::string(padding, 'w'));
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
  PreparedWorkerAssignment sent;
  sent.assignment.identity = protocol_identity(*spec);
  sent.assignment.spec = spec;
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
  EXPECT_EQ(received.graph.yaml_path, sent.graph.yaml_path);
  EXPECT_EQ(received.heartbeat_interval, sent.heartbeat_interval);
}

TEST(WorkerProtocol, StatefulDecoderPreservesHeaderAndPayloadAcrossTimeouts) {
  ScopedSocketPair sockets;
  const std::array<std::byte, 18U> frame_bytes{
      std::byte{0x50}, std::byte{0x53}, std::byte{0x57}, std::byte{0x31},
      std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x03},
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

TEST(WorkerProtocol, RebuildsTightImageIntoIndependentCpuOwner) {
  ScopedSocketPair sockets;
  const JobSpec spec(GraphArtifactId("graph.protocol"), 7,
                     OutputSlotId("image.final"), protocol_resources());
  JobAttemptReport sent;
  sent.identity = protocol_identity(spec);
  sent.outcome = JobAttemptOutcome::Succeeded;
  sent.settled = true;
  sent.failure = JobAttemptFailure::None;
  sent.image = make_aligned_cpu_image_buffer(2, 2, 3, DataType::UINT8, 64U);
  auto* sent_bytes = static_cast<std::byte*>(sent.image->data.get());
  for (std::size_t row = 0U; row < 2U; ++row) {
    for (std::size_t column = 0U; column < 6U; ++column) {
      sent_bytes[row * sent.image->step + column] =
          static_cast<std::byte>(row * 10U + column);
    }
  }

  send_worker_report(sockets.at(0U), sent, spec, protocol_deadline());
  const WorkerProtocolFrame frame =
      read_worker_frame(sockets.at(1U), protocol_deadline());
  const JobAttemptReport received = decode_worker_report(frame, spec);

  ASSERT_TRUE(received.image.has_value());
  EXPECT_NE(received.image->data.get(), sent.image->data.get());
  EXPECT_EQ(image_buffer_row_bytes(*received.image), 6U);
  for (std::size_t row = 0U; row < 2U; ++row) {
    EXPECT_EQ(
        std::memcmp(
            image_buffer_row_data(*received.image, static_cast<int>(row)),
            image_buffer_row_data(*sent.image, static_cast<int>(row)), 6U),
        0);
  }
}

TEST(WorkerProtocol,
     CompleteReportFrameAcceptsExactBoundaryAndTypesOneByteOver) {
  /**
   * @brief Variable identity and diagnostic sizes for aggregate accounting.
   * @throws Nothing for aggregate initialization and value operations.
   */
  struct FrameProfile final {
    /** @brief Suffix bytes added to every opaque identity field. */
    std::size_t identity_padding;
    /** @brief Success diagnostic retained in the boundary-size calculation. */
    std::string diagnostic;
  };
  const std::array<FrameProfile, 2U> profiles{{
      {0U, ""},
      {73U, std::string(777U, 'd')},
  }};
  const JobSpec spec(GraphArtifactId("graph.protocol.frame-bound"), 7,
                     OutputSlotId("image.final"), frame_bound_resources());

  for (const FrameProfile& profile : profiles) {
    JobAttemptReport probe = frame_bound_success_report(
        padded_protocol_identity(spec, profile.identity_padding),
        profile.diagnostic, 1U);
    const WorkerProtocolFrame probe_frame = encode_worker_report(probe, spec);
    ASSERT_GT(probe_frame.payload.size(), 1U);
    const std::size_t report_overhead = probe_frame.payload.size() - 1U;
    ASSERT_LT(report_overhead, kMaximumWorkerFramePayloadBytes);
    const std::size_t exact_image_bytes =
        kMaximumWorkerFramePayloadBytes - report_overhead;

    {
      JobAttemptReport exact = frame_bound_success_report(
          padded_protocol_identity(spec, profile.identity_padding),
          profile.diagnostic, exact_image_bytes);
      WorkerProtocolFrame exact_frame = encode_worker_report(exact, spec);
      EXPECT_EQ(exact_frame.payload.size(), kMaximumWorkerFramePayloadBytes);
      exact.image.reset();
      const JobAttemptReport exact_decoded =
          decode_worker_report(exact_frame, spec);
      ASSERT_TRUE(exact_decoded.image.has_value());
      EXPECT_EQ(image_buffer_row_bytes(*exact_decoded.image),
                exact_image_bytes);
      EXPECT_EQ(exact_decoded.outcome, JobAttemptOutcome::Succeeded);
      EXPECT_EQ(exact_decoded.failure, JobAttemptFailure::None);
    }

    JobAttemptReport oversized = frame_bound_success_report(
        padded_protocol_identity(spec, profile.identity_padding),
        profile.diagnostic, exact_image_bytes + 1U);
    const WorkerProtocolFrame oversized_frame =
        encode_worker_report(oversized, spec);
    const JobAttemptReport oversized_decoded =
        decode_worker_report(oversized_frame, spec);
    EXPECT_EQ(oversized_decoded.identity, oversized.identity);
    EXPECT_EQ(oversized_decoded.outcome, JobAttemptOutcome::Failed);
    EXPECT_TRUE(oversized_decoded.settled);
    EXPECT_EQ(oversized_decoded.failure, JobAttemptFailure::Compute);
    EXPECT_FALSE(oversized_decoded.image.has_value());
    EXPECT_EQ(oversized_decoded.message,
              "worker candidate image exceeds private Report bounds");
  }
}

TEST(WorkerProtocol, SuccessfulCandidateAboveJobResourcesBecomesTypedFailure) {
  JobResourceRequest resources = protocol_resources();
  resources.output_bytes = 1U;
  resources.staging_bytes = 1U;
  resources.retention_bytes = 1U;
  const JobSpec spec(GraphArtifactId("graph.protocol.resource-bound"), 7,
                     OutputSlotId("image.final"), resources);
  const JobAttemptReport report = frame_bound_success_report(
      protocol_identity(spec), "candidate computed", 2U);

  const WorkerProtocolFrame frame = encode_worker_report(report, spec);
  const JobAttemptReport decoded = decode_worker_report(frame, spec);

  EXPECT_EQ(decoded.identity, report.identity);
  EXPECT_EQ(decoded.outcome, JobAttemptOutcome::Failed);
  EXPECT_TRUE(decoded.settled);
  EXPECT_EQ(decoded.failure, JobAttemptFailure::Compute);
  EXPECT_EQ(decoded.message,
            "worker candidate image exceeds private Report bounds");
  EXPECT_FALSE(decoded.image.has_value());
}

TEST(WorkerProtocol, RejectsUnsupportedVersionBeforePayloadAllocation) {
  ScopedSocketPair sockets;
  const std::array<std::byte, 12U> header{
      std::byte{0x50}, std::byte{0x53}, std::byte{0x57}, std::byte{0x31},
      std::byte{0x00}, std::byte{0x02}, std::byte{0x00}, std::byte{0x03},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
  write_raw(sockets.at(0U), header);
  EXPECT_THROW(read_worker_frame(sockets.at(1U), protocol_deadline()),
               WorkerProtocolError);
}

TEST(WorkerProtocol, RejectsOversizedDeclaredPayloadBeforeRead) {
  ScopedSocketPair sockets;
  const std::array<std::byte, 12U> header{
      std::byte{0x50}, std::byte{0x53}, std::byte{0x57}, std::byte{0x31},
      std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x03},
      std::byte{0x04}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};
  write_raw(sockets.at(0U), header);
  EXPECT_THROW(read_worker_frame(sockets.at(1U), protocol_deadline()),
               WorkerProtocolError);
}

TEST(WorkerProtocol, RejectsWorkerControlledImageShapeBeyondJobBounds) {
  ScopedSocketPair sockets;
  const JobSpec spec(GraphArtifactId("graph.protocol"), 7,
                     OutputSlotId("image.final"), protocol_resources());
  JobAttemptReport sent;
  sent.identity = protocol_identity(spec);
  sent.outcome = JobAttemptOutcome::Succeeded;
  sent.settled = true;
  sent.failure = JobAttemptFailure::None;
  sent.image = make_aligned_cpu_image_buffer(1, 1, 1, DataType::UINT8, 64U);
  send_worker_report(sockets.at(0U), sent, spec, protocol_deadline());
  WorkerProtocolFrame frame =
      read_worker_frame(sockets.at(1U), protocol_deadline());

  const AttemptIdentity& identity = sent.identity;
  const std::size_t dimension_offset =
      4U + identity.tenant_id.value().size() + 4U +
      identity.job_id.value().size() + identity.job_spec_digest.bytes.size() +
      4U + identity.attempt_id.value().size() + 4U +
      identity.worker_instance_id.value().size() + sizeof(std::uint64_t) + 3U +
      4U + sent.message.size() + 1U;
  overwrite_u32(&frame.payload, dimension_offset, 2U << 20U);
  EXPECT_THROW(decode_worker_report(frame, spec), WorkerProtocolError);
}

}  // namespace
}  // namespace ps::server
