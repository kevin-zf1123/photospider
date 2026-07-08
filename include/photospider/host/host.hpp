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

/**
 * @file host.hpp
 * @brief Frontend-facing Photospider Host interface and embedded adapter
 * factory.
 *
 * Host is the stable local frontend boundary. Implementations translate these
 * request and snapshot values into an embedded backend stack or a future IPC
 * transport without exposing backend runtime, model, execution, compute,
 * scheduler-queue, image-library, or parser object ownership through
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
 *       operation. The Host owns plugin handles until unload.
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
 * @brief Frontend-facing Photospider graph host.
 *
 * Host is the narrow API that local GUI/WebUI code and future IPC adapters
 * should call. Each method either returns a copied value snapshot or a
 * non-throwing status. Implementations may use embedded backend services
 * internally, but those implementation objects never appear in the public ABI.
 *
 * @throws Nothing from the destructor.
 * @note Methods are not specified as thread-safe at the Host interface level.
 *       The embedded adapter delegates graph-state serialization to the same
 *       backend boundary used by the existing CLI path.
 */
class PHOTOSPIDER_API Host {
 public:
  /** @brief Destroys the Host implementation. */
  virtual ~Host() = default;

  /**
   * @brief Loads a graph session.
   *
   * @param request Session name, root, YAML, config, and cache-root values.
   * @return Loaded session id on success, or a failure status.
   * @throws Nothing directly; implementations convert backend failures to
   *         OperationStatus.
   * @note The returned session is a value label, never a runtime pointer.
   */
  virtual Result<GraphSessionId> load_graph(
      const GraphLoadRequest& request) = 0;

  /**
   * @brief Closes a loaded graph session.
   *
   * @param session Session to close.
   * @return Success or failure status.
   * @throws Nothing directly.
   * @note Closing releases backend resources owned by the implementation.
   */
  virtual VoidResult close_graph(const GraphSessionId& session) = 0;

  /**
   * @brief Lists graph sessions known to the Host.
   *
   * @return Value list of session ids.
   * @throws Nothing directly.
   * @note Session ids are copied labels and do not imply the caller owns
   *       backend graph resources.
   */
  virtual Result<std::vector<GraphSessionId>> list_graphs() const = 0;

  /**
   * @brief Reloads one graph session from a YAML file.
   *
   * @param session Session to reload.
   * @param yaml_path YAML path to load.
   * @return Success, `GraphErrc::NotFound` for missing sessions, or a reload
   *         failure status for rejected YAML.
   * @throws Nothing directly.
   * @note The embedded adapter checks session existence before invoking the
   *       backend reload path and serializes mutation through the backend
   *       graph-state boundary.
   */
  virtual VoidResult reload_graph(const GraphSessionId& session,
                                  const std::string& yaml_path) = 0;

  /**
   * @brief Saves one graph session to a YAML file.
   *
   * @param session Session to save.
   * @param yaml_path Destination YAML path.
   * @return Success or failure status.
   * @throws Nothing directly.
   * @note The Host returns only status; file ownership remains with the
   *       caller-provided path.
   */
  virtual VoidResult save_graph(const GraphSessionId& session,
                                const std::string& yaml_path) = 0;

  /**
   * @brief Clears graph model state for a loaded session.
   *
   * @param session Session to clear.
   * @return Success or failure status.
   * @throws Nothing directly.
   * @note This mirrors the existing clear-graph frontend behavior.
   */
  virtual VoidResult clear_graph(const GraphSessionId& session) = 0;

  /**
   * @brief Computes one node synchronously.
   *
   * @param request Host compute request.
   * @return Success, NotFound when the graph session is missing or closed, or
   *         a compute failure status for existing sessions.
   * @throws Nothing directly.
   * @note Backend LastError diagnostics are used only after the Host has
   *       established that the requested graph session exists.
   */
  virtual VoidResult compute(const HostComputeRequest& request) = 0;

  /**
   * @brief Schedules one node compute asynchronously.
   *
   * @param request Host compute request captured by value.
   * @return Future resolving to the final operation status, or a failure status
   *         when scheduling cannot start.
   * @throws Nothing directly except allocation failure while building the
   *         result/future wrapper.
   * @note The embedded adapter keeps its backend state alive until the returned
   *       future completes and waits for Host-submitted async work before graph
   *       close.
   */
  virtual Result<std::future<OperationStatus>> compute_async(
      HostComputeRequest request) = 0;

  /**
   * @brief Computes one node and returns an image snapshot descriptor.
   *
   * @param request Host compute request.
   * @return ImageBuffer descriptor on success, or a failure status.
   * @throws Nothing directly.
   * @note The embedded adapter clones the backend image before wrapping it in
   *       the returned ImageBuffer descriptor.
   */
  virtual Result<ImageBuffer> compute_and_get_image(
      const HostComputeRequest& request) = 0;

  /**
   * @brief Returns the latest timing snapshot for a session.
   *
   * @param session Session to inspect.
   * @return Copied timing rows and total, or a failure status.
   * @throws Nothing directly.
   * @note Timing data is populated only when a compute request enabled timing.
   */
  virtual Result<TimingSnapshot> timing(const GraphSessionId& session) = 0;

