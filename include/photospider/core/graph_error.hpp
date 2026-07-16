#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include "photospider/core/export.hpp"

/**
 * @file graph_error.hpp
 * @brief Stable graph error code and exception values.
 *
 * The declarations in this header are the public error vocabulary for
 * Photospider graph, compute, IO, and plugin boundaries. They intentionally do
 * not include graph model, runtime, scheduler, or service implementation
 * headers.
 */

namespace ps {

/**
 * @brief Stable category for graph and compute failures.
 *
 * These codes describe the reason a request failed without exposing the
 * throwing service or internal implementation type. They are suitable for
 * public result objects, exceptions, logs, and future IPC error payloads.
 *
 * @throws Nothing.
 * @note Preserve numeric meaning when adding values because downstream
 *       frontends may serialize the enum.
 */
enum class GraphErrc : std::uint32_t {
  /** @brief Failure did not map to a more specific category. */
  Unknown = 1U,

  /** @brief Requested graph, node, file, plugin, or resource was not found. */
  NotFound = 2U,

  /** @brief Graph topology contains a cycle where an acyclic graph is needed.
   */
  Cycle = 3U,

  /** @brief Filesystem, cache, or external IO operation failed. */
  Io = 4U,

  /** @brief YAML text or node structure is invalid for the requested action. */
  InvalidYaml = 5U,

  /** @brief A graph dependency is missing or cannot be resolved. */
  MissingDependency = 6U,

  /** @brief No operation implementation is registered for a node. */
  NoOperation = 7U,

  /** @brief A request parameter or node parameter is invalid. */
  InvalidParameter = 8U,

  /** @brief Compute planning, dispatch, or execution failed. */
  ComputeError = 9U,
};

/**
 * @brief Exception carrying a stable graph error code.
 *
 * GraphError is used at internal service boundaries and may cross a controlled
 * scheduler call when it is an already host-owned task failure. A
 * DSO-origin `GraphError` never escapes an unloadable plugin object: the host
 * copies its code and message into a new host-owned `GraphError` while the
 * plugin library lease is still live. The object owns its diagnostic message
 * through `std::runtime_error` and stores only a small stable code in addition
 * to that message.
 *
 * @throws std::bad_alloc if copying the diagnostic message into the exception
 *         object allocates and the allocation fails.
 * @note Frontend-facing APIs may convert this exception into `OperationStatus`
 *       or another non-throwing result value. Plugin authors must not depend on
 *       exception object identity surviving a host ABI boundary.
 */
struct PHOTOSPIDER_API GraphError : public std::runtime_error {
  /**
   * @brief Creates an unknown graph error with diagnostic text.
   *
   * @param what Human-readable diagnostic message.
   * @throws std::bad_alloc if storing `what` allocates and fails.
   * @note The resulting code is `GraphErrc::Unknown`.
   */
  explicit GraphError(const std::string& what)
      : std::runtime_error(what), code_(GraphErrc::Unknown) {}

  /**
   * @brief Creates a graph error with a stable category and diagnostic text.
   *
   * @param code Stable failure category.
   * @param what Human-readable diagnostic message.
   * @throws std::bad_alloc if storing `what` allocates and fails.
   * @note The message is intended for diagnostics, not for programmatic
   *       branching; use `code()` for branching.
   */
  GraphError(GraphErrc code, const std::string& what)
      : std::runtime_error(what), code_(code) {}

  /**
   * @brief Returns the stable failure category.
   *
   * @return Error code captured when the exception was created.
   * @throws Nothing.
   * @note The returned value remains valid for the exception lifetime.
   */
  GraphErrc code() const noexcept { return code_; }

 private:
  /** @brief Stable error code associated with the diagnostic message. */
  GraphErrc code_;
};

}  // namespace ps
