#pragma once

#include <mutex>
#include <optional>
#include <string>

#include "ipc/collection_snapshot_registry.hpp"
#include "ipc/compute_request_registry.hpp"
#include "ipc/output_store.hpp"
#include "ipc/session_registry.hpp"
#include "photospider/host/host.hpp"

namespace ps::ipc::internal {

/**
 * @brief Routes version 1 requests through one daemon-owned Host.
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
   *       constructed stopped. Capability advertisement remains owned by the
   *       exact table in the private protocol codec.
   */
  RequestRouter(Host& host, std::string service_version);

  /**
   * @brief Creates a router with an injectable collection-snapshot policy.
   *
   * @param host Sole daemon Host instance borrowed by this router.
   * @param service_version Reproducible CMake project version string.
   * @param snapshot_limits Count, byte, page, and TTL policy.
   * @param snapshot_clock Monotonic clock used for cursor expiry.
   * @param snapshot_id_generator Stable opaque cursor source.
   * @throws std::bad_alloc if callback or metadata allocation fails.
   * @throws std::invalid_argument if the snapshot policy is inconsistent.
   * @throws std::runtime_error if instance-id entropy fails.
   * @note Production uses the two-argument overload. Tests may inject smaller
   *       limits and deterministic time/ids, but runtime admission still begins
   *       only through `start_runtime()`.
   */
  RequestRouter(Host& host, std::string service_version,
                CollectionSnapshotLimits snapshot_limits,
                CollectionSnapshotRegistry::Clock snapshot_clock,
                CollectionSnapshotRegistry::IdGenerator snapshot_id_generator);

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
   *       Stable graph-list/inspection pages and direct YAML, timing, dirty,
   *       node, and current-planning codecs locally map returned byte/count
   *       bounds to `response_too_large`; malformed returned values and other
   *       standard request failures become daemon `internal_error`. Compute
   *       submission validates its complete nested request before admission;
   *       job polling/release needs no live session or Host lock, and accepted
   *       execution/publication failures remain nested terminal statuses.
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
  /**
   * @brief Opaque cpp-owned adapter over one borrowed parsed params object.
   *
   * @throws Nothing for declaration and reference binding.
   * @note The complete type remains in `request_router.cpp` so this private
   *       server header does not expose or require the JSON implementation.
   *       Its borrowed value never outlives the active `route()` call.
   */
  struct RoutedParams;

  /**
   * @brief Routes one graph-state control or diagnostic Host method.
   *
   * @param id Valid request id correlated with the response.
   * @param method Exact version 1 method name.
   * @param routed_params Cpp-only adapter borrowing the structurally valid
   *        params object whose known fields have not yet been method-validated.
   * @return Complete response payload when `method` belongs to this family, or
   *         `std::nullopt` when another router family must handle it.
   * @throws std::bad_alloc if validation, Host result copying, or response
   *         construction cannot allocate.
   * @throws std::invalid_argument if a successful Host returns a malformed
   *         public value that cannot be represented on version 1.
   * @throws Whatever the matching Host method propagates; `route()` preserves
   *         resource exhaustion and maps other standard exceptions to daemon
   *         `internal_error`.
   * @note Every known parameter is validated before session resolution. Each
   *       handled call obtains one session admission, invokes exactly one
   *       matching Host method under `host_mutex_`, and never retries a
   *       mutation. Host-returned status failures become top-level errors,
   *       except `compute.last_error`, whose observed status is nested data in
   *       a successful envelope.
   */
  std::optional<std::string> route_session_control_method(
      const std::string& id, const std::string& method,
      const RoutedParams& routed_params);

  /**
   * @brief Routes stable collection and remaining inspection methods.
   *
   * @param id Valid request id correlated with the response.
   * @param method Exact version 1 method name.
   * @param routed_params Cpp-only adapter borrowing the structurally valid
   *        params object.
   * @return Complete response when this family recognizes `method`, or
   *         `std::nullopt` for another router family.
   * @throws std::bad_alloc if validation, snapshot ownership, or response
   *         construction cannot allocate.
   * @throws std::invalid_argument if Host returns a malformed public value.
   * @throws Whatever the matching Host method propagates; `route()` preserves
   *         resource exhaustion and maps other standard exceptions.
   * @note Initial collection calls reserve one slot and 64 MiB before exactly
   *       one Host call under `host_mutex_`. Continuations validate only their
   *       frozen cursor identity and never resolve a live session or call Host.
   *       Returned collection limits count outer and nested public vector/map
   *       entries recursively; dependency/traversal results are pre-scanned
   *       before router-owned header or row transformation allocations.
   *       Indivisible node, dirty-region, and current-planning values remain
   *       direct results and are rejected whole when they cannot fit a frame.
   */
  std::optional<std::string> route_inspection_method(
      const std::string& id, const std::string& method,
      const RoutedParams& routed_params);

  /**
   * @brief Routes bounded polling compute-job lifecycle methods.
   *
   * @param id Valid request id correlated with the response.
   * @param method Exact version 1 compute-job method name.
   * @param routed_params Cpp-only adapter borrowing the structurally valid
   *        params object.
   * @return Complete response when this family recognizes `method`, or
   *         `std::nullopt` for another router family.
   * @throws std::bad_alloc if validation, registry lookup, or response
   *         construction cannot allocate.
   * @throws Whatever pre-commit registry admission propagates; accepted worker
   *         and output-publication failures are retained as nested terminal
   *         statuses instead.
   * @note Submit validates the complete public Host request before one
   *       registry admission. Status, result, and release resolve only the
   *       opaque compute id and never acquire `host_mutex_` or require a live
   *       session. Submit/status/result use one stable snapshot schema; release
   *       atomically returns `{compute_id,released:true}`. The current final
   *       nullable `output` field remains null; protected metadata delivery is
   *       owned by the separate image-result routing boundary.
   */
  std::optional<std::string> route_compute_method(
      const std::string& id, const std::string& method,
      const RoutedParams& routed_params);

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
