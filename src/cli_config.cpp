// CLI configuration YAML read/write implementation
#include "cli_config.hpp"

#include <fstream>
#include <iostream>
#include <yaml-cpp/yaml.h>
#include "ps_types.hpp" // for ps::fs alias

using namespace ps; // for fs

bool write_config_to_file(const CliConfig& config, const std::string& path) {
    YAML::Node root;
    root["_comment1"] = "Photospider CLI configuration.";
    root["cache_root_dir"] = config.cache_root_dir;
    root["cache_precision"] = config.cache_precision;
    root["plugin_dirs"] = config.plugin_dirs;
    root["history_size"] = config.history_size;
    root["default_print_mode"] = config.default_print_mode;
    root["default_traversal_arg"] = config.default_traversal_arg;
    root["default_cache_clear_arg"] = config.default_cache_clear_arg;
    root["default_exit_save_path"] = config.default_exit_save_path;
    root["exit_prompt_sync"] = config.exit_prompt_sync;
    root["config_save_behavior"] = config.config_save_behavior;
    root["editor_save_behavior"] = config.editor_save_behavior;
    root["default_timer_log_path"] = config.default_timer_log_path;
    root["default_ops_list_mode"] = config.default_ops_list_mode;
    root["ops_plugin_path_mode"] = config.ops_plugin_path_mode;
    root["default_compute_args"] = config.default_compute_args;

    try {
        std::ofstream fout(path);
        fout << root;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void load_or_create_config(const std::string& config_path, CliConfig& config) {
    if (fs::exists(config_path)) {
        config.loaded_config_path = fs::absolute(config_path).string();
        try {
            YAML::Node root = YAML::LoadFile(config_path);
            if (root["cache_root_dir"]) config.cache_root_dir = root["cache_root_dir"].as<std::string>();
            if (root["cache_precision"]) config.cache_precision = root["cache_precision"].as<std::string>();
            if (root["history_size"]) config.history_size = root["history_size"].as<int>();

            if (root["plugin_dirs"] && root["plugin_dirs"].IsSequence()) {
                config.plugin_dirs = root["plugin_dirs"].as<std::vector<std::string>>();
            } else if (root["plugin_dir"] && root["plugin_dir"].IsScalar()) {
                config.plugin_dirs.clear();
                config.plugin_dirs.push_back(root["plugin_dir"].as<std::string>());
            }

            if (root["default_print_mode"]) config.default_print_mode = root["default_print_mode"].as<std::string>();
            if (root["default_traversal_arg"]) config.default_traversal_arg = root["default_traversal_arg"].as<std::string>();
            if (root["default_cache_clear_arg"]) config.default_cache_clear_arg = root["default_cache_clear_arg"].as<std::string>();
            if (root["default_exit_save_path"]) config.default_exit_save_path = root["default_exit_save_path"].as<std::string>();
            if (root["exit_prompt_sync"]) config.exit_prompt_sync = root["exit_prompt_sync"].as<bool>();
            if (root["config_save_behavior"]) config.config_save_behavior = root["config_save_behavior"].as<std::string>();
            if (root["editor_save_behavior"]) config.editor_save_behavior = root["editor_save_behavior"].as<std::string>();
            if (root["default_timer_log_path"]) config.default_timer_log_path = root["default_timer_log_path"].as<std::string>();
            if (root["default_ops_list_mode"]) config.default_ops_list_mode = root["default_ops_list_mode"].as<std::string>();
            if (root["ops_plugin_path_mode"]) config.ops_plugin_path_mode = root["ops_plugin_path_mode"].as<std::string>();
            if (root["default_compute_args"]) config.default_compute_args = root["default_compute_args"].as<std::string>();
            std::cout << "Loaded configuration from '" << config_path << "'." << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Warning: Could not parse config file '" << config_path
                      << "'. Using default settings. Error: " << e.what() << std::endl;
        }
    } else if (config_path == "config.yaml") {
        std::cout << "Configuration file 'config.yaml' not found. Creating a default one." << std::endl;
        config.plugin_dirs = {"build/plugins"};
        config.cache_precision = "int8";
        config.history_size = 1000;
        config.editor_save_behavior = "ask";
        config.default_print_mode = "detailed";
        config.default_traversal_arg = "n";
        config.config_save_behavior = "current";
        config.default_timer_log_path = "out/timer.yaml";
        config.default_ops_list_mode = "all";
        config.ops_plugin_path_mode = "name_only";
        config.default_compute_args = "";
        if (write_config_to_file(config, "config.yaml")) {
            config.loaded_config_path = fs::absolute("config.yaml").string();
        }
    }
}
