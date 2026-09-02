#pragma once

#include <cstdint>

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
 * @brief Scheduler-owned failure boundaries exposed to deterministic tests.
 *
 * @note Values identify only noninstalled test-kernel observation points; they
 * do not extend the public execution status or installed ABI.
 */
enum class SchedulerFailurePoint : std::uint8_t {
  /** @brief A selected GPU step has no local GPU lane and cannot fall back. */
  GpuBackendUnavailable,
  /** @brief Context-wide waiting-callback admission rejected one attempt. */
  WaitingAdmissionRejected,
  /** @brief A backend FIFO rejected one callback before ownership transfer. */
  CallbackSubmitRejected,
  /** @brief Callback submission raised before transferring queue ownership. */
  CallbackSubmitException,
};

/**
 * @brief Deterministic test action applied at backend FIFO submission.
 *
 * @note Production builds never compile or observe this vocabulary.
 */
enum class CallbackSubmitAction : std::uint8_t {
  /** @brief Preserve ordinary backend queue submission. */
  Proceed,
  /** @brief Return the same false rejection as a stopped backend queue. */
  Reject,
  /** @brief Raise `std::bad_alloc` before backend queue mutation. */
  ThrowBadAlloc,
};

/**
 * @brief Callback invoked immediately before one scheduler failure is saved.
 * @param point Exact scheduler-owned failure boundary.
 * @return No value.
 * @throws Any test exception, which the notification boundary fences.
 * @note Tests use this point to make cancellation or graph replacement win
 * before the Run takes its first-failure lock.
 */
using BeforeSchedulerFailureHook = void (*)(SchedulerFailurePoint point);

/**
 * @brief Selects one deterministic backend queue-submission test action.
 * @param backend Exact CPU or optional GPU queue being submitted.
 * @return Proceed, reject, or raise allocation failure before queue mutation.
 * @throws Any test exception, which is fenced and treated as Proceed.
 */
using CallbackSubmitActionHook = CallbackSubmitAction (*)(Backend backend);

/**
 * @brief Selects deterministic failure of exception-status construction.
 * @return True to enter the allocation-free `finish_failure_safely` fallback.
 * @throws Any test exception, which is fenced and treated as false.
 */
using FailFailureStatusConstructionHook = bool (*)();

/**
 * @brief Private callback set for execution queue-admission regressions.
 *
 * @note This type exists only in the noninstalled test-kernel variant and has
 * no public ABI, product archive, export, or package-consumer effect.
 */
struct ExecutionTestHooks final {
  /** @brief Optional callback-waiting boundary observer. */
  CallbackQueuedHook callback_queued = nullptr;
  /** @brief Optional scheduler failure pre-commit observer. */
  BeforeSchedulerFailureHook before_scheduler_failure = nullptr;
  /** @brief Optional deterministic queue submission controller. */
  CallbackSubmitActionHook callback_submit_action = nullptr;
  /** @brief Optional exception-fallback construction controller. */
  FailFailureStatusConstructionHook fail_failure_status_construction = nullptr;
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

/**
 * @brief Notifies the installed scheduler failure pre-commit observer.
 * @param point Exact failure boundary about to be recorded.
 * @return No value.
 * @throws Nothing; every test exception is fenced.
 * @note Only the noninstalled test-kernel variant invokes this function.
 */
void notify_before_scheduler_failure(SchedulerFailurePoint point) noexcept;

/**
 * @brief Returns the installed deterministic queue-submission action.
 * @param backend Exact lane receiving a callback.
 * @return Requested action, or Proceed when no hook is installed or it throws.
 * @throws Nothing.
 * @note Only the noninstalled test-kernel variant invokes this function.
 */
CallbackSubmitAction callback_submit_action(Backend backend) noexcept;

/**
 * @brief Reports whether failure-status construction must fail in this test.
 * @return True only when the installed hook requests the allocation fallback.
 * @throws Nothing.
 * @note Only the noninstalled test-kernel variant invokes this function.
 */
bool fail_failure_status_construction() noexcept;

}  // namespace ps::execution_testing
