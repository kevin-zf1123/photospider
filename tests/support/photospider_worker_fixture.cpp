/**
 * @file photospider_worker_fixture.cpp
 * @brief Provides deterministic real-process faults for WorkerManager tests.
 */
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "server/worker_process_launch.hpp"  // NOLINT(build/include_subdir)
#include "server/worker_protocol.hpp"        // NOLINT(build/include_subdir)

namespace ps::server {
namespace {

/** @brief Fast heartbeat cadence used by active fixture modes. */
constexpr std::chrono::milliseconds kFixtureHeartbeatCadence{25};
/** @brief Cancel relay gap crossing one slice but fitting three times in 100ms.
 */
constexpr std::chrono::milliseconds kFixtureCancelFragmentGap{27};
/** @brief Fixed private worker frame header width. */
constexpr std::size_t kFixtureFrameHeaderBytes = 12U;
/** @brief One bulk byte beyond the former aggregate control-frame ceiling. */
constexpr std::size_t kBulkPayloadAboveFormerControlBytes = (64U << 20U) + 1U;
/** @brief Graph-artifact mode prefix carrying one forbidden inherited fd. */
constexpr std::string_view kClosedDescriptorPrefix = "fixture.fd.closed.";
/** @brief Mode proving manager deadlines cross exec without legacy caps. */
constexpr auto kLaunchDeadlineMode = "fixture.launch.deadlines.12000.3500";
/** @brief Startup bound encoded by the launch-deadline fixture mode. */
constexpr std::chrono::milliseconds kExpectedLaunchStartupTimeout{12000};
/** @brief I/O bound encoded by the launch-deadline fixture mode. */
constexpr std::chrono::milliseconds kExpectedLaunchIoTimeout{3500};
/** @brief Mode that blocks in trusted filesystem I/O after assignment accept.
 */
constexpr std::string_view kFilesystemBlockMode = "fixture.fs.block";
/** @brief Retry mode whose fresh generation blocks on trusted FIFO input. */
constexpr std::string_view kRetryFilesystemHoldMode = "fixture.retry.hold";
/** @brief Mode producing bytes at the former aggregate control-frame cap. */
constexpr std::string_view kFormerControlBoundOutputMode =
    "fixture.former-control-bound-output";  // NOLINT(whitespace/indent_namespace)
/**
 * @brief Natural signal delay that remains inside the long cancellation grace.
 * @note The gap is deliberately much larger than one supervisor poll slice so
 * a shorter ordinary deadline deterministically exercises cancellation
 * precedence for both channel-loss and candidate-Report paths without
 * exposing process or signal authority to the test.
 */
constexpr std::chrono::milliseconds kDelayedCancelSignalExit{300};

/**
 * @brief Parses an optional descriptor non-inheritance fixture mode.
 * @param mode Exact trusted graph-artifact mode from the assignment.
 * @return Empty for another fixture mode, otherwise the nonnegative descriptor
 * that must be absent after exec.
 * @throws std::invalid_argument when the prefixed decimal value is malformed
 * or cannot name a process descriptor.
 * @throws std::out_of_range when decimal conversion exceeds `long long`.
 * @note The returned number grants no capability; `run_fixture()` uses only
 * `F_GETFD` to prove that the manager did not pass the descriptor through
 * exec.
 */
std::optional<int> parse_closed_descriptor_mode(std::string_view mode) {
  if (mode.compare(0U, kClosedDescriptorPrefix.size(),
                   kClosedDescriptorPrefix) != 0) {
    return std::nullopt;
  }
  const std::string encoded(mode.substr(kClosedDescriptorPrefix.size()));
  std::size_t parsed = 0U;
  const std::int64_t value = std::stoll(encoded, &parsed, 10);
  if (encoded.empty() || parsed != encoded.size() || value < 0 ||
      value > std::numeric_limits<int>::max()) {
    throw std::invalid_argument(
        "worker fixture closed descriptor mode is invalid");
  }
  return static_cast<int>(value);
}

/**
 * @brief Creates one settled success report whose pixels encode the child PID.
 * @param assignment Exact current assignment.
 * @return One-by-one four-channel CPU result.
 * @throws Image allocation failures unchanged.
 */
JobAttemptReport success_report(const JobAssignment& assignment) {
  JobAttemptReport report;
  report.identity = assignment.identity;
  report.outcome = JobAttemptOutcome::Succeeded;
  report.settled = true;
  report.failure = JobAttemptFailure::None;
  report.image = make_aligned_cpu_image_buffer(1, 1, 4, DataType::UINT8, 64U);
  const std::uint32_t pid = static_cast<std::uint32_t>(::getpid());
  std::memcpy(report.image->data.get(), &pid, sizeof(pid));
  return report;
}

/**
 * @brief Creates a successful candidate above the former control-frame cap.
 * @param assignment Exact current assignment.
 * @return Settled success candidate that must use only the artifact data plane.
 * @throws std::overflow_error if the historical cap cannot fit one image
 * dimension; image allocation failures propagate unchanged.
 * @note The staged payload is one byte above the old 64-MiB aggregate frame
 * cap, while the Report control frame remains bounded metadata only.
 */
JobAttemptReport former_control_bound_output_report(
    const JobAssignment& assignment) {
  const std::size_t payload_bytes = kBulkPayloadAboveFormerControlBytes;
  if (payload_bytes >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::overflow_error(
        "worker fixture checkpoint payload exceeds image width");
  }
  JobAttemptReport report;
  report.identity = assignment.identity;
  report.outcome = JobAttemptOutcome::Succeeded;
  report.settled = true;
  report.failure = JobAttemptFailure::None;
  report.image = make_aligned_cpu_image_buffer(static_cast<int>(payload_bytes),
                                               1, 1, DataType::UINT8, 64U);
  return report;
}

/**
 * @brief Creates one settled cooperative-cancellation report.
 * @param assignment Exact current assignment.
 * @return Closed cancelled report without image.
 * @throws Identity/message allocation failures unchanged.
 */
JobAttemptReport cancelled_report(const JobAssignment& assignment) {
  JobAttemptReport report;
  report.identity = assignment.identity;
  report.outcome = JobAttemptOutcome::Cancelled;
  report.settled = true;
  report.failure = JobAttemptFailure::CancellationObserved;
  report.message = "fixture observed exact cooperative cancellation";
  return report;
}

/**
 * @brief Creates one settled worker-owned failure report for cancel races.
 * @param assignment Exact current assignment.
 * @return Closed Failed/Compute report without image.
 * @throws Identity/message allocation failures unchanged.
 */
JobAttemptReport failed_report(const JobAssignment& assignment) {
  JobAttemptReport report;
  report.identity = assignment.identity;
  report.outcome = JobAttemptOutcome::Failed;
  report.settled = true;
  report.failure = JobAttemptFailure::Compute;
  report.message = "fixture preserved worker failure after cancel send closed";
  return report;
}

/**
 * @brief Closes one fixture-owned descriptor through interrupted syscalls.
 * @param fd Descriptor or negative sentinel.
 * @return Nothing.
 * @throws Nothing.
 */
void close_fixture_fd(int fd) noexcept {
  if (fd < 0) {
    return;
  }
  while (::close(fd) < 0 && errno == EINTR) {
  }
}

/**
 * @brief Owns one fixture-side inherited data-plane descriptor.
 * @throws Nothing for construction, reset, and destruction.
 * @note Ownership is cleared before one non-retried close attempt so an
 * interrupted close cannot target a later numeric-descriptor reuse.
 */
class FixtureDataDescriptor final {
 public:
  /**
   * @brief Takes ownership of one exact inherited descriptor.
   * @param descriptor Nonnegative descriptor or invalid sentinel.
   * @throws Nothing.
   */
  explicit FixtureDataDescriptor(int descriptor) noexcept
      : descriptor_(descriptor) {}

