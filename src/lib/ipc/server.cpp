#include "ipc/server.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ipc/codec.hpp"
#include "ipc/frame.hpp"
#include "ipc/unix_socket.hpp"

namespace ps::ipc::internal {
namespace {

/**
 * @brief Maximum number of active connection workers in version 1.
 *
 * @throws Nothing.
 * @note Excess accepted connections are closed without spawning a thread.
 */
constexpr std::size_t kMaximumClientWorkers = 32;

/**
 * @brief Outcome of probing a same-owner existing Unix socket.
 *
 * @throws Nothing.
 */
enum class ExistingSocketState {
  /** @brief A listener accepted the probe; startup must preserve it and fail.
   */
  Live,

  /** @brief Connect was refused or the inode disappeared; safe stale cleanup.
   */
  Stale,

  /** @brief Probe failed ambiguously; startup must preserve the path and fail.
   */
  Unsafe,
};

/**
 * @brief RAII cleanup guard for exactly one daemon-created socket inode.
 *
 * @throws std::bad_alloc when path storage cannot be allocated.
 * @note Cleanup compares owner, device, and inode before unlinking while the
 *       persistent lifecycle lock serializes cooperating daemon instances.
 */
class BoundSocketCleanup {
 public:
  /**
   * @brief Creates an inactive cleanup guard.
   *
   * @throws Nothing.
   */
  BoundSocketCleanup() = default;

  /**
   * @brief Prevents two guards from claiming one socket inode.
   *
   * @throws Nothing because this operation is unavailable.
   * @note Exactly one guard may own cleanup identity.
   */
  BoundSocketCleanup(const BoundSocketCleanup&) = delete;

  /**
   * @brief Prevents duplicating socket cleanup identity by assignment.
   *
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   * @note Use `reset()` before activating the same guard for another run.
   */
  BoundSocketCleanup& operator=(const BoundSocketCleanup&) = delete;

  /**
   * @brief Removes the matching owned socket inode when active.
   *
   * @throws Nothing.
   */
  ~BoundSocketCleanup() { reset(); }

  /**
   * @brief Records the inode created by a successful bind.
   *
   * @param path Bound socket path.
   * @param metadata `lstat` metadata captured after bind/chmod.
   * @throws std::bad_alloc if path storage cannot be allocated.
   * @note The guard becomes active only after all fields are copied.
   */
  void activate(std::string path, const struct stat& metadata) {
    path_ = std::move(path);
    device_ = metadata.st_dev;
    inode_ = metadata.st_ino;
    owner_ = metadata.st_uid;
    active_ = true;
  }

  /**
   * @brief Removes the exact recorded socket inode and deactivates the guard.
   *
   * @throws Nothing.
   */
  void reset() noexcept {
    if (!active_) {
      return;
    }
    struct stat current{};
    if (::lstat(path_.c_str(), &current) == 0 && S_ISSOCK(current.st_mode) &&
        current.st_dev == device_ && current.st_ino == inode_ &&
        current.st_uid == owner_) {
      (void)::unlink(path_.c_str());
    }
    active_ = false;
    path_.clear();
  }

 private:
  /** @brief Bound path recorded after successful bind. */
  std::string path_;

  /** @brief Device number of the daemon-created socket inode. */
  dev_t device_ = 0;

  /** @brief Inode number of the daemon-created socket. */
  ino_t inode_ = 0;

  /** @brief Effective user that created the socket. */
  uid_t owner_ = 0;

  /** @brief Whether cleanup currently owns an inode identity. */
  bool active_ = false;
};

/**
 * @brief Restores one process umask at scope exit.
 *
 * @throws Nothing.
 * @note Used only before worker threads start, so process-global umask changes
 *       cannot race file creation in another daemon thread.
 */
class ScopedUmask {
 public:
  /**
   * @brief Applies a restrictive temporary umask.
   *
   * @param mask Mask to apply until destruction.
   * @throws Nothing.
   */
  explicit ScopedUmask(mode_t mask) noexcept : previous_(::umask(mask)) {}

  /**
   * @brief Prevents multiple scopes from restoring one captured umask.
   *
   * @throws Nothing because this operation is unavailable.
   * @note The process-global value is restored by exactly one destructor.
   */
  ScopedUmask(const ScopedUmask&) = delete;

