#include "ipc/codec.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ps::ipc::internal {
namespace {

/**
 * @brief Encodes one finite double or the version 1 null sentinel.
 *
 * @param value Public floating-point value.
 * @return JSON number for finite input, otherwise JSON null.
 * @throws Nothing.
 */
Json encode_double(double value) {
  return std::isfinite(value) ? Json(value) : Json(nullptr);
}

/**
 * @brief Decodes one version 1 floating-point field.
 *
 * @param value JSON number or null.
 * @param output Receives a finite number or quiet NaN for null.
 * @return True for a number or null, otherwise false.
 * @throws Nothing.
 */
bool decode_double(const Json& value, double* output) {
  if (value.is_null()) {
    *output = std::numeric_limits<double>::quiet_NaN();
    return true;
  }
  if (!value.is_number()) {
    return false;
  }
  const double decoded = value.get<double>();
  if (!std::isfinite(decoded)) {
    return false;
  }
  *output = decoded;
  return true;
}

/**
 * @brief Encodes a public pixel extent.
 *
 * @param size Extent to serialize.
 * @return Object with integer width and height.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 */
Json encode_size(const PixelSize& size) {
  return Json{{"width", size.width}, {"height", size.height}};
}

/**
 * @brief Decodes a public pixel extent.
 *
 * @param value Candidate object.
 * @param size Receives decoded integer width and height.
 * @return True when both required integers fit `int`.
 * @throws Nothing.
 */
bool decode_size(const Json& value, PixelSize* size) {
  if (!value.is_object() || !value.contains("width") ||
      !value.contains("height") || !value["width"].is_number_integer() ||
      !value["height"].is_number_integer()) {
    return false;
  }
  try {
    size->width = value["width"].get<int>();
    size->height = value["height"].get<int>();
  } catch (const Json::exception&) {
    return false;
  }
  return true;
}

/**
 * @brief Encodes a public pixel rectangle.
 *
 * @param rect Rectangle to serialize.
 * @return Object with integer x, y, width, and height.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 */
Json encode_rect(const PixelRect& rect) {
  return Json{{"x", rect.x},
              {"y", rect.y},
              {"width", rect.width},
              {"height", rect.height}};
}

/**
 * @brief Decodes a public pixel rectangle.
 *
 * @param value Candidate object.
 * @param rect Receives decoded integer coordinates and dimensions.
 * @return True when all four required integers fit `int`.
 * @throws Nothing.
 */
bool decode_rect(const Json& value, PixelRect* rect) {
  static constexpr const char* kFields[] = {"x", "y", "width", "height"};
  if (!value.is_object()) {
    return false;
  }
  for (const char* field : kFields) {
    if (!value.contains(field) || !value[field].is_number_integer()) {
      return false;
    }
  }
  try {
    rect->x = value["x"].get<int>();
    rect->y = value["y"].get<int>();
    rect->width = value["width"].get<int>();
    rect->height = value["height"].get<int>();
  } catch (const Json::exception&) {
    return false;
  }
  return true;
}

/**
 * @brief Encodes a fixed row-major 3x3 matrix.
 *
 * @param matrix Nine public matrix elements.
 * @return Nine-element array using null for non-finite values.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 */
Json encode_matrix(const double (&matrix)[9]) {
  Json result = Json::array();
  for (double value : matrix) {
    result.push_back(encode_double(value));
  }
  return result;
}

/**
 * @brief Decodes a fixed row-major 3x3 matrix.
 *
 * @param value Candidate nine-element array.
 * @param matrix Receives finite values or quiet NaN for null entries.
 * @return True when every element follows the floating-point null policy.
 * @throws Nothing.
 */
bool decode_matrix(const Json& value, double (&matrix)[9]) {
  if (!value.is_array() || value.size() != 9) {
    return false;
  }
  for (std::size_t index = 0; index < 9; ++index) {
    if (!decode_double(value[index], &matrix[index])) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Encodes optional node debug metadata.
 *
 * @param debug Metadata snapshot to serialize.
 * @return Snake-case debug object.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 */
Json encode_debug(const DebugMetadataSnapshot& debug) {
  return Json{{"computed_by_worker_id", debug.computed_by_worker_id},
              {"timestamp_us", debug.timestamp_us},
              {"execution_time_ms", debug.execution_time_ms},
              {"min_val", encode_double(debug.min_val)},
              {"max_val", encode_double(debug.max_val)},
              {"has_nan", debug.has_nan},
              {"compute_device", debug.compute_device}};
}

/**
 * @brief Decodes node debug metadata.
 *
 * @param value Candidate debug object.
 * @param debug Receives the copied public snapshot.
 * @return True when all required fields have valid types and ranges.
 * @throws std::bad_alloc if the device string cannot be copied.
 */
bool decode_debug(const Json& value, DebugMetadataSnapshot* debug) {
  if (!value.is_object() ||
      !value.value("compute_device", Json()).is_string() ||
      !value.value("has_nan", Json()).is_boolean() ||
      !value.value("computed_by_worker_id", Json()).is_number_integer() ||
      !value.contains("timestamp_us") ||
      !value["timestamp_us"].is_number_unsigned() ||
      !value.contains("execution_time_ms") ||
      !value["execution_time_ms"].is_number_unsigned() ||
      !value.contains("min_val") || !value.contains("max_val")) {
    return false;
  }
  try {
    debug->computed_by_worker_id = value["computed_by_worker_id"].get<int>();
    debug->timestamp_us = value["timestamp_us"].get<std::uint64_t>();
    debug->execution_time_ms = value["execution_time_ms"].get<std::uint64_t>();
    debug->has_nan = value["has_nan"].get<bool>();
    debug->compute_device = value["compute_device"].get<std::string>();
  } catch (const Json::exception&) {
    return false;
  }
  return decode_double(value["min_val"], &debug->min_val) &&
         decode_double(value["max_val"], &debug->max_val);
}

/**
 * @brief Encodes optional node spatial metadata.
 *
 * @param space Spatial snapshot to serialize.
 * @return Snake-case spatial object with fixed matrices.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 */
Json encode_space(const SpatialSnapshot& space) {
  return Json{
      {"extent", encode_size(space.extent)},
      {"absolute_roi", encode_rect(space.absolute_roi)},
      {"global_scale_x", encode_double(space.global_scale_x)},
      {"global_scale_y", encode_double(space.global_scale_y)},
      {"transform_matrix", encode_matrix(space.transform_matrix)},
      {"inverse_matrix", encode_matrix(space.inverse_matrix)},
      {"local_inverse_matrix", encode_matrix(space.local_inverse_matrix)}};
}

/**
 * @brief Decodes node spatial metadata.
 *
 * @param value Candidate spatial object.
 * @param space Receives the copied public snapshot.
 * @return True when extents, ROI, scales, and matrices follow version 1.
 * @throws Nothing.
 */
bool decode_space(const Json& value, SpatialSnapshot* space) {
  if (!value.is_object() || !value.contains("extent") ||
      !value.contains("absolute_roi") || !value.contains("global_scale_x") ||
      !value.contains("global_scale_y") ||
      !value.contains("transform_matrix") ||
      !value.contains("inverse_matrix") ||
      !value.contains("local_inverse_matrix")) {
    return false;
  }
  return decode_size(value["extent"], &space->extent) &&
         decode_rect(value["absolute_roi"], &space->absolute_roi) &&
         decode_double(value["global_scale_x"], &space->global_scale_x) &&
         decode_double(value["global_scale_y"], &space->global_scale_y) &&
         decode_matrix(value["transform_matrix"], space->transform_matrix) &&
         decode_matrix(value["inverse_matrix"], space->inverse_matrix) &&
         decode_matrix(value["local_inverse_matrix"],
                       space->local_inverse_matrix);
}

/**
 * @brief Encodes one public dependency edge.
 *
 * @param edge Edge snapshot to serialize.
 * @return Snake-case JSON edge object.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 */
Json encode_edge(const HostGraphEdgeSnapshot& edge) {
  const char* kind = edge.kind == HostGraphEdgeKind::ImageInput
                         ? "image_input"
                         : "parameter_input";
  return Json{{"from_node_id", edge.from_node.value},
              {"to_node_id", edge.to_node.value},
              {"kind", kind},
              {"from_output_name", edge.from_output_name},
              {"to_input_name", edge.to_input_name},
              {"input_index", edge.input_index}};
}

/**
 * @brief Decodes one public dependency edge.
 *
 * @param value Candidate edge object.
 * @param edge Receives the copied edge snapshot.
 * @return True when ids, kind, names, and nonnegative input index are valid.
 * @throws std::bad_alloc if copied names cannot be allocated.
 */
bool decode_edge(const Json& value, HostGraphEdgeSnapshot* edge) {
  if (!value.is_object() ||
      !value.value("from_node_id", Json()).is_number_integer() ||
      !value.value("to_node_id", Json()).is_number_integer() ||
      !value.value("kind", Json()).is_string() ||
      !value.value("from_output_name", Json()).is_string() ||
      !value.value("to_input_name", Json()).is_string() ||
      !value.contains("input_index") ||
      !value["input_index"].is_number_unsigned()) {
    return false;
  }
  try {
    edge->from_node.value = value["from_node_id"].get<int>();
    edge->to_node.value = value["to_node_id"].get<int>();
    edge->from_output_name = value["from_output_name"].get<std::string>();
    edge->to_input_name = value["to_input_name"].get<std::string>();
    edge->input_index = value["input_index"].get<std::size_t>();
  } catch (const Json::exception&) {
    return false;
  }
  const std::string kind = value["kind"].get<std::string>();
  if (kind == "image_input") {
    edge->kind = HostGraphEdgeKind::ImageInput;
  } else if (kind == "parameter_input") {
    edge->kind = HostGraphEdgeKind::ParameterInput;
  } else {
    return false;
  }
  return true;
}

/**
 * @brief Returns the stable wire name for one graph error code.
 *
 * @param code Current or future public graph error value.
 * @return Stable name for every current value, otherwise `unknown`.
 * @throws Nothing.
 * @note The switch is intentionally explicit rather than arithmetic.
 */
const char* graph_error_name(GraphErrc code) noexcept {
  switch (code) {
    case GraphErrc::Unknown:
      return "unknown";
    case GraphErrc::NotFound:
      return "not_found";
    case GraphErrc::Cycle:
      return "cycle";
    case GraphErrc::Io:
      return "io";
    case GraphErrc::InvalidYaml:
      return "invalid_yaml";
    case GraphErrc::MissingDependency:
      return "missing_dependency";
    case GraphErrc::NoOperation:
      return "no_operation";
    case GraphErrc::InvalidParameter:
      return "invalid_parameter";
    case GraphErrc::ComputeError:
      return "compute_error";
  }
  return "unknown";
}

/**
 * @brief Converts one wire-domain string into the public enum.
 *
 * @param name Lowercase version 1 domain name.
 * @param domain Receives the public domain.
 * @return True for the four remote failure domains.
 * @throws Nothing.
 */
bool decode_domain(const std::string& name, IpcErrorDomain* domain) noexcept {
  if (name == "transport") {
    *domain = IpcErrorDomain::Transport;
  } else if (name == "protocol") {
    *domain = IpcErrorDomain::Protocol;
  } else if (name == "graph") {
    *domain = IpcErrorDomain::Graph;
  } else if (name == "daemon") {
    *domain = IpcErrorDomain::Daemon;
  } else {
    return false;
  }
  return true;
}

/**
 * @brief Converts one public error domain into its wire name.
 *
 * @param domain Public domain to encode.
 * @return Stable lowercase name; `none` is used only defensively.
 * @throws Nothing.
 */
const char* encode_domain(IpcErrorDomain domain) noexcept {
  switch (domain) {
    case IpcErrorDomain::None:
      return "none";
    case IpcErrorDomain::Transport:
      return "transport";
    case IpcErrorDomain::Protocol:
      return "protocol";
    case IpcErrorDomain::Graph:
      return "graph";
    case IpcErrorDomain::Daemon:
      return "daemon";
  }
  return "none";
}

}  // namespace

/** @copydoc parse_json */
JsonParseResult parse_json(const std::string& payload) {
  std::vector<std::set<std::string>> object_keys;
  bool duplicate = false;
  bool ambiguous_top_level_id = false;
  auto callback = [&object_keys, &duplicate, &ambiguous_top_level_id](
                      int depth, Json::parse_event_t event,
                      Json& parsed) -> bool {
    if (event == Json::parse_event_t::object_start) {
      const std::size_t index = static_cast<std::size_t>(depth);
      if (object_keys.size() <= index) {
        object_keys.resize(index + 1);
      }
      object_keys[index].clear();
    } else if (event == Json::parse_event_t::key) {
      const std::size_t index =
          depth > 0 ? static_cast<std::size_t>(depth - 1) : 0U;
      if (object_keys.size() <= index) {
        object_keys.resize(index + 1);
      }
      const std::string key = parsed.get<std::string>();
      if (!object_keys[index].insert(key).second) {
        duplicate = true;
        if (index == 0 && key == "id") {
          ambiguous_top_level_id = true;
        }
      }
    }
    return true;
  };

  try {
    Json parsed = Json::parse(payload, callback, true, false);
    if (duplicate) {
      return {false, true, ambiguous_top_level_id, std::move(parsed),
              "duplicate JSON object key"};
    }
    return {true, false, false, std::move(parsed), {}};
  } catch (const Json::exception& error) {
    return {false, duplicate, ambiguous_top_level_id, {}, error.what()};
  }
}

/** @copydoc ok_status */
IpcStatus ok_status() {
  return {};
}

/** @copydoc failure_status */
IpcStatus failure_status(IpcErrorDomain domain, std::int32_t code,
                         std::string name, std::string message) {
  return {false, domain, code, std::move(name), std::move(message)};
}

/** @copydoc graph_status */
IpcStatus graph_status(const OperationStatus& status) {
  if (status.ok) {
    return ok_status();
  }
  return failure_status(IpcErrorDomain::Graph,
                        static_cast<std::int32_t>(status.code),
                        graph_error_name(status.code), status.message);
}

/** @copydoc encode_error */
Json encode_error(const IpcStatus& status) {
  return Json{{"domain", encode_domain(status.domain)},
              {"code", status.code},
              {"name", status.name},
              {"message", status.message}};
}

/** @copydoc decode_error */
bool decode_error(const Json& value, IpcStatus* status, std::string* message) {
  if (!value.is_object() || !value.value("domain", Json()).is_string() ||
      !value.value("code", Json()).is_number_integer() ||
      !value.value("name", Json()).is_string() ||
      !value.value("message", Json()).is_string()) {
    *message = "error object requires domain/code/name/message";
    return false;
  }
  IpcErrorDomain domain = IpcErrorDomain::None;
  const std::string domain_name = value["domain"].get<std::string>();
  if (!decode_domain(domain_name, &domain) ||
      domain == IpcErrorDomain::Transport) {
    *message = "error object has unknown domain";
    return false;
  }
  try {
    *status = failure_status(domain, value["code"].get<std::int32_t>(),
                             value["name"].get<std::string>(),
                             value["message"].get<std::string>());
  } catch (const Json::exception&) {
    *message = "error code is outside signed 32-bit range";
    return false;
  }
  return true;
}

/** @copydoc encode_success_response */
std::string encode_success_response(const std::string& id, Json result) {
  Json response{{"protocol_version", kProtocolVersion},
                {"id", id},
                {"result", std::move(result)}};
  std::string payload = response.dump();
  if (payload.size() <= kMaximumFramePayloadBytes) {
    return payload;
  }
  const IpcStatus status = failure_status(
      IpcErrorDomain::Protocol, kResponseTooLargeCode, "response_too_large",
      "serialized response exceeds 16777216 bytes");
  return Json{{"protocol_version", kProtocolVersion},
              {"id", id},
              {"error", encode_error(status)}}
      .dump();
}

/** @copydoc encode_node */
Json encode_node(const NodeInspectionView& node) {
  Json source_label =
      node.source_label ? Json(*node.source_label) : Json(nullptr);
  Json debug = node.debug ? encode_debug(*node.debug) : Json(nullptr);
  Json space = node.space ? encode_space(*node.space) : Json(nullptr);
  return Json{{"id", node.id.value},
              {"name", node.name},
              {"type", node.type},
              {"subtype", node.subtype},
              {"parameters", node.parameters},
              {"has_cached_output", node.has_cached_output},
              {"source_label", std::move(source_label)},
              {"debug", std::move(debug)},
              {"space", std::move(space)}};
}

/** @copydoc decode_node */
bool decode_node(const Json& value, NodeInspectionView* node,
                 std::string* message) {
  if (!value.is_object() || !value.value("id", Json()).is_number_integer() ||
      !value.value("name", Json()).is_string() ||
      !value.value("type", Json()).is_string() ||
      !value.value("subtype", Json()).is_string() ||
      !value.value("parameters", Json()).is_object() ||
      !value.value("has_cached_output", Json()).is_boolean() ||
      !value.contains("source_label") || !value.contains("debug") ||
      !value.contains("space")) {
    *message = "node snapshot has invalid required fields";
    return false;
  }

  NodeInspectionView decoded;
  try {
    decoded.id.value = value["id"].get<int>();
    decoded.name = value["name"].get<std::string>();
    decoded.type = value["type"].get<std::string>();
    decoded.subtype = value["subtype"].get<std::string>();
    decoded.parameters =
        value["parameters"].get<std::map<std::string, std::string>>();
    decoded.has_cached_output = value["has_cached_output"].get<bool>();
  } catch (const Json::exception&) {
    *message = "node snapshot contains an out-of-range or non-string value";
    return false;
  }

  if (value["source_label"].is_string()) {
    decoded.source_label = value["source_label"].get<std::string>();
  } else if (!value["source_label"].is_null()) {
    *message = "node source_label must be string or null";
    return false;
  }
  if (!value["debug"].is_null()) {
    DebugMetadataSnapshot debug;
    if (!decode_debug(value["debug"], &debug)) {
      *message = "node debug snapshot is invalid";
      return false;
    }
    decoded.debug = std::move(debug);
  }
  if (!value["space"].is_null()) {
    SpatialSnapshot space;
    if (!decode_space(value["space"], &space)) {
      *message = "node spatial snapshot is invalid";
      return false;
    }
    decoded.space = std::move(space);
  }
  *node = std::move(decoded);
  return true;
}

/** @copydoc encode_graph */
Json encode_graph(const IpcSessionId& session_id,
                  const GraphInspectionView& graph) {
  Json nodes = Json::array();
  for (const NodeInspectionView& node : graph.nodes) {
    nodes.push_back(encode_node(node));
  }
  return Json{{"session_id", session_id.value}, {"nodes", std::move(nodes)}};
}

/** @copydoc decode_graph */
bool decode_graph(const Json& value, GraphInspectionView* graph,
                  std::string* message) {
  if (!value.is_object() || !value.value("session_id", Json()).is_string() ||
      !value.value("nodes", Json()).is_array()) {
    *message = "graph snapshot requires session_id and nodes";
    return false;
  }
  GraphInspectionView decoded;
  decoded.session.value = value["session_id"].get<std::string>();
  for (const Json& node_json : value["nodes"]) {
    NodeInspectionView node;
    if (!decode_node(node_json, &node, message)) {
      return false;
    }
    decoded.nodes.push_back(std::move(node));
  }
  *graph = std::move(decoded);
  return true;
}

/** @copydoc encode_dependency_tree */
Json encode_dependency_tree(const IpcSessionId& session_id,
                            const HostDependencyTreeSnapshot& tree) {
  Json roots = Json::array();
  for (NodeId node : tree.root_nodes) {
    roots.push_back(node.value);
  }
  Json entries = Json::array();
  for (const HostDependencyTreeEntry& entry : tree.entries) {
    entries.push_back(
        Json{{"depth", entry.depth},
             {"incoming_edge", entry.incoming_edge
                                   ? encode_edge(*entry.incoming_edge)
                                   : Json(nullptr)},
             {"node", encode_node(entry.node)},
             {"cycle", entry.cycle}});
  }
  return Json{{"session_id", session_id.value},
              {"scope", tree.scope == HostDependencyTreeScope::EndingNodes
                            ? "ending_nodes"
                            : "start_node"},
              {"start_node_id",
               tree.start_node ? Json(tree.start_node->value) : Json(nullptr)},
              {"graph_empty", tree.graph_empty},
              {"start_node_found", tree.start_node_found},
              {"no_ending_nodes", tree.no_ending_nodes},
              {"root_node_ids", std::move(roots)},
              {"entries", std::move(entries)}};
}

/** @copydoc decode_dependency_tree */
bool decode_dependency_tree(const Json& value, HostDependencyTreeSnapshot* tree,
                            std::string* message) {
  if (!value.is_object() || !value.value("scope", Json()).is_string() ||
      !value.contains("start_node_id") ||
      !value.value("graph_empty", Json()).is_boolean() ||
      !value.value("start_node_found", Json()).is_boolean() ||
      !value.value("no_ending_nodes", Json()).is_boolean() ||
      !value.value("root_node_ids", Json()).is_array() ||
      !value.value("entries", Json()).is_array()) {
    *message = "dependency tree has invalid required fields";
    return false;
  }
  HostDependencyTreeSnapshot decoded;
  const std::string scope = value["scope"].get<std::string>();
  if (scope == "ending_nodes") {
    decoded.scope = HostDependencyTreeScope::EndingNodes;
  } else if (scope == "start_node") {
    decoded.scope = HostDependencyTreeScope::StartNode;
  } else {
    *message = "dependency tree has unknown scope";
    return false;
  }
  if (value["start_node_id"].is_number_integer()) {
    try {
      decoded.start_node = NodeId{value["start_node_id"].get<int>()};
    } catch (const Json::exception&) {
      *message = "dependency tree start_node_id is out of range";
      return false;
    }
  } else if (!value["start_node_id"].is_null()) {
    *message = "dependency tree start_node_id must be integer or null";
    return false;
  }
  decoded.graph_empty = value["graph_empty"].get<bool>();
  decoded.start_node_found = value["start_node_found"].get<bool>();
  decoded.no_ending_nodes = value["no_ending_nodes"].get<bool>();
  try {
    for (const Json& root : value["root_node_ids"]) {
      if (!root.is_number_integer()) {
        *message = "dependency tree root id must be an integer";
        return false;
      }
      decoded.root_nodes.push_back(NodeId{root.get<int>()});
    }
  } catch (const Json::exception&) {
    *message = "dependency tree root id is out of range";
    return false;
  }
  for (const Json& entry_json : value["entries"]) {
    if (!entry_json.is_object() ||
        !entry_json.value("depth", Json()).is_number_integer() ||
        !entry_json.contains("incoming_edge") || !entry_json.contains("node") ||
        !entry_json.value("cycle", Json()).is_boolean()) {
      *message = "dependency tree entry has invalid required fields";
      return false;
    }
    HostDependencyTreeEntry entry;
    try {
      entry.depth = entry_json["depth"].get<int>();
    } catch (const Json::exception&) {
      *message = "dependency tree depth is out of range";
      return false;
    }
    if (entry.depth < 0) {
      *message = "dependency tree depth must be nonnegative";
      return false;
    }
    if (!entry_json["incoming_edge"].is_null()) {
      HostGraphEdgeSnapshot edge;
      if (!decode_edge(entry_json["incoming_edge"], &edge)) {
        *message = "dependency tree incoming edge is invalid";
        return false;
      }
      entry.incoming_edge = std::move(edge);
    }
    if (!decode_node(entry_json["node"], &entry.node, message)) {
      return false;
    }
    entry.cycle = entry_json["cycle"].get<bool>();
    decoded.entries.push_back(std::move(entry));
  }
  *tree = std::move(decoded);
  return true;
}

}  // namespace ps::ipc::internal
