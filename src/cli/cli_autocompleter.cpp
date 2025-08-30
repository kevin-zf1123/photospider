#include "cli/cli_autocompleter.hpp"
#include <sstream>
#include <algorithm>
#include <iostream>
#include "cli/path_complete.hpp"
#include "kernel/interaction.hpp"
#include <filesystem>

namespace ps {

CliAutocompleter::CliAutocompleter(ps::InteractionService& svc)
    : svc_(svc) {
    // This list should be kept in sync with commands in `process_command`
    commands_ = {
        "help", "clear", "config", "ops", "read", "source", "print", "node",
        "traversal", "output", "clear-graph", "cc", "clear-cache", "compute",
        "save", "free", "exit",
        // keep in sync with process_command in cli/graph_cli.cpp
        "graphs", "load", "switch", "close"
    };
    std::sort(commands_.begin(), commands_.end());
}

std::vector<std::string> CliAutocompleter::Tokenize(const std::string& line) const {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

std::string CliAutocompleter::FindLongestCommonPrefix(const std::vector<std::string>& options) const {
    if (options.empty()) return "";
    if (options.size() == 1) return options[0];

    std::string prefix = options[0];
    for (size_t i = 1; i < options.size(); ++i) {
        while (options[i].rfind(prefix, 0) != 0) {
            prefix.pop_back();
            if (prefix.empty()) return "";
        }
    }
    return prefix;
}

void CliAutocompleter::CompleteCommand(const std::string& prefix, std::vector<std::string>& options) const {
    for (const auto& cmd : commands_) {
        if (cmd.rfind(prefix, 0) == 0) {
            options.push_back(cmd);
        }
    }
}

void CliAutocompleter::CompletePath(const std::string& prefix, std::vector<std::string>& options) const {
    auto opts = PathCompleteOptions(prefix);
    options.insert(options.end(), opts.begin(), opts.end());
}

void CliAutocompleter::CompleteYamlPath(const std::string& prefix, std::vector<std::string>& options) const {
    // Use generic path options, then filter to YAML files, but keep directories to allow traversal.
    auto opts = PathCompleteOptions(prefix);
    for (const auto& o : opts) {
        if (!o.empty() && o.back() == '/') {
            options.push_back(o); // allow continuing into subdirectories
        } else {
            // Match .yaml or .yml (case-sensitive as typical on *nix)
            if (o.size() >= 5 && o.rfind(".yaml") == o.size() - 5) options.push_back(o);
            else if (o.size() >= 4 && o.rfind(".yml") == o.size() - 4) options.push_back(o);
        }
    }
}

void CliAutocompleter::CompleteNodeId(const std::string& prefix, std::vector<std::string>& options) const {
    if (std::string("all").rfind(prefix, 0) == 0) {
        options.push_back("all");
    }
    if (!current_graph_.empty()) {
        auto ids = svc_.cmd_list_node_ids(current_graph_);
        if (ids) {
            for (int id : *ids) {
                std::string id_str = std::to_string(id);
                if (id_str.rfind(prefix, 0) == 0) options.push_back(id_str);
            }
        }
    }
}

void CliAutocompleter::CompletePrintArgs(const std::string& prefix, std::vector<std::string>& options, bool only_mode_args) const {
    // When completing the first argument, only suggest id/all.
    // From the second argument onward, only suggest print mode args.
    if (!only_mode_args) {
        CompleteNodeId(prefix, options);
        return;
    }
    const std::vector<std::string> mode_args = {"full", "simplified", "f", "s"};
    for(const auto& arg : mode_args) {
        if(arg.rfind(prefix, 0) == 0) options.push_back(arg);
    }
}
void CliAutocompleter::CompleteComputeArgs(const std::string& prefix, std::vector<std::string>& options) const {
    // Only compute flags here. Node id/all is handled as the first arg in Complete().
    const std::vector<std::string> mode_args = {"force", "force-deep", "parallel", "t", "-t", "timer", "tl", "-tl", "m", "-m", "mute"};
    for(const auto& arg : mode_args) {
        if(arg.rfind(prefix, 0) == 0) options.push_back(arg);
    }
}

void CliAutocompleter::CompleteTraversalArgs(const std::string& prefix, std::vector<std::string>& options) const {
    // Support print-style tree mode args for traversal as well
    const std::vector<std::string> tree_args = {"full", "simplified", "no_tree", "f", "s", "n"};
    for(const auto& arg : tree_args) {
        if(arg.rfind(prefix, 0) == 0) options.push_back(arg);
    }
    // And common traversal cache/check flags (optional but helpful)
    const std::vector<std::string> trav_flags = {"m", "md", "d", "c", "cr"};
    for(const auto& arg : trav_flags) {
        if(arg.rfind(prefix, 0) == 0) options.push_back(arg);
    }
}


CompletionResult CliAutocompleter::Complete(const std::string& line, int cursor_pos) {
    CompletionResult result;
    result.new_line = line;
    result.new_cursor_pos = cursor_pos;
    
    // Find the word to complete based on cursor position
    auto start_of_word = line.find_last_of(" \t", cursor_pos - 1);
    start_of_word = (start_of_word == std::string::npos) ? 0 : start_of_word + 1;
    
    std::string prefix = line.substr(start_of_word, cursor_pos - start_of_word);

    // Tokenize the line up to the cursor to get context
    std::vector<std::string> tokens = Tokenize(line.substr(0, cursor_pos));

    // Determine completion type
    if (tokens.empty() || (tokens.size() == 1 && !line.empty() && line.back() != ' ')) {
        // First word is always a command
        CompleteCommand(prefix, result.options);
    } else {
        const std::string& cmd = tokens[0];
        if ((cmd == "read" || cmd == "source" || cmd == "output" || cmd == "save") && (tokens.size() > 1)) {
            CompletePath(prefix, result.options);
        } else if (cmd == "load") {
            // load <name> <yaml>
            bool completing_first_arg = (tokens.size() == 1) || (tokens.size() == 2 && line.back() != ' ');
            if (completing_first_arg) {
                CompleteSessionName(prefix, result.options);
            } else {
                CompleteYamlPath(prefix, result.options);
            }
        } else if (cmd == "node" || cmd == "save") {
            CompleteNodeId(prefix, result.options);
        } else if (cmd == "print") {
            // print <id|all> <mode>
            bool completing_first_arg = (tokens.size() == 1) || (tokens.size() == 2 && line.back() != ' ');
            if (completing_first_arg) {
                CompletePrintArgs(prefix, result.options, /*only_mode_args=*/false);
            } else {
                CompletePrintArgs(prefix, result.options, /*only_mode_args=*/true);
            }
        } else if (cmd == "compute") {
            // compute <id|all> [flags]
            bool completing_first_arg = (tokens.size() == 1) || (tokens.size() == 2 && line.back() != ' ');
            if (completing_first_arg) {
                CompleteNodeId(prefix, result.options);
            } else {
                CompleteComputeArgs(prefix, result.options);
            }
        } else if (cmd == "traversal") {
            CompleteTraversalArgs(prefix, result.options);
        } else if (cmd == "switch" || cmd == "close") {
            CompleteGraphName(prefix, result.options);
        } else if (cmd == "ops") {
            CompleteOpsMode(prefix, result.options);
        }
        // ... add more context-aware completions for other commands ...
    }

    if (result.options.empty()) {
        return result;
    }

    std::string common_prefix = FindLongestCommonPrefix(result.options);
    if (common_prefix.empty()) {
        return result;
    }

    // Replace the partial word with the common prefix
    result.new_line = line.substr(0, start_of_word) + common_prefix + line.substr(cursor_pos);
    result.new_cursor_pos = start_of_word + common_prefix.length();

    // If there's only one option and it's a perfect match, add a space
    if (result.options.size() == 1 && common_prefix == result.options[0]) {
        // unless it's a directory
        if (result.options[0].back() != '/') {
            result.new_line += " ";
            result.new_cursor_pos++;
        }
    }

    return result;
}

void CliAutocompleter::CompleteSessionName(const std::string& prefix, std::vector<std::string>& options) const {
    namespace fs = std::filesystem;
    try {
        fs::path sessions_dir("sessions");
        if (!fs::exists(sessions_dir) || !fs::is_directory(sessions_dir)) return;
        for (const auto& entry : fs::directory_iterator(sessions_dir)) {
            if (!fs::is_directory(entry.status())) continue;
            auto name = entry.path().filename().string();
            if (name.rfind(prefix, 0) == 0) options.push_back(name);
        }
    } catch (...) {
        // ignore filesystem errors
    }
}

void CliAutocompleter::CompleteGraphName(const std::string& prefix, std::vector<std::string>& options) const {
    auto names = svc_.cmd_list_graphs();
    for (const auto& n : names) {
        if (n.rfind(prefix, 0) == 0) options.push_back(n);
    }
}

void CliAutocompleter::CompleteOpsMode(const std::string& prefix, std::vector<std::string>& options) const {
    const std::vector<std::string> args = {"all", "a", "builtin", "b", "plugins", "p", "custom", "c"};
    for (const auto& a : args) {
        if (a.rfind(prefix, 0) == 0) options.push_back(a);
    }
}

} // namespace ps
