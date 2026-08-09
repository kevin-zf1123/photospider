/**
 * @file photospider_worker_fixture.cpp
 * @brief Provides deterministic real-process faults for WorkerManager tests.
 */
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "server/worker_protocol.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {
namespace {

/** @brief Maximum deterministic fixture assignment wait. */
constexpr std::chrono::seconds kFixtureAssignmentTimeout{5};
/** @brief Per-frame deterministic fixture I/O bound. */
constexpr std::chrono::seconds kFixtureIoTimeout{1};
/** @brief Fast heartbeat cadence used by active fixture modes. */
constexpr std::chrono::milliseconds kFixtureHeartbeatCadence{25};

/**
 * @brief Parses the exact private control descriptor argument.
 * @param argc Process argument count.
 * @param argv Process argument vector.
 * @return Descriptor at or above three.
 * @throws std::invalid_argument for malformed input.
 */
int parse_control_descriptor(int argc, char* argv[]) {
  constexpr std::string_view kPrefix = "--control-fd=";
  if (argc != 2 || argv == nullptr || argv[1] == nullptr) {
    throw std::invalid_argument("worker fixture requires --control-fd");
  }
  const std::string argument(argv[1]);
  if (argument.compare(0U, kPrefix.size(), kPrefix) != 0) {
    throw std::invalid_argument("worker fixture control argument is invalid");
  }
  std::size_t parsed = 0U;
  const std::int64_t value =
      std::stoll(argument.substr(kPrefix.size()), &parsed, 10);
  if (parsed != argument.size() - kPrefix.size() || value < 3 ||
      value > 1024 * 1024) {
    throw std::invalid_argument("worker fixture descriptor is out of range");
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
 * @brief Sends one fixture heartbeat for the exact current lease.
 * @param fd Connected manager socket.
 * @param identity Exact current attempt identity.
 * @throws Protocol I/O failures unchanged.
 */
void send_heartbeat(int fd, const AttemptIdentity& identity) {
  send_worker_identity(fd, WorkerMessageKind::Heartbeat, identity,
                       std::chrono::steady_clock::now() + kFixtureIoTimeout);
}

/**
 * @brief Waits for exact cancellation while optionally continuing heartbeats.
 * @param fd Connected manager socket.
 * @param assignment Exact current assignment.
 * @param ignore_cancel Whether to ignore valid cancel indefinitely.
 * @return Cooperative cancelled report when not ignoring; never returns in
 * ignore mode unless the manager closes or signals the process.
 * @throws Protocol failures unchanged.
 */
JobAttemptReport wait_for_cancel(int fd, const JobAssignment& assignment,
                                 bool ignore_cancel) {
  auto next_heartbeat =
      std::chrono::steady_clock::now() + kFixtureHeartbeatCadence;
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_heartbeat) {
      send_heartbeat(fd, assignment.identity);
      next_heartbeat =
          std::chrono::steady_clock::now() + kFixtureHeartbeatCadence;
    }
    try {
      const WorkerProtocolFrame frame = read_worker_frame(
          fd, std::min(next_heartbeat, std::chrono::steady_clock::now() +
                                           kFixtureHeartbeatCadence));
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
 * @brief Runs an active heartbeat loop for a bounded duration.
 * @param fd Connected manager socket.
 * @param identity Exact current attempt identity.
 * @param duration Total active duration.
 * @return Nothing after duration expiry.
 * @throws Protocol I/O failures unchanged.
 */
void heartbeat_for(int fd, const AttemptIdentity& identity,
                   std::chrono::milliseconds duration) {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    send_heartbeat(fd, identity);
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
 * @brief Executes one graph-id-selected deterministic process behavior.
 * @param fd Connected manager socket.
 * @return Exact intended process exit code.
 * @throws Protocol/assignment failures unchanged.
 */
int run_fixture(int fd) {
  PreparedWorkerAssignment prepared = receive_worker_assignment(
      fd, std::chrono::steady_clock::now() + kFixtureAssignmentTimeout);
  const JobAssignment& assignment = prepared.assignment;
  const std::string& mode = assignment.spec->graph_artifact_id().value();
  if (mode == "fixture.preaccept.nonzero") {
    return 22;
  }
  send_worker_identity(fd, WorkerMessageKind::AssignmentAccepted,
                       assignment.identity,
                       std::chrono::steady_clock::now() + kFixtureIoTimeout);

  if (mode == "fixture.nonzero" ||
      (mode == "fixture.retry" &&
       assignment.identity.worker_lease_generation.value == 1U)) {
    return 23;
  }
  if (mode == "fixture.signal") {
    static_cast<void>(::kill(::getpid(), SIGKILL));
    return 24;
  }
  if (mode == "fixture.channel") {
    static_cast<void>(::close(fd));
    return 0;
  }
  if (mode == "fixture.malformed") {
    send_malformed_frame(fd);
    return 0;
  }
  if (mode == "fixture.stall") {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    return 25;
  }
  if (mode == "fixture.runtime") {
    for (;;) {
      send_heartbeat(fd, assignment.identity);
      std::this_thread::sleep_for(kFixtureHeartbeatCadence);
    }
  }
  if (mode == "fixture.cooperative") {
    const JobAttemptReport report = wait_for_cancel(fd, assignment, false);
    send_worker_report(fd, report, *assignment.spec,
                       std::chrono::steady_clock::now() + kFixtureIoTimeout);
    return 0;
  }
  if (mode == "fixture.ignore") {
    static_cast<void>(wait_for_cancel(fd, assignment, true));
    return 26;
  }
  if (mode == "fixture.report.hang") {
    const JobAttemptReport report = success_report(assignment);
    send_worker_report(fd, report, *assignment.spec,
                       std::chrono::steady_clock::now() + kFixtureIoTimeout);
    std::this_thread::sleep_for(std::chrono::seconds(10));
    return 27;
  }
  if (mode == "fixture.slow.success") {
    heartbeat_for(fd, assignment.identity, std::chrono::milliseconds(300));
  }
  if (mode == "fixture.checkpoint" && assignment.checkpoint == nullptr) {
    return 28;
  }
  const JobAttemptReport report = success_report(assignment);
  send_worker_report(fd, report, *assignment.spec,
                     std::chrono::steady_clock::now() + kFixtureIoTimeout);
  static_cast<void>(::shutdown(fd, SHUT_WR));
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
 */
int main(int argc, char* argv[]) {
  int descriptor = -1;
  try {
    descriptor = ps::server::parse_control_descriptor(argc, argv);
    const int result = ps::server::run_fixture(descriptor);
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
    }
    return result;
  } catch (...) {
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
    }
    return EXIT_FAILURE;
  }
}