  /**
   * @brief Closes any retained descriptor at scope exit.
   * @throws Nothing; ownership clears before one ignored close result.
   */
  ~FixtureDataDescriptor() noexcept { reset(); }

  /**
   * @brief Prevents duplicate descriptor ownership.
   * @param other Existing owner that remains unchanged.
   * @throws Nothing because the operation is deleted.
   */
  FixtureDataDescriptor(const FixtureDataDescriptor& other) = delete;
  /**
   * @brief Prevents duplicate descriptor assignment.
   * @param other Existing owner that remains unchanged.
   * @return No assignment result because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  FixtureDataDescriptor& operator=(const FixtureDataDescriptor& other) = delete;

  /**
   * @brief Returns the exact retained descriptor without transfer.
   * @return Nonnegative descriptor or -1.
   * @throws Nothing.
   * @note The borrowed descriptor remains valid only until `reset()` or owner
   * destruction.
   */
  int get() const noexcept { return descriptor_; }

  /**
   * @brief Clears ownership and closes the exact retained descriptor once.
   * @return Nothing.
   * @throws Nothing; close results are ignored without retry.
   */
  void reset() noexcept {
    const int owned = std::exchange(descriptor_, -1);
    if (owned >= 0) {
      static_cast<void>(::close(owned));
    }
  }