  /**
   * @brief Prevents overwriting captured umask restoration state.
   *
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   * @note Create a separate scope for a separate temporary mask.
   */
  ScopedUmask& operator=(const ScopedUmask&) = delete;

  /**
   * @brief Restores the previous process umask.
   *
   * @throws Nothing.
   */
  ~ScopedUmask() { (void)::umask(previous_); }

 private:
  /** @brief Process umask captured before the temporary change. */
  mode_t previous_;
};

/**
 * @brief Checks whether a directory is an owned protected runtime directory.
 *
 * @param path Existing candidate path.
 * @return True for a real directory owned by the effective uid with no group
 *         or other permission bits.
 * @throws Nothing.
 * @note `lstat` rejects symlink candidates.
 */
bool protected_directory(const std::filesystem::path& path) noexcept {
  struct stat metadata{};
  return ::lstat(path.c_str(), &metadata) == 0 && S_ISDIR(metadata.st_mode) &&
         metadata.st_uid == ::geteuid() && (metadata.st_mode & 0077) == 0;
}

/**
 * @brief Checks whether a path fits a Unix socket address including NUL.
 *
 * @param path Candidate filesystem path text.
 * @return True when nonempty and strictly shorter than `sun_path` capacity.
 * @throws Nothing.
 */
bool socket_path_fits(const std::string& path) noexcept {
  sockaddr_un address{};
  return !path.empty() && path.size() + 1 <= sizeof(address.sun_path);
}

/**
 * @brief Adds nonblocking and close-on-exec flags to the listener descriptor.
 *
 * @param fd Newly created listener socket.
 * @param message Receives an `fcntl` diagnostic on failure.
 * @return True when both descriptor flag updates succeed.
 * @throws std::bad_alloc if diagnostic construction fails.
 * @note Nonblocking accept prevents a poll-readiness withdrawal race from
 *       delaying self-pipe shutdown.
 */
bool configure_listener_flags(int fd, std::string* message) {
  const int status_flags = ::fcntl(fd, F_GETFL, 0);
  const int descriptor_flags = ::fcntl(fd, F_GETFD, 0);
  if (status_flags < 0 || descriptor_flags < 0 ||
      ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) != 0 ||
      ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
    *message = std::string("listener fcntl failed: ") + std::strerror(errno);
    return false;
  }
  return true;
}

/**
 * @brief Configures one accepted socket for blocking IO and close-on-exec.
 *
 * @param fd Newly accepted client descriptor.
 * @param message Receives an `fcntl` diagnostic on failure.
 * @return True when blocking and descriptor flags are established.
 * @throws std::bad_alloc if diagnostic construction fails.
 * @note This explicitly clears inherited `O_NONBLOCK` on BSD/macOS so frame IO
 *       cannot mistake a temporary `EAGAIN` for connection failure.
 */
bool configure_accepted_flags(int fd, std::string* message) {
  const int status_flags = ::fcntl(fd, F_GETFL, 0);
  const int descriptor_flags = ::fcntl(fd, F_GETFD, 0);
  if (status_flags < 0 || descriptor_flags < 0 ||
      ::fcntl(fd, F_SETFL, status_flags & ~O_NONBLOCK) != 0 ||
      ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
    *message =
        std::string("accepted socket fcntl failed: ") + std::strerror(errno);
    return false;
  }
  return true;
}

/**
 * @brief Creates and validates the direct parent of a socket path.
 *
 * @param socket_path Absolute requested socket path.
 * @param message Receives a startup diagnostic.
 * @return True when the parent is a uid-owned non-symlink directory with mode
 *         `0700` or stricter.
 * @throws std::bad_alloc if diagnostics cannot be allocated.
 * @throws std::filesystem::filesystem_error if parent inspection fails before
 *         the error-code-based directory creation boundary.
 * @note Newly created directory components inherit temporary umask `0077`, and
 *       a newly created direct parent is explicitly chmodded to `0700`.
 */
bool prepare_parent_directory(const std::filesystem::path& socket_path,
                              std::string* message) {
  if (!socket_path.is_absolute() || socket_path.filename().empty()) {
    *message = "daemon socket path must be absolute and name a file";
    return false;
  }
  const std::filesystem::path parent = socket_path.parent_path();
  const bool existed = std::filesystem::exists(parent);
  if (existed && !protected_directory(parent)) {
    *message = "daemon socket directory is not a protected uid-owned directory";
    return false;
  }
  std::error_code error;
  {
    ScopedUmask restrictive_umask(0077);
    std::filesystem::create_directories(parent, error);
  }
  if (error) {
    *message = "cannot create daemon socket directory: " + error.message();
    return false;
  }
  if (!existed && ::chmod(parent.c_str(), 0700) != 0) {
    *message = std::string("cannot protect daemon socket directory: ") +
               std::strerror(errno);
    return false;
  }
  if (!protected_directory(parent)) {
    *message = "daemon socket directory is not a protected uid-owned directory";
    return false;
  }
  return true;
}

/**
 * @brief Validates that an opened lock descriptor still names the protected
 * persistent lock path.
 *
 * @param fd Open lock descriptor.
 * @param path Persistent lock path inspected without following symlinks.
 * @param message Receives a validation diagnostic.
 * @return True only for one-link regular-file identity owned by the effective
 *         uid with exact mode `0600`.
 * @throws std::bad_alloc if diagnostic storage cannot be allocated.
 * @note Descriptor and path device/inode equality prevents accepting a lock
 *       path replaced between `open` and validation.
 */
bool validate_instance_lock(int fd, const std::string& path,
                            std::string* message) {
  struct stat descriptor_metadata{};
  struct stat path_metadata{};
  if (::fstat(fd, &descriptor_metadata) != 0 ||
      ::lstat(path.c_str(), &path_metadata) != 0) {
    *message = std::string("cannot inspect daemon instance lock: ") +
               std::strerror(errno);
    return false;
  }
  const bool valid =
      S_ISREG(descriptor_metadata.st_mode) && S_ISREG(path_metadata.st_mode) &&
      descriptor_metadata.st_uid == ::geteuid() &&
      path_metadata.st_uid == descriptor_metadata.st_uid &&
      (descriptor_metadata.st_mode & 07777) == 0600 &&
      (path_metadata.st_mode & 07777) == 0600 &&
      descriptor_metadata.st_nlink == 1 && path_metadata.st_nlink == 1 &&
      path_metadata.st_dev == descriptor_metadata.st_dev &&
      path_metadata.st_ino == descriptor_metadata.st_ino;
  if (!valid) {
    *message = "daemon instance lock is not a stable uid-owned mode-0600 file";
  }
  return valid;
}

/**
 * @brief Acquires the persistent per-socket daemon lifecycle lock.
 *
 * @param socket_path Validated absolute Unix socket path.
 * @param message Receives an open, validation, or contention diagnostic.
 * @return Owned locked descriptor, or an empty owner on failure.
 * @throws std::bad_alloc if lock-path or diagnostic storage cannot be
 *         allocated.
 * @note The `${socket_path}.lock` inode is never unlinked. Closing the returned
 *       descriptor releases `flock`, so a crashed daemon cannot strand the
 *       lock and later instances always synchronize on the same inode.
 */
UniqueFd acquire_instance_lock(const std::string& socket_path,
                               std::string* message) {
  const std::string lock_path = socket_path + ".lock";
  UniqueFd instance_lock;
  {
    ScopedUmask restrictive_umask(0077);
    instance_lock.reset(::open(
        lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
  }
  if (!instance_lock) {
    *message = std::string("cannot open daemon instance lock: ") +
               std::strerror(errno);
    return {};
  }
  if (!validate_instance_lock(instance_lock.get(), lock_path, message)) {
    return {};
  }
  int lock_result = -1;
  do {
    lock_result = ::flock(instance_lock.get(), LOCK_EX | LOCK_NB);
  } while (lock_result != 0 && errno == EINTR);
  if (lock_result != 0) {
    const int lock_error = errno;
    *message = lock_error == EWOULDBLOCK || lock_error == EAGAIN
                   ? "another daemon owns the requested socket lifecycle"
                   : std::string("cannot lock daemon instance file: ") +
                         std::strerror(lock_error);
    return {};
  }
  if (!validate_instance_lock(instance_lock.get(), lock_path, message)) {
    return {};
  }
  return instance_lock;
}

/**
 * @brief Probes one existing same-owner Unix socket without unlinking it.
 *
 * @param path Existing socket path.
 * @param message Receives an ambiguous probe diagnostic.
 * @return Live, stale, or unsafe state.
 * @throws std::bad_alloc if diagnostics cannot be allocated.
 * @note Only `ECONNREFUSED`/`ENOENT` establish staleness; other failures
 *       preserve the existing path.
 */
ExistingSocketState probe_existing_socket(const std::string& path,
                                          std::string* message) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  UniqueFd probe(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!probe) {
    *message = std::string("cannot create socket liveness probe: ") +
               std::strerror(errno);
    return ExistingSocketState::Unsafe;
  }
  if (!configure_no_sigpipe(probe.get(), message)) {
    return ExistingSocketState::Unsafe;
  }
  const int flags = ::fcntl(probe.get(), F_GETFL, 0);
  if (flags < 0 || ::fcntl(probe.get(), F_SETFL, flags | O_NONBLOCK) != 0) {
    *message = std::string("socket liveness probe fcntl failed: ") +
               std::strerror(errno);
    return ExistingSocketState::Unsafe;
  }
  const socklen_t length =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
  if (::connect(probe.get(), reinterpret_cast<sockaddr*>(&address), length) ==
      0) {
    return ExistingSocketState::Live;
  }
  int connect_error = errno;
  if (connect_error == EINPROGRESS || connect_error == EAGAIN) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (true) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) {
        *message = "existing socket liveness probe timed out";
        return ExistingSocketState::Unsafe;
      }
      pollfd descriptor{probe.get(), POLLOUT, 0};
      const int ready =
          ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      if (ready <= 0) {
        *message = ready == 0 ? "existing socket liveness probe timed out"
                              : std::string("socket probe poll failed: ") +
                                    std::strerror(errno);
        return ExistingSocketState::Unsafe;
      }
      socklen_t error_size = sizeof(connect_error);
      if (::getsockopt(probe.get(), SOL_SOCKET, SO_ERROR, &connect_error,
                       &error_size) != 0) {
        *message = std::string("socket probe SO_ERROR failed: ") +
                   std::strerror(errno);
        return ExistingSocketState::Unsafe;
      }
      break;
    }
  }
  if (connect_error == 0) {
    return ExistingSocketState::Live;
  }
  if (connect_error == ECONNREFUSED || connect_error == ENOENT) {
    return ExistingSocketState::Stale;
  }
  *message = std::string("existing socket liveness probe failed: ") +
             std::strerror(connect_error);
  return ExistingSocketState::Unsafe;
}

