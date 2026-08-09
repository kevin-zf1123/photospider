/**
 * @file main.cpp
 * @brief Runs exactly one private Issue #100 Embedded Host worker assignment.
 */
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "server/embedded_job_worker.hpp"  // NOLINT(build/include_subdir)
#include "server/worker_protocol.hpp"      // NOLINT(build/include_subdir)

namespace ps::server {
namespace {

/** @brief Maximum initial assignment wait in the single-use worker. */
constexpr std::chrono::seconds kAssignmentTimeout{10};
/** @brief Per-message worker-side write bound. */
constexpr std::chrono::seconds kWorkerWriteTimeout{2};
/** @brief Short control-read slice used to revisit heartbeat state. */
constexpr std::chrono::milliseconds kControlPollInterval{20};

/**
 * @brief Parses the sole `--control-fd=<number>` worker argument.
 * @param argc Process argument count.
 * @param argv Process argument vector.
 * @return Nonnegative private control descriptor.
 * @throws std::invalid_argument for any extra, missing, malformed, or standard
 * descriptor value.
 */
int parse_control_descriptor(int argc, char* argv[]) {
  constexpr std::string_view kPrefix = "--control-fd=";
  if (argc != 2 || argv == nullptr || argv[1] == nullptr) {
    throw std::invalid_argument(
        "photospider-worker requires one --control-fd argument");
  }
  const std::string argument(argv[1]);
  if (argument.compare(0U, kPrefix.size(), kPrefix) != 0) {
    throw std::invalid_argument(
        "photospider-worker control argument is invalid");
  }
  const std::string number = argument.substr(kPrefix.size());
  std::size_t parsed = 0U;
  const std::int64_t value = std::stoll(number, &parsed, 10);
  if (parsed != number.size() || value < 3 || value > 1024 * 1024) {
    throw std::invalid_argument(
        "photospider-worker control descriptor is out of range");
  }
  return static_cast<int>(value);
}

/**
 * @brief Resolver that exposes only one manager-prepared graph occurrence.
 * @throws Construction may allocate while retaining exact values.
 */
class PreparedGraphResolver final : public GraphArtifactResolver {
 public:
  /**
   * @brief Retains one exact graph identity and trusted prepared material.
   * @param graph_id Exact immutable JobSpec graph identity.
   * @param graph Trusted manager-prepared local graph material.
   * @throws Nothing after argument construction.
   */
  PreparedGraphResolver(GraphArtifactId graph_id,
                        ResolvedGraphArtifact graph) noexcept
      : graph_id_(std::move(graph_id)), graph_(std::move(graph)) {}

  /**
   * @brief Resolves only the exact graph identity assigned to this process.
   * @param graph_artifact_id Candidate immutable graph identity.
   * @return Prepared graph for an exact match, otherwise a closed rejection.
   * @throws std::bad_alloc when creating mismatch diagnostics exhausts memory.
   */
  ResolvedGraphArtifact resolve(
      const GraphArtifactId& graph_artifact_id) const override {
    if (graph_artifact_id == graph_id_) {
      return graph_;
    }
    ResolvedGraphArtifact rejected;
    rejected.message =
        "worker resolver rejected an unassigned graph artifact identity";
    return rejected;
  }