 private:
  /** @brief Sole exact descriptor owned by this fixture scope. */
  int descriptor_ = -1;
};

/**
 * @brief Stages one fixture candidate and returns metadata-only report facts.
 * @param output_data Non-null exact inherited output-stage descriptor owner.
 * @param report Complete fixture report whose image, if any, is consumed.
 * @param spec Exact immutable assignment JobSpec.
 * @param output_stage Exact manager-assigned private stage metadata.
 * @return Image-free report plus optional staged-output metadata.
 * @throws std::invalid_argument for null ownership or invalid report metadata.
 * @throws WorkerArtifactDataPlaneError and system/allocation/overflow failures
 * from bounded staging unchanged.
 * @note The write descriptor is closed before returning, preventing fixture
 * code from mutating the staged occurrence after control metadata is formed.
 */
PreparedWorkerReport stage_fixture_report(
    FixtureDataDescriptor* output_data, JobAttemptReport report,
    const JobSpec& spec, const WorkerOutputStageReference& output_stage) {
  if (output_data == nullptr) {
    throw std::invalid_argument("fixture output descriptor owner is null");
  }
  std::optional<WorkerOutputDataReference> output =
      stage_worker_output(output_data->get(), spec, output_stage, &report);
  output_data->reset();
  return PreparedWorkerReport{std::move(report), std::move(output)};
}

/**
 * @brief Stages and sends one fixture report over metadata-only control.
 * @param fd Connected manager control socket.
 * @param output_data Non-null exact inherited output-stage owner.
 * @param report Complete fixture report whose image, if any, is consumed.
 * @param spec Exact immutable assignment JobSpec.
 * @param output_stage Exact manager-assigned private stage metadata.
 * @param io_timeout Positive manager-selected write bound.
 * @return Nothing after stage closure and complete Report-frame transport.
 * @throws Staging, encoding, deadline, and channel failures unchanged.
 * @note Bulk bytes never enter `fd`; only the returned data reference does.
 */
void send_fixture_report(int fd, FixtureDataDescriptor* output_data,
                         JobAttemptReport report, const JobSpec& spec,
                         const WorkerOutputStageReference& output_stage,
                         std::chrono::milliseconds io_timeout) {
  const PreparedWorkerReport prepared =
      stage_fixture_report(output_data, std::move(report), spec, output_stage);
  send_worker_report(
      fd, prepared, spec, output_stage,
      checked_worker_deadline(std::chrono::steady_clock::now(), io_timeout));
}

/**
 * @brief Waits for one nonblocking fixture descriptor readiness direction.
 * @param fd Connected descriptor.
 * @param events `POLLIN` or `POLLOUT`.
 * @return Nothing once the descriptor is ready or hung up.
 * @throws std::runtime_error after one second or on a poll failure.
 */
void wait_fixture_ready(int fd, std::int16_t events) {
  for (;;) {
    pollfd descriptor{fd, events, 0};
    const int result = ::poll(&descriptor, 1U, 1000);
    if (result > 0) {
      return;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    throw std::runtime_error(result == 0 ? "fixture relay I/O timed out"
                                         : "fixture relay poll failed");
  }
}

/**
 * @brief Reads one exact byte range from a connected fixture socket.
 * @param fd Readable connected descriptor.
 * @param destination Writable range of at least `size` bytes.
 * @param size Exact number of bytes to receive.
 * @return Nothing after the range is full.
 * @throws std::runtime_error for premature EOF or syscall failure.
 */
void read_exact_fixture_bytes(int fd, std::byte* destination,
                              std::size_t size) {
  std::size_t offset = 0U;
  while (offset != size) {
    const ssize_t received = ::read(fd, destination + offset, size - offset);
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      wait_fixture_ready(fd, POLLIN);
      continue;
    }
    if (received <= 0) {
      throw std::runtime_error("fixture captured frame ended early");
    }
    offset += static_cast<std::size_t>(received);
  }
}

/**
 * @brief Writes one exact byte range to a connected fixture socket.
 * @param fd Writable connected descriptor.
 * @param source Readable range of at least `size` bytes.
 * @param size Exact number of bytes to send.
 * @return Nothing after the range is consumed.
 * @throws std::runtime_error for a closed peer or syscall failure.
 */
void write_exact_fixture_bytes(int fd, const std::byte* source,
                               std::size_t size) {
  std::size_t offset = 0U;
  while (offset != size) {
    const ssize_t written = ::write(fd, source + offset, size - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      wait_fixture_ready(fd, POLLOUT);
      continue;
    }
    if (written <= 0) {
      throw std::runtime_error("fixture fragmented-frame write failed");
    }
    offset += static_cast<std::size_t>(written);
  }
}

/**
 * @brief Copies one exact byte count between connected fixture sockets.
 * @param source Readable connected descriptor.
 * @param destination Writable connected descriptor.
 * @param size Positive number of bytes to transfer.
 * @return Nothing after the exact range is forwarded.
 * @throws std::runtime_error for premature EOF or syscall failure.
 */
void forward_exact_bytes(int source, int destination, std::size_t size) {
  std::array<std::byte, 4096U> buffer{};
  std::size_t remaining = size;
  while (remaining != 0U) {
    const std::size_t requested = std::min(remaining, buffer.size());
    read_exact_fixture_bytes(source, buffer.data(), requested);
    write_exact_fixture_bytes(destination, buffer.data(), requested);
    remaining -= requested;
  }
}

/**
 * @brief Relays one manager Cancel frame through timeout-crossing fragments.
 * @param source Manager-connected descriptor that supplies exactly one frame.
 * @param destination Local receiver descriptor.
 * @return Nothing after forwarding the complete header and bounded payload.
 * @throws std::runtime_error for a short, oversized, or failed relay.
 * @note Pieces are 5 header bytes, 7 header bytes, 3 payload bytes, then the
 * remaining payload. Each gap exceeds the 25-ms receiver slice while all
 * three remain well below the test's 300-ms cooperative-cancel grace.
 */
void relay_fragmented_cancel(int source, int destination) {
  std::array<std::byte, kFixtureFrameHeaderBytes> header{};
  read_exact_fixture_bytes(source, header.data(), 5U);
  write_exact_fixture_bytes(destination, header.data(), 5U);
  std::this_thread::sleep_for(kFixtureCancelFragmentGap);
  read_exact_fixture_bytes(source, header.data() + 5U, 7U);
  write_exact_fixture_bytes(destination, header.data() + 5U, 7U);
  const std::uint32_t payload_size =
      (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[8U]))
       << 24U) |
      (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[9U]))
       << 16U) |
      (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[10U]))
       << 8U) |
      static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[11U]));
  if (payload_size < 3U || payload_size > kMaximumWorkerControlPayloadBytes) {
    throw std::runtime_error("fixture cancel payload size is invalid");
  }
  std::this_thread::sleep_for(kFixtureCancelFragmentGap);
  forward_exact_bytes(source, destination, 3U);
  std::this_thread::sleep_for(kFixtureCancelFragmentGap);
  forward_exact_bytes(source, destination,
                      static_cast<std::size_t>(payload_size) - 3U);
  static_cast<void>(::shutdown(destination, SHUT_WR));
}

