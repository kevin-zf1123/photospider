#pragma once

#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "photospider/core/export.hpp"
#include "photospider/core/image_buffer.hpp"
#include "photospider/core/inspection_types.hpp"
#include "photospider/core/result_types.hpp"
#include "photospider/host/compute_request.hpp"
#include "photospider/host/event_stream.hpp"
#include "photospider/host/graph_session.hpp"
#include "photospider/scheduler/scheduler.hpp"

/**
 * @file host.hpp
 * @brief Frontend-facing Photospider Host interface and embedded adapter
 * factory.
 *
 * Host is the stable local frontend boundary. Implementations translate these
 * request and snapshot values into an embedded backend stack or the installed
 * typed IPC transport without exposing backend runtime, model, execution,
 * compute, scheduler-queue, image-library, or parser object ownership through
 * installable headers.
 */

namespace ps {

/**
 * @brief Public graph-topology edge kind used by dependency-tree snapshots.
 *
 * @throws Nothing.
 * @note The values describe graph YAML relationships only; they do not expose
 *       mutable backend adjacency containers.
 */
enum class HostGraphEdgeKind {
  /** @brief Image input edge between two graph nodes. */
  ImageInput,

  /** @brief Parameter input edge between two graph nodes. */
  ParameterInput,
};

/**
 * @brief Copied dependency edge metadata for Host inspection.
 *
 * @throws Nothing for value operations except string allocation on mutation.
 * @note The edge is a point-in-time snapshot and is safe to serialize.
 */
struct HostGraphEdgeSnapshot {
  /** @brief Upstream node id. */
  NodeId from_node;

  /** @brief Downstream node id. */
  NodeId to_node;

  /** @brief Edge category from graph topology. */
  HostGraphEdgeKind kind = HostGraphEdgeKind::ImageInput;

  /** @brief Upstream output name, when present. */
  std::string from_output_name;

  /** @brief Downstream input or parameter name, when present. */
  std::string to_input_name;

  /** @brief Downstream input index for ordered image inputs. */
  size_t input_index = 0;
};

/**
 * @brief Scope used to build a Host dependency-tree snapshot.
 *
 * @throws Nothing.
 * @note The scope mirrors the existing inspection service without exposing its
 *       internal enum type.
 */
enum class HostDependencyTreeScope {
  /** @brief Tree was rooted at all graph ending nodes. */
  EndingNodes,

  /** @brief Tree was rooted at a single requested start node. */
  StartNode,
};

/**
 * @brief One row in a Host dependency-tree snapshot.
 *
 * @throws Nothing for value operations except string/container allocation.
 * @note `cycle` is an inspection flag. Host callers cannot mutate graph
 *       topology through this value.
 */
struct HostDependencyTreeEntry {
  /** @brief Display depth used by tree renderers. */
  int depth = 0;

  /** @brief Incoming edge from the parent row, if one exists. */
  std::optional<HostGraphEdgeSnapshot> incoming_edge;

  /** @brief Copied node inspection view for the row. */
  NodeInspectionView node;

  /** @brief Whether this row closes a cycle in the inspected path. */
  bool cycle = false;
};

/**
 * @brief Copied dependency-tree snapshot for frontend graph inspection.
 *
 * @throws Nothing for value operations except vector allocation on mutation.
 * @note The snapshot is derived from graph topology under backend state
 *       serialization and contains no model references.
 */
struct HostDependencyTreeSnapshot {
  /** @brief Tree scope selected by the request. */
  HostDependencyTreeScope scope = HostDependencyTreeScope::EndingNodes;

  /** @brief Requested start node when scope is StartNode. */
  std::optional<NodeId> start_node;

  /** @brief Whether the inspected graph had no nodes. */
  bool graph_empty = false;

  /** @brief Whether the requested start node was present. */
  bool start_node_found = true;

  /** @brief Whether no ending nodes were found for an ending-node tree. */
  bool no_ending_nodes = false;

  /** @brief Root node ids used to build the tree. */
  std::vector<NodeId> root_nodes;

  /** @brief Flattened dependency-tree entries. */
  std::vector<HostDependencyTreeEntry> entries;
};

/**
 * @brief Cache visibility for one node in a traversal branch.
 *
 * @throws Nothing for value operations except string allocation on mutation.
 * @note Disk-cache visibility is an inspection result and may be stale after
 *       later cache mutations.
 */
struct HostTraversalNodeSnapshot {
  /** @brief Node id represented by the traversal row. */
  NodeId node;

  /** @brief Human-readable node name. */
  std::string name;

  /** @brief Whether HP memory cache was observed for the node. */
  bool has_memory_cache = false;

