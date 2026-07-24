#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/kernel.hpp"
#include "support/kernel_test_dependencies.hpp"

#if defined(PHOTOSPIDER_INTERNAL_KERNEL_CLOSE_TESTING)
#error \
    "Production Kernel lifecycle concurrency tests must not enable close hooks."
#endif

namespace ps {
namespace {

/**
 * @brief Coordinates a fixed set of test workers at one explicit start edge.
 *
 * @throws std::system_error when mutex, condition-variable, or wait operations
 * fail.
 * @note The main test always calls release(), including after a readiness
 * timeout, so worker futures are not stranded during assertion cleanup.
 */
class ConcurrentStartGate final {
 public:
  /**
   * @brief Creates a closed gate for one fixed worker count.
   * @param expected_workers Number of workers that must arrive.
   * @throws Nothing.
   */
  explicit ConcurrentStartGate(std::size_t expected_workers) noexcept
      : expected_workers_(expected_workers) {}

  /**
   * @brief Records one worker arrival and waits for the shared release.
   * @return Nothing.
   * @throws std::system_error from locking or condition-variable waiting.
   */
  void arrive_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++arrived_workers_;
    changed_.notify_all();
    changed_.wait(lock, [this]() { return released_; });
  }

  /**
   * @brief Waits until every configured worker has reached the gate.
   * @param timeout Maximum bounded readiness interval.
   * @return True when all workers arrived before the deadline.
   * @throws std::system_error from locking or condition-variable waiting.
   */
  bool wait_until_ready(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this]() {
      return arrived_workers_ == expected_workers_;
    });
  }

  /**
   * @brief Releases every current and future waiter.
   * @return Nothing.
   * @throws Nothing; synchronization failure terminates the test process.
   */
  void release() noexcept {
    try {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
      }
      changed_.notify_all();
    } catch (...) {
      std::terminate();
    }
  }

  /**
   * @brief Prevents copying mutex and release ownership.
   * @param other Unused source because construction is unavailable.
   * @throws Nothing because the operation is deleted.
   */
  ConcurrentStartGate(const ConcurrentStartGate&) = delete;
  /**
   * @brief Prevents assigning mutex and release ownership.
   * @param other Unused source because assignment is unavailable.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ConcurrentStartGate& operator=(const ConcurrentStartGate&) = delete;

 private:
  /** @brief Fixed number of participating workers. */
  const std::size_t expected_workers_;
  /** @brief Serializes arrival and release state. */
  std::mutex mutex_;
  /** @brief Wakes the main test and released workers. */
  std::condition_variable changed_;
  /** @brief Number of workers currently waiting or already released. */
  std::size_t arrived_workers_ = 0U;
  /** @brief True after the one-way release edge. */
  bool released_ = false;
};

/**
 * @brief Exercises production graph publication, listing, and close races.
 *
 * @return Nothing; GoogleTest reports duplicate publication, unsafe listing,
 * timeout, or incomplete removal.
 * @throws Standard filesystem, future, Kernel, or synchronization exceptions
 * when the fixture cannot execute.
 * @note This binary links the installable production archive and deliberately
 * has no close observer seam. The service pool is configured before worker
 * launch so this test isolates graph-registry publication rather than
 * composition-time execution configuration. Repetition broadens interleavings
 * while explicit gates avoid relying on fixed operation sleeps.
 */
