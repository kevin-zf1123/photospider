// Photospider kernel: multi-graph Kernel facade
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <map>

#include "kernel/graph_runtime.hpp"
#include "kernel/plugin_manager.hpp"
#include <opencv2/opencv.hpp>

namespace ps {

class Kernel {
public:
    // Create and start a new graph runtime with a given name and filesystem root.
    // The YAML path is loaded into the NodeGraph immediately.
    // Returns the runtime id (same as name) if created, or std::nullopt on error.
    std::optional<std::string> load_graph(const std::string& name,
                                          const std::string& root_dir,
                                          const std::string& yaml_path,
                                          const std::string& config_path = "");

    // Stop and erase a graph runtime.
    bool close_graph(const std::string& name);

    // List all loaded graphs.
    std::vector<std::string> list_graphs() const;

    // Submit a compute to a specific graph runtime.
    // Returns true if scheduled.
    bool compute(const std::string& name, int node_id, const std::string& cache_precision,
                 bool force_recache, bool enable_timing, bool parallel);

    // Access last timing results snapshot (thread-safe by posting fetch task).
    std::optional<TimingCollector> get_timing(const std::string& name);

    // ---- Additional control/IO helpers for frontends ----
    bool reload_graph_yaml(const std::string& name, const std::string& yaml_path);
    bool save_graph_yaml(const std::string& name, const std::string& yaml_path);
    bool clear_drive_cache(const std::string& name);
    bool clear_memory_cache(const std::string& name);
    bool clear_cache(const std::string& name);
    bool cache_all_nodes(const std::string& name, const std::string& cache_precision);
    bool free_transient_memory(const std::string& name);
    bool synchronize_disk_cache(const std::string& name, const std::string& cache_precision);
    bool clear_graph(const std::string& name);

    std::optional<std::string> dump_dependency_tree(const std::string& name, std::optional<int> node_id, bool show_parameters);
    std::optional<std::vector<int>> ending_nodes(const std::string& name);
    std::optional<std::vector<int>> topo_postorder_from(const std::string& name, int end_node_id);
    std::optional<std::map<int, std::vector<int>>> traversal_orders(const std::string& name);
    std::optional<cv::Mat> compute_and_get_image(const std::string& name, int node_id, const std::string& cache_precision,
                                                 bool force_recache, bool enable_timing, bool parallel);

    // Nodes inspection/editing for frontends
    std::optional<std::vector<int>> list_node_ids(const std::string& name);
    std::optional<std::string> get_node_yaml(const std::string& name, int node_id);
    bool set_node_yaml(const std::string& name, int node_id, const std::string& yaml_text);

    // For full node editor: list trees containing a node
    std::optional<std::vector<int>> trees_containing_node(const std::string& name, int node_id);

    // Plugin management
    PluginManager& plugins() { return plugin_mgr_; }

private:
    std::map<std::string, std::unique_ptr<GraphRuntime>> graphs_; // name -> runtime
    PluginManager plugin_mgr_;
};

} // namespace ps
