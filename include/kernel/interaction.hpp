// Photospider kernel: Interaction API between CLI and Kernel
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "kernel/kernel.hpp"

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

    // Compute
    bool cmd_compute(const std::string& graph, int node_id, const std::string& cache_precision,
                     bool force, bool timing, bool parallel, bool quiet = false) {
        return kernel_.compute(graph, node_id, cache_precision, force, timing, parallel, quiet);
    }
    std::optional<TimingCollector> cmd_timing(const std::string& graph) { return kernel_.get_timing(graph); }

    // Plugins
    void cmd_plugins_load(const std::vector<std::string>& dirs) { kernel_.plugins().load_from_dirs(dirs); }
    int cmd_plugins_unload_all() { return kernel_.plugins().unload_all_plugins(); }
    void cmd_seed_builtin_ops() { kernel_.plugins().seed_builtins_from_registry(); }

    // Ops overview (type:subtype -> source path or "built-in")
    std::map<std::string, std::string> cmd_ops_sources() const { return kernel_.plugins().op_sources(); }

    // IO / cache / traversal / printing
    bool cmd_reload_yaml(const std::string& graph, const std::string& yaml_path) { return kernel_.reload_graph_yaml(graph, yaml_path); }
    bool cmd_save_yaml(const std::string& graph, const std::string& yaml_path) { return kernel_.save_graph_yaml(graph, yaml_path); }
    bool cmd_clear_drive_cache(const std::string& graph) { return kernel_.clear_drive_cache(graph); }
    bool cmd_clear_memory_cache(const std::string& graph) { return kernel_.clear_memory_cache(graph); }
    bool cmd_clear_cache(const std::string& graph) { return kernel_.clear_cache(graph); }
    bool cmd_cache_all_nodes(const std::string& graph, const std::string& precision) { return kernel_.cache_all_nodes(graph, precision); }
    bool cmd_free_transient_memory(const std::string& graph) { return kernel_.free_transient_memory(graph); }
    bool cmd_synchronize_disk_cache(const std::string& graph, const std::string& precision) { return kernel_.synchronize_disk_cache(graph, precision); }
    bool cmd_clear_graph(const std::string& graph) { return kernel_.clear_graph(graph); }

    std::optional<std::string> cmd_dump_tree(const std::string& graph, std::optional<int> node_id, bool show_parameters) {
        return kernel_.dump_dependency_tree(graph, node_id, show_parameters);
    }
    std::optional<std::map<int, std::vector<int>>> cmd_traversal_orders(const std::string& graph) {
        return kernel_.traversal_orders(graph);
    }
    std::optional<cv::Mat> cmd_compute_and_get_image(const std::string& graph, int node_id, const std::string& precision,
                                                     bool force, bool timing, bool parallel) {
        return kernel_.compute_and_get_image(graph, node_id, precision, force, timing, parallel);
    }
    std::optional<std::vector<int>> cmd_trees_containing_node(const std::string& graph, int node_id) {
        return kernel_.trees_containing_node(graph, node_id);
    }

    // Nodes
    std::optional<std::vector<int>> cmd_list_node_ids(const std::string& graph) { return kernel_.list_node_ids(graph); }
    std::optional<std::string> cmd_get_node_yaml(const std::string& graph, int node_id) { return kernel_.get_node_yaml(graph, node_id); }
    bool cmd_set_node_yaml(const std::string& graph, int node_id, const std::string& yaml_text) { return kernel_.set_node_yaml(graph, node_id, yaml_text); }

private:
    Kernel& kernel_;
};

} // namespace ps
