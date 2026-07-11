#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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
 * @brief Stable domain for an IPC operation status.
 *
 * @throws Nothing.
 * @note Transport failures are always produced locally. Protocol failures may
 *       be produced by local response validation or decoded from a correlated
 *       daemon response; Graph and Daemon failures are remote.
 */
enum class IpcErrorDomain {
  /** @brief No failure occurred. */
  None,

  /** @brief Local connect, read, write, or peer-lifecycle failure. */
  Transport,

  /** @brief Local or remote version 1 protocol-contract failure. */
  Protocol,

  /** @brief Remote failure reported by the public graph Host boundary. */
  Graph,

  /** @brief Remote daemon invariant or request-processing failure. */
  Daemon,
};

/**
 * @brief Owned status returned by public IPC client operations.
 *
 * @throws std::bad_alloc when copied diagnostics cannot be allocated.
 * @note Callers can always branch on `domain`. Remote Protocol, Graph, and
 *       Daemon `code`/`name` mappings are stable for version 1; local Protocol
 *       statuses use version 1 validation categories. Local Transport
 *       `code`/`name` values are diagnostic categories, not a promised durable
 *       mapping. `message` is always diagnostic text.
 */
struct IpcStatus {
  /** @brief True when the requested operation completed successfully. */
  bool ok = true;

  /** @brief Stable failure domain, or `None` on success. */
  IpcErrorDomain domain = IpcErrorDomain::None;

  /** @brief Version 1 Protocol/remote code or diagnostic Transport code. */
  std::int32_t code = 0;

  /** @brief Version 1 Protocol/remote name or diagnostic Transport name. */
  std::string name;

  /** @brief Human-readable diagnostic owned by this value. */
  std::string message;
};

/**
 * @brief Status-oriented result carrying an owned typed value.
 *
 * @tparam Value Default-constructible public value returned on success.
 * @throws Whatever `Value` or `IpcStatus` throws during value operations.
 * @note `value` is authoritative only when `status.ok` is true.
 */
template <typename Value>
struct IpcResult {
  /** @brief Completion status for the operation. */
  IpcStatus status;

  /** @brief Owned operation result, or a default value after failure. */
  Value value{};
};

/**
 * @brief Status-oriented result for a typed call with no payload.
 *
 * @throws std::bad_alloc when status diagnostics cannot be allocated.
 * @note This shape keeps graph-close failures distinguishable from transport
 *       lifecycle operations.
 */
struct IpcVoidResult {
  /** @brief Completion status for the operation. */
  IpcStatus status;
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
