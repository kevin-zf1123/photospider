#pragma once

namespace ps::testing {

/**
 * @brief Required-target checkpoint available only to Kernel concurrency tests.
 *
 * @throws Nothing for value construction and comparison.
 * @note Production builds compile out this test-access contract completely.
 */
enum class RequiredTargetTestEvent {
  /** @brief set_node_yaml resolved its required existing node. */
  SetNodeYamlTargetResolved,
  /** @brief Forward ROI projection resolved both required endpoints. */
  ForwardRoiEndpointsResolved,
  /** @brief Backward ROI projection resolved both required endpoints. */
  BackwardRoiEndpointsResolved,
};

/**
 * @brief Borrowed synchronization hook for one required-target checkpoint.
 *
 * @throws Nothing for aggregate construction.
 * @note The callback may wait on test-owned synchronization, but must not
 * re-enter Host, Kernel, or GraphStateExecutor and must contain all failures.
 */
struct RequiredTargetTestHook {
  /** @brief Borrowed test context that outlives the installed hook. */
  void* context = nullptr;

  /**
   * @brief Observes and optionally blocks one resolved-target checkpoint.
   * @param context Borrowed context supplied by the installing test.
   * @param event Checkpoint reached inside one graph-state work item.
   * @return Nothing.
   * @throws Nothing; implementations must contain every exception.
   */
  void (*wait)(void* context, RequiredTargetTestEvent event) noexcept = nullptr;
};

/**
 * @brief Installs or clears the process-local required-target test hook.
 *
 * @param hook Borrowed hook that outlives every in-flight callback, or nullptr.
 * @return Nothing.
 * @throws Nothing.
 * @note Tests serialize installation and clear the hook after joining all
 * affected operations.
 */
void set_required_target_test_hook(const RequiredTargetTestHook* hook) noexcept;

/**
 * @brief Publishes a resolved-target checkpoint to the installed hook.
 *
 * @param event Checkpoint reached while GraphStateExecutor remains locked.
 * @return Nothing.
 * @throws Nothing; callback failures are contained by its contract.
 */
void notify_required_target_test_hook(RequiredTargetTestEvent event) noexcept;

}  // namespace ps::testing
