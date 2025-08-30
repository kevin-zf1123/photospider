// Photospider kernel: PluginManager interface
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace ps {

// Forward-declare minimal OpRegistry extension hooks.
class OpRegistry;

// Manages dynamic plugins and the ops they register.
// - Loads shared libraries using existing load_plugins() implementation.
// - Tracks which ops came from which plugin path for safe unload.
class PluginManager {
public:
    // Load plugins from the given directory patterns and record op->source mapping.
    void load_from_dirs(const std::vector<std::string>& dir_patterns);
    // Seed built-in ops from current OpRegistry into source map as "built-in".
    void seed_builtins_from_registry();

    // Unload all ops that were registered by a specific plugin path.
    // Returns number of ops unregistered. The shared library handle is not closed
    // (platform-specific and not tracked here); this only unregisters ops.
    int unload_by_plugin_path(const std::string& absolute_plugin_path);

    // Unload all ops registered from any plugin (does not touch built-ins).
    int unload_all_plugins();

    // List registered ops with their sources ("built-in" or absolute .so/.dylib path).
    const std::map<std::string, std::string>& op_sources() const { return op_sources_; }

private:
    // Map: op key (type:subtype) -> source ("built-in" or plugin absolute path)
    std::map<std::string, std::string> op_sources_;
};

} // namespace ps
