#include "plugin/plugin_manager.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iterator>
#include <map>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/ops.hpp"
#include "ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps {
namespace {

/**
 * @brief Per-thread identity for the allocation-free recursive manager lock.
 * @note The thread-local address is unique without allocation or hashing.
 */
thread_local const unsigned char plugin_manager_state_lock_token = 0;

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
 * @brief Splices restore snapshots around an unloading shadowed plugin.
 *
 * @param source_path Plugin path whose handle is about to be released.
 * @param source_plugin Plugin record retaining the state that preceded it.
 * @param loaded_plugins Retained plugin metadata to sanitize.
 * @return Nothing.
 * @throws Nothing; map lookup and preallocated value swaps do not allocate.
 * @note If an older plugin is unloaded while a newer plugin has replaced its
 * key, the newer plugin inherits only predecessor slots owned by the older
 * plugin. Source strings retain their key-level chain, while registry values
 * are never swapped as a whole. The older callback is moved into
 * `source_plugin` for destruction after registry publication, so later unload
 * restores the real predecessor rather than code from the released library.
 */
void splice_dependent_restoration(const std::string& source_path,
                                  LoadedOpPlugin& source_plugin,
                                  LoadedOpPluginMap& loaded_plugins) noexcept {
  for (const auto& [key, owned] : source_plugin.owned_registry_entries) {
    auto source_previous = source_plugin.previous_registry_entries.find(key);
    auto source_retirement =
        source_plugin.retirement_registry_entries.find(key);
    if (source_previous == source_plugin.previous_registry_entries.end() ||
        source_retirement == source_plugin.retirement_registry_entries.end()) {
      continue;
    }
    for (auto& [path, plugin] : loaded_plugins) {
      if (path == source_path) {
        continue;
      }
      auto dependent = plugin.previous_registry_entries.find(key);
      if (dependent == plugin.previous_registry_entries.end()) {
        continue;
      }
      OpRegistry::splice_owned_snapshot_noexcept(dependent->second, owned,
                                                 source_previous->second,
                                                 source_retirement->second);
    }
  }

  for (auto& [path, plugin] : loaded_plugins) {
    if (path == source_path) {
      continue;
    }
    for (auto& [key, previous_source] : plugin.previous_sources) {
      if (previous_source && *previous_source == source_path) {
        auto source_prior = source_plugin.previous_sources.find(key);
        if (source_prior != source_plugin.previous_sources.end()) {
          previous_source.swap(source_prior->second);
        } else {
          previous_source.reset();
        }
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
 * @param removed Output number of active plugin keys removed or restored.
 * @return Extracted map node whose destruction retires callbacks and handle.
 * @throws Nothing. All required keys and snapshots were allocated during load.
 * @note Callback values are swapped into the plugin record, dependent restore
 * chains are spliced, and only then is the record extracted. The caller
 * destroys the returned node after releasing the registry lock; member order
 * still destroys callback snapshots before the retained library handle.
 */
LoadedOpPluginMap::node_type detach_loaded_plugin_noexcept(
    LoadedOpPluginMap::iterator loaded_it, LoadedOpPluginMap& loaded_plugins,
    std::map<std::string, std::string>& op_sources, int& removed) noexcept {
  const std::string& source_path = loaded_it->first;
  LoadedOpPlugin& plugin = loaded_it->second;
  auto& registry = OpRegistry::instance();
  removed = 0;

  for (const auto& key : plugin.registered_keys) {
    const auto source = op_sources.find(key);
    const bool active_source =
        source != op_sources.end() && source->second == source_path;
    auto owned = plugin.owned_registry_entries.find(key);
    auto previous = plugin.previous_registry_entries.find(key);
    auto retirement = plugin.retirement_registry_entries.find(key);
    if (owned != plugin.owned_registry_entries.end() &&
        previous != plugin.previous_registry_entries.end() &&
        retirement != plugin.retirement_registry_entries.end()) {
      removed += registry.retire_owned_entry_noexcept(
                     key, owned->second, previous->second, retirement->second)
                     ? 1
                     : 0;
    } else if (active_source) {
      // A published loader record always stages all three maps before commit.
      // Releasing its handle without retirement storage would leave an active
      // callback behind, so corrupted internal state is not recoverable here.
      std::terminate();
    }
    if (active_source) {
      restore_source_entry_noexcept(key, plugin, op_sources);
    }
  }

  splice_dependent_restoration(source_path, plugin, loaded_plugins);
  return loaded_plugins.extract(loaded_it);
}

}  // namespace

/** @copydoc PluginManager::StateLockGuard::StateLockGuard */
PluginManager::StateLockGuard::
    StateLockGuard(  // NOLINT(whitespace/indent_namespace)
        const PluginManager& manager) noexcept
    : manager_(manager) {  // NOLINT(whitespace/indent_namespace)
  manager_.lock_state();
}

/** @copydoc PluginManager::StateLockGuard::~StateLockGuard */
PluginManager::StateLockGuard::~StateLockGuard() {
  manager_.unlock_state();
}

/** @copydoc PluginManager::lock_state */
void PluginManager::lock_state() const noexcept {
  const void* token = &plugin_manager_state_lock_token;
  if (state_lock_owner_.load(std::memory_order_relaxed) == token) {
    ++state_lock_depth_;
    return;
  }

  const void* expected = nullptr;
  while (!state_lock_owner_.compare_exchange_weak(
      expected, token, std::memory_order_acquire, std::memory_order_relaxed)) {
    expected = nullptr;
    std::this_thread::yield();
  }
  state_lock_depth_ = 1;
}

/** @copydoc PluginManager::unlock_state */
void PluginManager::unlock_state() const noexcept {
  if (--state_lock_depth_ == 0) {
    state_lock_owner_.store(nullptr, std::memory_order_release);
  }
}

/** @copydoc PluginManager::process_instance */
PluginManager& PluginManager::process_instance() {
  static PluginManager* manager = new PluginManager();
  return *manager;
}

/** @copydoc PluginManager::load_from_dirs */
void PluginManager::load_from_dirs(
    const std::vector<std::string>& dir_patterns) {
  (void)load_from_dirs_report(dir_patterns);
}

/** @copydoc PluginManager::load_from_dirs_report */
PluginLoadResult PluginManager::load_from_dirs_report(
    const std::vector<std::string>& dir_patterns) {
  StateLockGuard lock(*this);
  OpRegistry::StateLockGuard registry_lock(OpRegistry::instance());
  synchronize_builtins_locked();
  return load_plugins_for_process_owner(ProcessPluginOwnerToken{}, dir_patterns,
                                        op_sources_, loaded_plugins_);
}

/** @copydoc PluginManager::synchronize_builtins_locked */
void PluginManager::synchronize_builtins_locked() {
  auto& registry = OpRegistry::instance();
  if (!builtins_seeded_) {
    ops::register_builtin();
    builtins_seeded_ = true;
  }

  const auto registry_keys = registry.get_keys();
  for (auto source = op_sources_.begin(); source != op_sources_.end();) {
    if (source->second == "built-in" &&
        !std::binary_search(registry_keys.begin(), registry_keys.end(),
                            source->first)) {
      source = op_sources_.erase(source);
      continue;
    }
    ++source;
  }
  for (const auto& key : registry_keys) {
    if (!op_sources_.count(key)) {
      op_sources_[key] = "built-in";
    }
  }
}

/** @copydoc PluginManager::effective_sources_locked */
std::map<std::string, std::string> PluginManager::effective_sources_locked()
    const {  // NOLINT(whitespace/indent_namespace)
  auto& registry = OpRegistry::instance();
  std::map<std::string, std::string> effective = op_sources_;
  const auto registry_keys = registry.get_keys();

  for (auto source = effective.begin(); source != effective.end();) {
    if (!std::binary_search(registry_keys.begin(), registry_keys.end(),
                            source->first)) {
      source = effective.erase(source);
    } else {
      ++source;
    }
  }

  for (const auto& key : registry_keys) {
    const std::string* complete_owner = nullptr;
    bool partial_owner = false;
    for (const auto& [path, plugin] : loaded_plugins_) {
      const auto owned = plugin.owned_registry_entries.find(key);
      if (owned == plugin.owned_registry_entries.end()) {
        continue;
      }
      const auto match = registry.classify_active_ownership(key, owned->second);
      if (match == OpRegistry::OwnershipMatch::Complete) {
        complete_owner = &path;
        partial_owner = false;
        break;
      }
      if (match == OpRegistry::OwnershipMatch::Partial) {
        partial_owner = true;
      }
    }

    if (complete_owner) {
      effective[key] = *complete_owner;
    } else if (partial_owner) {
      effective[key] = "mixed";
    } else {
      effective[key] = "built-in";
    }
  }
  return effective;
}

/** @copydoc PluginManager::seed_builtins_from_registry */
void PluginManager::seed_builtins_from_registry() {
  StateLockGuard lock(*this);
  OpRegistry::StateLockGuard registry_lock(OpRegistry::instance());
  synchronize_builtins_locked();
}

/** @copydoc PluginManager::unload_by_plugin_path */
int PluginManager::unload_by_plugin_path(
    const std::string& absolute_plugin_path) {
  StateLockGuard lock(*this);
  auto loaded_it = loaded_plugins_.find(absolute_plugin_path);
  if (loaded_it != loaded_plugins_.end()) {
    int removed = 0;
    LoadedOpPluginMap::node_type retired;
    {
      OpRegistry::StateLockGuard registry_lock(OpRegistry::instance());
      retired = detach_loaded_plugin_noexcept(loaded_it, loaded_plugins_,
                                              op_sources_, removed);
    }
    return removed;
  }

  // Relative/non-normalized input is a convenience boundary. It runs before
  // any registry/source/handle mutation; callers that retain the recorded
  // absolute load key take the allocation-free lookup above.
  const std::string source_path = plugin_source_key(absolute_plugin_path);
  loaded_it = loaded_plugins_.find(source_path);
  if (loaded_it != loaded_plugins_.end()) {
    int removed = 0;
    LoadedOpPluginMap::node_type retired;
    {
      OpRegistry::StateLockGuard registry_lock(OpRegistry::instance());
      retired = detach_loaded_plugin_noexcept(loaded_it, loaded_plugins_,
                                              op_sources_, removed);
    }
    return removed;
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
  StateLockGuard lock(*this);
  int removed = 0;
  while (!loaded_plugins_.empty()) {
    LoadedOpPluginMap::node_type retired;
    {
      OpRegistry::StateLockGuard registry_lock(OpRegistry::instance());
      auto newest = loaded_plugins_.begin();
      for (auto candidate = std::next(newest);
           candidate != loaded_plugins_.end(); ++candidate) {
        if (candidate->second.load_sequence > newest->second.load_sequence) {
          newest = candidate;
        }
      }
      int candidate_removed = 0;
      retired = detach_loaded_plugin_noexcept(newest, loaded_plugins_,
                                              op_sources_, candidate_removed);
      removed += candidate_removed;
    }
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

/** @copydoc PluginManager::op_sources */
std::map<std::string, std::string> PluginManager::op_sources() const {
  StateLockGuard lock(*this);
  OpRegistry::StateLockGuard registry_lock(OpRegistry::instance());
  return effective_sources_locked();
}

/** @copydoc PluginManager::combined_keys */
std::vector<std::string> PluginManager::combined_keys() const {
  StateLockGuard lock(*this);
  OpRegistry::StateLockGuard registry_lock(OpRegistry::instance());
  return OpRegistry::instance().get_combined_keys();
}

/** @copydoc PluginManager::combined_sources */
std::map<std::string, std::string> PluginManager::combined_sources() const {
  StateLockGuard lock(*this);
  OpRegistry::StateLockGuard registry_lock(OpRegistry::instance());
  std::map<std::string, std::string> combined;
  const auto effective_sources = effective_sources_locked();
  const auto keys = OpRegistry::instance().get_combined_keys();
  for (const auto& key : keys) {
    const auto source = effective_sources.find(key);
    if (source != effective_sources.end()) {
      combined[key] = source->second;
      continue;
    }

    const auto separator = key.rfind(':');
    if (separator != std::string::npos) {
      const std::string tiled =
          key.substr(0, separator + 1) + key.substr(separator + 1) + "_tiled";
      const auto tiled_source = effective_sources.find(tiled);
      if (tiled_source != effective_sources.end()) {
        combined[key] = tiled_source->second;
        continue;
      }
    }
    combined[key] = "built-in";
  }
  return combined;
}

/** @copydoc PluginManager::loaded_plugin_count */
size_t PluginManager::loaded_plugin_count() const noexcept {
  StateLockGuard lock(*this);
  return loaded_plugins_.size();
}

}  // namespace ps
