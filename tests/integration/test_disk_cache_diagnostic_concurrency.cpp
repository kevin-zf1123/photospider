#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "support/graph_model_test_access.hpp"

namespace allocation_probe {

/** @brief Disabled allocation index sentinel. */
constexpr std::int64_t kDisabled = -1;

/** @brief Per-thread allocation countdown used by the snapshot probe. */
thread_local std::int64_t countdown = kDisabled;

/** @brief Whether the armed probe injected std::bad_alloc. */
thread_local bool fired = false;

/**
 * @brief Arms a one-shot failure for one current-thread allocation.
 * @param allocation_index Zero-based allocation that must fail.
 * @return Nothing.
 * @throws Nothing.
 * @note The probe disarms itself before throwing so recovery can allocate.
 */
void arm(std::int64_t allocation_index) noexcept {
  countdown = allocation_index;
  fired = false;
}

/**
 * @brief Disarms current-thread allocation failure injection.
 * @return Nothing.
 * @throws Nothing.
 */
void disarm() noexcept {
  countdown = kDisabled;
}

/**
 * @brief Reports whether the armed allocation failure fired.
 * @return True only after this thread injected std::bad_alloc.
 * @throws Nothing.
 */
bool did_fire() noexcept {
  return fired;
}

/**
 * @brief Applies the current thread's next allocation decision.
 * @return Nothing.
 * @throws std::bad_alloc when the armed countdown reaches zero.
 * @note Called only by this test executable's global allocation operators.
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
 * @brief Test-executable allocation operator with a one-shot failure probe.
 * @param size Requested allocation size.
 * @return Heap storage compatible with free().
 * @throws std::bad_alloc when injected or when malloc fails.
 * @note This override is linked only into the diagnostic concurrency test.
 */
void* operator new(std::size_t size) {
  allocation_probe::maybe_fail();
  if (void* memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc{};
}

/**
 * @brief Array counterpart to the test allocation operator.
 * @param size Requested allocation size.
 * @return Heap storage compatible with free().
 * @throws std::bad_alloc when injected or when malloc fails.
 */
void* operator new[](std::size_t size) {
  return ::operator new(size);
}

/**
 * @brief Releases storage allocated by the test allocation operator.
 * @param memory Storage to release, or null.
 * @return Nothing.
 * @throws Nothing.
 */
void operator delete(void* memory) noexcept {
  std::free(memory);
}

/**
 * @brief Releases array storage allocated by the test allocation operator.
 * @param memory Storage to release, or null.
 * @return Nothing.
 * @throws Nothing.
 */
void operator delete[](void* memory) noexcept {
  std::free(memory);
}

/**
 * @brief Sized release counterpart required by C++17 implementations.
 * @param memory Storage to release, or null.
 * @param size Original allocation size; unused by free().
 * @return Nothing.
 * @throws Nothing.
 */
void operator delete(void* memory, std::size_t size) noexcept {
  (void)size;
  std::free(memory);
}

/**
 * @brief Sized array release counterpart required by C++17 implementations.
 * @param memory Storage to release, or null.
 * @param size Original allocation size; unused by free().
 * @return Nothing.
 * @throws Nothing.
 */
void operator delete[](void* memory, std::size_t size) noexcept {
  (void)size;
  std::free(memory);
}

namespace ps {
namespace {

/**
 * @brief RAII scope for one current-thread allocation failure.
 * @note The scope owns no storage and always restores normal allocation.
 */
class ScopedAllocationFailure {
 public:
  /**
   * @brief Arms the requested zero-based allocation failure.
   * @param allocation_index Allocation index that must throw.
   * @throws Nothing.
   */
  explicit ScopedAllocationFailure(std::int64_t allocation_index) noexcept {
    allocation_probe::arm(allocation_index);
  }

  /**
   * @brief Restores ordinary allocation for the current thread.
   * @throws Nothing.
   */
  ~ScopedAllocationFailure() noexcept { allocation_probe::disarm(); }

  /**
   * @brief Disables copying of failure-probe ownership.
   * @param other Scope whose ownership cannot be duplicated.
   * @throws Nothing because construction is unavailable.
   */
  ScopedAllocationFailure(const ScopedAllocationFailure& other) = delete;

  /**
   * @brief Disables assignment of failure-probe ownership.
   * @param other Scope whose ownership cannot replace this scope.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedAllocationFailure& operator=(const ScopedAllocationFailure& other) =
      delete;
};

/**
 * @brief Captures one deterministic allocation-failure invocation.
 * @note Fields are written before the allocation probe scope is destroyed.
 */
struct AllocationFailureObservation {
  /** @brief Whether the requested allocation failure fired. */
  bool fired = false;
  /** @brief Whether the boundary propagated std::bad_alloc. */
  bool propagated = false;
};

/**
 * @brief Invokes one callable with a selected allocation failed.
 * @tparam Fn Nullary callable under test.
 * @param allocation_index Zero-based allocation that must fail.
 * @param fn Callable invoked once while failure injection is armed.
 * @return Fired and propagation observations captured before disarming.
 * @throws Any non-std::bad_alloc exception from fn after RAII cleanup.
 * @note No GoogleTest assertion executes while allocation failure is armed.
 */
template <typename Fn>
AllocationFailureObservation observe_allocation_failure(
    std::int64_t allocation_index, Fn&& fn) {
  AllocationFailureObservation observation;
  {
    ScopedAllocationFailure failure(allocation_index);
    try {
      std::forward<Fn>(fn)();
    } catch (const std::bad_alloc&) {
      observation.propagated = true;
    }
    observation.fired = allocation_probe::did_fire();
  }
  return observation;
}

/**
 * @brief Builds a diagnostic whose fields encode one shared sequence.
 * @param sequence Positive identity embedded in every owned field.
 * @return Complete value suitable for torn-snapshot detection.
 * @throws std::bad_alloc when string or path construction allocates.
 */
GraphModel::DiskCacheLoadResult make_diagnostic(int sequence) {
  const std::string token = std::to_string(sequence);
  GraphModel::DiskCacheLoadResult result;
  result.node_id = sequence;
  result.cache_type = "image-" + token;
  result.location = "entry-" + token + ".png";
  result.cache_file = std::filesystem::path("cache") / result.location;
  result.metadata_file =
      std::filesystem::path("metadata") / ("entry-" + token + ".yml");
  result.status = GraphModel::DiskCacheLoadStatus::Miss;
  result.code = GraphErrc::Unknown;
  result.message = "diagnostic-" + token;
  return result;
}

/**
 * @brief Checks that a snapshot contains fields from exactly one record.
 * @param result Snapshot returned by GraphModel.
 * @return True when every field carries the same positive sequence.
 * @throws std::bad_alloc when expected string or path construction allocates.
 */
bool is_complete_diagnostic(const GraphModel::DiskCacheLoadResult& result) {
  const std::string token = std::to_string(result.node_id);
  return result.node_id > 0 && result.cache_type == "image-" + token &&
         result.location == "entry-" + token + ".png" &&
         result.cache_file ==
             std::filesystem::path("cache") / result.location &&
         result.metadata_file ==
             std::filesystem::path("metadata") / ("entry-" + token + ".yml") &&
         result.status == GraphModel::DiskCacheLoadStatus::Miss &&
         result.code == GraphErrc::Unknown &&
         result.message == "diagnostic-" + token;
}

/**
 * @brief Owns record/snapshot workers behind an explicit start gate.
 *
 * The pool releases, stops, and consumes every worker before its borrowed
 * GraphModel can be destroyed. Product deadlock intentionally leaves get()
 * blocked; the dedicated CTest process timeout is the outer fault boundary.
 *
 * @note No product seam participates in worker traffic. The independent
 * executable prevents a stuck future destructor or join from retaining the
 * broad kernel-contract process.
 */
class DiagnosticWorkerPool {
 public:
  /**
   * @brief Launches gated workers that borrow one GraphModel.
   * @param graph Graph borrowed until stop_and_join() completes.
   * @param worker_count Positive worker count.
   * @throws std::invalid_argument when worker_count is zero.
   * @throws std::bad_alloc or std::system_error when launch fails.
   * @note Partial launch failure releases and consumes launched workers before
   * rethrowing.
   */
  DiagnosticWorkerPool(GraphModel& graph, std::size_t worker_count)
      : graph_(graph), worker_count_(worker_count) {
    if (worker_count_ == 0) {
      throw std::invalid_argument("diagnostic worker count must be positive");
    }
    workers_.reserve(worker_count_);
    try {
      for (std::size_t index = 0; index < worker_count_; ++index) {
        workers_.push_back(std::async(std::launch::async,
                                      [this]() noexcept { worker_loop(); }));
      }
    } catch (...) {
      started_.store(true, std::memory_order_release);
      stop_.store(true, std::memory_order_release);
      for (auto& worker : workers_) {
        if (worker.valid()) {
          worker.get();
        }
      }
      throw;
    }
  }

  /**
   * @brief Stops and consumes every worker before GraphModel member teardown.
   * @throws Nothing; worker exceptions are contained as failed health.
   */
  ~DiagnosticWorkerPool() noexcept { stop_and_join(); }

  /**
   * @brief Disables copying of worker and borrow ownership.
   * @param other Pool whose ownership cannot be duplicated.
   * @throws Nothing because construction is unavailable.
   */
  DiagnosticWorkerPool(const DiagnosticWorkerPool& other) = delete;

  /**
   * @brief Disables assignment of worker and borrow ownership.
   * @param other Pool whose ownership cannot replace this pool.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  DiagnosticWorkerPool& operator=(const DiagnosticWorkerPool& other) = delete;

  /**
   * @brief Waits for all workers to reach the closed start gate.
   * @param timeout Maximum readiness observation interval.
   * @return True when every worker reported ready before the deadline.
   * @throws Nothing.
   */
  bool wait_until_ready(std::chrono::milliseconds timeout) const noexcept {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (ready_count_.load(std::memory_order_acquire) == worker_count_) {
        return true;
      }
      std::this_thread::yield();
    }
    return ready_count_.load(std::memory_order_acquire) == worker_count_;
  }

  /**
   * @brief Waits for at least one complete record/snapshot cycle.
   * @param timeout Maximum activity observation interval.
   * @return True when any worker completed a cycle before the deadline.
   * @throws Nothing.
   */
  bool wait_until_active(std::chrono::milliseconds timeout) const noexcept {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (operation_count_.load(std::memory_order_acquire) > 0) {
        return true;
      }
      std::this_thread::yield();
    }
    return operation_count_.load(std::memory_order_acquire) > 0;
  }

  /**
   * @brief Opens the worker start gate.
   * @return Nothing.
   * @throws Nothing.
   */
  void start() noexcept { started_.store(true, std::memory_order_release); }

  /**
   * @brief Requests stop and consumes every asynchronous worker.
   * @return Nothing.
   * @throws Nothing; worker exceptions are contained as failed health.
   * @note Product deadlock is deliberately bounded by this executable's CTest
   * TIMEOUT rather than by an unsafe detach or a future destructor.
   */
  void stop_and_join() noexcept {
    if (joined_) {
      return;
    }
    joined_ = true;
    started_.store(true, std::memory_order_release);
    stop_.store(true, std::memory_order_release);
    for (auto& worker : workers_) {
      if (!worker.valid()) {
        continue;
      }
      try {
        worker.get();
      } catch (...) {
        healthy_.store(false, std::memory_order_release);
      }
    }
  }

  /**
   * @brief Reports whether workers observed only complete snapshots.
   * @return True when no worker exception or torn snapshot occurred.
   * @throws Nothing.
   */
  bool healthy() const noexcept {
    return healthy_.load(std::memory_order_acquire);
  }

  /**
   * @brief Returns the aggregate number of completed worker cycles.
   * @return Positive count after successful activity observation.
   * @throws Nothing.
   */
  std::uint64_t operation_count() const noexcept {
    return operation_count_.load(std::memory_order_acquire);
  }

 private:
  /**
   * @brief Runs one record/snapshot loop with exception containment.
   * @return Nothing.
   * @throws Nothing; every failure marks the pool unhealthy and requests stop.
   */
  void worker_loop() noexcept {
    ready_count_.fetch_add(1, std::memory_order_release);
    while (!started_.load(std::memory_order_acquire) &&
           !stop_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    try {
      while (!stop_.load(std::memory_order_acquire)) {
        const int sequence =
            next_sequence_.fetch_add(1, std::memory_order_relaxed);
        graph_.record_disk_cache_load_result(make_diagnostic(sequence));
        const auto snapshot = graph_.last_disk_cache_load_result_snapshot();
        if (snapshot.has_value() && !is_complete_diagnostic(*snapshot)) {
          healthy_.store(false, std::memory_order_release);
          stop_.store(true, std::memory_order_release);
          return;
        }
        operation_count_.fetch_add(1, std::memory_order_release);
        std::this_thread::yield();
      }
    } catch (...) {
      healthy_.store(false, std::memory_order_release);
      stop_.store(true, std::memory_order_release);
    }
  }

  /** @brief GraphModel borrowed by every worker. */
  GraphModel& graph_;
  /** @brief Number of workers expected at the start gate. */
  const std::size_t worker_count_;
  /** @brief Futures retaining every worker until deterministic recovery. */
  std::vector<std::future<void>> workers_;
  /** @brief Number of workers that reached the start gate. */
  std::atomic<std::size_t> ready_count_{0};
  /** @brief True after workers may enter diagnostic operations. */
  std::atomic<bool> started_{false};
  /** @brief True when workers must stop borrowing GraphModel. */
  std::atomic<bool> stop_{false};
  /** @brief True unless a worker or snapshot invariant failed. */
  std::atomic<bool> healthy_{true};
  /** @brief Positive sequence reserved for the next record. */
  std::atomic<int> next_sequence_{1};
  /** @brief Aggregate completed record/snapshot cycle count. */
  std::atomic<std::uint64_t> operation_count_{0};
  /** @brief Prevents repeated future consumption. */
  bool joined_ = false;
};

/**
 * @brief Waits for an atomic counter to reach one expected value.
 * @param counter Counter published by asynchronous workers.
 * @param expected Exact minimum value required.
 * @param timeout Maximum readiness observation interval.
 * @return True when counter reaches expected before the deadline.
 * @throws Nothing.
 */
bool wait_for_count(const std::atomic<int>& counter, int expected,
                    std::chrono::milliseconds timeout) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (counter.load(std::memory_order_acquire) >= expected) {
      return true;
    }
    std::this_thread::yield();
  }
  return counter.load(std::memory_order_acquire) >= expected;
}

}  // namespace

/**
 * @brief Exercises diagnostic record/snapshot against clear and publication.
 *
 * @return Nothing; GoogleTest reports readiness, liveness, and torn values.
 * @throws std::bad_alloc or std::system_error from fixture/worker setup.
 * @note A product deadlock cannot escape this dedicated CTest process because
 * CMake assigns every discovered case a finite timeout.
 */
TEST(DiskCacheDiagnosticConcurrency,
     RecordSnapshotClearAndPublicationRemainLive) {
  GraphModel graph{std::filesystem::path{}};
  constexpr std::size_t kWorkerCount = 8;
  constexpr int kLifecycleRounds = 192;
  DiagnosticWorkerPool workers(graph, kWorkerCount);
  ASSERT_TRUE(workers.wait_until_ready(std::chrono::seconds(2)));
  workers.start();
  ASSERT_TRUE(workers.wait_until_active(std::chrono::seconds(2)));

  for (int round = 0; round < kLifecycleRounds; ++round) {
    if (round % 3 == 0) {
      graph.clear();
    } else if (round % 3 == 1) {
      graph.record_disk_cache_load_result(make_diagnostic(100000 + round));
    } else {
      std::unique_ptr<GraphModel> staged = graph.clone_for_compute();
      staged->record_disk_cache_load_result(make_diagnostic(200000 + round));
      graph.publish_compute_snapshot(*staged);
    }

    const auto snapshot = graph.last_disk_cache_load_result_snapshot();
    if (snapshot.has_value()) {
      EXPECT_TRUE(is_complete_diagnostic(*snapshot));
    }
  }

  workers.stop_and_join();
  EXPECT_TRUE(workers.healthy());
  EXPECT_GT(workers.operation_count(), 0U);
}

/**
 * @brief Exercises same-store and opposite-direction two-store exchange.
 *
 * @return Nothing; GoogleTest reports readiness and final value integrity.
 * @throws std::bad_alloc or std::system_error from setup/thread launch.
 * @note The source-tree test bridge touches only diagnostic stores, avoiding
 * data races in unrelated GraphModel state. CTest bounds any lock regression.
 */
TEST(DiskCacheDiagnosticConcurrency,
     SameStoreAndOppositeDirectionExchangeRemainLive) {
  GraphModel first{std::filesystem::path{}};
  GraphModel second{std::filesystem::path{}};
  first.record_disk_cache_load_result(make_diagnostic(1));
  second.record_disk_cache_load_result(make_diagnostic(2));

  testing::GraphModelTestAccess::exchange_disk_cache_diagnostics(first, first);
  const auto after_self_exchange = first.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(after_self_exchange.has_value());
  EXPECT_EQ(after_self_exchange->node_id, 1);

  std::atomic<bool> start{false};
  std::atomic<int> ready{0};
  constexpr int kExchangeRounds = 10000;
  auto exchange_loop = [&](bool reverse) noexcept {
    ready.fetch_add(1, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int round = 0; round < kExchangeRounds; ++round) {
      if (reverse) {
        testing::GraphModelTestAccess::exchange_disk_cache_diagnostics(second,
                                                                       first);
      } else {
        testing::GraphModelTestAccess::exchange_disk_cache_diagnostics(first,
                                                                       second);
      }
    }
  };

  std::thread forward;
  std::thread reverse;
  try {
    forward = std::thread(exchange_loop, false);
    reverse = std::thread(exchange_loop, true);
  } catch (...) {
    start.store(true, std::memory_order_release);
    if (forward.joinable()) {
      forward.join();
    }
    if (reverse.joinable()) {
      reverse.join();
    }
    throw;
  }

  const bool both_ready = wait_for_count(ready, 2, std::chrono::seconds(2));
  start.store(true, std::memory_order_release);
  forward.join();
  reverse.join();
  ASSERT_TRUE(both_ready);

  const auto first_snapshot = first.last_disk_cache_load_result_snapshot();
  const auto second_snapshot = second.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(first_snapshot.has_value());
  ASSERT_TRUE(second_snapshot.has_value());
  EXPECT_TRUE(is_complete_diagnostic(*first_snapshot));
  EXPECT_TRUE(is_complete_diagnostic(*second_snapshot));
  EXPECT_NE(first_snapshot->node_id, second_snapshot->node_id);
  EXPECT_TRUE((first_snapshot->node_id == 1 && second_snapshot->node_id == 2) ||
              (first_snapshot->node_id == 2 && second_snapshot->node_id == 1));
}

/**
 * @brief Proves a throwing snapshot copy releases the scoped store guard.
 *
 * @return Nothing; GoogleTest reports injection, propagation, and recovery.
 * @throws std::bad_alloc only if unarmed fixture preparation exhausts memory.
 * @note A large owned string guarantees the first snapshot copy allocation is
 * eligible for deterministic failure. Subsequent record/snapshot proves the
 * same mutex was released during exception unwinding.
 */
TEST(DiskCacheDiagnosticConcurrency, SnapshotBadAllocReleasesScopedGuard) {
  GraphModel graph{std::filesystem::path{}};
  GraphModel::DiskCacheLoadResult diagnostic = make_diagnostic(300001);
  diagnostic.cache_type.assign(4096, 'x');
  graph.record_disk_cache_load_result(std::move(diagnostic));

  const AllocationFailureObservation failed = observe_allocation_failure(
      0, [&] { (void)graph.last_disk_cache_load_result_snapshot(); });
  EXPECT_TRUE(failed.fired);
  EXPECT_TRUE(failed.propagated);

  graph.record_disk_cache_load_result(make_diagnostic(300002));
  const auto recovered = graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(recovered.has_value());
  EXPECT_TRUE(is_complete_diagnostic(*recovered));
  EXPECT_EQ(recovered->node_id, 300002);
}

}  // namespace ps
