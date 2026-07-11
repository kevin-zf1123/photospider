#include "ipc/frame.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "photospider/ipc/protocol.hpp"

namespace ps::ipc::internal {
namespace {

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
 * @return True only after all requested bytes were received.
 * @throws std::bad_alloc if diagnostic string construction fails.
 * @note The helper retries `EINTR` and never reads beyond `size`.
 */
bool receive_exact(int fd, unsigned char* destination, std::size_t size,
                   bool allow_clean_eof, bool* clean_eof, bool* truncated,
                   std::string* message) {
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t count = ::recv(fd, destination + offset, size - offset, 0);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count == 0) {
      *clean_eof = allow_clean_eof && offset == 0;
      *truncated = !*clean_eof;
      return false;
    }
    if (errno == EINTR) {
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
 * @return True only after all requested bytes were sent.
 * @throws std::bad_alloc if diagnostic string construction fails.
 * @note Linux suppresses SIGPIPE per send; macOS relies on socket-local
 *       `SO_NOSIGPIPE` configured when the descriptor is created or accepted.
 */
bool send_exact(int fd, const unsigned char* source, std::size_t size,
                std::string* message) {
  std::size_t offset = 0;
  while (offset < size) {
#if defined(__linux__)
    constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    constexpr int kSendFlags = 0;
#endif
    const ssize_t count =
        ::send(fd, source + offset, size - offset, kSendFlags);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
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

}  // namespace

/** @copydoc read_frame */
FrameReadResult read_frame(int fd) {
  unsigned char header[sizeof(std::uint32_t)]{};
  bool clean_eof = false;
  bool truncated = false;
  std::string message;
  if (!receive_exact(fd, header, sizeof(header), true, &clean_eof, &truncated,
                     &message)) {
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

  std::string payload(payload_length, '\0');
  clean_eof = false;
  truncated = false;
  message.clear();
  if (!receive_exact(fd, reinterpret_cast<unsigned char*>(payload.data()),
                     payload.size(), false, &clean_eof, &truncated, &message)) {
    if (truncated) {
      return {FrameReadState::Truncated, {}, "EOF inside frame payload"};
    }
    return {FrameReadState::IoError, {}, std::move(message)};
  }
  return {FrameReadState::Complete, std::move(payload), {}};
}

/** @copydoc write_frame */
FrameWriteResult write_frame(int fd, const std::string& payload) {
  if (payload.empty() || payload.size() > kMaximumFramePayloadBytes ||
      payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    return {false, "frame payload length is outside 1..16777216 bytes"};
  }

  const std::uint32_t network_length =
      htonl(static_cast<std::uint32_t>(payload.size()));
  unsigned char header[sizeof(network_length)]{};
  std::memcpy(header, &network_length, sizeof(network_length));
  std::string message;
  if (!send_exact(fd, header, sizeof(header), &message)) {
    return {false, std::move(message)};
  }
  if (!send_exact(fd, reinterpret_cast<const unsigned char*>(payload.data()),
                  payload.size(), &message)) {
    return {false, std::move(message)};
  }
  return {true, {}};
}

}  // namespace ps::ipc::internal
