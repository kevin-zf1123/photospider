// FILE: include/cli/command/commands.hpp
#pragma once

#include <new>
#include <sstream>
#include <string>

#include "cli_config.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"

// Each command exposes a Host-facing handler and a maintained help renderer.

// help
/**
 * @brief Prints general help or command-specific help for the REPL.
 * @param iss Command arguments containing an optional command name.
 * @param svc Borrowed Host compatibility parameter; not used.
 * @param current_graph Borrowed session state; not modified.
 * @param modified Borrowed modification state; not modified.
 * @param config CLI configuration used to render maintained help.
 * @return True so the REPL continues.
 * @throws std::bad_alloc if parsing or help rendering cannot allocate.
 * @note No graph, cache, Host ownership, or background-thread state changes.
 */
bool handle_help(std::istringstream& iss, ps::Host& svc,
                 std::string& current_graph, bool& modified, CliConfig& config);
/**
 * @brief Prints maintained help for `help`.
 * @param config Borrowed CLI configuration used by the help renderer.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_help(const CliConfig& config);

// clear / cls
/**
 * @brief Clears the interactive terminal display.
 * @param iss Borrowed command stream; ignored.
 * @param svc Borrowed Host compatibility parameter; not used.
 * @param current_graph Borrowed session state; not modified.
 * @param modified Borrowed modification state; not modified.
 * @param config Borrowed CLI configuration; not modified.
 * @return True so the REPL continues.
 * @throws std::bad_alloc only if standard stream buffering cannot allocate.
 * @note This is a synchronous terminal side effect with no Host/cache effect.
 */
bool handle_clear(std::istringstream& iss, ps::Host& svc,
                  std::string& current_graph, bool& modified,
                  CliConfig& config);
/**
 * @brief Prints maintained help for `clear`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_clear(const CliConfig& config);

// graphs
/**
 * @brief Lists Host-owned graph sessions and marks the current session.
 * @param iss Borrowed command stream; ignored.
 * @param svc Borrowed product Host used for the session snapshot.
 * @param current_graph Current session label used only for presentation.
 * @param modified Borrowed modification state; not modified.
 * @param config Borrowed CLI configuration; not modified.
 * @return True after success or a recoverable Host status failure.
 * @throws std::bad_alloc if Host result or output storage cannot allocate.
 * @note The returned snapshot is CLI-owned; Host session lifetime is unchanged.
 */
bool handle_graphs(std::istringstream& iss, ps::Host& svc,
                   std::string& current_graph, bool& modified,
                   CliConfig& config);
/**
 * @brief Prints maintained help for `graphs`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_graphs(const CliConfig& config);

// load
/**
 * @brief Loads or reloads a graph session through the product Host.
 * @param iss Arguments in `load <name> [yaml]` or `load <yaml>` form.
 * @param svc Borrowed Host used for graph lifecycle operations.
 * @param current_graph Active session label, updated when configured.
 * @param modified Modification flag cleared after a successful reload.
 * @param config Session paths, warnings, cache root, and switch policy.
 * @return True after success, cancellation, validation, or status failure.
 * @throws std::bad_alloc if parsing, paths, Host results, or output allocate.
 * @throws std::filesystem::filesystem_error for uncaught path queries.
 * @note Host owns graph sessions; the CLI owns only labels and config paths.
 */
bool handle_load(std::istringstream& iss, ps::Host& svc,
                 std::string& current_graph, bool& modified, CliConfig& config);
/**
 * @brief Prints maintained help for `load`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_load(const CliConfig& config);

// switch
/**
 * @brief Switches sessions or copies the active session before switching.
 * @param iss Arguments containing a target session and optional `c` flag.
 * @param svc Borrowed Host used for save/load/reload and session snapshots.
 * @param current_graph Active session label updated on success.
 * @param modified Borrowed modification state; not changed by this handler.
 * @param config Mutable session warning and configuration-path state.
 * @return True after success or a recoverable command failure.
 * @throws std::bad_alloc if parsing, filesystem, Host, or output allocates.
 * @throws std::filesystem::filesystem_error for uncaught absolute-path work.
 * @note File-copy failures are rendered locally; Host retains session
 * ownership.
 */
