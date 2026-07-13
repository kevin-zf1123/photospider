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

/**
 * @brief Prepared filesystem Unix address and its exact kernel byte length.
 * @throws Nothing after construction.
 */
struct PreparedUnixAddress {
  /** @brief Zero-initialized address with family and path populated. */
  sockaddr_un address{};

  /** @brief Exact address bytes through the terminating path NUL. */
  socklen_t length = 0;
};

/**
 * @brief Mutable state for one logical nonblocking connection attempt.
 * @throws Nothing after construction.
 * @note This state never owns or closes the socket descriptor; helper-side
 *       diagnostic assignment can throw `std::bad_alloc`.
 */
struct ConnectAttemptState {
  /** @brief Whether the logical connection has completed successfully. */
  bool connected = false;

  /** @brief Whether `EAGAIN` or a pending connect has already been observed. */
  bool transient_connect_seen = false;

  /** @brief Empty while work may continue, otherwise the terminal diagnostic.
   */
  std::string outcome_message;
};

/**
 * @brief Outcome of one pending-connect writable/SO_ERROR wait loop.
 * @throws Nothing.
 */
enum class PendingConnectOutcome {
  /** @brief The socket completed its logical connection. */
  Connected,

  /** @brief Linux AF_UNIX reported no in-flight connect; retry same-fd connect.
   */
  RetryConnect,

  /** @brief Interruption or a terminal system error ended the attempt. */
  Failed,
};

/**
 * @brief Validates and encodes one absolute filesystem Unix socket address.
 * @param socket_path Candidate absolute socket path.
 * @param prepared Receives the zero-padded address and exact byte length.
 * @param message Receives the stable validation diagnostic on failure.
 * @return True when the path fits and was copied into `prepared`.
 * @throws std::bad_alloc if diagnostic assignment allocates.
 * @note The helper writes no socket state and does not retain `socket_path`.
 */
bool prepare_unix_address(const std::string& socket_path,
                          PreparedUnixAddress* prepared, std::string* message) {
  if (socket_path.empty() || socket_path.front() != '/' ||
      socket_path.find('\0') != std::string::npos) {
    *message = "Unix socket path must be absolute";
    return false;
  }
  if (socket_path.size() + 1 > sizeof(prepared->address.sun_path)) {
    *message = "Unix socket path exceeds sun_path capacity";
    return false;
  }

  prepared->address.sun_family = AF_UNIX;
  std::memcpy(prepared->address.sun_path, socket_path.c_str(),
              socket_path.size() + 1);
  prepared->length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                            socket_path.size() + 1);
  return true;
}

/**
 * @brief Enables temporary nonblocking mode and captures original flags.
 * @param fd Borrowed configured socket descriptor.
 * @param original_flags Receives the flags that must later be restored.
 * @param message Receives the stable `fcntl` diagnostic on failure.
 * @return True when `O_NONBLOCK` was installed.
 * @throws std::bad_alloc if diagnostic construction fails.
 * @note The helper never owns or closes `fd`.
 */
bool enable_nonblocking_connect(int fd, int* original_flags,
                                std::string* message) {
  *original_flags = ::fcntl(fd, F_GETFL, 0);
  if (*original_flags < 0 ||
      ::fcntl(fd, F_SETFL, *original_flags | O_NONBLOCK) != 0) {
    *message =
        std::string("socket nonblocking setup failed: ") + std::strerror(errno);
    return false;
  }
  return true;
}

/**
 * @brief Records successful kernel completion unless lifecycle stop won.
 * @param should_stop Latched lifecycle predicate.
 * @param state Mutable logical-attempt state.
 * @return Nothing.
 * @throws std::bad_alloc if interruption diagnostic assignment allocates, or
 *         whatever the supplied predicate throws.
 * @note The caller invokes this only after connect or `SO_ERROR` reports
 *       success; no descriptor ownership changes occur.
 */
void record_connect_completion(const std::function<bool()>& should_stop,
                               ConnectAttemptState* state) {
  if (should_stop()) {
    state->outcome_message = "connect interrupted";
  } else {
    state->connected = true;
  }
}

/**
 * @brief Waits for one pending nonblocking connect to finish or require retry.
 * @param fd Borrowed socket with an in-flight nonblocking connect.
 * @param should_stop Latched lifecycle predicate checked around every wait.
 * @param state Mutable logical-attempt state receiving completion/diagnostic.
 * @return Connected, same-fd retry, or terminal failure.
 * @throws std::bad_alloc if diagnostic construction fails, or whatever the
 *         supplied predicate throws.
 * @note The helper polls in bounded slices, preserves exact `SO_ERROR`
 *       classification, and never closes, reconnects, or writes through `fd`.
 */
