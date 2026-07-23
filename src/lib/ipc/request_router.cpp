#include "ipc/request_router.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
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
 * @brief Builds the sorted version 2 method inventory.
 *
 * @return Exact 60 method names advertised by `daemon.version`.
 * @throws std::bad_alloc if vector/string allocation fails.
 * @note The returned copy preserves the strict lexical order of the
 *       authoritative compile-time table and contains no aliases or future
 *       cancellation, streaming, shutdown, or compatibility methods.
 */
std::vector<std::string> version_methods() {
  std::vector<std::string> methods;
  methods.reserve(kVersionTwoMethodNames.size());
  for (std::string_view method : kVersionTwoMethodNames) {
    methods.emplace_back(method);
  }
  return methods;
}

/**
 * @brief Tests whether one decoded method belongs to the advertised surface.
 *
 * @param method Exact bounded UTF-8 request method.
 * @return True only for one entry in the sorted 60-method version 2 table.
 * @throws Nothing; binary search compares borrowed immutable string views.
 * @note `RequestRouter::route` applies this allowlist before every route
 *       family. Consequently an added private matcher cannot accidentally
 *       expose an unadvertised method; advertised methods still use separate
 *       family matchers so a missing implementation reaches the final
 *       `method_not_found` contract check.
 */
bool is_version_two_method(std::string_view method) noexcept {
  return std::binary_search(kVersionTwoMethodNames.begin(),
                            kVersionTwoMethodNames.end(), method);
}

/**
 * @brief Builds a version 2 failure envelope.
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
 * @return Payload smaller than the version 2 frame limit.
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
 * @return Stable version 2 invalid-params status.
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
 * @return True when the required field is a valid version 2 token, without
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
 * @brief Matches the four daemon-owned polling compute lifecycle methods.
 * @param method Exact decoded request method.
 * @return True only for submit, status, result, or release.
 * @throws Nothing.
 * @note Capability admission remains owned by the exact 60-method version
 *       table; this independent matcher lets tests detect missing compute
 *       dispatch.
 */
bool is_compute_method(std::string_view method) noexcept {
  static constexpr std::array<std::string_view, 4> kMethods = {
      "compute.release", "compute.result", "compute.status", "compute.submit"};
  return std::find(kMethods.begin(), kMethods.end(), method) != kMethods.end();
}

/**
 * @brief Matches process-global operation-plugin routes.
 * @param method Exact decoded request method.
 * @return True only for report/load composition, global mutations, or views.
 * @throws Nothing.
 * @note Capability admission remains owned by the exact 60-method version
 *       table; this independent matcher lets tests detect missing plugin
 *       dispatch.
 */
bool is_plugin_method(std::string_view method) noexcept {
  static constexpr std::array<std::string_view, 6> kMethods = {
      "plugins.load_report",          "plugins.ops_combined_keys",
      "plugins.ops_combined_sources", "plugins.ops_sources",
      "plugins.seed_builtins",        "plugins.unload_all"};
  return std::find(kMethods.begin(), kMethods.end(), method) != kMethods.end();
}

/**
 * @brief Matches bounded Host observation methods.
 * @param method Exact decoded request method.
 * @return True only for compute-event drain or execution-trace paging.
 * @throws Nothing.
 * @note Capability admission remains owned by the exact 60-method version
 *       table; this independent matcher lets tests detect missing observation
 *       dispatch.
 */
bool is_observation_method(std::string_view method) noexcept {
  return method == "events.drain" || method == "execution.trace";
}

/**
 * @brief Matches policy discovery, configuration, and session routes.
 * @param method Exact decoded request method.
 * @return True only for policy methods other than bounded trace paging.
 * @throws Nothing.
 * @note The trace method remains owned by `is_observation_method()`.
 *       Capability admission remains owned by the exact 60-method table, so
 *       this independent matcher can reveal missing policy dispatch.
 */
bool is_policy_method(std::string_view method) noexcept {
  static constexpr std::array<std::string_view, 8> kMethods = {
      "policy.configure_defaults",
      "policy.description",
      "policy.info",
      "policy.load",
      "policy.loaded_plugins",
      "policy.replace",
      "policy.scan",
      "policy.types"};
  return std::find(kMethods.begin(), kMethods.end(), method) != kMethods.end();
}

/**
 * @brief Matches private execution discovery, defaults, and session routes.
 * @param method Exact decoded request method.
 * @return True for the five execution methods routed outside observation.
 * @throws Nothing.
 * @note `execution.trace` remains owned by `is_observation_method()`.
 */
bool is_execution_method(std::string_view method) noexcept {
  static constexpr std::array<std::string_view, 5> kMethods = {
      "execution.configure_defaults", "execution.description", "execution.info",
      "execution.replace", "execution.types"};
  return std::find(kMethods.begin(), kMethods.end(), method) != kMethods.end();
}

/**
 * @brief Decodes one required opaque compute-job identity.
 * @param params Typed method params object.
 * @param compute_id Receives the validated daemon job identity.
 * @return True only when `compute_id` has the exact 32-lowercase-hex shape.
 * @throws std::bad_alloc if copied identity storage cannot be allocated.
 */
bool read_compute_id(const Json& params, ComputeRequestId* compute_id) {
  if (compute_id == nullptr ||
      !params.value("compute_id", Json()).is_string()) {
    return false;
  }
  std::string decoded;
  if (!decode_opaque_id(params["compute_id"], &decoded)) {
    return false;
  }
  compute_id->value = std::move(decoded);
  return true;
}

/**
 * @brief Decodes the optional stable delivery lease identity on release.
 * @param params Typed compute.release params object.
 * @param delivery_id Receives nullopt when absent or the validated identity.
 * @return True when absent or exactly 32 lowercase hexadecimal characters;
 *         false without publishing a malformed value.
 * @throws std::bad_alloc if copied identity storage cannot be allocated.
 * @note Unknown release members remain forward-compatible. A present known
 *       delivery_id is validated before any job or OutputStore mutation.
 */
bool read_optional_delivery_id(const Json& params,
                               std::optional<std::string>* delivery_id) {
  if (delivery_id == nullptr) {
    return false;
  }
  if (!params.contains("delivery_id")) {
    delivery_id->reset();
    return true;
  }
  if (!params["delivery_id"].is_string()) {
    return false;
  }
  std::string decoded;
  if (!decode_opaque_id(params["delivery_id"], &decoded)) {
    return false;
  }
  *delivery_id = std::move(decoded);
  return true;
}

/**
 * @brief Decodes one optional object of boolean compute controls.
 * @param params Submit params containing the optional object.
 * @param object_name Known object member name.
 * @param fields Known boolean field names and destination pointers.
 * @return True when the object is absent or every present known field is
 *         boolean; false without accessing Host or session state otherwise.
 * @throws std::bad_alloc if JSON object lookup or diagnostics allocate.
 * @note Unknown members are ignored for forward compatibility. Destinations
 *       retain their public default values when known members are absent.
 */
bool read_compute_boolean_object(
    const Json& params, const char* object_name,
    const std::vector<std::pair<const char*, bool*>>& fields) {
  if (!params.contains(object_name)) {
    return true;
  }
  const Json& object = params[object_name];
  if (!object.is_object()) {
    return false;
  }
  for (const auto& field : fields) {
    if (!object.contains(field.first)) {
      continue;
    }
    if (!object[field.first].is_boolean()) {
      return false;
    }
    *field.second = object[field.first].get<bool>();
  }
  return true;
}

/**
 * @brief Decodes the complete final compute-submit request schema.
 *
 * @param params Structurally valid submit params object.
 * @param session_id Receives the opaque session identity.
 * @param request Receives the public Host compute value with its private
 *        session left empty for registry admission to overwrite.
 * @param mode Receives the exact status or image executor selection.
 * @param message Receives one stable validation diagnostic on failure.
 * @return True only after every known submit member validates completely.
 * @throws std::bad_alloc if owned request strings or diagnostics allocate.
 * @note Optional cache/execution/telemetry objects preserve public defaults;
 *       optional maximum parallelism, intent, and dirty ROI accept absence or
 *       JSON null. Unknown fields at every object level are
 *       forward-compatible. No registry or Host access occurs in this helper.
 */
bool decode_compute_submit(const Json& params, IpcSessionId* session_id,
                           HostComputeRequest* request, ComputeResultMode* mode,
                           std::string* message) {
  if (session_id == nullptr || request == nullptr || mode == nullptr ||
      message == nullptr || !read_session_id(params, session_id) ||
      !read_node_id(params, "node_id", &request->node)) {
    if (message != nullptr) {
      *message =
          "compute.submit requires valid session_id and nonnegative "
          "node_id";
    }
    return false;
  }

  std::string result_mode;
  if (!params.contains("result_mode") ||
      !decode_bounded_string(params["result_mode"], kShortTextMaxBytes,
                             &result_mode) ||
      (result_mode != "status" && result_mode != "image")) {
    *message = "compute.submit result_mode must be status or image";
    return false;
  }
  *mode = result_mode == "status" ? ComputeResultMode::Status
                                  : ComputeResultMode::Image;

  if (params.contains("cache")) {
    const Json& cache = params["cache"];
    if (!cache.is_object()) {
      *message = "compute.submit cache must be an object";
      return false;
    }
    if (cache.contains("precision") &&
        !decode_bounded_string(cache["precision"], kShortTextMaxBytes,
                               &request->cache.precision)) {
      *message = "compute.submit cache precision must be bounded UTF-8";
      return false;
    }
  }
  if (!read_compute_boolean_object(
          params, "cache",
          {{"force_recache", &request->cache.force_recache},
           {"disable_disk_cache", &request->cache.disable_disk_cache},
           {"nosave", &request->cache.nosave}})) {
    *message = "compute.submit cache controls must be boolean";
    return false;
  }
  if (!read_compute_boolean_object(params, "execution",
                                   {{"parallel", &request->execution.parallel},
                                    {"quiet", &request->execution.quiet}})) {
    *message = "compute.submit execution controls must be boolean";
    return false;
  }
  if (params.contains("execution")) {
    const Json& execution = params["execution"];
    if (execution.contains("maximum_parallelism") &&
        !execution["maximum_parallelism"].is_null()) {
      std::uint32_t maximum_parallelism = 0U;
      if (!decode_integer(execution["maximum_parallelism"],
                          &maximum_parallelism) ||
          maximum_parallelism == 0U) {
        *message =
            "compute.submit maximum_parallelism must be null or a positive "
            "uint32";
        return false;
      }
      request->execution.maximum_parallelism = maximum_parallelism;
    }
  }
  if (!read_compute_boolean_object(
          params, "telemetry",
          {{"enable_timing", &request->telemetry.enable_timing}})) {
    *message = "compute.submit telemetry controls must be boolean";
    return false;
  }

  if (params.contains("intent") && !params["intent"].is_null()) {
    ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
    if (!decode_enum(params["intent"], &intent)) {
      *message = "compute.submit intent must be null or a valid enum";
      return false;
    }
    request->intent = intent;
  }
  if (params.contains("dirty_roi") && !params["dirty_roi"].is_null()) {
    PixelRect dirty_roi;
    if (!decode_pixel_rect(params["dirty_roi"], &dirty_roi)) {
      *message = "compute.submit dirty_roi must be null or an exact ROI";
      return false;
    }
    request->dirty_roi = dirty_roi;
  }
  return true;
}

/**
 * @brief Returns the stable lowercase lifecycle label for one registry state.
 * @param state Private forward-only job state.
 * @return Exact version 2 state label.
 * @throws std::invalid_argument if an invalid future enum value is observed.
 */
std::string_view compute_state_label(ComputeRequestState state) {
  switch (state) {
    case ComputeRequestState::Queued:
      return "queued";
    case ComputeRequestState::Running:
      return "running";
    case ComputeRequestState::Succeeded:
      return "succeeded";
    case ComputeRequestState::Failed:
      return "failed";
  }
  throw std::invalid_argument("compute registry returned an invalid state");
}

/**
 * @brief Encodes one immutable polling-job snapshot using stable common fields.
 * @param snapshot Complete registry snapshot at one lookup instant.
 * @param output Nullable protected image metadata supplied only by result.
 * @return Owned JSON value containing nullable nested status and output.
 * @throws std::bad_alloc if JSON or copied status storage cannot allocate.
 * @throws std::invalid_argument for an invalid state or noncanonical terminal
 *         snapshot.
 * @note Submit and status pass JSON null. Result may pass one already
 *       revalidated metadata delivery. The private reference is never exposed
 *       as an extra `output_reference` field; its opaque value appears only as
 *       the normalized `output.output_id` metadata member after revalidation.
 */
Json encode_compute_snapshot(const ComputeRequestSnapshot& snapshot,
                             Json output = nullptr) {
  const bool terminal = snapshot.state == ComputeRequestState::Succeeded ||
                        snapshot.state == ComputeRequestState::Failed;
  if (terminal != snapshot.terminal_status.has_value() ||
      (snapshot.state == ComputeRequestState::Succeeded &&
       !snapshot.terminal_status->ok) ||
      (snapshot.state == ComputeRequestState::Failed &&
       snapshot.terminal_status->ok)) {
    throw std::invalid_argument(
        "compute registry returned an inconsistent terminal snapshot");
  }
  Json status = nullptr;
  if (snapshot.terminal_status) {
    status = encode_operation_status(*snapshot.terminal_status);
  }
  return Json{{"compute_id", snapshot.compute_id.value},
              {"session_id", snapshot.session_id.value},
              {"state", compute_state_label(snapshot.state)},
              {"cancellable", snapshot.cancellable},
              {"status", std::move(status)},
              {"output", std::move(output)}};
}

/**
 * @brief Encodes one revalidated protected output delivery for compute.result.
 * @param delivery Metadata and stable lease returned atomically by OutputStore.
 * @param expected_output_reference Private job reference used for lookup.
 * @return Exact version 2 metadata object without pixel bytes or private
 *         registry references.
 * @throws std::bad_alloc if JSON or enum string storage cannot allocate.
 * @throws std::invalid_argument if trusted store metadata violates the wire
 *         invariant.
 * @note The absolute path is the protected daemon artifact path required by
 *       version 2. It is not a backend cache path or caller-selected path.
 */
Json encode_output_delivery(const OutputArtifactDelivery& delivery,
                            const std::string& expected_output_reference) {
  const OutputArtifactMetadata& metadata = delivery.metadata;
  Json data_type;
  Json device;
  if (!valid_output_delivery_for_wire(delivery, expected_output_reference) ||
      !encode_enum(metadata.data_type, &data_type) ||
      !encode_enum(metadata.device, &device)) {
    throw std::invalid_argument(
        "output store returned malformed delivery metadata");
  }
  return Json{{"output_id", metadata.output_id},
              {"delivery_id", delivery.delivery_id},
              {"path", metadata.path},
              {"width", metadata.width},
              {"height", metadata.height},
              {"channels", metadata.channels},
              {"data_type", std::move(data_type)},
              {"device", std::move(device)},
              {"row_step", metadata.row_step},
              {"byte_size", metadata.byte_size},
              {"filesystem_device", metadata.filesystem_device},
              {"inode", metadata.inode}};
}

/**
 * @brief Matches the session-control methods implemented by one route family.
 *
 * @param method Exact decoded request method.
 * @return True for graph mutation, node YAML, cache, dirty, ROI, timing,
 *         last-IO, or last-error routing.
 * @throws Nothing.
 * @note Capability admission remains owned by the exact 60-method version
 *       table; this independent matcher lets tests detect missing session
 *       dispatch.
 */
