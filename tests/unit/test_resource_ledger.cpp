#include <gtest/gtest.h>

#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "runtime/resource_ledger.hpp"
#include "scheduler/scheduler_worker_limits.hpp"

namespace ps {
namespace {

static_assert(!std::is_copy_constructible<ResourceLedger::Reservation>::value,
              "root reservations must not duplicate resource authority");
static_assert(!std::is_copy_assignable<ResourceLedger::Reservation>::value,
              "root reservations must not copy-assign resource authority");
static_assert(
    std::is_nothrow_move_constructible<ResourceLedger::Reservation>::value,
    "reservation moves must preserve rollback safety");
static_assert(
    std::is_nothrow_move_assignable<ResourceLedger::Reservation>::value,
    "reservation assignment must preserve rollback safety");
static_assert(!std::is_copy_constructible<ResourceLedger::Grant>::value,
              "child grants must not duplicate resource authority");
static_assert(!std::is_copy_assignable<ResourceLedger::Grant>::value,
              "child grants must not copy-assign resource authority");
static_assert(std::is_nothrow_move_constructible<ResourceLedger::Grant>::value,
              "grant moves must preserve exact release");
static_assert(std::is_nothrow_move_assignable<ResourceLedger::Grant>::value,
              "grant assignment must preserve exact release");

/**
 * @brief Deterministically releases a fixed set of admission contenders.
 *
 * @throws std::system_error if synchronization primitives fail.
 * @note The gate must outlive every participant.
 */
class ConcurrentAdmissionGate final {
 public:
  /**
   * @brief Creates a closed gate for an exact participant count.
   * @param participants Threads that must arrive before release.
   * @throws Nothing.
   */
  explicit ConcurrentAdmissionGate(std::size_t participants) noexcept
      : participants_(participants) {}

  /** @brief Prevents copying synchronization ownership. */
  ConcurrentAdmissionGate(const ConcurrentAdmissionGate& other) = delete;

  /** @brief Prevents assigning synchronization ownership. */
  ConcurrentAdmissionGate& operator=(const ConcurrentAdmissionGate& other) =
      delete;

  /**
   * @brief Records one participant and waits for controller release.
   * @return Nothing.
   * @throws std::system_error if mutex or condition-variable operations fail.
   */
  void arrive_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++arrived_;
    condition_.notify_all();
    condition_.wait(lock, [this] { return open_; });
  }

  /**
   * @brief Waits for every participant and releases them together.
   * @return Nothing.
   * @throws std::system_error if mutex or condition-variable operations fail.
   */
  void open() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return arrived_ == participants_; });
    open_ = true;
    lock.unlock();
    condition_.notify_all();
  }

 private:
  /** @brief Serializes arrival and release state. */
  std::mutex mutex_;

  /** @brief Wakes the controller and participant threads. */
  std::condition_variable condition_;

  /** @brief Exact number of required participants. */
  const std::size_t participants_;

  /** @brief Participants currently waiting. */
  std::size_t arrived_ = 0U;

  /** @brief True after the controller opens the gate. */
  bool open_ = false;
};

/**
 * @brief Returns a vector with one selected dimension set.
 * @param dimension Zero-based `ResourceVector` field index.
 * @param value Exact selected amount.
 * @return Single-dimension resource vector.
 * @throws std::out_of_range for an invalid dimension.
 */
ResourceVector one_dimension(std::size_t dimension, std::uint64_t value) {
  ResourceVector resources;
  switch (dimension) {
    case 0U:
      resources.cpu_slots = value;
      break;
    case 1U:
      resources.retained_memory_bytes = value;
      break;
    case 2U:
      resources.scratch_bytes = value;
      break;
    case 3U:
      resources.ready_entries = value;
      break;
    case 4U:
      resources.ready_bytes = value;
      break;
    default:
      throw std::out_of_range("invalid test resource dimension");
  }
  return resources;
}

/**
 * @brief Verifies bounded automatic worker resolution remains independent.
 * @throws Nothing when pure planning matches its fixed request contract.
 */
TEST(SchedulerWorkerCountResolution, AutomaticAndExplicitRequestsStayBounded) {
  EXPECT_EQ(resolve_scheduler_worker_count(0U, 0U), 1U);
  EXPECT_EQ(resolve_scheduler_worker_count(0U, 1U), 1U);
  EXPECT_EQ(
      resolve_scheduler_worker_count(0U, kSchedulerWorkerRequestMax + 17U),
      kSchedulerWorkerRequestMax);
  EXPECT_EQ(resolve_scheduler_worker_count(kSchedulerWorkerRequestMax, 1U),
            kSchedulerWorkerRequestMax);
  EXPECT_THROW(
      (void)resolve_scheduler_worker_count(kSchedulerWorkerRequestMax + 1U, 1U),
      std::invalid_argument);
}

