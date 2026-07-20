/**
 * @file scheduler_worker_limits.cpp
 * @brief Implements bounded scheduler worker-count planning helpers.
 */

#include "scheduler/scheduler_worker_limits.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ps {

/** @copydoc resolve_scheduler_worker_count */
unsigned int resolve_scheduler_worker_count(
    unsigned int configured_workers, unsigned int detected_hardware_workers) {
  if (configured_workers > kSchedulerWorkerRequestMax) {
    throw std::invalid_argument(
        "scheduler worker count exceeds the per-scheduler request maximum");
  }
  if (configured_workers != 0U) {
    return configured_workers;
  }
  return std::max(
      1U, std::min(detected_hardware_workers, kSchedulerWorkerRequestMax));
}

/** @copydoc checked_add_scheduler_worker_slots */
unsigned int checked_add_scheduler_worker_slots(unsigned int current,
                                                unsigned int addition) {
  if (current > std::numeric_limits<unsigned int>::max() - addition) {
    throw std::overflow_error("scheduler worker-slot addition overflowed");
  }
  return current + addition;
}

}  // namespace ps
