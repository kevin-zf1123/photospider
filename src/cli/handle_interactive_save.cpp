// FILE: src/cli/handle_interactive_save.cpp
#include <iostream>
#include "cli/ask.hpp"
#include "cli/ask_yesno.hpp"
#include "cli_config.hpp"
#include "cli/handle_interactive_save.hpp"

void handle_interactive_save(CliConfig& config) {
    if (config.editor_save_behavior == "ask") {
        if (ask_yesno("Save configuration changes?", true)) {
            if (config.loaded_config_path.empty()) {
                std::string path = ask("Enter path to save new config file", "config.yaml");
                if (!path.empty()) write_config_to_file(config, path);
            } else {
                write_config_to_file(config, config.loaded_config_path);
            }
        }
    } else if (config.editor_save_behavior == "auto_save_on_apply") {
        if (config.loaded_config_path.empty()) {
            std::cout << "Warning: auto_save is on, but no config file was loaded. Cannot save." << std::endl;
        } else {
            write_config_to_file(config, config.loaded_config_path);
        }
    }
}

