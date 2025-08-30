// FILE: src/cli/command/command_ops.cpp
#include <iostream>
#include <sstream>
#include <map>
#include <vector>
#include <algorithm>
#include <filesystem>
#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

namespace fs = std::filesystem;

bool handle_ops(std::istringstream& iss,
                ps::InteractionService& svc,
                std::string& /*current_graph*/,
                bool& /*modified*/,
                CliConfig& config) {
    std::string mode_arg; iss >> mode_arg;
    if (mode_arg.empty()) { mode_arg = config.default_ops_list_mode; }

    std::string display_mode, display_title;

    if (mode_arg == "all" || mode_arg == "a") { display_mode = "all"; display_title = "all"; }
    else if (mode_arg == "builtin" || mode_arg == "b") { display_mode = "builtin"; display_title = "built-in"; }
    else if (mode_arg == "plugins" || mode_arg == "custom" || mode_arg == "p" || mode_arg == "c") { display_mode = "plugins"; display_title = "plugins"; }
    else { std::cout << "Error: Invalid mode for 'ops'. Use: all (a), builtin (b), or plugins (p/c)." << std::endl; return true; }

    std::map<std::string, std::vector<std::pair<std::string, std::string>>> grouped_ops;
    int op_count = 0;

    auto op_sources = svc.cmd_ops_sources();
    for (const auto& pair : op_sources) {
        const std::string& key = pair.first;
        const std::string& source = pair.second;
        bool is_builtin = (source == "built-in");

        if ((display_mode == "builtin" && !is_builtin) || (display_mode == "plugins" && is_builtin)) continue;

        size_t colon_pos = key.find(':');
        if (colon_pos != std::string::npos) {
            std::string type = key.substr(0, colon_pos);
            std::string subtype = key.substr(colon_pos + 1);
            grouped_ops[type].push_back({subtype, source});
            op_count++;
        }
    }

    if (op_count == 0) {
        if (display_mode == "plugins") std::cout << "No plugin operations are registered." << std::endl;
        else std::cout << "No operations are registered." << std::endl;
        return true;
    }

    std::cout << "Available Operations (" << display_title << "):" << std::endl;
    for (auto& pair : grouped_ops) {
        std::sort(pair.second.begin(), pair.second.end());
        std::cout << "\n  Type: " << pair.first << std::endl;
        for (const auto& op_info : pair.second) {
            std::cout << "    - " << op_info.first;
            if (op_info.second != "built-in") {
                std::string plugin_path_str = op_info.second;
                std::string display_path;
                if (config.ops_plugin_path_mode == "absolute_path") { display_path = plugin_path_str; }
                else if (config.ops_plugin_path_mode == "relative_path") { display_path = fs::relative(plugin_path_str).string(); }
                else { display_path = fs::path(plugin_path_str).filename().string(); }
                std::cout << "  [plugin: " << display_path << "]";
            }
            std::cout << std::endl;
        }
    }
    return true;
}

void print_help_ops(const CliConfig& /*config*/) {
    print_help_from_file("help_ops.txt");
}

