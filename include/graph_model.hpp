#pragma once

#include "ps_types.hpp"
#include "node.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ps {

class GraphCacheService;
class GraphIOService;
class GraphTraversalService;
class ComputeService;

struct NodeTiming {
    int id = -1;
    std::string name;
    double elapsed_ms = 0.0;
    std::string source;
};

struct TimingCollector {
    std::vector<NodeTiming> node_timings;
    double total_ms = 0.0;
};

class GraphModel {
public:
    TimingCollector timing_results;
    std::unordered_map<int, Node> nodes;
    fs::path cache_root;

    struct DriveClearResult { uintmax_t removed_entries = 0; };
    struct MemoryClearResult { int cleared_nodes = 0; };
    struct CacheSaveResult { int saved_nodes = 0; };
    struct DiskSyncResult {
        int saved_nodes = 0;
        int removed_files = 0;
        int removed_dirs = 0;
    };

    explicit GraphModel(fs::path cache_root_dir = "cache");

    void set_quiet(bool q);
    bool is_quiet() const;

    void clear();
    void add_node(const Node& node);
    bool has_node(int id) const;

    void set_skip_save_cache(bool v);
    bool skip_save_cache() const;

    std::atomic<double> total_io_time_ms{0.0};

private:
    friend class GraphCacheService;
    friend class GraphIOService;
    friend class GraphTraversalService;
    friend class ComputeService;

    std::mutex graph_mutex_;
    mutable std::mutex timing_mutex_;
    bool quiet_ = true;
    std::atomic<bool> skip_save_cache_{false};
};

} // namespace ps