  /**
   * @brief Returns the last backend error recorded for a session.
   *
   * @param session Session to inspect.
   * @return Error status snapshot. When no error exists, status is ok.
   * @throws Nothing directly.
   * @note This is diagnostic state and should not be used for synchronization.
   */
  virtual OperationStatus last_error(const GraphSessionId& session) const = 0;

  /**
   * @brief Lists node ids for a session.
   *
   * @param session Session to inspect.
   * @return Copied node id list, or a failure status.
   * @throws Nothing directly.
   * @note The ids are a snapshot and can become stale after graph reload/edit.
   */
  virtual Result<std::vector<NodeId>> list_node_ids(
      const GraphSessionId& session) = 0;

  /**
   * @brief Lists ending node ids for a session.
   *
   * @param session Session to inspect.
   * @return Copied ending node ids, or a failure status.
   * @throws Nothing directly.
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
   * @throws Nothing directly.
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
   * @return Success or failure status.
   * @throws Nothing directly.
   * @note The backend owns validation and topology rebuilding.
   */
  virtual VoidResult set_node_yaml(const GraphSessionId& session, NodeId node,
                                   const std::string& yaml_text) = 0;

  /**
   * @brief Inspects one node.
   *
   * @param session Session containing the node.
   * @param node Node to inspect.
   * @return Copied node inspection view, or a failure status.
   * @throws Nothing directly.
   * @note The result contains only public value snapshots.
   */
  virtual Result<NodeInspectionView> inspect_node(const GraphSessionId& session,
                                                  NodeId node) = 0;

  /**
   * @brief Inspects all nodes in one graph session.
   *
   * @param session Session to inspect.
   * @return Copied graph inspection view, or a failure status.
   * @throws Nothing directly.
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
   * @throws Nothing directly.
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
   * @throws Nothing directly.
   * @note Keys and values are plain node ids.
   */
  virtual Result<std::map<int, std::vector<NodeId>>> traversal_orders(
      const GraphSessionId& session) = 0;

  /**
   * @brief Returns traversal metadata keyed by ending node id.
   *
   * @param session Session to inspect.
   * @return Copied traversal node metadata, or a failure status.
   * @throws Nothing directly.
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
   * @throws Nothing directly.
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
   * @return Projected ROI, or a failure status.
   * @throws Nothing directly.
   * @note Pixel rectangles are public value copies.
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
   * @return Projected ROI, or a failure status.
   * @throws Nothing directly.
   * @note This mirrors the current ROI projection facade.
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
   * @throws Nothing directly.
   * @note Snapshot values exclude scheduler queues and task counters.
   */
  virtual Result<DirtyRegionInspectionSnapshot> dirty_region_snapshot(
      const GraphSessionId& session) = 0;

  /**
   * @brief Begins a dirty source lifecycle event.
   *
   * @param session Session containing the graph.
   * @param node Source node.
   * @param domain Dirty domain.
   * @param source_roi Source-local dirty ROI.
   * @return Updated dirty-region snapshot, or a failure status.
   * @throws Nothing directly.
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
   * @throws Nothing directly.
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
   * @throws Nothing directly.
   * @note Ending the source marks it settled for the current generation.
   */
  virtual Result<DirtyRegionInspectionSnapshot> end_dirty_source(
      const GraphSessionId& session, NodeId node, DirtyDomain domain) = 0;

  /**
   * @brief Drains compute progress events for a session.
   *
   * @param session Session whose events should be drained.
   * @return Copied events, or a failure status.
   * @throws Nothing directly.
   * @note Draining removes the events from the backend event buffer.
   */
  virtual Result<std::vector<ComputeEventSnapshot>> drain_compute_events(
      const GraphSessionId& session) = 0;

  /**
   * @brief Returns copied scheduler trace events for a session.
   *
   * @param session Session to inspect.
   * @return Scheduler trace snapshot, or a failure status.
   * @throws Nothing directly.
   * @note Trace events are diagnostic and may be cleared by future backend
   *       controls outside this interface.
   */
  virtual Result<std::vector<SchedulerTraceEventSnapshot>> scheduler_trace(
      const GraphSessionId& session) = 0;

  /**
   * @brief Clears all cache layers for a session.
   *
   * @param session Session whose caches should be cleared.
   * @return Success or failure status.
   * @throws Nothing directly.
   * @note This mirrors the existing `clear-cache all` behavior.
   */
  virtual VoidResult clear_cache(const GraphSessionId& session) = 0;

  /**
   * @brief Clears disk cache for a session.
   *
   * @param session Session whose disk cache should be cleared.
   * @return Success or failure status.
   * @throws Nothing directly.
   * @note The first Host slice returns status only; detailed cache counts can
   *       be added as a follow-up value contract.
   */
  virtual VoidResult clear_drive_cache(const GraphSessionId& session) = 0;

  /**
   * @brief Clears memory cache for a session.
   *
   * @param session Session whose memory cache should be cleared.
   * @return Success or failure status.
   * @throws Nothing directly.
   * @note This does not clear graph topology.
   */
  virtual VoidResult clear_memory_cache(const GraphSessionId& session) = 0;

