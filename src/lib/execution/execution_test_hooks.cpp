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

}  // namespace ps::execution_testing
