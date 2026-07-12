#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "photospider/core/export.hpp"
#include "photospider/host/graph_session.hpp"
#include "photospider/host/host.hpp"
#include "photospider/ipc/protocol.hpp"

/**
 * @file client.hpp
 * @brief Move-only typed client for Photospider local IPC protocol version 1.
 *
 * The class hides Unix connection ownership and private JSON envelopes behind
 * typed calls for the exact 55-method version 1 surface. Collection methods
 * aggregate stable daemon cursor pages before publishing their owned values;
 * no raw JSON or cursor-storage type enters the public API.
 */

namespace ps::ipc {

/**
 * @brief Move-only client for one sequential Unix IPC connection.
 *
 * One instance permits one outstanding operation at a time and is not
 * concurrently callable. Independent instances may be used concurrently.
 * Recoverable transport, protocol, graph, and daemon failures are returned as
 * typed statuses and are never automatically retried.
 *
 * @throws std::bad_alloc when connection metadata, requests, responses, or
 *         copied public values exhaust memory.
 * @note Destruction and `disconnect()` release the private descriptor exactly
 *       once. Disconnecting never closes daemon-owned graph sessions.
 */
class PHOTOSPIDER_API Client {
 public:
  /**
   * @brief Creates a disconnected client.
   *
   * @throws std::bad_alloc if private state allocation fails.
   * @note Call `connect()` before issuing a typed request.
   */
  Client();

  /**
   * @brief Releases the private connection when present.
   *
   * @throws Nothing.
   */
  ~Client();

  /**
   * @brief Prevents sharing one private connection between client objects.
   *
   * @throws Nothing because this operation is unavailable.
   * @note Create an independent `Client` when concurrent connections are
   *       required.
   */
  Client(const Client&) = delete;

  /**
   * @brief Prevents copying private connection ownership by assignment.
   *
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   * @note Move assignment is the only supported ownership transfer.
   */
  Client& operator=(const Client&) = delete;

  /**
   * @brief Transfers private connection ownership from another client.
   *
   * @param other Client whose descriptor and request sequence are transferred.
   * @throws Nothing.
   * @note The moved-from client becomes safely disconnected.
   */
  Client(Client&& other) noexcept;

  /**
   * @brief Replaces this connection with ownership from another client.
   *
   * @param other Client whose descriptor and request sequence are transferred.
   * @return This client after the transfer.
   * @throws Nothing.
   * @note Any previous descriptor owned by this object is released first; the
   *       moved-from client becomes safely disconnected.
   */
  Client& operator=(Client&& other) noexcept;

  /**
   * @brief Connects to one explicit Unix domain socket path.
   *
   * @param socket_path Absolute daemon socket path.
   * @return Success, or a local transport status with copied diagnostics.
   * @throws std::bad_alloc if private path or diagnostic storage is exhausted.
   * @note An existing connection is closed first. The operation does not
   *       discover, start, retry, or authenticate a daemon.
   */
  OperationStatus connect(const std::string& socket_path);

  /**
   * @brief Closes this client's transport connection idempotently.
   *
   * @throws Nothing.
   * @note Active daemon graph sessions remain owned by the daemon.
   */
  void disconnect() noexcept;

  /**
   * @brief Reports whether this object currently owns a connection.
   *
   * @return True while a private descriptor is owned.
   * @throws Nothing.
   * @note Peer failure may only become visible on the next typed call.
   */
  bool connected() const noexcept;

  /**
   * @brief Calls `daemon.ping`.
   *
   * @return Typed liveness metadata or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note This call has no Host dependency on the daemon.
   */
  IpcResult<DaemonPing> ping();

  /**
   * @brief Calls `daemon.version`.
   *
   * @return Typed service/version/method metadata or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note The response is correlated and validated against the exact sorted
   *       55-method version 1 inventory before publication.
   */
  IpcResult<DaemonVersion> version();

  /**
   * @brief Calls `graph.load` with preserved Host path semantics.
   *
   * @param request Caller session name and absolute graph paths.
   * @return Opaque session summary or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note The client does not retry this mutating request automatically.
   */
  IpcResult<GraphSessionSummary> load_graph(const GraphLoadRequest& request);

