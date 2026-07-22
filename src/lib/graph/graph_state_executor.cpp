#include "graph/graph_state_executor.hpp"

#include <limits>
#include <memory>
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
 * @param queue_capacity Fixed waiting-only or total-admission bound.
 * @param queued_tasks Tasks currently waiting in FIFO storage.
 * @param active_tasks Tasks currently executing on the sole worker.
 * @param worker_threads Live worker loops owned by the executor.
 * @param admitted_units Total charged units in total-admission mode.
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
    std::size_t worker_threads, std::size_t admitted_units) noexcept {
  testing::notify_graph_state_executor_test_hook(
      testing::GraphStateExecutorTestSnapshot{event, queue_capacity,
                                              queued_tasks, active_tasks,
                                              worker_threads, admitted_units});
}

}  // namespace
#endif

/** @copydoc GraphStateExecutor::current_executor_ */
thread_local const GraphStateExecutor* GraphStateExecutor::current_executor_ =
    nullptr;  // NOLINT(whitespace/indent_namespace)

/** @copydoc GraphStateExecutor::GraphStateExecutor */
GraphStateExecutor::GraphStateExecutor(GraphModel& model,
                                       std::size_t queue_capacity,
                                       CapacityMode capacity_mode)
    : model_(model),
      queue_capacity_(queue_capacity),
      capacity_mode_(capacity_mode) {
  if (queue_capacity_ == 0) {
    throw std::invalid_argument(
        "GraphStateExecutor queue capacity must be nonzero");
  }
  one_shot_nodes_.reserve(queue_capacity_);
  continuations_.reserve(queue_capacity_);
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
      for (auto& [identity, ticket] : continuations_) {
        (void)identity;
        if (ticket->phase == ContinuationPhase::Parked) {
          ticket->close_turn_pending = true;
        }
      }
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
        queue_capacity_, queued_task_count_, active_task_count_,
        worker_thread_count_, admitted_unit_count_);
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
        queued_task_count_, active_task_count_, worker_thread_count_,
        admitted_unit_count_);
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
      queued_task_count_, active_task_count_, worker_thread_count_,
      admitted_unit_count_);
#endif
}

/** @copydoc GraphStateExecutor::enqueue */
void GraphStateExecutor::enqueue(std::packaged_task<void()> task) {
  auto node = std::make_unique<QueueNode>();
  node->kind = QueueNode::Kind::OneShot;
  node->task = std::move(task);
  QueueNode* const node_address = node.get();
  {
    std::unique_lock<std::mutex> lock(mutex_);
    space_available_.wait(lock, [this] {
      if (state_ != State::Accepting) {
        return true;
      }
      return capacity_mode_ == CapacityMode::TotalAdmission
                 ? admitted_unit_count_ < queue_capacity_
                 : queued_task_count_ < queue_capacity_;
    });
    if (state_ != State::Accepting) {
      throw std::runtime_error("GraphStateExecutor admission is closed");
    }
    one_shot_nodes_.emplace(node_address, std::move(node));
    if (capacity_mode_ == CapacityMode::TotalAdmission) {
      ++admitted_unit_count_;
    }
    append_node_locked(node_address);
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
    publish_graph_state_executor_test_snapshot(
        testing::GraphStateExecutorTestEvent::TaskQueued, queue_capacity_,
        queued_task_count_, active_task_count_, worker_thread_count_,
        admitted_unit_count_);
#endif
  }
  work_available_.notify_one();
}

/** @copydoc GraphStateExecutor::ContinuationTicket::wake */
bool GraphStateExecutor::ContinuationTicket::wake() const {
  return owner_ != nullptr && state_ != nullptr &&
         owner_->wake_continuation(state_);
}

/** @copydoc GraphStateExecutor::ContinuationTicket::resume_after_settlement */
bool GraphStateExecutor::ContinuationTicket::resume_after_settlement() const {
  return owner_ != nullptr && state_ != nullptr &&
         owner_->resume_continuation(state_);
}

