// FILE: include/cli/command/commands.hpp
#pragma once

#include <string>
#include <sstream>
#include "cli_config.hpp"
#include "kernel/interaction.hpp"

// Each command exposes two functions:
//  - handle_<command>: executes the command; returns whether to continue the REPL
//  - print_help_<command>: prints detailed help for the command

// help
bool handle_help(std::istringstream& iss,
                 ps::InteractionService& svc,
                 std::string& current_graph,
                 bool& modified,
                 CliConfig& config);
void print_help_help(const CliConfig& config);

// clear / cls
bool handle_clear(std::istringstream& iss,
                  ps::InteractionService& svc,
                  std::string& current_graph,
                  bool& modified,
                  CliConfig& config);
void print_help_clear(const CliConfig& config);

// graphs
bool handle_graphs(std::istringstream& iss,
                   ps::InteractionService& svc,
                   std::string& current_graph,
                   bool& modified,
                   CliConfig& config);
void print_help_graphs(const CliConfig& config);

// load
bool handle_load(std::istringstream& iss,
                 ps::InteractionService& svc,
                 std::string& current_graph,
                 bool& modified,
                 CliConfig& config);
void print_help_load(const CliConfig& config);

// switch
bool handle_switch(std::istringstream& iss,
                   ps::InteractionService& svc,
                   std::string& current_graph,
                   bool& modified,
                   CliConfig& config);
void print_help_switch(const CliConfig& config);

// close
bool handle_close(std::istringstream& iss,
                  ps::InteractionService& svc,
                  std::string& current_graph,
                  bool& modified,
                  CliConfig& config);
void print_help_close(const CliConfig& config);

// print
bool handle_print(std::istringstream& iss,
                  ps::InteractionService& svc,
                  std::string& current_graph,
                  bool& modified,
                  CliConfig& config);
void print_help_print(const CliConfig& config);

// node
bool handle_node(std::istringstream& iss,
                 ps::InteractionService& svc,
                 std::string& current_graph,
                 bool& modified,
                 CliConfig& config);
void print_help_node(const CliConfig& config);

// ops
bool handle_ops(std::istringstream& iss,
                ps::InteractionService& svc,
                std::string& current_graph,
                bool& modified,
                CliConfig& config);
void print_help_ops(const CliConfig& config);

// traversal
bool handle_traversal(std::istringstream& iss,
                      ps::InteractionService& svc,
                      std::string& current_graph,
                      bool& modified,
                      CliConfig& config);
void print_help_traversal(const CliConfig& config);

// config
bool handle_config(std::istringstream& iss,
                   ps::InteractionService& svc,
                   std::string& current_graph,
                   bool& modified,
                   CliConfig& config);
void print_help_config(const CliConfig& config);

// read
bool handle_read(std::istringstream& iss,
                 ps::InteractionService& svc,
                 std::string& current_graph,
                 bool& modified,
                 CliConfig& config);
void print_help_read(const CliConfig& config);

// source
bool handle_source(std::istringstream& iss,
                   ps::InteractionService& svc,
                   std::string& current_graph,
                   bool& modified,
                   CliConfig& config);
void print_help_source(const CliConfig& config);

// output
bool handle_output(std::istringstream& iss,
                   ps::InteractionService& svc,
                   std::string& current_graph,
                   bool& modified,
                   CliConfig& config);
void print_help_output(const CliConfig& config);

// clear-graph
bool handle_clear_graph(std::istringstream& iss,
                        ps::InteractionService& svc,
                        std::string& current_graph,
                        bool& modified,
                        CliConfig& config);
void print_help_clear_graph(const CliConfig& config);

// clear-cache / cc
bool handle_clear_cache(std::istringstream& iss,
                        ps::InteractionService& svc,
                        std::string& current_graph,
                        bool& modified,
                        CliConfig& config);
void print_help_clear_cache(const CliConfig& config);

// free
bool handle_free(std::istringstream& iss,
                 ps::InteractionService& svc,
                 std::string& current_graph,
                 bool& modified,
                 CliConfig& config);
void print_help_free(const CliConfig& config);

// compute
bool handle_compute(std::istringstream& iss,
                    ps::InteractionService& svc,
                    std::string& current_graph,
                    bool& modified,
                    CliConfig& config);
void print_help_compute(const CliConfig& config);

// save
bool handle_save(std::istringstream& iss,
                 ps::InteractionService& svc,
                 std::string& current_graph,
                 bool& modified,
                 CliConfig& config);
void print_help_save(const CliConfig& config);

// exit / quit / q
bool handle_exit(std::istringstream& iss,
                 ps::InteractionService& svc,
                 std::string& current_graph,
                 bool& modified,
                 CliConfig& config);
void print_help_exit(const CliConfig& config);