 private:
  /** @brief Sole graph identity authorized for this process. */
  GraphArtifactId graph_id_;
  /** @brief Immutable trusted manager-prepared graph material. */
  ResolvedGraphArtifact graph_;
};

/**
 * @brief Sends one exact identity frame under the sole socket-write mutex.
 * @param fd Connected private manager socket.
 * @param kind AssignmentAccepted or Heartbeat.
 * @param identity Exact process assignment identity.
 * @param write_mutex Non-null sole write serializer.
 * @throws Protocol timeout/channel failures unchanged.
 */
void send_identity_locked(int fd, WorkerMessageKind kind,
                          const AttemptIdentity& identity,
                          std::mutex* write_mutex) {
  if (write_mutex == nullptr) {
    throw std::invalid_argument("worker write mutex is null");
  }
  std::lock_guard<std::mutex> lock(*write_mutex);
  send_worker_identity(fd, kind, identity,
                       std::chrono::steady_clock::now() + kWorkerWriteTimeout);
}

/**
 * @brief Runs heartbeat production and exact cooperative-cancel observation.
 * @param fd Connected private manager socket.
 * @param identity Exact process assignment identity.
 * @param heartbeat_interval Positive manager-requested cadence.
 * @param done Non-null main-thread completion flag.
 * @param cancellation_requested Non-null monotonic cancellation output.
 * @param control_failed Non-null monotonic protocol/channel failure output.
 * @param write_mutex Non-null sole socket-write serializer.
 * @return Nothing when `done` becomes true or control fails.
 * @throws Nothing; all failures set `control_failed` and request cancellation.
 */
void run_control_loop(int fd, const AttemptIdentity& identity,
                      std::chrono::milliseconds heartbeat_interval,
                      const std::atomic<bool>* done,
                      std::atomic<bool>* cancellation_requested,
                      std::atomic<bool>* control_failed,
                      std::mutex* write_mutex) noexcept {
  try {
    auto next_heartbeat = std::chrono::steady_clock::now() + heartbeat_interval;
    while (!done->load(std::memory_order_acquire)) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_heartbeat) {
        send_identity_locked(fd, WorkerMessageKind::Heartbeat, identity,
                             write_mutex);
        next_heartbeat = std::chrono::steady_clock::now() + heartbeat_interval;
      }
      try {
        WorkerProtocolFrame frame = read_worker_frame(
            fd, std::min(next_heartbeat, std::chrono::steady_clock::now() +
                                             kControlPollInterval));
        if (decode_worker_identity(frame, WorkerMessageKind::Cancel) !=
            identity) {
          throw WorkerProtocolError(
              "worker cancel identity does not match its exact lease");
        }
        cancellation_requested->store(true, std::memory_order_release);
      } catch (const WorkerProtocolTimeout&) {
        // The short slice exists only to revisit done/heartbeat state.
      }
    }
  } catch (...) {
    cancellation_requested->store(true, std::memory_order_release);
    control_failed->store(true, std::memory_order_release);
  }
}

/**
 * @brief Executes one received assignment and emits at most one report.
 * @param fd Connected private manager socket.
 * @return Process exit code: zero only after one report was sent cleanly.
 * @throws Assignment receive/validation and thread creation failures unchanged.
 */
int run_one_assignment(int fd) {
  PreparedWorkerAssignment prepared = receive_worker_assignment(
      fd, std::chrono::steady_clock::now() + kAssignmentTimeout);
  validate_attempt_identity(prepared.assignment.identity);
  if (prepared.assignment.spec == nullptr ||
      prepared.assignment.spec->digest() !=
          prepared.assignment.identity.job_spec_digest) {
    throw WorkerProtocolError("worker assignment lacks a joined JobSpec");
  }

  std::mutex write_mutex;
  send_identity_locked(fd, WorkerMessageKind::AssignmentAccepted,
                       prepared.assignment.identity, &write_mutex);
  std::atomic<bool> done{false};
  std::atomic<bool> cancellation_requested{false};
  std::atomic<bool> control_failed{false};
  std::thread control(run_control_loop, fd, prepared.assignment.identity,
                      prepared.heartbeat_interval, &done,
                      &cancellation_requested, &control_failed, &write_mutex);

  JobAttemptReport report;
  try {
    auto resolver = std::make_shared<const PreparedGraphResolver>(
        prepared.assignment.spec->graph_artifact_id(),
        std::move(prepared.graph));
    EmbeddedHostJobWorker worker(std::move(resolver));
    report = worker.execute(prepared.assignment, [&] {
      return cancellation_requested.load(std::memory_order_acquire);
    });
  } catch (...) {
    done.store(true, std::memory_order_release);
    control.join();
    throw;
  }
  done.store(true, std::memory_order_release);
  control.join();
  if (control_failed.load(std::memory_order_acquire)) {
    return 4;
  }
  {
    std::lock_guard<std::mutex> lock(write_mutex);
    send_worker_report(fd, report, *prepared.assignment.spec,
                       std::chrono::steady_clock::now() + kWorkerWriteTimeout);
  }
  static_cast<void>(::shutdown(fd, SHUT_WR));
  return 0;
}

}  // namespace
}  // namespace ps::server

/**
 * @brief Process entry point for the non-installed single-assignment worker.
 * @param argc Process argument count.
 * @param argv Process argument vector.
 * @return Zero only after clean assignment/report/channel completion.
 * @throws Nothing; diagnostics are printed and mapped to a nonzero exit.
 */
int main(int argc, char* argv[]) {
  int control_fd = -1;
  try {
    control_fd = ps::server::parse_control_descriptor(argc, argv);
    const int result = ps::server::run_one_assignment(control_fd);
    while (::close(control_fd) < 0 && errno == EINTR) {
    }
    return result;
  } catch (const std::exception& error) {
    std::cerr << "photospider-worker: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "photospider-worker: non-standard failure\n";
  }
  if (control_fd >= 0) {
    while (::close(control_fd) < 0 && errno == EINTR) {
    }
  }
  return EXIT_FAILURE;
}
