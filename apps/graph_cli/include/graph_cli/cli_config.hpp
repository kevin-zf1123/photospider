// Lightweight CLI configuration definition and I/O declarations
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ps {
class Host;
}  // namespace ps

/**
 * @brief Value configuration consumed by the graph CLI frontend.
 *
 * @note The type intentionally remains in the global namespace to preserve the
 * existing CLI call sites. Configuration loading parses into a temporary value
 * and publishes it only after the complete YAML document succeeds.
 */
struct CliConfig {
  /** @brief Absolute path of the successfully loaded or created config file. */
  std::string loaded_config_path;
  /** @brief Root directory used for graph cache storage. */
  std::string cache_root_dir = "cache";
  /** @brief Directories scanned for operation plugins at CLI startup. */
  std::vector<std::string> plugin_dirs = {"build/plugins"};
  /** @brief Directories scanned for policy plugins at CLI startup. */
  std::vector<std::string> policy_dirs = {"build/policies"};
  /** @brief Default precision label used by cache operations. */
  std::string cache_precision = "int8";
  /** @brief Default dependency-tree rendering mode. */
  std::string default_print_mode = "full";
  /** @brief Default argument text for the REPL traversal command. */
  std::string default_traversal_arg = "n";
  /** @brief Default argument text for cache clearing. */
  std::string default_cache_clear_arg = "md";
  /** @brief Default graph path offered when saving during exit. */
  std::string default_exit_save_path = "graph_out.yaml";
  /** @brief Whether the exit flow prompts before synchronizing caches. */
  bool exit_prompt_sync = true;
  /** @brief Policy used when saving configuration changes. */
  std::string config_save_behavior = "current";
  /** @brief Policy used when saving editor changes. */
  std::string editor_save_behavior = "ask";
  /** @brief Default timer-log output path. */
  std::string default_timer_log_path = "out/timer.yaml";
  /** @brief Default operation-list filtering mode. */
  std::string default_ops_list_mode = "all";
  /** @brief Rendering policy for operation plugin source paths. */
  std::string ops_plugin_path_mode = "name_only";
  /** @brief Space-separated default flags for the REPL compute command. */
  std::string default_compute_args = "";
  /** @brief Maximum number of commands retained in interactive history. */
  int history_size = 1000;
  /** @brief Whether a successful interactive load selects the new session. */
  bool switch_after_load = true;
  /** @brief Whether overwriting an existing session emits a warning. */
  bool session_warning = true;

  /** @brief Process policy type bound to the Interactive service class. */
  std::string policy_interactive_type = "interactive";

  /** @brief Process policy type bound to the Throughput service class. */
  std::string policy_throughput_type = "throughput";

  /** @brief Default private route for future high-precision sessions. */
  std::string execution_hp_type = "cpu";

  /** @brief Default private route for future real-time session updates. */
  std::string execution_rt_type = "cpu";

  /** @brief Process worker request in `[0, 8]`; zero selects automatic. */
  int execution_worker_count = 0;
};

/**
 * @brief Validates one CLI execution worker-count value.
 *
 * @param worker_count Parsed or programmatically supplied CLI value.
 * @return Nothing.
 * @throws std::invalid_argument If `worker_count` is outside the inclusive
 * range from zero through the public execution request ceiling of eight.
 * @note Zero is preserved as automatic-resolution intent. The check performs
 * no Host call and mutates no configuration state.
 */
void validate_cli_execution_worker_count(int worker_count);

/**
 * @brief Strictly parses one execution worker count entered in the CLI editor.
 *
 * @param text Complete editor field text with no accepted trailing characters.
 * @return Parsed value in the inclusive range `[0, 8]`.
 * @throws std::invalid_argument If the text is empty, malformed, has trailing
 * characters, overflows `int`, or represents a value outside `[0, 8]`.
 * @throws std::bad_alloc If construction of the failure diagnostic exhausts
 * memory.
 * @note The parser performs no mutation, allowing the editor to retain its
 * previous configuration snapshot when validation fails.
 */
int parse_cli_execution_worker_count(std::string_view text);

/**
 * @brief Applies validated CLI policy and execution defaults to a Host.
 *
 * @param host Borrowed Host that owns process policy and future-session route
 * defaults.
 * @param config Complete CLI configuration snapshot to translate.
 * @return Nothing.
 * @throws std::invalid_argument If the CLI worker count is outside `[0, 8]`.
 * @throws std::runtime_error If the Host rejects the otherwise valid candidate.
 * @throws std::bad_alloc If validation diagnostics, copied policy/route
 * strings, Host result storage, or failure diagnostics cannot allocate.
 * @note All local syntax/range validation precedes the two Host calls. Each
 * Host operation is transactional in its own domain; a policy success is not
 * rolled back if the later execution-domain call reports an external race.
 */
void apply_cli_policy_execution_defaults(ps::Host& host,
                                         const CliConfig& config);

/**
 * @brief Serializes one CLI configuration to a YAML file.
 *
 * @param config Value snapshot to serialize without mutation.
 * @param path Destination YAML path.
 * @return True only when YAML construction, file opening, and stream writing
 * all succeed; false for other recoverable YAML, filesystem, or stream errors.
 * @throws std::bad_alloc If YAML, path, or stream storage exhausts memory.
 * @note Resource exhaustion is rethrown before the broad recoverable-error
 * translation. A false result never changes `config`.
 */
bool write_config_to_file(const CliConfig& config, const std::string& path);

/**
 * @brief Loads an existing CLI config or creates the default config file.
 *
 * @param config_path Existing YAML path, or the literal default
 * `config.yaml` path that may be created when absent.
 * @param config Destination value published only after complete parsing or
 * complete default construction succeeds.
 * @return Nothing.
 * @throws std::bad_alloc If filesystem, YAML, string, or container storage
 * exhausts memory.
 * @throws std::filesystem::filesystem_error If the initial existence check
 * cannot inspect the requested path.
 * @note A recoverable parse/conversion failure leaves `config` unchanged. A
 * missing non-default path also leaves it unchanged.
 */
void load_or_create_config(const std::string& config_path, CliConfig& config);

/**
 * @brief Runs graph CLI argument handling against an injected Host instance.
 *
 * @param argc Number of command-line arguments.
 * @param argv Mutable argument vector consumed by `getopt_long`.
 * @param host Borrowed Host that remains alive for the complete call.
 * @return Zero for success, one for invalid options, or two for recoverable
 * configuration, startup, option-action, or REPL standard exceptions.
 * @throws std::bad_alloc If Host/API, filesystem, or CLI working storage
 * exhausts memory.
 * @note Ordered option actions retain the Host-returned session from a
 * successful `-r` even when interactive switch-after-load policy is disabled.
 * One outer catch chain translates recoverable standard exceptions. The
 * process entry point alone owns resource-exhaustion exit policy, so this
 * reusable boundary preserves `std::bad_alloc` unchanged.
 */
int run_graph_cli(int argc, char** argv, ps::Host& host);