/**
 * @brief Copies every remaining captured frame byte without another delay.
 * @param source Readable descriptor closed by its peer after one frame.
 * @param destination Writable manager descriptor.
 * @return Nothing after clean captured EOF.
 * @throws std::runtime_error for a read or write failure.
 */
void forward_remaining_bytes(int source, int destination) {
  std::array<std::byte, 4096U> buffer{};
  for (;;) {
    const ssize_t received = ::read(source, buffer.data(), buffer.size());
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      wait_fixture_ready(source, POLLIN);
      continue;
    }
    if (received == 0) {
      return;
    }
    if (received < 0) {
      throw std::runtime_error("fixture captured-frame read failed");
    }
    write_exact_fixture_bytes(destination, buffer.data(),
                              static_cast<std::size_t>(received));
  }
}

/**
 * @brief Sends one valid report with header and payload crossing poll slices.
 * @param fd Connected manager socket.
 * @param report Complete metadata-only exact report.
 * @param spec Immutable JobSpec that bounds the report.
 * @param output_stage Exact assigned output-stage metadata.
 * @param io_timeout Positive manager-selected write bound.
 * @return Nothing after the captured frame is forwarded in four pieces.
 * @throws std::invalid_argument for an invalid report contract or I/O duration.
 * @throws std::overflow_error for an unrepresentable deadline or report image
 * size.
 * @throws std::length_error when report encoding exceeds a private bound.
 * @throws std::bad_alloc when validation or report encoding exhausts memory.
 * @throws std::system_error when the capture socket pair cannot be created.
 * @throws WorkerProtocolTimeout or WorkerChannelError when capture transport
 * fails.
 * @throws std::runtime_error when captured bytes cannot be forwarded exactly.
 * @note The 35-ms gaps exceed the manager's 20-ms read slice while the total
 * stays below the fixture heartbeat timeout. Both capture descriptors are
 * fixture-owned and closed on success or exception; `fd` remains caller-owned.
 */
void send_fragmented_report(int fd, const PreparedWorkerReport& report,
                            const JobSpec& spec,
                            const WorkerOutputStageReference& output_stage,
                            std::chrono::milliseconds io_timeout) {
  int capture[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, capture) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "create fixture report capture socketpair");
  }
  try {
    send_worker_report(
        capture[0], report, spec, output_stage,
        checked_worker_deadline(std::chrono::steady_clock::now(), io_timeout));
    static_cast<void>(::shutdown(capture[0], SHUT_WR));
    forward_exact_bytes(capture[1], fd, 5U);
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    forward_exact_bytes(capture[1], fd, 7U);
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    forward_exact_bytes(capture[1], fd, 3U);
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    forward_remaining_bytes(capture[1], fd);
  } catch (...) {
    close_fixture_fd(capture[0]);
    close_fixture_fd(capture[1]);
    throw;
  }
  close_fixture_fd(capture[0]);
  close_fixture_fd(capture[1]);
}

/**
 * @brief Sends one fixture heartbeat for the exact current lease.
 * @param fd Connected manager socket.
 * @param identity Exact current attempt identity.
 * @param io_timeout Positive manager-selected write bound.
 * @return Nothing after the complete Heartbeat frame is written.
 * @throws std::invalid_argument for an invalid descriptor, identity, or I/O
 * duration.
 * @throws std::overflow_error if the captured base cannot represent the I/O
 * deadline.
 * @throws std::bad_alloc when deadline diagnostics or frame encoding exhaust
 * memory.
 * @throws WorkerProtocolTimeout or WorkerChannelError when bounded transport
 * fails.
 * @note The connected descriptor remains owned by the fixture control flow.
 */
void send_heartbeat(int fd, const AttemptIdentity& identity,
                    std::chrono::milliseconds io_timeout) {
  send_worker_identity(
      fd, WorkerMessageKind::Heartbeat, identity,
      checked_worker_deadline(std::chrono::steady_clock::now(), io_timeout));
}

