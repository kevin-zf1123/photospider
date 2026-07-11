#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "photospider/core/result_types.hpp"

/**
 * @file protocol.hpp
 * @brief Typed public values for Photospider local IPC protocol version 1.
 *
 * The declarations own their data and deliberately exclude JSON parser types,
 * Unix descriptors, socket structures, and backend implementation objects.
 */

namespace ps::ipc {

/**
 * @brief Wire protocol version implemented by this client product.
 *
 * @throws Nothing.
 * @note The constant is compile-time metadata, not a negotiated mutable state.
 */
inline constexpr std::int32_t kProtocolVersion = 1;

/**
 * @brief Largest accepted version 1 JSON payload in bytes.
 *
 * @throws Nothing.
 * @note The four-byte frame header is not included in this payload bound.
 */
inline constexpr std::size_t kMaximumFramePayloadBytes = 16U * 1024U * 1024U;

/**
 * @brief Status-oriented result carrying an owned typed value.
 *
 * @tparam Value Default-constructible public value returned on success.
 * @throws Whatever `Value` or `OperationStatus` throws during value
 *         operations.
 * @note `value` is authoritative only when `status.ok` is true.
 */
template <typename Value>
struct IpcResult {
  /** @brief Completion status for the operation. */
  OperationStatus status;

  /** @brief Owned operation result, or a default value after failure. */
  Value value{};
};

/**
 * @brief Opaque daemon-global identifier for one active graph session.
 *
 * @throws std::bad_alloc when copied or mutated storage cannot be allocated.
 * @note Version 1 values are 32 lowercase hexadecimal characters generated
 *       from operating-system entropy. Clients must not parse or derive them.
 */
struct IpcSessionId {
  /** @brief Opaque identifier copied from a daemon response. */
  std::string value;
};

/**
 * @brief Typed result of `daemon.ping`.
 *
 * @throws std::bad_alloc when copied string storage cannot be allocated.
 * @note The server instance id changes on each daemon process start.
 */
struct DaemonPing {
  /** @brief True when the daemon routed the ping request. */
  bool pong = false;

  /** @brief Opaque identity of the serving daemon process instance. */
  std::string server_instance_id;
};

/**
 * @brief Typed version and capability metadata for one daemon instance.
 *
 * @throws std::bad_alloc when copied strings or methods cannot be allocated.
 * @note `methods` is sorted and advertises only calls implemented by the
 *       connected protocol slice.
 */
struct DaemonVersion {
  /** @brief Negotiated wire protocol version. */
  std::int32_t protocol_version = 0;

  /** @brief Stable daemon service name. */
  std::string service_name;

  /** @brief Photospider project version used to build the daemon. */
  std::string service_version;

  /** @brief Opaque identity of the serving daemon process instance. */
  std::string server_instance_id;

  /** @brief Local transport name; version 1 reports `unix`. */
  std::string transport;

  /** @brief Sorted wire method names supported by the daemon. */
  std::vector<std::string> methods;
};

/**
 * @brief Public display row for one daemon-owned graph session.
 *
 * @throws std::bad_alloc when copied strings cannot be allocated.
 * @note Only `session_id` addresses later calls; `session_name` is preserved
 *       caller metadata and retains existing Host filesystem semantics.
 */
struct GraphSessionSummary {
  /** @brief Opaque daemon-global session identifier. */
  IpcSessionId session_id;

  /** @brief Caller-provided safe Host session name. */
  std::string session_name;
};

}  // namespace ps::ipc
