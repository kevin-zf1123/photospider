// FILE: cli/graph_cli.cpp
#include <getopt.h>

#include <filesystem>
#include <iostream>
#include <opencv2/core/ocl.hpp>
#include <string>

#include "cli/dependency_tree_formatter.hpp"
#include "cli/print_cli_help.hpp"
#include "cli/run_repl.hpp"
#include "cli_config.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"

namespace fs = std::filesystem;

static void apply_scheduler_config(ps::Host& host, const CliConfig& config) {
  ps::HostSchedulerConfig scheduler_config;
  scheduler_config.hp_type = config.scheduler_hp_type;
  scheduler_config.rt_type = config.scheduler_rt_type;
  scheduler_config.worker_count =
      config.scheduler_worker_count > 0
          ? static_cast<unsigned int>(config.scheduler_worker_count)
          : 0;
  (void)host.configure_scheduler_defaults(scheduler_config);
}

static void load_configured_scheduler_plugins(ps::Host& host,
                                              const CliConfig& config) {
  if (!config.scheduler_dirs.empty()) {
    (void)host.scheduler_scan(config.scheduler_dirs);
  }
}

int main(int argc, char** argv) {
  // Hard-disable OpenCL runtime at process start to avoid spurious driver
  // errors
  setenv("OPENCV_OPENCL_DEVICE", "disabled", 1);
  setenv("OPENCV_OPENCL_RUNTIME", "disabled", 1);
  cv::ocl::setUseOpenCL(false);

  // Fast path: if only asking for help, avoid initializing plugins/Metal to
  // prevent destructor-order issues on early exit.
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_cli_help();
      return 0;
    }
  }

  auto host = ps::create_embedded_host();
  ps::Host& svc = *host;
  (void)svc.seed_builtin_ops();

  CliConfig config;
  std::string custom_config_path;

  const char* const short_opts = "hr:o:pt:R";
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
  apply_scheduler_config(svc, config);
  (void)svc.plugins_load(config.plugin_dirs);
  load_configured_scheduler_plugins(svc, config);
  std::string current_graph;

  bool did_any_action = false;
  bool start_repl_after_actions = false;

  while ((opt = getopt_long(argc, argv, short_opts, long_opts, nullptr)) !=
         -1) {
    try {
      switch (opt) {
        case 'h':
          print_cli_help();
          return 0;
        case 'r': {
          auto result = svc.load_graph(ps::GraphLoadRequest{
              ps::GraphSessionId{"default"}, "sessions", optarg,
              config.loaded_config_path, config.cache_root_dir});
          if (result.status.ok) {
            if (config.switch_after_load) {
              current_graph = result.value.value;
            }
            config.loaded_config_path =
                (fs::absolute(fs::path("sessions") / "default" / "config.yaml"))
                    .string();
            std::cout << "Loaded graph from " << optarg << "\n";
            did_any_action = true;
          } else {
            std::cerr << "Failed to load graph from '" << optarg << "'.\n";
          }
          break;
        }
        case 'o': {
          if (current_graph.empty()) {
            std::cerr << "No graph loaded; use -r first.\n";
            break;
          }
          if (svc.save_graph(ps::GraphSessionId{current_graph}, optarg)
                  .status.ok) {
            std::cout << "Saved graph to " << optarg << "\n";
            did_any_action = true;
          } else {
            std::cerr << "Failed to save graph.\n";
          }
          break;
        }
        case 'p': {
          if (current_graph.empty()) {
            std::cerr << "No graph loaded; use -r first.\n";
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
          }
          break;
        }
        case 't': {
          if (current_graph.empty()) {
            std::cerr << "No graph loaded; use -r first.\n";
            break;
          }
          auto tree = svc.dependency_tree(ps::GraphSessionId{current_graph},
                                          std::nullopt);
          if (tree.status.ok)
            std::cout << ps::cli::format_dependency_tree(
                tree.value, /*show_parameters*/ true);
          auto orders = svc.traversal_orders(ps::GraphSessionId{current_graph});
          if (orders.status.ok) {
            for (const auto& kv : orders.value) {
              std::cout << "\nPost-order (eval order) for end node " << kv.first
                        << ":\n";
              bool first = true;
              for (const auto& id : kv.second) {
                if (!first)
                  std::cout << " -> ";
                std::cout << id.value;
                first = false;
              }
              std::cout << "\n";
            }
          }
          did_any_action = true;
          break;
        }

        case 1001:
          if (!current_graph.empty()) {
            (void)svc.clear_cache(ps::GraphSessionId{current_graph});
            did_any_action = true;
          }
          break;
        case 'R':
          start_repl_after_actions = true;
          break;
        case 2001:
          break;
        default:
          print_cli_help();
          return 1;
      }
    } catch (const std::exception& e) {
      std::cerr << "Error: " << e.what() << "\n";
      return 2;
    }
  }

  if (start_repl_after_actions || !did_any_action) {
    if (did_any_action) {
      std::cout << "\n--- Command-line actions complete. Entering interactive "
                   "shell. ---\n";
    }
    run_repl(svc, config, current_graph);
  } else {
    std::cout << "\n--- Command-line actions complete. Exiting. ---\n";
  }

  return 0;
}
