#include "ipc/unix_socket.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

namespace ps::ipc::internal {
namespace {

/**
 * @brief Milliseconds between checks of a latched connect interruption.
 * @throws Nothing; this is immutable compile-time policy.
 * @note Ten milliseconds bounds stop observation during local connect waits;
 *       repeated slices intentionally impose no total connection timeout.
 */
constexpr int kConnectPollSliceMilliseconds = 10;

/**
 * @brief Marks one newly created client descriptor close-on-exec.
 *
 * @param fd Socket descriptor to protect from fork/exec leakage.
 * @param message Receives an `fcntl` diagnostic on failure.
 * @return True when `FD_CLOEXEC` is installed.
 * @throws std::bad_alloc if diagnostic construction fails.
 */
bool configure_close_on_exec(int fd, std::string* message) {
  const int flags = ::fcntl(fd, F_GETFD, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
    *message = std::string("socket FD_CLOEXEC failed: ") + std::strerror(errno);
    return false;
  }
  return true;
}

/**
 * @brief Reports whether one nonblocking Unix connect remains in flight.
 * @param error_number Connect or `SO_ERROR` errno value.
 * @return True for portable pending-connect classifications.
 * @throws Nothing.
 */
bool connect_pending(int error_number) noexcept {
  return error_number == EINPROGRESS || error_number == EALREADY;
}

/**
 * @brief Waits one bounded slice before retrying Linux AF_UNIX `EAGAIN`.
 * @param should_stop Latched lifecycle predicate.
 * @param message Receives interruption or poll failure diagnostics.
 * @return True when the same unconnected fd may retry local connect.
 * @throws std::bad_alloc if diagnostics allocate, or whatever the predicate
 *         throws.
 * @note Linux returns `EAGAIN` before creating an in-flight connection when a
 *       listener backlog is full. This delay never writes a frame and has no
 *       total timeout.
 */
bool wait_connect_retry_slice(const std::function<bool()>& should_stop,
                              std::string* message) {
  while (true) {
    if (should_stop()) {
      *message = "connect interrupted";
      return false;
    }
    const int poll_result = ::poll(nullptr, 0, kConnectPollSliceMilliseconds);
    if (poll_result >= 0) {
      return true;
    }
    if (errno != EINTR) {
      const int poll_error = errno;
      *message =
          std::string("connect poll failed: ") + std::strerror(poll_error);
      return false;
    }
  }
}

}  // namespace

/** @copydoc UniqueFd::UniqueFd(int) */
UniqueFd::UniqueFd(int fd) noexcept : fd_(fd) {}

/** @copydoc UniqueFd::~UniqueFd */
UniqueFd::~UniqueFd() {
  reset();
}

/** @copydoc UniqueFd::UniqueFd(UniqueFd&&) */
UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

/** @copydoc UniqueFd::operator=(UniqueFd&&) */
UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
  if (this != &other) {
    reset(other.release());
  }
  return *this;
}

/** @copydoc UniqueFd::get */
int UniqueFd::get() const noexcept {
  return fd_;
}

/** @copydoc UniqueFd::operator bool */
UniqueFd::operator bool() const noexcept {
  return fd_ >= 0;
}

/** @copydoc UniqueFd::release */
int UniqueFd::release() noexcept {
  const int released = fd_;
  fd_ = -1;
  return released;
}

/** @copydoc UniqueFd::reset */
void UniqueFd::reset(int fd) noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
  }
  fd_ = fd;
}

/** @copydoc configure_no_sigpipe */
bool configure_no_sigpipe(int fd, std::string* message) {
#if defined(__APPLE__)
  int enabled = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) !=
      0) {
    *message = std::string("SO_NOSIGPIPE failed: ") + std::strerror(errno);
    return false;
  }
#else
  (void)fd;
  (void)message;
#endif
  return true;
}

/** @copydoc create_unix_stream_socket */
UniqueFd create_unix_stream_socket(std::string* message) {
  UniqueFd socket_fd(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!socket_fd) {
    *message = std::string("socket creation failed: ") + std::strerror(errno);
    return {};
  }
  if (!configure_no_sigpipe(socket_fd.get(), message) ||
      !configure_close_on_exec(socket_fd.get(), message)) {
    return {};
  }
  return socket_fd;
}