  /**
   * @brief Calls `graph.close` for an opaque session.
   *
   * @param session_id Opaque daemon session identifier.
   * @return Success or a categorized failure.
   * @throws std::bad_alloc if request or status allocation fails.
   * @note This is a daemon graph-lifecycle operation, distinct from
   *       `disconnect()`.
   */
  VoidResult close_graph(const IpcSessionId& session_id);

  /**
   * @brief Calls `graph.list`.
   *
   * @return Deterministically sorted active session summaries or a failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note Returned values own all strings and parser storage is discarded.
   */
  IpcResult<std::vector<GraphSessionSummary>> list_graphs();

  /**
   * @brief Calls `inspect.graph` for an opaque session.
   *
   * @param session_id Opaque daemon session identifier.
   * @return Copied public graph snapshot or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note The returned snapshot session value contains the opaque id.
   */
  IpcResult<GraphInspectionView> inspect_graph(const IpcSessionId& session_id);

  /**
   * @brief Calls `inspect.node` for one graph node.
   *
   * @param session_id Opaque daemon session identifier.
   * @param node Node identifier copied into the request.
   * @return Copied public node snapshot or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note Missing sessions or nodes preserve the remote graph error category.
   */
  IpcResult<NodeInspectionView> inspect_node(const IpcSessionId& session_id,
                                             NodeId node);

  /**
   * @brief Calls `inspect.dependency_tree` for graph or node scope.
   *
   * @param session_id Opaque daemon session identifier.
   * @param node Optional start node; absence selects graph ending nodes.
   * @param include_metadata Whether cache/spatial node metadata is requested.
   * @return Copied flattened dependency tree or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note Optional values and non-finite doubles use the version 1 null policy.
   */
  IpcResult<HostDependencyTreeSnapshot> inspect_dependency_tree(
      const IpcSessionId& session_id, std::optional<NodeId> node = std::nullopt,
      bool include_metadata = false);

  /**
   * @brief Calls `graph.reload` once for an opaque session.
   * @param session_id Opaque daemon session identifier.
   * @param yaml_path Absolute graph YAML path.
   * @return Success or the exact categorized failure.
   * @throws std::bad_alloc if request, path, or status allocation fails.
   * @note This mutating call is never automatically retried.
   */
  VoidResult reload_graph(const IpcSessionId& session_id,
                          const std::string& yaml_path);

  /**
   * @brief Calls `graph.save` once for an opaque session.
   * @param session_id Opaque daemon session identifier.
   * @param yaml_path Absolute destination YAML path.
   * @return Success or the exact categorized failure.
   * @throws std::bad_alloc if request, path, or status allocation fails.
   * @note This mutating call is never automatically retried.
   */
  VoidResult save_graph(const IpcSessionId& session_id,
                        const std::string& yaml_path);

  /**
   * @brief Calls `graph.clear` once for an opaque session.
   * @param session_id Opaque daemon session identifier.
   * @return Success or the exact categorized failure.
   * @throws std::bad_alloc if request or status allocation fails.
   * @note This mutating call is never automatically retried.
   */
  VoidResult clear_graph(const IpcSessionId& session_id);

  /**
   * @brief Calls `graph.node_yaml.get` for one node.
   * @param session_id Opaque daemon session identifier.
   * @param node Nonnegative graph node identifier.
   * @return Owned YAML text or the exact categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note The response must echo both opaque session and node identities.
   */
  IpcResult<std::string> get_node_yaml(const IpcSessionId& session_id,
                                       NodeId node);

  /**
   * @brief Calls `graph.node_yaml.set` once for one node.
   * @param session_id Opaque daemon session identifier.
   * @param node Nonnegative graph node identifier.
   * @param yaml_text Complete replacement node YAML text.
   * @return Success or the exact categorized failure.
   * @throws std::bad_alloc if request, YAML, or status allocation fails.
   * @note This mutating call is never automatically retried.
   */
  VoidResult set_node_yaml(const IpcSessionId& session_id, NodeId node,
                           const std::string& yaml_text);

  /**
   * @brief Calls `inspect.node_ids` and aggregates every stable page.
   * @param session_id Opaque daemon session identifier.
   * @return Complete ordered node-id snapshot or a categorized failure.
   * @throws std::bad_alloc if request, page, or aggregate allocation fails.
   * @note Continuations reuse one frozen daemon snapshot and exact offsets.
   */
  IpcResult<std::vector<NodeId>> list_node_ids(const IpcSessionId& session_id);

