// FILE: src/cli/command/command_compute.cpp
#include <iostream>
#include <sstream>
#include <vector>
#include <future>
#include <chrono>
#include <fstream>
#include <numeric>
#include <unordered_set>
#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

// [核心修复] 将异步执行和结果处理逻辑提取到一个辅助函数中
static bool execute_and_wait(
    ps::InteractionService& svc, 
    const std::string& current_graph, 
    int node_id, 
    const CliConfig& config,
    bool force, bool force_deep, bool parallel, bool timer_console, bool timer_log, bool mute, const std::string& timer_log_path,
    bool nosave
) {
    (void)svc.cmd_drain_compute_events(current_graph);

    auto future_opt = svc.cmd_compute_async(current_graph, node_id, config.cache_precision,
                                            (force || force_deep), (timer_console || timer_log),
                                            parallel, mute, force_deep, nosave);

    if (!future_opt) {
        std::cout << "Error: failed to schedule compute task for node " << node_id << ".\n";
        return false;
    }

    auto& future = *future_opt;
    if (!mute) {
        std::cout << "Computing node " << node_id << "..." << std::endl;
    }

    while (future.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout) {
        if (!mute) {
            if (auto events = svc.cmd_drain_compute_events(current_graph)) {
                for (const auto& event : *events) {
                    std::cout << "  - Node " << event.id << " (" << event.name << ") completed [" << event.source << "]" << std::endl;
                }
            }
        }
    }

    if (!mute) {
        if (auto tail_events = svc.cmd_drain_compute_events(current_graph)) {
            for (const auto& event : *tail_events) {
                std::cout << "  - Node " << event.id << " (" << event.name << ") completed [" << event.source << "]" << std::endl;
            }
        }
    }
    
    bool ok = future.get();
    if (!ok) {
        std::cout << "Error: Compute task failed for node " << node_id << ".\n";
        if (auto err = svc.cmd_last_error(current_graph)) {
            std::cout << "  Reason: " << err->message << std::endl;
        }
    }
    return ok;
}


bool handle_compute(std::istringstream& iss,
                    ps::InteractionService& svc,
                    std::string& current_graph,
                    bool& /*modified*/,
                    CliConfig& config) {
    if (current_graph.empty()) {
        std::cout << "No current graph. Use load/switch.\n";
        return true;
    }

    std::string target_str;
    iss >> target_str;
    if (target_str.empty()) {
        std::cout << "Usage: compute <id|all> [flags...]\n";
        return true;
    }

    // [核心修复] 参数解析与验证
    std::vector<int> nodes_to_compute;
    auto all_node_ids_opt = svc.cmd_list_node_ids(current_graph);
    if (!all_node_ids_opt) {
        std::cout << "Error: Could not retrieve node list for current graph.\n";
        return true;
    }
    auto& all_node_ids = *all_node_ids_opt;

    if (target_str == "all") {
        // [核心修复] 使用正确的 svc.cmd_ending_nodes
        auto ending_nodes_opt = svc.cmd_ending_nodes(current_graph);
        if (ending_nodes_opt) {
            nodes_to_compute = *ending_nodes_opt;
        }
        if (nodes_to_compute.empty()) {
            std::cout << "No ending nodes to compute in the graph.\n";
            return true;
        }
    } else {
        try {
            int node_id = std::stoi(target_str);
            bool exists = false;
            for(int id : all_node_ids) {
                if (id == node_id) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                std::cout << "Error: Node with ID " << node_id << " does not exist in the current graph.\n";
                return true;
            }
            nodes_to_compute.push_back(node_id);
        } catch (const std::invalid_argument&) {
            std::cout << "Error: Invalid target '" << target_str << "'. Must be an integer ID or 'all'.\n";
            return true;
        }
    }
    
    // 解析 flags
    std::vector<std::string> flags;
    std::string flag;
    while (iss >> flag) flags.push_back(flag);
    if (flags.empty() && !config.default_compute_args.empty()) {
        std::istringstream def_iss(config.default_compute_args);
        while (def_iss >> flag) flags.push_back(flag);
    }
    bool force = false, force_deep = false, parallel = false, timer_console = false, timer_log = false, mute = false, nosave = false;
    std::string timer_log_path = config.default_timer_log_path;
    // Known compute flags to avoid mis-parsing as a tl path
    static const std::unordered_set<std::string> kKnownFlags = {
        "force", "force-deep", "parallel",
        "t", "-t", "timer",
        "tl", "-tl",
        "m", "-m", "mute",
        "nosave", "ns"
    };
    for (size_t i = 0; i < flags.size(); ++i) {
        const auto& f = flags[i];
        if (f == "force") force = true;
        else if (f == "force-deep") force_deep = true;
        else if (f == "parallel") parallel = true;
        else if (f == "t" || f == "-t" || f == "timer") timer_console = true;
        else if (f == "tl" || f == "-tl") {
            // Enable timer log; only consume next token as a path if it is NOT a known flag.
            timer_log = true;
            if (i + 1 < flags.size()) {
                const std::string& maybe_path = flags[i + 1];
                if (kKnownFlags.find(maybe_path) == kKnownFlags.end()) {
                    timer_log_path = maybe_path;
                    ++i; // consume the path token
                }
            }
        }
        else if (f == "m" || f == "-m" || f == "mute") mute = true;
        else if (f == "nosave" || f == "ns") nosave = true;
    }

    // [核心修复] 循环执行计算，并聚合每次 compute 的计时
    bool all_ok = true;
    auto overall_start_time = std::chrono::high_resolution_clock::now();
    std::vector<ps::NodeTiming> aggregated_timings;
    aggregated_timings.reserve(128);
    
    for (int node_id : nodes_to_compute) {
        if (!execute_and_wait(svc, current_graph, node_id, config, force, force_deep, parallel, timer_console, timer_log, mute, timer_log_path, nosave)) {
            all_ok = false;
            break; // 一个失败就停止
        }
        // 在下一次 compute 重置计时之前，抓取本次的节点计时并聚合
        if (timer_console || timer_log) {
            if (auto timers_opt_local = svc.cmd_timing(current_graph)) {
                for (const auto& nt : timers_opt_local->node_timings) aggregated_timings.push_back(nt);
            }
        }
    }
    
    auto overall_end_time = std::chrono::high_resolution_clock::now();

    std::cout << (all_ok ? "Computation finished." : "Computation failed.") << std::endl;

    if (timer_console || timer_log) {
        // 构造聚合后的 TimingCollector
        ps::TimingCollector agg;
        agg.node_timings = std::move(aggregated_timings);
        // 使用节点耗时之和作为 "total"，避免包含 REPL 轮询等待等开销
        double total_node_ms = 0.0;
        for (const auto& nt : agg.node_timings) total_node_ms += nt.elapsed_ms;
        agg.total_ms = total_node_ms;

        const double wall_ms = std::chrono::duration<double, std::milli>(overall_end_time - overall_start_time).count();

        std::stringstream log_buffer;
        log_buffer << "Timing Report (total " << agg.total_ms << " ms, wall " << wall_ms << " ms):" << std::endl;
        for (const auto& nt : agg.node_timings) {
            log_buffer << "  - Node " << nt.id << " (" << nt.name << ") completed in "
                       << nt.elapsed_ms << " ms [" << nt.source << "]" << std::endl;
        }
        if (timer_console) std::cout << log_buffer.str();
        if (timer_log) {
            std::ofstream log_file(timer_log_path, std::ios::app); // 追加模式
            if (log_file) {
                log_file << log_buffer.str() << "\n";
                std::cout << "Timing report appended to " << timer_log_path << std::endl;
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
