/**
 * @file worker_protocol.hpp
 * @brief Declares the private Issue #100 manager/worker process protocol.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "server/single_tenant_job_service.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {

/** @brief Maximum encoded payload accepted in one private worker frame. */
inline constexpr std::size_t kMaximumWorkerFramePayloadBytes = 64U << 20U;
/** @brief Fixed v1 private worker frame header width. */
inline constexpr std::size_t kWorkerFrameHeaderBytes = 12U;

/**
 * @brief Closed message kinds in the one-assignment worker protocol.
 * @throws Nothing for value operations.
 */
enum class WorkerMessageKind : std::uint16_t {
  /** @brief Manager-to-worker immutable assignment. */
  Assignment = 1U,
  /** @brief Worker-to-manager exact assignment acceptance. */
  AssignmentAccepted = 2U,
  /** @brief Worker-to-manager exact lease heartbeat. */
  Heartbeat = 3U,
  /** @brief Manager-to-worker exact cooperative cancellation. */
  Cancel = 4U,
  /** @brief Worker-to-manager sole terminal attempt report. */
  Report = 5U,
};

/**
 * @brief One validated frame with an uninterpreted bounded payload.
 * @throws Nothing for default construction; payload moves may allocate before
 * construction.
 */
struct WorkerProtocolFrame final {
  /** @brief Validated closed frame kind. */
  WorkerMessageKind kind = WorkerMessageKind::Assignment;
  /** @brief Exact bounded payload bytes. */
  std::vector<std::byte> payload;
};

/**
 * @brief Reassembles one private worker byte stream across bounded read calls.
 *
 * The decoder retains the exact header and payload offsets after
 * `WorkerProtocolTimeout`, validates the complete header once, allocates only
 * the advertised bounded payload, and resets only after returning a complete
 * frame. One instance belongs to one socket byte stream and must not be shared
 * concurrently.
 *
 * @throws Nothing for construction and destruction.
 * @note After any exception other than `WorkerProtocolTimeout`, the channel is
 * faulted and this decoder must be discarded with it. Clean EOF is recognized
 * only when no byte of the next header has been consumed.
 */
class WorkerFrameDecoder final {
 public:
  /** @brief Creates one decoder positioned at a frame boundary. */
  WorkerFrameDecoder() noexcept = default;

  /** @brief Prevents duplicate ownership of one stream's partial frame. */
  WorkerFrameDecoder(const WorkerFrameDecoder& other) = delete;
  /** @brief Prevents duplicate assignment of one stream's partial frame. */
  WorkerFrameDecoder& operator=(const WorkerFrameDecoder& other) = delete;

  /**
   * @brief Advances and returns one complete frame before an absolute deadline.
   * @param fd Connected private Unix socket owned by this decoder's stream.
   * @param deadline Absolute monotonic deadline for this advance call.
   * @return Closed kind and exact bounded payload after full reassembly.
   * @throws WorkerProtocolTimeout when this call's deadline expires; retained
   * bytes remain available to a later call on the same descriptor.
   * @throws WorkerProtocolEof for EOF at a fresh frame boundary.
   * @throws WorkerProtocolError for malformed or truncated input.
   * @throws WorkerChannelError for an invalid descriptor or socket failure.
   */
  WorkerProtocolFrame read_frame(
      int fd, std::chrono::steady_clock::time_point deadline);

 private:
  /** @brief Returns to a fresh frame boundary after successful delivery. */
  void reset() noexcept;

  /** @brief Fixed header bytes retained across timeout slices. */
  std::array<std::byte, kWorkerFrameHeaderBytes> header_{};
  /** @brief Number of complete bytes currently retained in `header_`. */
  std::size_t header_offset_ = 0U;
  /** @brief Whether the complete retained header has been validated. */
  bool header_decoded_ = false;
  /** @brief Validated kind retained while its payload is incomplete. */
  WorkerMessageKind kind_ = WorkerMessageKind::Assignment;
  /** @brief Advertised bounded payload retained across timeout slices. */
  std::vector<std::byte> payload_;
  /** @brief Number of complete bytes currently retained in `payload_`. */
  std::size_t payload_offset_ = 0U;
};

