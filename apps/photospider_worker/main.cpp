/**
 * @file main.cpp
 * @brief Runs one metadata-control/data-plane Issue #105 worker assignment.
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
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "server/embedded_job_worker.hpp"  // NOLINT(build/include_subdir)
#include "server/worker_artifact_data_plane.hpp"  // NOLINT(build/include_subdir)
#include "server/worker_process_launch.hpp"  // NOLINT(build/include_subdir)
#include "server/worker_protocol.hpp"        // NOLINT(build/include_subdir)

namespace ps::server {
namespace {

/** @brief Short control-read slice used to revisit heartbeat state. */
constexpr std::chrono::milliseconds kControlPollInterval{20};

/**
 * @brief Owns one exact worker-side inherited data-plane descriptor.
 * @throws Nothing for construction, reset, and destruction.
 * @note Ownership is cleared before one non-retried close attempt so `EINTR`
 * cannot target a later numeric-descriptor reuse.
 */
class WorkerDataDescriptor final {
 public:
  /**
   * @brief Takes ownership of one inherited descriptor.
   * @param descriptor Exact nonnegative descriptor or invalid sentinel.
   * @throws Nothing.
   */
  explicit WorkerDataDescriptor(int descriptor) noexcept
      : descriptor_(descriptor) {}

  /**
   * @brief Closes any retained descriptor at scope exit.
   * @throws Nothing; ownership clears before one ignored close result.
   */
  ~WorkerDataDescriptor() noexcept { reset(); }

  /**
   * @brief Prevents duplicate descriptor ownership.
   * @param other Existing owner that remains unchanged.
   * @throws Nothing because the operation is deleted.
   */
  WorkerDataDescriptor(const WorkerDataDescriptor& other) = delete;
  /**
   * @brief Prevents duplicate descriptor assignment.
   * @param other Existing owner that remains unchanged.
   * @return No assignment result because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  WorkerDataDescriptor& operator=(const WorkerDataDescriptor& other) = delete;

  /**
   * @brief Returns the exact descriptor without transfer.
   * @return Retained descriptor or -1.
   * @throws Nothing.
   * @note The borrowed descriptor remains valid only until `reset()` or owner
   * destruction.
   */
  int get() const noexcept { return descriptor_; }

  /**
   * @brief Clears ownership and performs at most one close attempt.
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
  /** @brief Sole exact inherited descriptor owner. */
  int descriptor_ = -1;
};

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
 * @param io_timeout Positive manager-selected write bound.
 * @param write_mutex Non-null sole write serializer.
 * @return Nothing after the complete identity frame is written under the lock.
 * @throws std::invalid_argument for a null mutex, invalid descriptor,
 * identity/kind, or unsupported I/O duration.
 * @throws std::overflow_error if the captured monotonic base cannot represent
 * the validated I/O deadline.
 * @throws std::bad_alloc when deadline diagnostics or frame encoding exhaust
 * memory.
 * @throws std::system_error when locking the serializer fails.
 * @throws WorkerProtocolTimeout or WorkerChannelError when bounded frame
 * transport fails.
 * @note The descriptor, identity, and mutex remain caller-owned. All worker
 * socket writers share `write_mutex`, so no two frames can interleave.
 */
void send_identity_locked(int fd, WorkerMessageKind kind,
                          const AttemptIdentity& identity,
                          std::chrono::milliseconds io_timeout,
                          std::mutex* write_mutex) {
  if (write_mutex == nullptr) {
    throw std::invalid_argument("worker write mutex is null");
  }
  std::lock_guard<std::mutex> lock(*write_mutex);
  send_worker_identity(
      fd, kind, identity,
      checked_worker_deadline(std::chrono::steady_clock::now(), io_timeout));
}

