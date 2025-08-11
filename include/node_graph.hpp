#pragma once
#include "ps_types.hpp"
#include "node.hpp"
#include <unordered_set>

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

    void clear();
    void add_node(const Node& node);
    bool has_node(int id) const;

    void load_yaml(const fs::path& yaml_path);
    void save_yaml(const fs::path& yaml_path) const;
    void clear_drive_cache();
    void clear_memory_cache();
    void clear_cache();
    void cache_all_nodes();
    void free_transient_memory();
    void synchronize_disk_cache();
    fs::path node_cache_dir(int node_id) const;

    const NodeOutput& compute(int node_id, bool force_recache = false, bool enable_timing = false);    
    void clear_timing_results();
    std::vector<int> ending_nodes() const;
    void print_dependency_tree(std::ostream& os) const;
    std::vector<int> topo_postorder_from(int end_node_id) const;

private:
    const NodeOutput& compute_internal(int node_id, std::unordered_map<int, bool>& visiting, bool force_recache, bool enable_timing);
    bool is_ancestor(int potential_ancestor_id, int node_id, std::unordered_set<int>& visited) const; 
    std::vector<int> parents_of(int node_id) const;

    void save_cache_if_configured(const Node& node) const;
};

} // namespace ps