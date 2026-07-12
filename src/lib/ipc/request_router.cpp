#include "ipc/request_router.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ipc/codec.hpp"
#include "photospider/ipc/protocol.hpp"

namespace ps::ipc::internal {
namespace {

/**
 * @brief Builds the sorted version 1 method inventory.
 *
 * @return Exact eight method names advertised by `daemon.version`.
 * @throws std::bad_alloc if vector/string allocation fails.
 * @note The inventory contains no compute, scheduler, plugin, event, image, or
 *       shutdown method.
 */
std::vector<std::string> version_methods() {
  std::vector<std::string> methods;
  methods.reserve(kVersionOneMethodNames.size());
  for (std::string_view method : kVersionOneMethodNames) {
    methods.emplace_back(method);
  }
  return methods;
}

/**
 * @brief Builds a version 1 failure envelope.
 *
 * @param id Recovered request id string or JSON null.
 * @param status Stable failure status.
 * @param supported_versions Whether protocol negotiation metadata is needed.
 * @return Error response object with exactly one error branch.
 * @throws std::bad_alloc if JSON allocation fails.
 */
Json error_envelope(const Json& id, const OperationStatus& status,
                    bool supported_versions = false) {
  OperationStatus bounded = status;
  bounded.message = bounded_diagnostic(status.message);
  Json error = encode_error(bounded);
  if (supported_versions) {
    error["supported_versions"] = Json::array({kProtocolVersion});
  }
  return Json{{"protocol_version", kProtocolVersion},
              {"id", id},
              {"error", std::move(error)}};
}

/**
 * @brief Serializes one bounded error response.
 *
 * @param id Recovered request id string or JSON null.
 * @param status Stable failure status.
 * @param supported_versions Whether to include `[1]` negotiation metadata.
 * @return Payload smaller than the version 1 frame limit.
 * @throws std::bad_alloc if serialization cannot allocate.
 */
std::string bounded_error(const Json& id, const OperationStatus& status,
                          bool supported_versions = false) {
  return error_envelope(id, status, supported_versions).dump();
}

/**
 * @brief Encodes one routed result while containing value-size rejection.
 *
 * @tparam ResultFactory Callable returning the complete JSON result value.
 * @param id Valid request id correlated with the response.
 * @param factory Builds the result only after its Host call has completed.
 * @return Successful response, or protocol `response_too_large` when result
 *         construction or response serialization throws `std::length_error`.
 * @throws std::bad_alloc if result or error-response allocation fails.
 * @throws std::invalid_argument if the returned value is malformed.
 * @note Host calls must occur before this helper. Only `std::length_error`
 *       raised by the result factory or `encode_success_response` is mapped;
 *       every other exception propagates to the route-level daemon boundary.
 */
template <typename ResultFactory>
std::string encode_routed_value(const std::string& id,
                                ResultFactory&& factory) {
  try {
    return encode_success_response(id, std::forward<ResultFactory>(factory)());
  } catch (const std::length_error& error) {
    return bounded_error(
        id,
        failure_status(OperationErrorDomain::Protocol, kResponseTooLargeCode,
                       "response_too_large", error.what()));
  }
}

/**
 * @brief Creates a protocol-domain invalid-params status.
 *
 * @param message Human-readable validation diagnostic.
 * @return Stable version 1 invalid-params status.
 * @throws std::bad_alloc if message storage cannot be allocated.
 */
OperationStatus invalid_params(std::string message) {
  return failure_status(OperationErrorDomain::Protocol, kInvalidParamsCode,
                        "invalid_params", std::move(message));
}

/**
 * @brief Validates a required or optional absolute path parameter.
 *
 * @param path UTF-8 path text.
 * @param allow_empty Whether absence mapped to an empty Host string is valid.
 * @return True for allowed empty text or a valid UTF-8, NUL-free absolute path
 *         no longer than 4,096 bytes.
 * @throws Nothing; filesystem path construction failures are treated invalid.
 */
bool valid_absolute_path(const std::string& path, bool allow_empty) noexcept {
  if (path.empty()) {
    return allow_empty;
  }
  if (path.size() > kPathTextMaxBytes || !valid_utf8(path) ||
      path.find('\0') != std::string::npos) {
    return false;
  }
  try {
    return std::filesystem::path(path).is_absolute();
  } catch (...) {
    return false;
  }
}

/**
 * @brief Validates and reads an optional string path from params.
 *
 * @param params Typed method params object.
 * @param field Optional path field name.
 * @param output Receives empty text when absent or the supplied absolute path.
 * @return True when absent or a valid bounded string absolute path, without
 *         modifying `output` on malformed input.
 * @throws std::bad_alloc if copied path storage cannot be allocated.
 */
bool optional_path(const Json& params, const char* field, std::string* output) {
  if (!params.contains(field)) {
    output->clear();
    return true;
  }
  if (!params[field].is_string()) {
    return false;
  }
  std::string decoded;
  if (!decode_bounded_string(params[field], kPathTextMaxBytes, &decoded) ||
      !valid_absolute_path(decoded, true)) {
    return false;
  }
  *output = std::move(decoded);
  return true;
}

/**
 * @brief Extracts one validated opaque session id from method params.
 *
 * @param params Typed method params object.
 * @param session_id Receives the opaque id.
 * @return True when the required field is a valid version 1 token, without
 *         modifying `session_id` on malformed input.
 * @throws std::bad_alloc if string storage cannot be allocated.
 */
bool read_session_id(const Json& params, IpcSessionId* session_id) {
  if (!params.value("session_id", Json()).is_string()) {
    return false;
  }
  std::string decoded;
  if (!decode_opaque_id(params["session_id"], &decoded)) {
    return false;
  }
  session_id->value = std::move(decoded);
  return true;
}

/**
 * @brief Best-effort closes a Host session during failed registry publication.
 *
 * @param host Daemon-owned Host whose successful load must be compensated.
 * @param session Exact session id returned by the successful Host load.
 * @throws Nothing; status failures and exceptions are intentionally contained.
 * @note Callers remove the loading reservation before entering this helper, so
 *       even a throwing Host close cannot strand a registry loading row.
 */
void close_graph_best_effort(Host& host,
                             const GraphSessionId& session) noexcept {
  try {
    (void)host.close_graph(session);
  } catch (...) {
  }
}

/**
 * @brief Dispatches one structurally valid request to daemon metadata methods.
 *
 * @param method Exact method name.
 * @param params Valid params object.
 * @param service_version Immutable project version.
 * @param instance_id Immutable daemon instance id.
 * @param handled Set true when this helper recognizes the method.
 * @return Successful result object when handled; empty otherwise.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 * @note This helper never calls or locks Host.
 */
Json route_daemon_method(const std::string& method, const Json& params,
                         const std::string& service_version,
                         const std::string& instance_id, bool* handled) {
  if (method != "daemon.ping" && method != "daemon.version") {
    *handled = false;
    return {};
  }
  *handled = true;
  (void)params;
  if (method == "daemon.ping") {
    return Json{{"pong", true}, {"server_instance_id", instance_id}};
  }
  return Json{{"protocol_version", kProtocolVersion},
              {"service_name", "photospiderd"},
              {"service_version", service_version},
              {"server_instance_id", instance_id},
              {"transport", "unix"},
              {"methods", version_methods()}};
}

}  // namespace

/** @copydoc RequestRouter::RequestRouter */
RequestRouter::RequestRouter(Host& host, std::string service_version)
    : host_(host),
      compute_registry_(
          registry_,
          [this](const HostComputeRequest& request) {
            std::lock_guard<std::mutex> host_lock(host_mutex_);
            return graph_status(host_.compute(request).status);
          },                                           // NOLINT
          [this](const HostComputeRequest& request) {  // NOLINT
            std::lock_guard<std::mutex> host_lock(host_mutex_);
            Result<ImageBuffer> result = host_.compute_and_get_image(request);
            result.status = graph_status(result.status);
            return result;
          },                                          // NOLINT
          [this](const ComputeRequestId& compute_id,  // NOLINT
                 ImageBuffer image) {
            return output_store_.publish(compute_id, std::move(image));
          }),                                        // NOLINT
      service_version_(std::move(service_version)),  // NOLINT
      server_instance_id_(          // NOLINT(whitespace/indent_namespace)
          generate_opaque_id()) {}  // NOLINT

/** @copydoc RequestRouter::route */
std::string RequestRouter::route(const std::string& payload) {
  const JsonParseResult parsed = parse_json(payload);
  if (!parsed.ok) {
    Json response_id = nullptr;
    if (parsed.duplicate_key && !parsed.ambiguous_top_level_id &&
        parsed.value.is_object() &&
        parsed.value.value("id", Json()).is_string()) {
      std::string candidate;
      if (decode_bounded_string(parsed.value["id"], kRequestTextMaxBytes,
                                &candidate) &&
          !candidate.empty()) {
        response_id = candidate;
      }
    }
    return bounded_error(
        response_id,
        failure_status(
            OperationErrorDomain::Protocol,
            parsed.duplicate_key ? kInvalidRequestCode : kParseErrorCode,
            parsed.duplicate_key ? "invalid_request" : "parse_error",
            parsed.message));
  }
  if (!parsed.value.is_object()) {
    return bounded_error(nullptr,
                         failure_status(OperationErrorDomain::Protocol,
                                        kInvalidRequestCode, "invalid_request",
                                        "request envelope must be an object"));
  }
  const Json& request = parsed.value;
  Json response_id = nullptr;
  if (request.value("id", Json()).is_string()) {
    std::string candidate;
    if (decode_bounded_string(request["id"], kRequestTextMaxBytes,
                              &candidate) &&
        !candidate.empty()) {
      response_id = candidate;
    }
  }
  std::string decoded_method;
  const bool valid_method =
      request.contains("method") &&
      decode_bounded_string(request["method"], kRequestTextMaxBytes,
                            &decoded_method) &&
      !decoded_method.empty();
  if (!request.value("protocol_version", Json()).is_number_integer() ||
      !response_id.is_string() || !valid_method ||
      !request.value("params", Json()).is_object()) {
    return bounded_error(
        response_id,
        failure_status(OperationErrorDomain::Protocol, kInvalidRequestCode,
                       "invalid_request",
                       "request requires integer protocol_version, valid id, "
                       "nonempty method, and object params"));
  }
  const std::string id = response_id.get<std::string>();
  std::int32_t request_version = 0;
  if (!decode_integer(request["protocol_version"], &request_version) ||
      request_version != kProtocolVersion) {
    return bounded_error(
        id,
        failure_status(OperationErrorDomain::Protocol, kUnsupportedProtocolCode,
                       "unsupported_protocol",
                       "requested protocol version is not supported"),
        true);
  }
  const std::string method = std::move(decoded_method);
  const Json& params = request["params"];

  try {
    bool handled = false;
    OperationStatus status = ok_status();
    Json result = route_daemon_method(method, params, service_version_,
                                      server_instance_id_, &handled);
    if (handled) {
      return status.ok ? encode_success_response(id, std::move(result))
                       : bounded_error(id, status);
    }

    if (method == "graph.load") {
      GraphLoadRequest load_request;
      if (!params.contains("session_name") || !params.contains("root_dir") ||
          !decode_bounded_string(params["session_name"], kShortTextMaxBytes,
                                 &load_request.session.value) ||
          !decode_bounded_string(params["root_dir"], kPathTextMaxBytes,
                                 &load_request.root_dir)) {
        return bounded_error(
            id, invalid_params(
                    "graph.load requires string session_name and root_dir"));
      }
      if (!valid_session_name(load_request.session.value) ||
          !valid_absolute_path(load_request.root_dir, false) ||
          !optional_path(params, "yaml_path", &load_request.yaml_path) ||
          !optional_path(params, "config_path", &load_request.config_path) ||
          !optional_path(params, "cache_root_dir",
                         &load_request.cache_root_dir)) {
        return bounded_error(
            id,
            invalid_params("graph.load contains an unsafe session or path"));
      }

      std::lock_guard<std::mutex> host_lock(host_mutex_);
      IpcResult<IpcSessionId> reservation =
          registry_.reserve(load_request.session.value);
      if (!reservation.status.ok) {
        return bounded_error(id, reservation.status);
      }
      Result<GraphSessionId> loaded;
      try {
        loaded = host_.load_graph(load_request);
      } catch (...) {
        registry_.rollback(reservation.value);
        throw;
      }
      if (!loaded.status.ok) {
        registry_.rollback(reservation.value);
        return bounded_error(id, graph_status(loaded.status));
      }
      try {
        status = registry_.commit(reservation.value, loaded.value);
      } catch (...) {
        const std::exception_ptr publication_failure = std::current_exception();
        registry_.rollback(reservation.value);
        close_graph_best_effort(host_, loaded.value);
        std::rethrow_exception(publication_failure);
      }
      if (!status.ok) {
        registry_.rollback(reservation.value);
        close_graph_best_effort(host_, loaded.value);
        return bounded_error(id, status);
      }
      return encode_success_response(
          id, Json{{"session_id", reservation.value.value},
                   {"session_name", load_request.session.value}});
    }

    if (method == "graph.close") {
      IpcSessionId session_id;
      if (!read_session_id(params, &session_id)) {
        return bounded_error(
            id, invalid_params("graph.close requires a valid session_id"));
      }
      IpcResult<SessionRegistry::CloseClaim> claim =
          registry_.begin_close(session_id);
      if (!claim.status.ok) {
        return bounded_error(id, claim.status);
      }
      VoidResult closed;
      {
        std::lock_guard<std::mutex> host_lock(host_mutex_);
        closed = host_.close_graph(claim.value.host_session());
        if (closed.status.ok ||
            checked_graph_error_code(closed.status) == GraphErrc::NotFound) {
          claim.value.erase();
        } else {
          claim.value.reopen();
        }
      }
      if (!closed.status.ok) {
        return bounded_error(id, graph_status(closed.status));
      }
      return encode_success_response(id, Json{{"closed", true}});
    }

    if (method == "graph.list") {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      const Result<std::vector<GraphSessionId>> listed = host_.list_graphs();
      if (!listed.status.ok) {
        return bounded_error(id, graph_status(listed.status));
      }
      IpcResult<std::vector<GraphSessionSummary>> reconciled =
          registry_.reconcile(listed.value);
      if (!reconciled.status.ok) {
        return bounded_error(id, reconciled.status);
      }
      return encode_routed_value(id, [&reconciled] {
        return Json{{"sessions", encode_session_summaries(reconciled.value)}};
      });
    }

    if (method == "inspect.graph" || method == "inspect.node" ||
        method == "inspect.dependency_tree") {
      IpcSessionId session_id;
      if (!read_session_id(params, &session_id)) {
        return bounded_error(
            id, invalid_params("inspection requires a valid session_id"));
      }
      std::optional<NodeId> node;
      bool include_metadata = false;
      if (method == "inspect.node") {
        if (!params.value("node_id", Json()).is_number_integer()) {
          return bounded_error(
              id, invalid_params("inspect.node requires integer node_id"));
        }
        int node_id = 0;
        if (!decode_integer(params["node_id"], &node_id)) {
          return bounded_error(id, invalid_params("node_id is out of range"));
        }
        node = NodeId{node_id};
        if (node->value < 0) {
          return bounded_error(id,
                               invalid_params("node_id must be nonnegative"));
        }
      } else if (method == "inspect.dependency_tree") {
        if (params.contains("node_id") && !params["node_id"].is_null()) {
          if (!params["node_id"].is_number_integer()) {
            return bounded_error(
                id, invalid_params("node_id must be integer or null"));
          }
          int node_id = 0;
          if (!decode_integer(params["node_id"], &node_id)) {
            return bounded_error(id, invalid_params("node_id is out of range"));
          }
          node = NodeId{node_id};
          if (node->value < 0) {
            return bounded_error(id,
                                 invalid_params("node_id must be nonnegative"));
          }
        }
        if (params.contains("include_metadata")) {
          if (!params["include_metadata"].is_boolean()) {
            return bounded_error(
                id, invalid_params("include_metadata must be boolean"));
          }
          include_metadata = params["include_metadata"].get<bool>();
        }
      }

      IpcResult<SessionRegistry::HostCallAdmission> admission =
          registry_.admit_host_call(session_id);
      if (!admission.status.ok) {
        return bounded_error(id, admission.status);
      }
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      if (method == "inspect.graph") {
        const Result<GraphInspectionView> inspected =
            host_.inspect_graph(admission.value.host_session());
        if (!inspected.status.ok) {
          return bounded_error(id, graph_status(inspected.status));
        }
        return encode_routed_value(id, [&session_id, &inspected] {
          return encode_graph(session_id, inspected.value);
        });
      }
      if (method == "inspect.node") {
        const Result<NodeInspectionView> inspected =
            host_.inspect_node(admission.value.host_session(), *node);
        if (!inspected.status.ok) {
          return bounded_error(id, graph_status(inspected.status));
        }
        return encode_routed_value(id, [&session_id, &inspected] {
          return Json{{"session_id", session_id.value},
                      {"node", encode_node(inspected.value)}};
        });
      }
      const Result<HostDependencyTreeSnapshot> inspected =
          host_.dependency_tree(admission.value.host_session(), node,
                                include_metadata);
      if (!inspected.status.ok) {
        return bounded_error(id, graph_status(inspected.status));
      }
      return encode_routed_value(id, [&session_id, &inspected] {
        return encode_dependency_tree(session_id, inspected.value);
      });
    }

    return bounded_error(
        id, failure_status(OperationErrorDomain::Protocol, kMethodNotFoundCode,
                           "method_not_found",
                           "method is not implemented by protocol version 1"));
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    return bounded_error(
        id, failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                           "internal_error", error.what()));
  } catch (...) {
    return bounded_error(
        id, failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                           "internal_error",
                           "unexpected non-standard request failure"));
  }
}