/**
 * @brief Proves vector helpers reject overflow without clamping.
 * @throws Nothing when checked operations return no partial result.
 */
TEST(ResourceVectorArithmetic, RejectsAdditionAndMultiplicationOverflow) {
  const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
  EXPECT_FALSE(
      checked_add_resources(ResourceVector{maximum}, ResourceVector{1U})
          .has_value());
  EXPECT_FALSE(
      checked_add_resources(ResourceVector{0U, maximum}, ResourceVector{0U, 1U})
          .has_value());
  EXPECT_FALSE(checked_multiply_resources(ResourceVector{0U, 0U, maximum}, 2U)
                   .has_value());
  const auto exact =
      checked_multiply_resources(ResourceVector{1U, 2U, 3U, 4U, 5U}, 2U);
  ASSERT_TRUE(exact.has_value());
  EXPECT_EQ(*exact, (ResourceVector{2U, 4U, 6U, 8U, 10U}));
}

/**
 * @brief Saturates and recovers every independent ledger dimension.
 * @throws Nothing when rejected vectors leave all root state unchanged.
 */
TEST(ResourceLedgerAdmission, EveryDimensionRejectsAndRecoversExactly) {
  for (std::size_t dimension = 0U; dimension < 5U; ++dimension) {
    ResourceLedger ledger(one_dimension(dimension, 5U));
    auto full = ledger.try_reserve(one_dimension(dimension, 5U));
    ASSERT_TRUE(full.has_value()) << dimension;
    EXPECT_TRUE(full->active()) << dimension;
    EXPECT_FALSE(ledger.try_reserve(one_dimension(dimension, 1U)).has_value())
        << dimension;
    EXPECT_EQ(ledger.snapshot().reserved, one_dimension(dimension, 5U))
        << dimension;
    full.reset();
    EXPECT_EQ(ledger.snapshot().reserved, ResourceVector{}) << dimension;
    EXPECT_TRUE(ledger.try_reserve(one_dimension(dimension, 5U)).has_value())
        << dimension;
  }
}

/**
 * @brief Proves a mixed-vector rejection commits no partial dimension.
 * @throws Nothing when snapshot state remains at the baseline vector.
 */
TEST(ResourceLedgerAdmission, MixedVectorCommitsAllOrNothing) {
  const ResourceVector limits{4U, 8U, 12U, 16U, 20U};
  ResourceLedger ledger(limits);
  auto baseline = ledger.try_reserve(ResourceVector{1U, 2U, 3U, 4U, 5U});
  ASSERT_TRUE(baseline.has_value());
  const ResourceVector before = ledger.snapshot().reserved;
  EXPECT_FALSE(
      ledger.try_reserve(ResourceVector{1U, 1U, 1U, 1U, 16U}).has_value());
  EXPECT_EQ(ledger.snapshot().reserved, before);
}

/**
 * @brief Proves pair admission creates two owners in one root transaction.
 * @throws Nothing when failed and successful pairs preserve exact capacity.
 */
TEST(ResourceLedgerAdmission, PairReservationCommitsBothOrNothing) {
  ResourceLedger ledger(ResourceVector{5U, 5U, 5U, 5U, 5U});
  auto baseline = ledger.try_reserve(ResourceVector{1U, 1U, 1U, 1U, 1U});
  ASSERT_TRUE(baseline.has_value());
  EXPECT_FALSE(ledger
                   .try_reserve_pair(ResourceVector{2U, 2U, 2U, 2U, 2U},
                                     ResourceVector{2U, 2U, 2U, 2U, 3U})
                   .has_value());
  EXPECT_EQ(ledger.snapshot().reserved, (ResourceVector{1U, 1U, 1U, 1U, 1U}));

  baseline.reset();
  auto pair = ledger.try_reserve_pair(ResourceVector{2U, 2U, 2U, 2U, 2U},
                                      ResourceVector{3U, 3U, 3U, 3U, 3U});
  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(ledger.snapshot().reserved, (ResourceVector{5U, 5U, 5U, 5U, 5U}));
  pair.reset();
  EXPECT_EQ(ledger.snapshot().reserved, ResourceVector{});
}

/**
 * @brief Proves child grants cannot exceed or enlarge a reservation.
 * @throws Nothing when failed grants leave availability unchanged.
 */
