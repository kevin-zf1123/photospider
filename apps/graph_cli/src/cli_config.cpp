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

namespace {

/**
 * @brief Reports whether one CLI policy type has canonical ABI spelling.
 * @param value Candidate policy type bytes.
 * @return True for 1..128 ASCII bytes matching `[a-z][a-z0-9_.-]*`.
 * @throws Nothing.
 * @note This preflight prevents partial cross-domain default application; the
 * Host remains authoritative for registry visibility and class support.
 */
bool canonical_policy_type(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128U || value.front() < 'a' ||
      value.front() > 'z') {
    return false;
  }
  for (std::size_t index = 1U; index < value.size(); ++index) {
    const char byte = value[index];
    if (!((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
          byte == '_' || byte == '.' || byte == '-')) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Reports whether one CLI execution route is part of the fixed set.
 * @param value Candidate route name.
 * @return True only for `cpu`, `gpu_pipeline`, or `serial_debug`.
 * @throws Nothing.
 */
bool known_execution_type(std::string_view value) noexcept {
  return value == "cpu" || value == "gpu_pipeline" || value == "serial_debug";
}

/**
 * @brief Converts one failed Host default operation into a CLI exception.
 * @param operation Human-readable operation label.
 * @param status Host-owned copied operation status.
 * @return Nothing when status is successful.
 * @throws std::runtime_error when status reports failure.
 * @throws std::bad_alloc when diagnostic construction exhausts memory.
 */
void require_host_success(std::string_view operation,
                          const ps::OperationStatus& status) {
  if (status.ok) {
    return;
  }
  std::string diagnostic = "Host rejected ";
  diagnostic.append(operation);
  if (!status.name.empty()) {
    diagnostic += " (" + status.name + ")";
  }
  if (!status.message.empty()) {
    diagnostic += ": " + status.message;
  }
  throw std::runtime_error(diagnostic);
}

/**
 * @brief Rejects removed scheduler configuration keys before field parsing.
 * @param root Complete parsed YAML mapping.
 * @return Nothing when no removed key is present.
 * @throws std::invalid_argument when a key begins with `scheduler_`.
 * @throws YAML::Exception when a mapping key is not convertible to text.
 * @throws std::bad_alloc when copied key or diagnostic storage exhausts memory.
 * @note No compatibility translation is provided for the breaking generation.
 */
void reject_removed_scheduler_keys(const YAML::Node& root) {
  if (!root.IsMap()) {
    throw std::invalid_argument("CLI configuration root must be a mapping");
  }
  for (const auto& field : root) {
    const std::string key = field.first.as<std::string>();
    if (key.rfind("scheduler_", 0U) == 0U) {
      throw std::invalid_argument("unknown removed configuration key: " + key);
    }
  }
}

}  // namespace

/**
 * @brief Validates one CLI execution worker-count value.
 *
 * @param worker_count Parsed or programmatically supplied CLI value.
 * @return Nothing.
 * @throws std::invalid_argument If the count is outside `[0, 8]`.
 * @note Zero remains automatic-resolution intent and no state is mutated.
 */
void validate_cli_execution_worker_count(int worker_count) {
  if (worker_count < 0 || static_cast<unsigned int>(worker_count) >
                              ps::kExecutionWorkerRequestMax) {
    throw std::invalid_argument(
        "execution_worker_count must be an integer in range [0, 8]");
  }
}

/**
 * @brief Strictly parses one CLI editor execution worker-count value.
 *
 * @param text Complete editor text to parse.
 * @return Parsed count in `[0, 8]`.
 * @throws std::invalid_argument If parsing is incomplete, overflows, or yields
 * a value outside the public request range.
 * @throws std::bad_alloc If failure diagnostic construction cannot allocate.
 * @note Leading/trailing whitespace and trailing characters are rejected so
 * the editor cannot silently coerce a partially valid token.
 */
int parse_cli_execution_worker_count(std::string_view text) {
  if (text.empty()) {
    throw std::invalid_argument(
        "execution_worker_count must be an integer in range [0, 8]");
  }
  int worker_count = 0;
  const char* const first = text.data();
  const char* const last = first + text.size();
  const auto parsed = std::from_chars(first, last, worker_count);
  if (parsed.ec != std::errc{} || parsed.ptr != last) {
    throw std::invalid_argument(
        "execution_worker_count must be an integer in range [0, 8]");
  }
  validate_cli_execution_worker_count(worker_count);
  return worker_count;
}

/**
 * @brief Applies validated CLI policy and execution defaults to one Host.
 *
 * @param host Borrowed Host receiving process policy and future-session route
 * defaults.
 * @param config Complete CLI snapshot to translate.
 * @return Nothing.
 * @throws std::invalid_argument If the count is outside `[0, 8]`.
 * @throws std::runtime_error If the Host rejects the complete candidate.
 * @throws std::bad_alloc If policy/route configuration or diagnostics cannot
 * allocate.
 * @note Validation happens before the exactly-once Host call. Host failure is
 * exceptional here so both startup and interactive CLI boundaries observe it.
 */
void apply_cli_policy_execution_defaults(ps::Host& host,
                                         const CliConfig& config) {
  validate_cli_execution_worker_count(config.execution_worker_count);
  if (!canonical_policy_type(config.policy_interactive_type) ||
      !canonical_policy_type(config.policy_throughput_type)) {
    throw std::invalid_argument("policy defaults require canonical type names");
  }
  if (!known_execution_type(config.execution_hp_type) ||
      !known_execution_type(config.execution_rt_type)) {
    throw std::invalid_argument(
        "execution defaults require cpu, gpu_pipeline, or serial_debug");
  }

  ps::HostPolicyConfig policy_config;
  policy_config.interactive_type = config.policy_interactive_type;
  policy_config.throughput_type = config.policy_throughput_type;
  require_host_success("policy defaults",
                       host.configure_policy_defaults(policy_config).status);

  ps::HostExecutionConfig execution_config;
  execution_config.hp_type = config.execution_hp_type;
  execution_config.rt_type = config.execution_rt_type;
  execution_config.worker_count =
      static_cast<unsigned int>(config.execution_worker_count);
  require_host_success(
      "execution defaults",
      host.configure_execution_defaults(execution_config).status);
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
    root["policy_dirs"] = config.policy_dirs;
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
    root["policy_interactive_type"] = config.policy_interactive_type;
    root["policy_throughput_type"] = config.policy_throughput_type;
    root["execution_hp_type"] = config.execution_hp_type;
    root["execution_rt_type"] = config.execution_rt_type;
    root["execution_worker_count"] = config.execution_worker_count;

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
      reject_removed_scheduler_keys(root);
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

      if (root["policy_dirs"] && root["policy_dirs"].IsSequence()) {
        candidate.policy_dirs =
            root["policy_dirs"].as<std::vector<std::string>>();
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

      if (root["policy_interactive_type"])
        candidate.policy_interactive_type =
            root["policy_interactive_type"].as<std::string>();
      if (root["policy_throughput_type"])
        candidate.policy_throughput_type =
            root["policy_throughput_type"].as<std::string>();
      if (root["execution_hp_type"])
        candidate.execution_hp_type =
            root["execution_hp_type"].as<std::string>();
      if (root["execution_rt_type"])
        candidate.execution_rt_type =
            root["execution_rt_type"].as<std::string>();
      if (root["execution_worker_count"])
        candidate.execution_worker_count =
            root["execution_worker_count"].as<int>();
      validate_cli_execution_worker_count(candidate.execution_worker_count);
      if (!canonical_policy_type(candidate.policy_interactive_type) ||
          !canonical_policy_type(candidate.policy_throughput_type) ||
          !known_execution_type(candidate.execution_hp_type) ||
          !known_execution_type(candidate.execution_rt_type)) {
        throw std::invalid_argument(
            "configuration contains an invalid policy or execution type");
      }
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
