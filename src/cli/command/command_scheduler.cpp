// FILE: src/cli/command/command_scheduler.cpp
// M3.4: CLI scheduler 命令实现 - 查看和切换调度器
// M3.5: 添加动态加载调度器插件功能

#include <algorithm>
#include <iostream>
#include <sstream>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

bool handle_scheduler(std::istringstream& iss, ps::InteractionService& svc,
                      std::string& current_graph, bool& /*modified*/,
                      CliConfig& config) {
  std::string subcmd;
  iss >> subcmd;
  
  if (subcmd.empty() || subcmd == "help") {
    print_help_scheduler(config);
    return true;
  }
  
  // scheduler scan [dir] - 扫描并加载调度器插件
  if (subcmd == "scan") {
    std::string dir;
    iss >> dir;
    
    if (!dir.empty()) {
      // 扫描指定目录
      size_t count = svc.cmd_scheduler_scan({dir});
      std::cout << "Scanned '" << dir << "': loaded " << count << " scheduler(s).\n";
    } else {
      // 扫描配置中的所有目录
      size_t total = svc.cmd_scheduler_scan(config.scheduler_dirs);
      std::cout << "Scanned " << config.scheduler_dirs.size() 
                << " directories: loaded " << total << " scheduler(s).\n";
    }
    
    // 显示当前已加载的调度器
    std::cout << "\nAvailable schedulers:\n";
    for (const auto& type : svc.cmd_scheduler_available_types()) {
      auto desc = svc.cmd_scheduler_description(type);
      std::cout << "  " << type;
      if (!desc.empty()) {
        std::cout << " - " << desc;
      }
      std::cout << "\n";
    }
    return true;
  }
  
  // scheduler load <path> - 从指定路径加载单个调度器插件
  if (subcmd == "load") {
    std::string path;
    iss >> path;
    
    if (path.empty()) {
      std::cout << "Usage: scheduler load <path>\n";
      std::cout << "  Load a scheduler plugin from the specified dylib path.\n";
      return true;
    }
    
    if (svc.cmd_scheduler_load(path)) {
      std::cout << "Successfully loaded scheduler plugin from '" << path << "'.\n";
      std::cout << "Available schedulers:\n";
      for (const auto& type : svc.cmd_scheduler_available_types()) {
        std::cout << "  " << type << "\n";
      }
    } else {
      std::cout << "Error: Failed to load scheduler plugin from '" << path << "'.\n";
    }
    return true;
  }
  
  // scheduler plugins - 列出已加载的插件信息
  if (subcmd == "plugins") {
    auto plugins = svc.cmd_scheduler_loaded_plugins();
    
    if (plugins.empty()) {
      std::cout << "No scheduler plugins loaded.\n";
      std::cout << "Use 'scheduler scan' to load plugins from configured directories.\n";
    } else {
      std::cout << "Loaded scheduler plugins:\n";
      for (const auto& info : plugins) {
        std::cout << "  " << info << "\n";
      }
    }
    return true;
  }
  
  // scheduler list - 列出所有支持的调度器类型
  if (subcmd == "list") {
    std::cout << "Available scheduler types:\n";
    for (const auto& type : svc.cmd_scheduler_available_types()) {
      auto desc = svc.cmd_scheduler_description(type);
      std::cout << "  " << type << "\n";
      if (!desc.empty()) {
        std::cout << "    " << desc << "\n";
      }
    }
    return true;
  }
  
  // scheduler get [intent] - 获取当前调度器信息
  if (subcmd == "get") {
    if (current_graph.empty()) {
      std::cout << "Error: No graph loaded. Use 'load' to load a graph first.\n";
      return true;
    }
    
    std::string intent_str;
    iss >> intent_str;
    
    auto& kernel = svc.kernel();
    
    // 如果没有指定 intent，显示所有
    if (intent_str.empty() || intent_str == "all") {
      // HP 调度器
      auto hp_info = kernel.get_scheduler_info(current_graph, 
                                                ps::ComputeIntent::GlobalHighPrecision);
      std::cout << "HP (GlobalHighPrecision) Scheduler:\n";
      if (hp_info.has_value()) {
        std::cout << "  Type: " << hp_info->first << "\n";
        std::cout << "  Stats:\n" << hp_info->second << "\n";
      } else {
        std::cout << "  (not configured)\n";
      }
      
      // RT 调度器
      auto rt_info = kernel.get_scheduler_info(current_graph,
                                                ps::ComputeIntent::RealTimeUpdate);
      std::cout << "RT (RealTimeUpdate) Scheduler:\n";
      if (rt_info.has_value()) {
        std::cout << "  Type: " << rt_info->first << "\n";
        std::cout << "  Stats:\n" << rt_info->second << "\n";
      } else {
        std::cout << "  (not configured)\n";
      }
    } else {
      // 指定 intent
      ps::ComputeIntent intent;
      std::string intent_name;
      if (intent_str == "hp" || intent_str == "HP") {
        intent = ps::ComputeIntent::GlobalHighPrecision;
        intent_name = "HP (GlobalHighPrecision)";
      } else if (intent_str == "rt" || intent_str == "RT") {
        intent = ps::ComputeIntent::RealTimeUpdate;
        intent_name = "RT (RealTimeUpdate)";
      } else {
        std::cout << "Error: Unknown intent '" << intent_str 
                  << "'. Use 'hp' or 'rt'.\n";
        return true;
      }
      
      auto info = kernel.get_scheduler_info(current_graph, intent);
      std::cout << intent_name << " Scheduler:\n";
      if (info.has_value()) {
        std::cout << "  Type: " << info->first << "\n";
        std::cout << "  Stats:\n" << info->second << "\n";
      } else {
        std::cout << "  (not configured)\n";
      }
    }
    return true;
  }
  
  // scheduler set <intent> <type> - 切换调度器
  if (subcmd == "set") {
    if (current_graph.empty()) {
      std::cout << "Error: No graph loaded. Use 'load' to load a graph first.\n";
      return true;
    }
    
    std::string intent_str, type_str;
    iss >> intent_str >> type_str;
    
    if (intent_str.empty() || type_str.empty()) {
      std::cout << "Usage: scheduler set <intent> <type>\n";
      std::cout << "  intent: hp | rt\n";
      std::cout << "  type: ";
      for (const auto& t : svc.cmd_scheduler_available_types()) {
        std::cout << t << " ";
      }
      std::cout << "\n";
      return true;
    }
    
    // 解析 intent
    ps::ComputeIntent intent;
    std::string intent_name;
    if (intent_str == "hp" || intent_str == "HP") {
      intent = ps::ComputeIntent::GlobalHighPrecision;
      intent_name = "HP";
    } else if (intent_str == "rt" || intent_str == "RT") {
      intent = ps::ComputeIntent::RealTimeUpdate;
      intent_name = "RT";
    } else {
      std::cout << "Error: Unknown intent '" << intent_str 
                << "'. Use 'hp' or 'rt'.\n";
      return true;
    }
    
    // 检查类型是否支持
    auto available_types = svc.cmd_scheduler_available_types();
    bool is_supported = std::find(available_types.begin(), available_types.end(), 
                                   type_str) != available_types.end();
    
    if (!is_supported) {
      std::cout << "Error: Unknown scheduler type '" << type_str << "'.\n";
      std::cout << "Available types: ";
      for (const auto& t : available_types) {
        std::cout << t << " ";
      }
      std::cout << "\n";
      return true;
    }
    
    // 执行切换
    auto& kernel = svc.kernel();
    if (kernel.replace_scheduler(current_graph, intent, type_str)) {
      std::cout << "Successfully switched " << intent_name 
                << " scheduler to '" << type_str << "'.\n";
    } else {
      std::cout << "Error: Failed to switch scheduler.\n";
    }
    return true;
  }
  
  // 未知子命令
  std::cout << "Unknown scheduler subcommand: " << subcmd << "\n";
  std::cout << "Use 'scheduler help' for usage information.\n";
  return true;
}

void print_help_scheduler(const CliConfig& /*config*/) {
  print_help_from_file("help_scheduler.txt");
}
