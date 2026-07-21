#include "graph/graph_state_executor.hpp"

#include <limits>
#include <string>
#include <utility>

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
#include <atomic>

#include "graph/graph_state_executor_test_access.hpp"  // NOLINT(build/include_subdir)
#endif

namespace ps {

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
namespace {

/**
 * @brief Publishes one scalar lane snapshot through the test-only observer.
 * @param event Lifecycle checkpoint represented by the snapshot.
 * @param queue_capacity Fixed maximum number of waiting tasks.
 * @param queued_tasks Tasks currently waiting in FIFO storage.
 * @param active_tasks Tasks currently executing on the sole worker.
 * @param worker_threads Live worker loops owned by the executor.
 * @return Nothing.
 * @throws Nothing.
 * @note Callers hold the executor lifecycle mutex while capturing these
 *       values. The observer must not allocate, block, or re-enter the lane.
 *       Keeping this helper outside `GraphStateExecutor` preserves one class
 *       definition across production and test-only translation-unit variants.
 */
void publish_graph_state_executor_test_snapshot(
    testing::GraphStateExecutorTestEvent event, std::size_t queue_capacity,
    std::size_t queued_tasks, std::size_t active_tasks,
    std::size_t worker_threads) noexcept {
  testing::notify_graph_state_executor_test_hook(
      testing::GraphStateExecutorTestSnapshot{
          event, queue_capacity, queued_tasks, active_tasks, worker_threads});
}

}  // namespace
#endif

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

/** @copydoc GraphStateExecutor::stop_admission */
void GraphStateExecutor::stop_admission() {
  (void)stop_admission_for_close("stop_admission");
}

/** @copydoc GraphStateExecutor::stop_admission_for_close */
GraphStateExecutor::CloseGeneration
GraphStateExecutor::stop_admission_for_close(const char* operation) {
  reject_worker_reentry(operation);
  bool close_started = false;
  CloseGeneration generation = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Accepting) {
      if (close_generation_ == std::numeric_limits<CloseGeneration>::max()) {
        throw std::overflow_error(
            "GraphStateExecutor close generation is exhausted");
      }
      ++close_generation_;
      state_ = State::Draining;
      close_started = true;
    }
    generation = close_generation_;
  }
  if (close_started) {
    work_available_.notify_all();
    space_available_.notify_all();
  }
  return generation;
}

/** @copydoc GraphStateExecutor::close_and_drain */
void GraphStateExecutor::close_and_drain() {
  const CloseGeneration target_generation =
      stop_admission_for_close("close_and_drain");
  std::thread worker_to_join;
  {
    std::unique_lock<std::mutex> lock(mutex_);
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
    publish_graph_state_executor_test_snapshot(
        testing::GraphStateExecutorTestEvent::CloseCallerWaiting,
        queue_capacity_, tasks_.size(), active_task_count_,
        worker_thread_count_);
#endif
    while (completed_close_generation_ < target_generation) {
      if (state_ == State::WorkerStopped && !join_in_progress_) {
        join_in_progress_ = true;
        worker_to_join = std::move(worker_);
        break;
      }
      state_changed_.wait(lock);
    }
    if (completed_close_generation_ >= target_generation) {
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
    completed_close_generation_ = target_generation;
    join_in_progress_ = false;
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
    publish_graph_state_executor_test_snapshot(
        testing::GraphStateExecutorTestEvent::Closed, queue_capacity_,
        tasks_.size(), active_task_count_, worker_thread_count_);
#endif
  }
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
  testing::notify_graph_state_executor_close_publish_test_hook();
#endif
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
  publish_graph_state_executor_test_snapshot(
      testing::GraphStateExecutorTestEvent::Reopened, queue_capacity_,
      tasks_.size(), active_task_count_, worker_thread_count_);
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
    publish_graph_state_executor_test_snapshot(
        testing::GraphStateExecutorTestEvent::TaskQueued, queue_capacity_,
        tasks_.size(), active_task_count_, worker_thread_count_);
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
    publish_graph_state_executor_test_snapshot(
        testing::GraphStateExecutorTestEvent::WorkerStarted, queue_capacity_,
        tasks_.size(), active_task_count_, worker_thread_count_);
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
        publish_graph_state_executor_test_snapshot(
            testing::GraphStateExecutorTestEvent::WorkerStopped,
            queue_capacity_, tasks_.size(), active_task_count_,
            worker_thread_count_);
#endif
        state_changed_.notify_all();
        current_executor_ = nullptr;
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
      active_task_count_ = 1;
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
      publish_graph_state_executor_test_snapshot(
          testing::GraphStateExecutorTestEvent::TaskStarted, queue_capacity_,
          tasks_.size(), active_task_count_, worker_thread_count_);
#endif
    }
    space_available_.notify_one();
    task();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_task_count_ = 0;
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
      publish_graph_state_executor_test_snapshot(
          testing::GraphStateExecutorTestEvent::TaskFinished, queue_capacity_,
          tasks_.size(), active_task_count_, worker_thread_count_);
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

/**
 * @brief Borrowed unlocked close-publication hook pointer.
 * @throws Nothing for alias use.
 */
using ClosePublishTestHookPtr = const GraphStateExecutorClosePublishTestHook*;

/**
 * @brief Process-local hook for deterministic Closed/restart overlap tests.
 * @throws Nothing for atomic initialization and pointer publication.
 * @note Tests serialize installation and clear the borrowed hook after every
 *       affected close has completed.
 */
std::atomic<ClosePublishTestHookPtr> g_close_publish_test_hook{
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

/** @copydoc ps::testing::set_graph_state_executor_close_publish_test_hook */
void set_graph_state_executor_close_publish_test_hook(
    const GraphStateExecutorClosePublishTestHook* hook) noexcept {
  g_close_publish_test_hook.store(hook, std::memory_order_release);
}

/** @copydoc ps::testing::notify_graph_state_executor_close_publish_test_hook */
void notify_graph_state_executor_close_publish_test_hook() noexcept {
  const GraphStateExecutorClosePublishTestHook* hook =
      g_close_publish_test_hook.load(std::memory_order_acquire);
  if (hook != nullptr && hook->before_waiter_notification != nullptr) {
    hook->before_waiter_notification(hook->context);
  }
}

}  // namespace testing
#endif

}  // namespace ps