bool handle_switch(std::istringstream& iss, ps::Host& svc,
                   std::string& current_graph, bool& modified,
                   CliConfig& config);
/**
 * @brief Prints maintained help for `switch`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_switch(const CliConfig& config);

// close
/**
 * @brief Closes a named Host graph session.
 * @param iss Arguments containing the session name.
 * @param svc Borrowed Host used for graph lifecycle mutation.
 * @param current_graph Active label, cleared when that session closes.
 * @param modified Borrowed modification state; not modified.
 * @param config Borrowed CLI configuration; not modified.
 * @return True after success, usage output, or Host status failure.
 * @throws std::bad_alloc if parsing, Host status, or output cannot allocate.
 * @note Closing releases Host-owned session state; no background work is kept.
 */
bool handle_close(std::istringstream& iss, ps::Host& svc,
                  std::string& current_graph, bool& modified,
                  CliConfig& config);
/**
 * @brief Prints maintained help for `close`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_close(const CliConfig& config);

// print
/**
 * @brief Prints a Host dependency-tree snapshot for the active graph.
 * @param iss Tree-format flags and an optional target node id.
 * @param svc Borrowed Host used for dependency inspection.
 * @param current_graph Active session label.
 * @param modified Borrowed modification state; not modified.
 * @param config Default tree-format configuration.
 * @return True after rendering or a recoverable validation/status failure.
 * @throws std::bad_alloc if parsing, snapshot formatting, or output allocates.
 * @note Inspection is synchronous and does not mutate graph or cache state.
 */
bool handle_print(std::istringstream& iss, ps::Host& svc,
                  std::string& current_graph, bool& modified,
                  CliConfig& config);
/**
 * @brief Prints maintained help for `print`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_print(const CliConfig& config);

// inspect
/**
 * @brief Inspects the active graph, one node, or the dirty-region snapshot.
 * @param iss Arguments containing `all`, `dirty`, or a node id.
 * @param svc Borrowed Host used for immutable inspection snapshots.
 * @param current_graph Active session label.
 * @param modified Borrowed modification state; not modified.
 * @param config Borrowed CLI configuration; not modified.
 * @return True after rendering or a recoverable validation/status failure.
 * @throws std::bad_alloc if Host results, formatting, or output allocate.
 * @note Host failures remain statuses; no graph/cache ownership is transferred.
 */
bool handle_inspect(std::istringstream& iss, ps::Host& svc,
                    std::string& current_graph, bool& modified,
                    CliConfig& config);
/**
 * @brief Prints maintained help for `inspect`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_inspect(const CliConfig& config);

// node
/**
 * @brief Runs the node editor for the active Host graph session.
 * @param iss Arguments containing an optional initial node id.
 * @param svc Borrowed Host used synchronously by the editor.
 * @param current_graph Active session label.
 * @param modified Borrowed modification state; not modified here.
 * @param config Borrowed CLI configuration; not modified here.
 * @return True after the editor exits.
 * @throws std::bad_alloc if parsing, editor state, or Host results allocate.
 * @note The editor may mutate Host graph state but does not retain the Host.
 */
bool handle_node(std::istringstream& iss, ps::Host& svc,
                 std::string& current_graph, bool& modified, CliConfig& config);
/**
 * @brief Prints maintained help for `node`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_node(const CliConfig& config);

// ops
/**
 * @brief Lists registered operation sources through the product Host.
 * @param iss Arguments selecting all, built-in, or plugin operations.
 * @param svc Borrowed Host used for the operation-source snapshot.
 * @param current_graph Borrowed session state; not modified.
 * @param modified Borrowed modification state; not modified.
 * @param config Default list and plugin-path display modes.
 * @return True after rendering or a recoverable Host status failure.
 * @throws std::bad_alloc if grouping, sorting, paths, or output allocate.
 * @throws std::filesystem::filesystem_error if relative-path conversion fails.
 * @note Snapshot strings are CLI-owned; plugin handles remain Host-owned.
 */
