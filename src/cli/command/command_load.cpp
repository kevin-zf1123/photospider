// FILE: src/cli/command/command_load.cpp
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <vector>

#include "cli/ask_yesno.hpp"
#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

namespace fs = std::filesystem;

bool handle_load(std::istringstream& iss, ps::InteractionService& svc,
                 std::string& current_graph, bool& modified,
                 CliConfig& config) {
  // Merged behavior: support
  //   - load <name> [yaml]  (session-based load, as before)
  //   - load <yaml>        (load into current session if set; else into
  //   [default])
  std::vector<std::string> args;
  {
    std::string a;
    while (iss >> a)
      args.push_back(a);
  }
  auto is_yaml_path = [](const std::string& s) {
    auto ends_with = [&](const char* ext) {
      return s.size() >= strlen(ext) && s.rfind(ext) == s.size() - strlen(ext);
    };
    return ends_with(".yaml") || ends_with(".yml");
  };
  if (args.empty()) {
    std::cout << "Usage: load <name> [yaml]  OR  load <yaml>\n";
    return true;
  }

  if (args.size() == 1 && (is_yaml_path(args[0]) || fs::exists(args[0]))) {
    // Variant: load <yaml>
    const std::string yaml_path = args[0];
    if (!current_graph.empty()) {
      // Overwrite current session's content by reloading YAML
      if (config.session_warning) {
        if (!ask_yesno("This will overwrite current session '" + current_graph +
                           "' contents from '" + yaml_path + "'. Continue?",
                       true)) {
          std::cout << "Aborted." << std::endl;
          return true;
        }
      }
      if (svc.cmd_reload_yaml(current_graph, yaml_path)) {
        modified = false;
        std::cout << "Loaded graph from " << yaml_path << " into session '"
                  << current_graph << "'\n";
      } else {
        std::cout << "Failed to load from '" << yaml_path << "'." << std::endl;
      }
    } else {
      // No current session: load into [default]
      const std::string def = "default";
      fs::path def_dir = fs::path("sessions") / def;
      fs::path def_yaml = def_dir / "content.yaml";
      bool will_overwrite = fs::exists(def_yaml);
      if (config.session_warning && will_overwrite) {
        if (!ask_yesno("Session 'default' already exists and will be "
                       "overwritten. Continue?",
                       true)) {
          std::cout << "Aborted." << std::endl;
          return true;
        }
      }
      auto ok = svc.cmd_load_graph(def, "sessions", yaml_path,
                                   config.loaded_config_path);
      if (!ok) {
        std::cout << "Error: failed to load session 'default' from '"
                  << yaml_path << "'.\n";
        return true;
      }
      if (config.switch_after_load)
        current_graph = *ok;
      config.loaded_config_path =
          (ps::fs::absolute(ps::fs::path("sessions") / def / "config.yaml"))
              .string();
      std::cout << "Loaded graph into session 'default' (yaml: " << yaml_path
                << ").\n";
    }
    return true;
  }

  // Variant: load <name> [yaml]
  const std::string name = args[0];
  if (args.size() == 1) {
    // load <name>  (from sessions/<name>/content.yaml)
    auto session_yaml = ps::fs::path("sessions") / name / "content.yaml";
    if (!ps::fs::exists(session_yaml)) {
      std::cout << "Error: session YAML not found: " << session_yaml << "\n";
      std::cout << "Hint: provide an explicit YAML path: load <name> <yaml>\n";
      return true;
    }
    auto ok =
        svc.cmd_load_graph(name, "sessions", "", config.loaded_config_path);
    if (!ok) {
      std::cout << "Error: failed to load session '" << name << "'.\n";
      return true;
    }
    if (config.switch_after_load)
      current_graph = *ok;
    config.loaded_config_path =
        (ps::fs::absolute(ps::fs::path("sessions") / name / "config.yaml"))
            .string();
    std::cout << "Loaded session '" << name << "'.\n";
    return true;
  }

  if (args.size() >= 2) {
    auto yaml_path = args[1];
    auto ok = svc.cmd_load_graph(name, "sessions", yaml_path,
                                 config.loaded_config_path);
    if (!ok) {
      std::cout << "Error: failed to load session '" << name << "' from '"
                << yaml_path << "'.\n";
      return true;
    }
    if (config.switch_after_load)
      current_graph = *ok;
    config.loaded_config_path =
        (ps::fs::absolute(ps::fs::path("sessions") / name / "config.yaml"))
            .string();
    std::cout << "Loaded session '" << name << "' (yaml: " << yaml_path
              << ").\n";
    return true;
  }

  return true;
}

void print_help_load(const CliConfig& /*config*/) {
  print_help_from_file("help_load.txt");
}
