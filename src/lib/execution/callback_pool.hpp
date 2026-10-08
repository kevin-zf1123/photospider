#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "execution/callback_queue_timing.hpp"
#include "execution/cpu_range.hpp"
#include "photospider/execution/execution.hpp"

namespace ps::execution_internal {
/**
 * @brief Move-only backend-queue entry with shared admission ownership.
 *
 * @note Destruction before worker start rolls back the waiting token.
 */
struct QueuedCallback final {
  /** @brief Complete callback invoked by exactly one worker. */
  std::function<void()> callback;
  /** @brief Aggregate waiting admission released before callback entry. */
  WaitingAdmission::Lease admission;
  /** @brief Optional completion notification after the callback body retires.
   */
  std::function<void()> retired = {};
  /** @brief Shared envelope lease; only its waiting slot retires at entry. */
  ResourceLease managed_queue;
  std::chrono::steady_clock::time_point published_at{};
};

// Owns the fixed workers and backend FIFO. Queue captures retire outside the
// queue mutex; shutdown rejects unstarted work and joins started callbacks
// before admission/resource owners may leave scope.
class ThreadPool final {
 public:
  ThreadPool(std::uint32_t worker_count, Backend backend,
             WaitingAdmission& admission, bool collect_timing);
  ~ThreadPool() noexcept;
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  [[nodiscard]] bool submit(QueuedCallback callback);
  CallbackQueueStatistics statistics();
  bool timing_enabled() const noexcept { return timing_.enabled(); }
  CpuRangeQueue& ranges() noexcept { return ranges_; }

 private:
  void worker_loop() noexcept;
  void stop_and_join() noexcept;
  /** @brief Serializes callback queue and stop state. */
  std::mutex mutex_;
  /** @brief Wakes workers for callbacks or shutdown. */
  std::condition_variable ready_;
  execution_internal::CpuRangeQueue ranges_;
  bool prefer_range_ = true;
  /** @brief Deterministic FIFO governed by shared waiting admission. */
  std::deque<QueuedCallback> callbacks_;
  /** @brief Owned fixed worker set. */
  std::vector<std::thread> workers_;
  /** @brief Exact local backend lane served by this pool. */
  [[maybe_unused]] const Backend backend_;
  execution_internal::CallbackQueueMeter timing_;
  /** @brief Monotonic stop flag guarded by `mutex_`. */
  bool stopping_ = false;
};
Status scheduler_exception_status(ErrorCode,
                                  const char* diagnostic = nullptr) noexcept;
}  // namespace ps::execution_internal
