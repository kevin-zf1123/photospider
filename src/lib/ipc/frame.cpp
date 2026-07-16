#include "ipc/frame.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "photospider/ipc/protocol.hpp"

namespace ps::ipc::internal {
namespace {

/**
 * @brief One absolute monotonic IO deadline and diagnostic stage label.
 *
 * @throws Nothing.
 * @note Instances live on the calling stack and are never retained across an
 *       IO helper call.
 */
struct IoDeadline {
  /** @brief Absolute monotonic endpoint retained across every retry. */
  std::chrono::steady_clock::time_point value;

  /** @brief Stable literal prefix used in local timeout diagnostics. */
  const char* stage = nullptr;
};

/**
 * @brief Waits for nonblocking socket progress before an absolute deadline.
 *
 * @param fd Connected stream descriptor.
 * @param events Requested `poll` readiness mask.
 * @param deadline Absolute endpoint and stable diagnostic stage.
 * @param message Receives timeout, invalid-descriptor, or poll diagnostics.
 * @return True when the caller should attempt its next nonblocking syscall;
 *         false after deadline expiry or a non-interrupted poll failure.
 * @throws std::bad_alloc if diagnostic construction allocates.
 * @note Hangup and socket error are returned as progress so `recv`/`send` owns
 *       the established EOF or concrete socket-error classification. `EINTR`
 *       never renews the deadline.
 */
bool wait_for_io(int fd, std::int16_t events, const IoDeadline& deadline,
                 std::string* message) {
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline.value) {
      *message = std::string(deadline.stage) + " deadline exceeded";
      return false;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline.value -
                                                              now)
            .count();
    const auto bounded = std::min<std::chrono::milliseconds::rep>(
        std::max<std::chrono::milliseconds::rep>(remaining, 1),
        std::numeric_limits<int>::max());
    pollfd descriptor{fd, events, 0};
    const int ready = ::poll(&descriptor, 1, static_cast<int>(bounded));
    if (ready > 0) {
      if ((descriptor.revents & POLLNVAL) != 0) {
        *message = std::string(deadline.stage) + " descriptor became invalid";
        return false;
      }
      if ((descriptor.revents & (events | POLLHUP | POLLERR)) != 0) {
        return true;
      }
      continue;
    }
    if (ready == 0) {
      if (std::chrono::steady_clock::now() < deadline.value) {
        continue;
      }
      *message = std::string(deadline.stage) + " deadline exceeded";
      return false;
    }
    if (errno != EINTR) {
      *message =
          std::string(deadline.stage) + " poll failed: " + std::strerror(errno);
      return false;
    }
  }
}

/**
 * @brief Receives exactly one fixed-size byte range with EOF classification.
 *
 * @param fd Connected stream descriptor.
 * @param destination Writable byte range.
 * @param size Required byte count.
 * @param allow_clean_eof Whether zero bytes before the range may be clean EOF.
 * @param clean_eof Set true only for allowed EOF before any byte arrived.
 * @param truncated Set true when EOF arrived after a partial range or when
 *        clean EOF is not allowed.
 * @param message Receives a non-EINTR socket diagnostic.
 * @param deadline Optional absolute deadline selecting poll plus nonblocking
 *        receive; null preserves the established blocking transport behavior.
 * @return True only after all requested bytes were received.
 * @throws std::bad_alloc if diagnostic string construction fails.
 * @note The helper retries `EINTR` and never reads beyond `size`. Deadline mode
 *       also retries transient readiness races without renewing the endpoint.
 */
bool receive_exact(int fd, unsigned char* destination, std::size_t size,
                   bool allow_clean_eof, bool* clean_eof, bool* truncated,
                   std::string* message, const IoDeadline* deadline = nullptr) {
  std::size_t offset = 0;
  while (offset < size) {
    if (deadline != nullptr && !wait_for_io(fd, POLLIN, *deadline, message)) {
      return false;
    }
    const int flags = deadline == nullptr ? 0 : MSG_DONTWAIT;
    const ssize_t count =
        ::recv(fd, destination + offset, size - offset, flags);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count == 0) {
      *clean_eof = allow_clean_eof && offset == 0;
      *truncated = !*clean_eof;
      return false;
    }
    if (errno == EINTR ||
        (deadline != nullptr && (errno == EAGAIN || errno == EWOULDBLOCK))) {
      continue;
    }
    *message = std::string("socket read failed: ") + std::strerror(errno);
    return false;
  }
  return true;
}

/**
 * @brief Sends exactly one byte range without process-global SIGPIPE changes.
 *
 * @param fd Connected stream descriptor.
 * @param source Read-only byte range.
 * @param size Required byte count.
 * @param message Receives a non-EINTR socket diagnostic.
 * @param deadline Optional absolute deadline selecting poll plus nonblocking
 *        send; null preserves the established blocking transport behavior.
 * @return True only after all requested bytes were sent.
 * @throws std::bad_alloc if diagnostic string construction fails.
 * @note Linux suppresses SIGPIPE per send; macOS relies on socket-local
 *       `SO_NOSIGPIPE` configured when the descriptor is created or accepted.
 *       Deadline mode retains one endpoint across partial progress and retries.
 */
