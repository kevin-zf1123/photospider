// FILE: src/cli/command/command_switch.cpp
#include <iostream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <filesystem>
#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"
#include "cli/ask_yesno.hpp"

namespace fs = std::filesystem;

bool handle_switch(std::istringstream& iss,
                   ps::InteractionService& svc,
                   std::string& current_graph,
                   bool& /*modified*/,
                   CliConfig& config) {
    // Extended: optional 'c' arg to copy current session to target session then switch
    std::string name; iss >> name; if (name.empty()) { std::cout << "Usage: switch <name> [c]\n"; return true; }
    std::string arg; iss >> arg;
    if (arg == "c") {
        if (current_graph.empty()) { std::cout << "No current graph to copy. Use load first.\n"; return true; }
        if (name == current_graph) { std::cout << "Target session equals current; nothing to copy.\n"; return true; }

        // Ensure current session YAML is saved before copying
        fs::path src_yaml = fs::path("sessions")/current_graph/"content.yaml";
        (void)svc.cmd_save_yaml(current_graph, src_yaml.string());
        fs::path src_cfg = fs::path("sessions")/current_graph/"config.yaml";

        fs::path dst_dir = fs::path("sessions")/name;
        fs::path dst_yaml = dst_dir/"content.yaml";
        fs::path dst_cfg = dst_dir/"config.yaml";

        bool will_overwrite = fs::exists(dst_yaml) || fs::exists(dst_cfg);
        if (config.session_warning && will_overwrite) {
            if (!ask_yesno("Session '" + name + "' already exists and will be overwritten by copy. Continue?", true)) {
                std::cout << "Aborted." << std::endl; return true;
            }
        }

        try {
            fs::create_directories(dst_dir);
            if (fs::exists(src_yaml)) fs::copy_file(src_yaml, dst_yaml, fs::copy_options::overwrite_existing);
            if (fs::exists(src_cfg)) fs::copy_file(src_cfg, dst_cfg, fs::copy_options::overwrite_existing);
        } catch (const std::exception& e) {
            std::cout << "Error: failed to copy session files: " << e.what() << "\n"; return true;
        }

        // If target graph already loaded, reload its YAML; else load it.
        auto loaded = svc.cmd_list_graphs();
        if (std::find(loaded.begin(), loaded.end(), name) != loaded.end()) {
            if (!svc.cmd_reload_yaml(name, dst_yaml.string())) { std::cout << "Error: failed to reload target session.\n"; return true; }
        } else {
            auto ok = svc.cmd_load_graph(name, "sessions", "", config.loaded_config_path);
            if (!ok) { std::cout << "Error: failed to load copied session '" << name << "'.\n"; return true; }
        }
        current_graph = name;
        config.loaded_config_path = (ps::fs::absolute(ps::fs::path("sessions")/name/"config.yaml")).string();
        std::cout << "Copied current session to '" << name << "' and switched.\n";
    } else {
        auto names = svc.cmd_list_graphs();
        if (std::find(names.begin(), names.end(), name) == names.end()) { std::cout << "Graph not found: " << name << "\n"; return true; }
        current_graph = name; std::cout << "Switched to '" << name << "'.\n";
    }
    return true;
}

void print_help_switch(const CliConfig& /*config*/) {
    print_help_from_file("help_switch.txt");
}

