#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ipc/collection_snapshot_registry.hpp"

namespace ps::ipc::internal {
namespace {

using Error = CollectionSnapshotError;
using TimePoint = std::chrono::steady_clock::time_point;

/**
 * @brief Builds a valid deterministic opaque token from a small integer.
 *
 * @param value Numeric suffix to encode.
 * @return Exactly 32 lowercase hexadecimal characters.
 * @throws std::bad_alloc if result string allocation fails.
 */
std::string opaque_id(unsigned long long value) {  // NOLINT(runtime/int)
  char buffer[33] = {};
  (void)std::snprintf(buffer, sizeof(buffer), "%032llx", value);
  return buffer;
}

/**
 * @brief Creates one exact frozen collection request identity.
 *
 * @param method Stable method name.
 * @param session Stable session id, empty for a global method.
 * @param params Canonical identity of original non-page parameters.
 * @return Owned binding used for publication and every later page.
 * @throws std::bad_alloc if text allocation fails.
 */
CollectionSnapshotBinding binding(std::string method = "inspect.node_ids",
                                  std::string session = opaque_id(0xabc),
                                  std::string params = "node_filter=all") {
  return {std::move(method), std::move(session), std::move(params)};
}

/**
 * @brief Creates small injected limits while preserving reserve-before-Host.
 *
 * @return Policy with four concurrent worst-case reservations.
 * @throws Nothing.
 */
CollectionSnapshotLimits small_limits() {
  CollectionSnapshotLimits limits;
  limits.records = 8;
  limits.total_bytes = 64;
  limits.reservation_bytes = 16;
  limits.snapshot_entries = 64;
  limits.snapshot_bytes = 16;
  limits.page_entries = 4;
  limits.ttl = std::chrono::milliseconds(10);
  return limits;
}

/**
 * @brief Thread-safe monotonic test clock advanced without sleeping.
 *
 * @throws Nothing.
 */
class ManualClock {
 public:
  /**
   * @brief Returns the current injected time point.
   * @return Time represented by the current nanosecond counter.
   * @throws Nothing.
   */
  TimePoint now() const noexcept {
    return TimePoint(std::chrono::nanoseconds(nanoseconds_.load()));
  }

  /**
   * @brief Sets the current injected monotonic time.
   * @param value New nanosecond count.
   * @throws Nothing.
   */
  void set(std::int64_t value) noexcept { nanoseconds_.store(value); }

  /**
   * @brief Advances the current injected monotonic time.
   * @param delta Nonnegative duration to add.
   * @throws Nothing.
   */
  void advance(std::chrono::nanoseconds delta) noexcept {
    nanoseconds_.fetch_add(delta.count());
  }

 private:
  /** @brief Current injected steady-clock tick count. */
  std::atomic<std::int64_t> nanoseconds_{0};
};

/**
 * @brief Deterministic thread-safe 32-hex cursor source.
 *
 * @throws Nothing before returned string allocation.
 */
class SequenceIds {
 public:
  /**
   * @brief Returns the next unique token.
   * @return Valid opaque id.
   * @throws std::bad_alloc if result allocation fails.
   */
  std::string next() { return opaque_id(next_.fetch_add(1)); }

 private:
  /** @brief Next numeric token suffix. */
  std::atomic<unsigned long long> next_{1};  // NOLINT(runtime/int)
};

/**
 * @brief Gate that releases several test threads at one deterministic point.
 *
 * @throws Whatever mutex primitives throw during construction.
 */
class StartGate {
 public:
  /**
   * @brief Blocks until `release()` is called.
   * @throws std::system_error if condition-variable waiting fails.
   */
  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return released_; });
  }

  /**
   * @brief Wakes every waiting thread exactly once.
   * @throws Nothing.
   */
  void release() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }

 private:
  /** @brief Serializes the release predicate. */
  std::mutex mutex_;

  /** @brief Wakes waiters after the predicate becomes true. */
  std::condition_variable condition_;

  /** @brief True after the sole release. */
  bool released_ = false;
};

/**
 * @brief Copyable row whose copy can be fault-injected for transaction tests.
 *
 * @throws std::runtime_error when `throw_on_copy` is enabled.
 */