  /** @brief Whether disk-cache files were observed for the node. */
  bool has_disk_cache = false;
};

/**
 * @brief Public plugin load failure record.
 *
 * @throws Nothing for value operations except string allocation on mutation.
 * @note The path is diagnostic text from the adapter and is not a plugin
 *       handle.
 */
struct HostPluginLoadError {
  /** @brief Candidate plugin path that failed, when known. */
  std::string path;

  /** @brief Stable failure category. */
  GraphErrc code = GraphErrc::Unknown;

  /** @brief Human-readable diagnostic text. */
  std::string message;
};

/**
 * @brief Public plugin scan/load report.
 *
 * @throws Nothing for value operations except vector/string allocation.
 * @note `new_op_keys` lists operation keys registered or replaced by the load
 *       operation. Successful handles belong to the process-wide operation
 *       plugin owner, not to the Host that initiated the load.
 */
struct HostPluginLoadReport {
  /** @brief Number of plugin candidates considered. */
  int attempted = 0;

  /** @brief Number of plugin libraries loaded and retained. */
  int loaded = 0;

  /** @brief Plugin load failures. */
  std::vector<HostPluginLoadError> errors;

  /** @brief Operation keys registered or replaced by successful plugins. */
  std::vector<std::string> new_op_keys;
};

/**
 * @brief Scheduler defaults applied to graph sessions loaded after update.
 *
 * @throws Nothing for value operations except string allocation on mutation.
 * @note This mirrors the embedded backend's scheduler default configuration
 *       without exposing scheduler factories or runtime ownership.
 */
struct HostSchedulerConfig {
  /** @brief Scheduler type used for global high-precision compute. */
  std::string hp_type = "cpu_work_stealing";

  /** @brief Scheduler type used for real-time dirty-region updates. */
  std::string rt_type = "cpu_work_stealing";

  /**
   * @brief CPU/plugin worker request; zero means bounded automatic selection.
   * @note For a built-in CPU route, zero resolves once per Host to a value in
   *       `[1,8]` and fixes that Host's shared CPU service pool. Positive
   *       values are exact and must not exceed `kSchedulerWorkerRequestMax`.
   *       Legacy scheduler routes interpret the same request when their
   *       Graph-owned scheduler is planned. This field does not configure the
   *       fixed process-wide aggregate capacity.
   */
  unsigned int worker_count = 0;
};

/**
 * @brief Frontend-facing Photospider graph host.
 *
 * Host is the narrow API that local GUI/WebUI code and the installed typed IPC
 * adapter call. Each non-destructor method returns a copied value snapshot or a
 * status for recoverable failures. Resource exhaustion remains exceptional so
 * callers can distinguish it from a domain/runtime failure status. Embedded
 * backend objects never appear in the public ABI.
 *
 * @throws std::bad_alloc from any non-destructor method when request handling,
 *         backend execution, backend-to-status translation, or result
 * construction exhausts memory.
 * @throws std::system_error from IPC compute polling when production mutex or
 *         condition-variable operations fail, and from documented async worker
 *         lifecycle failures.
 * @note Methods are not specified as thread-safe at the Host interface level.
 *       The embedded adapter delegates graph-state serialization to the same
 *       backend boundary used by the existing CLI path.
 */
class PHOTOSPIDER_API Host {
 public:
  /**
   * @brief Destroys the Host implementation.
   *
   * @throws Nothing.
   */
  virtual ~Host() = default;

  /**
   * @brief Loads a graph session.
   *
   * @param request Session name, root, YAML, config, and cache-root values.
   * @return Loaded session id on success; `GraphErrc::InvalidParameter` for a
   *         duplicate session or scheduler-request validation;
   *         `GraphErrc::ComputeError` when a complete legacy HP+RT scheduler
   *         pair cannot fit the Host resource ledger; `GraphErrc::Io` for an
   *         explicit
   *         missing/unreadable/uncopyable source or session-path failure;
   *         `GraphErrc::InvalidYaml` for syntax, root-shape, duplicate-id, or
   *         node-schema rejection;
   *         `GraphErrc::MissingDependency` or `GraphErrc::Cycle` for topology
   *         rejection; or `GraphErrc::Unknown` for an unexpected internal
   *         failure.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note The returned session is a value label, never a runtime pointer. The
   *       embedded implementation plans both scheduler intents before document
   *       ingestion. The first built-in CPU selection may configure and start
   *       the Host-lifetime ExecutionService even if this Graph load later
   *       fails, but the fixed pool is uncharged infrastructure and retains no
   *       ledger reservation. Graph candidates, legacy scheduler candidates
   *       and reservations, and session publication still roll back
   *       transactionally; built-in CPU routes create no per-Graph scheduler
   *       owner. Empty `yaml_path` loads existing session-local
   *       content or intentionally creates an empty graph; nonempty `yaml_path`
   *       is explicit and never falls back. Nonempty relative request paths use
   *       the caller process working directory; an IPC implementation resolves
   *       them in its client process before sending the typed request.
   */
  virtual Result<GraphSessionId> load_graph(
      const GraphLoadRequest& request) = 0;

