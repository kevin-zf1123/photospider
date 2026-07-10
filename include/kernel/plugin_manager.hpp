#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "plugin_loader.hpp"  // NOLINT(build/include_subdir)

namespace ps {

/**
 * @brief Owns operation plugin registrations and dynamic library handles.
 *
 * `PluginManager` is the kernel-facing owner for operation plugins. It loads
 * shared libraries through the operation plugin loader, records which operation
 * keys came from which plugin path, and keeps each library handle alive while
 * registered callbacks may still be invoked.
 *
 * Unload operations first remove plugin-owned keys from `OpRegistry`, restore
 * any previous implementation replaced by the plugin, then release the
 * retained library handle. Built-in operations are tracked as `"built-in"`
 * sources and are restored when an overriding plugin is unloaded.
 *
 * @note The manager is intentionally separate from scheduler plugin loading
 * because operation plugins register callbacks into `OpRegistry`, while
 * scheduler plugins own live `IScheduler` instances and destroy functions.
 */
class PluginManager {
 public:
  /**
   * @brief Unregisters plugin operations and releases handles at destruction.
   *
   * @throws Nothing. Cleanup uses only load-time keys/snapshots, in-place
   * callback/source swaps, erasure, and reverse-sequence traversal.
   * @note Every callback and metadata record is removed or restored before its
   * dynamic-library handle is released, even if global allocation is failing
   * while a `Kernel` or test fixture tears down.
   */
  ~PluginManager() noexcept;

  /**
   * @brief Loads operation plugins from directory patterns.
   *
   * @param dir_patterns Directories or trailing-star patterns to scan for
   * platform shared libraries.
   * @return Nothing.
   * @throws std::bad_alloc if path, result, registry, or plugin handle storage
   *         exhausts memory.
   * @throws std::overflow_error if successful-load sequence space is exhausted.
   * @throws std::filesystem_error when iteration fails for an existing
   * directory.
   * @note Errors are discarded by this convenience wrapper; callers needing
   * diagnostics should use `load_from_dirs_report`.
   */
  void load_from_dirs(const std::vector<std::string>& dir_patterns);

  /**
   * @brief Loads operation plugins and returns structured diagnostics.
   *
   * @param dir_patterns Directories or trailing-star patterns to scan.
   * @return Load result containing attempted files, successful libraries,
   * keys registered or replaced by plugins, and per-plugin errors.
   * @throws std::bad_alloc if path, result, registry, or plugin handle storage
   *         exhausts memory.
   * @throws std::overflow_error if successful-load sequence space is exhausted.
   * @throws std::filesystem_error when iteration fails for an existing
   * directory.
   * @note Successful plugin handles are retained by this manager until the
   * plugin is unloaded or the manager is destroyed. A candidate allocation
   * failure leaves registry, source map, report prefix, and handle map
   * unchanged for that candidate; previously committed candidates remain
   * loaded.
   */
  PluginLoadResult load_from_dirs_report(
      const std::vector<std::string>& dir_patterns);

  /**
   * @brief Records current built-in operation keys as `"built-in"` sources.
   *
   * @return Nothing.
   * @throws std::bad_alloc if built-in registration or source-map population
   *         exhausts memory.
   * @throws Exceptions from source-map population after best-effort built-in
   *         registration; non-resource registration failures are ignored.
   * @note This method does not load dynamic libraries and does not overwrite
   * existing plugin source entries.
   */
  void seed_builtins_from_registry();

  /**
   * @brief Unloads operation keys registered or replaced by one plugin path.
   *
   * @param absolute_plugin_path Plugin path to unload. Relative inputs are
   * normalized with `std::filesystem::absolute` for convenience.
   * @return Number of operation keys removed from `OpRegistry`.
   * @throws std::bad_alloc if a relative or non-normalized path requires
   * convenience normalization and that normalization cannot allocate. No
   * manager state has changed when this exception propagates.
   * @throws std::filesystem_error when path normalization fails.
   * @note Passing the exact absolute key recorded at load time takes a direct,
   * allocation-free lookup and cleanup path. The retained dynamic library
   * handle is released only after registry callbacks from that plugin have
   * been removed and previous callbacks, if any, have been restored. If the
   * plugin was already shadowed by a later plugin and owns no active keys,
   * dependent restoration snapshots are still cleared before releasing the
   * handle.
   */
  int unload_by_plugin_path(const std::string& absolute_plugin_path);

  /**
   * @brief Unloads all dynamic operation plugins while preserving built-ins.
   *
   * @return Number of operation keys removed from `OpRegistry`.
   * @throws Nothing. Unload consumes keys, source snapshots, registry
   * snapshots, and load sequences allocated during successful load.
   * @note Plugins are unwound in reverse successful-load order. Each plugin's
   * callbacks and metadata are removed or restored before its retained handle
   * is released, including during global allocation failure and destruction.
   */
  int unload_all_plugins() noexcept;

  /**
   * @brief Returns registered operation keys and their source labels.
   *
   * @return Map from operation key to `"built-in"` or absolute plugin path.
   * @throws Nothing.
   * @note The returned reference remains valid until the next mutating
   * `PluginManager` call.
   */
  const std::map<std::string, std::string>& op_sources() const {
    return op_sources_;
  }

  /**
   * @brief Returns the number of retained dynamic operation plugin handles.
   *
   * @return Count of entries in the manager-owned handle map.
   * @throws Nothing.
   * @note This is a narrow inspection API used by tests and diagnostics to
   * verify handle ownership.
   */
  size_t loaded_plugin_count() const noexcept { return loaded_plugins_.size(); }

 private:
  /**
   * @brief Operation source map shown to frontends and unload code.
   *
   * Keys use `type:subtype`; values are either `"built-in"` or absolute plugin
   * paths. The map does not own dynamic libraries.
   */
  std::map<std::string, std::string> op_sources_;

  /**
   * @brief Manager-owned dynamic library handles keyed by absolute path.
   *
   * Each value also records the operation keys discovered during registration
   * so unload can remove callbacks before releasing the final handle reference.
   */
  LoadedOpPluginMap loaded_plugins_;
};

}  // namespace ps
