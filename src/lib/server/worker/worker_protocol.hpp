/**
 * @file worker_protocol.hpp
 * @brief Declares the metadata-only Issue #105 manager/worker control protocol.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "server/worker/worker_artifact_data_plane.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {

/**
 * @brief Maximum metadata payload accepted in one private worker control frame.
 * @note Protocol v2 carries no artifact, image, blob, or Value bytes. The
 * 128-KiB limit covers the complete worst-case bounded Assignment metadata.
 */
inline constexpr std::size_t kMaximumWorkerControlPayloadBytes = 128U << 10U;
/**
 * @brief Maximum bytes accepted in one private worker text field.
 * @note This single source-private bound covers transported graph paths,
 * configuration text, and diagnostics. The exact boundary is accepted.
 */
inline constexpr std::size_t kMaximumWorkerTextFieldBytes = 16U << 10U;
/** @brief Fixed v2 private worker control-frame header width. */
inline constexpr std::size_t kWorkerFrameHeaderBytes = 12U;

/**
 * @brief Validates every text field transported with prepared graph material.
 * @param graph Complete trusted graph result before catalog retention or
 * Assignment encoding.
 * @return Nothing when root, YAML, config, cache-root, and diagnostic strings
 * each fit the inclusive `kMaximumWorkerTextFieldBytes` bound.
 * @throws std::length_error with the exact field name, observed byte count,
 * and maximum when any transported string exceeds the shared bound.
 * @throws std::bad_alloc when constructing the validation diagnostic fails.
 * @note `PreparedExternalGraphCatalog` uses this boundary before service,
 * quota, Job, supervision-thread, or process ownership. Assignment encoding
 * reuses it defensively; decoding applies the same constant before copying.
 * Length is measured in opaque encoded bytes rather than code points.
 */
void validate_worker_assignment_graph_transport(
    const ResolvedGraphArtifact& graph);

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
  /** @brief Manager-to-worker acknowledgement that completion data was joined.
   */
  CompletionReady = 6U,
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
 * @brief One frame committed at its semantic acceptance linearization point.
 * @note `accepted_at` is the decoder's single fresh monotonic sample taken
 * after caller interpretation and before retained-frame ownership is cleared.
 * The bounded payload allocation occurs during transport decode before this
 * record is constructed. The record grants no lifecycle authority beyond the
 * successfully accepted frame.
 */
struct AcceptedWorkerProtocolFrame final {
  /** @brief Exact semantically interpreted frame moved from decoder ownership.
   */
  WorkerProtocolFrame frame;
  /** @brief Fresh strict-before semantic acceptance time. */
  std::chrono::steady_clock::time_point accepted_at;
};