/**
 * @brief Waits for exact cancellation while optionally continuing heartbeats.
 * @param fd Connected manager socket.
 * @param assignment Exact current assignment.
 * @param ignore_cancel Whether to ignore valid cancel indefinitely.
 * @param io_timeout Positive manager-selected write bound.
 * @return Cooperative cancelled report when not ignoring; never returns in
 * ignore mode unless the manager closes or signals the process.
 * @throws WorkerProtocolTimeout when a heartbeat write deadline expires.
 * @throws WorkerProtocolError, WorkerProtocolEof, or WorkerChannelError for a
 * malformed Cancel, fresh channel EOF, or channel failure.
 * @throws std::invalid_argument for an invalid descriptor, identity, or
 * deadline duration.
 * @throws std::overflow_error if a captured base cannot represent a heartbeat
 * or poll deadline.
 * @throws std::bad_alloc when deadline diagnostics, frame processing, or report
 * construction exhausts memory.
 * @note One decoder retains partial Cancel bytes across heartbeat poll slices;
 * read-slice timeouts are contained and retried. The descriptor is borrowed and
 * remains open for the caller's report path.
 */
JobAttemptReport wait_for_cancel(int fd, const JobAssignment& assignment,
                                 bool ignore_cancel,
                                 std::chrono::milliseconds io_timeout) {
  auto next_heartbeat = checked_worker_deadline(
      std::chrono::steady_clock::now(), kFixtureHeartbeatCadence);
  WorkerFrameDecoder frame_decoder;
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_heartbeat) {
      send_heartbeat(fd, assignment.identity, io_timeout);
      next_heartbeat = checked_worker_deadline(std::chrono::steady_clock::now(),
                                               kFixtureHeartbeatCadence);
    }
    try {
      const WorkerProtocolFrame frame = frame_decoder.read_frame(
          fd, std::min(next_heartbeat,
                       checked_worker_deadline(std::chrono::steady_clock::now(),
                                               kFixtureHeartbeatCadence)));
      if (decode_worker_identity(frame, WorkerMessageKind::Cancel) !=
          assignment.identity) {
        throw WorkerProtocolError(
            "fixture cancel identity does not match assignment");
      }
      if (!ignore_cancel) {
        return cancelled_report(assignment);
      }
    } catch (const WorkerProtocolTimeout&) {
    }
  }
}

/**
 * @brief Waits for one exact cancel and then exits only after manager closure.
 * @param fd Connected manager socket.
 * @param assignment Exact current assignment.
 * @param io_timeout Positive manager-selected write bound.
 * @return Nothing after the cancel was validated and clean channel EOF arrived.
 * @throws WorkerProtocolTimeout when a heartbeat write deadline expires.
 * @throws WorkerProtocolError or WorkerChannelError for malformed/early
 * channel termination or another channel failure.
 * @throws std::invalid_argument for an invalid descriptor, identity, or
 * deadline duration.
 * @throws std::overflow_error if a captured base cannot represent a heartbeat
 * or poll deadline.
 * @throws std::bad_alloc when deadline diagnostics or frame processing exhausts
 * memory.
 * @note Heartbeats continue only until cancellation is observed. The final
 * return lets the fixture process reach normal `exit(0)` after channel
 * revocation, which the manager test seam can retain as a zombie. Read-slice
 * timeouts and the expected post-cancel EOF are contained; `fd` is borrowed.
 */
void wait_for_cancel_then_channel_close(int fd, const JobAssignment& assignment,
                                        std::chrono::milliseconds io_timeout) {
  auto next_heartbeat = checked_worker_deadline(
      std::chrono::steady_clock::now(), kFixtureHeartbeatCadence);
  bool cancel_observed = false;
  WorkerFrameDecoder frame_decoder;
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (!cancel_observed && now >= next_heartbeat) {
      send_heartbeat(fd, assignment.identity, io_timeout);
      next_heartbeat = checked_worker_deadline(std::chrono::steady_clock::now(),
                                               kFixtureHeartbeatCadence);
    }
    try {
      const WorkerProtocolFrame frame = frame_decoder.read_frame(
          fd, checked_worker_deadline(std::chrono::steady_clock::now(),
                                      kFixtureHeartbeatCadence));
      if (decode_worker_identity(frame, WorkerMessageKind::Cancel) !=
          assignment.identity) {
        throw WorkerProtocolError(
            "fixture cancel identity does not match assignment");
      }
      cancel_observed = true;
    } catch (const WorkerProtocolTimeout&) {
    } catch (const WorkerProtocolEof&) {
      if (!cancel_observed) {
        throw WorkerProtocolError(
            "fixture channel closed before exact cancellation");
      }
      return;
    }
  }
}

/**
 * @brief Receives one manager Cancel through a fragmenting local relay.
 * @param fd Connected manager socket supplying the valid Cancel frame.
 * @param assignment Exact current assignment expected by the receiver.
 * @param io_timeout Positive manager-selected write bound.
 * @return Cooperative cancellation report after all fragments reassemble.
 * @throws WorkerProtocolTimeout when a receiver heartbeat write expires.
 * @throws WorkerProtocolError, WorkerProtocolEof, or WorkerChannelError for a
 * malformed Cancel, fresh relay EOF, or channel failure.
 * @throws std::invalid_argument for an invalid identity or deadline duration.
 * @throws std::overflow_error if a captured base cannot represent a heartbeat
 * or poll deadline.
 * @throws std::bad_alloc when relay, deadline, frame, or report state cannot be
 * retained.
 * @throws std::system_error when the relay socket or thread cannot be created.
 * @throws std::runtime_error when exact fragment relay fails.
 * @note The relay owns only the manager-to-worker byte direction; the report
 * remains sent on `fd` so the supervisor exercises its normal receive path.
 * Relay and receive exceptions are retained until the relay thread is joined;
 * both local descriptors are then closed before rethrow. `fd` remains borrowed.
 */
