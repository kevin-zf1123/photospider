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
 * @return True for graph mutation, node YAML, cache, dirty, ROI, timing,
 *         last-IO, or last-error routing.
 * @throws Nothing.
 * @note This dispatch matcher is not the daemon capability-advertisement
 *       table and does not change `daemon.version` metadata.
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
 * @note Capability advertisement remains owned by the separate exact table.
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
 *         exceed one version 1 frame.
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
 *         tokens alone exceed one version 1 frame, or size arithmetic
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
 *         its version 1 byte bound.
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
    : RequestRouter(host, std::move(service_version), {}, {}, {}) {
}  // NOLINT(whitespace/indent_namespace)

/** @copydoc RequestRouter::RequestRouter */
RequestRouter::RequestRouter(
    Host& host, std::string service_version,
    CollectionSnapshotLimits snapshot_limits,
    CollectionSnapshotRegistry::Clock snapshot_clock,
    CollectionSnapshotRegistry::IdGenerator snapshot_id_generator)
    : host_(host),
      collection_snapshots_(snapshot_limits, std::move(snapshot_clock),
                            std::move(snapshot_id_generator)),
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

/** @copydoc RequestRouter::route_inspection_method */
std::optional<std::string> RequestRouter::route_inspection_method(
    const std::string& id, const std::string& method,
    const RoutedParams& routed_params) {
  if (!is_inspection_method(method)) {
    return std::nullopt;
  }
  const Json& params = routed_params.value;
  const bool graph_list = method == "graph.list";
  IpcSessionId session_id;
  if (!graph_list && !read_session_id(params, &session_id)) {
    return bounded_error(
        id, invalid_params(method + " requires a valid session_id"));
  }

  std::optional<NodeId> optional_node;
  bool include_metadata = false;
  if (method == "inspect.node" || method == "inspect.trees_containing_node") {
    NodeId node;
    if (!read_node_id(params, "node_id", &node)) {
      return bounded_error(
          id, invalid_params(method + " requires a nonnegative node_id"));
    }
    optional_node = node;
  } else if (method == "inspect.dependency_tree") {
    if (params.contains("node_id") && !params["node_id"].is_null()) {
      NodeId node;
      if (!read_node_id(params, "node_id", &node)) {
        return bounded_error(
            id,
            invalid_params("node_id must be a nonnegative integer or null"));
      }
      optional_node = node;
    }
    if (params.contains("include_metadata")) {
      if (!params["include_metadata"].is_boolean()) {
        return bounded_error(
            id, invalid_params("include_metadata must be boolean"));
      }
      include_metadata = params["include_metadata"].get<bool>();
    }
  }

  const bool direct_value = method == "inspect.node" ||
                            method == "inspect.dirty_region" ||
                            method == "inspect.compute_planning";
  if (direct_value) {
    IpcResult<SessionRegistry::HostCallAdmission> admission =
        registry_.admit_host_call(session_id);
    if (!admission.status.ok) {
      return bounded_error(id, admission.status);
    }
    if (method == "inspect.node") {
      Result<NodeInspectionView> inspected;
      {
        std::lock_guard<std::mutex> host_lock(host_mutex_);
        inspected =
            host_.inspect_node(admission.value.host_session(), *optional_node);
      }
      if (!inspected.status.ok) {
        return bounded_error(id, graph_status(inspected.status));
      }
      return encode_routed_value(id, [&] {
        require_node_preencoding_budget(inspected.value);
        return Json{{"session_id", session_id.value},
                    {"node", encode_node(inspected.value)}};
      });
    }
    if (method == "inspect.dirty_region") {
      Result<DirtyRegionInspectionSnapshot> inspected;
      {
        std::lock_guard<std::mutex> host_lock(host_mutex_);
        inspected = host_.dirty_region_snapshot(admission.value.host_session());
      }
      if (!inspected.status.ok) {
        return bounded_error(id, graph_status(inspected.status));
      }
      return encode_routed_value(id, [&] {
        return encode_dirty_region(id, session_id, inspected.value);
      });
    }
    Result<std::optional<ComputePlanningInspectionSnapshot>> inspected;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      inspected =
          host_.compute_planning_snapshot(admission.value.host_session());
    }
    if (!inspected.status.ok) {
      return bounded_error(id, graph_status(inspected.status));
    }
    return encode_routed_value(id, [&] {
      return Json{{"session_id", session_id.value},
                  {"planning", inspected.value
                                   ? encode_compute_planning(*inspected.value)
                                   : Json(nullptr)}};
    });
  }

  CollectionPageRequest page_request;
  std::string page_message;
  if (!read_collection_page(params, &page_request, &page_message)) {
    return bounded_error(id, invalid_params(std::move(page_message)));
  }
  CollectionSnapshotBinding binding{
      method,
      graph_list ? std::string{} : session_id.value,
      {}};
  if (method == "inspect.trees_containing_node") {
    binding.original_params = "node_id=" + std::to_string(optional_node->value);
  } else if (method == "inspect.dependency_tree") {
    binding.original_params =
        std::string("node_id=") +
        (optional_node ? std::to_string(optional_node->value) : "null") +
        ";include_metadata=" + (include_metadata ? "true" : "false");
  }

  if (page_request.cursor) {
    if (method == "graph.list") {
      auto page = collection_snapshots_.page<GraphSessionSummary>(
          *page_request.cursor, binding, page_request.offset,
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
    if (method == "inspect.node_ids" || method == "inspect.ending_nodes" ||
        method == "inspect.trees_containing_node") {
      auto page = collection_snapshots_.page<NodeId>(
          *page_request.cursor, binding, page_request.offset,
          page_request.limit);
      if (page.error != CollectionSnapshotError::None) {
        return bounded_error(id, collection_error_status(page.error));
      }
      return encode_routed_value(id, [&] {
        const char* field = method == "inspect.node_ids" ? "node_ids"
                            : method == "inspect.ending_nodes"
                                ? "ending_node_ids"
                                : "ending_node_ids";
        Json result{{"session_id", session_id.value},
                    {field, encode_node_ids(page.entries)}};
        add_collection_page_metadata(&result, page);
        return result;
      });
    }
    if (method == "inspect.graph") {
      auto page = collection_snapshots_.page<NodeInspectionView>(
          *page_request.cursor, binding, page_request.offset,
          page_request.limit);
      if (page.error != CollectionSnapshotError::None) {
        return bounded_error(id, collection_error_status(page.error));
      }
      return encode_routed_value(id, [&] {
        GraphInspectionView graph;
        graph.nodes = page.entries;
        Json result = encode_graph(session_id, graph);
        add_collection_page_metadata(&result, page);
        return result;
      });
    }
    if (method == "inspect.dependency_tree") {
      auto page = collection_snapshots_.page<DependencyTreePageRow>(
          *page_request.cursor, binding, page_request.offset,
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
        Json result = encode_dependency_tree(session_id, tree);
        add_collection_page_metadata(&result, page);
        return result;
      });
    }
    if (method == "inspect.traversal_orders") {
      auto page = collection_snapshots_.page<TraversalOrderRow>(
          *page_request.cursor, binding, page_request.offset,
          page_request.limit);
      if (page.error != CollectionSnapshotError::None) {
        return bounded_error(id, collection_error_status(page.error));
      }
      return encode_routed_value(id, [&] {
        Json rows = Json::array();
        for (const TraversalOrderRow& row : page.entries) {
          rows.push_back(encode_traversal_order_row(row));
        }
        Json result{{"session_id", session_id.value},
                    {"orders", std::move(rows)}};
        add_collection_page_metadata(&result, page);
        return result;
      });
    }
    if (method == "inspect.traversal_details") {
      auto page = collection_snapshots_.page<TraversalDetailRow>(
          *page_request.cursor, binding, page_request.offset,
          page_request.limit);
      if (page.error != CollectionSnapshotError::None) {
        return bounded_error(id, collection_error_status(page.error));
      }
      return encode_routed_value(id, [&] {
        Json rows = Json::array();
        for (const TraversalDetailRow& row : page.entries) {
          rows.push_back(encode_traversal_detail_row(row));
        }
        Json result{{"session_id", session_id.value},
                    {"branches", std::move(rows)}};
        add_collection_page_metadata(&result, page);
        return result;
      });
    }
    auto page = collection_snapshots_.page<ComputePlanningInspectionSnapshot>(
        *page_request.cursor, binding, page_request.offset, page_request.limit);
    if (page.error != CollectionSnapshotError::None) {
      return bounded_error(id, collection_error_status(page.error));
    }
    return encode_routed_value(id, [&] {
      Json snapshots = Json::array();
      for (const ComputePlanningInspectionSnapshot& snapshot : page.entries) {
        snapshots.push_back(encode_compute_planning(snapshot));
      }
      Json result{{"session_id", session_id.value},
                  {"snapshots", std::move(snapshots)}};
      add_collection_page_metadata(&result, page);
      return result;
    });
  }

  if (method == "graph.list") {
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
          empty_page, page_request.limit, reconciled.value.size());
      auto page = collection_snapshots_.publish(
          std::move(reserved.reservation), std::move(binding),
          std::move(reconciled.value), measured.entries, measured.bytes,
          page_request.limit, measured.page_limit);
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

  IpcResult<SessionRegistry::HostCallAdmission> admission =
      registry_.admit_host_call(session_id);
  if (!admission.status.ok) {
    return bounded_error(id, admission.status);
  }
  const GraphSessionId& host_session = admission.value.host_session();
  auto reserved = collection_snapshots_.reserve();
  if (reserved.error != CollectionSnapshotError::None) {
    return bounded_error(id, collection_error_status(reserved.error));
  }

  if (method == "inspect.node_ids" || method == "inspect.ending_nodes" ||
      method == "inspect.trees_containing_node") {
    Result<std::vector<NodeId>> inspected;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      if (method == "inspect.node_ids") {
        inspected = host_.list_node_ids(host_session);
      } else if (method == "inspect.ending_nodes") {
        inspected = host_.ending_nodes(host_session);
      } else {
        inspected = host_.trees_containing_node(host_session, *optional_node);
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
          [](NodeId node) { return encode_node_ids({node}).front(); },
          empty_page, page_request.limit, inspected.value.size());
      auto page = collection_snapshots_.publish(
          std::move(reserved.reservation), std::move(binding),
          std::move(inspected.value), measured.entries, measured.bytes,
          page_request.limit, measured.page_limit);
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

  if (method == "inspect.graph") {
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
          empty_page, page_request.limit, measured_entries);
      auto page = collection_snapshots_.publish(
          std::move(reserved.reservation), std::move(binding),
          std::move(inspected.value.nodes), measured.entries, measured.bytes,
          page_request.limit, measured.page_limit);
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

  if (method == "inspect.dependency_tree") {
    Result<HostDependencyTreeSnapshot> inspected;
    {
      std::lock_guard<std::mutex> host_lock(host_mutex_);
      inspected =
          host_.dependency_tree(host_session, optional_node, include_metadata);
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
          empty_page, page_request.limit, measured_entries);
      if (header_bytes < 2U) {
        throw std::invalid_argument(
            "dependency header lost its empty entries array");
      }
      measured.bytes = checked_size_sum(header_bytes - 2U, measured.bytes);
      if (measured.bytes > kSnapshotMaxBytes) {
        throw std::length_error(
            "dependency tree exceeds 64 MiB snapshot bytes");
      }
      auto page = collection_snapshots_.publish(
          std::move(reserved.reservation), std::move(binding), std::move(rows),
          measured.entries, measured.bytes, page_request.limit,
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
          empty_page, page_request.limit, measured_entries);
      auto page = collection_snapshots_.publish(
          std::move(reserved.reservation), std::move(binding), std::move(rows),
          measured.entries, measured.bytes, page_request.limit,
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
          empty_page, page_request.limit, measured_entries);
      auto page = collection_snapshots_.publish(
          std::move(reserved.reservation), std::move(binding), std::move(rows),
          measured.entries, measured.bytes, page_request.limit,
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
        empty_page, page_request.limit, measured_entries);
    auto page = collection_snapshots_.publish(
        std::move(reserved.reservation), std::move(binding),
        std::move(inspected.value), measured.entries, measured.bytes,
        page_request.limit, measured.page_limit);
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
