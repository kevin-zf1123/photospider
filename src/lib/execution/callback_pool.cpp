#include "execution/callback_pool.hpp"

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
#include "execution/execution_test_hooks.hpp"
#endif

namespace ps::execution_internal {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
/**
 * @brief Allocation-free test exception exposing a null diagnostic pointer.
 *
 * @note This type exists only in the noninstalled test-kernel variant and
 * exercises defensive `std::exception::what()` handling at the scheduler
 * exception fence.
 */
class NullDiagnosticException final : public std::exception {
 public:
  /**
   * @brief Returns the deliberately absent test diagnostic.
   * @return Null by design.
   * @throws Nothing.
   */
  [[nodiscard]] const char* what() const noexcept override { return nullptr; }
};
#endif

Status scheduler_exception_status(ErrorCode code,
                                  const char* diagnostic) noexcept {
  const auto reason = code == ErrorCode::ResourceExhausted
                          ? FailureReason::CapacityLimit
                          : FailureReason::HostException;
  Status fallback{code, {}, reason};
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
  if (execution_testing::fail_failure_status_construction(
          execution_testing::FailureStatusConstructionPoint::ExceptionFence))
    return fallback;
#endif
  try {
    fallback.message =
        diagnostic ? std::string(diagnostic).substr(0, 4096) : "";
  } catch (...) {
  }
  return fallback;
}

ThreadPool::ThreadPool(std::uint32_t worker_count, Backend backend,
                       WaitingAdmission& admission, bool collect_timing)
    : ranges_(mutex_, ready_, worker_count, admission),
      backend_(backend),
      timing_(collect_timing) {
  if (worker_count == 0U) {
    throw std::invalid_argument("thread-pool worker count must be positive");
  }
  try {
    workers_.reserve(worker_count);
    for (std::uint32_t index = 0; index < worker_count; ++index) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  } catch (...) {
    stop_and_join();
    throw;
  }
}

ThreadPool::~ThreadPool() noexcept {
  stop_and_join();
}

bool ThreadPool::submit(QueuedCallback callback) {
  const auto begin = timing_.enabled()
                         ? std::chrono::steady_clock::now()
                         : std::chrono::steady_clock::time_point{};
  std::lock_guard<std::mutex> lock(mutex_);
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
  const execution_testing::CallbackSubmitAction test_action =
      execution_testing::callback_submit_action(backend_);
  if (test_action == execution_testing::CallbackSubmitAction::Drop)
    return true;
  if (test_action == execution_testing::CallbackSubmitAction::Reject) {
    return false;
  }
  if (test_action == execution_testing::CallbackSubmitAction::ThrowBadAlloc) {
    throw std::bad_alloc();
  }
  if (test_action ==
      execution_testing::CallbackSubmitAction::ThrowNullDiagnostic) {
    throw NullDiagnosticException();
  }
#endif
  if (stopping_) {
    return false;
  }
  callbacks_.push_back(std::move(callback));
  if (timing_.enabled()) {
    const auto published = std::chrono::steady_clock::now();
    callbacks_.back().published_at = published;
    timing_.published(begin, published, callbacks_.size());
  }
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
  execution_testing::notify_callback_queued(backend_);
#endif
  ready_.notify_one();
  return true;
}

CallbackQueueStatistics ThreadPool::statistics() {
  std::lock_guard<std::mutex> lock(mutex_);
  return timing_.statistics();
}

void ThreadPool::worker_loop() noexcept {
  execution_internal::in_kernel_worker = true;
  for (;;) {
    QueuedCallback callback;
    execution_internal::CpuRangeQueue::Claim range;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_.wait(lock, [this] {
        return stopping_ || !callbacks_.empty() || ranges_.ready_locked();
      });
      if (stopping_) {
        return;
      }
      if (prefer_range_ || callbacks_.empty()) {
        range = ranges_.take_locked();
        if (range.job)
          prefer_range_ = false;
      }
      if (!range.job) {
        // Cancellation can remove range eligibility after the wait predicate.
        if (callbacks_.empty())
          continue;
        callback = std::move(callbacks_.front());
        callbacks_.pop_front();
        if (timing_.enabled())
          timing_.started(callback.published_at,
                          std::chrono::steady_clock::now());
        prefer_range_ = true;
      }
    }
    if (range.job) {
      ranges_.execute(range);
      continue;
    }
    callback.admission.release();
    if (callback.managed_queue.valid()) {
      ResourceCapacity waiting;
      waiting[ResourceKind::Queue] = 1;
      (void)callback.managed_queue.shrink(waiting);
    }
    try {
      callback.callback();
    } catch (...) {
      // Execution callbacks have their own status fence. This final fence
      // preserves pool liveness if a future callback violates that contract.
    }
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    execution_testing::notify_callback_body_finished();
#endif
    if (callback.retired)
      callback.retired();
  }
}

void ThreadPool::stop_and_join() noexcept {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!stopping_) {
      stopping_ = true;
      while (!callbacks_.empty()) {
        auto rejected = std::move(callbacks_.front());
        callbacks_.pop_front();
        lock.unlock();
        rejected.admission.release();
        rejected.callback = {};
        rejected.retired = {};
        rejected.managed_queue = {};
        lock.lock();
      }
    }
  }
  ready_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
}
}  // namespace ps::execution_internal
