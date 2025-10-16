#include "cli/cli_autocompleter.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>

#include "cli/path_complete.hpp"
#include "kernel/interaction.hpp"

namespace ps {

CliAutocompleter::CliAutocompleter(ps::InteractionService& svc) : svc_(svc) {
  // This list should be kept in sync with commands in `process_command`
  commands_ = {
      "bench",       "benchmark", "clear",       "cls",  // 如果您想补全别名，也在这里添加
      "clear-cache", "cc",        "clear-graph", "close", "compute",
      "config",      "exit",      "quit",        "q",     "free",
      "graphs",      "help",      "load",        "node",  "ops",
      "output",      "print",     "read",        "save",  "source",
      "switch",      "traversal"};
  std::sort(commands_.begin(), commands_.end());
}

CompletionResult CliAutocompleter::Complete(const std::string& line,
                                            int cursor_pos) {
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
  if (tokens.empty() ||
      (tokens.size() == 1 && !line.empty() && line.back() != ' ')) {
    // First word is always a command
    CompleteCommand(prefix, result.options);
  } else {
    const std::string& cmd = tokens[0];
    if (cmd == "help") {
      // help <command> — suggest available commands/topics
      CompleteCommand(prefix, result.options);
    } else if ((cmd == "read" || cmd == "source" || cmd == "output" ||
                cmd == "save") &&
               (tokens.size() > 1)) {
      CompletePath(prefix, result.options);
    } else if (cmd == "bench" || cmd == "benchmark") {
      CompletePath(prefix, result.options);
    } else if (cmd == "load") {
      // load <name> <yaml>
      bool completing_first_arg =
          (tokens.size() == 1) || (tokens.size() == 2 && line.back() != ' ');
      if (completing_first_arg) {
        CompleteSessionName(prefix, result.options);
      } else {
        CompleteYamlPath(prefix, result.options);
      }
    } else if (cmd == "node" || cmd == "save") {
      CompleteNodeId(prefix, result.options);
    } else if (cmd == "print") {
      // print <id|all> <mode>
      bool completing_first_arg =
          (tokens.size() == 1) || (tokens.size() == 2 && line.back() != ' ');
      if (completing_first_arg) {
        CompletePrintArgs(prefix, result.options, /*only_mode_args=*/false);
      } else {
        CompletePrintArgs(prefix, result.options, /*only_mode_args=*/true);
      }
    } else if (cmd == "compute") {
      // compute <id|all> [flags]
      bool completing_first_arg =
          (tokens.size() == 1) || (tokens.size() == 2 && line.back() != ' ');
      if (completing_first_arg) {
        CompleteNodeId(prefix, result.options);
      } else {
        CompleteComputeArgs(prefix, result.options);
      }
    } else if (cmd == "traversal") {
      CompleteTraversalArgs(prefix, result.options);
    } else if (cmd == "switch") {
      // switch <name> [c]
      bool completing_first_arg =
          (tokens.size() == 1) || (tokens.size() == 2 && line.back() != ' ');
      if (completing_first_arg) {
        // Complete session directory names like `load`
        CompleteSessionName(prefix, result.options);
      } else {
        // Second arg: offer copy mode flag
        const std::string copy_flag = "c";
        if (copy_flag.rfind(prefix, 0) == 0)
          result.options.push_back(copy_flag);
      }
    } else if (cmd == "close") {
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
  result.new_line =
      line.substr(0, start_of_word) + common_prefix + line.substr(cursor_pos);
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

}  // namespace ps