bool handle_ops(std::istringstream& iss, ps::Host& svc,
                std::string& current_graph, bool& modified, CliConfig& config);
/**
 * @brief Prints maintained help for `ops`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_ops(const CliConfig& config);

/**
 * @brief Executes the CLI `bench` command through BenchmarkService and Host.
 *
 * @param iss Command arguments containing benchmark and output directories.
 * @param svc Product Host used for graph lifecycle and compute.
 * @param current_graph Current graph compatibility state; not modified.
 * @param modified Modification compatibility state; not modified.
 * @param config CLI configuration compatibility state; not modified.
 * @return True after handling recoverable command outcomes.
 * @throws std::bad_alloc unchanged if CLI, benchmark, Host, or result storage
 * exhausts memory.
 * @note Other standard exceptions are printed as command failures.
 */
bool handle_bench(std::istringstream& iss, ps::Host& svc,
                  std::string& current_graph, bool& modified,
                  CliConfig& config);

/**
 * @brief Prints maintained help for the CLI `bench` command.
 *
 * @param config CLI configuration compatibility state; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage exhausts memory.
 * @note Help content remains owned by the shared CLI help utility.
 */
void print_help_bench(const CliConfig& config);

/**
 * @brief Opens the benchmark-configuration editor for one directory.
 * @param iss Arguments containing the benchmark directory.
 * @param svc Borrowed Host used to enumerate operation types.
 * @param current_graph Borrowed session state; not modified.
 * @param modified Borrowed modification state; not modified.
 * @param config Borrowed CLI configuration; not modified.
 * @return True after the editor exits or usage help is shown.
 * @throws std::bad_alloc if parsing, filesystem, Host, YAML, or TUI allocates.
 * @throws std::filesystem::filesystem_error if directory creation fails.
 * @note The fullscreen editor borrows Host only for its synchronous lifetime.
 */
bool handle_benchmark(std::istringstream& iss, ps::Host& svc,
                      std::string& current_graph, bool& modified,
                      CliConfig& config);
/**
 * @brief Prints maintained help for `benchmark`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_benchmark(const CliConfig& config);

// traversal
/**
 * @brief Traverses the active graph and optionally synchronizes cache state.
 * @param iss Tree, cache-display, and cache-action flags.
 * @param svc Borrowed Host used for cache actions and traversal snapshots.
 * @param current_graph Active session label.
 * @param modified Borrowed modification state; not modified.
 * @param config Default traversal flags and cache precision.
 * @return True after rendering or a recoverable Host status failure.
 * @throws std::bad_alloc if parsing, Host results, formatting, or output
 * allocates.
 * @note Cache actions are synchronous; Host retains graph and cache ownership.
 */
bool handle_traversal(std::istringstream& iss, ps::Host& svc,
                      std::string& current_graph, bool& modified,
                      CliConfig& config);
/**
 * @brief Prints maintained help for `traversal`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_traversal(const CliConfig& config);

// config
/**
 * @brief Edits CLI configuration and applies scheduler defaults to Host.
 * @param iss Borrowed command stream; ignored.
 * @param svc Borrowed Host receiving accepted scheduler defaults.
 * @param current_graph Borrowed session state; not modified.
 * @param modified Borrowed modification state; not modified.
 * @param config Mutable configuration owned by the REPL.
 * @return True after the editor exits.
 * @throws std::bad_alloc if editor or Host-result storage cannot allocate.
 * @note Host scheduler defaults change only after the editor accepts changes.
 */
bool handle_config(std::istringstream& iss, ps::Host& svc,
                   std::string& current_graph, bool& modified,
                   CliConfig& config);