PendingConnectOutcome wait_pending_connect(
    int fd, const std::function<bool()>& should_stop,
    ConnectAttemptState* state) {
  while (!state->connected && state->outcome_message.empty()) {
    if (should_stop()) {
      state->outcome_message = "connect interrupted";
      return PendingConnectOutcome::Failed;
    }
    pollfd pending{fd, POLLOUT, 0};
    const int poll_result = ::poll(&pending, 1, kConnectPollSliceMilliseconds);
    if (poll_result == 0 || (poll_result < 0 && errno == EINTR)) {
      continue;
    }
    if (poll_result < 0) {
      const int poll_error = errno;
      state->outcome_message =
          std::string("connect poll failed: ") + std::strerror(poll_error);
      return PendingConnectOutcome::Failed;
    }
    if (should_stop()) {
      state->outcome_message = "connect interrupted";
      return PendingConnectOutcome::Failed;
    }

    int socket_error = 0;
    socklen_t socket_error_size = sizeof(socket_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                     &socket_error_size) != 0) {
      const int option_error = errno;
      state->outcome_message = std::string("connect SO_ERROR failed: ") +
                               std::strerror(option_error);
      return PendingConnectOutcome::Failed;
    }
    if (socket_error == 0) {
      record_connect_completion(should_stop, state);
      return state->connected ? PendingConnectOutcome::Connected
                              : PendingConnectOutcome::Failed;
    }
    if (socket_error == EAGAIN) {
      return PendingConnectOutcome::RetryConnect;
    }
    if (!connect_pending(socket_error)) {
      state->outcome_message =
          std::string("connect failed: ") + std::strerror(socket_error);
      return PendingConnectOutcome::Failed;
    }
  }
  return state->connected ? PendingConnectOutcome::Connected
                          : PendingConnectOutcome::Failed;
}

/**
 * @brief Executes one logical same-fd connection including local retries.
 * @param fd Borrowed socket already placed in nonblocking mode.
 * @param prepared Validated address bytes for every injected attempt.
 * @param should_stop Latched lifecycle predicate.
 * @param connect_attempt Injected connect-compatible system-call seam.
 * @return Terminal connection state and any stable diagnostic.
 * @throws std::bad_alloc if diagnostic storage fails, or whatever either
 *         injected callable throws.
 * @note `EAGAIN` retries only an unstarted local AF_UNIX connection; pending
 *       connections complete through poll/`SO_ERROR`. No frame is retried and
 *       descriptor ownership remains with the caller.
 */
ConnectAttemptState run_nonblocking_connect(
    int fd, const PreparedUnixAddress& prepared,
    const std::function<bool()>& should_stop,
    const std::function<int(int, const void*, std::size_t)>& connect_attempt) {
  ConnectAttemptState state;
  while (!state.connected && state.outcome_message.empty()) {
    if (should_stop()) {
      state.outcome_message = "connect interrupted";
      break;
    }
    const int connect_result = connect_attempt(
        fd, static_cast<const void*>(&prepared.address), prepared.length);
    const int connect_error = connect_result == 0 ? 0 : errno;
    if (connect_result == 0 ||
        (connect_error == EISCONN && state.transient_connect_seen)) {
      record_connect_completion(should_stop, &state);
      break;
    }
    if (connect_error == EAGAIN) {
      state.transient_connect_seen = true;
      if (!wait_connect_retry_slice(should_stop, &state.outcome_message)) {
        break;
      }
      continue;
    }
    if (!connect_pending(connect_error)) {
      state.outcome_message =
          std::string("connect failed: ") + std::strerror(connect_error);
      break;
    }
    state.transient_connect_seen = true;
    if (wait_pending_connect(fd, should_stop, &state) ==
            PendingConnectOutcome::RetryConnect &&
        !wait_connect_retry_slice(should_stop, &state.outcome_message)) {
      break;
    }
  }
  return state;
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
  PreparedUnixAddress prepared;
  if (!prepare_unix_address(socket_path, &prepared, message)) {
    return false;
  }
  int original_flags = 0;
  if (!enable_nonblocking_connect(fd, &original_flags, message)) {
    return false;
  }

  ConnectAttemptState state;
  try {
    state = run_nonblocking_connect(fd, prepared, should_stop, connect_attempt);
  } catch (...) {
    (void)::fcntl(fd, F_SETFL, original_flags);
    throw;
  }

  if (::fcntl(fd, F_SETFL, original_flags) != 0) {
    *message =
        std::string("socket blocking restore failed: ") + std::strerror(errno);
    return false;
  }
  if (state.connected && should_stop()) {
    *message = "connect interrupted";
    return false;
  }
  if (!state.connected) {
    *message = std::move(state.outcome_message);
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