/**
 * @brief Runs heartbeat production and exact cooperative-cancel observation.
 * @param fd Connected private manager socket.
 * @param identity Exact process assignment identity.
 * @param heartbeat_interval Positive manager-requested cadence.
 * @param io_timeout Positive manager-selected write bound.
 * @param done Non-null main-thread completion flag.
 * @param cancellation_requested Non-null monotonic cancellation output.
 * @param control_failed Non-null monotonic protocol/channel failure output.
 * @param write_mutex Non-null sole socket-write serializer.
 * @param frame_decoder Non-null decoder retained through completion readiness.
 * @return Nothing when `done` becomes true or control fails.
 * @throws Nothing; all failures set `control_failed` and request cancellation.
 * @note One decoder retains partial or complete Cancel header/payload bytes
 * across short poll deadlines while heartbeat and completion observation stay
 * responsive. A Cancel mutates state only while the short absolute acceptance
 * deadline remains active after exact identity decoding.
 * The outer catch contains deadline validation/overflow/allocation, protocol,
 * channel, and mutex failures, so none cross this `noexcept` thread boundary.
 * All pointer arguments remain owned by `run_one_assignment()` until join.
 */
void run_control_loop(int fd, const AttemptIdentity& identity,
                      std::chrono::milliseconds heartbeat_interval,
                      std::chrono::milliseconds io_timeout,
                      const std::atomic<bool>* done,
                      std::atomic<bool>* cancellation_requested,
                      std::atomic<bool>* control_failed,
                      std::mutex* write_mutex,
                      WorkerFrameDecoder* frame_decoder) noexcept {
  try {
    if (frame_decoder == nullptr) {
      throw std::invalid_argument("worker control decoder is null");
    }
    auto next_heartbeat = checked_worker_deadline(
        std::chrono::steady_clock::now(), heartbeat_interval);
    while (!done->load(std::memory_order_acquire)) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_heartbeat) {
        send_identity_locked(fd, WorkerMessageKind::Heartbeat, identity,
                             io_timeout, write_mutex);
        next_heartbeat = checked_worker_deadline(
            std::chrono::steady_clock::now(), heartbeat_interval);
      }
      try {
        const auto read_deadline =
            std::min(next_heartbeat,
                     checked_worker_deadline(std::chrono::steady_clock::now(),
                                             kControlPollInterval));
        WorkerProtocolFrame frame =
            frame_decoder->read_frame(fd, read_deadline);
        if (decode_worker_identity(frame, WorkerMessageKind::Cancel) !=
            identity) {
          throw WorkerProtocolError(
              "worker cancel identity does not match its exact lease");
        }
        if (std::chrono::steady_clock::now() >= read_deadline) {
          throw WorkerProtocolTimeout(
              "worker cancel acceptance deadline expired");
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
 * @brief Waits for manager completion readiness after one terminal Report.
 * @param fd Connected manager control socket.
 * @param identity Exact current worker lease.
 * @param deadline Absolute report/acknowledgement deadline.
 * @param decoder Non-null stateful decoder retained from the control loop.
 * @return Nothing after one exact `CompletionReady` identity.
 * @throws Worker protocol timeout, EOF, channel, malformed-kind, or identity
 * failures unchanged.
 * @note A late exact Cancel may race the stopped heartbeat/control loop. It is
 * consumed without rewriting the already settled Report; all other kinds fail
 * closed. The worker stays signalable throughout this bounded wait.
 */
void await_completion_ready(int fd, const AttemptIdentity& identity,
                            std::chrono::steady_clock::time_point deadline,
                            WorkerFrameDecoder* decoder) {
  if (decoder == nullptr) {
    throw std::invalid_argument("worker completion decoder is null");
  }
  for (;;) {
    const WorkerProtocolFrame frame = decoder->read_frame(fd, deadline);
    if (frame.kind == WorkerMessageKind::CompletionReady) {
      if (decode_worker_identity(frame, WorkerMessageKind::CompletionReady) !=
          identity) {
        throw WorkerProtocolError(
            "worker completion acknowledgement identity does not match its "
            "lease");
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        throw WorkerProtocolTimeout(
            "worker completion acknowledgement deadline expired");
      }
      return;
    }
    if (frame.kind == WorkerMessageKind::Cancel) {
      if (decode_worker_identity(frame, WorkerMessageKind::Cancel) !=
          identity) {
        throw WorkerProtocolError(
            "late worker cancel identity does not match its lease");
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        throw WorkerProtocolTimeout(
            "late worker cancel acceptance deadline expired");
      }
      continue;
    }
    throw WorkerProtocolError(
        "worker received an invalid frame while awaiting completion");
  }
}

/**
 * @brief Executes one received assignment and emits at most one report.
 * @param launch Exact validated manager-selected bootstrap policy.
 * @return Process exit code: zero only after one report was sent cleanly and
 * the manager acknowledged its exact completed metadata/data-plane join.
 * @throws WorkerProtocolError and its timeout, EOF, or channel subclasses for
 * assignment receipt, acceptance, or report transport failure.
 * @throws std::invalid_argument for invalid assignment/report contracts or a
 * deadline duration outside the shared worker bound.
 * @throws std::overflow_error if the captured monotonic base cannot represent
 * a validated launch deadline or report image arithmetic overflows.
 * @throws WorkerArtifactDataPlaneError for checkpoint/output reference,
 * descriptor, size, or digest mismatch.
 * @throws std::bad_alloc when assignment, worker, thread, or report state
 * cannot be retained.
 * @throws std::system_error when control-thread creation or mutex locking
 * fails.
 * @note The control thread contains its own exceptions and maps them to exit
 * code 4. Any exception escaping resolver construction or Embedded Host
 * execution propagates unchanged only after `done` is published and the
 * control thread is joined. The worker remains alive and
 * manager-terminable while its control thread emits authenticated heartbeats
 * during bulk output. It sends metadata first, closes the output lane only
 * after exact bytes, then synchronously awaits `CompletionReady` under a fresh
 * absolute acknowledgement deadline.
 */
int run_one_assignment(const WorkerProcessLaunchOptions& launch) {
  WorkerDataDescriptor checkpoint_data(launch.checkpoint_data_fd);
  WorkerDataDescriptor output_data(launch.output_data_fd);
  PreparedWorkerAssignment prepared = receive_worker_assignment(
      launch.control_fd,
      checked_worker_deadline(std::chrono::steady_clock::now(),
                              launch.startup_timeout));
  validate_attempt_identity(prepared.assignment.identity);
  if (prepared.assignment.spec == nullptr ||
      prepared.assignment.spec->digest() !=
          prepared.assignment.identity.job_spec_digest) {
    throw WorkerProtocolError("worker assignment lacks a joined JobSpec");
  }
  prepared.assignment.checkpoint = materialize_worker_checkpoint(
      checkpoint_data.get(), prepared.assignment, prepared.data_plane);
  checkpoint_data.reset();

  std::mutex write_mutex;
  send_identity_locked(launch.control_fd, WorkerMessageKind::AssignmentAccepted,
                       prepared.assignment.identity, launch.io_timeout,
                       &write_mutex);
  std::atomic<bool> done{false};
  std::atomic<bool> cancellation_requested{false};
  std::atomic<bool> control_failed{false};
  WorkerFrameDecoder control_decoder;
  std::thread control(run_control_loop, launch.control_fd,
                      prepared.assignment.identity, prepared.heartbeat_interval,
                      launch.io_timeout, &done, &cancellation_requested,
                      &control_failed, &write_mutex, &control_decoder);

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
  PreparedWorkerOutputTransfer output_transfer;
  try {
    output_transfer = prepare_worker_output_transfer(
        *prepared.assignment.spec, prepared.data_plane.output, &report);
    {
      std::lock_guard<std::mutex> lock(write_mutex);
      const PreparedWorkerReport prepared_report{std::move(report),
                                                 output_transfer.reference};
      send_worker_report(
          launch.control_fd, prepared_report, *prepared.assignment.spec,
          prepared.data_plane.output,
          checked_worker_deadline(std::chrono::steady_clock::now(),
                                  launch.io_timeout));
    }
    send_worker_output_transfer(output_data.get(), output_transfer);
  } catch (...) {
    done.store(true, std::memory_order_release);
    control.join();
    throw;
  }
  done.store(true, std::memory_order_release);
  control.join();
  output_data.reset();
  if (control_failed.load(std::memory_order_acquire)) {
    return 4;
  }
  const auto completion_deadline = checked_worker_deadline(
      std::chrono::steady_clock::now(), launch.io_timeout);
  await_completion_ready(launch.control_fd, prepared.assignment.identity,
                         completion_deadline, &control_decoder);
  static_cast<void>(::shutdown(launch.control_fd, SHUT_WR));
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
    const ps::server::WorkerProcessLaunchOptions launch =
        ps::server::parse_worker_process_launch_options(argc, argv);
    control_fd = launch.control_fd;
    const int result = ps::server::run_one_assignment(launch);
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
