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
  std::vector<std::string> scheduler_dirs = {"build/schedulers"};
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
  // Space-separated default flags for the REPL `compute` command (e.g. "t
  // parallel").
  std::string default_compute_args = "";
  int history_size = 1000;
  // Behavior: after a successful `load`, set current graph to the loaded one.
  bool switch_after_load = true;
  // Whether to show a warning when loading/copying over an existing session's
  // content.
  bool session_warning = true;

  /** @brief Default scheduler type for HP high-precision computations.
   *
   * Built-in values are "cpu_work_stealing", "serial_debug",
   * "gpu_pipeline", and "heterogeneous". Scheduler plugin names become valid
   * after graph_cli scans scheduler_dirs or a user runs scheduler scan/load.
   */
  std::string scheduler_hp_type = "cpu_work_stealing";

  /** @brief Default scheduler type for RT real-time kernel intent work.
   *
   * This selects the scheduler attached to ComputeIntent::RealTimeUpdate for
   * kernel/API callers. It does not make graph_cli expose RT compute commands
   * or dirty source lifecycle commands.
   */
  std::string scheduler_rt_type = "cpu_work_stealing";

  /** @brief Worker count for built-in CPU-backed schedulers; 0 means auto. */
  int scheduler_worker_count = 0;
};

// Persist the configuration to a YAML file at `path`.
// Returns true on success.
bool write_config_to_file(const CliConfig& config, const std::string& path);

// Load an existing config from `config_path` if it exists.
// If `config_path` is the default "config.yaml" and does not exist, create it
// with defaults.
void load_or_create_config(const std::string& config_path, CliConfig& config);
