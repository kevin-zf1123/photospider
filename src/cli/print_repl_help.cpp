// FILE: src/cli/print_repl_help.cpp
#include <iostream>
#include "cli/print_repl_help.hpp"

void print_repl_help(const CliConfig& config) {
    std::cout << "Available REPL (interactive shell) commands:\n\n"
              << "  help\n"
              << "    Show this help message.\n\n"

              << "  clear\n"
              << "    Clear the terminal screen.\n\n"

              << "  config\n"
              << "    Open the interactive configuration editor.\n\n"

              << "  graphs\n"
              << "    List loaded graphs and current selection.\n\n"

              << "  load <name> [yaml]\n"
              << "    Load a graph (from sessions/<name>/content.yaml if no yaml provided).\n"
              << "  load <yaml>\n"
              << "    Load YAML into the current session; if none, into [default].\n\n"

              << "  switch <name> [c]\n"
              << "    Switch current graph. With 'c', copy current session to <name> and switch.\n\n"

              << "  close <name>\n"
              << "    Close a loaded graph.\n\n"

              << "  print [all|<id>] [full|simplified]\n"
              << "    Print dependency tree.\n\n"

              << "  node [<id>]\n"
              << "    Open the FTXUI node editor (optionally for a specific id).\n\n"

              << "  ops [all|builtin|plugins]\n"
              << "    List registered operations.\n\n"

              << "  traversal [f|s|n] [m|d|md] [c|cr]\n"
              << "    Show eval order; optionally cache-check or sync.\n\n"

              << "  read <file>\n"
              << "    Load YAML into current graph (alias: 'load <yaml>').\n\n"

              << "  source <file>\n"
              << "    Execute commands from a script file.\n\n"

              << "  output <file>\n"
              << "    Save current graph to YAML.\n\n"

              << "  clear-graph\n"
              << "    Clear current graph.\n\n"

              << "  clear-cache [m|d|md]\n"
              << "    Clear memory/disk/both caches. Default: '" << config.default_cache_clear_arg << "'.\n\n"

              << "  free\n"
              << "    Free memory used by non-essential intermediate nodes.\n\n"

              << "  compute <id|all> [force] [force-deep] [parallel] [t] [tl [path]] [m] [nosave]\n"
              << "    Compute node(s) with optional flags:\n"
              << "      force:     Clear in-memory caches before compute.\n"
              << "      force-deep: Clear disk+memory caches before compute.\n"
              << "      parallel:  Use multiple threads to compute.\n"
              << "      t:         Print a simple timer summary to the console.\n"
              << "      tl [path]: Log detailed timings to a YAML file.\n"
              << "      m | -m:    Mute node result output (timers still print when enabled).\n"
              << "      nosave:    Skip saving caches for this compute.\n"
              << "    Defaults: flags='" << config.default_compute_args << "', log_path='" << config.default_timer_log_path << "'\n\n"

              << "  save <id> <file>\n"
              << "    Compute a node and save its image output to a file.\n\n"

              << "  free\n"
              << "    Free memory used by non-essential intermediate nodes.\n\n"

              << "  exit\n"
              << "    Quit the shell.\n"
              << "    Sync prompt default (exit_prompt_sync): "
              << (config.exit_prompt_sync ? "true" : "false") << "\n";
}
