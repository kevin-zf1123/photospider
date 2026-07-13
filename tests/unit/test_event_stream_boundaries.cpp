#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "photospider/host/event_stream.hpp"
#include "runtime/graph_event_service.hpp"
#include "runtime/graph_runtime.hpp"

namespace ps {
namespace {

/** @brief Unique suffix source for temporary runtime roots. */
std::atomic<uint64_t> g_runtime_directory_sequence{1};

/**
 * @brief Owns an isolated temporary filesystem root for GraphRuntime tests.
 *
 * @throws std::bad_alloc if path construction fails.
 * @note GraphRuntime creates the directory contents; destruction performs
 *       best-effort recursive cleanup without throwing.
 */
class ScopedRuntimeDirectory {
 public:
  /**
   * @brief Selects a process-local unique temporary root.
   * @throws std::bad_alloc if path construction fails.
   */
  ScopedRuntimeDirectory()
      : root_(
            std::filesystem::temp_directory_path() /
            ("photospider-event-stream-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" +
             std::to_string(g_runtime_directory_sequence.fetch_add(
                 1, std::memory_order_relaxed)))) {}

  /**
   * @brief Removes the temporary root without propagating cleanup errors.
   * @throws Nothing; filesystem cleanup reports through an ignored error code.
   */
  ~ScopedRuntimeDirectory() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  /**
   * @brief Copying would duplicate filesystem cleanup ownership.
   * @throws Nothing because construction is unavailable.
   */
  ScopedRuntimeDirectory(const ScopedRuntimeDirectory&) = delete;