bool is_session_control_method(std::string_view method) noexcept {
  static constexpr std::array<std::string_view, 19> kMethods = {
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
      "inspect.roi_backward",
      "inspect.roi_forward",
  };
  return std::find(kMethods.begin(), kMethods.end(), method) != kMethods.end();
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
 * @return Nothing.
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

/**
 * @brief Decoded stable-collection page controls.
 *
 * @throws std::bad_alloc when cursor storage is copied.
 * @note An absent cursor denotes the one Host-backed first request. A present
 *       cursor requires the exact next offset and performs snapshot-only IO.
 */
struct CollectionPageRequest {
  /** @brief Opaque continuation cursor, absent for the first page. */
  std::optional<std::string> cursor;
  /** @brief Required zero-based continuation offset, or zero initially. */
  std::size_t offset = 0;
  /** @brief Caller page ceiling in `1..4096`. */
  std::size_t limit = kGeneralPageMaxEntries;
};

/**
 * @brief One traversal-order map row retained by stable paging.
 *
 * @throws std::bad_alloc when its node-id vector is copied.
 * @note A map branch is indivisible on the wire; its nested id array is
 *       independently bounded by the general page limit.
 */
struct TraversalOrderRow {
  /** @brief Nonnegative ending-node map key. */
  int ending_node_id = -1;
  /** @brief Ordered traversal node ids for this ending node. */
  std::vector<NodeId> node_ids;
};

/**
 * @brief One traversal-detail map row retained by stable paging.
 *
 * @throws std::bad_alloc when copied names or vectors allocate.
 * @note A complete ending-node branch remains one indivisible page entry.
 */
struct TraversalDetailRow {
  /** @brief Nonnegative ending-node map key. */
  int ending_node_id = -1;
  /** @brief Ordered copied traversal metadata for the branch. */
  std::vector<HostTraversalNodeSnapshot> nodes;
};

/**
 * @brief One sorted operation-plugin source-map row retained by stable paging.
 *
 * @throws std::bad_alloc when copied key or source storage allocates.
 * @note Both fields are copied Host values. The row contains no loader,
 *       callback, registry, factory, or dynamic-library ownership state.
 */
struct PluginSourceRow {
  /** @brief Nonempty public operation key used as the deterministic sort key.
   */
  std::string key;
  /** @brief Copied source label or plugin path. */
  std::string source;
};

/**
 * @brief Immutable dependency-tree metadata shared by paged entry rows.
 *
 * @throws std::bad_alloc when root ids are copied.
 * @note The shared object is private cursor storage containing only copied
 *       public values; it has no Host, graph, or backend reference.
 */
struct DependencyTreePageHeader {
  /** @brief Public tree scope. */
  HostDependencyTreeScope scope = HostDependencyTreeScope::EndingNodes;
  /** @brief Optional requested start node. */
  std::optional<NodeId> start_node;
  /** @brief Whether the inspected graph was empty. */
  bool graph_empty = false;
  /** @brief Whether a requested start node existed. */
  bool start_node_found = true;
  /** @brief Whether an ending-node query found no roots. */
  bool no_ending_nodes = false;
  /** @brief Complete bounded root-id header repeated on each page. */
  std::vector<NodeId> root_nodes;
};

/**
 * @brief One paged dependency-tree entry plus immutable shared metadata.
 *
 * @throws std::bad_alloc when copied public entry state allocates.
 * @note Shared ownership avoids duplicating the root-id header for every
 *       retained entry while keeping continuation pages independent of Host.
 */
struct DependencyTreePageRow {
  /** @brief Shared copied header owned by the retained snapshot. */
  std::shared_ptr<const DependencyTreePageHeader> header;
  /** @brief One flattened public dependency-tree entry. */
  HostDependencyTreeEntry entry;
};

/**
 * @brief Matches the collection and remaining inspection route family.
 * @param method Exact candidate method.
 * @return True only for methods implemented by task-independent inspection
 *         routing in this source file.
 * @throws Nothing.
 * @note Capability admission remains owned by the exact 60-method version
 *       table; this independent matcher lets tests detect missing inspection
 *       dispatch.
 */
bool is_inspection_method(std::string_view method) noexcept {
  static constexpr std::array<std::string_view, 12> kMethods = {
      "graph.list",
      "inspect.compute_planning",
      "inspect.dependency_tree",
      "inspect.dirty_region",
      "inspect.ending_nodes",
      "inspect.graph",
      "inspect.node",
      "inspect.node_ids",
      "inspect.recent_compute_planning",
      "inspect.traversal_details",
      "inspect.traversal_orders",
      "inspect.trees_containing_node",
  };
  return std::find(kMethods.begin(), kMethods.end(), method) != kMethods.end();
}

/**
 * @brief Decodes optional first-page or required continuation controls.
 * @param params Structurally valid method params.
 * @param page Receives complete controls only after every known field passes.
 * @param message Receives a stable diagnostic on malformed controls.
 * @return True on success; false without partially publishing `page`.
 * @throws std::bad_alloc if cursor or diagnostic storage cannot allocate.
 * @note Unknown params remain forward-compatible. `offset` without `cursor`,
 *       a cursor without offset/limit, malformed ids, and arithmetic overflow
 *       are rejected before session resolution or Host access.
 */
bool read_collection_page(const Json& params, CollectionPageRequest* page,
                          std::string* message) {
  if (page == nullptr || message == nullptr) {
    return false;
  }
  CollectionPageRequest decoded;
  if (!params.contains("cursor")) {
    if (params.contains("offset")) {
      *message = "offset requires a continuation cursor";
      return false;
    }
    if (params.contains("limit") &&
        !decode_page_limit(params["limit"], 1, kGeneralPageMaxEntries,
                           &decoded.limit)) {
      *message = "limit must be an integer in 1..4096";
      return false;
    }
    *page = std::move(decoded);
    return true;
  }
  std::string cursor;
  if (!decode_opaque_id(params["cursor"], &cursor) ||
      !params.contains("offset") || !params.contains("limit") ||
      !decode_page_window(params["offset"], params["limit"],
                          kGeneralPageMaxEntries, &decoded.offset,
                          &decoded.limit)) {
    *message = "continuation requires cursor and exact offset/limit";
    return false;
  }
  decoded.cursor = std::move(cursor);
  *page = std::move(decoded);
  return true;
}

/**
 * @brief Maps one private snapshot-registry outcome to a wire status.
 * @param error Private bounded-registry result.
 * @return Stable Protocol or Daemon status; success maps defensively to an
 *         internal invariant failure and must not be returned to clients.
 * @throws std::bad_alloc if diagnostic storage cannot allocate.
 */
OperationStatus collection_error_status(CollectionSnapshotError error) {
  switch (error) {
    case CollectionSnapshotError::InvalidParams:
      return invalid_params("collection page controls are invalid");
    case CollectionSnapshotError::CapacityExceeded:
    case CollectionSnapshotError::Stopped:
      return failure_status(OperationErrorDomain::Daemon, kCapacityExceededCode,
                            "capacity_exceeded",
                            "collection snapshot capacity is unavailable");
    case CollectionSnapshotError::ResponseTooLarge:
      return failure_status(OperationErrorDomain::Protocol,
                            kResponseTooLargeCode, "response_too_large",
                            "complete collection exceeds snapshot bounds");
    case CollectionSnapshotError::CursorNotFound:
      return failure_status(OperationErrorDomain::Daemon, kCursorNotFoundCode,
                            "cursor_not_found",
                            "collection cursor is absent or mismatched");
    case CollectionSnapshotError::None:
      break;
  }
  return failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                        "internal_error",
                        "collection snapshot outcome invariant failed");
}

/**
 * @brief Builds a bounded whole-value size rejection.
 * @param id Correlated request id.
 * @param message Exact validation diagnostic.
 * @return Protocol `response_too_large` response payload.
 * @throws std::bad_alloc if bounded error construction cannot allocate.
 */
std::string collection_too_large(const std::string& id,
                                 std::string_view message) {
  return bounded_error(
      id, failure_status(OperationErrorDomain::Protocol, kResponseTooLargeCode,
                         "response_too_large", std::string(message)));
}

/**
 * @brief Appends canonical cursor metadata to one collection result.
 * @tparam T Stable copied page entry type.
 * @param result Result object that already contains method-specific values.
 * @param page Successful registry page.
 * @return Nothing.
 * @throws std::bad_alloc if JSON member storage cannot allocate.
 * @note `cursor` is always present and null on the final or only page.
 */
template <typename T>
void add_collection_page_metadata(
    Json* result, const CollectionSnapshotRegistry::PageResult<T>& page) {
  (*result)["offset"] = page.offset;
  (*result)["has_more"] = page.has_more;
  (*result)["cursor"] = page.cursor ? Json(*page.cursor) : Json(nullptr);
}

/**
 * @brief Adds worst-sized valid cursor metadata for frame budgeting.
 * @param result Method-specific result containing an empty target array.
 * @return Nothing.
 * @throws std::bad_alloc if JSON member storage cannot allocate.
 * @note The maximum offset and fixed 32-byte cursor conservatively cover every
 *       first or continuation response without changing the real wire value.
 */
void add_worst_collection_page_metadata(Json* result) {
  (*result)["offset"] = std::numeric_limits<std::size_t>::max();
  (*result)["has_more"] = true;
  (*result)["cursor"] = std::string(32, 'f');
}

/**
 * @brief Adds two sizes without allowing quota-measurement wrap.
 * @param current Current byte count.
 * @param addition Bytes to add.
 * @return Checked sum.
 * @throws std::length_error on size_t overflow.
 */
std::size_t checked_size_sum(std::size_t current, std::size_t addition) {
  if (addition > std::numeric_limits<std::size_t>::max() - current) {
    throw std::length_error("collection byte measurement overflowed");
  }
  return current + addition;
}

/**
 * @brief Adds recursive public collection entries without overflow.
 * @param current Entries already measured for the complete snapshot.
 * @param addition Newly observed vector, map, or fixed-array entries.
 * @param context Snapshot family used by the stable rejection diagnostic.
 * @return Exact checked sum while it remains within the production snapshot
 *         entry bound.
 * @throws std::length_error on arithmetic overflow or more than 262,144
 *         recursively visible entries.
 * @note Injected limits smaller than production remain authoritative in
 *       `CollectionSnapshotRegistry::publish`; this pre-scan prevents large
 *       router-owned copies for values that exceed the production contract.
 */
std::size_t checked_entry_sum(std::size_t current, std::size_t addition,
                              std::string_view context) {
  if (addition > std::numeric_limits<std::size_t>::max() - current ||
      current + addition > kSnapshotMaxEntries) {
    throw std::length_error(std::string(context) +
                            " exceeds 262144 recursive snapshot entries");
  }
  return current + addition;
}

/**
 * @brief Counts nested public collection entries in one copied node.
 * @param node Complete Host-returned node snapshot.
 * @param context Owning snapshot family for bounded rejection diagnostics.
 * @return Parameter-map entries plus the 27 values in three public 3x3
 *         matrices when spatial metadata is present.
 * @throws std::length_error if the recursive production entry bound is
 *         exceeded.
 * @note The caller separately counts the node's containing vector/map entry.
 *       Scalar object members and optional-presence markers are not collection
 *       entries.
 */
std::size_t node_nested_entry_count(const NodeInspectionView& node,
                                    std::string_view context) {
  std::size_t entries = checked_entry_sum(0, node.parameters.size(), context);
  if (node.space) {
    static constexpr std::size_t kSpatialMatrixEntries = 3U * 9U;
    entries = checked_entry_sum(entries, kSpatialMatrixEntries, context);
  }
  return entries;
}

/**
 * @brief Counts graph rows and all nested public node collections.
 * @param nodes Complete copied graph node vector.
 * @return Exact recursive snapshot entry count.
 * @throws std::length_error on overflow or the production entry bound.
 */
std::size_t graph_entry_count(const std::vector<NodeInspectionView>& nodes) {
  std::size_t entries = checked_entry_sum(0, nodes.size(), "graph inspection");
  for (const NodeInspectionView& node : nodes) {
    entries = checked_entry_sum(
        entries, node_nested_entry_count(node, "graph inspection"),
        "graph inspection");
  }
  return entries;
}

/**
 * @brief Pre-scans one dependency tree before shared-header allocation.
 * @param tree Complete Host-returned dependency value.
 * @return Exact root, flattened-entry, parameter-map, and spatial-matrix
 *         entry count.
 * @throws std::length_error for an over-limit root page, arithmetic overflow,
 *         or the production recursive entry bound.
 * @note This function allocates nothing and must run before copying roots into
 *       the retained shared header or transforming flattened entries.
 */
std::size_t dependency_tree_entry_count(
    const HostDependencyTreeSnapshot& tree) {
  if (tree.root_nodes.size() > kGeneralPageMaxEntries) {
    throw std::length_error("dependency root_node_ids exceed 4096 entries");
  }
  std::size_t entries = checked_entry_sum(0, tree.root_nodes.size(),
                                          "dependency tree inspection");
  entries = checked_entry_sum(entries, tree.entries.size(),
                              "dependency tree inspection");
  for (const HostDependencyTreeEntry& entry : tree.entries) {
    entries = checked_entry_sum(
        entries,
        node_nested_entry_count(entry.node, "dependency tree inspection"),
        "dependency tree inspection");
  }
  return entries;
}

/**
 * @brief Pre-scans traversal-order map and nested node-id vectors.
 * @param orders Complete Host-returned ending-node map.
 * @return Exact map-entry plus nested-vector entry count.
 * @throws std::length_error for an indivisible over-limit branch, overflow,
 *         or the production recursive entry bound.
 * @note This function allocates nothing and runs before map-to-vector
 *       transformation.
 */
std::size_t traversal_order_entry_count(
    const std::map<int, std::vector<NodeId>>& orders) {
  std::size_t entries = checked_entry_sum(0, orders.size(), "traversal orders");
  for (const auto& branch : orders) {
    if (branch.second.size() > kGeneralPageMaxEntries) {
      throw std::length_error(
          "one traversal-order branch exceeds 4096 node ids");
    }
    entries =
        checked_entry_sum(entries, branch.second.size(), "traversal orders");
  }
  return entries;
}

/**
 * @brief Pre-scans traversal-detail map and nested node vectors.
 * @param details Complete Host-returned ending-node map.
 * @return Exact map-entry plus nested-vector entry count.
 * @throws std::length_error for an indivisible over-limit branch, overflow,
 *         or the production recursive entry bound.
 * @note This function allocates nothing and runs before map-to-vector
 *       transformation.
 */
std::size_t traversal_detail_entry_count(
    const std::map<int, std::vector<HostTraversalNodeSnapshot>>& details) {
  std::size_t entries =
      checked_entry_sum(0, details.size(), "traversal details");
  for (const auto& branch : details) {
    if (branch.second.size() > kGeneralPageMaxEntries) {
      throw std::length_error("one traversal-detail branch exceeds 4096 nodes");
    }
    entries =
        checked_entry_sum(entries, branch.second.size(), "traversal details");
  }
  return entries;
}

/**
 * @brief Counts one planning value's public sample collections.
 * @param snapshot Complete copied planning snapshot.
 * @param current Entries already counted by the owning history vector.
 * @return Exact count after planned-node, task, and task-dependency entries.
 * @throws std::length_error on overflow or the production recursive bound.
 */
std::size_t add_planning_nested_entry_count(
    const ComputePlanningInspectionSnapshot& snapshot, std::size_t current) {
  current = checked_entry_sum(current, snapshot.planned_node_sample.size(),
                              "recent compute planning");
  current = checked_entry_sum(current, snapshot.task_sample.size(),
                              "recent compute planning");
  for (const ComputePlanningTaskSnapshot& task : snapshot.task_sample) {
    current = checked_entry_sum(current, task.dependency_task_ids.size(),
                                "recent compute planning");
  }
  return current;
}

/**
 * @brief Counts history rows and every nested planning sample collection.
 * @param snapshots Complete Host-returned recent-planning history.
 * @return Exact recursive snapshot entry count.
 * @throws std::length_error on overflow or the production entry bound.
 */
std::size_t recent_planning_entry_count(
    const std::vector<ComputePlanningInspectionSnapshot>& snapshots) {
  std::size_t entries =
      checked_entry_sum(0, snapshots.size(), "recent compute planning");
  for (const ComputePlanningInspectionSnapshot& snapshot : snapshots) {
    entries = add_planning_nested_entry_count(snapshot, entries);
  }
  return entries;
}