  /**
   * @brief Calls `inspect.ending_nodes` and aggregates every stable page.
   * @param session_id Opaque daemon session identifier.
   * @return Complete ordered ending-node snapshot or a categorized failure.
   * @throws std::bad_alloc if request, page, or aggregate allocation fails.
   * @note Duplicate Host values and order are preserved across pages.
   */
  IpcResult<std::vector<NodeId>> ending_nodes(const IpcSessionId& session_id);

  /**
   * @brief Calls `inspect.traversal_orders` and aggregates stable map rows.
   * @param session_id Opaque daemon session identifier.
   * @return Complete ending-node keyed traversal map or a failure.
   * @throws std::bad_alloc if request, page, or map allocation fails.
   * @note Duplicate map keys in a malformed response are rejected rather than
   *       silently overwritten.
   */
  IpcResult<std::map<int, std::vector<NodeId>>> traversal_orders(
      const IpcSessionId& session_id);

  /**
   * @brief Calls `inspect.traversal_details` and aggregates stable map rows.
   * @param session_id Opaque daemon session identifier.
   * @return Complete traversal metadata map or a categorized failure.
   * @throws std::bad_alloc if request, page, string, or map allocation fails.
   * @note Every nested id, cache flag, name, and page offset is validated.
   */
  IpcResult<std::map<int, std::vector<HostTraversalNodeSnapshot>>>
  traversal_details(const IpcSessionId& session_id);

  /**
   * @brief Calls `inspect.trees_containing_node` and aggregates all pages.
   * @param session_id Opaque daemon session identifier.
   * @param node Nonnegative node to locate in traversal trees.
   * @return Complete ordered ending-node list or a categorized failure.
   * @throws std::bad_alloc if request, page, or aggregate allocation fails.
   * @note The original node parameter is repeated on every continuation.
   */
  IpcResult<std::vector<NodeId>> trees_containing_node(
      const IpcSessionId& session_id, NodeId node);

  /**
   * @brief Calls `inspect.roi_forward` for one source-to-target projection.
   * @param session_id Opaque daemon session identifier.
   * @param start_node Source graph node.
   * @param start_roi Source-local pixel rectangle.
   * @param target_node Target graph node.
   * @return Projected rectangle or the exact categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note The response must echo the requested opaque session identity.
   */
  IpcResult<PixelRect> project_roi(const IpcSessionId& session_id,
                                   NodeId start_node,
                                   const PixelRect& start_roi,
                                   NodeId target_node);

  /**
   * @brief Calls `inspect.roi_backward` for one target-to-source projection.
   * @param session_id Opaque daemon session identifier.
   * @param target_node Target graph node.
   * @param target_roi Target-local pixel rectangle.
   * @param source_node Source graph node.
   * @return Projected rectangle or the exact categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note The response must echo the requested opaque session identity.
   */
  IpcResult<PixelRect> project_roi_backward(const IpcSessionId& session_id,
                                            NodeId target_node,
                                            const PixelRect& target_roi,
                                            NodeId source_node);

  /**
   * @brief Calls `inspect.dirty_region` for one session.
   * @param session_id Opaque daemon session identifier.
   * @return Complete owned dirty-region snapshot or a categorized failure.
   * @throws std::bad_alloc if request or nested result allocation fails.
   * @note Every nested collection is decoded transactionally before publish.
   */
  IpcResult<DirtyRegionInspectionSnapshot> dirty_region_snapshot(
      const IpcSessionId& session_id);

  /**
   * @brief Calls `inspect.compute_planning` for one session.
   * @param session_id Opaque daemon session identifier.
   * @return Optional latest planning snapshot or a categorized failure.
   * @throws std::bad_alloc if request or nested result allocation fails.
   * @note A successful JSON null planning value becomes `std::nullopt`.
   */
  IpcResult<std::optional<ComputePlanningInspectionSnapshot>>
  compute_planning_snapshot(const IpcSessionId& session_id);

