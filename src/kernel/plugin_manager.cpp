// Photospider kernel: PluginManager implementation
#include "kernel/plugin_manager.hpp"

#include <algorithm>
#include <iostream>
#include <vector>

#include "kernel/ops.hpp"     // register_builtin()
#include "plugin_loader.hpp"  // load_plugins(dir_patterns, op_sources)
#include "ps_types.hpp"       // OpRegistry

namespace ps {

// Extend OpRegistry with an unregister method locally (non-breaking):
namespace {
struct RegistryEditor {
  static int unregister_keys(const std::vector<std::string>& keys) {
    auto& reg = OpRegistry::instance();
    int count = 0;
    for (const auto& k : keys)
      count += reg.unregister_key(k) ? 1 : 0;
    return count;
  }
};
}  // namespace

void PluginManager::load_from_dirs(
    const std::vector<std::string>& dir_patterns) {
  (void)load_from_dirs_report(dir_patterns);
}

PluginLoadResult PluginManager::load_from_dirs_report(
    const std::vector<std::string>& dir_patterns) {
  return load_plugins(dir_patterns, op_sources_);
}

void PluginManager::seed_builtins_from_registry() {
  // Ensure built-ins are registered in the global registry before seeding.
  try {
    ops::register_builtin();
  } catch (...) { /* ignore */
  }
  auto& reg = OpRegistry::instance();
  for (const auto& key : reg.get_keys()) {
    if (!op_sources_.count(key))
      op_sources_[key] = "built-in";
  }
}

int PluginManager::unload_by_plugin_path(
    const std::string& absolute_plugin_path) {
  std::vector<std::string> to_remove;
  for (const auto& [key, src] : op_sources_)
    if (src == absolute_plugin_path)
      to_remove.push_back(key);
  if (to_remove.empty())
    return 0;

  // Attempt to unregister from the live registry (best-effort; see comments
  // above)
  int removed = RegistryEditor::unregister_keys(to_remove);

  // Remove from our mapping regardless to keep user-visible state consistent
  for (const auto& k : to_remove)
    op_sources_.erase(k);
  return removed;
}

int PluginManager::unload_all_plugins() {
  std::vector<std::string> plugin_keys;
  for (const auto& [key, src] : op_sources_)
    if (src != "built-in")
      plugin_keys.push_back(key);
  if (plugin_keys.empty())
    return 0;
  int removed = RegistryEditor::unregister_keys(plugin_keys);
  for (const auto& k : plugin_keys)
    op_sources_.erase(k);
  return removed;
}

}  // namespace ps
