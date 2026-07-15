#include "graph/graph_state_executor.hpp"

#include <string>
#include <utility>

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
#include <atomic>

#include "graph/graph_state_executor_test_access.hpp"  // NOLINT(build/include_subdir)
#endif

namespace ps {

/** @copydoc GraphStateExecutor::current_executor_ */
thread_local const GraphStateExecutor* GraphStateExecutor::current_executor_ =
    nullptr;  // NOLINT(whitespace/indent_namespace)

/** @copydoc GraphStateExecutor::GraphStateExecutor */
GraphStateExecutor::GraphStateExecutor(GraphModel& model,
                                       std::size_t queue_capacity)
    : model_(model), queue_capacity_(queue_capacity) {
  if (queue_capacity_ == 0) {
    throw std::invalid_argument(
        "GraphStateExecutor queue capacity must be nonzero");
  }
  worker_ = std::thread(&GraphStateExecutor::worker_loop, this);
}

/** @copydoc GraphStateExecutor::~GraphStateExecutor */
GraphStateExecutor::~GraphStateExecutor() noexcept {
  try {
    close_and_drain();
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc GraphStateExecutor::close_and_drain */
void GraphStateExecutor::close_and_drain() {
  reject_worker_reentry("close_and_drain");
  std::thread worker_to_join;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ == State::Accepting) {
      state_ = State::Draining;
      work_available_.notify_all();
      space_available_.notify_all();
    }

    while (state_ != State::Closed) {
      if (state_ == State::WorkerStopped && !join_in_progress_) {
        join_in_progress_ = true;
        worker_to_join = std::move(worker_);
        break;
      }
      state_changed_.wait(lock);
    }
    if (state_ == State::Closed) {
      return;
    }
  }

  try {
    worker_to_join.join();
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      worker_ = std::move(worker_to_join);
      join_in_progress_ = false;
    }
    state_changed_.notify_all();
    throw;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = State::Closed;
    join_in_progress_ = false;
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
    publish_test_snapshot_locked(testing::GraphStateExecutorTestEvent::Closed);
#endif
  }
  state_changed_.notify_all();
}

/** @copydoc GraphStateExecutor::restart_after_close_failure */
void GraphStateExecutor::restart_after_close_failure() {
  reject_worker_reentry("restart_after_close_failure");
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == State::Accepting) {
    return;
  }
  if (state_ != State::Closed || join_in_progress_ || worker_.joinable()) {
    throw std::logic_error(
        "GraphStateExecutor can restart only after its worker is joined");
  }

  state_ = State::Accepting;
  try {
    worker_ = std::thread(&GraphStateExecutor::worker_loop, this);
  } catch (...) {
    state_ = State::Closed;
    throw;
  }
  worker_thread_count_ = 1;
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
  publish_test_snapshot_locked(testing::GraphStateExecutorTestEvent::Reopened);
#endif
}

/** @copydoc GraphStateExecutor::enqueue */
void GraphStateExecutor::enqueue(std::packaged_task<void()> task) {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    space_available_.wait(lock, [this] {
      return state_ != State::Accepting || tasks_.size() < queue_capacity_;
    });
    if (state_ != State::Accepting) {
      throw std::runtime_error("GraphStateExecutor admission is closed");
    }
    tasks_.push_back(std::move(task));
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
    publish_test_snapshot_locked(
        testing::GraphStateExecutorTestEvent::TaskQueued);
#endif
  }
  work_available_.notify_one();
}

/** @copydoc GraphStateExecutor::worker_loop */
void GraphStateExecutor::worker_loop() {
  current_executor_ = this;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    worker_thread_count_ = 1;
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
    publish_test_snapshot_locked(
        testing::GraphStateExecutorTestEvent::WorkerStarted);
#endif
  }
  while (true) {
    std::packaged_task<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_available_.wait(lock, [this] {
        return state_ != State::Accepting || !tasks_.empty();
      });
      if (tasks_.empty()) {
        worker_thread_count_ = 0;
        state_ = State::WorkerStopped;
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
        publish_test_snapshot_locked(
            testing::GraphStateExecutorTestEvent::WorkerStopped);
#endif
        state_changed_.notify_all();
        current_executor_ = nullptr;
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
      active_task_count_ = 1;
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
      publish_test_snapshot_locked(
          testing::GraphStateExecutorTestEvent::TaskStarted);
#endif
    }
    space_available_.notify_one();
    task();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_task_count_ = 0;
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
      publish_test_snapshot_locked(
          testing::GraphStateExecutorTestEvent::TaskFinished);
#endif
    }
  }
}

/** @copydoc GraphStateExecutor::reject_worker_reentry */
void GraphStateExecutor::reject_worker_reentry(const char* operation) const {
  if (current_executor_ == this) {
    throw std::logic_error(std::string("GraphStateExecutor worker cannot ") +
                           operation + " its own lane");
  }
}

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
/** @copydoc GraphStateExecutor::publish_test_snapshot_locked */
void GraphStateExecutor::publish_test_snapshot_locked(
    testing::GraphStateExecutorTestEvent event) const noexcept {
  testing::notify_graph_state_executor_test_hook(
      testing::GraphStateExecutorTestSnapshot{event, queue_capacity_,
                                              tasks_.size(), active_task_count_,
                                              worker_thread_count_});
}

namespace testing {
namespace {

/**
 * @brief Borrowed bounded-lane observer pointer stored by the atomic seam.
 * @throws Nothing for alias use.
 */
using GraphStateExecutorTestHookPtr = const GraphStateExecutorTestHook*;

/**
 * @brief Process-local borrowed observer shared by test-enabled executors.
 * @throws Nothing for atomic initialization and pointer publication.
 * @note Tests serialize installation and remove the observer before destroying
 *       its hook or context.
 */
std::atomic<GraphStateExecutorTestHookPtr> g_graph_state_executor_test_hook{
    nullptr};  // NOLINT(whitespace/indent_namespace)

}  // namespace

/** @copydoc ps::testing::set_graph_state_executor_test_hook */
void set_graph_state_executor_test_hook(
    const GraphStateExecutorTestHook* hook) noexcept {
  g_graph_state_executor_test_hook.store(hook, std::memory_order_release);
}

/** @copydoc ps::testing::notify_graph_state_executor_test_hook */
void notify_graph_state_executor_test_hook(
    const GraphStateExecutorTestSnapshot& snapshot) noexcept {
  const GraphStateExecutorTestHook* hook =
      g_graph_state_executor_test_hook.load(std::memory_order_acquire);
  if (hook != nullptr && hook->notify != nullptr) {
    hook->notify(hook->context, snapshot);
  }
}

}  // namespace testing
#endif

}  // namespace ps
