#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "ipc/protocol_bounds.hpp"
#include "photospider/core/result_types.hpp"
#include "photospider/host/host.hpp"
#include "photospider/ipc/protocol.hpp"

namespace ps::ipc::internal {

/**
 * @brief Version 1 JSON value used only inside IPC implementation targets.
 *
 * @throws std::bad_alloc when value construction or mutation allocates.
 * @note This private alias is not installed or exposed by the typed public IPC
 *       ABI; values own their parser and object storage.
 */
using Json = nlohmann::json;

/**
 * @brief Exact hexadecimal character count of every version 1 opaque id.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The bound applies uniformly to server, session, compute, output,
 *       delivery, and cursor identities without defining their ownership.
 */
inline constexpr std::size_t kOpaqueIdHexCharacters = 32;

/**
 * @brief Maximum UTF-8 byte length of a request id or method name.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The decoded UTF-8 bytes are counted after JSON escape processing.
 */
inline constexpr std::size_t kRequestTextMaxBytes = 128;

/**
 * @brief Maximum UTF-8 byte length of a short public label.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note Version 1 uses this for session names, scheduler/precision labels,
 *       operation/plugin keys, stable enum/error names, and event text.
 */
inline constexpr std::size_t kShortTextMaxBytes = 1024;

/**
 * @brief Maximum UTF-8 byte length of a filesystem path.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note Path syntax, absoluteness, ownership, and NUL rules are validated in
 *       addition to this decoded-byte bound.
 */
inline constexpr std::size_t kPathTextMaxBytes = 4096;

/**
 * @brief Maximum UTF-8 byte length of a wire diagnostic.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note Diagnostics are repaired and truncated at UTF-8 scalar boundaries;
 *       programmatic domain/code/name fields are never truncated.
 */
inline constexpr std::size_t kDiagnosticTextMaxBytes = 4096;

/**
 * @brief Maximum UTF-8 byte length of YAML or one copied source value.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The limit applies independently to each decoded YAML, parameter, or
 *       source string and remains below the 16 MiB frame maximum.
 */
inline constexpr std::size_t kLargeTextMaxBytes = 8U * 1024U * 1024U;

/**
 * @brief Maximum number of directories or paths in one input array.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note Every element must also satisfy its field-specific path/text bound
 *       before a plugin, scheduler, graph, or filesystem mutation begins.
 */
inline constexpr std::size_t kPathArrayMaxEntries = 256;

/**
 * @brief Exact sorted method subset advertised by current version 1 metadata.
 *
 * @throws Nothing; this is immutable compile-time metadata.
 * @note `daemon.version` and the installed typed Client use this exact subset.
 *       The request router additionally accepts the documented Host-backed
 *       graph-state and diagnostic schemas without a public raw-JSON client
 *       escape hatch.
 */
inline constexpr std::array<std::string_view, 8> kVersionOneMethodNames = {
    "daemon.ping",   "daemon.version", "graph.close",
    "graph.list",    "graph.load",     "inspect.dependency_tree",
    "inspect.graph", "inspect.node"};  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Decodes one JSON integer without signedness loss or narrowing.
 *
 * @tparam Integer Non-boolean integral destination type no wider than the JSON
 *         library's unsigned integer storage.
 * @param value Candidate signed or unsigned JSON integer.
 * @param output Receives the exact value only after range validation succeeds.
 * @return True when `value` is an integer representable by `Integer` and
 *         `output` is nonnull; otherwise false without modifying `output`.
 * @throws Nothing.
 * @note The JSON storage category is inspected before conversion. This avoids
 *       implementation-defined signed/unsigned wraparound and prevents
 *       `nlohmann::json::get<Integer>()` from silently narrowing a wire value.
 */
template <typename Integer>
bool decode_integer(const Json& value, Integer* output) noexcept {
  using Target = std::remove_cv_t<Integer>;
  static_assert(std::is_integral_v<Target>,
                "IPC integer destination must be integral");
  static_assert(!std::is_same_v<Target, bool>,
                "IPC booleans require explicit boolean decoding");
  static_assert(std::numeric_limits<Target>::digits <=
                    std::numeric_limits<Json::number_unsigned_t>::digits,
                "IPC integer destination exceeds JSON integer storage");
  if (output == nullptr) {
    return false;
  }

  if (value.is_number_unsigned()) {
    const Json::number_unsigned_t decoded =
        value.template get_ref<const Json::number_unsigned_t&>();
    const Json::number_unsigned_t maximum =
        static_cast<Json::number_unsigned_t>(
            std::numeric_limits<Target>::max());
    if (decoded > maximum) {
      return false;
    }
    *output = static_cast<Target>(decoded);
    return true;
  }
  if (!value.is_number_integer()) {
    return false;
  }

  const Json::number_integer_t decoded =
      value.template get_ref<const Json::number_integer_t&>();
  if constexpr (std::numeric_limits<Target>::is_signed) {
    if (decoded < static_cast<Json::number_integer_t>(
                      std::numeric_limits<Target>::min()) ||
        decoded > static_cast<Json::number_integer_t>(
                      std::numeric_limits<Target>::max())) {
      return false;
    }
  } else {
    if (decoded < 0 || static_cast<Json::number_unsigned_t>(decoded) >
                           static_cast<Json::number_unsigned_t>(
                               std::numeric_limits<Target>::max())) {
      return false;
    }
  }
  *output = static_cast<Target>(decoded);
  return true;
}

/**
 * @brief JSON-RPC-compatible parse failure code used by version 1.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The numeric wire value is stable for protocol version 1.
 */
inline constexpr std::int32_t kParseErrorCode = -32700;

/**
 * @brief Malformed request or response envelope code used by version 1.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The numeric wire value is stable for protocol version 1.
 */
inline constexpr std::int32_t kInvalidRequestCode = -32600;

/**
 * @brief Unknown method code used by version 1.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The numeric wire value is stable for protocol version 1.
 */
inline constexpr std::int32_t kMethodNotFoundCode = -32601;

/**
 * @brief Invalid typed parameter code used by version 1.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The numeric wire value is stable for protocol version 1.
 */
inline constexpr std::int32_t kInvalidParamsCode = -32602;

/**
 * @brief Unexpected daemon request-processing failure code.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The numeric wire value is stable for protocol version 1.
 */
inline constexpr std::int32_t kInternalErrorCode = -32603;

/**
 * @brief Unsupported protocol negotiation code used by version 1.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The numeric wire value is stable for protocol version 1.
 */
inline constexpr std::int32_t kUnsupportedProtocolCode = -32001;

/**
 * @brief Bounded-response overflow code used by version 1.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The numeric wire value is stable for protocol version 1.
 */
inline constexpr std::int32_t kResponseTooLargeCode = -32002;

/**
 * @brief Unknown, expired, released, or evicted compute-job code.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The numeric wire value is stable for protocol version 1.
 */
inline constexpr std::int32_t kJobNotFoundCode = -32010;

/**
 * @brief Nonterminal compute-job result or release code.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The numeric wire value is stable for protocol version 1.
 */
inline constexpr std::int32_t kJobNotReadyCode = -32011;

/**
 * @brief Bounded daemon-registry admission failure code.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The numeric wire value is stable for protocol version 1.
 */
inline constexpr std::int32_t kCapacityExceededCode = -32012;

/**
 * @brief Missing or identity-mismatched output artifact code.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The numeric wire value is stable for protocol version 1.
 */
inline constexpr std::int32_t kArtifactNotFoundCode = -32013;

/**
 * @brief Output-store count or byte quota failure code.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The numeric wire value is stable for protocol version 1.
 */
inline constexpr std::int32_t kArtifactLimitExceededCode = -32014;

/**
 * @brief Unknown, expired, or identity-mismatched snapshot cursor code.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The numeric wire value is stable for protocol version 1.
 */
inline constexpr std::int32_t kCursorNotFoundCode = -32015;

/**
 * @brief Validates one complete UTF-8 byte sequence.
 *
 * @param value Candidate decoded JSON string bytes.
 * @return True only when `value` contains canonical Unicode scalar encodings.
 * @throws Nothing.
 * @note The check rejects truncated, overlong, surrogate, and out-of-range
 *       sequences. Embedded NUL is valid UTF-8 and is governed separately by
 *       field-specific path or identifier validation.
 */
bool valid_utf8(std::string_view value) noexcept;

/**
 * @brief Validates the exact version 1 opaque identifier representation.
 *
 * @param value Candidate identifier bytes.
 * @return True only for 32 lowercase hexadecimal ASCII characters.
 * @throws Nothing.
 * @note The same shape applies to server, session, compute, output, delivery,
 *       and cursor ids; the helper does not assign ownership semantics.
 */
bool valid_opaque_id(std::string_view value) noexcept;

/**
 * @brief Decodes one exact version 1 opaque identifier transactionally.
 *
 * @param value Candidate JSON string.
 * @param output Receives the 32-character lowercase hexadecimal id only after
 *        type, UTF-8, length, and alphabet validation.
 * @return True on success; false without modifying `output` otherwise.
 * @throws std::bad_alloc if the validated identifier copy cannot be allocated.
 * @note The helper validates shape only and does not resolve registry
 * ownership.
 */
bool decode_opaque_id(const Json& value, std::string* output);

/**
 * @brief Validates one version 1 graph session display name.
 *
 * @param value Candidate decoded UTF-8 bytes.
 * @return True only for a nonempty, at-most-1,024-byte UTF-8 path component
 *         other than dot/dot-dot and without slash, backslash, or NUL.
 * @throws Nothing.
 * @note This validates the public display-name shape only; it neither resolves
 *       a daemon opaque id nor accesses the filesystem.
 */
bool valid_session_name(std::string_view value) noexcept;

/**
 * @brief Generates a 128-bit operating-system-entropy opaque identifier.
 *
 * @return Exactly 32 lowercase hexadecimal characters.
 * @throws std::runtime_error if the operating-system entropy source fails.
 * @throws std::bad_alloc if result storage cannot be allocated.
 * @note Values contain no pointer, path, session name, or process-address data.
 */
std::string generate_opaque_id();

/**
 * @brief Decodes one bounded UTF-8 JSON string transactionally.
 *
 * @param value Candidate JSON string.
 * @param maximum_bytes Inclusive decoded UTF-8 byte limit.
 * @param output Receives a copy only after type, UTF-8, and size validation.
 * @return True on success; false without modifying `output` otherwise.
 * @throws std::bad_alloc if the validated string copy cannot be allocated.
 * @note JSON escapes are already decoded by the parser, so limits apply to the
 *       resulting UTF-8 bytes rather than source escape spelling.
 */
bool decode_bounded_string(const Json& value, std::size_t maximum_bytes,
                           std::string* output);

/**
 * @brief Decodes one bounded array of bounded UTF-8 strings transactionally.
 *
 * @param value Candidate JSON array.
 * @param maximum_entries Inclusive element-count limit.
 * @param maximum_element_bytes Inclusive decoded byte limit for each element.
 * @param output Receives the complete array only after every element validates.
 * @return True on success; false without modifying `output` otherwise.
 * @throws std::bad_alloc if temporary or output storage cannot be allocated.
 */
bool decode_bounded_string_array(const Json& value, std::size_t maximum_entries,
                                 std::size_t maximum_element_bytes,
                                 std::vector<std::string>* output);

/**
 * @brief Validates one JSON array against an inclusive entry limit.
 *
 * @param value Candidate JSON array.
 * @param maximum_entries Inclusive maximum element count.
 * @return True only when `value` is an array within the bound.
 * @throws Nothing.
 * @note Element schemas remain the responsibility of the typed value codec;
 *       this primitive defines no public page or cursor envelope.
 */
bool valid_bounded_array(const Json& value,
                         std::size_t maximum_entries) noexcept;

/**
 * @brief Encodes one bounded page of graph-session summary rows.
 *
 * @param summaries Public session rows returned by the daemon registry.
 * @return JSON array containing `session_id` and `session_name` per row.
 * @throws std::bad_alloc if validation diagnostics or JSON storage cannot be
 *         allocated.
 * @throws std::length_error if the page exceeds
 *         `kGeneralPageMaxEntries`.
 * @throws std::invalid_argument if a row contains a malformed opaque id or
 *         session display name.
 * @note The complete page validates before any response or cursor is
 *       published. Row order is preserved and unknown fields are not emitted.
 */
Json encode_session_summaries(
    const std::vector<GraphSessionSummary>& summaries);

/**
 * @brief Decodes one bounded graph-session summary page transactionally.
 *
 * @param value Candidate JSON array; unknown members of each row are ignored.
 * @param summaries Receives all decoded rows only after the complete page
 *        validates.
 * @param message Receives an owned validation diagnostic on failure.
 * @return True when the page and every required row field are valid; false
 *         without modifying `summaries` otherwise.
 * @throws std::bad_alloc if temporary rows, strings, or diagnostics cannot be
 *         allocated.
 * @note The codec validates shape, the general page bound, opaque ids, and
 *       session display names. Callers retain method-specific ordering rules.
 */
bool decode_session_summaries(const Json& value,
                              std::vector<GraphSessionSummary>* summaries,
                              std::string* message);

/**
 * @brief Bounds and repairs an untrusted diagnostic for a JSON response.
 *
 * @param message Arbitrary bytes from Host, parser, or exception boundaries.
 * @return Valid UTF-8 no longer than `kDiagnosticTextMaxBytes`.
 * @throws std::bad_alloc if result storage cannot be allocated.
 * @note Invalid sequences are replaced with U+FFFD. Truncation occurs only at
 *       code-point boundaries and includes a suffix within the same limit.
 */
std::string bounded_diagnostic(std::string_view message);

/**
 * @brief Decodes a bounded page limit with exact integer semantics.
 *
 * @param value Candidate signed or unsigned JSON integer.
 * @param minimum Inclusive minimum accepted limit.
 * @param maximum Inclusive maximum accepted limit.
 * @param output Receives the exact limit only after all checks succeed.
 * @return True on success; false without modifying `output` otherwise.
 * @throws Nothing.
 */
bool decode_page_limit(const Json& value, std::size_t minimum,
                       std::size_t maximum, std::size_t* output) noexcept;

/**
 * @brief Decodes an offset/limit window without arithmetic overflow.
 *
 * @param offset_value Candidate zero-based unsigned offset.
 * @param limit_value Candidate bounded positive limit.
 * @param maximum_limit Inclusive method-specific page limit.
 * @param offset Receives the exact offset only after both values validate.
 * @param limit Receives the exact limit only after both values validate.
 * @return True when both integers fit and `offset + limit` is representable;
 *         false without modifying either output otherwise.
 * @throws Nothing.
 * @note This primitive does not define a public page envelope or cursor shape.
 */
bool decode_page_window(const Json& offset_value, const Json& limit_value,
                        std::size_t maximum_limit, std::size_t* offset,
                        std::size_t* limit) noexcept;

/**
 * @brief Encodes one public pixel rectangle.
 *
 * @param rect Rectangle to serialize without changing empty-ROI semantics.
 * @return Object containing exact x, y, width, and height integers.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 */
Json encode_pixel_rect(const PixelRect& rect);

/**
 * @brief Decodes one public pixel rectangle transactionally.
 *
 * @param value Candidate object; unknown members are ignored.
 * @param rect Receives all four exact `int` values only after validation.
 * @return True on success; false without modifying `rect` otherwise.
 * @throws Nothing.
 * @note Non-positive width or height remains representable as documented by
 *       `PixelRect`; this codec validates shape and range only.
 */
bool decode_pixel_rect(const Json& value, PixelRect* rect) noexcept;

/**
 * @brief Encodes one direct bounded page of public node identifiers.
 *
 * @param nodes Complete Host-returned node-id value for the current direct
 *        result shape.
 * @return JSON array preserving Host order and exact nonnegative ids.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 * @throws std::length_error if the direct page exceeds
 *         `kGeneralPageMaxEntries`.
 * @throws std::invalid_argument if any Host-returned node id is negative.
 * @note This direct encoder neither publishes nor consumes a stable
 *       collection cursor. Its 4,096-entry integer-only bound is structurally
 *       below the frame limit, so it cannot create an aggregate JSON peak.
 */
Json encode_node_ids(const std::vector<NodeId>& nodes);

/**
 * @brief Encodes one bounded node-YAML result.
 *
 * @param session_id Opaque daemon session id visible to the caller.
 * @param node Exact nonnegative node id supplied to Host.
 * @param yaml_text Complete YAML text returned by Host.
 * @return Object containing `session_id`, `node_id`, and `yaml_text`.
 * @throws std::bad_alloc if validation diagnostics or JSON storage cannot be
 *         allocated.
 * @throws std::length_error if YAML exceeds `kLargeTextMaxBytes`.
 * @throws std::invalid_argument if the opaque id, node id, or UTF-8 text is
 *         malformed.
 * @note YAML is rejected whole rather than truncated or repaired.
 *       One raw string is capped at 8 MiB; any escape expansion that exceeds
 *       the frame is mapped by final response serialization rather than by an
 *       unbounded multi-value aggregate.
 */
Json encode_node_yaml(const IpcSessionId& session_id, NodeId node,
                      const std::string& yaml_text);

/**
 * @brief Encodes one copied Host timing snapshot.
 *
 * @param request_id Valid nonempty correlated id for response-size preflight.
 * @param session_id Opaque daemon session id visible to the caller.
 * @param timing Complete timing value returned by Host.
 * @return Object containing `session_id`, ordered `node_timings`, and
 *         `total_ms`.
 * @throws std::bad_alloc if validation diagnostics or JSON storage cannot be
 *         allocated.
 * @throws std::length_error if rows/strings exceed their version 1 bounds or
 *         an overflow-safe serialized-size lower bound proves that the actual
 *         success frame cannot fit in 16 MiB.
 * @throws std::invalid_argument if the request id, opaque id, a node id, or
 *         UTF-8 text is malformed.
 * @note Before result JSON allocation, fixed schema, actual request/session
 *       ids, exact string escaping, and exact integer sizes are aggregated.
 *       Finite doubles use a one-byte lower bound so a potentially encodable
 *       near-limit value is never rejected by preflight. Non-finite values
 *       encode as JSON null without changing row order.
 */
Json encode_timing(std::string_view request_id, const IpcSessionId& session_id,
                   const TimingSnapshot& timing);

/**
 * @brief Encodes one bounded destructive compute-event batch.
 *
 * @param session_id Opaque daemon session id visible to the caller.
 * @param batch Complete Host-returned bounded event batch.
 * @param requested_limit Exact validated wire limit passed to Host.
 * @return Object containing session id, ordered events, and locked metadata.
 * @throws std::bad_alloc if validation diagnostics or JSON storage cannot be
 *         allocated.
 * @throws std::length_error if event count or text exceeds its version 1
 *         bound.
 * @throws std::invalid_argument if ids, sequences, UTF-8, page metadata, or
 *         requested-limit invariants are malformed.
 * @note Event text is never repaired or truncated. Non-finite elapsed values
 *       use the established JSON-null observation representation.
 */
Json encode_compute_event_batch(const IpcSessionId& session_id,
                                const ComputeEventBatch& batch,
                                std::size_t requested_limit);

/**
 * @brief Encodes one bounded non-destructive scheduler-trace page.
 *
 * @param session_id Opaque daemon session id visible to the caller.
 * @param after_sequence Exact exclusive wire cursor passed to Host.
 * @param page Complete Host-returned sequence page.
 * @param requested_limit Exact validated wire limit passed to Host.
 * @return Object containing session id, ordered traces, and locked metadata.
 * @throws std::bad_alloc if validation diagnostics or JSON storage cannot be
 *         allocated.
 * @throws std::length_error if the event count exceeds the version 1 bound.
 * @throws std::invalid_argument if ids, sequences, enums, page metadata, or
 *         requested-limit invariants are malformed.
 * @note The encoder accepts nonnegative node/worker ids and preserves `-1` as
 *       the no-specific-node/worker sentinel. Values below -1 are rejected,
 *       and Host trace state is never mutated.
 */
Json encode_scheduler_trace_page(const IpcSessionId& session_id,
                                 uint64_t after_sequence,
                                 const SchedulerTracePage& page,
                                 std::size_t requested_limit);

/**
 * @brief Encodes one copied dirty-region snapshot.
 *
 * @param request_id Valid nonempty correlated id for response-size preflight.
 * @param session_id Opaque daemon session id visible to the caller.
 * @param snapshot Complete public Host snapshot.
 * @return Object containing the opaque session plus all copied dirty fields.
 * @throws std::bad_alloc if validation diagnostics or JSON storage cannot be
 *         allocated.
 * @throws std::length_error if any direct collection exceeds its current page
 *         bound or exact overflow-safe aggregate response size exceeds 16 MiB.
 * @throws std::invalid_argument if the request id, opaque id, node id, or
 *         public enum is malformed.
 * @note Every top-level and nested ROI collection is measured in one exact
 *       compact serialized-size budget before result arrays are allocated.
 *       `actual_dirty_rois` is encoded as ordered `{node_id, rois}` rows so
 *       integer node identity never depends on JSON object-key parsing.
 */
Json encode_dirty_region(std::string_view request_id,
                         const IpcSessionId& session_id,
                         const DirtyRegionInspectionSnapshot& snapshot);

/**
 * @brief Encodes one copied last-IO diagnostic value.
 *
 * @param session_id Opaque daemon session id visible to the caller.
 * @param milliseconds Host-returned last IO duration in milliseconds.
 * @return Object containing `session_id` and `last_io_time_ms`.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 * @throws std::invalid_argument if the opaque id is malformed.
 * @note A non-finite public diagnostic encodes as JSON null.
 */
Json encode_last_io_time(const IpcSessionId& session_id, double milliseconds);

/**
 * @brief Encodes one compute intent as its stable lowercase label.
 * @param value Public enum value.
 * @param output Receives the label only for a recognized enum value.
 * @return True on success; false without modifying `output` otherwise.
 * @throws std::bad_alloc if JSON string storage cannot be allocated.
 */
bool encode_enum(ComputeIntent value, Json* output);

/**
 * @brief Decodes one stable compute-intent label.
 * @param value Candidate JSON string.
 * @param output Receives the enum only after exact label validation.
 * @return True on success; false without modifying `output` otherwise.
 * @throws Nothing.
 */
bool decode_enum(const Json& value, ComputeIntent* output) noexcept;

/**
 * @brief Encodes one dirty compute domain as its stable lowercase label.
 * @param value Public enum value.
 * @param output Receives the label only for a recognized enum value.
 * @return True on success; false without modifying `output` otherwise.
 * @throws std::bad_alloc if JSON string storage cannot be allocated.
 */
bool encode_enum(DirtyDomain value, Json* output);

/**
 * @brief Decodes one stable dirty-domain label.
 * @param value Candidate JSON string.
 * @param output Receives the enum only after exact label validation.
 * @return True on success; false without modifying `output` otherwise.
 * @throws Nothing.
 */
bool decode_enum(const Json& value, DirtyDomain* output) noexcept;

/**
 * @brief Encodes one dirty-source lifecycle as its stable lowercase label.
 * @param value Public enum value.
 * @param output Receives the label only for a recognized enum value.
 * @return True on success; false without modifying `output` otherwise.
 * @throws std::bad_alloc if JSON string storage cannot be allocated.
 */
bool encode_enum(DirtySourceLifecycleState value, Json* output);

/**
 * @brief Decodes one stable dirty-source lifecycle label.
 * @param value Candidate JSON string.
 * @param output Receives the enum only after exact label validation.
 * @return True on success; false without modifying `output` otherwise.
 * @throws Nothing.
 */
bool decode_enum(const Json& value, DirtySourceLifecycleState* output) noexcept;

/**
 * @brief Encodes one dirty-edge direction as its stable lowercase label.
 * @param value Public enum value.
 * @param output Receives the label only for a recognized enum value.
 * @return True on success; false without modifying `output` otherwise.
 * @throws std::bad_alloc if JSON string storage cannot be allocated.
 */
bool encode_enum(DirtyEdgeDirection value, Json* output);

/**
 * @brief Decodes one stable dirty-edge direction label.
 * @param value Candidate JSON string.
 * @param output Receives the enum only after exact label validation.
 * @return True on success; false without modifying `output` otherwise.
 * @throws Nothing.
 */
bool decode_enum(const Json& value, DirtyEdgeDirection* output) noexcept;

/**
 * @brief Encodes one graph edge kind as its stable lowercase label.
 * @param value Public enum value.
 * @param output Receives the label only for a recognized enum value.
 * @return True on success; false without modifying `output` otherwise.
 * @throws std::bad_alloc if JSON string storage cannot be allocated.
 */
bool encode_enum(HostGraphEdgeKind value, Json* output);

/**
 * @brief Decodes one stable graph-edge-kind label.
 * @param value Candidate JSON string.
 * @param output Receives the enum only after exact label validation.
 * @return True on success; false without modifying `output` otherwise.
 * @throws Nothing.
 */
bool decode_enum(const Json& value, HostGraphEdgeKind* output) noexcept;

/**
 * @brief Encodes one dependency-tree scope as its stable lowercase label.
 * @param value Public enum value.
 * @param output Receives the label only for a recognized enum value.
 * @return True on success; false without modifying `output` otherwise.
 * @throws std::bad_alloc if JSON string storage cannot be allocated.
 */
bool encode_enum(HostDependencyTreeScope value, Json* output);

/**
 * @brief Decodes one stable dependency-tree-scope label.
 * @param value Candidate JSON string.
 * @param output Receives the enum only after exact label validation.
 * @return True on success; false without modifying `output` otherwise.
 * @throws Nothing.
 */
bool decode_enum(const Json& value, HostDependencyTreeScope* output) noexcept;

/**
 * @brief Encodes one scheduler trace action as its stable lowercase label.
 * @param value Public enum value.
 * @param output Receives the label only for a recognized enum value.
 * @return True on success; false without modifying `output` otherwise.
 * @throws std::bad_alloc if JSON string storage cannot be allocated.
 */
bool encode_enum(HostSchedulerTraceAction value, Json* output);

/**
 * @brief Decodes one stable scheduler-trace-action label.
 * @param value Candidate JSON string.
 * @param output Receives the enum only after exact label validation.
 * @return True on success; false without modifying `output` otherwise.
 * @throws Nothing.
 */
bool decode_enum(const Json& value, HostSchedulerTraceAction* output) noexcept;

/**
 * @brief Encodes one image channel data type as its stable lowercase label.
 * @param value Public enum value.
 * @param output Receives the label only for a recognized enum value.
 * @return True on success; false without modifying `output` otherwise.
 * @throws std::bad_alloc if JSON string storage cannot be allocated.
 */
bool encode_enum(DataType value, Json* output);

/**
 * @brief Decodes one stable image data-type label.
 * @param value Candidate JSON string.
 * @param output Receives the enum only after exact label validation.
 * @return True on success; false without modifying `output` otherwise.
 * @throws Nothing.
 */
bool decode_enum(const Json& value, DataType* output) noexcept;

/**
 * @brief Encodes one image device as its stable lowercase label.
 * @param value Public enum value.
 * @param output Receives the label only for a recognized enum value.
 * @return True on success; false without modifying `output` otherwise.
 * @throws std::bad_alloc if JSON string storage cannot be allocated.
 */
bool encode_enum(Device value, Json* output);

/**
 * @brief Decodes one stable image-device label.
 * @param value Candidate JSON string.
 * @param output Receives the enum only after exact label validation.
 * @return True on success; false without modifying `output` otherwise.
 * @throws Nothing.
 */
bool decode_enum(const Json& value, Device* output) noexcept;

/**
 * @brief Result of parsing one JSON payload with duplicate-key rejection.
 *
 * @throws std::bad_alloc when parsed storage or diagnostics cannot be
 *         allocated.
 * @note A duplicate key makes `ok` false even if the underlying parser could
 *       otherwise produce a last-value-wins object.
 */
struct JsonParseResult {
  /** @brief True only for valid JSON with no duplicate object keys. */
  bool ok = false;

