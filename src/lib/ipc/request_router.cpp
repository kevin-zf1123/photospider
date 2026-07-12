#include "ipc/request_router.hpp"

#include <algorithm>
#include <array>
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
 * @brief Matches the session-control methods implemented by one route family.
 *
 * @param method Exact decoded request method.
 * @return True for graph mutation, node-list/YAML, cache, dirty, ROI, timing,
 *         last-IO, or last-error routing.
 * @throws Nothing.
 * @note This dispatch matcher is not the daemon capability-advertisement
 *       table and does not change `daemon.version` metadata.
 */
bool is_session_control_method(std::string_view method) noexcept {
  static constexpr std::array<std::string_view, 21> kMethods = {
      "cache.cache_all_nodes",
      "cache.clear_all",
      "cache.clear_drive",
      "cache.clear_memory",
      "cache.free_transient",
      "cache.synchronize_disk",
      "compute.last_error",
      "compute.last_io_time",
      "compute.timing",
      "dirty.begin",
      "dirty.end",
      "dirty.update",
      "graph.clear",
      "graph.node_yaml.get",
      "graph.node_yaml.set",
      "graph.reload",
      "graph.save",
      "inspect.ending_nodes",
      "inspect.node_ids",
      "inspect.roi_backward",
      "inspect.roi_forward",
  };
  return std::find(kMethods.begin(), kMethods.end(), method) != kMethods.end();
}

/**
 * @brief Decodes one required nonnegative public node id.
 *
 * @param params Typed method params object.
 * @param field Required integer field name.
 * @param node Receives the exact public node id after validation.
 * @return True when the field is an exact `int` value greater than or equal to
 *         zero; false without modifying `node` otherwise.
 * @throws std::bad_alloc if JSON object lookup requires temporary storage and
 *         allocation fails.
 */
bool read_node_id(const Json& params, const char* field, NodeId* node) {
  if (node == nullptr || !params.contains(field)) {
    return false;
  }
  int decoded = 0;
  if (!decode_integer(params[field], &decoded) || decoded < 0) {
    return false;
  }
  node->value = decoded;
  return true;
}

/**
 * @brief Decodes one required nonempty absolute path.
 *
 * @param params Typed method params object.
 * @param field Required path field name.
 * @param path Receives the validated path after all checks succeed.
 * @return True for valid UTF-8, NUL-free absolute text no longer than 4,096
 *         bytes; false without modifying `path` otherwise.
 * @throws std::bad_alloc if copied path storage cannot be allocated.
 */
bool read_required_path(const Json& params, const char* field,
                        std::string* path) {
  if (path == nullptr || !params.contains(field)) {
    return false;
  }
  std::string decoded;
  if (!decode_bounded_string(params[field], kPathTextMaxBytes, &decoded) ||
      !valid_absolute_path(decoded, false)) {
    return false;
  }
  *path = std::move(decoded);
  return true;
}

/**
 * @brief Decodes one required bounded UTF-8 string.
 *
 * @param params Typed method params object.
 * @param field Required string field name.
 * @param maximum_bytes Inclusive field-specific byte bound.
 * @param text Receives the validated string after all checks succeed.
 * @return True for valid UTF-8 within the bound; false without modifying
 *         `text` otherwise. Empty text remains a Host-visible value.
 * @throws std::bad_alloc if copied text storage cannot be allocated.
 * @note This helper deliberately adds no semantic nonempty or NUL rule to
 *       precision/YAML values; matching Host methods retain that validation.
 */
