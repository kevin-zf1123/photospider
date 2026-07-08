#pragma once

/**
 * @file compute_intent.hpp
 * @brief Stable compute intent values for frontend and scheduler requests.
 *
 * Compute intent is a small value contract shared by host APIs, IPC messages,
 * scheduler selection, and legacy kernel entry points. The enum does not own
 * runtime state and does not expose planner, graph, or scheduler
 * implementation types.
 */

namespace ps {

/**
 * @brief Selects the compute domain requested by a frontend.
 *
 * `GlobalHighPrecision` asks the kernel to produce the authoritative
 * full-resolution output. `RealTimeUpdate` asks the kernel to use the
 * low-latency update path associated with an active dirty ROI.
 *
 * @throws Nothing.
 * @note Values are stable wire/API labels. Adding a value requires updating
 *       host, IPC, scheduler, and compatibility translation code together.
 */
enum class ComputeIntent {
  /** @brief Full-resolution high-precision compute path. */
  GlobalHighPrecision,

  /** @brief Low-latency dirty-region update path. */
  RealTimeUpdate,
};

}  // namespace ps
