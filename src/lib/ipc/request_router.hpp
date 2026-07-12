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
 * @brief Complete private runtime policy injected into one request router.
 *
 * The value groups the existing bounded snapshot, compute-job, and output
 * store policies with their monotonic clocks and opaque-id sources. Empty
 * callbacks retain each component's production default. This is a private
 * composition contract for the daemon server and deterministic process
 * fixtures; it is not installed or represented on the version 1 wire.
 *
 * @throws std::bad_alloc when copied callback storage cannot be allocated.
 * @note Callbacks must satisfy the thread-safety and non-throwing constraints
 *       documented by their owning registry/store. Production constructs the
 *       default value; tests may inject coherent small policies without adding
 *       a product flag, environment failpoint, or protocol method.
 */
struct RequestRouterRuntimeDependencies {
  /** @brief Stable collection snapshot capacity and retention policy. */
  CollectionSnapshotLimits snapshot_limits;

  /** @brief Monotonic clock used only by the snapshot registry. */
  CollectionSnapshotRegistry::Clock snapshot_clock;

  /** @brief Opaque cursor candidate source used by the snapshot registry. */
  CollectionSnapshotRegistry::IdGenerator snapshot_id_generator;

  /** @brief Active/terminal compute capacity and terminal retention policy. */
  ComputeRequestRegistryLimits compute_limits;

  /** @brief Monotonic clock used only by the compute registry. */
  ComputeRequestRegistry::Clock compute_clock;

  /** @brief Opaque compute-id candidate source used by the job registry. */
  ComputeRequestRegistry::IdGenerator compute_id_generator;

  /** @brief Artifact quota plus job-owner and delivery-lease policy. */
  OutputStoreLimits output_limits;

  /** @brief Monotonic clock used only by the output store. */
  OutputStore::Clock output_clock;

  /** @brief Opaque output/delivery/stage id source used by the output store. */
  OutputStore::IdGenerator output_id_generator;
};

/**
 * @brief Validates one OutputStore delivery for version 1 encoding.
 * @param delivery Revalidated metadata and lease candidate.
 * @param expected_output_reference Private job reference that selected the
 *        OutputStore record.
 * @return True only when ids, path, enum values, and exact tight-row byte
 *         layout are internally consistent.
 * @throws Nothing.
 * @note This pure internal validator performs no filesystem access and does
 *       not duplicate OutputStore ancestry or identity checks. It exists as a
 *       narrow malformed-delivery unit seam and never publishes JSON itself.
 */
bool valid_output_delivery_for_wire(
    const OutputArtifactDelivery& delivery,
    const std::string& expected_output_reference) noexcept;

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
   * @brief Creates a router with complete private runtime dependencies.
   *
   * @param host Sole daemon Host instance borrowed by this router.
   * @param service_version Reproducible CMake project version string.
   * @param dependencies Snapshot, compute, and output policies/callbacks.
   * @throws std::bad_alloc if callback or metadata allocation fails.
   * @throws std::invalid_argument if any injected policy is inconsistent.
   * @throws std::runtime_error if instance-id entropy fails.
   * @note Production uses the two-argument overload. Tests may inject smaller
   *       coherent limits and deterministic time/ids, but runtime admission
   *       still begins only through `start_runtime()`. This overload is private
   *       to the non-installed daemon implementation surface.
   */
  RequestRouter(Host& host, std::string service_version,
                RequestRouterRuntimeDependencies dependencies);

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
   *       execution/publication failures remain nested terminal statuses;
   *       only result-time missing or identity-mismatched artifacts become the
   *       top-level `artifact_not_found` error. Lease-aware release remains
   *       available after concurrent terminal-record removal.
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
   * @throws std::invalid_argument if the registry returns a noncanonical job
   *         snapshot or OutputStore returns metadata inconsistent with the
   *         job's private reference or exact tight-row wire layout.
   * @throws Whatever pre-commit registry admission propagates; accepted worker
   *         and output-publication failures are retained as nested terminal
   *         statuses instead.
   * @note Submit validates the complete public Host request before one
   *       registry admission. Status, result, and release resolve only the
   *       opaque compute id and never acquire `host_mutex_` or require a live
   *       session. Submit/status/result use one stable snapshot schema;
   *       terminal image result revalidates OutputStore metadata and refreshes
   *       its stable lease before encoding the non-null `output`. Release
   *       atomically returns `{compute_id,released:true}`, optionally removes a
   *       matching lease, and can release that lease after concurrent normal
   *       job removal. The registry reference is exposed only as the
   *       revalidated `output.output_id`, never as an extra
   *       `output_reference` field or backend handle.
   *       If response allocation/encoding fails after lease acquisition, the
   *       stable lease is left to explicit release or its bounded TTL because
   *       it may already protect metadata returned by an earlier result call.
   *       `compute.submit`, `compute.status`, and `compute.result` delegate to
   *       the matching `ComputeRequestRegistry` operation; result additionally
   *       calls `OutputStore::acquire_delivery` only for a private output
   *       reference. `compute.release` first calls the registry release and,
   *       only for a well-formed matching pair after `job_not_found`, may call
   *       `OutputStore::release_orphaned_delivery`. Invalid/admission/lookup/
   *       premature-result failures use their established top-level mapping,
   *       while accepted compute/publication failures remain nested. A lease
   *       acquired before an encoding exception is intentionally not rolled
   *       back blindly; `route()` maps that standard exception to daemon
   *       `internal_error`.
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
