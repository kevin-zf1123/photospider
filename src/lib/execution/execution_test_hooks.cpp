#include "execution/execution_test_hooks.hpp"

#include <algorithm>
#include <atomic>

namespace ps::execution_testing {
namespace {

/** @brief Borrowed active callback set, or null outside one scoped test. */
std::atomic<const ExecutionTestHooks*> g_hooks{nullptr};

}  // namespace

/**
 * @brief Implements private execution callback installation.
 * @copydetails install_execution_test_hooks
 */
void install_execution_test_hooks(const ExecutionTestHooks* hooks) noexcept {
  g_hooks.store(hooks, std::memory_order_release);
}

ExecutionTimingHook execution_timing_hook() noexcept {
  const auto* hooks = g_hooks.load(std::memory_order_acquire);
  return hooks ? hooks->execution_timing : nullptr;
}

bool use_native_device() noexcept {
  const ExecutionTestHooks* hooks = g_hooks.load(std::memory_order_acquire);
  return hooks && hooks->native_device;
}

/**
 * @brief Implements the callback-waiting boundary notification fence.
 * @copydetails notify_callback_queued
 */
void notify_callback_queued(Backend backend) noexcept {
  const ExecutionTestHooks* hooks = g_hooks.load(std::memory_order_acquire);
  if (!hooks || !hooks->callback_queued) {
    return;
  }
  try {
    hooks->callback_queued(backend);
  } catch (...) {
  }
}

/**
 * @brief Implements the scheduler failure pre-commit notification fence.
 * @copydetails notify_before_scheduler_failure
 */
void notify_before_scheduler_failure(SchedulerFailurePoint point) noexcept {
  const ExecutionTestHooks* hooks = g_hooks.load(std::memory_order_acquire);
  if (!hooks || !hooks->before_scheduler_failure) {
    return;
  }
  try {
    hooks->before_scheduler_failure(point);
  } catch (...) {
  }
}

/**
 * @brief Implements deterministic queue-submission action selection.
 * @copydetails callback_submit_action
 */
CallbackSubmitAction callback_submit_action(Backend backend) noexcept {
  const ExecutionTestHooks* hooks = g_hooks.load(std::memory_order_acquire);
  if (!hooks || !hooks->callback_submit_action) {
    return CallbackSubmitAction::Proceed;
  }
  try {
    return hooks->callback_submit_action(backend);
  } catch (...) {
    return CallbackSubmitAction::Proceed;
  }
}

/**
 * @brief Implements protected diagnostic/Status construction failure selection.
 * @copydetails fail_failure_status_construction
 */
bool fail_failure_status_construction(
    FailureStatusConstructionPoint point) noexcept {
  const ExecutionTestHooks* hooks = g_hooks.load(std::memory_order_acquire);
  if (!hooks || !hooks->fail_failure_status_construction) {
    return false;
  }
  try {
    return hooks->fail_failure_status_construction(point);
  } catch (...) {
    return false;
  }
}

/**
 * @brief Implements final result-publication boundary notification.
 * @copydetails notify_final_result_ready
 */
void notify_final_result_ready() noexcept {
  const ExecutionTestHooks* hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->final_result_ready) {
    hooks->final_result_ready();
  }
}

/**
 * @brief Implements the post-submit external-stop observation notification.
 * @copydetails notify_post_submit_observation
 */
void notify_post_submit_observation() noexcept {
  const ExecutionTestHooks* hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->post_submit_observation) {
    hooks->post_submit_observation();
  }
}

/** @brief Implements the private callback body/owner retirement boundary. */
void notify_callback_body_finished() noexcept {
  const ExecutionTestHooks* hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->callback_body_finished)
    hooks->callback_body_finished();
}
/** @brief Dispatches the private pre-adoption handoff fault hook. */
void notify_structured_handoff_ready() {
  const auto* hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->structured_handoff_ready)
    hooks->structured_handoff_ready();
}

/** @brief Dispatches the post-I/O, pre-retention test failure hook. */
void notify_structured_io_completed() {
  const auto* hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->structured_io_completed)
    hooks->structured_io_completed();
}

/** @brief Implements the private successful-checkpoint publication boundary. */
void notify_checkpoint_published() noexcept {
  const ExecutionTestHooks* hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->checkpoint_published)
    hooks->checkpoint_published();
}

/** @brief Implements the private checkpoint lookup boundary. */
void notify_checkpoint_borrowed() noexcept {
  const ExecutionTestHooks* hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->checkpoint_borrowed)
    hooks->checkpoint_borrowed();
}

void notify_shared_joined(std::string_view operation, std::uint64_t node,
                          const void* actor) noexcept {
  const auto* hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->shared_joined)
    hooks->shared_joined(hooks->structured_context, operation, node, actor);
}
void notify_phase_work_started(const PhaseWorkPoint& point) noexcept {
  const auto* hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->phase_work_started)
    hooks->phase_work_started(hooks->structured_context, point);
}
void notify_phase_work_finished(const PhaseWorkPoint& point,
                                ErrorCode status) noexcept {
  const auto* hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->phase_work_finished)
    hooks->phase_work_finished(hooks->structured_context, point, status);
}

void notify_native_submitted() noexcept {
  const ExecutionTestHooks* hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->native_submitted)
    hooks->native_submitted();
}
bool fail_native_allocation(std::uint32_t checkpoint) noexcept {
  const auto* hooks = g_hooks.load(std::memory_order_acquire);
  return hooks && hooks->native_allocation_failure == checkpoint;
}
std::uint32_t native_allocation_limit(std::uint32_t physical) noexcept {
  const auto* hooks = g_hooks.load(std::memory_order_acquire);
  return hooks && hooks->native_allocation_limit
             ? std::min(physical, hooks->native_allocation_limit)
             : physical;
}
void notify_native_memory_freed() noexcept {
  const auto* hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->native_memory_freed)
    hooks->native_memory_freed();
}

ErrorCode native_capacity_error() noexcept {
  const auto* hooks = g_hooks.load(std::memory_order_acquire);
  return hooks && hooks->native_capacity_error ? hooks->native_capacity_error()
                                               : ErrorCode::Ok;
}

}  // namespace ps::execution_testing
