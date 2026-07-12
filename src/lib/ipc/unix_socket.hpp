#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace ps::ipc::internal {

/**
 * @brief Move-only RAII owner for one Unix descriptor.
 *
 * @throws Nothing.
 * @note The class owns only descriptor lifecycle; protocol reads and writes are
 *       performed by the frame layer.
 */
class UniqueFd {
 public:
  /**
   * @brief Creates an empty descriptor owner.
   *
   * @throws Nothing.
   */
  UniqueFd() noexcept = default;

  /**
   * @brief Takes ownership of one descriptor.
   *
   * @param fd Descriptor to own, or -1 for no descriptor.
   * @throws Nothing.
   */
  explicit UniqueFd(int fd) noexcept;

  /**
   * @brief Closes the owned descriptor once.
   *
   * @throws Nothing.
   */
  ~UniqueFd();

  /**
   * @brief Prevents copying ownership of one descriptor.
   *
   * @throws Nothing because this operation is unavailable.
   */
  UniqueFd(const UniqueFd&) = delete;

  /**
   * @brief Prevents descriptor ownership duplication by assignment.
   *
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  UniqueFd& operator=(const UniqueFd&) = delete;

  /**
   * @brief Transfers descriptor ownership.
   *
   * @param other Owner to empty.
   * @throws Nothing.
   */
  UniqueFd(UniqueFd&& other) noexcept;

  /**
   * @brief Replaces this descriptor with one transferred from another owner.
   *
   * @param other Owner to empty.
   * @return This owner after replacement.
   * @throws Nothing.
   */
  UniqueFd& operator=(UniqueFd&& other) noexcept;

  /**
   * @brief Returns the owned descriptor without transferring it.
   *
   * @return Descriptor number, or -1 when empty.
   * @throws Nothing.
   */
  int get() const noexcept;

  /**
   * @brief Reports whether a descriptor is owned.
   *
   * @return True when `get()` is nonnegative.
   * @throws Nothing.
   */
  explicit operator bool() const noexcept;

  /**
   * @brief Releases ownership without closing.
   *
   * @return Previously owned descriptor, or -1.
   * @throws Nothing.
   * @note The caller becomes responsible for closing the returned descriptor.
   */
  int release() noexcept;

  /**
   * @brief Closes the current descriptor and optionally adopts another.
   *
   * @param fd Replacement descriptor, or -1.
   * @throws Nothing.
   * @note Close errors are intentionally ignored during ownership cleanup.
   */
  void reset(int fd = -1) noexcept;

 private:
  /** @brief Descriptor owned by this object, or -1. */
  int fd_ = -1;
};

/**
 * @brief Configures socket-local SIGPIPE suppression where required.
 *
 * @param fd Newly created or accepted Unix socket descriptor.
 * @param message Receives a diagnostic when configuration fails.
 * @return True on Linux or after successful macOS `SO_NOSIGPIPE` setup.
 * @throws std::bad_alloc if diagnostic construction fails.
 * @note The helper never changes process-global signal disposition.
 */
bool configure_no_sigpipe(int fd, std::string* message);

/**
 * @brief Creates one configured but unconnected Unix stream socket.
 *
 * @param message Receives a bounded socket/fcntl diagnostic on failure.
 * @return Owned `FD_CLOEXEC` descriptor or an empty owner.
 * @throws std::bad_alloc if diagnostic construction fails.
 * @note Separating creation from connect lets lifecycle owners publish the
 *       descriptor before an interruptible nonblocking connect begins.
 */
UniqueFd create_unix_stream_socket(std::string* message);

/**
 * @brief Connects one already-owned socket with interruptible nonblocking IO.
 *
 * @param fd Configured Unix stream socket descriptor.
 * @param socket_path Absolute filesystem path fitting `sun_path`.
 * @param should_stop Stop predicate checked before connect and between bounded
 *        backlog-retry/writable-wait slices.
 * @param message Receives validation, connect, interruption, or socket
 *        diagnostics.
 * @return True after connection and restoration of blocking mode.
 * @throws std::bad_alloc if diagnostic construction fails, or whatever the
 *         supplied stop predicate throws.
 * @note The function performs one logical nonblocking connection. It completes
 *       `EINPROGRESS`/`EALREADY` through writable poll plus `SO_ERROR`; Linux
 *       AF_UNIX `EAGAIN` means no connection began, so it waits one bounded
 *       slice and re-enters connect on the same fd; `EISCONN` after such a
 *       transient is successful logical completion. It never closes,
 *       transfers ownership, writes a frame, or imposes a total timeout.
 */
bool connect_prepared_unix_socket(int fd, const std::string& socket_path,
                                  const std::function<bool()>& should_stop,
                                  std::string* message);

/**
 * @brief Connects through one injected attempt and the production wait logic.
 *
 * @param fd Configured Unix stream socket descriptor.
 * @param socket_path Absolute filesystem path fitting `sun_path`.
 * @param should_stop Stop predicate checked before attempt and while pending.
 * @param connect_attempt Callable receiving descriptor, sockaddr bytes, and
 *        address length; it must follow connect's return/errno contract.
 * @param message Receives validation, connect, interruption, or socket
 *        diagnostics.
 * @return True after the logical connection completes and original blocking
 *         flags are restored.
 * @throws std::bad_alloc if diagnostic construction fails, or whatever either
 *         injected callable throws.
 * @note This source-tree-private seam makes pending completion and Linux
 *       backlog-`EAGAIN` retries deterministic in tests. Re-entry is limited to
 *       a same-fd local connection that has not begun; no frame/RPC is retried.
 *       The helper never owns or closes `fd`.
 */
bool connect_prepared_unix_socket_with_attempt(
    int fd, const std::string& socket_path,
    const std::function<bool()>& should_stop,
    const std::function<int(int, const void*, std::size_t)>& connect_attempt,
    std::string* message);

/**
 * @brief Connects one owned stream socket to an absolute Unix path.
 *
 * @param socket_path Absolute filesystem path that fits `sun_path` including
 *        its terminating NUL.
 * @param message Receives a local validation, socket, or connect diagnostic.
 * @return Owned connected descriptor, or an empty owner on failure.
 * @throws std::bad_alloc if diagnostic construction fails.
 * @note The function performs one logical connection. Linux backlog `EAGAIN`
 *       may re-enter local connect on the same unconnected fd; it never retries
 *       a frame or potentially mutating protocol operation.
 */
UniqueFd connect_unix_socket(const std::string& socket_path,
                             std::string* message);

}  // namespace ps::ipc::internal
