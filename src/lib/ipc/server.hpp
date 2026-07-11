#pragma once

#include <memory>
#include <string>

#include "ipc/request_router.hpp"
#include "photospider/host/host.hpp"
#include "photospider/ipc/protocol.hpp"

namespace ps::ipc::internal {

/**
 * @brief Immutable startup options for one foreground Unix IPC server.
 *
 * @throws std::bad_alloc when copied path storage cannot be allocated.
 * @note An empty socket path selects the protected per-user default.
 */
struct ServerOptions {
  /** @brief Explicit absolute socket path, or empty for the per-user default.
   */
  std::string socket_path;
};

/**
 * @brief Resolves the protected default per-user daemon socket path.
 *
 * @return Path below a valid `XDG_RUNTIME_DIR/photospider`, otherwise below a
 *         uid-qualified platform temporary directory.
 * @throws std::bad_alloc if path construction cannot allocate.
 * @throws std::filesystem::filesystem_error if fallback path construction
 *         fails.
 * @throws std::runtime_error if the platform Unix-socket path limit cannot
 *         contain even the uid-qualified fallback.
 * @note The function creates no directories and never selects graph/cache
 *       directories.
 */
std::string default_socket_path();

/**
 * @brief Foreground Unix listener with bounded tracked client workers.
 *
 * The server owns its listener and accepted descriptors, processes requests
 * sequentially per connection, allows frame/JSON work across connections, and
 * delegates Host serialization to `RequestRouter`. At most 32 active workers
 * exist. All threads remain joinable and shutdown wakes blocked reads before
 * joining them.
 *
 * @throws std::bad_alloc when immutable metadata allocation fails.
 * @throws std::runtime_error if daemon instance entropy fails.
 * @note The server borrows one Host that must outlive `run()` and destruction.
 */
class Server {
 public:
  /**
   * @brief Creates a server/router around one daemon-owned Host.
   *
   * @param host Sole Host instance borrowed by this server.
   * @param service_version Reproducible CMake project version.
   * @throws std::bad_alloc if metadata allocation fails.
   * @throws std::runtime_error if instance-id entropy fails.
   */
  Server(Host& host, std::string service_version);

  /**
   * @brief Stops and releases any remaining listener/worker resources.
   *
   * @throws Nothing.
   * @note Normal use returns from `run()` only after deterministic cleanup.
   */
  ~Server();

  /**
   * @brief Prevents copying listener, worker, and Host-session ownership.
   *
   * @throws Nothing because this operation is unavailable.
   */
  Server(const Server&) = delete;

  /**
   * @brief Prevents duplicating server runtime ownership by assignment.
   *
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  Server& operator=(const Server&) = delete;

  /**
   * @brief Runs the protected listener until the self-pipe becomes readable.
   *
   * @param options Explicit/default socket selection.
   * @param stop_fd Read end of the pre-created nonblocking signal self-pipe.
   * @return Success after normal signal shutdown, or daemon-domain startup/
   *         accept/poll failure with an owned diagnostic.
   * @throws std::bad_alloc if worker or response storage allocation fails
   *         outside a containable connection boundary.
   * @throws std::filesystem::filesystem_error if default socket path or parent
   *         directory inspection fails unexpectedly.
   * @throws std::runtime_error if the default path cannot fit the platform
   *         Unix-socket address or another startup invariant fails.
   * @throws std::system_error if a worker thread cannot be created.
   * @note Shutdown closes the listener, wakes and joins clients, closes Host
   *       sessions, then removes only the socket inode created by this run.
   */
  IpcStatus run(const ServerOptions& options, int stop_fd);

 private:
  /** @brief Opaque listener/worker/socket-lifecycle implementation. */
  class Impl;

  /** @brief Sole owner of server runtime state. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps::ipc::internal