  /**
   * @brief Calls `inspect.recent_compute_planning` and aggregates all pages.
   * @param session_id Opaque daemon session identifier.
   * @return Complete ordered planning history or a categorized failure.
   * @throws std::bad_alloc if request, page, or aggregate allocation fails.
   * @note Each planning snapshot is an indivisible validated page row.
   */
  IpcResult<std::vector<ComputePlanningInspectionSnapshot>>
  recent_compute_planning_snapshots(const IpcSessionId& session_id);

  /**
   * @brief Calls `compute.submit` exactly once.
   * @param request Owned opaque-session compute submission.
   * @return Accepted queued job snapshot or a categorized admission failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note The call never reconnects, retries, or resubmits after ambiguity.
   */
  IpcResult<ComputeJobSnapshot> submit_compute(
      const ComputeSubmitRequest& request);

  /**
   * @brief Calls `compute.status` exactly once and non-destructively.
   * @param compute_id Opaque accepted compute-job identifier.
   * @return Current validated job snapshot or a categorized lookup failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note This primitive performs one RPC attempt; polling policy belongs to
   *       the later IPC Host adapter and cannot trigger an implicit retry here.
   */
  IpcResult<ComputeJobSnapshot> compute_status(
      const ComputeRequestId& compute_id);

  /**
   * @brief Calls terminal-only `compute.result` exactly once.
   * @param compute_id Opaque accepted compute-job identifier.
   * @return Terminal job plus optional leased output metadata or a failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note The Client copies metadata only; mapping the artifact is task 4.3.
   */
  IpcResult<ComputeJobSnapshot> compute_result(
      const ComputeRequestId& compute_id);

  /**
   * @brief Calls terminal-only `compute.release` exactly once.
   * @param compute_id Opaque accepted compute-job identifier.
   * @param delivery_id Optional matching artifact delivery lease.
   * @return Echoed release acknowledgement or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note Supplying a lease permits daemon-side atomic job/lease release; the
   *       Client never retries this mutation after transport ambiguity.
   */
  IpcResult<ComputeReleaseResult> release_compute(
      const ComputeRequestId& compute_id,
      std::optional<DeliveryLeaseId> delivery_id = std::nullopt);

  /**
   * @brief Calls `compute.timing` for one session.
   * @param session_id Opaque daemon session identifier.
   * @return Complete timing snapshot or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note JSON null timing numbers are restored as quiet NaN.
   */
  IpcResult<TimingSnapshot> timing(const IpcSessionId& session_id);

  /**
   * @brief Calls `compute.last_io_time` for one session.
   * @param session_id Opaque daemon session identifier.
   * @return Latest milliseconds value or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note JSON null is restored as quiet NaN under the version 1 policy.
   */
  IpcResult<double> last_io_time(const IpcSessionId& session_id);

  /**
   * @brief Calls `compute.last_error` for one session.
   * @param session_id Opaque daemon session identifier.
   * @return Nested observed status or an outer categorized RPC failure.
   * @throws std::bad_alloc if request or status allocation fails.
   * @note The outer `IpcResult::status` describes the RPC; `value` preserves
   *       the Host diagnostic status, including canonical success.
   */
  IpcResult<OperationStatus> last_error(const IpcSessionId& session_id);

  /**
   * @brief Calls `dirty.begin` exactly once.
   * @param session_id Opaque daemon session identifier.
   * @param node Dirty source node.
   * @param domain HP or RT dirty domain.
   * @param source_roi Source-local dirty rectangle.
   * @return Updated owned dirty snapshot or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note The mutation is never automatically retried.
   */
  IpcResult<DirtyRegionInspectionSnapshot> begin_dirty_source(
      const IpcSessionId& session_id, NodeId node, DirtyDomain domain,
      const PixelRect& source_roi);

  /**
   * @brief Calls `dirty.update` exactly once.
   * @param session_id Opaque daemon session identifier.
   * @param node Dirty source node.
   * @param domain HP or RT dirty domain.
   * @param source_roi Source-local dirty rectangle.
   * @return Updated owned dirty snapshot or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note The mutation is never automatically retried.
   */
  IpcResult<DirtyRegionInspectionSnapshot> update_dirty_source(
      const IpcSessionId& session_id, NodeId node, DirtyDomain domain,
      const PixelRect& source_roi);