/**
 * @brief Returns the exact compact JSON byte count of one string token.
 * @param value Raw bytes whose validity is checked by the typed encoder.
 * @return Quotes plus the exact escaping size used by compact JSON output.
 * @throws std::length_error on size arithmetic overflow.
 * @note This bounded preflight does not replace UTF-8 or component validation;
 *       it only prevents construction of an inevitably oversized JSON DOM.
 */
std::size_t encoded_json_string_bytes(std::string_view value) {
  std::size_t bytes = 2;
  for (const unsigned char byte : value) {
    std::size_t addition = 1;
    switch (byte) {
      case '"':
      case '\\':
      case '\b':
      case '\t':
      case '\n':
      case '\f':
      case '\r':
        addition = 2;
        break;
      default:
        addition = byte <= 0x1fU ? 6U : 1U;
        break;
    }
    bytes = checked_size_sum(bytes, addition);
  }
  return bytes;
}

/**
 * @brief Returns a necessary compact-JSON byte lower bound for an integer
 *        array.
 * @param entry_count Number of integer elements in the array.
 * @return Two bracket bytes plus at least one digit per element and one comma
 *         between adjacent elements.
 * @throws std::length_error when the lower-bound arithmetic overflows.
 * @note Signed values and multi-digit values require more bytes, so this
 *       calculation never overestimates the serialized array size. Typed
 *       encoding remains responsible for rejecting negative ids.
 */
std::size_t minimum_integer_array_bytes(std::size_t entry_count) {
  std::size_t bytes = 2;
  if (entry_count == 0) {
    return bytes;
  }
  bytes = checked_size_sum(bytes, entry_count);
  return checked_size_sum(bytes, entry_count - 1U);
}

/**
 * @brief Rejects an inevitably oversized node before JSON object allocation.
 * @param node Complete Host-returned public node value.
 * @return Nothing.
 * @throws std::length_error when escaped strings and minimal structure alone
 *         exceed one version 2 frame.
 * @note Every counted string token appears exactly once in the compact JSON;
 *       omitted object punctuation and scalar fields make this a necessary
 *       lower bound rather than an estimate. `encode_node()` remains
 *       authoritative for UTF-8, enum, id, component, and exact final shape
 *       validation.
 */
void require_node_preencoding_budget(const NodeInspectionView& node) {
  std::size_t bytes = 0;
  const auto add_text = [&](std::string_view text) {
    bytes = checked_size_sum(bytes, encoded_json_string_bytes(text));
    if (bytes > kMaximumFramePayloadBytes) {
      throw std::length_error("one node snapshot exceeds one frame");
    }
  };
  add_text(node.name);
  add_text(node.type);
  add_text(node.subtype);
  for (const auto& parameter : node.parameters) {
    add_text(parameter.first);
    add_text(parameter.second);
  }
  if (node.source_label) {
    add_text(*node.source_label);
  }
  if (node.debug) {
    add_text(node.debug->compute_device);
  }
  if (bytes > kMaximumFramePayloadBytes) {
    throw std::length_error("one node snapshot exceeds one frame");
  }
}

/**
 * @brief Rejects an inevitably oversized planning snapshot before encoding.
 * @param snapshot Complete Host-returned public planning value.
 * @return Nothing.
 * @throws std::length_error when necessary escaped-string and integer-array
 *         tokens alone exceed one version 2 frame, or size arithmetic
 *         overflows.
 * @note Current planning is an indivisible direct value and therefore does
 *       not use the stable collection snapshot entry quota. This preflight
 *       counts only tokens guaranteed to occur in compact JSON, preventing an
 *       inevitably oversized dependency matrix from constructing a huge DOM
 *       without rejecting any potentially frame-safe value. The typed encoder
 *       and final serializer remain authoritative for component validation
 *       and exact frame size.
 */
void require_planning_preencoding_budget(
    const ComputePlanningInspectionSnapshot& snapshot) {
  std::size_t bytes = encoded_json_string_bytes(snapshot.expansion_cache_key);
  bytes = checked_size_sum(
      bytes, minimum_integer_array_bytes(snapshot.planned_node_sample.size()));
  for (const ComputePlanningTaskSnapshot& task : snapshot.task_sample) {
    bytes = checked_size_sum(bytes, encoded_json_string_bytes(task.kind));
    bytes = checked_size_sum(
        bytes, minimum_integer_array_bytes(task.dependency_task_ids.size()));
    if (bytes > kMaximumFramePayloadBytes) {
      throw std::length_error("one planning snapshot exceeds one frame");
    }
  }
  if (bytes > kMaximumFramePayloadBytes) {
    throw std::length_error("one planning snapshot exceeds one frame");
  }
}

/**
 * @brief Snapshot measurement plus a frame-safe per-page entry ceiling.
 *
 * @throws Nothing for value operations.
 * @note `page_limit` is no greater than the caller ceiling and is safe for
 *       every contiguous retained page, including a 128-byte request id.
 */
struct CollectionMeasurement {
  /** @brief Exact recursive public vector/map/fixed-array entry count. */
  std::size_t entries = 0;
  /** @brief Compact encoded complete-array byte count. */
  std::size_t bytes = 2;
  /** @brief Maximum entries the registry may copy in any one page. */
  std::size_t page_limit = 1;
};

/**
 * @brief Measures a collection one encoded public row at a time.
 * @tparam T Stable public row type.
 * @tparam Encoder Callable returning one validated row JSON value.
 * @param rows Complete Host-returned rows.
 * @param encoder Typed row encoder.
 * @param empty_page_result Method-specific result with an empty target array,
 *        worst-case string cursor, maximum offset, and `has_more: true`.
 * @param requested_limit Caller-selected page ceiling.
 * @param measured_entries Exact recursive entry count from a pre-scan that
 *        includes the outer rows and every nested public collection.
 * @return Snapshot bytes plus the largest fixed page size whose every
 *         contiguous window fits the 16 MiB response frame.
 * @throws std::length_error when the snapshot or one indivisible row exceeds
 *         its version 2 byte bound.
 * @throws std::invalid_argument for malformed Host-returned public values.
 * @throws std::bad_alloc if bounded per-row encoding cannot allocate.
 * @note No complete collection DOM is constructed. Each row is discarded
 *       after measurement, so oversize rejection precedes cursor publication.
 */
template <typename T, typename Encoder>
CollectionMeasurement measure_collection_rows(const std::vector<T>& rows,
                                              Encoder&& encoder,
                                              const Json& empty_page_result,
                                              std::size_t requested_limit,
                                              std::size_t measured_entries) {
  if (measured_entries > kSnapshotMaxEntries) {
    throw std::length_error("collection exceeds 262144 snapshot entries");
  }
  const Json empty_response{{"protocol_version", kProtocolVersion},
                            {"id", std::string(kRequestTextMaxBytes, '\x01')},
                            {"result", empty_page_result}};
  const std::size_t empty_payload_bytes = empty_response.dump().size();
  if (empty_payload_bytes > kMaximumFramePayloadBytes) {
    throw std::length_error("collection page header exceeds one frame");
  }
  const std::size_t array_budget =
      kMaximumFramePayloadBytes - empty_payload_bytes;

  std::size_t bytes = 2;
  std::vector<std::size_t> row_bytes;
  row_bytes.reserve(rows.size());
  bool first = true;
  for (const T& row : rows) {
    const std::string encoded = encoder(row).dump();
    if (encoded.size() > array_budget) {
      throw std::length_error("one collection entry exceeds one frame");
    }
    row_bytes.push_back(encoded.size());
    if (!first) {
      bytes = checked_size_sum(bytes, 1);
    }
    first = false;
    bytes = checked_size_sum(bytes, encoded.size());
    if (bytes > kSnapshotMaxBytes) {
      throw std::length_error("collection exceeds 64 MiB snapshot bytes");
    }
  }
  if (rows.empty()) {
    return {measured_entries, bytes, requested_limit};
  }

  const auto windows_fit = [&](std::size_t count) {
    std::size_t window = count - 1;
    for (std::size_t index = 0; index < count; ++index) {
      window = checked_size_sum(window, row_bytes[index]);
    }
    if (window > array_budget) {
      return false;
    }
    for (std::size_t begin = 1; begin + count <= row_bytes.size(); ++begin) {
      window -= row_bytes[begin - 1];
      window += row_bytes[begin + count - 1];
      if (window > array_budget) {
        return false;
      }
    }
    return true;
  };

  std::size_t lower = 1;
  std::size_t upper = std::min(requested_limit, rows.size());
  while (lower < upper) {
    const std::size_t middle = lower + (upper - lower + 1) / 2;
    if (windows_fit(middle)) {
      lower = middle;
    } else {
      upper = middle - 1;
    }
  }
  return {measured_entries, bytes, lower};
}

/**
 * @brief Encodes one traversal-order branch.
 * @param row Copied public map row.
 * @return Snake-case branch object.
 * @throws std::length_error if the nested branch exceeds 4,096 ids.
 * @throws std::invalid_argument for a negative map key or node id.
 * @throws std::bad_alloc if JSON storage cannot allocate.
 */
Json encode_traversal_order_row(const TraversalOrderRow& row) {
  if (row.ending_node_id < 0) {
    throw std::invalid_argument("traversal order has a negative ending node");
  }
  return Json{{"ending_node_id", row.ending_node_id},
              {"node_ids", encode_node_ids(row.node_ids)}};
}

/**
 * @brief Encodes one traversal-detail branch.
 * @param row Copied public map row.
 * @return Snake-case branch object with ordered node metadata.
 * @throws std::length_error if the branch or one label exceeds a wire bound.
 * @throws std::invalid_argument for negative ids.
 * @throws std::bad_alloc if JSON storage cannot allocate.
 */
Json encode_traversal_detail_row(const TraversalDetailRow& row) {
  if (row.ending_node_id < 0) {
    throw std::invalid_argument(
        "traversal details have a negative ending node");
  }
  if (row.nodes.size() > kGeneralPageMaxEntries) {
    throw std::length_error("one traversal-detail branch exceeds 4096 nodes");
  }
  Json nodes = Json::array();
  for (const HostTraversalNodeSnapshot& node : row.nodes) {
    if (node.node.value < 0) {
      throw std::invalid_argument("traversal detail has a negative node id");
    }
    if (node.name.size() > kShortTextMaxBytes) {
      throw std::length_error("traversal node name exceeds its wire bound");
    }
    if (!valid_utf8(node.name)) {
      throw std::invalid_argument("traversal node name is not valid UTF-8");
    }
    nodes.push_back(Json{{"node_id", node.node.value},
                         {"name", node.name},
                         {"has_memory_cache", node.has_memory_cache},
                         {"has_disk_cache", node.has_disk_cache}});
  }
  return Json{{"ending_node_id", row.ending_node_id},
              {"nodes", std::move(nodes)}};
}

/**
 * @brief Encodes one copied compute-planning task sample.
 * @param task Public planning task value.
 * @return Snake-case task object.
 * @throws std::length_error for an over-limit kind or dependency list.
 * @throws std::invalid_argument for invalid ids or enum values.
 * @throws std::bad_alloc if JSON storage cannot allocate.
 */
Json encode_planning_task(const ComputePlanningTaskSnapshot& task) {
  if (task.task_id < 0 || task.node.value < 0) {
    throw std::invalid_argument("planning task contains a negative id");
  }
  if (task.kind.size() > kShortTextMaxBytes) {
    throw std::length_error("planning task kind exceeds its wire bound");
  }
  if (!valid_utf8(task.kind)) {
    throw std::invalid_argument("planning task kind is not valid UTF-8");
  }
  if (task.dependency_task_ids.size() > kGeneralPageMaxEntries) {
    throw std::length_error("planning task dependencies exceed 4096 entries");
  }
  if (std::any_of(task.dependency_task_ids.begin(),
                  task.dependency_task_ids.end(),
                  [](int dependency) { return dependency < 0; })) {
    throw std::invalid_argument(
        "planning task dependencies contain a negative id");
  }
  Json domain;
  if (!encode_enum(task.domain, &domain)) {
    throw std::invalid_argument("planning task domain has no wire label");
  }
  return Json{{"task_id", task.task_id},
              {"node_id", task.node.value},
              {"kind", task.kind},
              {"domain", std::move(domain)},
              {"output_roi", encode_pixel_rect(task.output_roi)},
              {"tile_x", task.tile_x},
              {"tile_y", task.tile_y},
              {"tile_size", task.tile_size},
              {"whole_output", task.whole_output},
              {"dirty_selected", task.dirty_selected},
              {"dirty_generation", task.dirty_generation},
              {"dependency_task_ids", task.dependency_task_ids}};
}

/**
 * @brief Encodes one indivisible compute-planning snapshot.
 * @param snapshot Complete copied public planning value.
 * @return Snake-case planning object.
 * @throws std::length_error for over-limit nested arrays or strings.
 * @throws std::invalid_argument for invalid ids or enum values.
 * @throws std::bad_alloc if JSON storage cannot allocate.
 * @note Current planning is one indivisible direct value. Recent planning
 *       treats each complete planning object as one indivisible row: multiple
 *       small rows may share a page, while a single oversized row is rejected
 *       and the aggregate page count is limited by the measured dynamic frame
 *       ceiling.
 */
Json encode_compute_planning(
    const ComputePlanningInspectionSnapshot& snapshot) {
  require_planning_preencoding_budget(snapshot);
  if (snapshot.target_node.value < 0) {
    throw std::invalid_argument("planning snapshot has a negative target node");
  }
  if (snapshot.expansion_cache_key.size() > kLargeTextMaxBytes) {
    throw std::length_error("planning cache key exceeds its wire bound");
  }
  if (!valid_utf8(snapshot.expansion_cache_key)) {
    throw std::invalid_argument("planning cache key is not valid UTF-8");
  }
  if (snapshot.planned_node_sample.size() > kGeneralPageMaxEntries ||
      snapshot.task_sample.size() > kGeneralPageMaxEntries) {
    throw std::length_error("planning sample exceeds 4096 entries");
  }
  Json intent;
  if (!encode_enum(snapshot.intent, &intent)) {
    throw std::invalid_argument("planning intent has no wire label");
  }
  Json planned_nodes = encode_node_ids(snapshot.planned_node_sample);
  Json tasks = Json::array();
  for (const ComputePlanningTaskSnapshot& task : snapshot.task_sample) {
    tasks.push_back(encode_planning_task(task));
  }
  return Json{
      {"intent", std::move(intent)},
      {"target_node_id", snapshot.target_node.value},
      {"parallel", snapshot.parallel},
      {"topology_generation", snapshot.topology_generation},
      {"expansion_cache_key", snapshot.expansion_cache_key},
      {"planned_node_count", snapshot.planned_node_count},
      {"task_count", snapshot.task_count},
      {"tile_task_count", snapshot.tile_task_count},
      {"monolithic_task_count", snapshot.monolithic_task_count},
      {"node_task_count", snapshot.node_task_count},
      {"dependency_count", snapshot.dependency_count},
      {"initial_task_count", snapshot.initial_task_count},
      {"active_task_count", snapshot.active_task_count},
      {"dirty_source_task_count", snapshot.dirty_source_task_count},
      {"downstream_task_count", snapshot.downstream_task_count},
      {"initial_downstream_task_count", snapshot.initial_downstream_task_count},
      {"planned_node_sample", std::move(planned_nodes)},
      {"task_sample", std::move(tasks)}};
}

/**
 * @brief Owned validated values from one complete request envelope.
 *
 * @throws std::bad_alloc when owned id or method text cannot be allocated.
 * @note The value owns only the decoded request id, method, and params subtree;
 *       it owns no backend/session state and performs no routing by itself.
 */
struct DecodedRequestEnvelope {
  /** @brief Correlated nonempty bounded request id. */
  std::string id;

  /** @brief Advertised nonempty bounded version 2 method. */
  std::string method;

  /** @brief Structurally valid params object owned independently of parser. */
  Json params = Json::object();
};

