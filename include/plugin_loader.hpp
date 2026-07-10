#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "kernel/plugin_result.hpp"

namespace ps {

class PluginManager;

/**
 * @brief Unforgeable capability for mutating process-owned plugin state.
 *
 * @throws Nothing.
 * @note Only `PluginManager` can construct this token. The low-level loader
 *       therefore cannot publish callbacks into the process `OpRegistry` with
 *       a caller-owned source/handle map in production code.
 */
class ProcessPluginOwnerToken final {
 private:
  /**
   * @brief Creates one loader capability for the unique process owner.
   *
   * @throws Nothing.
   * @note The token carries no state and is valid only as evidence that the
   * call originated inside `PluginManager`; external code cannot construct it.
   */
  ProcessPluginOwnerToken() noexcept = default;

  /**
   * @brief Grants only the process plugin owner access to token construction.
   *
   * @note Friendship exposes no loader-owned data; it only permits construction
   *       at the serialized production call site.
   */
  friend class PluginManager;
};

/**
 * @brief Retained lifetime information for one loaded operation plugin.
 *
 * The dynamic library handle is held by `library` and is closed by its custom
 * deleter when the last owner releases it. `registered_keys` records the
 * operation keys touched by the plugin registration entry point, including
 * replacements of existing keys.
 *
 * @note The operation registry stores callable objects that may refer to code
 * inside the plugin library. The process `PluginManager` removes or restores
 * `registered_keys` before releasing its `library` reference. Copied callback
 * and `NodeOutput` leases may defer final unmapping beyond registry removal.
 */
struct LoadedOpPlugin {
  /**
   * @brief Shared dynamic library lifetime retained while callbacks exist.
   *
   * @note This member is declared first and therefore destroyed last, after
   *       callback-bearing predecessor snapshots and keys/source strings.
   */
  std::shared_ptr<void> library;
  /**
   * @brief Monotonic order assigned when this plugin became visible.
   *
   * Higher values represent later successful loads. `PluginManager` consumes
   * this value in descending order so an override chain is unwound from newest
   * callback to oldest callback and finally to the built-in implementation.
   */
  std::uint64_t load_sequence = 0;
  /**
   * @brief Operation keys registered or replaced by this plugin.
   *
   * @note Keys are precomputed during load so explicit unload never needs to
   *       allocate a temporary registry inventory.
   */
  std::vector<std::string> registered_keys;
  /**
   * @brief Registry state that existed before plugin registration.
   *
   * Empty snapshot fields mean either no predecessor existed for that slot or
   * the registrar did not replace it. Populated fields are restored only when
   * the matching plugin-owned revision remains active at unload.
   */
  std::unordered_map<std::string, OpRegistry::RegistryEntrySnapshot>
      previous_registry_entries;
  /**
   * @brief Slot revisions actually published by this plugin registration.
   *
   * Scalar fields identify the final callback/metadata revision written by the
   * registrar. Device token vectors contain only elements appended by this
   * plugin. Unload compares these stable tokens with active registry ownership
   * instead of comparing callable targets.
   */
  std::unordered_map<std::string, OpRegistry::RegistryEntryOwnership>
      owned_registry_entries;
  /**
   * @brief Preallocated empty slots that receive callbacks during unload.
   *
   * The loader allocates one placeholder for every plugin-owned callback or
   * device implementation before publication. Allocation-independent unload
   * swaps retiring state into these snapshots, releases the registry lock, and
   * only then destroys callback objects and the retained library.
   */
  std::unordered_map<std::string, OpRegistry::RegistryEntrySnapshot>
      retirement_registry_entries;
  /**
   * @brief Source-map state that existed before plugin registration.
   *
   * A null optional means no source entry existed. A populated string is
   * restored after the plugin's callbacks are removed.
   */
  std::map<std::string, std::optional<std::string>> previous_sources;
};

/**
 * @brief Absolute plugin path to retained operation plugin handle.
 *
 * The key is the absolute shared-library path used by `PluginManager` when it
 * later unloads a plugin. The value keeps the library mapped for as long as
 * registered operation callbacks can be invoked.
 */
using LoadedOpPluginMap = std::map<std::string, LoadedOpPlugin>;

/**
 * @brief Scans operation plugin directories for the unique process owner.
 *
 * This internal boundary scans platform shared libraries, invokes the versioned
 * host-provided registrar, and stores every successful source, restoration
 * snapshot, and retained handle in maps owned by the unique process
 * `PluginManager`. Its capability token prevents production callers from
 * creating a second source/handle/restoration owner around the process-global
 * `OpRegistry`.
 *
 * @param owner_token Capability constructible only by `PluginManager`.
 * @param plugin_dir_paths Directories or suffix patterns to scan.
 * @param op_sources Map updated with registered or replaced operation sources.
 * @param loaded_plugins Absolute plugin path to retained handle map owned by
 * the process manager. Already-loaded paths in this map are skipped.
 * @return Structured load result for the scan.
 * @throws std::bad_alloc unchanged if candidate staging cannot allocate. The
 * failing candidate leaves registry, sources, result, and handles unchanged.
 * @throws std::overflow_error if successful-load sequence space is exhausted.
 * @throws std::filesystem::filesystem_error when directory iteration fails for
 * an existing plugin directory.
 * @note `PluginManager` serializes this complete call with process mutation and
 * registry publication. The BUILD_TESTING-only single-candidate seam remains
 * separate so failure tests cannot become a production ownership path.
 */
PluginLoadResult load_plugins_for_process_owner(
    ProcessPluginOwnerToken owner_token,
    const std::vector<std::string>& plugin_dir_paths,
    std::map<std::string, std::string>& op_sources,
    LoadedOpPluginMap& loaded_plugins);

}  // namespace ps