JobAttemptReport receive_fragmented_cancel(
    int fd, const JobAssignment& assignment,
    std::chrono::milliseconds io_timeout) {
  int relay_pair[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, relay_pair) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "create fixture cancel relay socketpair");
  }

  std::exception_ptr relay_error;
  std::thread relay;
  try {
    relay = std::thread([&] {
      try {
        relay_fragmented_cancel(fd, relay_pair[0]);
      } catch (...) {
        relay_error = std::current_exception();
        static_cast<void>(::shutdown(relay_pair[0], SHUT_WR));
      }
    });
  } catch (...) {
    close_fixture_fd(relay_pair[0]);
    close_fixture_fd(relay_pair[1]);
    throw;
  }

  std::optional<JobAttemptReport> report;
  std::exception_ptr receive_error;
  try {
    report = wait_for_cancel(relay_pair[1], assignment, false, io_timeout);
  } catch (...) {
    receive_error = std::current_exception();
  }
  relay.join();
  close_fixture_fd(relay_pair[0]);
  close_fixture_fd(relay_pair[1]);
  if (relay_error != nullptr) {
    std::rethrow_exception(relay_error);
  }
  if (receive_error != nullptr) {
    std::rethrow_exception(receive_error);
  }
  if (!report.has_value()) {
    throw std::runtime_error("fixture fragmented cancel produced no report");
  }
  return std::move(*report);
}

/**
 * @brief Runs an active heartbeat loop for a bounded duration.
 * @param fd Connected manager socket.
 * @param identity Exact current attempt identity.
 * @param duration Total active duration.
 * @param io_timeout Positive manager-selected write bound.
 * @return Nothing after duration expiry.
 * @throws WorkerProtocolTimeout or WorkerChannelError when a heartbeat write
 * fails.
 * @throws std::invalid_argument when the descriptor, active duration, I/O
 * duration, or identity is invalid.
 * @throws std::overflow_error when a captured base cannot represent the active
 * or per-heartbeat I/O deadline.
 * @throws std::bad_alloc when deadline diagnostics or heartbeat encoding
 * exhausts memory.
 * @note The total deadline is captured once; each heartbeat receives its own
 * bounded write deadline. The connected descriptor remains caller-owned.
 */
void heartbeat_for(int fd, const AttemptIdentity& identity,
                   std::chrono::milliseconds duration,
                   std::chrono::milliseconds io_timeout) {
  const auto deadline =
      checked_worker_deadline(std::chrono::steady_clock::now(), duration);
  while (std::chrono::steady_clock::now() < deadline) {
    send_heartbeat(fd, identity, io_timeout);
    std::this_thread::sleep_for(kFixtureHeartbeatCadence);
  }
}

/**
 * @brief Sends a deliberately invalid-magic empty frame.
 * @param fd Connected manager socket.
 * @return Nothing after exactly twelve bytes are written.
 * @throws std::runtime_error for a short/failed write.
 */
void send_malformed_frame(int fd) {
  const std::array<std::byte, 12U> frame{
      std::byte{0x00}, std::byte{0x53}, std::byte{0x57}, std::byte{0x31},
      std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x03},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
  std::size_t offset = 0U;
  while (offset != frame.size()) {
    const ssize_t written =
        ::write(fd, frame.data() + offset, frame.size() - offset);
    if (written <= 0) {
      throw std::runtime_error("fixture malformed-frame write failed");
    }
    offset += static_cast<std::size_t>(written);
  }
}

/**
 * @brief Reads one byte from manager-prepared trusted filesystem material.
 * @param path Nonempty prepared FIFO or regular-file path.
 * @return Zero after exactly one byte; a stable nonzero fixture status on
 * open, read, or empty-file failure.
 * @throws Nothing.
 * @note A FIFO with a connected writer that sends no data blocks this function
 * after exec and acceptance, allowing the manager to prove bounded signalling
 * and exact reaping around trusted I/O.
 */
int run_filesystem_block_probe(const std::string& path) noexcept {
  if (path.empty()) {
    return 33;
  }
  int descriptor = -1;
  do {
    descriptor = ::open(path.c_str(), O_RDONLY);
  } while (descriptor < 0 && errno == EINTR);
  if (descriptor < 0) {
    return 34;
  }
  char byte = 0;
  ssize_t received = -1;
  do {
    received = ::read(descriptor, &byte, sizeof(byte));
  } while (received < 0 && errno == EINTR);
  close_fixture_fd(descriptor);
  return received == 1 ? 0 : 35;
}