/**
 * @brief Prints maintained help for `config`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_config(const CliConfig& config);

// read
/**
 * @brief Reloads the active Host graph from a YAML file.
 * @param iss Arguments containing the input file path.
 * @param svc Borrowed Host used for graph reload.
 * @param current_graph Active session label.
 * @param modified Modification flag cleared after successful reload.
 * @param config Borrowed CLI configuration; not modified.
 * @return True after success, usage output, or Host status failure.
 * @throws std::bad_alloc if parsing, Host status, or output cannot allocate.
 * @note Host owns the reloaded graph; this handler only updates CLI state.
 */
bool handle_read(std::istringstream& iss, ps::Host& svc,
                 std::string& current_graph, bool& modified, CliConfig& config);
/**
 * @brief Prints maintained help for `read`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_read(const CliConfig& config);

// source
/**
 * @brief Executes commands from a text script against the current REPL state.
 * @param iss Arguments containing the script filename.
 * @param svc Borrowed Host forwarded to each parsed command.
 * @param current_graph Mutable active session label shared by script commands.
 * @param modified Mutable modification flag shared by script commands.
 * @param config Mutable CLI configuration shared by script commands.
 * @return False if a nested command requests exit; true otherwise.
 * @throws std::bad_alloc if parsing, nested commands, or output allocate.
 * @note The file is synchronously consumed and no stream or Host is retained.
 */
bool handle_source(std::istringstream& iss, ps::Host& svc,
                   std::string& current_graph, bool& modified,
                   CliConfig& config);
/**
 * @brief Prints maintained help for `source`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_source(const CliConfig& config);

// output
/**
 * @brief Saves the active Host graph to a YAML file.
 * @param iss Arguments containing the output path.
 * @param svc Borrowed Host used for graph serialization.
 * @param current_graph Active session label.
 * @param modified Modification flag cleared after successful save.
 * @param config Borrowed CLI configuration; not modified.
 * @return True after success, usage output, or Host status failure.
 * @throws std::bad_alloc if parsing, Host status, or output cannot allocate.
 * @note Host performs serialization; the CLI retains only session metadata.
 */
bool handle_output(std::istringstream& iss, ps::Host& svc,
                   std::string& current_graph, bool& modified,
                   CliConfig& config);
/**
 * @brief Prints maintained help for `output`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_output(const CliConfig& config);

// clear-graph
/**
 * @brief Clears all nodes from the active Host graph session.
 * @param iss Borrowed command stream; ignored.
 * @param svc Borrowed Host used for graph mutation.
 * @param current_graph Active session label.
 * @param modified Modification flag set after the clear request.
 * @param config Borrowed CLI configuration; not modified.
 * @return True after success or missing-session output.
 * @throws std::bad_alloc if Host status or output storage cannot allocate.
 * @note Host owns cleared graph/cache state; the session itself remains open.
 */
bool handle_clear_graph(std::istringstream& iss, ps::Host& svc,
                        std::string& current_graph, bool& modified,
                        CliConfig& config);
/**
 * @brief Prints maintained help for `clear-graph`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_clear_graph(const CliConfig& config);

// clear-cache / cc
/**
 * @brief Clears memory, drive, or both caches for the active graph.
 * @param iss Arguments selecting the cache tier.
 * @param svc Borrowed Host used for cache mutation.
 * @param current_graph Active session label.
 * @param modified Borrowed graph-modification flag; not changed.
 * @param config Default cache-clear selection.
 * @return True after success, validation, or missing-session output.
 * @throws std::bad_alloc if parsing, Host status, or output cannot allocate.
 * @note Cache ownership remains with Host and the operation is synchronous.
 */
bool handle_clear_cache(std::istringstream& iss, ps::Host& svc,
                        std::string& current_graph, bool& modified,
                        CliConfig& config);