  /**
   * @brief Calls `dirty.end` exactly once.
   * @param session_id Opaque daemon session identifier.
   * @param node Dirty source node.
   * @param domain HP or RT dirty domain.
   * @return Updated owned dirty snapshot or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note The mutation is never automatically retried.
   */
  IpcResult<DirtyRegionInspectionSnapshot> end_dirty_source(
      const IpcSessionId& session_id, NodeId node, DirtyDomain domain);

  /**
   * @brief Calls bounded destructive `events.drain` exactly once.
   * @param session_id Opaque daemon session identifier.
   * @param limit Maximum events to remove in the Host-defined valid range.
   * @return Sequenced event batch or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note Result validation enforces limit, ordering, drop, and next-sequence
   *       invariants without issuing a second destructive call.
   */
  IpcResult<ComputeEventBatch> drain_compute_events(
      const IpcSessionId& session_id, std::size_t limit);

  /**
   * @brief Calls `cache.clear_all` exactly once.
   * @param session_id Opaque daemon session identifier.
   * @return Success or the exact categorized failure.
   * @throws std::bad_alloc if request or status allocation fails.
   * @note This mutation is never automatically retried.
   */
  VoidResult clear_cache(const IpcSessionId& session_id);

  /**
   * @brief Calls `cache.clear_drive` exactly once.
   * @param session_id Opaque daemon session identifier.
   * @return Success or the exact categorized failure.
   * @throws std::bad_alloc if request or status allocation fails.
   * @note This mutation is never automatically retried.
   */
  VoidResult clear_drive_cache(const IpcSessionId& session_id);

  /**
   * @brief Calls `cache.clear_memory` exactly once.
   * @param session_id Opaque daemon session identifier.
   * @return Success or the exact categorized failure.
   * @throws std::bad_alloc if request or status allocation fails.
   * @note This mutation is never automatically retried.
   */
  VoidResult clear_memory_cache(const IpcSessionId& session_id);

  /**
   * @brief Calls `cache.cache_all_nodes` exactly once.
   * @param session_id Opaque daemon session identifier.
   * @param precision Cache precision label passed unchanged.
   * @return Success or the exact categorized failure.
   * @throws std::bad_alloc if request or status allocation fails.
   * @note This mutation is never automatically retried.
   */
  VoidResult cache_all_nodes(const IpcSessionId& session_id,
                             const std::string& precision);

  /**
   * @brief Calls `cache.free_transient` exactly once.
   * @param session_id Opaque daemon session identifier.
   * @return Success or the exact categorized failure.
   * @throws std::bad_alloc if request or status allocation fails.
   * @note This mutation is never automatically retried.
   */
  VoidResult free_transient_memory(const IpcSessionId& session_id);

  /**
   * @brief Calls `cache.synchronize_disk` exactly once.
   * @param session_id Opaque daemon session identifier.
   * @param precision Cache precision label passed unchanged.
   * @return Success or the exact categorized failure.
   * @throws std::bad_alloc if request or status allocation fails.
   * @note This mutation is never automatically retried.
   */
  VoidResult synchronize_disk_cache(const IpcSessionId& session_id,
                                    const std::string& precision);

  /**
   * @brief Calls `plugins.load_report` exactly once.
   * @param directories Plugin directories or path patterns.
   * @return Complete owned Host load report or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note Successful process-owned plugin state outlives this Client; the
   *       mutation is never automatically retried.
   */
  IpcResult<HostPluginLoadReport> plugins_load_report(
      const std::vector<std::string>& directories);

  /**
   * @brief Calls `plugins.unload_all` exactly once.
   * @return Removed/restored operation-key count or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note This process-global mutation is never automatically retried.
   */
  IpcResult<int> plugins_unload_all();

  /**
   * @brief Calls `plugins.seed_builtins` exactly once.
   * @return Success or the exact categorized failure.
   * @throws std::bad_alloc if request or status allocation fails.
   * @note This process-global mutation is never automatically retried.
   */
  VoidResult seed_builtin_ops();

  /**
   * @brief Calls `plugins.ops_sources` and aggregates all sorted pages.
   * @return Complete operation-key/source map or a categorized failure.
   * @throws std::bad_alloc if request, page, or map allocation fails.
   * @note Duplicate keys in a malformed response are rejected.
   */
  IpcResult<std::map<std::string, std::string>> ops_sources();