  /**
   * @brief Closes a loaded graph session.
   *
   * @param session Session to close.
   * @return Success, `GraphErrc::NotFound` only when the session is absent, or
   *         another failure code when shutdown fails while the session remains
   *         loaded.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note The embedded implementation first publishes its closing marker and
   *       drains synchronous calls admitted before that marker. It then stops
   *       graph-state lane admission so full-FIFO producers are awakened and
   *       rejected before close waits pre-registered async schedulers and every
   *       backend-accepted caller-visible status future. Only then does it join
   *       the drained lane and begin scheduler shutdown or backend-resource
   *       release. A failed scheduler shutdown recreates one lane worker and
   *       reopens admission for the retained session so callers may retry it.
   */
  virtual VoidResult close_graph(const GraphSessionId& session) = 0;

  /**
   * @brief Lists graph sessions known to the Host.
   *
   * @return Value list of session ids.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Session ids are copied labels and do not imply the caller owns
   *       backend graph resources.
   */
  virtual Result<std::vector<GraphSessionId>> list_graphs() const = 0;

  /**
   * @brief Reloads one graph session from a YAML file.
   *
   * @param session Session to reload.
   * @param yaml_path YAML path to load.
   * @return Success; `GraphErrc::NotFound` for a missing or closing session;
   *         `GraphErrc::InvalidParameter` for an empty path on an existing
   *         session; `GraphErrc::Io` for missing/unreadable input;
   *         `GraphErrc::InvalidYaml` for syntax or node-schema rejection;
   *         `GraphErrc::MissingDependency` or `GraphErrc::Cycle` for topology
   *         rejection; or `GraphErrc::Unknown` for an unexpected internal
   *         failure.
   * @throws std::bad_alloc if request processing, reload execution, backend-
   *         to-status translation, or copied result construction exhausts
   *         memory.
   * @note The embedded adapter admits the session before checking existence,
   *       then retains that admission across backend graph-state mutation and
   *       exact public failure translation. Concurrent close therefore waits
   *       for an accepted reload, while reload after the close marker returns
   *       NotFound without touching Kernel state. Any failure or propagated
   *       resource exhaustion preserves the published nodes, topology
   *       adjacency and generation, runtime graph state, and session identity.
   *       A successful reload installs the complete replacement, resets
   *       runtime state, and advances topology generation within the serialized
   *       commit. A nonempty relative `yaml_path` uses the caller process
   *       working directory; the IPC Host resolves it in the client process
   *       before transport.
   */
  virtual VoidResult reload_graph(const GraphSessionId& session,
                                  const std::string& yaml_path) = 0;

  /**
   * @brief Saves one graph session to a YAML file.
   *
   * @param session Session to save.
   * @param yaml_path Destination YAML path.
   * @return Success, `GraphErrc::NotFound` for a missing or closing session,
   *         or `GraphErrc::Io` for recoverable node serialization, YAML
   *         emission, or destination preparation/open/write/flush/close
   *         failure.
   * @throws std::bad_alloc if graph-state submission, node/YAML serialization,
   *         path handling, backend-to-status translation, or copied result
   *         construction exhausts memory.
   * @note The Host returns only status; file ownership remains with the
   *       caller-provided path. Embedded execution retains a session admission
   *       across required-session resolution and graph-state serialization so
   *       concurrent close cannot invalidate the runtime. Saving writes
   *       directly to the supplied path rather than atomically replacing it.
   *       All outcomes preserve graph topology, runtime state, and session
   *       ownership. A failure before destination open preserves existing
   *       bytes; a post-open failure may leave a created, truncated, or
   *       partially written destination. A nonempty relative `yaml_path` uses
   *       the caller process working directory; the IPC Host resolves it in
   *       the client process before transport and never automatically retries
   *       this mutation.
   */
  virtual VoidResult save_graph(const GraphSessionId& session,
                                const std::string& yaml_path) = 0;

  /**
   * @brief Clears graph model state for a loaded session.
   *
   * @param session Session to clear.
   * @return Success or failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note This mirrors the existing clear-graph frontend behavior.
   */
  virtual VoidResult clear_graph(const GraphSessionId& session) = 0;

