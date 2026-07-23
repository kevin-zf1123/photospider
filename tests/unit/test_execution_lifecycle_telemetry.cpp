#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#include "compute/execution_lifecycle_telemetry.hpp"

namespace allocation_probe {

/** @brief Disabled allocation-failure sentinel. */
constexpr std::int64_t kDisabled = -1;

/** @brief Current-thread allocation countdown. */
thread_local std::int64_t countdown = kDisabled;

/** @brief Whether the armed allocation failure fired. */
thread_local bool fired = false;

/**
 * @brief Arms one current-thread allocation failure.
 * @param allocation_index Zero-based allocation index that must fail.
 * @return Nothing.
 * @throws Nothing.
 */
void arm(std::int64_t allocation_index) noexcept {
  countdown = allocation_index;
  fired = false;
}

/**
 * @brief Restores normal current-thread allocation.
 * @return Nothing.
 * @throws Nothing.
 */
void disarm() noexcept {
  countdown = kDisabled;
}

/**
 * @brief Reports whether the armed probe fired.
 * @return True only after one injected std::bad_alloc.
 * @throws Nothing.
 */
bool did_fire() noexcept {
  return fired;
}

/**
 * @brief Applies one allocation-failure decision.
 * @return Nothing.
 * @throws std::bad_alloc when the countdown reaches zero.
 */
void maybe_fail() {
  if (countdown < 0) {
    return;
  }
  if (countdown == 0) {
    countdown = kDisabled;
    fired = true;
    throw std::bad_alloc{};
  }
  --countdown;
}

}  // namespace allocation_probe

/**
 * @brief Test-executable allocation operator with one-shot injection.
 * @param size Requested byte count.
 * @return malloc-compatible storage.
 * @throws std::bad_alloc when injected or allocation fails.
 */
void* operator new(std::size_t size) {
  allocation_probe::maybe_fail();
  if (void* memory = std::malloc(size == 0U ? 1U : size)) {
    return memory;
  }
  throw std::bad_alloc{};
}

/**
 * @brief Array allocation counterpart for the test executable.
 * @param size Requested byte count.
 * @return malloc-compatible storage.
 * @throws std::bad_alloc when injected or allocation fails.
 */
void* operator new[](std::size_t size) {
  return ::operator new(size);
}

/**
 * @brief Releases scalar storage from the test allocation operator.
 * @param memory Nullable storage.
 * @return Nothing.
 * @throws Nothing.
 */
void operator delete(void* memory) noexcept {
  std::free(memory);
}

/**
 * @brief Releases array storage from the test allocation operator.
 * @param memory Nullable storage.
 * @return Nothing.
 * @throws Nothing.
 */
void operator delete[](void* memory) noexcept {
  std::free(memory);
}

/**
 * @brief Sized scalar delete required by C++17 implementations.
 * @param memory Nullable storage.
 * @param size Original byte count, unused by free().
 * @return Nothing.
 * @throws Nothing.
 */
void operator delete(void* memory, std::size_t size) noexcept {
  (void)size;
  std::free(memory);
}

/**
 * @brief Sized array delete required by C++17 implementations.
 * @param memory Nullable storage.
 * @param size Original byte count, unused by free().
 * @return Nothing.
 * @throws Nothing.
 */
void operator delete[](void* memory, std::size_t size) noexcept {
  (void)size;
  std::free(memory);
}

namespace ps::testing {

/**
 * @brief Friend bridge for deterministic telemetry sequence/time boundaries.
 *
 * @note The bridge changes only isolated unit-test stores before concurrent
 * access begins. It is not linked into the installed product.
 */
class ExecutionLifecycleTelemetryTestAccess final {
 public:
  /**
   * @brief Publishes one ordinary event at an explicit steady duration.
   * @param telemetry Isolated test store.
   * @param elapsed Explicit duration since its origin.
   * @return Assigned sequence.
   * @throws Telemetry publication exceptions unchanged.
   */
  static std::uint64_t publish_at(
      compute::ExecutionLifecycleTelemetry& telemetry,
      std::chrono::steady_clock::duration elapsed) {
    return telemetry.publish_at(
        compute::ExecutionLifecycleEventKind::CandidateBegan,
        compute::ExecutionLifecycleCategory::None, 1U, 0U, 0U, 0U, {}, elapsed,
        false);
  }