  /**
   * @brief Calls `plugins.ops_combined_keys` and aggregates all sorted pages.
   * @return Complete combined operation-key list or a categorized failure.
   * @throws std::bad_alloc if request, page, or aggregate allocation fails.
   * @note Strict lexical ordering is validated across page boundaries.
   */
  IpcResult<std::vector<std::string>> ops_combined_keys();

  /**
   * @brief Calls `plugins.ops_combined_sources` and aggregates sorted pages.
   * @return Complete combined operation source map or a categorized failure.
   * @throws std::bad_alloc if request, page, or map allocation fails.
   * @note Duplicate keys in a malformed response are rejected.
   */
  IpcResult<std::map<std::string, std::string>> ops_combined_sources();

  /**
   * @brief Calls `scheduler.types` and aggregates every sorted stable page.
   * @return Complete available scheduler type list or a categorized failure.
   * @throws std::bad_alloc if request, page, or aggregate allocation fails.
   * @note Strict lexical ordering is validated across page boundaries.
   */
  IpcResult<std::vector<std::string>> scheduler_available_types();

  /**
   * @brief Calls `scheduler.description` for one type.
   * @param type_name Scheduler type name.
   * @return Owned description or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note The response must echo the exact requested type name.
   */
  IpcResult<std::string> scheduler_description(const std::string& type_name);

  /**
   * @brief Calls `scheduler.scan` exactly once.
   * @param directories Scheduler plugin directories to scan.
   * @return Exact loaded scheduler-type count or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note This process-global mutation is never automatically retried.
   */
  IpcResult<std::size_t> scheduler_scan(
      const std::vector<std::string>& directories);

  /**
   * @brief Calls `scheduler.load` exactly once.
   * @param path Scheduler plugin library path.
   * @return Success or the exact categorized failure.
   * @throws std::bad_alloc if request or status allocation fails.
   * @note This process-global mutation is never automatically retried.
   */
  VoidResult scheduler_load(const std::string& path);

  /**
   * @brief Calls `scheduler.loaded_plugins` and aggregates sorted pages.
   * @return Complete loaded plugin-label list or a categorized failure.
   * @throws std::bad_alloc if request, page, or aggregate allocation fails.
   * @note Strict lexical ordering is validated across page boundaries.
   */
  IpcResult<std::vector<std::string>> scheduler_loaded_plugins();

  /**
   * @brief Calls `scheduler.configure_defaults` exactly once.
   * @param config Scheduler type labels and exact worker count.
   * @return Success or the exact categorized failure.
   * @throws std::bad_alloc if request or status allocation fails.
   * @note Existing sessions are not replaced and this mutation is not retried.
   */
  VoidResult configure_scheduler_defaults(const HostSchedulerConfig& config);

  /**
   * @brief Calls `scheduler.info` for one session and compute intent.
   * @param session_id Opaque daemon session identifier.
   * @param intent Scheduler intent to inspect.
   * @return Complete owned scheduler information or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note The response must echo both requested session and intent.
   */
  IpcResult<SchedulerInfoSnapshot> scheduler_info(
      const IpcSessionId& session_id, ComputeIntent intent);

  /**
   * @brief Calls `scheduler.replace` exactly once.
   * @param session_id Opaque daemon session identifier.
   * @param intent Scheduler intent whose owner is replaced.
   * @param type Scheduler type name.
   * @return Success or the exact categorized failure.
   * @throws std::bad_alloc if request or status allocation fails.
   * @note This mutation is never automatically retried.
   */
  VoidResult replace_scheduler(const IpcSessionId& session_id,
                               ComputeIntent intent, const std::string& type);

  /**
   * @brief Calls bounded non-destructive `scheduler.trace` exactly once.
   * @param session_id Opaque daemon session identifier.
   * @param after_sequence Exclusive observation sequence cursor.
   * @param limit Maximum trace events in the Host-defined valid range.
   * @return Validated scheduler trace page or a categorized failure.
   * @throws std::bad_alloc if request or result allocation fails.
   * @note This is one status-like observation RPC and is never retried.
   */
  IpcResult<SchedulerTracePage> scheduler_trace(const IpcSessionId& session_id,
                                                std::uint64_t after_sequence,
                                                std::size_t limit);

 private:
  /** @brief Private transport/envelope implementation. */
  class Impl;

  /** @brief Sole owner of the private implementation and Unix descriptor. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps::ipc
