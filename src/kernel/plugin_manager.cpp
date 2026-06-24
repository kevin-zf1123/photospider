#include "kernel/plugin_manager.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
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
 * @throws std::filesystem_error if the current directory cannot be resolved.
 * @note The path does not need to exist; unload callers may pass a path that
 * was loaded earlier but has since been removed from disk.
 */
std::string plugin_source_key(const std::string& plugin_path) {
  return fs::absolute(plugin_path).string();
}

/**
 * @brief Removes keys from the global operation registry.
 *
 * @param keys Operation keys to remove.
 * @return Count of keys actually removed from `OpRegistry`.
 * @throws Nothing under current `OpRegistry::unregister_key` behavior.
 * @note Unregistration must happen before the owning dynamic library handle is
 * released, because callbacks may point into that library.
 */
int unregister_operation_keys(const std::vector<std::string>& keys) {
  auto& registry = OpRegistry::instance();
  int removed = 0;
  for (const auto& key : keys) {
    removed += registry.unregister_key(key) ? 1 : 0;
  }
  return removed;
}

/**
 * @brief Collects operation keys in `op_sources` that belong to a plugin path.
 *
 * @param source_path Absolute plugin source path.
 * @param op_sources Current operation source map.
 * @return Operation keys whose source equals `source_path`.
 * @throws std::bad_alloc from vector growth.
 * @note This fallback covers legacy source-map entries and plugins loaded
 * before handle metadata existed.
 */
std::vector<std::string> keys_for_source(
    const std::string& source_path,
    const std::map<std::string, std::string>& op_sources) {
  std::vector<std::string> keys;
  for (const auto& [key, source] : op_sources) {
    if (source == source_path) {
      keys.push_back(key);
    }
  }
  return keys;
}

/**
 * @brief Returns unique keys currently owned by a plugin path.
 *
 * @param source_path Absolute plugin source path.
 * @param loaded_plugins Retained plugin handle map.
 * @param op_sources Current operation source map.
 * @return Sorted unique operation keys to unregister.
 * @throws std::bad_alloc from vector or set allocation.
 * @note Registered-key metadata is filtered through current `op_sources` so an
 * older plugin that was replaced by a newer plugin cannot unregister the
 * newer plugin's active callback.
 */
std::vector<std::string> collect_plugin_keys(
    const std::string& source_path, const LoadedOpPluginMap& loaded_plugins,
    const std::map<std::string, std::string>& op_sources) {
  std::set<std::string> unique_keys;

  auto loaded_it = loaded_plugins.find(source_path);
  if (loaded_it != loaded_plugins.end()) {
    for (const auto& key : loaded_it->second.registered_keys) {
      auto source_it = op_sources.find(key);
      if (source_it == op_sources.end() || source_it->second == source_path) {
        unique_keys.insert(key);
      }
    }
  }

  const auto mapped_keys = keys_for_source(source_path, op_sources);
  unique_keys.insert(mapped_keys.begin(), mapped_keys.end());

  return std::vector<std::string>(unique_keys.begin(), unique_keys.end());
}

/**
 * @brief Drops restore snapshots that depend on an unloading plugin path.
 *
 * @param source_path Plugin path whose handle is about to be released.
 * @param loaded_plugins Retained plugin metadata to sanitize.
 * @throws Nothing under current map erase/optional reset behavior.
 * @note If an older plugin is unloaded while a newer plugin has replaced its
 * key, the newer plugin must not later restore callbacks into the released
 * older library.
 */
