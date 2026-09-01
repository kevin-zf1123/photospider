#include "plugin/library_test_hooks.hpp"

#include <atomic>

namespace ps::plugin_testing {
namespace {

/** @brief Borrowed active callback set, or null outside one scoped test. */
std::atomic<const LibraryTestHooks*> g_hooks{nullptr};

}  // namespace

/**
 * @brief Implements private lifecycle callback installation.
 * @copydetails install_library_test_hooks
 */
void install_library_test_hooks(const LibraryTestHooks* hooks) noexcept {
  g_hooks.store(hooks, std::memory_order_release);
}

/**
 * @brief Implements deterministic owner-allocation boundary injection.
 * @copydetails invoke_before_owner_allocation
 */
void invoke_before_owner_allocation(LibraryKind kind) {
  const LibraryTestHooks* hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->before_owner_allocation) {
    hooks->before_owner_allocation(kind);
  }
}

/**
 * @brief Implements exact native close-call observation.
 * @copydetails notify_native_close
 */
void notify_native_close(LibraryKind kind) noexcept {
  const LibraryTestHooks* hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->native_close) {
    hooks->native_close(kind);
  }
}

/**
 * @brief Implements copied-provider-schema retirement observation.
 * @copydetails notify_provider_schemas_retired
 */
void notify_provider_schemas_retired(std::size_t count) noexcept {
  const LibraryTestHooks* hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->provider_schemas_retired) {
    hooks->provider_schemas_retired(count);
  }
}

}  // namespace ps::plugin_testing
