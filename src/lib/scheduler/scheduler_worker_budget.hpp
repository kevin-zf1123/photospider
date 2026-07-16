#pragma once

#include <memory>
#include <optional>

#include "photospider/scheduler/scheduler.hpp"

/**
 * @file scheduler_worker_budget.hpp
 * @brief Declares bounded scheduler worker planning and process admission.
 */

namespace ps {

/**
 * @brief Resolves one configured CPU/plugin worker request to a hard grant.
 *
 * @param configured_workers Zero for automatic selection, otherwise an exact
 *        positive request.
 * @param detected_hardware_workers Hardware concurrency observed by the
 *        caller; zero denotes an unavailable platform value.
 * @return A nonzero worker grant no greater than
 *         `kSchedulerWorkerRequestMax`.
 * @throws std::invalid_argument if `configured_workers` exceeds
 *         `kSchedulerWorkerRequestMax`.
 * @note Automatic selection clamps the detected value into
 *       `1..kSchedulerWorkerRequestMax`; explicit legal values remain exact.
 */
unsigned int resolve_scheduler_worker_count(
    unsigned int configured_workers, unsigned int detected_hardware_workers);

/**
 * @brief Adds scheduler worker-slot components with checked arithmetic.
 * @param current Existing worker-slot component.
 * @param addition Worker-slot component to add.
 * @return Exact sum when representable by `unsigned int`.
 * @throws std::overflow_error If the addition would wrap.
 * @note Planning uses this helper before comparing the result with its
 * type-specific or process ceiling.
 */
unsigned int checked_add_scheduler_worker_slots(unsigned int current,
                                                unsigned int addition);

/**
 * @brief Owns transitional process-wide scheduler-worker admission state.
 *
 * The budget serializes fixed-capacity reservations shared by independently
 * constructed Hosts and Kernels. Test code may construct a small private
 * budget; production code uses only `process()` and its fixed limit of
 * `kSchedulerWorkerProcessMax`.
 *
 * @throws std::bad_alloc If shared admission-state allocation fails.
 * @note This migration-only owner is not public ABI and does not replace the
 * explicit, injected, non-singleton ExecutionService defined by ADR 0003.
 */
class SchedulerWorkerBudget final {
 private:
  /** @brief Mutex-protected aggregate state hidden from callers. */
  struct State;

 public:
  /**
   * @brief Move-only RAII ownership of one admitted worker-slot count.
   *
   * @throws Nothing while moved or destroyed.
   * @note Destruction releases the exact owned count once. Shared state remains
   * alive until every issued reservation has been released.
   */
  class Reservation final {
   public:
    /** @brief Creates an inactive reservation. @throws Nothing. */
    Reservation() noexcept = default;

    /** @brief Releases active capacity exactly once. @throws Nothing. */
    ~Reservation() noexcept;

    /**
     * @brief Prevents duplicate capacity ownership.
     * @param other Reservation that remains the sole owner.
     * @throws Nothing because this operation is unavailable.
     */
    Reservation(const Reservation&) = delete;

    /**
     * @brief Prevents copy-assignment of capacity ownership.
     * @return No value because this operation is unavailable.
     * @throws Nothing because this operation is unavailable.
     */
    Reservation& operator=(const Reservation&) = delete;

    /**
     * @brief Transfers capacity ownership and deactivates the source.
     * @param other Source reservation made inactive.
     * @throws Nothing.
     */
    Reservation(Reservation&& other) noexcept;

    /**
     * @brief Releases current ownership, then transfers another reservation.
     * @param other Source reservation made inactive.
     * @return This reservation after transfer.
     * @throws Nothing.
     */
    Reservation& operator=(Reservation&& other) noexcept;

    /**
     * @brief Reports whether this value owns an admitted reservation.
     * @return True for active positive or zero-slot reservations.
     * @throws Nothing.
     */
    bool active() const noexcept;

    /**
     * @brief Reports the exact worker slots owned by this value.
     * @return Owned slot count, or zero when inactive.
     * @throws Nothing.
     */
    unsigned int slots() const noexcept;

   private:
    friend class SchedulerWorkerBudget;

