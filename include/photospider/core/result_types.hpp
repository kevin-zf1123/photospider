#pragma once

#include <cstdint>
#include <optional>
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
 * @brief Stable source domain for a completed-operation failure.
 *
 * The domain separates local transport and protocol failures from backend
 * graph failures and daemon invariant failures. Numeric codes are interpreted
 * only within their domain.
 *
 * @throws Nothing.
 * @note Successful operations always use `None`; callers must not infer a
 *       domain from the diagnostic message.
 */
enum class OperationErrorDomain {
  /** @brief No failure occurred. */
  None,

  /** @brief Local connection, framing, read, write, or peer failure. */
  Transport,

  /** @brief Local or remote typed-protocol contract failure. */
  Protocol,

  /** @brief Failure reported by the public graph Host boundary. */
  Graph,

  /** @brief Daemon lifecycle, invariant, or orchestration failure. */
  Daemon,
};

/**
 * @brief Returns the stable lowercase name for one graph error code.
 *
 * @param code Public graph error code to name.
 * @return Stable lowercase identifier used by statuses and IPC payloads.
 * @throws Nothing.
 * @note Every current `GraphErrc` value is handled explicitly. An unknown
 *       future numeric value defensively maps to `unknown` until the public
 *       vocabulary is extended.
 */
inline constexpr const char* graph_error_stable_name(GraphErrc code) noexcept {
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
 * @brief Domain-complete status value for a completed operation.
 *
 * The status is the sole public failure representation shared by embedded
 * Host and IPC products. Programmatic consumers branch on `ok`, `domain`, and
 * `code`; `name` is a stable lowercase identifier where the owning domain
 * defines one, while `message` is diagnostic text.
 *
 * @throws std::bad_alloc when diagnostic string construction, copy, or
 * mutation exhausts memory.
 * @note Successful statuses have the canonical shape `ok=true`, `domain=None`,
 *       `code=0`, and empty `name`/`message`. Current embedded-Host Graph
 *       failures carry the numeric `GraphErrc` value and
 *       `graph_error_stable_name(code)`. IPC-decoded unknown future Graph
 *       code/name pairs preserve the received numeric code and name unchanged.
 */
struct OperationStatus {
  /** @brief True when the operation completed successfully. */
  bool ok = true;

  /** @brief Stable failure source, or `None` for canonical success. */
  OperationErrorDomain domain = OperationErrorDomain::None;

  /** @brief Signed domain-specific code, or zero for canonical success. */
  std::int32_t code = 0;

  /** @brief Stable lowercase domain-specific name, empty on success. */
  std::string name;

  /** @brief Human-readable diagnostic text for logs or UI display. */
  std::string message;
};

/**
 * @brief Validates and converts a failed Graph-domain status code.
 *
 * @param status Completed-operation status to inspect.
 * @return The exact `GraphErrc` for a failed Graph status whose numeric code is
 *         one of the nine public values; otherwise `std::nullopt`.
 * @throws Nothing.
 * @note This helper checks `ok`, `domain`, and every recognized numeric value
 *       before conversion, preventing non-Graph domains from being interpreted
 *       as graph errors merely because their integer codes overlap.
 */
inline constexpr std::optional<GraphErrc> checked_graph_error_code(
    const OperationStatus& status) noexcept {
  if (status.ok || status.domain != OperationErrorDomain::Graph) {
    return std::nullopt;
  }
  switch (status.code) {
    case static_cast<std::int32_t>(GraphErrc::Unknown):
      return GraphErrc::Unknown;
    case static_cast<std::int32_t>(GraphErrc::NotFound):
      return GraphErrc::NotFound;
    case static_cast<std::int32_t>(GraphErrc::Cycle):
      return GraphErrc::Cycle;
    case static_cast<std::int32_t>(GraphErrc::Io):
      return GraphErrc::Io;
    case static_cast<std::int32_t>(GraphErrc::InvalidYaml):
      return GraphErrc::InvalidYaml;
    case static_cast<std::int32_t>(GraphErrc::MissingDependency):
      return GraphErrc::MissingDependency;
    case static_cast<std::int32_t>(GraphErrc::NoOperation):
      return GraphErrc::NoOperation;
    case static_cast<std::int32_t>(GraphErrc::InvalidParameter):
      return GraphErrc::InvalidParameter;
    case static_cast<std::int32_t>(GraphErrc::ComputeError):
      return GraphErrc::ComputeError;
    default:
      return std::nullopt;
  }
}

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
