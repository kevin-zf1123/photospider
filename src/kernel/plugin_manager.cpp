#include "kernel/plugin_manager.hpp"

#include <filesystem>
#include <iterator>
#include <map>
#include <new>
#include <string>
#include <vector>

#include "kernel/ops.hpp"
#include "ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps {
namespace {

/**
 * @brief Normalizes a plugin path to the loader's absolute-path key.
 *
 * @param plugin_path User-provided plugin path, absolute or relative.
 * @return Absolute path string used by `LoadedOpPluginMap` and `op_sources`.
 * @throws std::bad_alloc if path/string normalization cannot allocate.
 * @throws std::filesystem_error if the current directory cannot be resolved.
 * @note The path does not need to exist; unload callers may pass a path that
 * was loaded earlier but has since been removed from disk.
 */
std::string plugin_source_key(const std::string& plugin_path) {
  return fs::absolute(plugin_path).string();
}

/**
 * @brief Drops restore snapshots that depend on an unloading plugin path.
 *
 * @param source_path Plugin path whose handle is about to be released.
 * @param loaded_plugins Retained plugin metadata to sanitize.
 * @return Nothing.
 * @throws Nothing; map lookup, erase, and optional reset do not allocate.
 * @note If an older plugin is unloaded while a newer plugin has replaced its
 * key, the newer plugin must not later restore callbacks into the released
 * older library.
 */
void drop_dependent_restoration(const std::string& source_path,
                                LoadedOpPluginMap& loaded_plugins) noexcept {
  for (auto& [path, plugin] : loaded_plugins) {
    if (path == source_path) {
      continue;
    }
    for (auto& [key, previous_source] : plugin.previous_sources) {
      if (previous_source && *previous_source == source_path) {
        previous_source.reset();
        plugin.previous_registry_entries.erase(key);
      }
    }
  }
}

/**
 * @brief Restores one source entry using its preallocated snapshot.
 *
 * @param key Canonical operation key being removed.
 * @param loaded_plugin Retained plugin metadata with prior sources.
 * @param op_sources Mutable operation source map.
 * @return Nothing.
 * @throws Nothing; restoration swaps existing strings or erases the key.
 * @note A missing active entry is left absent rather than allocating a new map
 * node. Loader-produced state always has the active source node.
 */
void restore_source_entry_noexcept(
    const std::string& key, LoadedOpPlugin& loaded_plugin,
    std::map<std::string, std::string>& op_sources) noexcept {
  auto active = op_sources.find(key);
  auto previous = loaded_plugin.previous_sources.find(key);
  if (previous == loaded_plugin.previous_sources.end() || !previous->second) {
    if (active != op_sources.end()) {
      op_sources.erase(active);
    }
    return;
  }
  if (active != op_sources.end()) {
    active->second.swap(*previous->second);
  }
}

/**
 * @brief Removes or restores one retained plugin without allocating.
 *
 * @param loaded_it Plugin map iterator selected by path or reverse load order.
 * @param loaded_plugins Manager-owned retained plugin map.
 * @param op_sources Manager-owned operation source map.
 * @return Number of active plugin keys removed or restored.
 * @throws Nothing. All required keys and snapshots were allocated during load.
 * @note Callback values are swapped into the plugin record, dependent
 * snapshots are sanitized, and only then is the record erased so callback
 * destruction precedes dynamic-library release.
 */
int unload_loaded_plugin_noexcept(
    LoadedOpPluginMap::iterator loaded_it, LoadedOpPluginMap& loaded_plugins,
    std::map<std::string, std::string>& op_sources) noexcept {
  const std::string& source_path = loaded_it->first;
  LoadedOpPlugin& plugin = loaded_it->second;
  auto& registry = OpRegistry::instance();
  int removed = 0;

  for (const auto& key : plugin.registered_keys) {
    const auto source = op_sources.find(key);
    if (source != op_sources.end() && source->second != source_path) {
      continue;
    }

    auto previous = plugin.previous_registry_entries.find(key);
    if (previous != plugin.previous_registry_entries.end()) {
      removed += registry.restore_entry_noexcept(key, previous->second) ? 1 : 0;
    } else {
      removed += registry.unregister_key(key) ? 1 : 0;
    }
    restore_source_entry_noexcept(key, plugin, op_sources);
  }

  drop_dependent_restoration(source_path, loaded_plugins);
  loaded_plugins.erase(loaded_it);
  return removed;
}

}  // namespace