/**
 * @brief Complete immutable material assigned to one external worker.
 * @throws Nothing for default construction; contained values may allocate.
 * @note This value contains no server state root, quota owner, artifact commit
 * capability, credential, native handle, or process-local runtime identity.
 */
struct PreparedWorkerAssignment final {
  /** @brief Exact current Job assignment and optional immutable checkpoint. */
  JobAssignment assignment;
  /** @brief Trusted graph material resolved before process assignment. */
  ResolvedGraphArtifact graph;
  /** @brief Requested worker heartbeat cadence. */
  std::chrono::milliseconds heartbeat_interval{0};
};

/**
 * @brief Base failure for malformed or unsupported worker protocol content.
 * @throws std::bad_alloc when storing the diagnostic exhausts memory.
 */
class WorkerProtocolError : public std::runtime_error {
 public:
  /**
   * @brief Creates one protocol failure with a stable diagnostic.
   * @param message Human-readable closed-protocol rejection reason.
   * @throws std::bad_alloc when storing the diagnostic exhausts memory.
   */
  explicit WorkerProtocolError(const std::string& message)
      : std::runtime_error(message) {}
};

/**
 * @brief Reports expiry of one bounded protocol I/O deadline.
 * @throws std::bad_alloc when storing the diagnostic exhausts memory.
 */
class WorkerProtocolTimeout final : public WorkerProtocolError {
 public:
  /**
   * @brief Creates one deadline-expiry failure.
   * @param message Human-readable operation context.
   * @throws std::bad_alloc when storing the diagnostic exhausts memory.
   */
  explicit WorkerProtocolTimeout(const std::string& message)
      : WorkerProtocolError(message) {}
};

/**
 * @brief Reports clean channel EOF at a frame boundary.
 * @throws std::bad_alloc when storing the diagnostic exhausts memory.
 */
class WorkerProtocolEof final : public WorkerProtocolError {
 public:
  /**
   * @brief Creates one frame-boundary EOF observation.
   * @param message Human-readable channel context.
   * @throws std::bad_alloc when storing the diagnostic exhausts memory.
   */
  explicit WorkerProtocolEof(const std::string& message)
      : WorkerProtocolError(message) {}
};

/**
 * @brief Reports a local socket read/write failure rather than malformed bytes.
 * @throws std::bad_alloc when storing the diagnostic exhausts memory.
 */
class WorkerChannelError final : public WorkerProtocolError {
 public:
  /**
   * @brief Creates one channel-system failure.
   * @param message Human-readable syscall context.
   * @throws std::bad_alloc when storing the diagnostic exhausts memory.
   */
  explicit WorkerChannelError(const std::string& message)
      : WorkerProtocolError(message) {}
};

/**
 * @brief Writes one checked private worker frame before an absolute deadline.
 * @param fd Connected private Unix socket.
 * @param kind Closed frame kind.
 * @param payload Exact payload, bounded by
 * `kMaximumWorkerFramePayloadBytes`.
 * @param deadline Absolute monotonic I/O deadline.
 * @return Nothing after the complete header and payload are written.
 * @throws std::invalid_argument for an invalid descriptor, kind, or oversized
 * payload.
 * @throws WorkerProtocolTimeout on deadline expiry.
 * @throws WorkerChannelError on socket failure or closure.
 * @note Writes suppress `SIGPIPE`; an unavailable peer becomes an exception.
 */
void write_worker_frame(int fd, WorkerMessageKind kind,
                        const std::vector<std::byte>& payload,
                        std::chrono::steady_clock::time_point deadline);

/**
 * @brief Reads and validates one complete frame with a one-shot decoder.
 * @param fd Connected private Unix socket.
 * @param deadline Absolute monotonic I/O deadline.
 * @return Closed kind and exact bounded payload.
 * @throws WorkerProtocolTimeout on deadline expiry.
 * @throws WorkerProtocolEof for EOF before the next frame begins.
 * @throws WorkerProtocolError for a malformed/truncated header or payload.
 * @throws WorkerChannelError for a socket-system failure.
 * @note Callers that intentionally retry short read deadlines on one stream
 * must retain `WorkerFrameDecoder`; this convenience function cannot preserve
 * a partial frame after it throws `WorkerProtocolTimeout`.
 */
WorkerProtocolFrame read_worker_frame(
    int fd, std::chrono::steady_clock::time_point deadline);

