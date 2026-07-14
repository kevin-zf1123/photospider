#include <gtest/gtest.h>

#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "scheduler/scheduler_worker_budget.hpp"

namespace ps {
namespace {

static_assert(
    !std::is_copy_constructible<SchedulerWorkerBudget::Reservation>::value,
    "scheduler worker reservations must not duplicate capacity ownership");
static_assert(
    !std::is_copy_assignable<SchedulerWorkerBudget::Reservation>::value,
    "scheduler worker reservations must not copy-assign capacity ownership");
static_assert(
    std::is_nothrow_move_constructible<
        SchedulerWorkerBudget::Reservation>::value,
    "scheduler worker reservation moves must preserve rollback safety");
static_assert(
    std::is_nothrow_move_assignable<SchedulerWorkerBudget::Reservation>::value,
    "scheduler worker reservation assignment must preserve rollback safety");

/**
 * @brief Deterministically releases a fixed set of reservation contenders.
 *
 * Each worker records its arrival under one mutex and waits on the same
 * condition variable. The test thread opens the gate only after every expected
 * contender has arrived, so the concurrency test requires no timing sleeps.
 *
 * @throws std::system_error If mutex or condition-variable operations fail.
 * @note The gate must outlive every participant thread.
 */
class ConcurrentReservationGate final {
 public:
  /**
   * @brief Creates a closed gate for an exact participant count.
   * @param participants Threads that must arrive before `open()` returns.
   * @throws Nothing.
   */
  explicit ConcurrentReservationGate(unsigned int participants) noexcept
      : participants_(participants) {}

  ConcurrentReservationGate(const ConcurrentReservationGate&) = delete;
  ConcurrentReservationGate& operator=(const ConcurrentReservationGate&) =
      delete;

  /**
   * @brief Records one participant and blocks it until the gate opens.
   * @return Nothing.
   * @throws std::system_error If synchronization primitives fail.
   * @note Each participant thread must call this exactly once.
   */
  void arrive_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++arrived_;
    condition_.notify_all();
    condition_.wait(lock, [this] { return open_; });
  }

  /**
   * @brief Waits for every participant, then releases them together.
   * @return Nothing.
   * @throws std::system_error If synchronization primitives fail.
   * @note The method does not impose an ordering among released contenders.
   */
  void open() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return arrived_ == participants_; });
    open_ = true;
    lock.unlock();
    condition_.notify_all();
  }

 private:
  /** @brief Serializes participant count and release state. */
  std::mutex mutex_;
  /** @brief Wakes the controller and blocked contenders. */
  std::condition_variable condition_;
  /** @brief Exact number of contenders required before release. */
  const unsigned int participants_;
  /** @brief Number of contenders currently waiting at the gate. */
  unsigned int arrived_ = 0U;
  /** @brief True after the controller atomically opens the gate. */
  bool open_ = false;
};

/**
 * @brief Proves an unavailable hardware count resolves to one worker.
 * @throws Nothing when the pure resolver matches its contract.
 */
TEST(SchedulerWorkerCountResolution,
     AutoUsesOneWhenHardwareConcurrencyIsUnknown) {
  EXPECT_EQ(resolve_scheduler_worker_count(0U, 0U), 1U);
}

/**
 * @brief Proves automatic worker resolution remains within the hard ceiling.
 * @throws Nothing when exact and oversized detected values resolve correctly.
 */
TEST(SchedulerWorkerCountResolution,
     AutoClampsDetectedHardwareToInstanceCeiling) {
  EXPECT_EQ(resolve_scheduler_worker_count(0U, 1U), 1U);
  EXPECT_EQ(
      resolve_scheduler_worker_count(0U, kSchedulerWorkerRequestMax + 17U),
      kSchedulerWorkerRequestMax);
}

/**
 * @brief Proves explicit requests are exact and are never silently clamped.
 * @throws Nothing when the expected invalid_argument is observed.
 */
TEST(SchedulerWorkerCountResolution,
     AcceptsExactInstanceCeilingAndRejectsOneAbove) {
  EXPECT_EQ(resolve_scheduler_worker_count(kSchedulerWorkerRequestMax, 1U),
            kSchedulerWorkerRequestMax);
  EXPECT_THROW((void)resolve_scheduler_worker_count(
                   kSchedulerWorkerRequestMax + 1U, kSchedulerWorkerRequestMax),
               std::invalid_argument);
}