    /**
     * @brief Creates one active reservation against shared state.
     * @param state Shared aggregate state whose total already includes slots.
     * @param slots Exact count released at destruction.
     * @throws Nothing.
     */
    Reservation(std::shared_ptr<State> state, unsigned int slots) noexcept;

    /**
     * @brief Releases active capacity and makes this value inactive.
     * @return Nothing.
     * @throws Nothing; mutex failures terminate at this noexcept boundary.
     * @note Positive and zero-slot owners both discard shared-state ownership.
     */
    void release() noexcept;

    /** @brief Shared aggregate state retained while this value is active. */
    std::shared_ptr<State> state_;

    /** @brief Exact admitted count released with `state_`. */
    unsigned int slots_ = 0U;
  };

  /**
   * @brief Atomic HP/RT reservation ownership returned by pair admission.
   *
   * @throws Nothing while moved when Reservation moves remain non-throwing.
   * @note Each intent owns an independently movable share of one atomic commit.
   */
  struct ReservationPair {
    /** @brief High-precision intent reservation. */
    Reservation high_precision;
    /** @brief Real-time intent reservation. */
    Reservation real_time;
  };

  /**
   * @brief Creates one isolated budget, primarily for deterministic tests.
   * @param limit Fixed maximum admitted worker slots for this state.
   * @throws std::bad_alloc If shared state allocation fails.
   * @note Production must use `process()` rather than constructing another
   * budget, so independent Hosts share the required process aggregate.
   */
  explicit SchedulerWorkerBudget(unsigned int limit);

  /**
   * @brief Prevents accidentally duplicating one budget facade.
   * @param other Facade that retains the sole process-state reference.
   * @throws Nothing because this operation is unavailable.
   */
  SchedulerWorkerBudget(const SchedulerWorkerBudget&) = delete;

  /**
   * @brief Prevents replacing one budget facade's shared state.
   * @param other Facade that remains unchanged.
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  SchedulerWorkerBudget& operator=(const SchedulerWorkerBudget&) = delete;

  /**
   * @brief Prevents moving the process facade while callers borrow it.
   * @param other Facade that remains at its stable address.
   * @throws Nothing because this operation is unavailable.
   */
  SchedulerWorkerBudget(SchedulerWorkerBudget&&) = delete;

  /**
   * @brief Prevents move-assignment of one process facade.
   * @param other Facade that remains at its stable address.
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  SchedulerWorkerBudget& operator=(SchedulerWorkerBudget&&) = delete;

  /**
   * @brief Returns the transitional process-wide admission owner.
   * @return Stable borrowed facade with limit `kSchedulerWorkerProcessMax`.
   * @throws std::bad_alloc On the first call if shared-state allocation fails.
   * @note This singleton is migration-only and shall be removed when the
   * explicitly injected ExecutionService owns admission.
   */
  static SchedulerWorkerBudget& process();

  /**
   * @brief Attempts to reserve one exact scheduler worker-slot count.
   * @param slots Requested count; zero is a valid no-op reservation.
   * @return Active reservation on success, or `std::nullopt` when the request
   * cannot fit without overflow or exceeding the fixed limit.
   * @throws std::system_error If mutex locking fails.
   * @note Validation and aggregate commit occur while holding one state mutex.
   * Admission never waits, queues, or promises ordering/fairness.
   */
  std::optional<Reservation> try_reserve(unsigned int slots);

  /**
   * @brief Atomically reserves complete HP and RT scheduler plans.
   * @param high_precision_slots Exact HP worker-slot demand.
   * @param real_time_slots Exact RT worker-slot demand.
   * @return Two active reservations on success, or `std::nullopt` without
   * partial mutation when checked addition or remaining capacity rejects the
   * complete pair.
   * @throws std::system_error If mutex locking fails.
   * @note Both counts are validated and committed under one state mutex.
   * Admission never waits, queues, or promises ordering/fairness.
   */
  std::optional<ReservationPair> try_reserve_pair(
      unsigned int high_precision_slots, unsigned int real_time_slots);

  /**
   * @brief Returns the immutable capacity of this budget.
   * @return Fixed worker-slot limit.
   * @throws Nothing.
   */
  unsigned int limit() const noexcept;

 private:
  /** @brief Shared state retained by this facade and every reservation. */
  std::shared_ptr<State> state_;
};

}  // namespace ps