/**
 * @brief Prints maintained help for `clear-cache`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_clear_cache(const CliConfig& config);

// free
/**
 * @brief Releases transient intermediate memory for the active graph.
 * @param iss Borrowed command stream; ignored.
 * @param svc Borrowed Host used for transient-memory release.
 * @param current_graph Active session label.
 * @param modified Borrowed modification state; not modified.
 * @param config Borrowed CLI configuration; not modified.
 * @return True after success or missing-session output.
 * @throws std::bad_alloc if Host status or output storage cannot allocate.
 * @note Persistent graph/cache ownership remains with Host.
 */
bool handle_free(std::istringstream& iss, ps::Host& svc,
                 std::string& current_graph, bool& modified, CliConfig& config);
/**
 * @brief Prints maintained help for `free`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_free(const CliConfig& config);

// compute
/**
 * @brief Parses, schedules, and waits for Host compute requests.
 * @param iss Target node or `all` followed by compute/cache/timing flags.
 * @param svc Borrowed Host used for async compute, events, and diagnostics.
 * @param current_graph Active session label.
 * @param modified Borrowed modification state; not modified.
 * @param config Default compute flags, precision, and timing path.
 * @return True after success or a recoverable validation/status failure.
 * @throws std::bad_alloc if request, Host result, future, or output allocates.
 * @throws std::out_of_range if a numeric target exceeds the supported range.
 * @note Futures are consumed before return; no Host or callback is retained.
 */
bool handle_compute(std::istringstream& iss, ps::Host& svc,
                    std::string& current_graph, bool& modified,
                    CliConfig& config);
/**
 * @brief Prints maintained help for `compute`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_compute(const CliConfig& config);

// save
/**
 * @brief Computes a node image through Host and saves its CPU view.
 * @param iss Arguments containing `<node_id> <file>`.
 * @param svc Borrowed Host used for compute-and-get-image.
 * @param current_graph Active session label.
 * @param modified Borrowed modification state; not modified.
 * @param config Cache precision and image-output settings.
 * @return True after success or a recoverable command/Host failure.
 * @throws std::bad_alloc if parsing, Host image, or save storage allocates.
 * @note The cv::Mat view borrows Host result memory only until this call ends.
 */
bool handle_save(std::istringstream& iss, ps::Host& svc,
                 std::string& current_graph, bool& modified, CliConfig& config);
/**
 * @brief Prints maintained help for `save`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_save(const CliConfig& config);

// exit / quit / q
/**
 * @brief Optionally synchronizes disk cache, then requests REPL shutdown.
 * @param iss Borrowed command stream; ignored.
 * @param svc Borrowed Host used for optional cache synchronization.
 * @param current_graph Active session label.
 * @param modified Borrowed modification state; not modified.
 * @param config Exit prompt default and cache precision.
 * @return False to terminate the REPL.
 * @throws std::bad_alloc if prompt, Host status, or output cannot allocate.
 * @note Cache synchronization is synchronous; Host lifetime is caller-owned.
 */
bool handle_exit(std::istringstream& iss, ps::Host& svc,
                 std::string& current_graph, bool& modified, CliConfig& config);
/**
 * @brief Prints maintained help for `exit`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_exit(const CliConfig& config);

// scheduler
/**
 * @brief Inspects, loads, scans, and configures Host scheduler state.
 * @param iss Scheduler subcommand and its arguments.
 * @param svc Borrowed Host used for scheduler registry/session operations.
 * @param current_graph Active session label for intent-specific operations.
 * @param modified Borrowed graph-modification state; not modified.
 * @param config Scheduler directories and default scheduler settings.
 * @return True after success, help, validation, or Host status failure.
 * @throws std::bad_alloc if parsing, plugin snapshots, or output allocates.
 * @note Host owns scheduler plugins and workers; CLI retains no handles.
 */
bool handle_scheduler(std::istringstream& iss, ps::Host& svc,
                      std::string& current_graph, bool& modified,
                      CliConfig& config);
/**
 * @brief Prints maintained help for `scheduler`.
 * @param config Borrowed CLI configuration; currently unused.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage cannot allocate.
 * @note No Host, graph, cache, or lifecycle state changes.
 */
void print_help_scheduler(const CliConfig& config);