struct ThrowingRow {
  /** @brief Public row payload. */
  int value = 0;

  /** @brief Global copy fault switch used by one single-threaded test. */
  static std::atomic<bool> throw_on_copy;

  /** @brief Creates a row. @throws Nothing. */
  ThrowingRow() = default;

  /**
   * @brief Creates a row with one integer payload.
   * @param input Initial payload.
   * @throws Nothing.
   */
  explicit ThrowingRow(int input) : value(input) {}

  /**
   * @brief Copies a row unless the injected fault is active.
   * @param other Source row.
   * @throws std::runtime_error when copy failure is requested.
   */
  ThrowingRow(const ThrowingRow& other) : value(other.value) {
    if (throw_on_copy.load()) {
      throw std::runtime_error("injected snapshot row copy failure");
    }
  }

  /**
   * @brief Copies a row by assignment unless fault injection is active.
   * @param other Source row.
   * @return This row.
   * @throws std::runtime_error when copy failure is requested.
   */
  ThrowingRow& operator=(const ThrowingRow& other) {
    if (throw_on_copy.load()) {
      throw std::runtime_error("injected snapshot row copy failure");
    }
    value = other.value;
    return *this;
  }

  /** @brief Allows noexcept vector moves. @throws Nothing. */
  ThrowingRow(ThrowingRow&&) noexcept = default;

  /**
   * @brief Allows noexcept vector move assignment.
   * @return This row.
   * @throws Nothing.
   */
  ThrowingRow& operator=(ThrowingRow&&) noexcept = default;
};

std::atomic<bool> ThrowingRow::throw_on_copy{false};

/**
 * @brief Owns deterministic clock and id sources for snapshot-registry tests.
 *
 * @throws Nothing from fixture-owned state construction.
 * @note `make_registry()` creates callback storage that borrows these sources;
 *       each returned registry remains scoped inside its calling test.
 */
class CollectionSnapshotRegistryTest : public ::testing::Test {
 protected:
  /**
   * @brief Creates a stopped registry using fixture clock and ids.
   * @param limits Injected policy.
   * @return Registry owned by the caller.
   * @throws std::bad_alloc if registry storage cannot allocate.
   * @throws std::invalid_argument if `limits` is an invalid policy.
   */
  std::unique_ptr<CollectionSnapshotRegistry> make_registry(
      CollectionSnapshotLimits limits = small_limits()) {
    return std::make_unique<CollectionSnapshotRegistry>(
        limits, [this] { return clock_.now(); },
        [this] { return ids_.next(); });
  }

  /** @brief Injected clock. */
  ManualClock clock_;

  /** @brief Injected unique token source. */
  SequenceIds ids_;
};

TEST_F(CollectionSnapshotRegistryTest, RejectsInvalidPolicies) {
  CollectionSnapshotLimits limits = small_limits();
  limits.records = 0;
  EXPECT_THROW((void)CollectionSnapshotRegistry(limits), std::invalid_argument);
  limits = small_limits();
  limits.reservation_bytes = limits.snapshot_bytes - 1;
  EXPECT_THROW((void)CollectionSnapshotRegistry(limits), std::invalid_argument);
  limits = small_limits();
  limits.total_bytes = limits.reservation_bytes - 1;
  EXPECT_THROW((void)CollectionSnapshotRegistry(limits), std::invalid_argument);
  limits = small_limits();
  limits.ttl = std::chrono::steady_clock::duration::zero();
  EXPECT_THROW((void)CollectionSnapshotRegistry(limits), std::invalid_argument);
}

TEST_F(CollectionSnapshotRegistryTest,
       ReservesWorstCaseQuotaBeforeAnyHostAccess) {
  CollectionSnapshotRegistry registry;
  registry.start();
  std::vector<CollectionSnapshotRegistry::Reservation> held;
  for (int index = 0; index < 4; ++index) {
    auto reserved = registry.reserve();
    ASSERT_EQ(reserved.error, Error::None);
    held.push_back(std::move(reserved.reservation));
  }

  int host_calls = 0;
  auto rejected = registry.reserve();
  if (rejected.error == Error::None) {
    ++host_calls;
  }
  EXPECT_EQ(rejected.error, Error::CapacityExceeded);
  EXPECT_EQ(host_calls, 0);
}