/** @copydoc RequestRouter::start_runtime */
OperationStatus RequestRouter::start_runtime(const std::string& socket_path,
                                             int lifecycle_lock_fd) {
  registry_.stop_admission();
  OperationStatus output_started =
      output_store_.start(socket_path, server_instance_id_, lifecycle_lock_fd);
  if (!output_started.ok) {
    return output_started;
  }
  OperationStatus started;
  try {
    collection_snapshots_.start();
    started = compute_registry_.start();
  } catch (...) {
    collection_snapshots_.finish_shutdown();
    output_store_.shutdown();
    throw;
  }
  if (started.ok) {
    registry_.start_admission();
  } else {
    collection_snapshots_.finish_shutdown();
    output_store_.shutdown();
  }
  return started;
}

/** @copydoc RequestRouter::begin_shutdown */
void RequestRouter::begin_shutdown() noexcept {
  registry_.stop_admission();
  collection_snapshots_.begin_shutdown();
  compute_registry_.stop_admission();
  output_store_.stop_leases();
}

/** @copydoc RequestRouter::finish_shutdown */
void RequestRouter::finish_shutdown() noexcept {
  compute_registry_.shutdown();
  collection_snapshots_.finish_shutdown();
  output_store_.shutdown();
  try {
    const auto sessions = registry_.active_sessions();
    for (const auto& session : sessions) {
      try {
        std::lock_guard<std::mutex> host_lock(host_mutex_);
        (void)host_.close_graph(session.second);
      } catch (...) {
      }
    }
  } catch (...) {
  }
  registry_.clear();
}

/** @copydoc RequestRouter::server_instance_id */
const std::string& RequestRouter::server_instance_id() const noexcept {
  return server_instance_id_;
}

}  // namespace ps::ipc::internal
