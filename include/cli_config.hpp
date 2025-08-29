// Lightweight CLI configuration definition and I/O declarations
#pragma once

#include <string>
#include <vector>

// Note: Keep this struct in the global namespace to match existing usage
// in cli/graph_cli.cpp and avoid widespread refactors.
struct CliConfig {
    std::string loaded_config_path;
    std::string cache_root_dir = "cache";
    std::vector<std::string> plugin_dirs = {"build/plugins"};
    std::string cache_precision = "int8";
    std::string default_print_mode = "full";
    std::string default_traversal_arg = "n";
    std::string default_cache_clear_arg = "md";
    std::string default_exit_save_path = "graph_out.yaml";
    bool exit_prompt_sync = true;
    std::string config_save_behavior = "current";
    std::string editor_save_behavior = "ask";
    std::string default_timer_log_path = "out/timer.yaml";
    std::string default_ops_list_mode = "all";
    std::string ops_plugin_path_mode = "name_only";
    // Space-separated default flags for the REPL `compute` command (e.g. "t parallel").
    std::string default_compute_args = "";
    int history_size = 1000;
};

// Persist the configuration to a YAML file at `path`.
// Returns true on success.
bool write_config_to_file(const CliConfig& config, const std::string& path);

// Load an existing config from `config_path` if it exists.
// If `config_path` is the default "config.yaml" and does not exist, create it with defaults.
void load_or_create_config(const std::string& config_path, CliConfig& config);
