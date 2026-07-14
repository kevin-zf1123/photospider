/**
 * @file scheduler_worker_budget.cpp
 * @brief Implements bounded scheduler worker planning and process admission.
 */

#include "scheduler/scheduler_worker_budget.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace ps {

/**
 * @brief Mutex-protected aggregate retained by the process facade and owners.
 *
 * @throws Nothing for direct construction.
 * @note `reserved` is accessed only while `mutex` is held and always remains
 * less than or equal to immutable `limit`.
 */
struct SchedulerWorkerBudget::State final {
  /**
   * @brief Creates empty admission state with one immutable capacity.
   * @param worker_limit Exact maximum aggregate worker slots.
   * @throws Nothing.
   */
  explicit State(unsigned int worker_limit) noexcept : limit(worker_limit) {}

  /** @brief Serializes every aggregate check, commit, and release. */
  std::mutex mutex;
  /** @brief Immutable maximum aggregate admitted worker slots. */
  const unsigned int limit;
  /** @brief Aggregate slots retained by all active reservations. */
  unsigned int reserved = 0U;
};

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

/** @copydoc SchedulerWorkerBudget::Reservation::Reservation */
SchedulerWorkerBudget::Reservation::Reservation(std::shared_ptr<State> state,
                                                unsigned int slots) noexcept
    : state_(std::move(state)), slots_(slots) {}  // NOLINT

/** @copydoc SchedulerWorkerBudget::Reservation::~Reservation */
SchedulerWorkerBudget::Reservation::~Reservation() noexcept {
  release();
}

/** @copydoc SchedulerWorkerBudget::Reservation::Reservation */
SchedulerWorkerBudget::Reservation::Reservation(Reservation&& other) noexcept
    : state_(std::move(other.state_)),            // NOLINT
      slots_(std::exchange(other.slots_, 0U)) {}  // NOLINT

/** @copydoc SchedulerWorkerBudget::Reservation::operator= */
SchedulerWorkerBudget::Reservation&
SchedulerWorkerBudget::Reservation::operator=(Reservation&& other) noexcept {
  if (this != &other) {
    release();
    state_ = std::move(other.state_);
    slots_ = std::exchange(other.slots_, 0U);
  }
  return *this;
}

/** @copydoc SchedulerWorkerBudget::Reservation::active */
bool SchedulerWorkerBudget::Reservation::active() const noexcept {
  return state_ != nullptr;
}

/** @copydoc SchedulerWorkerBudget::Reservation::slots */
unsigned int SchedulerWorkerBudget::Reservation::slots() const noexcept {
  return active() ? slots_ : 0U;
}

/** @copydoc SchedulerWorkerBudget::Reservation::release */
void SchedulerWorkerBudget::Reservation::release() noexcept {
  std::shared_ptr<State> state = std::move(state_);
  const unsigned int slots = std::exchange(slots_, 0U);
  if (state == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(state->mutex);
  assert(slots <= state->reserved);
  state->reserved -= slots;
}

/** @copydoc SchedulerWorkerBudget::SchedulerWorkerBudget */
SchedulerWorkerBudget::SchedulerWorkerBudget(unsigned int limit)
    : state_(std::make_shared<State>(limit)) {}

/** @copydoc SchedulerWorkerBudget::process */
SchedulerWorkerBudget& SchedulerWorkerBudget::process() {
  static SchedulerWorkerBudget budget(kSchedulerWorkerProcessMax);
  return budget;
}

/** @copydoc SchedulerWorkerBudget::try_reserve */
std::optional<SchedulerWorkerBudget::Reservation>
SchedulerWorkerBudget::try_reserve(unsigned int slots) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (slots > state_->limit - state_->reserved) {
    return std::nullopt;
  }
  state_->reserved += slots;
  Reservation reservation(state_, slots);
  return std::optional<Reservation>(std::move(reservation));
}

/** @copydoc SchedulerWorkerBudget::try_reserve_pair */
std::optional<SchedulerWorkerBudget::ReservationPair>
SchedulerWorkerBudget::try_reserve_pair(unsigned int high_precision_slots,
                                        unsigned int real_time_slots) {
  if (high_precision_slots >
      std::numeric_limits<unsigned int>::max() - real_time_slots) {
    return std::nullopt;
  }
  const unsigned int total = high_precision_slots + real_time_slots;

  std::lock_guard<std::mutex> lock(state_->mutex);
  if (total > state_->limit - state_->reserved) {
    return std::nullopt;
  }
  state_->reserved += total;
  ReservationPair pair{Reservation(state_, high_precision_slots),
                       Reservation(state_, real_time_slots)};
  return std::optional<ReservationPair>(std::move(pair));
}

/** @copydoc SchedulerWorkerBudget::limit */
unsigned int SchedulerWorkerBudget::limit() const noexcept {
  return state_->limit;
}

}  // namespace ps