/**
 * @brief Fills one small budget and proves destruction restores exact capacity.
 * @throws Nothing when single admission rejects excess and later fully
 * recovers.
 */
TEST(SchedulerWorkerBudgetReservation,
     SingleAdmissionUsesExactCapacityAndRecoversAfterDestruction) {
  SchedulerWorkerBudget budget(5U);
  {
    auto full = budget.try_reserve(5U);
    ASSERT_TRUE(full.has_value());
    EXPECT_TRUE(full->active());
    EXPECT_EQ(full->slots(), 5U);
    EXPECT_FALSE(budget.try_reserve(1U).has_value());
  }

  auto recovered = budget.try_reserve(5U);
  ASSERT_TRUE(recovered.has_value());
  EXPECT_TRUE(recovered->active());
  EXPECT_EQ(recovered->slots(), 5U);
}

/**
 * @brief Proves pair admission commits the complete HP/RT demand or nothing.
 * @throws Nothing when failed and successful pairs preserve exact capacity.
 */
TEST(SchedulerWorkerBudgetReservation, PairAdmissionCommitsBothOrNothing) {
  SchedulerWorkerBudget budget(5U);
  auto baseline = budget.try_reserve(2U);
  ASSERT_TRUE(baseline.has_value());

  EXPECT_FALSE(budget.try_reserve_pair(2U, 2U).has_value());
  auto all_remaining = budget.try_reserve(3U);
  ASSERT_TRUE(all_remaining.has_value());
  EXPECT_FALSE(budget.try_reserve(1U).has_value());
  all_remaining.reset();
  baseline.reset();

  auto pair = budget.try_reserve_pair(2U, 3U);
  ASSERT_TRUE(pair.has_value());
  EXPECT_TRUE(pair->high_precision.active());
  EXPECT_EQ(pair->high_precision.slots(), 2U);
  EXPECT_TRUE(pair->real_time.active());
  EXPECT_EQ(pair->real_time.slots(), 3U);
  EXPECT_FALSE(budget.try_reserve(1U).has_value());

  pair->high_precision = SchedulerWorkerBudget::Reservation{};
  auto hp_replacement = budget.try_reserve(2U);
  ASSERT_TRUE(hp_replacement.has_value());
  pair->real_time = SchedulerWorkerBudget::Reservation{};
  auto rt_replacement = budget.try_reserve(3U);
  ASSERT_TRUE(rt_replacement.has_value());
  EXPECT_FALSE(budget.try_reserve(1U).has_value());
}

/**
 * @brief Proves pair and aggregate arithmetic cannot wrap around capacity.
 * @throws Nothing when overflowing requests fail without consuming capacity.
 */
TEST(SchedulerWorkerBudgetReservation,
     CheckedArithmeticRejectsWrappedPairAndAggregate) {
  const unsigned int maximum = std::numeric_limits<unsigned int>::max();
  SchedulerWorkerBudget small_budget(5U);
  EXPECT_FALSE(small_budget.try_reserve_pair(maximum, 1U).has_value());
  EXPECT_FALSE(small_budget.try_reserve_pair(1U, maximum).has_value());
  EXPECT_TRUE(small_budget.try_reserve(5U).has_value());

  SchedulerWorkerBudget maximum_budget(maximum);
  auto one = maximum_budget.try_reserve(1U);
  ASSERT_TRUE(one.has_value());
  EXPECT_FALSE(maximum_budget.try_reserve(maximum).has_value());
  auto remaining = maximum_budget.try_reserve(maximum - 1U);
  ASSERT_TRUE(remaining.has_value());
  EXPECT_FALSE(maximum_budget.try_reserve(1U).has_value());
}

/**
 * @brief Proves move construction and assignment transfer unique release duty.
 * @throws Nothing when overwritten capacity and final capacity recover exactly.
 */
