#pragma once

#include <cstddef>
#include <string>

namespace ps::ipc::internal {

/**
 * @brief Outcome category for reading one complete version 1 frame.
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

  /** @brief The advertised payload length was outside the version 1 bounds. */
  InvalidLength,

  /** @brief A non-interrupted socket read failed. */
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

}  // namespace ps::ipc::internal