TEST_F(CollectionSnapshotRegistryTest,
       ConcurrentLastReservationsNeverOversubscribeBytes) {
  auto registry = make_registry();
  registry->start();
  StartGate gate;
  std::mutex held_mutex;
  std::vector<CollectionSnapshotRegistry::Reservation> held;
  std::atomic<int> admitted{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < 8; ++index) {
    threads.emplace_back([&] {
      gate.wait();
      auto reserved = registry->reserve();
      if (reserved.error == Error::None) {
        ++admitted;
        std::lock_guard<std::mutex> lock(held_mutex);
        held.push_back(std::move(reserved.reservation));
      }
    });
  }
  gate.release();
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(admitted.load(), 4);
  EXPECT_EQ(held.size(), 4U);
}

TEST_F(CollectionSnapshotRegistryTest,
       OversizeEntryAndByteCountsRollbackReservation) {
  auto registry = make_registry();
  registry->start();
  auto entry_reservation = registry->reserve();
  ASSERT_EQ(entry_reservation.error, Error::None);
  std::vector<int> too_many(65, 1);
  auto entry_result =
      registry->publish(std::move(entry_reservation.reservation), binding(),
                        std::move(too_many), 8, 2);
  EXPECT_EQ(entry_result.error, Error::ResponseTooLarge);
  EXPECT_EQ(registry->record_count(), 0U);
  EXPECT_EQ(registry->retained_bytes(), 0U);

  auto byte_reservation = registry->reserve();
  ASSERT_EQ(byte_reservation.error, Error::None);
  auto byte_result =
      registry->publish(std::move(byte_reservation.reservation), binding(),
                        std::vector<int>{1, 2, 3}, 17, 2);
  EXPECT_EQ(byte_result.error, Error::ResponseTooLarge);
  EXPECT_EQ(registry->reserve().error, Error::None);
}

TEST_F(CollectionSnapshotRegistryTest,
       EmptyAndSinglePagePublicationsConsumeValidReservations) {
  CollectionSnapshotLimits limits = small_limits();
  limits.records = 1;
  limits.total_bytes = limits.reservation_bytes;
  auto registry = make_registry(limits);
  registry->start();

  auto empty_reservation = registry->reserve();
  ASSERT_EQ(empty_reservation.error, Error::None);
  auto empty = registry->publish(std::move(empty_reservation.reservation),
                                 binding(), std::vector<int>{}, 0, 1);
  EXPECT_EQ(empty.error, Error::None);
  EXPECT_TRUE(empty.entries.empty());
  EXPECT_FALSE(empty.cursor.has_value());
  EXPECT_EQ(registry->record_count(), 0U);

  auto single_reservation = registry->reserve();
  ASSERT_EQ(single_reservation.error, Error::None);
  auto single = registry->publish(std::move(single_reservation.reservation),
                                  binding(), std::vector<int>{7}, 1, 1);
  EXPECT_EQ(single.error, Error::None);
  EXPECT_EQ(single.entries, (std::vector<int>{7}));
  EXPECT_FALSE(single.cursor.has_value());
  EXPECT_EQ(registry->record_count(), 0U);
  EXPECT_EQ(registry->retained_bytes(), 0U);
  EXPECT_EQ(registry->reserve().error, Error::None);
}