bool send_exact(int fd, const unsigned char* source, std::size_t size,
                std::string* message, const IoDeadline* deadline = nullptr) {
  std::size_t offset = 0;
  while (offset < size) {
    if (deadline != nullptr && !wait_for_io(fd, POLLOUT, *deadline, message)) {
      return false;
    }
#if defined(__linux__)
    int send_flags = MSG_NOSIGNAL;
#else
    int send_flags = 0;
#endif
    if (deadline != nullptr) {
      send_flags |= MSG_DONTWAIT;
    }
    const ssize_t count =
        ::send(fd, source + offset, size - offset, send_flags);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 &&
        (errno == EINTR ||
         (deadline != nullptr && (errno == EAGAIN || errno == EWOULDBLOCK)))) {
      continue;
    }
    if (count == 0) {
      *message = "socket write made no progress";
    } else {
      *message = std::string("socket write failed: ") + std::strerror(errno);
    }
    return false;
  }
  return true;
}

/**
 * @brief Implements blocking and stage-bounded frame reads in one parser.
 *
 * @param fd Connected stream descriptor.
 * @param header_deadline Optional remaining-header absolute deadline.
 * @param payload_timeout Optional fresh payload budget paired with the header
 *        deadline.
 * @return Complete frame or contained EOF, framing, deadline, or IO outcome.
 * @throws std::bad_alloc if payload or diagnostic allocation fails.
 * @note Length validation always precedes payload allocation. Deadline mode
 *       starts the payload clock only after successful header validation.
 */
FrameReadResult read_frame_impl(
    int fd, const IoDeadline* header_deadline,
    const std::chrono::steady_clock::duration* payload_timeout) {
  unsigned char header[sizeof(std::uint32_t)]{};
  bool clean_eof = false;
  bool truncated = false;
  std::string message;
  if (!receive_exact(fd, header, sizeof(header), true, &clean_eof, &truncated,
                     &message, header_deadline)) {
    if (clean_eof) {
      return {FrameReadState::CleanEof, {}, {}};
    }
    if (truncated) {
      return {FrameReadState::Truncated, {}, "EOF inside frame header"};
    }
    return {FrameReadState::IoError, {}, std::move(message)};
  }

  std::uint32_t network_length = 0;
  std::memcpy(&network_length, header, sizeof(network_length));
  const std::uint32_t payload_length = ntohl(network_length);
  if (payload_length == 0 || payload_length > kMaximumFramePayloadBytes) {
    return {FrameReadState::InvalidLength,
            {},
            "frame payload length is outside 1..16777216 bytes"};
  }

  IoDeadline payload_deadline{std::chrono::steady_clock::time_point{},
                              "frame payload"};
  const IoDeadline* active_payload_deadline = nullptr;
  if (payload_timeout != nullptr) {
    payload_deadline.value =
        std::chrono::steady_clock::now() + *payload_timeout;
    active_payload_deadline = &payload_deadline;
  }
  std::string payload(payload_length, '\0');
  clean_eof = false;
  truncated = false;
  message.clear();
  if (!receive_exact(fd, reinterpret_cast<unsigned char*>(payload.data()),
                     payload.size(), false, &clean_eof, &truncated, &message,
                     active_payload_deadline)) {
    if (truncated) {
      return {FrameReadState::Truncated, {}, "EOF inside frame payload"};
    }
    return {FrameReadState::IoError, {}, std::move(message)};
  }
  return {FrameReadState::Complete, std::move(payload), {}};
}

/**
 * @brief Implements blocking and deadline-bounded frame writes in one encoder.
 *
 * @param fd Connected stream descriptor.
 * @param payload Nonempty bounded JSON payload.
 * @param deadline Optional absolute endpoint shared by header and payload.
 * @return Complete write or local validation, deadline, or IO failure.
 * @throws std::bad_alloc if diagnostic allocation fails.
 * @note Payload bounds are checked before any byte is sent.
 */
FrameWriteResult write_frame_impl(int fd, const std::string& payload,
                                  const IoDeadline* deadline) {
  if (payload.empty() || payload.size() > kMaximumFramePayloadBytes ||
      payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    return {false, "frame payload length is outside 1..16777216 bytes"};
  }

  const std::uint32_t network_length =
      htonl(static_cast<std::uint32_t>(payload.size()));
  unsigned char header[sizeof(network_length)]{};
  std::memcpy(header, &network_length, sizeof(network_length));
  std::string message;
  if (!send_exact(fd, header, sizeof(header), &message, deadline)) {
    return {false, std::move(message)};
  }
  if (!send_exact(fd, reinterpret_cast<const unsigned char*>(payload.data()),
                  payload.size(), &message, deadline)) {
    return {false, std::move(message)};
  }
  return {true, {}};
}

}  // namespace

/** @copydoc read_frame */
FrameReadResult read_frame(int fd) {
  return read_frame_impl(fd, nullptr, nullptr);
}

/** @copydoc read_frame_with_stage_deadlines */
FrameReadResult read_frame_with_stage_deadlines(
    int fd, std::chrono::steady_clock::time_point header_deadline,
    std::chrono::steady_clock::duration payload_timeout) {
  const IoDeadline deadline{header_deadline, "frame header"};
  return read_frame_impl(fd, &deadline, &payload_timeout);
}

/** @copydoc write_frame */
FrameWriteResult write_frame(int fd, const std::string& payload) {
  return write_frame_impl(fd, payload, nullptr);
}

/** @copydoc write_frame_before_deadline */
FrameWriteResult write_frame_before_deadline(
    int fd, const std::string& payload,
    std::chrono::steady_clock::time_point deadline) {
  const IoDeadline write_deadline{deadline, "frame write"};
  return write_frame_impl(fd, payload, &write_deadline);
}

}  // namespace ps::ipc::internal
