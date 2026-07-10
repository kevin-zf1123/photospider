// FILE: src/cli/command/command_scheduler.cpp
// M3.4: CLI scheduler 命令实现 - 查看和切换调度器
// M3.5: 添加动态加载调度器插件功能

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

namespace {

/**
 * @brief Parses a CLI scheduler intent token into a Host compute intent.
 *
 * @param token User-provided intent token such as `hp` or `rt`.
 * @param[out] intent Parsed Host compute intent when parsing succeeds.
 * @param[out] display_name Short display label for the parsed intent.
 * @return True when token names a supported intent; false otherwise.
 * @throws Nothing directly.
 * @note The parser accepts uppercase aliases for compatibility with existing
 *       scripts and does not mutate output parameters on failure.
 */
bool parse_intent(const std::string& token, ps::ComputeIntent& intent,
                  std::string& display_name) {
  if (token == "hp" || token == "HP") {
    intent = ps::ComputeIntent::GlobalHighPrecision;
    display_name = "HP";
    return true;
  }
  if (token == "rt" || token == "RT") {
    intent = ps::ComputeIntent::RealTimeUpdate;
    display_name = "RT";
    return true;
  }
  return false;
}

/**
 * @brief Prints scheduler info for one graph intent through the Host API.
 *
 * @param svc Host used for scheduler inspection.
 * @param current_graph Current graph session id.
 * @param intent Compute intent whose scheduler should be displayed.
 * @param display_name Reader-facing intent label.
 * @throws std::bad_alloc or iostream exceptions only if the standard library
 *         stream/container operations throw.
 * @note Missing or unavailable scheduler information is rendered as
 *       `(not configured)` instead of surfacing backend objects.
 */
void print_scheduler_info(ps::Host& svc, const std::string& current_graph,
                          ps::ComputeIntent intent,
                          const std::string& display_name) {
  auto info = svc.scheduler_info(ps::GraphSessionId{current_graph}, intent);
  std::cout << display_name << " Scheduler:\n";
  if (info.status.ok) {
    std::cout << "  Type: " << info.value.scheduler_name << "\n";
    std::cout << "  Stats:\n" << info.value.stats << "\n";
  } else {
    std::cout << "  (not configured)\n";
  }
}

/**
 * @brief Reads available scheduler types from Host for CLI presentation.
 *
 * @param svc Host used for scheduler registry inspection.
 * @return Scheduler type names, or an empty vector when Host reports failure.
 * @throws std::bad_alloc if copying the returned vector allocates.
 * @note The command layer treats failures as an empty registry so usage/error
 *       output remains deterministic.
 */
std::vector<std::string> available_scheduler_types(ps::Host& svc) {
  auto result = svc.scheduler_available_types();
  if (!result.status.ok) {
    return {};
  }
  return result.value;
}

}  // namespace