/**
 * @brief Sends one complete immutable external worker assignment.
 * @param fd Connected worker socket.
 * @param assignment Complete identity/spec/checkpoint/graph/control payload.
 * @param deadline Absolute monotonic I/O deadline.
 * @return Nothing after the assignment frame is written.
 * @throws Contract, allocation, protocol-bound, timeout, or channel failures.
 */
void send_worker_assignment(int fd, const PreparedWorkerAssignment& assignment,
                            std::chrono::steady_clock::time_point deadline);

/**
 * @brief Receives and reconstructs one complete immutable worker assignment.
 * @param fd Connected worker socket.
 * @param deadline Absolute monotonic I/O deadline.
 * @return Digest-revalidated assignment, checkpoint, graph, and cadence.
 * @throws WorkerProtocolError for malformed, oversized, or inconsistent data.
 * @throws WorkerProtocolTimeout/WorkerProtocolEof/WorkerChannelError for I/O
 * failure.
 */
PreparedWorkerAssignment receive_worker_assignment(
    int fd, std::chrono::steady_clock::time_point deadline);

/**
 * @brief Sends one identity-only acceptance, heartbeat, or cancel frame.
 * @param fd Connected worker socket.
 * @param kind AssignmentAccepted, Heartbeat, or Cancel.
 * @param identity Complete exact lease identity.
 * @param deadline Absolute monotonic I/O deadline.
 * @return Nothing after the exact frame is written.
 * @throws Contract, timeout, protocol-bound, or channel failures.
 */
void send_worker_identity(int fd, WorkerMessageKind kind,
                          const AttemptIdentity& identity,
                          std::chrono::steady_clock::time_point deadline);

/**
 * @brief Decodes one identity-only frame and verifies its expected kind.
 * @param frame Valid bounded frame.
 * @param expected_kind AssignmentAccepted, Heartbeat, or Cancel.
 * @return Complete validated identity tuple.
 * @throws WorkerProtocolError for wrong kind, malformed bytes, or trailing
 * fields.
 */
AttemptIdentity decode_worker_identity(const WorkerProtocolFrame& frame,
                                       WorkerMessageKind expected_kind);

/**
 * @brief Encodes one bounded worker attempt report without performing I/O.
 * @param report Complete worker-local attempt facts.
 * @param spec Immutable JobSpec used for image resource bounds.
 * @return Complete private Report frame ready for bounded transport. An
 * otherwise valid settled success whose image exceeds the aggregate frame or
 * Job resource envelope becomes one bounded `Failed/Compute` report without
 * an image.
 * @throws Contract, image, allocation, or aggregate protocol-bound failures.
 * @note This source-private seam lets protocol tests exercise the exact
 * aggregate Report boundary without changing the installed ABI or owning a
 * socket. The typed fallback preserves exact identity and settlement while
 * replacing untransportable candidate bytes and diagnostic with one fixed
 * bounded worker-owned failure. `send_worker_report()` is the sole transport
 * wrapper.
 */
WorkerProtocolFrame encode_worker_report(const JobAttemptReport& report,
                                         const JobSpec& spec);

/**
 * @brief Sends one bounded worker attempt report.
 * @param fd Connected worker socket.
 * @param report Complete worker-local attempt facts.
 * @param spec Immutable JobSpec used for image resource bounds.
 * @param deadline Absolute monotonic I/O deadline.
 * @return Nothing after one report frame is written.
 * @throws As `encode_worker_report`, plus timeout or channel failures.
 */
void send_worker_report(int fd, const JobAttemptReport& report,
                        const JobSpec& spec,
                        std::chrono::steady_clock::time_point deadline);

/**
 * @brief Decodes one report and rebuilds an independently owned CPU image.
 * @param frame Valid bounded frame expected to contain Report.
 * @param spec Immutable JobSpec used for output/staging/retention bounds.
 * @return Complete report with tight bytes copied into a new CPU owner.
 * @throws WorkerProtocolError for malformed, oversized, inconsistent, or
 * trailing data.
 * @throws std::bad_alloc when bounded reconstruction exhausts memory.
 */
JobAttemptReport decode_worker_report(const WorkerProtocolFrame& frame,
                                      const JobSpec& spec);

}  // namespace ps::server
