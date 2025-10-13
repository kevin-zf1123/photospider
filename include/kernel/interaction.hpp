// Photospider kernel: Interaction API between CLI and Kernel
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <future>

#include "kernel/kernel.hpp"
#include "kernel/plugin_result.hpp"

namespace ps {

// Minimal interaction facade to decouple frontends from Kernel internals.
class InteractionService {
public:
    explicit InteractionService(Kernel& kernel) : kernel_(kernel) {}

    // Graph lifecycle
    std::optional<std::string> cmd_load_graph(const std::string& name,
                                              const std::string& root_dir,
                                              const std::string& yaml_path,
                                              const std::string& config_path = "") {
        return kernel_.load_graph(name, root_dir, yaml_path, config_path);
    }
    bool cmd_close_graph(const std::string& name) { return kernel_.close_graph(name); }
    std::vector<std::string> cmd_list_graphs() const { return kernel_.list_graphs(); }

    // [API修复] 新增方法以从 CLI 安全地获取末端节点
    std::optional<std::vector<int>> cmd_ending_nodes(const std::string& graph) {
        return kernel_.ending_nodes(graph);
    }

    // Compute
    bool cmd_compute(const std::string& graph, int node_id, const std::string& cache_precision,
                     bool force, bool timing, bool parallel, bool quiet = false,
                     bool disable_disk_cache = false, bool nosave = false,
                     std::vector<BenchmarkEvent>* benchmark_events = nullptr) {
        return kernel_.compute(graph, node_id, cache_precision, force, timing, parallel, quiet, disable_disk_cache, nosave, benchmark_events);
    }
    std::optional<TimingCollector> cmd_timing(const std::string& graph) { return kernel_.get_timing(graph); }

    // Plugins
    void cmd_plugins_load(const std::vector<std::string>& dirs) { kernel_.plugins().load_from_dirs(dirs); }
    PluginLoadResult cmd_plugins_load_report(const std::vector<std::string>& dirs) { return kernel_.plugins().load_from_dirs_report(dirs); }
    int cmd_plugins_unload_all() { return kernel_.plugins().unload_all_plugins(); }
    void cmd_seed_builtin_ops() { kernel_.plugins().seed_builtins_from_registry(); }

    // Ops overview (type:subtype -> source path or "built-in")
    std::map<std::string, std::string> cmd_ops_sources() const { return kernel_.plugins().op_sources(); }
    // Combined ops: collapse monolithic/tiled HP/RT under a single op key; frontends should use this
    std::vector<std::string> cmd_ops_combined_keys() const {
        return ps::OpRegistry::instance().get_combined_keys();
    }
    std::map<std::string, std::string> cmd_ops_combined_sources() const {
        std::map<std::string, std::string> out;
        auto keys = ps::OpRegistry::instance().get_combined_keys();
        const auto& sources = kernel_.plugins().op_sources();
        for (const auto& k : keys) {
            auto it = sources.find(k);
            if (it != sources.end()) {
                out[k] = it->second;
                continue;
            }
            // Fallback: if an alias "*_tiled" exists in sources, use its source
            auto pos = k.rfind(':');
            if (pos != std::string::npos) {
                std::string base = k.substr(0, pos + 1);
                std::string tiled = base + (k.substr(pos + 1) + std::string("_tiled"));
                auto it2 = sources.find(tiled);
                if (it2 != sources.end()) { out[k] = it2->second; continue; }
            }
            out[k] = "built-in"; // default when unknown
        }
        return out;
    }