/** @copydoc GraphStateExecutor::ContinuationTicket::retire */
bool GraphStateExecutor::ContinuationTicket::retire() const {
  return owner_ != nullptr && state_ != nullptr &&
         owner_->retire_continuation(state_);
}

/** @copydoc GraphStateExecutor::reserve_continuation */
GraphStateExecutor::ContinuationTicket GraphStateExecutor::reserve_continuation(
    ContinuationCallback callback) {
  reject_worker_reentry("reserve a continuation on");
  if (capacity_mode_ != CapacityMode::TotalAdmission) {
    throw std::logic_error(
        "GraphStateExecutor continuations require total admission mode");
  }
  if (!callback) {
    throw std::invalid_argument(
        "GraphStateExecutor continuation callback must be non-empty");
  }

  std::unique_lock<std::mutex> lock(mutex_);
  space_available_.wait(lock, [this] {
    return state_ != State::Accepting || admitted_unit_count_ < queue_capacity_;
  });
  if (state_ != State::Accepting) {
    throw std::runtime_error("GraphStateExecutor admission is closed");
  }
  if (next_continuation_id_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error(
        "GraphStateExecutor continuation identity is exhausted");
  }
  const std::uint64_t identity = next_continuation_id_++;
  auto state =
      std::make_shared<ContinuationState>(identity, std::move(callback));
  continuations_.emplace(identity, state);
  ++admitted_unit_count_;
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
  publish_graph_state_executor_test_snapshot(
      testing::GraphStateExecutorTestEvent::ContinuationReserved,
      queue_capacity_, queued_task_count_, active_task_count_,
      worker_thread_count_, admitted_unit_count_);
#endif
  return ContinuationTicket(this, std::move(state));
}

/** @copydoc GraphStateExecutor::admitted_units */
std::size_t GraphStateExecutor::admitted_units() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return capacity_mode_ == CapacityMode::TotalAdmission ? admitted_unit_count_
                                                        : queued_task_count_;
}

/** @copydoc GraphStateExecutor::append_node_locked */
void GraphStateExecutor::append_node_locked(QueueNode* node) noexcept {
  node->next = nullptr;
  if (queue_tail_ == nullptr) {
    queue_head_ = node;
  } else {
    queue_tail_->next = node;
  }
  queue_tail_ = node;
  ++queued_task_count_;
}

/** @copydoc GraphStateExecutor::pop_node_locked */
GraphStateExecutor::QueueNode* GraphStateExecutor::pop_node_locked() noexcept {
  QueueNode* const node = queue_head_;
  if (node == nullptr) {
    return nullptr;
  }
  queue_head_ = node->next;
  if (queue_head_ == nullptr) {
    queue_tail_ = nullptr;
  }
  node->next = nullptr;
  --queued_task_count_;
  return node;
}

/** @copydoc GraphStateExecutor::oldest_close_turn_locked */
GraphStateExecutor::ContinuationState*
GraphStateExecutor::oldest_close_turn_locked() noexcept {
  ContinuationState* oldest = nullptr;
  for (auto& [identity, ticket] : continuations_) {
    (void)identity;
    if (ticket->phase == ContinuationPhase::Parked &&
        ticket->close_turn_pending &&
        (oldest == nullptr || ticket->id < oldest->id)) {
      oldest = ticket.get();
    }
  }
  return oldest;
}

/** @copydoc GraphStateExecutor::retire_continuation_locked */
void GraphStateExecutor::retire_continuation_locked(
    ContinuationState& ticket) noexcept {
  if (ticket.phase == ContinuationPhase::Retired) {
    return;
  }
  ticket.phase = ContinuationPhase::Retired;
  ticket.wake_pending = false;
  ticket.retire_pending = false;
  ticket.close_turn_pending = false;
  continuations_.erase(ticket.id);
  if (admitted_unit_count_ == 0) {
    std::terminate();
  }
  --admitted_unit_count_;
}