/**
 * @brief Result of request parsing and structural envelope validation.
 *
 * @throws std::bad_alloc when owned request or bounded error storage cannot be
 *         allocated.
 * @note Exactly one of `request` and `error_response` is populated. Duplicate
 *       key handling preserves only an unambiguous valid top-level id.
 */
struct DecodedRequestEnvelopeResult {
  /** @brief Validated request view on success. */
  std::optional<DecodedRequestEnvelope> request;

  /** @brief Complete bounded failure response when validation fails. */
  std::string error_response;
};

/**
 * @brief Parses and validates the common version 2 request envelope.
 *
 * @param payload Exact framed JSON payload.
 * @return Owned decoded request or a complete correlated protocol error.
 * @throws std::bad_alloc if parser, owned field, or error-response allocation
 *         fails.
 * @note Method advertisement is validated after protocol/envelope shape and
 *       before any Host/session access. This helper performs no routing,
 *       mutation, socket IO, or retry.
 */
DecodedRequestEnvelopeResult decode_request_envelope(
    const std::string& payload) {
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
    return {
        std::nullopt,
        bounded_error(
            response_id,
            failure_status(
                OperationErrorDomain::Protocol,
                parsed.duplicate_key ? kInvalidRequestCode : kParseErrorCode,
                parsed.duplicate_key ? "invalid_request" : "parse_error",
                parsed.message))};
  }
  if (!parsed.value.is_object()) {
    return {std::nullopt,
            bounded_error(
                nullptr, failure_status(OperationErrorDomain::Protocol,
                                        kInvalidRequestCode, "invalid_request",
                                        "request envelope must be an object"))};
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
  std::string method;
  const bool valid_method =
      request.contains("method") &&
      decode_bounded_string(request["method"], kRequestTextMaxBytes, &method) &&
      !method.empty();
  if (!request.value("protocol_version", Json()).is_number_integer() ||
      !response_id.is_string() || !valid_method ||
      !request.value("params", Json()).is_object()) {
    return {std::nullopt,
            bounded_error(response_id,
                          failure_status(
                              OperationErrorDomain::Protocol,
                              kInvalidRequestCode, "invalid_request",
                              "request requires integer protocol_version, "
                              "valid id, nonempty method, and object params"))};
  }

  const std::string id = response_id.get<std::string>();
  std::int32_t request_version = 0;
  if (!decode_integer(request["protocol_version"], &request_version) ||
      request_version != kProtocolVersion) {
    return {std::nullopt,
            bounded_error(
                id,
                failure_status(OperationErrorDomain::Protocol,
                               kUnsupportedProtocolCode, "unsupported_protocol",
                               "requested protocol version is not supported"),
                true)};
  }
  if (!is_version_two_method(method)) {
    return {std::nullopt,
            bounded_error(
                id, failure_status(
                        OperationErrorDomain::Protocol, kMethodNotFoundCode,
                        "method_not_found",
                        "method is not implemented by protocol version 2"))};
  }

  DecodedRequestEnvelope decoded{id, std::move(method), request["params"]};
  return {std::move(decoded), {}};
}

}  // namespace

/** @copydoc valid_output_delivery_for_wire */
bool valid_output_delivery_for_wire(
    const OutputArtifactDelivery& delivery,
    const std::string& expected_output_reference) noexcept {
  const OutputArtifactMetadata& metadata = delivery.metadata;
  std::size_t scalar_bytes = 0;
  switch (metadata.data_type) {
    case DataType::UINT8:
    case DataType::INT8:
      scalar_bytes = 1;
      break;
    case DataType::UINT16:
    case DataType::INT16:
      scalar_bytes = 2;
      break;
    case DataType::FLOAT32:
      scalar_bytes = 4;
      break;
    case DataType::FLOAT64:
      scalar_bytes = 8;
      break;
    default:
      return false;
  }
  if (!valid_opaque_id(metadata.output_id) ||
      metadata.output_id != expected_output_reference ||
      !valid_opaque_id(delivery.delivery_id) ||
      !valid_absolute_path(metadata.path, false) ||
      metadata.device != Device::CPU || metadata.width <= 0 ||
      metadata.height <= 0 || metadata.channels <= 0) {
    return false;
  }
  const std::size_t width = static_cast<std::size_t>(metadata.width);
  const std::size_t height = static_cast<std::size_t>(metadata.height);
  const std::size_t channels = static_cast<std::size_t>(metadata.channels);
  const std::size_t maximum = std::numeric_limits<std::size_t>::max();
  if (width > maximum / channels || width * channels > maximum / scalar_bytes) {
    return false;
  }
  const std::size_t expected_row_step = width * channels * scalar_bytes;
  return metadata.row_step == expected_row_step &&
         height <= maximum / expected_row_step &&
         metadata.byte_size == expected_row_step * height;
}

/**
 * @brief Complete internal parsed-params adapter.
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

/**
 * @brief Complete validated state for one inspection route.
 *
 * @throws std::bad_alloc when method, cursor, or binding text cannot allocate.
 * @note The value owns decoded wire identities only. It owns no registry
 *       admission, Host identity, snapshot reservation, or backend state.
 */
struct RequestRouter::InspectionRequest {
  /** @brief Exact recognized inspection method. */
  std::string method;

  /** @brief Valid opaque session id, empty only for `graph.list`. */
  IpcSessionId session_id;

  /** @brief Optional node selector for node/tree inspection. */
  std::optional<NodeId> optional_node;

  /** @brief Exact dependency metadata intent. */
  bool include_metadata = false;

  /** @brief Validated first-page or continuation controls. */
  CollectionPageRequest page_request;

  /** @brief Exact cursor identity frozen across pages. */
  CollectionSnapshotBinding binding;
};

/**
 * @brief Complete decoded values for one session-control method.
 *
 * @throws std::bad_alloc when owned text cannot be allocated.
 * @note The value owns only wire scalars/text. Registry admission and the Host
 *       identity remain scoped to the dispatch call after validation succeeds.
 */
struct RequestRouter::SessionControlRequest {
  /** @brief Valid daemon-opaque session identity. */
  IpcSessionId session_id;

  /** @brief Method-specific path, YAML, or precision text. */
  std::string text;

  /** @brief First method-specific node identity. */
  NodeId first_node;

  /** @brief Second method-specific node identity. */
  NodeId second_node;

  /** @brief Exact method-specific ROI. */
  PixelRect roi;

  /** @brief Exact dirty domain for dirty lifecycle calls. */
  DirtyDomain dirty_domain = DirtyDomain::HighPrecision;
};

/**
 * @brief Complete validated identity for one stable collection route.
 *
 * @throws std::bad_alloc when method, cursor, or binding text cannot allocate.
 * @note The value owns page and frozen-binding inputs only. Snapshot
 *       reservation/publication remains transactional inside first-page
 *       helpers; continuations never acquire Host or session state.
 */
struct RequestRouter::StableCollectionRequest {
  /** @brief Exact recognized collection method. */
  std::string method;

  /** @brief Validated cursor, offset, and requested page bound. */
  CollectionPageRequest page_request;

  /** @brief Exact method/session/original-params frozen identity. */
  CollectionSnapshotBinding binding;
};

/** @copydoc RequestRouter::RequestRouter */
RequestRouter::RequestRouter(Host& host, std::string service_version)
    : RequestRouter(host, std::move(service_version),
                    RequestRouterRuntimeDependencies{}) {
}  // NOLINT(whitespace/indent_namespace)

/** @copydoc RequestRouter::RequestRouter */
RequestRouter::RequestRouter(Host& host, std::string service_version,
                             RequestRouterRuntimeDependencies dependencies)
    : host_(host),
      collection_snapshots_(dependencies.snapshot_limits,
                            std::move(dependencies.snapshot_clock),
                            std::move(dependencies.snapshot_id_generator)),
      output_store_(dependencies.output_limits,
                    std::move(dependencies.output_clock),
                    std::move(dependencies.output_id_generator)),
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
          },  // NOLINT
          dependencies.compute_limits, std::move(dependencies.compute_clock),
          std::move(dependencies.compute_id_generator)),  // NOLINT
      service_version_(std::move(service_version)),       // NOLINT
      server_instance_id_(          // NOLINT(whitespace/indent_namespace)
          generate_opaque_id()) {}  // NOLINT

/** @copydoc RequestRouter::route_inspection_direct_value */
std::optional<std::string> RequestRouter::route_inspection_direct_value(
    const std::string& id, const InspectionRequest& request) {
  const bool direct_value = request.method == "inspect.node" ||
                            request.method == "inspect.dirty_region" ||
                            request.method == "inspect.compute_planning";
  if (!direct_value) {
    return std::nullopt;
  }
  IpcResult<SessionRegistry::HostCallAdmission> admission =
      registry_.admit_host_call(request.session_id);
  if (!admission.status.ok) {
    return bounded_error(id, admission.status);
  }
  if (request.method == "inspect.node") {
    Result<NodeInspectionView> inspected;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      inspected = host_.inspect_node(admission.value.host_session(),
                                     *request.optional_node);
    }
    if (!inspected.status.ok) {
      return bounded_error(id, graph_status(inspected.status));
    }
    return encode_routed_value(id, [&] {
      require_node_preencoding_budget(inspected.value);
      return Json{{"session_id", request.session_id.value},
                  {"node", encode_node(inspected.value)}};
    });
  }
  if (request.method == "inspect.dirty_region") {
    Result<DirtyRegionInspectionSnapshot> inspected;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      inspected = host_.dirty_region_snapshot(admission.value.host_session());
    }
    if (!inspected.status.ok) {
      return bounded_error(id, graph_status(inspected.status));
    }
    return encode_routed_value(id, [&] {
      return encode_dirty_region(id, request.session_id, inspected.value);
    });
  }
  Result<std::optional<ComputePlanningInspectionSnapshot>> inspected;
  {
    std::lock_guard<std::mutex> host_lock(host_mutex_);
    inspected = host_.compute_planning_snapshot(admission.value.host_session());
  }
  if (!inspected.status.ok) {
    return bounded_error(id, graph_status(inspected.status));
  }
  return encode_routed_value(id, [&] {
    return Json{
        {"session_id", request.session_id.value},
        {"planning", inspected.value ? encode_compute_planning(*inspected.value)
                                     : Json(nullptr)}};
  });
}

/** @copydoc RequestRouter::route_inspection_continuation */
std::string RequestRouter::route_inspection_continuation(
    const std::string& id, const InspectionRequest& request) {
  if (request.method == "graph.list" || request.method == "inspect.node_ids" ||
      request.method == "inspect.ending_nodes" ||
      request.method == "inspect.trees_containing_node" ||
      request.method == "inspect.graph") {
    return route_inspection_basic_continuation(id, request);
  }
  if (request.method == "inspect.dependency_tree") {
    return route_inspection_dependency_continuation(id, request);
  }
  if (request.method == "inspect.traversal_orders" ||
      request.method == "inspect.traversal_details") {
    return route_inspection_traversal_continuation(id, request);
  }
  if (request.method == "inspect.recent_compute_planning") {
    return route_inspection_planning_continuation(id, request);
  }
  return bounded_error(
      id, failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                         "internal_error",
                         "inspection continuation dispatch invariant failed"));
}

/** @copydoc RequestRouter::route_inspection_basic_continuation */
std::string RequestRouter::route_inspection_basic_continuation(
    const std::string& id, const InspectionRequest& request) {
  const CollectionPageRequest& page_request = request.page_request;
  if (request.method == "graph.list") {
    auto page = collection_snapshots_.page<GraphSessionSummary>(
        *page_request.cursor, request.binding, page_request.offset,
        page_request.limit);
    if (page.error != CollectionSnapshotError::None) {
      return bounded_error(id, collection_error_status(page.error));
    }
    return encode_routed_value(id, [&] {
      Json result{{"sessions", encode_session_summaries(page.entries)}};
      add_collection_page_metadata(&result, page);
      return result;
    });
  }
  if (request.method == "inspect.node_ids" ||
      request.method == "inspect.ending_nodes" ||
      request.method == "inspect.trees_containing_node") {
    auto page = collection_snapshots_.page<NodeId>(
        *page_request.cursor, request.binding, page_request.offset,
        page_request.limit);
    if (page.error != CollectionSnapshotError::None) {
      return bounded_error(id, collection_error_status(page.error));
    }
    return encode_routed_value(id, [&] {
      const char* field =
          request.method == "inspect.node_ids" ? "node_ids" : "ending_node_ids";
      Json result{{"session_id", request.session_id.value},
                  {field, encode_node_ids(page.entries)}};
      add_collection_page_metadata(&result, page);
      return result;
    });
  }
  if (request.method == "inspect.graph") {
    auto page = collection_snapshots_.page<NodeInspectionView>(
        *page_request.cursor, request.binding, page_request.offset,
        page_request.limit);
    if (page.error != CollectionSnapshotError::None) {
      return bounded_error(id, collection_error_status(page.error));
    }
    return encode_routed_value(id, [&] {
      GraphInspectionView graph;
      graph.nodes = page.entries;
      Json result = encode_graph(request.session_id, graph);
      add_collection_page_metadata(&result, page);
      return result;
    });
  }
  return bounded_error(
      id, failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                         "internal_error",
                         "basic continuation dispatch invariant failed"));
}

/** @copydoc RequestRouter::route_inspection_dependency_continuation */
std::string RequestRouter::route_inspection_dependency_continuation(
    const std::string& id, const InspectionRequest& request) {
  const CollectionPageRequest& page_request = request.page_request;
  auto page = collection_snapshots_.page<DependencyTreePageRow>(
      *page_request.cursor, request.binding, page_request.offset,
      page_request.limit);
  if (page.error != CollectionSnapshotError::None) {
    return bounded_error(id, collection_error_status(page.error));
  }
  if (page.entries.empty() || !page.entries.front().header) {
    return bounded_error(
        id, failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                           "internal_error",
                           "dependency cursor lost its public header"));
  }
  return encode_routed_value(id, [&] {
    const DependencyTreePageHeader& header = *page.entries.front().header;
    HostDependencyTreeSnapshot tree;
    tree.scope = header.scope;
    tree.start_node = header.start_node;
    tree.graph_empty = header.graph_empty;
    tree.start_node_found = header.start_node_found;
    tree.no_ending_nodes = header.no_ending_nodes;
    tree.root_nodes = header.root_nodes;
    for (const DependencyTreePageRow& row : page.entries) {
      tree.entries.push_back(row.entry);
    }
    Json result = encode_dependency_tree(request.session_id, tree);
    add_collection_page_metadata(&result, page);
    return result;
  });
}

/** @copydoc RequestRouter::route_inspection_traversal_continuation */
std::string RequestRouter::route_inspection_traversal_continuation(
    const std::string& id, const InspectionRequest& request) {
  const CollectionPageRequest& page_request = request.page_request;
  if (request.method == "inspect.traversal_orders") {
    auto page = collection_snapshots_.page<TraversalOrderRow>(
        *page_request.cursor, request.binding, page_request.offset,
        page_request.limit);
    if (page.error != CollectionSnapshotError::None) {
      return bounded_error(id, collection_error_status(page.error));
    }
    return encode_routed_value(id, [&] {
      Json rows = Json::array();
      for (const TraversalOrderRow& row : page.entries) {
        rows.push_back(encode_traversal_order_row(row));
      }
      Json result{{"session_id", request.session_id.value},
                  {"orders", std::move(rows)}};
      add_collection_page_metadata(&result, page);
      return result;
    });
  }
  if (request.method == "inspect.traversal_details") {
    auto page = collection_snapshots_.page<TraversalDetailRow>(
        *page_request.cursor, request.binding, page_request.offset,
        page_request.limit);
    if (page.error != CollectionSnapshotError::None) {
      return bounded_error(id, collection_error_status(page.error));
    }
    return encode_routed_value(id, [&] {
      Json rows = Json::array();
      for (const TraversalDetailRow& row : page.entries) {
        rows.push_back(encode_traversal_detail_row(row));
      }
      Json result{{"session_id", request.session_id.value},
                  {"branches", std::move(rows)}};
      add_collection_page_metadata(&result, page);
      return result;
    });
  }
  return bounded_error(
      id, failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                         "internal_error",
                         "traversal continuation dispatch invariant failed"));
}

