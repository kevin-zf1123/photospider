#include "execution/execution_task_runtime.hpp"

#include <exception>
#include <memory>
#include <utility>

namespace ps {
namespace {

/**
 * @brief Adapts a single-batch runtime's any-thread route to ReadyFence.
 *
 * @throws Nothing from submission and destruction.
 * @note The runtime outlives every callback because the task plan increments
 * completion before registering the fence wait and its caller waits for
 * settlement. Multi-Run services replace this adapter with exact Run
 * retention.
 */
class RuntimeReadyFenceExecutor final : public ReadyFenceExecutor {
 public:
  /**
   * @brief Borrows one active runtime through its matching batch wait.
   * @param runtime Runtime whose any-thread queue owns admitted callbacks.
   * @throws Nothing.
   */
  explicit RuntimeReadyFenceExecutor(ExecutionTaskRuntime& runtime) noexcept
      : runtime_(&runtime) {}

  /** @copydoc ReadyFenceExecutor::submit */
  void submit(Task task) noexcept override {
    try {
      runtime_->submit_ready_task_any_thread(std::move(task));
    } catch (...) {
      try {
        runtime_->set_exception(std::current_exception());
      } catch (...) {
      }
    }
  }

 private:
  /** @brief Borrowed runtime retained by its active completion interval. */
  ExecutionTaskRuntime* runtime_ = nullptr;
};

}  // namespace

/** @copydoc ExecutionTaskRuntime::make_ready_fence_executor */
std::shared_ptr<ReadyFenceExecutor>
ExecutionTaskRuntime::make_ready_fence_executor() {
  return std::make_shared<RuntimeReadyFenceExecutor>(*this);
}

}  // namespace ps
