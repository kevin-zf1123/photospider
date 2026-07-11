#pragma once

#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <type_traits>

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
IpcStatus ok_status();

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
IpcStatus failure_status(IpcErrorDomain domain, std::int32_t code,
                         std::string name, std::string message);

/**
 * @brief Maps every current graph error explicitly into an IPC status.
 *
 * @param status Host operation status to map.
 * @return Success or graph-domain status with stable numeric/name mapping.
 * @throws std::bad_alloc if diagnostics cannot be copied.
 * @note The implementation uses a complete switch and does not rely on enum
 *       contiguity for future values.
 */
IpcStatus graph_status(const OperationStatus& status);

/**
 * @brief Encodes a failure status into the exact version 1 error schema.
 *
 * @param status Failed status to encode.
 * @return Object containing domain, code, name, and message.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 */
Json encode_error(const IpcStatus& status);

/**
 * @brief Decodes and validates the exact required version 1 error fields.
 *
 * @param value Candidate error object.
 * @param status Receives the owned decoded status.
 * @param message Receives a validation diagnostic on failure.
 * @return True when all required fields and the domain are valid.
 * @throws std::bad_alloc if copied strings cannot be allocated.
 * @note Unknown future numeric codes and names are preserved verbatim.
 */
bool decode_error(const Json& value, IpcStatus* status, std::string* message);

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
