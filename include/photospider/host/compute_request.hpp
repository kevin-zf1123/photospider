#pragma once

#include <optional>
#include <string>

#include "photospider/core/compute_intent.hpp"
#include "photospider/core/geometry.hpp"
#include "photospider/core/inspection_types.hpp"

/**
 * @file compute_request.hpp
 * @brief Public Host compute request values.
 *
 * Host compute requests mirror the existing compute capabilities while
 * remaining independent from backend request objects, image-library
 * rectangles, benchmark collectors, and runtime ownership.
 */

namespace ps {

/**
 * @brief Cache and persistence controls for one Host compute request.
 *
 * @throws Nothing for value operations except string allocation on mutation.
 * @note The embedded adapter translates these values into backend cache
 *       options for one request and does not retain the request object.
 */
struct HostComputeCacheOptions {
  /** @brief Cache precision label passed to backend cache paths. */
  std::string precision;

  /** @brief Whether selected in-memory caches should be recomputed. */
  bool force_recache = false;

  /** @brief Whether disk-cache reads are disabled for this request. */
  bool disable_disk_cache = false;

  /** @brief Whether this request skips saving cache outputs. */
  bool nosave = false;
};

/**
 * @brief Execution controls for one Host compute request.
 *
 * @throws Nothing.
 * @note `parallel` selects the scheduler-backed path where supported, while
 *       `quiet` suppresses graph output for this request only.
 */
struct HostComputeExecutionOptions {
  /** @brief Whether scheduler-backed execution should be requested. */
  bool parallel = false;

  /** @brief Whether graph output should be quiet during this request. */
  bool quiet = false;
};

/**
 * @brief Telemetry controls for one Host compute request.
 *
 * @throws Nothing.
 * @note Host telemetry intentionally omits borrowed benchmark sinks so the
 *       public request can be safely copied across adapter and IPC boundaries.
 */
struct HostComputeTelemetryOptions {
  /** @brief Whether backend node timing should be collected. */
  bool enable_timing = false;
};

/**
 * @brief Frontend-facing compute request accepted by Host implementations.
 *
 * @throws Nothing for value operations except string allocation on mutation.
 * @note A missing `intent` selects the legacy HP path. A populated intent and
 *       optional `dirty_roi` are translated into the intent-aware compute path
 *       by the embedded adapter.
 */
struct HostComputeRequest {
  /** @brief Graph session to compute. */
  GraphSessionId session;

  /** @brief Target node to compute. */
  NodeId node;

  /** @brief Cache precision and persistence controls. */
  HostComputeCacheOptions cache;

  /** @brief Scheduler and quiet-mode controls. */
  HostComputeExecutionOptions execution;

  /** @brief Timing and telemetry controls. */
  HostComputeTelemetryOptions telemetry;

  /** @brief Optional compute intent for HP/RT coordinated paths. */
  std::optional<ComputeIntent> intent;

  /** @brief Optional HP-space dirty ROI for dirty HP or RT updates. */
  std::optional<PixelRect> dirty_roi;
};

}  // namespace ps
