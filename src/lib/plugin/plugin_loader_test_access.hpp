#pragma once

#if !defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
#error "plugin_loader_test_access.hpp is available only in BUILD_TESTING builds"
#endif

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>

#include "plugin/plugin_loader.hpp"  // NOLINT(build/include_subdir)

namespace ps::testing {

/**
 * @brief Post-registration allocation sites exposed to deterministic tests.
 *
 * Each value identifies one bookkeeping boundary after the real operation
 * plugin registrar has returned. Production builds do not compile this type or
 * its failpoint implementation.
 */
enum class OperationPluginLoadFailpoint {
  /** @brief No injected failure. */
  None,
  /** @brief Fail before collecting the previous source-map state. */
  PreviousSource,
  /** @brief Fail before recording staged source/result entries. */
  SourceAndResult,
  /** @brief Fail before transferring captured registry snapshots. */
  Snapshot,
  /** @brief Fail immediately before staging the retained handle record. */
  HandleCommit,
};

/**
 * @brief Arms one thread-local operation-plugin loader failpoint.
 *
 * @param failpoint Bookkeeping boundary that will throw `std::bad_alloc`.
 * @return Nothing.
 * @throws Nothing.
 * @note The failpoint is consumed only by the calling thread. Arming it resets
 * the hit counter so tests can prove the requested boundary was reached.
 */
void set_operation_plugin_load_failpoint(
    OperationPluginLoadFailpoint failpoint) noexcept;

/**
 * @brief Returns how many times the armed loader failpoint was reached.
 *
 * @return Thread-local hit count since the last failpoint installation.
 * @throws Nothing.
 * @note A deterministic failure test must observe exactly one hit; merely
 * catching an unrelated `std::bad_alloc` is not sufficient evidence.
 */
std::size_t operation_plugin_load_failpoint_hits() noexcept;

/**
 * @brief Exercises the real one-file operation-plugin load transaction.
 *
 * @param path Real dynamic-library fixture path.
 * @param op_sources Mutable source map owned by the test harness.
 * @param loaded_plugins Mutable retained-handle map owned by the test harness.
 * @param result Mutable accumulated result whose strong guarantee is audited.
 * @return Nothing.
 * @throws std::bad_alloc from the selected failpoint or real allocation.
 * @throws std::filesystem::filesystem_error if absolute-path normalization
 *         fails.
 * @note This is a BUILD_TESTING-only internal seam over the same
 * `load_one_plugin` implementation used by `PluginManager`; it does not alter
 * the public or plugin ABI.
 */
void load_one_operation_plugin_for_testing(
    const std::filesystem::path& path,
    std::map<std::string, std::string>& op_sources,
    LoadedOpPluginMap& loaded_plugins, PluginLoadResult& result);

}  // namespace ps::testing