/**
 * @brief Refuses unsafe paths and removes only a proven same-owner stale
 * socket.
 *
 * @param path Requested socket filesystem path.
 * @param message Receives a startup diagnostic.
 * @return True when the path is absent or safely reclaimed.
 * @throws std::bad_alloc if diagnostics cannot be allocated.
 */
bool prepare_existing_path(const std::string& path, std::string* message) {
  struct stat metadata{};
  if (::lstat(path.c_str(), &metadata) != 0) {
    if (errno == ENOENT) {
      return true;
    }
    *message = std::string("cannot inspect daemon socket path: ") +
               std::strerror(errno);
    return false;
  }
  if (!S_ISSOCK(metadata.st_mode) || metadata.st_uid != ::geteuid()) {
    *message = "existing daemon path is not a same-owner Unix socket";
    return false;
  }
  const ExistingSocketState state = probe_existing_socket(path, message);
  if (state == ExistingSocketState::Live) {
    *message = "a live daemon already owns the requested socket";
    return false;
  }
  if (state == ExistingSocketState::Unsafe) {
    return false;
  }
  struct stat current{};
  if (::lstat(path.c_str(), &current) != 0) {
    if (errno == ENOENT) {
      return true;
    }
    *message = "stale socket identity cannot be revalidated";
    return false;
  }
  if (!S_ISSOCK(current.st_mode) || current.st_uid != metadata.st_uid ||
      current.st_dev != metadata.st_dev || current.st_ino != metadata.st_ino) {
    *message = "existing socket changed during stale-path validation";
    return false;
  }
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    *message = std::string("cannot remove same-owner stale socket: ") +
               std::strerror(errno);
    return false;
  }
  return true;
}

