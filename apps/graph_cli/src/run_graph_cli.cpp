// Photospider CLI reusable option/run boundary.
#include <getopt.h>

#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <new>
#include <optional>
#include <string>

#include "graph_cli/cli_config.hpp"  // NOLINT(build/include_subdir)
#include "graph_cli/dependency_tree_formatter.hpp"
#include "graph_cli/print_cli_help.hpp"
#include "graph_cli/run_repl.hpp"
#include "photospider/host/host.hpp"

namespace fs = std::filesystem;

namespace {

/**
 * @brief Checks whether an argument vector requests CLI help.
 * @param argc Number of entries in `argv`.
 * @param argv Argument vector whose pointed-to strings remain valid.
 * @return True when `-h` or `--help` is present.
 * @throws Nothing.
 * @note The scan performs no allocation and avoids configuration/Host work on
 * the reusable boundary's help fast path.
 */
bool arguments_request_help(int argc, char** argv) noexcept {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-h") == 0 ||
        std::strcmp(argv[i], "--help") == 0) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Scans configured directories for scheduler plugins before graph load.
 * @param host Borrowed Host that owns loaded scheduler plugin handles.
 * @param config CLI value snapshot containing directories to scan.
 * @return Nothing.
 * @throws std::bad_alloc If path copying, plugin discovery, or Host result
 * storage exhausts memory.
 * @note An empty directory list performs no Host call; recoverable scan status
 * remains status-only and graph loading decides whether a configured type is
 * available.
 */
void load_configured_scheduler_plugins(ps::Host& host,
                                       const CliConfig& config) {
  if (!config.scheduler_dirs.empty()) {
    (void)host.scheduler_scan(config.scheduler_dirs);
  }
}

}  // namespace

/**
 * @brief Executes CLI configuration and option actions against a borrowed Host.
 * @param argc Number of command-line arguments.
 * @param argv Mutable argument vector consumed by `getopt_long`.
 * @param svc Borrowed Host that remains alive until this function returns.
 * @return Zero for success, one for invalid options, or two for recoverable
 * configuration, startup, option-action, or REPL standard exceptions.
 * @throws std::bad_alloc If CLI storage or any filesystem/Host/API operation
 * exhausts memory.
 * @note Parsing first locates configuration, then replays ordered actions. A
 * successful `-r` always supplies the invocation-local target for later graph
 * actions, independent of interactive switch policy. Replay continues after
 * recoverable action failures so later independent actions may run, but any
 * failed Host result or missing loaded-graph precondition makes the invocation
 * return two before either the implicit or requested REPL can start. Earlier
 * successful side effects are not rolled back. With no requested action, the
 * normal REPL fallback remains active. One outer catch chain covers
 * configuration and every action. Resource exhaustion is rethrown unchanged;
 * `main` alone owns the process exit-code and allocation-independent
 * diagnostic policy.
 */