/**
 * @brief Executes one graph-id-selected deterministic process behavior.
 * @param launch Exact validated manager-selected bootstrap policy.
 * @return Exact intended process exit code.
 * @throws WorkerProtocolError and its timeout, EOF, or channel subclasses for
 * assignment, cancellation, acceptance, heartbeat, or report transport failure.
 * @throws std::invalid_argument for an invalid control descriptor, malformed
 * mode, identity/report contract, or deadline duration.
 * @throws std::out_of_range when a descriptor-mode decimal is not
 * representable.
 * @throws std::overflow_error for an unrepresentable deadline, report image, or
 * checkpoint-test dimension.
 * @throws std::length_error when report encoding exceeds a private bound.
 * @throws std::bad_alloc when deadline diagnostics, assignment, relay, report,
 * or image state cannot be retained.
 * @throws std::system_error when a relay socket or thread cannot be created.
 * @throws std::runtime_error when deterministic relay or malformed-frame I/O
 * fails.
 * @note Cancel-race modes retain all PID, self-signal, channel, and process
 * lifetime authority inside this fixture; the calling test only owns Job
 * submission, cancellation intent, and terminal observation. The function first
 * receives and accepts one assignment, then selects exactly one graph-id mode;
 * deliberate signal/infinite-loop modes may not return. `launch.control_fd`
 * remains owned by `main()` unless a selected fault mode closes it
 * deliberately.
 */
