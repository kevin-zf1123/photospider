#pragma once

#include <string>

#include "photospider/core/graph_error.hpp"

/**
 * @file result_types.hpp
 * @brief Stable non-throwing result values for public Photospider APIs.
 *
 * These types let host, IPC, plugin, and frontend boundaries report success or
 * failure without exposing exceptions or implementation services. They are
 * intentionally small value objects and own only copied diagnostic text.
 */

namespace ps {

/**
 * @brief Non-throwing status value for a completed operation.
 *
 * @throws Nothing for value operations except string allocation on mutation.
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
 * @brief Non-throwing result for an operation that returns a value.
 *
 * @tparam Value Copyable or movable payload type returned by the operation.
 *
 * @throws Whatever `Value` throws during its own copy, move, or destruction.
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
 * @brief Non-throwing result for an operation with no payload.
 *
 * @throws Nothing for value operations except string allocation on status
 *         mutation.
 * @note This type keeps void-returning APIs shape-compatible with
 *       `Result<Value>` in host or IPC adapters.
 */
struct VoidResult {
  /** @brief Operation status describing success or failure. */
  OperationStatus status;
};

}  // namespace ps