int run_graph_cli(int argc, char** argv, ps::Host& svc) {
  try {
    if (arguments_request_help(argc, argv)) {
      print_cli_help();
      return 0;
    }

    optind = 1;
    (void)svc.seed_builtin_ops();

    CliConfig config;
    std::string custom_config_path;

    const char* const short_opts = "hr:o:ptR";
    const option long_opts[] = {{"help", no_argument, nullptr, 'h'},
                                {"read", required_argument, nullptr, 'r'},
                                {"output", required_argument, nullptr, 'o'},
                                {"print", no_argument, nullptr, 'p'},
                                {"traversal", no_argument, nullptr, 't'},
                                {"clear-cache", no_argument, nullptr, 1001},
                                {"repl", no_argument, nullptr, 'R'},
                                {"config", required_argument, nullptr, 2001},
                                {nullptr, 0, nullptr, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, short_opts, long_opts, nullptr)) !=
           -1) {
      if (opt == 2001) {
        custom_config_path = optarg;
      }
    }
    optind = 1;

    std::string config_to_load =
        custom_config_path.empty() ? "config.yaml" : custom_config_path;
    load_or_create_config(config_to_load, config);
    apply_cli_scheduler_defaults(svc, config);
    (void)svc.plugins_load(config.plugin_dirs);
    load_configured_scheduler_plugins(svc, config);
    std::string current_graph;

    bool did_any_action = false;
    bool action_failed = false;
    bool start_repl_after_actions = false;

    while ((opt = getopt_long(argc, argv, short_opts, long_opts, nullptr)) !=
           -1) {
      switch (opt) {
        case 'h':
          print_cli_help();
          return 0;
        case 'r': {
          auto result = svc.load_graph(ps::GraphLoadRequest{
              ps::GraphSessionId{"default"}, "sessions", optarg,
              config.loaded_config_path, config.cache_root_dir});
          if (result.status.ok) {
            current_graph = result.value.value;
            config.loaded_config_path =
                (fs::absolute(fs::path("sessions") / "default" / "config.yaml"))
                    .string();
            std::cout << "Loaded graph from " << optarg << "\n";
            did_any_action = true;
          } else {
            std::cerr << "Failed to load graph from '" << optarg << "'.\n";
            action_failed = true;
          }
          break;
        }
        case 'o': {
          if (current_graph.empty()) {
            std::cerr << "No graph loaded; use -r first.\n";
            action_failed = true;
            break;
          }
          if (svc.save_graph(ps::GraphSessionId{current_graph}, optarg)
                  .status.ok) {
            std::cout << "Saved graph to " << optarg << "\n";
            did_any_action = true;
          } else {
            std::cerr << "Failed to save graph.\n";
            action_failed = true;
          }
          break;
        }
        case 'p': {
          if (current_graph.empty()) {
            std::cerr << "No graph loaded; use -r first.\n";
            action_failed = true;
            break;
          }
          auto tree = svc.dependency_tree(ps::GraphSessionId{current_graph},
                                          std::nullopt);
          if (tree.status.ok) {
            std::cout << ps::cli::format_dependency_tree(
                tree.value, /*show_parameters*/ true);
            did_any_action = true;
          } else {
            std::cerr << "Failed to print tree.\n";
            action_failed = true;
          }
          break;
        }
        case 't': {
          if (current_graph.empty()) {
            std::cerr << "No graph loaded; use -r first.\n";
            action_failed = true;
            break;
          }
          auto tree = svc.dependency_tree(ps::GraphSessionId{current_graph},
                                          std::nullopt);
          if (tree.status.ok) {
            std::cout << ps::cli::format_dependency_tree(
                tree.value, /*show_parameters*/ true);
          } else {
            std::cerr << "Failed to print tree.\n";
            action_failed = true;
          }
          auto orders = svc.traversal_orders(ps::GraphSessionId{current_graph});
          if (orders.status.ok) {
            for (const auto& kv : orders.value) {
              std::cout << "\nPost-order (eval order) for end node " << kv.first
                        << ":\n";
              bool first = true;
              for (const auto& id : kv.second) {
                if (!first) {
                  std::cout << " -> ";
                }
                std::cout << id.value;
                first = false;
              }
              std::cout << "\n";
            }
          } else {
            std::cerr << "Failed to compute traversal.\n";
            action_failed = true;
          }
          did_any_action = true;
          break;
        }
        case 1001: {
          if (current_graph.empty()) {
            std::cerr << "No graph loaded; use -r first.\n";
            action_failed = true;
            break;
          }
          if (svc.clear_cache(ps::GraphSessionId{current_graph}).status.ok) {
            did_any_action = true;
          } else {
            std::cerr << "Failed to clear cache.\n";
            action_failed = true;
          }
          break;
        }
        case 'R':
          start_repl_after_actions = true;
          break;
        case 2001:
          break;
        default:
          print_cli_help();
          return 1;
      }
    }

    if (action_failed) {
      return 2;
    }

    if (start_repl_after_actions || !did_any_action) {
      if (did_any_action) {
        std::cout
            << "\n--- Command-line actions complete. Entering interactive "
               "shell. ---\n";
      }
      run_repl(svc, config, current_graph);
    } else {
      std::cout << "\n--- Command-line actions complete. Exiting. ---\n";
    }

    return 0;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << "\n";
    return 2;
  }
}