    // IO / cache / traversal / printing
    bool cmd_reload_yaml(const std::string& graph, const std::string& yaml_path) { return kernel_.reload_graph_yaml(graph, yaml_path); }
    bool cmd_save_yaml(const std::string& graph, const std::string& yaml_path) { return kernel_.save_graph_yaml(graph, yaml_path); }
    bool cmd_clear_drive_cache(const std::string& graph) { return kernel_.clear_drive_cache(graph); }
    bool cmd_clear_memory_cache(const std::string& graph) { return kernel_.clear_memory_cache(graph); }
    bool cmd_clear_cache(const std::string& graph) { return kernel_.clear_cache(graph); }
    bool cmd_cache_all_nodes(const std::string& graph, const std::string& precision) { return kernel_.cache_all_nodes(graph, precision); }
    // Structured stats APIs
    std::optional<GraphModel::DriveClearResult> cmd_clear_drive_cache_stats(const std::string& graph) { return kernel_.clear_drive_cache_stats(graph); }
    std::optional<GraphModel::MemoryClearResult> cmd_clear_memory_cache_stats(const std::string& graph) { return kernel_.clear_memory_cache_stats(graph); }
    std::optional<GraphModel::CacheSaveResult> cmd_cache_all_nodes_stats(const std::string& graph, const std::string& precision) { return kernel_.cache_all_nodes_stats(graph, precision); }
    std::optional<GraphModel::MemoryClearResult> cmd_free_transient_memory_stats(const std::string& graph) { return kernel_.free_transient_memory_stats(graph); }
    std::optional<GraphModel::DiskSyncResult> cmd_synchronize_disk_cache_stats(const std::string& graph, const std::string& precision) { return kernel_.synchronize_disk_cache_stats(graph, precision); }
    // Structured stats wrappers can be added when needed.
    bool cmd_free_transient_memory(const std::string& graph) { return kernel_.free_transient_memory(graph); }
    bool cmd_synchronize_disk_cache(const std::string& graph, const std::string& precision) { return kernel_.synchronize_disk_cache(graph, precision); }
    bool cmd_clear_graph(const std::string& graph) { return kernel_.clear_graph(graph); }

    std::optional<std::string> cmd_dump_tree(const std::string& graph, std::optional<int> node_id, bool show_parameters) {
        return kernel_.dump_dependency_tree(graph, node_id, show_parameters);
    }
    std::optional<Kernel::LastError> cmd_last_error(const std::string& graph) const {
        return kernel_.last_error(graph);
    }
    std::optional<std::map<int, std::vector<int>>> cmd_traversal_orders(const std::string& graph) {
        return kernel_.traversal_orders(graph);
    }
    std::optional<std::map<int, std::vector<Kernel::TraversalNodeInfo>>> cmd_traversal_details(const std::string& graph) {
        return kernel_.traversal_details(graph);
    }
    std::optional<std::vector<GraphEventService::ComputeEvent>> cmd_drain_compute_events(const std::string& graph) {
        return kernel_.drain_compute_events(graph);
    }
    std::optional<cv::Mat> cmd_compute_and_get_image(const std::string& graph, int node_id, const std::string& precision,
                                                     bool force, bool timing, bool parallel,
                                                     bool disable_disk_cache = false) {
        return kernel_.compute_and_get_image(graph, node_id, precision, force, timing, parallel, disable_disk_cache);
    }
    std::optional<std::vector<int>> cmd_trees_containing_node(const std::string& graph, int node_id) {
        return kernel_.trees_containing_node(graph, node_id);
    }

    // Nodes
    std::optional<std::vector<int>> cmd_list_node_ids(const std::string& graph) { return kernel_.list_node_ids(graph); }
    std::optional<std::string> cmd_get_node_yaml(const std::string& graph, int node_id) { return kernel_.get_node_yaml(graph, node_id); }
    bool cmd_set_node_yaml(const std::string& graph, int node_id, const std::string& yaml_text) { return kernel_.set_node_yaml(graph, node_id, yaml_text); }
    std::optional<std::future<bool>> cmd_compute_async(const std::string& graph, int node_id, const std::string& cache_precision,
                                                      bool force, bool timing, bool parallel, bool quiet = false,
                                                      bool disable_disk_cache = false, bool nosave = false,
                                                      std::vector<BenchmarkEvent>* benchmark_events = nullptr) {
        return kernel_.compute_async(graph, node_id, cache_precision, force, timing, parallel, quiet, disable_disk_cache, nosave, benchmark_events);
    }
    std::optional<double> cmd_get_last_io_time(const std::string& graph) {
        return kernel_.get_last_io_time(graph);
    }

    // [新增] GPU 上下文访问器
    id cmd_get_metal_device(const std::string& graph) {
        return kernel_.get_metal_device(graph);
    }
private:
    Kernel& kernel_;
};

} // namespace ps