  /**
   * @brief Computes one node synchronously.
   *
   * @param request Host compute request.
   * @return Success, NotFound when the graph session is missing or closed, or
   *         a compute failure status for existing sessions.
   * @throws std::bad_alloc if request processing, compute execution, backend-
   *         to-status translation, or copied result construction exhausts
   *         memory.
   * @throws std::system_error if IPC polling mutex or condition-variable
   *         operations fail.
   * @note The embedded adapter admits the complete Kernel call/status mapping
   *       against concurrent close. Backend LastError diagnostics are used only
   *       after it has established that the requested graph session exists.
   */
  virtual VoidResult compute(const HostComputeRequest& request) = 0;

  /**
   * @brief Schedules one node compute asynchronously.
   *
   * @param request Host compute request captured by value.
   * @return Future resolving to the final operation status, or a failure status
   *         when scheduling cannot start.
   * @throws std::bad_alloc if request processing, async submission, backend-
   *         to-status translation, or copied result construction exhausts
   *         memory.
   * @throws std::system_error if local worker creation or a worker-join
   *         system/lifecycle invariant fails. The IPC worker is created before
   *         submission, so creation failure has no remote side effect.
   * @note The embedded backend work item owns its exact failure category and
   *       message; the wrapper never reconstructs the result from mutable
   *       LastError state. Embedded scheduling pre-registers a close-visible
   *       placeholder, releases the Host lifecycle mutex before bounded-lane
   *       submission, and either publishes the accepted backend future or
   *       removes the rejected placeholder. Close first stops lane admission,
   *       then waits until every accepted caller-visible promise is ready
   *       before releasing the runtime. Consuming the returned future may
   *       rethrow `std::bad_alloc` from backend compute/result translation or
   *       `std::system_error` from IPC polling synchronization.
   */
  virtual Result<std::future<OperationStatus>> compute_async(
      HostComputeRequest request) = 0;

  /**
   * @brief Computes one node and returns an image snapshot descriptor.
   *
   * @param request Host compute request.
   * @return ImageBuffer descriptor on success, an ok empty descriptor when the
   *         compute succeeds without image output, `GraphErrc::NotFound` when
   *         the graph session is missing or closed, or a compute failure status
   *         for existing sessions.
   * @throws std::bad_alloc if request processing, compute/image execution,
   *         backend-to-status translation, or copied result construction
   *         exhausts memory.
   * @throws std::system_error if IPC polling mutex or condition-variable
   *         operations fail.
   * @note The embedded adapter admits the complete image compute and result
   *       mapping against concurrent close, checks session existence before
   *       dispatch, and consults LastError only to distinguish handled failure
   *       from successful empty output.
   * @note Backend GraphError classifications such as `GraphErrc::NoOperation`
   *       are preserved when image compute fails after session validation, and
   *       successful backend image memory is cloned into the public descriptor.
   * @note Callers must treat the returned payload as a read-only snapshot
   *       unless they own an adapter-specific writable contract. The IPC Host
   *       returns tight rows over a page-aligned-base
   *       `PROT_READ|MAP_PRIVATE` mapping; only its base is page aligned, so
   *       later row starts need not satisfy the kernel-owned 64-byte alignment
   *       contract. Copies share that mapping until the final reference.
   */
  virtual Result<ImageBuffer> compute_and_get_image(
      const HostComputeRequest& request) = 0;

  /**
   * @brief Returns the latest timing snapshot for a session.
   *
   * @param session Session to inspect.
   * @return Copied timing rows and total, or `GraphErrc::NotFound` for a
   *         missing or closing session/timing snapshot.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Timing data is populated only when a compute request enabled timing.
   * The embedded Host retains one session admission through backend access,
   * value copying, and public result translation, so concurrent close waits for
   * an already accepted inspection.
   */
  virtual Result<TimingSnapshot> timing(const GraphSessionId& session) = 0;

  /**
   * @brief Returns the latest backend IO time for a session.
   *
   * @param session Session whose latest IO accumulator should be read.
   * @return IO time in milliseconds, or a failure status when the session is
   *         missing or has no readable timing state.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note The value is copied from backend diagnostic state after compute and
   *       is telemetry only; it does not synchronize with active computes.
   */
  virtual Result<double> last_io_time(const GraphSessionId& session) const = 0;

  /**
   * @brief Returns the last backend error recorded for a session.
   *
   * @param session Session to inspect.
   * @return Error status snapshot. When no error exists, status is ok.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note This is diagnostic state and should not be used for synchronization.
   */
  virtual OperationStatus last_error(const GraphSessionId& session) const = 0;

  /**
   * @brief Lists node ids for a session.
   *
   * @param session Session to inspect.
   * @return Copied node id list, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note The ids are a snapshot and can become stale after graph reload/edit.
   */
  virtual Result<std::vector<NodeId>> list_node_ids(
      const GraphSessionId& session) = 0;

