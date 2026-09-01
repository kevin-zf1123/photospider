#pragma once

#include "photospider/plugin/operation_registry.hpp"

namespace ps::execution_testing {

/**
 * @brief Callback invoked after one backend callback enters its waiting queue.
 * @param backend Exact CPU or optional GPU lane holding the callback.
 * @return No value.
 * @throws Any test exception, which the notification boundary fences.
 * @note The pool mutex remains held so the matching worker cannot pop until the
 * callback returns; use only for deterministic noninstalled regressions.
 */
using CallbackQueuedHook = void (*)(Backend backend);

/**
 * @brief Private callback set for execution queue-admission regressions.
 *
 * @note This type exists only in the noninstalled test-kernel variant and has
 * no public ABI, product archive, export, or package-consumer effect.
 */
struct ExecutionTestHooks final {
  /** @brief Optional callback-waiting boundary observer. */
  CallbackQueuedHook callback_queued = nullptr;
};

/**
 * @brief Installs or clears the private process-global execution callbacks.
 * @param hooks Borrowed immutable callback set, or null to clear.
 * @return No value.
 * @throws Nothing.
 * @note The caller keeps a nonnull set alive until clearing it and must not
 * install competing sets concurrently.
 */
void install_execution_test_hooks(const ExecutionTestHooks* hooks) noexcept;

/**
 * @brief Notifies the installed callback-waiting boundary observer.
 * @param backend Exact backend lane whose queue accepted one callback.
 * @return No value.
 * @throws Nothing; every test exception is fenced.
 * @note Only the noninstalled test-kernel variant invokes this function.
 */
void notify_callback_queued(Backend backend) noexcept;

}  // namespace ps::execution_testing