/**
 * @brief Creates, binds, protects, and listens on one Unix socket.
 *
 * @param path Validated absolute socket path.
 * @param cleanup Receives exact post-bind inode ownership.
 * @param instance_lock Receives the persistent lifecycle lock held through
 *        socket cleanup.
 * @param message Receives a startup diagnostic.
 * @return Owned listener descriptor or an empty owner.
 * @throws std::bad_alloc if diagnostics cannot be allocated.
 * @throws std::filesystem::filesystem_error if parent inspection fails before
 *         the error-code-based creation boundary.
 * @note Bind uses temporary umask `0077`; the resulting socket is chmodded
 *       `0600` and accepts no TCP traffic.
 */
UniqueFd create_listener(const std::string& path, BoundSocketCleanup* cleanup,
                         UniqueFd* instance_lock, std::string* message) {
  sockaddr_un address{};
  if (path.size() + 1 > sizeof(address.sun_path)) {
    *message = "daemon socket path exceeds sun_path capacity";
    return {};
  }
  if (!prepare_parent_directory(path, message)) {
    return {};
  }
  UniqueFd acquired_lock = acquire_instance_lock(path, message);
  if (!acquired_lock || !prepare_existing_path(path, message)) {
    return {};
  }
  UniqueFd listener(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!listener) {
    *message =
        std::string("listener socket creation failed: ") + std::strerror(errno);
    return {};
  }
  if (!configure_no_sigpipe(listener.get(), message)) {
    return {};
  }
  if (!configure_listener_flags(listener.get(), message)) {
    return {};
  }
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const socklen_t length =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
  {
    ScopedUmask restrictive_umask(0077);
    if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), length) !=
        0) {
      *message =
          std::string("daemon socket bind failed: ") + std::strerror(errno);
      return {};
    }
  }
  if (::chmod(path.c_str(), 0600) != 0) {
    *message = std::string("cannot set daemon socket mode 0600: ") +
               std::strerror(errno);
    (void)::unlink(path.c_str());
    return {};
  }
  struct stat metadata{};
  if (::lstat(path.c_str(), &metadata) != 0 || !S_ISSOCK(metadata.st_mode) ||
      metadata.st_uid != ::geteuid()) {
    *message = "bound daemon socket ownership verification failed";
    (void)::unlink(path.c_str());
    return {};
  }
  try {
    cleanup->activate(path, metadata);
  } catch (...) {
    (void)::unlink(path.c_str());
    throw;
  }
  if (::listen(listener.get(), static_cast<int>(kMaximumClientWorkers)) != 0) {
    *message =
        std::string("daemon socket listen failed: ") + std::strerror(errno);
    cleanup->reset();
    return {};
  }
  *instance_lock = std::move(acquired_lock);
  return listener;
}

}  // namespace