  /**
   * @brief Lists ending node ids for a session.
   *
   * @param session Session to inspect.
   * @return Copied ending node ids, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Ending-node semantics match the backend topology service.
   */
  virtual Result<std::vector<NodeId>> ending_nodes(
      const GraphSessionId& session) = 0;

  /**
   * @brief Reads one node as YAML text.
   *
   * @param session Session containing the node.
   * @param node Node to read.
   * @return YAML text, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note YAML is exposed as text so host clients do not depend on yaml-cpp.
   */
  virtual Result<std::string> get_node_yaml(const GraphSessionId& session,
                                            NodeId node) = 0;

  /**
   * @brief Replaces one node from YAML text.
   *
   * @param session Session containing the node.
   * @param node Node to replace.
   * @param yaml_text YAML text for the replacement node.
   * @return Success, `GraphErrc::NotFound` for a missing/closing session or
   *         missing node, or `GraphErrc::InvalidYaml` for parsing or complete
   *         candidate-topology validation failure.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note The backend owns validation and topology rebuilding. Embedded
   *       execution retains a session admission across one serialized
   *       required-node lookup, parse, validation, and replacement operation.
   */
  virtual VoidResult set_node_yaml(const GraphSessionId& session, NodeId node,
                                   const std::string& yaml_text) = 0;

  /**
   * @brief Inspects one node.
   *
   * @param session Session containing the node.
   * @param node Node to inspect.
   * @return Copied node inspection view, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note The result contains only public value snapshots.
   */
  virtual Result<NodeInspectionView> inspect_node(const GraphSessionId& session,
                                                  NodeId node) = 0;

  /**
   * @brief Inspects all nodes in one graph session.
   *
   * @param session Session to inspect.
   * @return Copied graph inspection view, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note The result does not expose backend model or node references.
   */
  virtual Result<GraphInspectionView> inspect_graph(
      const GraphSessionId& session) = 0;

  /**
   * @brief Builds a dependency-tree snapshot.
   *
   * @param session Session to inspect.
   * @param node Optional start node; nullopt uses graph ending nodes.
   * @param include_metadata Whether node cache/spatial metadata should be
   *        included.
   * @return Copied dependency-tree snapshot, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Tree entries are flattened so CLI, GUI, and IPC clients can render
   *       them without backend data structures.
   */
  virtual Result<HostDependencyTreeSnapshot> dependency_tree(
      const GraphSessionId& session, std::optional<NodeId> node,
      bool include_metadata = false) = 0;

  /**
   * @brief Returns traversal orders keyed by ending node id.
   *
   * @param session Session to inspect.
   * @return Copied traversal orders, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Keys and values are plain node ids.
   */
  virtual Result<std::map<int, std::vector<NodeId>>> traversal_orders(
      const GraphSessionId& session) = 0;

  /**
   * @brief Returns traversal metadata keyed by ending node id.
   *
   * @param session Session to inspect.
   * @return Copied traversal node metadata, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Cache flags are observational snapshots.
   */
  virtual Result<std::map<int, std::vector<HostTraversalNodeSnapshot>>>
  traversal_details(const GraphSessionId& session) = 0;

  /**
   * @brief Returns tree roots that contain a node.
   *
   * @param session Session to inspect.
   * @param node Node to search for.
   * @return Copied ending-node ids, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note This mirrors the node editor's existing topology query.
   */
  virtual Result<std::vector<NodeId>> trees_containing_node(
      const GraphSessionId& session, NodeId node) = 0;

  /**
   * @brief Projects a source ROI forward to a target node.
   *
   * @param session Session containing the graph.
   * @param start_node Source node.
   * @param start_roi Source ROI.
   * @param target_node Target node.
   * @return Projected ROI, `GraphErrc::NotFound` for a missing/closing session
   *         or endpoint, or `GraphErrc::InvalidParameter` when existing
   *         endpoints produce no valid projection.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Pixel rectangles are public value copies. Embedded execution retains
   *       a session admission while endpoint lookup and projection run in one
   *       graph-state work item.
   */
  virtual Result<PixelRect> project_roi(const GraphSessionId& session,
                                        NodeId start_node,
                                        const PixelRect& start_roi,
                                        NodeId target_node) = 0;