  /**
   * @brief Selects the next sequence for exhaustion-boundary testing.
   * @param telemetry Empty isolated test store.
   * @param next_sequence Exact next value.
   * @return Nothing.
   * @throws std::system_error when locking fails.
   */
  static void set_next_sequence(compute::ExecutionLifecycleTelemetry& telemetry,
                                std::uint64_t next_sequence) {
    std::lock_guard<std::mutex> lock(telemetry.mutex_);
    telemetry.next_sequence_ = next_sequence;
  }

  /**
   * @brief Selects the cumulative drop state for saturation testing.
   * @param telemetry Empty isolated test store.
   * @param total Exact saturated numeric value.
   * @param saturated Sticky lower-bound flag.
   * @return Nothing.
   * @throws std::system_error when locking fails.
   */
  static void set_drop_state(compute::ExecutionLifecycleTelemetry& telemetry,
                             std::uint64_t total, bool saturated) {
    std::lock_guard<std::mutex> lock(telemetry.mutex_);
    telemetry.global_dropped_total_ = total;
    telemetry.global_dropped_saturated_ = saturated;
  }

  /**
   * @brief Records one synthetic lost publication.
   * @param telemetry Isolated test store.
   * @return Nothing.
   * @throws std::system_error when locking fails.
   */
  static void record_drop(compute::ExecutionLifecycleTelemetry& telemetry) {
    std::lock_guard<std::mutex> lock(telemetry.mutex_);
    telemetry.record_drop_locked();
  }
};

}  // namespace ps::testing