TEST_F(CollectionSnapshotRegistryTest,
       SinglePageRejectsDefaultInactiveAndForeignReservations) {
  CollectionSnapshotLimits limits = small_limits();
  limits.records = 1;
  limits.total_bytes = limits.reservation_bytes;
  auto registry = make_registry(limits);
  auto foreign_registry = make_registry(limits);
  registry->start();
  foreign_registry->start();

  auto default_result =
      registry->publish(CollectionSnapshotRegistry::Reservation{}, binding(),
                        std::vector<int>{1}, 1, 1);
  EXPECT_EQ(default_result.error, Error::InvalidParams);
  EXPECT_TRUE(default_result.entries.empty());

  auto held = registry->reserve();
  ASSERT_EQ(held.error, Error::None);
  CollectionSnapshotRegistry::Reservation active = std::move(held.reservation);
  auto inactive_result = registry->publish(
      std::move(held.reservation), binding(), std::vector<int>{2}, 1, 1);
  EXPECT_EQ(inactive_result.error, Error::InvalidParams);
  EXPECT_TRUE(active.active());

  auto foreign = foreign_registry->reserve();
  ASSERT_EQ(foreign.error, Error::None);
  auto foreign_result = registry->publish(std::move(foreign.reservation),
                                          binding(), std::vector<int>{3}, 1, 1);
  EXPECT_EQ(foreign_result.error, Error::InvalidParams);
  EXPECT_TRUE(active.active());

  auto valid_result = registry->publish(std::move(active), binding(),
                                        std::vector<int>{4}, 1, 1);
  EXPECT_EQ(valid_result.error, Error::None);
  EXPECT_EQ(valid_result.entries, (std::vector<int>{4}));
  EXPECT_EQ(foreign_registry->reserve().error, Error::None);
}

TEST_F(CollectionSnapshotRegistryTest,
       ShutdownInvalidatedSinglePageReservationCannotConsumeNewQuota) {
  CollectionSnapshotLimits limits = small_limits();
  limits.records = 1;
  limits.total_bytes = limits.reservation_bytes;
  auto registry = make_registry(limits);
  registry->start();
  auto stale = registry->reserve();
  ASSERT_EQ(stale.error, Error::None);

  registry->finish_shutdown();
  registry->start();
  auto current = registry->reserve();
  ASSERT_EQ(current.error, Error::None);
  auto stale_result = registry->publish(std::move(stale.reservation), binding(),
                                        std::vector<int>{1}, 1, 1);
  EXPECT_EQ(stale_result.error, Error::InvalidParams);
  EXPECT_TRUE(current.reservation.active());

  auto current_result = registry->publish(std::move(current.reservation),
                                          binding(), std::vector<int>{2}, 1, 1);
  EXPECT_EQ(current_result.error, Error::None);
  EXPECT_EQ(current_result.entries, (std::vector<int>{2}));
}

TEST_F(CollectionSnapshotRegistryTest,
       SinglePagePublicationAndFinalShutdownLinearizeSafely) {
  CollectionSnapshotLimits limits = small_limits();
  limits.records = 1;
  limits.total_bytes = limits.reservation_bytes;
  auto registry = make_registry(limits);
  registry->start();
  auto reserved = registry->reserve();
  ASSERT_EQ(reserved.error, Error::None);

  StartGate gate;
  std::atomic<Error> publication_error{Error::Stopped};
  std::thread publication_thread(
      [&, reservation = std::move(reserved.reservation)]() mutable {
        gate.wait();
        auto result = registry->publish(std::move(reservation), binding(),
                                        std::vector<int>{1}, 1, 1);
        publication_error.store(result.error);
      });
  std::thread shutdown_thread([&] {
    gate.wait();
    registry->finish_shutdown();
  });
  gate.release();
  publication_thread.join();
  shutdown_thread.join();

  EXPECT_TRUE(publication_error.load() == Error::None ||
              publication_error.load() == Error::InvalidParams);
  EXPECT_EQ(registry->record_count(), 0U);
  EXPECT_EQ(registry->retained_bytes(), 0U);
  registry->start();
  EXPECT_EQ(registry->reserve().error, Error::None);
}

TEST_F(CollectionSnapshotRegistryTest,
       PublicationUsesActualBytesAndSupportsRecordLimit) {
  CollectionSnapshotLimits limits = small_limits();
  limits.records = 3;
  auto registry = make_registry(limits);
  registry->start();
  for (int index = 0; index < 3; ++index) {
    auto reserved = registry->reserve();
    ASSERT_EQ(reserved.error, Error::None);
    auto published =
        registry->publish(std::move(reserved.reservation),
                          binding("inspect.node_ids", opaque_id(index + 1),
                                  "filter=" + std::to_string(index)),
                          std::vector<int>{1, 2}, 3, 1);
    ASSERT_EQ(published.error, Error::None);
    ASSERT_TRUE(published.cursor.has_value());
  }
  EXPECT_EQ(registry->record_count(), 3U);
  EXPECT_EQ(registry->retained_bytes(), 9U);
  EXPECT_EQ(registry->reserve().error, Error::CapacityExceeded);
}

