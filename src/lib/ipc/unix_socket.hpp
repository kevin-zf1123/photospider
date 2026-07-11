#pragma once

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
 * @brief Connects one owned stream socket to an absolute Unix path.
 *
 * @param socket_path Absolute filesystem path that fits `sun_path` including
 *        its terminating NUL.
 * @param message Receives a local validation, socket, or connect diagnostic.
 * @return Owned connected descriptor, or an empty owner on failure.
 * @throws std::bad_alloc if diagnostic construction fails.
 * @note The function performs one connect attempt and never retries a
 *       potentially mutating protocol operation.
 */
UniqueFd connect_unix_socket(const std::string& socket_path,
                             std::string* message);

}  // namespace ps::ipc::internal