  /**
   * @brief Saves all node caches for a session at one precision.
   *
   * @param session Session whose nodes should be cached.
   * @param precision Cache precision label.
   * @return Success or failure status.
   * @throws Nothing directly.
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
   * @throws Nothing directly.
   * @note Persistent graph and disk-cache state remain owned by the backend.
   */
  virtual VoidResult free_transient_memory(const GraphSessionId& session) = 0;

  /**
   * @brief Synchronizes disk cache for a session at one precision.
   *
   * @param session Session whose cache should be synchronized.
   * @param precision Cache precision label.
   * @return Success or failure status.
   * @throws Nothing directly.
   * @note The first Host slice returns status only.
   */
  virtual VoidResult synchronize_disk_cache(const GraphSessionId& session,
                                            const std::string& precision) = 0;

  /**
   * @brief Loads operation plugins from directories.
   *
   * @param dirs Directories or path patterns to scan.
   * @return Structured plugin load report.
   * @throws Nothing directly.
   * @note The Host owns successful plugin handles until unload.
   */
  virtual Result<HostPluginLoadReport> plugins_load_report(
      const std::vector<std::string>& dirs) = 0;

  /**
   * @brief Loads operation plugins and returns status only.
   *
   * @param dirs Directories or path patterns to scan.
   * @return Success or failure status.
   * @throws Nothing directly.
   * @note This mirrors the older status-only plugin helper.
   */
  virtual VoidResult plugins_load(const std::vector<std::string>& dirs) = 0;

  /**
   * @brief Unloads operation plugins owned by the Host.
   *
   * @return Number of plugin handles unloaded, or a failure status.
   * @throws Nothing directly.
   * @note Built-in operation registrations remain available.
   */
  virtual Result<int> plugins_unload_all() = 0;

  /**
   * @brief Seeds built-in operation sources into plugin inspection state.
   *
   * @return Success status.
   * @throws Nothing directly.
   * @note This is useful for frontend operation browsers that want built-in
   *       source labels without loading external plugins.
   */
  virtual VoidResult seed_builtin_ops() = 0;

  /**
   * @brief Returns operation source labels keyed by operation key.
   *
   * @return Copied operation source map.
   * @throws Nothing directly.
   * @note Source labels are diagnostic strings such as paths or `built-in`.
   */
  virtual Result<std::map<std::string, std::string>> ops_sources() const = 0;

  /**
   * @brief Returns combined operation keys for frontend operation lists.
   *
   * @return Copied operation keys.
   * @throws Nothing directly.
   * @note Combined keys collapse compatible HP/RT/tiled implementations.
   */
  virtual Result<std::vector<std::string>> ops_combined_keys() const = 0;

  /**
   * @brief Returns combined operation source labels.
   *
   * @return Copied combined operation source map.
   * @throws Nothing directly.
   * @note This mirrors the existing frontend operation-source helper.
   */
  virtual Result<std::map<std::string, std::string>> ops_combined_sources()
      const = 0;

  /**
   * @brief Lists scheduler types available to the Host.
   *
   * @return Copied scheduler type names.
   * @throws Nothing directly.
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
   * @throws Nothing directly.
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
   * @throws Nothing directly.
   * @note Loaded scheduler plugin handles remain owned by the backend loader.
   */
  virtual Result<size_t> scheduler_scan(
      const std::vector<std::string>& dirs) = 0;

  /**
   * @brief Loads one scheduler plugin.
   *
   * @param path Plugin library path.
   * @return Success or failure status.
   * @throws Nothing directly.
   * @note Loading registers scheduler factory entries for later graph use.
   */
  virtual VoidResult scheduler_load(const std::string& path) = 0;

  /**
   * @brief Lists loaded scheduler plugin labels.
   *
   * @return Copied plugin labels.
   * @throws Nothing directly.
   * @note Labels are diagnostic strings, not plugin handles.
   */
  virtual Result<std::vector<std::string>> scheduler_loaded_plugins() const = 0;

  /**
   * @brief Returns scheduler information for a graph intent.
   *
   * @param session Session to inspect.
   * @param intent Compute intent served by the scheduler.
   * @return Copied scheduler info, or a failure status.
   * @throws Nothing directly.
   * @note The Host returns value text instead of exposing scheduler objects.
   */
  virtual Result<SchedulerInfoSnapshot> scheduler_info(
      const GraphSessionId& session, ComputeIntent intent) const = 0;

  /**
   * @brief Replaces the scheduler for a graph intent.
   *
   * @param session Session to update.
   * @param intent Compute intent whose scheduler should be replaced.
   * @param type Scheduler type name.
   * @return Success or failure status.
   * @throws Nothing directly.
   * @note Replacement preserves backend lifecycle ordering and returns only a
   *       status snapshot to the caller.
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
 * @note The adapter keeps existing CLI behavior intact because CLI code still
 *       calls its existing backend facade directly; the Host is an additional
 *       frontend seam for local embedding and future IPC parity.
 */
PHOTOSPIDER_API std::unique_ptr<Host> create_embedded_host();

}  // namespace ps