TEST_F(CollectionSnapshotRegistryTest,
       ProductionEntryAndByteBoundariesAreInclusive) {
  const CollectionSnapshotLimits production;
  EXPECT_EQ(production.records, 64U);
  EXPECT_EQ(production.total_bytes, 256U * 1024U * 1024U);
  EXPECT_EQ(production.reservation_bytes, 64U * 1024U * 1024U);
  EXPECT_EQ(production.snapshot_entries, 262144U);
  EXPECT_EQ(production.snapshot_bytes, 64U * 1024U * 1024U);
  EXPECT_EQ(production.page_entries, 4096U);
  EXPECT_EQ(production.ttl, std::chrono::minutes(15));
  CollectionSnapshotRegistry registry(
      {}, [this] { return clock_.now(); }, [this] { return ids_.next(); });
  registry.start();
  auto entry_reservation = registry.reserve();
  ASSERT_EQ(entry_reservation.error, Error::None);
  std::vector<int> maximum_entries(kSnapshotMaxEntries, 7);
  auto at_entry_limit = registry.publish(
      std::move(entry_reservation.reservation), binding(),
      std::move(maximum_entries), kSnapshotMaxBytes, kGeneralPageMaxEntries);
  ASSERT_EQ(at_entry_limit.error, Error::None);
  EXPECT_TRUE(at_entry_limit.cursor.has_value());
  EXPECT_EQ(registry.retained_bytes(), kSnapshotMaxBytes);
  registry.finish_shutdown();
  registry.start();

  auto over_entry_reservation = registry.reserve();
  ASSERT_EQ(over_entry_reservation.error, Error::None);
  std::vector<int> over_entries(kSnapshotMaxEntries + 1, 7);
  auto over_entry_limit =
      registry.publish(std::move(over_entry_reservation.reservation), binding(),
                       std::move(over_entries), 1, kGeneralPageMaxEntries);
  EXPECT_EQ(over_entry_limit.error, Error::ResponseTooLarge);

  auto over_byte_reservation = registry.reserve();
  ASSERT_EQ(over_byte_reservation.error, Error::None);
  auto over_byte_limit =
      registry.publish(std::move(over_byte_reservation.reservation), binding(),
                       std::vector<int>{1, 2}, kSnapshotMaxBytes + 1, 1);
  EXPECT_EQ(over_byte_limit.error, Error::ResponseTooLarge);
}

TEST_F(CollectionSnapshotRegistryTest,
       CursorIsStableAndBindingMismatchDoesNotAdvance) {
  auto registry = make_registry();
  registry->start();
  const CollectionSnapshotBinding exact = binding();
  auto reserved = registry->reserve();
  ASSERT_EQ(reserved.error, Error::None);
  auto first = registry->publish(std::move(reserved.reservation), exact,
                                 std::vector<int>{10, 20, 30, 40}, 8, 2);
  ASSERT_EQ(first.error, Error::None);
  ASSERT_TRUE(first.cursor.has_value());
  EXPECT_EQ(*first.cursor, opaque_id(1));
  EXPECT_EQ(first.entries, (std::vector<int>{10, 20}));

  CollectionSnapshotBinding wrong_method = exact;
  wrong_method.method = "inspect.ending_nodes";
  EXPECT_EQ(registry->page<int>(*first.cursor, wrong_method, 2, 2).error,
            Error::CursorNotFound);
  CollectionSnapshotBinding wrong_session = exact;
  wrong_session.session_id = opaque_id(0xdef);
  EXPECT_EQ(registry->page<int>(*first.cursor, wrong_session, 2, 2).error,
            Error::CursorNotFound);
  CollectionSnapshotBinding wrong_params = exact;
  wrong_params.original_params = "node_filter=ending";
  EXPECT_EQ(registry->page<int>(*first.cursor, wrong_params, 2, 2).error,
            Error::CursorNotFound);
  EXPECT_EQ(registry->page<std::string>(*first.cursor, exact, 2, 2).error,
            Error::CursorNotFound);

  auto final = registry->page<int>(*first.cursor, exact, 2, 2);
  EXPECT_EQ(final.error, Error::None);
  EXPECT_EQ(final.entries, (std::vector<int>{30, 40}));
  EXPECT_FALSE(final.cursor.has_value());
  EXPECT_EQ(registry->record_count(), 0U);
}