/** @copydoc RequestRouter::route_inspection_planning_continuation */
std::string RequestRouter::route_inspection_planning_continuation(
    const std::string& id, const InspectionRequest& request) {
  const CollectionPageRequest& page_request = request.page_request;
  auto page = collection_snapshots_.page<ComputePlanningInspectionSnapshot>(
      *page_request.cursor, request.binding, page_request.offset,
      page_request.limit);
  if (page.error != CollectionSnapshotError::None) {
    return bounded_error(id, collection_error_status(page.error));
  }
  return encode_routed_value(id, [&] {
    Json snapshots = Json::array();
    for (const ComputePlanningInspectionSnapshot& snapshot : page.entries) {
      snapshots.push_back(encode_compute_planning(snapshot));
    }
    Json result{{"session_id", request.session_id.value},
                {"snapshots", std::move(snapshots)}};
    add_collection_page_metadata(&result, page);
    return result;
  });
}

/** @copydoc RequestRouter::route_inspection_graph_list_first_page */
std::string RequestRouter::route_inspection_graph_list_first_page(
    const std::string& id, InspectionRequest request) {
  auto reserved = collection_snapshots_.reserve();
  if (reserved.error != CollectionSnapshotError::None) {
    return bounded_error(id, collection_error_status(reserved.error));
  }
  Result<std::vector<GraphSessionId>> listed;
  IpcResult<std::vector<GraphSessionSummary>> reconciled;
  {
    std::lock_guard<std::mutex> host_lock(host_mutex_);
    listed = host_.list_graphs();
    if (listed.status.ok) {
      reconciled = registry_.reconcile(listed.value);
    }
  }
  if (!listed.status.ok) {
    return bounded_error(id, graph_status(listed.status));
  }
  if (!reconciled.status.ok) {
    return bounded_error(id, reconciled.status);
  }
  try {
    Json empty_page{{"sessions", Json::array()}};
    add_worst_collection_page_metadata(&empty_page);
    const CollectionMeasurement measured = measure_collection_rows(
        reconciled.value,
        [](const GraphSessionSummary& row) {
          return encode_session_summaries({row}).front();
        },
        empty_page, request.page_request.limit, reconciled.value.size());
    auto page = collection_snapshots_.publish(
        std::move(reserved.reservation), std::move(request.binding),
        std::move(reconciled.value), measured.entries, measured.bytes,
        request.page_request.limit, measured.page_limit);
    if (page.error != CollectionSnapshotError::None) {
      return bounded_error(id, collection_error_status(page.error));
    }
    Json result{{"sessions", encode_session_summaries(page.entries)}};
    add_collection_page_metadata(&result, page);
    return encode_success_response(id, std::move(result));
  } catch (const std::length_error& error) {
    return collection_too_large(id, error.what());
  }
}

/** @copydoc RequestRouter::decode_inspection_identity */
OperationStatus RequestRouter::decode_inspection_identity(
    const std::string& method, const RoutedParams& routed_params,
    InspectionRequest* request) {
  const Json& params = routed_params.value;
  request->method = method;
  if (method != "graph.list" &&
      !read_session_id(params, &request->session_id)) {
    return invalid_params(method + " requires a valid session_id");
  }
  if (method == "inspect.node" || method == "inspect.trees_containing_node") {
    NodeId node;
    if (!read_node_id(params, "node_id", &node)) {
      return invalid_params(method + " requires a nonnegative node_id");
    }
    request->optional_node = node;
    return ok_status();
  }
  if (method != "inspect.dependency_tree") {
    return ok_status();
  }
  if (params.contains("node_id") && !params["node_id"].is_null()) {
    NodeId node;
    if (!read_node_id(params, "node_id", &node)) {
      return invalid_params("node_id must be a nonnegative integer or null");
    }
    request->optional_node = node;
  }
  if (params.contains("include_metadata")) {
    if (!params["include_metadata"].is_boolean()) {
      return invalid_params("include_metadata must be boolean");
    }
    request->include_metadata = params["include_metadata"].get<bool>();
  }
  return ok_status();
}

/** @copydoc RequestRouter::decode_inspection_page_request */
OperationStatus RequestRouter::decode_inspection_page_request(
    const RoutedParams& routed_params, InspectionRequest* request) {
  std::string page_message;
  if (!read_collection_page(routed_params.value, &request->page_request,
                            &page_message)) {
    return invalid_params(std::move(page_message));
  }
  request->binding = CollectionSnapshotBinding{request->method,
                                               request->method == "graph.list"
                                                   ? std::string{}
                                                   : request->session_id.value,
                                               {}};
  if (request->method == "inspect.trees_containing_node") {
    request->binding.original_params =
        "node_id=" + std::to_string(request->optional_node->value);
  } else if (request->method == "inspect.dependency_tree") {
    request->binding.original_params =
        std::string("node_id=") +
        (request->optional_node ? std::to_string(request->optional_node->value)
                                : "null") +
        ";include_metadata=" + (request->include_metadata ? "true" : "false");
  }
  return ok_status();
}

/** @copydoc RequestRouter::route_inspection_method */
std::optional<std::string> RequestRouter::route_inspection_method(
    const std::string& id, const std::string& method,
    const RoutedParams& routed_params) {
  if (!is_inspection_method(method)) {
    return std::nullopt;
  }
  InspectionRequest request;
  const OperationStatus identity =
      decode_inspection_identity(method, routed_params, &request);
  if (!identity.ok) {
    return bounded_error(id, identity);
  }

  if (std::optional<std::string> routed =
          route_inspection_direct_value(id, request)) {
    return routed;
  }

  const OperationStatus page =
      decode_inspection_page_request(routed_params, &request);
  if (!page.ok) {
    return bounded_error(id, page);
  }

  if (request.page_request.cursor) {
    return route_inspection_continuation(id, request);
  }

  if (method == "graph.list") {
    return route_inspection_graph_list_first_page(id, std::move(request));
  }
  return route_inspection_first_page(id, std::move(request));
}

/** @copydoc RequestRouter::route_inspection_first_page */
std::string RequestRouter::route_inspection_first_page(
    const std::string& id, InspectionRequest request) {
  IpcResult<SessionRegistry::HostCallAdmission> admission =
      registry_.admit_host_call(request.session_id);
  if (!admission.status.ok) {
    return bounded_error(id, admission.status);
  }
  const GraphSessionId& host_session = admission.value.host_session();
  auto reserved = collection_snapshots_.reserve();
  if (reserved.error != CollectionSnapshotError::None) {
    return bounded_error(id, collection_error_status(reserved.error));
  }

  if (request.method == "inspect.node_ids" ||
      request.method == "inspect.ending_nodes" ||
      request.method == "inspect.trees_containing_node") {
    return route_inspection_node_list_first_page(
        id, std::move(request), host_session, std::move(reserved.reservation));
  }
  if (request.method == "inspect.graph") {
    return route_inspection_graph_first_page(
        id, std::move(request), host_session, std::move(reserved.reservation));
  }
  if (request.method == "inspect.dependency_tree") {
    return route_inspection_dependency_first_page(
        id, std::move(request), host_session, std::move(reserved.reservation));
  }
  if (request.method == "inspect.traversal_orders" ||
      request.method == "inspect.traversal_details") {
    return route_inspection_traversal_first_page(
        id, std::move(request), host_session, std::move(reserved.reservation));
  }
  if (request.method == "inspect.recent_compute_planning") {
    return route_inspection_planning_first_page(
        id, std::move(request), host_session, std::move(reserved.reservation));
  }
  return bounded_error(
      id, failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                         "internal_error",
                         "inspection first-page dispatch invariant failed"));
}

/** @copydoc RequestRouter::route_inspection_node_list_first_page */
std::string RequestRouter::route_inspection_node_list_first_page(
    const std::string& id, InspectionRequest request,
    const GraphSessionId& host_session,
    CollectionSnapshotRegistry::Reservation reservation) {
  const std::string& method = request.method;
  const IpcSessionId& session_id = request.session_id;
  Result<std::vector<NodeId>> inspected;
  {
    std::lock_guard<std::mutex> host_lock(host_mutex_);
    if (method == "inspect.node_ids") {
      inspected = host_.list_node_ids(host_session);
    } else if (method == "inspect.ending_nodes") {
      inspected = host_.ending_nodes(host_session);
    } else {
      inspected =
          host_.trees_containing_node(host_session, *request.optional_node);
    }
  }
  if (!inspected.status.ok) {
    return bounded_error(id, graph_status(inspected.status));
  }
  try {
    const char* field =
        method == "inspect.node_ids" ? "node_ids" : "ending_node_ids";
    Json empty_page{{"session_id", session_id.value}, {field, Json::array()}};
    add_worst_collection_page_metadata(&empty_page);
    const CollectionMeasurement measured = measure_collection_rows(
        inspected.value,
        [](NodeId node) { return encode_node_ids({node}).front(); }, empty_page,
        request.page_request.limit, inspected.value.size());
    auto page = collection_snapshots_.publish(
        std::move(reservation), std::move(request.binding),
        std::move(inspected.value), measured.entries, measured.bytes,
        request.page_request.limit, measured.page_limit);
    if (page.error != CollectionSnapshotError::None) {
      return bounded_error(id, collection_error_status(page.error));
    }
    Json result{{"session_id", session_id.value},
                {field, encode_node_ids(page.entries)}};
    add_collection_page_metadata(&result, page);
    return encode_success_response(id, std::move(result));
  } catch (const std::length_error& error) {
    return collection_too_large(id, error.what());
  }
}

/** @copydoc RequestRouter::route_inspection_graph_first_page */
std::string RequestRouter::route_inspection_graph_first_page(
    const std::string& id, InspectionRequest request,
    const GraphSessionId& host_session,
    CollectionSnapshotRegistry::Reservation reservation) {
  const IpcSessionId& session_id = request.session_id;
  Result<GraphInspectionView> inspected;
  {
    std::lock_guard<std::mutex> host_lock(host_mutex_);
    inspected = host_.inspect_graph(host_session);
  }
  if (!inspected.status.ok) {
    return bounded_error(id, graph_status(inspected.status));
  }
  try {
    const std::size_t measured_entries =
        graph_entry_count(inspected.value.nodes);
    GraphInspectionView empty_graph;
    Json empty_page = encode_graph(session_id, empty_graph);
    add_worst_collection_page_metadata(&empty_page);
    const CollectionMeasurement measured = measure_collection_rows(
        inspected.value.nodes,
        [](const NodeInspectionView& node) {
          require_node_preencoding_budget(node);
          return encode_node(node);
        },
        empty_page, request.page_request.limit, measured_entries);
    auto page = collection_snapshots_.publish(
        std::move(reservation), std::move(request.binding),
        std::move(inspected.value.nodes), measured.entries, measured.bytes,
        request.page_request.limit, measured.page_limit);
    if (page.error != CollectionSnapshotError::None) {
      return bounded_error(id, collection_error_status(page.error));
    }
    GraphInspectionView graph;
    graph.nodes = page.entries;
    Json result = encode_graph(session_id, graph);
    add_collection_page_metadata(&result, page);
    return encode_success_response(id, std::move(result));
  } catch (const std::length_error& error) {
    return collection_too_large(id, error.what());
  }
}

/** @copydoc RequestRouter::route_inspection_dependency_first_page */
std::string RequestRouter::route_inspection_dependency_first_page(
    const std::string& id, InspectionRequest request,
    const GraphSessionId& host_session,
    CollectionSnapshotRegistry::Reservation reservation) {
  const IpcSessionId& session_id = request.session_id;
  Result<HostDependencyTreeSnapshot> inspected;
  {
    std::lock_guard<std::mutex> host_lock(host_mutex_);
    inspected = host_.dependency_tree(host_session, request.optional_node,
                                      request.include_metadata);
  }
  if (!inspected.status.ok) {
    return bounded_error(id, graph_status(inspected.status));
  }
  try {
    const std::size_t measured_entries =
        dependency_tree_entry_count(inspected.value);
    auto header = std::make_shared<DependencyTreePageHeader>();
    header->scope = inspected.value.scope;
    header->start_node = inspected.value.start_node;
    header->graph_empty = inspected.value.graph_empty;
    header->start_node_found = inspected.value.start_node_found;
    header->no_ending_nodes = inspected.value.no_ending_nodes;
    header->root_nodes = std::move(inspected.value.root_nodes);
    HostDependencyTreeSnapshot header_value;
    header_value.scope = header->scope;
    header_value.start_node = header->start_node;
    header_value.graph_empty = header->graph_empty;
    header_value.start_node_found = header->start_node_found;
    header_value.no_ending_nodes = header->no_ending_nodes;
    header_value.root_nodes = header->root_nodes;
    const std::size_t header_bytes =
        encode_dependency_tree(session_id, header_value).dump().size();
    std::vector<DependencyTreePageRow> rows;
    rows.reserve(inspected.value.entries.size());
    for (HostDependencyTreeEntry& entry : inspected.value.entries) {
      rows.push_back(DependencyTreePageRow{header, std::move(entry)});
    }
    Json empty_page = encode_dependency_tree(session_id, header_value);
    add_worst_collection_page_metadata(&empty_page);
    CollectionMeasurement measured = measure_collection_rows(
        rows,
        [&session_id](const DependencyTreePageRow& row) {
          require_node_preencoding_budget(row.entry.node);
          HostDependencyTreeSnapshot one;
          one.entries.push_back(row.entry);
          return encode_dependency_tree(session_id, one)["entries"].front();
        },
        empty_page, request.page_request.limit, measured_entries);
    if (header_bytes < 2U) {
      throw std::invalid_argument(
          "dependency header lost its empty entries array");
    }
    measured.bytes = checked_size_sum(header_bytes - 2U, measured.bytes);
    if (measured.bytes > kSnapshotMaxBytes) {
      throw std::length_error("dependency tree exceeds 64 MiB snapshot bytes");
    }
    auto page = collection_snapshots_.publish(
        std::move(reservation), std::move(request.binding), std::move(rows),
        measured.entries, measured.bytes, request.page_request.limit,
        measured.page_limit);
    if (page.error != CollectionSnapshotError::None) {
      return bounded_error(id, collection_error_status(page.error));
    }
    HostDependencyTreeSnapshot tree = std::move(header_value);
    for (const DependencyTreePageRow& row : page.entries) {
      tree.entries.push_back(row.entry);
    }
    Json result = encode_dependency_tree(session_id, tree);
    add_collection_page_metadata(&result, page);
    return encode_success_response(id, std::move(result));
  } catch (const std::length_error& error) {
    return collection_too_large(id, error.what());
  }
}