/**
 * @brief Reassembles one private control byte stream across bounded read calls.
 *
 * The decoder retains the exact header and payload offsets after
 * `WorkerProtocolTimeout`, including when a complete frame misses its semantic
 * acceptance deadline. It validates the complete header once, allocates only
 * the advertised bounded payload, and can retain one complete transport frame
 * across a later semantic-acceptance timeout. `inspect_frame()` plus
 * `accept_frame()` lets a caller validate identity/report meaning before the
 * complete frame is consumed. One instance belongs to one socket byte stream
 * and must not be shared concurrently.
 *
 * @throws Nothing for construction and destruction.
 * @note After any transport/decode exception other than
 * `WorkerProtocolTimeout`, the channel is faulted and this decoder must be
 * discarded with it. Caller-misuse `std::logic_error` from `accept_frame()`
 * performs no channel I/O and does not itself fault the stream. Clean EOF is
 * recognized only when no byte of the next header has been consumed.
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
   * @note Equality with `deadline` is already late. This overload uses the
   * same absolute value for readiness waiting and semantic acceptance.
   */
  WorkerProtocolFrame read_frame(
      int fd, std::chrono::steady_clock::time_point deadline);

  /**
   * @brief Advances one frame with separate readiness and acceptance bounds.
   * @param fd Connected private Unix socket owned by this decoder's stream.
   * @param poll_deadline Absolute monotonic bound for waiting on socket
   * readiness. A due value permits one nonblocking readiness probe.
   * @param acceptance_deadline Absolute monotonic lifecycle bound; a complete
   * frame is returned only while current time is strictly before this value.
   * @return Closed kind and exact bounded payload after full reassembly.
   * @throws WorkerProtocolTimeout when readiness is unavailable within the
   * poll budget or semantic acceptance reaches its deadline. All retained
   * partial or complete bytes remain available to a later call.
   * @throws WorkerProtocolEof for EOF at a fresh frame boundary.
   * @throws WorkerProtocolError for malformed or truncated input.
   * @throws WorkerChannelError for an invalid descriptor or socket failure.
   * @note This split permits WorkerManager's zero-budget control probe to
   * observe a ready control frame before the independent lifecycle deadline;
   * an unavailable or partial probe times out without losing decoder state so
   * one bounded bulk slice can run.
   */
  WorkerProtocolFrame read_frame(
      int fd, std::chrono::steady_clock::time_point poll_deadline,
      std::chrono::steady_clock::time_point acceptance_deadline);

  /**
   * @brief Reassembles and retains one frame for semantic interpretation.
   * @param fd Connected private Unix socket owned by this decoder's stream.
   * @param deadline Absolute monotonic transport and acceptance deadline.
   * @return Borrowed complete frame retained until `accept_frame()` succeeds.
   * @throws Worker protocol timeout, EOF, malformed-input, channel, or
   * allocation failures unchanged.
   * @note A prior complete frame whose semantic acceptance timed out is
   * returned again under this fresh deadline without reading another byte.
   * The borrowed reference becomes invalid after successful acceptance, the
   * next decoder mutation, or decoder destruction. Interpretation must not
   * mutate external lifecycle state before `accept_frame()` succeeds.
   */
  const WorkerProtocolFrame& inspect_frame(
      int fd, std::chrono::steady_clock::time_point deadline);

  /**
   * @brief Reassembles and retains a frame under split protocol deadlines.
   * @param fd Connected private Unix socket owned by this decoder's stream.
   * @param poll_deadline Absolute readiness-wait budget.
   * @param acceptance_deadline Absolute semantic lifecycle deadline.
   * @return Borrowed complete frame retained until `accept_frame()` succeeds.
   * @throws Worker protocol timeout, EOF, malformed-input, channel, or
   * allocation failures unchanged.
   * @note A due poll deadline still permits the same nonblocking probe as
   * `read_frame()`. A retained complete frame performs no new channel I/O.
   */
  const WorkerProtocolFrame& inspect_frame(
      int fd, std::chrono::steady_clock::time_point poll_deadline,
      std::chrono::steady_clock::time_point acceptance_deadline);

  /**
   * @brief Commits one semantically interpreted complete frame before D.
   * @param acceptance_deadline Unchanged exclusive lifecycle deadline.
   * @return Accepted frame and its exact semantic linearization sample.
   * @throws std::logic_error when no complete inspected frame is retained.
   * @throws WorkerProtocolTimeout at a deadline tie or later; the complete
   * frame remains retained for a later bounded semantic retry.
   * @note The fresh monotonic sample linearizes semantic acceptance after the
   * caller has decoded and validated the borrowed frame. Failure consumes no
   * bytes, resets no state, and grants no Job, cancellation, liveness, report,
   * artifact, or completion authority.
   */
  AcceptedWorkerProtocolFrame accept_frame(
      std::chrono::steady_clock::time_point acceptance_deadline);

 private:
  /**
   * @brief Advances incremental transport into one complete owned frame.
   * @param fd Connected private Unix socket.
   * @param poll_deadline Absolute readiness-wait budget.
   * @param acceptance_deadline Absolute frame-transport deadline.
   * @return Complete transport-accepted frame after partial state resets.
   * @throws Worker protocol failures unchanged.
   * @note This method does not perform caller-specific semantic acceptance.
   */
  WorkerProtocolFrame advance_frame(
      int fd, std::chrono::steady_clock::time_point poll_deadline,
      std::chrono::steady_clock::time_point acceptance_deadline);

  /** @brief Returns to a fresh frame boundary after successful delivery. */
  void reset() noexcept;

  /** @brief Complete frame retained through semantic acceptance timeout. */
  std::optional<WorkerProtocolFrame> retained_frame_;

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
 * @brief Complete immutable metadata assigned to one external worker.
 * @throws Nothing for default construction; contained values may allocate.
 * @note The manager-side `JobAssignment` may still retain an authorized
 * checkpoint record so the registered supervisor can pump its exact bytes;
 * the encoder emits only `data_plane.checkpoint` receipt/reference metadata.
 * A decoded worker-side value leaves `assignment.checkpoint` null until the
 * worker receives and revalidates the separate checkpoint stream. No control
 * field contains artifact bytes, a server root, quota owner, publication
 * capability, credential, native handle, or process-local runtime identity.
 */