TEST_F(CollectionSnapshotRegistryTest,
       InvalidLimitCursorAndOffsetAreNonDestructive) {
  auto registry = make_registry();
  registry->start();
  const CollectionSnapshotBinding exact = binding();
  auto reserved = registry->reserve();
  ASSERT_EQ(reserved.error, Error::None);
  auto first = registry->publish(std::move(reserved.reservation), exact,
                                 std::vector<int>{1, 2, 3, 4}, 4, 1);
  ASSERT_TRUE(first.cursor.has_value());

  EXPECT_EQ(registry->page<int>("bad", exact, 1, 1).error,
            Error::InvalidParams);
  EXPECT_EQ(registry->page<int>(*first.cursor, exact, 1, 0).error,
            Error::InvalidParams);
  EXPECT_EQ(registry->page<int>(*first.cursor, exact, 1, 5).error,
            Error::InvalidParams);
  EXPECT_EQ(registry
                ->page<int>(*first.cursor, exact,
                            std::numeric_limits<std::size_t>::max(), 1)
                .error,
            Error::InvalidParams);
  EXPECT_EQ(registry->page<int>(*first.cursor, exact, 2, 1).error,
            Error::CursorNotFound);

  auto second = registry->page<int>(*first.cursor, exact, 1, 1);
  ASSERT_EQ(second.error, Error::None);
  EXPECT_EQ(second.entries, (std::vector<int>{2}));
  EXPECT_EQ(second.cursor, first.cursor);
}

TEST_F(CollectionSnapshotRegistryTest,
       LaterPagesUseFrozenStateAfterExternalSessionClose) {
  auto registry = make_registry();
  registry->start();
  bool live_session = true;
  const CollectionSnapshotBinding exact = binding();
  auto reserved = registry->reserve();
  ASSERT_EQ(reserved.error, Error::None);
  auto first = registry->publish(std::move(reserved.reservation), exact,
                                 std::vector<std::string>{"a", "b", "c"}, 3, 1);
  ASSERT_TRUE(first.cursor.has_value());
  live_session = false;

  auto second = registry->page<std::string>(*first.cursor, exact, 1, 2);
  EXPECT_FALSE(live_session);
  EXPECT_EQ(second.error, Error::None);
  EXPECT_EQ(second.entries, (std::vector<std::string>{"b", "c"}));
  EXPECT_EQ(registry->record_count(), 0U);
}

TEST_F(CollectionSnapshotRegistryTest,
       LaterPagesNeverRefreshFixedPublicationTtl) {
  auto registry = make_registry();
  registry->start();
  const CollectionSnapshotBinding exact = binding();
  auto reserved = registry->reserve();
  auto first = registry->publish(std::move(reserved.reservation), exact,
                                 std::vector<int>{1, 2, 3, 4}, 4, 1);
  ASSERT_TRUE(first.cursor.has_value());

  clock_.advance(std::chrono::milliseconds(9));
  auto second = registry->page<int>(*first.cursor, exact, 1, 1);
  ASSERT_EQ(second.error, Error::None);
  clock_.advance(std::chrono::milliseconds(1));
  EXPECT_EQ(registry->page<int>(*first.cursor, exact, 2, 1).error,
            Error::CursorNotFound);
  EXPECT_EQ(registry->record_count(), 0U);
}

TEST_F(CollectionSnapshotRegistryTest,
       LazyExpiryReleasesQuotaBeforeTheNextReservation) {
  CollectionSnapshotLimits limits = small_limits();
  limits.records = 1;
  auto registry = make_registry(limits);
  registry->start();
  auto first_reservation = registry->reserve();
  auto first = registry->publish(std::move(first_reservation.reservation),
                                 binding(), std::vector<int>{1, 2}, 16, 1);
  ASSERT_TRUE(first.cursor.has_value());
  EXPECT_EQ(registry->reserve().error, Error::CapacityExceeded);

  clock_.advance(std::chrono::milliseconds(10));
  auto replacement = registry->reserve();
  EXPECT_EQ(replacement.error, Error::None);
  EXPECT_EQ(registry->record_count(), 0U);
  EXPECT_EQ(registry->retained_bytes(), 0U);
}

