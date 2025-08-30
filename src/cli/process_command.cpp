// FILE: src/cli/process_command.cpp
#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#include <algorithm>
#include <optional>
#include <filesystem>

#include "cli/print_repl_help.hpp"
#include "cli/process_command.hpp"
#include "cli/handle_interactive_save.hpp"
#include "cli/save_fp32_image.hpp"
#include "cli/ask_yesno.hpp"
#include "cli_config.hpp"
#include "plugin_loader.hpp"
#include "cli/node_editor_full.hpp"
#include "cli/config_editor.hpp"

namespace fs = std::filesystem;

bool process_command(const std::string& line,
                     ps::InteractionService& svc,
                     std::string& current_graph,
                     bool& modified,
                     CliConfig& config) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    if (cmd.empty()) return true;
    try {
        if (cmd == "help") {
            print_repl_help(config);
        } else if (cmd == "clear" || cmd == "cls") {
            std::cout << "\033[2J\033[1;1H";
        } else if (cmd == "graphs") {
            auto names = svc.cmd_list_graphs();
            if (names.empty()) { std::cout << "(no graphs loaded)\n"; return true; }
            std::cout << "Loaded graphs:" << std::endl;
            for (auto& n : names) {
                std::cout << "  - " << n;
                if (n == current_graph) std::cout << "  [current]";
                std::cout << std::endl;
            }
        } else if (cmd == "load") {
            std::string name, yaml; iss >> name >> yaml;
            if (name.empty()) { std::cout << "Usage: load <name> [yaml]\n"; return true; }
            if (yaml.empty()) {
                auto session_yaml = ps::fs::path("sessions")/name/"content.yaml";
                if (!ps::fs::exists(session_yaml)) {
                    std::cout << "Error: session YAML not found: " << session_yaml << "\n";
                    std::cout << "Hint: provide an explicit YAML path: load <name> <yaml>\n";
                    return true;
                }
                auto ok = svc.cmd_load_graph(name, "sessions", "", config.loaded_config_path);
                if (!ok) { std::cout << "Error: failed to load session graph '" << name << "' from disk.\n"; return true; }
                if (config.switch_after_load) {
                    current_graph = *ok;
                    config.loaded_config_path = (ps::fs::absolute(ps::fs::path("sessions")/name/"config.yaml")).string();
                }
                std::cout << "Loaded graph '" << name << "' from session (content.yaml).\n";
            } else {
                auto ok = svc.cmd_load_graph(name, "sessions", yaml, config.loaded_config_path);
                if (!ok) { std::cout << "Error: failed to load graph '" << name << "' from '" << yaml << "'.\n"; return true; }
                if (config.switch_after_load) {
                    current_graph = *ok;
                    config.loaded_config_path = (ps::fs::absolute(ps::fs::path("sessions")/name/"config.yaml")).string();
                }
                std::cout << "Loaded graph '" << name << "' (yaml: " << yaml << ").\n";
            }
        } else if (cmd == "switch") {
            std::string name; iss >> name; if (name.empty()) { std::cout << "Usage: switch <name>\n"; return true; }
            auto names = svc.cmd_list_graphs();
            if (std::find(names.begin(), names.end(), name) == names.end()) { std::cout << "Graph not found: " << name << "\n"; return true; }
            current_graph = name; std::cout << "Switched to '" << name << "'.\n";
        } else if (cmd == "close") {
            std::string name; iss >> name; if (name.empty()) { std::cout << "Usage: close <name>\n"; return true; }
            if (!svc.cmd_close_graph(name)) { std::cout << "Error: failed to close '" << name << "'.\n"; return true; }
            if (current_graph == name) current_graph.clear();
            std::cout << "Closed graph '" << name << "'.\n";
        } else if (cmd == "print") {
            if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
            std::string target_str = "all";
            std::string mode_str = config.default_print_mode;
            bool target_is_set = false;

            std::string arg;
            while(iss >> arg) {
                if (arg == "f" || arg == "full" || arg == "s" || arg == "simplified") { mode_str = arg; }
                else {
                    if (target_is_set) { std::cout << "Warning: Multiple targets specified for print; using last one ('" << arg << "').\n"; }
                    target_str = arg;
                    target_is_set = true;
                }
            }

            bool show_params = (mode_str == "f" || mode_str == "full");
            if (target_str == "all") {
                auto dump = svc.cmd_dump_tree(current_graph, std::nullopt, show_params);
                if (dump) std::cout << *dump; else std::cout << "(failed to dump tree)\n";
            } else {
                try {
                    int node_id = std::stoi(target_str);
                    auto dump = svc.cmd_dump_tree(current_graph, node_id, show_params);
                    if (dump) std::cout << *dump; else std::cout << "(failed to dump tree)\n";
                } catch (const std::exception&) {
                    std::cout << "Error: Invalid target '" << target_str << "'. Must be an integer ID or 'all'." << std::endl;
                }
            }
        } else if (cmd == "node") {
            if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
            std::optional<int> maybe_id;
            std::string word;
            if (iss >> word) { try { maybe_id = std::stoi(word); } catch (...) { maybe_id.reset(); } }
            run_node_editor_full(svc, current_graph, maybe_id);
            std::cout << "\033[2J\033[1;1H";
        } else if (cmd == "ops") {
            std::string mode_arg; iss >> mode_arg;
            if (mode_arg.empty()) { mode_arg = config.default_ops_list_mode; }

            std::string display_mode, display_title;

            if (mode_arg == "all" || mode_arg == "a") { display_mode = "all"; display_title = "all"; }
            else if (mode_arg == "builtin" || mode_arg == "b") { display_mode = "builtin"; display_title = "built-in"; }
            else if (mode_arg == "plugins" || mode_arg == "custom" || mode_arg == "p" || mode_arg == "c") { display_mode = "plugins"; display_title = "plugins"; }
            else { std::cout << "Error: Invalid mode for 'ops'. Use: all (a), builtin (b), or plugins (p/c)." << std::endl; return true; }

            std::map<std::string, std::vector<std::pair<std::string, std::string>>> grouped_ops;
            int op_count = 0;

            auto op_sources = svc.cmd_ops_sources();
            for (const auto& pair : op_sources) {
                const std::string& key = pair.first;
                const std::string& source = pair.second;
                bool is_builtin = (source == "built-in");

                if ((display_mode == "builtin" && !is_builtin) || (display_mode == "plugins" && is_builtin)) continue;

                size_t colon_pos = key.find(':');
                if (colon_pos != std::string::npos) {
                    std::string type = key.substr(0, colon_pos);
                    std::string subtype = key.substr(colon_pos + 1);
                    grouped_ops[type].push_back({subtype, source});
                    op_count++;
                }
            }

            if (op_count == 0) {
                if (display_mode == "plugins") std::cout << "No plugin operations are registered." << std::endl;
                else std::cout << "No operations are registered." << std::endl;
                return true;
            }

            std::cout << "Available Operations (" << display_title << "):" << std::endl;
            for (auto& pair : grouped_ops) {
                std::sort(pair.second.begin(), pair.second.end());
                std::cout << "\n  Type: " << pair.first << std::endl;
                for (const auto& op_info : pair.second) {
                    std::cout << "    - " << op_info.first;
                    if (op_info.second != "built-in") {
                        std::string plugin_path_str = op_info.second;
                        std::string display_path;
                        if (config.ops_plugin_path_mode == "absolute_path") { display_path = plugin_path_str; }
                        else if (config.ops_plugin_path_mode == "relative_path") { display_path = fs::relative(plugin_path_str).string(); }
                        else { display_path = fs::path(plugin_path_str).filename().string(); }
                        std::cout << "  [plugin: " << display_path << "]";
                    }
                    std::cout << std::endl;
                }
            }

        } else if (cmd == "traversal") {
            if (current_graph.empty()) {
                std::cout << "No current graph. Use load/switch.\n";
                return true;
            }
            std::string arg;
            std::string print_tree_mode = "none";
            bool show_mem = false, show_disk = false, do_check = false, do_check_remove = false;

            // 1. 参数解析 (保持不变，逻辑是正确的)
            std::vector<std::string> args;
            while (iss >> arg) args.push_back(arg);
            const auto parse_tokens = [&](const std::vector<std::string>& toks){
                for (const auto& a : toks) {
                    if (a == "f" || a == "full") print_tree_mode = "full";
                    else if (a == "s" || a == "simplified") print_tree_mode = "simplified";
                    else if (a == "n" || a == "no_tree") print_tree_mode = "none";
                    else if (a == "md") { show_mem = true; show_disk = true; }
                    else if (a == "m") show_mem = true;
                    else if (a == "d") show_disk = true;
                    else if (a == "cr") do_check_remove = true;
                    else if (a == "c") do_check = true;
                }
            };
            if (args.empty()) {
                std::istringstream default_iss(config.default_traversal_arg);
                std::vector<std::string> def_args; while (default_iss >> arg) def_args.push_back(arg);
                parse_tokens(def_args);
            } else {
                parse_tokens(args);
            }

            // 2. 执行缓存操作 (添加用户反馈)
            if (do_check_remove) {
                std::cout << "Synchronizing disk cache with memory state..." << std::endl;
                svc.cmd_synchronize_disk_cache(current_graph, config.cache_precision);
                std::cout << "Done." << std::endl;
            } else if (do_check) {
                std::cout << "Checking and saving caches for all nodes..." << std::endl;
                svc.cmd_cache_all_nodes(current_graph, config.cache_precision);
                std::cout << "Done." << std::endl;
            }
            
            // 3. 打印可选的依赖树 (保持不变)
            if (print_tree_mode == "full") { 
                auto s = svc.cmd_dump_tree(current_graph, std::nullopt, true); 
                if (s) std::cout << *s; 
            } else if (print_tree_mode == "simplified") { 
                auto s = svc.cmd_dump_tree(current_graph, std::nullopt, false); 
                if (s) std::cout << *s; 
            }

            // 4. MODIFIED: 调用新的 API 并按要求格式化输出
            auto details_map_opt = svc.cmd_traversal_details(current_graph);
            if (!details_map_opt || details_map_opt->empty()) {
                std::cout << "(No ending nodes found or graph is cyclic)\n";
                return true;
            }

            bool first_tree = true;
            for (const auto& [end_node_id, node_info_vector] : *details_map_opt) {
                if (!first_tree) {
                    std::cout << "\n"; // 在不同的遍历树之间加一个空行
                }
                std::cout << "Post-order (eval order) for end node " << end_node_id << ":" << std::endl;
                
                int counter = 1;
                for (const auto& node_info : node_info_vector) {
                    // 打印行号、ID 和名称
                    std::cout << counter++ << ". " << node_info.id << " (" << node_info.name << ")";

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
        } else if (cmd == "config") {
            run_config_editor(config);
        } else if (cmd == "read") {
            if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
            std::string path; iss >> path; if (path.empty()) std::cout << "Usage: read <filepath>\n";
            else { if (svc.cmd_reload_yaml(current_graph, path)) { modified = false; std::cout << "Loaded graph from " << path << "\n"; } else { std::cout << "Failed to load." << std::endl; } }
        } else if (cmd == "source") {
            std::string filename; iss >> filename;
            if (filename.empty()) { std::cout << "Usage: source <filename>\n"; return true; }
            std::ifstream script_file(filename);
            if (!script_file) { std::cout << "Error: Cannot open script file: " << filename << "\n"; return true; }
            std::string script_line;
            while (std::getline(script_file, script_line)) {
                if (script_line.empty() || script_line.find_first_not_of(" \t") == std::string::npos || script_line[0] == '#') continue;
                std::cout << "ps> " << script_line << std::endl;
                if (!process_command(script_line, svc, current_graph, modified, config)) return false;
            }
        } else if (cmd == "output") {
            if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
            std::string path; iss >> path; if (path.empty()) std::cout << "Usage: output <filepath>\n";
            else { if (svc.cmd_save_yaml(current_graph, path)) { modified = false; std::cout << "Saved to " << path << "\n"; } else { std::cout << "Failed to save." << std::endl; } }
        } else if (cmd == "clear-graph") {
            if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
            svc.cmd_clear_graph(current_graph); modified = true; std::cout << "Graph cleared.\n";
        } else if (cmd == "clear-cache" || cmd == "cc") {
            if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
            std::string arg; iss >> arg;
            if (arg.empty()) { arg = config.default_cache_clear_arg; }
            if (arg == "both" || arg == "md" || arg == "dm") svc.cmd_clear_cache(current_graph);
            else if (arg == "drive" || arg == "d") svc.cmd_clear_drive_cache(current_graph);
            else if (arg == "memory" || arg == "m") svc.cmd_clear_memory_cache(current_graph);
            else { std::cout << "Error: Invalid argument for clear-cache. Use: m, d, or md." << std::endl; }
        } else if (cmd == "free") {
            if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
            svc.cmd_free_transient_memory(current_graph);
            std::cout << "Freed intermediate memory." << std::endl;
        } else if (cmd == "compute") {
            if (current_graph.empty()) {
                std::cout << "No current graph. Use load/switch.\n";
                return true;
            }
            int node_id = -1;
            iss >> node_id;
            if (node_id < 0) {
                std::cout << "Usage: compute <id> [flags...]\n";
                return true;
            }

            // --- 参数解析 (与之前相同) ---
            std::vector<std::string> flags;
            std::string flag;
            while (iss >> flag) flags.push_back(flag);
            // If no flags provided inline, fall back to defaults from config
            if (flags.empty() && !config.default_compute_args.empty()) {
                std::istringstream def_iss(config.default_compute_args);
                while (def_iss >> flag) flags.push_back(flag);
            }
            bool force = false, force_deep = false, parallel = false, timer_console = false, timer_log = false, mute = false;
            std::string timer_log_path = config.default_timer_log_path;

            // ... (保持原来的参数解析逻辑不变) ...
            for (size_t i = 0; i < flags.size(); ++i) {
                const auto& f = flags[i];
                if (f == "force") force = true;
                else if (f == "force-deep") force_deep = true; // 注意: force_deep 在后端尚未完全实现，此处仅为示例
                else if (f == "parallel") parallel = true;
                else if (f == "t" || f == "-t" || f == "timer") timer_console = true;
                else if (f == "tl" || f == "-tl") { timer_log = true; if (i + 1 < flags.size()) { timer_log_path = flags[i + 1]; ++i; } }
                else if (f == "m" || f == "-m" || f == "mute") mute = true;
            }

            // 在开始之前清空可能残留的事件，避免上一次计算遗留的消息被误打印
            (void)svc.cmd_drain_compute_events(current_graph);

            // --- 异步执行与轮询 ---
            auto future_opt = svc.cmd_compute_async(current_graph, node_id, config.cache_precision,
                                                    /*force_recache*/ (force || force_deep),
                                                    /*timing*/ (timer_console || timer_log),
                                                    /*parallel*/ parallel,
                                                    /*quiet*/ mute,
                                                    /*disable_disk_cache*/ force_deep);

            if (!future_opt) {
                std::cout << "Error: failed to schedule compute task.\n";
                return true;
            }
            auto& future = *future_opt;

            std::cout << "Computing..." << std::endl;

            // 轮询直到计算完成
            while (future.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout) {
                auto events = svc.cmd_drain_compute_events(current_graph);
                if (!events) continue;
                
                if (!mute) {
                    for (const auto& event : *events) {
                        // 格式: xx node completed [source]
                        std::cout << "  - Node " << event.id << " (" << event.name << ") completed [" << event.source << "]" << std::endl;
                    }
                }
            }

            // 计算已结束（成功或失败），做一次最终事件回收，确保最后一个节点消息不会丢失
            if (auto tail_events = svc.cmd_drain_compute_events(current_graph)) {
                if (!mute) {
                    for (const auto& event : *tail_events) {
                        std::cout << "  - Node " << event.id << " (" << event.name << ") completed [" << event.source << "]" << std::endl;
                    }
                }
            }

            // 获取最终结果并处理错误
            bool ok = future.get();
            if (!ok) {
                std::cout << "Error: Compute task failed.\n";
                if (auto err = svc.cmd_last_error(current_graph)) {
                    std::cout << "  Reason: " << err->message << std::endl;
                }
                // 出错时如果请求了计时，也尝试打印部分计时信息（若有）
                if (timer_console || timer_log) {
                    if (auto timers = svc.cmd_timing(current_graph)) {
                        std::stringstream log_buffer;
                        log_buffer << "Timing Report (partial, total " << timers->total_ms << " ms):" << std::endl;
                        for (const auto& nt : timers->node_timings) {
                            log_buffer << "  - Node " << nt.id << " (" << nt.name << ") completed in "
                                       << nt.elapsed_ms << " ms [" << nt.source << "]" << std::endl;
                        }
                        if (timer_console) std::cout << log_buffer.str();
                        if (timer_log) {
                            std::ofstream log_file(timer_log_path);
                            if (log_file) {
                                log_file << log_buffer.str();
                                std::cout << "Timing report saved to " << timer_log_path << std::endl;
                            } else {
                                std::cout << "Error: Could not open log file " << timer_log_path << std::endl;
                            }
                        }
                    }
                }
                return true;
            }

            std::cout << "Computation finished." << std::endl;

            // --- NEW: 详细计时器报告 ---
            if (timer_console || timer_log) {
                auto timers = svc.cmd_timing(current_graph);
                if (!timers) {
                    std::cout << "Could not retrieve timing information." << std::endl;
                    return true;
                }

                std::stringstream log_buffer;
                log_buffer << "Timing Report (total " << timers->total_ms << " ms):" << std::endl;

                for (const auto& nt : timers->node_timings) {
                    // 格式: xx node completed in [xx time][source]
                    log_buffer << "  - Node " << nt.id << " (" << nt.name << ") completed in "
                            << nt.elapsed_ms << " ms [" << nt.source << "]" << std::endl;
                }

                if (timer_console) {
                    std::cout << log_buffer.str();
                }

                if (timer_log) {
                    // 此处可以实现更复杂的YAML日志记录，为简化先直接写入
                    std::ofstream log_file(timer_log_path);
                    if (log_file) {
                        log_file << log_buffer.str();
                        std::cout << "Timing report saved to " << timer_log_path << std::endl;
                    } else {
                        std::cout << "Error: Could not open log file " << timer_log_path << std::endl;
                        }
                    }
                }
            } else if (cmd == "save") {
                if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
                int node_id = -1; iss >> node_id; if (node_id < 0) { std::cout << "Usage: save <id> <file>\n"; return true; }
                std::string path; iss >> path; if (path.empty()) { std::cout << "Usage: save <id> <file>\n"; return true; }

            auto image = svc.cmd_compute_and_get_image(current_graph, node_id, config.cache_precision,
                                                       /*force*/false, /*timing*/false, /*parallel*/false,
                                                       /*disable_disk_cache*/false);
            if (!image || image->empty()) { std::cout << "No image to save (node produced no image).\n"; return true; }
            if (save_fp32_image(*image, path, config)) { std::cout << "Saved image to " << path << "\n"; }
            else { std::cout << "Failed to save image to " << path << "\n"; }
        } else if (cmd == "exit" || cmd == "quit" || cmd == "q") {
            if (!current_graph.empty() && ask_yesno("Synchronize disk cache with memory state before exiting?", config.exit_prompt_sync)) {
                svc.cmd_synchronize_disk_cache(current_graph, config.cache_precision);
            }
            return false;
        } else {
            std::cout << "Unknown command: " << cmd << ". Type 'help' for a list of commands.\n";
        }
    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << "\n";
    }
    return true;
}
