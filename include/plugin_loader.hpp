// Plugin loading utilities for Photospider
#pragma once

#include <map>
#include <string>
#include <vector>

// Scans directories for plugins and loads them, registering ops.
// - plugin_dir_paths: list of directories to scan for shared libraries.
// - op_sources: map updated with op key -> plugin path (or "built-in").
void load_plugins(const std::vector<std::string>& plugin_dir_paths,
                  std::map<std::string, std::string>& op_sources);