int run_fixture(const WorkerProcessLaunchOptions& launch) {
  FixtureDataDescriptor checkpoint_data(launch.checkpoint_data_fd);
  FixtureDataDescriptor output_data(launch.output_data_fd);
  PreparedWorkerAssignment prepared = receive_worker_assignment(
      launch.control_fd,
      checked_worker_deadline(std::chrono::steady_clock::now(),
                              launch.startup_timeout));
  prepared.assignment.checkpoint = materialize_worker_checkpoint(
      checkpoint_data.get(), prepared.assignment, prepared.data_plane);
  checkpoint_data.reset();
  const JobAssignment& assignment = prepared.assignment;
  const std::string& mode = assignment.spec->graph_artifact_id().value();
  if (mode == kLaunchDeadlineMode &&
      (launch.startup_timeout != kExpectedLaunchStartupTimeout ||
       launch.io_timeout != kExpectedLaunchIoTimeout)) {
    return 32;
  }
  if (mode == "fixture.preaccept.nonzero") {
    return 22;
  }
  send_worker_identity(launch.control_fd, WorkerMessageKind::AssignmentAccepted,
                       assignment.identity,
                       checked_worker_deadline(std::chrono::steady_clock::now(),
                                               launch.io_timeout));

  if (mode == kFilesystemBlockMode ||
      (mode == kRetryFilesystemHoldMode &&
       assignment.identity.worker_lease_generation.value > 1U)) {
    const int filesystem_result =
        run_filesystem_block_probe(prepared.graph.yaml_path);
    if (filesystem_result != 0) {
      return filesystem_result;
    }
  }

  const std::optional<int> forbidden_descriptor =
      parse_closed_descriptor_mode(mode);
  if (forbidden_descriptor.has_value()) {
    errno = 0;
    if (::fcntl(*forbidden_descriptor, F_GETFD) != -1 || errno != EBADF) {
      return 31;
    }
  }

  if (mode == "fixture.nonzero" ||
      ((mode == "fixture.retry" || mode == kRetryFilesystemHoldMode) &&
       assignment.identity.worker_lease_generation.value == 1U)) {
    return 23;
  }
  if (mode == "fixture.signal") {
    static_cast<void>(::kill(::getpid(), SIGKILL));
    return 24;
  }
  if (mode == "fixture.channel") {
    static_cast<void>(::close(launch.control_fd));
    return 0;
  }
  if (mode == "fixture.malformed") {
    send_malformed_frame(launch.control_fd);
    return 0;
  }
  if (mode == "fixture.stall") {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    return 25;
  }
  if (mode == "fixture.runtime") {
    for (;;) {
      send_heartbeat(launch.control_fd, assignment.identity, launch.io_timeout);
      std::this_thread::sleep_for(kFixtureHeartbeatCadence);
    }
  }
  if (mode == "fixture.cooperative") {
    JobAttemptReport report = wait_for_cancel(launch.control_fd, assignment,
                                              false, launch.io_timeout);
    send_fixture_report(launch.control_fd, &output_data, std::move(report),
                        *assignment.spec, prepared.data_plane.output,
                        launch.io_timeout);
    return 0;
  }
  if (mode == "fixture.fragmented.cancel") {
    JobAttemptReport report = receive_fragmented_cancel(
        launch.control_fd, assignment, launch.io_timeout);
    send_fixture_report(launch.control_fd, &output_data, std::move(report),
                        *assignment.spec, prepared.data_plane.output,
                        launch.io_timeout);
    static_cast<void>(::shutdown(launch.control_fd, SHUT_WR));
    return 0;
  }
  if (mode == "fixture.cancel-race.failed-report") {
    static_cast<void>(::shutdown(launch.control_fd, SHUT_RD));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    JobAttemptReport report = failed_report(assignment);
    send_fixture_report(launch.control_fd, &output_data, std::move(report),
                        *assignment.spec, prepared.data_plane.output,
                        launch.io_timeout);
    static_cast<void>(::shutdown(launch.control_fd, SHUT_WR));
    return 0;
  }
  if (mode == "fixture.cancel-race.nonzero") {
    static_cast<void>(::shutdown(launch.control_fd, SHUT_RD));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    return 29;
  }
  if (mode == "fixture.cancel-race.signal") {
    static_cast<void>(::shutdown(launch.control_fd, SHUT_RD));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    static_cast<void>(::kill(::getpid(), SIGKILL));
    return 30;
  }
  if (mode == "fixture.cancel-race.delayed-signal") {
    static_cast<void>(wait_for_cancel(launch.control_fd, assignment, false,
                                      launch.io_timeout));
    std::this_thread::sleep_for(kDelayedCancelSignalExit);
    static_cast<void>(::kill(::getpid(), SIGKILL));
    return 36;
  }
  if (mode == "fixture.cancel-race.report-delayed-signal") {
    JobAttemptReport report = wait_for_cancel(launch.control_fd, assignment,
                                              false, launch.io_timeout);
    send_fixture_report(launch.control_fd, &output_data, std::move(report),
                        *assignment.spec, prepared.data_plane.output,
                        launch.io_timeout);
    std::this_thread::sleep_for(kDelayedCancelSignalExit);
    static_cast<void>(::kill(::getpid(), SIGKILL));
    return 37;
  }
  if (mode == "fixture.cancel-race.channel-close") {
    close_fixture_fd(launch.control_fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    return 0;
  }
  if (mode == "fixture.cancel-race.zero-exit-zombie") {
    wait_for_cancel_then_channel_close(launch.control_fd, assignment,
                                       launch.io_timeout);
    return 0;
  }
  if (mode == "fixture.ignore") {
    static_cast<void>(wait_for_cancel(launch.control_fd, assignment, true,
                                      launch.io_timeout));
    return 26;
  }
  if (mode == "fixture.report.hang") {
    JobAttemptReport report = success_report(assignment);
    send_fixture_report(launch.control_fd, &output_data, std::move(report),
                        *assignment.spec, prepared.data_plane.output,
                        launch.io_timeout);
    std::this_thread::sleep_for(std::chrono::seconds(10));
    return 27;
  }
  if (mode == "fixture.fragmented.report") {
    JobAttemptReport report = success_report(assignment);
    const PreparedWorkerReport prepared_report =
        stage_fixture_report(&output_data, std::move(report), *assignment.spec,
                             prepared.data_plane.output);
    send_fragmented_report(launch.control_fd, prepared_report, *assignment.spec,
                           prepared.data_plane.output, launch.io_timeout);
    static_cast<void>(::shutdown(launch.control_fd, SHUT_WR));
    return 0;
  }
  if (mode == "fixture.slow.success") {
    heartbeat_for(launch.control_fd, assignment.identity,
                  std::chrono::milliseconds(300), launch.io_timeout);
  }
  if (mode == kFormerControlBoundOutputMode) {
    JobAttemptReport report = former_control_bound_output_report(assignment);
    send_fixture_report(launch.control_fd, &output_data, std::move(report),
                        *assignment.spec, prepared.data_plane.output,
                        launch.io_timeout);
    static_cast<void>(::shutdown(launch.control_fd, SHUT_WR));
    return 0;
  }
  if (mode == "fixture.data.digest-mismatch") {
    JobAttemptReport report = success_report(assignment);
    PreparedWorkerReport prepared_report =
        stage_fixture_report(&output_data, std::move(report), *assignment.spec,
                             prepared.data_plane.output);
    if (!prepared_report.output.has_value()) {
      return 38;
    }
    prepared_report.output->content_digest.bytes.at(0U) ^= std::byte{0x01};
    send_worker_report(launch.control_fd, prepared_report, *assignment.spec,
                       prepared.data_plane.output,
                       checked_worker_deadline(std::chrono::steady_clock::now(),
                                               launch.io_timeout));
    static_cast<void>(::shutdown(launch.control_fd, SHUT_WR));
    return 0;
  }
  if (mode == "fixture.checkpoint" && assignment.checkpoint == nullptr) {
    return 28;
  }
  JobAttemptReport report = success_report(assignment);
  send_fixture_report(launch.control_fd, &output_data, std::move(report),
                      *assignment.spec, prepared.data_plane.output,
                      launch.io_timeout);
  static_cast<void>(::shutdown(launch.control_fd, SHUT_WR));
  return 0;
}

}  // namespace
}  // namespace ps::server

/**
 * @brief Entry point for the non-CTest deterministic worker process fixture.
 * @param argc Process argument count.
 * @param argv Process argument vector.
 * @return Mode-selected exit status, or failure for unexpected exceptions.
 * @throws Nothing.
 * @note Unexpected fixture failures emit one bounded stderr diagnostic to make
 * integration-test process exits attributable.
 */
int main(int argc, char* argv[]) {
  int descriptor = -1;
  try {
    const ps::server::WorkerProcessLaunchOptions launch =
        ps::server::parse_worker_process_launch_options(argc, argv);
    descriptor = launch.control_fd;
    const int result = ps::server::run_fixture(launch);
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
    }
    return result;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "worker fixture failed: %s\n", error.what());
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
    }
    return EXIT_FAILURE;
  } catch (...) {
    std::fprintf(stderr, "worker fixture failed: non-standard exception\n");
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
    }
    return EXIT_FAILURE;
  }
}