bool read_required_text(const Json& params, const char* field,
                        std::size_t maximum_bytes, std::string* text) {
  if (text == nullptr || !params.contains(field)) {
    return false;
  }
  std::string decoded;
  if (!decode_bounded_string(params[field], maximum_bytes, &decoded)) {
    return false;
  }
  *text = std::move(decoded);
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

/**
 * @brief Complete cpp-only parsed-params adapter.
 *
 * @throws Nothing for reference binding.
 * @note The adapter owns no JSON storage. `route()` constructs it only while
 *       its parsed request object remains alive and never stores it in router
 *       state.
 */
struct RequestRouter::RoutedParams {
  /** @brief Structurally valid params object borrowed from the active route. */
  const Json& value;
};

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

/** @copydoc RequestRouter::route_session_control_method */
std::optional<std::string> RequestRouter::route_session_control_method(
    const std::string& id, const std::string& method,
    const RoutedParams& routed_params) {
  const Json& params = routed_params.value;
  if (!is_session_control_method(method)) {
    return std::nullopt;
  }

  IpcSessionId session_id;
  if (!read_session_id(params, &session_id)) {
    return bounded_error(
        id, invalid_params(method + " requires a valid session_id"));
  }

  std::string text;
  NodeId first_node;
  NodeId second_node;
  PixelRect roi;
  DirtyDomain dirty_domain = DirtyDomain::HighPrecision;
  if (method == "graph.reload" || method == "graph.save") {
    if (!read_required_path(params, "yaml_path", &text)) {
      return bounded_error(
          id,
          invalid_params(method + " requires a nonempty absolute yaml_path"));
    }
  } else if (method == "graph.node_yaml.get" ||
             method == "graph.node_yaml.set") {
    if (!read_node_id(params, "node_id", &first_node)) {
      return bounded_error(
          id, invalid_params(method + " requires a nonnegative node_id"));
    }
    if (method == "graph.node_yaml.set" &&
        !read_required_text(params, "yaml_text", kLargeTextMaxBytes, &text)) {
      return bounded_error(
          id, invalid_params(
                  "graph.node_yaml.set requires bounded string yaml_text"));
    }
  } else if (method == "cache.cache_all_nodes" ||
             method == "cache.synchronize_disk") {
    if (!read_required_text(params, "precision", kShortTextMaxBytes, &text)) {
      return bounded_error(
          id, invalid_params(method + " requires bounded string precision"));
    }
  } else if (method == "dirty.begin" || method == "dirty.update" ||
             method == "dirty.end") {
    if (!read_node_id(params, "node_id", &first_node) ||
        !params.contains("domain") ||
        !decode_enum(params["domain"], &dirty_domain)) {
      return bounded_error(
          id, invalid_params(method +
                             " requires nonnegative node_id and valid domain"));
    }
    if (method != "dirty.end" &&
        (!params.contains("source_roi") ||
         !decode_pixel_rect(params["source_roi"], &roi))) {
      return bounded_error(
          id, invalid_params(method + " requires an exact source_roi"));
    }
  } else if (method == "inspect.roi_forward") {
    if (!read_node_id(params, "start_node_id", &first_node) ||
        !read_node_id(params, "target_node_id", &second_node) ||
        !params.contains("start_roi") ||
        !decode_pixel_rect(params["start_roi"], &roi)) {
      return bounded_error(
          id, invalid_params(
                  "inspect.roi_forward requires start_node_id, start_roi, "
                  "and target_node_id"));
    }
  } else if (method == "inspect.roi_backward") {
    if (!read_node_id(params, "target_node_id", &first_node) ||
        !read_node_id(params, "source_node_id", &second_node) ||
        !params.contains("target_roi") ||
        !decode_pixel_rect(params["target_roi"], &roi)) {
      return bounded_error(
          id, invalid_params(
                  "inspect.roi_backward requires target_node_id, target_roi, "
                  "and source_node_id"));
    }
  }

  IpcResult<SessionRegistry::HostCallAdmission> admission =
      registry_.admit_host_call(session_id);
  if (!admission.status.ok) {
    return bounded_error(id, admission.status);
  }
  const GraphSessionId& host_session = admission.value.host_session();

  if (method == "graph.reload" || method == "graph.save" ||
      method == "graph.clear" || method == "graph.node_yaml.set" ||
      method == "cache.clear_all" || method == "cache.clear_drive" ||
      method == "cache.clear_memory" || method == "cache.cache_all_nodes" ||
      method == "cache.free_transient" || method == "cache.synchronize_disk") {
    VoidResult routed;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      if (method == "graph.reload") {
        routed = host_.reload_graph(host_session, text);
      } else if (method == "graph.save") {
        routed = host_.save_graph(host_session, text);
      } else if (method == "graph.clear") {
        routed = host_.clear_graph(host_session);
      } else if (method == "graph.node_yaml.set") {
        routed = host_.set_node_yaml(host_session, first_node, text);
      } else if (method == "cache.clear_all") {
        routed = host_.clear_cache(host_session);
      } else if (method == "cache.clear_drive") {
        routed = host_.clear_drive_cache(host_session);
      } else if (method == "cache.clear_memory") {
        routed = host_.clear_memory_cache(host_session);
      } else if (method == "cache.cache_all_nodes") {
        routed = host_.cache_all_nodes(host_session, text);
      } else if (method == "cache.free_transient") {
        routed = host_.free_transient_memory(host_session);
      } else {
        routed = host_.synchronize_disk_cache(host_session, text);
      }
    }
    if (!routed.status.ok) {
      return bounded_error(id, graph_status(routed.status));
    }
    return encode_success_response(id, Json::object());
  }

  if (method == "graph.node_yaml.get") {
    Result<std::string> routed;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      routed = host_.get_node_yaml(host_session, first_node);
    }
    if (!routed.status.ok) {
      return bounded_error(id, graph_status(routed.status));
    }
    return encode_routed_value(id, [&] {
      return encode_node_yaml(session_id, first_node, routed.value);
    });
  }

  if (method == "inspect.node_ids" || method == "inspect.ending_nodes") {
    Result<std::vector<NodeId>> routed;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      routed = method == "inspect.node_ids" ? host_.list_node_ids(host_session)
                                            : host_.ending_nodes(host_session);
    }
    if (!routed.status.ok) {
      return bounded_error(id, graph_status(routed.status));
    }
    return encode_routed_value(id, [&] {
      const char* field =
          method == "inspect.node_ids" ? "node_ids" : "ending_node_ids";
      return Json{{"session_id", session_id.value},
                  {field, encode_node_ids(routed.value)}};
    });
  }

  if (method == "dirty.begin" || method == "dirty.update" ||
      method == "dirty.end") {
    Result<DirtyRegionInspectionSnapshot> routed;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      if (method == "dirty.begin") {
        routed = host_.begin_dirty_source(host_session, first_node,
                                          dirty_domain, roi);
      } else if (method == "dirty.update") {
        routed = host_.update_dirty_source(host_session, first_node,
                                           dirty_domain, roi);
      } else {
        routed = host_.end_dirty_source(host_session, first_node, dirty_domain);
      }
    }
    if (!routed.status.ok) {
      return bounded_error(id, graph_status(routed.status));
    }
    return encode_routed_value(
        id, [&] { return encode_dirty_region(id, session_id, routed.value); });
  }

  if (method == "inspect.roi_forward" || method == "inspect.roi_backward") {
    Result<PixelRect> routed;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      routed =
          method == "inspect.roi_forward"
              ? host_.project_roi(host_session, first_node, roi, second_node)
              : host_.project_roi_backward(host_session, first_node, roi,
                                           second_node);
    }
    if (!routed.status.ok) {
      return bounded_error(id, graph_status(routed.status));
    }
    return encode_success_response(
        id, Json{{"session_id", session_id.value},
                 {"roi", encode_pixel_rect(routed.value)}});
  }

  if (method == "compute.timing") {
    Result<TimingSnapshot> routed;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      routed = host_.timing(host_session);
    }
    if (!routed.status.ok) {
      return bounded_error(id, graph_status(routed.status));
    }
    return encode_routed_value(
        id, [&] { return encode_timing(id, session_id, routed.value); });
  }

  if (method == "compute.last_io_time") {
    Result<double> routed;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      routed = host_.last_io_time(host_session);
    }
    if (!routed.status.ok) {
      return bounded_error(id, graph_status(routed.status));
    }
    return encode_routed_value(
        id, [&] { return encode_last_io_time(session_id, routed.value); });
  }

  if (method == "compute.last_error") {
    OperationStatus observed;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      observed = graph_status(host_.last_error(host_session));
    }
    return encode_routed_value(id, [&] {
      return Json{{"session_id", session_id.value},
                  {"status", encode_operation_status(observed)}};
    });
  }

  return bounded_error(
      id, failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                         "internal_error",
                         "session-control dispatch invariant failed"));
}

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

    if (std::optional<std::string> routed =
            route_session_control_method(id, method, RoutedParams{params})) {
      return std::move(*routed);
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