  /** @brief True when at least one decoded object key appeared twice. */
  bool duplicate_key = false;

  /** @brief True when the top-level request/response id key was duplicated. */
  bool ambiguous_top_level_id = false;

  /**
   * @brief Parsed value for successful decoding or safe duplicate-id recovery.
   *
   * @note When `duplicate_key` is true, only a unique top-level id may be read;
   *       the value must never be dispatched or published.
   */
  Json value;

  /** @brief Human-readable parse or duplicate-key diagnostic. */
  std::string message;
};

/**
 * @brief Parses UTF-8 JSON while rejecting duplicate decoded object keys.
 *
 * @param payload Exact framed JSON bytes.
 * @return Parsed value or an owned diagnostic.
 * @throws std::bad_alloc if parser or diagnostic allocation fails.
 * @note Unknown fields are retained for forward compatibility. On duplicate
 *       keys, the parsed value is retained only so a unique top-level id can
 *       correlate the rejection; it is never dispatched as a valid request.
 */
JsonParseResult parse_json(const std::string& payload);

/**
 * @brief Creates one successful IPC status.
 *
 * @return Status with `ok=true`, domain `None`, zero code, and empty text.
 * @throws Nothing.
 */
OperationStatus ok_status();

/**
 * @brief Creates one owned failure status.
 *
 * @param domain Local or remote error domain.
 * @param code Versioned wire code or local diagnostic code.
 * @param name Versioned wire name or local diagnostic category.
 * @param message Human-readable diagnostic.
 * @return Owned failure status.
 * @throws std::bad_alloc if strings cannot be copied.
 */
OperationStatus failure_status(OperationErrorDomain domain, std::int32_t code,
                               std::string name, std::string message);

/**
 * @brief Canonicalizes one host graph operation status for IPC.
 *
 * @param status Host operation status to canonicalize.
 * @return Canonical success when `status.ok` is true; a graph-domain failure
 *         with the stable name for a recognized `GraphErrc` code; otherwise an
 *         unchanged copy of `status`.
 * @throws std::bad_alloc if diagnostics cannot be copied.
 * @note Recognized graph failures preserve their numeric code and message but
 *       replace any supplied name with the stable `GraphErrc` name. Non-graph
 *       failures and unrecognized graph codes are preserved verbatim.
 */
OperationStatus graph_status(const OperationStatus& status);

/**
 * @brief Encodes a failure status into the exact version 1 error schema.
 *
 * @param status Failed status to encode.
 * @return Object containing domain, code, name, and message.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 * @throws std::length_error if an unknown future name exceeds its wire bound.
 * @throws std::invalid_argument if the status is successful, uses a non-wire
 *         domain, has an invalid UTF-8 or empty unknown name, or pairs an
 *         unknown code with a currently known name.
 * @note Recognized codes are canonicalized to their stable name regardless of
 *       the supplied name. Unknown future code/name pairs are preserved
 *       without interpreting diagnostic text.
 */
Json encode_error(const OperationStatus& status);

/**
 * @brief Decodes and validates the exact required version 1 error fields.
 *
 * @param value Candidate error object.
 * @param status Receives the owned decoded status.
 * @param message Receives a validation diagnostic on failure.
 * @return True when all required fields, bounds, domain, and stable known
 *         code/name pairing are valid without modifying output on failure.
 * @throws std::bad_alloc if copied strings cannot be allocated.
 * @note Unknown object fields and future numeric code/name pairs are preserved
 *       for forward compatibility. Wire `none` and local-only `transport`
 *       domains are rejected.
 */
bool decode_error(const Json& value, OperationStatus* status,
                  std::string* message);

/**
 * @brief Encodes one canonical nested operation status value.
 *
 * @param status Successful or failed public operation status.
 * @return Object containing `ok`, domain, signed code, stable name, and a
 *         bounded UTF-8 diagnostic.
 * @throws std::bad_alloc if JSON or bounded diagnostic storage cannot allocate.
 * @throws std::length_error if an unknown future name exceeds its wire bound.
 * @throws std::invalid_argument if a failed status cannot be represented by
 *         the version 1 remote error schema.
 * @note Success is canonicalized to none/zero/empty fields. Recognized failure
 *       codes are encoded with their stable name; unknown future pairs are
 *       preserved without interpreting diagnostic text. The 1,024-byte name
 *       and 4,096-byte bounded diagnostic limits keep this indivisible value
 *       structurally below the frame without an aggregate preflight.
 */
Json encode_operation_status(const OperationStatus& status);

/**
 * @brief Decodes one canonical nested operation status transactionally.
 *
 * @param value Candidate status object; unknown members are ignored.
 * @param status Receives the fully owned status only after validation.
 * @param message Receives an owned validation diagnostic on failure.
 * @return True for canonical success or a valid remote failure; false without
 *         modifying `status` otherwise.
 * @throws std::bad_alloc if copied strings cannot be allocated.
 * @note Known code/name pairs are strict in both directions. Unknown future
 *       pairs are preserved. Local-only Transport failures are not accepted
 *       from a daemon wire value.
 */
bool decode_operation_status(const Json& value, OperationStatus* status,
                             std::string* message);

/**
 * @brief Serializes one bounded version 1 success response.
 *
 * @param id Correlated nonempty request id.
 * @param result Successful method result object.
 * @return Success payload, or a bounded `response_too_large` error payload
 *         carrying the same id when serialization exceeds 16 MiB.
 * @throws std::bad_alloc if response construction cannot allocate.
 * @note This shared helper makes the response-size policy independently
 *       testable without constructing an artificial backend graph.
 */
std::string encode_success_response(const std::string& id, Json result);

/**
 * @brief Encodes one copied public node snapshot.
 *
 * @param node Public value to serialize.
 * @return Snake-case JSON object with explicit nullable metadata.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 * @throws std::length_error if a returned string or collection exceeds its
 *         version 1 bound.
 * @throws std::invalid_argument if a string is invalid UTF-8 or the snapshot
 *         contains a negative node id.
 * @note Non-finite floating-point fields are encoded as JSON null.
 */
Json encode_node(const NodeInspectionView& node);

/**
 * @brief Decodes one copied public node snapshot.
 *
 * @param value Candidate snake-case node object.
 * @param node Receives the fully owned public value.
 * @param message Receives a validation diagnostic on failure.
 * @return True only when all required fields have valid version 1 shapes.
 * @throws std::bad_alloc if copied values cannot be allocated.
 * @note JSON null in a floating-point field is restored as quiet NaN.
 */
bool decode_node(const Json& value, NodeInspectionView* node,
                 std::string* message);

/**
 * @brief Encodes one copied graph inspection snapshot.
 *
 * @param session_id Opaque daemon session id published to the client.
 * @param graph Host graph snapshot whose private Host session is not exposed.
 * @return Object containing opaque `session_id` and copied nodes.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 * @throws std::length_error if the node page exceeds its current wire bound.
 * @throws std::invalid_argument if the opaque id, UTF-8, or a nested value is
 *         invalid.
 */
Json encode_graph(const IpcSessionId& session_id,
                  const GraphInspectionView& graph);

/**
 * @brief Decodes one copied graph inspection snapshot.
 *
 * @param value Candidate graph result object.
 * @param graph Receives a snapshot whose session value is the opaque id.
 * @param message Receives a validation diagnostic on failure.
 * @return True when the result follows the version 1 schema.
 * @throws std::bad_alloc if copied values cannot be allocated.
 */
bool decode_graph(const Json& value, GraphInspectionView* graph,
                  std::string* message);

/**
 * @brief Encodes one copied dependency-tree snapshot.
 *
 * @param session_id Opaque daemon session id published to the client.
 * @param tree Host dependency tree to serialize.
 * @return Snake-case flattened dependency-tree result object.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 * @throws std::length_error if a returned string or collection exceeds its
 *         current version 1 wire bound.
 * @throws std::invalid_argument if an id, depth, UTF-8 string, or public enum
 *         is invalid.
 */
Json encode_dependency_tree(const IpcSessionId& session_id,
                            const HostDependencyTreeSnapshot& tree);

/**
 * @brief Decodes one copied dependency-tree snapshot.
 *
 * @param value Candidate dependency-tree result object.
 * @param tree Receives the fully owned public tree.
 * @param message Receives a validation diagnostic on failure.
 * @return True when every scope, edge, node, and optional follows version 1.
 * @throws std::bad_alloc if copied values cannot be allocated.
 */
bool decode_dependency_tree(const Json& value, HostDependencyTreeSnapshot* tree,
                            std::string* message);

}  // namespace ps::ipc::internal
