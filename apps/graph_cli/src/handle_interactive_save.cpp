// FILE: apps/graph_cli/src/handle_interactive_save.cpp
#include "graph_cli/handle_interactive_save.hpp"

#include <iostream>
#include <string>

#include "graph_cli/ask.hpp"
#include "graph_cli/ask_yesno.hpp"
#include "graph_cli/cli_config.hpp"

/** @copydoc handle_interactive_save(CliConfig&) */
void handle_interactive_save(CliConfig& config) {
  if (config.editor_save_behavior == "ask") {
    if (ask_yesno("Save configuration changes?", true)) {
      if (config.loaded_config_path.empty()) {
        std::string path =
            ask("Enter path to save new config file", "config.yaml");
        if (!path.empty())
          write_config_to_file(config, path);
      } else {
        write_config_to_file(config, config.loaded_config_path);
      }
    }
  } else if (config.editor_save_behavior == "auto_save_on_apply") {
    if (config.loaded_config_path.empty()) {
      std::cout << "Warning: auto_save is on, but no config file was loaded. "
                   "Cannot save."
                << std::endl;
    } else {
      write_config_to_file(config, config.loaded_config_path);
    }
  }
}