TEST(ResourceLedgerGrant, ChildGrantIsTransactionalWithinParentVector) {
  const ResourceVector committed{2U, 4U, 6U, 8U, 10U};
  ResourceLedger ledger(committed);
  auto reservation = ledger.try_reserve(committed);
  ASSERT_TRUE(reservation.has_value());

  auto first = reservation->try_grant(ResourceVector{1U, 2U, 3U, 4U, 5U});
  ASSERT_TRUE(first.has_value());
  const ResourceVector remaining{1U, 2U, 3U, 4U, 5U};
  EXPECT_EQ(reservation->available(), remaining);
  EXPECT_FALSE(
      reservation->try_grant(ResourceVector{2U, 0U, 0U, 0U, 0U}).has_value());
  EXPECT_EQ(reservation->available(), remaining);

  first.reset();
  EXPECT_EQ(reservation->available(), committed);
}

/**
 * @brief Proves parent destruction defers root release until the last child.
 * @throws Nothing when root snapshots preserve and then recover exact state.
 */
TEST(ResourceLedgerGrant, ParentReleaseWaitsForEveryOutstandingChild) {
  const ResourceVector committed{2U, 4U, 6U, 8U, 10U};
  ResourceLedger ledger(committed);
  std::optional<ResourceLedger::Grant> first_child;
  std::optional<ResourceLedger::Grant> second_child;
  {
    auto reservation = ledger.try_reserve(committed);
    ASSERT_TRUE(reservation.has_value());
    first_child = reservation->try_grant(ResourceVector{1U, 1U, 1U, 1U, 1U});
    second_child = reservation->try_grant(ResourceVector{1U, 3U, 5U, 7U, 9U});
    ASSERT_TRUE(first_child.has_value());
    ASSERT_TRUE(second_child.has_value());
    reservation.reset();
  }

  EXPECT_EQ(ledger.snapshot().reserved, committed);
  first_child.reset();
  EXPECT_EQ(ledger.snapshot().reserved, committed);
  second_child.reset();
  EXPECT_EQ(ledger.snapshot().reserved, ResourceVector{});
}

/**
 * @brief Proves zero-vector reservations remain active under saturation.
 * @throws Nothing when structural owners consume no numeric capacity.
 */
TEST(ResourceLedgerAdmission, ZeroReservationsRemainActiveWithoutCapacity) {
  ResourceLedger ledger(ResourceVector{1U, 1U, 1U, 1U, 1U});
  auto full = ledger.try_reserve(ResourceVector{1U, 1U, 1U, 1U, 1U});
  ASSERT_TRUE(full.has_value());
  auto zero = ledger.try_reserve(ResourceVector{});
  ASSERT_TRUE(zero.has_value());
  EXPECT_TRUE(zero->active());
  EXPECT_EQ(zero->resources(), ResourceVector{});
  auto zero_pair = ledger.try_reserve_pair(ResourceVector{}, ResourceVector{});
  ASSERT_TRUE(zero_pair.has_value());
  EXPECT_TRUE(zero_pair->first.active());
  EXPECT_TRUE(zero_pair->second.active());
  EXPECT_FALSE(ledger.try_reserve(ResourceVector{1U}).has_value());
}

/**
 * @brief Proves concurrent complete vectors never exceed root capacity.
 * @throws std::system_error if deterministic synchronization fails.
 */
TEST(ResourceLedgerAdmission, ConcurrentVectorsNeverOvercommitAndRecover) {
  constexpr std::size_t kParticipants = 16U;
  const ResourceVector unit{2U, 2U, 2U, 2U, 2U};
  ResourceLedger ledger(ResourceVector{7U, 7U, 7U, 7U, 7U});
  ConcurrentAdmissionGate gate(kParticipants);
  std::vector<std::optional<ResourceLedger::Reservation>> results(
      kParticipants);
  std::vector<std::thread> threads;
  threads.reserve(kParticipants);

  for (std::size_t index = 0U; index < kParticipants; ++index) {
    threads.emplace_back([&ledger, &gate, &results, index, unit] {
      gate.arrive_and_wait();
      results[index] = ledger.try_reserve(unit);
    });
  }
  gate.open();
  for (std::thread& thread : threads) {
    thread.join();
  }

  std::size_t admitted = 0U;
  for (const auto& result : results) {
    if (result.has_value()) {
      ++admitted;
    }
  }
  EXPECT_EQ(admitted, 3U);
  EXPECT_EQ(ledger.snapshot().reserved, (ResourceVector{6U, 6U, 6U, 6U, 6U}));
  auto final = ledger.try_reserve(ResourceVector{1U, 1U, 1U, 1U, 1U});
  ASSERT_TRUE(final.has_value());
  EXPECT_FALSE(ledger.try_reserve(ResourceVector{1U}).has_value());

  final.reset();
  results.clear();
  EXPECT_EQ(ledger.snapshot().reserved, ResourceVector{});
  EXPECT_TRUE(
      ledger.try_reserve(ResourceVector{7U, 7U, 7U, 7U, 7U}).has_value());
}

}  // namespace
}  // namespace ps