/** @copydoc connect_prepared_unix_socket_with_attempt */
bool connect_prepared_unix_socket_with_attempt(
    int fd, const std::string& socket_path,
    const std::function<bool()>& should_stop,
    const std::function<int(int, const void*, std::size_t)>& connect_attempt,
    std::string* message) {
  if (fd < 0 || !should_stop || !connect_attempt || message == nullptr) {
    return false;
  }
  if (socket_path.empty() || socket_path.front() != '/' ||
      socket_path.find('\0') != std::string::npos) {
    *message = "Unix socket path must be absolute";
    return false;
  }
  sockaddr_un address{};
  if (socket_path.size() + 1 > sizeof(address.sun_path)) {
    *message = "Unix socket path exceeds sun_path capacity";
    return false;
  }

  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
  const socklen_t address_length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + socket_path.size() + 1);
  const int original_flags = ::fcntl(fd, F_GETFL, 0);
  if (original_flags < 0 ||
      ::fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) != 0) {
    *message =
        std::string("socket nonblocking setup failed: ") + std::strerror(errno);
    return false;
  }

  bool connected = false;
  bool transient_connect_seen = false;
  std::string outcome_message;
  try {
    while (!connected && outcome_message.empty()) {
      if (should_stop()) {
        outcome_message = "connect interrupted";
        break;
      }
      const int connect_result = connect_attempt(
          fd, static_cast<const void*>(&address), address_length);
      const int connect_error = connect_result == 0 ? 0 : errno;
      if (connect_result == 0) {
        if (should_stop()) {
          outcome_message = "connect interrupted";
        } else {
          connected = true;
        }
        break;
      }
      if (connect_error == EISCONN && transient_connect_seen) {
        if (should_stop()) {
          outcome_message = "connect interrupted";
        } else {
          connected = true;
        }
        break;
      }
      if (connect_error == EAGAIN) {
        transient_connect_seen = true;
        if (!wait_connect_retry_slice(should_stop, &outcome_message)) {
          break;
        }
        continue;
      }
      if (!connect_pending(connect_error)) {
        outcome_message =
            std::string("connect failed: ") + std::strerror(connect_error);
        break;
      }
      transient_connect_seen = true;

      bool retry_connect = false;
      while (!connected && outcome_message.empty() && !retry_connect) {
        if (should_stop()) {
          outcome_message = "connect interrupted";
          break;
        }
        pollfd pending{fd, POLLOUT, 0};
        const int poll_result =
            ::poll(&pending, 1, kConnectPollSliceMilliseconds);
        if (poll_result == 0 || (poll_result < 0 && errno == EINTR)) {
          continue;
        }
        if (poll_result < 0) {
          const int poll_error = errno;
          outcome_message =
              std::string("connect poll failed: ") + std::strerror(poll_error);
          break;
        }
        if (should_stop()) {
          outcome_message = "connect interrupted";
          break;
        }

        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                         &socket_error_size) != 0) {
          const int option_error = errno;
          outcome_message = std::string("connect SO_ERROR failed: ") +
                            std::strerror(option_error);
          break;
        }
        if (socket_error == 0) {
          if (should_stop()) {
            outcome_message = "connect interrupted";
          } else {
            connected = true;
          }
        } else if (socket_error == EAGAIN) {
          retry_connect = true;
        } else if (!connect_pending(socket_error)) {
          outcome_message =
              std::string("connect failed: ") + std::strerror(socket_error);
        }
      }
      if (retry_connect &&
          !wait_connect_retry_slice(should_stop, &outcome_message)) {
        break;
      }
    }
  } catch (...) {
    (void)::fcntl(fd, F_SETFL, original_flags);
    throw;
  }

  if (::fcntl(fd, F_SETFL, original_flags) != 0) {
    *message =
        std::string("socket blocking restore failed: ") + std::strerror(errno);
    return false;
  }
  if (connected && should_stop()) {
    *message = "connect interrupted";
    return false;
  }
  if (!connected) {
    *message = std::move(outcome_message);
    return false;
  }
  return true;
}

/** @copydoc connect_prepared_unix_socket */
bool connect_prepared_unix_socket(int fd, const std::string& socket_path,
                                  const std::function<bool()>& should_stop,
                                  std::string* message) {
  return connect_prepared_unix_socket_with_attempt(
      fd, socket_path, should_stop,
      [](int socket_fd, const void* address, std::size_t address_length) {
        return ::connect(socket_fd, static_cast<const sockaddr*>(address),
                         static_cast<socklen_t>(address_length));
      },
      message);
}

/** @copydoc connect_unix_socket */
UniqueFd connect_unix_socket(const std::string& socket_path,
                             std::string* message) {
  UniqueFd socket_fd = create_unix_stream_socket(message);
  if (!socket_fd ||
      !connect_prepared_unix_socket(
          socket_fd.get(), socket_path, [] { return false; }, message)) {
    return {};
  }
  return socket_fd;
}

}  // namespace ps::ipc::internal
