#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace ps::ipc::internal {

/**
 * @brief Outcome category for reading one complete version 2 frame.
 *
 * @throws Nothing.
 * @note Clean EOF is distinct from an EOF that truncates a trusted header or
 *       payload boundary.
 */
enum class FrameReadState {
  /** @brief One complete valid frame was read. */
  Complete,

  /** @brief The peer closed between frames before any header byte arrived. */
  CleanEof,

  /** @brief The peer closed inside a header or payload. */
  Truncated,

  /** @brief The advertised payload length was outside the version 2 bounds. */
  InvalidLength,

  /** @brief A socket read failed or a private stage deadline expired. */
  IoError,
};

/**
 * @brief Owned result of reading one framed JSON payload.
 *
 * @throws std::bad_alloc when payload or diagnostic storage allocation fails.
 * @note `payload` is populated only for `FrameReadState::Complete`.
 */
struct FrameReadResult {
  /** @brief Read completion category. */
  FrameReadState state = FrameReadState::IoError;

  /** @brief Exact payload bytes for a complete frame. */
  std::string payload;

  /** @brief Human-readable local IO or framing diagnostic. */
  std::string message;
};

/**
 * @brief Owned result of writing one framed JSON payload.
 *
 * @throws std::bad_alloc when diagnostic storage allocation fails.
 * @note An invalid payload is rejected before any header byte is written.
 */
struct FrameWriteResult {
  /** @brief True when the complete header and payload were transferred. */
  bool ok = false;

  /** @brief Human-readable local IO or framing diagnostic. */
  std::string message;
};

/**
 * @brief Reads one bounded big-endian length-prefixed frame.
 *
 * @param fd Connected Unix stream descriptor owned by the caller.
 * @return Complete payload, clean EOF, contained truncation/length failure, or
 *         an IO diagnostic.
 * @throws std::bad_alloc if payload or diagnostic allocation fails.
 * @note Partial reads and `EINTR` are retried. Length is validated before
 *       allocating the payload and no bytes from a following frame are read.
 */
FrameReadResult read_frame(int fd);

/**
 * @brief Reads one frame with independent ordinary header/payload deadlines.
 *
 * @param fd Connected Unix stream descriptor owned by the caller.
 * @param header_deadline Absolute monotonic deadline for the remaining header.
 * @param payload_timeout Fresh relative budget begun only after a valid header
 *        has been decoded and before payload allocation/read.
 * @return The same framing categories as `read_frame()`; deadline expiry is a
 *         contained `FrameReadState::IoError` with a stage diagnostic.
 * @throws std::bad_alloc if payload or diagnostic allocation fails.
 * @note The caller must establish `header_deadline` immediately after observing
 *       the first header byte. Poll, `recv(MSG_DONTWAIT)`, and `EINTR` retries
 *       all retain the original absolute stage deadline. This private API is
 *       not used by the public Client transport.
 */
FrameReadResult read_frame_with_stage_deadlines(
    int fd, std::chrono::steady_clock::time_point header_deadline,
    std::chrono::steady_clock::duration payload_timeout);

/**
 * @brief Writes one bounded big-endian length-prefixed frame.
 *
 * @param fd Connected Unix stream descriptor owned by the caller.
 * @param payload Nonempty JSON payload no larger than 16 MiB.
 * @return Success or a local framing/write diagnostic.
 * @throws std::bad_alloc if diagnostic allocation fails.
 * @note Partial writes and `EINTR` are retried. Linux uses `MSG_NOSIGNAL`;
 *       callers configure `SO_NOSIGPIPE` on macOS sockets.
 */
FrameWriteResult write_frame(int fd, const std::string& payload);

/**
 * @brief Writes one complete frame before one ordinary write deadline.
 *
 * @param fd Connected Unix stream descriptor owned by the caller.
 * @param payload Nonempty JSON payload no larger than 16 MiB.
 * @param deadline Absolute monotonic deadline shared by header and payload.
 * @return Success or the same framing/write diagnostics as `write_frame()`;
 *         deadline expiry is a contained failure with a stage diagnostic.
 * @throws std::bad_alloc if diagnostic allocation fails.
 * @note Poll, `send(MSG_DONTWAIT)`, partial progress, and interrupted retries
 *       retain the absolute deadline. Linux additionally uses
 *       `MSG_NOSIGNAL`; macOS relies on socket-local `SO_NOSIGPIPE`. This
 *       private API is not used by the public Client transport.
 */
FrameWriteResult write_frame_before_deadline(
    int fd, const std::string& payload,
    std::chrono::steady_clock::time_point deadline);

}  // namespace ps::ipc::internal