/** @copydoc RequestRouter::route_inspection_traversal_first_page */
std::string RequestRouter::route_inspection_traversal_first_page(
    const std::string& id, InspectionRequest request,
    const GraphSessionId& host_session,
    CollectionSnapshotRegistry::Reservation reservation) {
  const std::string& method = request.method;
  const IpcSessionId& session_id = request.session_id;
  if (method == "inspect.traversal_orders") {
    Result<std::map<int, std::vector<NodeId>>> inspected;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      inspected = host_.traversal_orders(host_session);
    }
    if (!inspected.status.ok) {
      return bounded_error(id, graph_status(inspected.status));
    }
    try {
      const std::size_t measured_entries =
          traversal_order_entry_count(inspected.value);
      std::vector<TraversalOrderRow> rows;
      rows.reserve(inspected.value.size());
      for (auto& branch : inspected.value) {
        rows.push_back(
            TraversalOrderRow{branch.first, std::move(branch.second)});
      }
      Json empty_page{{"session_id", session_id.value},
                      {"orders", Json::array()}};
      add_worst_collection_page_metadata(&empty_page);
      const CollectionMeasurement measured = measure_collection_rows(
          rows,
          [](const TraversalOrderRow& row) {
            return encode_traversal_order_row(row);
          },
          empty_page, request.page_request.limit, measured_entries);
      auto page = collection_snapshots_.publish(
          std::move(reservation), std::move(request.binding), std::move(rows),
          measured.entries, measured.bytes, request.page_request.limit,
          measured.page_limit);
      if (page.error != CollectionSnapshotError::None) {
        return bounded_error(id, collection_error_status(page.error));
      }
      Json encoded = Json::array();
      for (const TraversalOrderRow& row : page.entries) {
        encoded.push_back(encode_traversal_order_row(row));
      }
      Json result{{"session_id", session_id.value},
                  {"orders", std::move(encoded)}};
      add_collection_page_metadata(&result, page);
      return encode_success_response(id, std::move(result));
    } catch (const std::length_error& error) {
      return collection_too_large(id, error.what());
    }
  }

  if (method == "inspect.traversal_details") {
    Result<std::map<int, std::vector<HostTraversalNodeSnapshot>>> inspected;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      inspected = host_.traversal_details(host_session);
    }
    if (!inspected.status.ok) {
      return bounded_error(id, graph_status(inspected.status));
    }
    try {
      const std::size_t measured_entries =
          traversal_detail_entry_count(inspected.value);
      std::vector<TraversalDetailRow> rows;
      rows.reserve(inspected.value.size());
      for (auto& branch : inspected.value) {
        rows.push_back(
            TraversalDetailRow{branch.first, std::move(branch.second)});
      }
      Json empty_page{{"session_id", session_id.value},
                      {"branches", Json::array()}};
      add_worst_collection_page_metadata(&empty_page);
      const CollectionMeasurement measured = measure_collection_rows(
          rows,
          [](const TraversalDetailRow& row) {
            return encode_traversal_detail_row(row);
          },
          empty_page, request.page_request.limit, measured_entries);
      auto page = collection_snapshots_.publish(
          std::move(reservation), std::move(request.binding), std::move(rows),
          measured.entries, measured.bytes, request.page_request.limit,
          measured.page_limit);
      if (page.error != CollectionSnapshotError::None) {
        return bounded_error(id, collection_error_status(page.error));
      }
      Json encoded = Json::array();
      for (const TraversalDetailRow& row : page.entries) {
        encoded.push_back(encode_traversal_detail_row(row));
      }
      Json result{{"session_id", session_id.value},
                  {"branches", std::move(encoded)}};
      add_collection_page_metadata(&result, page);
      return encode_success_response(id, std::move(result));
    } catch (const std::length_error& error) {
      return collection_too_large(id, error.what());
    }
  }

  return bounded_error(
      id, failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                         "internal_error",
                         "traversal first-page dispatch invariant failed"));
}

/** @copydoc RequestRouter::route_inspection_planning_first_page */
std::string RequestRouter::route_inspection_planning_first_page(
    const std::string& id, InspectionRequest request,
    const GraphSessionId& host_session,
    CollectionSnapshotRegistry::Reservation reservation) {
  const IpcSessionId& session_id = request.session_id;
  Result<std::vector<ComputePlanningInspectionSnapshot>> inspected;
  {
    std::lock_guard<std::mutex> host_lock(host_mutex_);
    inspected = host_.recent_compute_planning_snapshots(host_session);
  }
  if (!inspected.status.ok) {
    return bounded_error(id, graph_status(inspected.status));
  }
  try {
    const std::size_t measured_entries =
        recent_planning_entry_count(inspected.value);
    Json empty_page{{"session_id", session_id.value},
                    {"snapshots", Json::array()}};
    add_worst_collection_page_metadata(&empty_page);
    const CollectionMeasurement measured = measure_collection_rows(
        inspected.value,
        [](const ComputePlanningInspectionSnapshot& snapshot) {
          return encode_compute_planning(snapshot);
        },
        empty_page, request.page_request.limit, measured_entries);
    auto page = collection_snapshots_.publish(
        std::move(reservation), std::move(request.binding),
        std::move(inspected.value), measured.entries, measured.bytes,
        request.page_request.limit, measured.page_limit);
    if (page.error != CollectionSnapshotError::None) {
      return bounded_error(id, collection_error_status(page.error));
    }
    Json snapshots = Json::array();
    for (const ComputePlanningInspectionSnapshot& snapshot : page.entries) {
      snapshots.push_back(encode_compute_planning(snapshot));
    }
    Json result{{"session_id", session_id.value},
                {"snapshots", std::move(snapshots)}};
    add_collection_page_metadata(&result, page);
    return encode_success_response(id, std::move(result));
  } catch (const std::length_error& error) {
    return collection_too_large(id, error.what());
  }
}

/** @copydoc RequestRouter::decode_session_control_request */
OperationStatus RequestRouter::decode_session_control_request(
    const std::string& method, const RoutedParams& routed_params,
    SessionControlRequest* request) {
  const Json& params = routed_params.value;
  if (!read_session_id(params, &request->session_id)) {
    return invalid_params(method + " requires a valid session_id");
  }
  if (method == "dirty.begin" || method == "dirty.update" ||
      method == "dirty.end") {
    return decode_session_dirty_request(method, routed_params, request);
  }
  if (method == "inspect.roi_forward" || method == "inspect.roi_backward") {
    return decode_session_roi_request(method, routed_params, request);
  }
  return decode_session_text_request(method, routed_params, request);
}

/** @copydoc RequestRouter::decode_session_text_request */
OperationStatus RequestRouter::decode_session_text_request(
    const std::string& method, const RoutedParams& routed_params,
    SessionControlRequest* request) {
  const Json& params = routed_params.value;
  if (method == "graph.reload" || method == "graph.save") {
    if (!read_required_path(params, "yaml_path", &request->text)) {
      return invalid_params(method + " requires a nonempty absolute yaml_path");
    }
  } else if (method == "graph.node_yaml.get" ||
             method == "graph.node_yaml.set") {
    if (!read_node_id(params, "node_id", &request->first_node)) {
      return invalid_params(method + " requires a nonnegative node_id");
    }
    if (method == "graph.node_yaml.set" &&
        !read_required_text(params, "yaml_text", kLargeTextMaxBytes,
                            &request->text)) {
      return invalid_params(
          "graph.node_yaml.set requires bounded string yaml_text");
    }
  } else if (method == "cache.cache_all_nodes" ||
             method == "cache.synchronize_disk") {
    if (!read_required_text(params, "precision", kShortTextMaxBytes,
                            &request->text)) {
      return invalid_params(method + " requires bounded string precision");
    }
  }
  return ok_status();
}

/** @copydoc RequestRouter::decode_session_dirty_request */
OperationStatus RequestRouter::decode_session_dirty_request(
    const std::string& method, const RoutedParams& routed_params,
    SessionControlRequest* request) {
  const Json& params = routed_params.value;
  if (!read_node_id(params, "node_id", &request->first_node) ||
      !params.contains("domain") ||
      !decode_enum(params["domain"], &request->dirty_domain)) {
    return invalid_params(method +
                          " requires nonnegative node_id and valid domain");
  }
  if (method != "dirty.end" &&
      (!params.contains("source_roi") ||
       !decode_pixel_rect(params["source_roi"], &request->roi))) {
    return invalid_params(method + " requires an exact source_roi");
  }
  return ok_status();
}

/** @copydoc RequestRouter::decode_session_roi_request */
OperationStatus RequestRouter::decode_session_roi_request(
    const std::string& method, const RoutedParams& routed_params,
    SessionControlRequest* request) {
  const Json& params = routed_params.value;
  if (method == "inspect.roi_forward") {
    if (!read_node_id(params, "start_node_id", &request->first_node) ||
        !read_node_id(params, "target_node_id", &request->second_node) ||
        !params.contains("start_roi") ||
        !decode_pixel_rect(params["start_roi"], &request->roi)) {
      return invalid_params(
          "inspect.roi_forward requires start_node_id, start_roi, "
          "and target_node_id");
    }
  } else {
    if (!read_node_id(params, "target_node_id", &request->first_node) ||
        !read_node_id(params, "source_node_id", &request->second_node) ||
        !params.contains("target_roi") ||
        !decode_pixel_rect(params["target_roi"], &request->roi)) {
      return invalid_params(
          "inspect.roi_backward requires target_node_id, target_roi, "
          "and source_node_id");
    }
  }
  return ok_status();
}

/** @copydoc RequestRouter::route_session_mutation_method */
std::optional<std::string> RequestRouter::route_session_mutation_method(
    const std::string& id, const std::string& method,
    const SessionControlRequest& request, const GraphSessionId& host_session) {
  if (method == "graph.reload" || method == "graph.save" ||
      method == "graph.clear" || method == "graph.node_yaml.set") {
    return route_session_graph_mutation(id, method, request, host_session);
  }
  if (method == "cache.clear_all" || method == "cache.clear_drive" ||
      method == "cache.clear_memory" || method == "cache.cache_all_nodes" ||
      method == "cache.free_transient" || method == "cache.synchronize_disk") {
    return route_session_cache_mutation(id, method, request, host_session);
  }
  return std::nullopt;
}

/** @copydoc RequestRouter::route_session_graph_mutation */
std::string RequestRouter::route_session_graph_mutation(
    const std::string& id, const std::string& method,
    const SessionControlRequest& request, const GraphSessionId& host_session) {
  VoidResult routed;
  {
    std::lock_guard<std::mutex> host_lock(host_mutex_);
    if (method == "graph.reload") {
      routed = host_.reload_graph(host_session, request.text);
    } else if (method == "graph.save") {
      routed = host_.save_graph(host_session, request.text);
    } else if (method == "graph.clear") {
      routed = host_.clear_graph(host_session);
    } else {
      routed =
          host_.set_node_yaml(host_session, request.first_node, request.text);
    }
  }
  if (!routed.status.ok) {
    return bounded_error(id, graph_status(routed.status));
  }
  return encode_success_response(id, Json::object());
}

/** @copydoc RequestRouter::route_session_cache_mutation */
std::string RequestRouter::route_session_cache_mutation(
    const std::string& id, const std::string& method,
    const SessionControlRequest& request, const GraphSessionId& host_session) {
  VoidResult routed;
  {
    std::lock_guard<std::mutex> host_lock(host_mutex_);
    if (method == "cache.clear_all") {
      routed = host_.clear_cache(host_session);
    } else if (method == "cache.clear_drive") {
      routed = host_.clear_drive_cache(host_session);
    } else if (method == "cache.clear_memory") {
      routed = host_.clear_memory_cache(host_session);
    } else if (method == "cache.cache_all_nodes") {
      routed = host_.cache_all_nodes(host_session, request.text);
    } else if (method == "cache.free_transient") {
      routed = host_.free_transient_memory(host_session);
    } else {
      routed = host_.synchronize_disk_cache(host_session, request.text);
    }
  }
  if (!routed.status.ok) {
    return bounded_error(id, graph_status(routed.status));
  }
  return encode_success_response(id, Json::object());
}

/** @copydoc RequestRouter::route_session_dirty_method */
std::optional<std::string> RequestRouter::route_session_dirty_method(
    const std::string& id, const std::string& method,
    const SessionControlRequest& request, const GraphSessionId& host_session) {
  if (method == "dirty.begin" || method == "dirty.update" ||
      method == "dirty.end") {
    Result<DirtyRegionInspectionSnapshot> routed;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      if (method == "dirty.begin") {
        routed = host_.begin_dirty_source(host_session, request.first_node,
                                          request.dirty_domain, request.roi);
      } else if (method == "dirty.update") {
        routed = host_.update_dirty_source(host_session, request.first_node,
                                           request.dirty_domain, request.roi);
      } else {
        routed = host_.end_dirty_source(host_session, request.first_node,
                                        request.dirty_domain);
      }
    }
    if (!routed.status.ok) {
      return bounded_error(id, graph_status(routed.status));
    }
    return encode_routed_value(id, [&] {
      return encode_dirty_region(id, request.session_id, routed.value);
    });
  }
  return std::nullopt;
}

/** @copydoc RequestRouter::route_session_roi_method */
std::optional<std::string> RequestRouter::route_session_roi_method(
    const std::string& id, const std::string& method,
    const SessionControlRequest& request, const GraphSessionId& host_session) {
  if (method == "inspect.roi_forward" || method == "inspect.roi_backward") {
    Result<PixelRect> routed;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      routed =
          method == "inspect.roi_forward"
              ? host_.project_roi(host_session, request.first_node, request.roi,
                                  request.second_node)
              : host_.project_roi_backward(host_session, request.first_node,
                                           request.roi, request.second_node);
    }
    if (!routed.status.ok) {
      return bounded_error(id, graph_status(routed.status));
    }
    return encode_success_response(
        id, Json{{"session_id", request.session_id.value},
                 {"roi", encode_pixel_rect(routed.value)}});
  }
  return std::nullopt;
}

/** @copydoc RequestRouter::route_session_diagnostic_method */
std::optional<std::string> RequestRouter::route_session_diagnostic_method(
    const std::string& id, const std::string& method,
    const SessionControlRequest& request, const GraphSessionId& host_session) {
  if (method == "graph.node_yaml.get") {
    Result<std::string> routed;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      routed = host_.get_node_yaml(host_session, request.first_node);
    }
    if (!routed.status.ok) {
      return bounded_error(id, graph_status(routed.status));
    }
    return encode_routed_value(id, [&] {
      return encode_node_yaml(request.session_id, request.first_node,
                              routed.value);
    });
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
    return encode_routed_value(id, [&] {
      return encode_timing(id, request.session_id, routed.value);
    });
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
    return encode_routed_value(id, [&] {
      return encode_last_io_time(request.session_id, routed.value);
    });
  }

  if (method == "compute.last_error") {
    OperationStatus observed;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      observed = graph_status(host_.last_error(host_session));
    }
    return encode_routed_value(id, [&] {
      return Json{{"session_id", request.session_id.value},
                  {"status", encode_operation_status(observed)}};
    });
  }

  return std::nullopt;
}

/** @copydoc RequestRouter::route_session_control_method */
std::optional<std::string> RequestRouter::route_session_control_method(
    const std::string& id, const std::string& method,
    const RoutedParams& routed_params) {
  if (!is_session_control_method(method)) {
    return std::nullopt;
  }
  SessionControlRequest request;
  const OperationStatus decoded =
      decode_session_control_request(method, routed_params, &request);
  if (!decoded.ok) {
    return bounded_error(id, decoded);
  }
  IpcResult<SessionRegistry::HostCallAdmission> admission =
      registry_.admit_host_call(request.session_id);
  if (!admission.status.ok) {
    return bounded_error(id, admission.status);
  }
  const GraphSessionId& host_session = admission.value.host_session();

  if (std::optional<std::string> routed =
          route_session_mutation_method(id, method, request, host_session)) {
    return routed;
  }
  if (std::optional<std::string> routed =
          route_session_dirty_method(id, method, request, host_session)) {
    return routed;
  }
  if (std::optional<std::string> routed =
          route_session_roi_method(id, method, request, host_session)) {
    return routed;
  }
  if (std::optional<std::string> routed =
          route_session_diagnostic_method(id, method, request, host_session)) {
    return routed;
  }

  return bounded_error(
      id, failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                         "internal_error",
                         "session-control dispatch invariant failed"));
}

/** @copydoc RequestRouter::route_plugin_method */
std::optional<std::string> RequestRouter::route_plugin_method(
    const std::string& id, const std::string& method,
    const RoutedParams& routed_params) {
  if (!is_plugin_method(method)) {
    return std::nullopt;
  }
  if (std::optional<std::string> routed =
          route_plugin_direct_method(id, method, routed_params)) {
    return routed;
  }

  CollectionPageRequest page_request;
  std::string page_message;
  if (!read_collection_page(routed_params.value, &page_request,
                            &page_message)) {
    return bounded_error(id, invalid_params(std::move(page_message)));
  }
  StableCollectionRequest request{method, std::move(page_request),
                                  CollectionSnapshotBinding{method, {}, {}}};
  if (request.page_request.cursor) {
    return route_plugin_continuation(id, request);
  }
  return route_plugin_first_page(id, std::move(request));
}