/** @copydoc handle_scheduler */
bool handle_scheduler(std::istringstream& iss, ps::Host& svc,
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
      auto scan = svc.scheduler_scan({dir});
      std::cout << "Scanned '" << dir << "': loaded "
                << (scan.status.ok ? scan.value : 0) << " scheduler(s).\n";
    } else {
      // 扫描配置中的所有目录
      auto scan = svc.scheduler_scan(config.scheduler_dirs);
      std::cout << "Scanned " << config.scheduler_dirs.size()
                << " directories: loaded " << (scan.status.ok ? scan.value : 0)
                << " scheduler(s).\n";
    }

    // 显示当前已加载的调度器
    std::cout << "\nAvailable schedulers:\n";
    for (const auto& type : available_scheduler_types(svc)) {
      auto desc = svc.scheduler_description(type);
      std::cout << "  " << type;
      if (desc.status.ok && !desc.value.empty()) {
        std::cout << " - " << desc.value;
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

    if (svc.scheduler_load(path).status.ok) {
      std::cout << "Successfully loaded scheduler plugin from '" << path
                << "'.\n";
      std::cout << "Available schedulers:\n";
      for (const auto& type : available_scheduler_types(svc)) {
        std::cout << "  " << type << "\n";
      }
    } else {
      std::cout << "Error: Failed to load scheduler plugin from '" << path
                << "'.\n";
    }
    return true;
  }

  // scheduler plugins - 列出已加载的插件信息
  if (subcmd == "plugins") {
    auto plugins = svc.scheduler_loaded_plugins();

    if (!plugins.status.ok || plugins.value.empty()) {
      std::cout << "No scheduler plugins loaded.\n";
      std::cout << "Use 'scheduler scan' to load plugins from configured "
                   "directories.\n";
    } else {
      std::cout << "Loaded scheduler plugins:\n";
      for (const auto& info : plugins.value) {
        std::cout << "  " << info << "\n";
      }
    }
    return true;
  }

  // scheduler list - 列出所有支持的调度器类型
  if (subcmd == "list") {
    std::cout << "Available scheduler types:\n";
    for (const auto& type : available_scheduler_types(svc)) {
      auto desc = svc.scheduler_description(type);
      std::cout << "  " << type << "\n";
      if (desc.status.ok && !desc.value.empty()) {
        std::cout << "    " << desc.value << "\n";
      }
    }
    return true;
  }

  // scheduler get [intent] - 获取当前调度器信息
  if (subcmd == "get") {
    if (current_graph.empty()) {
      std::cout
          << "Error: No graph loaded. Use 'load' to load a graph first.\n";
      return true;
    }

    std::string intent_str;
    iss >> intent_str;

    // 如果没有指定 intent，显示所有
    if (intent_str.empty() || intent_str == "all") {
      print_scheduler_info(svc, current_graph,
                           ps::ComputeIntent::GlobalHighPrecision,
                           "HP (GlobalHighPrecision)");
      print_scheduler_info(svc, current_graph,
                           ps::ComputeIntent::RealTimeUpdate,
                           "RT (RealTimeUpdate)");
    } else {
      // 指定 intent
      ps::ComputeIntent intent;
      std::string intent_name;
      if (parse_intent(intent_str, intent, intent_name) &&
          intent == ps::ComputeIntent::GlobalHighPrecision) {
        intent_name = "HP (GlobalHighPrecision)";
      } else if (parse_intent(intent_str, intent, intent_name)) {
        intent_name = "RT (RealTimeUpdate)";
      } else {
        std::cout << "Error: Unknown intent '" << intent_str
                  << "'. Use 'hp' or 'rt'.\n";
        return true;
      }

      print_scheduler_info(svc, current_graph, intent, intent_name);
    }
    return true;
  }

  // scheduler set <intent> <type> - 切换调度器
  if (subcmd == "set") {
    if (current_graph.empty()) {
      std::cout
          << "Error: No graph loaded. Use 'load' to load a graph first.\n";
      return true;
    }

    std::string intent_str, type_str;
    iss >> intent_str >> type_str;

    if (intent_str.empty() || type_str.empty()) {
      std::cout << "Usage: scheduler set <intent> <type>\n";
      std::cout << "  intent: hp | rt\n";
      std::cout << "  type: ";
      for (const auto& t : available_scheduler_types(svc)) {
        std::cout << t << " ";
      }
      std::cout << "\n";
      return true;
    }

    // 解析 intent
    ps::ComputeIntent intent;
    std::string intent_name;
    if (!parse_intent(intent_str, intent, intent_name)) {
      std::cout << "Error: Unknown intent '" << intent_str
                << "'. Use 'hp' or 'rt'.\n";
      return true;
    }

    // 检查类型是否支持
    auto available_types = available_scheduler_types(svc);
    bool is_supported =
        std::find(available_types.begin(), available_types.end(), type_str) !=
        available_types.end();

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
    if (svc.replace_scheduler(ps::GraphSessionId{current_graph}, intent,
                              type_str)
            .status.ok) {
      std::cout << "Successfully switched " << intent_name << " scheduler to '"
                << type_str << "'.\n";
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

/** @copydoc print_help_scheduler */
void print_help_scheduler(const CliConfig& /*config*/) {
  print_help_from_file("help_scheduler.txt");
}
