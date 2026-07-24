#pragma once

namespace ps::testing {

/**
 * @brief Deterministic checkpoints from the private Kernel staged-commit path.
 * @throws Nothing for value construction and comparison.
 * @note This contract exists only in BUILD_TESTING product builds and is not
 *       installed or exposed through Host.
 */
enum class KernelComputeCommitTestEvent {
  /** @brief HP publication copies are ready before graph-state submission. */
  HighPrecisionCommitPrepared,
  /** @brief HP live predicate passed before the no-throw visible swap. */
  HighPrecisionPredicateValidated,
  /**
   * @brief HP visible publication and Run success completed after graph-state
   * ownership was released but before the service callback returned.
   */
  HighPrecisionCommitCompleted,
  /** @brief RT proxy publication copy is ready before graph-state submission.
   */
  RealTimeCommitPrepared,
  /** @brief RT live predicate passed before the no-throw proxy swap. */
  RealTimePredicateValidated,
  /** @brief RT proxy publication completed while commit ownership is held. */
  RealTimePublished,
};

/**
 * @brief Borrowed observer for deterministic staged-commit concurrency tests.
 * @throws Nothing for aggregate construction.
 * @note Tests serialize installation and retain the hook and context until all
 *       affected compute requests have settled. A callback may block to create
 *       a deterministic publication window but must remain noexcept and must
 *       not re-enter the same graph-state lane from its worker thread.
 */
struct KernelComputeCommitTestHook {
  /** @brief Borrowed test context that outlives the installed hook. */
  void* context = nullptr;

  /**
   * @brief Observes one exact staged-commit checkpoint.
   * @param context Borrowed context supplied by the installing test.
   * @param event Exact checkpoint reached by the product commit policy.
   * @return Nothing.
   * @throws Nothing; callbacks must contain all failures.
   */
  void (*notify)(void* context,
                 KernelComputeCommitTestEvent event) noexcept = nullptr;
};

/**
 * @brief Installs or clears the process-local staged-commit observer.
 * @param hook Borrowed hook that outlives in-flight notifications, or nullptr.
 * @return Nothing.
 * @throws Nothing.
 */
void set_kernel_compute_commit_test_hook(
    const KernelComputeCommitTestHook* hook) noexcept;

/**
 * @brief Publishes one staged-commit checkpoint to the installed observer.
 * @param event Exact checkpoint reached by the product commit policy.
 * @return Nothing.
 * @throws Nothing.
 */
void notify_kernel_compute_commit_test_hook(
    KernelComputeCommitTestEvent event) noexcept;

}  // namespace ps::testing
