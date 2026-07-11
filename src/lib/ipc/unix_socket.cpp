#include "ipc/unix_socket.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>

namespace ps::ipc::internal {
namespace {

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

/** @copydoc connect_unix_socket */
UniqueFd connect_unix_socket(const std::string& socket_path,
                             std::string* message) {
  if (socket_path.empty() || socket_path.front() != '/' ||
      socket_path.find('\0') != std::string::npos) {
    *message = "Unix socket path must be absolute";
    return {};
  }
  sockaddr_un address{};
  if (socket_path.size() + 1 > sizeof(address.sun_path)) {
    *message = "Unix socket path exceeds sun_path capacity";
    return {};
  }

  UniqueFd socket_fd(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!socket_fd) {
    *message = std::string("socket creation failed: ") + std::strerror(errno);
    return {};
  }
  if (!configure_no_sigpipe(socket_fd.get(), message)) {
    return {};
  }
  if (!configure_close_on_exec(socket_fd.get(), message)) {
    return {};
  }

  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
  const socklen_t address_length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + socket_path.size() + 1);
  if (::connect(socket_fd.get(), reinterpret_cast<sockaddr*>(&address),
                address_length) != 0) {
    *message = std::string("connect failed: ") + std::strerror(errno);
    return {};
  }
  return socket_fd;
}

}  // namespace ps::ipc::internal
