// Plugin loading utilities for Photospider
#pragma once

#include <map>
#include <string>
#include <vector>

// Scans directories for plugins and loads them, registering ops.
// - plugin_dir_paths: list of directories or simple wildcard patterns to scan for shared libraries.
//   Suffix semantics:
//     - "path" or "path/*"  => shallow scan (only the directory itself)
//     - "path/**"            => recursive scan of all subdirectories
// - op_sources: map updated with op key -> plugin path (or "built-in").
#include "kernel/plugin_result.hpp"

namespace ps {

// Load plugins and report result (no console I/O in kernel).
PluginLoadResult load_plugins(const std::vector<std::string>& plugin_dir_paths,
                              std::map<std::string, std::string>& op_sources);

} // namespace ps
