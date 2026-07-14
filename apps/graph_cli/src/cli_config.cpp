// CLI configuration YAML read/write implementation
#include "graph_cli/cli_config.hpp"  // NOLINT(build/include_subdir)

#include <yaml-cpp/yaml.h>

#include <charconv>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"

namespace fs = ps::fs;

/**
 * @brief Validates one CLI scheduler worker-count value.
 *
 * @param worker_count Parsed or programmatically supplied CLI value.
 * @return Nothing.
 * @throws std::invalid_argument If the count is outside `[0, 8]`.
 * @note Zero remains automatic-resolution intent and no state is mutated.
 */
void validate_cli_scheduler_worker_count(int worker_count) {
  if (worker_count < 0 || static_cast<unsigned int>(worker_count) >
                              ps::kSchedulerWorkerRequestMax) {
    throw std::invalid_argument(
        "scheduler_worker_count must be an integer in range [0, 8]");
  }
}

/**
 * @brief Strictly parses one CLI editor scheduler worker-count value.
 *
 * @param text Complete editor text to parse.
 * @return Parsed count in `[0, 8]`.
 * @throws std::invalid_argument If parsing is incomplete, overflows, or yields
 * a value outside the public request range.
 * @throws std::bad_alloc If failure diagnostic construction cannot allocate.
 * @note Leading/trailing whitespace and trailing characters are rejected so
 * the editor cannot silently coerce a partially valid token.
 */
int parse_cli_scheduler_worker_count(std::string_view text) {
  if (text.empty()) {
    throw std::invalid_argument(
        "scheduler_worker_count must be an integer in range [0, 8]");
  }
  int worker_count = 0;
  const char* const first = text.data();
  const char* const last = first + text.size();
  const auto parsed = std::from_chars(first, last, worker_count);
  if (parsed.ec != std::errc{} || parsed.ptr != last) {
    throw std::invalid_argument(
        "scheduler_worker_count must be an integer in range [0, 8]");
  }
  validate_cli_scheduler_worker_count(worker_count);
  return worker_count;
}

/**
 * @brief Applies one validated CLI scheduler-default snapshot to a Host.
 *
 * @param host Borrowed Host receiving future-Graph scheduler defaults.
 * @param config Complete CLI snapshot to translate.
 * @return Nothing.
 * @throws std::invalid_argument If the count is outside `[0, 8]`.
 * @throws std::runtime_error If the Host rejects the complete candidate.
 * @throws std::bad_alloc If scheduler configuration or diagnostics cannot
 * allocate.
 * @note Validation happens before the exactly-once Host call. Host failure is
 * exceptional here so both startup and interactive CLI boundaries observe it.
 */
void apply_cli_scheduler_defaults(ps::Host& host, const CliConfig& config) {
  validate_cli_scheduler_worker_count(config.scheduler_worker_count);

  ps::HostSchedulerConfig scheduler_config;
  scheduler_config.hp_type = config.scheduler_hp_type;
  scheduler_config.rt_type = config.scheduler_rt_type;
  scheduler_config.worker_count =
      static_cast<unsigned int>(config.scheduler_worker_count);
  const ps::VoidResult result =
      host.configure_scheduler_defaults(scheduler_config);
  if (result.status.ok) {
    return;
  }

  std::string diagnostic = "Host rejected scheduler defaults";
  if (!result.status.name.empty()) {
    diagnostic += " (" + result.status.name + ")";
  }
  if (!result.status.message.empty()) {
    diagnostic += ": " + result.status.message;
  }
  throw std::runtime_error(diagnostic);
}

/**
 * @brief Serializes one CLI configuration to a YAML file.
 *
 * @param config Value snapshot to serialize without mutation.
 * @param path Destination YAML path.
 * @return True only when YAML construction and stream output both succeed.
 * @throws std::bad_alloc If YAML, path, or stream storage exhausts memory.
 * @note All allocating YAML assignments are inside the guarded chain so the
 * first catch preserves resource exhaustion before translating other errors.
 */
bool write_config_to_file(const CliConfig& config, const std::string& path) {
  try {
    YAML::Node root;
    root["_comment1"] = "Photospider CLI configuration.";
    root["cache_root_dir"] = config.cache_root_dir;
    root["cache_precision"] = config.cache_precision;
    root["plugin_dirs"] = config.plugin_dirs;
    root["scheduler_dirs"] = config.scheduler_dirs;
    root["history_size"] = config.history_size;
    root["default_print_mode"] = config.default_print_mode;
    root["default_traversal_arg"] = config.default_traversal_arg;
    root["default_cache_clear_arg"] = config.default_cache_clear_arg;
    root["default_exit_save_path"] = config.default_exit_save_path;
    root["exit_prompt_sync"] = config.exit_prompt_sync;
    root["config_save_behavior"] = config.config_save_behavior;
    root["editor_save_behavior"] = config.editor_save_behavior;
    root["default_timer_log_path"] = config.default_timer_log_path;
    root["default_ops_list_mode"] = config.default_ops_list_mode;
    root["ops_plugin_path_mode"] = config.ops_plugin_path_mode;
    root["default_compute_args"] = config.default_compute_args;
    root["switch_after_load"] = config.switch_after_load;
    root["session_warning"] = config.session_warning;
    root["scheduler_hp_type"] = config.scheduler_hp_type;
    root["scheduler_rt_type"] = config.scheduler_rt_type;
    root["scheduler_worker_count"] = config.scheduler_worker_count;

    std::ofstream fout(path);
    if (!fout) {
      return false;
    }
    fout << root;
    return static_cast<bool>(fout);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception&) {
    return false;
  }
}