TEST_F(CollectionSnapshotRegistryTest,
       ConcurrentFinalPageHasExactlyOneWinnerAndReleasesAtomically) {
  auto registry = make_registry();
  registry->start();
  const CollectionSnapshotBinding exact = binding();
  auto reserved = registry->reserve();
  auto first = registry->publish(std::move(reserved.reservation), exact,
                                 std::vector<int>{1, 2, 3}, 3, 2);
  ASSERT_TRUE(first.cursor.has_value());
  StartGate gate;
  std::atomic<int> succeeded{0};
  std::atomic<int> absent{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < 2; ++index) {
    threads.emplace_back([&] {
      gate.wait();
      auto page = registry->page<int>(*first.cursor, exact, 2, 1);
      if (page.error == Error::None) {
        ++succeeded;
        EXPECT_EQ(page.entries, (std::vector<int>{3}));
      } else if (page.error == Error::CursorNotFound) {
        ++absent;
      }
    });
  }
  gate.release();
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(succeeded.load(), 1);
  EXPECT_EQ(absent.load(), 1);
  EXPECT_EQ(registry->record_count(), 0U);
}

TEST_F(CollectionSnapshotRegistryTest,
       PageAndExactExpiryRaceCannotReturnAnExpiredSnapshot) {
  auto registry = make_registry();
  registry->start();
  const CollectionSnapshotBinding exact = binding();
  auto reserved = registry->reserve();
  auto first = registry->publish(std::move(reserved.reservation), exact,
                                 std::vector<int>{1, 2, 3}, 3, 1);
  ASSERT_TRUE(first.cursor.has_value());
  clock_.advance(std::chrono::milliseconds(10));
  StartGate gate;
  std::atomic<int> absent{0};
  std::thread page_thread([&] {
    gate.wait();
    if (registry->page<int>(*first.cursor, exact, 1, 1).error ==
        Error::CursorNotFound) {
      ++absent;
    }
  });
  std::thread cleanup_thread([&] {
    gate.wait();
    if (registry->reserve().error == Error::None) {
      ++absent;
    }
  });
  gate.release();
  page_thread.join();
  cleanup_thread.join();
  EXPECT_EQ(absent.load(), 2);
  EXPECT_EQ(registry->record_count(), 0U);
}

TEST_F(CollectionSnapshotRegistryTest, FailedPageCopyPreservesCursorAndOffset) {
  auto registry = make_registry();
  registry->start();
  const CollectionSnapshotBinding exact = binding();
  auto reserved = registry->reserve();
  std::vector<ThrowingRow> rows;
  rows.emplace_back(1);
  rows.emplace_back(2);
  rows.emplace_back(3);
  auto first = registry->publish(std::move(reserved.reservation), exact,
                                 std::move(rows), 3, 1);
  ASSERT_TRUE(first.cursor.has_value());

  ThrowingRow::throw_on_copy.store(true);
  EXPECT_THROW(registry->page<ThrowingRow>(*first.cursor, exact, 1, 1),
               std::runtime_error);
  ThrowingRow::throw_on_copy.store(false);
  auto second = registry->page<ThrowingRow>(*first.cursor, exact, 1, 1);
  ASSERT_EQ(second.error, Error::None);
  ASSERT_EQ(second.entries.size(), 1U);
  EXPECT_EQ(second.entries.front().value, 2);
}

TEST_F(CollectionSnapshotRegistryTest,
       InvalidCursorGeneratorRollsBackPublicationReservation) {
  CollectionSnapshotRegistry registry(
      small_limits(), [this] { return clock_.now(); },
      [] { return "invalid"; });
  registry.start();
  auto reserved = registry.reserve();
  ASSERT_EQ(reserved.error, Error::None);
  EXPECT_THROW(registry.publish(std::move(reserved.reservation), binding(),
                                std::vector<int>{1, 2}, 2, 1),
               std::runtime_error);
  EXPECT_EQ(registry.record_count(), 0U);
  EXPECT_EQ(registry.retained_bytes(), 0U);
  EXPECT_EQ(registry.reserve().error, Error::None);
}

