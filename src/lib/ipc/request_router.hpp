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
   *       constructed stopped. Capability advertisement and pre-dispatch
   *       admission share the exact 55-method table in the private protocol
   *       codec.
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
   * @note No Host call occurs for malformed envelopes, unadvertised methods,
   *       unsupported advertised routes, or invalid params. The exact
   *       55-method table gates all route families before dispatch, while
   *       independent family matchers leave an advertised-but-unimplemented
   *       method observable as `method_not_found` for contract verification.
   *       A `graph.load` reservation is removed before any
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
   *       available after concurrent terminal-record removal. Event drains
   *       and scheduler trace pages validate their exact bounds before one
   *       serialized Host call; they never use collection snapshot storage.
   *       Process-global plugin mutations likewise use one serialized Host
   *       call, while plugin views reserve, sort, measure, and freeze one
   *       stable snapshot before their first Host call result is published;
   *       continuations never resolve a session or reenter Host. Global
   *       scheduler discovery/control follows the same Host-only boundary;
   *       scheduler type/plugin lists use stable sorted snapshots, while
   *       per-session information/replacement uses opaque admission and the
   *       common Host mutex shared with compute execution.
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
   * @return Nothing.
   * @throws Nothing.
   * @note Already reserved collection calls and admitted compute jobs retain
   *       their publication rules; the draining worker may still publish an
   *       output before final shutdown.
   */
  void begin_shutdown() noexcept;

  /**
   * @brief Drains compute/snapshots/output, then closes every Host session.
   *
   * @return Nothing.
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
   * @brief Opaque internal adapter over one borrowed parsed params object.
   *
   * @throws Nothing for declaration and reference binding.
   * @note The complete type remains in `request_router.cpp` so this private
   *       server header does not expose or require the JSON implementation.
   *       Its borrowed value never outlives the active `route()` call.
   */
  struct RoutedParams;

  /**
   * @brief Opaque internal state for one validated inspection request.
   * @throws Nothing for declaration; construction rules live in the cpp file.
   * @note Owns only copied scalar/page identity while borrowing no Host state.
   */
  struct InspectionRequest;

  /**
   * @brief Opaque internal state for one validated session-control request.
   * @throws Nothing for declaration; construction rules live in the cpp file.
   * @note Owns decoded wire values but no registry admission or Host session.
   */
  struct SessionControlRequest;

  /**
   * @brief Opaque internal state for one stable collection route.
   * @throws Nothing for declaration; construction rules live in the cpp file.
   * @note Owns cursor/page binding values but no snapshot reservation.
   */
  struct StableCollectionRequest;

  /**
   * @brief Routes graph load and close lifecycle methods.
   *
   * @param id Valid request id correlated with the response.
   * @param method Exact version 1 method name.
   * @param routed_params Internal adapter borrowing structurally valid params.
   * @return Complete response for graph load/close, or `std::nullopt` when the
   *         method belongs to another router family.
   * @throws std::bad_alloc if validation, reservation, Host result copying, or
   *         response construction cannot allocate.
   * @throws Whatever graph load/close or registry publication propagates;
   *         `route()` preserves resource exhaustion and maps other standard
   *         exceptions to daemon `internal_error`.
   * @note Load keeps the Host mutex across reservation, Host load, and registry
   *       publication; every failed publication rolls back before best-effort
   *       Host close. Close marks/waits session admission before Host locking
   *       and erases/reopens the claim at the same commit points as before.
   */
  std::optional<std::string> route_graph_lifecycle_method(
      const std::string& id, const std::string& method,
      const RoutedParams& routed_params);

  /**
   * @brief Routes one graph-state control or diagnostic Host method.
   *
   * @param id Valid request id correlated with the response.
   * @param method Exact version 1 method name.
   * @param routed_params Internal adapter borrowing the structurally valid
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
   * @brief Decodes every known session-control field before registry access.
   * @param method Exact recognized session-control method.
   * @param routed_params Internal adapter borrowing structural params.
   * @param request Receives the complete decoded request on success.
   * @return Canonical success or protocol `invalid_params` status.
   * @throws std::bad_alloc if decoded text or diagnostic allocation fails.
   * @note Failure leaves registry and Host untouched; unknown fields remain
   *       forward-compatible and no method is retried.
   */
  static OperationStatus decode_session_control_request(
      const std::string& method, const RoutedParams& routed_params,
      SessionControlRequest* request);

  /**
   * @brief Decodes graph/cache text and node selectors.
   * @param method Exact recognized non-spatial session-control method.
   * @param routed_params Internal adapter borrowing structural params.
   * @param request Receives decoded method-specific values.
   * @return Canonical success or protocol `invalid_params` status.
   * @throws std::bad_alloc if owned text or diagnostics allocate.
   * @note Session identity is already validated; this helper touches no
   *       registry or Host state and accepts unknown fields.
   */
  static OperationStatus decode_session_text_request(
      const std::string& method, const RoutedParams& routed_params,
      SessionControlRequest* request);

  /**
   * @brief Decodes one dirty lifecycle request.
   * @param method Exact dirty method.
   * @param routed_params Internal adapter borrowing structural params.
   * @param request Receives node, domain, and optional ROI values.
   * @return Canonical success or protocol `invalid_params` status.
   * @throws std::bad_alloc if diagnostics allocate.
   * @note Session identity is already validated; failure has no side effect.
   */
  static OperationStatus decode_session_dirty_request(
      const std::string& method, const RoutedParams& routed_params,
      SessionControlRequest* request);

  /**
   * @brief Decodes one forward/backward ROI projection request.
   * @param method Exact ROI method.
   * @param routed_params Internal adapter borrowing structural params.
   * @param request Receives ordered node identities and exact ROI.
   * @return Canonical success or protocol `invalid_params` status.
   * @throws std::bad_alloc if diagnostics allocate.
   * @note Preserves directional field ordering and touches no Host state.
   */
  static OperationStatus decode_session_roi_request(
      const std::string& method, const RoutedParams& routed_params,
      SessionControlRequest* request);

  /**
   * @brief Routes graph/cache mutations for one admitted session.
   * @param id Valid correlated request id.
   * @param method Exact recognized session-control method.
   * @param request Fully decoded request values.
   * @param host_session Host identity protected by the caller's admission.
   * @return Complete response, or `std::nullopt` for another subfamily.
   * @throws std::bad_alloc if Host/result/error allocation fails.
   * @throws Whatever the matching Host mutation propagates.
   * @note Invokes at most one matching Host call under `host_mutex_` and never
   *       retries a mutation.
   */
  std::optional<std::string> route_session_mutation_method(
      const std::string& id, const std::string& method,
      const SessionControlRequest& request, const GraphSessionId& host_session);

  /**
   * @brief Executes one admitted graph mutation.
   * @param id Valid correlated request id.
   * @param method Exact graph mutation method.
   * @param request Fully decoded request values.
   * @param host_session Host identity protected by caller admission.
   * @return Complete mutation response.
   * @throws std::bad_alloc if Host/result/error allocation fails.
   * @throws Whatever the selected Host graph mutation propagates.
   * @note Invokes exactly one serialized Host call and never retries it.
   */
  std::string route_session_graph_mutation(const std::string& id,
                                           const std::string& method,
                                           const SessionControlRequest& request,
                                           const GraphSessionId& host_session);

  /**
   * @brief Executes one admitted cache mutation.
   * @param id Valid correlated request id.
   * @param method Exact cache mutation method.
   * @param request Fully decoded request values.
   * @param host_session Host identity protected by caller admission.
   * @return Complete mutation response.
   * @throws std::bad_alloc if Host/result/error allocation fails.
   * @throws Whatever the selected Host cache mutation propagates.
   * @note Invokes exactly one serialized Host call and never retries it.
   */
  std::string route_session_cache_mutation(const std::string& id,
                                           const std::string& method,
                                           const SessionControlRequest& request,
                                           const GraphSessionId& host_session);

  /**
   * @brief Routes dirty-source operations for one admitted session.
   * @param id Valid correlated request id.
   * @param method Exact recognized session-control method.
   * @param request Fully decoded request values.
   * @param host_session Host identity protected by the caller's admission.
   * @return Complete response, or `std::nullopt` for another subfamily.
   * @throws std::bad_alloc if Host/result/error allocation fails.
   * @throws Whatever the matching Host dirty operation propagates.
   * @note Invokes exactly one dirty Host call under `host_mutex_`; returned
   *       snapshots are encoded whole without synthesizing lifecycle state.
   */
  std::optional<std::string> route_session_dirty_method(
      const std::string& id, const std::string& method,
      const SessionControlRequest& request, const GraphSessionId& host_session);

  /**
   * @brief Routes ROI projection for one admitted session.
   * @param id Valid correlated request id.
   * @param method Exact recognized session-control method.
   * @param request Fully decoded request values.
   * @param host_session Host identity protected by the caller's admission.
   * @return Complete response, or `std::nullopt` for another subfamily.
   * @throws std::bad_alloc if Host/result/error allocation fails.
   * @throws Whatever the matching Host projection propagates.
   * @note Preserves node order and rectangle axes under one serialized Host
   *       call and performs no cache or scheduler access.
   */
  std::optional<std::string> route_session_roi_method(
      const std::string& id, const std::string& method,
      const SessionControlRequest& request, const GraphSessionId& host_session);

  /**
   * @brief Routes node-YAML and compute diagnostic reads.
   * @param id Valid correlated request id.
   * @param method Exact recognized session-control method.
   * @param request Fully decoded request values.
   * @param host_session Host identity protected by the caller's admission.
   * @return Complete response, or `std::nullopt` for another subfamily.
   * @throws std::bad_alloc if Host/result/error allocation fails.
   * @throws std::invalid_argument for malformed successful Host values.
   * @throws Whatever the matching Host read propagates.
   * @note Each handled read invokes one Host method under `host_mutex_`; the
   *       observed last-error status remains nested successful response data.
   */
  std::optional<std::string> route_session_diagnostic_method(
      const std::string& id, const std::string& method,
      const SessionControlRequest& request, const GraphSessionId& host_session);

  /**
   * @brief Routes stable collection and remaining inspection methods.
   *
   * @param id Valid request id correlated with the response.
   * @param method Exact version 1 method name.
   * @param routed_params Internal adapter borrowing the structurally valid
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
   * @brief Decodes session, node, and dependency selectors for inspection.
   * @param method Exact recognized inspection method.
   * @param routed_params Internal adapter borrowing structural params.
   * @param request Receives owned validated identity values.
   * @return Canonical success or protocol `invalid_params` status.
   * @throws std::bad_alloc if identity or diagnostic allocation fails.
   * @note Performs no Host, session-registry admission, or snapshot access;
   *       `graph.list` is the only method without a session id.
   */
  static OperationStatus decode_inspection_identity(
      const std::string& method, const RoutedParams& routed_params,
      InspectionRequest* request);

  /**
   * @brief Decodes inspection page controls and freezes cursor binding input.
   * @param routed_params Internal adapter borrowing structural params.
   * @param request Identity-decoded request receiving page/binding values.
   * @return Canonical success or protocol `invalid_params` status.
   * @throws std::bad_alloc if cursor/binding/diagnostic allocation fails.
   * @note Called only for collection methods after direct-value dispatch;
   *       performs no reservation, Host call, or cursor lookup.
   */
  static OperationStatus decode_inspection_page_request(
      const RoutedParams& routed_params, InspectionRequest* request);

  /**
   * @brief Routes one non-paged inspection value after complete validation.
   * @param id Valid correlated request id.
   * @param request Validated inspection identity and method values.
   * @return Complete response for a direct method, or `std::nullopt` otherwise.
   * @throws std::bad_alloc if admission, Host copying, or encoding allocates.
   * @throws std::invalid_argument for malformed successful Host values.
   * @throws Whatever the matching Host inspection propagates.
   * @note Admits one live session and invokes exactly one Host call under
   *       `host_mutex_`; direct values never reserve snapshot quota.
   */
  std::optional<std::string> route_inspection_direct_value(
      const std::string& id, const InspectionRequest& request);

  /**
   * @brief Reads one inspection continuation from a frozen snapshot.
   * @param id Valid correlated request id.
   * @param request Validated inspection cursor and frozen binding.
   * @return Complete bounded continuation response.
   * @throws std::bad_alloc if page copying or encoding cannot allocate.
   * @throws std::invalid_argument for malformed retained public values.
   * @note Performs no live-session resolution, Host call, or new reservation;
   *       graph close therefore cannot revoke an already published cursor.
   */
  std::string route_inspection_continuation(const std::string& id,
                                            const InspectionRequest& request);

  /**
   * @brief Reads graph-list, node-id, or graph-view continuation values.
   * @param id Valid correlated request id.
   * @param request Validated cursor and frozen binding.
   * @return Complete bounded continuation response.
   * @throws std::bad_alloc if retained values or encoding allocate.
   * @throws std::invalid_argument for malformed retained node values.
   * @note Performs snapshot-only access and releases no live session state.
   */
  std::string route_inspection_basic_continuation(
      const std::string& id, const InspectionRequest& request);

  /**
   * @brief Reads one dependency-tree continuation page.
   * @param id Valid correlated request id.
   * @param request Validated cursor and frozen binding.
   * @return Complete bounded continuation response.
   * @throws std::bad_alloc if retained header/rows or encoding allocate.
   * @throws std::invalid_argument for malformed retained tree values.
   * @note Rejects a missing immutable header as an internal invariant failure
   *       without resolving the original session or calling Host.
   */
  std::string route_inspection_dependency_continuation(
      const std::string& id, const InspectionRequest& request);

  /**
   * @brief Reads one traversal continuation page.
   * @param id Valid correlated request id.
   * @param request Validated cursor and frozen binding.
   * @return Complete bounded continuation response.
   * @throws std::bad_alloc if retained rows or encoding allocate.
   * @throws std::invalid_argument for malformed retained traversal values.
   * @note Selects order/detail row type from the frozen method and performs no
   *       Host or registry-admission operation.
   */
  std::string route_inspection_traversal_continuation(
      const std::string& id, const InspectionRequest& request);

  /**
   * @brief Reads one recent-planning continuation page.
   * @param id Valid correlated request id.
   * @param request Validated cursor and frozen binding.
   * @return Complete bounded continuation response.
   * @throws std::bad_alloc if retained snapshots or encoding allocate.
   * @throws std::invalid_argument for malformed retained planning values.
   * @note Performs snapshot-only access and no live-session resolution.
   */
  std::string route_inspection_planning_continuation(
      const std::string& id, const InspectionRequest& request);

  /**
   * @brief Reconciles and publishes one graph-list first page.
   * @param id Valid correlated request id.
   * @param request Validated graph-list request moved for publication.
   * @return Complete bounded first-page response.
   * @throws std::bad_alloc if reservation, Host copying, or encoding allocates.
   * @throws std::invalid_argument for malformed successful Host values.
   * @throws Whatever Host listing or registry reconciliation propagates.
   * @note Reserves before one serialized Host listing; reconciliation occurs
   *       under the same Host lock and publication is transactional.
   */
  std::string route_inspection_graph_list_first_page(const std::string& id,
                                                     InspectionRequest request);

  /**
   * @brief Admits a session and dispatches one inspection first-page family.
   * @param id Valid correlated request id.
   * @param request Validated inspection request moved toward publication.
   * @return Complete bounded first-page response.
   * @throws std::bad_alloc if admission/reservation/dispatch allocates.
   * @throws Whatever the selected Host inspection propagates.
   * @note Keeps one admission alive across one pre-Host reservation and the
   *       selected helper; fallback is an internal invariant error.
   */
  std::string route_inspection_first_page(const std::string& id,
                                          InspectionRequest request);

  /**
   * @brief Publishes node-id collection inspection values.
   * @param id Valid correlated request id.
   * @param request Validated inspection request moved for publication.
   * @param host_session Host identity protected by caller admission.
   * @param reservation Active pre-Host snapshot quota ownership.
   * @return Complete bounded first-page response.
   * @throws std::bad_alloc if Host copying/measurement/publication allocates.
   * @throws Whatever the selected Host list operation propagates.
   * @note Invokes exactly one matching Host call under `host_mutex_`; failed
   *       paths roll reservation ownership back by destruction.
   */
  std::string route_inspection_node_list_first_page(
      const std::string& id, InspectionRequest request,
      const GraphSessionId& host_session,
      CollectionSnapshotRegistry::Reservation reservation);

  /**
   * @brief Publishes one complete graph inspection snapshot first page.
   * @param id Valid correlated request id.
   * @param request Validated inspection request moved for publication.
   * @param host_session Host identity protected by caller admission.
   * @param reservation Active pre-Host snapshot quota ownership.
   * @return Complete bounded first-page response.
   * @throws std::bad_alloc if Host copying/measurement/publication allocates.
   * @throws std::invalid_argument for malformed successful nodes.
   * @throws Whatever the Host graph inspection propagates.
   * @note Calls Host once, measures recursive node content, then moves the
   *       admitted snapshot into cursor storage transactionally.
   */
  std::string route_inspection_graph_first_page(
      const std::string& id, InspectionRequest request,
      const GraphSessionId& host_session,
      CollectionSnapshotRegistry::Reservation reservation);

  /**
   * @brief Publishes one dependency-tree snapshot first page.
   * @param id Valid correlated request id.
   * @param request Validated inspection request moved for publication.
   * @param host_session Host identity protected by caller admission.
   * @param reservation Active pre-Host snapshot quota ownership.
   * @return Complete bounded first-page response.
   * @throws std::bad_alloc if Host/header/rows/publication allocate.
   * @throws std::invalid_argument for malformed successful tree content.
   * @throws Whatever the Host dependency-tree operation propagates.
   * @note Calls Host once and shares a copied immutable header across rows;
   *       recursive counts/bytes are checked before cursor publication.
   */
  std::string route_inspection_dependency_first_page(
      const std::string& id, InspectionRequest request,
      const GraphSessionId& host_session,
      CollectionSnapshotRegistry::Reservation reservation);

  /**
   * @brief Publishes traversal-order or traversal-detail first-page rows.
   * @param id Valid correlated request id.
   * @param request Validated inspection request moved for publication.
   * @param host_session Host identity protected by caller admission.
   * @param reservation Active pre-Host snapshot quota ownership.
   * @return Complete bounded first-page response.
   * @throws std::bad_alloc if Host/row/publication storage allocates.
   * @throws std::invalid_argument for malformed successful traversal values.
   * @throws Whatever the matching Host traversal operation propagates.
   * @note Calls exactly one selected Host method; each map branch remains an
   *       indivisible retained row and is measured before publication.
   */
  std::string route_inspection_traversal_first_page(
      const std::string& id, InspectionRequest request,
      const GraphSessionId& host_session,
      CollectionSnapshotRegistry::Reservation reservation);

  /**
   * @brief Publishes recent compute-planning history first-page rows.
   * @param id Valid correlated request id.
   * @param request Validated inspection request moved for publication.
   * @param host_session Host identity protected by caller admission.
   * @param reservation Active pre-Host snapshot quota ownership.
   * @return Complete bounded first-page response.
   * @throws std::bad_alloc if Host copying/measurement/publication allocates.
   * @throws std::invalid_argument for malformed successful planning values.
   * @throws Whatever the Host planning-history operation propagates.
   * @note Calls Host exactly once and retains only copied public snapshots;
   *       oversized values publish no cursor.
   */
  std::string route_inspection_planning_first_page(
      const std::string& id, InspectionRequest request,
      const GraphSessionId& host_session,
      CollectionSnapshotRegistry::Reservation reservation);

  /**
   * @brief Routes bounded polling compute-job lifecycle methods.
   *
   * @param id Valid request id correlated with the response.
   * @param method Exact version 1 compute-job method name.
   * @param routed_params Internal adapter borrowing the structurally valid
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

  /**
   * @brief Routes process-global operation-plugin control and copied views.
   *
   * @param id Valid request id correlated with the response.
   * @param method Exact version 1 operation-plugin method name.
   * @param routed_params Internal adapter borrowing structurally valid params.
   * @return Complete response for this family, or `std::nullopt` when another
   *         route family must handle the method.
   * @throws std::bad_alloc if validation, Host copying, snapshot ownership, or
   *         response construction cannot allocate.
   * @throws std::invalid_argument if a successful Host returns malformed
   *         counts, GraphErrc values, UTF-8, keys, or source labels.
   * @throws Whatever the matching Host method propagates; `route()` preserves
   *         resource exhaustion and maps other standard exceptions.
   * @note These methods are process-global and never resolve a graph session.
   *       Every mutation invokes exactly one matching Host method under
   *       `host_mutex_` and is never retried. Each first view request reserves
   *       bounded snapshot quota before exactly one Host call, sorts by public
   *       key, and freezes the copied result. Continuations use only that
   *       frozen cursor record and never call Host. This private route does not
   *       change the separately maintained exact 55-method advertisement.
   */
  std::optional<std::string> route_plugin_method(
      const std::string& id, const std::string& method,
      const RoutedParams& routed_params);

  /**
   * @brief Routes non-paged operation-plugin mutations and reports.
   * @param id Valid correlated request id.
   * @param method Exact recognized plugin method.
   * @param routed_params Internal adapter borrowing structural params.
   * @return Complete response, or `std::nullopt` for a paged plugin view.
   * @throws std::bad_alloc if validation, Host values, or encoding allocate.
   * @throws std::invalid_argument for malformed successful Host counts/values.
   * @throws Whatever the matching Host operation propagates.
   * @note Each handled method invokes exactly one Host call under
   *       `host_mutex_`; mutations are process-global and never retried.
   */
  std::optional<std::string> route_plugin_direct_method(
      const std::string& id, const std::string& method,
      const RoutedParams& routed_params);

  /**
   * @brief Reads one plugin view continuation from its frozen snapshot.
   * @param id Valid correlated request id.
   * @param request Validated method/page/binding values.
   * @return Complete bounded continuation response.
   * @throws std::bad_alloc if page copying or encoding cannot allocate.
   * @throws std::invalid_argument for malformed retained public values.
   * @note Performs no Host call, session resolution, or new reservation; final
   *       page ownership is released by the snapshot registry.
   */
  std::string route_plugin_continuation(const std::string& id,
                                        const StableCollectionRequest& request);

  /**
   * @brief Calls Host once and publishes one first plugin view page.
   * @param id Valid correlated request id.
   * @param request Validated method/page/binding values.
   * @return Complete bounded first-page response.
   * @throws std::bad_alloc if reservation, Host copying, or encoding allocates.
   * @throws std::invalid_argument for malformed successful Host values.
   * @throws Whatever the matching Host view propagates.
   * @note Reserves quota before one serialized Host call, sorts the copied
   *       values, measures them, and atomically publishes the frozen snapshot.
   */
  std::string route_plugin_first_page(const std::string& id,
                                      StableCollectionRequest request);

  /**
   * @brief Routes scheduler discovery, configuration, and session control.
   *
   * @param id Valid request id correlated with the response.
   * @param method Exact version 1 scheduler method name other than trace.
   * @param routed_params Internal adapter borrowing structurally valid params.
   * @return Complete response for this family, or `std::nullopt` when another
   *         route family must handle the method.
   * @throws std::bad_alloc if validation, Host copying, snapshot ownership, or
   *         response construction cannot allocate.
   * @throws std::invalid_argument if a successful Host returns malformed
   *         counts, descriptions, labels, type names, intent, or info text.
   * @throws Whatever the matching Host method propagates; `route()` preserves
   *         resource exhaustion and maps other standard exceptions.
   * @note Discovery/default methods are process-global and never resolve a
   *       graph session. Type/plugin first pages reserve quota before exactly
   *       one Host call, sort copied labels, and freeze a stable snapshot;
   *       continuations call no Host method. Information and replacement
   *       validate all known values before opaque-session admission and then
   *       invoke exactly one Host method under `host_mutex_`. Mutations are
   *       never retried. `scheduler.trace` remains in the bounded observation
   *       route and retains its existing non-destructive sequence semantics.
   */
  std::optional<std::string> route_scheduler_method(
      const std::string& id, const std::string& method,
      const RoutedParams& routed_params);

  /**
   * @brief Routes non-session scheduler discovery/configuration methods.
   * @param id Valid correlated request id.
   * @param method Exact recognized scheduler method.
   * @param routed_params Internal adapter borrowing structural params.
   * @return Complete response, or `std::nullopt` for session/list methods.
   * @throws std::bad_alloc if validation, Host values, or encoding allocate.
   * @throws std::invalid_argument for malformed successful Host values.
   * @throws Whatever the matching Host operation propagates.
   * @note Each handled method invokes one process-global Host operation under
   *       `host_mutex_` and no mutation is retried.
   */
  std::optional<std::string> route_scheduler_global_method(
      const std::string& id, const std::string& method,
      const RoutedParams& routed_params);

  /**
   * @brief Routes one scheduler-type description lookup.
   * @param id Valid correlated request id.
   * @param routed_params Internal adapter borrowing structural params.
   * @return Complete description response or validation/Host error.
   * @throws std::bad_alloc if validation, Host copying, or encoding allocates.
   * @throws std::invalid_argument for malformed successful description text.
   * @throws Whatever the Host description operation propagates.
   * @note Validates type text before exactly one serialized Host call.
   */
  std::string route_scheduler_description_method(
      const std::string& id, const RoutedParams& routed_params);

  /**
   * @brief Routes scheduler scan or explicit plugin load.
   * @param id Valid correlated request id.
   * @param method Exact scan/load method.
   * @param routed_params Internal adapter borrowing structural params.
   * @return Complete mutation response or validation/Host error.
   * @throws std::bad_alloc if decoded inputs, Host result, or encoding
   * allocates.
   * @throws Whatever the selected Host plugin operation propagates.
   * @note Validates every input before one serialized process-global mutation
   *       and never retries after ambiguous failure.
   */
  std::string route_scheduler_plugin_method(const std::string& id,
                                            const std::string& method,
                                            const RoutedParams& routed_params);

  /**
   * @brief Routes scheduler default configuration.
   * @param id Valid correlated request id.
   * @param routed_params Internal adapter borrowing structural params.
   * @return Complete mutation response or validation/Host error.
   * @throws std::bad_alloc if decoded inputs, Host result, or encoding
   * allocates.
   * @throws Whatever the Host default-configuration operation propagates.
   * @note Validates both type labels and an exact worker count in `[0,8]`
   *       before acquiring `host_mutex_` or making one serialized Host
   *       mutation. The mutation is never retried.
   */
  std::string route_scheduler_defaults_method(
      const std::string& id, const RoutedParams& routed_params);

  /**
   * @brief Routes per-session scheduler information and replacement.
   * @param id Valid correlated request id.
   * @param method Exact recognized scheduler method.
   * @param routed_params Internal adapter borrowing structural params.
   * @return Complete response, or `std::nullopt` for global/list methods.
   * @throws std::bad_alloc if validation, admission, or encoding allocates.
   * @throws std::invalid_argument for mismatched successful scheduler info.
   * @throws Whatever the matching Host operation propagates.
   * @note Validates before opaque-session admission, then makes exactly one
   *       serialized Host call shared with compute execution.
   */
  std::optional<std::string> route_scheduler_session_method(
      const std::string& id, const std::string& method,
      const RoutedParams& routed_params);

  /**
   * @brief Reads one scheduler-list continuation from a frozen snapshot.
   * @param id Valid correlated request id.
   * @param request Validated method/page/binding values.
   * @return Complete bounded continuation response.
   * @throws std::bad_alloc if page copying or encoding cannot allocate.
   * @throws std::invalid_argument for malformed retained public values.
   * @note Performs no Host call or reservation and releases final-page
   *       ownership through the snapshot registry.
   */
  std::string route_scheduler_continuation(
      const std::string& id, const StableCollectionRequest& request);

  /**
   * @brief Calls Host once and publishes one first scheduler-list page.
   * @param id Valid correlated request id.
   * @param request Validated method/page/binding values.
   * @return Complete bounded first-page response.
   * @throws std::bad_alloc if reservation, Host copying, or encoding allocates.
   * @throws std::invalid_argument for malformed successful Host labels.
   * @throws Whatever the matching Host list operation propagates.
   * @note Reserves quota before one serialized Host call, sorts labels, then
   *       measures and publishes the stable snapshot transactionally.
   */
  std::string route_scheduler_first_page(const std::string& id,
                                         StableCollectionRequest request);

  /**
   * @brief Routes bounded compute events and scheduler trace observations.
   *
   * @param id Valid request id correlated with the response.
   * @param method Exact `events.drain` or `scheduler.trace` method name.
   * @param routed_params Internal adapter borrowing structurally valid params.
   * @return Complete response for this family, or `std::nullopt` when another
   *         route family must handle the method.
   * @throws std::bad_alloc if validation, Host copying, or encoding cannot
   *         allocate.
   * @throws std::invalid_argument if a successful Host returns malformed
   *         sequences, UTF-8, enum values, or locked page metadata.
   * @throws Whatever the matching Host method propagates; `route()` preserves
   *         resource exhaustion and maps other standard exceptions.
   * @note All known params are validated before session resolution. Each call
   *       holds one session admission and invokes exactly one matching Host
   *       operation under `host_mutex_`. Compute-event drains are destructive
   *       only inside Host; scheduler traces are non-destructive. Neither path
   *       reserves a stable collection snapshot or advertises a new method.
   */
  std::optional<std::string> route_observation_method(
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