/** @copydoc RequestRouter::route_plugin_direct_method */
std::optional<std::string> RequestRouter::route_plugin_direct_method(
    const std::string& id, const std::string& method,
    const RoutedParams& routed_params) {
  const Json& params = routed_params.value;

  if (method == "plugins.load_report") {
    std::vector<std::string> directories;
    if (!params.contains("directories") ||
        !decode_bounded_string_array(params["directories"],
                                     kPathArrayMaxEntries, kPathTextMaxBytes,
                                     &directories) ||
        std::any_of(directories.begin(), directories.end(),
                    [](const std::string& directory) {
                      return directory.empty() ||
                             directory.find('\0') != std::string::npos;
                    })) {
      return bounded_error(
          id, invalid_params(
                  "plugins.load_report requires up to 256 nonempty bounded "
                  "directory strings"));
    }
    Result<HostPluginLoadReport> report;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      report = host_.plugins_load_report(directories);
    }
    if (!report.status.ok) {
      return bounded_error(id, graph_status(report.status));
    }
    return encode_routed_value(
        id, [&] { return encode_plugin_load_report(report.value); });
  }

  if (method == "plugins.unload_all") {
    Result<int> unloaded;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      unloaded = host_.plugins_unload_all();
    }
    if (!unloaded.status.ok) {
      return bounded_error(id, graph_status(unloaded.status));
    }
    if (unloaded.value < 0) {
      throw std::invalid_argument(
          "plugins.unload_all returned a negative removed-key count");
    }
    return encode_success_response(id, Json{{"unloaded", unloaded.value}});
  }

  if (method == "plugins.seed_builtins") {
    VoidResult seeded;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      seeded = host_.seed_builtin_ops();
    }
    if (!seeded.status.ok) {
      return bounded_error(id, graph_status(seeded.status));
    }
    return encode_success_response(id, Json::object());
  }

  return std::nullopt;
}

/** @copydoc RequestRouter::route_plugin_continuation */
std::string RequestRouter::route_plugin_continuation(
    const std::string& id, const StableCollectionRequest& request) {
  if (request.method == "plugins.ops_combined_keys") {
    auto page = collection_snapshots_.page<std::string>(
        *request.page_request.cursor, request.binding,
        request.page_request.offset, request.page_request.limit);
    if (page.error != CollectionSnapshotError::None) {
      return bounded_error(id, collection_error_status(page.error));
    }
    return encode_routed_value(id, [&] {
      Json keys = Json::array();
      for (const std::string& key : page.entries) {
        keys.push_back(encode_plugin_key(key));
      }
      Json result{{"keys", std::move(keys)}};
      add_collection_page_metadata(&result, page);
      return result;
    });
  }
  auto page = collection_snapshots_.page<PluginSourceRow>(
      *request.page_request.cursor, request.binding,
      request.page_request.offset, request.page_request.limit);
  if (page.error != CollectionSnapshotError::None) {
    return bounded_error(id, collection_error_status(page.error));
  }
  return encode_routed_value(id, [&] {
    Json sources = Json::array();
    for (const PluginSourceRow& row : page.entries) {
      sources.push_back(encode_plugin_source_row(row.key, row.source));
    }
    Json result{{"sources", std::move(sources)}};
    add_collection_page_metadata(&result, page);
    return result;
  });
}

/** @copydoc RequestRouter::route_plugin_first_page */
std::string RequestRouter::route_plugin_first_page(
    const std::string& id, StableCollectionRequest request) {
  auto reserved = collection_snapshots_.reserve();
  if (reserved.error != CollectionSnapshotError::None) {
    return bounded_error(id, collection_error_status(reserved.error));
  }

  if (request.method == "plugins.ops_combined_keys") {
    Result<std::vector<std::string>> viewed;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      viewed = host_.ops_combined_keys();
    }
    if (!viewed.status.ok) {
      return bounded_error(id, graph_status(viewed.status));
    }
    try {
      std::sort(viewed.value.begin(), viewed.value.end());
      Json empty_page{{"keys", Json::array()}};
      add_worst_collection_page_metadata(&empty_page);
      const CollectionMeasurement measured = measure_collection_rows(
          viewed.value,
          [](const std::string& key) { return encode_plugin_key(key); },
          empty_page, request.page_request.limit, viewed.value.size());
      auto page = collection_snapshots_.publish(
          std::move(reserved.reservation), std::move(request.binding),
          std::move(viewed.value), measured.entries, measured.bytes,
          request.page_request.limit, measured.page_limit);
      if (page.error != CollectionSnapshotError::None) {
        return bounded_error(id, collection_error_status(page.error));
      }
      Json keys = Json::array();
      for (const std::string& key : page.entries) {
        keys.push_back(encode_plugin_key(key));
      }
      Json result{{"keys", std::move(keys)}};
      add_collection_page_metadata(&result, page);
      return encode_success_response(id, std::move(result));
    } catch (const std::length_error& error) {
      return collection_too_large(id, error.what());
    }
  }

  Result<std::map<std::string, std::string>> viewed;
  {
    std::lock_guard<std::mutex> host_lock(host_mutex_);
    viewed = request.method == "plugins.ops_sources"
                 ? host_.ops_sources()
                 : host_.ops_combined_sources();
  }
  if (!viewed.status.ok) {
    return bounded_error(id, graph_status(viewed.status));
  }
  try {
    std::vector<PluginSourceRow> rows;
    rows.reserve(viewed.value.size());
    for (auto& entry : viewed.value) {
      rows.push_back(
          PluginSourceRow{std::move(entry.first), std::move(entry.second)});
    }
    std::sort(rows.begin(), rows.end(),
              [](const PluginSourceRow& left, const PluginSourceRow& right) {
                return left.key < right.key;
              });
    Json empty_page{{"sources", Json::array()}};
    add_worst_collection_page_metadata(&empty_page);
    const CollectionMeasurement measured = measure_collection_rows(
        rows,
        [](const PluginSourceRow& row) {
          return encode_plugin_source_row(row.key, row.source);
        },
        empty_page, request.page_request.limit, rows.size());
    auto page = collection_snapshots_.publish(
        std::move(reserved.reservation), std::move(request.binding),
        std::move(rows), measured.entries, measured.bytes,
        request.page_request.limit, measured.page_limit);
    if (page.error != CollectionSnapshotError::None) {
      return bounded_error(id, collection_error_status(page.error));
    }
    Json sources = Json::array();
    for (const PluginSourceRow& row : page.entries) {
      sources.push_back(encode_plugin_source_row(row.key, row.source));
    }
    Json result{{"sources", std::move(sources)}};
    add_collection_page_metadata(&result, page);
    return encode_success_response(id, std::move(result));
  } catch (const std::length_error& error) {
    return collection_too_large(id, error.what());
  }
}

/** @copydoc RequestRouter::route_policy_method */
std::optional<std::string> RequestRouter::route_policy_method(
    const std::string& id, const std::string& method,
    const RoutedParams& routed_params) {
  if (!is_policy_method(method)) {
    return std::nullopt;
  }
  if (std::optional<std::string> routed =
          route_policy_global_method(id, method, routed_params)) {
    return routed;
  }
  if (std::optional<std::string> routed =
          route_policy_binding_method(id, method, routed_params)) {
    return routed;
  }

  CollectionPageRequest page_request;
  std::string page_message;
  if (!read_collection_page(routed_params.value, &page_request,
                            &page_message)) {
    return bounded_error(id, invalid_params(std::move(page_message)));
  }
  StableCollectionRequest request{method, std::move(page_request),
                                  CollectionSnapshotBinding{method, {}, {}}};
  if (request.page_request.cursor) {
    return route_policy_continuation(id, request);
  }
  return route_policy_first_page(id, std::move(request));
}

/** @copydoc RequestRouter::route_policy_global_method */
std::optional<std::string> RequestRouter::route_policy_global_method(
    const std::string& id, const std::string& method,
    const RoutedParams& routed_params) {
  if (method == "policy.description") {
    return route_policy_description_method(id, routed_params);
  }
  if (method == "policy.scan" || method == "policy.load") {
    return route_policy_plugin_method(id, method, routed_params);
  }
  if (method == "policy.configure_defaults") {
    return route_policy_defaults_method(id, routed_params);
  }
  return std::nullopt;
}

/** @copydoc RequestRouter::route_policy_description_method */
std::string RequestRouter::route_policy_description_method(
    const std::string& id, const RoutedParams& routed_params) {
  const Json& params = routed_params.value;
  std::string type;
  if (!read_required_text(params, "type", kPolicyTypeMaxBytes, &type) ||
      !valid_policy_type(type)) {
    return bounded_error(
        id, invalid_params("policy.description requires a canonical type"));
  }
  Result<std::string> described;
  {
    std::lock_guard<std::mutex> host_lock(host_mutex_);
    described = host_.policy_description(type);
  }
  if (!described.status.ok) {
    return bounded_error(id, graph_status(described.status));
  }
  return encode_routed_value(
      id, [&] { return encode_policy_description(type, described.value); });
}

/** @copydoc RequestRouter::route_policy_plugin_method */
std::string RequestRouter::route_policy_plugin_method(
    const std::string& id, const std::string& method,
    const RoutedParams& routed_params) {
  const Json& params = routed_params.value;
  if (method == "policy.scan") {
    std::vector<std::string> directories;
    if (!params.contains("directories") ||
        !decode_bounded_string_array(params["directories"],
                                     kPathArrayMaxEntries, kPathTextMaxBytes,
                                     &directories) ||
        std::any_of(directories.begin(), directories.end(),
                    [](const std::string& directory) {
                      return directory.empty() ||
                             directory.find('\0') != std::string::npos;
                    })) {
      return bounded_error(
          id, invalid_params("policy.scan requires up to 256 nonempty bounded "
                             "directory strings"));
    }
    Result<std::size_t> scanned;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      scanned = host_.policy_scan(directories);
    }
    if (!scanned.status.ok) {
      return bounded_error(id, graph_status(scanned.status));
    }
    return encode_success_response(id, Json{{"loaded", scanned.value}});
  }
  std::string path;
  if (!read_required_text(params, "path", kPathTextMaxBytes, &path) ||
      path.empty() || path.find('\0') != std::string::npos) {
    return bounded_error(
        id, invalid_params("policy.load requires a nonempty bounded path"));
  }
  VoidResult loaded;
  {
    std::lock_guard<std::mutex> host_lock(host_mutex_);
    loaded = host_.policy_load(path);
  }
  if (!loaded.status.ok) {
    return bounded_error(id, graph_status(loaded.status));
  }
  return encode_success_response(id, Json::object());
}

/** @copydoc RequestRouter::route_policy_defaults_method */
std::string RequestRouter::route_policy_defaults_method(
    const std::string& id, const RoutedParams& routed_params) {
  const Json& params = routed_params.value;
  HostPolicyConfig config;
  if (!read_required_text(params, "interactive_type", kPolicyTypeMaxBytes,
                          &config.interactive_type) ||
      !read_required_text(params, "throughput_type", kPolicyTypeMaxBytes,
                          &config.throughput_type) ||
      !valid_policy_type(config.interactive_type) ||
      !valid_policy_type(config.throughput_type)) {
    return bounded_error(
        id, invalid_params("policy.configure_defaults requires canonical "
                           "interactive_type and throughput_type"));
  }
  VoidResult configured;
  {
    std::lock_guard<std::mutex> host_lock(host_mutex_);
    configured = host_.configure_policy_defaults(config);
  }
  if (!configured.status.ok) {
    return bounded_error(id, graph_status(configured.status));
  }
  return encode_success_response(id, Json::object());
}

/** @copydoc RequestRouter::route_policy_binding_method */
std::optional<std::string> RequestRouter::route_policy_binding_method(
    const std::string& id, const std::string& method,
    const RoutedParams& routed_params) {
  const Json& params = routed_params.value;
  if (method == "policy.info" || method == "policy.replace") {
    PolicyClass policy_class = PolicyClass::Interactive;
    std::string type;
    if (!params.contains("policy_class") ||
        !decode_enum(params["policy_class"], &policy_class)) {
      return bounded_error(
          id, invalid_params(method + " requires a valid policy_class"));
    }
    if (method == "policy.replace" &&
        (!read_required_text(params, "type", kPolicyTypeMaxBytes, &type) ||
         !valid_policy_type(type))) {
      return bounded_error(
          id, invalid_params("policy.replace requires a canonical type"));
    }

    if (method == "policy.info") {
      Result<PolicyInfoSnapshot> info;
      {
        std::lock_guard<std::mutex> host_lock(host_mutex_);
        info = host_.policy_info(policy_class);
      }
      if (!info.status.ok) {
        return bounded_error(id, graph_status(info.status));
      }
      if (info.value.policy_class != policy_class) {
        throw std::invalid_argument(
            "policy.info returned a mismatched policy class");
      }
      return encode_routed_value(
          id, [&] { return encode_policy_info(info.value); });
    }

    VoidResult replaced;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      replaced = host_.replace_policy(policy_class, type);
    }
    if (!replaced.status.ok) {
      return bounded_error(id, graph_status(replaced.status));
    }
    return encode_success_response(id, Json::object());
  }

  return std::nullopt;
}

/** @copydoc RequestRouter::route_policy_continuation */
std::string RequestRouter::route_policy_continuation(
    const std::string& id, const StableCollectionRequest& request) {
  const bool type_list = request.method == "policy.types";
  auto page = collection_snapshots_.page<std::string>(
      *request.page_request.cursor, request.binding,
      request.page_request.offset, request.page_request.limit);
  if (page.error != CollectionSnapshotError::None) {
    return bounded_error(id, collection_error_status(page.error));
  }
  return encode_routed_value(id, [&] {
    Json rows = Json::array();
    for (const std::string& row : page.entries) {
      rows.push_back(type_list ? encode_policy_type(row)
                               : encode_policy_plugin_label(row));
    }
    Json result{{type_list ? "types" : "plugins", std::move(rows)}};
    add_collection_page_metadata(&result, page);
    return result;
  });
}

/** @copydoc RequestRouter::route_policy_first_page */
std::string RequestRouter::route_policy_first_page(
    const std::string& id, StableCollectionRequest request) {
  const bool type_list = request.method == "policy.types";
  auto reserved = collection_snapshots_.reserve();
  if (reserved.error != CollectionSnapshotError::None) {
    return bounded_error(id, collection_error_status(reserved.error));
  }

  Result<std::vector<std::string>> listed;
  {
    std::lock_guard<std::mutex> host_lock(host_mutex_);
    listed = type_list ? host_.policy_available_types()
                       : host_.policy_loaded_plugins();
  }
  if (!listed.status.ok) {
    return bounded_error(id, graph_status(listed.status));
  }
  try {
    std::sort(listed.value.begin(), listed.value.end());
    if (type_list &&
        std::adjacent_find(listed.value.begin(), listed.value.end()) !=
            listed.value.end()) {
      throw std::invalid_argument(
          "policy.types Host result contains a duplicate type");
    }
    Json empty_page{{type_list ? "types" : "plugins", Json::array()}};
    add_worst_collection_page_metadata(&empty_page);
    const CollectionMeasurement measured = measure_collection_rows(
        listed.value,
        [type_list](const std::string& row) {
          return type_list ? encode_policy_type(row)
                           : encode_policy_plugin_label(row);
        },
        empty_page, request.page_request.limit, listed.value.size());
    auto page = collection_snapshots_.publish(
        std::move(reserved.reservation), std::move(request.binding),
        std::move(listed.value), measured.entries, measured.bytes,
        request.page_request.limit, measured.page_limit);
    if (page.error != CollectionSnapshotError::None) {
      return bounded_error(id, collection_error_status(page.error));
    }
    Json rows = Json::array();
    for (const std::string& row : page.entries) {
      rows.push_back(type_list ? encode_policy_type(row)
                               : encode_policy_plugin_label(row));
    }
    Json result{{type_list ? "types" : "plugins", std::move(rows)}};
    add_collection_page_metadata(&result, page);
    return encode_success_response(id, std::move(result));
  } catch (const std::length_error& error) {
    return collection_too_large(id, error.what());
  }
}

