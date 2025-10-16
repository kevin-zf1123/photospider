// FILE: src/cli/command/command_traversal.cpp
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

bool handle_traversal(std::istringstream& iss, ps::InteractionService& svc,
                      std::string& current_graph, bool& /*modified*/,
                      CliConfig& config) {
  if (current_graph.empty()) {
    std::cout << "No current graph. Use load/switch.\n";
    return true;
  }
  std::string arg;
  std::string print_tree_mode = "none";
  bool show_mem = false, show_disk = false, do_check = false,
       do_check_remove = false;

  // 1. 参数解析
  std::vector<std::string> args;
  while (iss >> arg)
    args.push_back(arg);
  const auto parse_tokens = [&](const std::vector<std::string>& toks) {
    for (const auto& a : toks) {
      if (a == "f" || a == "full")
        print_tree_mode = "full";
      else if (a == "s" || a == "simplified")
        print_tree_mode = "simplified";
      else if (a == "n" || a == "no_tree")
        print_tree_mode = "none";
      else if (a == "md") {
        show_mem = true;
        show_disk = true;
      } else if (a == "m")
        show_mem = true;
      else if (a == "d")
        show_disk = true;
      else if (a == "cr")
        do_check_remove = true;
      else if (a == "c")
        do_check = true;
    }
  };
  if (args.empty()) {
    std::istringstream default_iss(config.default_traversal_arg);
    std::vector<std::string> def_args;
    while (default_iss >> arg)
      def_args.push_back(arg);
    parse_tokens(def_args);
  } else {
    parse_tokens(args);
  }

  // 2. 执行缓存操作
  if (do_check_remove) {
    std::cout << "Synchronizing disk cache with memory state..." << std::endl;
    svc.cmd_synchronize_disk_cache(current_graph, config.cache_precision);
    std::cout << "Done." << std::endl;
  } else if (do_check) {
    std::cout << "Checking and saving caches for all nodes..." << std::endl;
    svc.cmd_cache_all_nodes(current_graph, config.cache_precision);
    std::cout << "Done." << std::endl;
  }

  // 3. 打印可选的依赖树
  if (print_tree_mode == "full") {
    auto s = svc.cmd_dump_tree(current_graph, std::nullopt, true);
    if (s)
      std::cout << *s;
  } else if (print_tree_mode == "simplified") {
    auto s = svc.cmd_dump_tree(current_graph, std::nullopt, false);
    if (s)
      std::cout << *s;
  }

  // 4. 输出后序遍历详细信息
  auto details_map_opt = svc.cmd_traversal_details(current_graph);
  if (!details_map_opt || details_map_opt->empty()) {
    std::cout << "(No ending nodes found or graph is cyclic)\n";
    return true;
  }

  bool first_tree = true;
  for (const auto& [end_node_id, node_info_vector] : *details_map_opt) {
    if (!first_tree) {
      std::cout << "\n";
    }
    std::cout << "Post-order (eval order) for end node " << end_node_id << ":"
              << std::endl;

    int counter = 1;
    for (const auto& node_info : node_info_vector) {
      // 打印行号、ID 和名称
      std::cout << counter++ << ". " << node_info.id << " (" << node_info.name
                << ")";

      // 构建并打印缓存状态
      std::vector<std::string> statuses;
      if (show_mem && node_info.has_memory_cache) {
        statuses.push_back("in memory");
      }
      if (show_disk && node_info.has_disk_cache) {
        statuses.push_back("on disk");
      }

      if (!statuses.empty()) {
        std::cout << " (";
        for (size_t i = 0; i < statuses.size(); ++i) {
          std::cout << statuses[i] << (i < statuses.size() - 1 ? ", " : "");
        }
        std::cout << ")";
      }
      std::cout << std::endl;
    }
    first_tree = false;
  }
  return true;
}

void print_help_traversal(const CliConfig& /*config*/) {
  print_help_from_file("help_traversal.txt");
}