/** @copydoc GraphStateExecutor::finish_continuation_turn_locked */
void GraphStateExecutor::finish_continuation_turn_locked(
    ContinuationState& ticket, ContinuationAction action) noexcept {
  if (ticket.retire_pending || action == ContinuationAction::Retire) {
    retire_continuation_locked(ticket);
    return;
  }
  if (ticket.wake_pending || action == ContinuationAction::Queue) {
    ticket.wake_pending = false;
    if (state_ == State::Accepting) {
      ticket.phase = ContinuationPhase::Queued;
      append_node_locked(&ticket.node);
    } else {
      ticket.phase = ContinuationPhase::Parked;
      ticket.close_turn_pending = true;
    }
    return;
  }
  ticket.phase = ContinuationPhase::Parked;
}

/** @copydoc GraphStateExecutor::wake_continuation */
bool GraphStateExecutor::wake_continuation(
    const std::shared_ptr<ContinuationState>& ticket) {
  bool notify_worker = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = continuations_.find(ticket->id);
    if (state_ != State::Accepting || found == continuations_.end() ||
        found->second != ticket ||
        ticket->phase == ContinuationPhase::Retired || ticket->retire_pending) {
      return false;
    }
    if (ticket->phase == ContinuationPhase::Parked) {
      ticket->phase = ContinuationPhase::Queued;
      append_node_locked(&ticket->node);
      notify_worker = true;
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
      publish_graph_state_executor_test_snapshot(
          testing::GraphStateExecutorTestEvent::TaskQueued, queue_capacity_,
          queued_task_count_, active_task_count_, worker_thread_count_,
          admitted_unit_count_);
#endif
    } else if (ticket->phase == ContinuationPhase::Running) {
      ticket->wake_pending = true;
    }
  }
  if (notify_worker) {
    work_available_.notify_one();
  }
  return true;
}

/** @copydoc GraphStateExecutor::resume_continuation */
bool GraphStateExecutor::resume_continuation(
    const std::shared_ptr<ContinuationState>& ticket) {
  bool notify_worker = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = continuations_.find(ticket->id);
    if (found == continuations_.end() || found->second != ticket ||
        ticket->phase == ContinuationPhase::Retired || ticket->retire_pending) {
      return false;
    }
    if (ticket->phase == ContinuationPhase::Parked) {
      if (state_ == State::Accepting) {
        ticket->phase = ContinuationPhase::Queued;
        append_node_locked(&ticket->node);
      } else {
        ticket->close_turn_pending = true;
      }
      notify_worker = true;
    } else if (ticket->phase == ContinuationPhase::Running) {
      ticket->wake_pending = true;
    }
  }
  if (notify_worker) {
    work_available_.notify_one();
  }
  return true;
}

/** @copydoc GraphStateExecutor::retire_continuation */
bool GraphStateExecutor::retire_continuation(
    const std::shared_ptr<ContinuationState>& ticket) {
  bool released_capacity = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = continuations_.find(ticket->id);
    if (found == continuations_.end() || found->second != ticket ||
        ticket->phase == ContinuationPhase::Retired || ticket->retire_pending) {
      return false;
    }
    if (ticket->phase == ContinuationPhase::Parked) {
      retire_continuation_locked(*ticket);
      released_capacity = true;
    } else {
      ticket->retire_pending = true;
      ticket->wake_pending = false;
      ticket->close_turn_pending = false;
    }
  }
  if (released_capacity) {
    space_available_.notify_all();
    state_changed_.notify_all();
    work_available_.notify_all();
  }
  return true;
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
        queued_task_count_, active_task_count_, worker_thread_count_,
        admitted_unit_count_);
