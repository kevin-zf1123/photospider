#include "execution/execution_test_hooks.hpp"

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
bool fail_failure_status_construction() noexcept {
  const ExecutionTestHooks* hooks = g_hooks.load(std::memory_order_acquire);
  if (!hooks || !hooks->fail_failure_status_construction) {
    return false;
  }
  try {
    return hooks->fail_failure_status_construction();
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

}  // namespace ps::execution_testing
