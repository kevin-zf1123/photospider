#include "photospider/ipc/client.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ipc/codec.hpp"
#include "ipc/frame.hpp"
#include "ipc/unix_socket.hpp"

namespace ps::ipc {
namespace {

/**
 * @brief Internal correlated-call result before typed value decoding.
 *
 * @throws std::bad_alloc when status or JSON storage allocation fails.
 * @note `result` is authoritative only when `status.ok` is true.
 */
struct RawCallResult {
  /** @brief Transport/protocol/remote completion status. */
  IpcStatus status;

  /** @brief Owned successful result object. */
  internal::Json result;
};

/**
 * @brief Builds a local invalid-response status.
 *
 * @param message Human-readable shape/correlation diagnostic.
 * @return Protocol-domain invalid-request failure.
 * @throws std::bad_alloc if diagnostic allocation fails.
 */
IpcStatus invalid_response(std::string message) {
  return internal::failure_status(IpcErrorDomain::Protocol,
                                  internal::kInvalidRequestCode,
                                  "invalid_request", std::move(message));
}

/**
 * @brief Validates a daemon-generated opaque identifier.
 *
 * @param value Candidate session or server instance identifier.
 * @return True for exactly 32 lowercase hexadecimal characters.
 * @throws Nothing.
 * @note This shared result validator prevents typed metadata and lifecycle
 *       calls from publishing names, uppercase tokens, or malformed wire
 *       identifiers as opaque public values.
 */
bool valid_opaque_id(const std::string& value) noexcept {
  return value.size() == 32 &&
         std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

/**
 * @brief Creates a failed typed result while preserving its status.
 *
 * @tparam Value Default-constructible public result type.
 * @param status Failure status to move into the result.
 * @return Failed typed result with default payload.
 * @throws Whatever moving `IpcStatus` or constructing `Value` throws.
 */
template <typename Value>
IpcResult<Value> failed_result(IpcStatus status) {
  return {std::move(status), {}};
}

}  // namespace

/**
 * @brief Private connection, sequence, and correlated-envelope implementation.
 *
 * @throws std::bad_alloc when request/response storage cannot be allocated.
 * @note The object is called only by one public `Client` operation at a time.
 */
class Client::Impl {
 public:
  /**
   * @brief Connects this implementation to one explicit socket.
   *
   * @param socket_path Absolute Unix socket path.
   * @return Success or local transport diagnostic.
   * @throws std::bad_alloc if path or diagnostic storage cannot be allocated.
   * @note Any current connection is closed before the one-shot connect.
   */
  IpcStatus connect(const std::string& socket_path) {
    socket_.reset();
    std::string message;
    internal::UniqueFd connected =
        internal::connect_unix_socket(socket_path, &message);
    if (!connected) {
      return internal::failure_status(IpcErrorDomain::Transport, 1,
                                      "connect_failed", std::move(message));
    }
    socket_ = std::move(connected);
    return internal::ok_status();
  }

  /**
   * @brief Closes the private connection once.
   *
   * @throws Nothing.
   */
  void disconnect() noexcept { socket_.reset(); }

  /**
   * @brief Reports whether this implementation owns a descriptor.
   *
   * @return True when connected or until a peer failure is observed.
   * @throws Nothing.
   */
  bool connected() const noexcept { return static_cast<bool>(socket_); }

