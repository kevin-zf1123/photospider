#include "ipc/server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
#include "ipc/server_lifecycle_test_access.hpp"
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

/** @brief Kernel backlog request; it does not reserve a proof-only slot. */
constexpr int kListenerBacklog = static_cast<int>(kMaximumClientWorkers);

/** @brief Single absolute budget for connect, write, accept, and
 * classification. */
constexpr auto kListenerProofTimeout = std::chrono::seconds(2);

/** @brief Maximum stop/deadline observation interval during listener proof. */
constexpr int kListenerProofPollSliceMilliseconds = 10;

/**
 * @brief Byte count of one hexadecimal 128-bit listener proof token.
 *
 * @throws Nothing.
 */
constexpr std::size_t kListenerProofTokenBytes = 32;

/**
 * @brief Scalar filesystem identity sampled for a bound socket candidate.
 *
 * @throws Nothing.
 * @note The value owns no descriptor or pathname storage. It is only a
 *       candidate until pathname self-proof and final dirfd revalidation both
 *       succeed.
 */
struct BoundSocketCandidate {
  /** @brief Filesystem device containing the sampled socket. */
  dev_t device = 0;

  /** @brief Inode number of the sampled socket. */
  ino_t inode = 0;

  /** @brief Effective user that owned the sampled socket. */
  uid_t owner = 0;
};

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
 * @throws std::bad_alloc when pre-bind path storage cannot be allocated.
 * @note Cleanup fixes the parent directory descriptor before bind, remains
 *       inactive through Candidate capture and pathname self-proof, then
 *       compares type, owner, exact mode, device, and inode before `unlinkat`
 *       while the persistent lifecycle lock serializes cooperating instances.
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
   * @brief Preallocates the cleanup path and fixes its parent directory.
   *
   * @param path Validated absolute socket path.
   * @param message Receives an open or validation diagnostic.
   * @return True when the protected parent descriptor and basename are owned.
   * @throws std::bad_alloc if path or diagnostic storage cannot allocate.
   * @note This operation must finish before bind. A prepared inactive guard
   *       never removes a pathname.
   */
  bool prepare(const std::string& path, std::string* message) {
    reset();
    const std::size_t separator = path.find_last_of('/');
    if (separator == std::string::npos || separator + 1 >= path.size()) {
      *message = "daemon socket cleanup path is invalid";
      return false;
    }
    std::string parent = separator == 0 ? "/" : path.substr(0, separator);
    std::string name = path.substr(separator + 1);
    UniqueFd parent_fd(::open(parent.c_str(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!parent_fd) {
      *message = std::string("cannot open daemon socket parent: ") +
                 std::strerror(errno);
      return false;
    }
    struct stat metadata{};
    if (::fstat(parent_fd.get(), &metadata) != 0 ||
        !S_ISDIR(metadata.st_mode) || metadata.st_uid != ::geteuid() ||
        (metadata.st_mode & 0077) != 0) {
      *message = "daemon socket parent descriptor is not protected";
      return false;
    }
    parent_fd_ = std::move(parent_fd);
    parent_path_ = std::move(parent);
    parent_device_ = metadata.st_dev;
    parent_inode_ = metadata.st_ino;
    name_ = std::move(name);
    return true;
  }

  /**
   * @brief Samples the freshly bound pathname as an inactive candidate.
   *
   * @param message Receives a fixed startup diagnostic on failure.
   * @return True only for a same-owner Unix socket with exact mode `0600`.
   * @throws std::bad_alloc only if assigning the failure diagnostic allocates.
   * @note Validation uses the parent descriptor and basename prepared before
   *       bind. The method copies scalar identity only and never authorizes
   *       cleanup.
   */
  bool capture_candidate(std::string* message) {
    if (!parent_path_matches()) {
      *message =
          "daemon socket parent identity changed before Candidate capture";
      return false;
    }
    struct stat metadata{};
    if (::fstatat(parent_fd_.get(), name_.c_str(), &metadata,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISSOCK(metadata.st_mode) || metadata.st_uid != ::geteuid() ||
        (metadata.st_mode & 07777) != 0600) {
      *message = "bound daemon socket identity or mode verification failed";
      return false;
    }
    candidate_.device = metadata.st_dev;
    candidate_.inode = metadata.st_ino;
    candidate_.owner = metadata.st_uid;
    candidate_captured_ = true;
    return true;
  }

  /**
   * @brief Final-revalidates the candidate and authorizes later cleanup.
   *
   * @return True only when type, uid, exact mode, device, and inode still equal
   *         the captured candidate.
   * @throws Nothing.
   * @note The caller must first prove that the pathname routes to the original
   *       listener. This operation performs no allocation or descriptor
   *       acquisition; success changes only the scalar active flag.
   */
  bool activate_after_proof() noexcept {
    if (!candidate_captured_ || !parent_path_matches()) {
      return false;
    }
    struct stat current{};
    if (::fstatat(parent_fd_.get(), name_.c_str(), &current,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISSOCK(current.st_mode) || current.st_uid != candidate_.owner ||
        (current.st_mode & 07777) != 0600 ||
        current.st_dev != candidate_.device ||
        current.st_ino != candidate_.inode) {
      return false;
    }
    active_ = true;
    return true;
  }

  /**
   * @brief Revalidates the recorded socket identity and deactivates the guard.
   *
   * @throws Nothing.
   * @note The fixed parent descriptor prevents parent-path redirection.
   *       Portable POSIX still exposes a same-uid replacement window between
   *       the final `fstatat` and `unlinkat`; a mismatched identity observed by
   *       the guard is always preserved.
   */
  void reset() noexcept {
    if (active_ && parent_path_matches()) {
      struct stat current{};
      if (::fstatat(parent_fd_.get(), name_.c_str(), &current,
                    AT_SYMLINK_NOFOLLOW) == 0 &&
          S_ISSOCK(current.st_mode) && (current.st_mode & 07777) == 0600 &&
          current.st_dev == candidate_.device &&
          current.st_ino == candidate_.inode &&
          current.st_uid == candidate_.owner) {
        (void)::unlinkat(parent_fd_.get(), name_.c_str(), 0);
      }
    }
    active_ = false;
    candidate_captured_ = false;
    candidate_ = {};
    parent_fd_.reset();
    parent_path_.clear();
    parent_device_ = 0;
    parent_inode_ = 0;
    name_.clear();
  }

 private:
  /**
   * @brief Verifies that the configured parent pathname still names the dirfd.
   * @return True only for the captured device/inode pair and protected owner.
   * @throws Nothing.
   * @note This scalar check fails closed on stable parent rename/recreation but
   *       cannot make pathname lookup atomic against a same-uid directory
   *       writer.
   */
  bool parent_path_matches() const noexcept {
    struct stat current{};
    return !parent_path_.empty() &&
           ::lstat(parent_path_.c_str(), &current) == 0 &&
           S_ISDIR(current.st_mode) && current.st_uid == ::geteuid() &&
           (current.st_mode & 0077) == 0 && current.st_dev == parent_device_ &&
           current.st_ino == parent_inode_;
  }

  /** @brief Fixed protected parent directory acquired before bind. */
  UniqueFd parent_fd_;

  /** @brief Original parent pathname used only for scalar identity checks. */
  std::string parent_path_;

  /** @brief Device identity captured from the fixed parent descriptor. */
  dev_t parent_device_ = 0;

  /** @brief Inode identity captured from the fixed parent descriptor. */
  ino_t parent_inode_ = 0;

  /** @brief Preallocated final pathname component below `parent_fd_`. */
  std::string name_;

  /** @brief Inactive scalar identity captured after bind. */
  BoundSocketCandidate candidate_;

  /** @brief Whether `candidate_` contains a validated first sample. */
  bool candidate_captured_ = false;

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
 * @brief Proves that a pathname routes to the original listening descriptor.
 *
 * @param path Bound Unix socket pathname.
 * @param listener Original descriptor on which `bind` and `listen` succeeded.
 * @param token High-entropy fixed-size payload generated before bind.
 * @param pending_clients Pre-reserved owner for accepted non-probe clients.
 * @param undecided Pre-reserved owner for matching or empty prefixes.
 * @param message Receives a startup diagnostic on failure.
 * @return True only after the framed token sent through `path` is identified
 *         on an accepted connection from `listener`.
 * @throws std::bad_alloc if a syscall diagnostic cannot be allocated.
 * @note The method peeks at most one complete probe frame and never consumes
 *       bytes from nonmatching clients. Such descriptors remain bounded by the
 *       32-client limit and are transferred to normal admission only after the
 *       request-router runtime starts. Once the unique complete proof is found,
 *       every other still-undecided descriptor is retained as an ordinary
 *       client, including an empty or matching partial frame. Every failure
 *       leaves the pathname untouched and closes the self connector plus all
 *       pending descriptors through their owners.
 */
bool prove_listener_path(const std::string& path, int listener,
                         const std::string& token,
                         std::vector<UniqueFd>* pending_clients,
                         std::vector<UniqueFd>* undecided, int stop_fd,
                         const ServerLifecycleTestDependencies* dependencies,
                         std::string* message) {
  if (token.size() != kListenerProofTokenBytes) {
    *message = "listener self-proof token length invariant failed";
    return false;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + kListenerProofTimeout;
  UniqueFd self_connector = create_unix_stream_socket(message);
  if (!self_connector) {
    return false;
  }
  const int connector_flags = ::fcntl(self_connector.get(), F_GETFL, 0);
  if (connector_flags < 0 || ::fcntl(self_connector.get(), F_SETFL,
                                     connector_flags | O_NONBLOCK) != 0) {
    *message = std::string("listener self-proof nonblocking setup failed: ") +
               std::strerror(errno);
    return false;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const socklen_t address_length =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
  std::array<unsigned char, sizeof(std::uint32_t) + kListenerProofTokenBytes>
      expected{};
  const std::uint32_t network_length =
      htonl(static_cast<std::uint32_t>(token.size()));
  std::memcpy(expected.data(), &network_length, sizeof(network_length));
  std::memcpy(expected.data() + sizeof(network_length), token.data(),
              token.size());
  undecided->clear();
  bool connector_started = false;
  bool connector_pending = false;
  bool connector_connected = false;
  std::size_t written = 0;
  const std::size_t configured_chunk =
      dependencies == nullptr ? 0 : dependencies->proof_write_chunk_bytes;
  const std::size_t write_chunk =
      configured_chunk == 0 ? expected.size() : configured_chunk;
  const std::size_t configured_limit =
      dependencies == nullptr ? 0 : dependencies->proof_write_limit_bytes;
  const std::size_t write_limit =
      configured_limit == 0 ? expected.size() : configured_limit;

  const auto stop_is_ready = [&] {
    if (stop_fd < 0) {
      return false;
    }
    pollfd stop{stop_fd, POLLIN, 0};
    return ::poll(&stop, 1, 0) > 0 &&
           (stop.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
  };
  const auto cancelled = [&] {
    return std::chrono::steady_clock::now() >= deadline || stop_is_ready();
  };

  while (!cancelled()) {
    while (true) {
      const int accepted = ::accept(listener, nullptr, nullptr);
      if (accepted < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        *message = std::string("listener pathname self-proof accept failed: ") +
                   std::strerror(errno);
        return false;
      }
      if (pending_clients->size() + undecided->size() >=
          kMaximumClientWorkers + 1) {
        (void)::close(accepted);
        *message = "listener self-proof exceeded 32-client capacity";
        return false;
      }
      undecided->emplace_back(accepted);
    }

    for (auto iterator = undecided->begin(); iterator != undecided->end();) {
      std::array<unsigned char,
                 sizeof(std::uint32_t) + kListenerProofTokenBytes>
          observed{};
      const ssize_t peeked = ::recv(iterator->get(), observed.data(),
                                    observed.size(), MSG_PEEK | MSG_DONTWAIT);
      if (peeked < 0 && errno == EINTR) {
        continue;
      }
      if (peeked < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        *message =
            std::string("listener client peek failed during self-proof: ") +
            std::strerror(errno);
        return false;
      }
      const std::size_t visible =
          peeked > 0 ? static_cast<std::size_t>(peeked) : 0;
      const bool matching =
          visible == 0 ||
          std::equal(observed.begin(), observed.begin() + visible,
                     expected.begin());
      if (matching && visible == expected.size()) {
        UniqueFd completed_proof = std::move(*iterator);
        iterator = undecided->erase(iterator);
        for (UniqueFd& candidate : *undecided) {
          if (pending_clients->size() >= kMaximumClientWorkers) {
            *message = "listener self-proof exceeded 32-client capacity";
            return false;
          }
          std::string connection_message;
          if (!configure_no_sigpipe(candidate.get(), &connection_message) ||
              !configure_accepted_flags(candidate.get(), &connection_message)) {
            *message =
                "pending client setup failed after listener self-proof: " +
                connection_message;
            return false;
          }
          pending_clients->push_back(std::move(candidate));
        }
        undecided->clear();
        return true;
      }
      if (matching) {
        if (visible > 0 && dependencies != nullptr &&
            dependencies->proof_prefix_observer != nullptr) {
          dependencies->proof_prefix_observer(dependencies->context, visible);
        }
        ++iterator;
        continue;
      }
      if (pending_clients->size() >= kMaximumClientWorkers) {
        *message = "listener self-proof exceeded 32-client capacity";
        return false;
      }
      std::string connection_message;
      if (!configure_no_sigpipe(iterator->get(), &connection_message) ||
          !configure_accepted_flags(iterator->get(), &connection_message)) {
        *message = "pending client setup failed during listener self-proof: " +
                   connection_message;
        return false;
      }
      pending_clients->push_back(std::move(*iterator));
      iterator = undecided->erase(iterator);
    }

    if (!connector_connected) {
      if (connector_pending) {
        pollfd pending{self_connector.get(), POLLOUT, 0};
        const int ready = ::poll(&pending, 1, 0);
        if (ready > 0 &&
            (pending.revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
          int socket_error = 0;
          socklen_t socket_error_size = sizeof(socket_error);
          if (::getsockopt(self_connector.get(), SOL_SOCKET, SO_ERROR,
                           &socket_error, &socket_error_size) != 0) {
            *message = std::string("listener self-connect SO_ERROR failed: ") +
                       std::strerror(errno);
            return false;
          }
          if (socket_error == 0) {
            connector_connected = true;
            connector_pending = false;
          } else if (socket_error == EAGAIN) {
            connector_pending = false;
          } else if (socket_error != EINPROGRESS && socket_error != EALREADY) {
            *message = std::string("listener pathname self-connect failed: ") +
                       std::strerror(socket_error);
            return false;
          }
        }
      } else {
        const int result =
            ::connect(self_connector.get(),
                      reinterpret_cast<sockaddr*>(&address), address_length);
        const int connect_error = result == 0 ? 0 : errno;
        if (result == 0 || (connector_started && connect_error == EISCONN)) {
          connector_connected = true;
        } else if (connect_error == EINPROGRESS || connect_error == EALREADY) {
          connector_started = true;
          connector_pending = true;
        } else if (connect_error == EAGAIN || connect_error == EINTR) {
          connector_started = true;
        } else {
          *message = std::string("listener pathname self-connect failed: ") +
                     std::strerror(connect_error);
          return false;
        }
      }
    }

    if (connector_connected && written < write_limit) {
      const std::size_t amount = std::min(write_chunk, write_limit - written);
      int send_flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
      send_flags |= MSG_NOSIGNAL;
#endif
      const ssize_t sent = ::send(
          self_connector.get(), expected.data() + written, amount, send_flags);
      if (sent > 0) {
        written += static_cast<std::size_t>(sent);
      } else if (sent < 0 && errno != EINTR && errno != EAGAIN &&
                 errno != EWOULDBLOCK) {
        *message = std::string("listener pathname self-proof write failed: ") +
                   std::strerror(errno);
        return false;
      }
    }

    std::array<pollfd, kMaximumClientWorkers + 4> descriptors{};
    nfds_t descriptor_count = 0;
    descriptors[descriptor_count++] = {listener, POLLIN, 0};
    descriptors[descriptor_count++] = {self_connector.get(), POLLOUT, 0};
    if (stop_fd >= 0) {
      descriptors[descriptor_count++] = {stop_fd, POLLIN, 0};
    }
    for (const UniqueFd& candidate : *undecided) {
      descriptors[descriptor_count++] = {candidate.get(), POLLIN, 0};
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    const int timeout =
        std::max(0, std::min(kListenerProofPollSliceMilliseconds,
                             static_cast<int>(remaining.count())));
    const int ready = ::poll(descriptors.data(), descriptor_count, timeout);
    if (ready < 0 && errno != EINTR) {
      *message = std::string("listener pathname self-proof poll failed: ") +
                 std::strerror(errno);
      return false;
    }
  }
  *message = stop_is_ready() ? "listener pathname self-proof cancelled"
                             : "listener pathname self-proof deadline exceeded";
  return false;
}

/**
 * @brief Creates, binds, protects, and listens on one Unix socket.
 *
 * @param path Validated absolute socket path.
 * @param cleanup Receives inactive Candidate identity and becomes Active only
 *        after proof plus final parent/path identity revalidation.
 * @param instance_lock Receives the persistent lifecycle lock held through
 *        socket cleanup.
 * @param message Receives a startup diagnostic.
 * @param pending_clients Receives bounded non-probe clients accepted during
 *        pathname proof; storage is reserved before bind.
 * @param stop_fd Optional lifecycle stop descriptor observed during proof.
 * @param test_dependencies Optional non-installed deterministic syscall seam;
 *        production callers always use the default null value.
 * @return Owned listener descriptor or an empty owner.
 * @throws std::bad_alloc if diagnostics cannot be allocated.
 * @throws std::filesystem::filesystem_error if parent inspection fails before
 *         the error-code-based creation boundary.
 * @throws std::runtime_error if proof-token entropy generation fails.
 * @note Bind uses temporary umask `0177`, which creates exact mode `0600`
 *       directly. The fixed-dirfd first sample captures only Candidate scalar
 *       identity. Cleanup becomes active without allocation only after
 *       pathname self-proof and final exact revalidation. Every earlier
 *       failure preserves the pathname.
 */
UniqueFd create_listener(
    const std::string& path, BoundSocketCleanup* cleanup,
    UniqueFd* instance_lock, std::string* message,
    std::vector<UniqueFd>* pending_clients, int stop_fd,
    const ServerLifecycleTestDependencies* test_dependencies = nullptr) {
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
  if (!cleanup->prepare(path, message)) {
    return {};
  }
  pending_clients->clear();
  pending_clients->reserve(kMaximumClientWorkers);
  std::vector<UniqueFd> proof_candidates;
  proof_candidates.reserve(kMaximumClientWorkers + 1);
  const std::string proof_token = generate_opaque_id();
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
    ScopedUmask restrictive_umask(0177);
    if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), length) !=
        0) {
      *message =
          std::string("daemon socket bind failed: ") + std::strerror(errno);
      return {};
    }
  }
  if (test_dependencies != nullptr &&
      test_dependencies->stage_hook != nullptr) {
    test_dependencies->stage_hook(test_dependencies->context,
                                  ServerLifecycleTestStage::AfterBind,
                                  path.c_str());
  }
  if (!cleanup->capture_candidate(message)) {
    return {};
  }
  if (test_dependencies != nullptr &&
      test_dependencies->stage_hook != nullptr) {
    test_dependencies->stage_hook(
        test_dependencies->context,
        ServerLifecycleTestStage::AfterCandidateBeforeListen, path.c_str());
  }
  const auto listen_call =
      test_dependencies != nullptr && test_dependencies->listen_call != nullptr
          ? test_dependencies->listen_call
          : ::listen;
  if (listen_call(listener.get(), kListenerBacklog) != 0) {
    const int listen_error = errno;
    *message = std::string("daemon socket listen failed: ") +
               std::strerror(listen_error);
    errno = listen_error;
    return {};
  }
  if (test_dependencies != nullptr &&
      test_dependencies->stage_hook != nullptr) {
    test_dependencies->stage_hook(
        test_dependencies->context,
        ServerLifecycleTestStage::AfterListenBeforeProof, path.c_str());
  }
  if (!prove_listener_path(path, listener.get(), proof_token, pending_clients,
                           &proof_candidates, stop_fd, test_dependencies,
                           message)) {
    return {};
  }
  if (test_dependencies != nullptr &&
      test_dependencies->stage_hook != nullptr) {
    test_dependencies->stage_hook(
        test_dependencies->context,
        ServerLifecycleTestStage::AfterProofBeforeFinalRevalidate,
        path.c_str());
  }
  if (!cleanup->activate_after_proof()) {
    *message = "bound daemon socket final identity verification failed";
    return {};
  }
  if (test_dependencies != nullptr &&
      test_dependencies->stage_hook != nullptr) {
    test_dependencies->stage_hook(
        test_dependencies->context,
        ServerLifecycleTestStage::AfterActivationBeforeRuntimeStart,
        path.c_str());
  }
  *instance_lock = std::move(acquired_lock);
  return listener;
}

}  // namespace

/** @copydoc test_create_listener */
OperationStatus test_create_listener(
    const std::string& path,
    const ServerLifecycleTestDependencies& dependencies) {
  BoundSocketCleanup cleanup;
  UniqueFd instance_lock;
  std::string message;
  std::vector<UniqueFd> pending_clients;
  UniqueFd listener = create_listener(path, &cleanup, &instance_lock, &message,
                                      &pending_clients, -1, &dependencies);
  if (!listener) {
    return failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                          "internal_error", std::move(message));
  }
  if (dependencies.before_active_cleanup != nullptr) {
    dependencies.before_active_cleanup(dependencies.context, listener.get(),
                                       path.c_str());
  }
  cleanup.reset();
  listener.reset();
  instance_lock.reset();
  return ok_status();
}

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
   * @brief Creates runtime state with an injected private router policy.
   * @param host Sole daemon Host instance.
   * @param service_version Reproducible project version.
   * @param dependencies Snapshot, compute, and output runtime dependencies.
   * @throws std::bad_alloc if metadata or callback storage cannot allocate.
   * @throws std::invalid_argument if an injected policy is inconsistent.
   * @throws std::runtime_error if OS entropy fails.
   * @note Product construction uses the two-argument overload; this internal
   *       seam exists for deterministic separate-process fixtures only.
   */
  Impl(Host& host, std::string service_version,
       RequestRouterRuntimeDependencies dependencies)
      : router_(host, std::move(service_version), std::move(dependencies)) {}

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
   * @param test_dependencies Optional borrowed source-tree listener seam;
   *        production callers leave it null.
   * @return Success after normal stop or a startup/runtime failure.
   * @throws std::bad_alloc if non-connection runtime allocation fails.
   * @throws std::filesystem::filesystem_error if default socket path or parent
   *         directory inspection fails unexpectedly.
   * @throws std::runtime_error if the default path cannot fit the platform
   *         Unix-socket address or another startup invariant fails.
   * @throws std::system_error if a worker thread cannot be created.
   * @note Pending descriptors captured during pathname proof enter ordinary
   *       worker admission only after `RequestRouter::start_runtime` succeeds.
   */
  OperationStatus run(
      const ServerOptions& options, int stop_fd,
      const ServerLifecycleTestDependencies* test_dependencies = nullptr) {
    stop();
    active_test_dependencies_ = test_dependencies;
    socket_path_ = options.socket_path.empty() ? default_socket_path()
                                               : options.socket_path;
    std::string message;
    std::vector<UniqueFd> pending_clients;
    listener_ =
        create_listener(socket_path_, &socket_cleanup_, &instance_lock_,
                        &message, &pending_clients, stop_fd, test_dependencies);
    if (!listener_) {
      socket_cleanup_.reset();
      active_test_dependencies_ = nullptr;
      return failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                            "internal_error", std::move(message));
    }
    OperationStatus runtime_started;
    try {
      runtime_started =
          test_dependencies != nullptr && test_dependencies->fail_runtime_start
              ? failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                               "internal_error",
                               "injected request-router startup failure")
              : router_.start_runtime(socket_path_, instance_lock_.get());
    } catch (...) {
      cleanup_listener(test_dependencies);
      instance_lock_.reset();
      socket_path_.clear();
      active_test_dependencies_ = nullptr;
      throw;
    }
    if (!runtime_started.ok) {
      cleanup_listener(test_dependencies);
      instance_lock_.reset();
      socket_path_.clear();
      active_test_dependencies_ = nullptr;
      return runtime_started;
    }
    try {
      for (UniqueFd& pending_client : pending_clients) {
        admit_client(std::move(pending_client));
      }
      pending_clients.clear();
    } catch (...) {
      stop();
      throw;
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
   * @brief Executes deterministic admission/client/compute/session shutdown.
   *
   * @throws Nothing.
   * @note Admission stops before descriptors are woken; accepted compute drains
   *       and joins before Host sessions and socket ownership are released.
   */
  void stop() noexcept {
    running_ = false;
    router_.begin_shutdown();
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
    router_.finish_shutdown();
    cleanup_listener(active_test_dependencies_);
    instance_lock_.reset();
    socket_path_.clear();
    active_test_dependencies_ = nullptr;
  }

 private:
  /**
   * @brief Identity-unlinks an Active pathname while its listener stays open.
   * @param dependencies Optional source-tree cleanup observer.
   * @throws Nothing.
   * @note The listener descriptor is closed only after identity-aware cleanup,
   *       eliminating the close-to-unlink inode-reuse interval.
   */
  void cleanup_listener(
      const ServerLifecycleTestDependencies* dependencies) noexcept {
    if (dependencies != nullptr &&
        dependencies->before_active_cleanup != nullptr) {
      dependencies->before_active_cleanup(
          dependencies->context, listener_.get(), socket_path_.c_str());
    }
    socket_cleanup_.reset();
    listener_.reset();
  }
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
   *       connection-level transient outcomes; descriptor/resource failures
   *       are fatal so poll cannot spin on a perpetually readable listener.
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
    if (active_worker_count() >= kMaximumClientWorkers) {
      return true;
    }
    admit_client(std::move(accepted_owner));
    return true;
  }

  /**
   * @brief Counts live normal workers at the accept-loop observation point.
   *
   * @return Number of workers that have not published completion.
   * @throws Nothing.
   * @note Only the foreground server thread calls this helper, so vector
   *       traversal requires no additional mutex.
   */
  std::size_t active_worker_count() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(workers_.begin(), workers_.end(), [](const auto& worker) {
          return !worker->done.load(std::memory_order_acquire);
        }));
  }

  /**
   * @brief Transfers one configured descriptor into normal worker admission.
   *
   * @param accepted_owner Sole owner of a blocking accepted client descriptor.
   * @throws std::bad_alloc if worker/vector storage cannot allocate.
   * @throws std::system_error if thread creation fails.
   * @note The request-router runtime must already be active. On failure the
   *       descriptor is closed and no joinable worker is lost.
   */
  void admit_client(UniqueFd accepted_owner) {
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

  /** @brief Identity-aware cleanup guard for the bound socket. */
  BoundSocketCleanup socket_cleanup_;

  /** @brief Persistent lock held through identity-checked socket cleanup. */
  UniqueFd instance_lock_;

  /** @brief Current explicit or resolved default socket path. */
  std::string socket_path_;

  /** @brief True while the foreground accept loop should continue. */
  bool running_ = false;

  /** @brief Borrowed non-installed callbacks valid for the active test run. */
  const ServerLifecycleTestDependencies* active_test_dependencies_ = nullptr;
};

/** @copydoc Server::Server */
Server::Server(Host& host, std::string service_version)
    : Server(host, std::move(service_version),
             RequestRouterRuntimeDependencies{}) {  // NOLINT
}  // NOLINT(whitespace/indent_namespace)

/** @copydoc Server::Server */
Server::Server(Host& host, std::string service_version,
               RequestRouterRuntimeDependencies dependencies)
    : impl_(std::make_unique<Impl>(host, std::move(service_version),
                                   std::move(dependencies))) {  // NOLINT
}  // NOLINT(whitespace/indent_namespace)

/** @copydoc Server::~Server */
Server::~Server() = default;

/** @copydoc Server::run */
OperationStatus Server::run(const ServerOptions& options, int stop_fd) {
  return impl_->run(options, stop_fd);
}

/** @copydoc test_run_server */
OperationStatus test_run_server(
    Server& server, const ServerOptions& options, int stop_fd,
    const ServerLifecycleTestDependencies& dependencies) {
  return server.impl_->run(options, stop_fd, &dependencies);
}

}  // namespace ps::ipc::internal
