#include "photospider/ipc/client.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "ipc/client_collection_budget.hpp"
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
  OperationStatus status;

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
OperationStatus invalid_response(std::string message) {
  return internal::failure_status(OperationErrorDomain::Protocol,
                                  internal::kInvalidRequestCode,
                                  "invalid_request", std::move(message));
}

/**
 * @brief Builds the canonical local disconnected-client failure.
 * @return Transport code 2/name `not_connected` with copied diagnostics.
 * @throws std::bad_alloc if diagnostic storage cannot be allocated.
 */
OperationStatus not_connected_status() {
  return internal::failure_status(OperationErrorDomain::Transport, 2,
                                  "not_connected",
                                  "IPC client is not connected");
}

/**
 * @brief Creates a failed typed result while preserving its status.
 *
 * @tparam Value Default-constructible public result type.
 * @param status Failure status to move into the result.
 * @return Failed typed result with default payload.
 * @throws Whatever moving `OperationStatus` or constructing `Value` throws.
 */
template <typename Value>
IpcResult<Value> failed_result(OperationStatus status) {
  return {std::move(status), {}};
}

/**
 * @brief Decoded private metadata shared by stable collection result pages.
 *
 * @throws std::bad_alloc when copied cursor storage cannot be allocated.
 * @note This wire-only value deliberately remains private because public Host
 *       collection contracts return complete owned values rather than cursors.
 */
struct CollectionPageMetadata {
  /** @brief Zero-based offset echoed for this page. */
  std::size_t offset = 0;

  /** @brief Exact next offset derived from the current outer row count. */
  std::size_t next_offset = 0;

  /** @brief Whether another retained page follows. */
  bool has_more = false;

  /** @brief Stable cursor while more rows remain; absent on the final page. */
  std::optional<std::string> cursor;
};

/**
 * @brief Decodes and cross-checks one stable collection page envelope.
 *
 * @param result Method-specific result object containing common metadata.
 * @param expected_offset Exact continuation offset sent by the Client.
 * @param page_rows Number of indivisible outer rows decoded from this page.
 * @param stable_cursor Cursor established by an earlier page, when present.
 * @param metadata Receives complete metadata only after all checks succeed.
 * @param message Receives an owned diagnostic on failure.
 * @return True for a coherent first, continuation, or final page.
 * @throws std::bad_alloc if cursor or diagnostic storage cannot be allocated.
 * @note A multi-page result must make progress, retain one opaque cursor, and
 *       remain within the 262,144-row snapshot ceiling. Final pages must carry
 *       JSON null rather than retaining a cursor that the daemon already
 *       released.
 */
bool decode_collection_page_metadata(
    const internal::Json& result, std::size_t expected_offset,
    std::size_t page_rows, const std::optional<std::string>& stable_cursor,
    CollectionPageMetadata* metadata, std::string* message) {
  if (metadata == nullptr || message == nullptr || !result.is_object() ||
      !result.contains("offset") ||
      !result.value("has_more", internal::Json()).is_boolean() ||
      !result.contains("cursor") ||
      page_rows > internal::kGeneralPageMaxEntries) {
    if (message != nullptr) {
      *message = "collection result has invalid page metadata";
    }
    return false;
  }

  CollectionPageMetadata decoded;
  if (!internal::decode_integer(result["offset"], &decoded.offset) ||
      decoded.offset != expected_offset ||
      expected_offset > internal::kSnapshotMaxEntries ||
      page_rows > internal::kSnapshotMaxEntries - expected_offset) {
    *message = "collection result offset or row count is invalid";
    return false;
  }
  decoded.next_offset = expected_offset + page_rows;
  if (decoded.next_offset > internal::kSnapshotMaxEntries) {
    *message = "collection result exceeds the snapshot row limit";
    return false;
  }
  decoded.has_more = result["has_more"].get<bool>();
  if (decoded.has_more) {
    std::string cursor;
    if (page_rows == 0 ||
        !internal::decode_opaque_id(result["cursor"], &cursor) ||
        (stable_cursor && cursor != *stable_cursor)) {
      *message = "collection continuation cursor is absent or unstable";
      return false;
    }
    decoded.cursor = std::move(cursor);
  } else if (!result["cursor"].is_null()) {
    *message = "final collection page retained a cursor";
    return false;
  }
  *metadata = std::move(decoded);
  return true;
}

/**
 * @brief Adds stable page controls to one method's copied base parameters.
 *
 * @param base_params Typed non-page parameters for the method.
 * @param cursor Stable continuation cursor, absent for the first page.
 * @param offset Exact next outer-row offset.
 * @param limit Requested row ceiling; first call uses 4,096 and continuation
 *        calls use the observed frame-safe first-page size.
 * @return Complete owned params object for one RPC attempt.
 * @throws std::bad_alloc if copied JSON storage cannot be allocated.
 * @note The helper never stores or retries the request. It preserves all
 *       original typed parameters on every continuation.
 */
internal::Json collection_page_params(const internal::Json& base_params,
                                      const std::optional<std::string>& cursor,
                                      std::size_t offset, std::size_t limit) {
  internal::Json params = base_params;
  params["limit"] = limit;
  if (cursor) {
    params["cursor"] = *cursor;
    params["offset"] = offset;
  }
  return params;
}

/**
 * @brief Measures normalized compact JSON row bytes for one decoded page.
 * @param canonical_rows Canonically re-encoded known public row values.
 * @param recursive_entries Recursively visible public entries in the page.
 * @param outer_rows Number of decoded indivisible page rows.
 * @param shared_bytes First-page bytes retained outside the row array.
 * @param measurement Receives the complete footprint only after validation.
 * @param message Receives a local protocol diagnostic on malformed input.
 * @return True when canonical rows and decoded counts agree and summation fits.
 * @throws std::bad_alloc if compact row encoding or diagnostics allocate.
 * @note The canonical known-field row array is bounded by one response frame,
 *       compact-encoded row by row, and discarded after measurement.
 */
bool measure_collection_page(const internal::Json& canonical_rows,
                             std::size_t recursive_entries,
                             std::size_t outer_rows, std::size_t shared_bytes,
                             internal::CollectionPageMeasurement* measurement,
                             std::string* message) {
  if (measurement == nullptr || message == nullptr ||
      !canonical_rows.is_array() || canonical_rows.size() != outer_rows) {
    if (message != nullptr) {
      *message = "stable collection row measurement is inconsistent";
    }
    return false;
  }

  std::size_t row_bytes = 0;
  for (const internal::Json& row : canonical_rows) {
    const std::size_t encoded_size = row.dump().size();
    if (encoded_size > std::numeric_limits<std::size_t>::max() - row_bytes) {
      *message = "stable collection row byte measurement overflowed";
      return false;
    }
    row_bytes += encoded_size;
  }
  *measurement = {recursive_entries, outer_rows, row_bytes, shared_bytes};
  return true;
}

/**
 * @brief Aggregates one complete stable collection through repeated typed RPCs.
 *
 * @tparam A Complete public aggregate value returned to the caller.
 * @tparam P Private decoded page value.
 * @tparam C Callable returning one `RawCallResult` for cursor controls.
 * @tparam D Callable transactionally decoding method-specific rows.
 * @tparam M Callable measuring recursive entries and canonical encoded bytes.
 * @tparam E Callable validating and moving one page into aggregate.
 * @param call_page Performs one exact RPC attempt for supplied page controls.
 * @param decode_page Decodes result rows and reports their outer-row count.
 * @param measure_page Measures the decoded page before aggregate publication.
 * @param append_page Extends local unpublished aggregate transactionally.
 * @return Complete aggregate after the final page, or the first exact failure.
 * @throws std::bad_alloc if requests, pages, diagnostics, or aggregate storage
 *         cannot be allocated.
 * @note No page or partial aggregate is published after failure. Recursive
 *       entry and 64 MiB byte admission occurs before append. The function
 *       never retries a call. A frame-safe first page may contain fewer than
 *       4,096 rows, so that observed size becomes the continuation limit.
 */
template <typename A, typename P, typename C, typename D, typename M,
          typename E>  // NOLINT(whitespace/indent_namespace)
IpcResult<A> aggregate_collection(C call_page, D decode_page, M measure_page,
                                  E append_page) {
  A aggregate;
  std::optional<std::string> stable_cursor;
  std::size_t expected_offset = 0;
  std::size_t request_limit = internal::kGeneralPageMaxEntries;
  internal::CollectionAggregateBudget aggregate_budget;
  while (true) {
    RawCallResult call =
        call_page(stable_cursor, expected_offset, request_limit);
    if (!call.status.ok) {
      return failed_result<A>(std::move(call.status));
    }
    P page;
    std::size_t page_rows = 0;
    std::string message;
    if (!decode_page(call.result, &page, &page_rows, &message)) {
      return failed_result<A>(invalid_response(std::move(message)));
    }
    CollectionPageMetadata metadata;
    if (!decode_collection_page_metadata(call.result, expected_offset,
                                         page_rows, stable_cursor, &metadata,
                                         &message)) {
      return failed_result<A>(invalid_response(std::move(message)));
    }
    internal::CollectionPageMeasurement measurement;
    if (!measure_page(call.result, page, !stable_cursor.has_value(),
                      &measurement, &message) ||
        !aggregate_budget.admit(measurement, &message)) {
      return failed_result<A>(invalid_response(std::move(message)));
    }
    if (!append_page(&aggregate, std::move(page), &message)) {
      return failed_result<A>(invalid_response(std::move(message)));
    }
    if (!metadata.has_more) {
      return {internal::ok_status(), std::move(aggregate)};
    }
    stable_cursor = std::move(metadata.cursor);
    expected_offset = metadata.next_offset;
    request_limit = std::min(request_limit, page_rows);
  }
}

/**
 * @brief Decodes one exact session-id echo from a successful result object.
 * @param result Candidate method result.
 * @param expected Exact opaque session id sent by the Client.
 * @param message Receives a diagnostic on failure.
 * @return True only when the required result id is valid and equal.
 * @throws std::bad_alloc if copied id or diagnostic storage cannot allocate.
 * @note Unknown result members are ignored for forward compatibility.
 */
bool decode_session_echo(const internal::Json& result,
                         const IpcSessionId& expected, std::string* message) {
  std::string decoded;
  if (!result.is_object() || !result.contains("session_id") ||
      !internal::decode_opaque_id(result["session_id"], &decoded) ||
      decoded != expected.value) {
    if (message != nullptr) {
      *message = "result returned an invalid or mismatched session_id";
    }
    return false;
  }
  return true;
}

/**
 * @brief Decodes one nullable version 1 timing value.
 * @param value Candidate JSON number or null.
 * @param output Receives a finite value or quiet NaN for null.
 * @return True on success; false without modifying output otherwise.
 * @throws Nothing.
 * @note Non-finite Host values are encoded as null. Other JSON types and
 *       parser-produced non-finite numbers are invalid result shapes.
 */
