#pragma once

#include <string>

#include "photospider/core/graph_error.hpp"

/**
 * @file result_types.hpp
 * @brief Stable status-oriented result values for public Photospider APIs.
 *
 * These types let host, IPC, plugin, and frontend boundaries report success or
 * recoverable failure without exposing implementation services. Resource
 * exhaustion remains exceptional: construction/copy/mutation may propagate
 * std::bad_alloc instead of manufacturing a misleading failure status. These
 * types are intentionally small value objects and own copied diagnostics.
 */

namespace ps {

/**
 * @brief Status value for a completed operation.
 *
 * @throws std::bad_alloc when diagnostic string construction, copy, or
 * mutation exhausts memory.
 * @note `ok == true` should be paired with `GraphErrc::Unknown` and an empty
 *       message by convention; consumers should branch primarily on `ok`.
 */
struct OperationStatus {
  /** @brief True when the operation completed successfully. */
  bool ok = true;

  /** @brief Stable failure category when ok is false. */
  GraphErrc code = GraphErrc::Unknown;

  /** @brief Human-readable diagnostic text for logs or UI display. */
  std::string message;
};

/**
 * @brief Status-oriented result for an operation that returns a value.
 *
 * @tparam Value Copyable or movable payload type returned by the operation.
 *
 * @throws std::bad_alloc when status diagnostics or payload storage exhausts
 * memory.
 * @throws Whatever `Value` otherwise throws during its own copy, move, or
 * destruction.
 * @note When `status.ok` is false, `value` contains its default-constructed or
 *       caller-provided fallback value and should not be treated as
 *       authoritative.
 */
template <typename Value>
struct Result {
  /** @brief Operation status describing success or failure. */
  OperationStatus status;

  /** @brief Payload value produced by the operation on success. */
  Value value{};
};

/**
 * @brief Status-oriented result for an operation with no payload.
 *
 * @throws std::bad_alloc when status diagnostic construction, copy, or
 * mutation exhausts memory.
 * @note This type keeps void-returning APIs shape-compatible with
 *       `Result<Value>` in host or IPC adapters.
 */
struct VoidResult {
  /** @brief Operation status describing success or failure. */
  OperationStatus status;
};

}  // namespace ps