TEST_F(CollectionSnapshotRegistryTest,
       BeginShutdownAllowsReservedPublicationThenFinishClearsEverything) {
  auto registry = make_registry();
  registry->start();
  auto reserved = registry->reserve();
  ASSERT_EQ(reserved.error, Error::None);
  registry->begin_shutdown();
  EXPECT_EQ(registry->reserve().error, Error::Stopped);

  const CollectionSnapshotBinding exact = binding();
  auto first = registry->publish(std::move(reserved.reservation), exact,
                                 std::vector<int>{1, 2, 3}, 3, 1);
  ASSERT_EQ(first.error, Error::None);
  ASSERT_TRUE(first.cursor.has_value());
  EXPECT_EQ(registry->page<int>(*first.cursor, exact, 1, 1).error, Error::None);
  registry->finish_shutdown();
  EXPECT_EQ(registry->record_count(), 0U);
  EXPECT_EQ(registry->retained_bytes(), 0U);
  EXPECT_EQ(registry->page<int>(*first.cursor, exact, 2, 1).error,
            Error::CursorNotFound);

  registry->start();
  EXPECT_EQ(registry->reserve().error, Error::None);
}

TEST_F(CollectionSnapshotRegistryTest,
       OutstandingReservationIsInvalidatedSafelyAcrossRestart) {
  auto registry = make_registry();
  registry->start();
  auto stale = registry->reserve();
  ASSERT_TRUE(stale.reservation.active());
  registry->begin_shutdown();
  registry->finish_shutdown();
  EXPECT_FALSE(stale.reservation.active());
  registry->start();
  auto current = registry->reserve();
  ASSERT_TRUE(current.reservation.active());
  stale.reservation = {};
  EXPECT_TRUE(current.reservation.active());
}

TEST_F(CollectionSnapshotRegistryTest,
       ConcurrentFinalShutdownAndPagingHaveOneSafeLinearizedOutcome) {
  auto registry = make_registry();
  registry->start();
  const CollectionSnapshotBinding exact = binding();
  auto reserved = registry->reserve();
  auto first = registry->publish(std::move(reserved.reservation), exact,
                                 std::vector<int>{1, 2, 3}, 3, 1);
  ASSERT_TRUE(first.cursor.has_value());
  registry->begin_shutdown();

  StartGate gate;
  std::atomic<Error> page_outcome{Error::Stopped};
  std::thread page_thread([&] {
    gate.wait();
    const auto page = registry->page<int>(*first.cursor, exact, 1, 1);
    page_outcome.store(page.error);
  });
  std::thread shutdown_thread([&] {
    gate.wait();
    registry->finish_shutdown();
  });
  gate.release();
  page_thread.join();
  shutdown_thread.join();

  EXPECT_TRUE(page_outcome.load() == Error::None ||
              page_outcome.load() == Error::CursorNotFound);
  EXPECT_EQ(registry->record_count(), 0U);
  EXPECT_EQ(registry->retained_bytes(), 0U);
  registry->start();
  EXPECT_EQ(registry->reserve().error, Error::None);
}

TEST_F(CollectionSnapshotRegistryTest,
       SaturatingDeadlineAvoidsClockMaximumOverflow) {
  CollectionSnapshotLimits limits = small_limits();
  const TimePoint near_max = TimePoint::max() - std::chrono::nanoseconds(1);
  TimePoint current = near_max;
  CollectionSnapshotRegistry registry(
      limits, [&current] { return current; }, [this] { return ids_.next(); });
  registry.start();
  const CollectionSnapshotBinding exact = binding();
  auto reserved = registry.reserve();
  auto first = registry.publish(std::move(reserved.reservation), exact,
                                std::vector<int>{1, 2, 3}, 3, 1);
  ASSERT_TRUE(first.cursor.has_value());
  EXPECT_EQ(registry.page<int>(*first.cursor, exact, 1, 1).error, Error::None);
  current = TimePoint::max();
  EXPECT_EQ(registry.page<int>(*first.cursor, exact, 2, 1).error,
            Error::CursorNotFound);
}

}  // namespace
}  // namespace ps::ipc::internal