  /**
   * @brief Projects a target ROI backward to a source node.
   *
   * @param session Session containing the graph.
   * @param target_node Target node.
   * @param target_roi Target ROI.
   * @param source_node Source node.
   * @return Projected ROI, `GraphErrc::NotFound` for a missing/closing session
   *         or endpoint, or `GraphErrc::InvalidParameter` when existing
   *         endpoints produce no valid projection.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Embedded execution retains a session admission while endpoint lookup
   *       and projection run in one graph-state work item.
   */
  virtual Result<PixelRect> project_roi_backward(const GraphSessionId& session,
                                                 NodeId target_node,
                                                 const PixelRect& target_roi,
                                                 NodeId source_node) = 0;

  /**
   * @brief Returns the latest dirty-region inspection snapshot.
   *
   * @param session Session to inspect.
   * @return Copied dirty-region snapshot, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Snapshot values exclude scheduler queues and task counters.
   */
  virtual Result<DirtyRegionInspectionSnapshot> dirty_region_snapshot(
      const GraphSessionId& session) = 0;

  /**
   * @brief Returns the latest compute planning inspection snapshot.
   *
   * @param session Session to inspect.
   * @return Optional copied planning snapshot, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Loaded sessions that have not computed yet return an empty optional
   *       with success status. Snapshot values exclude task closures,
   *       scheduler queues, backend object references, and mutable graph state.
   */
  virtual Result<std::optional<ComputePlanningInspectionSnapshot>>
  compute_planning_snapshot(const GraphSessionId& session) = 0;

  /**
   * @brief Returns bounded recent compute planning inspection snapshots.
   *
   * @param session Session to inspect.
   * @return Copied planning snapshot history, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note The backend bounds history length and returns values suitable for
   *       frontend display or typed IPC serialization.
   */
  virtual Result<std::vector<ComputePlanningInspectionSnapshot>>
  recent_compute_planning_snapshots(const GraphSessionId& session) = 0;

  /**
   * @brief Begins a dirty source lifecycle event.
   *
   * @param session Session containing the graph.
   * @param node Source node.
   * @param domain Dirty domain.
   * @param source_roi Source-local dirty ROI.
   * @return Updated dirty-region snapshot, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note The backend serializes dirty-source mutation through graph-state
   *       ownership.
   */
  virtual Result<DirtyRegionInspectionSnapshot> begin_dirty_source(
      const GraphSessionId& session, NodeId node, DirtyDomain domain,
      const PixelRect& source_roi) = 0;

  /**
   * @brief Updates a dirty source lifecycle event.
   *
   * @param session Session containing the graph.
   * @param node Source node.
   * @param domain Dirty domain.
   * @param source_roi Source-local dirty ROI.
   * @return Updated dirty-region snapshot, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Repeated updates accumulate in backend dirty source state.
   */
  virtual Result<DirtyRegionInspectionSnapshot> update_dirty_source(
      const GraphSessionId& session, NodeId node, DirtyDomain domain,
      const PixelRect& source_roi) = 0;

  /**
   * @brief Ends a dirty source lifecycle event.
   *
   * @param session Session containing the graph.
   * @param node Source node.
   * @param domain Dirty domain.
   * @return Updated dirty-region snapshot, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Ending the source marks it settled for the current generation.
   */
  virtual Result<DirtyRegionInspectionSnapshot> end_dirty_source(
      const GraphSessionId& session, NodeId node, DirtyDomain domain) = 0;

  /**
   * @brief Drains compute progress events for a session.
   *
   * @param session Session whose events should be drained.
   * @param limit Maximum events to remove; must be in
   *        `kComputeEventDrainMinLimit..kComputeEventDrainMaxLimit`.
   * @return Bounded sequenced event batch, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note A successful call removes only returned events and atomically resets
   *       the shared drop counter. Invalid/over-1,024-byte event name or source
   *       publications are dropped whole before retention and counted once.
   *       Invalid limits return `GraphErrc::InvalidParameter` without draining
   *       or resetting state.
   */
  virtual Result<ComputeEventBatch> drain_compute_events(
      const GraphSessionId& session, std::size_t limit) = 0;

  /**
   * @brief Returns copied scheduler trace events for a session.
   *
   * @param session Session to inspect.
   * @param after_sequence Exclusive sequence cursor; zero starts at the oldest
   *        retained trace and `kObservationSequenceExhausted` observes a
   *        terminal empty page.
   * @param limit Maximum events to copy; must be in
   *        `kSchedulerTraceMinLimit..kSchedulerTraceMaxLimit`.
   * @return Bounded non-destructive scheduler trace page, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Trace reads never remove events. Invalid limits, future cursors, or
   *       an exhausted sentinel used before actual exhaustion return
   *       `GraphErrc::InvalidParameter` without copying a page.
   */
  virtual Result<SchedulerTracePage> scheduler_trace(
      const GraphSessionId& session, uint64_t after_sequence,
      std::size_t limit) = 0;

