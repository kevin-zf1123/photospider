#include "execution/execution_test_hooks.hpp"

#include <algorithm>
#include <atomic>

namespace ps::execution_testing {
namespace {
std::atomic<const ExecutionTestHooks*> g_hooks{nullptr};
}  // namespace

void install_execution_test_hooks(const ExecutionTestHooks* hooks) noexcept {
  g_hooks.store(hooks, std::memory_order_release);
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