TEST(SchedulerWorkerBudgetReservation,
     ReservationMoveTransfersUniqueReleaseOwnership) {
  SchedulerWorkerBudget budget(6U);
  auto first = budget.try_reserve(2U);
  auto second = budget.try_reserve(3U);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());

  SchedulerWorkerBudget::Reservation owner(std::move(*first));
  EXPECT_TRUE(owner.active());
  EXPECT_EQ(owner.slots(), 2U);
  EXPECT_FALSE(first->active());
  owner = std::move(*second);
  EXPECT_TRUE(owner.active());
  EXPECT_EQ(owner.slots(), 3U);
  EXPECT_FALSE(second->active());

  auto reclaimed = budget.try_reserve(3U);
  ASSERT_TRUE(reclaimed.has_value());
  EXPECT_FALSE(budget.try_reserve(1U).has_value());
  owner = SchedulerWorkerBudget::Reservation{};
  auto transferred_release = budget.try_reserve(3U);
  ASSERT_TRUE(transferred_release.has_value());
  EXPECT_FALSE(budget.try_reserve(1U).has_value());

  reclaimed.reset();
  transferred_release.reset();
  auto recovered = budget.try_reserve(6U);
  ASSERT_TRUE(recovered.has_value());
}

/**
 * @brief Proves zero-slot reservations remain active without using capacity.
 * @throws Nothing when zero requests succeed even while positive capacity is
 * full.
 */
TEST(SchedulerWorkerBudgetReservation,
     ZeroSlotReservationsRemainValidWithoutConsumingCapacity) {
  SchedulerWorkerBudget budget(3U);
  auto full = budget.try_reserve(3U);
  ASSERT_TRUE(full.has_value());

  auto zero = budget.try_reserve(0U);
  ASSERT_TRUE(zero.has_value());
  EXPECT_TRUE(zero->active());
  EXPECT_EQ(zero->slots(), 0U);
  auto zero_pair = budget.try_reserve_pair(0U, 0U);
  ASSERT_TRUE(zero_pair.has_value());
  EXPECT_TRUE(zero_pair->high_precision.active());
  EXPECT_TRUE(zero_pair->real_time.active());
  EXPECT_FALSE(budget.try_reserve(1U).has_value());

  zero.reset();
  zero_pair.reset();
  EXPECT_FALSE(budget.try_reserve(1U).has_value());
  full.reset();
  EXPECT_TRUE(budget.try_reserve(3U).has_value());
}

/**
 * @brief Proves the migration accessor exposes one fixed process budget.
 * @throws Nothing when both references coordinate the exact capacity of 32.
 */
TEST(SchedulerWorkerBudgetReservation,
     ProcessAccessorUsesOneFixedAggregateState) {
  SchedulerWorkerBudget& first = SchedulerWorkerBudget::process();
  SchedulerWorkerBudget& second = SchedulerWorkerBudget::process();
  EXPECT_EQ(&first, &second);
  EXPECT_EQ(first.limit(), kSchedulerWorkerProcessMax);

  auto full = first.try_reserve(kSchedulerWorkerProcessMax);
  ASSERT_TRUE(full.has_value());
  EXPECT_FALSE(second.try_reserve(1U).has_value());
  full.reset();
  EXPECT_TRUE(second.try_reserve(kSchedulerWorkerProcessMax).has_value());
}

/**
 * @brief Proves simultaneous contenders never admit more than fixed capacity.
 * @throws std::system_error If deterministic test synchronization fails.
 */
TEST(SchedulerWorkerBudgetReservation,
     ConcurrentAdmissionNeverExceedsCapacityAndFullyRecovers) {
  constexpr unsigned int kParticipants = 16U;
  SchedulerWorkerBudget budget(7U);
  ConcurrentReservationGate gate(kParticipants);
  std::vector<std::optional<SchedulerWorkerBudget::Reservation>> results(
      kParticipants);
  std::vector<std::thread> threads;
  threads.reserve(kParticipants);

  for (unsigned int index = 0U; index < kParticipants; ++index) {
    threads.emplace_back([&budget, &gate, &results, index] {
      gate.arrive_and_wait();
      results[index] = budget.try_reserve(2U);
    });
  }
  gate.open();
  for (std::thread& thread : threads) {
    thread.join();
  }

  unsigned int admitted = 0U;
  for (const auto& result : results) {
    if (result.has_value()) {
      ++admitted;
      EXPECT_TRUE(result->active());
      EXPECT_EQ(result->slots(), 2U);
    }
  }
  EXPECT_EQ(admitted, 3U);
  auto final_slot = budget.try_reserve(1U);
  ASSERT_TRUE(final_slot.has_value());
  EXPECT_FALSE(budget.try_reserve(1U).has_value());

  final_slot.reset();
  results.clear();
  auto recovered = budget.try_reserve(7U);
  ASSERT_TRUE(recovered.has_value());
}

}  // namespace
}  // namespace ps