bool decode_wire_double(const internal::Json& value, double* output) noexcept {
  if (output == nullptr) {
    return false;
  }
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
 * @brief Decodes one bounded array of nonnegative public node ids.
 * @param value Candidate JSON array.
 * @param nodes Receives the complete ordered page transactionally.
 * @param message Receives a diagnostic on failure.
 * @return True when every id fits `NodeId` and the general page bound.
 * @throws std::bad_alloc if page or diagnostic storage cannot be allocated.
 */
bool decode_node_id_array(const internal::Json& value,
                          std::vector<NodeId>* nodes, std::string* message) {
  if (nodes == nullptr || message == nullptr ||
      !internal::valid_bounded_array(value, internal::kGeneralPageMaxEntries)) {
    if (message != nullptr) {
      *message = "node-id page is not a bounded array";
    }
    return false;
  }
  std::vector<NodeId> decoded;
  decoded.reserve(value.size());
  for (const internal::Json& item : value) {
    NodeId node;
    if (!internal::decode_integer(item, &node.value) || node.value < 0) {
      *message = "node-id page contains an invalid node id";
      return false;
    }
    decoded.push_back(node);
  }
  *nodes = std::move(decoded);
  return true;
}

/**
 * @brief Decodes one bounded array of nonnegative planning task ids.
 * @param value Candidate JSON array.
 * @param task_ids Receives the complete ordered ids transactionally.
 * @param message Receives a diagnostic on failure.
 * @return True when every id fits `int` and the general page bound.
 * @throws std::bad_alloc if temporary or diagnostic storage cannot allocate.
 */
bool decode_task_id_array(const internal::Json& value,
                          std::vector<int>* task_ids, std::string* message) {
  if (task_ids == nullptr || message == nullptr ||
      !internal::valid_bounded_array(value, internal::kGeneralPageMaxEntries)) {
    if (message != nullptr) {
      *message = "planning dependency ids are not a bounded array";
    }
    return false;
  }
  std::vector<int> decoded;
  decoded.reserve(value.size());
  for (const internal::Json& item : value) {
    int task_id = -1;
    if (!internal::decode_integer(item, &task_id) || task_id < 0) {
      *message = "planning dependency ids contain an invalid task id";
      return false;
    }
    decoded.push_back(task_id);
  }
  *task_ids = std::move(decoded);
  return true;
}

/**
 * @brief Decodes one copied compute-planning task sample.
 * @param value Candidate task object.
 * @param task Receives the complete public task transactionally.
 * @param message Receives a diagnostic on failure.
 * @return True when every required scalar, enum, ROI, and dependency is valid.
 * @throws std::bad_alloc if copied strings, vectors, or diagnostics allocate.
 */
bool decode_planning_task(const internal::Json& value,
                          ComputePlanningTaskSnapshot* task,
                          std::string* message) {
  static constexpr std::array<const char*, 12> kFields = {
      "task_id",        "node_id",          "kind",
      "domain",         "output_roi",       "tile_x",
      "tile_y",         "tile_size",        "whole_output",
      "dirty_selected", "dirty_generation", "dependency_task_ids"};
  if (task == nullptr || message == nullptr || !value.is_object() ||
      std::any_of(
          kFields.begin(), kFields.end(),
          [&value](const char* field) { return !value.contains(field); }) ||
      !value["whole_output"].is_boolean() ||
      !value["dirty_selected"].is_boolean()) {
    if (message != nullptr) {
      *message = "planning task has an invalid shape";
    }
    return false;
  }
  ComputePlanningTaskSnapshot decoded;
  if (!internal::decode_integer(value["task_id"], &decoded.task_id) ||
      decoded.task_id < 0 ||
      !internal::decode_integer(value["node_id"], &decoded.node.value) ||
      decoded.node.value < 0 ||
      !internal::decode_bounded_string(
          value["kind"], internal::kShortTextMaxBytes, &decoded.kind) ||
      !internal::decode_enum(value["domain"], &decoded.domain) ||
      !internal::decode_pixel_rect(value["output_roi"], &decoded.output_roi) ||
      !internal::decode_integer(value["tile_x"], &decoded.tile_x) ||
      !internal::decode_integer(value["tile_y"], &decoded.tile_y) ||
      !internal::decode_integer(value["tile_size"], &decoded.tile_size) ||
      !internal::decode_integer(value["dirty_generation"],
                                &decoded.dirty_generation) ||
      !decode_task_id_array(value["dependency_task_ids"],
                            &decoded.dependency_task_ids, message)) {
    if (message->empty()) {
      *message = "planning task contains an invalid typed value";
    }
    return false;
  }
  decoded.whole_output = value["whole_output"].get<bool>();
  decoded.dirty_selected = value["dirty_selected"].get<bool>();
  *task = std::move(decoded);
  return true;
}

/**
 * @brief Decodes one complete indivisible compute-planning snapshot.
 * @param value Candidate planning object.
 * @param snapshot Receives the complete public value transactionally.
 * @param message Receives a diagnostic on failure.
 * @return True when every required value and bounded sample validates.
 * @throws std::bad_alloc if copied text, samples, or diagnostics allocate.
 * @note Counts use exact `std::size_t` decoding and are never routed through
 *       floating point or narrowed from an oversized unsigned integer.
 */
bool decode_compute_planning(const internal::Json& value,
                             ComputePlanningInspectionSnapshot* snapshot,
                             std::string* message) {
  static constexpr std::array<const char*, 18> kFields = {
      "intent",
      "target_node_id",
      "parallel",
      "topology_generation",
      "expansion_cache_key",
      "planned_node_count",
      "task_count",
      "tile_task_count",
      "monolithic_task_count",
      "node_task_count",
      "dependency_count",
      "initial_task_count",
      "active_task_count",
      "dirty_source_task_count",
      "downstream_task_count",
      "initial_downstream_task_count",
      "planned_node_sample",
      "task_sample"};
  if (snapshot == nullptr || message == nullptr || !value.is_object() ||
      std::any_of(
          kFields.begin(), kFields.end(),
          [&value](const char* field) { return !value.contains(field); }) ||
      !value["parallel"].is_boolean() ||
      !internal::valid_bounded_array(value["task_sample"],
                                     internal::kGeneralPageMaxEntries)) {
    if (message != nullptr) {
      *message = "compute planning result has an invalid shape";
    }
    return false;
  }

  ComputePlanningInspectionSnapshot decoded;
  if (!internal::decode_enum(value["intent"], &decoded.intent) ||
      !internal::decode_integer(value["target_node_id"],
                                &decoded.target_node.value) ||
      decoded.target_node.value < 0 ||
      !internal::decode_integer(value["topology_generation"],
                                &decoded.topology_generation) ||
      !internal::decode_bounded_string(value["expansion_cache_key"],
                                       internal::kLargeTextMaxBytes,
                                       &decoded.expansion_cache_key) ||
      !internal::decode_integer(value["planned_node_count"],
                                &decoded.planned_node_count) ||
      !internal::decode_integer(value["task_count"], &decoded.task_count) ||
      !internal::decode_integer(value["tile_task_count"],
                                &decoded.tile_task_count) ||
      !internal::decode_integer(value["monolithic_task_count"],
                                &decoded.monolithic_task_count) ||
      !internal::decode_integer(value["node_task_count"],
                                &decoded.node_task_count) ||
      !internal::decode_integer(value["dependency_count"],
                                &decoded.dependency_count) ||
      !internal::decode_integer(value["initial_task_count"],
                                &decoded.initial_task_count) ||
      !internal::decode_integer(value["active_task_count"],
                                &decoded.active_task_count) ||
      !internal::decode_integer(value["dirty_source_task_count"],
                                &decoded.dirty_source_task_count) ||
      !internal::decode_integer(value["downstream_task_count"],
                                &decoded.downstream_task_count) ||
      !internal::decode_integer(value["initial_downstream_task_count"],
                                &decoded.initial_downstream_task_count) ||
      !decode_node_id_array(value["planned_node_sample"],
                            &decoded.planned_node_sample, message)) {
    if (message->empty()) {
      *message = "compute planning result contains an invalid typed value";
    }
    return false;
  }
  decoded.parallel = value["parallel"].get<bool>();
  decoded.task_sample.reserve(value["task_sample"].size());
  for (const internal::Json& item : value["task_sample"]) {
    ComputePlanningTaskSnapshot task;
    if (!decode_planning_task(item, &task, message)) {
      return false;
    }
    decoded.task_sample.push_back(std::move(task));
  }
  *snapshot = std::move(decoded);
  return true;
}

/**
 * @brief Decodes one bounded ordered rectangle array.
 * @param value Candidate JSON array.
 * @param rectangles Receives every rectangle transactionally.
 * @param message Receives a diagnostic on failure.
 * @return True when the array and every exact integer rectangle validate.
 * @throws std::bad_alloc if temporary or diagnostic storage cannot allocate.
 */
bool decode_rectangle_array(const internal::Json& value,
                            std::vector<PixelRect>* rectangles,
                            std::string* message) {
  if (rectangles == nullptr || message == nullptr ||
      !internal::valid_bounded_array(value, internal::kGeneralPageMaxEntries)) {
    if (message != nullptr) {
      *message = "rectangle collection is not a bounded array";
    }
    return false;
  }
  std::vector<PixelRect> decoded;
  decoded.reserve(value.size());
  for (const internal::Json& item : value) {
    PixelRect rectangle;
    if (!internal::decode_pixel_rect(item, &rectangle)) {
      *message = "rectangle collection contains an invalid rectangle";
      return false;
    }
    decoded.push_back(rectangle);
  }
  *rectangles = std::move(decoded);
  return true;
}

/**
 * @brief Decodes one complete dirty-region inspection result.
 * @param value Candidate result object including the session echo.
 * @param expected_session Exact opaque session requested by the Client.
 * @param snapshot Receives the complete public value transactionally.
 * @param message Receives a diagnostic on failure.
 * @return True when every nested id, enum, ROI, count, and boolean validates.
 * @throws std::bad_alloc if nested public storage or diagnostics allocate.
 * @note Duplicate `actual_dirty_rois` node keys are rejected so a malformed
 *       response cannot silently overwrite one Host row during map assembly.
 */
bool decode_dirty_region(const internal::Json& value,
                         const IpcSessionId& expected_session,
                         DirtyRegionInspectionSnapshot* snapshot,
                         std::string* message) {
  static constexpr std::array<const char*, 6> kFields = {
      "graph_generation",       "sources",           "dirty_tiles",
      "dirty_monolithic_nodes", "actual_dirty_rois", "edge_mappings"};
  if (snapshot == nullptr || message == nullptr ||
      !decode_session_echo(value, expected_session, message) ||
      std::any_of(
          kFields.begin(), kFields.end(),
          [&value](const char* field) { return !value.contains(field); }) ||
      !internal::valid_bounded_array(value["sources"],
                                     internal::kGeneralPageMaxEntries) ||
      !internal::valid_bounded_array(value["dirty_tiles"],
                                     internal::kGeneralPageMaxEntries) ||
      !internal::valid_bounded_array(value["dirty_monolithic_nodes"],
                                     internal::kGeneralPageMaxEntries) ||
      !internal::valid_bounded_array(value["actual_dirty_rois"],
                                     internal::kGeneralPageMaxEntries) ||
      !internal::valid_bounded_array(value["edge_mappings"],
                                     internal::kGeneralPageMaxEntries)) {
    if (message != nullptr && message->empty()) {
      *message = "dirty-region result has an invalid shape";
    }
    return false;
  }

  DirtyRegionInspectionSnapshot decoded;
  if (!internal::decode_integer(value["graph_generation"],
                                &decoded.graph_generation)) {
    *message = "dirty-region graph_generation is invalid";
    return false;
  }

  decoded.sources.reserve(value["sources"].size());
  for (const internal::Json& item : value["sources"]) {
    if (!item.is_object() || !item.contains("node_id") ||
        !item.contains("domain") || !item.contains("lifecycle") ||
        !item.contains("generation") || !item.contains("source_rois")) {
      *message = "dirty source row has an invalid shape";
      return false;
    }
    DirtySourceSnapshot source;
    if (!internal::decode_integer(item["node_id"], &source.node.value) ||
        source.node.value < 0 ||
        !internal::decode_enum(item["domain"], &source.domain) ||
        !internal::decode_enum(item["lifecycle"], &source.lifecycle) ||
        !internal::decode_integer(item["generation"], &source.generation) ||
        !decode_rectangle_array(item["source_rois"], &source.source_rois,
                                message)) {
      if (message->empty()) {
        *message = "dirty source row contains an invalid typed value";
      }
      return false;
    }
    decoded.sources.push_back(std::move(source));
  }

  decoded.dirty_tiles.reserve(value["dirty_tiles"].size());
  for (const internal::Json& item : value["dirty_tiles"]) {
    if (!item.is_object() || !item.contains("node_id") ||
        !item.contains("domain") || !item.contains("tile_x") ||
        !item.contains("tile_y") || !item.contains("tile_size") ||
        !item.contains("pixel_roi")) {
      *message = "dirty tile row has an invalid shape";
      return false;
    }
    DirtyTileSnapshot tile;
    if (!internal::decode_integer(item["node_id"], &tile.node.value) ||
        tile.node.value < 0 ||
        !internal::decode_enum(item["domain"], &tile.domain) ||
        !internal::decode_integer(item["tile_x"], &tile.tile_x) ||
        !internal::decode_integer(item["tile_y"], &tile.tile_y) ||
        !internal::decode_integer(item["tile_size"], &tile.tile_size) ||
        !internal::decode_pixel_rect(item["pixel_roi"], &tile.pixel_roi)) {
      *message = "dirty tile row contains an invalid typed value";
      return false;
    }
    decoded.dirty_tiles.push_back(tile);
  }

  decoded.dirty_monolithic_nodes.reserve(
      value["dirty_monolithic_nodes"].size());
  for (const internal::Json& item : value["dirty_monolithic_nodes"]) {
    if (!item.is_object() || !item.contains("node_id") ||
        !item.contains("domain") || !item.contains("pixel_roi") ||
        !item.value("whole_output", internal::Json()).is_boolean()) {
      *message = "dirty monolithic row has an invalid shape";
      return false;
    }
    DirtyMonolithicRegionSnapshot region;
    if (!internal::decode_integer(item["node_id"], &region.node.value) ||
        region.node.value < 0 ||
        !internal::decode_enum(item["domain"], &region.domain) ||
        !internal::decode_pixel_rect(item["pixel_roi"], &region.pixel_roi)) {
      *message = "dirty monolithic row contains an invalid typed value";
      return false;
    }
    region.whole_output = item["whole_output"].get<bool>();
    decoded.dirty_monolithic_nodes.push_back(region);
  }

  for (const internal::Json& item : value["actual_dirty_rois"]) {
    if (!item.is_object() || !item.contains("node_id") ||
        !item.contains("rois")) {
      *message = "actual dirty ROI row has an invalid shape";
      return false;
    }
    int node_id = -1;
    std::vector<PixelRect> rectangles;
    if (!internal::decode_integer(item["node_id"], &node_id) || node_id < 0 ||
        !decode_rectangle_array(item["rois"], &rectangles, message) ||
        !decoded.actual_dirty_rois.emplace(node_id, std::move(rectangles))
             .second) {
      if (message->empty()) {
        *message = "actual dirty ROI row is invalid or duplicated";
      }
      return false;
    }
  }

  decoded.edge_mappings.reserve(value["edge_mappings"].size());
  for (const internal::Json& item : value["edge_mappings"]) {
    static constexpr std::array<const char*, 6> kEdgeFields = {
        "from_node_id", "to_node_id", "domain",
        "from_roi",     "to_roi",     "direction"};
    if (!item.is_object() || std::any_of(kEdgeFields.begin(), kEdgeFields.end(),
                                         [&item](const char* field) {
                                           return !item.contains(field);
                                         })) {
      *message = "dirty edge row has an invalid shape";
      return false;
    }
    DirtyEdgeMappingSnapshot mapping;
    if (!internal::decode_integer(item["from_node_id"],
                                  &mapping.from_node.value) ||
        !internal::decode_integer(item["to_node_id"], &mapping.to_node.value) ||
        mapping.from_node.value < 0 || mapping.to_node.value < 0 ||
        !internal::decode_enum(item["domain"], &mapping.domain) ||
        !internal::decode_pixel_rect(item["from_roi"], &mapping.from_roi) ||
        !internal::decode_pixel_rect(item["to_roi"], &mapping.to_roi) ||
        !internal::decode_enum(item["direction"], &mapping.direction)) {
      *message = "dirty edge row contains an invalid typed value";
      return false;
    }
    decoded.edge_mappings.push_back(mapping);
  }
  *snapshot = std::move(decoded);
  return true;
}

/**
 * @brief Decodes one complete timing response for an opaque session.
 * @param value Candidate result object.
 * @param expected_session Exact session requested by the Client.
 * @param timing Receives the complete timing snapshot transactionally.
 * @param message Receives a diagnostic on failure.
 * @return True when every row and nullable timing number validates.
 * @throws std::bad_alloc if copied row strings or diagnostics allocate.
 */
bool decode_timing_result(const internal::Json& value,
                          const IpcSessionId& expected_session,
                          TimingSnapshot* timing, std::string* message) {
  if (timing == nullptr || message == nullptr ||
      !decode_session_echo(value, expected_session, message) ||
      !value.contains("node_timings") || !value.contains("total_ms") ||
      !internal::valid_bounded_array(value["node_timings"],
                                     internal::kGeneralPageMaxEntries)) {
    if (message != nullptr && message->empty()) {
      *message = "compute timing result has an invalid shape";
    }
    return false;
  }
  TimingSnapshot decoded;
  if (!decode_wire_double(value["total_ms"], &decoded.total_ms)) {
    *message = "compute timing total_ms is invalid";
    return false;
  }
  decoded.node_timings.reserve(value["node_timings"].size());
  for (const internal::Json& item : value["node_timings"]) {
    if (!item.is_object() || !item.contains("node_id") ||
        !item.contains("name") || !item.contains("elapsed_ms") ||
        !item.contains("source")) {
      *message = "compute timing row has an invalid shape";
      return false;
    }
    NodeTimingSnapshot row;
    if (!internal::decode_integer(item["node_id"], &row.node.value) ||
        row.node.value < 0 ||
        !internal::decode_bounded_string(
            item["name"], internal::kShortTextMaxBytes, &row.name) ||
        !decode_wire_double(item["elapsed_ms"], &row.elapsed_ms) ||
        !internal::decode_bounded_string(
            item["source"], internal::kLargeTextMaxBytes, &row.source)) {
      *message = "compute timing row contains an invalid typed value";
      return false;
    }
    decoded.node_timings.push_back(std::move(row));
  }
  *timing = std::move(decoded);
  return true;
}

/**
 * @brief Returns the version 1 successor for one valid observation sequence.
 * @param sequence Valid publication sequence below the exhausted sentinel.
 * @return Next sequence, or the exhausted sentinel after the final value.
 * @throws Nothing.
 */
std::uint64_t observation_successor(std::uint64_t sequence) noexcept {
  return sequence == kObservationSequenceExhausted - 1
             ? kObservationSequenceExhausted
             : sequence + 1;
}

/**
 * @brief Decodes one bounded destructive compute-event result.
 * @param value Candidate result object.
 * @param expected_session Exact session requested by the Client.
 * @param requested_limit Exact destructive limit sent by the Client.
 * @param batch Receives the complete batch transactionally.
 * @param message Receives a diagnostic on failure.
 * @return True when event order and all batch metadata are coherent.
 * @throws std::bad_alloc if copied event strings or diagnostics allocate.
 * @note Validation never triggers another drain, so a malformed response
 *       cannot remove a second page while attempting recovery.
 */
bool decode_compute_event_result(const internal::Json& value,
                                 const IpcSessionId& expected_session,
                                 std::size_t requested_limit,
                                 ComputeEventBatch* batch,
                                 std::string* message) {
  if (batch == nullptr || message == nullptr ||
      requested_limit < kComputeEventDrainMinLimit ||
      requested_limit > kComputeEventDrainMaxLimit ||
      !decode_session_echo(value, expected_session, message) ||
      !value.contains("events") || !value.contains("next_sequence") ||
      !value.value("has_more", internal::Json()).is_boolean() ||
      !value.contains("dropped_count") ||
      !internal::valid_bounded_array(value["events"], requested_limit)) {
    if (message != nullptr && message->empty()) {
      *message = "compute-event result has an invalid shape or limit";
    }
    return false;
  }
  ComputeEventBatch decoded;
  decoded.events.reserve(value["events"].size());
  std::uint64_t previous_sequence = 0;
  for (const internal::Json& item : value["events"]) {
    if (!item.is_object() || !item.contains("sequence") ||
        !item.contains("node_id") || !item.contains("name") ||
        !item.contains("source") || !item.contains("elapsed_ms")) {
      *message = "compute-event row has an invalid shape";
      return false;
    }
    ComputeEventSnapshot event;
    if (!internal::decode_integer(item["sequence"], &event.sequence) ||
        event.sequence == 0 ||
        event.sequence == kObservationSequenceExhausted ||
        event.sequence <= previous_sequence ||
        !internal::decode_integer(item["node_id"], &event.node.value) ||
        event.node.value < 0 ||
        !internal::decode_bounded_string(
            item["name"], kComputeEventTextMaxBytes, &event.name) ||
        !internal::decode_bounded_string(
            item["source"], kComputeEventTextMaxBytes, &event.source) ||
        !decode_wire_double(item["elapsed_ms"], &event.elapsed_ms)) {
      *message = "compute-event row contains an invalid typed value";
      return false;
    }
    previous_sequence = event.sequence;
    decoded.events.push_back(std::move(event));
  }
  if (!internal::decode_integer(value["next_sequence"],
                                &decoded.next_sequence) ||
      !internal::decode_integer(value["dropped_count"],
                                &decoded.dropped_count) ||
      decoded.next_sequence == 0) {
    *message = "compute-event sequence metadata is invalid";
    return false;
  }
  decoded.has_more = value["has_more"].get<bool>();
  if ((decoded.has_more &&
       (decoded.events.size() != requested_limit || decoded.events.empty() ||
        decoded.next_sequence == kObservationSequenceExhausted)) ||
      (!decoded.events.empty() &&
       decoded.next_sequence !=
           observation_successor(decoded.events.back().sequence)) ||
      (decoded.events.empty() && decoded.has_more)) {
    *message = "compute-event page metadata is inconsistent";
    return false;
  }
  *batch = std::move(decoded);
  return true;
}

/**
 * @brief Decodes one bounded non-destructive scheduler-trace result.
 * @param value Candidate result object.
 * @param expected_session Exact session requested by the Client.
 * @param after_sequence Exact exclusive sequence cursor sent by the Client.
 * @param requested_limit Exact trace limit sent by the Client.
 * @param page Receives the complete trace page transactionally.
 * @param message Receives a diagnostic on failure.
 * @return True when event order, sentinel, gap, and page metadata validate.
 * @throws std::bad_alloc if page or diagnostic storage cannot allocate.
 */
bool decode_scheduler_trace_result(const internal::Json& value,
                                   const IpcSessionId& expected_session,
                                   std::uint64_t after_sequence,
                                   std::size_t requested_limit,
                                   SchedulerTracePage* page,
                                   std::string* message) {
  if (page == nullptr || message == nullptr ||
      requested_limit < kSchedulerTraceMinLimit ||
      requested_limit > kSchedulerTraceMaxLimit ||
      !decode_session_echo(value, expected_session, message) ||
      !value.contains("events") || !value.contains("next_sequence") ||
      !value.value("has_more", internal::Json()).is_boolean() ||
      !value.contains("dropped_count") ||
      !internal::valid_bounded_array(value["events"], requested_limit)) {
    if (message != nullptr && message->empty()) {
      *message = "scheduler-trace result has an invalid shape or limit";
    }
    return false;
  }
  SchedulerTracePage decoded;
  decoded.events.reserve(value["events"].size());
  std::uint64_t previous_sequence = after_sequence;
  for (const internal::Json& item : value["events"]) {
    static constexpr std::array<const char*, 6> kFields = {
        "sequence", "epoch", "node_id", "worker_id", "action", "timestamp_us"};
    if (!item.is_object() ||
        std::any_of(kFields.begin(), kFields.end(), [&item](const char* field) {
          return !item.contains(field);
        })) {
      *message = "scheduler-trace row has an invalid shape";
      return false;
    }
    SchedulerTraceEventSnapshot event;
    if (!internal::decode_integer(item["sequence"], &event.sequence) ||
        event.sequence == 0 ||
        event.sequence == kObservationSequenceExhausted ||
        event.sequence <= previous_sequence ||
        !internal::decode_integer(item["epoch"], &event.epoch) ||
        !internal::decode_integer(item["node_id"], &event.node.value) ||
        event.node.value < -1 ||
        !internal::decode_integer(item["worker_id"], &event.worker_id) ||
        event.worker_id < -1 ||
        !internal::decode_enum(item["action"], &event.action) ||
        !internal::decode_integer(item["timestamp_us"], &event.timestamp_us)) {
      *message = "scheduler-trace row contains an invalid typed value";
      return false;
    }
    previous_sequence = event.sequence;
    decoded.events.push_back(event);
  }
  if (!internal::decode_integer(value["next_sequence"],
                                &decoded.next_sequence) ||
      !internal::decode_integer(value["dropped_count"],
                                &decoded.dropped_count)) {
    *message = "scheduler-trace sequence metadata is invalid";
    return false;
  }
  decoded.has_more = value["has_more"].get<bool>();
  const bool exhausted = decoded.next_sequence == kObservationSequenceExhausted;
  if ((decoded.has_more && (decoded.events.size() != requested_limit ||
                            decoded.events.empty() || exhausted)) ||
      (exhausted &&
       (decoded.has_more ||
        (!decoded.events.empty() && decoded.events.back().sequence !=
                                        kObservationSequenceExhausted - 1))) ||
      (!exhausted && !decoded.events.empty() &&
       decoded.next_sequence != decoded.events.back().sequence) ||
      (!exhausted && decoded.events.empty() &&
       (decoded.next_sequence != after_sequence || decoded.has_more))) {
    *message = "scheduler-trace page metadata is inconsistent";
    return false;
  }
  *page = std::move(decoded);
  return true;
}

/**
 * @brief Private owned row used while decoding operation source pages.
 *
 * @throws std::bad_alloc when copied key or source storage cannot allocate.
 * @note Rows are converted into the public map only after global strict
 *       ordering and duplicate checks succeed across every page.
 */
struct PluginSourcePageRow {
  /** @brief Nonempty operation key used for deterministic ordering. */
  std::string key;

  /** @brief Copied source label or plugin path. */
  std::string source;
};

/**
 * @brief Private owned traversal-order row used by stable page aggregation.
 *
 * @throws std::bad_alloc when copied node-id storage cannot allocate.
 * @note The row remains indivisible on the wire and becomes one public map
 *       entry only after duplicate-key validation.
 */
struct TraversalOrderPageRow {
  /** @brief Nonnegative ending-node map key. */
  int ending_node_id = -1;

  /** @brief Complete ordered traversal ids for the branch. */
  std::vector<NodeId> node_ids;
};

/**
 * @brief Private owned traversal-detail row used by stable page aggregation.
 *
 * @throws std::bad_alloc when copied names or node storage cannot allocate.
 * @note The row contains only public snapshots and no cache or graph handle.
 */
struct TraversalDetailPageRow {
  /** @brief Nonnegative ending-node map key. */
  int ending_node_id = -1;

  /** @brief Complete ordered traversal metadata for the branch. */
  std::vector<HostTraversalNodeSnapshot> nodes;
};

/**
 * @brief Canonically encodes one decoded node-id page.
 * @param page Validated public node ids.
 * @return Exact known-field row array used by the daemon measurement.
 * @throws std::invalid_argument if a decoded id violates its invariant.
 * @throws std::bad_alloc if JSON storage cannot allocate.
 */
internal::Json canonical_collection_rows(const std::vector<NodeId>& page) {
  return internal::encode_node_ids(page);
}

/**
 * @brief Canonically encodes one decoded string page.
 * @param page Validated public string rows.
 * @return Exact known-field row array used by the daemon measurement.
 * @throws std::bad_alloc if JSON storage cannot allocate.
 * @note Component UTF-8 and byte limits were already enforced by the decoder;
 *       this helper deliberately drops forward-compatible unknown members.
 */
internal::Json canonical_collection_rows(const std::vector<std::string>& page) {
  internal::Json rows = internal::Json::array();
  for (const std::string& row : page) {
    rows.push_back(row);
  }
  return rows;
}

/**
 * @brief Canonically encodes one decoded graph-session page.
 * @param page Validated owned session summaries.
 * @return Exact known-field row array used by the daemon measurement.
 * @throws std::length_error for an impossible over-limit decoded name.
 * @throws std::invalid_argument for an impossible malformed decoded id/name.
 * @throws std::bad_alloc if JSON storage cannot allocate.
 */
internal::Json canonical_collection_rows(
    const std::vector<GraphSessionSummary>& page) {
  return internal::encode_session_summaries(page);
}

/**
 * @brief Canonically encodes one decoded operation-source page.
 * @param page Validated private key/source rows.
 * @return Exact known-field row array used by the daemon measurement.
 * @throws std::length_error for an impossible over-limit decoded value.
 * @throws std::invalid_argument for impossible invalid UTF-8.
 * @throws std::bad_alloc if JSON storage cannot allocate.
 */
internal::Json canonical_collection_rows(
    const std::vector<PluginSourcePageRow>& page) {
  internal::Json rows = internal::Json::array();
  for (const PluginSourcePageRow& row : page) {
    rows.push_back(internal::encode_plugin_source_row(row.key, row.source));
  }
  return rows;
}

/**
 * @brief Canonically encodes one decoded traversal-order page.
 * @param page Validated private branch rows.
 * @return Exact known-field row array used by the daemon measurement.
 * @throws std::invalid_argument for an impossible negative decoded id.
 * @throws std::bad_alloc if JSON storage cannot allocate.
 */
internal::Json canonical_collection_rows(
    const std::vector<TraversalOrderPageRow>& page) {
  internal::Json rows = internal::Json::array();
  for (const TraversalOrderPageRow& row : page) {
    rows.push_back(
        internal::Json{{"ending_node_id", row.ending_node_id},
                       {"node_ids", internal::encode_node_ids(row.node_ids)}});
  }
  return rows;
}

/**
 * @brief Canonically encodes one decoded traversal-detail page.
 * @param page Validated private branch rows.
 * @return Exact known-field row array used by the daemon measurement.
 * @throws std::bad_alloc if copied names or JSON storage cannot allocate.
 * @note All ids, strings, and per-branch bounds were validated by the decoder.
 */
internal::Json canonical_collection_rows(
    const std::vector<TraversalDetailPageRow>& page) {
  internal::Json rows = internal::Json::array();
  for (const TraversalDetailPageRow& row : page) {
    internal::Json nodes = internal::Json::array();
    for (const HostTraversalNodeSnapshot& node : row.nodes) {
      nodes.push_back(
          internal::Json{{"node_id", node.node.value},
                         {"name", node.name},
                         {"has_memory_cache", node.has_memory_cache},
                         {"has_disk_cache", node.has_disk_cache}});
    }
    rows.push_back(internal::Json{{"ending_node_id", row.ending_node_id},
                                  {"nodes", std::move(nodes)}});
  }
  return rows;
}

/**
 * @brief Canonically encodes one decoded planning task sample.
 * @param task Validated public planning task.
 * @param encoded Receives the exact known-field object on success.
 * @param message Receives a protocol diagnostic for an impossible enum.
 * @return True when the task domain retains a version 1 wire label.
 * @throws std::bad_alloc if JSON or diagnostics cannot allocate.
 */
bool canonical_planning_task(const ComputePlanningTaskSnapshot& task,
                             internal::Json* encoded, std::string* message) {
  if (encoded == nullptr || message == nullptr) {
    return false;
  }
  internal::Json domain;
  if (!internal::encode_enum(task.domain, &domain)) {
    *message = "decoded planning task lost its domain label";
    return false;
  }
  *encoded = internal::Json{
      {"task_id", task.task_id},
      {"node_id", task.node.value},
      {"kind", task.kind},
      {"domain", std::move(domain)},
      {"output_roi", internal::encode_pixel_rect(task.output_roi)},
      {"tile_x", task.tile_x},
      {"tile_y", task.tile_y},
      {"tile_size", task.tile_size},
      {"whole_output", task.whole_output},
      {"dirty_selected", task.dirty_selected},
      {"dirty_generation", task.dirty_generation},
      {"dependency_task_ids", task.dependency_task_ids}};
  return true;
}

/**
 * @brief Canonically encodes one decoded planning snapshot row.
 * @param snapshot Validated public planning snapshot.
 * @param encoded Receives the exact known-field object on success.
 * @param message Receives a protocol diagnostic for an impossible enum.
 * @return True when all public enums retain version 1 labels.
 * @throws std::invalid_argument for an impossible negative decoded node id.
 * @throws std::bad_alloc if JSON, samples, or diagnostics cannot allocate.
 */
bool canonical_planning_snapshot(
    const ComputePlanningInspectionSnapshot& snapshot, internal::Json* encoded,
    std::string* message) {
  if (encoded == nullptr || message == nullptr) {
    return false;
  }
  internal::Json intent;
  if (!internal::encode_enum(snapshot.intent, &intent)) {
    *message = "decoded planning snapshot lost its intent label";
    return false;
  }
  internal::Json tasks = internal::Json::array();
  for (const ComputePlanningTaskSnapshot& task : snapshot.task_sample) {
    internal::Json encoded_task;
    if (!canonical_planning_task(task, &encoded_task, message)) {
      return false;
    }
    tasks.push_back(std::move(encoded_task));
  }
  *encoded = internal::Json{
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
      {"planned_node_sample",
       internal::encode_node_ids(snapshot.planned_node_sample)},
      {"task_sample", std::move(tasks)}};
  return true;
}

/**
 * @brief Adds recursively visible page entries without overflow or truncation.
 * @param addition Newly observed vector, map, or fixed-array entries.
 * @param entries Running page-local recursive total updated on success.
 * @param message Receives the stable snapshot-limit diagnostic on rejection.
 * @return True when the new total remains within 262,144 entries.
 * @throws std::bad_alloc if diagnostic assignment fails.
 * @note Page-local rejection complements the cross-page aggregate budget and
 *       avoids allowing an arithmetic wrap to disguise one hostile page.
 */
bool add_recursive_entries(std::size_t addition, std::size_t* entries,
                           std::string* message) {
  if (entries == nullptr || message == nullptr) {
    return false;
  }
  if (*entries > internal::kSnapshotMaxEntries ||
      addition > internal::kSnapshotMaxEntries - *entries) {
    *message = "stable collection exceeds 262144 recursive snapshot entries";
    return false;
  }
  *entries += addition;
  return true;
}

/**
 * @brief Adds parameter-map and fixed spatial-matrix entries for one node.
 * @param node Decoded public node snapshot.
 * @param entries Running page-local recursive total.
 * @param message Receives a limit diagnostic on rejection.
 * @return True when all nested entries fit the production snapshot bound.
 * @throws std::bad_alloc if diagnostic assignment fails.
 * @note Scalar object fields and optional-presence markers are not collection
 *       entries, matching daemon `node_nested_entry_count()` semantics.
 */
bool add_node_nested_entries(const NodeInspectionView& node,
                             std::size_t* entries, std::string* message) {
  if (!add_recursive_entries(node.parameters.size(), entries, message)) {
    return false;
  }
  static constexpr std::size_t kSpatialMatrixEntries = 3U * 9U;
  return !node.space ||
         add_recursive_entries(kSpatialMatrixEntries, entries, message);
}

/**
 * @brief Measures a page whose public rows contain no nested collections.
 * @tparam T Decoded row type.
 * @param page Decoded page rows.
 * @param first_page Whether this is the initial page; unused for flat values.
 * @param measurement Receives recursive entries and compact encoded bytes.
 * @param message Receives a local protocol diagnostic on rejection.
 * @return True when the normalized page footprint is valid.
 * @throws std::bad_alloc if compact row encoding or diagnostics allocate.
 * @note Flat graph/session/id/string/source rows contribute exactly one entry
 *       per outer row, matching daemon stable-snapshot measurement.
 */
template <typename T>
bool measure_flat_collection_page(
    const std::vector<T>& page, bool first_page,
    internal::CollectionPageMeasurement* measurement, std::string* message) {
  (void)first_page;
  return measure_collection_page(canonical_collection_rows(page), page.size(),
                                 page.size(), 0, measurement, message);
}

/**
 * @brief Measures graph rows plus every nested node collection on one page.
 * @param page Transactionally decoded public graph page.
 * @param first_page Whether this is the initial page; graph has no shared
 *        measured header and therefore does not use the value.
 * @param measurement Receives exact recursive and compact-byte footprint.
 * @param message Receives a local protocol diagnostic on rejection.
 * @return True when outer nodes, parameters, matrices, and bytes are bounded.
 * @throws std::bad_alloc if compact row encoding or diagnostics allocate.
 */
bool measure_graph_collection_page(
    const GraphInspectionView& page, bool first_page,
    internal::CollectionPageMeasurement* measurement, std::string* message) {
  (void)first_page;
  std::size_t entries = 0;
  if (!add_recursive_entries(page.nodes.size(), &entries, message)) {
    return false;
  }
  for (const NodeInspectionView& node : page.nodes) {
    if (!add_node_nested_entries(node, &entries, message)) {
      return false;
    }
  }
  const internal::Json canonical =
      internal::encode_graph(IpcSessionId{page.session.value}, page)["nodes"];
  return measure_collection_page(canonical, entries, page.nodes.size(), 0,
                                 measurement, message);
}

/**
 * @brief Measures one dependency page with its shared header exactly once.
 * @param session_id Exact opaque session id validated for every page.
 * @param page Transactionally decoded dependency page.
 * @param first_page Whether shared roots and header belong to this admission.
 * @param measurement Receives exact recursive and compact-byte footprint.
 * @param message Receives a local protocol diagnostic on rejection.
 * @return True when roots, entries, nested nodes, header, and row bytes fit.
 * @throws std::bad_alloc if copied JSON, compact encoding, or diagnostics
 *         allocate.
 * @note Continuations repeat the frozen header on the wire but the final
 *       public aggregate retains it once, matching daemon header accounting.
 */
bool measure_dependency_collection_page(
    const IpcSessionId& session_id, const HostDependencyTreeSnapshot& page,
    bool first_page, internal::CollectionPageMeasurement* measurement,
    std::string* message) {
  std::size_t entries = 0;
  if ((first_page &&
       !add_recursive_entries(page.root_nodes.size(), &entries, message)) ||
      !add_recursive_entries(page.entries.size(), &entries, message)) {
    return false;
  }
  for (const HostDependencyTreeEntry& entry : page.entries) {
    if (!add_node_nested_entries(entry.node, &entries, message)) {
      return false;
    }
  }

  std::size_t shared_bytes = 0;
  if (first_page) {
    HostDependencyTreeSnapshot header_value = page;
    header_value.entries.clear();
    const internal::Json header =
        internal::encode_dependency_tree(session_id, header_value);
    const std::size_t encoded_header_bytes = header.dump().size();
    if (encoded_header_bytes < 2U) {
      *message = "dependency-tree shared header lost its entries array";
      return false;
    }
    shared_bytes = encoded_header_bytes - 2U;
  }
  const internal::Json canonical =
      internal::encode_dependency_tree(session_id, page)["entries"];
  return measure_collection_page(canonical, entries, page.entries.size(),
                                 shared_bytes, measurement, message);
}

/**
 * @brief Measures traversal-order map rows and nested node-id vectors.
 * @param page Decoded private branch rows.
 * @param first_page Whether this is the initial page; no shared header exists.
 * @param measurement Receives exact recursive and compact-byte footprint.
 * @param message Receives a local protocol diagnostic on rejection.
 * @return True when outer branches, nested ids, and bytes are bounded.
 * @throws std::bad_alloc if compact row encoding or diagnostics allocate.
 */
bool measure_traversal_order_collection_page(
    const std::vector<TraversalOrderPageRow>& page, bool first_page,
    internal::CollectionPageMeasurement* measurement, std::string* message) {
  (void)first_page;
  std::size_t entries = 0;
  if (!add_recursive_entries(page.size(), &entries, message)) {
    return false;
  }
  for (const TraversalOrderPageRow& row : page) {
    if (!add_recursive_entries(row.node_ids.size(), &entries, message)) {
      return false;
    }
  }
  return measure_collection_page(canonical_collection_rows(page), entries,
                                 page.size(), 0, measurement, message);
}

/**
 * @brief Measures traversal-detail map rows and nested node vectors.
 * @param page Decoded private branch rows.
 * @param first_page Whether this is the initial page; no shared header exists.
 * @param measurement Receives exact recursive and compact-byte footprint.
 * @param message Receives a local protocol diagnostic on rejection.
 * @return True when outer branches, nested nodes, and bytes are bounded.
 * @throws std::bad_alloc if compact row encoding or diagnostics allocate.
 */
bool measure_traversal_detail_collection_page(
    const std::vector<TraversalDetailPageRow>& page, bool first_page,
    internal::CollectionPageMeasurement* measurement, std::string* message) {
  (void)first_page;
  std::size_t entries = 0;
  if (!add_recursive_entries(page.size(), &entries, message)) {
    return false;
  }
  for (const TraversalDetailPageRow& row : page) {
    if (!add_recursive_entries(row.nodes.size(), &entries, message)) {
      return false;
    }
  }
  return measure_collection_page(canonical_collection_rows(page), entries,
                                 page.size(), 0, measurement, message);
}

/**
 * @brief Measures recent-planning rows and every nested sample collection.
 * @param page Decoded public planning snapshots.
 * @param first_page Whether this is the initial page; no shared header exists.
 * @param measurement Receives exact recursive and compact-byte footprint.
 * @param message Receives a local protocol diagnostic on rejection.
 * @return True when rows, samples, task dependencies, and bytes are bounded.
 * @throws std::bad_alloc if compact row encoding or diagnostics allocate.
 */
bool measure_planning_collection_page(
    const std::vector<ComputePlanningInspectionSnapshot>& page, bool first_page,
    internal::CollectionPageMeasurement* measurement, std::string* message) {
  (void)first_page;
  std::size_t entries = 0;
  if (!add_recursive_entries(page.size(), &entries, message)) {
    return false;
  }
  for (const ComputePlanningInspectionSnapshot& snapshot : page) {
    if (!add_recursive_entries(snapshot.planned_node_sample.size(), &entries,
                               message) ||
        !add_recursive_entries(snapshot.task_sample.size(), &entries,
                               message)) {
      return false;
    }
    for (const ComputePlanningTaskSnapshot& task : snapshot.task_sample) {
      if (!add_recursive_entries(task.dependency_task_ids.size(), &entries,
                                 message)) {
        return false;
      }
    }
  }
  internal::Json canonical = internal::Json::array();
  for (const ComputePlanningInspectionSnapshot& snapshot : page) {
    internal::Json encoded;
    if (!canonical_planning_snapshot(snapshot, &encoded, message)) {
      return false;
    }
    canonical.push_back(std::move(encoded));
  }
  return measure_collection_page(canonical, entries, page.size(), 0,
                                 measurement, message);
}

/**
 * @brief Validates one exact GraphErrc numeric/name pair from a plugin row.
 * @param code Candidate signed graph error code.
 * @param name Candidate stable lowercase graph error name.
 * @param output Receives the exact public enum on success.
 * @return True only for one of the nine current explicit mappings.
 * @throws Nothing.
 */
bool decode_graph_error_pair(std::int32_t code, std::string_view name,
                             GraphErrc* output) noexcept {
  if (output == nullptr) {
    return false;
  }
  GraphErrc decoded = GraphErrc::Unknown;
  switch (code) {
    case static_cast<std::int32_t>(GraphErrc::Unknown):
      decoded = GraphErrc::Unknown;
      break;
    case static_cast<std::int32_t>(GraphErrc::NotFound):
      decoded = GraphErrc::NotFound;
      break;
    case static_cast<std::int32_t>(GraphErrc::Cycle):
      decoded = GraphErrc::Cycle;
      break;
    case static_cast<std::int32_t>(GraphErrc::Io):
      decoded = GraphErrc::Io;
      break;
    case static_cast<std::int32_t>(GraphErrc::InvalidYaml):
      decoded = GraphErrc::InvalidYaml;
      break;
    case static_cast<std::int32_t>(GraphErrc::MissingDependency):
      decoded = GraphErrc::MissingDependency;
      break;
    case static_cast<std::int32_t>(GraphErrc::NoOperation):
      decoded = GraphErrc::NoOperation;
      break;
    case static_cast<std::int32_t>(GraphErrc::InvalidParameter):
      decoded = GraphErrc::InvalidParameter;
      break;
    case static_cast<std::int32_t>(GraphErrc::ComputeError):
      decoded = GraphErrc::ComputeError;
      break;
    default:
      return false;
  }
  if (name != graph_error_stable_name(decoded)) {
    return false;
  }
  *output = decoded;
  return true;
}

/**
 * @brief Decodes one complete operation-plugin load report.
 * @param value Candidate result object.
 * @param report Receives the complete public report transactionally.
 * @param message Receives a diagnostic on failure.
 * @return True when counts, GraphErrc pairs, diagnostics, and keys validate.
 * @throws std::bad_alloc if copied strings, vectors, or diagnostics allocate.
 */
bool decode_plugin_load_report(const internal::Json& value,
                               HostPluginLoadReport* report,
                               std::string* message) {
  if (report == nullptr || message == nullptr || !value.is_object() ||
      !value.contains("attempted") || !value.contains("loaded") ||
      !internal::valid_bounded_array(value.value("errors", internal::Json()),
                                     internal::kGeneralPageMaxEntries) ||
      !internal::valid_bounded_array(
          value.value("new_op_keys", internal::Json()),
          internal::kGeneralPageMaxEntries)) {
    if (message != nullptr) {
      *message = "plugin load report has an invalid shape";
    }
    return false;
  }
  HostPluginLoadReport decoded;
  if (!internal::decode_integer(value["attempted"], &decoded.attempted) ||
      !internal::decode_integer(value["loaded"], &decoded.loaded) ||
      decoded.attempted < 0 || decoded.loaded < 0 ||
      decoded.loaded > decoded.attempted) {
    *message = "plugin load report has invalid counts";
    return false;
  }
  decoded.errors.reserve(value["errors"].size());
  for (const internal::Json& item : value["errors"]) {
    if (!item.is_object() || !item.contains("path") || !item.contains("code") ||
        !item.contains("name") || !item.contains("message")) {
      *message = "plugin load error has an invalid shape";
      return false;
    }
    HostPluginLoadError error;
    std::int32_t numeric_code = 0;
    std::string stable_name;
    if (!internal::decode_bounded_string(
            item["path"], internal::kPathTextMaxBytes, &error.path) ||
        !internal::decode_integer(item["code"], &numeric_code) ||
        !internal::decode_bounded_string(
            item["name"], internal::kShortTextMaxBytes, &stable_name) ||
        !internal::decode_bounded_string(item["message"],
                                         internal::kDiagnosticTextMaxBytes,
                                         &error.message) ||
        !decode_graph_error_pair(numeric_code, stable_name, &error.code)) {
      *message = "plugin load error contains an invalid typed value";
      return false;
    }
    decoded.errors.push_back(std::move(error));
  }
  if (static_cast<std::size_t>(decoded.loaded) + decoded.errors.size() !=
      static_cast<std::size_t>(decoded.attempted)) {
    *message = "plugin load report counts are inconsistent";
    return false;
  }
  if (!internal::decode_bounded_string_array(
          value["new_op_keys"], internal::kGeneralPageMaxEntries,
          internal::kShortTextMaxBytes, &decoded.new_op_keys) ||
      std::any_of(decoded.new_op_keys.begin(), decoded.new_op_keys.end(),
                  [](const std::string& key) { return key.empty(); })) {
    *message = "plugin load report contains an invalid operation key";
    return false;
  }
  *report = std::move(decoded);
  return true;
}

/**
 * @brief Decodes one bounded stable page of operation source rows.
 * @param value Candidate JSON array.
 * @param rows Receives complete owned rows transactionally.
 * @param message Receives a diagnostic on failure.
 * @return True when every nonempty key and bounded source validates.
 * @throws std::bad_alloc if copied rows or diagnostics allocate.
 */
bool decode_plugin_source_rows(const internal::Json& value,
                               std::vector<PluginSourcePageRow>* rows,
                               std::string* message) {
  if (rows == nullptr || message == nullptr ||
      !internal::valid_bounded_array(value, internal::kGeneralPageMaxEntries)) {
    if (message != nullptr) {
      *message = "plugin source page is not a bounded array";
    }
    return false;
  }
  std::vector<PluginSourcePageRow> decoded;
  decoded.reserve(value.size());
  for (const internal::Json& item : value) {
    if (!item.is_object() || !item.contains("key") ||
        !item.contains("source")) {
      *message = "plugin source row has an invalid shape";
      return false;
    }
    PluginSourcePageRow row;
    if (!internal::decode_bounded_string(
            item["key"], internal::kShortTextMaxBytes, &row.key) ||
        row.key.empty() ||
        !internal::decode_bounded_string(
            item["source"], internal::kLargeTextMaxBytes, &row.source)) {
      *message = "plugin source row contains an invalid typed value";
      return false;
    }
    decoded.push_back(std::move(row));
  }
  *rows = std::move(decoded);
  return true;
}

/**
 * @brief Decodes one bounded page of traversal-order rows.
 * @param value Candidate rows array.
 * @param rows Receives complete private rows transactionally.
 * @param message Receives a diagnostic on failure.
 * @return True when every ending id and nested node-id list validates.
 * @throws std::bad_alloc if copied rows or diagnostics allocate.
 */
bool decode_traversal_order_rows(const internal::Json& value,
                                 std::vector<TraversalOrderPageRow>* rows,
                                 std::string* message) {
  if (rows == nullptr || message == nullptr ||
      !internal::valid_bounded_array(value, internal::kGeneralPageMaxEntries)) {
    if (message != nullptr) {
      *message = "traversal-order page is not a bounded array";
    }
    return false;
  }
  std::vector<TraversalOrderPageRow> decoded;
  decoded.reserve(value.size());
  std::set<int> page_keys;
  for (const internal::Json& item : value) {
    if (!item.is_object() || !item.contains("ending_node_id") ||
        !item.contains("node_ids")) {
      *message = "traversal-order row has an invalid shape";
      return false;
    }
    TraversalOrderPageRow row;
    if (!internal::decode_integer(item["ending_node_id"],
                                  &row.ending_node_id) ||
        row.ending_node_id < 0 ||
        !decode_node_id_array(item["node_ids"], &row.node_ids, message) ||
        !page_keys.insert(row.ending_node_id).second) {
      if (message->empty()) {
        *message = "traversal-order row is invalid or duplicated";
      }
      return false;
    }
    decoded.push_back(std::move(row));
  }
  *rows = std::move(decoded);
  return true;
}

/**
 * @brief Decodes one bounded page of traversal-detail rows.
 * @param value Candidate rows array.
 * @param rows Receives complete private rows transactionally.
 * @param message Receives a diagnostic on failure.
 * @return True when every branch, nested node, label, and cache flag validates.
 * @throws std::bad_alloc if copied rows, names, or diagnostics allocate.
 */
bool decode_traversal_detail_rows(const internal::Json& value,
                                  std::vector<TraversalDetailPageRow>* rows,
                                  std::string* message) {
  if (rows == nullptr || message == nullptr ||
      !internal::valid_bounded_array(value, internal::kGeneralPageMaxEntries)) {
    if (message != nullptr) {
      *message = "traversal-detail page is not a bounded array";
    }
    return false;
  }
  std::vector<TraversalDetailPageRow> decoded;
  decoded.reserve(value.size());
  std::set<int> page_keys;
  for (const internal::Json& item : value) {
    if (!item.is_object() || !item.contains("ending_node_id") ||
        !internal::valid_bounded_array(item.value("nodes", internal::Json()),
                                       internal::kGeneralPageMaxEntries)) {
      *message = "traversal-detail row has an invalid shape";
      return false;
    }
    TraversalDetailPageRow row;
    if (!internal::decode_integer(item["ending_node_id"],
                                  &row.ending_node_id) ||
        row.ending_node_id < 0 ||
        !page_keys.insert(row.ending_node_id).second) {
      *message = "traversal-detail ending node is invalid or duplicated";
      return false;
    }
    row.nodes.reserve(item["nodes"].size());
    for (const internal::Json& node_value : item["nodes"]) {
      if (!node_value.is_object() || !node_value.contains("node_id") ||
          !node_value.contains("name") ||
          !node_value.value("has_memory_cache", internal::Json())
               .is_boolean() ||
          !node_value.value("has_disk_cache", internal::Json()).is_boolean()) {
        *message = "traversal-detail node has an invalid shape";
        return false;
      }
      HostTraversalNodeSnapshot node;
      if (!internal::decode_integer(node_value["node_id"], &node.node.value) ||
          node.node.value < 0 ||
          !internal::decode_bounded_string(
              node_value["name"], internal::kShortTextMaxBytes, &node.name)) {
        *message = "traversal-detail node contains an invalid typed value";
        return false;
      }
      node.has_memory_cache = node_value["has_memory_cache"].get<bool>();
      node.has_disk_cache = node_value["has_disk_cache"].get<bool>();
      row.nodes.push_back(std::move(node));
    }
    decoded.push_back(std::move(row));
  }
  *rows = std::move(decoded);
  return true;
}

/**
 * @brief Classifies which typed job operation produced one common result.
 *
 * @throws Nothing.
 * @note The classification tightens state/output invariants beyond the common
 *       wire object without adding another public job state vocabulary.
 */
enum class ComputeJobResponseKind {
  /** @brief Accepted submission, which must be queued without output. */
  Submit,

  /** @brief Non-destructive status observation, which never carries output. */
  Status,

  /** @brief Terminal result observation, which may carry one output. */
  Result,
};

/**
 * @brief Returns the exact channel byte width for one known data type.
 * @param data_type Public image scalar type.
 * @return One, two, four, or eight; zero for an invalid future enum value.
 * @throws Nothing.
 */
std::size_t channel_bytes(DataType data_type) noexcept {
  switch (data_type) {
    case DataType::UINT8:
    case DataType::INT8:
      return 1;
    case DataType::UINT16:
    case DataType::INT16:
      return 2;
    case DataType::FLOAT32:
      return 4;
    case DataType::FLOAT64:
      return 8;
  }
  return 0;
}

/**
 * @brief Decodes one protected output delivery from its flat wire object.
 * @param value Candidate flat output object.
 * @param delivery Receives structured public metadata and lease id.
 * @param message Receives a diagnostic on failure.
 * @return True when identities, path, image layout, and filesystem values are
 *         exact and overflow-safe.
 * @throws std::bad_alloc if copied ids, path, or diagnostics allocate.
 * @note This validates metadata only. Opening, identity revalidation, mmap,
 *       and mapping ownership remain task 4.3 responsibilities.
 */
bool decode_output_delivery(const internal::Json& value,
                            OutputArtifactDelivery* delivery,
                            std::string* message) {
  static constexpr std::array<const char*, 12> kFields = {
      "output_id",         "delivery_id", "path",   "width",    "height",
      "channels",          "data_type",   "device", "row_step", "byte_size",
      "filesystem_device", "inode"};
  if (delivery == nullptr || message == nullptr || !value.is_object() ||
      std::any_of(kFields.begin(), kFields.end(), [&value](const char* field) {
        return !value.contains(field);
      })) {
    if (message != nullptr) {
      *message = "compute output has an invalid shape";
    }
    return false;
  }
  OutputArtifactDelivery decoded;
  OutputArtifactMetadata& metadata = decoded.metadata;
  if (!internal::decode_opaque_id(value["output_id"],
                                  &metadata.output_id.value) ||
      !internal::decode_opaque_id(value["delivery_id"],
                                  &decoded.delivery_id.value) ||
      !internal::decode_bounded_string(
          value["path"], internal::kPathTextMaxBytes, &metadata.path) ||
      metadata.path.empty() || metadata.path.front() != '/' ||
      metadata.path.find('\0') != std::string::npos ||
      !internal::decode_integer(value["width"], &metadata.width) ||
      !internal::decode_integer(value["height"], &metadata.height) ||
      !internal::decode_integer(value["channels"], &metadata.channels) ||
      metadata.width <= 0 || metadata.height <= 0 || metadata.channels <= 0 ||
      !internal::decode_enum(value["data_type"], &metadata.data_type) ||
      !internal::decode_enum(value["device"], &metadata.device) ||
      metadata.device != Device::CPU ||
      !internal::decode_integer(value["row_step"], &metadata.row_step) ||
      !internal::decode_integer(value["byte_size"], &metadata.byte_size) ||
      !internal::decode_integer(value["filesystem_device"],
                                &metadata.filesystem_device) ||
      !internal::decode_integer(value["inode"], &metadata.inode)) {
    *message = "compute output contains an invalid typed value";
    return false;
  }
  const std::size_t scalar_bytes = channel_bytes(metadata.data_type);
  const std::size_t width = static_cast<std::size_t>(metadata.width);
  const std::size_t height = static_cast<std::size_t>(metadata.height);
  const std::size_t channels = static_cast<std::size_t>(metadata.channels);
  const std::size_t maximum = std::numeric_limits<std::size_t>::max();
  if (scalar_bytes == 0 || width > maximum / channels ||
      width * channels > maximum / scalar_bytes) {
    *message = "compute output row width overflows";
    return false;
  }
  const std::size_t expected_step = width * channels * scalar_bytes;
  if (height > maximum / expected_step || metadata.row_step != expected_step ||
      metadata.byte_size != expected_step * height) {
    *message = "compute output byte layout is inconsistent";
    return false;
  }
  *delivery = std::move(decoded);
  return true;
}

/**
 * @brief Decodes one exact forward-only compute job result.
 * @param value Candidate common job object.
 * @param kind Submit, status, or terminal-result operation.
 * @param expected_compute Optional job identity that the daemon must echo.
 * @param expected_session Optional session identity that the daemon must echo.
 * @param snapshot Receives the complete public job transactionally.
 * @param message Receives a diagnostic on failure.
 * @return True when state/status/output coupling and every identity validate.
 * @throws std::bad_alloc if copied ids, statuses, output, or diagnostics
 *         allocate.
 * @note Accepted Host failures remain successful RPC envelopes with a Failed
 *       state and exact nested status; this helper never converts them into an
 *       outer `IpcResult` failure.
 */
bool decode_compute_job(const internal::Json& value,
                        ComputeJobResponseKind kind,
                        const std::optional<ComputeRequestId>& expected_compute,
                        const std::optional<IpcSessionId>& expected_session,
                        ComputeJobSnapshot* snapshot, std::string* message) {
  static constexpr std::array<const char*, 6> kFields = {
      "compute_id", "session_id", "state", "cancellable", "status", "output"};
  if (snapshot == nullptr || message == nullptr || !value.is_object() ||
      std::any_of(
          kFields.begin(), kFields.end(),
          [&value](const char* field) { return !value.contains(field); }) ||
      !value["cancellable"].is_boolean() || value["cancellable"].get<bool>()) {
    if (message != nullptr) {
      *message = "compute job has an invalid shape or cancellable flag";
    }
    return false;
  }
  ComputeJobSnapshot decoded;
  std::string state;
  if (!internal::decode_opaque_id(value["compute_id"],
                                  &decoded.compute_id.value) ||
      !internal::decode_opaque_id(value["session_id"],
                                  &decoded.session_id.value) ||
      !internal::decode_bounded_string(value["state"],
                                       internal::kShortTextMaxBytes, &state) ||
      (expected_compute &&
       decoded.compute_id.value != expected_compute->value) ||
      (expected_session &&
       decoded.session_id.value != expected_session->value)) {
    *message = "compute job returned an invalid or mismatched identity";
    return false;
  }
  if (state == "queued") {
    decoded.state = ComputeJobState::Queued;
  } else if (state == "running") {
    decoded.state = ComputeJobState::Running;
  } else if (state == "succeeded") {
    decoded.state = ComputeJobState::Succeeded;
  } else if (state == "failed") {
    decoded.state = ComputeJobState::Failed;
  } else {
    *message = "compute job returned an unknown state";
    return false;
  }
  decoded.cancellable = false;
  const bool terminal = decoded.state == ComputeJobState::Succeeded ||
                        decoded.state == ComputeJobState::Failed;
  if (terminal) {
    OperationStatus terminal_status;
    if (!internal::decode_operation_status(value["status"], &terminal_status,
                                           message) ||
        (decoded.state == ComputeJobState::Succeeded && !terminal_status.ok) ||
        (decoded.state == ComputeJobState::Failed && terminal_status.ok)) {
      if (message->empty()) {
        *message = "compute terminal status is inconsistent with its state";
      }
      return false;
    }
    decoded.status = std::move(terminal_status);
  } else if (!value["status"].is_null()) {
    *message = "nonterminal compute job returned a status";
    return false;
  }

  if (kind == ComputeJobResponseKind::Submit) {
    if (decoded.state != ComputeJobState::Queued ||
        !value["output"].is_null()) {
      *message = "compute.submit did not return a queued output-free job";
      return false;
    }
  } else if (kind == ComputeJobResponseKind::Status) {
    if (!value["output"].is_null()) {
      *message = "compute.status unexpectedly returned output metadata";
      return false;
    }
  } else {
    if (!terminal) {
      *message = "compute.result returned a nonterminal job";
      return false;
    }
    if (!value["output"].is_null()) {
      if (decoded.state != ComputeJobState::Succeeded) {
        *message = "failed compute job returned output metadata";
        return false;
      }
      OutputArtifactDelivery output;
      if (!decode_output_delivery(value["output"], &output, message)) {
        return false;
      }
      decoded.output = std::move(output);
    }
  }
  *snapshot = std::move(decoded);
  return true;
}

/**
 * @brief Decodes one exact successful compute-release acknowledgement.
 * @param value Candidate result object.
 * @param expected_compute Exact job id supplied by the Client.
 * @param released Receives the acknowledgement transactionally.
 * @param message Receives a diagnostic on failure.
 * @return True only for an exact id echo and boolean true release flag.
 * @throws std::bad_alloc if copied id or diagnostic storage allocates.
 */
bool decode_compute_release(const internal::Json& value,
                            const ComputeRequestId& expected_compute,
                            ComputeReleaseResult* released,
                            std::string* message) {
  if (released == nullptr || message == nullptr || !value.is_object() ||
      !value.contains("compute_id") ||
      !value.value("released", internal::Json()).is_boolean()) {
    if (message != nullptr) {
      *message = "compute.release result has an invalid shape";
    }
    return false;
  }
  ComputeReleaseResult decoded;
  if (!internal::decode_opaque_id(value["compute_id"],
                                  &decoded.compute_id.value) ||
      decoded.compute_id.value != expected_compute.value ||
      !value["released"].get<bool>()) {
    *message = "compute.release result has an invalid acknowledgement";
    return false;
  }
  decoded.released = true;
  *released = std::move(decoded);
  return true;
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
  OperationStatus connect(const std::string& socket_path) {
    socket_.reset();
    std::string message;
    internal::UniqueFd connected =
        internal::connect_unix_socket(socket_path, &message);
    if (!connected) {
      return internal::failure_status(OperationErrorDomain::Transport, 1,
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
   * @param method One of the exact 55 version 1 method names.
   * @param params Typed method parameters already encoded as an object.
   * @return Owned result object or categorized local/remote failure.
   * @throws std::bad_alloc if request/response storage cannot be allocated.
   * @note The method must be nonempty valid UTF-8 within 128 bytes and params
   *       must be an object. The call performs no retry. Frame, JSON, envelope,
   * correlation, and error-object protocol violations close the connection
   * because message synchronization is no longer trustworthy. After this
   * function returns a correlated result object, a typed payload-shape failure
   *       becomes a local Protocol error and leaves the connection open.
   */
  RawCallResult call(const std::string& method, const internal::Json& params) {
    if (!socket_) {
      return {internal::failure_status(OperationErrorDomain::Transport, 2,
                                       "not_connected",
                                       "IPC client is not connected"),
              {}};
    }
    if (method.empty() || method.size() > internal::kRequestTextMaxBytes ||
        !internal::valid_utf8(method) || !params.is_object()) {
      return {internal::failure_status(
                  OperationErrorDomain::Protocol, internal::kInvalidParamsCode,
                  "invalid_params",
                  "client method or params violate version 1 bounds"),
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
      return {internal::failure_status(OperationErrorDomain::Protocol,
                                       internal::kInvalidParamsCode,
                                       "invalid_params", error.what()),
              {}};
    }
    if (payload.empty() || payload.size() > kMaximumFramePayloadBytes) {
      return {internal::failure_status(
                  OperationErrorDomain::Protocol, internal::kInvalidParamsCode,
                  "invalid_params",
                  "serialized request exceeds the version 1 frame limit"),
              {}};
    }
    const internal::FrameWriteResult written =
        internal::write_frame(socket_.get(), payload);
    if (!written.ok) {
      socket_.reset();
      return {internal::failure_status(OperationErrorDomain::Transport, 3,
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
                  OperationErrorDomain::Transport, 4,
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
                  OperationErrorDomain::Protocol,
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
      OperationStatus status;
      std::string message;
      if (!internal::decode_error(response["error"], &status, &message)) {
        socket_.reset();
        return {invalid_response(std::move(message)), {}};
      }
      return {std::move(status), {}};
    }
    return {internal::ok_status(), response["result"]};
  }

  /**
   * @brief Performs one typed status-only call whose result has no known field.
   * @param method Exact version 1 method name.
   * @param params Complete typed params object.
   * @return Success or the first transport/protocol/remote failure.
   * @throws std::bad_alloc if request, response, or diagnostics allocate.
   * @note Unknown successful result members are forward-compatible. The call
   *       delegates to `call()` exactly once and never retries a mutation.
   */
  VoidResult void_call(const std::string& method,
                       const internal::Json& params) {
    RawCallResult result = call(method, params);
    return {std::move(result.status)};
  }

  /**
   * @brief Aggregates one stable session-scoped node-id collection.
   * @param method Exact inspection wire method.
   * @param base_params Session and optional node parameters repeated per page.
   * @param field Result array field (`node_ids` or `ending_node_ids`).
   * @param expected_session Exact opaque session id expected on every page.
   * @return Complete ordered node-id value or the first categorized failure.
   * @throws std::bad_alloc if requests, pages, diagnostics, or aggregate
   *         storage allocate.
   * @note The helper validates and advances by actual rows and never retries a
   *       page. Host order and duplicate node ids remain unchanged.
   */
  IpcResult<std::vector<NodeId>> node_collection(
      const std::string& method, const internal::Json& base_params,
      const char* field, const IpcSessionId& expected_session) {
    return aggregate_collection<std::vector<NodeId>, std::vector<NodeId>>(
        [this, &method, &base_params](const std::optional<std::string>& cursor,
                                      std::size_t offset, std::size_t limit) {
          return call(method, collection_page_params(base_params, cursor,
                                                     offset, limit));
        },
        [&expected_session, field](const internal::Json& result,
                                   std::vector<NodeId>* page, std::size_t* rows,
                                   std::string* message) {
          if (!decode_session_echo(result, expected_session, message) ||
              !result.contains(field) ||
              !decode_node_id_array(result[field], page, message)) {
            if (message->empty()) {
              *message = "node-id collection returned an invalid page";
            }
            return false;
          }
          *rows = page->size();
          return true;
        },
        [](const internal::Json&, const std::vector<NodeId>& page,
           bool first_page, internal::CollectionPageMeasurement* measurement,
           std::string* message) {
          return measure_flat_collection_page(page, first_page, measurement,
                                              message);
        },
        [](std::vector<NodeId>* aggregate, std::vector<NodeId> page,
           std::string*) {
          aggregate->insert(aggregate->end(),
                            std::make_move_iterator(page.begin()),
                            std::make_move_iterator(page.end()));
          return true;
        });
  }

  /**
   * @brief Aggregates one globally non-decreasing stable string collection.
   * @param method Exact plugin or scheduler wire method.
   * @param field Result array field.
   * @param maximum_bytes Field-specific per-string UTF-8 byte ceiling.
   * @return Complete non-decreasing owned string list or a failure.
   * @throws std::bad_alloc if requests, pages, strings, or diagnostics
   * allocate.
   * @note Empty rows and cross-page ordering regressions are local Protocol
   *       failures. Equal adjacent rows are preserved in their original page
   *       order because sorted Host string collections may contain duplicate
   *       public values. No continuation is retried.
   */
  IpcResult<std::vector<std::string>> sorted_string_collection(
      const std::string& method, const char* field, std::size_t maximum_bytes) {
    return aggregate_collection<std::vector<std::string>,
                                std::vector<std::string>>(
        [this, &method](const std::optional<std::string>& cursor,
                        std::size_t offset, std::size_t limit) {
          return call(method, collection_page_params(internal::Json::object(),
                                                     cursor, offset, limit));
        },
        [field, maximum_bytes](const internal::Json& result,
                               std::vector<std::string>* page,
                               std::size_t* rows, std::string* message) {
          if (!result.contains(field) ||
              !internal::decode_bounded_string_array(
                  result[field], internal::kGeneralPageMaxEntries,
                  maximum_bytes, page) ||
              std::any_of(page->begin(), page->end(),
                          [](const std::string& row) { return row.empty(); })) {
            *message = "sorted string collection returned an invalid page";
            return false;
          }
          *rows = page->size();
          return true;
        },
        [](const internal::Json&, const std::vector<std::string>& page,
           bool first_page, internal::CollectionPageMeasurement* measurement,
           std::string* message) {
          return measure_flat_collection_page(page, first_page, measurement,
                                              message);
        },
        [](std::vector<std::string>* aggregate, std::vector<std::string> page,
           std::string* message) {
          for (std::string& row : page) {
            if (!aggregate->empty() && row < aggregate->back()) {
              *message = "string collection is not globally non-decreasing";
              return false;
            }
            aggregate->push_back(std::move(row));
          }
          return true;
        });
  }

  /**
   * @brief Aggregates one globally key-sorted operation source collection.
   * @param method Exact operation-source wire method.
   * @return Complete owned source map or the first categorized failure.
   * @throws std::bad_alloc if requests, pages, map rows, or diagnostics
   *         allocate.
   * @note Rows must remain strictly key sorted across the frozen snapshot;
   *       duplicate keys are rejected rather than overwritten.
   */
  IpcResult<std::map<std::string, std::string>> plugin_source_collection(
      const std::string& method) {
    using SourceMap = std::map<std::string, std::string>;
    using SourcePage = std::vector<PluginSourcePageRow>;
    return aggregate_collection<SourceMap, SourcePage>(
        [this, &method](const std::optional<std::string>& cursor,
                        std::size_t offset, std::size_t limit) {
          return call(method, collection_page_params(internal::Json::object(),
                                                     cursor, offset, limit));
        },
        [](const internal::Json& result, SourcePage* page, std::size_t* rows,
           std::string* message) {
          if (!result.contains("sources") ||
              !decode_plugin_source_rows(result["sources"], page, message)) {
            if (message->empty()) {
              *message = "plugin source collection returned an invalid page";
            }
            return false;
          }
          *rows = page->size();
          return true;
        },
        [](const internal::Json&, const SourcePage& page, bool first_page,
           internal::CollectionPageMeasurement* measurement,
           std::string* message) {
          return measure_flat_collection_page(page, first_page, measurement,
                                              message);
        },
        [](SourceMap* aggregate, SourcePage page, std::string* message) {
          for (PluginSourcePageRow& row : page) {
            if ((!aggregate->empty() &&
                 !(aggregate->rbegin()->first < row.key)) ||
                !aggregate->emplace(std::move(row.key), std::move(row.source))
                     .second) {
              *message =
                  "plugin source keys are duplicated or unsorted across pages";
              return false;
            }
          }
          return true;
        });
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
OperationStatus Client::connect(const std::string& socket_path) {
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
    return failed_result<DaemonPing>(internal::failure_status(
        OperationErrorDomain::Transport, 2, "not_connected",
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
  std::string server_instance_id;
  if (!internal::decode_opaque_id(call.result["server_instance_id"],
                                  &server_instance_id)) {
    return failed_result<DaemonPing>(
        invalid_response("daemon.ping returned an invalid instance id"));
  }
  return {internal::ok_status(), {true, server_instance_id}};
}

/** @copydoc Client::version */
IpcResult<DaemonVersion> Client::version() {
  if (!impl_) {
    return failed_result<DaemonVersion>(internal::failure_status(
        OperationErrorDomain::Transport, 2, "not_connected",
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
  if (!internal::decode_bounded_string(call.result["service_name"],
                                       internal::kShortTextMaxBytes,
                                       &result.service_name) ||
      !internal::decode_bounded_string(call.result["service_version"],
                                       internal::kShortTextMaxBytes,
                                       &result.service_version) ||
      !internal::decode_opaque_id(call.result["server_instance_id"],
                                  &result.server_instance_id) ||
      !internal::decode_bounded_string(call.result["transport"],
                                       internal::kShortTextMaxBytes,
                                       &result.transport) ||
      !internal::decode_bounded_string_array(
          call.result["methods"], internal::kGeneralPageMaxEntries,
          internal::kRequestTextMaxBytes, &result.methods)) {
    return failed_result<DaemonVersion>(invalid_response(
        "daemon.version contains an invalid or over-limit string/array"));
  }
  if (result.protocol_version != kProtocolVersion ||
      result.service_name != "photospiderd" || result.service_version.empty() ||
      result.transport != "unix" ||
      result.methods.size() != internal::kVersionOneMethodNames.size() ||
      !std::equal(result.methods.begin(), result.methods.end(),
                  internal::kVersionOneMethodNames.begin(),
                  [](const std::string& actual, std::string_view expected) {
                    return actual == expected;
                  })) {
    return failed_result<DaemonVersion>(invalid_response(
        "daemon.version metadata or exact method inventory is invalid"));
  }
  return {internal::ok_status(), std::move(result)};
}

/** @copydoc Client::load_graph */
IpcResult<GraphSessionSummary> Client::load_graph(
    const GraphLoadRequest& request) {
  if (!impl_) {
    return failed_result<GraphSessionSummary>(internal::failure_status(
        OperationErrorDomain::Transport, 2, "not_connected",
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
  if (!internal::decode_opaque_id(call.result["session_id"],
                                  &summary.session_id.value) ||
      !internal::decode_bounded_string(call.result["session_name"],
                                       internal::kShortTextMaxBytes,
                                       &summary.session_name) ||
      !internal::valid_session_name(summary.session_name) ||
      summary.session_name != request.session.value) {
    return failed_result<GraphSessionSummary>(invalid_response(
        "graph.load result has an invalid session id or session name"));
  }
  return {internal::ok_status(), std::move(summary)};
}

/** @copydoc Client::close_graph */
VoidResult Client::close_graph(const IpcSessionId& session_id) {
  if (!impl_) {
    return {internal::failure_status(OperationErrorDomain::Transport, 2,
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
        not_connected_status());
  }
  return aggregate_collection<std::vector<GraphSessionSummary>,
                              std::vector<GraphSessionSummary>>(
      [this](const std::optional<std::string>& cursor, std::size_t offset,
             std::size_t limit) {
        return impl_->call("graph.list",
                           collection_page_params(internal::Json::object(),
                                                  cursor, offset, limit));
      },
      [](const internal::Json& result, std::vector<GraphSessionSummary>* page,
         std::size_t* rows, std::string* message) {
        if (!result.contains("sessions") ||
            !internal::decode_session_summaries(result["sessions"], page,
                                                message)) {
          if (message->empty()) {
            *message = "graph.list result requires valid session rows";
          }
          return false;
        }
        *rows = page->size();
        return true;
      },
      [](const internal::Json&, const std::vector<GraphSessionSummary>& page,
         bool first_page, internal::CollectionPageMeasurement* measurement,
         std::string* message) {
        return measure_flat_collection_page(page, first_page, measurement,
                                            message);
      },
      [](std::vector<GraphSessionSummary>* aggregate,
         std::vector<GraphSessionSummary> page, std::string* message) {
        for (GraphSessionSummary& row : page) {
          if (!aggregate->empty() &&
              !(std::tie(aggregate->back().session_name,
                         aggregate->back().session_id.value) <
                std::tie(row.session_name, row.session_id.value))) {
            *message =
                "graph.list sessions are not strictly sorted across pages";
            return false;
          }
          aggregate->push_back(std::move(row));
        }
        return true;
      });
}

/** @copydoc Client::inspect_graph */
IpcResult<GraphInspectionView> Client::inspect_graph(
    const IpcSessionId& session_id) {
  if (!impl_) {
    return failed_result<GraphInspectionView>(not_connected_status());
  }
  const internal::Json base{{"session_id", session_id.value}};
  return aggregate_collection<GraphInspectionView, GraphInspectionView>(
      [this, base](const std::optional<std::string>& cursor, std::size_t offset,
                   std::size_t limit) {
        return impl_->call("inspect.graph",
                           collection_page_params(base, cursor, offset, limit));
      },
      [&session_id](const internal::Json& result, GraphInspectionView* page,
                    std::size_t* rows, std::string* message) {
        if (!internal::decode_graph(result, page, message) ||
            page->session.value != session_id.value) {
          if (message->empty()) {
            *message = "inspect.graph returned a mismatched session id";
          }
          return false;
        }
        *rows = page->nodes.size();
        return true;
      },
      [](const internal::Json&, const GraphInspectionView& page,
         bool first_page, internal::CollectionPageMeasurement* measurement,
         std::string* message) {
        return measure_graph_collection_page(page, first_page, measurement,
                                             message);
      },
      [&session_id](GraphInspectionView* aggregate, GraphInspectionView page,
                    std::string*) {
        aggregate->session.value = session_id.value;
        aggregate->nodes.insert(aggregate->nodes.end(),
                                std::make_move_iterator(page.nodes.begin()),
                                std::make_move_iterator(page.nodes.end()));
        return true;
      });
}

/** @copydoc Client::inspect_node */
IpcResult<NodeInspectionView> Client::inspect_node(
    const IpcSessionId& session_id, NodeId node) {
  if (!impl_) {
    return failed_result<NodeInspectionView>(internal::failure_status(
        OperationErrorDomain::Transport, 2, "not_connected",
        "IPC client is not connected"));
  }
  RawCallResult call = impl_->call(
      "inspect.node", internal::Json{{"session_id", session_id.value},
                                     {"node_id", node.value}});
  if (!call.status.ok) {
    return failed_result<NodeInspectionView>(std::move(call.status));
  }
  std::string returned_session_id;
  if (!call.result.contains("session_id") ||
      !internal::decode_opaque_id(call.result["session_id"],
                                  &returned_session_id) ||
      returned_session_id != session_id.value ||
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
    return failed_result<HostDependencyTreeSnapshot>(not_connected_status());
  }
  internal::Json base{{"session_id", session_id.value},
                      {"include_metadata", include_metadata}};
  if (node) {
    base["node_id"] = node->value;
  }
  return aggregate_collection<HostDependencyTreeSnapshot,
                              HostDependencyTreeSnapshot>(
      [this, base](const std::optional<std::string>& cursor, std::size_t offset,
                   std::size_t limit) {
        return impl_->call("inspect.dependency_tree",
                           collection_page_params(base, cursor, offset, limit));
      },
      [&session_id](const internal::Json& result,
                    HostDependencyTreeSnapshot* page, std::size_t* rows,
                    std::string* message) {
        if (!decode_session_echo(result, session_id, message) ||
            !internal::decode_dependency_tree(result, page, message)) {
          if (message->empty()) {
            *message = "inspect.dependency_tree returned an invalid page";
          }
          return false;
        }
        *rows = page->entries.size();
        return true;
      },
      [&session_id](const internal::Json&,
                    const HostDependencyTreeSnapshot& page, bool first_page,
                    internal::CollectionPageMeasurement* measurement,
                    std::string* message) {
        return measure_dependency_collection_page(session_id, page, first_page,
                                                  measurement, message);
      },
      [header_initialized = false](HostDependencyTreeSnapshot* aggregate,
                                   HostDependencyTreeSnapshot page,
                                   std::string* message) mutable {
        if (!header_initialized) {
          aggregate->scope = page.scope;
          aggregate->start_node = page.start_node;
          aggregate->graph_empty = page.graph_empty;
          aggregate->start_node_found = page.start_node_found;
          aggregate->no_ending_nodes = page.no_ending_nodes;
          aggregate->root_nodes = std::move(page.root_nodes);
          header_initialized = true;
        } else {
          const bool same_start =
              aggregate->start_node.has_value() ==
                  page.start_node.has_value() &&
              (!aggregate->start_node ||
               aggregate->start_node->value == page.start_node->value);
          const bool same_roots =
              aggregate->root_nodes.size() == page.root_nodes.size() &&
              std::equal(aggregate->root_nodes.begin(),
                         aggregate->root_nodes.end(), page.root_nodes.begin(),
                         [](NodeId left, NodeId right) {
                           return left.value == right.value;
                         });
          if (aggregate->scope != page.scope || !same_start ||
              aggregate->graph_empty != page.graph_empty ||
              aggregate->start_node_found != page.start_node_found ||
              aggregate->no_ending_nodes != page.no_ending_nodes ||
              !same_roots) {
            *message = "dependency-tree header changed across stable pages";
            return false;
          }
        }
        aggregate->entries.insert(aggregate->entries.end(),
                                  std::make_move_iterator(page.entries.begin()),
                                  std::make_move_iterator(page.entries.end()));
        return true;
      });
}

/** @copydoc Client::reload_graph */
VoidResult Client::reload_graph(const IpcSessionId& session_id,
                                const std::string& yaml_path) {
  if (!impl_) {
    return {not_connected_status()};
  }
  return impl_->void_call("graph.reload",
                          internal::Json{{"session_id", session_id.value},
                                         {"yaml_path", yaml_path}});
}

/** @copydoc Client::save_graph */
VoidResult Client::save_graph(const IpcSessionId& session_id,
                              const std::string& yaml_path) {
  if (!impl_) {
    return {not_connected_status()};
  }
  return impl_->void_call("graph.save",
                          internal::Json{{"session_id", session_id.value},
                                         {"yaml_path", yaml_path}});
}

/** @copydoc Client::clear_graph */
VoidResult Client::clear_graph(const IpcSessionId& session_id) {
  if (!impl_) {
    return {not_connected_status()};
  }
  return impl_->void_call("graph.clear",
                          internal::Json{{"session_id", session_id.value}});
}

/** @copydoc Client::get_node_yaml */
IpcResult<std::string> Client::get_node_yaml(const IpcSessionId& session_id,
                                             NodeId node) {
  if (!impl_) {
    return failed_result<std::string>(not_connected_status());
  }
  RawCallResult call = impl_->call(
      "graph.node_yaml.get", internal::Json{{"session_id", session_id.value},
                                            {"node_id", node.value}});
  if (!call.status.ok) {
    return failed_result<std::string>(std::move(call.status));
  }
  std::string message;
  std::string yaml_text;
  int returned_node = -1;
  if (!decode_session_echo(call.result, session_id, &message) ||
      !call.result.contains("node_id") ||
      !internal::decode_integer(call.result["node_id"], &returned_node) ||
      returned_node != node.value || !call.result.contains("yaml_text") ||
      !internal::decode_bounded_string(
          call.result["yaml_text"], internal::kLargeTextMaxBytes, &yaml_text)) {
    if (message.empty()) {
      message = "graph.node_yaml.get returned an invalid node or YAML value";
    }
    return failed_result<std::string>(invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(yaml_text)};
}

/** @copydoc Client::list_node_ids */
IpcResult<std::vector<NodeId>> Client::list_node_ids(
    const IpcSessionId& session_id) {
  if (!impl_) {
    return failed_result<std::vector<NodeId>>(not_connected_status());
  }
  return impl_->node_collection(
      "inspect.node_ids", internal::Json{{"session_id", session_id.value}},
      "node_ids", session_id);
}

/** @copydoc Client::ending_nodes */
IpcResult<std::vector<NodeId>> Client::ending_nodes(
    const IpcSessionId& session_id) {
  if (!impl_) {
    return failed_result<std::vector<NodeId>>(not_connected_status());
  }
  return impl_->node_collection(
      "inspect.ending_nodes", internal::Json{{"session_id", session_id.value}},
      "ending_node_ids", session_id);
}

/** @copydoc Client::trees_containing_node */
IpcResult<std::vector<NodeId>> Client::trees_containing_node(
    const IpcSessionId& session_id, NodeId node) {
  if (!impl_) {
    return failed_result<std::vector<NodeId>>(not_connected_status());
  }
  return impl_->node_collection(
      "inspect.trees_containing_node",
      internal::Json{{"session_id", session_id.value}, {"node_id", node.value}},
      "ending_node_ids", session_id);
}

/** @copydoc Client::set_node_yaml */
VoidResult Client::set_node_yaml(const IpcSessionId& session_id, NodeId node,
                                 const std::string& yaml_text) {
  if (!impl_) {
    return {not_connected_status()};
  }
  return impl_->void_call("graph.node_yaml.set",
                          internal::Json{{"session_id", session_id.value},
                                         {"node_id", node.value},
                                         {"yaml_text", yaml_text}});
}

/** @copydoc Client::project_roi */
IpcResult<PixelRect> Client::project_roi(const IpcSessionId& session_id,
                                         NodeId start_node,
                                         const PixelRect& start_roi,
                                         NodeId target_node) {
  if (!impl_) {
    return failed_result<PixelRect>(not_connected_status());
  }
  RawCallResult call = impl_->call(
      "inspect.roi_forward",
      internal::Json{{"session_id", session_id.value},
                     {"start_node_id", start_node.value},
                     {"start_roi", internal::encode_pixel_rect(start_roi)},
                     {"target_node_id", target_node.value}});
  if (!call.status.ok) {
    return failed_result<PixelRect>(std::move(call.status));
  }
  PixelRect result;
  std::string message;
  if (!decode_session_echo(call.result, session_id, &message) ||
      !call.result.contains("roi") ||
      !internal::decode_pixel_rect(call.result["roi"], &result)) {
    if (message.empty()) {
      message = "inspect.roi_forward returned an invalid ROI";
    }
    return failed_result<PixelRect>(invalid_response(std::move(message)));
  }
  return {internal::ok_status(), result};
}

/** @copydoc Client::project_roi_backward */
IpcResult<PixelRect> Client::project_roi_backward(
    const IpcSessionId& session_id, NodeId target_node,
    const PixelRect& target_roi, NodeId source_node) {
  if (!impl_) {
    return failed_result<PixelRect>(not_connected_status());
  }
  RawCallResult call = impl_->call(
      "inspect.roi_backward",
      internal::Json{{"session_id", session_id.value},
                     {"target_node_id", target_node.value},
                     {"target_roi", internal::encode_pixel_rect(target_roi)},
                     {"source_node_id", source_node.value}});
  if (!call.status.ok) {
    return failed_result<PixelRect>(std::move(call.status));
  }
  PixelRect result;
  std::string message;
  if (!decode_session_echo(call.result, session_id, &message) ||
      !call.result.contains("roi") ||
      !internal::decode_pixel_rect(call.result["roi"], &result)) {
    if (message.empty()) {
      message = "inspect.roi_backward returned an invalid ROI";
    }
    return failed_result<PixelRect>(invalid_response(std::move(message)));
  }
  return {internal::ok_status(), result};
}

/** @copydoc Client::dirty_region_snapshot */
IpcResult<DirtyRegionInspectionSnapshot> Client::dirty_region_snapshot(
    const IpcSessionId& session_id) {
  if (!impl_) {
    return failed_result<DirtyRegionInspectionSnapshot>(not_connected_status());
  }
  RawCallResult call = impl_->call(
      "inspect.dirty_region", internal::Json{{"session_id", session_id.value}});
  if (!call.status.ok) {
    return failed_result<DirtyRegionInspectionSnapshot>(std::move(call.status));
  }
  DirtyRegionInspectionSnapshot result;
  std::string message;
  if (!decode_dirty_region(call.result, session_id, &result, &message)) {
    return failed_result<DirtyRegionInspectionSnapshot>(
        invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(result)};
}

/** @copydoc Client::compute_planning_snapshot */
IpcResult<std::optional<ComputePlanningInspectionSnapshot>>
Client::compute_planning_snapshot(const IpcSessionId& session_id) {
  using PlanningResult =
      IpcResult<std::optional<ComputePlanningInspectionSnapshot>>;
  if (!impl_) {
    return failed_result<std::optional<ComputePlanningInspectionSnapshot>>(
        not_connected_status());
  }
  RawCallResult call =
      impl_->call("inspect.compute_planning",
                  internal::Json{{"session_id", session_id.value}});
  if (!call.status.ok) {
    return failed_result<std::optional<ComputePlanningInspectionSnapshot>>(
        std::move(call.status));
  }
  std::string message;
  if (!decode_session_echo(call.result, session_id, &message) ||
      !call.result.contains("planning")) {
    if (message.empty()) {
      message = "inspect.compute_planning result is missing planning";
    }
    return failed_result<std::optional<ComputePlanningInspectionSnapshot>>(
        invalid_response(std::move(message)));
  }
  if (call.result["planning"].is_null()) {
    return PlanningResult{internal::ok_status(), std::nullopt};
  }
  ComputePlanningInspectionSnapshot planning;
  if (!decode_compute_planning(call.result["planning"], &planning, &message)) {
    return failed_result<std::optional<ComputePlanningInspectionSnapshot>>(
        invalid_response(std::move(message)));
  }
  return PlanningResult{internal::ok_status(), std::move(planning)};
}

/** @copydoc Client::recent_compute_planning_snapshots */
IpcResult<std::vector<ComputePlanningInspectionSnapshot>>
Client::recent_compute_planning_snapshots(const IpcSessionId& session_id) {
  using PlanningList = std::vector<ComputePlanningInspectionSnapshot>;
  if (!impl_) {
    return failed_result<PlanningList>(not_connected_status());
  }
  const internal::Json base{{"session_id", session_id.value}};
  return aggregate_collection<PlanningList, PlanningList>(
      [this, base](const std::optional<std::string>& cursor, std::size_t offset,
                   std::size_t limit) {
        return impl_->call("inspect.recent_compute_planning",
                           collection_page_params(base, cursor, offset, limit));
      },
      [&session_id](const internal::Json& result, PlanningList* page,
                    std::size_t* rows, std::string* message) {
        if (!decode_session_echo(result, session_id, message) ||
            !result.contains("snapshots") ||
            !internal::valid_bounded_array(result["snapshots"],
                                           internal::kGeneralPageMaxEntries)) {
          if (message->empty()) {
            *message = "recent planning result has an invalid page shape";
          }
          return false;
        }
        PlanningList decoded;
        decoded.reserve(result["snapshots"].size());
        for (const internal::Json& item : result["snapshots"]) {
          ComputePlanningInspectionSnapshot snapshot;
          if (!decode_compute_planning(item, &snapshot, message)) {
            return false;
          }
          decoded.push_back(std::move(snapshot));
        }
        *rows = decoded.size();
        *page = std::move(decoded);
        return true;
      },
      [](const internal::Json&, const PlanningList& page, bool first_page,
         internal::CollectionPageMeasurement* measurement,
         std::string* message) {
        return measure_planning_collection_page(page, first_page, measurement,
                                                message);
      },
      [](PlanningList* aggregate, PlanningList page, std::string*) {
        aggregate->insert(aggregate->end(),
                          std::make_move_iterator(page.begin()),
                          std::make_move_iterator(page.end()));
        return true;
      });
}

/** @copydoc Client::traversal_orders */
IpcResult<std::map<int, std::vector<NodeId>>> Client::traversal_orders(
    const IpcSessionId& session_id) {
  using TraversalMap = std::map<int, std::vector<NodeId>>;
  using TraversalPage = std::vector<TraversalOrderPageRow>;
  if (!impl_) {
    return failed_result<TraversalMap>(not_connected_status());
  }
  const internal::Json base{{"session_id", session_id.value}};
  return aggregate_collection<TraversalMap, TraversalPage>(
      [this, base](const std::optional<std::string>& cursor, std::size_t offset,
                   std::size_t limit) {
        return impl_->call("inspect.traversal_orders",
                           collection_page_params(base, cursor, offset, limit));
      },
      [&session_id](const internal::Json& result, TraversalPage* page,
                    std::size_t* rows, std::string* message) {
        if (!decode_session_echo(result, session_id, message) ||
            !result.contains("orders") ||
            !decode_traversal_order_rows(result["orders"], page, message)) {
          if (message->empty()) {
            *message = "traversal orders returned an invalid page";
          }
          return false;
        }
        *rows = page->size();
        return true;
      },
      [](const internal::Json&, const TraversalPage& page, bool first_page,
         internal::CollectionPageMeasurement* measurement,
         std::string* message) {
        return measure_traversal_order_collection_page(page, first_page,
                                                       measurement, message);
      },
      [](TraversalMap* aggregate, TraversalPage page, std::string* message) {
        for (TraversalOrderPageRow& row : page) {
          if ((!aggregate->empty() &&
               row.ending_node_id <= aggregate->rbegin()->first) ||
              !aggregate->emplace(row.ending_node_id, std::move(row.node_ids))
                   .second) {
            *message =
                "traversal-order keys are duplicated or unsorted across pages";
            return false;
          }
        }
        return true;
      });
}

/** @copydoc Client::traversal_details */
IpcResult<std::map<int, std::vector<HostTraversalNodeSnapshot>>>
Client::traversal_details(const IpcSessionId& session_id) {
  using TraversalMap = std::map<int, std::vector<HostTraversalNodeSnapshot>>;
  using TraversalPage = std::vector<TraversalDetailPageRow>;
  if (!impl_) {
    return failed_result<TraversalMap>(not_connected_status());
  }
  const internal::Json base{{"session_id", session_id.value}};
  return aggregate_collection<TraversalMap, TraversalPage>(
      [this, base](const std::optional<std::string>& cursor, std::size_t offset,
                   std::size_t limit) {
        return impl_->call("inspect.traversal_details",
                           collection_page_params(base, cursor, offset, limit));
      },
      [&session_id](const internal::Json& result, TraversalPage* page,
                    std::size_t* rows, std::string* message) {
        if (!decode_session_echo(result, session_id, message) ||
            !result.contains("branches") ||
            !decode_traversal_detail_rows(result["branches"], page, message)) {
          if (message->empty()) {
            *message = "traversal details returned an invalid page";
          }
          return false;
        }
        *rows = page->size();
        return true;
      },
      [](const internal::Json&, const TraversalPage& page, bool first_page,
         internal::CollectionPageMeasurement* measurement,
         std::string* message) {
        return measure_traversal_detail_collection_page(page, first_page,
                                                        measurement, message);
      },
      [](TraversalMap* aggregate, TraversalPage page, std::string* message) {
        for (TraversalDetailPageRow& row : page) {
          if ((!aggregate->empty() &&
               row.ending_node_id <= aggregate->rbegin()->first) ||
              !aggregate->emplace(row.ending_node_id, std::move(row.nodes))
                   .second) {
            *message =
                "traversal-detail keys are duplicated or unsorted across pages";
            return false;
          }
        }
        return true;
      });
}

/** @copydoc Client::submit_compute */
IpcResult<ComputeJobSnapshot> Client::submit_compute(
    const ComputeSubmitRequest& request) {
  if (!impl_) {
    return failed_result<ComputeJobSnapshot>(not_connected_status());
  }
  internal::Json params{
      {"session_id", request.session_id.value},
      {"node_id", request.node.value},
      {"result_mode",
       request.result_mode == ComputeResultMode::Status ? "status" : "image"},
      {"cache",
       internal::Json{{"precision", request.cache.precision},
                      {"force_recache", request.cache.force_recache},
                      {"disable_disk_cache", request.cache.disable_disk_cache},
                      {"nosave", request.cache.nosave}}},
      {"execution", internal::Json{{"parallel", request.execution.parallel},
                                   {"quiet", request.execution.quiet}}},
      {"telemetry",
       internal::Json{{"enable_timing", request.telemetry.enable_timing}}}};
  if (request.result_mode != ComputeResultMode::Status &&
      request.result_mode != ComputeResultMode::Image) {
    return failed_result<ComputeJobSnapshot>(internal::failure_status(
        OperationErrorDomain::Protocol, internal::kInvalidParamsCode,
        "invalid_params", "compute result mode has no version 1 label"));
  }
  if (request.intent) {
    internal::Json intent;
    if (!internal::encode_enum(*request.intent, &intent)) {
      return failed_result<ComputeJobSnapshot>(internal::failure_status(
          OperationErrorDomain::Protocol, internal::kInvalidParamsCode,
          "invalid_params", "compute intent has no version 1 label"));
    }
    params["intent"] = std::move(intent);
  }
  if (request.dirty_roi) {
    params["dirty_roi"] = internal::encode_pixel_rect(*request.dirty_roi);
  }
  RawCallResult call = impl_->call("compute.submit", params);
  if (!call.status.ok) {
    return failed_result<ComputeJobSnapshot>(std::move(call.status));
  }
  ComputeJobSnapshot result;
  std::string message;
  if (!decode_compute_job(call.result, ComputeJobResponseKind::Submit,
                          std::nullopt, request.session_id, &result,
                          &message)) {
    return failed_result<ComputeJobSnapshot>(
        invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(result)};
}

/** @copydoc Client::compute_status */
IpcResult<ComputeJobSnapshot> Client::compute_status(
    const ComputeRequestId& compute_id) {
  if (!impl_) {
    return failed_result<ComputeJobSnapshot>(not_connected_status());
  }
  RawCallResult call = impl_->call(
      "compute.status", internal::Json{{"compute_id", compute_id.value}});
  if (!call.status.ok) {
    return failed_result<ComputeJobSnapshot>(std::move(call.status));
  }
  ComputeJobSnapshot result;
  std::string message;
  if (!decode_compute_job(call.result, ComputeJobResponseKind::Status,
                          compute_id, std::nullopt, &result, &message)) {
    return failed_result<ComputeJobSnapshot>(
        invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(result)};
}

/** @copydoc Client::compute_result */
IpcResult<ComputeJobSnapshot> Client::compute_result(
    const ComputeRequestId& compute_id) {
  if (!impl_) {
    return failed_result<ComputeJobSnapshot>(not_connected_status());
  }
  RawCallResult call = impl_->call(
      "compute.result", internal::Json{{"compute_id", compute_id.value}});
  if (!call.status.ok) {
    return failed_result<ComputeJobSnapshot>(std::move(call.status));
  }
  ComputeJobSnapshot result;
  std::string message;
  if (!decode_compute_job(call.result, ComputeJobResponseKind::Result,
                          compute_id, std::nullopt, &result, &message)) {
    return failed_result<ComputeJobSnapshot>(
        invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(result)};
}

/** @copydoc Client::release_compute */
IpcResult<ComputeReleaseResult> Client::release_compute(
    const ComputeRequestId& compute_id,
    std::optional<DeliveryLeaseId> delivery_id) {
  if (!impl_) {
    return failed_result<ComputeReleaseResult>(not_connected_status());
  }
  internal::Json params{{"compute_id", compute_id.value}};
  if (delivery_id) {
    params["delivery_id"] = delivery_id->value;
  }
  RawCallResult call = impl_->call("compute.release", params);
  if (!call.status.ok) {
    return failed_result<ComputeReleaseResult>(std::move(call.status));
  }
  ComputeReleaseResult result;
  std::string message;
  if (!decode_compute_release(call.result, compute_id, &result, &message)) {
    return failed_result<ComputeReleaseResult>(
        invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(result)};
}

/** @copydoc Client::timing */
IpcResult<TimingSnapshot> Client::timing(const IpcSessionId& session_id) {
  if (!impl_) {
    return failed_result<TimingSnapshot>(not_connected_status());
  }
  RawCallResult call = impl_->call(
      "compute.timing", internal::Json{{"session_id", session_id.value}});
  if (!call.status.ok) {
    return failed_result<TimingSnapshot>(std::move(call.status));
  }
  TimingSnapshot result;
  std::string message;
  if (!decode_timing_result(call.result, session_id, &result, &message)) {
    return failed_result<TimingSnapshot>(invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(result)};
}

/** @copydoc Client::last_io_time */
IpcResult<double> Client::last_io_time(const IpcSessionId& session_id) {
  if (!impl_) {
    return failed_result<double>(not_connected_status());
  }
  RawCallResult call = impl_->call(
      "compute.last_io_time", internal::Json{{"session_id", session_id.value}});
  if (!call.status.ok) {
    return failed_result<double>(std::move(call.status));
  }
  double result = 0.0;
  std::string message;
  if (!decode_session_echo(call.result, session_id, &message) ||
      !call.result.contains("last_io_time_ms") ||
      !decode_wire_double(call.result["last_io_time_ms"], &result)) {
    if (message.empty()) {
      message = "compute.last_io_time returned an invalid value";
    }
    return failed_result<double>(invalid_response(std::move(message)));
  }
  return {internal::ok_status(), result};
}

/** @copydoc Client::last_error */
IpcResult<OperationStatus> Client::last_error(const IpcSessionId& session_id) {
  if (!impl_) {
    return failed_result<OperationStatus>(not_connected_status());
  }
  RawCallResult call = impl_->call(
      "compute.last_error", internal::Json{{"session_id", session_id.value}});
  if (!call.status.ok) {
    return failed_result<OperationStatus>(std::move(call.status));
  }
  OperationStatus result;
  std::string message;
  if (!decode_session_echo(call.result, session_id, &message) ||
      !call.result.contains("status") ||
      !internal::decode_operation_status(call.result["status"], &result,
                                         &message)) {
    if (message.empty()) {
      message = "compute.last_error returned an invalid nested status";
    }
    return failed_result<OperationStatus>(invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(result)};
}

/** @copydoc Client::clear_cache */
VoidResult Client::clear_cache(const IpcSessionId& session_id) {
  if (!impl_) {
    return {not_connected_status()};
  }
  return impl_->void_call("cache.clear_all",
                          internal::Json{{"session_id", session_id.value}});
}

/** @copydoc Client::clear_drive_cache */
VoidResult Client::clear_drive_cache(const IpcSessionId& session_id) {
  if (!impl_) {
    return {not_connected_status()};
  }
  return impl_->void_call("cache.clear_drive",
                          internal::Json{{"session_id", session_id.value}});
}

/** @copydoc Client::clear_memory_cache */
VoidResult Client::clear_memory_cache(const IpcSessionId& session_id) {
  if (!impl_) {
    return {not_connected_status()};
  }
  return impl_->void_call("cache.clear_memory",
                          internal::Json{{"session_id", session_id.value}});
}

/** @copydoc Client::cache_all_nodes */
VoidResult Client::cache_all_nodes(const IpcSessionId& session_id,
                                   const std::string& precision) {
  if (!impl_) {
    return {not_connected_status()};
  }
  return impl_->void_call("cache.cache_all_nodes",
                          internal::Json{{"session_id", session_id.value},
                                         {"precision", precision}});
}

/** @copydoc Client::free_transient_memory */
VoidResult Client::free_transient_memory(const IpcSessionId& session_id) {
  if (!impl_) {
    return {not_connected_status()};
  }
  return impl_->void_call("cache.free_transient",
                          internal::Json{{"session_id", session_id.value}});
}

/** @copydoc Client::synchronize_disk_cache */
VoidResult Client::synchronize_disk_cache(const IpcSessionId& session_id,
                                          const std::string& precision) {
  if (!impl_) {
    return {not_connected_status()};
  }
  return impl_->void_call("cache.synchronize_disk",
                          internal::Json{{"session_id", session_id.value},
                                         {"precision", precision}});
}

/** @copydoc Client::begin_dirty_source */
IpcResult<DirtyRegionInspectionSnapshot> Client::begin_dirty_source(
    const IpcSessionId& session_id, NodeId node, DirtyDomain domain,
    const PixelRect& source_roi) {
  if (!impl_) {
    return failed_result<DirtyRegionInspectionSnapshot>(not_connected_status());
  }
  internal::Json encoded_domain;
  if (!internal::encode_enum(domain, &encoded_domain)) {
    return failed_result<DirtyRegionInspectionSnapshot>(
        internal::failure_status(OperationErrorDomain::Protocol,
                                 internal::kInvalidParamsCode, "invalid_params",
                                 "dirty domain has no version 1 label"));
  }
  RawCallResult call = impl_->call(
      "dirty.begin",
      internal::Json{{"session_id", session_id.value},
                     {"node_id", node.value},
                     {"domain", std::move(encoded_domain)},
                     {"source_roi", internal::encode_pixel_rect(source_roi)}});
  if (!call.status.ok) {
    return failed_result<DirtyRegionInspectionSnapshot>(std::move(call.status));
  }
  DirtyRegionInspectionSnapshot result;
  std::string message;
  if (!decode_dirty_region(call.result, session_id, &result, &message)) {
    return failed_result<DirtyRegionInspectionSnapshot>(
        invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(result)};
}

/** @copydoc Client::update_dirty_source */
IpcResult<DirtyRegionInspectionSnapshot> Client::update_dirty_source(
    const IpcSessionId& session_id, NodeId node, DirtyDomain domain,
    const PixelRect& source_roi) {
  if (!impl_) {
    return failed_result<DirtyRegionInspectionSnapshot>(not_connected_status());
  }
  internal::Json encoded_domain;
  if (!internal::encode_enum(domain, &encoded_domain)) {
    return failed_result<DirtyRegionInspectionSnapshot>(
        internal::failure_status(OperationErrorDomain::Protocol,
                                 internal::kInvalidParamsCode, "invalid_params",
                                 "dirty domain has no version 1 label"));
  }
  RawCallResult call = impl_->call(
      "dirty.update",
      internal::Json{{"session_id", session_id.value},
                     {"node_id", node.value},
                     {"domain", std::move(encoded_domain)},
                     {"source_roi", internal::encode_pixel_rect(source_roi)}});
  if (!call.status.ok) {
    return failed_result<DirtyRegionInspectionSnapshot>(std::move(call.status));
  }
  DirtyRegionInspectionSnapshot result;
  std::string message;
  if (!decode_dirty_region(call.result, session_id, &result, &message)) {
    return failed_result<DirtyRegionInspectionSnapshot>(
        invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(result)};
}

/** @copydoc Client::end_dirty_source */
IpcResult<DirtyRegionInspectionSnapshot> Client::end_dirty_source(
    const IpcSessionId& session_id, NodeId node, DirtyDomain domain) {
  if (!impl_) {
    return failed_result<DirtyRegionInspectionSnapshot>(not_connected_status());
  }
  internal::Json encoded_domain;
  if (!internal::encode_enum(domain, &encoded_domain)) {
    return failed_result<DirtyRegionInspectionSnapshot>(
        internal::failure_status(OperationErrorDomain::Protocol,
                                 internal::kInvalidParamsCode, "invalid_params",
                                 "dirty domain has no version 1 label"));
  }
  RawCallResult call = impl_->call(
      "dirty.end", internal::Json{{"session_id", session_id.value},
                                  {"node_id", node.value},
                                  {"domain", std::move(encoded_domain)}});
  if (!call.status.ok) {
    return failed_result<DirtyRegionInspectionSnapshot>(std::move(call.status));
  }
  DirtyRegionInspectionSnapshot result;
  std::string message;
  if (!decode_dirty_region(call.result, session_id, &result, &message)) {
    return failed_result<DirtyRegionInspectionSnapshot>(
        invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(result)};
}

/** @copydoc Client::drain_compute_events */
IpcResult<ComputeEventBatch> Client::drain_compute_events(
    const IpcSessionId& session_id, std::size_t limit) {
  if (!impl_) {
    return failed_result<ComputeEventBatch>(not_connected_status());
  }
  RawCallResult call = impl_->call(
      "events.drain",
      internal::Json{{"session_id", session_id.value}, {"limit", limit}});
  if (!call.status.ok) {
    return failed_result<ComputeEventBatch>(std::move(call.status));
  }
  ComputeEventBatch result;
  std::string message;
  if (!decode_compute_event_result(call.result, session_id, limit, &result,
                                   &message)) {
    return failed_result<ComputeEventBatch>(
        invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(result)};
}

/** @copydoc Client::scheduler_trace */
IpcResult<SchedulerTracePage> Client::scheduler_trace(
    const IpcSessionId& session_id, std::uint64_t after_sequence,
    std::size_t limit) {
  if (!impl_) {
    return failed_result<SchedulerTracePage>(not_connected_status());
  }
  RawCallResult call = impl_->call(
      "scheduler.trace", internal::Json{{"session_id", session_id.value},
                                        {"after_sequence", after_sequence},
                                        {"limit", limit}});
  if (!call.status.ok) {
    return failed_result<SchedulerTracePage>(std::move(call.status));
  }
  SchedulerTracePage result;
  std::string message;
  if (!decode_scheduler_trace_result(call.result, session_id, after_sequence,
                                     limit, &result, &message)) {
    return failed_result<SchedulerTracePage>(
        invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(result)};
}

/** @copydoc Client::plugins_load_report */
IpcResult<HostPluginLoadReport> Client::plugins_load_report(
    const std::vector<std::string>& directories) {
  if (!impl_) {
    return failed_result<HostPluginLoadReport>(not_connected_status());
  }
  RawCallResult call = impl_->call(
      "plugins.load_report", internal::Json{{"directories", directories}});
  if (!call.status.ok) {
    return failed_result<HostPluginLoadReport>(std::move(call.status));
  }
  HostPluginLoadReport result;
  std::string message;
  if (!decode_plugin_load_report(call.result, &result, &message)) {
    return failed_result<HostPluginLoadReport>(
        invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(result)};
}

/** @copydoc Client::ops_sources */
IpcResult<std::map<std::string, std::string>> Client::ops_sources() {
  if (!impl_) {
    return failed_result<std::map<std::string, std::string>>(
        not_connected_status());
  }
  return impl_->plugin_source_collection("plugins.ops_sources");
}

/** @copydoc Client::ops_combined_keys */
IpcResult<std::vector<std::string>> Client::ops_combined_keys() {
  if (!impl_) {
    return failed_result<std::vector<std::string>>(not_connected_status());
  }
  return impl_->sorted_string_collection("plugins.ops_combined_keys", "keys",
                                         internal::kShortTextMaxBytes);
}

/** @copydoc Client::ops_combined_sources */
IpcResult<std::map<std::string, std::string>> Client::ops_combined_sources() {
  if (!impl_) {
    return failed_result<std::map<std::string, std::string>>(
        not_connected_status());
  }
  return impl_->plugin_source_collection("plugins.ops_combined_sources");
}

/** @copydoc Client::plugins_unload_all */
IpcResult<int> Client::plugins_unload_all() {
  if (!impl_) {
    return failed_result<int>(not_connected_status());
  }
  RawCallResult call =
      impl_->call("plugins.unload_all", internal::Json::object());
  if (!call.status.ok) {
    return failed_result<int>(std::move(call.status));
  }
  int unloaded = -1;
  if (!call.result.contains("unloaded") ||
      !internal::decode_integer(call.result["unloaded"], &unloaded) ||
      unloaded < 0) {
    return failed_result<int>(invalid_response(
        "plugins.unload_all returned an invalid unloaded count"));
  }
  return {internal::ok_status(), unloaded};
}

/** @copydoc Client::seed_builtin_ops */
VoidResult Client::seed_builtin_ops() {
  if (!impl_) {
    return {not_connected_status()};
  }
  return impl_->void_call("plugins.seed_builtins", internal::Json::object());
}

/** @copydoc Client::scheduler_available_types */
IpcResult<std::vector<std::string>> Client::scheduler_available_types() {
  if (!impl_) {
    return failed_result<std::vector<std::string>>(not_connected_status());
  }
  return impl_->sorted_string_collection("scheduler.types", "types",
                                         internal::kShortTextMaxBytes);
}

/** @copydoc Client::scheduler_description */
IpcResult<std::string> Client::scheduler_description(
    const std::string& type_name) {
  if (!impl_) {
    return failed_result<std::string>(not_connected_status());
  }
  RawCallResult call =
      impl_->call("scheduler.description", internal::Json{{"type", type_name}});
  if (!call.status.ok) {
    return failed_result<std::string>(std::move(call.status));
  }
  std::string returned_type;
  std::string description;
  if (!call.result.contains("type") || !call.result.contains("description") ||
      !internal::decode_bounded_string(
          call.result["type"], internal::kShortTextMaxBytes, &returned_type) ||
      returned_type.empty() || returned_type != type_name ||
      !internal::decode_bounded_string(call.result["description"],
                                       internal::kPathTextMaxBytes,
                                       &description)) {
    return failed_result<std::string>(invalid_response(
        "scheduler.description returned an invalid type or description"));
  }
  return {internal::ok_status(), std::move(description)};
}

/** @copydoc Client::scheduler_scan */
IpcResult<std::size_t> Client::scheduler_scan(
    const std::vector<std::string>& directories) {
  if (!impl_) {
    return failed_result<std::size_t>(not_connected_status());
  }
  RawCallResult call = impl_->call(
      "scheduler.scan", internal::Json{{"directories", directories}});
  if (!call.status.ok) {
    return failed_result<std::size_t>(std::move(call.status));
  }
  std::size_t loaded = 0;
  if (!call.result.contains("loaded") ||
      !internal::decode_integer(call.result["loaded"], &loaded)) {
    return failed_result<std::size_t>(
        invalid_response("scheduler.scan returned an invalid loaded count"));
  }
  return {internal::ok_status(), loaded};
}

/** @copydoc Client::scheduler_load */
VoidResult Client::scheduler_load(const std::string& path) {
  if (!impl_) {
    return {not_connected_status()};
  }
  return impl_->void_call("scheduler.load", internal::Json{{"path", path}});
}

/** @copydoc Client::scheduler_loaded_plugins */
IpcResult<std::vector<std::string>> Client::scheduler_loaded_plugins() {
  if (!impl_) {
    return failed_result<std::vector<std::string>>(not_connected_status());
  }
  return impl_->sorted_string_collection("scheduler.loaded_plugins", "plugins",
                                         internal::kPathTextMaxBytes);
}

/** @copydoc Client::configure_scheduler_defaults */
VoidResult Client::configure_scheduler_defaults(
    const HostSchedulerConfig& config) {
  if (!impl_) {
    return {not_connected_status()};
  }
  return impl_->void_call(
      "scheduler.configure_defaults",
      internal::Json{{"hp_type", config.hp_type},
                     {"rt_type", config.rt_type},
                     {"worker_count", config.worker_count}});
}

/** @copydoc Client::scheduler_info */
IpcResult<SchedulerInfoSnapshot> Client::scheduler_info(
    const IpcSessionId& session_id, ComputeIntent intent) {
  if (!impl_) {
    return failed_result<SchedulerInfoSnapshot>(not_connected_status());
  }
  internal::Json encoded_intent;
  if (!internal::encode_enum(intent, &encoded_intent)) {
    return failed_result<SchedulerInfoSnapshot>(internal::failure_status(
        OperationErrorDomain::Protocol, internal::kInvalidParamsCode,
        "invalid_params", "scheduler intent has no version 1 label"));
  }
  RawCallResult call = impl_->call(
      "scheduler.info", internal::Json{{"session_id", session_id.value},
                                       {"intent", encoded_intent}});
  if (!call.status.ok) {
    return failed_result<SchedulerInfoSnapshot>(std::move(call.status));
  }
  SchedulerInfoSnapshot result;
  std::string message;
  if (!decode_session_echo(call.result, session_id, &message) ||
      !call.result.contains("intent") ||
      !internal::decode_enum(call.result["intent"], &result.intent) ||
      result.intent != intent || !call.result.contains("scheduler_name") ||
      !internal::decode_bounded_string(call.result["scheduler_name"],
                                       internal::kShortTextMaxBytes,
                                       &result.scheduler_name) ||
      result.scheduler_name.empty() || !call.result.contains("stats") ||
      !internal::decode_bounded_string(
          call.result["stats"], internal::kPathTextMaxBytes, &result.stats)) {
    if (message.empty()) {
      message = "scheduler.info returned invalid or mismatched values";
    }
    return failed_result<SchedulerInfoSnapshot>(
        invalid_response(std::move(message)));
  }
  return {internal::ok_status(), std::move(result)};
}

/** @copydoc Client::replace_scheduler */
VoidResult Client::replace_scheduler(const IpcSessionId& session_id,
                                     ComputeIntent intent,
                                     const std::string& type) {
  if (!impl_) {
    return {not_connected_status()};
  }
  internal::Json encoded_intent;
  if (!internal::encode_enum(intent, &encoded_intent)) {
    return {internal::failure_status(
        OperationErrorDomain::Protocol, internal::kInvalidParamsCode,
        "invalid_params", "scheduler intent has no version 1 label")};
  }
  return impl_->void_call("scheduler.replace",
                          internal::Json{{"session_id", session_id.value},
                                         {"intent", std::move(encoded_intent)},
                                         {"type", type}});
}

}  // namespace ps::ipc