/** @copydoc RequestRouter::route_execution_method */
std::optional<std::string> RequestRouter::route_execution_method(
    const std::string& id, const std::string& method,
    const RoutedParams& routed_params) {
  if (!is_execution_method(method)) {
    return std::nullopt;
  }
  const Json& params = routed_params.value;
  if (method == "execution.description") {
    std::string type;
    if (!read_required_text(params, "type", kShortTextMaxBytes, &type) ||
        !valid_execution_type(type)) {
      return bounded_error(
          id, invalid_params(
                  "execution.description requires a known execution type"));
    }
    Result<std::string> described;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      described = host_.execution_description(type);
    }
    if (!described.status.ok) {
      return bounded_error(id, graph_status(described.status));
    }
    return encode_routed_value(id, [&] {
      return encode_execution_description(type, described.value);
    });
  }

  if (method == "execution.configure_defaults") {
    HostExecutionConfig config;
    if (!read_required_text(params, "hp_type", kShortTextMaxBytes,
                            &config.hp_type) ||
        !read_required_text(params, "rt_type", kShortTextMaxBytes,
                            &config.rt_type) ||
        !valid_execution_type(config.hp_type) ||
        !valid_execution_type(config.rt_type) ||
        !params.contains("worker_count") ||
        !decode_integer(params["worker_count"], &config.worker_count) ||
        config.worker_count > kExecutionWorkerRequestMax) {
      return bounded_error(
          id,
          invalid_params("execution.configure_defaults requires known hp_type "
                         "and rt_type plus worker_count in [0,8]"));
    }
    VoidResult configured;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      configured = host_.configure_execution_defaults(config);
    }
    if (!configured.status.ok) {
      return bounded_error(id, graph_status(configured.status));
    }
    return encode_success_response(id, Json::object());
  }

  if (method == "execution.info" || method == "execution.replace") {
    IpcSessionId session_id;
    ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
    std::string type;
    const bool replacement_valid =
        method != "execution.replace" ||
        (read_required_text(params, "type", kShortTextMaxBytes, &type) &&
         valid_execution_type(type));
    if (!replacement_valid || !read_session_id(params, &session_id) ||
        !params.contains("intent") || !decode_enum(params["intent"], &intent)) {
      return bounded_error(
          id, invalid_params(method +
                             " requires valid session_id, intent, and type"));
    }

    IpcResult<SessionRegistry::HostCallAdmission> admission =
        registry_.admit_host_call(session_id);
    if (!admission.status.ok) {
      return bounded_error(id, admission.status);
    }
    const GraphSessionId& host_session = admission.value.host_session();
    if (method == "execution.info") {
      Result<ExecutionInfoSnapshot> info;
      {
        std::lock_guard<std::mutex> host_lock(host_mutex_);
        info = host_.execution_info(host_session, intent);
      }
      if (!info.status.ok) {
        return bounded_error(id, graph_status(info.status));
      }
      if (info.value.intent != intent) {
        throw std::invalid_argument(
            "execution.info returned a mismatched compute intent");
      }
      return encode_routed_value(
          id, [&] { return encode_execution_info(session_id, info.value); });
    }

    VoidResult replaced;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      replaced = host_.replace_execution(host_session, intent, type);
    }
    if (!replaced.status.ok) {
      return bounded_error(id, graph_status(replaced.status));
    }
    return encode_success_response(id, Json::object());
  }

  CollectionPageRequest page_request;
  std::string page_message;
  if (!read_collection_page(params, &page_request, &page_message)) {
    return bounded_error(id, invalid_params(std::move(page_message)));
  }
  StableCollectionRequest request{method, std::move(page_request),
                                  CollectionSnapshotBinding{method, {}, {}}};
  if (request.page_request.cursor) {
    return route_execution_continuation(id, request);
  }
  return route_execution_first_page(id, std::move(request));
}

/** @copydoc RequestRouter::route_execution_continuation */
std::string RequestRouter::route_execution_continuation(
    const std::string& id, const StableCollectionRequest& request) {
  auto page = collection_snapshots_.page<std::string>(
      *request.page_request.cursor, request.binding,
      request.page_request.offset, request.page_request.limit);
  if (page.error != CollectionSnapshotError::None) {
    return bounded_error(id, collection_error_status(page.error));
  }
  return encode_routed_value(id, [&] {
    Json rows = Json::array();
    for (const std::string& row : page.entries) {
      rows.push_back(encode_execution_type(row));
    }
    Json result{{"types", std::move(rows)}};
    add_collection_page_metadata(&result, page);
    return result;
  });
}

/** @copydoc RequestRouter::route_execution_first_page */
std::string RequestRouter::route_execution_first_page(
    const std::string& id, StableCollectionRequest request) {
  auto reserved = collection_snapshots_.reserve();
  if (reserved.error != CollectionSnapshotError::None) {
    return bounded_error(id, collection_error_status(reserved.error));
  }
  Result<std::vector<std::string>> listed;
  {
    std::lock_guard<std::mutex> host_lock(host_mutex_);
    listed = host_.execution_available_types();
  }
  if (!listed.status.ok) {
    return bounded_error(id, graph_status(listed.status));
  }
  try {
    std::sort(listed.value.begin(), listed.value.end());
    if (std::adjacent_find(listed.value.begin(), listed.value.end()) !=
        listed.value.end()) {
      throw std::invalid_argument(
          "execution.types Host result contains a duplicate type");
    }
    Json empty_page{{"types", Json::array()}};
    add_worst_collection_page_metadata(&empty_page);
    const CollectionMeasurement measured = measure_collection_rows(
        listed.value,
        [](const std::string& row) { return encode_execution_type(row); },
        empty_page, request.page_request.limit, listed.value.size());
    auto page = collection_snapshots_.publish(
        std::move(reserved.reservation), std::move(request.binding),
        std::move(listed.value), measured.entries, measured.bytes,
        request.page_request.limit, measured.page_limit);
    if (page.error != CollectionSnapshotError::None) {
      return bounded_error(id, collection_error_status(page.error));
    }
    Json rows = Json::array();
    for (const std::string& row : page.entries) {
      rows.push_back(encode_execution_type(row));
    }
    Json result{{"types", std::move(rows)}};
    add_collection_page_metadata(&result, page);
    return encode_success_response(id, std::move(result));
  } catch (const std::length_error& error) {
    return collection_too_large(id, error.what());
  }
}

/** @copydoc RequestRouter::route_observation_method */
std::optional<std::string> RequestRouter::route_observation_method(
    const std::string& id, const std::string& method,
    const RoutedParams& routed_params) {
  if (!is_observation_method(method)) {
    return std::nullopt;
  }
  const Json& params = routed_params.value;

  IpcSessionId session_id;
  std::size_t limit = 0;
  uint64_t after_sequence = 0;
  const std::size_t minimum_limit = method == "events.drain"
                                        ? kComputeEventDrainMinLimit
                                        : kExecutionTraceMinLimit;
  const std::size_t maximum_limit = method == "events.drain"
                                        ? kComputeEventDrainMaxLimit
                                        : kExecutionTraceMaxLimit;
  if (!read_session_id(params, &session_id) || !params.contains("limit") ||
      !decode_page_limit(params["limit"], minimum_limit, maximum_limit,
                         &limit)) {
    return bounded_error(
        id, invalid_params(method + " requires a valid session_id and limit"));
  }
  if (method == "execution.trace" &&
      (!params.contains("after_sequence") ||
       !decode_integer(params["after_sequence"], &after_sequence))) {
    return bounded_error(
        id, invalid_params(
                "execution.trace requires an exact unsigned after_sequence"));
  }

  IpcResult<SessionRegistry::HostCallAdmission> admission =
      registry_.admit_host_call(session_id);
  if (!admission.status.ok) {
    return bounded_error(id, admission.status);
  }
  const GraphSessionId& host_session = admission.value.host_session();

  if (method == "events.drain") {
    Result<ComputeEventBatch> routed;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      routed = host_.drain_compute_events(host_session, limit);
    }
    if (!routed.status.ok) {
      return bounded_error(id, graph_status(routed.status));
    }
    return encode_routed_value(id, [&] {
      return encode_compute_event_batch(session_id, routed.value, limit);
    });
  }

  Result<ExecutionTracePage> routed;
  {
    std::lock_guard<std::mutex> host_lock(host_mutex_);
    routed = host_.execution_trace(host_session, after_sequence, limit);
  }
  if (!routed.status.ok) {
    return bounded_error(id, graph_status(routed.status));
  }
  return encode_routed_value(id, [&] {
    return encode_execution_trace_page(session_id, after_sequence, routed.value,
                                       limit);
  });
}

/** @copydoc RequestRouter::route_compute_method */
std::optional<std::string> RequestRouter::route_compute_method(
    const std::string& id, const std::string& method,
    const RoutedParams& routed_params) {
  if (!is_compute_method(method)) {
    return std::nullopt;
  }
  const Json& params = routed_params.value;

  if (method == "compute.submit") {
    IpcSessionId session_id;
    HostComputeRequest request;
    ComputeResultMode mode = ComputeResultMode::Status;
    std::string message;
    if (!decode_compute_submit(params, &session_id, &request, &mode,
                               &message)) {
      return bounded_error(id, invalid_params(std::move(message)));
    }
    IpcResult<ComputeRequestSnapshot> submitted =
        compute_registry_.submit(session_id, std::move(request), mode);
    if (!submitted.status.ok) {
      return bounded_error(id, submitted.status);
    }
    return encode_routed_value(
        id, [&] { return encode_compute_snapshot(submitted.value); });
  }

  ComputeRequestId compute_id;
  if (!read_compute_id(params, &compute_id)) {
    return bounded_error(
        id, invalid_params(method + " requires a valid compute_id"));
  }

  if (method == "compute.status") {
    IpcResult<ComputeRequestSnapshot> status =
        compute_registry_.status(compute_id);
    if (!status.status.ok) {
      return bounded_error(id, status.status);
    }
    return encode_routed_value(
        id, [&] { return encode_compute_snapshot(status.value); });
  }

  if (method == "compute.result") {
    IpcResult<ComputeRequestSnapshot> result =
        compute_registry_.result(compute_id);
    if (!result.status.ok) {
      return bounded_error(id, result.status);
    }
    if (result.value.output_reference.has_value()) {
      IpcResult<OutputArtifactDelivery> delivery =
          output_store_.acquire_delivery(*result.value.output_reference);
      if (!delivery.status.ok) {
        return bounded_error(id, delivery.status);
      }
      return encode_routed_value(id, [&] {
        return encode_compute_snapshot(
            result.value, encode_output_delivery(
                              delivery.value, *result.value.output_reference));
      });
    }
    return encode_routed_value(
        id, [&] { return encode_compute_snapshot(result.value); });
  }

  std::optional<std::string> delivery_id;
  if (!read_optional_delivery_id(params, &delivery_id)) {
    return bounded_error(
        id, invalid_params(
                "compute.release delivery_id must be a valid opaque id"));
  }
  OperationStatus released = compute_registry_.release(compute_id, delivery_id);
  if (!released.ok && delivery_id.has_value() &&
      released.domain == OperationErrorDomain::Daemon &&
      released.code == kJobNotFoundCode &&
      output_store_.release_orphaned_delivery(compute_id, *delivery_id)) {
    released = ok_status();
  }
  if (!released.ok) {
    return bounded_error(id, released);
  }
  return encode_success_response(
      id, Json{{"compute_id", compute_id.value}, {"released", true}});
}

/** @copydoc RequestRouter::route_graph_lifecycle_method */
std::optional<std::string> RequestRouter::route_graph_lifecycle_method(
    const std::string& id, const std::string& method,
    const RoutedParams& routed_params) {
  const Json& params = routed_params.value;
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
          id, invalid_params("graph.load contains an unsafe session or path"));
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
    OperationStatus status;
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

  if (method != "graph.close") {
    return std::nullopt;
  }
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
  OperationStatus close_status;
  if (claim.value.role() == SessionRegistry::CloseClaim::Role::Joiner) {
    close_status = claim.value.wait_result();
  } else {
    VoidResult closed;
    bool host_close_started = false;
    try {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      claim.value.mark_host_close_started();
      host_close_started = true;
      closed = host_.close_graph(claim.value.host_session());
    } catch (...) {
      if (host_close_started) {
        std::terminate();
      }
      throw;
    }
    if (!closed.status.ok &&
        checked_graph_error_code(closed.status) != GraphErrc::NotFound) {
      std::terminate();
    }
    claim.value.complete_host_close(std::move(closed.status));
    close_status = claim.value.wait_result();
  }
  if (!close_status.ok) {
    return bounded_error(id, graph_status(close_status));
  }
  return encode_success_response(id, Json{{"closed", true}});
}

/** @copydoc RequestRouter::route */
std::string RequestRouter::route(const std::string& payload) {
  DecodedRequestEnvelopeResult decoded = decode_request_envelope(payload);
  if (!decoded.request) {
    return std::move(decoded.error_response);
  }
  const std::string& id = decoded.request->id;
  const std::string& method = decoded.request->method;
  const Json& params = decoded.request->params;

  try {
    bool handled = false;
    OperationStatus status = ok_status();
    Json result = route_daemon_method(method, params, service_version_,
                                      server_instance_id_, &handled);
    if (handled) {
      return status.ok ? encode_success_response(id, std::move(result))
                       : bounded_error(id, status);
    }

    if (std::optional<std::string> routed =
            route_graph_lifecycle_method(id, method, RoutedParams{params})) {
      return std::move(*routed);
    }

    if (std::optional<std::string> routed =
            route_compute_method(id, method, RoutedParams{params})) {
      return std::move(*routed);
    }

    if (std::optional<std::string> routed =
            route_observation_method(id, method, RoutedParams{params})) {
      return std::move(*routed);
    }

    if (std::optional<std::string> routed =
            route_execution_method(id, method, RoutedParams{params})) {
      return std::move(*routed);
    }

    if (std::optional<std::string> routed =
            route_policy_method(id, method, RoutedParams{params})) {
      return std::move(*routed);
    }

    if (std::optional<std::string> routed =
            route_plugin_method(id, method, RoutedParams{params})) {
      return std::move(*routed);
    }

    if (std::optional<std::string> routed =
            route_inspection_method(id, method, RoutedParams{params})) {
      return std::move(*routed);
    }

    if (std::optional<std::string> routed =
            route_session_control_method(id, method, RoutedParams{params})) {
      return std::move(*routed);
    }

    return bounded_error(
        id, failure_status(OperationErrorDomain::Protocol, kMethodNotFoundCode,
                           "method_not_found",
                           "method is not implemented by protocol version 2"));
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

/** @copydoc RequestRouter::close_shutdown_session */
void RequestRouter::close_shutdown_session(
    const IpcSessionId& session_id) noexcept {
  try {
    IpcResult<SessionRegistry::CloseClaim> claim =
        registry_.begin_shutdown_close(session_id);
    if (!claim.status.ok) {
      return;
    }
    if (claim.value.role() == SessionRegistry::CloseClaim::Role::Joiner) {
      (void)claim.value.wait_result();
      return;
    }

    VoidResult closed;
    bool host_close_started = false;
    try {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      claim.value.mark_host_close_started();
      host_close_started = true;
      closed = host_.close_graph(claim.value.host_session());
    } catch (...) {
      if (host_close_started) {
        std::terminate();
      }
      return;
    }
    if (!closed.status.ok &&
        checked_graph_error_code(closed.status) != GraphErrc::NotFound) {
      std::terminate();
    }
    claim.value.complete_host_close(std::move(closed.status));
    (void)claim.value.wait_result();
  } catch (...) {
    return;
  }
}

/** @copydoc RequestRouter::finish_shutdown */
void RequestRouter::finish_shutdown() noexcept {
  compute_registry_.shutdown();
  collection_snapshots_.finish_shutdown();
  output_store_.shutdown();
  try {
    const auto sessions = registry_.active_sessions();
    for (const auto& session : sessions) {
      close_shutdown_session(session.first);
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
