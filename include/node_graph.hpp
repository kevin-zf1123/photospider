// FILE: include/node_graph.hpp
#pragma once
#include "ps_types.hpp"
#include "node.hpp"
#include <unordered_set>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>

namespace ps {
struct NodeTiming {
    int id = -1;
    std::string name;
    double elapsed_ms = 0.0;
    std::string source; // "memory_cache", "disk_cache", or "computed"
};
struct TimingCollector {
    std::vector<NodeTiming> node_timings;
    double total_ms = 0.0;
};
class NodeGraph {
public:
    TimingCollector timing_results;
    std::unordered_map<int, Node> nodes;
    fs::path cache_root;

    explicit NodeGraph(fs::path cache_root_dir = "cache") : cache_root(std::move(cache_root_dir)) {
        if (!cache_root.empty()) {
            fs::create_directories(cache_root);
        }
    }

    // Control stdout verbosity for compute/save messages.
    void set_quiet(bool q) { quiet_ = q; }
    bool is_quiet() const { return quiet_; }

    void clear();
    void add_node(const Node& node);
    bool has_node(int id) const;

    void load_yaml(const fs::path& yaml_path);
    void save_yaml(const fs::path& yaml_path) const;
    struct DriveClearResult { uintmax_t removed_entries = 0; };
    struct MemoryClearResult { int cleared_nodes = 0; };
    struct CacheSaveResult { int saved_nodes = 0; };
    struct DiskSyncResult { int saved_nodes = 0; int removed_files = 0; int removed_dirs = 0; };

    DriveClearResult clear_drive_cache();
    MemoryClearResult clear_memory_cache();
    void clear_cache();
    CacheSaveResult cache_all_nodes(const std::string& cache_precision);
    MemoryClearResult free_transient_memory();
    DiskSyncResult synchronize_disk_cache(const std::string& cache_precision);
    fs::path node_cache_dir(int node_id) const;

    NodeOutput& compute(int node_id, const std::string& cache_precision, bool force_recache = false, bool enable_timing = false);    
    NodeOutput& compute_parallel(int node_id, const std::string& cache_precision, bool force_recache = false, bool enable_timing = false);
    void clear_timing_results();
    std::vector<int> ending_nodes() const;
    void print_dependency_tree(std::ostream& os, bool show_parameters = true) const;
    void print_dependency_tree(std::ostream& os, int start_node_id, bool show_parameters = true) const;
    std::vector<int> topo_postorder_from(int end_node_id) const;
    std::vector<int> get_trees_containing_node(int node_id) const;
    // Streaming compute events (separate from timers)
    struct ComputeEvent { int id; std::string name; std::string source; double elapsed_ms; };
    std::vector<ComputeEvent> drain_compute_events();

private:
    NodeOutput& compute_internal(int node_id, const std::string& cache_precision, std::unordered_map<int, bool>& visiting, bool enable_timing);
    bool is_ancestor(int potential_ancestor_id, int node_id, std::unordered_set<int>& visited) const; 
    std::vector<int> parents_of(int node_id) const;

    void save_cache_if_configured(const Node& node, const std::string& cache_precision) const;
    bool try_load_from_disk_cache(Node& node);
    void execute_op_for_node(int node_id, const std::string& cache_precision, bool enable_timing);

    std::mutex graph_mutex_;
    bool quiet_ = true;
    // event buffer for streaming compute messages
    std::mutex event_mutex_;
    std::vector<ComputeEvent> event_buffer_;
    void push_compute_event(int id, const std::string& name, const std::string& source, double ms);
};

} // namespace ps