  /**
   * @brief Copy assignment would duplicate filesystem cleanup ownership.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedRuntimeDirectory& operator=(const ScopedRuntimeDirectory&) = delete;

  /**
   * @brief Returns the selected runtime root.
   * @return Immutable temporary path.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Unique temporary filesystem root. */
  std::filesystem::path root_;
};

/**
 * @brief Creates internal GraphRuntime construction inputs for one test root.
 * @param directory Temporary root owner.
 * @return Info with a stable name and directory-local paths.
 * @throws std::bad_alloc if path/string copying fails.
 */
GraphRuntime::Info make_runtime_info(const ScopedRuntimeDirectory& directory) {
  GraphRuntime::Info info;
  info.name = "event_stream_boundaries";
  info.root = directory.root();
  info.cache_root = directory.root() / "cache";
  return info;
}

/**
 * @brief Verifies strict monotonic sequence ordering in one trace page.
 * @param page Page whose retained events are inspected.
 * @return true when every adjacent sequence increases.
 * @throws Nothing.
 */
bool trace_sequences_are_strictly_increasing(
    const GraphRuntime::SchedulerEventPage& page) noexcept {
  return std::adjacent_find(page.events.begin(), page.events.end(),
                            [](const auto& lhs, const auto& rhs) {
                              return lhs.sequence >= rhs.sequence;
                            }) == page.events.end();
}

TEST(EventStreamConstants, PublishExactLongLivedBounds) {
  EXPECT_EQ(kComputeEventDrainMinLimit, 1u);
  EXPECT_EQ(kComputeEventDrainMaxLimit, 1024u);
  EXPECT_EQ(kComputeEventRingCapacity, 8192u);
  EXPECT_EQ(kComputeEventTextMaxBytes, 1024u);
  EXPECT_EQ(kSchedulerTraceMinLimit, 1u);
  EXPECT_EQ(kSchedulerTraceMaxLimit, 4096u);
  EXPECT_EQ(kSchedulerTraceRingCapacity, 65536u);
  EXPECT_EQ(kObservationSequenceExhausted,
            std::numeric_limits<uint64_t>::max());
}

TEST(ComputeEventRing, CapacityTwoEvictsOldestAndDrainIsTransactional) {
  GraphEventService events(2);
  events.push(1, "one", "test", 1.0);
  events.push(2, "two", "test", 2.0);
  events.push(3, "three", "test", 3.0);

  EXPECT_THROW((void)events.drain(0), std::invalid_argument);
  EXPECT_THROW((void)events.drain(kComputeEventDrainMaxLimit + 1),
               std::invalid_argument);

  ComputeEventBatch first = events.drain(1);
  ASSERT_EQ(first.events.size(), 1u);
  EXPECT_EQ(first.events.front().sequence, 2u);
  EXPECT_EQ(first.events.front().name, "two");
  EXPECT_EQ(first.next_sequence, 3u);
  EXPECT_TRUE(first.has_more);
  EXPECT_EQ(first.dropped_count, 1u);

  ComputeEventBatch second = events.drain(2);
  ASSERT_EQ(second.events.size(), 1u);
  EXPECT_EQ(second.events.front().sequence, 3u);
  EXPECT_EQ(second.next_sequence, 4u);
  EXPECT_FALSE(second.has_more);
  EXPECT_EQ(second.dropped_count, 0u);

  ComputeEventBatch empty = events.drain(1);
  EXPECT_TRUE(empty.events.empty());
  EXPECT_EQ(empty.next_sequence, 4u);
  EXPECT_FALSE(empty.has_more);
  EXPECT_EQ(empty.dropped_count, 0u);
}

TEST(ComputeEventRing, MeasuresUtf8BytesAndDropsWholePublication) {
  GraphEventService events(8);
  std::string exact;
  for (int index = 0; index < 341; ++index) {
    exact += "中";
  }
  exact += "a";
  ASSERT_EQ(exact.size(), kComputeEventTextMaxBytes);
  const std::string oversized = exact + "b";
  ASSERT_EQ(oversized.size(), kComputeEventTextMaxBytes + 1);
  const std::string invalid_utf8("\xc3\x28", 2);

  events.push(1, exact, exact, 1.0);
  events.push(2, oversized, "source", 2.0);
  events.push(3, "name", oversized, 3.0);
  events.push(4, invalid_utf8, "source", 4.0);
  events.push(5, "name", invalid_utf8, 5.0);
  events.push(6, "six", "source", 6.0);

  ComputeEventBatch batch = events.drain(8);
  ASSERT_EQ(batch.events.size(), 2u);
  EXPECT_EQ(batch.events[0].sequence, 1u);
  EXPECT_EQ(batch.events[0].name, exact);
  EXPECT_EQ(batch.events[1].sequence, 6u);
  EXPECT_EQ(batch.dropped_count, 4u);
  EXPECT_EQ(batch.next_sequence, 7u);
}

TEST(ComputeEventRing, ExhaustionNeverPublishesSentinelOrDoubleCounts) {
  GraphEventService events(2, kObservationSequenceExhausted - 1);
  events.push(1, "final", "test", 1.0);
  events.push(2, "after", "test", 2.0);
  events.push(3, std::string(kComputeEventTextMaxBytes + 1, 'x'), "test", 3.0);

  ComputeEventBatch batch = events.drain(2);
  ASSERT_EQ(batch.events.size(), 1u);
  EXPECT_EQ(batch.events.front().sequence, kObservationSequenceExhausted - 1);
  EXPECT_EQ(batch.next_sequence, kObservationSequenceExhausted);
  EXPECT_FALSE(batch.has_more);
  EXPECT_EQ(batch.dropped_count, 2u);

  GraphEventService saturated(1, 1, kObservationSequenceExhausted);
  saturated.push(1, std::string(kComputeEventTextMaxBytes + 1, 'x'), "test",
                 1.0);
  EXPECT_EQ(saturated.drain(1).dropped_count, kObservationSequenceExhausted);
}

TEST(ComputeEventRing,
     ExhaustionAfterOversizedFinalAttemptReturnsTerminalCursor) {
  GraphEventService events(2, kObservationSequenceExhausted - 2);
  events.push(1, "retained", "test", 1.0);
  events.push(2, std::string(kComputeEventTextMaxBytes + 1, 'x'), "test", 2.0);

  ComputeEventBatch batch = events.drain(kComputeEventDrainMaxLimit);
  ASSERT_EQ(batch.events.size(), 1u);
  EXPECT_EQ(batch.events.front().sequence, kObservationSequenceExhausted - 2);
  EXPECT_NE(batch.events.front().sequence, kObservationSequenceExhausted);
  EXPECT_EQ(batch.next_sequence, kObservationSequenceExhausted);
  EXPECT_FALSE(batch.has_more);
  EXPECT_EQ(batch.dropped_count, 1u);
}

TEST(ComputeEventRing, ProductionCapacityRetainsExactlyNewest8192) {
  GraphEventService events;
  for (std::size_t index = 0; index < kComputeEventRingCapacity + 1; ++index) {
    events.push(static_cast<int>(index), "event", "capacity", 0.0);
  }

  std::vector<uint64_t> sequences;
  bool first_page = true;
  bool has_more = false;
  do {
    ComputeEventBatch batch = events.drain(kComputeEventDrainMaxLimit);
    if (first_page) {
      EXPECT_EQ(batch.dropped_count, 1u);
      first_page = false;
    } else {
      EXPECT_EQ(batch.dropped_count, 0u);
    }
    for (const auto& event : batch.events) {
      sequences.push_back(event.sequence);
    }
    has_more = batch.has_more;
  } while (has_more);

  ASSERT_EQ(sequences.size(), kComputeEventRingCapacity);
  EXPECT_EQ(sequences.front(), 2u);
  EXPECT_EQ(sequences.back(), kComputeEventRingCapacity + 1);
}

TEST(ComputeEventRing, ConcurrentProducersAndDrainerPreserveEverySequence) {
  constexpr int kProducerCount = 4;
  constexpr int kEventsPerProducer = 200;
  GraphEventService events(kProducerCount * kEventsPerProducer);
  std::atomic<int> producers_remaining{kProducerCount};
  std::vector<uint64_t> observed;

  std::thread drainer([&] {
    while (producers_remaining.load(std::memory_order_acquire) != 0) {
      ComputeEventBatch batch = events.drain(17);
      for (const auto& event : batch.events) {
        observed.push_back(event.sequence);
      }
      if (batch.events.empty()) {
        std::this_thread::yield();
      }
    }
    for (;;) {
      ComputeEventBatch batch = events.drain(17);
      for (const auto& event : batch.events) {
        observed.push_back(event.sequence);
      }
      if (!batch.has_more && batch.events.empty()) {
        break;
      }
    }
  });

  std::vector<std::thread> producers;
  for (int producer = 0; producer < kProducerCount; ++producer) {
    producers.emplace_back([&, producer] {
      for (int index = 0; index < kEventsPerProducer; ++index) {
        events.push(producer * kEventsPerProducer + index, "event",
                    "concurrent", 0.0);
      }
      producers_remaining.fetch_sub(1, std::memory_order_release);
    });
  }
  for (auto& producer : producers) {
    producer.join();
  }
  drainer.join();

  ASSERT_EQ(observed.size(),
            static_cast<std::size_t>(kProducerCount * kEventsPerProducer));
  EXPECT_TRUE(std::is_sorted(observed.begin(), observed.end()));
  EXPECT_EQ(observed.front(), 1u);
  EXPECT_EQ(observed.back(), observed.size());
}

TEST(SchedulerTraceRing, CapacityTwoPagesAreStableNonDestructiveAndExact) {
  ScopedRuntimeDirectory directory;
  GraphRuntime::Info info = make_runtime_info(directory);
  info.scheduler_trace_capacity = 2;
  GraphRuntime runtime(info);
  runtime.log_event(GraphRuntime::SchedulerEvent::EXECUTE, 1, 7, 11);
  runtime.log_event(GraphRuntime::SchedulerEvent::EXECUTE_TILE, 2, 8, 12);
  runtime.log_event(GraphRuntime::SchedulerEvent::RETHROW_EXCEPTION, 3, 9, 13);

  EXPECT_THROW((void)runtime.scheduler_trace_page(0, 0), std::invalid_argument);
  EXPECT_THROW(
      (void)runtime.scheduler_trace_page(0, kSchedulerTraceMaxLimit + 1),
      std::invalid_argument);
  EXPECT_THROW((void)runtime.scheduler_trace_page(4, 1), std::invalid_argument);

  GraphRuntime::SchedulerEventPage first = runtime.scheduler_trace_page(0, 1);
  ASSERT_EQ(first.events.size(), 1u);
  EXPECT_EQ(first.events.front().sequence, 2u);
  EXPECT_EQ(first.next_sequence, 2u);
  EXPECT_TRUE(first.has_more);
  EXPECT_EQ(first.dropped_count, 1u);

  GraphRuntime::SchedulerEventPage repeated =
      runtime.scheduler_trace_page(0, 1);
  ASSERT_EQ(repeated.events.size(), 1u);
  EXPECT_EQ(repeated.events.front().sequence, 2u);
  EXPECT_EQ(repeated.dropped_count, 1u);

  GraphRuntime::SchedulerEventPage second =
      runtime.scheduler_trace_page(first.next_sequence, 1);
  ASSERT_EQ(second.events.size(), 1u);
  EXPECT_EQ(second.events.front().sequence, 3u);
  EXPECT_EQ(second.next_sequence, 3u);
  EXPECT_FALSE(second.has_more);
  EXPECT_EQ(second.dropped_count, 0u);

  runtime.clear_scheduler_log();
  GraphRuntime::SchedulerEventPage cleared = runtime.scheduler_trace_page(0, 1);
  EXPECT_TRUE(cleared.events.empty());
  EXPECT_EQ(cleared.next_sequence, 0u);
  EXPECT_FALSE(cleared.has_more);
  EXPECT_EQ(cleared.dropped_count, 3u);
}

TEST(SchedulerTraceRing, EmptyAndExhaustedCursorsAreDeterministic) {
  ScopedRuntimeDirectory empty_directory;
  GraphRuntime empty_runtime(make_runtime_info(empty_directory));
  GraphRuntime::SchedulerEventPage empty =
      empty_runtime.scheduler_trace_page(0, 1);
  EXPECT_TRUE(empty.events.empty());
  EXPECT_EQ(empty.next_sequence, 0u);
  EXPECT_FALSE(empty.has_more);
  EXPECT_EQ(empty.dropped_count, 0u);
  EXPECT_THROW((void)empty_runtime.scheduler_trace_page(
                   kObservationSequenceExhausted, 1),
               std::invalid_argument);

  ScopedRuntimeDirectory exhausted_directory;
  GraphRuntime::Info info = make_runtime_info(exhausted_directory);
  info.scheduler_trace_capacity = 2;
  info.scheduler_trace_initial_sequence = kObservationSequenceExhausted - 1;
  info.scheduler_trace_initial_dropped_count =
      kObservationSequenceExhausted - 1;
  GraphRuntime exhausted(info);
  exhausted.log_event(GraphRuntime::SchedulerEvent::EXECUTE, 1, 0, 1);
  exhausted.log_event(GraphRuntime::SchedulerEvent::EXECUTE, 2, 0, 1);

  GraphRuntime::SchedulerEventPage final =
      exhausted.scheduler_trace_page(kObservationSequenceExhausted - 2, 1);
  ASSERT_EQ(final.events.size(), 1u);
  EXPECT_EQ(final.events.front().sequence, kObservationSequenceExhausted - 1);
  EXPECT_EQ(final.next_sequence, kObservationSequenceExhausted);
  EXPECT_FALSE(final.has_more);
  EXPECT_EQ(final.dropped_count, kObservationSequenceExhausted);

  GraphRuntime::SchedulerEventPage repeated =
      exhausted.scheduler_trace_page(kObservationSequenceExhausted - 2, 1);
  EXPECT_EQ(repeated.events.size(), 1u);
  EXPECT_EQ(repeated.next_sequence, kObservationSequenceExhausted);
  EXPECT_EQ(repeated.dropped_count, kObservationSequenceExhausted);

  GraphRuntime::SchedulerEventPage terminal =
      exhausted.scheduler_trace_page(kObservationSequenceExhausted, 1);
  EXPECT_TRUE(terminal.events.empty());
  EXPECT_EQ(terminal.next_sequence, kObservationSequenceExhausted);
  EXPECT_FALSE(terminal.has_more);
  EXPECT_EQ(terminal.dropped_count, 0u);
}

TEST(SchedulerTraceRing, ProductionCapacityRetainsExactlyNewest65536) {
  ScopedRuntimeDirectory directory;
  GraphRuntime runtime(make_runtime_info(directory));
  for (std::size_t index = 0; index < kSchedulerTraceRingCapacity + 1;
       ++index) {
    runtime.log_event(GraphRuntime::SchedulerEvent::EXECUTE,
                      static_cast<int>(index), 0, 1);
  }

  uint64_t cursor = 0;
  std::vector<uint64_t> observed;
  bool first_page = true;
  for (;;) {
    GraphRuntime::SchedulerEventPage page =
        runtime.scheduler_trace_page(cursor, kSchedulerTraceMaxLimit);
    EXPECT_TRUE(trace_sequences_are_strictly_increasing(page));
    if (first_page) {
      EXPECT_EQ(page.dropped_count, 1u);
      first_page = false;
    } else {
      EXPECT_EQ(page.dropped_count, 0u);
    }
    for (const auto& event : page.events) {
      observed.push_back(event.sequence);
    }
    cursor = page.next_sequence;
    if (!page.has_more) {
      break;
    }
  }

  ASSERT_EQ(observed.size(), kSchedulerTraceRingCapacity);
  EXPECT_EQ(observed.front(), 2u);
  EXPECT_EQ(observed.back(), kSchedulerTraceRingCapacity + 1);
  GraphRuntime::SchedulerEventPage repeated =
      runtime.scheduler_trace_page(0, 1);
  ASSERT_EQ(repeated.events.size(), 1u);
  EXPECT_EQ(repeated.events.front().sequence, 2u);
  EXPECT_EQ(repeated.dropped_count, 1u);
}

TEST(SchedulerTraceRing, ConcurrentProducersAndReadersRemainCoherent) {
  constexpr int kProducerCount = 4;
  constexpr int kEventsPerProducer = 200;
  ScopedRuntimeDirectory directory;
  GraphRuntime::Info info = make_runtime_info(directory);
  info.scheduler_trace_capacity = kProducerCount * kEventsPerProducer;
  GraphRuntime runtime(info);
  std::atomic<int> producers_remaining{kProducerCount};
  std::atomic<bool> reader_failed{false};

  std::vector<std::thread> readers;
  for (int reader = 0; reader < 3; ++reader) {
    readers.emplace_back([&] {
      while (producers_remaining.load(std::memory_order_acquire) != 0) {
        GraphRuntime::SchedulerEventPage page =
            runtime.scheduler_trace_page(0, 64);
        if (page.events.size() > 64 ||
            !trace_sequences_are_strictly_increasing(page)) {
          reader_failed.store(true, std::memory_order_release);
        }
        std::this_thread::yield();
      }
    });
  }

  std::vector<std::thread> producers;
  for (int producer = 0; producer < kProducerCount; ++producer) {
    producers.emplace_back([&, producer] {
      for (int index = 0; index < kEventsPerProducer; ++index) {
        runtime.log_event(GraphRuntime::SchedulerEvent::EXECUTE,
                          producer * kEventsPerProducer + index, producer, 1);
      }
      producers_remaining.fetch_sub(1, std::memory_order_release);
    });
  }
  for (auto& producer : producers) {
    producer.join();
  }
  for (auto& reader : readers) {
    reader.join();
  }
  EXPECT_FALSE(reader_failed.load(std::memory_order_acquire));

  uint64_t cursor = 0;
  std::size_t observed_count = 0;
  for (;;) {
    GraphRuntime::SchedulerEventPage page =
        runtime.scheduler_trace_page(cursor, 64);
    EXPECT_TRUE(trace_sequences_are_strictly_increasing(page));
    observed_count += page.events.size();
    cursor = page.next_sequence;
    if (!page.has_more) {
      break;
    }
  }
  EXPECT_EQ(observed_count,
            static_cast<std::size_t>(kProducerCount * kEventsPerProducer));
}

}  // namespace
}  // namespace ps