  /**
   * @brief Clears all cache layers for a session.
   *
   * @param session Session whose caches should be cleared.
   * @return Success or `GraphErrc::NotFound` for a missing or closing session.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note This mirrors the existing `clear-cache all` behavior. The embedded
   * Host retains one session admission through backend mutation and public
   * status translation, so concurrent close waits for an already accepted
   * clear.
   */
  virtual VoidResult clear_cache(const GraphSessionId& session) = 0;

  /**
   * @brief Clears disk cache for a session.
   *
   * @param session Session whose disk cache should be cleared.
   * @return Success or failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note The first Host slice returns status only; detailed cache counts can
   *       be added as a follow-up value contract.
   */
  virtual VoidResult clear_drive_cache(const GraphSessionId& session) = 0;

  /**
   * @brief Clears memory cache for a session.
   *
   * @param session Session whose memory cache should be cleared.
   * @return Success or failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note This does not clear graph topology.
   */
  virtual VoidResult clear_memory_cache(const GraphSessionId& session) = 0;

  /**
   * @brief Saves all node caches for a session at one precision.
   *
   * @param session Session whose nodes should be cached.
   * @param precision Cache precision label.
   * @return Success or failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Precision semantics are backend-defined and match existing CLI
   *       behavior.
   */
  virtual VoidResult cache_all_nodes(const GraphSessionId& session,
                                     const std::string& precision) = 0;

  /**
   * @brief Releases transient memory cache for a session.
   *
   * @param session Session whose transient memory should be released.
   * @return Success or failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Persistent graph and disk-cache state remain owned by the backend.
   */
  virtual VoidResult free_transient_memory(const GraphSessionId& session) = 0;

  /**
   * @brief Synchronizes disk cache for a session at one precision.
   *
   * @param session Session whose cache should be synchronized.
   * @param precision Cache precision label.
   * @return Success or failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note The first Host slice returns status only.
   */
  virtual VoidResult synchronize_disk_cache(const GraphSessionId& session,
                                            const std::string& precision) = 0;

  /**
   * @brief Loads operation plugins from directories.
   *
   * @param dirs Directories or path patterns to scan.
   * @return Structured plugin load report.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note All Host instances share one process plugin owner. Destroying this
   *       Host does not unload successful plugins; another Host may inspect and
   *       use them until an explicit process-global unload.
   */
  virtual Result<HostPluginLoadReport> plugins_load_report(
      const std::vector<std::string>& dirs) = 0;

  /**
   * @brief Loads operation plugins and returns status only.
   *
   * @param dirs Directories or path patterns to scan.
   * @return Success or failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note This mutates the same process-global owner as plugins_load_report();
   *       Host destruction does not reverse a successful load.
   */
  virtual VoidResult plugins_load(const std::vector<std::string>& dirs) = 0;

  /**
   * @brief Explicitly unloads all process-global operation plugins.
   *
   * @return Number of active operation keys removed or restored, or a failure
   *         status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note The mutation is visible to every Host. Built-in registrations remain
   *       available. Already copied callbacks and plugin-derived return values
   *       retain their library lease until their own destruction completes.
   */
  virtual Result<int> plugins_unload_all() = 0;

  /**
   * @brief Seeds built-in operation sources into plugin inspection state.
   *
   * @return Success status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Built-in registration runs at most once through the process owner;
   *       repeated calls only reconcile labels and cannot overwrite an active
   *       plugin replacement.
   */
  virtual VoidResult seed_builtin_ops() = 0;

  /**
   * @brief Returns operation source labels keyed by operation key.
   *
   * @return Copied operation source map.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Source labels are process-global copied diagnostics such as paths or
   *       `built-in`; the result borrows no manager storage.
   */
  virtual Result<std::map<std::string, std::string>> ops_sources() const = 0;

  /**
   * @brief Returns combined operation keys for frontend operation lists.
   *
   * @return Copied operation keys.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note The process-global snapshot collapses compatible HP/RT/tiled
   *       implementations and remains independent of later Host mutations.
   */
  virtual Result<std::vector<std::string>> ops_combined_keys() const = 0;

  /**
   * @brief Returns combined operation source labels.
   *
   * @return Copied combined operation source map.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note The copied map and registry keys come from one serialized
   *       process-owner snapshot.
   */
  virtual Result<std::map<std::string, std::string>> ops_combined_sources()
      const = 0;

  /**
   * @brief Lists scheduler types available to the Host.
   *
   * @return Copied scheduler type names.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Includes built-in scheduler types and any loaded scheduler plugins.
   */
  virtual Result<std::vector<std::string>> scheduler_available_types()
      const = 0;