void drop_dependent_restoration(const std::string& source_path,
                                LoadedOpPluginMap& loaded_plugins) {
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
 * @brief Restores operation source entries after a plugin is removed.
 *
 * @param keys Operation keys whose source records should be restored.
 * @param loaded_plugin Optional retained plugin metadata with prior sources.
 * @param op_sources Mutable operation source map.
 * @throws Nothing under current `std::map::erase(key)` behavior.
 * @note Keys without retained prior-source metadata are erased for
 * compatibility with legacy plugin metadata.
 */
void restore_source_entries(const std::vector<std::string>& keys,
                            const LoadedOpPlugin* loaded_plugin,
                            std::map<std::string, std::string>& op_sources) {
  for (const auto& key : keys) {
    if (loaded_plugin) {
      auto previous_it = loaded_plugin->previous_sources.find(key);
      if (previous_it != loaded_plugin->previous_sources.end()) {
        if (previous_it->second) {
          op_sources[key] = *previous_it->second;
        } else {
          op_sources.erase(key);
        }
        continue;
      }
    }
    op_sources.erase(key);
  }
}

/**
 * @brief Restores registry callbacks that existed before plugin registration.
 *
 * @param keys Operation keys removed from the plugin-owned registry entries.
 * @param loaded_plugin Optional retained plugin metadata with prior registry
 * snapshots.
 * @throws std::bad_alloc if restoring callbacks or metadata allocates.
 * @note The plugin-owned callbacks must already have been unregistered before
 * this helper restores any previous implementation.
 */
void restore_registry_entries(const std::vector<std::string>& keys,
                              const LoadedOpPlugin* loaded_plugin) {
  if (!loaded_plugin) {
    return;
  }

  OpRegistry::RegistrationCapture restore_capture;
  for (const auto& key : keys) {
    auto previous_it = loaded_plugin->previous_registry_entries.find(key);
    if (previous_it == loaded_plugin->previous_registry_entries.end()) {
      continue;
    }
    restore_capture.registered_keys.push_back(key);
    restore_capture.previous_entries.emplace(key, previous_it->second);
  }
  OpRegistry::instance().restore_registration_capture(restore_capture);
}

}  // namespace

PluginManager::~PluginManager() {
  try {
    (void)unload_all_plugins();
  } catch (...) {
    // Destructors must not surface plugin cleanup failures.
  }
}

void PluginManager::load_from_dirs(
    const std::vector<std::string>& dir_patterns) {
  (void)load_from_dirs_report(dir_patterns);
}

PluginLoadResult PluginManager::load_from_dirs_report(
    const std::vector<std::string>& dir_patterns) {
  return load_plugins_retaining_handles(dir_patterns, op_sources_,
                                        loaded_plugins_);
}

void PluginManager::seed_builtins_from_registry() {
  try {
    ops::register_builtin();
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

int PluginManager::unload_by_plugin_path(
    const std::string& absolute_plugin_path) {
  const std::string source_path = plugin_source_key(absolute_plugin_path);
  const auto loaded_it = loaded_plugins_.find(source_path);
  const LoadedOpPlugin* loaded_plugin =
      loaded_it == loaded_plugins_.end() ? nullptr : &loaded_it->second;
  const auto plugin_keys =
      collect_plugin_keys(source_path, loaded_plugins_, op_sources_);
  if (plugin_keys.empty()) {
    loaded_plugins_.erase(source_path);
    return 0;
  }

  const int removed = unregister_operation_keys(plugin_keys);
  restore_registry_entries(plugin_keys, loaded_plugin);
  restore_source_entries(plugin_keys, loaded_plugin, op_sources_);
  drop_dependent_restoration(source_path, loaded_plugins_);
  loaded_plugins_.erase(source_path);
  return removed;
}

int PluginManager::unload_all_plugins() {
  std::set<std::string> plugin_paths;
  for (const auto& [path, _] : loaded_plugins_) {
    plugin_paths.insert(path);
  }
  for (const auto& [_, source] : op_sources_) {
    if (source != "built-in") {
      plugin_paths.insert(source);
    }
  }

  int removed = 0;
  for (const auto& path : plugin_paths) {
    const auto loaded_it = loaded_plugins_.find(path);
    const LoadedOpPlugin* loaded_plugin =
        loaded_it == loaded_plugins_.end() ? nullptr : &loaded_it->second;
    const auto plugin_keys =
        collect_plugin_keys(path, loaded_plugins_, op_sources_);
    removed += unregister_operation_keys(plugin_keys);
    restore_registry_entries(plugin_keys, loaded_plugin);
    restore_source_entries(plugin_keys, loaded_plugin, op_sources_);
    drop_dependent_restoration(path, loaded_plugins_);
    loaded_plugins_.erase(path);
  }
  return removed;
}

}  // namespace ps
