#pragma once

#include <mutex>
#include <string>

#include "ipc/collection_snapshot_registry.hpp"
#include "ipc/compute_request_registry.hpp"
#include "ipc/output_store.hpp"
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
 * @note The router owns session mappings, bounded stable collection snapshots,
 *       the protected OutputStore, and the private joined compute registry,
 *       but borrows Host for a lifetime that must exceed the router and server.
 *       Runtime state starts only through `start_runtime()`.
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
   * @note Collection snapshot registry, OutputStore, and compute registry are
   *       constructed stopped; the current wire inventory remains the exact
   *       eight names defined by protocol v1.
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
   *       publication throws. Session-scoped calls retain a counted admission;
   *       `graph.close` marks Closing and waits admissions before Host locking.
   *       The `graph.list` and inspection result codecs locally map
   *       `std::length_error` to `response_too_large`; malformed returned
   *       values and other standard request failures become daemon
   *       `internal_error`.
   *       Resource exhaustion is rethrown. The function performs no socket IO.
   */
  std::string route(const std::string& payload);

  /**
   * @brief Starts the output, snapshot, compute, and session runtime layers.
   *
   * @param socket_path Absolute bound socket path naming the output-store base.
   * @param lifecycle_lock_fd Open matching lifecycle lock held by Server.
   * @return Success or daemon lifecycle failure.
   * @throws std::system_error if worker creation fails.
   * @throws std::bad_alloc if failure diagnostic construction cannot allocate.
   * @note The server calls this only after socket ownership is established,
   *       while retaining its lifecycle lock, and before any client worker can
   *       route a request. Output-store failure prevents all admission;
   *       compute-start failure rolls the snapshot registry back to empty.
   */
  OperationStatus start_runtime(const std::string& socket_path,
                                int lifecycle_lock_fd);

  /**
   * @brief Rejects new sessions, snapshots, compute work, and output leases.
   * @throws Nothing.
   * @note Already reserved collection calls and admitted compute jobs retain
   *       their publication rules; the draining worker may still publish an
   *       output before final shutdown.
   */
  void begin_shutdown() noexcept;

  /**
   * @brief Drains compute/snapshots/output, then closes every Host session.
   *
   * @throws Nothing; job cleanup, Host failures, and Host exceptions are
   *         contained so socket lifecycle cleanup can continue.
   * @note Call after accepted client workers have stopped. Compute shutdown
   *       releases job ownership first; snapshot records/reservations are then
   *       cleared, and OutputStore waits active leases before Host sessions and
   *       registry rows are cleared.
   */
  void finish_shutdown() noexcept;

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

  /**
   * @brief Bounded stable full-value collection snapshot ownership.
   * @note Declaration order makes compute/output owners destruct before this
   *       registry, which itself destructs before session mappings.
   */
  CollectionSnapshotRegistry collection_snapshots_;

  /** @brief Socket-specific private image artifact and delivery lease store. */
  OutputStore output_store_;

  /** @brief Bounded private compute lifecycle with one joined worker. */
  ComputeRequestRegistry compute_registry_;

  /** @brief Reproducible CMake project version. */
  std::string service_version_;

  /** @brief One 128-bit opaque identity generated at process start. */
  std::string server_instance_id_;
};

}  // namespace ps::ipc::internal