  /**
   * @brief Returns a scheduler type description.
   *
   * @param type_name Scheduler type name.
   * @return Description text, or `GraphErrc::NotFound` when the scheduler type
   *         is not available to the Host.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Descriptions are for display and must not drive behavior; callers can
   *       use the status code to distinguish typos from real descriptions.
   */
  virtual Result<std::string> scheduler_description(
      const std::string& type_name) const = 0;

  /**
   * @brief Scans directories for scheduler plugins.
   *
   * @param dirs Directories to scan.
   * @return Number of scheduler types loaded.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Loaded scheduler plugin handles remain owned by the backend loader.
   */
  virtual Result<size_t> scheduler_scan(
      const std::vector<std::string>& dirs) = 0;

  /**
   * @brief Loads one scheduler plugin.
   *
   * @param path Plugin library path.
   * @return Success or failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Loading registers scheduler factory entries for later graph use.
   */
  virtual VoidResult scheduler_load(const std::string& path) = 0;

  /**
   * @brief Lists loaded scheduler plugin labels.
   *
   * @return Copied plugin labels.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Labels are diagnostic strings, not plugin handles.
   */
  virtual Result<std::vector<std::string>> scheduler_loaded_plugins() const = 0;

  /**
   * @brief Applies scheduler defaults used when loading future sessions.
   *
   * @param config Scheduler type names and worker count.
   * @return Success, or `GraphErrc::InvalidParameter` when the positive worker
   *         request exceeds `kSchedulerWorkerRequestMax`, conflicts with an
   *         already fixed built-in CPU service pool, or exceeds the private
   *         composition CPU limit.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Selecting a built-in CPU type resolves and starts exactly one
   *       Host-lifetime CPU pool. Pool threads are uncharged infrastructure;
   *       only admitted Runs and legacy scheduler owners reserve the ledger CPU
   *       dimension. Later zero/equal worker requests are idempotent, while a
   *       different positive request is rejected. Existing loaded graph
   *       bindings remain unchanged; callers can use replace_scheduler() to
   *       update them explicitly. A rejected candidate leaves the previous
   *       future-session defaults unchanged. Legacy Graph-owned scheduler
   *       capacity is still reserved transactionally by later graph load or
   *       replacement.
   */
  virtual VoidResult configure_scheduler_defaults(
      const HostSchedulerConfig& config) = 0;

  /**
   * @brief Returns scheduler information for a graph intent.
   *
   * @param session Session to inspect.
   * @param intent Compute intent served by the scheduler.
   * @return Copied scheduler info, or a failure status.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note The Host returns value text instead of exposing scheduler objects.
   *       Embedded inspection shares graph-state serialization with compute,
   *       replacement, and close so no borrowed scheduler outlives its owner.
   */
  virtual Result<SchedulerInfoSnapshot> scheduler_info(
      const GraphSessionId& session, ComputeIntent intent) const = 0;

  /**
   * @brief Replaces the scheduler for a graph intent.
   *
   * @param session Session to update.
   * @param intent Compute intent whose scheduler should be replaced.
   * @param type Scheduler type name.
   * @return Success, `GraphErrc::NotFound` for missing/closed sessions, or
   *         `GraphErrc::InvalidParameter` for unavailable/null scheduler types
   *         or handled candidate failures, or `GraphErrc::ComputeError` when
   *         process capacity cannot reserve candidate headroom.
   * @throws std::bad_alloc if request processing, backend-to-status
   *         translation, or copied result construction exhausts memory.
   * @note Replacement shares graph-state serialization with compute,
   *       scheduler inspection, and close. Session lifecycle failures are
   *       reported before scheduler type validation. Planning and reservation
   *       retain the old scheduler in its pre-call state until candidate
   *       preparation and publication succeed, so exhaustion never destroys an
   *       owner still usable by later compute. Candidate/plugin GraphError
   *       retains the established handled InvalidParameter mapping; only the
   *       new budget category is preserved exactly.
   */
  virtual VoidResult replace_scheduler(const GraphSessionId& session,
                                       ComputeIntent intent,
                                       const std::string& type) = 0;
};

/**
 * @brief Creates an embedded in-process Host adapter.
 *
 * @return Unique Host implementation backed by the local embedded backend
 *         stack.
 * @throws std::bad_alloc if allocation of adapter state fails.
 * @note graph_cli uses this Host-backed embedded path for local operation.
 *       The adapter preserves in-process behavior while keeping the CLI
 *       boundary shared with the installed IPC-backed Host implementation. Each
 *       adapter owns its graph backend state but shares the process operation
 *       plugin owner; adapter destruction never performs plugin unload.
 */
PHOTOSPIDER_API std::unique_ptr<Host> create_embedded_host();

}  // namespace ps
