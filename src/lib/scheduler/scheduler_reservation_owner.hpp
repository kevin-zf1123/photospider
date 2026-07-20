#pragma once

#include <memory>

#include "photospider/scheduler/scheduler.hpp"
#include "runtime/resource_ledger.hpp"

/**
 * @file scheduler_reservation_owner.hpp
 * @brief Declares private scheduler/reservation lifetime composition.
 */

namespace ps {

/**
 * @brief Composes one concrete scheduler with its admitted worker reservation.
 *
 * The returned scheduler transparently delegates the complete inherited
 * lifecycle and task-runtime contract. Destroying it destroys the concrete
 * scheduler, including any plugin destroy/DSO owner chain, before releasing
 * the reservation.
 *
 * @param scheduler Concrete host or plugin-owned scheduler; null is accepted
 *        as a failed candidate construction.
 * @param reservation Active reservation associated with the candidate.
 * @return Transparent owner, or nullptr when `scheduler` is null.
 * @throws std::invalid_argument If `reservation` is inactive for a non-null
 * scheduler.
 * @throws std::bad_alloc If transparent owner allocation fails.
 * @note Explicit lifecycle calls and their exceptions are forwarded unchanged.
 * On allocation failure the concrete scheduler is destroyed before the local
 * reservation releases.
 */
std::unique_ptr<IScheduler> make_reservation_owned_scheduler(
    std::unique_ptr<IScheduler> scheduler,
    ResourceLedger::Reservation reservation);

}  // namespace ps