struct PreparedWorkerAssignment final {
  /** @brief Exact current Job identity/spec and endpoint-local checkpoint. */
  JobAssignment assignment;
  /** @brief Trusted graph material resolved before process assignment. */
  ResolvedGraphArtifact graph;
  /** @brief Metadata joining separate checkpoint/output data-plane lanes. */
  WorkerDataPlaneAssignment data_plane;
  /** @brief Requested worker heartbeat cadence. */
  std::chrono::milliseconds heartbeat_interval{0};
};

/**
 * @brief One metadata-first worker report plus optional output reference.
 * @throws Nothing for default construction; retained values may allocate.
 * @note `report.image` MUST be empty. A successful worker sends this metadata
 * before streaming bytes through its separate output descriptor. WorkerManager
 * creates one exact lazy final owner only for the live identity-current worker,
 * then directly receives and incrementally hashes one fixed slice per deadline
 * arbitration before replying with exact `CompletionReady`. The worker remains
 * alive, heartbeating, and killable until that acknowledgement; clean reap
 * performs no bulk-data or filesystem I/O.
 */
struct PreparedWorkerReport final {
  /** @brief Exact attempt outcome/settlement/failure facts without bytes. */
  JobAttemptReport report;
  /** @brief Candidate stage metadata present only for settled success. */
  std::optional<WorkerOutputDataReference> output;
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
 * @param payload Exact metadata, bounded by
 * `kMaximumWorkerControlPayloadBytes`.
 * @param deadline Absolute monotonic I/O deadline.
 * @return Nothing after the complete header and payload are written.
 * @throws std::invalid_argument for an invalid descriptor, kind, or oversized
 * payload.
 * @throws WorkerProtocolTimeout on deadline expiry.
 * @throws WorkerChannelError on socket failure or closure.
 * @note Writes suppress `SIGPIPE`; an unavailable peer becomes an exception.
 * A timeout after positive write progress can mean the peer received a frame
 * prefix or even the final byte. Callers must treat the write as failed and
 * never retry that frame. A cancellation owner may retain the channel only for
 * its existing bounded receive-side report/EOF/exit drainage.
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
 * a partial or complete frame after it throws `WorkerProtocolTimeout` and is
 * therefore reserved for lifecycle paths that fail closed on timeout.
 */
WorkerProtocolFrame read_worker_frame(
    int fd, std::chrono::steady_clock::time_point deadline);

/**
 * @brief Encodes one complete immutable external worker assignment.
 * @param assignment Complete identity/spec/receipt/graph/data-reference
 * metadata. Manager-side checkpoint bytes are validated but never encoded.
 * @return Complete private Assignment frame ready for bounded transport.
 * @throws Contract, allocation, digest, field, or aggregate protocol-bound
 * failures. An oversized graph text field raises `std::length_error`.
 * @note This source-private seam lets protocol tests prove the exact aggregate
 * Assignment boundary and byte independence without blocking on a socket.
 * It validates checkpoint receipt identity and exact length but performs no
 * manager-side bulk hash; the killable worker verifies the received stream
 * against the authoritative receipt digest. `send_worker_assignment` is the
 * sole transport wrapper.
 */
WorkerProtocolFrame encode_worker_assignment(
    const PreparedWorkerAssignment& assignment);

/**
 * @brief Canonically encodes validated Assignment control metadata only.
 *
 * @param assignment Complete identity/spec/data-plane/graph/cadence metadata.
 * A checkpoint declaration is joined through `data_plane.checkpoint`; the
 * endpoint-local `assignment.checkpoint` bulk owner may be absent after decode.
 * @return Complete canonical private Assignment frame.
 * @throws WorkerProtocolError, contract, allocation, digest, field, or
 * aggregate protocol-bound failures for invalid metadata.
 * @note This pure codec opens no descriptor and reads no artifact byte. It does
 * not authorize sending a manager Assignment: `encode_worker_assignment` and
 * `send_worker_assignment` additionally require the authorized checkpoint bulk
 * owner when the JobSpec declares one.
 */
WorkerProtocolFrame encode_worker_assignment_metadata(
    const PreparedWorkerAssignment& assignment);

/**
 * @brief Decodes one bounded Assignment frame without performing transport I/O.
 *
 * The decoder verifies the closed message kind, reconstructs and validates the
 * exact Attempt/JobSpec digest join, validates checkpoint/output data-plane
 * descriptors, copies bounded graph-preparation text, rejects a zero heartbeat,
 * and requires complete payload consumption.
 *
 * @param frame Already framed payload no larger than
 * `kMaximumWorkerControlPayloadBytes`.
 * @return Complete validated control metadata. Checkpoint artifact bytes remain
 * absent until the separately authenticated data-plane transfer completes.
 * @throws WorkerProtocolError for a wrong kind, malformed/truncated payload,
 * inconsistent identity/spec/descriptor metadata, zero cadence, or trailing
 * bytes.
 * @throws std::bad_alloc when bounded decoded storage cannot allocate.
 * @note This pure semantic seam opens no socket, transfers no artifact bytes,
 * acquires no lease, and selects no current attempt. Socket receive delegates
 * to this function and retains deadline acceptance as a transport concern.
 */
PreparedWorkerAssignment decode_worker_assignment(
    const WorkerProtocolFrame& frame);

/**
 * @brief Sends one complete immutable external worker assignment.
 * @param fd Connected worker socket.
 * @param assignment Complete bounded metadata; checkpoint bytes remain in the
 * separate manager-to-worker stream lane.
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
 * @return Digest-revalidated identity/spec, checkpoint receipt/reference,
 * graph metadata, output-stage reference, and cadence. Checkpoint bytes remain
 * absent until separate descriptor materialization.
 * @throws WorkerProtocolError for malformed, oversized, or inconsistent data.
 * @throws WorkerProtocolTimeout/WorkerProtocolEof/WorkerChannelError for I/O
 * failure.
 * @note Semantic reconstruction delegates to `decode_worker_assignment` and is
 * accepted only while current monotonic time remains strictly before
 * `deadline`; timeout fails the startup channel closed.
 */
PreparedWorkerAssignment receive_worker_assignment(
    int fd, std::chrono::steady_clock::time_point deadline);

/**
 * @brief Sends one identity-only lifecycle frame.
 * @param fd Connected worker socket.
 * @param kind AssignmentAccepted, Heartbeat, Cancel, or CompletionReady.
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
 * @param expected_kind AssignmentAccepted, Heartbeat, Cancel, or
 * CompletionReady.
 * @return Complete validated identity tuple.
 * @throws WorkerProtocolError for wrong kind, malformed bytes, or trailing
 * fields.
 */
AttemptIdentity decode_worker_identity(const WorkerProtocolFrame& frame,
                                       WorkerMessageKind expected_kind);

/**
 * @brief Encodes one bounded metadata-only worker attempt report.
 * @param report Complete image-free attempt facts and optional output-stage
 * descriptor/digest/reference metadata.
 * @param spec Immutable JobSpec used for exact digest/output joins.
 * @param output_stage Exact Assignment output reference and byte maximum.
 * @return Complete private Report control frame ready for bounded transport.
 * @throws Contract, metadata, allocation, or control-bound failures.
 * @note Image bytes must not be serialized here. The worker sends this frame
 * before its separately retained source bytes, and the encoder rejects an
 * `ImageBuffer` or mismatched output reference. `send_worker_report()` is the
 * sole wrapper.
 */
WorkerProtocolFrame encode_worker_report(
    const PreparedWorkerReport& report, const JobSpec& spec,
    const WorkerOutputStageReference& output_stage);

/**
 * @brief Sends one bounded worker attempt report.
 * @param fd Connected worker socket.
 * @param report Complete image-free attempt facts and optional output metadata.
 * @param spec Immutable JobSpec used for exact joins.
 * @param output_stage Exact assigned output reference and maximum.
 * @param deadline Absolute monotonic I/O deadline.
 * @return Nothing after one report frame is written.
 * @throws As `encode_worker_report`, plus timeout or channel failures.
 */
void send_worker_report(int fd, const PreparedWorkerReport& report,
                        const JobSpec& spec,
                        const WorkerOutputStageReference& output_stage,
                        std::chrono::steady_clock::time_point deadline);

/**
 * @brief Decodes one metadata-only report without reading artifact bytes.
 * @param frame Valid bounded frame expected to contain Report.
 * @param spec Immutable JobSpec used for digest and output-slot joins.
 * @param output_stage Exact Assignment output reference and maximum.
 * @return Image-free attempt facts plus optional exact output metadata.
 * @throws WorkerProtocolError for malformed, inconsistent, oversized, or
 * trailing metadata, or for any encoded image-bearing report shape.
 * @throws std::bad_alloc when bounded metadata reconstruction exhausts memory.
 * @note WorkerManager validates this metadata while the exact PID remains
 * owned, creates the exact final owner, and separately drains one nonblocking
 * output slice between absolute lifecycle arbitration points. Only valid
 * Heartbeat frames renew liveness; output progress never does. The manager
 * completes the digest/EOF join before clean-reap completion handoff.
 */
PreparedWorkerReport decode_worker_report(
    const WorkerProtocolFrame& frame, const JobSpec& spec,
    const WorkerOutputStageReference& output_stage);

}  // namespace ps::server