TEST(KernelLifecycleConcurrency,
     ConcurrentPublicationListingAndCloseUseProductionObjects) {
  constexpr std::size_t kRounds = 32U;
  constexpr std::size_t kLoaderCount = 4U;
  constexpr std::size_t kCloserCount = 4U;
  constexpr std::size_t kReaderCount = 2U;
  const auto root = std::filesystem::temp_directory_path() /
                    "photospider-kernel-production-lifecycle-concurrency";
  std::filesystem::remove_all(root);
  Kernel kernel = testing::make_kernel_with_yaml_graph_documents();
  Kernel::ExecutionConfig execution_config;
  kernel.set_execution_config(execution_config);
  std::atomic<std::uint64_t> read_iterations{0U};

  for (std::size_t round = 0U; round < kRounds; ++round) {
    const std::string graph_name =
        "production_lifecycle_" + std::to_string(round);
    ConcurrentStartGate load_gate(kLoaderCount);
    std::vector<std::future<bool>> loaders;
    loaders.reserve(kLoaderCount);
    for (std::size_t loader = 0U; loader < kLoaderCount; ++loader) {
      loaders.push_back(std::async(
          std::launch::async, [&kernel, &load_gate, &root, &graph_name]() {
            load_gate.arrive_and_wait();
            return kernel.load_graph(graph_name, root.string(), "").has_value();
          }));
    }
    const bool loaders_ready =
        load_gate.wait_until_ready(std::chrono::seconds(2));
    load_gate.release();
    EXPECT_TRUE(loaders_ready);
    std::size_t published_count = 0U;
    for (auto& loader : loaders) {
      ASSERT_EQ(loader.wait_for(std::chrono::seconds(2)),
                std::future_status::ready);
      published_count += loader.get() ? 1U : 0U;
    }
    ASSERT_EQ(published_count, 1U);
    EXPECT_EQ(kernel.list_graphs(), std::vector<std::string>{graph_name});

    ConcurrentStartGate close_gate(kCloserCount + kReaderCount);
    std::atomic<std::size_t> readers_started{0U};
    std::atomic<bool> stop_readers{false};
    std::vector<std::future<bool>> readers;
    readers.reserve(kReaderCount);
    for (std::size_t reader = 0U; reader < kReaderCount; ++reader) {
      readers.push_back(std::async(
          std::launch::async,
          [&kernel, &close_gate, &graph_name, &readers_started, &stop_readers,
           &read_iterations]() {
            close_gate.arrive_and_wait();
            readers_started.fetch_add(1U, std::memory_order_release);
            bool valid = true;
            while (!stop_readers.load(std::memory_order_acquire)) {
              const std::vector<std::string> names = kernel.list_graphs();
              valid = valid && (names.empty() || (names.size() == 1U &&
                                                  names.front() == graph_name));
              read_iterations.fetch_add(1U, std::memory_order_relaxed);
              std::this_thread::yield();
            }
            return valid;
          }));
    }

    std::vector<std::future<bool>> closers;
    closers.reserve(kCloserCount);
    for (std::size_t closer = 0U; closer < kCloserCount; ++closer) {
      closers.push_back(std::async(std::launch::async, [&kernel, &close_gate,
                                                        &graph_name,
                                                        &readers_started]() {
        close_gate.arrive_and_wait();
        while (readers_started.load(std::memory_order_acquire) < kReaderCount) {
          std::this_thread::yield();
        }
        return kernel.close_graph(graph_name);
      }));
    }

    const bool close_workers_ready =
        close_gate.wait_until_ready(std::chrono::seconds(2));
    close_gate.release();
    EXPECT_TRUE(close_workers_ready);
    std::size_t successful_closes = 0U;
    bool every_closer_ready = true;
    for (auto& closer : closers) {
      const bool ready =
          closer.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
      every_closer_ready = every_closer_ready && ready;
      if (ready) {
        successful_closes += closer.get() ? 1U : 0U;
      }
    }
    stop_readers.store(true, std::memory_order_release);
    bool every_reader_ready = true;
    bool every_reader_valid = true;
    for (auto& reader : readers) {
      const bool ready =
          reader.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
      every_reader_ready = every_reader_ready && ready;
      if (ready) {
        every_reader_valid = every_reader_valid && reader.get();
      }
    }
    ASSERT_TRUE(every_closer_ready);
    ASSERT_TRUE(every_reader_ready);
    EXPECT_TRUE(every_reader_valid);
    EXPECT_GE(successful_closes, 1U);
    EXPECT_TRUE(kernel.list_graphs().empty());
  }

  EXPECT_GT(read_iterations.load(std::memory_order_relaxed), 0U);
  std::filesystem::remove_all(root);
}

/**
 * @brief Races production shutdown against late graph publication.
 *
 * @return Nothing; GoogleTest reports a leaked graph, timeout, or reopened
 * publication.
 * @throws Standard filesystem, future, Kernel, or synchronization exceptions
 * when the fixture cannot execute.
 * @note A loader that wins the graph-registry lock before shutdown is drained
 * by shutdown; a loader that loses observes the monotonic publication-rejection
 * flag. No registration may linearize between those two effects.
 */
TEST(KernelLifecycleConcurrency,
     ShutdownAndGraphPublicationShareOneProductionAdmissionBoundary) {
  constexpr std::size_t kLoaderCount = 6U;
  const auto root = std::filesystem::temp_directory_path() /
                    "photospider-kernel-production-shutdown-concurrency";
  std::filesystem::remove_all(root);
  Kernel kernel = testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(
      kernel.load_graph("shutdown_seed", root.string(), "").has_value());
  ASSERT_TRUE(kernel.close_graph("shutdown_seed"));

  ConcurrentStartGate gate(kLoaderCount + 1U);
  std::vector<std::future<bool>> loaders;
  loaders.reserve(kLoaderCount);
  for (std::size_t loader = 0U; loader < kLoaderCount; ++loader) {
    loaders.push_back(
        std::async(std::launch::async, [&kernel, &gate, &root, loader]() {
          gate.arrive_and_wait();
          return kernel
              .load_graph("shutdown_race_" + std::to_string(loader),
                          root.string(), "")
              .has_value();
        }));
  }
  auto shutdown = std::async(std::launch::async, [&kernel, &gate]() {
    gate.arrive_and_wait();
    kernel.shutdown();
  });

  const bool workers_ready = gate.wait_until_ready(std::chrono::seconds(2));
  gate.release();
  EXPECT_TRUE(workers_ready);
  for (auto& loader : loaders) {
    ASSERT_EQ(loader.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    (void)loader.get();
  }
  ASSERT_EQ(shutdown.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  shutdown.get();

  EXPECT_TRUE(kernel.list_graphs().empty());
  EXPECT_FALSE(
      kernel.load_graph("shutdown_late", root.string(), "").has_value());
  std::filesystem::remove_all(root);
}

}  // namespace
}  // namespace ps
