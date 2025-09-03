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
    struct LastError { GraphErrc code = GraphErrc::Unknown; std::string message; };

    std::optional<std::string> load_graph(const std::string& name,
                                          const std::string& root_dir,
                                          const std::string& yaml_path,
                                          const std::string& config_path = "");

    bool close_graph(const std::string& name);
    std::vector<std::string> list_graphs() const;

    // [修复] 新增 post 方法
    template<typename Fn>
    auto post(const std::string& name, Fn&& fn) -> std::future<decltype(fn(std::declval<NodeGraph&>()))> {
        auto it = graphs_.find(name);
        if (it == graphs_.end()) {
            throw std::runtime_error("Graph not found: " + name);
        }
        return it->second->post(std::forward<Fn>(fn));
    }

    bool compute(const std::string& name, int node_id, const std::string& cache_precision,
                 bool force_recache, bool enable_timing, bool parallel, bool quiet,
                 bool disable_disk_cache,
                 std::vector<BenchmarkEvent>* benchmark_events = nullptr);
    std::optional<std::future<bool>> compute_async(const std::string& name, int node_id, const std::string& cache_precision,
                                                   bool force_recache, bool enable_timing, bool parallel, bool quiet,
                                                   bool disable_disk_cache,
                                                   std::vector<BenchmarkEvent>* benchmark_events = nullptr);
    std::optional<TimingCollector> get_timing(const std::string& name);

    bool reload_graph_yaml(const std::string& name, const std::string& yaml_path);
    bool save_graph_yaml(const std::string& name, const std::string& yaml_path);
    bool clear_drive_cache(const std::string& name);
    bool clear_memory_cache(const std::string& name);
    bool clear_cache(const std::string& name);
    bool cache_all_nodes(const std::string& name, const std::string& cache_precision);
    bool free_transient_memory(const std::string& name);
    bool synchronize_disk_cache(const std::string& name, const std::string& cache_precision);
    bool clear_graph(const std::string& name);

    std::optional<NodeGraph::DriveClearResult> clear_drive_cache_stats(const std::string& name);
    std::optional<NodeGraph::MemoryClearResult> clear_memory_cache_stats(const std::string& name);
    std::optional<NodeGraph::CacheSaveResult> cache_all_nodes_stats(const std::string& name, const std::string& cache_precision);
    std::optional<NodeGraph::MemoryClearResult> free_transient_memory_stats(const std::string& name);
    std::optional<NodeGraph::DiskSyncResult> synchronize_disk_cache_stats(const std::string& name, const std::string& cache_precision);

    std::optional<std::string> dump_dependency_tree(const std::string& name, std::optional<int> node_id, bool show_parameters);
    std::optional<LastError> last_error(const std::string& name) const;
    std::optional<std::vector<int>> ending_nodes(const std::string& name);
    std::optional<std::vector<int>> topo_postorder_from(const std::string& name, int end_node_id);
    std::optional<std::map<int, std::vector<int>>> traversal_orders(const std::string& name);
    struct TraversalNodeInfo {
        int id;
        std::string name;
        bool has_memory_cache;
        bool has_disk_cache;
    };
    std::optional<std::map<int, std::vector<TraversalNodeInfo>>> traversal_details(const std::string& name);
    std::optional<std::vector<NodeGraph::ComputeEvent>> drain_compute_events(const std::string& name);
    std::optional<cv::Mat> compute_and_get_image(const std::string& name, int node_id, const std::string& cache_precision,
                                                 bool force_recache, bool enable_timing, bool parallel,
                                                 bool disable_disk_cache = false,
                                                 std::vector<BenchmarkEvent>* benchmark_events = nullptr);

    std::optional<std::vector<int>> list_node_ids(const std::string& name);
    std::optional<std::string> get_node_yaml(const std::string& name, int node_id);
    bool set_node_yaml(const std::string& name, int node_id, const std::string& yaml_text);

    std::optional<std::vector<int>> trees_containing_node(const std::string& name, int node_id);

    PluginManager& plugins() { return plugin_mgr_; }
    std::optional<double> get_last_io_time(const std::string& name);
private:
    std::map<std::string, std::unique_ptr<GraphRuntime>> graphs_;
    PluginManager plugin_mgr_;
    std::map<std::string, LastError> last_error_;
};

} // namespace ps