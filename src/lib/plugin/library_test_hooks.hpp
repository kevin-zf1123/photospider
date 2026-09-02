#pragma once

#include <cstddef>
#include <cstdint>

namespace ps::plugin_testing {

/** @brief Closed native-library kind vocabulary for private lifecycle tests. */
enum class LibraryKind : std::uint32_t {
  /** @brief Operation ABI v2 library. */
  Operation = 1U,
  /** @brief Data-provider ABI v1 library. */
  Provider = 2U,
};

/**
 * @brief Callback invoked immediately before heap owner allocation.
 * @param kind Exact library kind approaching heap owner allocation.
 * @return No value.
 * @throws Any exception deliberately raised by the installed test callback.
 * @note The callback runs only in the noninstalled test-kernel variant.
 */
using BeforeOwnerAllocationHook = void (*)(LibraryKind kind);

/**
 * @brief Nonthrowing callback invoked immediately before native loading.
 * @param kind Exact operation or provider library kind.
 * @return No value.
 * @throws Nothing.
 * @note The callback observes whether exact path validation reached the
 * platform loader and never receives or owns the path.
 */
using NativeLoadHook = void (*)(LibraryKind kind) noexcept;

/**
 * @brief Nonthrowing callback invoked after one native close call.
 * @param kind Exact closed library kind.
 * @return No value.
 * @throws Nothing.
 * @note The callback observes cleanup and never owns the native handle.
 */
using NativeCloseHook = void (*)(LibraryKind kind) noexcept;

/**
 * @brief Callback invoked after registry-owned provider schemas retire.
 * @param count Exact number of retired copied schemas.
 * @return No value.
 * @throws Nothing.
 * @note Provider leases remain alive while the callback runs.
 */
using ProviderSchemasRetiredHook = void (*)(std::size_t count) noexcept;

/**
 * @brief Private deterministic callbacks for native-library lifecycle tests.
 *
 * @note This structure is private to the noninstalled test-kernel variant and
 * has no effect unless a test explicitly installs one process-global callback
 * set.
 */
struct LibraryTestHooks final {
  /** @brief Optional owner-allocation boundary callback. */
  BeforeOwnerAllocationHook before_owner_allocation = nullptr;
  /** @brief Optional native-load attempt observer. */
  NativeLoadHook native_load = nullptr;
  /** @brief Optional native close-call observer. */
  NativeCloseHook native_close = nullptr;
  /** @brief Optional copied-provider-schema retirement observer. */
  ProviderSchemasRetiredHook provider_schemas_retired = nullptr;
};

/**
 * @brief Installs or clears the private process-global lifecycle callbacks.
 * @param hooks Borrowed callback set, or null to clear.
 * @return No value.
 * @throws Nothing.
 * @note The caller keeps a nonnull set alive and immutable until clearing it;
 * tests must not install competing sets concurrently.
 */
void install_library_test_hooks(const LibraryTestHooks* hooks) noexcept;

/**
 * @brief Invokes the installed owner-allocation boundary callback.
 * @param kind Exact library kind approaching heap owner allocation.
 * @return No value.
 * @throws Any exception deliberately raised by the installed test callback.
 * @note Only the noninstalled test-kernel variant calls this function.
 */
void invoke_before_owner_allocation(LibraryKind kind);

/**
 * @brief Notifies the installed observer before one platform load call.
 * @param kind Exact library kind about to reach the platform loader.
 * @return No value.
 * @throws Nothing.
 * @note Exact path validation has succeeded, but no native handle exists yet.
 */
void notify_native_load(LibraryKind kind) noexcept;

/**
 * @brief Notifies the installed observer after one native close call.
 * @param kind Exact closed library kind.
 * @return No value.
 * @throws Nothing.
 * @note The observer counts close calls, not operating-system return codes.
 */
void notify_native_close(LibraryKind kind) noexcept;

/**
 * @brief Notifies the installed observer after copied provider schemas retire.
 * @param count Exact number of registry-owned schemas cleared by destruction.
 * @return No value.
 * @throws Nothing.
 * @note Provider leases remain alive until after this callback returns.
 */
void notify_provider_schemas_retired(std::size_t count) noexcept;

}  // namespace ps::plugin_testing