/** @copydoc default_socket_path */
std::string default_socket_path() {
  const char* xdg_runtime = std::getenv("XDG_RUNTIME_DIR");
  if (xdg_runtime != nullptr && xdg_runtime[0] != '\0') {
    const std::filesystem::path root(xdg_runtime);
    if (root.is_absolute() && protected_directory(root)) {
      const std::string candidate =
          (root / "photospider" / "photospiderd-v1.sock").string();
      if (socket_path_fits(candidate)) {
        return candidate;
      }
    }
  }
  const std::filesystem::path root =
      std::filesystem::path("/tmp") /
      ("photospider-" + std::to_string(::geteuid()));
  const std::string fallback = (root / "photospiderd-v1.sock").string();
  if (!socket_path_fits(fallback)) {
    throw std::runtime_error("default daemon socket path exceeds sun_path");
  }
  return fallback;
}

/**
 * @brief Mutable runtime behind the public internal `Server` shell.
 *
 * @throws std::bad_alloc when worker/runtime storage allocation fails.
 * @note Worker descriptors are protected from close/shutdown races by their
 *       per-worker mutex; every worker remains joinable.
 */
class Server::Impl {
 public:
  /**
   * @brief Creates runtime state and the Host-only request router.
   *
   * @param host Sole daemon Host instance.
   * @param service_version Reproducible project version.
   * @throws std::bad_alloc if metadata allocation fails.
   * @throws std::runtime_error if OS entropy fails.
   */
  Impl(Host& host, std::string service_version)
      : router_(host, std::move(service_version)) {}

