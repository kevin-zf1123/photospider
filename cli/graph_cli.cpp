// FILE: cli/graph_cli.cpp
#include <optional>
#include <getopt.h>
#include <limits>
#include <regex>
#include <sstream>
#include <iostream>
#include <fstream>
#include <unordered_set>
#include <chrono>
#include <algorithm>
#include <map>
#include "graph_model.hpp"
#include <filesystem>
#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "cli/tui_editor.hpp"
#include "ftxui/dom/node.hpp"
#include <opencv2/core/ocl.hpp>

#include "cli/terminal_input.hpp"
#include "cli/cli_history.hpp"
#include "cli/cli_autocompleter.hpp"
#include "cli/path_complete.hpp"
#include "input_match_state.hpp"
#include "cli/path_complete.hpp"
#include "cli_config.hpp"
#include "plugin_loader.hpp"
#include "kernel/kernel.hpp"
#include "kernel/interaction.hpp"
#include "cli/node_editor.hpp"
#include "cli/node_editor_full.hpp"

// Extracted CLI functions
#include "cli/print_cli_help.hpp"
#include "cli/print_repl_help.hpp"
#include "cli/run_repl.hpp"
#include "cli/process_command.hpp"
#include "cli/handle_interactive_save.hpp"
#include "cli/do_traversal.hpp"
#include "cli/save_fp32_image.hpp"

using namespace ps;
using namespace ftxui;
namespace fs = std::filesystem;

// Front-end config editor extracted
#include "cli/config_editor.hpp"
// Kernel interaction layer
#include "kernel/kernel.hpp"
#include "kernel/interaction.hpp"



int main(int argc, char** argv) {
    // Hard-disable OpenCL runtime at process start to avoid spurious driver errors
    setenv("OPENCV_OPENCL_DEVICE", "disabled", 1);
    setenv("OPENCV_OPENCL_RUNTIME", "disabled", 1);
    cv::ocl::setUseOpenCL(false);

    // Fast path: if only asking for help, avoid initializing plugins/Metal to prevent
    // destructor-order issues on early exit.
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_cli_help();
            return 0;
        }
    }

    ps::Kernel kernel;
    ps::InteractionService svc(kernel);
    svc.cmd_seed_builtin_ops();

    CliConfig config;
    std::string custom_config_path;

    const char* const short_opts = "hr:o:pt:R";
    const option long_opts[] = {
        {"help", no_argument, nullptr, 'h'}, {"read", required_argument, nullptr, 'r'},
        {"output", required_argument, nullptr, 'o'}, {"print", no_argument, nullptr, 'p'}, 
        {"traversal", no_argument, nullptr, 't'}, {"clear-cache", no_argument, nullptr, 1001},
        {"repl", no_argument, nullptr, 'R'}, {"config", required_argument, nullptr, 2001},
        {nullptr, 0, nullptr, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, short_opts, long_opts, nullptr)) != -1) {
        if (opt == 2001) { custom_config_path = optarg; }
    }
    optind = 1;

    std::string config_to_load = custom_config_path.empty() ? "config.yaml" : custom_config_path;
    load_or_create_config(config_to_load, config);
    svc.cmd_plugins_load(config.plugin_dirs);
    std::string current_graph;
    
    bool did_any_action = false;
    bool start_repl_after_actions = false;

    while ((opt = getopt_long(argc, argv, short_opts, long_opts, nullptr)) != -1) {
        try {
            switch (opt) {
            case 'h': print_cli_help(); return 0;
            case 'r': {
                auto ok = svc.cmd_load_graph("default", "sessions", optarg, config.loaded_config_path);
                if (ok) {
                    if (config.switch_after_load) current_graph = *ok;
                    config.loaded_config_path = (ps::fs::absolute(ps::fs::path("sessions")/"default"/"config.yaml")).string();
                    std::cout << "Loaded graph from " << optarg << "\n"; did_any_action = true;
                }
                else { std::cerr << "Failed to load graph from '" << optarg << "'.\n"; }
                break; }
            case 'o': {
                if (current_graph.empty()) { std::cerr << "No graph loaded; use -r first.\n"; break; }
                if (svc.cmd_save_yaml(current_graph, optarg)) { std::cout << "Saved graph to " << optarg << "\n"; did_any_action = true; }
                else { std::cerr << "Failed to save graph.\n"; }
                break; }
            case 'p': {
                if (current_graph.empty()) { std::cerr << "No graph loaded; use -r first.\n"; break; }
                auto dump = svc.cmd_dump_tree(current_graph, std::nullopt, /*show_params*/true);
                if (dump) { std::cout << *dump; did_any_action = true; }
                else std::cerr << "Failed to print tree.\n";
                break; }
            case 't': {
                if (current_graph.empty()) { std::cerr << "No graph loaded; use -r first.\n"; break; }
                auto dump = svc.cmd_dump_tree(current_graph, std::nullopt, /*show_params*/true);
                if (dump) std::cout << *dump;
                auto orders = svc.cmd_traversal_orders(current_graph);
                if (orders) {
                    for (const auto& kv : *orders) {
                        std::cout << "\nPost-order (eval order) for end node " << kv.first << ":\n";
                        bool first = true; for (int id : kv.second) { if (!first) std::cout << " -> "; std::cout << id; first = false; }
                        std::cout << "\n";
                    }
                }
                did_any_action = true; break; }
            
            case 1001: if (!current_graph.empty()) { svc.cmd_clear_cache(current_graph); did_any_action = true; } break;
            case 'R': start_repl_after_actions = true; break;
            case 2001: break;
            default: print_cli_help(); return 1;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n"; return 2;
        }
    }

    if (start_repl_after_actions || !did_any_action) {
        if (did_any_action) {
            std::cout << "\n--- Command-line actions complete. Entering interactive shell. ---\n";
        }
        run_repl(svc, config, current_graph);
    } else {
         std::cout << "\n--- Command-line actions complete. Exiting. ---\n";
    }
    
    return 0;
}
