#pragma once

#include <mutex>
#include <string>

#include "ipc/session_registry.hpp"
#include "photospider/host/host.hpp"

namespace ps::ipc::internal {

/**
 * @brief Routes the eight version 1 methods through one daemon-owned Host.
 *
 * Envelope/parameter validation and immutable daemon metadata do not acquire
 * the Host mutex. Every actual Host method call is serialized through the one
 * dedicated mutex, including `list_graphs() const`. Socket IO is owned by the
 * server and never occurs while this router holds the Host mutex.
 *
 * @throws std::bad_alloc when immutable metadata or registry storage cannot be
 *         allocated.
 * @throws std::runtime_error if the operating-system instance-id entropy
 *         source fails.
 * @note The router owns session mappings but borrows Host for a lifetime that
 *       must exceed the router and server.
 */
class RequestRouter {
 public:
  /**
   * @brief Creates a production router with OS-entropy session tokens.
   *
   * @param host Sole daemon Host instance borrowed by this router.
   * @param service_version Reproducible CMake project version string.
   * @throws std::bad_alloc if metadata allocation fails.
   * @throws std::runtime_error if instance-id entropy fails.
   */
  RequestRouter(Host& host, std::string service_version);

  /**
   * @brief Prevents copying Host, mutex, and registry ownership.
   *
   * @throws Nothing because this operation is unavailable.
   * @note Construct one router per daemon-owned Host.
   */
  RequestRouter(const RequestRouter&) = delete;

  /**
   * @brief Prevents replacing router ownership by copy assignment.
   *
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   * @note The borrowed Host reference cannot be rebound.
   */
  RequestRouter& operator=(const RequestRouter&) = delete;

  /**
   * @brief Validates and routes one complete framed JSON request payload.
   *
   * @param payload Exact JSON bytes from one valid-sized frame.
   * @return Bounded version 1 response payload with correlated id or JSON null
   *         when no valid id can be recovered.
   * @throws std::bad_alloc if even bounded response construction cannot
   *         allocate; the connection boundary then closes only that client.
   * @note No Host call occurs for malformed envelopes, unsupported methods, or
   *       invalid params. A `graph.load` reservation is removed before any
   *       compensating Host close, including when Host load or registry
   *       publication throws. The function performs no socket IO.
   */
  std::string route(const std::string& payload);

  /**
   * @brief Closes every active Host session during daemon shutdown.
   *
   * @throws Nothing; individual Host exceptions/failures are contained so the
   *         process can complete deterministic descriptor/socket cleanup.
   * @note The function holds the Host mutex around each close and clears all
   *       registry state after every close has been attempted.
   */
  void close_all_sessions() noexcept;

  /**
   * @brief Returns the immutable daemon instance id.
   *
   * @return Reference valid for this router's lifetime.
   * @throws Nothing.
   */
  const std::string& server_instance_id() const noexcept;

 private:
  /** @brief Sole daemon Host borrowed from `photospiderd`. */
  Host& host_;

  /** @brief Serializes every Host call without covering socket IO. */
  mutable std::mutex host_mutex_;

  /** @brief Loading/active opaque-to-Host session registry. */
  SessionRegistry registry_;

  /** @brief Reproducible CMake project version. */
  std::string service_version_;

  /** @brief One 128-bit opaque identity generated at process start. */
  std::string server_instance_id_;
};

}  // namespace ps::ipc::internal
