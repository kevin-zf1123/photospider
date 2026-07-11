#pragma once

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
 * typed graph and inspection calls. It does not implement the full `ps::Host`
 * surface; compute and other methods remain assigned to issue #37.
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
   * @note The response is correlated and validated before publication.
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

 private:
  /** @brief Private transport/envelope implementation. */
  class Impl;

  /** @brief Sole owner of the private implementation and Unix descriptor. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps::ipc