namespace ps::compute {
namespace {

static_assert(kExecutionLifecycleTelemetrySchemaVersion == 1U);
static_assert(kExecutionLifecycleTelemetryCapacity == 65536U);
static_assert(kExecutionLifecycleTelemetryMinPageSize == 1U);
static_assert(kExecutionLifecycleTelemetryMaxPageSize == 4096U);
static_assert(static_cast<std::uint16_t>(
                  ExecutionLifecycleServiceState::Accepting) == 1U);
static_assert(
    static_cast<std::uint16_t>(ExecutionLifecycleServiceState::Stopping) == 2U);
static_assert(
    static_cast<std::uint16_t>(ExecutionLifecycleServiceState::Stopped) == 3U);
static_assert(static_cast<std::uint16_t>(
                  ExecutionLifecycleEventKind::ServiceStopped) == 15U);
static_assert(
    static_cast<std::uint16_t>(ExecutionLifecycleCategory::FailureOther) == 9U);

/**
 * @brief Publishes one ordinary default-counter event.
 * @param telemetry Target isolated store.
 * @return Assigned sequence.
 * @throws Telemetry publication exceptions unchanged.
 */
std::uint64_t publish_event(ExecutionLifecycleTelemetry& telemetry) {
  return telemetry.publish(ExecutionLifecycleEventKind::CandidateBegan,
                           ExecutionLifecycleCategory::None, 1U, 0U, 0U, 0U,
                           {});
}

/**
 * @brief Verifies distinct service identities and every physical counter.
 *
 * @throws Telemetry construction/snapshot failures unchanged.
 * @note Registry-derived counters remain zero; the six trusted physical
 * selectors appear in snapshots between events and retire exactly to zero.
 */
TEST(ExecutionLifecycleTelemetry,
     MintsDistinctEpochsAndCopiesPhysicalCounters) {
  ExecutionLifecycleTelemetry first;
  ExecutionLifecycleTelemetry second;
  EXPECT_NE(first.service_instance_id(), 0U);
  EXPECT_NE(first.telemetry_epoch(), 0U);
  EXPECT_NE(first.service_instance_id(), second.service_instance_id());
  EXPECT_NE(first.telemetry_epoch(), second.telemetry_epoch());

  first.increment_physical_counter(
      ExecutionLifecyclePhysicalCounter::ReadyEntry);
  first.increment_physical_counter(
      ExecutionLifecyclePhysicalCounter::EnteredCallback);
  first.increment_physical_counter(
      ExecutionLifecyclePhysicalCounter::LiveRootReservation);
  first.increment_physical_counter(
      ExecutionLifecyclePhysicalCounter::LiveChildGrant);
  first.increment_physical_counter(
      ExecutionLifecyclePhysicalCounter::LivePolicyInvocation);
  first.increment_physical_counter(
      ExecutionLifecyclePhysicalCounter::LivePolicyBinding);
  const ExecutionLifecyclePage live = first.snapshot(0U, 1U);
  EXPECT_EQ(live.counters.ready_entry_count, 1U);
  EXPECT_EQ(live.counters.entered_callback_count, 1U);
  EXPECT_EQ(live.counters.live_root_reservation_count, 1U);
  EXPECT_EQ(live.counters.live_child_grant_count, 1U);
  EXPECT_EQ(live.counters.live_policy_invocation_count, 1U);
  EXPECT_EQ(live.counters.live_policy_binding_count, 1U);
  EXPECT_FALSE(first.physical_counters_zero());

  first.decrement_physical_counter(
      ExecutionLifecyclePhysicalCounter::LivePolicyBinding);
  first.decrement_physical_counter(
      ExecutionLifecyclePhysicalCounter::LivePolicyInvocation);
  first.decrement_physical_counter(
      ExecutionLifecyclePhysicalCounter::LiveChildGrant);
  first.decrement_physical_counter(
      ExecutionLifecyclePhysicalCounter::LiveRootReservation);
  first.decrement_physical_counter(
      ExecutionLifecyclePhysicalCounter::EnteredCallback);
  first.decrement_physical_counter(
      ExecutionLifecyclePhysicalCounter::ReadyEntry);
  EXPECT_TRUE(first.physical_counters_zero());
}

TEST(ExecutionLifecycleTelemetry,
     PreservesTimestampTiesAndSafeMaximumConversion) {
  ExecutionLifecycleTelemetry telemetry;
  const auto tied = std::chrono::steady_clock::duration{5};
  EXPECT_EQ(testing::ExecutionLifecycleTelemetryTestAccess::publish_at(
                telemetry, tied),
            1U);
  EXPECT_EQ(testing::ExecutionLifecycleTelemetryTestAccess::publish_at(
                telemetry, tied),
            2U);
  EXPECT_EQ(testing::ExecutionLifecycleTelemetryTestAccess::publish_at(
                telemetry, std::chrono::steady_clock::duration::max()),
            3U);

  const ExecutionLifecyclePage page = telemetry.snapshot(0U, 3U);
  ASSERT_EQ(page.records.size(), 3U);
  EXPECT_EQ(page.records[0].timestamp_us, page.records[1].timestamp_us);
  EXPECT_EQ(page.records[0].sequence, 1U);
  EXPECT_EQ(page.records[1].sequence, 2U);
  EXPECT_LE(page.records[1].timestamp_us, page.records[2].timestamp_us);
  const long double maximum_microseconds =
      std::chrono::duration<long double, std::micro>(
          std::chrono::steady_clock::duration::max())
          .count();
  const bool must_saturate =
      maximum_microseconds >
      static_cast<long double>(std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(page.records[2].timestamp_saturated, must_saturate);
  EXPECT_EQ(page.records[2].timestamp_us,
            must_saturate
                ? std::numeric_limits<std::uint64_t>::max()
                : static_cast<std::uint64_t>(
                      std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::duration::max())
                          .count()));
}

TEST(ExecutionLifecycleTelemetry,
     ValidatesLimitsCursorsAndNonDestructiveReads) {
  ExecutionLifecycleTelemetry telemetry;
  EXPECT_THROW((void)telemetry.snapshot(0U, 0U), std::invalid_argument);
  EXPECT_THROW((void)telemetry.snapshot(
                   0U, kExecutionLifecycleTelemetryMaxPageSize + 1U),
               std::invalid_argument);
  EXPECT_THROW((void)telemetry.snapshot(1U, 1U), std::invalid_argument);
  EXPECT_THROW(
      (void)telemetry.snapshot(std::numeric_limits<std::uint64_t>::max(), 1U),
      std::invalid_argument);

  EXPECT_EQ(publish_event(telemetry), 1U);
  EXPECT_EQ(publish_event(telemetry), 2U);
  const ExecutionLifecyclePage first = telemetry.snapshot(0U, 1U);
  const ExecutionLifecyclePage repeated = telemetry.snapshot(0U, 1U);
  ASSERT_EQ(first.records.size(), 1U);
  ASSERT_EQ(repeated.records.size(), 1U);
  EXPECT_EQ(first.snapshot_cut, 2U);
  EXPECT_EQ(first.records.front().sequence, repeated.records.front().sequence);
  EXPECT_EQ(first.next_cursor, 1U);
  EXPECT_TRUE(first.has_more);
  EXPECT_THROW((void)telemetry.snapshot(3U, 1U), std::invalid_argument);

  const ExecutionLifecyclePage tail = telemetry.snapshot(first.next_cursor, 2U);
  ASSERT_EQ(tail.records.size(), 1U);
  EXPECT_EQ(tail.records.front().sequence, 2U);
  EXPECT_EQ(tail.next_cursor, 2U);
  EXPECT_FALSE(tail.has_more);
}

TEST(ExecutionLifecycleTelemetry, ReportsExactRingEvictionAndTooOldGap) {
  ExecutionLifecycleTelemetry telemetry;
  constexpr std::uint64_t kPublished =
      static_cast<std::uint64_t>(kExecutionLifecycleTelemetryCapacity) + 2U;
  for (std::uint64_t index = 0U; index < kPublished; ++index) {
    ASSERT_NE(publish_event(telemetry), 0U);
  }

  const ExecutionLifecyclePage oldest = telemetry.snapshot(0U, 1U);
  ASSERT_EQ(oldest.records.size(), 1U);
  EXPECT_EQ(oldest.first_retained_sequence, 3U);
  EXPECT_EQ(oldest.records.front().sequence, 3U);
  EXPECT_EQ(oldest.cursor_gap, 0U);
  EXPECT_EQ(oldest.global_dropped_total, 2U);
  EXPECT_FALSE(oldest.global_dropped_saturated);

  const ExecutionLifecyclePage gap = telemetry.snapshot(1U, 1U);
  ASSERT_EQ(gap.records.size(), 1U);
  EXPECT_EQ(gap.cursor_gap, 1U);
  EXPECT_EQ(gap.records.front().sequence, 3U);
}

TEST(ExecutionLifecycleTelemetry, ReservesFinalSequenceAndSentinelSemantics) {
  ExecutionLifecycleTelemetry telemetry;
  const std::uint64_t ordinary_last =
      std::numeric_limits<std::uint64_t>::max() - 2U;
  testing::ExecutionLifecycleTelemetryTestAccess::set_next_sequence(
      telemetry, ordinary_last);
  EXPECT_EQ(publish_event(telemetry), ordinary_last);
  EXPECT_EQ(publish_event(telemetry), 0U);

  telemetry.mark_stopping(7U, {});
  const std::uint64_t final = telemetry.publish_service_stopped(7U, {});
  EXPECT_EQ(final, std::numeric_limits<std::uint64_t>::max() - 1U);
  EXPECT_EQ(telemetry.publish_service_stopped(7U, {}), final);

  const ExecutionLifecyclePage exhausted =
      telemetry.snapshot(std::numeric_limits<std::uint64_t>::max(), 1U);
  EXPECT_TRUE(exhausted.records.empty());
  EXPECT_EQ(exhausted.service_state, ExecutionLifecycleServiceState::Stopped);
  EXPECT_EQ(exhausted.next_sequence, std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(exhausted.next_cursor, std::numeric_limits<std::uint64_t>::max());
  EXPECT_FALSE(exhausted.has_more);
}

TEST(ExecutionLifecycleTelemetry,
     ConcurrentServiceStopPublishesOneFinalEventWithoutDrops) {
  ExecutionLifecycleTelemetry telemetry;
  constexpr std::uint64_t kShutdownGeneration = 17U;
  constexpr std::size_t kCallerCount = 16U;
  telemetry.mark_stopping(kShutdownGeneration, {});

  std::atomic<std::size_t> ready_count{0U};
  std::atomic<bool> start{false};
  std::vector<std::uint64_t> sequences(kCallerCount, 0U);
  std::vector<std::thread> callers;
  callers.reserve(kCallerCount);
  for (std::size_t index = 0U; index < kCallerCount; ++index) {
    callers.emplace_back(
        [&telemetry, &ready_count, &start, &sequences, index]() {
          ready_count.fetch_add(1U, std::memory_order_release);
          while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
          }
          sequences[index] =
              telemetry.publish_service_stopped(kShutdownGeneration, {});
        });
  }
  while (ready_count.load(std::memory_order_acquire) != kCallerCount) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (std::thread& caller : callers) {
    caller.join();
  }

  ASSERT_NE(sequences.front(), 0U);
  EXPECT_TRUE(std::all_of(sequences.begin(), sequences.end(),
                          [&sequences](std::uint64_t sequence) {
                            return sequence == sequences.front();
                          }));
  const ExecutionLifecyclePage page = telemetry.snapshot(0U, 64U);
  EXPECT_EQ(page.service_state, ExecutionLifecycleServiceState::Stopped);
  EXPECT_EQ(page.global_dropped_total, 0U);
  EXPECT_EQ(std::count_if(page.records.begin(), page.records.end(),
                          [](const ExecutionLifecycleEvent& event) {
                            return event.kind ==
                                   ExecutionLifecycleEventKind::ServiceStopped;
                          }),
            1);
}

TEST(ExecutionLifecycleTelemetry,
     DropSaturationFlagBecomesStickyAfterBoundary) {
  ExecutionLifecycleTelemetry telemetry;
  testing::ExecutionLifecycleTelemetryTestAccess::set_drop_state(
      telemetry, std::numeric_limits<std::uint64_t>::max() - 1U, false);
  testing::ExecutionLifecycleTelemetryTestAccess::record_drop(telemetry);
  ExecutionLifecyclePage exact = telemetry.snapshot(0U, 1U);
  EXPECT_EQ(exact.global_dropped_total,
            std::numeric_limits<std::uint64_t>::max());
  EXPECT_FALSE(exact.global_dropped_saturated);

  testing::ExecutionLifecycleTelemetryTestAccess::record_drop(telemetry);
  const ExecutionLifecyclePage overflowed = telemetry.snapshot(0U, 1U);
  EXPECT_EQ(overflowed.global_dropped_total,
            std::numeric_limits<std::uint64_t>::max());
  EXPECT_TRUE(overflowed.global_dropped_saturated);
  testing::ExecutionLifecycleTelemetryTestAccess::record_drop(telemetry);
  EXPECT_TRUE(telemetry.snapshot(0U, 1U).global_dropped_saturated);
}

TEST(ExecutionLifecycleTelemetry, SnapshotAllocationFailureHasNoSideEffect) {
  ExecutionLifecycleTelemetry telemetry;
  ASSERT_EQ(publish_event(telemetry), 1U);
  const ExecutionLifecyclePage before = telemetry.snapshot(0U, 1U);

  bool caught = false;
  allocation_probe::arm(0);
  try {
    (void)telemetry.snapshot(0U, 4096U);
  } catch (const std::bad_alloc&) {
    caught = true;
  }
  allocation_probe::disarm();

  EXPECT_TRUE(caught);
  EXPECT_TRUE(allocation_probe::did_fire());
  const ExecutionLifecyclePage after = telemetry.snapshot(0U, 1U);
  EXPECT_EQ(after.snapshot_cut, before.snapshot_cut);
  EXPECT_EQ(after.next_sequence, before.next_sequence);
  EXPECT_EQ(after.global_dropped_total, before.global_dropped_total);
  ASSERT_EQ(after.records.size(), before.records.size());
  EXPECT_EQ(after.records.front().sequence, before.records.front().sequence);
}

TEST(ExecutionLifecycleTelemetry, ConcurrentSnapshotUsesOneBoundedCut) {
  ExecutionLifecycleTelemetry telemetry;
  std::atomic<bool> start{false};
  std::thread publisher([&]() {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (std::uint64_t index = 0U; index < 5000U; ++index) {
      (void)publish_event(telemetry);
    }
  });

  start.store(true, std::memory_order_release);
  for (int index = 0; index < 200; ++index) {
    const ExecutionLifecyclePage page = telemetry.snapshot(0U, 37U);
    for (const ExecutionLifecycleEvent& event : page.records) {
      EXPECT_LE(event.sequence, page.snapshot_cut);
    }
    if (!page.records.empty()) {
      EXPECT_EQ(page.next_cursor, page.records.back().sequence);
    }
  }
  publisher.join();
  EXPECT_EQ(telemetry.snapshot(0U, 1U).snapshot_cut, 5000U);
}

TEST(ExecutionLifecycleTelemetry, CopiedPageOutlivesStoreDestruction) {
  ExecutionLifecyclePage retained;
  {
    auto telemetry = std::make_unique<ExecutionLifecycleTelemetry>();
    ASSERT_EQ(publish_event(*telemetry), 1U);
    retained = telemetry->snapshot(0U, 1U);
  }
  ASSERT_EQ(retained.records.size(), 1U);
  EXPECT_EQ(retained.records.front().sequence, 1U);
  EXPECT_NE(retained.service_instance_id, 0U);
  EXPECT_NE(retained.telemetry_epoch, 0U);
}

}  // namespace
}  // namespace ps::compute