  /**
   * @brief Stops any remaining runtime state.
   *
   * @throws Nothing.
   */
  ~Impl() { stop(); }

  /**
   * @brief Owns state for one joinable connection worker.
   *
   * @throws std::system_error if thread construction fails.
   * @note `fd_mutex` prevents shutdown from targeting a descriptor after the
   *       worker closed it and the OS potentially reused its number.
   */
  struct Worker {
    /** @brief Serializes descriptor shutdown/close and invalidation. */
    std::mutex fd_mutex;

    /** @brief Accepted descriptor owned until worker completion, or -1. */
    int fd = -1;

    /** @brief True after the connection loop releases its descriptor. */
    std::atomic<bool> done{false};

    /** @brief Joinable thread executing sequential request processing. */
    std::thread thread;
  };

  /**
   * @brief Runs the accept loop until stop fd readiness or fatal listener
   * error.
   *
   * @param options Socket selection options.
   * @param stop_fd Self-pipe read descriptor.
   * @return Success after normal stop or a startup/runtime failure.
   * @throws std::bad_alloc if non-connection runtime allocation fails.
   * @throws std::filesystem::filesystem_error if default socket path or parent
   *         directory inspection fails unexpectedly.
   * @throws std::runtime_error if the default path cannot fit the platform
   *         Unix-socket address or another startup invariant fails.
   * @throws std::system_error if a worker thread cannot be created.
   */
  OperationStatus run(const ServerOptions& options, int stop_fd) {
    stop();
    socket_path_ = options.socket_path.empty() ? default_socket_path()
                                               : options.socket_path;
    std::string message;
    listener_ = create_listener(socket_path_, &socket_cleanup_, &instance_lock_,
                                &message);
    if (!listener_) {
      socket_cleanup_.reset();
      return failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                            "internal_error", std::move(message));
    }
    running_ = true;
    OperationStatus outcome = ok_status();
    while (running_) {
      reap_finished_workers();
      pollfd descriptors[2] = {{listener_.get(), POLLIN, 0},
                               {stop_fd, POLLIN, 0}};
      const nfds_t count = stop_fd >= 0 ? 2 : 1;
      const int ready = ::poll(descriptors, count, -1);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        outcome = failure_status(
            OperationErrorDomain::Daemon, kInternalErrorCode, "internal_error",
            std::string("daemon poll failed: ") + std::strerror(errno));
        break;
      }
      if (stop_fd >= 0 &&
          (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        break;
      }
      if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        outcome = failure_status(OperationErrorDomain::Daemon,
                                 kInternalErrorCode, "internal_error",
                                 "daemon listener became unavailable");
        break;
      }
      if ((descriptors[0].revents & POLLIN) != 0) {
        if (!accept_one(&message)) {
          outcome =
              failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                             "internal_error", std::move(message));
          break;
        }
      }
    }
    stop();
    return outcome;
  }

  /**
   * @brief Executes deterministic listener/client/session/socket shutdown.
   *
   * @throws Nothing.
   */
  void stop() noexcept {
    running_ = false;
    listener_.reset();
    for (const std::shared_ptr<Worker>& worker : workers_) {
      std::lock_guard<std::mutex> lock(worker->fd_mutex);
      if (worker->fd >= 0) {
        (void)::shutdown(worker->fd, SHUT_RDWR);
      }
    }
    for (const std::shared_ptr<Worker>& worker : workers_) {
      if (worker->thread.joinable()) {
        worker->thread.join();
      }
    }
    workers_.clear();
    router_.close_all_sessions();
    socket_cleanup_.reset();
    instance_lock_.reset();
    socket_path_.clear();
  }

 private:
  /**
   * @brief Accepts one client, enforcing the 32-active-worker cap.
   *
   * @param message Receives a diagnostic for a fatal accept failure.
   * @return True after accepting/containing one connection or a transient
   *         accept race; false for a listener/resource failure that must stop
   *         the run loop.
   * @throws std::bad_alloc or std::system_error if diagnostics or worker
   *         creation fail.
   * @note Excess connections are closed immediately without spawning threads.
   *       `EINTR`, readiness withdrawal, and `ECONNABORTED` are
   * connection-level transient outcomes; descriptor/resource failures are fatal
   * so poll cannot spin on a perpetually readable listener.
   */
  bool accept_one(std::string* message) {
    const int accepted = ::accept(listener_.get(), nullptr, nullptr);
    if (accepted < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
          errno == ECONNABORTED) {
        return true;
      }
      *message = std::string("daemon accept failed: ") + std::strerror(errno);
      return false;
    }
    UniqueFd accepted_owner(accepted);
    std::string connection_message;
    if (!configure_no_sigpipe(accepted_owner.get(), &connection_message)) {
      return true;
    }
    if (!configure_accepted_flags(accepted_owner.get(), &connection_message)) {
      return true;
    }
    const std::size_t active = static_cast<std::size_t>(
        std::count_if(workers_.begin(), workers_.end(), [](const auto& worker) {
          return !worker->done.load(std::memory_order_acquire);
        }));
    if (active >= kMaximumClientWorkers) {
      return true;
    }
    auto worker = std::make_shared<Worker>();
    worker->fd = accepted_owner.get();
    workers_.push_back(worker);
    (void)accepted_owner.release();
    try {
      worker->thread = std::thread([this, worker] { serve_client(worker); });
    } catch (...) {
      UniqueFd cleanup(worker->fd);
      worker->fd = -1;
      workers_.pop_back();
      throw;
    }
    return true;
  }

  /**
   * @brief Processes framed requests sequentially for one connection.
   *
   * @param worker Stable shared state owning the accepted descriptor.
   * @throws Nothing; request, allocation, and IO failures close only this
   *         connection.
   * @note Router execution never overlaps socket IO while holding Host state.
   */
  void serve_client(const std::shared_ptr<Worker>& worker) noexcept {
    int fd = -1;
    {
      std::lock_guard<std::mutex> lock(worker->fd_mutex);
      fd = worker->fd;
    }
    try {
      while (fd >= 0) {
        FrameReadResult frame = read_frame(fd);
        if (frame.state != FrameReadState::Complete) {
          break;
        }
        std::string response = router_.route(frame.payload);
        if (!write_frame(fd, response).ok) {
          break;
        }
      }
    } catch (...) {
    }
    {
      std::lock_guard<std::mutex> lock(worker->fd_mutex);
      if (worker->fd >= 0) {
        ::close(worker->fd);
        worker->fd = -1;
      }
    }
    worker->done.store(true, std::memory_order_release);
  }

  /**
   * @brief Joins and erases completed connection workers.
   *
   * @throws Nothing.
   * @note Called only by the accept-loop thread, so the worker vector needs no
   *       separate mutex.
   */
  void reap_finished_workers() noexcept {
    auto iterator = workers_.begin();
    while (iterator != workers_.end()) {
      const std::shared_ptr<Worker>& worker = *iterator;
      if (!worker->done.load(std::memory_order_acquire)) {
        ++iterator;
        continue;
      }
      if (worker->thread.joinable()) {
        worker->thread.join();
      }
      iterator = workers_.erase(iterator);
    }
  }

  /** @brief Host-only request router and session owner. */
  RequestRouter router_;

  /** @brief Owned listener descriptor. */
  UniqueFd listener_;

  /** @brief Joinable accepted-connection worker states. */
  std::vector<std::shared_ptr<Worker>> workers_;

  /** @brief Exact inode cleanup guard for the bound socket. */
  BoundSocketCleanup socket_cleanup_;

  /** @brief Persistent per-socket lock held through exact socket cleanup. */
  UniqueFd instance_lock_;

  /** @brief Current explicit or resolved default socket path. */
  std::string socket_path_;

  /** @brief True while the foreground accept loop should continue. */
  bool running_ = false;
};

/** @copydoc Server::Server */
Server::Server(Host& host, std::string service_version)
    : impl_(std::make_unique<Impl>(host, std::move(service_version))) {}

/** @copydoc Server::~Server */
Server::~Server() = default;

/** @copydoc Server::run */
OperationStatus Server::run(const ServerOptions& options, int stop_fd) {
  return impl_->run(options, stop_fd);
}

}  // namespace ps::ipc::internal