  /**
   * @brief Sends one request and validates its correlated response envelope.
   *
   * @param method One of the eight version 1 method names.
   * @param params Typed method parameters already encoded as an object.
   * @return Owned result object or categorized local/remote failure.
   * @throws std::bad_alloc if request/response storage cannot be allocated.
   * @note The call performs no retry. Frame, JSON, envelope, correlation, and
   *       error-object protocol violations close the connection because message
   *       synchronization is no longer trustworthy. After this function
   *       returns a correlated result object, a typed payload-shape failure
   *       becomes a local Protocol error and leaves the connection open.
   */
  RawCallResult call(const std::string& method, const internal::Json& params) {
    if (!socket_) {
      return {internal::failure_status(IpcErrorDomain::Transport, 2,
                                       "not_connected",
                                       "IPC client is not connected"),
              {}};
    }
    const std::string id = "client-" + std::to_string(++request_sequence_);
    const internal::Json request{{"protocol_version", kProtocolVersion},
                                 {"id", id},
                                 {"method", method},
                                 {"params", params}};
    std::string payload;
    try {
      payload = request.dump();
    } catch (const internal::Json::type_error& error) {
      return {internal::failure_status(IpcErrorDomain::Protocol,
                                       internal::kInvalidParamsCode,
                                       "invalid_params", error.what()),
              {}};
    }
    if (payload.empty() || payload.size() > kMaximumFramePayloadBytes) {
      return {internal::failure_status(
                  IpcErrorDomain::Protocol, internal::kInvalidParamsCode,
                  "invalid_params",
                  "serialized request exceeds the version 1 frame limit"),
              {}};
    }
    const internal::FrameWriteResult written =
        internal::write_frame(socket_.get(), payload);
    if (!written.ok) {
      socket_.reset();
      return {internal::failure_status(IpcErrorDomain::Transport, 3,
                                       "write_failed", written.message),
              {}};
    }
    const internal::FrameReadResult frame = internal::read_frame(socket_.get());
    if (frame.state != internal::FrameReadState::Complete) {
      socket_.reset();
      if (frame.state == internal::FrameReadState::InvalidLength) {
        return {invalid_response(frame.message), {}};
      }
      const std::string message = frame.message.empty()
                                      ? "daemon closed the IPC connection"
                                      : frame.message;
      return {internal::failure_status(
                  IpcErrorDomain::Transport, 4,
                  frame.state == internal::FrameReadState::Truncated
                      ? "truncated_frame"
                      : "read_failed",
                  message),
              {}};
    }

    const internal::JsonParseResult parsed =
        internal::parse_json(frame.payload);
    if (!parsed.ok) {
      socket_.reset();
      return {internal::failure_status(
                  IpcErrorDomain::Protocol,
                  parsed.duplicate_key ? internal::kInvalidRequestCode
                                       : internal::kParseErrorCode,
                  parsed.duplicate_key ? "invalid_request" : "parse_error",
                  parsed.message),
              {}};
    }
    const internal::Json& response = parsed.value;
    const bool has_result = response.is_object() && response.contains("result");
    const bool has_error = response.is_object() && response.contains("error");
    if (!response.is_object() ||
        !response.value("protocol_version", internal::Json())
             .is_number_integer() ||
        !response.value("id", internal::Json()).is_string() ||
        has_result == has_error ||
        (has_result && !response["result"].is_object()) ||
        (has_error && !response["error"].is_object())) {
      socket_.reset();
      return {invalid_response("response envelope has an invalid shape"), {}};
    }
    std::int32_t response_version = 0;
    if (!internal::decode_integer(response["protocol_version"],
                                  &response_version)) {
      socket_.reset();
      return {invalid_response("response protocol_version is out of range"),
              {}};
    }
    if (response_version != kProtocolVersion ||
        response["id"].get<std::string>() != id) {
      socket_.reset();
      return {invalid_response(
                  "response protocol version or request id does not correlate"),
              {}};
    }
    if (has_error) {
      IpcStatus status;
      std::string message;
      if (!internal::decode_error(response["error"], &status, &message)) {
        socket_.reset();
        return {invalid_response(std::move(message)), {}};
      }
      return {std::move(status), {}};
    }
    return {internal::ok_status(), response["result"]};
  }

 private:
  /** @brief Sole owner of the connected Unix descriptor. */
  internal::UniqueFd socket_;