/** @copydoc PluginManager::~PluginManager */
PluginManager::~PluginManager() noexcept {
  (void)unload_all_plugins();
}

/** @copydoc PluginManager::load_from_dirs */
void PluginManager::load_from_dirs(
    const std::vector<std::string>& dir_patterns) {
  (void)load_from_dirs_report(dir_patterns);
}

/** @copydoc PluginManager::load_from_dirs_report */
PluginLoadResult PluginManager::load_from_dirs_report(
    const std::vector<std::string>& dir_patterns) {
  return load_plugins_retaining_handles(dir_patterns, op_sources_,
                                        loaded_plugins_);
}

/**
 * @brief Registers built-ins best-effort and records their current source.
 *
 * @return Nothing.
 * @throws std::bad_alloc if built-in registration, key enumeration, or source
 * map population exhausts memory.
 * @throws Exceptions from source-map population after non-resource built-in
 * registration failures have been ignored.
 * @note Existing plugin source entries are never overwritten by the built-in
 * label.
 */
void PluginManager::seed_builtins_from_registry() {
  try {
    ops::register_builtin();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    // Builtin registration is idempotent in practice; keep seeding best-effort.
  }

  auto& registry = OpRegistry::instance();
  for (const auto& key : registry.get_keys()) {
    if (!op_sources_.count(key)) {
      op_sources_[key] = "built-in";
    }
  }
}

/** @copydoc PluginManager::unload_by_plugin_path */
int PluginManager::unload_by_plugin_path(
    const std::string& absolute_plugin_path) {
  auto loaded_it = loaded_plugins_.find(absolute_plugin_path);
  if (loaded_it != loaded_plugins_.end()) {
    return unload_loaded_plugin_noexcept(loaded_it, loaded_plugins_,
                                         op_sources_);
  }

  // Relative/non-normalized input is a convenience boundary. It runs before
  // any registry/source/handle mutation; callers that retain the recorded
  // absolute load key take the allocation-free lookup above.
  const std::string source_path = plugin_source_key(absolute_plugin_path);
  loaded_it = loaded_plugins_.find(source_path);
  if (loaded_it != loaded_plugins_.end()) {
    return unload_loaded_plugin_noexcept(loaded_it, loaded_plugins_,
                                         op_sources_);
  }

  int removed = 0;
  for (auto it = op_sources_.begin(); it != op_sources_.end();) {
    if (it->second != source_path) {
      ++it;
      continue;
    }
    removed += OpRegistry::instance().unregister_key(it->first) ? 1 : 0;
    it = op_sources_.erase(it);
  }
  return removed;
}

/** @copydoc PluginManager::unload_all_plugins */
int PluginManager::unload_all_plugins() noexcept {
  int removed = 0;
  while (!loaded_plugins_.empty()) {
    auto newest = loaded_plugins_.begin();
    for (auto candidate = std::next(newest); candidate != loaded_plugins_.end();
         ++candidate) {
      if (candidate->second.load_sequence > newest->second.load_sequence) {
        newest = candidate;
      }
    }
    removed +=
        unload_loaded_plugin_noexcept(newest, loaded_plugins_, op_sources_);
  }

  for (auto it = op_sources_.begin(); it != op_sources_.end();) {
    if (it->second == "built-in") {
      ++it;
      continue;
    }
    removed += OpRegistry::instance().unregister_key(it->first) ? 1 : 0;
    it = op_sources_.erase(it);
  }
  return removed;
}

}  // namespace ps
