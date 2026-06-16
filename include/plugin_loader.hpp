#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "kernel/plugin_result.hpp"

namespace ps {

/**
 * @brief Retained lifetime information for one loaded operation plugin.
 *
 * The dynamic library handle is held by `library` and is closed by its custom
 * deleter when the last owner releases it. `registered_keys` records the
 * operation keys that appeared after the plugin registration entry point ran.
 *
 * @note The operation registry stores callable objects that may refer to code
 * inside the plugin library. Hosts must unregister `registered_keys` before
 * releasing the final `library` reference.
 */
struct LoadedOpPlugin {
  std::shared_ptr<void> library;
  std::vector<std::string> registered_keys;
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
 * @brief Scans operation plugin directories and keeps libraries resident.
 *
 * This legacy entry point loads platform shared libraries from the requested
 * directory patterns, calls `register_photospider_ops`, updates `op_sources`
 * with newly registered operation keys, and retains successful dynamic library
 * handles for the remainder of the process.
 *
 * @param plugin_dir_paths Directories or suffix patterns to scan. Plain paths
 * and a trailing single-star suffix perform a shallow scan, while a trailing
 * double-star suffix scans recursively. Empty paths are ignored.
 * @param op_sources Map updated with `op key -> absolute plugin path`; existing
 * entries, including `"built-in"`, are preserved unless a newly discovered key
 * is reported by `OpRegistry`.
 * @return Structured load result with attempted file count, successful library
 * count, new operation keys, and per-plugin errors.
 * @throws std::filesystem_error when directory iteration fails for an existing
 * plugin directory.
 * @note The kernel loader performs no console I/O. Frontends should report
 * `PluginLoadResult` to users when needed.
 */
PluginLoadResult load_plugins(const std::vector<std::string>& plugin_dir_paths,
                              std::map<std::string, std::string>& op_sources);

/**
 * @brief Scans operation plugin directories and returns explicit handles.
 *
 * This overload has the same scan and registration behavior as `load_plugins`,
 * but stores each successful library in `loaded_plugins`. `PluginManager` uses
 * this form so plugin handles have an owner, can be released on unload, and do
 * not depend on hidden process-lifetime retention.
 *
 * @param plugin_dir_paths Directories or suffix patterns to scan.
 * @param op_sources Map updated with newly registered operation sources.
 * @param loaded_plugins Absolute plugin path to retained handle map owned by
 * the caller. Already-loaded paths in this map are skipped.
 * @return Structured load result for the scan.
 * @throws std::filesystem_error when directory iteration fails for an existing
 * plugin directory.
 * @note Callers must unregister each plugin's `registered_keys` before erasing
 * the corresponding entry from `loaded_plugins`.
 */
PluginLoadResult load_plugins_retaining_handles(
    const std::vector<std::string>& plugin_dir_paths,
    std::map<std::string, std::string>& op_sources,
    LoadedOpPluginMap& loaded_plugins);

}  // namespace ps