#endif
  }
  while (true) {
    std::unique_ptr<QueueNode> one_shot_node;
    std::shared_ptr<ContinuationState> continuation;
    bool waiting_capacity_released = false;
    bool skip_continuation_callback = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_available_.wait(lock, [this] {
        return queue_head_ != nullptr ||
               (state_ != State::Accepting &&
                (oldest_close_turn_locked() != nullptr ||
                 admitted_unit_count_ == 0));
      });
      QueueNode* node = pop_node_locked();
      ContinuationState* close_turn = nullptr;
      if (node == nullptr && state_ != State::Accepting) {
        close_turn = oldest_close_turn_locked();
      }
      if (node == nullptr && close_turn == nullptr &&
          (capacity_mode_ != CapacityMode::TotalAdmission ||
           admitted_unit_count_ == 0)) {
        worker_thread_count_ = 0;
        state_ = State::WorkerStopped;
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
        publish_graph_state_executor_test_snapshot(
            testing::GraphStateExecutorTestEvent::WorkerStopped,
            queue_capacity_, queued_task_count_, active_task_count_,
            worker_thread_count_, admitted_unit_count_);
#endif
        state_changed_.notify_all();
        current_executor_ = nullptr;
        return;
      }

      if (close_turn != nullptr) {
        close_turn->close_turn_pending = false;
        close_turn->phase = ContinuationPhase::Running;
        continuation = continuations_.at(close_turn->id);
        skip_continuation_callback = close_turn->retire_pending;
      } else if (node->kind == QueueNode::Kind::Continuation) {
        ContinuationState* const ticket = node->continuation;
        ticket->phase = ContinuationPhase::Running;
        continuation = continuations_.at(ticket->id);
        skip_continuation_callback = ticket->retire_pending;
      } else {
        auto owner = one_shot_nodes_.find(node);
        if (owner == one_shot_nodes_.end()) {
          std::terminate();
        }
        one_shot_node = std::move(owner->second);
        one_shot_nodes_.erase(owner);
        waiting_capacity_released = capacity_mode_ == CapacityMode::WaitingOnly;
      }
      active_task_count_ = 1;
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
      publish_graph_state_executor_test_snapshot(
          testing::GraphStateExecutorTestEvent::TaskStarted, queue_capacity_,
          queued_task_count_, active_task_count_, worker_thread_count_,
          admitted_unit_count_);
#endif
    }
    if (waiting_capacity_released) {
      space_available_.notify_one();
    }

    ContinuationAction continuation_action = ContinuationAction::Park;
    if (continuation != nullptr) {
      if (skip_continuation_callback) {
        continuation_action = ContinuationAction::Retire;
      } else {
        try {
          continuation_action =
              continuation->callback(ContinuationTicket(this, continuation));
        } catch (...) {
          continuation_action = ContinuationAction::Retire;
        }
      }
    } else {
      one_shot_node->task();
    }

    bool total_capacity_released = false;
    bool continuation_queued = false;
    bool draining = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_task_count_ = 0;
      if (continuation != nullptr) {
        const std::size_t before = admitted_unit_count_;
        finish_continuation_turn_locked(*continuation, continuation_action);
        total_capacity_released = admitted_unit_count_ < before;
        continuation_queued =
            continuation->phase == ContinuationPhase::Queued ||
            continuation->close_turn_pending;
      } else if (capacity_mode_ == CapacityMode::TotalAdmission) {
        if (admitted_unit_count_ == 0) {
          std::terminate();
        }
        --admitted_unit_count_;
        total_capacity_released = true;
      }
      draining = state_ != State::Accepting;
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
      publish_graph_state_executor_test_snapshot(
          testing::GraphStateExecutorTestEvent::TaskFinished, queue_capacity_,
          queued_task_count_, active_task_count_, worker_thread_count_,
          admitted_unit_count_);
#endif
    }
    if (total_capacity_released) {
      space_available_.notify_all();
      state_changed_.notify_all();
    }
    if (continuation_queued || draining) {
      work_available_.notify_all();
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
