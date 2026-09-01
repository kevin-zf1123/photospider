#pragma once

#include <cstdint>

#include "photospider/core/export.hpp"

namespace ps::plugin_testing {

/** @brief Closed native-library kind vocabulary for private lifecycle tests. */
enum class LibraryKind : std::uint32_t {
  /** @brief Operation ABI v2 library. */
  Operation = 1U,
  /** @brief Data-provider ABI v1 library. */
  Provider = 2U,
};

/** @brief Callback invoked immediately before heap owner allocation. */
using BeforeOwnerAllocationHook = void (*)(LibraryKind kind);

/** @brief Nonthrowing callback invoked after one native close call. */
using NativeCloseHook = void (*)(LibraryKind kind) noexcept;

/**
 * @brief Private deterministic callbacks for native-library lifecycle tests.
 *
 * @note This structure is not installed and has no effect unless a test
 * explicitly installs one process-global callback set.
 */
struct LibraryTestHooks final {
  /** @brief Optional owner-allocation boundary callback. */
  BeforeOwnerAllocationHook before_owner_allocation = nullptr;
  /** @brief Optional native close-call observer. */
  NativeCloseHook native_close = nullptr;
};

/**
 * @brief Installs or clears the private process-global lifecycle callbacks.
 * @param hooks Borrowed callback set, or null to clear.
 * @throws Nothing.
 * @note The caller keeps a nonnull set alive and immutable until clearing it;
 * tests must not install competing sets concurrently.
 */
PHOTOSPIDER_API void install_library_test_hooks(
    const LibraryTestHooks* hooks) noexcept;

/**
 * @brief Invokes the installed owner-allocation boundary callback.
 * @param kind Exact library kind approaching heap owner allocation.
 * @throws Any exception deliberately raised by the installed test callback.
 * @note Production builds without `BUILD_TESTING` never call this function.
 */
PHOTOSPIDER_API void invoke_before_owner_allocation(LibraryKind kind);

/**
 * @brief Notifies the installed observer after one native close call.
 * @param kind Exact closed library kind.
 * @throws Nothing.
 * @note The observer counts close calls, not operating-system return codes.
 */
PHOTOSPIDER_API void notify_native_close(LibraryKind kind) noexcept;

}  // namespace ps::plugin_testing