/**
 * @brief Loads an existing CLI config or creates its default file.
 *
 * @param config_path Existing YAML path, or `config.yaml` for default creation.
 * @param config Destination committed only after complete temporary parsing.
 * @return Nothing.
 * @throws std::bad_alloc If filesystem, YAML, string, or container storage
 * exhausts memory.
 * @throws std::filesystem::filesystem_error If the initial existence check
 * cannot inspect the requested path.
 * @note Recoverable YAML/conversion failures retain the complete prior value;
 * no partially parsed fields or loaded path become externally visible.
 */
void load_or_create_config(const std::string& config_path, CliConfig& config) {
  static_assert(std::is_nothrow_move_assignable<CliConfig>::value,
                "CliConfig commit must not fail after successful parsing");
  if (fs::exists(config_path)) {
    CliConfig candidate = config;
    try {
      candidate.loaded_config_path = fs::absolute(config_path).string();
      YAML::Node root = YAML::LoadFile(config_path);
      if (root["cache_root_dir"])
        candidate.cache_root_dir = root["cache_root_dir"].as<std::string>();
      if (root["cache_precision"])
        candidate.cache_precision = root["cache_precision"].as<std::string>();
      if (root["history_size"])
        candidate.history_size = root["history_size"].as<int>();

      if (root["plugin_dirs"] && root["plugin_dirs"].IsSequence()) {
        candidate.plugin_dirs =
            root["plugin_dirs"].as<std::vector<std::string>>();
      } else if (root["plugin_dir"] && root["plugin_dir"].IsScalar()) {
        candidate.plugin_dirs.clear();
        candidate.plugin_dirs.push_back(root["plugin_dir"].as<std::string>());
      }

      // Scheduler directories
      if (root["scheduler_dirs"] && root["scheduler_dirs"].IsSequence()) {
        candidate.scheduler_dirs =
            root["scheduler_dirs"].as<std::vector<std::string>>();
      }

      if (root["default_print_mode"])
        candidate.default_print_mode =
            root["default_print_mode"].as<std::string>();
      // Backward-compat: map legacy values
      if (candidate.default_print_mode == "detailed" ||
          candidate.default_print_mode == "d")
        candidate.default_print_mode = "full";
      if (candidate.default_print_mode == "s")
        candidate.default_print_mode = "simplified";
      if (root["default_traversal_arg"])
        candidate.default_traversal_arg =
            root["default_traversal_arg"].as<std::string>();
      // Backward-compat: replace legacy 'detailed' (tree mode) with 'full'.
      // Do NOT rewrite token 'd' here because 'd' is a valid cache flag (disk).
      {
        std::string& s = candidate.default_traversal_arg;
        size_t pos;
        while ((pos = s.find("detailed")) != std::string::npos)
          s.replace(pos, 8, "full");
      }
      if (root["default_cache_clear_arg"])
        candidate.default_cache_clear_arg =
            root["default_cache_clear_arg"].as<std::string>();
      if (root["default_exit_save_path"])
        candidate.default_exit_save_path =
            root["default_exit_save_path"].as<std::string>();
      if (root["exit_prompt_sync"])
        candidate.exit_prompt_sync = root["exit_prompt_sync"].as<bool>();
      if (root["config_save_behavior"])
        candidate.config_save_behavior =
            root["config_save_behavior"].as<std::string>();
      if (root["editor_save_behavior"])
        candidate.editor_save_behavior =
            root["editor_save_behavior"].as<std::string>();
      if (root["default_timer_log_path"])
        candidate.default_timer_log_path =
            root["default_timer_log_path"].as<std::string>();
      if (root["default_ops_list_mode"])
        candidate.default_ops_list_mode =
            root["default_ops_list_mode"].as<std::string>();
      if (root["ops_plugin_path_mode"])
        candidate.ops_plugin_path_mode =
            root["ops_plugin_path_mode"].as<std::string>();
      if (root["default_compute_args"])
        candidate.default_compute_args =
            root["default_compute_args"].as<std::string>();
      if (root["switch_after_load"])
        candidate.switch_after_load = root["switch_after_load"].as<bool>();
      if (root["session_warning"])
        candidate.session_warning = root["session_warning"].as<bool>();

      // Scheduler selection from config accepts built-ins and scanned plugin
      // scheduler names.
      if (root["scheduler_hp_type"])
        candidate.scheduler_hp_type =
            root["scheduler_hp_type"].as<std::string>();
      if (root["scheduler_rt_type"])
        candidate.scheduler_rt_type =
            root["scheduler_rt_type"].as<std::string>();
      if (root["scheduler_worker_count"])
        candidate.scheduler_worker_count =
            root["scheduler_worker_count"].as<int>();
      validate_cli_scheduler_worker_count(candidate.scheduler_worker_count);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception& e) {
      std::cerr << "Warning: Could not parse config file '" << config_path
                << "'. Keeping previous settings. Error: " << e.what()
                << std::endl;
      return;
    }
    config = std::move(candidate);
    std::cout << "Loaded configuration from '" << config_path << "'."
              << std::endl;
  } else if (config_path == "config.yaml") {
    std::cout
        << "Configuration file 'config.yaml' not found. Creating a default one."
        << std::endl;
    CliConfig candidate;
    if (write_config_to_file(candidate, "config.yaml")) {
      candidate.loaded_config_path = fs::absolute("config.yaml").string();
    }
    config = std::move(candidate);
  }
}
