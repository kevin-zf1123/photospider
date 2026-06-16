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
 * @brief Returns unique keys from plugin handle metadata and source mappings.
 *
 * @param source_path Absolute plugin source path.
 * @param loaded_plugins Retained plugin handle map.
 * @param op_sources Current operation source map.
 * @return Sorted unique operation keys to unregister.
 * @throws std::bad_alloc from vector or set allocation.
 * @note Combining both sources keeps unload robust if a caller mutates
 * `op_sources` or if a plugin registered no newly discovered keys.
 */
std::vector<std::string> collect_plugin_keys(
    const std::string& source_path, const LoadedOpPluginMap& loaded_plugins,
    const std::map<std::string, std::string>& op_sources) {
  std::set<std::string> unique_keys;

  auto loaded_it = loaded_plugins.find(source_path);
  if (loaded_it != loaded_plugins.end()) {
    unique_keys.insert(loaded_it->second.registered_keys.begin(),
                       loaded_it->second.registered_keys.end());
  }

  const auto mapped_keys = keys_for_source(source_path, op_sources);
  unique_keys.insert(mapped_keys.begin(), mapped_keys.end());

  return std::vector<std::string>(unique_keys.begin(), unique_keys.end());
}

/**
 * @brief Erases operation source entries for a list of keys.
 *
 * @param keys Operation keys whose source records should be removed.
 * @param op_sources Mutable operation source map.
 * @throws Nothing under current `std::map::erase(key)` behavior.
 * @note Built-in entries are not passed to this helper by unload callers.
 */
void erase_source_entries(const std::vector<std::string>& keys,
                          std::map<std::string, std::string>& op_sources) {
  for (const auto& key : keys) {
    op_sources.erase(key);
  }
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
  const auto plugin_keys =
      collect_plugin_keys(source_path, loaded_plugins_, op_sources_);
  if (plugin_keys.empty()) {
    loaded_plugins_.erase(source_path);
    return 0;
  }

  const int removed = unregister_operation_keys(plugin_keys);
  erase_source_entries(plugin_keys, op_sources_);
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
    const auto plugin_keys =
        collect_plugin_keys(path, loaded_plugins_, op_sources_);
    removed += unregister_operation_keys(plugin_keys);
    erase_source_entries(plugin_keys, op_sources_);
    loaded_plugins_.erase(path);
  }
  return removed;
}

}  // namespace ps