  /** @brief Monotonic per-client request-id sequence. */
  std::uint64_t request_sequence_ = 0;
};

/** @copydoc Client::Client */
Client::Client() : impl_(std::make_unique<Impl>()) {}

/** @copydoc Client::~Client */
Client::~Client() = default;

/** @copydoc Client::Client(Client&&) */
Client::Client(Client&& other) noexcept = default;

/** @copydoc Client::operator=(Client&&) */
Client& Client::operator=(Client&& other) noexcept = default;

/** @copydoc Client::connect */
IpcStatus Client::connect(const std::string& socket_path) {
  if (!impl_) {
    impl_ = std::make_unique<Impl>();
  }
  return impl_->connect(socket_path);
}

/** @copydoc Client::disconnect */
void Client::disconnect() noexcept {
  if (impl_) {
    impl_->disconnect();
  }
}

/** @copydoc Client::connected */
bool Client::connected() const noexcept {
  return impl_ != nullptr && impl_->connected();
}

/** @copydoc Client::ping */
IpcResult<DaemonPing> Client::ping() {
  if (!impl_) {
    return failed_result<DaemonPing>(
        internal::failure_status(IpcErrorDomain::Transport, 2, "not_connected",
                                 "IPC client is not connected"));
  }
  RawCallResult call = impl_->call("daemon.ping", internal::Json::object());
  if (!call.status.ok) {
    return failed_result<DaemonPing>(std::move(call.status));
  }
  if (!call.result.value("pong", internal::Json()).is_boolean() ||
      !call.result["pong"].get<bool>() ||
      !call.result.value("server_instance_id", internal::Json()).is_string()) {
    return failed_result<DaemonPing>(
        invalid_response("daemon.ping result has an invalid shape"));
  }
  const std::string server_instance_id =
      call.result["server_instance_id"].get<std::string>();
  if (!valid_opaque_id(server_instance_id)) {
    return failed_result<DaemonPing>(
        invalid_response("daemon.ping returned an invalid instance id"));
  }
  return {internal::ok_status(), {true, server_instance_id}};
}

/** @copydoc Client::version */
IpcResult<DaemonVersion> Client::version() {
  if (!impl_) {
    return failed_result<DaemonVersion>(
        internal::failure_status(IpcErrorDomain::Transport, 2, "not_connected",
                                 "IPC client is not connected"));
  }
  RawCallResult call = impl_->call("daemon.version", internal::Json::object());
  if (!call.status.ok) {
    return failed_result<DaemonVersion>(std::move(call.status));
  }
  if (!call.result.value("protocol_version", internal::Json())
           .is_number_integer() ||
      !call.result.value("service_name", internal::Json()).is_string() ||
      !call.result.value("service_version", internal::Json()).is_string() ||
      !call.result.value("server_instance_id", internal::Json()).is_string() ||
      !call.result.value("transport", internal::Json()).is_string() ||
      !call.result.value("methods", internal::Json()).is_array()) {
    return failed_result<DaemonVersion>(
        invalid_response("daemon.version result has an invalid shape"));
  }
  DaemonVersion result;
  if (!internal::decode_integer(call.result["protocol_version"],
                                &result.protocol_version)) {
    return failed_result<DaemonVersion>(
        invalid_response("daemon.version protocol_version is out of range"));
  }
  try {
    result.service_name = call.result["service_name"].get<std::string>();
    result.service_version = call.result["service_version"].get<std::string>();
    result.server_instance_id =
        call.result["server_instance_id"].get<std::string>();
    result.transport = call.result["transport"].get<std::string>();
    result.methods = call.result["methods"].get<std::vector<std::string>>();
  } catch (const internal::Json::exception&) {
    return failed_result<DaemonVersion>(invalid_response(
        "daemon.version contains an out-of-range or non-string value"));
  }
  if (result.protocol_version != kProtocolVersion ||
      result.service_name != "photospiderd" || result.service_version.empty() ||
      !valid_opaque_id(result.server_instance_id) ||
      result.transport != "unix" ||
      !std::is_sorted(result.methods.begin(), result.methods.end())) {
    return failed_result<DaemonVersion>(invalid_response(
        "daemon.version metadata or method ordering is invalid"));
  }
  return {internal::ok_status(), std::move(result)};
}

/** @copydoc Client::load_graph */
IpcResult<GraphSessionSummary> Client::load_graph(
    const GraphLoadRequest& request) {
  if (!impl_) {
    return failed_result<GraphSessionSummary>(
        internal::failure_status(IpcErrorDomain::Transport, 2, "not_connected",
                                 "IPC client is not connected"));
  }
  internal::Json params{{"session_name", request.session.value},
                        {"root_dir", request.root_dir}};
  if (!request.yaml_path.empty()) {
    params["yaml_path"] = request.yaml_path;
  }
  if (!request.config_path.empty()) {
    params["config_path"] = request.config_path;
  }
  if (!request.cache_root_dir.empty()) {
    params["cache_root_dir"] = request.cache_root_dir;
  }
  RawCallResult call = impl_->call("graph.load", params);
  if (!call.status.ok) {
    return failed_result<GraphSessionSummary>(std::move(call.status));
  }
  if (!call.result.value("session_id", internal::Json()).is_string() ||
      !call.result.value("session_name", internal::Json()).is_string()) {
    return failed_result<GraphSessionSummary>(
        invalid_response("graph.load result has an invalid shape"));
  }
  GraphSessionSummary summary;
  summary.session_id.value = call.result["session_id"].get<std::string>();
  summary.session_name = call.result["session_name"].get<std::string>();
  if (!valid_opaque_id(summary.session_id.value) ||
      summary.session_name != request.session.value) {
    return failed_result<GraphSessionSummary>(invalid_response(
        "graph.load result has an invalid session id or session name"));
  }
  return {internal::ok_status(), std::move(summary)};
}

/** @copydoc Client::close_graph */
IpcVoidResult Client::close_graph(const IpcSessionId& session_id) {
  if (!impl_) {
    return {internal::failure_status(IpcErrorDomain::Transport, 2,
                                     "not_connected",
                                     "IPC client is not connected")};
  }
  RawCallResult call = impl_->call(
      "graph.close", internal::Json{{"session_id", session_id.value}});
  if (!call.status.ok) {
    return {std::move(call.status)};
  }
  if (!call.result.value("closed", internal::Json()).is_boolean() ||
      !call.result["closed"].get<bool>()) {
    return {invalid_response("graph.close result must report closed=true")};
  }
  return {internal::ok_status()};
}

/** @copydoc Client::list_graphs */
IpcResult<std::vector<GraphSessionSummary>> Client::list_graphs() {
  if (!impl_) {
    return failed_result<std::vector<GraphSessionSummary>>(
        internal::failure_status(IpcErrorDomain::Transport, 2, "not_connected",
                                 "IPC client is not connected"));
  }
  RawCallResult call = impl_->call("graph.list", internal::Json::object());
  if (!call.status.ok) {
    return failed_result<std::vector<GraphSessionSummary>>(
        std::move(call.status));
  }
  if (!call.result.value("sessions", internal::Json()).is_array()) {
    return failed_result<std::vector<GraphSessionSummary>>(
        invalid_response("graph.list result requires a sessions array"));
  }
  std::vector<GraphSessionSummary> sessions;
  for (const internal::Json& value : call.result["sessions"]) {
    if (!value.is_object() ||
        !value.value("session_id", internal::Json()).is_string() ||
        !value.value("session_name", internal::Json()).is_string()) {
      return failed_result<std::vector<GraphSessionSummary>>(
          invalid_response("graph.list session row is invalid"));
    }
    GraphSessionSummary summary{{value["session_id"].get<std::string>()},
                                value["session_name"].get<std::string>()};
    if (!valid_opaque_id(summary.session_id.value)) {
      return failed_result<std::vector<GraphSessionSummary>>(invalid_response(
          "graph.list session row has an invalid opaque session id"));
    }
    sessions.push_back(std::move(summary));
  }
  if (!std::is_sorted(
          sessions.begin(), sessions.end(),
          [](const GraphSessionSummary& left,
             const GraphSessionSummary& right) {
            return std::tie(left.session_name, left.session_id.value) <
                   std::tie(right.session_name, right.session_id.value);
          })) {
    return failed_result<std::vector<GraphSessionSummary>>(invalid_response(
        "graph.list sessions are not deterministically sorted"));
  }
  return {internal::ok_status(), std::move(sessions)};
}

/** @copydoc Client::inspect_graph */
IpcResult<GraphInspectionView> Client::inspect_graph(
    const IpcSessionId& session_id) {
  if (!impl_) {
    return failed_result<GraphInspectionView>(
        internal::failure_status(IpcErrorDomain::Transport, 2, "not_connected",
                                 "IPC client is not connected"));
  }
  RawCallResult call = impl_->call(
      "inspect.graph", internal::Json{{"session_id", session_id.value}});
  if (!call.status.ok) {
    return failed_result<GraphInspectionView>(std::move(call.status));
  }
  GraphInspectionView graph;
  std::string message;
  if (!internal::decode_graph(call.result, &graph, &message) ||
      graph.session.value != session_id.value) {
    if (message.empty()) {
      message = "inspect.graph returned a different opaque session id";
    }
    return failed_result<GraphInspectionView>(
        invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(graph)};
}

/** @copydoc Client::inspect_node */
IpcResult<NodeInspectionView> Client::inspect_node(
    const IpcSessionId& session_id, NodeId node) {
  if (!impl_) {
    return failed_result<NodeInspectionView>(
        internal::failure_status(IpcErrorDomain::Transport, 2, "not_connected",
                                 "IPC client is not connected"));
  }
  RawCallResult call = impl_->call(
      "inspect.node", internal::Json{{"session_id", session_id.value},
                                     {"node_id", node.value}});
  if (!call.status.ok) {
    return failed_result<NodeInspectionView>(std::move(call.status));
  }
  if (!call.result.value("session_id", internal::Json()).is_string() ||
      call.result["session_id"].get<std::string>() != session_id.value ||
      !call.result.contains("node")) {
    return failed_result<NodeInspectionView>(
        invalid_response("inspect.node result has an invalid session or node"));
  }
  NodeInspectionView result;
  std::string message;
  if (!internal::decode_node(call.result["node"], &result, &message)) {
    return failed_result<NodeInspectionView>(
        invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(result)};
}

/** @copydoc Client::inspect_dependency_tree */
IpcResult<HostDependencyTreeSnapshot> Client::inspect_dependency_tree(
    const IpcSessionId& session_id, std::optional<NodeId> node,
    bool include_metadata) {
  if (!impl_) {
    return failed_result<HostDependencyTreeSnapshot>(
        internal::failure_status(IpcErrorDomain::Transport, 2, "not_connected",
                                 "IPC client is not connected"));
  }
  internal::Json params{{"session_id", session_id.value},
                        {"include_metadata", include_metadata}};
  if (node) {
    params["node_id"] = node->value;
  }
  RawCallResult call = impl_->call("inspect.dependency_tree", params);
  if (!call.status.ok) {
    return failed_result<HostDependencyTreeSnapshot>(std::move(call.status));
  }
  if (!call.result.value("session_id", internal::Json()).is_string() ||
      call.result["session_id"].get<std::string>() != session_id.value) {
    return failed_result<HostDependencyTreeSnapshot>(invalid_response(
        "inspect.dependency_tree returned a different opaque session id"));
  }
  HostDependencyTreeSnapshot result;
  std::string message;
  if (!internal::decode_dependency_tree(call.result, &result, &message)) {
    return failed_result<HostDependencyTreeSnapshot>(
        invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(result)};
}

}  // namespace ps::ipc
