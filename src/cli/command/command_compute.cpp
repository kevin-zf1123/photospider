// FILE: src/cli/command/command_compute.cpp
#include <iostream>
#include <sstream>
#include <vector>
#include <future>
#include <chrono>
#include <fstream>
#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

bool handle_compute(std::istringstream& iss,
                    ps::InteractionService& svc,
                    std::string& current_graph,
                    bool& /*modified*/,
                    CliConfig& config) {
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

    // 参数解析
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

    for (size_t i = 0; i < flags.size(); ++i) {
        const auto& f = flags[i];
        if (f == "force") force = true;
        else if (f == "force-deep") force_deep = true; // 注意: force_deep 在后端尚未完全实现，此处仅为示例
        else if (f == "parallel") parallel = true;
        else if (f == "t" || f == "-t" || f == "timer") timer_console = true;
        else if (f == "tl" || f == "-tl") { timer_log = true; if (i + 1 < flags.size()) { timer_log_path = flags[i + 1]; ++i; } }
        else if (f == "m" || f == "-m" || f == "mute") mute = true;
    }

    // 在开始之前清空可能残留的事件
    (void)svc.cmd_drain_compute_events(current_graph);

    // 异步执行与轮询
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

    while (future.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout) {
        auto events = svc.cmd_drain_compute_events(current_graph);
        if (!events) continue;
        
        if (!mute) {
            for (const auto& event : *events) {
                std::cout << "  - Node " << event.id << " (" << event.name << ") completed [" << event.source << "]" << std::endl;
            }
        }
    }

    // 计算已结束，做一次最终事件回收
    if (auto tail_events = svc.cmd_drain_compute_events(current_graph)) {
        if (!mute) {
            for (const auto& event : *tail_events) {
                std::cout << "  - Node " << event.id << " (" << event.name << ") completed [" << event.source << "]" << std::endl;
            }
        }
    }

    bool ok = future.get();
    if (!ok) {
        std::cout << "Error: Compute task failed.\n";
        if (auto err = svc.cmd_last_error(current_graph)) {
            std::cout << "  Reason: " << err->message << std::endl;
        }
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

    if (timer_console || timer_log) {
        auto timers = svc.cmd_timing(current_graph);
        if (!timers) {
            std::cout << "Could not retrieve timing information." << std::endl;
            return true;
        }

        std::stringstream log_buffer;
        log_buffer << "Timing Report (total " << timers->total_ms << " ms):" << std::endl;

        for (const auto& nt : timers->node_timings) {
            log_buffer << "  - Node " << nt.id << " (" << nt.name << ") completed in "
                       << nt.elapsed_ms << " ms [" << nt.source << "]" << std::endl;
        }

        if (timer_console) {
            std::cout << log_buffer.str();
        }

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
    return true;
}

void print_help_compute(const CliConfig& /*config*/) {
    print_help_from_file("help_compute.txt");
}

