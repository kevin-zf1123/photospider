/**
 * @file scheduler_worker_limits.hpp
 * @brief Declares bounded scheduler worker-count planning helpers.
 */
#pragma once

#include "photospider/scheduler/scheduler.hpp"

namespace ps {

/**
 * @brief Resolves one configured CPU/plugin worker request to a hard grant.
 * @param configured_workers Zero for automatic selection, otherwise an exact
 * positive request.
 * @param detected_hardware_workers Hardware concurrency observed by the
 * caller; zero denotes an unavailable platform value.
 * @return A nonzero worker grant no greater than
 * `kSchedulerWorkerRequestMax`.
 * @throws std::invalid_argument if `configured_workers` exceeds
 * `kSchedulerWorkerRequestMax`.
 * @note Automatic selection clamps detected hardware into the fixed request
 * domain; this helper owns no worker or resource reservation.
 */
unsigned int resolve_scheduler_worker_count(
    unsigned int configured_workers, unsigned int detected_hardware_workers);

/**
 * @brief Adds scheduler worker-slot components with checked arithmetic.
 * @param current Existing worker-slot component.
 * @param addition Worker-slot component to add.
 * @return Exact sum when representable by `unsigned int`.
 * @throws std::overflow_error if the addition would wrap.
 * @note Planning uses this before ledger admission and type-specific ceiling
 * validation.
 */
unsigned int checked_add_scheduler_worker_slots(unsigned int current,
                                                unsigned int addition);

}  // namespace ps
